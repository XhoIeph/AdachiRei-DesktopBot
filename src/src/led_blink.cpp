#include "led_blink.h"
#include <Arduino.h>

LedBlink::LedBlink(uint8_t pin)
    : _pin(pin)
    , _periodMs(500)
    , _lastTick(0)
    , _state(false)
{}

void LedBlink::begin() {
    pinMode(_pin, OUTPUT);
    off();
    _lastTick = millis();
}

void LedBlink::update() {
    uint32_t now = millis();
    if (now - _lastTick >= _periodMs) {
        _lastTick = now;
        toggle();
    }
}

void LedBlink::on() {
    _state = true;
    digitalWrite(_pin, HIGH);
}

void LedBlink::off() {
    _state = false;
    digitalWrite(_pin, LOW);
}

void LedBlink::toggle() {
    if (_state) off();
    else        on();
}

void LedBlink::setPeriod(uint32_t ms) {
    _periodMs = (ms > 0) ? ms : 1;
}
