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
  wifiManager.setAPCallback([](WiFiManager* manager) {
    Serial.println("[WiFi] Setup portal started.");
    Serial.print("[WiFi] AP SSID: ");
    Serial.println(manager->getConfigPortalSSID());
    Serial.println("[WiFi] Open http://192.168.4.1");
  });

  bool connected = wifiManager.autoConnect(kConfigPortalName, kConfigPortalPassword);
  if (!connected) {
    Serial.println("[WiFi] Setup portal timeout or connection failed.");
    return false;
  }

  Serial.print("[WiFi] Connected. IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

}  // namespace odf
