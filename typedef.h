#ifndef TYPEDEF_H
#define TYPEDEF_H

#include "nexa-tx.h"

typedef struct {
  const char* name;
  uint8_t addr[8];
  float temp;
} Sensor;

typedef struct {
  const char* name;
  NexaType type;
  unsigned long id;
  int linkedSensor;
  int activeInMode;
  bool state;
  bool lastDisplayedState;
} Nexa;

#endif
