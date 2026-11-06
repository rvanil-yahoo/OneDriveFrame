#pragma once

#include <Arduino.h>

namespace odf {

constexpr const char* kOneDriveFolderName = "ESP32Frame";
constexpr const char* kConsumerAuthority = "https://login.microsoftonline.com/consumers";
constexpr const char* kDeviceCodeEndpoint = "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
constexpr const char* kTokenEndpoint = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
constexpr const char* kGraphBaseUrl = "https://graph.microsoft.com/v1.0";
constexpr const char* kGraphScope = "Files.ReadWrite offline_access";
constexpr const char* kPhotoDirectory = "/photos";
constexpr const char* kConfigPortalName = "OneDriveFrame Setup";
constexpr const char* kConfigPortalPassword = "oneframe";
constexpr uint8_t kSdChipSelectPin = 10;
constexpr uint8_t kSdMosiPin = 11;
constexpr uint8_t kSdClockPin = 12;
constexpr uint8_t kSdMisoPin = 13;
constexpr uint8_t kRtpChipSelectPin = 38;
constexpr uint32_t kSdSpiFrequencyHz = 4000000UL;
constexpr bool kSdFormatIfEmpty = true;
constexpr uint32_t kWifiConnectTimeoutMs = 20000UL;
constexpr uint32_t kWifiPortalTimeoutMs = 300000UL;

constexpr uint32_t kSyncIntervalMs = 3UL * 60UL * 60UL * 1000UL;
constexpr uint32_t kSlideDurationMs = 8UL * 1000UL;
constexpr uint32_t kMaxLocalUsageBytes = 4UL * 1024UL * 1024UL * 1024UL;
constexpr uint32_t kDefaultDeviceCodePollMs = 5000UL;

#ifndef CLIENT_ID
#define CLIENT_ID "e29eee94-f016-4d63-9796-e57d83276541"
#endif

}  // namespace odf
