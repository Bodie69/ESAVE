#pragma once
// Shared controller state, the state mutex, NVS persistence and small helpers.

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"

extern Preferences prefs;
extern SemaphoreHandle_t stateMutex;
extern SemaphoreHandle_t pzemMutex;
extern bool relayState[RELAY_COUNT];
extern bool relayGpioOk[RELAY_COUNT];
extern bool automaticMode;
extern int occupantCount;
extern bool occupancyValid;
extern unsigned long vacancyStartedAt;
extern String lastOccupancyEvent;
extern String lastControlSource;
extern volatile bool stateDirty;
extern volatile bool bluetoothStateDirty;
extern volatile bool firebaseStateDirty;
extern String stateDirtyReason;
extern volatile uint32_t telemetryRevision;
extern uint64_t lastRelayCommandId[RELAY_COUNT];
extern uint64_t lastModeCommandId;
extern uint64_t lastAllOffCommandId;
extern uint64_t lastSetCountCommandId;
extern uint64_t lastCalibrateIrCommandId;
extern uint64_t lastResetEnergyCommandId;
extern uint64_t lastBluetoothSessionCommandId;

struct StateLock {
  SemaphoreHandle_t mutex;
  bool held;
  explicit StateLock(SemaphoreHandle_t m = stateMutex, TickType_t wait = portMAX_DELAY)
      : mutex(m), held(m != nullptr && xSemaphoreTakeRecursive(m, wait) == pdTRUE) {}
  ~StateLock() { if (held) xSemaphoreGiveRecursive(mutex); }
  StateLock(const StateLock &) = delete;
  StateLock &operator=(const StateLock &) = delete;
};

String roomPath(const String &child = "");
void markStateDirty(const String &reason);
unsigned long epochNow();
void loadRuntimeState();
