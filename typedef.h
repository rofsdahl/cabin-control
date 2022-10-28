#ifndef TYPEDEF_H
#define TYPEDEF_H

#include "nexa-tx.h"

#define NEXAS_PER_ZONE 2

enum ZoneType { AUTO, SENSOR, MANUAL };

typedef struct {
  NexaType type;
  unsigned long id;
} Nexa;

typedef struct {
  const char* name;
  ZoneType type;
  uint64_t sensorId;
  Nexa nexas[NEXAS_PER_ZONE];
  float temp;
  byte value;
  bool state;
  bool lastDisplayedState;
  unsigned long tOn;
  unsigned long tAccu;
} Zone;

#endif
