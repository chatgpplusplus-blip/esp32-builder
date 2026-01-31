#pragma once
#include <Arduino.h>

#ifndef DEVICE_ID
  #define DEVICE_ID "bari1"
#endif

#ifndef BASE_TOPIC_PREFIX
  #define BASE_TOPIC_PREFIX "bari/esp32"
#endif

// === MQTT broker: IGUAL que tu código base que funciona ===
#ifndef MQTT_HOST
  #define MQTT_HOST "broker.emqx.io"
#endif

#ifndef MQTT_PORT
  #define MQTT_PORT 1883
#endif

#ifndef MQTT_USER
  #define MQTT_USER ""
#endif

#ifndef MQTT_PASS
  #define MQTT_PASS ""
#endif

#ifndef HEARTBEAT_MS
  #define HEARTBEAT_MS 5000UL
#endif

// OTA tuning
#ifndef OTA_READ_TIMEOUT_MS
  #define OTA_READ_TIMEOUT_MS 8000UL
#endif

#ifndef OTA_STALL_TIMEOUT_MS
  #define OTA_STALL_TIMEOUT_MS 6000UL
#endif

#ifndef OTA_BUF_SIZE
  #define OTA_BUF_SIZE 16384   // 16 KB
#endif

// Igual que el define del sketch base, para soportar mensajes grandes
#ifndef MQTT_MAX_PACKET_SIZE
  #define MQTT_MAX_PACKET_SIZE 2048
#endif

// LED “core” (el 2 de tu código base), distinto del de user_app
#ifndef CORE_LED_PIN
  #define CORE_LED_PIN 2
#endif

void CoreOtaSetup();
void CoreOtaLoop();
