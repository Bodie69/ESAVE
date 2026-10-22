#pragma once
// Command execution shared by Bluetooth and Firebase, Firebase command polling and ACKs.

#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"

void handleJsonCommand(JsonDocument &doc, const String &source, bool fromBluetooth);
void pollFirebaseCommands();
// Switches MANUAL back to AUTO (saved in NVS) when no app link is left; see
// CONTROL_LINK_LOST_AUTO_MS. Takes stateMutex itself.
void fallBackToAutoMode(const char *reason);
