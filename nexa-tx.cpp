#include "nexa-tx.h"

NexaTx::NexaTx(uint8_t pin) {
  this->pin = pin;
  pinMode(pin, OUTPUT);
}

void NexaTx::transmit(NexaType type, unsigned long id, boolean activation) {
  transmit(type, id, activation, 4);
}

void NexaTx::transmit(NexaType type, unsigned long id, boolean activation, int repetitions) {
  int pulseLen;
  String bitSequence = "";

  String packet;
  if (type == Learning) {
    pulseLen = 250;
    byte unit = 1;
    packet = toSelfLearningPacket(id, unit, activation);
    for (byte symbol : packet) {
      bitSequence += toSelfLearningBits(symbol);
    }
  }
  else {
    byte house = (id & 0xF0) >> 4;
    byte unit = id & 0x0F;

    if (type == Simple) {
      pulseLen = 350;
      packet = toSimplePacket(house, unit, activation);
    }
    else {
      pulseLen = 420;
      packet = toHellmertzPacket(house, unit, activation);
    }

    for (byte symbol : packet) {
      bitSequence += toSimpleBits(symbol);
    }
  }

  for (int x=0; x<repetitions; x++) {
    for (int y=0; y<5; y++) {
      transmitBitSeq(bitSequence, pulseLen);
    }
    ets_delay_us(35000);
  }
}

String NexaTx::toSelfLearningPacket(unsigned long uniqueId, byte unit, boolean activation) {
  String packet = "S";
  packet += toSelfLearningUniqueId(uniqueId);
  packet += "1";
  packet += (activation ? "0" : "1");
  packet += "11";
  if (unit == 1) packet += "11";
  if (unit == 2) packet += "10";
  if (unit == 3) packet += "01";
  if (unit == 4) packet += "00";
  packet += "P";
  return packet;
}

String NexaTx::toSelfLearningUniqueId(unsigned long uniqueId) {
  char uniqueIdStr[27] = "00000000000000000000000000";
  unsigned long mask=0x02000000;
  for (int i=0; i<26; i++, mask>>=1) {
    if (uniqueId & mask) {
      uniqueIdStr[i] = '1';
    }
  }
  return uniqueIdStr;
}

String NexaTx::toSelfLearningBits(byte symbol) {
  if (symbol == 'S') return "10000000000";
  if (symbol == '0') return "10000010";
  if (symbol == '1') return "10100000";
  if (symbol == 'P') return "10000000000000000000000000000000000000000";
  return "";
}

String NexaTx::toSimplePacket(byte house, byte unit, boolean activation) {
  String packet = "";
  if (house == 0x0A) packet += "0000";
  if (house == 0x0B) packet += "X000";
  if (house == 0x0C) packet += "0X00";
  if (house == 0x0D) packet += "XX00";
  if (house == 0x0E) packet += "00X0";
  if (house == 0x0F) packet += "X0X0";
  if (unit == 1) packet += "0000";
  if (unit == 2) packet += "X000";
  if (unit == 3) packet += "0X00";
  if (unit == 4) packet += "XX00";
  if (unit == 5) packet += "00X0";
  if (unit == 6) packet += "X0X0";
  if (unit == 7) packet += "0XX0";
  if (unit == 8) packet += "XXX0";
  packet += "0XX";
  packet += (activation ? "X" : "0");
  packet += "P";
  return packet;
}

String NexaTx::toHellmertzPacket(byte house, byte unit, boolean activation) {
  String packet = "";
  if (house == 0x0A) packet += "0XXXXX";
  if (house == 0x0B) packet += "X0XXXX";
  if (house == 0x0C) packet += "XX0XXX";
  if (house == 0x0D) packet += "XXX0XX";
  if (house == 0x0E) packet += "XXXX0X";
  if (house == 0x0F) packet += "XXXXX0";
  if (unit == 1)     packet += "XXX0";
  if (unit == 2)     packet += "XX0X";
  if (unit == 3)     packet += "X0XX";
  if (unit == 4)     packet += "0XXX";
  packet += "0";
  packet += (activation ? "X" : "0");
  packet += "P";
  return packet;
}

String NexaTx::toSimpleBits(byte symbol) {
  if (symbol == '0') return "10001000";
  if (symbol == '1') return "11101110";
  if (symbol == 'X') return "10001110";
  if (symbol == 'P') return "100000000000000000000000000000000";
  return "";
}

void NexaTx::transmitBitSeq(String bitSequence, int pulseLen) {
  unsigned long tNext = micros();
  for (int i=0; i<bitSequence.length(); i++) {
    tNext += pulseLen;
    digitalWrite(pin, bitSequence[i]=='1' ? HIGH : LOW);
    while (micros() < tNext);
  }
}
