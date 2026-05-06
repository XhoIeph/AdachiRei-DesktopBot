#include <Arduino.h>
#include "led_blink.h"

#define LED_PIN 48

LedBlink led(LED_PIN);

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("ESP32-S3 LED Blink — Module Architecture");

    led.begin();
    led.setPeriod(500);
}

void loop() {
    led.update();
}
