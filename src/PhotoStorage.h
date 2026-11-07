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
  bool refreshPhotoList();
  bool showPhoto(const String& path);

  static constexpr size_t kMaxPhotoFiles = 256;
  bool isMounted_ = false;
  bool displayInitialized_ = false;
  size_t photoFileCount_ = 0;
  size_t slideshowIndex_ = 0;
  uint32_t lastSlideAtMs_ = 0;
  String photoFiles_[kMaxPhotoFiles];
  PhotoCatalog catalog_;
};

}  // namespace odf
