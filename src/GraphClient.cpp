#include "GraphClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include "FrameConfig.h"

namespace odf {

namespace {

constexpr const char* kPlaceholderClientId = "00000000-0000-0000-0000-000000000000";
constexpr size_t kHttpBufferSize = 2048;

class StorageLock {
 public:
  explicit StorageLock(SemaphoreHandle_t mutex) : mutex_(mutex) {
    if (mutex_ != nullptr) {
      xSemaphoreTake(mutex_, portMAX_DELAY);
    }
  }
  ~StorageLock() {
    if (mutex_ != nullptr) {
      xSemaphoreGive(mutex_);
    }
  }
  StorageLock(const StorageLock&) = delete;
  StorageLock& operator=(const StorageLock&) = delete;

 private:
  SemaphoreHandle_t mutex_;
};

bool endsWithJpegEoi(File& file) {
  const size_t size = file.size();
  if (size < 4) {
    return false;
  }
  // Some encoders pad after EOI; scan the last few bytes for FF D9.
  const size_t tailLen = size < 32 ? size : 32;
  uint8_t tail[32];
  if (!file.seek(size - tailLen) || file.read(tail, tailLen) != static_cast<int>(tailLen)) {
    return false;
  }
  for (size_t i = tailLen - 1; i > 0; --i) {
    if (tail[i - 1] == 0xFF && tail[i] == 0xD9) {
      return true;
    }
  }
  return false;
}

bool isJpegName(const String& name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".jpg") || lower.endsWith(".jpeg");
}

void trimAtFirstNul(String& payload);

String urlEncode(const String& value) {
  String encoded;
  encoded.reserve(value.length() * 3);

  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      encoded += c;
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
      encoded += hex;
    }
  }

  return encoded;
}

bool postForm(const String& url, const String& formBody, int& statusCode, String& responseBody) {
  const uint8_t maxRetries = 3U;

  for (uint8_t attempt = 0; attempt <= maxRetries; ++attempt) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.print("[OAuth] WiFi not connected before POST attempt ");
      Serial.print(attempt + 1);
      Serial.print("/");
      Serial.println(maxRetries + 1);
      delay(1000U);
      if (WiFi.status() != WL_CONNECTED) {
        return false;
      }
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    if (!http.begin(client, url)) {
      Serial.println("[OAuth] Failed to initialize HTTPS client.");
      return false;
    }

    http.setTimeout(15000);
    http.setConnectTimeout(10000);
    http.setReuse(false);
    http.useHTTP10(true);
    http.addHeader("Connection", "close");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    statusCode = http.POST(formBody);
    responseBody = http.getString();
    trimAtFirstNul(responseBody);
    http.end();

    if (statusCode > 0) {
      return true;
    }

    Serial.print("[OAuth] HTTPS request failed: ");
    Serial.print(statusCode);
    Serial.print(" (attempt ");
    Serial.print(attempt + 1);
    Serial.print("/");
    Serial.print(maxRetries + 1);
    Serial.println(")");

    if (attempt < maxRetries) {
      delay(1500UL * (attempt + 1));
      continue;
    }
    return false;
  }

  return false;
}

bool parseTokenResponse(const String& payload, OAuthToken& token) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("[OAuth] Failed to parse token response JSON: ");
    Serial.println(err.c_str());
    return false;
  }

  const String accessToken = doc["access_token"] | "";
  if (accessToken.isEmpty()) {
    Serial.println("[OAuth] Token response missing access_token.");
    return false;
  }

  token.accessToken = accessToken;

  const String refreshToken = doc["refresh_token"] | "";
  if (!refreshToken.isEmpty()) {
    token.refreshToken = refreshToken;
  }

  const uint32_t expiresInSeconds = doc["expires_in"] | 3600U;
  const uint32_t refreshBufferSeconds = 60U;
  const uint32_t effectiveExpiresIn = expiresInSeconds > refreshBufferSeconds ? expiresInSeconds - refreshBufferSeconds : 30U;
  token.expiresAtMs = millis() + (effectiveExpiresIn * 1000UL);

  return true;
}

bool isClientIdConfigured() {
  return String(CLIENT_ID) != String(kPlaceholderClientId);
}

bool isAllowedImage(const String& name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".png");
}

String sanitizeFilename(const String& name) {
  String out;
  out.reserve(name.length());
  for (size_t i = 0; i < name.length(); ++i) {
    char c = name[i];
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      out += '_';
    } else {
      out += c;
    }
  }
  if (out.length() == 0) {
    out = "photo.jpg";
  }
  return out;
}

void trimAtFirstNul(String& payload) {
  const int nulIndex = payload.indexOf('\0');
  if (nulIndex >= 0) {
    payload.remove(static_cast<unsigned int>(nulIndex));
  }
}

uint32_t parseRetryAfterSeconds(HTTPClient& http) {
  String retryAfterHeader = http.header("Retry-After");
  if (retryAfterHeader.length() == 0) {
    return 0U;
  }

  retryAfterHeader.trim();
  if (retryAfterHeader.length() == 0) {
    return 0U;
  }

  const unsigned long seconds = retryAfterHeader.toInt();
  if (seconds > 0UL) {
    return seconds * 1000UL;
  }

  return 0U;
}

bool getWithBearer(const String& url, const String& token, int& statusCode, String& responseBody) {
  const uint32_t kBaseRetryDelayMs = 2000UL;
  const uint8_t kMaxRetries = 5U;

  for (uint8_t attempt = 0; attempt <= kMaxRetries; ++attempt) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.print("[Graph] WiFi not connected before GET attempt ");
      Serial.print(attempt + 1);
      Serial.print("/");
      Serial.println(kMaxRetries + 1);
      delay(1000U);
      if (WiFi.status() != WL_CONNECTED) {
        statusCode = -5;
        return false;
      }
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    if (!http.begin(client, url)) {
      Serial.println("[Graph] Failed to initialize HTTPS client.");
      return false;
    }

    http.setTimeout(15000);
    http.setConnectTimeout(10000);
    http.setReuse(false);
    http.useHTTP10(true);
    http.addHeader("Connection", "close");
    http.addHeader("Authorization", "Bearer " + token);
    http.addHeader("Accept", "application/json");

    statusCode = http.GET();
    responseBody = http.getString();
    trimAtFirstNul(responseBody);
    const uint32_t retryAfterMs = parseRetryAfterSeconds(http);
    http.end();

    if (statusCode <= 0) {
      Serial.print("[Graph] HTTPS request failed: ");
      Serial.print(statusCode);
      Serial.print(" (attempt ");
      Serial.print(attempt + 1);
      Serial.print("/");
      Serial.print(kMaxRetries + 1);
      Serial.printf(") internalFree=%u largest=%u\n", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
      if (attempt < kMaxRetries) {
        delay(1500UL * (attempt + 1));
        continue;
      }
      return false;
    }

    if (statusCode == 429) {
      if (attempt >= kMaxRetries) {
        Serial.print("[Graph] Rate limited (429) after ");
        Serial.print(kMaxRetries + 1);
        Serial.println(" attempts. Giving up.");
        return false;
      }

      uint32_t delayMs = retryAfterMs;
      if (delayMs == 0U) {
        delayMs = kBaseRetryDelayMs << std::min<uint8_t>(attempt, 3U);
      }
      Serial.print("[Graph] Rate limited (429); retrying after ");
      Serial.print(delayMs);
      Serial.print(" ms (attempt ");
      Serial.print(attempt + 1);
      Serial.print("/");
      Serial.print(kMaxRetries + 1);
      Serial.println(")");
      delay(delayMs);
      continue;
    }

    return true;
  }

  return false;
}

void logPayloadPreview(const String& label, const String& payload) {
  Serial.print(label);
  Serial.print(" length=");
  Serial.println(payload.length());

  const size_t headLen = payload.length() > 400 ? 400 : payload.length();
  const size_t tailLen = payload.length() > 200 ? 200 : payload.length();

  if (headLen > 0) {
    Serial.println("[Graph] Payload head:");
    Serial.println(payload.substring(0, headLen));
  }
  if (payload.length() > tailLen) {
    Serial.println("[Graph] Payload tail:");
    Serial.println(payload.substring(payload.length() - tailLen));
  }
}

bool fetchDownloadUrlForItem(const String& itemId, const String& accessToken, String& downloadUrl) {
  const String endpoint =
      String(kGraphBaseUrl) + "/me/drive/items/" + urlEncode(itemId) + "?select=id,name,@microsoft.graph.downloadUrl";

  int statusCode = 0;
  String responseBody;
  if (!getWithBearer(endpoint, accessToken, statusCode, responseBody)) {
    return false;
  }

  if (statusCode != 200) {
    Serial.print("[Graph] Item details request failed with HTTP ");
    Serial.println(statusCode);
    Serial.println(responseBody);
    return false;
  }

  JsonDocument doc;
  DeserializationError parseErr = deserializeJson(doc, responseBody);
  if (parseErr) {
    Serial.print("[Graph] Failed to parse item details JSON: ");
    Serial.println(parseErr.c_str());
    return false;
  }

  downloadUrl = doc["@microsoft.graph.downloadUrl"] | "";
  if (downloadUrl.isEmpty()) {
    Serial.println("[Graph] Item details missing @microsoft.graph.downloadUrl.");
    return false;
  }

  return true;
}

}  // namespace

bool GraphClient::listFolder(const OAuthToken& token) {
  authFailure_ = false;
  pendingPhotoCount_ = 0;
  if (token.accessToken.isEmpty()) {
    Serial.println("[Graph] Missing access token.");
    return false;
  }
  {
    StorageLock lock(storageMutex_);
    if (catalog_ == nullptr || !catalog_->begin()) {
      Serial.println("[Graph] Failed to initialize photo catalog.");
      return false;
    }
  }

  Serial.println("[Graph] Listing OneDrive folder: " + String(kOneDriveFolderName));
  String endpoint = String(kGraphBaseUrl) + "/me/drive/root:/" + urlEncode(String(kOneDriveFolderName)) + ":/children";
  endpoint += "?$top=25&$select=id,name,size,lastModifiedDateTime,eTag,file,image,photo";
  bool remoteSyncStarted = false;
  {
    StorageLock lock(storageMutex_);
    remoteSyncStarted = catalog_->beginRemoteSync();
  }
  if (!remoteSyncStarted) {
    Serial.println("[Graph] Failed to reset remote catalog presence flags.");
    return false;
  }

  size_t totalChildren = 0;
  size_t totalFiles = 0;
  size_t totalImages = 0;
  size_t cataloged = 0;
  bool queueLimitReached = false;

  while (!endpoint.isEmpty()) {
    int statusCode = 0;
    String responseBody;
    if (!getWithBearer(endpoint, token.accessToken, statusCode, responseBody)) {
      Serial.println("[Graph] Folder request transport failure.");
      return false;
    }

    if (statusCode != 200) {
      Serial.print("[Graph] Folder listing failed with HTTP status ");
      Serial.println(statusCode);
      logPayloadPreview("[Graph] Folder error payload", responseBody);
      if (statusCode == 401) {
        authFailure_ = true;
      }
      return false;
    }

    logPayloadPreview("[Graph] Folder response payload", responseBody);

    JsonDocument doc;
    DeserializationError parseErr = deserializeJson(doc, responseBody);
    if (parseErr) {
      Serial.print("[Graph] Failed to parse folder list JSON: ");
      Serial.println(parseErr.c_str());
      Serial.print("[Graph] Endpoint: ");
      Serial.println(endpoint);
      logPayloadPreview("[Graph] Unparsed payload", responseBody);
      return false;
    }

    JsonArray items = doc["value"].as<JsonArray>();
    if (items.isNull()) {
      Serial.println("[Graph] Folder list missing 'value' array.");
      return false;
    }

    StorageLock pageLock(storageMutex_);
    if (!catalog_->beginTransaction()) {
      Serial.println("[Graph] Failed to begin catalog transaction for folder page.");
      return false;
    }
    bool pageOk = true;
    for (JsonVariant item : items) {
      ++totalChildren;
      const bool isFile = !item["file"].isNull();
      const String name = item["name"] | "";
      const String itemId = item["id"] | "";
      const String modified = item["lastModifiedDateTime"] | "";
      const uint32_t sizeBytes = item["size"] | 0U;
      const String etag = item["eTag"] | "";
      const String mimeType = item["file"]["mimeType"] | "";
      const int32_t width = item["image"]["width"] | -1;
      const int32_t height = item["image"]["height"] | -1;
      const String orientation = item["photo"]["orientation"] | "";
      if (isFile) {
        ++totalFiles;
      }

      if (!isFile || !isAllowedImage(name) || modified.isEmpty() || itemId.isEmpty()) {
        continue;
      }

      ++totalImages;

      const String localPath = String(kPhotoDirectory) + "/" + sanitizeFilename(name);
      PhotoCatalog::RemotePhotoMetadata metadata;
      metadata.itemId = itemId;
      metadata.filename = name;
      metadata.localPath = localPath;
      metadata.modifiedUtc = modified;
      metadata.etag = etag;
      metadata.mimeType = mimeType;
      metadata.width = width;
      metadata.height = height;
      metadata.orientation = orientation;
      metadata.sizeBytes = sizeBytes;
      if (catalog_->upsertRemotePhoto(metadata)) {
        ++cataloged;
      } else {
        Serial.println("[Graph] Catalog upsert failed for: " + name);
        pageOk = false;
        break;
      }

      if (pendingPhotoCount_ >= (sizeof(pendingPhotos_) / sizeof(pendingPhotos_[0]))) {
        queueLimitReached = true;
        continue;
      }

      pendingPhotos_[pendingPhotoCount_].itemId = itemId;
      pendingPhotos_[pendingPhotoCount_].name = name;
      pendingPhotos_[pendingPhotoCount_].modifiedUtc = modified;
      pendingPhotos_[pendingPhotoCount_].etag = etag;
      pendingPhotos_[pendingPhotoCount_].mimeType = mimeType;
      pendingPhotos_[pendingPhotoCount_].width = width;
      pendingPhotos_[pendingPhotoCount_].height = height;
      pendingPhotos_[pendingPhotoCount_].orientation = orientation;
      pendingPhotos_[pendingPhotoCount_].sizeBytes = sizeBytes;
      ++pendingPhotoCount_;
    }

    if (!pageOk || !catalog_->commitTransaction()) {
      catalog_->rollbackTransaction();
      Serial.println("[Graph] Catalog page transaction rolled back.");
      return false;
    }
    endpoint = doc["@odata.nextLink"] | "";
  }

  for (size_t i = 1; i < pendingPhotoCount_; ++i) {
    PendingPhoto key = pendingPhotos_[i];
    size_t j = i;
    while (j > 0 && pendingPhotos_[j - 1].modifiedUtc < key.modifiedUtc) {
      pendingPhotos_[j] = pendingPhotos_[j - 1];
      --j;
    }
    pendingPhotos_[j] = key;
  }

  Serial.print("[Graph] Folder children/files/images: ");
  Serial.print(totalChildren);
  Serial.print("/");
  Serial.print(totalFiles);
  Serial.print("/");
  Serial.println(totalImages);
  Serial.print("[Graph] Cataloged image metadata rows: ");
  Serial.println(cataloged);
  Serial.print("[Graph] Queued image files: ");
  Serial.println(pendingPhotoCount_);
  if (queueLimitReached) {
    Serial.print("[Graph] Queue limit reached; only the first ");
    Serial.print(sizeof(pendingPhotos_) / sizeof(pendingPhotos_[0]));
    Serial.println(" images will be downloaded this sync pass.");
  }
  return pendingPhotoCount_ > 0;
}

bool GraphClient::downloadNewestPhotos(const OAuthToken& token) {
  authFailure_ = false;
  if (token.accessToken.isEmpty()) {
    Serial.println("[Graph] Missing access token for download.");
    return false;
  }

  if (pendingPhotoCount_ == 0) {
    Serial.println("[Graph] No queued photos to download.");
    return true;
  }

  Serial.println("[Graph] Downloading queued OneDrive folder photos.");
  Serial.print("[Graph] Download queue size: ");
  Serial.println(pendingPhotoCount_);

  size_t successCount = 0;
  for (size_t i = 0; i < pendingPhotoCount_; ++i) {
    const PendingPhoto& photo = pendingPhotos_[i];
    const String filePath = String(kPhotoDirectory) + "/" + sanitizeFilename(photo.name);
    const String tempFilePath = filePath + ".part";

    Serial.print("[Graph] Preparing to download: ");
    Serial.print(photo.name);
    Serial.print(" (id=");
    Serial.print(photo.itemId);
    Serial.print(", size=");
    Serial.print(photo.sizeBytes);
    Serial.println(" bytes)");

    {
      StorageLock lock(storageMutex_);
      if (SD.exists(filePath)) {
        File existing = SD.open(filePath, FILE_READ);
        const bool sizeMatches = existing && existing.size() == photo.sizeBytes;
        const bool contentOk = sizeMatches && (!isJpegName(photo.name) || endsWithJpegEoi(existing));
        if (existing) {
          existing.close();
        }
        if (contentOk) {
          Serial.println("[Graph] Skipping unchanged file: " + photo.name);
          ++successCount;
          continue;
        }
        Serial.println("[Graph] Local copy missing/incomplete; re-downloading: " + photo.name);
        SD.remove(filePath);
      }
      if (SD.exists(tempFilePath)) {
        SD.remove(tempFilePath);
      }
    }

    String effectiveUrl;
    if (!fetchDownloadUrlForItem(photo.itemId, token.accessToken, effectiveUrl)) {
      Serial.println("[Graph] Could not resolve download URL for item: " + photo.name);
      continue;
    }

    Serial.print("[Graph] Download URL resolved for ");
    Serial.print(photo.name);
    Serial.print(" -> ");
    Serial.println(effectiveUrl.substring(0, 120));
    Serial.print("[Graph] Source file target: ");
    Serial.println(filePath);
    Serial.print("[Graph] Download expected size: ");
    Serial.print(photo.sizeBytes);
    Serial.println(" bytes");


    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, effectiveUrl)) {
      Serial.println("[Graph] Failed to start download request for: " + photo.name);
      continue;
    }

    http.setTimeout(20000);
    http.setConnectTimeout(10000);
    http.setReuse(false);
    http.useHTTP10(true);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setRedirectLimit(5);
    http.setUserAgent("OneDriveFrame/0.1");
    http.addHeader("Connection", "close");
    http.addHeader("Accept", "application/octet-stream");

    const int statusCode = http.GET();
    if (statusCode != 200) {
      const String errorBody = http.getString();
      Serial.print("[Graph] Download failed for ");
      Serial.print(photo.name);
      Serial.print(" with HTTP ");
      Serial.println(statusCode);
      Serial.print("[Graph] Download URL: ");
      Serial.println(effectiveUrl);
      if (!errorBody.isEmpty()) {
        Serial.println("[Graph] Response body:");
        Serial.println(errorBody);
      } else {
        Serial.println("[Graph] Response body is empty.");
      }
      http.end();
      if (statusCode == 401) {
        authFailure_ = true;
        return false;
      }
      continue;
    }

    File outFile;
    {
      StorageLock lock(storageMutex_);
      outFile = SD.open(tempFilePath, FILE_WRITE);
    }
    if (!outFile) {
      Serial.println("[Graph] Failed to open SD temp file for write: " + tempFilePath);
      http.end();
      continue;
    }

    auto* stream = http.getStreamPtr();
    // Allocated once in PSRAM to keep internal RAM free for TLS; only the sync task downloads.
    static uint8_t* buffer = static_cast<uint8_t*>(ps_malloc(kHttpBufferSize));
    // Recent bytes kept so a retry can rewrite data FATFS lost from its buffers after an I/O error.
    constexpr size_t kResumeHistorySize = 8192;
    static uint8_t* history = static_cast<uint8_t*>(ps_malloc(kResumeHistorySize));
    if (buffer == nullptr || history == nullptr) {
      Serial.println("[Graph] Failed to allocate download buffers.");
      {
        StorageLock lock(storageMutex_);
        outFile.close();
        SD.remove(tempFilePath);
      }
      http.end();
      return false;
    }
    size_t historyLen = 0;
    const int contentLength = http.getSize();
    int remaining = contentLength;
    size_t received = 0;
    size_t written = 0;
    uint32_t lastDataMs = millis();
    const char* exitReason = "complete";
    bool writeFailed = false;

    while (remaining > 0 || remaining == -1) {
      const size_t available = stream->available();
      if (available > 0) {
        const size_t toRead = available > kHttpBufferSize ? kHttpBufferSize : available;
        const int readBytes = stream->readBytes(buffer, toRead);
        if (readBytes <= 0) {
          continue;
        }
        const size_t chunkLen = static_cast<size_t>(readBytes);
        received += chunkLen;
        bool chunkOk = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
          StorageLock lock(storageMutex_);
          size_t offset = written;
          if (attempt > 0) {
            // FATFS handles stay failed after EIO and buffered tail data may be lost; reopen and resume
            // from the real on-disk size using recently written bytes still held in memory.
            outFile.close();
            delay(250 * attempt);
            outFile = SD.open(tempFilePath, FILE_APPEND);
            if (!outFile) {
              Serial.println("[Graph] SD reopen for retry failed.");
              continue;
            }
            offset = outFile.size();
            if (offset + historyLen < written || offset > written + chunkLen) {
              Serial.printf("[Graph] SD retry cannot resume: onDisk=%u expected=%u.\n", static_cast<unsigned>(offset),
                            static_cast<unsigned>(written));
              break;
            }
            if (offset < written) {
              // history is a ring indexed by absolute file position; restore may wrap into two writes.
              const size_t missing = written - offset;
              const size_t start = offset % kResumeHistorySize;
              const size_t firstLen = min(missing, kResumeHistorySize - start);
              if (outFile.write(history + start, firstLen) != firstLen ||
                  outFile.write(history, missing - firstLen) != missing - firstLen) {
                Serial.println("[Graph] SD retry failed while restoring recent bytes.");
                continue;
              }
              offset = written;
            }
          }
          const size_t skip = offset - written;
          errno = 0;
          const size_t chunkWritten = outFile.write(buffer + skip, chunkLen - skip);
          if (chunkWritten == chunkLen - skip) {
            chunkOk = true;
            break;
          }
          Serial.printf("[Graph] SD write failed at offset %u: wrote %u/%u errno=%d (%s), attempt %d/4\n",
                        static_cast<unsigned>(offset), static_cast<unsigned>(chunkWritten),
                        static_cast<unsigned>(chunkLen - skip), errno, strerror(errno), attempt + 1);
        }
        if (!chunkOk) {
          exitReason = "sd-write-failed";
          writeFailed = true;
          break;
        }
        {
          const size_t start = written % kResumeHistorySize;
          const size_t firstLen = min(chunkLen, kResumeHistorySize - start);
          memcpy(history + start, buffer, firstLen);
          memcpy(history, buffer + firstLen, chunkLen - firstLen);
        }
        written += chunkLen;
        historyLen = min(written, kResumeHistorySize);
        if (remaining > 0) {
          remaining -= readBytes;
        }
        lastDataMs = millis();
      } else if (!http.connected()) {
        exitReason = remaining == -1 ? "connection-closed" : "connection-closed-early";
        break;
      } else if (millis() - lastDataMs > 20000UL) {
        exitReason = "idle-timeout";
        break;
      } else {
        delay(1);
      }
    }

    size_t fileSize = 0;
    {
      StorageLock lock(storageMutex_);
      outFile.flush();
      outFile.close();
      File check = SD.open(tempFilePath, FILE_READ);
      if (check) {
        fileSize = check.size();
        check.close();
      }
    }
    http.end();

    const size_t expected = photo.sizeBytes > 0 ? photo.sizeBytes
                                                : (contentLength > 0 ? static_cast<size_t>(contentLength) : 0);
    Serial.print("[Graph] Download result for ");
    Serial.print(photo.name);
    Serial.print(": exit=");
    Serial.print(exitReason);
    Serial.print(" expected=");
    Serial.print(expected);
    Serial.print(" contentLength=");
    Serial.print(contentLength);
    Serial.print(" received=");
    Serial.print(received);
    Serial.print(" written=");
    Serial.print(written);
    Serial.print(" fileSize=");
    Serial.println(fileSize);

    bool valid = !writeFailed && expected > 0 && received == expected && written == expected && fileSize == expected;
    if (contentLength > 0 && static_cast<size_t>(contentLength) != expected) {
      valid = false;
    }
    if (valid && isJpegName(photo.name)) {
      StorageLock lock(storageMutex_);
      File check = SD.open(tempFilePath, FILE_READ);
      valid = check && endsWithJpegEoi(check);
      if (check) {
        check.close();
      }
      if (!valid) {
        Serial.println("[Graph] JPEG end marker (FFD9) missing; treating as incomplete.");
      }
    }

    if (!valid) {
      Serial.println("[Graph] Incomplete download discarded: " + photo.name);
      StorageLock lock(storageMutex_);
      SD.remove(tempFilePath);
      continue;
    }

    {
      StorageLock lock(storageMutex_);
      if (SD.exists(filePath) && !SD.remove(filePath)) {
        Serial.println("[Graph] Failed to replace existing file: " + filePath);
        SD.remove(tempFilePath);
        continue;
      }
      if (!SD.rename(tempFilePath, filePath)) {
        Serial.println("[Graph] Failed to rename temp file into place: " + photo.name);
        SD.remove(tempFilePath);
        continue;
      }
      if (catalog_ == nullptr ||
          !catalog_->markDownloaded(photo.itemId, photo.name, filePath, static_cast<uint32_t>(written))) {
        Serial.println("[Graph] Failed to mark download in catalog: " + photo.name);
      }
    }

    Serial.print("[Graph] Saved ");
    Serial.print(photo.name);
    Serial.print(" (");
    Serial.print(written);
    Serial.println(" bytes)");

    ++successCount;
    Serial.print("[Graph] Download progress: ");
    Serial.print(successCount);
    Serial.print("/");
    Serial.println(pendingPhotoCount_);
  }

  Serial.print("[Graph] Downloaded files this sync: ");
  Serial.print(successCount);
  Serial.print("/");
  Serial.println(pendingPhotoCount_);
  return successCount > 0;
}

bool GraphClient::refreshAccessToken(OAuthToken& token) {
  authFailure_ = false;
  if (!isClientIdConfigured()) {
    Serial.println("[OAuth] Configure CLIENT_ID before attempting token refresh.");
    return false;
  }

  if (token.refreshToken.isEmpty()) {
    Serial.println("[OAuth] No refresh token available.");
    return false;
  }

  Serial.println("[OAuth] Refresh token exists; attempting refresh.");

  String body = "client_id=" + urlEncode(String(CLIENT_ID));
  body += "&grant_type=refresh_token";
  body += "&refresh_token=" + urlEncode(token.refreshToken);
  body += "&scope=" + urlEncode(String(kGraphScope));

  int statusCode = 0;
  String responseBody;
  if (!postForm(String(kTokenEndpoint), body, statusCode, responseBody)) {
    return false;
  }

  if (statusCode != 200) {
    Serial.print("[OAuth] Token refresh failed with HTTP status ");
    Serial.println(statusCode);
    Serial.println(responseBody);
    return false;
  }

  if (!parseTokenResponse(responseBody, token)) {
    return false;
  }

  Serial.println("[OAuth] Token refresh succeeded.");
  return true;
}

bool GraphClient::authenticateWithDeviceCode(OAuthToken& token) {
  authFailure_ = false;
  if (!isClientIdConfigured()) {
    Serial.println("[OAuth] Configure CLIENT_ID before starting device-code login.");
    return false;
  }

  Serial.println("[OAuth] Starting device-code flow.");

  String body = "client_id=" + urlEncode(String(CLIENT_ID));
  body += "&scope=" + urlEncode(String(kGraphScope));

  int statusCode = 0;
  String responseBody;
  if (!postForm(String(kDeviceCodeEndpoint), body, statusCode, responseBody)) {
    return false;
  }

  if (statusCode != 200) {
    Serial.print("[OAuth] Device-code request failed with HTTP status ");
    Serial.println(statusCode);
    Serial.println(responseBody);
    return false;
  }

  JsonDocument deviceCodeDoc;
  DeserializationError parseErr = deserializeJson(deviceCodeDoc, responseBody);
  if (parseErr) {
    Serial.print("[OAuth] Failed to parse device-code response: ");
    Serial.println(parseErr.c_str());
    return false;
  }

  const String deviceCode = deviceCodeDoc["device_code"] | "";
  const String userCode = deviceCodeDoc["user_code"] | "";
  const String verificationUri = deviceCodeDoc["verification_uri"] | "";
  const String message = deviceCodeDoc["message"] | "";
  uint32_t intervalSeconds = deviceCodeDoc["interval"] | (kDefaultDeviceCodePollMs / 1000U);
  const uint32_t expiresInSeconds = deviceCodeDoc["expires_in"] | 900U;

  if (deviceCode.isEmpty() || userCode.isEmpty() || verificationUri.isEmpty()) {
    Serial.println("[OAuth] Device-code response missing required fields.");
    return false;
  }

  if (!message.isEmpty()) {
    Serial.println("[OAuth] " + message);
  } else {
    Serial.println("[OAuth] Complete sign-in on another device:");
    Serial.println("[OAuth] URL: " + verificationUri);
    Serial.println("[OAuth] Code: " + userCode);
  }

  const uint32_t pollDeadlineMs = millis() + (expiresInSeconds * 1000UL);
  while (millis() < pollDeadlineMs) {
    delay(intervalSeconds * 1000UL);

    String tokenBody = "grant_type=urn:ietf:params:oauth:grant-type:device_code";
    tokenBody += "&client_id=" + urlEncode(String(CLIENT_ID));
    tokenBody += "&device_code=" + urlEncode(deviceCode);

    int tokenStatus = 0;
    String tokenResponse;
    if (!postForm(String(kTokenEndpoint), tokenBody, tokenStatus, tokenResponse)) {
      continue;
    }

    if (tokenStatus == 200) {
      if (!parseTokenResponse(tokenResponse, token)) {
        return false;
      }
      Serial.println("[OAuth] Device-code sign-in completed.");
      return true;
    }

    JsonDocument errorDoc;
    if (deserializeJson(errorDoc, tokenResponse)) {
      Serial.print("[OAuth] Token polling HTTP ");
      Serial.println(tokenStatus);
      Serial.println(tokenResponse);
      return false;
    }

    const String errorCode = errorDoc["error"] | "";
    if (errorCode == "authorization_pending") {
      continue;
    }

    if (errorCode == "slow_down") {
      intervalSeconds += 5U;
      continue;
    }

    Serial.print("[OAuth] Device-code flow failed: ");
    Serial.println(errorCode);
    Serial.println(tokenResponse);
    return false;
  }

  Serial.println("[OAuth] Device-code flow timed out before authorization completed.");
  return false;
}

bool GraphClient::consumeAuthFailure() {
  const bool value = authFailure_;
  authFailure_ = false;
  return value;
}

}  // namespace odf
