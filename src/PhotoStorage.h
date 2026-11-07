#pragma once

#include <Arduino.h>

#include "PhotoCatalog.h"

namespace odf {

class PhotoStorage {
 public:
  bool begin();
  uint64_t currentUsageBytes() const;
  bool pruneOldestPhotos();
  void displaySlideshow();

 private:
  bool isMounted_ = false;
  PhotoCatalog catalog_;
};

}  // namespace odf
