#include "PhotoStorage.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <SD.h>
#include <SPI.h>
#include <TJpg_Decoder.h>
#include <esp_display_panel.hpp>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "FrameConfig.h"

namespace odf {

namespace {

std::unique_ptr<esp_panel::board::Board> gDisplayBoard;
esp_panel::drivers::LCD* gLcd = nullptr;
lv_obj_t* gLvCanvas = nullptr;
lv_color_t* gLvCanvasBuffer = nullptr;
lv_color_t* gLvDisplayBuffer = nullptr;
lv_disp_draw_buf_t gLvDisplayDrawBuf;
lv_disp_drv_t gLvDisplayDriver;
uint16_t gLvCanvasWidth = 0;
uint16_t gLvCanvasHeight = 0;
bool gLvglInitialized = false;
TaskHandle_t gReadyCopyTaskHandle = nullptr;
String gReadyCopyQueue[8];
size_t gReadyCopyQueueCount = 0;

constexpr uint16_t kReadyImageMagic = 0x5244;  // "RD"

bool ensureReadyDirectory() {
  const String readyDir = String(kPhotoDirectory) + "/.ready";
  if (SD.exists(readyDir)) {
    return true;
  }
  return SD.mkdir(readyDir);
}

String sanitizeReadyStem(const String& sourcePath) {
  String filename = sourcePath;
  const int lastSlash = filename.lastIndexOf('/');
  if (lastSlash >= 0) {
    filename = filename.substring(lastSlash + 1);
  }
  const int lastDot = filename.lastIndexOf('.');
  if (lastDot > 0) {
    filename = filename.substring(0, lastDot);
  }
  filename.replace(" ", "_");
  filename.replace("/", "_");
  filename.replace("\\", "_");
  filename.replace("::", "_");
  return filename;
}

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
  const size_t sourcePixelCount = static_cast<size_t>(w) * h;
  for (uint16_t row = y; row < maxY; ++row) {
    const uint16_t srcRow = row - y;
    const size_t dstIndex = static_cast<size_t>(row) * gDecodedImage.width;
    const size_t srcOffset = static_cast<size_t>(srcRow) * w;
    for (uint16_t col = x; col < maxX; ++col) {
      const uint16_t srcCol = col - x;
      const size_t srcIndex = srcOffset + srcCol;
      if (srcIndex < sourcePixelCount) {
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

void lvglFlushCallback(lv_disp_drv_t* disp_drv, const lv_area_t* area, lv_color_t* color_p) {
  if (gLcd == nullptr || disp_drv == nullptr || area == nullptr || color_p == nullptr) {
    if (disp_drv != nullptr) {
      lv_disp_flush_ready(disp_drv);
    }
    return;
  }

  const int32_t width = lv_area_get_width(area);
  const int32_t height = lv_area_get_height(area);
  if (width <= 0 || height <= 0) {
    lv_disp_flush_ready(disp_drv);
    return;
  }

  int32_t xStart = area->x1;
  int32_t yStart = area->y1;
  if (xStart < 0) {
    xStart = 0;
  }
  if (yStart < 0) {
    yStart = 0;
  }

  for (int32_t y = 0; y < height; ++y) {
    const int32_t panelY = yStart + y;
    if (panelY >= gLcd->getFrameHeight()) {
      continue;
    }
    const uint8_t* rowPixels = reinterpret_cast<uint8_t*>(color_p + (y * width));
    gLcd->drawBitmap(xStart, panelY, width, 1, const_cast<uint8_t*>(rowPixels), -1);
  }

  lv_disp_flush_ready(disp_drv);
}

bool ensureLvglDisplay() {
  if (gLvglInitialized) {
    return true;
  }

  if (gLcd == nullptr) {
    return false;
  }

  lv_init();
  const int displayWidth = gLcd->getFrameWidth();
  const int displayHeight = gLcd->getFrameHeight();
  const size_t drawBufferPixels = static_cast<size_t>(displayWidth) * displayHeight;
  gLvDisplayBuffer = static_cast<lv_color_t*>(heap_caps_malloc(drawBufferPixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (gLvDisplayBuffer == nullptr) {
    gLvDisplayBuffer = static_cast<lv_color_t*>(malloc(drawBufferPixels * sizeof(lv_color_t)));
  }
  if (gLvDisplayBuffer == nullptr) {
    Serial.println("[LVGL] Failed to allocate display buffer.");
    return false;
  }

  lv_disp_draw_buf_init(&gLvDisplayDrawBuf, gLvDisplayBuffer, nullptr, drawBufferPixels);
  lv_disp_drv_init(&gLvDisplayDriver);
  gLvDisplayDriver.hor_res = displayWidth;
  gLvDisplayDriver.ver_res = displayHeight;
  gLvDisplayDriver.flush_cb = lvglFlushCallback;
  gLvDisplayDriver.draw_buf = &gLvDisplayDrawBuf;
  gLvDisplayDriver.rotated = LV_DISP_ROT_NONE;
  gLvDisplayDriver.antialiasing = 1;
  lv_disp_drv_register(&gLvDisplayDriver);

  gLvglInitialized = true;
  Serial.println("[LVGL] Display initialized and bound to panel output.");
  return true;
}

void updateLvglCanvas(const uint16_t* imagePixels, uint16_t imageW, uint16_t imageH) {
  if (!gLvglInitialized && !ensureLvglDisplay()) {
    return;
  }

  if (imagePixels == nullptr || imageW == 0 || imageH == 0) {
    return;
  }

  if (gLvCanvas == nullptr) {
    gLvCanvas = lv_canvas_create(lv_scr_act());
    if (gLvCanvas == nullptr) {
      return;
    }
  }

  if (gLvCanvasWidth != imageW || gLvCanvasHeight != imageH || gLvCanvasBuffer == nullptr) {
    if (gLvCanvasBuffer != nullptr) {
      heap_caps_free(gLvCanvasBuffer);
      gLvCanvasBuffer = nullptr;
    }
    const size_t canvasBytes = static_cast<size_t>(imageW) * imageH * sizeof(lv_color_t);
    gLvCanvasBuffer = static_cast<lv_color_t*>(heap_caps_malloc(canvasBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (gLvCanvasBuffer == nullptr) {
      gLvCanvasBuffer = static_cast<lv_color_t*>(malloc(canvasBytes));
    }
    if (gLvCanvasBuffer == nullptr) {
      Serial.println("[LVGL] Failed to allocate canvas buffer.");
      return;
    }
    gLvCanvasWidth = imageW;
    gLvCanvasHeight = imageH;
  }

  memcpy(gLvCanvasBuffer, imagePixels, static_cast<size_t>(imageW) * imageH * sizeof(uint16_t));
  lv_canvas_set_buffer(gLvCanvas, gLvCanvasBuffer, imageW, imageH, LV_IMG_CF_TRUE_COLOR);
  lv_obj_center(gLvCanvas);
  lv_obj_set_size(gLvCanvas, imageW, imageH);
  lv_obj_invalidate(gLvCanvas);
  lv_tick_inc(16);
  lv_timer_handler();
}

void resizeRgb565Nearest(const uint16_t* src, uint16_t srcW, uint16_t srcH, uint16_t dstW, uint16_t dstH, uint16_t* dst) {
  if (src == nullptr || dst == nullptr || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) {
    return;
  }

  for (uint16_t y = 0; y < dstH; ++y) {
    const uint16_t srcY = static_cast<uint16_t>((static_cast<uint32_t>(y) * srcH) / dstH);
    const size_t srcYIndex = static_cast<size_t>(srcY) * srcW;
    for (uint16_t x = 0; x < dstW; ++x) {
      const uint16_t srcX = static_cast<uint16_t>((static_cast<uint32_t>(x) * srcW) / dstW);
      dst[static_cast<size_t>(y) * dstW + x] = src[srcYIndex + srcX];
    }
  }
}

}  // namespace

void readyCopyWorker(void* arg) {
  (void)arg;

  while (gReadyCopyQueueCount > 0) {
    const String sourcePath = gReadyCopyQueue[0];
    for (size_t i = 1; i < gReadyCopyQueueCount; ++i) {
      gReadyCopyQueue[i - 1] = gReadyCopyQueue[i];
    }
    gReadyCopyQueue[--gReadyCopyQueueCount] = "";

      Serial.print("[Storage] Starting background ready-copy generation for: ");
    Serial.println(sourcePath);
      uint16_t targetW = 800U;
      uint16_t targetH = 480U;
      if (gLcd != nullptr) {
        targetW = static_cast<uint16_t>(gLcd->getFrameWidth());
        targetH = static_cast<uint16_t>(gLcd->getFrameHeight());
    }
      Serial.print("[Storage] Ready-copy target size: ");
      Serial.print(targetW);
      Serial.print("x");
      Serial.println(targetH);

      const bool ok = PhotoStorage::prepareDisplayReadyCopy(sourcePath);
      if (!ok) {
        Serial.print("[Storage] Background ready-copy generation failed for: ");
        Serial.println(sourcePath);
      }
  }

  gReadyCopyTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

bool PhotoStorage::prepareDisplayReadyCopyAsync(const String& sourcePath) {
  if (sourcePath.isEmpty() || !SD.exists(sourcePath)) {
    return false;
  }

  for (size_t i = 0; i < gReadyCopyQueueCount; ++i) {
    if (gReadyCopyQueue[i] == sourcePath) {
      return true;
    }
  }

  if (gReadyCopyQueueCount >= (sizeof(gReadyCopyQueue) / sizeof(gReadyCopyQueue[0]))) {
    Serial.print("[Storage] Ready-copy queue full; dropping: ");
    Serial.println(sourcePath);
    return false;
  }

  gReadyCopyQueue[gReadyCopyQueueCount++] = sourcePath;
  Serial.print("[Storage] Queued ready-copy generation for: ");
  Serial.println(sourcePath);

  if (gReadyCopyTaskHandle == nullptr) {
    const BaseType_t created = xTaskCreatePinnedToCore(
        readyCopyWorker, "readycopy", 32768U, nullptr, 2, &gReadyCopyTaskHandle, 1);
    if (created != pdPASS) {
      gReadyCopyQueue[--gReadyCopyQueueCount] = "";
      gReadyCopyTaskHandle = nullptr;
      Serial.print("[Storage] Failed to create ready-copy worker for: ");
      Serial.println(sourcePath);
      return false;
    }
  }

  return true;
}

bool PhotoStorage::prepareDisplayReadyCopy(const String& sourcePath) {
  if (sourcePath.isEmpty() || !SD.exists(sourcePath)) {
    return false;
  }

  uint16_t targetW = 800U;
  uint16_t targetH = 480U;
  if (gLcd != nullptr) {
    targetW = static_cast<uint16_t>(gLcd->getFrameWidth());
    targetH = static_cast<uint16_t>(gLcd->getFrameHeight());
  }
  if (targetW == 0 || targetH == 0) {
    return false;
  }

  PhotoStorage storage;
  std::unique_ptr<uint16_t[]> pixels;
  uint16_t readyW = 0;
  uint16_t readyH = 0;
  if (!storage.ensureDisplayReadyCopy(sourcePath, targetW, targetH, pixels, readyW, readyH)) {
    Serial.print("[Storage] Post-download ready-copy generation failed for: ");
    Serial.println(sourcePath);
    if (SD.exists(sourcePath)) {
      Serial.print("[Storage] Source file still exists on SD: ");
      Serial.println(sourcePath);
      File probe = SD.open(sourcePath, FILE_READ);
      if (probe) {
        Serial.print("[Storage] Source file size: ");
        Serial.print(probe.size());
        Serial.println(" bytes");
        uint8_t header[16] = {0};
        const size_t headerLen = probe.read(header, sizeof(header));
        probe.close();
        Serial.print("[Storage] Source file header bytes: ");
        for (size_t i = 0; i < headerLen; ++i) {
          if (i > 0) {
            Serial.print(' ');
          }
          if (header[i] < 0x10) {
            Serial.print('0');
          }
          Serial.print(header[i], HEX);
        }
        Serial.println();
      }
    }
    return false;
  }

  Serial.print("[Storage] Generated display-ready copy for ");
  Serial.print(sourcePath);
  Serial.print(" -> ");
  Serial.print(readyW);
  Serial.print("x");
  Serial.println(readyH);
  gReadyCopyTaskHandle = nullptr;
  return true;
}

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
      if (!isJpegFile(path)) {
        entry.close();
        entry = root.openNextFile();
        continue;
      }

      uint16_t testW = 0;
      uint16_t testH = 0;
      const JRESULT sizeResult = TJpgDec.getSdJpgSize(&testW, &testH, path.c_str());
      if (sizeResult != JDR_OK) {
        Serial.print("[Display] Skipping unreadable JPEG file: ");
        Serial.print(path);
        Serial.print(" (JRESULT=");
        Serial.print(static_cast<int>(sizeResult));
        Serial.print(", size=");
        Serial.print(entry.size());
        Serial.println(" bytes)");
        SD.remove(path);
        entry.close();
        entry = root.openNextFile();
        continue;
      }

      if (photoFileCount_ < kMaxPhotoFiles) {
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

String PhotoStorage::readyImagePathFor(const String& sourcePath) const {
  String readyDir = String(kPhotoDirectory) + "/.ready";
  String base = sanitizeReadyStem(sourcePath);
  return readyDir + "/" + base + ".rgb565";
}

bool PhotoStorage::loadReadyImage(const String& readyPath, uint16_t expectedW, uint16_t expectedH,
                                  std::unique_ptr<uint16_t[]>& pixels, uint16_t& actualW, uint16_t& actualH) {
  if (!SD.exists(readyPath)) {
    return false;
  }

  File imageFile = SD.open(readyPath, FILE_READ);
  if (!imageFile) {
    return false;
  }

  if (imageFile.size() < (long)(sizeof(kReadyImageMagic) + 2 * sizeof(uint16_t) + 2 * sizeof(uint32_t))) {
    imageFile.close();
    return false;
  }

  uint16_t magic = 0;
  imageFile.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
  if (magic != kReadyImageMagic) {
    imageFile.close();
    return false;
  }

  imageFile.read(reinterpret_cast<uint8_t*>(&actualW), sizeof(actualW));
  imageFile.read(reinterpret_cast<uint8_t*>(&actualH), sizeof(actualH));
  if (actualW != expectedW || actualH != expectedH) {
    imageFile.close();
    return false;
  }

  const size_t pixelCount = static_cast<size_t>(actualW) * actualH;
  pixels = std::make_unique<uint16_t[]>(pixelCount);
  if (pixels == nullptr || pixelCount == 0) {
    imageFile.close();
    return false;
  }

  const uint32_t pixelBytes = static_cast<uint32_t>(pixelCount * sizeof(uint16_t));
  if (imageFile.read(reinterpret_cast<uint8_t*>(pixels.get()), pixelBytes) != static_cast<int>(pixelBytes)) {
    pixels.reset();
    imageFile.close();
    return false;
  }

  imageFile.close();
  return true;
}

bool PhotoStorage::saveReadyImage(const String& readyPath, uint16_t width, uint16_t height, const uint16_t* pixels) {
  if (!ensureReadyDirectory()) {
    Serial.println("[Display] Failed to create .ready image cache directory.");
    return false;
  }

  if (width == 0 || height == 0 || pixels == nullptr) {
    return false;
  }

  File imageFile = SD.open(readyPath, FILE_WRITE);
  if (!imageFile) {
    return false;
  }

  uint16_t magic = kReadyImageMagic;
  imageFile.write(reinterpret_cast<uint8_t*>(&magic), sizeof(magic));
  imageFile.write(reinterpret_cast<const uint8_t*>(&width), sizeof(width));
  imageFile.write(reinterpret_cast<const uint8_t*>(&height), sizeof(height));
  const size_t pixelCount = static_cast<size_t>(width) * height;
  imageFile.write(reinterpret_cast<const uint8_t*>(pixels), static_cast<size_t>(pixelCount) * sizeof(uint16_t));
  imageFile.close();
  return true;
}

bool PhotoStorage::decodeJpegToRgb565(const String& sourcePath, uint16_t targetW, uint16_t targetH,
                                      std::unique_ptr<uint16_t[]>& pixels, uint16_t& outW, uint16_t& outH) {
  uint16_t srcW = 0;
  uint16_t srcH = 0;
  const JRESULT sizeResult = TJpgDec.getSdJpgSize(&srcW, &srcH, sourcePath.c_str());
  if (sizeResult != JDR_OK) {
    Serial.print("[Display] Failed to read JPEG size for ");
    Serial.print(sourcePath);
    Serial.print(" (JRESULT=");
    Serial.print(static_cast<int>(sizeResult));
    Serial.println(")");
    Serial.println("[Display] The source file is not a valid JPEG for display; keeping the original and invalidating the ready cache.");
    const String readyPath = readyImagePathFor(sourcePath);
    if (SD.exists(readyPath)) {
      SD.remove(readyPath);
    }
    return false;
  }

  const uint8_t jpegScale = chooseJpegScale(srcW, srcH, targetW, targetH);
  const uint16_t scaledW = scaledDimension(srcW, jpegScale);
  const uint16_t scaledH = scaledDimension(srcH, jpegScale);

  uint16_t fitW = scaledW;
  uint16_t fitH = scaledH;
  if (!fitToPanel(scaledW, scaledH, targetW, targetH, fitW, fitH)) {
    fitW = scaledW;
    fitH = scaledH;
  }

  pixels = std::make_unique<uint16_t[]>(static_cast<size_t>(scaledW) * scaledH);
  if (pixels == nullptr || scaledW == 0 || scaledH == 0) {
    return false;
  }

  gDecodedImage.width = scaledW;
  gDecodedImage.height = scaledH;
  gDecodedImage.pixels = pixels.get();
  gDecodedImage.active = true;

  TJpgDec.setSwapBytes(false);
  TJpgDec.setJpgScale(jpegScale);
  TJpgDec.setCallback([](int16_t x, int16_t y, uint16_t blockW, uint16_t blockH, uint16_t* bitmap) {
    if (bitmap == nullptr || !gDecodedImage.active || gDecodedImage.pixels == nullptr) {
      return false;
    }

    if (x < 0 || y < 0 || x >= static_cast<int16_t>(gDecodedImage.width) || y >= static_cast<int16_t>(gDecodedImage.height)) {
      return true;
    }

    for (uint16_t row = 0; row < blockH; ++row) {
      const int16_t drawY = y + row;
      if (drawY < 0 || drawY >= static_cast<int16_t>(gDecodedImage.height)) {
        continue;
      }
      for (uint16_t col = 0; col < blockW; ++col) {
        const int16_t drawX = x + col;
        if (drawX < 0 || drawX >= static_cast<int16_t>(gDecodedImage.width)) {
          continue;
        }
        gDecodedImage.pixels[static_cast<size_t>(drawY) * gDecodedImage.width + drawX] = bitmap[row * blockW + col];
      }
    }
    return true;
  });

  const JRESULT renderResult = TJpgDec.drawSdJpg(0, 0, sourcePath.c_str());
  gDecodedImage.active = false;
  gDecodedImage.pixels = nullptr;
  gDecodedImage.width = 0;
  gDecodedImage.height = 0;

  if (renderResult != JDR_OK) {
    Serial.print("[Display] JPEG decode failed for: ");
    Serial.print(sourcePath);
    Serial.print(" (JRESULT=");
    Serial.print(static_cast<int>(renderResult));
    Serial.println(")");
    Serial.println("[Display] Keeping the original source file; only invalidating the ready-image cache so the slideshow can continue.");
    const String readyPath = readyImagePathFor(sourcePath);
    if (SD.exists(readyPath)) {
      SD.remove(readyPath);
    }
    return false;
  }

  if (scaledW == fitW && scaledH == fitH) {
    outW = scaledW;
    outH = scaledH;
    return true;
  }

  std::unique_ptr<uint16_t[]> resized = std::make_unique<uint16_t[]>(static_cast<size_t>(fitW) * fitH);
  if (resized == nullptr || fitW == 0 || fitH == 0) {
    return false;
  }

  resizeRgb565Nearest(pixels.get(), scaledW, scaledH, fitW, fitH, resized.get());
  pixels = std::move(resized);
  outW = fitW;
  outH = fitH;
  return true;
}

bool PhotoStorage::ensureDisplayReadyCopy(const String& sourcePath, uint16_t targetW, uint16_t targetH,
                                         std::unique_ptr<uint16_t[]>& pixels, uint16_t& readyW, uint16_t& readyH) {
  if (targetW == 0 || targetH == 0) {
    return false;
  }

  const String readyPath = readyImagePathFor(sourcePath);
  if (loadReadyImage(readyPath, targetW, targetH, pixels, readyW, readyH)) {
    return true;
  }

  if (!decodeJpegToRgb565(sourcePath, targetW, targetH, pixels, readyW, readyH)) {
    return false;
  }

  saveReadyImage(readyPath, readyW, readyH, pixels.get());
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

  uint16_t srcW = 0;
  uint16_t srcH = 0;
  const JRESULT sizeResult = TJpgDec.getSdJpgSize(&srcW, &srcH, path.c_str());
  if (sizeResult != JDR_OK) {
    Serial.print("[Display] Failed to read JPEG size for ");
    Serial.print(path);
    Serial.print(" (JRESULT=");
    Serial.print(static_cast<int>(sizeResult));
    Serial.println(")");
    return false;
  }

  const int frameWidth = gLcd->getFrameWidth();
  const int frameHeight = gLcd->getFrameHeight();
  uint16_t readyW = 0;
  uint16_t readyH = 0;
  if (!fitToPanel(srcW, srcH, static_cast<uint16_t>(frameWidth), static_cast<uint16_t>(frameHeight), readyW, readyH)) {
    Serial.print("[Display] Could not calculate fit dimensions for ");
    Serial.println(path);
    return false;
  }

  std::unique_ptr<uint16_t[]> readyPixels;
  if (!decodeJpegToRgb565(path, readyW, readyH, readyPixels, readyW, readyH)) {
    Serial.print("[Display] Failed to decode image for display: ");
    Serial.println(path);
    return false;
  }

  const int offsetX = (frameWidth - readyW) / 2;
  const int offsetY = (frameHeight - readyH) / 2;

  Serial.print("[Display] Render ");
  Serial.print(path);
  Serial.print(" src=");
  Serial.print(srcW);
  Serial.print("x");
  Serial.print(srcH);
  Serial.print(" ready=");
  Serial.print(readyW);
  Serial.print("x");
  Serial.print(readyH);
  Serial.print(" offset=");
  Serial.print(offsetX);
  Serial.print("x");
  Serial.println(offsetY);

  if (gLvglInitialized || ensureLvglDisplay()) {
    updateLvglCanvas(readyPixels.get(), readyW, readyH);
    Serial.println("[Display] Showing decoded image via LVGL canvas: " + path);
    return true;
  }

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

  for (uint16_t row = 0; row < readyH; ++row) {
    const int y = offsetY + row;
    if (y < 0 || y >= frameHeight) {
      continue;
    }
    const uint16_t* rowPixels = readyPixels.get() + static_cast<size_t>(row) * readyW;
    gLcd->drawBitmap(offsetX, y, readyW, 1, reinterpret_cast<uint8_t*>(const_cast<uint16_t*>(rowPixels)), -1);
  }

  Serial.println("[Display] Showing decoded image via direct RGB565 bitmap: " + path);
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
