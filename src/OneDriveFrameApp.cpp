#include "OneDriveFrameApp.h"

#include "FrameConfig.h"

namespace odf {

void OneDriveFrameApp::setup() {
  Serial.begin(115200);
  printBootBanner();
  Serial.println("[Boot] Enabling dual-core mode (display on core 0, sync on core 1).");

  photoStorageMutex_ = xSemaphoreCreateMutex();
  if (photoStorageMutex_ == nullptr) {
    Serial.println("[Boot] Failed to create photo storage mutex; startup halted.");
    return;
  }

  Serial.println("[Boot] Waiting for mission-critical storage initialization...");
  uint32_t nextRetryLogMs = 0;
  bool storageReady = false;
  while (!storageReady) {
    xSemaphoreTake(photoStorageMutex_, portMAX_DELAY);
    storageReady = photoStorage_.begin();
    if (storageReady) {
      photoStorage_.displaySlideshow();
    }
    xSemaphoreGive(photoStorageMutex_);

    if (storageReady) {
      break;
    }

    const uint32_t now = millis();
    if (nextRetryLogMs == 0 || now >= nextRetryLogMs) {
      Serial.println("[Boot] Storage not ready yet; retrying until available.");
      nextRetryLogMs = now + 5000UL;
    }
    delay(500);
  }

  storageReady_.store(storageReady);
  if (storageReady) {
    Serial.println("[Boot] Local photo storage is ready; slideshow will start immediately.");
  }

  TaskHandle_t displayTaskHandle = nullptr;
  const BaseType_t displayCreated =
      xTaskCreatePinnedToCore(displayTaskCore0, "display-core0", 16384U, this, 3, &displayTaskHandle, 0);
  if (displayCreated != pdPASS) {
    Serial.println("[Boot] Failed to create display task on core 0.");
  } else {
    displayTaskStarted_.store(true);
  }

  TaskHandle_t syncTaskHandle = nullptr;
  const BaseType_t syncCreated =
      xTaskCreatePinnedToCore(syncTaskCore1, "sync-core1", 16384U, this, 1, &syncTaskHandle, 1);
  if (syncCreated != pdPASS) {
    Serial.println("[Boot] Failed to create sync task on core 1.");
  } else {
    syncTaskStarted_.store(true);
  }

  if (!displayTaskStarted_.load() || !syncTaskStarted_.load()) {
    Serial.println("[Boot] Task startup incomplete; loop() will run only the missing task.");
  }
}

void OneDriveFrameApp::loop() {
  if (photoStorageMutex_ == nullptr) {
    delay(1000);
    return;
  }
  if (displayTaskStarted_.load() && syncTaskStarted_.load()) {
    delay(1000);
    return;
  }

  if (!displayTaskStarted_.load()) {
    runDisplayStep();
  }
  if (!syncTaskStarted_.load()) {
    runSyncStep();
  }
  delay(250);
}

void OneDriveFrameApp::displayTaskCore0(void* parameter) {
  auto* app = static_cast<OneDriveFrameApp*>(parameter);
  if (app == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  app->runDisplayTask();
}

void OneDriveFrameApp::syncTaskCore1(void* parameter) {
  auto* app = static_cast<OneDriveFrameApp*>(parameter);
  if (app == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  app->runSyncTask();
}

void OneDriveFrameApp::runDisplayStep() {
  if (photoStorageMutex_ == nullptr) {
    return;
  }

  if (!storageReady_.load()) {
    const uint32_t now = millis();
    if (lastStorageRetryMs_ == 0 || now - lastStorageRetryMs_ >= 5000UL) {
      lastStorageRetryMs_ = now;
      xSemaphoreTake(photoStorageMutex_, portMAX_DELAY);
      const bool storageReady = photoStorage_.begin();
      xSemaphoreGive(photoStorageMutex_);
      storageReady_.store(storageReady);
      if (!storageReady) {
        return;
      }
    } else {
      return;
    }
  }

  xSemaphoreTake(photoStorageMutex_, portMAX_DELAY);
  photoStorage_.displaySlideshow();
  xSemaphoreGive(photoStorageMutex_);
}

void OneDriveFrameApp::runSyncStep() {
  switch (state_) {
    case AppState::kStart: {
      logStateTransition("START");
      xSemaphoreTake(photoStorageMutex_, portMAX_DELAY);
      const bool catalogReady = photoStorage_.initializeCatalog();
      xSemaphoreGive(photoStorageMutex_);
      if (!catalogReady) {
        Serial.println("[Storage] Catalog not ready; delaying synchronization.");
        delay(1000);
        break;
      }
      if (!tokenStoreInitialized_) {
        tokenStoreInitialized_ = tokenStore_.begin();
      }
      state_ = AppState::kWifiConnect;
      break;
    }

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
    {
      logStateTransition("LIST_ONEDRIVE_FOLDER");
      if (photoStorageMutex_ == nullptr) {
        Serial.println("[Storage] Photo storage mutex not initialized.");
        state_ = AppState::kDisplaySlideshow;
        break;
      }
      bool storageReady = storageReady_.load();
      if (!storageReady) {
        xSemaphoreTake(photoStorageMutex_, portMAX_DELAY);
        storageReady = photoStorage_.begin();
        xSemaphoreGive(photoStorageMutex_);
        storageReady_.store(storageReady);
      }
      if (!storageReady) {
        Serial.println("[Storage] SD card unavailable; switching to local slideshow mode.");
        state_ = AppState::kDisplaySlideshow;
        break;
      }
      if (graphClient_.listFolder(token_)) {
        Serial.println("[Graph] Folder list succeeded. Starting download queue for new images.");
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
    }

    case AppState::kDownloadNewPhotos:
      logStateTransition("DOWNLOAD_NEW_PHOTOS");
      Serial.println("[Graph] Starting background photo download pass.");
      if (graphClient_.downloadNewestPhotos(token_)) {
        Serial.println("[Graph] Download pass completed successfully.");
      } else {
        Serial.println("[Graph] Download pass did not complete successfully.");
        if (graphClient_.consumeAuthFailure()) {
          Serial.println("[OAuth] Download returned 401. Forcing refresh/login.");
          token_.accessToken = "";
          token_.expiresAtMs = 0;
          state_ = AppState::kRefreshOrLogin;
          break;
        }
      }
      lastSuccessfulSyncMs_ = millis();
      state_ = AppState::kDisplaySlideshow;
      break;

    case AppState::kPruneOldPhotos:
      logStateTransition("PRUNE_OLD_PHOTOS");
      if (photoStorageMutex_ != nullptr) {
        xSemaphoreTake(photoStorageMutex_, portMAX_DELAY);
        photoStorage_.pruneOldestPhotos();
        xSemaphoreGive(photoStorageMutex_);
      }
      lastSuccessfulSyncMs_ = millis();
      state_ = AppState::kDisplaySlideshow;
      break;

    case AppState::kDisplaySlideshow:
      logStateTransition("DISPLAY_SLIDESHOW");
      state_ = AppState::kIdleWaitForSync;
      break;

    case AppState::kIdleWaitForSync:
      if (millis() - lastSuccessfulSyncMs_ >= kSyncIntervalMs) {
        state_ = AppState::kWifiConnect;
      }
      break;

    default:
      state_ = AppState::kStart;
      break;
  }
}

void OneDriveFrameApp::runDisplayTask() {
  for (;;) {
    runDisplayStep();
    vTaskDelay(pdMS_TO_TICKS(16));
  }
}

void OneDriveFrameApp::runSyncTask() {
  for (;;) {
    runSyncStep();
    vTaskDelay(pdMS_TO_TICKS(250));
  }
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
