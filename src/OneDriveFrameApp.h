#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

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
  static void displayTaskCore0(void* parameter);
  static void syncTaskCore1(void* parameter);
  void runDisplayStep();
  void runSyncStep();
  void runDisplayTask();
  void runSyncTask();
  void logStateTransition(const char* stateName) const;
  void printBootBanner() const;

  AppState state_ = AppState::kStart;
  OAuthToken token_;
  TokenStore tokenStore_;
  GraphClient graphClient_;
  PhotoStorage photoStorage_;
  WifiSetupManager wifiSetupManager_;
  uint32_t lastSuccessfulSyncMs_ = 0;
  uint32_t lastStorageRetryMs_ = 0;
  bool tokenStoreInitialized_ = false;
  std::atomic<bool> storageReady_{false};
  std::atomic<bool> displayTaskStarted_{false};
  std::atomic<bool> syncTaskStarted_{false};
  SemaphoreHandle_t photoStorageMutex_ = nullptr;
};

}  // namespace odf
