#pragma once
// Bluetooth Classic SPP link: JSON command intake, state pushes and ACKs.

#include <Arduino.h>
#include <BluetoothSerial.h>

#include "config.h"

extern BluetoothSerial SerialBT;
extern unsigned long lastBtCommandAt;
extern unsigned long lastBtStateAt;
// SPP is up (stays true until its heap is freed). Without BT_CLOUD_COEXIST, Bluetooth is switched
// off while Wi-Fi is connected so Firebase TLS gets its heap; Bluetooth and Firebase never run
// together. With it, Bluetooth stays on and the handoff below is unused.
extern volatile bool bluetoothActive;
// loop() wants Bluetooth on: firebaseNetworkTask then closes Firebase, frees its TLS heap and
// signs out before Bluetooth starts.
extern volatile bool bluetoothWanted;
// Handshake so Bluetooth starts only after Firebase is stopped: loop() bumps the request when
// bluetoothWanted rises, firebaseNetworkTask copies it to the ack once Firebase is stopped.
extern volatile uint32_t bluetoothPauseRequest;
extern volatile uint32_t bluetoothPauseAck;

// Last Bluetooth command other than get_state (0 = none); keeps a local session alive.
extern unsigned long lastBtControlAt;

bool startBluetooth();
void stopBluetooth();
// Starts a local Bluetooth session while online (the app asks through Firebase): Firebase is
// stopped and Bluetooth turned on, see BT_SESSION_WAIT_MS. Only sets a flag; safe from any task.
void requestBluetoothSession(const char *source);
// Called from loop(): Bluetooth is on while offline (after Wi-Fi has been down a while) or during
// a local session, and off otherwise (once Wi-Fi has been up a while).
void maintainBluetooth(bool wifiConnected);

void sendStateToBluetooth();
void sendBtAck(uint64_t id, bool ok, const String &action, const String &error = "");
void processBluetooth();
