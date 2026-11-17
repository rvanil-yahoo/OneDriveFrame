# OneDriveFrame Design Document

This document describes the firmware **as currently implemented**. Items that are designed but not active are called out explicitly, and planned work is collected in [section 13](#13-known-gaps-and-future-work).

## 1. Goal

A standalone Wi‑Fi photo frame built on an ESP32-S3 RGB display board. The user curates photos in a single OneDrive folder; the frame copies them to an SD card and runs a slideshow from local storage.

Design constraints:

- No hosted backend, PC, or server — the device talks directly to Microsoft identity and Microsoft Graph.
- No on-device AI, search, or ranking; the folder contents *are* the playlist.
- Local photos are always viewable, even with no Wi‑Fi or no sign-in.
- The user manages photo sizes; the firmware does not create resized or "display-ready" copies.

## 2. Hardware

| Component | Details |
| --- | --- |
| Board | Viewe UEDX80480043E-WB-A (ESP32-S3, 4.3" 800 × 480 RGB panel) |
| PSRAM | Octal PSRAM (`qio_opi`), required for frame buffer, LVGL buffers, and JPEG decode |
| Flash | 4 MB (`board_upload.flash_size`) |
| SD card | SPI: CS 10, MOSI 11, SCK 12, MISO 13 at 4 MHz |
| Touch | Resistive-touch CS (GPIO 38) is held high during SD init; touch is not used |

Pins and timings live in [FrameConfig.h](../src/FrameConfig.h). The panel driver uses the `BOARD_VIEWE_UEDX80480043E_WB_A` definition from ESP32_Display_Panel, selected in [platformio.ini](../platformio.ini).

## 3. Software stack

| Library | Use |
| --- | --- |
| Arduino-ESP32 / FreeRTOS | Runtime, tasks, mutex |
| ESP32_Display_Panel v1.0.3 | RGB panel bus and LCD driver |
| LVGL 8.4 | Image widget and display refresh |
| TJpg_Decoder | JPEG decoding from SD |
| WiFiManager | Captive-portal Wi‑Fi setup |
| ArduinoJson 7 | Graph / OAuth JSON parsing |
| Sqlite3Esp32 | Photo catalog on SD |
| Preferences (NVS) | Token persistence |

## 4. Module overview

| Module | Responsibility |
| --- | --- |
| [OneDriveFrameApp](../src/OneDriveFrameApp.cpp) | Boot order, storage mutex, FreeRTOS tasks, sync state machine |
| [PhotoStorage](../src/PhotoStorage.cpp) | SD mount, photo list, JPEG decode/resize, LVGL + panel output, pruning entry point |
| [GraphClient](../src/GraphClient.cpp) | OAuth device-code and refresh, folder listing, downloads with validation |
| [PhotoCatalog](../src/PhotoCatalog.cpp) | SQLite catalog (`/sd/catalog.db`): reconcile, upsert, mark downloaded, prune queries |
| [TokenStore](../src/TokenStore.cpp) | NVS namespace `odf_auth` for access/refresh tokens |
| [WifiSetupManager](../src/WifiSetupManager.cpp) | WiFiManager auto-connect or setup portal |
| [FrameConfig.h](../src/FrameConfig.h) | All tunable constants |

## 5. Boot and tasking

### 5.1 Storage-first boot

[`OneDriveFrameApp::setup()`](../src/OneDriveFrameApp.cpp) runs in this order:

1. Create the photo-storage mutex and hand it to `GraphClient`.
2. Loop until the SD card mounts (`PhotoStorage::begin()`), retrying every 500 ms and logging every 5 s.
3. Immediately call `displaySlideshow()` once. This initializes the panel and LVGL, and renders the **first** readable JPEG found in `/photos` (header-probed, no full scan, no catalog work).
4. Start the two worker tasks.

Catalog reconciliation, Wi‑Fi, and authentication all happen later in the sync task, so the first photo appears without waiting on the network.

### 5.2 Tasks

| Task | Core | Priority | Stack | Period | Work |
| --- | --- | --- | --- | --- | --- |
| `display-core0` | 0 | 3 | 16 KB | 16 ms | `PhotoStorage::displaySlideshow()` under the mutex |
| `sync-core1` | 1 | 1 | 16 KB | 250 ms | One step of the sync state machine |

If either task fails to start, `loop()` runs the missing step(s) itself.

### 5.3 SD access serialization

The SD card (SPI) and SQLite catalog are shared by both tasks. A single FreeRTOS mutex serializes them:

- The display task holds it for a whole slideshow step (scan + decode + render).
- `GraphClient` uses an RAII `StorageLock` and only takes the mutex around SD/catalog operations — never across network I/O — so downloads don't block the slideshow. Folder listing takes one lock per page of results.

## 6. Synchronization

### 6.1 State machine

States are defined in [AppState.h](../src/AppState.h):

```text
START ──▶ WIFI_CONNECT ──▶ LOAD_TOKEN ──▶ CHECK_TOKEN_VALIDITY
              │ fail                         │ valid        │ expired/missing
              ▼                              ▼              ▼
     IDLE_WAIT_FOR_SYNC          LIST_ONEDRIVE_FOLDER ◀── REFRESH_TOKEN_OR_LOGIN
              ▲                     │ ok        │ 401 ─────────▲   │ fail
              │                     ▼           │ other fail      ▼
              │             DOWNLOAD_NEW_PHOTOS │           IDLE_WAIT_FOR_SYNC
              │                     │ 401 ──────┼──▶ REFRESH_TOKEN_OR_LOGIN
              │                     ▼           ▼
              └──────────────  DISPLAY_SLIDESHOW
```

- **START** – Open/create the catalog and reconcile it with files on SD (retries every second until it succeeds), then open the NVS token store.
- **WIFI_CONNECT** – WiFiManager `autoConnect`; on failure go idle.
- **LOAD_TOKEN / CHECK_TOKEN_VALIDITY** – Tokens are restored from NVS, but expiry is not trusted across reboots (`expiresAtMs = 0`), so the first sync after boot always refreshes.
- **REFRESH_TOKEN_OR_LOGIN** – Try refresh-token grant; if that fails, run the device-code flow. Save tokens on success.
- **LIST_ONEDRIVE_FOLDER** – See 6.3. A 401 clears the access token and goes back to refresh/login.
- **DOWNLOAD_NEW_PHOTOS** – See 6.4. Sets `lastSuccessfulSyncMs_` whether or not every file succeeded.
- **DISPLAY_SLIDESHOW** – Marker state only (the display task runs independently); moves to idle.
- **IDLE_WAIT_FOR_SYNC** – Re-enters WIFI_CONNECT once `kSyncIntervalMs` (3 h) has passed since the last completed download pass.

`PRUNE_OLD_PHOTOS` exists but is **not reachable** from any transition today (see section 13).

### 6.2 Authentication

- Authority: `https://login.microsoftonline.com/consumers` (personal Microsoft accounts), public client, no secret.
- Scopes: `Files.Read offline_access`.
- Client ID: `CLIENT_ID` macro (default in [FrameConfig.h](../src/FrameConfig.h)); a placeholder all-zero ID disables auth.
- **Device-code flow:** POST to `/devicecode`, print the message/URL/code to serial, then poll `/token` at the server-provided interval until success, `expires_in` (default 900 s), or a terminal error. `authorization_pending` continues; `slow_down` adds 5 s to the interval.
- **Refresh:** `grant_type=refresh_token`; a rotated refresh token replaces the stored one.
- Access-token expiry is stored as `millis() + expires_in − 60 s`.
- OAuth POSTs retry transport failures up to 4 times with linear back-off.

### 6.3 Folder listing

```text
GET /v1.0/me/drive/root:/ESP32Frame:/children
    ?$top=25&$select=id,name,size,lastModifiedDateTime,eTag,file,image,photo
```

- Follows `@odata.nextLink` until exhausted (25 items per page keeps each JSON response small for the ESP32's limited internal RAM).
- `beginRemoteSync()` first clears `remote_present` on all catalog rows that have an `item_id`.
- For each child that is a file with a `.jpg`, `.jpeg`, or `.png` name, a non-empty `id`, and a `lastModifiedDateTime`:
  - The catalog row is upserted (matched by `item_id`, else by `local_path`, else inserted) with `remote_present = 1`.
  - The item is appended to an in-memory download queue of **128** entries. Once full, further items are still cataloged but not queued for this pass.
- The queue is then sorted by `lastModifiedDateTime` descending (newest first).
- Local filenames are the OneDrive names with `/ \ : * ? " < > |` replaced by `_`.
- `getWithBearer` retries transport failures up to 6 attempts and honours `Retry-After` on HTTP 429 (exponential back-off when the header is absent). Response bodies are truncated at the first NUL byte before JSON parsing.
- Listing returns failure if no images were queued; the state machine then skips downloads for this cycle.

### 6.4 Downloads

For each queued photo, newest first:

1. **Skip check** – If `/photos/<name>` exists, its size equals the Graph `size`, and (for JPEGs) it ends with the `FF D9` end-of-image marker, skip it. Otherwise delete it and any stale `.part` file.
2. **Resolve URL** – `GET /me/drive/items/{id}?select=id,name,@microsoft.graph.downloadUrl`.
3. **Fetch** – HTTPS GET of the pre-authenticated URL (redirects followed, HTTP/1.0, `Connection: close`) into `/photos/<name>.part`.
4. **Stream to SD** – 2 KB reads; the loop ends on completion, connection close, or **20 s** without data.
5. **Validate** – The file is accepted only if received bytes, written bytes, on-disk size, Graph `size`, and `Content-Length` (when present) all agree, and a JPEG ends with `FF D9`. Otherwise the `.part` file is deleted.
6. **Publish** – Rename `.part` to the final name and `markDownloaded()` in the catalog.

A `Download result` line logs exit reason and all byte counts for each file. A failed file is skipped; a 401 aborts the pass and triggers token refresh.

#### SD write-error recovery

SPI SD writes intermittently fail with `EIO`. After an error the FATFS handle stays failed and buffered data may be lost, so the downloader:

- Retries a failed chunk up to 4 times with increasing delay.
- On each retry, closes and reopens the `.part` file in append mode and reads its **actual** on-disk size.
- Rewrites any missing tail from an 8 KB ring buffer of the most recently downloaded bytes, then writes the remainder of the chunk.
- Gives up (and discards the file) if the on-disk size is outside what the history can repair.

The 2 KB download buffer and 8 KB history are allocated once in PSRAM to keep internal RAM free for TLS.

### 6.5 Catalog

SQLite database at `/sd/catalog.db`, table `photos`:

| Column | Meaning |
| --- | --- |
| `item_id` (unique) | OneDrive item ID; `NULL` for files found only on SD |
| `filename`, `local_path` (unique) | Original name and `/photos/...` path |
| `size_bytes`, `width`, `height`, `orientation`, `mime_type`, `etag`, `last_modified_utc` | Graph metadata |
| `remote_present` | Seen in the latest folder listing |
| `local_present` | File currently on SD |
| `downloaded_utc` | When the file was last saved |

Indexes exist on `item_id`, `last_modified_utc`, `local_present`, and `size_bytes`. A migration adds `remote_present` to older databases. Each operation opens and closes the DB, and `execute()` retries `SQLITE_IOERR` up to 3 times.

`reconcileWithStorage()` (run at START) marks every row `local_present = 0`, then walks `/photos` and marks or inserts each file found.

## 7. Display pipeline

### 7.1 Panel and LVGL setup

On the first `displaySlideshow()` call:

1. `Board::init()` loads the board definition.
2. Before `begin()`, the RGB bus is tuned:
   - 1 frame buffer (PSRAM).
   - **Bounce buffer 16 lines** (`kDisplayBounceBufferLines`, board default 10).
   - **Pixel clock 13 MHz** (`kDisplayPixelClockHz`, board default 15 MHz).
3. `Board::begin()`; if that fails (e.g. touch controller), retry LCD-only.
4. Orientation is set to the panel's native landscape: `swapXY(false)`, `mirrorX(false)`, `mirrorY(false)`.
5. LVGL is initialized with an 800 × 480 full-screen draw buffer in PSRAM and a black screen background.

The larger bounce buffer and slightly lower pixel clock give the LCD's bounce-buffer refill (a copy from the PSRAM frame buffer) enough slack while the sync task is busy with TLS and SD traffic. Without this, scan-out occasionally underran and showed a one-frame horizontal band or shift. Cost: about 19 KB more internal RAM in total, and refresh drops from about 34 Hz to about 30 Hz.

### 7.2 Slideshow loop

Each display-task step:

1. Run LVGL timers.
2. If the current slide has been shown for less than `kSlideDurationMs` (8 s), return.
3. Rescan `/photos` for `.jpg` / `.jpeg` files (up to 256), sorted by path. The very first scan header-probes files and stops at the first readable one; later scans skip probing for speed and yield every 16 entries.
4. Render the next file in round-robin order.
5. Start the hold timer **after** rendering completes, so slow decodes don't shorten the visible time. Failed renders also wait one interval.

A slide log line records scan time, render time, and file count.

### 7.3 Decode and fit

1. Read JPEG dimensions with `TJpgDec.getSdJpgSize`.
2. Pick the smallest TJpgDec scale (1, 1/2, 1/4, 1/8) that fits within 800 × 480.
3. Compute an **aspect-preserving** target: landscape images fill the 800-px width, portrait or tall images fill the 480-px height.
4. Decode into an RGB565 buffer, yielding every ~10 ms.
5. If needed, nearest-neighbour resize to the exact target size.

### 7.4 Rendering

- The RGB565 pixels are converted into an `lv_color_t` buffer for an `lv_img` widget, which is centered on the black screen.
- `lv_refr_now()` renders once. The flush callback converts each row back to RGB565 and calls `drawBitmap()` into the panel frame buffer, yielding every 24 rows.
- Nothing redraws between slides, so the frame buffer is static while a photo is held.

## 8. Concurrency and memory notes

- **Internal RAM is the scarcest resource.** TLS sessions need a large contiguous internal block (~31 KB largest block observed). Download buffers, LVGL buffers, decoded images, and the frame buffer all live in PSRAM.
- The panel ISR copies from PSRAM continuously; heavy PSRAM traffic from other work can starve it. Avoid large `memmove`s in PSRAM on hot paths (the download history buffer is a ring buffer for this reason).
- A full display-side scan that probed every JPEG header previously tripped the task watchdog; header probing is now limited to the first boot scan.

## 9. Failure handling

| Situation | Behaviour |
| --- | --- |
| SD not mounted at boot | Boot waits and retries; nothing else starts until SD is ready |
| Unreadable JPEG on first boot scan | File is **deleted** from SD and the scan continues |
| JPEG fails to decode during slideshow | File is kept; slide waits one interval, then advances |
| No Wi‑Fi / portal timeout | Go idle; retry at next sync interval; slideshow continues |
| Refresh and device-code both fail | Go idle; retry at next interval |
| Graph 401 (list or download) | Clear access token, refresh/login, retry |
| Graph 429 | Honour `Retry-After` or back off exponentially (max 6 attempts) |
| HTTPS transport failure | Retry with linear back-off; listing aborts after 6 attempts |
| Download truncated / stalled | Validation rejects it; `.part` deleted; next sync retries |
| SD `EIO` during download | Reopen-and-resume up to 4 times (6.4) |
| SD `EIO` in SQLite | `execute()` retries up to 3 times |
| Truncated file already on SD | Skip check fails on missing `FF D9`; re-downloaded |

## 10. Configuration reference

From [FrameConfig.h](../src/FrameConfig.h):

| Constant | Default | Purpose |
| --- | --- | --- |
| `kOneDriveFolderName` | `ESP32Frame` | OneDrive folder at drive root |
| `kPhotoDirectory` | `/photos` | Local photo folder on SD |
| `kGraphScope` | `Files.Read offline_access` | OAuth scopes |
| `CLIENT_ID` | built-in default | Entra app (public client) ID; override with `-DCLIENT_ID=...` |
| `kConfigPortalName` / `kConfigPortalPassword` | `OneDriveFrame Setup` / configured | Wi‑Fi setup AP |
| `kWifiConnectTimeoutMs` / `kWifiPortalTimeoutMs` | 20 s / 5 min | Wi‑Fi timeouts |
| `kSdSpiFrequencyHz` | 4 MHz | SD SPI clock |
| `kSdFormatIfEmpty` | `true` | **Formats the card** if the normal mount fails |
| `kSyncIntervalMs` | 3 h | Time between sync passes |
| `kSlideDurationMs` | 8 s | Time each photo is held |
| `kDisplayBounceBufferLines` | 16 | RGB bounce buffer height (must divide 240 evenly when × 800) |
| `kDisplayPixelClockHz` | 13 MHz | RGB pixel clock |
| `kEnableDisplayOutput` | `true` | Disable to run without rendering |
| `kMaxLocalUsageBytes` | 4 GiB¹ | Quota used by the (currently unreachable) prune step |
| `kDefaultDeviceCodePollMs` | 5 s | Poll interval if the server omits one |

¹ Declared as `uint32_t`, so `4 * 1024³` overflows to 0. This needs fixing before pruning is enabled.

## 11. Build and tooling

```powershell
python -m platformio run -e esp32-s3-devkitc-1             # build
python -m platformio run -e esp32-s3-devkitc-1 -t upload   # flash (COM13)
python -m platformio device monitor -b 115200              # serial log
```

Log prefixes: `[Boot]`, `[Storage]`, `[Catalog]`, `[Display]`, `[LVGL]`, `[WiFi]`, `[NVS]`, `[OAuth]`, `[Graph]`.

## 12. Security notes

- HTTPS certificate verification is disabled (`setInsecure()`) for all Graph and identity requests.
- Tokens are stored unencrypted in NVS.
- Serial logs can include download URLs and Graph payload previews; treat logs as sensitive.

## 13. Known gaps and future work

- **Pruning is not wired up.** `PhotoStorage::pruneOldestPhotos()` and the catalog queries (`removeFilesNotInRemoteCatalog`, `pruneOldestLocalFilesToQuota`) are implemented, but no state transition reaches `PRUNE_OLD_PHOTOS`. Photos removed from OneDrive stay on SD, and no quota is enforced. Enabling this also requires fixing the `kMaxLocalUsageBytes` overflow.
- **Queue selection order.** The 128-item queue fills in Graph listing order and is sorted afterwards, so with more than 128 images it is not guaranteed to hold the newest 128.
- **PNG** files are downloaded and cataloged, but the slideshow only renders JPEG.
- **Slideshow cap.** Only the first 256 JPEGs found in `/photos` are shown, in filename order.
- **First-scan deletion.** An unreadable JPEG found by the first boot scan is deleted rather than skipped.
- **EXIF orientation** is stored in the catalog but not applied when rendering.
- **Sign-in UX.** The device code is printed to serial only, not shown on the display.
- Possible enhancements: shuffle order, transitions, sleep schedule, manual refresh trigger, certificate pinning.
