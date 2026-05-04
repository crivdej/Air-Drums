#pragma once

#include <Arduino.h>

constexpr int NUM_SENSORS = 3;
constexpr int TRIGGER_CM = 10;
constexpr int DEBOUNCE_MS = 100;

struct DrumSensor {
  int trig;
  int echo;
  const char* name;
  const uint8_t* sample;
  uint32_t sampleLen;
  bool inZone;
  uint32_t lastTriggerMs;
};

extern DrumSensor sensors[NUM_SENSORS];

long getDistance(int trig, int echo);
void playDrum(const uint8_t* data, uint32_t length);
