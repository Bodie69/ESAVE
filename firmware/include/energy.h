#pragma once
// PZEM-004T v3 energy meter sampling and reset.

#include <Arduino.h>
#include <PZEM004Tv30.h>

#include "config.h"

extern HardwareSerial pzemSerial;
extern float voltage;
extern float currentA;
extern float powerW;
extern float energyKWh;
extern float frequencyHz;
extern float powerFactor;
extern bool pzemValid;
extern unsigned long lastPzemGoodAt;
extern uint64_t pzemSampleSequence;
extern unsigned long pzemInvalidSamples;

void readPzem();
bool resetPzemEnergy();
