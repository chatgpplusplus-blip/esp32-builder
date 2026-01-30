#include <Arduino.h>
#include "user_app.h"

const int APP_LED_PIN = 13;

void UserSetup() {
    Serial.println("=== UserSetup por defecto (LED blink) ===");
    pinMode(APP_LED_PIN, OUTPUT);
    digitalWrite(APP_LED_PIN, LOW);
}

void UserLoop() {
    digitalWrite(APP_LED_PIN, HIGH);
    delay(200);
    digitalWrite(APP_LED_PIN, LOW);
    delay(200);
}
