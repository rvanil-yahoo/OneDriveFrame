#include "OneDriveFrameApp.h"

#include "FrameConfig.h"

namespace odf {

void OneDriveFrameApp::setup() {
  Serial.begin(115200);
  delay(1000);
  printBootBanner();
  Serial.println("[Storage] SD mount deferred until sync phase.");
}

void OneDriveFrameApp::loop() {
  switch (state_) {
    case AppState::kStart:
      logStateTransition("START");
      if (!tokenStoreInitialized_) {
        tokenStoreInitialized_ = tokenStore_.begin();
      }
      state_ = AppState::kWifiConnect;
      break;

    case AppState::kWifiConnect:
      logStateTransition("WIFI_CONNECT");
      if (wifiSetupManager_.connect()) {
        state_ = AppState::kLoadToken;
      } else {
        state_ = AppState::kIdleWaitForSync;
      }
      break;

    case AppState::kLoadToken:
      logStateTransition("LOAD_TOKEN");
      if (tokenStoreInitialized_ && tokenStore_.load(token_)) {
        Serial.println("[NVS] Tokens restored from flash.");
      } else {
        Serial.println("[NVS] No saved token found.");
      }
      state_ = AppState::kCheckTokenValidity;
      break;

    case AppState::kCheckTokenValidity:
      logStateTransition("CHECK_TOKEN_VALIDITY");
      if (token_.isValid()) {
        Serial.println("[OAuth] Access token is still valid.");
        state_ = AppState::kListOneDriveFolder;
      } else {
        Serial.println("[OAuth] Token expired or missing; attempting refresh or login.");
        state_ = AppState::kRefreshOrLogin;
      }
      break;

    case AppState::kRefreshOrLogin:
      logStateTransition("REFRESH_TOKEN_OR_LOGIN");
      if (graphClient_.refreshAccessToken(token_)) {
        if (tokenStoreInitialized_) {
          tokenStore_.save(token_);
        }
        state_ = AppState::kListOneDriveFolder;
        break;
      }

      if (graphClient_.authenticateWithDeviceCode(token_)) {
        if (tokenStoreInitialized_) {
          tokenStore_.save(token_);
        }
        state_ = AppState::kListOneDriveFolder;
      } else {
        state_ = AppState::kIdleWaitForSync;
      }
      break;

    case AppState::kListOneDriveFolder:
      logStateTransition("LIST_ONEDRIVE_FOLDER");
      if (!photoStorage_.begin()) {
        Serial.println("[Storage] SD card unavailable; skipping cloud sync.");
        state_ = AppState::kDisplaySlideshow;
        break;
      }
      if (graphClient_.listFolder(token_)) {
        state_ = AppState::kDownloadNewPhotos;
      } else {
        if (graphClient_.consumeAuthFailure()) {
          Serial.println("[OAuth] Graph returned 401. Forcing refresh/login.");
          token_.accessToken = "";
          token_.expiresAtMs = 0;
          state_ = AppState::kRefreshOrLogin;
        } else {
          state_ = AppState::kDisplaySlideshow;
        }
      }
      break;

    case AppState::kDownloadNewPhotos:
      logStateTransition("DOWNLOAD_NEW_PHOTOS");
      if (graphClient_.downloadNewestPhotos(token_)) {
        state_ = AppState::kPruneOldPhotos;
      } else {
        if (graphClient_.consumeAuthFailure()) {
          Serial.println("[OAuth] Download returned 401. Forcing refresh/login.");
          token_.accessToken = "";
          token_.expiresAtMs = 0;
          state_ = AppState::kRefreshOrLogin;
        } else {
          state_ = AppState::kDisplaySlideshow;
        }
      }
      break;

    case AppState::kPruneOldPhotos:
      logStateTransition("PRUNE_OLD_PHOTOS");
      photoStorage_.pruneOldestPhotos();
      lastSuccessfulSyncMs_ = millis();
      state_ = AppState::kDisplaySlideshow;
      break;

    case AppState::kDisplaySlideshow:
      logStateTransition("DISPLAY_SLIDESHOW");
      photoStorage_.displaySlideshow();
      state_ = AppState::kIdleWaitForSync;
      break;

    case AppState::kIdleWaitForSync:
      logStateTransition("IDLE_WAIT_FOR_SYNC");
      delay(1000);
      if (millis() - lastSuccessfulSyncMs_ >= kSyncIntervalMs) {
        state_ = AppState::kWifiConnect;
      }
      break;

    default:
      state_ = AppState::kStart;
      break;
  }

  delay(250);
}

void OneDriveFrameApp::logStateTransition(const char* stateName) const {
  Serial.println();
  Serial.println("========================");
  Serial.println(stateName);
  Serial.println("========================");
}

void OneDriveFrameApp::printBootBanner() const {
  Serial.println("OneDriveFrame booting...");
  Serial.println("Project: ESP32-S3 digital photo frame");
  Serial.println("Target: Microsoft consumer OneDrive + local SD slideshow");
  Serial.println("Folder: " + String(kOneDriveFolderName));
}

}  // namespace odf
