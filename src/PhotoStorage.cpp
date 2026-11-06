#include "PhotoStorage.h"

#include <SD.h>

#include "FrameConfig.h"

namespace odf {

bool PhotoStorage::begin() {
  if (!SD.begin(kSdChipSelectPin)) {
    Serial.println("[Storage] Failed to mount SD card.");
    return false;
  }

  if (!SD.exists(kPhotoDirectory)) {
    if (!SD.mkdir(kPhotoDirectory)) {
      Serial.println("[Storage] Failed to create photo directory.");
      return false;
    }
  }

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
