#include "relays.h"

#include "state.h"

// Internal helpers
static void driveAutoChannels(uint8_t onMask, const char *source);

void setRelay(uint8_t index, bool on, const String &source) {
  if (index >= RELAY_COUNT) return;

  bool previousState = relayState[index];
  bool previousVerify = relayGpioOk[index];

  // Always drive the requested level, even if the cached state already matches.
  int expectedLevel = RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW);
  digitalWrite(RELAY_PINS[index], expectedLevel);
  delayMicroseconds(50);

  // GPIO readback verifies the ESP32 output level. This does NOT prove mechanical contact movement.
  int actualLevel = digitalRead(RELAY_PINS[index]);
  relayGpioOk[index] = (actualLevel == expectedLevel);
  relayState[index] = RELAY_ACTIVE_LOW ? (actualLevel == LOW) : (actualLevel == HIGH);
  lastControlSource = source;

  // Log only transitions. AUTO re-asserts channels from the 2 ms sensor task, so logging
  // every call would flood the UART and stall that task.
  if (relayState[index] == previousState && relayGpioOk[index] == previousVerify) return;

  markStateDirty(String("relay_ch") + String(index + 1));

  if (!relayGpioOk[index]) {
    Serial.printf("[RELAY] CH%d GPIO verify failed requested=%d expectedLevel=%d actualLevel=%d\n",
                  index + 1, on ? 1 : 0, expectedLevel, actualLevel);
  } else {
    Serial.printf("[RELAY] CH%d %s GPIO verified\n", index + 1, relayState[index] ? "ON" : "OFF");
  }
}

void setAllRelays(bool on, const String &source, bool includeSpares) {
  int limit = RELAY_COUNT; // CH7/CH8 removed; only six classroom loads are controllable.
  for (int i = 0; i < limit; i++) setRelay(i, on, source);
}

// Drive each AUTO channel to its bit in onMask, only when it differs from the target (or
// failed GPIO verify), so the 2 ms sensor task does not rewrite every relay on every pass.
static void driveAutoChannels(uint8_t onMask, const char *source) {
  for (int i = 0; i < RELAY_COUNT; i++) {
    if (!(AUTO_CHANNEL_MASK & (1 << i))) continue;
    bool on = (onMask & (1 << i)) != 0;
    if (relayState[i] != on || !relayGpioOk[i]) setRelay(i, on, source);
  }
}

// Caller must hold stateMutex.
void applyAutoRules() {
  if (!automaticMode) return;
  // AUTO always follows the best-estimate count. When the count is flagged uncertain
  // (after reboot, sensor noise or exit underflow) the flag is reported to the app so the
  // operator can correct it, but automation keeps running: freezing here used to leave
  // lights stuck ON (after underflow) or OFF with people inside (after reboot).
  unsigned long now = millis();
  if (occupantCount > AUTO_LOW_OCCUPANCY_MAX) {
    vacancyStartedAt = 0;
    driveAutoChannels(AUTO_CHANNEL_MASK, "auto_occupied_full");
  } else if (occupantCount > 0) {
    // Dropping from 11+ to 10 or fewer switches the extra channels off right away.
    vacancyStartedAt = 0;
    driveAutoChannels(AUTO_LOW_OCCUPANCY_MASK, "auto_occupied_low");
  } else {
    if (vacancyStartedAt == 0) vacancyStartedAt = now;
    if (now - vacancyStartedAt >= VACANCY_DELAY_MS) driveAutoChannels(0, "auto_vacant");
  }
}

int relayIndexForKey(const String &key) {
  for (int i = 0; i < RELAY_COUNT; i++) if (key.equalsIgnoreCase(RELAY_KEYS[i])) return i;
  return -1;
}
