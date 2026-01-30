#include "core_ota.h"
#include "config_wifi.h"
#include "user_app.h"

#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

#define MQTT_MAX_PACKET_SIZE 2048

// WiFi desde config_wifi.h
static const char* ssid     = WIFI_SSID;
static const char* password = WIFI_PASS;

// MQTT
static const char* mqtt_server = "broker.emqx.io";
static const int   mqtt_port   = 1883;
static const char* BaseTopic   = "bari/esp32/bari1";

String T(const char* tail) { return String(BaseTopic) + "/" + tail; }

WiFiClient      mqttNet;
PubSubClient    client(mqttNet);

const int CMD_LED_PIN = 2;   // LED para comandos / actividad
static bool otaRunning = false;

// ---------- MQTT helpers ----------
bool publishTopic(const String& topic, const String& msg, bool retain = false) {
  if (!client.connected()) return false;
  bool ok = client.publish(topic.c_str(), msg.c_str(), retain);
  Serial.println("[MQTT] " + topic + " -> " + msg);
  return ok;
}

void publishStatus(const String& msg, bool retain = true) {
  Serial.println("[STATUS] " + msg);
  publishTopic(T("status"), msg, retain);
}

void publishLog(const String& msg) {
  Serial.println("[LOG] " + msg);
  publishTopic(T("log"), msg, false);
}

// ---------- OTA HTTP (FAST manual streaming) ----------
void doHttpUpdate(const String& url, const String& fname) {
  otaRunning = true;

  publishStatus("OTA HTTP START: " + fname);
  publishStatus("OTA HTTP URL: " + url);

  // LED indicador
  pinMode(CMD_LED_PIN, OUTPUT);
  digitalWrite(CMD_LED_PIN, HIGH);

  bool isHttps = url.startsWith("https://");

  WiFiClient* stream = nullptr;

  WiFiClientSecure httpsClient;
  WiFiClient httpClient;

  HTTPClient http;

  if (isHttps) {
    httpsClient.setInsecure(); // Render / HTTPS sin CA
    if (!http.begin(httpsClient, url)) {
      publishStatus("OTA ERROR: http.begin(https) failed");
      digitalWrite(CMD_LED_PIN, LOW);
      otaRunning = false;
      return;
    }
  } else {
    if (!http.begin(httpClient, url)) {
      publishStatus("OTA ERROR: http.begin(http) failed");
      digitalWrite(CMD_LED_PIN, LOW);
      otaRunning = false;
      return;
    }
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    publishStatus("OTA ERROR: HTTP " + String(code));
    http.end();
    digitalWrite(CMD_LED_PIN, LOW);
    otaRunning = false;
    return;
  }

  int total = http.getSize();
  stream = http.getStreamPtr();
  if (!stream) {
    publishStatus("OTA ERROR: stream null");
    http.end();
    digitalWrite(CMD_LED_PIN, LOW);
    otaRunning = false;
    return;
  }

  if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN)) {
    publishStatus("OTA ERROR: Update.begin failed err=" + String(Update.getError()));
    http.end();
    digitalWrite(CMD_LED_PIN, LOW);
    otaRunning = false;
    return;
  }

  publishStatus("OTA HTTP: descargando y escribiendo...");

  // Buffer grande: 16KB (puedes probar 32768 si tu RAM lo permite cómodo)
  static uint8_t buf[16384];

  unsigned long lastProg = 0;
  int writtenTotal = 0;

  while (http.connected()) {
    size_t avail = stream->available();
    if (!avail) {
      delay(1);
      continue;
    }

    int toRead = (avail > sizeof(buf)) ? sizeof(buf) : (int)avail;
    int n = stream->readBytes(buf, toRead);
    if (n <= 0) break;

    size_t w = Update.write(buf, n);
    writtenTotal += (int)w;

    // Progreso cada ~500ms para no spamear
    if (millis() - lastProg > 500) {
      lastProg = millis();
      if (total > 0) {
        Serial.printf("[OTA] %d / %d\n", writtenTotal, total);
      } else {
        Serial.printf("[OTA] %d\n", writtenTotal);
      }
    }
  }

  if (!Update.end(true)) {
    publishStatus("OTA ERROR: Update.end failed err=" + String(Update.getError()));
    http.end();
    digitalWrite(CMD_LED_PIN, LOW);
    otaRunning = false;
    return;
  }

  http.end();

  publishStatus("OTA OK ✅ Reiniciando...");
  delay(500);
  ESP.restart();
}

// ---------- Parse mensaje OTA HTTP ----------
// HTTP|<url>|<size>|<sha256>|<filename>
bool parseHttpOtaMsg(const String& msg, String& url, String& fname) {
  if (!msg.startsWith("HTTP|")) return false;

  int p1 = msg.indexOf('|');
  int p2 = msg.indexOf('|', p1 + 1);
  int p3 = msg.indexOf('|', p2 + 1);
  int p4 = msg.indexOf('|', p3 + 1);
  if (p1 < 0 || p2 < 0 || p3 < 0 || p4 < 0) return false;

  url = msg.substring(p1 + 1, p2);
  // size (p2-p3) y sha (p3-p4) no los usamos aquí
  fname = msg.substring(p4 + 1);

  url.trim();
  fname.trim();

  if (!url.startsWith("http://") && !url.startsWith("https://")) return false;

  return true;
}

// ---------- MQTT callback ----------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t(topic);

  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  if (t == T("comandos")) {
    publishLog("CMD RX: " + msg);
