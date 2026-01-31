#include "core_ota.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

#include "PubSubClient.h"
#include "config_wifi.h"
#include "user_app.h"

static String baseTopic, topicStatus, topicCmd;

static inline String T(const char* suffix) {
  return baseTopic + "/" + suffix;
}

static inline String fileNameFromUrl(const String& url) {
  int s = url.lastIndexOf('/');
  if (s < 0) return "firmware.bin";
  return url.substring(s + 1);
}

static WiFiClient mqttNet;
static PubSubClient mqtt(mqttNet);

static bool otaRunning = false;
static unsigned long lastHb = 0;

static void publishStatus(const String& msg) {
  Serial.println("[STATUS] " + msg);
  if (mqtt.connected()) {
    mqtt.publish(topicStatus.c_str(), msg.c_str(), true);
    Serial.println("[MQTT] " + topicStatus + " -> " + msg);
  }
}

static bool connectWiFi(unsigned long timeoutMs = 20000UL) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  Serial.println("Conectando WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    if (millis() - t0 > timeoutMs) return false;
  }

  Serial.println("WiFi conectado ✅");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  publishStatus("WIFI OK IP=" + WiFi.localIP().toString());
  return true;
}

static void mqttCallback(char* topic, byte* payload, unsigned int length);

static void ensureMqtt() {
  if (mqtt.connected()) return;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  String clientId = String("esp32-") + DEVICE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  Serial.print("Conectando MQTT... ");
  bool ok;
  if (String(MQTT_USER).length() > 0) ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS);
  else ok = mqtt.connect(clientId.c_str());

  if (ok) {
    Serial.println("OK ✅");
    mqtt.subscribe(topicCmd.c_str());
    publishStatus("ONLINE HTTPUPDATE ✅ BaseTopic=" + baseTopic);
  } else {
    Serial.println("FAIL");
  }
}

static bool otaHttpUpdate(const String& url) {
  otaRunning = true;

  const String fname = fileNameFromUrl(url);
  publishStatus("OTA HTTP START: " + fname);
  publishStatus("OTA HTTP URL: " + url);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout((uint32_t)OTA_READ_TIMEOUT_MS);

  HTTPClient http;
  http.setTimeout((int)OTA_READ_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) {
    publishStatus("OTA FAIL: http.begin()");
    otaRunning = false;
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    publishStatus("OTA FAIL: HTTP " + String(code));
    http.end();
    otaRunning = false;
    return false;
  }

  int contentLen = http.getSize(); // -1 si chunked
  if (contentLen > 0) publishStatus("OTA HTTP: size=" + String(contentLen));
  else publishStatus("OTA HTTP: size desconocido (chunked)");

  if (!Update.begin(contentLen > 0 ? (size_t)contentLen : UPDATE_SIZE_UNKNOWN)) {
    publishStatus("OTA FAIL: Update.begin err=" + String(Update.getError()));
    http.end();
    otaRunning = false;
    return false;
  }

  publishStatus("OTA HTTP: descargando y escribiendo...");

  WiFiClient* stream = http.getStreamPtr();
  static uint8_t buf[OTA_BUF_SIZE];

  size_t written = 0;
  unsigned long lastProgress = 0;
  unsigned long lastByteAt = millis();

  while (true) {
    // Condición de salida:
    // - si hay contentLen, paramos exactamente cuando llegamos
    if (contentLen > 0 && written >= (size_t)contentLen) break;

    int avail = stream->available();
    if (avail > 0) {
      int toRead = avail;
      if (toRead > (int)sizeof(buf)) toRead = (int)sizeof(buf);

      int r = stream->readBytes(buf, toRead);
      if (r > 0) {
        lastByteAt = millis();

        size_t w = Update.write(buf, (size_t)r);
        if (w != (size_t)r) {
          publishStatus("OTA FAIL: write err=" + String(Update.getError()));
          Update.abort();
          http.end();
          otaRunning = false;
          return false;
        }

        written += w;

        if (millis() - lastProgress >= 500) {
          lastProgress = millis();
          if (contentLen > 0) {
            Serial.printf("[OTA] %u / %u\n", (unsigned)written, (unsigned)contentLen);
          } else {
            Serial.printf("[OTA] %u bytes\n", (unsigned)written);
          }
        }
      }
    } else {
      // Si es chunked y ya no está conectado, terminamos
      if (contentLen <= 0 && !http.connected()) break;

      // Stall detect: si no llegan bytes, cortamos (evita congelado)
      if (millis() - lastByteAt > OTA_STALL_TIMEOUT_MS) {
        publishStatus("OTA FAIL: stall (sin bytes)");
        Update.abort();
        http.end();
        otaRunning = false;
        return false;
      }

      delay(1);
      yield();
    }
  }

  // Cierra HTTP ya
  http.end();

  // Validación final
  if (contentLen > 0 && written != (size_t)contentLen) {
    publishStatus("OTA FAIL: size mismatch " + String((unsigned)written) + "/" + String(contentLen));
    Update.abort();
    otaRunning = false;
    return false;
  }

  if (!Update.end(true)) {
    publishStatus("OTA FAIL: Update.end err=" + String(Update.getError()));
    otaRunning = false;
    return false;
  }

  // Imprime final SIEMPRE
  if (contentLen > 0) Serial.printf("[OTA] %u / %u\n", (unsigned)written, (unsigned)contentLen);
  else Serial.printf("[OTA] done bytes=%u\n", (unsigned)written);

  publishStatus("OTA OK ✅ reiniciando...");

  // Evita “basura” al reiniciar
  if (mqtt.connected()) mqtt.disconnect();
  delay(50);
  Serial.flush();
  delay(200);

  ESP.restart();
  return true;
}

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t(topic);
  String msg;
  msg.reserve(length + 4);

  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  publishStatus("CMD RX: " + msg);

  if (t == topicCmd) {
    String url = msg;
    if (msg.startsWith("OTA ") || msg.startsWith("ota ")) {
      url = msg.substring(4);
      url.trim();
    }

    if (url.startsWith("http://") || url.startsWith("https://")) {
      if (!otaRunning) otaHttpUpdate(url);
      else publishStatus("OTA ya en progreso...");
    } else {
      publishStatus("CMD ignorado (no URL)");
    }
  }
}

void CoreOtaSetup() {
  Serial.begin(115200);
  delay(200);

  baseTopic   = String(BASE_TOPIC_PREFIX) + "/" + DEVICE_ID;
  topicStatus = T("status");
  topicCmd    = T("comandos");

  if (!connectWiFi()) {
    publishStatus("WIFI FAIL -> restart");
    delay(500);
    ESP.restart();
  }

  publishStatus("ESP32 CORE_OTA listo...");
  ensureMqtt();

  publishStatus("=== UserSetup desde web ===");
  UserSetup();
}

void CoreOtaLoop() {
  if (otaRunning) return;

  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  ensureMqtt();
  mqtt.loop();

  if (millis() - lastHb >= HEARTBEAT_MS) {
    lastHb = millis();
    publishStatus("HB milis=" + String(millis()));
  }

  UserLoop();
}
