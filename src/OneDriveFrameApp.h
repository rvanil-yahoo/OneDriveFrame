#pragma once

#include <Arduino.h>

#include "AppState.h"
#include "GraphClient.h"
#include "OAuthToken.h"
#include "PhotoStorage.h"
#include "TokenStore.h"
#include "WifiSetupManager.h"

namespace odf {

class OneDriveFrameApp {
 public:
  void setup();
  void loop();

 private:
  void logStateTransition(const char* stateName) const;
  void printBootBanner() const;

  AppState state_ = AppState::kStart;
  OAuthToken token_;
  TokenStore tokenStore_;
  GraphClient graphClient_;
  PhotoStorage photoStorage_;
  WifiSetupManager wifiSetupManager_;
  uint32_t lastSuccessfulSyncMs_ = 0;
  bool tokenStoreInitialized_ = false;
};

}  // namespace odf
