#include <Arduino.h>

#include "OneDriveFrameApp.h"

namespace {
odf::OneDriveFrameApp app;
}

void setup() {
  app.setup();
}

void loop() {
  app.loop();
}
