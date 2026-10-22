#pragma once
// Wi-Fi station link with credentials saved in NVS (nothing hardcoded), and the password-protected
// setup access point + captive portal used to enter them. The controller never needs Wi-Fi to run:
// counting, AUTO, relays, Bluetooth and the LCD work the same while offline or in setup mode.
//
// Everything below runs in firebaseNetworkTask (beginWifi() runs in setup() before that task
// starts), except requestWifiSetupPortal(), which only sets a flag and is safe from any task.

#include <Arduino.h>

#include "config.h"

extern volatile bool wifiPortalActive;  // setup access point is up
extern volatile bool wifiConfigured;    // credentials are saved in NVS
extern String wifiSsid;                 // saved network name; guarded by stateMutex

void beginWifi();
void maintainWifi();
// Reconnects the station with the saved credentials. Returns false when skipped (no credentials,
// or someone is connected to the setup portal and a reconnect would drop them).
bool restartWifiStation();
void requestWifiSetupPortal(const char *reason);
