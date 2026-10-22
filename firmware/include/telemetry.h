#pragma once
// Controller state JSON, Firebase state pushes and hourly kWh history.

#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"

extern uint32_t lastEnergyHistoryHourKey;

void buildState(JsonDocument &doc, bool forFirebase);
void maybeWriteEnergyHistory();
bool pushStateToFirebase();
