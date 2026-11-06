#include "GraphClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD.h>
#include <WiFiClientSecure.h>

#include "FrameConfig.h"

namespace odf {

namespace {

constexpr const char* kPlaceholderClientId = "00000000-0000-0000-0000-000000000000";
constexpr size_t kHttpBufferSize = 2048;

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
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("[OAuth] Failed to initialize HTTPS client.");
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  statusCode = http.POST(formBody);
  responseBody = http.getString();
  http.end();

  if (statusCode <= 0) {
    Serial.print("[OAuth] HTTPS request failed: ");
    Serial.println(statusCode);
    return false;
  }

  return true;
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

bool getWithBearer(const String& url, const String& token, int& statusCode, String& responseBody) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("[Graph] Failed to initialize HTTPS client.");
    return false;
  }

  http.setTimeout(15000);
  http.setConnectTimeout(10000);
  http.setReuse(false);
  http.useHTTP10(true);
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("Accept", "application/json");

  statusCode = http.GET();
  responseBody = http.getString();
  http.end();

  if (statusCode <= 0) {
    Serial.print("[Graph] HTTPS request failed: ");
    Serial.println(statusCode);
    return false;
  }

  return true;
}

}  // namespace

bool GraphClient::listFolder(const OAuthToken& token) {
  pendingPhotoCount_ = 0;
  if (token.accessToken.isEmpty()) {
    Serial.println("[Graph] Missing access token.");
    return false;
  }

  Serial.println("[Graph] Listing OneDrive folder: " + String(kOneDriveFolderName));
  String endpoint = String(kGraphBaseUrl) + "/me/drive/root:/" + urlEncode(String(kOneDriveFolderName)) + ":/children";
  endpoint += "?$select=name,size,lastModifiedDateTime,file,@microsoft.graph.downloadUrl";

  int statusCode = 0;
  String responseBody;
  if (!getWithBearer(endpoint, token.accessToken, statusCode, responseBody)) {
    return false;
  }

  if (statusCode != 200) {
    Serial.print("[Graph] Folder listing failed with HTTP status ");
    Serial.println(statusCode);
    Serial.println(responseBody);
    return false;
  }

  JsonDocument doc;
  DeserializationError parseErr = deserializeJson(doc, responseBody);
  if (parseErr) {
    Serial.print("[Graph] Failed to parse folder list JSON: ");
    Serial.println(parseErr.c_str());
    return false;
  }

  JsonArray items = doc["value"].as<JsonArray>();
  if (items.isNull()) {
    Serial.println("[Graph] Folder list missing 'value' array.");
    return false;
  }

  for (JsonVariant item : items) {
    const bool isFile = !item["file"].isNull();
    const String name = item["name"] | "";
    const String downloadUrl = item["@microsoft.graph.downloadUrl"] | "";
    const String modified = item["lastModifiedDateTime"] | "";
    const uint32_t sizeBytes = item["size"] | 0U;

    if (!isFile || !isAllowedImage(name) || downloadUrl.isEmpty() || modified.isEmpty()) {
      continue;
    }

    if (pendingPhotoCount_ >= (sizeof(pendingPhotos_) / sizeof(pendingPhotos_[0]))) {
      continue;
    }

    pendingPhotos_[pendingPhotoCount_].name = name;
    pendingPhotos_[pendingPhotoCount_].downloadUrl = downloadUrl;
    pendingPhotos_[pendingPhotoCount_].modifiedUtc = modified;
    pendingPhotos_[pendingPhotoCount_].sizeBytes = sizeBytes;
    ++pendingPhotoCount_;
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

  Serial.print("[Graph] Queued image files: ");
  Serial.println(pendingPhotoCount_);
  return pendingPhotoCount_ > 0;
}

bool GraphClient::downloadNewestPhotos(const OAuthToken& token) {
  if (token.accessToken.isEmpty()) {
    Serial.println("[Graph] Missing access token for download.");
    return false;
  }

  if (pendingPhotoCount_ == 0) {
    Serial.println("[Graph] No queued photos to download.");
    return true;
  }

  Serial.println("[Graph] Downloading newest photos that fit within the quota.");

  size_t successCount = 0;
  for (size_t i = 0; i < pendingPhotoCount_; ++i) {
    const PendingPhoto& photo = pendingPhotos_[i];
    const String filePath = String(kPhotoDirectory) + "/" + sanitizeFilename(photo.name);

    if (SD.exists(filePath)) {
      File existing = SD.open(filePath, FILE_READ);
      if (existing && existing.size() == photo.sizeBytes) {
        existing.close();
        Serial.println("[Graph] Skipping unchanged file: " + photo.name);
        ++successCount;
        continue;
      }
      if (existing) {
        existing.close();
      }
      SD.remove(filePath);
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, photo.downloadUrl)) {
      Serial.println("[Graph] Failed to start download request for: " + photo.name);
      continue;
    }

    http.setTimeout(20000);
    http.setConnectTimeout(10000);
    http.setReuse(false);
    http.useHTTP10(true);
    http.setUserAgent("OneDriveFrame/0.1");
    http.addHeader("Accept", "application/octet-stream");

    const int statusCode = http.GET();
    if (statusCode != 200) {
      Serial.print("[Graph] Download failed for ");
      Serial.print(photo.name);
      Serial.print(" with HTTP ");
      Serial.println(statusCode);
      http.end();
      continue;
    }

    File outFile = SD.open(filePath, FILE_WRITE);
    if (!outFile) {
      Serial.println("[Graph] Failed to open SD file for write: " + filePath);
      http.end();
      continue;
    }

    auto* stream = http.getStreamPtr();
    uint8_t buffer[kHttpBufferSize];
    int remaining = http.getSize();
    size_t written = 0;
    uint32_t idleLoops = 0;

    while (http.connected() && (remaining > 0 || remaining == -1)) {
      const size_t available = stream->available();
      if (available > 0) {
        const size_t toRead = available > sizeof(buffer) ? sizeof(buffer) : available;
        const int readBytes = stream->readBytes(buffer, toRead);
        if (readBytes > 0) {
          outFile.write(buffer, static_cast<size_t>(readBytes));
          written += static_cast<size_t>(readBytes);
          if (remaining > 0) {
            remaining -= readBytes;
          }
          idleLoops = 0;
        }
      } else {
        delay(1);
        ++idleLoops;
        if (idleLoops > 20000) {
          break;
        }
      }
    }

    outFile.close();
    http.end();

    if (photo.sizeBytes > 0 && written != photo.sizeBytes) {
      Serial.print("[Graph] Incomplete download for ");
      Serial.print(photo.name);
      Serial.print(". expected=");
      Serial.print(photo.sizeBytes);
      Serial.print(" written=");
      Serial.println(written);
      SD.remove(filePath);
      continue;
    }

    Serial.print("[Graph] Saved ");
    Serial.print(photo.name);
    Serial.print(" (");
    Serial.print(written);
    Serial.println(" bytes)");
    ++successCount;
  }

  Serial.print("[Graph] Downloaded files this sync: ");
  Serial.print(successCount);
  Serial.print("/");
  Serial.println(pendingPhotoCount_);
  return successCount > 0;
}

bool GraphClient::refreshAccessToken(OAuthToken& token) {
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

}  // namespace odf
