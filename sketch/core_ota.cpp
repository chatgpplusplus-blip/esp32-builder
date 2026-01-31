#include "core_ota.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

#include "PubSubClient.h"
#include "config_wifi.h"

// ================== Topics ==================

static String baseTopic;
static String topicStatus;

// Escucharemos varias variantes por si el backend cambia el topic
static String topicCmd1;  // .../comandos
static String topicCmd2;  // .../command
static String topicCmd3;  // .../ota
static String topicCmd4;  // .../ota_http;

// Helper para construir topics
static inline String T(const char* suffix) {
  return baseTopic + "/" + suffix;
}

static inline String fileNameFromUrl(const String& url) {
  int s = url.lastIndexOf('/');
  if (s < 0) return "firmware.bin";
  return url.substring(s + 1);
}

// ================== MQTT / estado global ==================

static WiFiClient mqttNet;
static PubSubClient mqtt(mqttNet);

static bool otaRunning = false;
static unsigned long lastHb = 0;

// ================== Helpers de log ==================

static void publishStatus(const String& msg) {
  Serial.println("[STATUS] " + msg);
  if (mqtt.connected()) {
    mqtt.publish(topicStatus.c_str(), msg.c_str(), true);
    Serial.println("[MQTT] " + topicStatus + " -> " + msg);
  }
}

// ================== WiFi ==================

static bool connectWiFi(unsigned long timeoutMs = 20000UL) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  Serial.println("Conectando WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    if (millis() - t0 > timeoutMs) {
      Serial.println("WiFi TIMEOUT ❌");
      return false;
    }
  }

  Serial.println("WiFi conectado ✅");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  publishStatus("WIFI OK IP=" + WiFi.localIP().toString());
  return true;
}

// ================== MQTT (forward del callback) ==================

static void mqttCallback(char* topic, byte* payload, unsigned int length);

static void ensureMqtt() {
  if (mqtt.connected()) return;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  String clientId = String("esp32-") + DEVICE_ID + "-" +
                    String((uint32_t)ESP.getEfuseMac(), HEX);

  Serial.print("Conectando MQTT... ");
  bool ok;
  if (String(MQTT_USER).length() > 0) {
    ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS);
  } else {
    ok = mqtt.connect(clientId.c_str());
  }

  if (ok) {
    Serial.println("OK ✅");

    // Suscribir a varios topics candidatos
    mqtt.subscribe(topicCmd1.c_str());
    mqtt.subscribe(topicCmd2.c_str());
    mqtt.subscribe(topicCmd3.c_str());
    mqtt.subscribe(topicCmd4.c_str());

    Serial.println("[MQTT] Subscribed to:");
    Serial.println("  " + topicCmd1);
    Serial.println("  " + topicCmd2);
    Serial.println("  " + topicCmd3);
    Serial.println("  " + topicCmd4);

    publishStatus("ONLINE HTTPUPDATE ✅ BaseTopic=" + baseTopic);
  } else {
    Serial.println("FAIL");
  }
}

// ================== OTA HTTP manual ==================

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

  int contentLen = http.getSize();  // -1 si chunked
  if (contentLen > 0) {
    publishStatus("OTA HTTP: size=" + String(contentLen));
  } else {
    publishStatus("OTA HTTP: size desconocido (chunked)");
  }

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
    if (contentLen > 0 && written >= (size_t)contentLen) {
      break;
    }

    int avail = stream->available();
    if (avail > 0) {
      int toRead = avail;
      if (toRead > (int)sizeof(buf)) {
        toRead = (int)sizeof(buf);
      }

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
            Serial.printf("[OTA] %u / %u\n",
                          (unsigned)written, (unsigned)contentLen);
          } else {
            Serial.printf("[OTA] %u bytes\n", (unsigned)written);
          }
        }
      }
    } else {
      // sin datos disponibles
      if (contentLen <= 0 && !http.connected()) {
        break;
      }

      if (millis() - lastByteAt > OTA_STALL_TIMEOUT_MS) {
        publishStatus("OTA FAIL: stall (sin bytes nuevos)");
        Update.abort();
        http.end();
        otaRunning = false;
        return false;
      }

      delay(1);
      yield();
    }
  }

  http.end();

  if (contentLen > 0 && written != (size_t)contentLen) {
    publishStatus("OTA FAIL: size mismatch " +
                  String((unsigned)written) + "/" +
                  String(contentLen));
    Update.abort();
    otaRunning = false;
    return false;
  }

  if (!Update.end(true)) {
    publishStatus("OTA FAIL: Update.end err=" + String(Update.getError()));
    otaRunning = false;
    return false;
  }

  if (contentLen > 0) {
    Serial.printf("[OTA] %u / %u\n",
                  (unsigned)written, (unsigned)contentLen);
  } else {
    Serial.printf("[OTA] done bytes=%u\n", (unsigned)written);
  }

  publishStatus("OTA OK ✅ reiniciando...");

  if (mqtt.connected()) mqtt.disconnect();
  delay(50);
  Serial.flush();
  delay(200);

  ESP.restart();   // ← aquí ya no hay que esperar 6 minutos
  return true;     // (prácticamente no se ejecuta nunca tras el restart)
}

// ================== MQTT callback ==================

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t(topic);
  String msg;
  msg.reserve(length + 4);
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  msg.trim();

  Serial.println("[MQTT CB] topic=" + t);
  Serial.println("[MQTT CB] payload=" + msg);

  bool isCmdTopic =
    (t == topicCmd1 || t == topicCmd2 || t == topicCmd3 || t == topicCmd4);

  if (!isCmdTopic) {
    // Mensajes de otros topics se ignoran
    return;
  }

  publishStatus("CMD RX: " + msg);

  String url = msg;

  // Permitir "OTA <url>"
  if (msg.startsWith("OTA ") || msg.startsWith("ota ")) {
    url = msg.substring(4);
    url.trim();
  }

  if (url.startsWith("http://") || url.startsWith("https://")) {
    if (!otaRunning) {
      otaHttpUpdate(url);
    } else {
      publishStatus("OTA ya en progreso...");
    }
  } else {
    publishStatus("CMD ignorado (no URL)");
  }
}

// ================== API pública (lo que llama tu sketch) ==================

void CoreOtaSetup() {
  Serial.begin(115200);
  delay(200);

  baseTopic   = String(BASE_TOPIC_PREFIX) + "/" + DEVICE_ID;
  topicStatus = T("status");

  topicCmd1   = baseTopic + "/comandos";
  topicCmd2   = baseTopic + "/command";
  topicCmd3   = baseTopic + "/ota";
  topicCmd4   = baseTopic + "/ota_http";

  Serial.println("=== CORE_OTA INIT ===");
  Serial.println("BaseTopic: " + baseTopic);
  Serial.println("StatusTopic: " + topicStatus);

  if (!connectWiFi()) {
    publishStatus("WIFI FAIL -> restart");
    delay(500);
    ESP.restart();
  }

  publishStatus("ESP32 CORE_OTA listo...");
  ensureMqtt();
}

void CoreOtaLoop() {
  // Nota: otaHttpUpdate es síncrono; mientras está corriendo,
  // no volvemos a entrar aquí hasta que termina o hace restart.

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  ensureMqtt();
  mqtt.loop();

  // Heartbeat
  if (millis() - lastHb >= HEARTBEAT_MS) {
    lastHb = millis();
    publishStatus("HB milis=" + String(millis()));
  }

  // La lógica de usuario la sigues llamando tú en loop():
  //   CoreOtaLoop();
  //   UserLoop();
}
