#include "core_ota.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

#include "PubSubClient.h"
#include "config_wifi.h"

// ================== MQTT / Topics ==================

static WiFiClient mqttNet;
static PubSubClient mqtt(mqttNet);

static bool otaRunning = false;
static unsigned long lastHb = 0;

static String baseTopic;
static String topicStatus;
static String topicLog;
static String topicCmd;
static String topicOtaHttp;

static inline String T(const char* tail) {
  return baseTopic + "/" + tail;
}

// ================== Helpers log ==================

static void publishStatus(const String& msg, bool retain = true) {
  Serial.println("[STATUS] " + msg);
  if (mqtt.connected()) {
    mqtt.publish(topicStatus.c_str(), msg.c_str(), retain);
    Serial.println("[MQTT] " + topicStatus + " -> " + msg);
  }
}

static void publishLog(const String& msg) {
  Serial.println("[LOG] " + msg);
  if (mqtt.connected()) {
    mqtt.publish(topicLog.c_str(), msg.c_str(), false);
    Serial.println("[MQTT] " + topicLog + " -> " + msg);
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
    delay(500);
    Serial.print(".");
    if (millis() - t0 > timeoutMs) {
      Serial.println("\nWiFi TIMEOUT ❌");
      return false;
    }
  }

  Serial.println("\nWiFi conectado ✅");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  publishStatus("WIFI OK IP=" + WiFi.localIP().toString(), true);
  return true;
}

// ================== OTA: parse de mensaje HTTP|url|size|sha|file ==================

static bool parseHttpOtaMsg(const String& msg, String& url, size_t& sz, String& fname) {
  if (!msg.startsWith("HTTP|")) return false;

  int p1 = msg.indexOf('|');
  int p2 = msg.indexOf('|', p1 + 1);
  int p3 = msg.indexOf('|', p2 + 1);
  int p4 = msg.indexOf('|', p3 + 1);
  if (p1 < 0 || p2 < 0 || p3 < 0 || p4 < 0) return false;

  url = msg.substring(p1 + 1, p2);
  String sizeStr = msg.substring(p2 + 1, p3);
  // sha = msg.substring(p3 + 1, p4); // ahora mismo no lo usamos
  fname = msg.substring(p4 + 1);

  url.trim();
  fname.trim();

  long s = sizeStr.toInt();
  if (s <= 0) return false;
  sz = (size_t)s;

  if (!url.startsWith("http://") && !url.startsWith("https://")) return false;

  return true;
}

// ================== OTA HTTP manual (HTTPClient + Update) ==================

static bool otaHttpUpdate(const String& url, size_t expectedSize, const String& fname) {
  otaRunning = true;

  publishStatus("OTA HTTP START: " + fname + " (" + String(expectedSize) + " bytes)");
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

  // Si el servidor te promete un tamaño, podemos comparar luego
  size_t beginSize = (contentLen > 0) ? (size_t)contentLen : UPDATE_SIZE_UNKNOWN;

  if (!Update.begin(beginSize)) {
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

  // Comprobación con content-length y con expectedSize (del mensaje)
  if (contentLen > 0 && written != (size_t)contentLen) {
    publishStatus("OTA FAIL: size mismatch " +
                  String((unsigned)written) + "/" +
                  String(contentLen));
    Update.abort();
    otaRunning = false;
    return false;
  }

  if (expectedSize > 0 && written != expectedSize) {
    publishStatus("OTA WARN: esperado=" +
                  String((unsigned)expectedSize) + " escrito=" +
                  String((unsigned)written));
    // No abortamos; sólo avisamos
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

  ESP.restart();        // no deberías volver de aquí
  return true;          // por si acaso
}

// ================== MQTT ==================

static void mqttCallback(char* topic, byte* payload, unsigned int length);

static void ensureMqtt() {
  if (mqtt.connected() || otaRunning) return;

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(MQTT_MAX_PACKET_SIZE);

  while (!mqtt.connected() && !otaRunning) {
    Serial.print("Conectando MQTT... ");
    String clientId = String("ESP32_HTTPUPDATE_") +
                      String((uint32_t)ESP.getEfuseMac(), HEX);

    bool ok;
    if (String(MQTT_USER).length() > 0) {
      ok = mqtt.connect(
        clientId.c_str(),
        topicStatus.c_str(), 0, true,
        "OFFLINE ❌"
      );
    } else {
      ok = mqtt.connect(
        clientId.c_str(),
        topicStatus.c_str(), 0, true,
        "OFFLINE ❌"
      );
    }

    if (ok) {
      Serial.println("OK ✅");
      mqtt.subscribe(topicCmd.c_str());
      mqtt.subscribe(topicOtaHttp.c_str());

      Serial.println("[MQTT] Subscribed to:");
      Serial.println("  " + topicCmd);
      Serial.println("  " + topicOtaHttp);

      publishStatus("ONLINE HTTPUPDATE ✅ BaseTopic=" + baseTopic, true);
    } else {
      Serial.print("Fallo rc=");
      Serial.print(mqtt.state());
      Serial.println(" reintento 3s...");
      delay(3000);
    }
  }
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

  // Comandos simples (LED_ON / LED_OFF)
  if (t == topicCmd) {
    publishLog("CMD RX: " + msg);

    if (msg == "LED_ON") {
      pinMode(CORE_LED_PIN, OUTPUT);
      digitalWrite(CORE_LED_PIN, HIGH);
    } else if (msg == "LED_OFF") {
      pinMode(CORE_LED_PIN, OUTPUT);
      digitalWrite(CORE_LED_PIN, LOW);
    }

    return;
  }

  // OTA HTTP: topic EXACTO: baseTopic + "/ota/http"
  if (t == topicOtaHttp) {
    if (otaRunning) {
      publishStatus("OTA ya en curso, ignoro nuevo mensaje.");
      return;
    }

    String url, fname;
    size_t sz = 0;
    if (!parseHttpOtaMsg(msg, url, sz, fname)) {
      publishStatus("OTA ERROR: mensaje HTTP inválido");
      return;
    }

    otaHttpUpdate(url, sz, fname);
    return;
  }
}

// ================== API pública ==================

void CoreOtaSetup() {
  Serial.begin(115200);
  delay(200);

  pinMode(CORE_LED_PIN, OUTPUT);
  digitalWrite(CORE_LED_PIN, LOW);

  baseTopic    = String(BASE_TOPIC_PREFIX) + "/" + DEVICE_ID;
  topicStatus  = T("status");
  topicLog     = T("log");
  topicCmd     = T("comandos");
  topicOtaHttp = baseTopic + "/ota/http";   // IMPORTANTE: igual que tu código base

  Serial.println("=== CORE_OTA INIT ===");
  Serial.println("BaseTopic: " + baseTopic);
  Serial.println("StatusTopic: " + topicStatus);

  if (!connectWiFi()) {
    publishStatus("WIFI FAIL -> restart");
    delay(500);
    ESP.restart();
  }

  publishStatus("ESP32 CORE_OTA listo...", true);
  ensureMqtt();
}

void CoreOtaLoop() {
  // Si hay OTA en curso, no hacemos nada más
  if (otaRunning) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  ensureMqtt();
  if (mqtt.connected()) {
    mqtt.loop();
  }

  // Heartbeat
  if (millis() - lastHb >= HEARTBEAT_MS) {
    lastHb = millis();
    publishStatus("HB milis=" + String(millis()), true);
  }
}
