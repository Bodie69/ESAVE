#pragma once
// Entrance/Exit E3F-DS200C4 (NPN NO) proximity sensing: ISRs, validated passage latching, noise guard, counting.

#include <Arduino.h>

#include "config.h"

extern unsigned long entranceEvents;
extern unsigned long exitEvents;
extern unsigned long exitUnderflowEvents;
extern unsigned long entranceRejectedShort;
extern unsigned long entranceRejectedLong;
extern unsigned long exitRejectedShort;
extern unsigned long exitRejectedLong;
extern unsigned long entranceNoiseRejected;
extern unsigned long exitNoiseRejected;
extern uint32_t entranceEdgesWindow;
extern uint32_t exitEdgesWindow;
extern uint64_t occupancyEventSequence;
extern unsigned long lastPassageLatencyMs;
extern String lastPassageDirection;
extern bool entranceSensorNoisy;
extern bool exitSensorNoisy;

struct BeamInput {
  uint8_t pin;
  bool clearLevel;
  bool lastRaw;
  bool stableRaw;
  bool broken;
  unsigned long rawChangedAt;
  unsigned long lastTriggeredAt;
};

extern BeamInput entranceBeam;
extern BeamInput exitBeam;

// GPIO CHANGE interrupt handlers (IRAM_ATTR is on the definitions).
void onEntranceBeamChange();
void onExitBeamChange();
void latchValidatedPassagesNow();
void calibrateIrSensors(const String &source);
void loadIrCalibration();
void processOccupancy();
