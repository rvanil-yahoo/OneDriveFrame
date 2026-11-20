#pragma once

#include <Arduino.h>
#include <memory>

#include "PhotoCatalog.h"

namespace odf {

struct PsramPixelDeleter {
  void operator()(uint16_t* pixels) const;
};

using PixelBuffer = std::unique_ptr<uint16_t[], PsramPixelDeleter>;

class PhotoStorage {
 public:
  void setCatalog(PhotoCatalog* catalog) { catalog_ = catalog; }
  bool begin();
  bool initializeCatalog();
  uint64_t currentUsageBytes() const;
  bool pruneOldestPhotos();
  void displaySlideshow();

 private:
  bool findFirstPhoto(String& path);
  bool selectNextPhoto(String& path, size_t& photoCount);
  bool preparePhoto(const String& path, PixelBuffer& pixels, uint16_t& width, uint16_t& height);
  bool showPreparedPhoto(const String& path, const PixelBuffer& pixels, uint16_t width, uint16_t height);
  bool showPhoto(const String& path);
  bool decodeJpegToRgb565(const String& sourcePath, uint16_t targetW, uint16_t targetH,
                          PixelBuffer& pixels, uint16_t& outW, uint16_t& outH);

  bool isMounted_ = false;
  bool catalogReady_ = false;
  bool displayInitialized_ = false;
  uint32_t lastSlideAtMs_ = 0;
  uint32_t lastPreparationMs_ = 0;
  String currentPhotoPath_;
  PhotoCatalog* catalog_ = nullptr;
};

}  // namespace odf
