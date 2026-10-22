#include "bluetooth.h"
#include <ArduinoJson.h>
#include <esp_bt.h>

#include "commands.h"
#include "relays.h"
#include "state.h"
#include "telemetry.h"

// Internal helpers
static void writeBluetoothLine(JsonDocument &doc);
#if BLUETOOTH_LEGACY_TEXT_COMMANDS
static void handleLegacyBluetoothText(String line);
#endif

BluetoothSerial SerialBT;
static String btRx;
unsigned long lastBtCommandAt = 0;
unsigned long lastBtStateAt = 0;
volatile bool bluetoothActive = false;
volatile bool bluetoothWanted = true; // setup() starts Bluetooth before Wi-Fi and Firebase
volatile uint32_t bluetoothPauseRequest = 0;
volatile uint32_t bluetoothPauseAck = 0;
unsigned long lastBtControlAt = 0;
static volatile bool bluetoothSessionRequested = false;

#if ESP_ARDUINO_VERSION_MAJOR < 3
// Core 2.x btStart() enables the controller in dual mode (Classic + BLE) and keeps the BLE
// controller memory reserved although only Classic SPP is used. That left too little heap for
// a TLS handshake, so Firebase sign-in was skipped forever ("low memory: TLS skipped").
// Start the controller Classic-only after releasing BLE memory; SerialBT.begin() then sees it
// already enabled. Core 3.x does the same inside SerialBT.begin(name, false, true).
static bool startClassicBtController() {
  static bool bleMemReleased = false; // the release is one-way; do it only on the first start
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
    if (!bleMemReleased) {
      esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
      bleMemReleased = true;
    }
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    cfg.mode = ESP_BT_MODE_CLASSIC_BT;
    if (esp_bt_controller_init(&cfg) != ESP_OK) return false;
    while (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {}
  }
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED &&
      esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
    return false;
  }
  return esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED;
}
#endif

bool startBluetooth() {
  if (bluetoothActive) return true;
  Serial.printf("[MEM] before Bluetooth heap=%u maxAlloc=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  bool btOk = SerialBT.begin(BT_NAME, false, true); // Classic only: release BLE memory
#else
  bool btOk = startClassicBtController() && SerialBT.begin(BT_NAME);
#endif
  Serial.printf("[MEM] after Bluetooth heap=%u maxAlloc=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  if (!btOk) {
    Serial.println("[BT] ERROR: Bluetooth SPP failed to start");
    SerialBT.end(); // undo a half-started stack so the next attempt starts clean
    return false;
  }
  // BluetoothSerial keeps the PIN and re-applies it on every begin(); setting it again is harmless.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  bool pinOk = SerialBT.setPin(BLUETOOTH_PIN, strlen(BLUETOOTH_PIN));
#else
  bool pinOk = SerialBT.setPin(BLUETOOTH_PIN);
#endif
  btRx = "";
  bluetoothActive = true;
  Serial.printf("[BT] ready: %s • fixed pairing PIN %s\n", BT_NAME, pinOk ? "configured" : "FAILED");
  return true;
}

void stopBluetooth() {
  if (!bluetoothActive) return;
  SerialBT.end(); // SPP, Bluedroid and the controller are deinitialized and their heap freed
  // Cleared only now: firebaseNetworkTask waits for this before it starts TLS again.
  bluetoothActive = false;
  btRx = "";
  Serial.printf("[BT] off while online: heap=%u maxAlloc=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void maintainBluetooth(bool wifiConnected) {
  // Debounced both ways: Wi-Fi status flickers while joining, and a short drop should not
  // bounce a phone's Bluetooth link.
  static unsigned long wifiChangedAt = 0;
  static bool lastWifiConnected = false;
  static unsigned long lastStartAttemptAt = 0;
  static bool inSession = false;
  static unsigned long sessionUntil = 0;
  static unsigned long sessionControlAt = 0; // session start or last control command, if later
  unsigned long now = millis();
  if (wifiConnected != lastWifiConnected) {
    lastWifiConnected = wifiConnected;
    wifiChangedAt = now;
  }
  unsigned long stableFor = now - wifiChangedAt;

  if (bluetoothSessionRequested) {
    bluetoothSessionRequested = false;
    inSession = true;
    sessionUntil = now + BT_SESSION_WAIT_MS;
    sessionControlAt = now;
    lastStartAttemptAt = 0; // start as soon as Firebase is stopped, not after the retry interval
  }
  if (inSession) {
    // Kept alive while a phone is connected, then BT_SESSION_IDLE_MS after it leaves...
    if (bluetoothActive && SerialBT.hasClient() && (long)(now + BT_SESSION_IDLE_MS - sessionUntil) > 0) {
      sessionUntil = now + BT_SESSION_IDLE_MS;
    }
    // ...but a phone left connected must not keep the cloud off for hours.
    if ((long)(lastBtControlAt - sessionControlAt) > 0) sessionControlAt = lastBtControlAt;
    if ((long)(now - sessionUntil) >= 0) {
      inSession = false;
      Serial.println("[BT] local session ended: no phone connected");
    } else if (now - sessionControlAt >= BT_SESSION_CONTROL_IDLE_MS) {
      inSession = false;
      Serial.println("[BT] local session ended: no commands for 10 min");
    }
  }

  bool want;
  if (BT_CLOUD_COEXIST || inSession) want = true;
  else if (wifiConnected) want = bluetoothActive && stableFor < BT_OFF_AFTER_WIFI_UP_MS;
  else want = bluetoothActive || stableFor >= BT_ON_AFTER_WIFI_DOWN_MS;

  if (want && !bluetoothWanted) bluetoothPauseRequest++; // before bluetoothWanted, see bluetooth.h
  bluetoothWanted = want;

  if (!want) {
    if (bluetoothActive && SerialBT.hasClient()) {
      // Tell the app why the link is about to drop, so it goes back to the cloud instead of
      // retrying a controller whose Bluetooth is off.
      JsonDocument bye;
      bye["type"] = "bye";
      bye["reason"] = "cloud_resume";
      writeBluetoothLine(bye);
      delay(300); // let the line go out before the stack is torn down
    }
    stopBluetooth();
  } else if (!bluetoothActive && bluetoothPauseAck == bluetoothPauseRequest &&
             (lastStartAttemptAt == 0 || now - lastStartAttemptAt >= BT_ON_AFTER_WIFI_DOWN_MS)) {
    lastStartAttemptAt = now;
    Serial.println(BT_CLOUD_COEXIST ? "[BT] restarting Bluetooth"
                   : inSession ? "[BT] Firebase stopped: starting Bluetooth for a local session"
                               : "[BT] Wi-Fi down, Firebase stopped: restarting Bluetooth");
    startBluetooth();
  }
}

void requestBluetoothSession(const char *source) {
  Serial.printf("[BT] local session requested by %s\n", source);
  bluetoothSessionRequested = true;
}

void sendStateToBluetooth() {
  if (!SerialBT.hasClient()) return;

  JsonDocument doc;

  {
    StateLock lock;
    buildState(doc, false);
  }

  doc["type"] = "state";

  writeBluetoothLine(doc);
}

// Serialize first, then write once. serializeJson() straight to SerialBT issued one SPP packet
// per token; when the link was congested each could block ~1 s, so a single state push stalled
// loop() past the 30 s task watchdog and rebooted the controller.
static void writeBluetoothLine(JsonDocument &doc) {
  String line;
  serializeJson(doc, line);
  line += '\n';
  SerialBT.write((const uint8_t *)line.c_str(), line.length());
}

void sendBtAck(uint64_t id, bool ok, const String &action, const String &error) {
  if (!SerialBT.hasClient()) return;
  JsonDocument doc;
  doc["type"] = "ack";
  doc["id"] = id;
  doc["ok"] = ok;
  doc["action"] = action;
  if (error.length()) doc["error"] = error;
  writeBluetoothLine(doc);
}

#if BLUETOOTH_LEGACY_TEXT_COMMANDS
static void handleLegacyBluetoothText(String line) {
  line.trim();
  String u = line;
  u.toUpperCase();
  JsonDocument doc;
  doc["id"] = (uint64_t)millis();
  if (u == "STATUS") doc["cmd"] = "get_state";
  else if (u == "MODE AUTO" || u == "AUTO") { doc["cmd"] = "set_mode"; doc["mode"] = "auto"; }
  else if (u == "MODE MANUAL" || u == "MANUAL") { doc["cmd"] = "set_mode"; doc["mode"] = "manual"; }
  else if (u == "ALL OFF") doc["cmd"] = "all_off";
  else if (u == "CALIBRATE IR") doc["cmd"] = "calibrate_ir";
  else if (u == "RESET ENERGY") doc["cmd"] = "reset_energy";
  else {
    int space = u.indexOf(' ');
    if (space > 0) {
      String dev = u.substring(0, space); dev.toLowerCase();
      String op = u.substring(space + 1);
      if (relayIndexForKey(dev) >= 0 && (op == "ON" || op == "OFF")) {
        doc["cmd"] = "set_relay"; doc["device"] = dev; doc["state"] = (op == "ON");
      } else {
        sendBtAck((uint64_t)millis(), false, "legacy", "UNKNOWN_COMMAND"); return;
      }
    } else {
      sendBtAck((uint64_t)millis(), false, "legacy", "UNKNOWN_COMMAND"); return;
    }
  }
  handleJsonCommand(doc, "bluetooth", true);
}
#endif

void processBluetooth() {
  if (!bluetoothActive) return;
  while (SerialBT.available()) {
    char c = (char)SerialBT.read();
    if (c == '\n' || c == '\r') {
      if (btRx.length()) {
        btRx.trim();
        lastBtCommandAt = millis();
        if (btRx.startsWith("{")) {
          JsonDocument doc;
          DeserializationError e = deserializeJson(doc, btRx);
          if (e) {
            sendBtAck((uint64_t)millis(), false, "parse", "BAD_JSON");
          } else {
            // get_state is the app's background polling, not someone using the room.
            if (strcmp(doc["cmd"] | "", "get_state") != 0) lastBtControlAt = millis();
            handleJsonCommand(doc, "bluetooth", true);
          }
        } else {
#if BLUETOOTH_LEGACY_TEXT_COMMANDS
          handleLegacyBluetoothText(btRx);
#else
          sendBtAck((uint64_t)millis(), false, "parse", "JSON_REQUIRED");
#endif
        }
        btRx = "";
      }
    } else if (btRx.length() < 1024) {
      btRx += c;
    } else {
      btRx = "";
      sendBtAck((uint64_t)millis(), false, "parse", "COMMAND_TOO_LONG");
    }
  }
}
