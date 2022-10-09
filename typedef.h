#ifndef TYPEDEF_H
#define TYPEDEF_H

#include "nexa-tx.h"

enum Activity {
  NONE,
  NEXA,
  WIFI,
  TIME,
  SYNC,
};

enum ZoneType {
  AUTO   = 'A',
  SENSOR = 'S',
  MANUAL = 'M',
};

typedef struct {
  NexaType type;
  unsigned long id;
} Nexa;

#define NEXAS_PER_ZONE 2

typedef struct {
  const char* name;
  ZoneType type;
  uint8_t sensorAddr[8];
  Nexa nexas[NEXAS_PER_ZONE];
  float temp;
  byte value;
  bool state;
  bool lastDisplayedState;
  unsigned long tOn;
  unsigned long tAccu;
} Zone;

#endif
