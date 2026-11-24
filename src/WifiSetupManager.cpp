#include "WifiSetupManager.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "FrameConfig.h"

namespace odf {

bool WifiSetupManager::connect() {
  WiFiManager wifiManager;
  wifiManager.setDebugOutput(true);
  wifiManager.setConnectTimeout(kWifiConnectTimeoutMs / 1000U);
  wifiManager.setConfigPortalTimeout(kWifiPortalTimeoutMs / 1000U);
  wifiManager.setAPCallback([this](WiFiManager* manager) {
    Serial.println("[WiFi] Setup portal started.");
    Serial.print("[WiFi] AP SSID: ");
    Serial.println(manager->getConfigPortalSSID());
    Serial.println("[WiFi] Open http://192.168.4.1");
    if (promptCallback_) {
      promptCallback_(
          "Wi-Fi setup required",
          "On your phone or computer:\n1. Connect to " + manager->getConfigPortalSSID() +
              "\n2. Password: " + String(kConfigPortalPassword) + "\n3. Open http://192.168.4.1",
          kSetupMessageTimeoutMs);
    }
  });

  bool connected = wifiManager.autoConnect(kConfigPortalName, kConfigPortalPassword);
  if (clearPromptCallback_) {
    clearPromptCallback_();
  }
  if (!connected) {
    Serial.println("[WiFi] Setup portal timeout or connection failed.");
    return false;
  }

  // Allocate/initiate PHY timers while internal RAM is least fragmented, before Graph TLS starts.
  WiFi.setSleep(false);
  Serial.print("[WiFi] Connected. IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

}  // namespace odf
