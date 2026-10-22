// ESAVE - Room 206 controller firmware (see README.md).
// ESP32-WROOM-32 - 2x IR break-beam, 6 relays, PZEM-004T v3, I2C LCD,
// Bluetooth Classic SPP + Firebase Realtime Database over verified HTTPS.
//
// Tasks: sensorRealtimeTask (IR counting + AUTO, priority 4), firebaseNetworkTask (Firebase,
// priority 1) and the Arduino loop() below (Bluetooth, PZEM sampling, LCD, watchdog checks).

#include <Arduino.h>
#include <WiFi.h>

#include "bluetooth.h"
#include "display.h"
#include "energy.h"
#include "cloud.h"
#include "commands.h"
#include "occupancy.h"
#include "provisioning.h"
#include "relays.h"
#include "state.h"
#include "tasks.h"

static unsigned long lastPzemReadAt = 0;
static unsigned long lastMemDiagAt = 0;
static unsigned long setupButtonDownAt = 0;
static bool setupButtonFired = false;
static unsigned long controlLinkLostSince = 0;  // 0 = Firebase or a Bluetooth phone is connected

// Returns true when the app can reach the controller: Firebase answered recently, or a phone is
// connected over Bluetooth.
static bool controlLinkUp() {
  bool firebaseUp = firebaseAuthenticated && lastFirebaseOkAt && millis() - lastFirebaseOkAt < 15000UL;
  return firebaseUp || (bluetoothActive && SerialBT.hasClient());
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("ESAVE V" FIRMWARE_VERSION " - realtime IR + verified Firebase + Bluetooth");

  // Must exist before any task starts or any StateLock is taken.
  stateMutex = xSemaphoreCreateRecursiveMutex();
  pzemMutex = xSemaphoreCreateRecursiveMutex();
  if (!stateMutex || !pzemMutex) {
    Serial.println("[BOOT] FATAL: could not allocate state mutexes; restarting");
    delay(1000);
    ESP.restart();
  }

  setupTaskWatchdog();
#if FIREBASE_TLS_INSECURE
  Serial.println("[TLS] WARNING: FIREBASE_TLS_INSECURE=1, server certificates are NOT verified");
#else
  Serial.println("[TLS] Firebase certificates verified against pinned Google roots");
#endif

  prefs.begin("rsu-room205", false);
  loadRuntimeState();

  // Relays OFF before anything else for safe boot.
  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], RELAY_ACTIVE_LOW ? HIGH : LOW);
    delayMicroseconds(50);
    int bootLevel = digitalRead(RELAY_PINS[i]);
    int expectedOffLevel = RELAY_ACTIVE_LOW ? HIGH : LOW;
    relayGpioOk[i] = (bootLevel == expectedOffLevel);
    relayState[i] = RELAY_ACTIVE_LOW ? (bootLevel == LOW) : (bootLevel == HIGH);
  }
  // Keep former physical relay-module input 8 permanently inactive. GPIO23 is dedicated to Entrance IR.
  for (uint8_t i = 0; i < 1; i++) {
    pinMode(UNUSED_RELAY_PINS[i], OUTPUT);
    digitalWrite(UNUSED_RELAY_PINS[i], RELAY_ACTIVE_LOW ? HIGH : LOW);
  }

  // IR sensors: calibrate/restore clear levels, then start edge capture and the realtime task.
  pinMode(ENTRANCE_IR_PIN, INPUT_PULLUP); // GPIO23 supports internal pull-up; stable for NPN/open-collector sensor output
  pinMode(EXIT_IR_PIN, INPUT_PULLUP);     // internal pull-up enabled; external 10k pull-up still recommended
  loadIrCalibration();
  attachInterrupt(digitalPinToInterrupt(ENTRANCE_IR_PIN), onEntranceBeamChange, CHANGE);
  attachInterrupt(digitalPinToInterrupt(EXIT_IR_PIN), onExitBeamChange, CHANGE);
  Serial.println("[IR] INSTANT validated latch enabled: count after 30ms BREAK; CLEAR rearms next person");
  xTaskCreatePinnedToCore(sensorRealtimeTask, "sensorRealtime", SENSOR_TASK_STACK_BYTES, nullptr, 4, &sensorRealtimeTaskHandle, 1);

  lcdBegin();
  updateLcd();  // intro page

  pzemSerial.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);
  readPzem();

  // Without BT_CLOUD_COEXIST, maintainBluetooth() turns Bluetooth off once Wi-Fi is up.
  Serial.println(BT_CLOUD_COEXIST ? "[BT] Bluetooth stays on with Firebase (mbedTLS dynamic buffers)"
                                  : "[BT] Bluetooth off while online (no mbedTLS dynamic buffers)");
  startBluetooth();

  // Wi-Fi is optional: without saved credentials this opens the setup AP and returns at once.
  pinMode(WIFI_SETUP_BUTTON_PIN, INPUT_PULLUP);
  beginWifi();
  configTime(LOCAL_UTC_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
  xTaskCreatePinnedToCore(firebaseNetworkTask, "firebaseNetwork", NETWORK_TASK_STACK_BYTES, nullptr, 1, &firebaseNetworkTaskHandle, 1);

  {
    StateLock lock;
    vacancyStartedAt = occupantCount == 0 ? millis() : 0;
    applyAutoRules();
  }
  updateLcd();
  lastLcdPageAt = millis();  // hold the intro for a full page period
}

void loop() {
  unsigned long now = millis();

  // Sensor counting runs in a high-priority realtime task and Firebase runs in its
  // own low-priority network task, so neither TLS nor a slow RTDB request can delay IR.
  processBluetooth();
  maintainBluetooth(WiFi.status() == WL_CONNECTED);

  // Wi-Fi/Firebase and Bluetooth both gone: nobody can drive MANUAL mode, so return to AUTO.
  if (controlLinkUp()) {
    controlLinkLostSince = 0;
  } else if (!controlLinkLostSince) {
    controlLinkLostSince = now ? now : 1;
  } else if (now - controlLinkLostSince >= CONTROL_LINK_LOST_AUTO_MS) {
    controlLinkLostSince = now ? now : 1;
    fallBackToAutoMode("no Firebase or Bluetooth link for 3 minutes");
  }

  // Event-driven sync. Bluetooth is ALWAYS flushed first so a slow Firebase/TLS
  // transaction can never delay local occupancy display. Each transport tracks dirty state
  // independently so a successful Bluetooth send cannot hide a failed Firebase push.
  if (stateDirty) {
    if (bluetoothStateDirty) {
      if (SerialBT.hasClient()) {
        lastBtStateAt = now;
        sendStateToBluetooth();
      }
      bluetoothStateDirty = false;
    }
    // Firebase dirty-state flushing is owned by firebaseNetworkTask().
    if (!bluetoothStateDirty && !firebaseStateDirty) stateDirty = false;
  }

  if (now - lastPzemReadAt >= PZEM_READ_MS) {
    lastPzemReadAt = now;
    readPzem();
  }

  // Firebase command polling and heartbeat pushes run in firebaseNetworkTask().
  if (SerialBT.hasClient() && now - lastBtStateAt >= BT_STATE_PUSH_MS) {
    lastBtStateAt = now;
    sendStateToBluetooth();
  }

  // Holding BOOT opens the Wi-Fi setup AP (e.g. to move the room to another network).
  if (digitalRead(WIFI_SETUP_BUTTON_PIN) != LOW) {
    setupButtonDownAt = 0;
    setupButtonFired = false;
  } else if (!setupButtonDownAt) {
    setupButtonDownAt = now;
  } else if (!setupButtonFired && now - setupButtonDownAt >= WIFI_SETUP_BUTTON_HOLD_MS) {
    setupButtonFired = true;
    Serial.println("[WIFI] BOOT button held: opening setup AP");
    requestWifiSetupPortal("BOOT button");
  }

  refreshLcdOnChange();
  if (now - lastLcdPageAt >= LCD_PAGE_MS) {
    lastLcdPageAt = now;
    nextLcdPage(wifiPortalActive);
    updateLcd();
  }

  // Heap and per-task free stack (bytes), to size TLS headroom and the task stacks.
  if (now - lastMemDiagAt >= MEM_DIAG_INTERVAL_MS) {
    lastMemDiagAt = now;
    Serial.printf("[MEM] heap=%u maxAlloc=%u minEver=%u stackFree sensor=%u network=%u loop=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap(),
                  sensorRealtimeTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(sensorRealtimeTaskHandle) : 0U,
                  firebaseNetworkTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(firebaseNetworkTaskHandle) : 0U,
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  }

  // The network task may block in HTTPS for a while, but never for minutes.
  unsigned long heartbeat = networkTaskHeartbeatAt;
  if (heartbeat && millis() - heartbeat > NETWORK_TASK_STALL_RESTART_MS) {
    Serial.println("[WDT] firebaseNetworkTask stalled for 5 minutes; restarting controller");
    delay(100);
    ESP.restart();
  }

  delay(2);
}
