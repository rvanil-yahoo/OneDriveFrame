#include "PhotoStorage.h"

#include <SD.h>
#include <SPI.h>

#include "FrameConfig.h"

namespace odf {

namespace {

const char* cardTypeName(uint8_t type) {
  switch (type) {
    case CARD_MMC:
      return "MMC";
    case CARD_SD:
      return "SDSC";
    case CARD_SDHC:
      return "SDHC/SDXC";
    default:
      return "UNKNOWN";
  }
}

}  // namespace

bool PhotoStorage::begin() {
  if (isMounted_) {
    return true;
  }

  pinMode(kSdChipSelectPin, OUTPUT);
  digitalWrite(kSdChipSelectPin, HIGH);
  pinMode(kRtpChipSelectPin, OUTPUT);
  digitalWrite(kRtpChipSelectPin, HIGH);

  SPI.begin(kSdClockPin, kSdMisoPin, kSdMosiPin, kSdChipSelectPin);
  SD.end();
  Serial.print("[Storage] Trying SD mount at ");
  Serial.print(kSdSpiFrequencyHz);
  Serial.println(" Hz");
  bool mounted = SD.begin(kSdChipSelectPin, SPI, kSdSpiFrequencyHz, "/sd", 5, false);

  if (!mounted) {
    if (kSdFormatIfEmpty) {
      Serial.println("[Storage] Trying one-time SD format and remount (this erases card contents).");
      if (SD.begin(kSdChipSelectPin, SPI, 400000UL, "/sd", 5, true)) {
        Serial.println("[Storage] SD card formatted and mounted successfully.");
        mounted = true;
      } else {
        SD.end();
      }
    }
  }

  if (!mounted) {
    Serial.println("[Storage] Failed to mount SD card over SPI.");
    Serial.println("[Storage] Check SD wiring/pin mapping and CS pin for your board.");
    Serial.print("[Storage] Current CS pin: ");
    Serial.println(kSdChipSelectPin);
    Serial.print("[Storage] RTP CS forced HIGH on pin: ");
    Serial.println(kRtpChipSelectPin);
    Serial.print("[Storage] SPI pins SCK/MISO/MOSI: ");
    Serial.print(kSdClockPin);
    Serial.print("/");
    Serial.print(kSdMisoPin);
    Serial.print("/");
    Serial.println(kSdMosiPin);
    return false;
  }

  const uint8_t type = SD.cardType();
  if (type == CARD_NONE) {
    Serial.println("[Storage] No SD card detected.");
    return false;
  }

  Serial.print("[Storage] Card type: ");
  Serial.println(cardTypeName(type));
  Serial.print("[Storage] Card size (MB): ");
  Serial.println(SD.cardSize() / (1024UL * 1024UL));

  if (!SD.exists(kPhotoDirectory)) {
    if (!SD.mkdir(kPhotoDirectory)) {
      Serial.println("[Storage] Failed to create photo directory.");
      return false;
    }
  }

  if (!catalog_.begin()) {
    Serial.println("[Storage] Failed to initialize photo catalog.");
    return false;
  }
  if (!catalog_.reconcileWithStorage()) {
    Serial.println("[Storage] Failed to reconcile photo catalog.");
    return false;
  }

  isMounted_ = true;
  Serial.println("[Storage] SD card mounted and photo directory ready.");
  return true;
}

uint64_t PhotoStorage::currentUsageBytes() const {
  File root = SD.open(kPhotoDirectory);
  if (!root || !root.isDirectory()) {
    return 0;
  }

  uint64_t total = 0;
  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      total += entry.size();
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  return total;
}

bool PhotoStorage::pruneOldestPhotos() {
  const uint64_t usage = currentUsageBytes();
  if (usage < kMaxLocalUsageBytes) {
    Serial.println("[Storage] Current usage is within quota.");
    return true;
  }

  Serial.println("[Storage] Pruning local photos to stay under the configured quota.");
  Serial.println("[Storage] Stub: implement age-based deletion of the oldest local files here.");
  return true;
}

void PhotoStorage::displaySlideshow() {
  Serial.println("[Display] Starting slideshow from " + String(kPhotoDirectory));
  Serial.println("[Display] Stub: implement JPEG decode, TFT/LCD updates, and timing loop here.");
}

}  // namespace odf
