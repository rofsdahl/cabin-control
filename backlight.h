#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#include <Arduino.h>

class Backlight {
  private:
    const uint8_t channel = 0;
    const double freq = 5000;

    uint8_t pin;
    uint8_t low;
    uint8_t high;
    uint8_t brightness;
    int dimDelay;
    unsigned long tLastActivity = 0;
  public:
    Backlight();
    void begin(uint8_t pin, uint8_t resolution, uint8_t low, uint8_t high, int dimDelay);
    void loop();
    void setBright();
    bool isBright();
};

#endif
