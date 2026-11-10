#pragma once

#include <Arduino.h>
#include <memory>

#include "PhotoCatalog.h"

namespace odf {

class PhotoStorage {
 public:
  bool begin();
  uint64_t currentUsageBytes() const;
  bool pruneOldestPhotos();
  static bool prepareDisplayReadyCopy(const String& sourcePath);
  static bool prepareDisplayReadyCopyAsync(const String& sourcePath);
  void displaySlideshow();

 private:
  bool refreshPhotoList();
  bool showPhoto(const String& path);
  String readyImagePathFor(const String& sourcePath) const;
  bool ensureDisplayReadyCopy(const String& sourcePath, uint16_t targetW, uint16_t targetH,
                             std::unique_ptr<uint16_t[]>& pixels, uint16_t& readyW, uint16_t& readyH);
  bool loadReadyImage(const String& readyPath, uint16_t expectedW, uint16_t expectedH,
                      std::unique_ptr<uint16_t[]>& pixels, uint16_t& actualW, uint16_t& actualH);
  bool saveReadyImage(const String& readyPath, uint16_t width, uint16_t height, const uint16_t* pixels);
  bool decodeJpegToRgb565(const String& sourcePath, uint16_t targetW, uint16_t targetH,
                          std::unique_ptr<uint16_t[]>& pixels, uint16_t& outW, uint16_t& outH);

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
