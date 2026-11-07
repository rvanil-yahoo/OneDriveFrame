#include "PhotoStorage.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <SD.h>
#include <SPI.h>
#include <TJpg_Decoder.h>
#include <esp_display_panel.hpp>

#include "FrameConfig.h"

namespace odf {

namespace {

std::unique_ptr<esp_panel::board::Board> gDisplayBoard;
esp_panel::drivers::LCD* gLcd = nullptr;

struct RenderContext {
  int frameWidth = 0;
  int frameHeight = 0;
  int srcWidth = 0;
  int srcHeight = 0;
  int dstWidth = 0;
  int dstHeight = 0;
  int offsetX = 0;
  int offsetY = 0;
  float scaleX = 1.0f;
  float scaleY = 1.0f;
  bool active = false;
};

RenderContext gRender;

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

bool isJpegFile(const String& path) {
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".jpg") || lower.endsWith(".jpeg");
}

struct DecodedImageBuffer {
  uint16_t* pixels = nullptr;
  uint16_t width = 0;
  uint16_t height = 0;
  bool active = false;
};

DecodedImageBuffer gDecodedImage;

bool tftOutputBuffer(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (bitmap == nullptr || w == 0 || h == 0 || !gDecodedImage.active || gDecodedImage.pixels == nullptr ||
      gDecodedImage.width == 0 || gDecodedImage.height == 0) {
    return false;
  }

  if (x < 0 || y < 0) {
    return true;
  }

  const uint16_t maxX = static_cast<uint16_t>(std::min<int>(x + w, gDecodedImage.width));
  const uint16_t maxY = static_cast<uint16_t>(std::min<int>(y + h, gDecodedImage.height));
  for (uint16_t row = y; row < maxY; ++row) {
    const uint16_t srcRow = row - y;
    const uint16_t dstIndex = static_cast<uint16_t>(row) * gDecodedImage.width;
    const uint16_t srcOffset = srcRow * w;
    for (uint16_t col = x; col < maxX; ++col) {
      const uint16_t srcCol = col - x;
      const uint16_t srcIndex = srcOffset + srcCol;
      if (srcIndex < static_cast<uint16_t>(w * h)) {
        gDecodedImage.pixels[dstIndex + col] = bitmap[srcIndex];
      }
    }
  }

  return true;
}

uint8_t chooseJpegScale(uint16_t sourceW, uint16_t sourceH, int frameW, int frameH) {
  const int panelW = std::max(1, frameW);
  const int panelH = std::max(1, frameH);

  uint8_t scale = 0;
  while (scale < 3) {
    const uint16_t scaledW = sourceW >> scale;
    const uint16_t scaledH = sourceH >> scale;
    if (scaledW <= static_cast<uint16_t>(panelW) && scaledH <= static_cast<uint16_t>(panelH)) {
      break;
    }
    ++scale;
  }

  return scale;
}

uint16_t scaledDimension(uint16_t value, uint8_t scale) {
  const uint16_t result = value >> scale;
  return result == 0 ? 1 : result;
}

bool fitToPanel(uint16_t srcW, uint16_t srcH, uint16_t frameW, uint16_t frameH, uint16_t& drawW, uint16_t& drawH) {
  if (srcW == 0 || srcH == 0 || frameW == 0 || frameH == 0) {
    return false;
  }

  if (srcW >= srcH) {
    drawW = frameW;
    drawH = static_cast<uint16_t>(std::max(1, static_cast<int>((static_cast<uint32_t>(srcH) * drawW + srcW / 2) / srcW)));
    if (drawH > frameH) {
      drawH = frameH;
      drawW = static_cast<uint16_t>(std::max(1, static_cast<int>((static_cast<uint32_t>(srcW) * drawH + srcH / 2) / srcH)));
    }
  } else {
    drawH = frameH;
    drawW = static_cast<uint16_t>(std::max(1, static_cast<int>((static_cast<uint32_t>(srcW) * drawH + srcH / 2) / srcH)));
    if (drawW > frameW) {
      drawW = frameW;
      drawH = static_cast<uint16_t>(std::max(1, static_cast<int>((static_cast<uint32_t>(srcH) * drawW + srcW / 2) / srcW)));
    }
  }

  return drawW > 0 && drawH > 0;
}

void resizeRgb565Nearest(const uint16_t* src, uint16_t srcW, uint16_t srcH, uint16_t dstW, uint16_t dstH, uint16_t* dst) {
  if (src == nullptr || dst == nullptr || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) {
    return;
  }

  for (uint16_t y = 0; y < dstH; ++y) {
    const uint16_t srcY = static_cast<uint16_t>((static_cast<uint32_t>(y) * srcH) / dstH);
    const uint16_t srcYIndex = srcY * srcW;
    for (uint16_t x = 0; x < dstW; ++x) {
      const uint16_t srcX = static_cast<uint16_t>((static_cast<uint32_t>(x) * srcW) / dstW);
      dst[y * dstW + x] = src[srcYIndex + srcX];
    }
  }
}

}  // namespace

bool PhotoStorage::begin() {
  if (isMounted_) {
    Serial.println("[Storage] SD already mounted.");
    return true;
  }

  Serial.println("[Storage] Initializing SD SPI bus...");
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
    Serial.print("[Storage] Creating photo directory: ");
    Serial.println(kPhotoDirectory);
    if (!SD.mkdir(kPhotoDirectory)) {
      Serial.println("[Storage] Failed to create photo directory.");
      return false;
    }
  }

  Serial.println("[Storage] Initializing SQLite photo catalog...");
  if (!catalog_.begin()) {
    Serial.println("[Storage] Failed to initialize photo catalog.");
    Serial.println("[Storage] Verify /sd/photos path is writable and catalog.db can be created.");
    return false;
  }
  Serial.println("[Storage] Reconciling catalog with current SD files...");
  if (!catalog_.reconcileWithStorage()) {
    Serial.println("[Storage] Failed to reconcile photo catalog.");
    return false;
  }

  isMounted_ = true;
  Serial.println("[Storage] SD card mounted and photo directory ready.");
  return true;
}

uint64_t PhotoStorage::currentUsageBytes() const {
  return const_cast<PhotoCatalog&>(catalog_).totalLocalBytes();
}

bool PhotoStorage::pruneOldestPhotos() {
  size_t removedStaleCount = 0;
  uint64_t removedStaleBytes = 0;
  if (!catalog_.removeFilesNotInRemoteCatalog(removedStaleCount, removedStaleBytes)) {
    Serial.println("[Storage] Failed to prune files missing from remote catalog.");
    return false;
  }

  if (removedStaleCount > 0) {
    Serial.print("[Storage] Removed files not in remote catalog: ");
    Serial.print(removedStaleCount);
    Serial.print(" (");
    Serial.print(removedStaleBytes);
    Serial.println(" bytes)");
  }

  const uint64_t usage = currentUsageBytes();
  if (usage <= kMaxLocalUsageBytes) {
    Serial.println("[Storage] Current usage is within quota.");
    return true;
  }

  size_t removedQuotaCount = 0;
  uint64_t removedQuotaBytes = 0;
  if (!catalog_.pruneOldestLocalFilesToQuota(kMaxLocalUsageBytes, removedQuotaCount, removedQuotaBytes)) {
    Serial.println("[Storage] Failed to prune oldest local files to quota.");
    return false;
  }

  if (removedQuotaCount > 0) {
    Serial.print("[Storage] Removed oldest files for quota: ");
    Serial.print(removedQuotaCount);
    Serial.print(" (");
    Serial.print(removedQuotaBytes);
    Serial.println(" bytes)");
  } else {
    Serial.println("[Storage] Quota exceeded but no eligible files were pruned.");
  }

  return currentUsageBytes() <= kMaxLocalUsageBytes;
}

bool PhotoStorage::refreshPhotoList() {
  photoFileCount_ = 0;
  File root = SD.open(kPhotoDirectory);
  if (!root || !root.isDirectory()) {
    if (root) {
      root.close();
    }
    return false;
  }

  File entry = root.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      String path = String(entry.path());
      if (isJpegFile(path) && photoFileCount_ < kMaxPhotoFiles) {
        photoFiles_[photoFileCount_++] = path;
      }
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();

  for (size_t i = 1; i < photoFileCount_; ++i) {
    String key = photoFiles_[i];
    size_t j = i;
    while (j > 0 && photoFiles_[j - 1] > key) {
      photoFiles_[j] = photoFiles_[j - 1];
      --j;
    }
    photoFiles_[j] = key;
  }

  if (slideshowIndex_ >= photoFileCount_) {
    slideshowIndex_ = 0;
  }
  return true;
}

bool PhotoStorage::showPhoto(const String& path) {
  if (!kEnableDisplayOutput) {
    Serial.println("[Display] Output disabled; slideshow selected: " + path);
    return true;
  }

  if (gLcd == nullptr) {
    Serial.println("[Display] LCD not ready; cannot render: " + path);
    return false;
  }

  uint16_t w = 0;
  uint16_t h = 0;
  int x = 0;
  int y = 0;
  uint8_t jpegScale = 0;

  const JRESULT sizeResult = TJpgDec.getSdJpgSize(&w, &h, path.c_str());
  if (sizeResult == JDR_OK) {
    const int frameWidth = gLcd->getFrameWidth();
    const int frameHeight = gLcd->getFrameHeight();
    jpegScale = chooseJpegScale(w, h, frameWidth, frameHeight);
    TJpgDec.setJpgScale(jpegScale);

    const uint16_t decodedW = scaledDimension(w, jpegScale);
    const uint16_t decodedH = scaledDimension(h, jpegScale);

    gDecodedImage.width = decodedW;
    gDecodedImage.height = decodedH;
    gDecodedImage.active = true;
    const size_t bufferSize = static_cast<size_t>(decodedW) * static_cast<size_t>(decodedH);
    gDecodedImage.pixels = new uint16_t[bufferSize];
    if (gDecodedImage.pixels == nullptr) {
      gDecodedImage.active = false;
      Serial.println("[Display] Failed to allocate JPEG decode buffer.");
      return false;
    }
    memset(gDecodedImage.pixels, 0, bufferSize * sizeof(uint16_t));

    uint16_t fitW = decodedW;
    uint16_t fitH = decodedH;
    fitToPanel(decodedW, decodedH, static_cast<uint16_t>(frameWidth), static_cast<uint16_t>(frameHeight), fitW, fitH);

    gRender.frameWidth = frameWidth;
    gRender.frameHeight = frameHeight;
    gRender.srcWidth = decodedW;
    gRender.srcHeight = decodedH;
    gRender.dstWidth = fitW;
    gRender.dstHeight = fitH;
    gRender.offsetX = (frameWidth - fitW) / 2;
    gRender.offsetY = (frameHeight - fitH) / 2;
    gRender.scaleX = 1.0f;
    gRender.scaleY = 1.0f;
    gRender.active = true;

    Serial.print("[Display] Render ");
    Serial.print(path);
    Serial.print(" src=");
    Serial.print(w);
    Serial.print("x");
    Serial.print(h);
    Serial.print(" scale=1/");
    Serial.print(1 << jpegScale);
    Serial.print(" fit=");
    Serial.print(fitW);
    Serial.print("x");
    Serial.println(fitH);
  } else {
    TJpgDec.setJpgScale(0);
    gDecodedImage.active = false;
    gRender.active = false;
    Serial.print("[Display] Failed to read JPEG size for ");
    Serial.print(path);
    Serial.print(" (JRESULT=");
    Serial.print(static_cast<int>(sizeResult));
    Serial.println(")");
  }

  if (gLcd != nullptr) {
    const int frameWidth = gLcd->getFrameWidth();
    const int frameHeight = gLcd->getFrameHeight();
    static std::unique_ptr<uint16_t[]> blackRow;
    static int blackRowWidth = 0;
    if (frameWidth > 0 && frameWidth != blackRowWidth) {
      blackRow = std::make_unique<uint16_t[]>(static_cast<size_t>(frameWidth));
      memset(blackRow.get(), 0, static_cast<size_t>(frameWidth) * sizeof(uint16_t));
      blackRowWidth = frameWidth;
    }
    for (int row = 0; row < frameHeight; ++row) {
      if (blackRow != nullptr) {
        gLcd->drawBitmap(0, row, frameWidth, 1, reinterpret_cast<uint8_t*>(blackRow.get()), -1);
      }
    }
  }

  const JRESULT drawResult = TJpgDec.drawSdJpg(0, 0, path.c_str());
  if (drawResult != JDR_OK) {
    Serial.print("[Display] JPEG decode failed for: ");
    Serial.print(path);
    Serial.print(" (JRESULT=");
    Serial.print(static_cast<int>(drawResult));
    Serial.println(")");
    return false;
  }

  if (gDecodedImage.pixels != nullptr && gDecodedImage.active) {
    const uint16_t finalW = gRender.dstWidth;
    const uint16_t finalH = gRender.dstHeight;
    std::unique_ptr<uint16_t[]> scaledPixels = std::make_unique<uint16_t[]>(static_cast<size_t>(finalW) * static_cast<size_t>(finalH));
    resizeRgb565Nearest(gDecodedImage.pixels, gDecodedImage.width, gDecodedImage.height, finalW, finalH, scaledPixels.get());
    if (gLcd != nullptr) {
      gLcd->drawBitmap(gRender.offsetX, gRender.offsetY, finalW, finalH, reinterpret_cast<uint8_t*>(scaledPixels.get()), -1);
    }
    delete[] gDecodedImage.pixels;
    gDecodedImage.pixels = nullptr;
    gDecodedImage.width = 0;
    gDecodedImage.height = 0;
    gDecodedImage.active = false;
  }

  Serial.println("[Display] Showing " + path);
  return true;
}

void PhotoStorage::displaySlideshow() {
  if (!isMounted_) {
    return;
  }

  if (!displayInitialized_) {
    if (kEnableDisplayOutput) {
      if (!psramFound()) {
        Serial.println("[Display] PSRAM not detected. RGB framebuffer allocation may fail.");
      } else {
        Serial.print("[Display] PSRAM detected (bytes): ");
        Serial.println(ESP.getPsramSize());
      }
      gDisplayBoard = std::make_unique<esp_panel::board::Board>();
      if (!gDisplayBoard->init()) {
        Serial.println("[Display] Failed to initialize RGB display board metadata.");
      }

      esp_panel::drivers::LCD* lcdForConfig = gDisplayBoard->getLCD();
      if (lcdForConfig != nullptr) {
        lcdForConfig->configFrameBufferNumber(1);
      }

      bool boardReady = false;
      if (!gDisplayBoard->begin()) {
        Serial.println("[Display] Board begin failed; retrying LCD-only init without touch controller.");
        if (gDisplayBoard->init()) {
          gLcd = gDisplayBoard->getLCD();
          if (gLcd != nullptr) {
            if (!gLcd->begin()) {
              Serial.println("[Display] LCD-only begin failed.");
            } else {
              boardReady = true;
            }
          }
        }
      } else {
        boardReady = true;
        gLcd = gDisplayBoard->getLCD();
      }

      if (!boardReady) {
        Serial.println("[Display] Failed to initialize RGB display board.");
      }
      if (gLcd == nullptr) {
        Serial.println("[Display] LCD driver unavailable after board init.");
      } else {
        gLcd->swapXY(false);
        gLcd->mirrorX(kDisplayRotation == 1 || kDisplayRotation == 2);
        gLcd->mirrorY(kDisplayRotation == 2 || kDisplayRotation == 3);
        if (gLcd->isFunctionSupported(esp_panel::drivers::LCD::BasicBusSpecification::FUNC_DISPLAY_ON_OFF)) {
          gLcd->setDisplayOnOff(true);
        }
      }
      TJpgDec.setSwapBytes(false);
      TJpgDec.setCallback(tftOutputBuffer);
      Serial.println("[Display] RGB display initialized for slideshow.");
    } else {
      Serial.println("[Display] Slideshow output is disabled in FrameConfig.");
    }
    displayInitialized_ = true;
  }

  const uint32_t now = millis();
  if (lastSlideAtMs_ != 0 && (now - lastSlideAtMs_) < kSlideDurationMs) {
    return;
  }

  if (!refreshPhotoList() || photoFileCount_ == 0) {
    static uint32_t lastNoPhotoLogMs = 0;
    if (lastNoPhotoLogMs == 0 || now - lastNoPhotoLogMs > 10000UL) {
      Serial.println("[Display] No JPEG photos available for slideshow.");
      lastNoPhotoLogMs = now;
    }
    return;
  }

  const String path = photoFiles_[slideshowIndex_];
  slideshowIndex_ = (slideshowIndex_ + 1) % photoFileCount_;
  if (showPhoto(path)) {
    lastSlideAtMs_ = now;
  }
}

}  // namespace odf
