#include "core_ota.h"
#include "user_app.h"

void setup() {
  CoreOtaSetup();   // WiFi + MQTT + OTA
  UserSetup();      // Lógica de aplicación
}

void loop() {
  CoreOtaLoop();    // Heartbeat, MQTT, OTA
  UserLoop();       // Tu lógica
}
