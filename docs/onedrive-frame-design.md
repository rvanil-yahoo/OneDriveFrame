# OneDriveFrame Design Document

## 1. Goal

Build a Wi‑Fi-connected digital photo frame based on an ESP32-S3 that automatically pulls images from a dedicated OneDrive folder and displays them locally from an SD card. The user uploads images into a known folder and the frame periodically syncs and shows the newest photos that fit within the card’s storage budget.

This design intentionally avoids:
- Azure Functions or any hosted backend
- local PC/server requirement
- semantic search or AI on the ESP32
- complex infra

## 2. Product behavior

The system behaves like this:

1. User signs into a Microsoft account (MSA) once using a browser/device-code flow.
2. The ESP32 receives tokens from Microsoft Identity and stores them in flash/NVS.
3. The ESP32 calls Microsoft Graph to inspect a dedicated OneDrive folder, such as `ESP32Frame`.
4. It downloads only the newest image files into a local `/photos` folder on the SD card.
5. It keeps only the newest N images that fit within the selected storage budget.
6. The ESP32 displays the images in a slideshow on the attached display.
7. The ESP32 periodically re-syncs the folder to refresh new images.

This gives a clean “manual curation” model: the user controls which photos are in the folder; the device simply keeps a fresh, local copy.

## 3. Key design decisions

### 3.1 ESP32 handles display, storage, and slideshow
The ESP32 is responsible for:
- Wi‑Fi connectivity
- Microsoft Graph API calls
- token storage and refresh
- downloading JPEGs
- writing them to the SD card
- displaying them in a slideshow

### 3.2 OneDrive is the source of truth
The source of photos is a single folder in the user’s OneDrive, for example:
- `ESP32Frame`

The user is expected to add or remove images in that folder as needed. This avoids the need for the ESP32 to do any semantic matching or AI ranking.

### 3.3 Storage is capped and pruned
The frame keeps only a bounded subset of the latest photos that fit on the SD card. This is essential to prevent the card from filling up and to provide predictable behavior.

### 3.4 Authentication uses Microsoft account consumer authority
Authentication is targeted at:
- `https://login.microsoftonline.com/consumers`

This is the sign-in authority for Microsoft personal accounts (MSA). The app is treated as a public client, not a confidential app.

## 4. High-level architecture

### 4.1 Device architecture
- ESP32-S3 board
- TFT/LCD display
- microSD card
- Wi‑Fi
- NVS or flash storage for tokens
- power management

### 4.2 Cloud architecture
- Microsoft Graph API
- OneDrive consumer account
- dedicated photo folder

### 4.3 Data flow
1. Mount SD storage first and display the first readable local JPEG before starting background tasks. Initial display does not wait for a full photo scan or catalog reconciliation.
2. Reconcile the catalog in the sync task, then connect to Wi‑Fi. The display task remains responsible for slideshow rendering; a fallback runs only tasks that could not be created.
3. Load stored refresh token and expiry from NVS.
4. If token is expired, refresh it. If refresh fails, begin device-code login.
5. Call OneDrive Graph to list the folder contents.
6. Sort files by last modified time descending.
7. Download newest files until the configured storage quota is reached.
8. Delete oldest local files when quota is exceeded.
9. Continue displaying images from the SD card in slideshow mode.
10. Repeat sync periodically.

Slide intervals begin after rendering finishes, so a slow decode does not trigger an immediate second render. Failed images also wait one interval before the next attempt. JPEG decoding yields periodically for watchdog safety without delaying every block row.

## 5. Recommended folder structure

On OneDrive:
- `ESP32Frame/`
  - image files
  - optional subfolders for manual organization if desired

On the ESP32 SD card:
- `/photos/`
  - current display set
  - only JPEGs, or JPEG + PNG if supported

The app should optionally avoid mixing too many file types and should focus on `jpg` and `jpeg` for consistency and ease of decode.

## 6. Authentication design

### 6.1 Authority
Use:
- `https://login.microsoftonline.com/consumers`

### 6.2 OAuth flow
Use Microsoft device code flow for a public client app.

Required flow:
1. Request device code from:
   - `https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode`
2. Show the user a URL and code on the display or a serial console.
3. User completes sign-in in a browser.
4. Poll token endpoint:
   - `https://login.microsoftonline.com/consumers/oauth2/v2.0/token`
5. Save refresh token and expiry in NVS.

### 6.3 Scopes
Use the minimum needed:
- `Files.ReadWrite`
- `offline_access`
- optionally `User.Read` if helpful for diagnostics

### 6.4 Token storage
Persist:
- access_token
- refresh_token
- expires_in / expires_at
- token_type

### 6.5 Token refresh policy
- Use access token while valid.
- If expired or near expiry, use refresh token.
- If refresh fails, require the user to sign in again using device code flow.

## 7. OneDrive sync logic

### 7.1 Folder listing
Graph endpoint:
- `GET https://graph.microsoft.com/v1.0/me/drive/root:/ESP32Frame:/children`

The response includes metadata:
- `name`
- `size`
- `lastModifiedDateTime`
- `file`
- `downloadUrl` or item id for file content

### 7.2 File selection
Sort by:
- `lastModifiedDateTime` descending

Then select files that:
- are images
- match allowed formats
- fit within the configured SD budget

### 7.3 Download strategy
For each selected file:
1. request the file content
2. write to `/photos/<filename>` on the SD card
3. keep metadata in memory if needed for ordering

### 7.4 Pruning strategy
If total local image usage exceeds the configured limit:
- delete oldest files first
- keep the newest files that fit under quota

## 8. Storage quota model

A practical default:
- target local usage = 2–6 GB, depending on SD card size
- keep the newest 100–500 image files, whichever fits first

The system should calculate based on:
- SD card total capacity
- free space available
- average per-file size
- configured safety margin

Example:
- 16 GB card
- allocate 4 GB for photos
- keep newest images until the local total is under 4 GB

This keeps the frame predictable and prevents SD card exhaustion.

## 9. Slideshow behavior

The framebuffer should do the following:
- scan `/photos`
- sort files in a stable order
- decode JPEGs
- display each image for a configured duration (e.g., 5–15 seconds)
- cycle indefinitely

Optional enhancements:
- randomize order
- fade transition
- transition speed setting
- sleep mode when idle
- manual refresh trigger

## 10. Failure modes and recovery

### 10.1 No Wi‑Fi
The frame should continue showing the last synced images from the SD card.

### 10.2 OneDrive folder missing
The frame should log a clear error and continue displaying local photos until re-sync succeeds.

### 10.3 Token expired or revoked
The firmware should:
- clear stored token state
- start device-code login
- retry sync once a valid token exists

### 10.4 SD full
The frame should:
- stop downloading new files
- prune oldest local images
- retry with the remaining quota

### 10.5 File download fails
The frame should skip the failed file and continue with remaining files.

## 11. Firmware module breakdown

### 11.1 Wi‑Fi module
- connect to SSID/password
- monitor connection status

### 11.2 NVS storage module
- store tokens and config
- restore them on boot

### 11.3 OAuth module
- device-code login flow
- token refresh logic
- Graph auth headers

### 11.4 Graph client module
- list folder contents
- fetch file metadata
- download file content

### 11.5 Storage manager
- mount SD card
- maintain local photo directory
- compute current used space
- prune older photos

### 11.6 Display module
- decode JPEGs
- update the TFT or LCD
- handle slideshow timing and transitions

## 12. Suggested firmware state machine

The device should have a clear state flow:

- START
- WIFI_CONNECT
- LOAD_TOKEN
- CHECK_TOKEN_VALIDITY
- REFRESH_TOKEN_OR_LOGIN
- LIST_ONEDRIVE_FOLDER
- DOWNLOAD_NEW_PHOTOS
- PRUNE_OLD_PHOTOS
- DISPLAY_SLIDESHOW
- IDLE_WAIT_FOR_SYNC

This keeps the firmware deterministic and easier to debug.

## 13. Application config (example)

```json
{
  "folderName": "ESP32Frame",
  "syncIntervalMinutes": 180,
  "slideSeconds": 8,
  "maxLocalUsageMb": 4096,
  "allowedExtensions": [".jpg", ".jpeg"],
  "wifi": {
    "ssid": "MyWiFi",
    "password": "secret"
  }
}
```

## 14. Minimum viable implementation

This project can start with a simple MVP:
- ESP32-S3 + display + microSD
- manual sign-in using device code flow
- OneDrive folder named `ESP32Frame`
- the device syncs this folder every 3 hours
- newest photos are downloaded and shown
- old local files are deleted when storage limit is reached

This MVP does not require:
- search
- AI
- backend hosting
- user management
- complex metadata indexing

## 15. Recommended next steps

1. Create a minimal app registration in Microsoft Entra for a public client app.
2. Validate device-code flow with a test script or desktop prototype.
3. Build the ESP32 OAuth token storage and refresh flow.
4. Implement Graph folder listing and file download.
5. Implement SD storage quota pruning.
6. Add slideshow rendering and startup sync.
7. Test with a real OneDrive consumer account and a dedicated `ESP32Frame` folder.

## 16. Risks and mitigations

### Risk: token expiry
Mitigation: save refresh token and refresh before expiry.

### Risk: OneDrive API access blocks
Mitigation: use device code flow and test with a straightforward folder read.

### Risk: SD card fills up
Mitigation: enforce quota and delete oldest files.

### Risk: unsupported image types
Mitigation: limit allowed extensions to `.jpg` and `.jpeg` for MVP.

### Risk: slow network or large downloads
Mitigation: sync on a timer and keep it bounded; download only files that fit under quota.

## 17. Final recommendation

This is a strong and low-complexity project design:
- no custom backend
- no Azure hosting required
- no on-device AI
- no app server
- a single OneDrive folder as the photo source
- automatic, incremental sync from a consumer OneDrive account

This is the best fit for a practical ESP32 photo frame that updates itself on a schedule while keeping the logic simple and maintainable.
