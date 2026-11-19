#pragma once

#include <Arduino.h>
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
  bool listFolder(const OAuthToken& token);
  bool downloadNewestPhotos(const OAuthToken& token);
  bool refreshAccessToken(OAuthToken& token);
  bool authenticateWithDeviceCode(OAuthToken& token);
  bool consumeAuthFailure();

 private:
  PhotoCatalog* catalog_ = nullptr;
  bool authFailure_ = false;
  SemaphoreHandle_t storageMutex_ = nullptr;
};

}  // namespace odf
