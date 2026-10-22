#pragma once
// Relay output control and the AUTO occupancy rules.

#include <Arduino.h>

#include "config.h"

void setRelay(uint8_t index, bool on, const String &source);
void setAllRelays(bool on, const String &source, bool includeSpares = false);
void applyAutoRules();
int relayIndexForKey(const String &key);
