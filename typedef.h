#ifndef TYPEDEF_H
#define TYPEDEF_H

#define NEXAS_PER_ZONE 2

typedef struct {
  const char* name;
  uint64_t sensorId;
  unsigned long nexas[NEXAS_PER_ZONE];
  float temp;
  byte value;
  bool state;
  bool lastDisplayedState;
  unsigned long tOn;
  unsigned long tAccu;
} Zone;

#endif
