#pragma once

#include <Arduino.h>
#include <memory>

#include "PhotoCatalog.h"

namespace odf {

class PhotoStorage {
 public:
  void setCatalog(PhotoCatalog* catalog) { catalog_ = catalog; }
  bool begin();
  bool initializeCatalog();
  uint64_t currentUsageBytes() const;
  bool pruneOldestPhotos();
  void displaySlideshow();

 private:
  bool refreshPhotoList(bool firstPhotoOnly = false);
  bool showPhoto(const String& path);
  bool decodeJpegToRgb565(const String& sourcePath, uint16_t targetW, uint16_t targetH,
                          std::unique_ptr<uint16_t[]>& pixels, uint16_t& outW, uint16_t& outH);

  static constexpr size_t kMaxPhotoFiles = 256;
  bool isMounted_ = false;
  bool displayInitialized_ = false;
  size_t photoFileCount_ = 0;
  size_t slideshowIndex_ = 0;
  uint32_t lastSlideAtMs_ = 0;
  String photoFiles_[kMaxPhotoFiles];
  PhotoCatalog* catalog_ = nullptr;
};

}  // namespace odf
