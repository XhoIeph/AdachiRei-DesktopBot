#pragma once

#include <cstdint>

class LedBlink {
public:
    explicit LedBlink(uint8_t pin);

    void begin();
    void update();
    void on();
    void off();
    void toggle();
    void setPeriod(uint32_t ms);

private:
    uint8_t     _pin;
    uint32_t    _periodMs;
    uint32_t    _lastTick;
    bool        _state;
};
