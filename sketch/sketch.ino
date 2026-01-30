#include <Arduino.h>

const int LED_PIN = 2;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== BUILD TEST desde GitHub Actions ===");
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
  digitalWrite(LED_PIN, HIGH);
  delay(300);
  digitalWrite(LED_PIN, LOW);
  delay(300);
}
