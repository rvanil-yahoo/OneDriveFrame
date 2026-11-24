#pragma once

#include <Arduino.h>
#include <functional>
#include <utility>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "PhotoCatalog.h"
#include "OAuthToken.h"

namespace odf {

class GraphClient {
 public:
  // SD card access is shared with the display task; all SD/catalog work is serialized via this mutex.
  void setStorageMutex(SemaphoreHandle_t mutex) { storageMutex_ = mutex; }
  void setCatalog(PhotoCatalog* catalog) { catalog_ = catalog; }
  void setPromptCallbacks(std::function<void(const String&, const String&, uint32_t)> prompt,
                          std::function<void()> clear) {
    promptCallback_ = std::move(prompt);
    clearPromptCallback_ = std::move(clear);
  }
  bool listFolder(const OAuthToken& token);
  bool downloadNewestPhotos(const OAuthToken& token);
  bool refreshAccessToken(OAuthToken& token);
  bool authenticateWithDeviceCode(OAuthToken& token);
  bool consumeAuthFailure();

 private:
  PhotoCatalog* catalog_ = nullptr;
  std::function<void(const String&, const String&, uint32_t)> promptCallback_;
  std::function<void()> clearPromptCallback_;
  bool authFailure_ = false;
  SemaphoreHandle_t storageMutex_ = nullptr;
};

}  // namespace odf
