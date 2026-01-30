#include "core_ota.h"
#include "user_app.h"

void setup() {
  CoreSetup();   // WiFi + MQTT + OTA
  UserSetup();   // Lógica de aplicación
}

void loop() {
  CoreLoop();    // Heartbeat, MQTT, OTA
  UserLoop();    // Tu lógica (setup/loop de aplicación)
}
