#include "occupancy.h"
#include <driver/gpio.h>

#include "state.h"

// Internal helpers
static bool updateBeam(BeamInput &beam, unsigned long now);

BeamInput entranceBeam{ENTRANCE_IR_PIN, HIGH, HIGH, HIGH, false, 0, 0};
BeamInput exitBeam{EXIT_IR_PIN, HIGH, HIGH, HIGH, false, 0, 0};

// Hardware interrupts only capture beam edges. A dedicated high-priority sensor task
// validates a BREAK that remains active for PASSAGE_MIN_BREAK_MS and latches the count
// immediately, without waiting for the person to fully clear the beam. CLEAR only rearms
// the next passage. This keeps sub-30 ms electrical glitches rejected while removing the
// old "wait-until-clear" display delay.
static volatile uint32_t entrancePendingPassages = 0;
static volatile uint32_t exitPendingPassages = 0;
static volatile uint32_t entranceRejectedShortIsr = 0;
static volatile uint32_t entranceEdgeCountIsr = 0;
// Edges that flipped the ISR state. Spikes too short to still read at ISR entry do not count,
// so they cannot trip the noise guard (they cannot become a count either).
static volatile uint32_t entranceTransitionCountIsr = 0;
static volatile uint32_t entranceRejectedLongIsr = 0;
static volatile uint32_t exitRejectedShortIsr = 0;
static volatile uint32_t exitEdgeCountIsr = 0;
static volatile uint32_t exitTransitionCountIsr = 0;
static volatile uint32_t exitRejectedLongIsr = 0;
static volatile uint32_t entranceBreakStartedUs = 0;
static volatile uint32_t exitBreakStartedUs = 0;
static volatile uint32_t entranceLastAcceptedUs = 0;
static volatile uint32_t exitLastAcceptedUs = 0;
static volatile uint32_t entranceLastClearUs = 0;
static volatile uint32_t exitLastClearUs = 0;
static volatile bool entranceBrokenIsr = false;
static volatile bool exitBrokenIsr = false;
static volatile bool entranceCountedThisBreak = false;
static volatile bool exitCountedThisBreak = false;
static volatile bool entrancePulseEligible = true;
static volatile bool exitPulseEligible = true;
static volatile int entranceClearLevelIsr = HIGH;
static volatile int exitClearLevelIsr = HIGH;

void IRAM_ATTR onEntranceBeamChange() {
  if (entranceEdgeCountIsr < 0xFFFFFFFFUL) entranceEdgeCountIsr++;
  int raw = gpio_get_level((gpio_num_t)ENTRANCE_IR_PIN);
  uint32_t nowUs = micros();
  bool broken = raw != entranceClearLevelIsr;
  if (broken != entranceBrokenIsr && entranceTransitionCountIsr < 0xFFFFFFFFUL) entranceTransitionCountIsr++;
  if (broken && !entranceBrokenIsr) {
    entranceBrokenIsr = true;
    entranceBreakStartedUs = nowUs;
    entranceCountedThisBreak = false;
    entrancePulseEligible = entranceLastClearUs == 0 ||
        (uint32_t)(nowUs - entranceLastClearUs) >= IR_MIN_CLEAR_GAP_US;
  } else if (!broken && entranceBrokenIsr) {
    uint32_t widthUs = (uint32_t)(nowUs - entranceBreakStartedUs);
    entranceBrokenIsr = false;
    entranceLastClearUs = nowUs;
    if (!entranceCountedThisBreak) {
      if (!entrancePulseEligible || widthUs < PASSAGE_MIN_BREAK_US) {
        if (entranceRejectedShortIsr < 0xFFFFFFFFUL) entranceRejectedShortIsr++;
      } else if (widthUs > PASSAGE_MAX_BREAK_US) {
        if (entranceRejectedLongIsr < 0xFFFFFFFFUL) entranceRejectedLongIsr++;
      } else {
        // Fallback for an unusually busy scheduler: accept a valid completed pulse here.
        entranceCountedThisBreak = true;
        entranceLastAcceptedUs = nowUs;
        if (entrancePendingPassages < 1000) entrancePendingPassages++;
      }
    } else if (widthUs > PASSAGE_MAX_BREAK_US) {
      if (entranceRejectedLongIsr < 0xFFFFFFFFUL) entranceRejectedLongIsr++;
    }
  }
}

void IRAM_ATTR onExitBeamChange() {
  if (exitEdgeCountIsr < 0xFFFFFFFFUL) exitEdgeCountIsr++;
  int raw = gpio_get_level((gpio_num_t)EXIT_IR_PIN);
  uint32_t nowUs = micros();
  bool broken = raw != exitClearLevelIsr;
  if (broken != exitBrokenIsr && exitTransitionCountIsr < 0xFFFFFFFFUL) exitTransitionCountIsr++;
  if (broken && !exitBrokenIsr) {
    exitBrokenIsr = true;
    exitBreakStartedUs = nowUs;
    exitCountedThisBreak = false;
    exitPulseEligible = exitLastClearUs == 0 ||
        (uint32_t)(nowUs - exitLastClearUs) >= IR_MIN_CLEAR_GAP_US;
  } else if (!broken && exitBrokenIsr) {
    uint32_t widthUs = (uint32_t)(nowUs - exitBreakStartedUs);
    exitBrokenIsr = false;
    exitLastClearUs = nowUs;
    if (!exitCountedThisBreak) {
      if (!exitPulseEligible || widthUs < PASSAGE_MIN_BREAK_US) {
        if (exitRejectedShortIsr < 0xFFFFFFFFUL) exitRejectedShortIsr++;
      } else if (widthUs > PASSAGE_MAX_BREAK_US) {
        if (exitRejectedLongIsr < 0xFFFFFFFFUL) exitRejectedLongIsr++;
      } else {
        exitCountedThisBreak = true;
        exitLastAcceptedUs = nowUs;
        if (exitPendingPassages < 1000) exitPendingPassages++;
      }
    } else if (widthUs > PASSAGE_MAX_BREAK_US) {
      if (exitRejectedLongIsr < 0xFFFFFFFFUL) exitRejectedLongIsr++;
    }
  }
}

// Latch a person as soon as the beam has remained broken for the validated minimum.
// Runs outside the Arduino loop, so a slow HTTPS/TLS request cannot delay detection.
void latchValidatedPassagesNow() {
  uint32_t nowUs = micros();
  noInterrupts();
  if (entranceBrokenIsr && entrancePulseEligible && !entranceCountedThisBreak) {
    uint32_t widthUs = (uint32_t)(nowUs - entranceBreakStartedUs);
    if (widthUs >= PASSAGE_MIN_BREAK_US && widthUs <= PASSAGE_MAX_BREAK_US) {
      entranceCountedThisBreak = true;
      entranceLastAcceptedUs = nowUs;
      if (entrancePendingPassages < 1000) entrancePendingPassages++;
    }
  }
  if (exitBrokenIsr && exitPulseEligible && !exitCountedThisBreak) {
    uint32_t widthUs = (uint32_t)(nowUs - exitBreakStartedUs);
    if (widthUs >= PASSAGE_MIN_BREAK_US && widthUs <= PASSAGE_MAX_BREAK_US) {
      exitCountedThisBreak = true;
      exitLastAcceptedUs = nowUs;
      if (exitPendingPassages < 1000) exitPendingPassages++;
    }
  }
  interrupts();
}

unsigned long entranceEvents = 0;
unsigned long exitEvents = 0;
unsigned long exitUnderflowEvents = 0;
unsigned long entranceRejectedShort = 0;
unsigned long entranceRejectedLong = 0;
unsigned long exitRejectedShort = 0;
unsigned long exitRejectedLong = 0;
unsigned long entranceNoiseRejected = 0;
unsigned long exitNoiseRejected = 0;
uint32_t entranceEdgesWindow = 0;
uint32_t exitEdgesWindow = 0;
uint64_t occupancyEventSequence = 0;
unsigned long lastPassageLatencyMs = 0;
String lastPassageDirection = "none";
bool entranceSensorNoisy = false;
bool exitSensorNoisy = false;
static unsigned long entranceNoiseUntil = 0;
static unsigned long exitNoiseUntil = 0;
static unsigned long sensorNoiseWindowStartedAt = 0;
static uint32_t entranceShortWindow = 0;
static uint32_t exitShortWindow = 0;
static uint32_t entranceTransitionsWindow = 0;
static uint32_t exitTransitionsWindow = 0;
static unsigned long lastSensorNoiseLogAt = 0;

// Re-arm both ISR state machines from the current pin levels. Polarity is fixed (NPN NO:
// LOW = detected), so nothing is learned here.
static void resetBeamState() {
  entranceBeam.clearLevel = exitBeam.clearLevel = PROX_CLEAR_LEVEL;
  entranceBeam.lastRaw = entranceBeam.stableRaw = digitalRead(ENTRANCE_IR_PIN);
  exitBeam.lastRaw = exitBeam.stableRaw = digitalRead(EXIT_IR_PIN);
  entranceBeam.broken = entranceBeam.stableRaw == PROX_DETECT_LEVEL;
  exitBeam.broken = exitBeam.stableRaw == PROX_DETECT_LEVEL;

  noInterrupts();
  entranceClearLevelIsr = exitClearLevelIsr = PROX_CLEAR_LEVEL;
  entrancePendingPassages = 0;
  exitPendingPassages = 0;
  entranceRejectedShortIsr = entranceRejectedLongIsr = 0;
  exitRejectedShortIsr = exitRejectedLongIsr = 0;
  entranceEdgeCountIsr = 0;
  exitEdgeCountIsr = 0;
  entranceTransitionCountIsr = 0;
  exitTransitionCountIsr = 0;
  // A sensor that already detects something at reset is treated as mid-detection and already
  // counted: it must go clear before the next passage can count.
  entranceBrokenIsr = entranceBeam.broken;
  exitBrokenIsr = exitBeam.broken;
  entranceCountedThisBreak = entranceBeam.broken;
  exitCountedThisBreak = exitBeam.broken;
  entrancePulseEligible = true;
  exitPulseEligible = true;
  uint32_t irNowUs = micros();
  entranceBreakStartedUs = exitBreakStartedUs = irNowUs;
  entranceLastAcceptedUs = exitLastAcceptedUs = irNowUs;
  entranceLastClearUs = exitLastClearUs = irNowUs - IR_MIN_CLEAR_GAP_US;
  interrupts();

  if (entranceBeam.broken || exitBeam.broken) {
    Serial.printf("[PROX] WARNING detecting at re-arm: entrance=%s exit=%s. Check for an object/door within range, or OUT shorted to GND\n",
                  entranceBeam.broken ? "DETECT" : "clear", exitBeam.broken ? "DETECT" : "clear");
  }
}

// Kept for the app's CALIBRATE IR command: re-arms the sensors and reports their raw levels.
void calibrateIrSensors(const String &source) {
  resetBeamState();
  lastControlSource = source;
  markStateDirty("ir_calibration");
  Serial.printf("[PROX] re-armed (fixed NPN NO, LOW=detect): entrance raw=%d exit raw=%d\n",
                entranceBeam.stableRaw, exitBeam.stableRaw);
}

void loadIrCalibration() {
  // Earlier firmware learned the break-beam clear level and saved it. That value is meaningless
  // for the NPN NO switch (and could invert it), so drop it once.
  if (prefs.isKey("irCal")) {
    prefs.remove("irCal");
    prefs.remove("entClear");
    prefs.remove("exitClear");
    Serial.println("[PROX] removed stale break-beam calibration from NVS");
  }
  resetBeamState();
}

static bool updateBeam(BeamInput &beam, unsigned long now) {
  bool raw = digitalRead(beam.pin);
  if (raw != beam.lastRaw) {
    beam.lastRaw = raw;
    beam.rawChangedAt = now;
  }
  if (raw != beam.stableRaw && now - beam.rawChangedAt >= IR_DEBOUNCE_MS) {
    bool wasBroken = beam.broken;
    beam.stableRaw = raw;
    beam.broken = beam.stableRaw != beam.clearLevel;
    if (!wasBroken && beam.broken && now - beam.lastTriggeredAt >= IR_RETRIGGER_GUARD_MS) {
      beam.lastTriggeredAt = now;
      return true;
    }
  }
  return false;
}

// Caller must hold stateMutex.
void processOccupancy() {
  unsigned long now = millis();
  int countBefore = occupantCount;

  // Keep live beam state updated for diagnostics/UI. Counting comes only from complete,
  // instant-latched validated pulses captured by the realtime sensor task + GPIO ISRs.
  updateBeam(entranceBeam, now);
  updateBeam(exitBeam, now);

  uint32_t entered = 0, exited = 0;
  uint32_t entShort = 0, entLong = 0, exitShort = 0, exitLong = 0;
  uint32_t entEdges = 0, exitEdges = 0;
  uint32_t entTransitions = 0, exitTransitions = 0;
  uint32_t entAcceptedUs = 0, exitAcceptedUs = 0;
  noInterrupts();
  entered = entrancePendingPassages;
  exited = exitPendingPassages;
  entShort = entranceRejectedShortIsr;
  entLong = entranceRejectedLongIsr;
  exitShort = exitRejectedShortIsr;
  exitLong = exitRejectedLongIsr;
  entEdges = entranceEdgeCountIsr;
  exitEdges = exitEdgeCountIsr;
  entTransitions = entranceTransitionCountIsr;
  exitTransitions = exitTransitionCountIsr;
  entAcceptedUs = entranceLastAcceptedUs;
  exitAcceptedUs = exitLastAcceptedUs;
  entrancePendingPassages = 0;
  exitPendingPassages = 0;
  entranceRejectedShortIsr = entranceRejectedLongIsr = 0;
  exitRejectedShortIsr = exitRejectedLongIsr = 0;
  entranceEdgeCountIsr = 0;
  exitEdgeCountIsr = 0;
  entranceTransitionCountIsr = 0;
  exitTransitionCountIsr = 0;
  interrupts();

  // Track short-pulse bursts in a time window. A floating/open-collector input without a
  // proper pull-up can generate many rapid transitions. During a noise burst we
  // intentionally refuse to count passages because preserving accuracy is safer than guessing.
  if (sensorNoiseWindowStartedAt == 0 || now - sensorNoiseWindowStartedAt >= SENSOR_NOISE_WINDOW_MS) {
    sensorNoiseWindowStartedAt = now;
    entranceShortWindow = 0;
    exitShortWindow = 0;
    entranceEdgesWindow = 0;
    exitEdgesWindow = 0;
    entranceTransitionsWindow = 0;
    exitTransitionsWindow = 0;
  }
  entranceShortWindow += entShort;
  exitShortWindow += exitShort;
  entranceEdgesWindow += entEdges;
  exitEdgesWindow += exitEdges;
  entranceTransitionsWindow += entTransitions;
  exitTransitionsWindow += exitTransitions;

  if (!entranceSensorNoisy && (entranceShortWindow >= SENSOR_SHORT_PULSE_ALARM_COUNT ||
      entranceTransitionsWindow >= SENSOR_TRANSITION_BURST_ALARM_COUNT)) {
    entranceSensorNoisy = true;
    entranceNoiseUntil = now + SENSOR_NOISE_LOCKOUT_MS;
    entranceNoiseRejected += entered;
    entered = 0;
    occupancyValid = false;
    prefs.putBool("occValid", false);
    lastOccupancyEvent = "entrance_sensor_noise";
    lastControlSource = "entrance_ir_noise_guard";
    markStateDirty("entrance_sensor_noisy");
  }
  if (!exitSensorNoisy && (exitShortWindow >= SENSOR_SHORT_PULSE_ALARM_COUNT ||
      exitTransitionsWindow >= SENSOR_TRANSITION_BURST_ALARM_COUNT)) {
    exitSensorNoisy = true;
    exitNoiseUntil = now + SENSOR_NOISE_LOCKOUT_MS;
    exitNoiseRejected += exited;
    exited = 0;
    occupancyValid = false;
    prefs.putBool("occValid", false);
    lastOccupancyEvent = "exit_sensor_noise";
    lastControlSource = "exit_ir_noise_guard";
    markStateDirty("exit_sensor_noisy");
  }

  if (entranceSensorNoisy) {
    if ((long)(now - entranceNoiseUntil) >= 0) {
      entranceSensorNoisy = false;
      entranceShortWindow = 0;
      entranceEdgesWindow = 0;
      entranceTransitionsWindow = 0;
      markStateDirty("entrance_sensor_quiet");
      Serial.println("[PROX] entrance line quiet again; counting unlocked, but reconcile occupancy count once");
    } else if (entered) {
      entranceNoiseRejected += entered;
      entered = 0;
    }
  }
  if (exitSensorNoisy) {
    if ((long)(now - exitNoiseUntil) >= 0) {
      exitSensorNoisy = false;
      exitShortWindow = 0;
      exitEdgesWindow = 0;
      exitTransitionsWindow = 0;
      markStateDirty("exit_sensor_quiet");
      Serial.println("[PROX] exit line quiet again; counting unlocked, but reconcile occupancy count once");
    } else if (exited) {
      exitNoiseRejected += exited;
      exited = 0;
    }
  }

  if ((entranceSensorNoisy || exitSensorNoisy) &&
      (lastSensorNoiseLogAt == 0 || now - lastSensorNoiseLogAt >= 2000UL)) {
    lastSensorNoiseLogAt = now;
    Serial.printf("[PROX][NOISE] entrance=%s short=%lu flips=%lu edges=%lu exit=%s short=%lu flips=%lu edges=%lu. GPIO23/GPIO32 use INPUT_PULLUP; add 4.7k pull-ups to 3.3 V and check sensor 0 V/common GND if noise persists.\n",
                  entranceSensorNoisy ? "NOISY" : "OK", (unsigned long)entranceShortWindow, (unsigned long)entranceTransitionsWindow, (unsigned long)entranceEdgesWindow,
                  exitSensorNoisy ? "NOISY" : "OK", (unsigned long)exitShortWindow, (unsigned long)exitTransitionsWindow, (unsigned long)exitEdgesWindow);
  }

  if (entShort || entLong || exitShort || exitLong) {
    entranceRejectedShort += entShort;
    entranceRejectedLong += entLong;
    exitRejectedShort += exitShort;
    exitRejectedLong += exitLong;
    markStateDirty("occupancy_rejected_noise");
    // Do not spam Serial for every electrical glitch. The NOISE line above reports bursts;
    // isolated rejects remain available in the app counters/state JSON.
  }

  if (entered > 0) {
    int roomLeft = 999 - occupantCount;
    uint32_t accepted = min((uint32_t)max(0, roomLeft), entered);
    occupantCount += (int)accepted;
    entranceEvents += accepted;
    if (accepted > 0) {
      occupancyEventSequence++;
      lastPassageDirection = "entrance";
      lastPassageLatencyMs = entAcceptedUs ? (unsigned long)((uint32_t)(micros() - entAcceptedUs) / 1000UL) : 0;
    }
    lastOccupancyEvent = "entrance_fast_validated";
    lastControlSource = "entrance_ir_fast_isr";
    vacancyStartedAt = 0;
    prefs.putInt("people", occupantCount);
    markStateDirty("occupancy_entrance_fast_validated");
    Serial.printf("[COUNT][FAST] entrance x%lu -> %d processLatency=%lums event#%llu\n", (unsigned long)accepted, occupantCount, lastPassageLatencyMs, (unsigned long long)occupancyEventSequence);
  }

  if (exited > 0) {
    uint32_t available = occupantCount > 0 ? (uint32_t)occupantCount : 0;
    uint32_t applied = min(available, exited);
    occupantCount -= (int)applied;
    if (exited > applied) {
      exitUnderflowEvents += (exited - applied);
      occupantCount = 0;
      occupancyValid = false;
    }
    exitEvents += applied;
    if (applied > 0) {
      occupancyEventSequence++;
      lastPassageDirection = "exit";
      lastPassageLatencyMs = exitAcceptedUs ? (unsigned long)((uint32_t)(micros() - exitAcceptedUs) / 1000UL) : 0;
    }
    lastOccupancyEvent = "exit_fast_validated";
    lastControlSource = "exit_ir_fast_isr";
    if (occupantCount == 0) vacancyStartedAt = now;
    prefs.putInt("people", occupantCount);
    prefs.putBool("occValid", occupancyValid);
    markStateDirty("occupancy_exit_fast_validated");
    Serial.printf("[COUNT][FAST] exit x%lu -> %d processLatency=%lums event#%llu\n", (unsigned long)applied, occupantCount, lastPassageLatencyMs, (unsigned long long)occupancyEventSequence);
  }

  // ALL OFF (app or Bluetooth) persists MANUAL mode in NVS, so after it the IR keeps counting
  // but applyAutoRules() no longer drives the relays, even across reboots. Say so on Serial.
  if (occupantCount != countBefore && !automaticMode) {
    Serial.printf("[AUTO] MANUAL mode: count %d -> %d, relays NOT following occupancy; set mode AUTO in the app\n",
                  countBefore, occupantCount);
  }
}
