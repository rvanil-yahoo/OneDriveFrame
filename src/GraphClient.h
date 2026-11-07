#pragma once

#include <Arduino.h>

#include "PhotoCatalog.h"
#include "OAuthToken.h"

namespace odf {

class GraphClient {
 public:
  bool listFolder(const OAuthToken& token);
  bool downloadNewestPhotos(const OAuthToken& token);
  bool refreshAccessToken(OAuthToken& token);
  bool authenticateWithDeviceCode(OAuthToken& token);
  bool consumeAuthFailure();

 private:
  struct PendingPhoto {
    String itemId;
    String name;
    String modifiedUtc;
    String etag;
    String mimeType;
    int32_t width = -1;
    int32_t height = -1;
    String orientation;
    uint32_t sizeBytes = 0;
  };

  PendingPhoto pendingPhotos_[128];
  size_t pendingPhotoCount_ = 0;
  PhotoCatalog catalog_;
  bool authFailure_ = false;
};

}  // namespace odf
