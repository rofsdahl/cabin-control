#ifndef TYPEDEF_H
#define TYPEDEF_H

#include "nexa-tx.h"

enum ZoneType {
  Auto   = 'A',
  Sensor = 'S',
  Manual = 'M',
};

typedef struct {
  const char* name;
  ZoneType type;
  uint8_t sensorAddr[8];
  float temp;
  byte value;
  bool state;
  bool lastDisplayedState;
} Zone;

typedef struct {
  int zone;
  NexaType type;
  unsigned long id;
} Nexa;

#endif
