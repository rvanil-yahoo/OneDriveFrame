#pragma once

#include <Arduino.h>

namespace odf {

struct OAuthToken {
  String accessToken;
  String refreshToken;
  uint32_t expiresAtMs = 0;

  bool isValid() const {
    return !accessToken.isEmpty() && expiresAtMs > millis();
  }

  bool isExpired() const {
    return expiresAtMs == 0 || expiresAtMs <= millis();
  }
};

}  // namespace odf
