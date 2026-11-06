#pragma once

#include <Arduino.h>

namespace odf {

class PhotoStorage {
 public:
  bool begin();
  uint64_t currentUsageBytes() const;
  bool pruneOldestPhotos();
  void displaySlideshow();
};

}  // namespace odf
