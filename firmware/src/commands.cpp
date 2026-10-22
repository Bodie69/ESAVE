#include "commands.h"
#include <WiFi.h>

#include "bluetooth.h"
#include "energy.h"
#include "cloud.h"
#include "occupancy.h"
#include "provisioning.h"
#include "relays.h"
#include "state.h"

// Internal helpers
static void writeFirebaseAck(uint64_t id, const String &action, bool ok, const String &detail = "");
static bool applyRelayCommand(int index, bool state, const String &source, String &error);
static void setMode(bool autoMode, const String &source);
static void setOccupancyCount(int count, const String &source);
static bool allOffAndManual(const String &source);
static bool firebaseCommandExpired(JsonObject cmd);

static void writeFirebaseAck(uint64_t id, const String &action, bool ok, const String &detail) {
  JsonDocument doc;
  doc["command_id"] = id;
  doc["action"] = action;
  doc["ok"] = ok;
  doc["detail"] = detail;
  doc["device_epoch"] = epochNow();
  JsonObject sv = doc["server_timestamp_ms"].to<JsonObject>();
  sv[".sv"] = "timestamp";
  String body, response;
  serializeJson(doc, body);
  // Per-command slot so concurrent commands from several phones cannot overwrite each
  // other's ACK; the app deletes its entry after reading it. acks/last stays for older apps.
  firebaseRequest("PUT", roomPath("acks/by_id/") + String((unsigned long long)id), body, response);
  firebaseRequest("PUT", roomPath("acks/last"), body, response);
}

static bool applyRelayCommand(int index, bool state, const String &source, String &error) {
  if (index < 0 || index >= RELAY_COUNT) { error = "UNKNOWN_DEVICE"; return false; }
  if (automaticMode && index < 6) {
    error = "AUTO_MODE_MANUAL_RELAY_BLOCKED";
    return false;
  }
  setRelay(index, state, source);
  if (!relayGpioOk[index] || relayState[index] != state) {
    error = "RELAY_GPIO_VERIFY_FAILED";
    return false;
  }
  return true;
}

static void setMode(bool autoMode, const String &source) {
  automaticMode = autoMode;
  prefs.putBool("autoMode", automaticMode);
  lastControlSource = source;
  markStateDirty("mode_change");
  if (automaticMode) applyAutoRules();
}

static void setOccupancyCount(int count, const String &source) {
  occupantCount = constrain(count, 0, 999);
  occupancyValid = true;
  prefs.putInt("people", occupantCount);
  prefs.putBool("occValid", true);
  lastControlSource = source;
  lastOccupancyEvent = "manual_correction";
  markStateDirty("occupancy_manual_correction");
  if (occupantCount == 0) vacancyStartedAt = millis(); else vacancyStartedAt = 0;
  applyAutoRules();
}

static bool allOffAndManual(const String &source) {
  automaticMode = false;

  prefs.putBool("autoMode", false);
  lastControlSource = source;
  markStateDirty("all_off");

  setAllRelays(false, source, true);

  bool verified = true;
  for (int i = 0; i < RELAY_COUNT; i++) {
    if (relayState[i] || !relayGpioOk[i]) verified = false;
  }
  return verified;
}

void fallBackToAutoMode(const char *reason) {
  StateLock lock;
  if (automaticMode) return;
  Serial.printf("[AUTO] %s: MANUAL -> AUTO, relays follow occupancy again\n", reason);
  setMode(true, "link_lost_auto");
}

void handleJsonCommand(JsonDocument &doc, const String &source, bool fromBluetooth) {
  String cmd = doc["cmd"] | "";
  uint64_t id = doc["id"] | (uint64_t)millis();
  cmd.toLowerCase();
  String error;
  bool ok = true;
  if (cmd == "get_state") {
    if (fromBluetooth) sendStateToBluetooth();
    return;
  } else if (cmd == "reset_energy") {
    // PZEM Modbus I/O is slow; it runs without holding stateMutex.
    if (!resetPzemEnergy()) { ok = false; error = "PZEM_RESET_FAILED"; }
  } else {
    // State changes run under stateMutex; the ACK/state replies below are sent after release.
    StateLock lock;
    if (cmd == "set_mode") {
      String mode = doc["mode"] | "";
      mode.toLowerCase();
      if (mode == "auto") setMode(true, source);
      else if (mode == "manual") setMode(false, source);
      else { ok = false; error = "BAD_MODE"; }
    } else if (cmd == "set_relay") {
      String device = doc["device"] | "";
      bool state = doc["state"] | false;
      ok = applyRelayCommand(relayIndexForKey(device), state, source, error);
    } else if (cmd == "all_off") {
      ok = allOffAndManual(source);
      if (!ok) error = "RELAY_GPIO_VERIFY_FAILED";
    } else if (cmd == "set_count") {
      int count = doc["count"] | -1;
      if (count < 0 || count > 999) { ok = false; error = "BAD_COUNT"; }
      else setOccupancyCount(count, source);
    } else if (cmd == "calibrate_ir") {
      calibrateIrSensors(source);
    } else if (cmd == "wifi_setup" && fromBluetooth) {
      // Local only: opens the setup AP so the room can be moved to another Wi-Fi network.
      requestWifiSetupPortal("bluetooth");
    } else {
      ok = false; error = "UNKNOWN_COMMAND";
    }
  }
  if (fromBluetooth) {
    sendBtAck(id, ok, cmd, error);
    sendStateToBluetooth();
  }
}

// ACKs are collected while stateMutex is held and written to Firebase after release.
struct PendingFirebaseAck {
  uint64_t id;
  String action;
  bool ok;
  String detail;
};

// A command older than FIREBASE_COMMAND_MAX_AGE_MS is rejected instead of executed late.
// Without NTP time or without requested_at_ms the age is unknown, so it is accepted.
static bool firebaseCommandExpired(JsonObject cmd) {
  uint64_t requestedAtMs = cmd["requested_at_ms"] | 0ULL;
  unsigned long epoch = epochNow();
  if (!requestedAtMs || !epoch) return false;
  uint64_t nowMs = (uint64_t)epoch * 1000ULL;
  return nowMs > requestedAtMs && nowMs - requestedAtMs > FIREBASE_COMMAND_MAX_AGE_MS;
}

void pollFirebaseCommands() {
  if (WiFi.status() != WL_CONNECTED) return;
  String response;
  if (!firebaseRequest("GET", roomPath("commands"), "", response)) return;
  JsonDocument root;
  if (deserializeJson(root, response)) return;
  PendingFirebaseAck acks[RELAY_COUNT + 6]; // mode, relays, all_off, set_count, calibrate_ir, reset_energy, bluetooth_session
  int ackCount = 0;
  auto queueAck = [&](uint64_t id, const String &action, bool ok, const String &detail) {
    if (ackCount < (int)(sizeof(acks) / sizeof(acks[0]))) acks[ackCount++] = PendingFirebaseAck{id, action, ok, detail};
  };
  // Energy reset needs slow PZEM Modbus I/O, so it runs before stateMutex is taken.
  JsonObject rst = root["reset_energy"].as<JsonObject>();
  if (!rst.isNull()) {
    uint64_t id = rst["command_id"] | 0ULL;
    if (id && id != lastResetEnergyCommandId) {
      bool expired = firebaseCommandExpired(rst);
      bool ok = !expired && resetPzemEnergy();
      lastResetEnergyCommandId = id;
      prefs.putULong64("cmdPzemRst", id);
      queueAck(id, "reset_energy", ok, expired ? "COMMAND_EXPIRED" : (ok ? "OK" : "PZEM_RESET_FAILED"));
    }
  }
  // Stops Firebase and turns Bluetooth on; the ACK below is written before Firebase stops.
  JsonObject btSession = root["bluetooth_session"].as<JsonObject>();
  if (!btSession.isNull()) {
    uint64_t id = btSession["command_id"] | 0ULL;
    if (id && id != lastBluetoothSessionCommandId) {
      bool expired = firebaseCommandExpired(btSession);
      if (!expired) requestBluetoothSession("app");
      lastBluetoothSessionCommandId = id;
      prefs.putULong64("cmdBtSess", id);
      queueAck(id, "bluetooth_session", !expired, expired ? "COMMAND_EXPIRED" : "BLUETOOTH_ON");
    }
  }
  {
    StateLock lock;
    JsonObject modeCmd = root["mode"].as<JsonObject>();
    if (!modeCmd.isNull()) {
      uint64_t id = modeCmd["command_id"] | 0ULL;
      String m = modeCmd["mode"] | "";
      m.toLowerCase();
      if (id && id != lastModeCommandId) {
        bool expired = firebaseCommandExpired(modeCmd);
        bool ok = !expired && (m == "auto" || m == "manual");
        if (ok) setMode(m == "auto", "firebase");
        lastModeCommandId = id;
        prefs.putULong64("cmdMode", id);
        queueAck(id, "set_mode", ok, expired ? "COMMAND_EXPIRED" : (ok ? (automaticMode ? "AUTO" : "MANUAL") : "BAD_MODE"));
      }
    }
    JsonObject devices = root["devices"].as<JsonObject>();
    if (!devices.isNull()) {
      for (int i = 0; i < RELAY_COUNT; i++) {
        JsonObject c = devices[RELAY_KEYS[i]].as<JsonObject>();
        if (c.isNull()) continue;
        uint64_t id = c["command_id"] | 0ULL;
        if (id == 0 || id == lastRelayCommandId[i]) continue;
        bool requested = c["state"] | false;
        String error;
        bool ok = false;
        if (firebaseCommandExpired(c)) error = "COMMAND_EXPIRED";
        else ok = applyRelayCommand(i, requested, "firebase", error);
        lastRelayCommandId[i] = id;
        String k = String("cmd") + i;
        prefs.putULong64(k.c_str(), id);
        queueAck(id, String("set_relay:") + RELAY_KEYS[i], ok, ok ? (relayState[i] ? "ON" : "OFF") : error);
      }
    }
    JsonObject allOff = root["all_off"].as<JsonObject>();
    if (!allOff.isNull()) {
      uint64_t id = allOff["command_id"] | 0ULL;
      if (id && id != lastAllOffCommandId) {
        bool expired = firebaseCommandExpired(allOff);
        bool ok = !expired && allOffAndManual("firebase");
        lastAllOffCommandId = id;
        prefs.putULong64("cmdAllOff", id);
        queueAck(id, "all_off", ok, expired ? "COMMAND_EXPIRED" : (ok ? "MANUAL_ALL_OFF" : "RELAY_GPIO_VERIFY_FAILED"));
      }
    }
    JsonObject setCount = root["set_count"].as<JsonObject>();
    if (!setCount.isNull()) {
      uint64_t id = setCount["command_id"] | 0ULL;
      int count = setCount["count"] | -1;
      if (id && id != lastSetCountCommandId) {
        bool expired = firebaseCommandExpired(setCount);
        bool ok = !expired && count >= 0 && count <= 999;
        if (ok) setOccupancyCount(count, "firebase");
        lastSetCountCommandId = id;
        prefs.putULong64("cmdCount", id);
        queueAck(id, "set_count", ok, expired ? String("COMMAND_EXPIRED") : (ok ? String(occupantCount) : String("BAD_COUNT")));
      }
    }
    JsonObject cal = root["calibrate_ir"].as<JsonObject>();
    if (!cal.isNull()) {
      uint64_t id = cal["command_id"] | 0ULL;
      if (id && id != lastCalibrateIrCommandId) {
        bool expired = firebaseCommandExpired(cal);
        if (!expired) calibrateIrSensors("firebase");
        lastCalibrateIrCommandId = id;
        prefs.putULong64("cmdCalIR", id);
        queueAck(id, "calibrate_ir", !expired, expired ? "COMMAND_EXPIRED" : "CLEAR_LEVELS_SAVED");
      }
    }
  }
  for (int i = 0; i < ackCount; i++) writeFirebaseAck(acks[i].id, acks[i].action, acks[i].ok, acks[i].detail);
}
