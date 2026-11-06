#pragma once

#include <Arduino.h>

#include "OAuthToken.h"

namespace odf {

class GraphClient {
 public:
  bool listFolder(const OAuthToken& token);
  bool downloadNewestPhotos(const OAuthToken& token);
  bool refreshAccessToken(OAuthToken& token);
  bool authenticateWithDeviceCode(OAuthToken& token);

 private:
  struct PendingPhoto {
    String name;
    String downloadUrl;
    String modifiedUtc;
    uint32_t sizeBytes = 0;
  };

  PendingPhoto pendingPhotos_[128];
  size_t pendingPhotoCount_ = 0;
};

}  // namespace odf
