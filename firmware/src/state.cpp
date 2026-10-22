#include "state.h"

#include "telemetry.h"

Preferences prefs;

// Controller state is shared by loop() (Bluetooth/PZEM/LCD), firebaseNetworkTask and
// sensorRealtimeTask. Every read/write of shared state (including Arduino Strings) is
// done while holding stateMutex. Never hold it across HTTPS, Bluetooth or PZEM I/O.
SemaphoreHandle_t stateMutex = nullptr;

// PZEM Modbus is used by loop() (sampling) and by the command paths (energy reset).
SemaphoreHandle_t pzemMutex = nullptr;
bool relayState[RELAY_COUNT] = {false, false, false, false, false, false};
bool relayGpioOk[RELAY_COUNT] = {true, true, true, true, true, true};
bool automaticMode = true;
int occupantCount = 0;
bool occupancyValid = true;
unsigned long vacancyStartedAt = 0;
String lastOccupancyEvent = "boot";
String lastControlSource = "boot";

// Set whenever real controller state changes so the app receives events quickly.
volatile bool stateDirty = true;
volatile bool bluetoothStateDirty = true;
volatile bool firebaseStateDirty = true;
String stateDirtyReason = "boot";
volatile uint32_t telemetryRevision = 1;
uint64_t lastRelayCommandId[RELAY_COUNT] = {0,0,0,0,0,0};
uint64_t lastModeCommandId = 0;
uint64_t lastAllOffCommandId = 0;
uint64_t lastSetCountCommandId = 0;
uint64_t lastCalibrateIrCommandId = 0;
uint64_t lastBluetoothSessionCommandId = 0;
uint64_t lastResetEnergyCommandId = 0;

String roomPath(const String &child) {
  String p = String("/rooms/") + ROOM_ID;
  if (child.length()) p += "/" + child;
  return p;
}

// Caller must hold stateMutex (stateDirtyReason is a shared String).
void markStateDirty(const String &reason) {
  telemetryRevision++;
  stateDirty = true;
  bluetoothStateDirty = true;
  firebaseStateDirty = true;
  stateDirtyReason = reason;
}

unsigned long epochNow() {
  time_t now = time(nullptr);
  return now > 1700000000 ? (unsigned long)now : 0;
}

void loadRuntimeState() {
  occupantCount = constrain(prefs.getInt("people", 0), 0, 999);
  // After a controller reboot, a nonzero saved count cannot be guaranteed to be exact.
  occupancyValid = false; // power loss creates an unavoidable counting gap; app must reconcile once
  automaticMode = prefs.getBool("autoMode", true);
  // The lights-off delay is fixed (VACANCY_DELAY_MS). Clear values saved by firmware
  // V25.14-V25.16, which let the app change it.
  if (prefs.isKey("vacSec")) prefs.remove("vacSec");
  if (prefs.isKey("cmdVacancy")) prefs.remove("cmdVacancy");
  for (int i = 0; i < RELAY_COUNT; i++) {
    String k = String("cmd") + i;
    lastRelayCommandId[i] = prefs.getULong64(k.c_str(), 0);
  }
  lastModeCommandId = prefs.getULong64("cmdMode", 0);
  lastAllOffCommandId = prefs.getULong64("cmdAllOff", 0);
  lastSetCountCommandId = prefs.getULong64("cmdCount", 0);
  lastCalibrateIrCommandId = prefs.getULong64("cmdCalIR", 0);
  lastBluetoothSessionCommandId = prefs.getULong64("cmdBtSess", 0);
  lastResetEnergyCommandId = prefs.getULong64("cmdPzemRst", 0);
  lastEnergyHistoryHourKey = prefs.getULong("kwhHourKey", 0);
}
