#include "backlight.h"

Backlight::Backlight() {
  pin = -1;
}

void Backlight::begin(uint8_t pin, uint8_t resolution, uint8_t low, uint8_t high, int dimDelay) {
  this->pin = pin;
  this->low = low;
  this->high = high;
  this->dimDelay = dimDelay;

  ledcSetup(channel, freq, resolution);
  ledcAttachPin(pin, channel);
  setBright();
}

void Backlight::loop() {
  if (pin > -1) {
    brightness = millis() - tLastActivity > dimDelay ? low : high;
    ledcWrite(channel, brightness);
  }
}

void Backlight::setBright() {
  tLastActivity = millis();
}

bool Backlight::isBright() {
  return brightness == high;
}
