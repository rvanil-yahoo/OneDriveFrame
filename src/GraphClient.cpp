#include "GraphClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "FrameConfig.h"

namespace odf {

namespace {

constexpr const char* kPlaceholderClientId = "00000000-0000-0000-0000-000000000000";

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

}  // namespace

bool GraphClient::listFolder() {
  Serial.println("[Graph] Listing OneDrive folder: " + String(kOneDriveFolderName));
  Serial.print("[Graph] Endpoint: ");
  Serial.println(String(kGraphBaseUrl) + "/me/drive/root:/" + kOneDriveFolderName + ":/children");
  Serial.println("[Graph] Stub: implement Microsoft Graph HTTP request and parse JSON here.");
  return true;
}

bool GraphClient::downloadNewestPhotos() {
  Serial.println("[Graph] Downloading newest photos that fit within the quota.");
  Serial.println("[Graph] Stub: implement file-by-file Graph download and SD write logic here.");
  return true;
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
