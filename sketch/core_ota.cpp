#include "core_ota.h"
#include "config_wifi.h"
#include "user_app.h"

#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>

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

// ---------- OTA HTTP usando HTTPUpdate ----------
void doHttpUpdate(const String& url, const String& fname) {
  otaRunning = true;

  publishStatus("OTA HTTP START: " + fname);
  publishStatus("OTA HTTP URL: " + url);

  WiFiClientSecure updateClient;
  updateClient.setInsecure();  // HTTPS sin validar CA (útil para Render)

  httpUpdate.setLedPin(CMD_LED_PIN, HIGH);

  httpUpdate.onStart([]() {
    Serial.println("[HTTPUPDATE] start");
  });
  httpUpdate.onEnd([]() {
    Serial.println("[HTTPUPDATE] end");
  });
  httpUpdate.onError([](int err) {
    Serial.printf("[HTTPUPDATE] Error %d\n", err);
  });
  httpUpdate.onProgress([](int cur, int total) {
    Serial.printf("[HTTPUPDATE] Progreso: %d / %d\n", cur, total);
  });

  publishStatus("OTA HTTP: llamando a httpUpdate.update(...)");

  t_httpUpdate_return ret = httpUpdate.update(updateClient, url);

  switch (ret) {
    case HTTP_UPDATE_FAILED: {
      int err = httpUpdate.getLastError();
      String errStr = httpUpdate.getLastErrorString();
      publishStatus("OTA ERROR: update failed err=" + String(err) + " (" + errStr + ")");
      break;
    }
    case HTTP_UPDATE_NO_UPDATES:
      publishStatus("OTA INFO: sin actualización (HTTP_UPDATE_NO_UPDATES)");
      break;

    case HTTP_UPDATE_OK:
      publishStatus("OTA OK ✅ (HTTP_UPDATE_OK). Reiniciando...");
      delay(1000);
      ESP.restart();
      break;
  }

  otaRunning = false;
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
    if (msg == "LED_ON")  digitalWrite(CMD_LED_PIN, HIGH);
    if (msg == "LED_OFF") digitalWrite(CMD_LED_PIN, LOW);
    return;
  }

  if (t == T("ota/http")) {
    if (otaRunning) {
      publishStatus("OTA ya en curso, ignoro nuevo mensaje.");
      return;
    }

    String url, fname;
    if (!parseHttpOtaMsg(msg, url, fname)) {
      publishStatus("OTA ERROR: mensaje HTTP inválido");
      return;
    }

    doHttpUpdate(url, fname);
    return;
  }
}

// ---------- WiFi / MQTT ----------
void setup_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi conectado ✅");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  publishStatus("WIFI OK IP=" + WiFi.localIP().toString(), true);
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Conectando MQTT... ");
    String clientId = "ESP32_HTTPUPDATE_" + String((uint32_t)ESP.getEfuseMac(), HEX);

    bool ok = client.connect(
      clientId.c_str(),
      T("status").c_str(), 0, true, "OFFLINE ❌"
    );

    if (ok) {
      Serial.println("OK ✅");
      client.subscribe(T("comandos").c_str());
      client.subscribe(T("ota/http").c_str());
      publishStatus("ONLINE HTTPUPDATE ✅ BaseTopic=" + String(BaseTopic), true);
    } else {
      Serial.print("Fallo rc="); Serial.print(client.state());
      Serial.println(" reintento 3s...");
      delay(3000);
    }
  }
}

// ---------- Implementaciones CoreSetup/CoreLoop ----------
void CoreSetup() {
  Serial.begin(115200);
  delay(200);

  pinMode(CMD_LED_PIN, OUTPUT);
  digitalWrite(CMD_LED_PIN, LOW);

  setup_wifi();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  client.setBufferSize(MQTT_MAX_PACKET_SIZE);

  Serial.println("ESP32 CORE_OTA listo...");
}

void CoreLoop() {
  if (!client.connected() && !otaRunning) reconnect();
  if (!otaRunning) client.loop();

  static unsigned long lastHb = 0;
  if (!otaRunning && millis() - lastHb > 5000) {
    lastHb = millis();
    publishStatus("HB milis=" + String(millis()), true);
  }
}
