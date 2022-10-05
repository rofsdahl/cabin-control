#ifndef NEXATX_H
#define NEXATX_H

#include <Arduino.h>

enum NexaType {
  LEARN  = 'L',
  SIMPLE = 'S',
  HE35   = 'H',
};

class NexaTx {
  public:
    NexaTx(uint8_t pin);
    void transmit(NexaType type, unsigned long id, boolean activation);
    void transmit(NexaType type, unsigned long id, boolean activation, int repetitions);
  private:
    uint8_t pin;
    String toSelfLearningPacket(unsigned long uniqueId, byte unit, boolean activation);
    String toSelfLearningUniqueId(unsigned long uniqueId);
    String toSelfLearningBits(byte symbol);
    String toSimplePacket(byte house, byte unit, boolean activation);
    String toHellmertzPacket(byte house, byte unit, boolean activation);
    String toSimpleBits(byte symbol);
    void transmitBitSeq(String bitSequence, int pulseLen);
};

#endif
