#pragma once

#include <Arduino.h>

#include "OAuthToken.h"

namespace odf {

class GraphClient {
 public:
  bool listFolder();
  bool downloadNewestPhotos();
  bool refreshAccessToken(OAuthToken& token);
  bool authenticateWithDeviceCode(OAuthToken& token);
};

}  // namespace odf
