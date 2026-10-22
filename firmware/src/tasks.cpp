#include "tasks.h"
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "bluetooth.h"
#include "commands.h"
#include "cloud.h"
#include "occupancy.h"
#include "provisioning.h"
#include "relays.h"
#include "state.h"
#include "telemetry.h"

TaskHandle_t sensorRealtimeTaskHandle = nullptr;
TaskHandle_t firebaseNetworkTaskHandle = nullptr;
volatile unsigned long networkTaskHeartbeatAt = 0;

void setupTaskWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t config = {};
  config.timeout_ms = TASK_WATCHDOG_TIMEOUT_MS;
  config.idle_core_mask = (1 << 0); // keep the Arduino default: watch core 0's idle task
  config.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&config) == ESP_ERR_INVALID_STATE) esp_task_wdt_init(&config);
#else
  esp_task_wdt_init(TASK_WATCHDOG_TIMEOUT_MS / 1000, true);
#endif
  enableLoopWDT(); // loop() is fed by the Arduino core after every pass
}

void sensorRealtimeTask(void *parameter) {
  (void)parameter;
  esp_task_wdt_add(nullptr);
  for (;;) {
    esp_task_wdt_reset();
    // Latching is lock-free (ISR-shared counters), so passages are never missed. If another
    // task briefly holds stateMutex, the pending counts are simply applied next pass.
    latchValidatedPassagesNow();
    {
      StateLock lock(stateMutex, pdMS_TO_TICKS(5));
      if (lock.held) {
        processOccupancy();
        applyAutoRules();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void firebaseNetworkTask(void *parameter) {
  (void)parameter;
  unsigned long taskLastCommandPollAt = 0;
  unsigned long taskLastPushAttemptAt = 0;
  bool lastPushFailed = false;
  bool pauseAnnounced = false;
  for (;;) {
    networkTaskHeartbeatAt = millis();
    maintainWifi();

    // Bluetooth and TLS do not both fit in the heap. While loop() wants Bluetooth (or it is still
    // shutting down), Firebase is stopped and signed out, and only then does the ack let
    // Bluetooth start. The TLS reserve is never held while Bluetooth is on: it made Bluetooth
    // abort on an incoming connection (vQueueDelete assert in l2cu_allocate_ccb).
    // With BT_CLOUD_COEXIST both run together and none of this applies.
    bool btWanted = bluetoothWanted;
    uint32_t pauseRequest = bluetoothPauseRequest; // read after bluetoothWanted, see bluetooth.h
    if (!BT_CLOUD_COEXIST && (btWanted || bluetoothActive)) {
      // Tell the app why updates stop (state shows connectivity.cloud_paused) before signing out.
      if (btWanted && !bluetoothActive && !pauseAnnounced && epochNow() &&
          WiFi.status() == WL_CONNECTED && firebaseAuthenticated) {
        pushStateToFirebase();
      }
      pauseAnnounced = true;
      stopFirebaseForBluetooth();
      bluetoothPauseAck = pauseRequest;
      vTaskDelay(pdMS_TO_TICKS(wifiPortalActive ? 20 : 500));
      continue;
    }
    pauseAnnounced = false;
    maintainTlsReserve(WiFi.status() == WL_CONNECTED);

    // TLS certificate checks need real time; never talk to Firebase before NTP sync.
    // Offline nothing else happens here; the setup page is served from maintainWifi().
    if (!epochNow() || WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(wifiPortalActive ? 20 : 500));
      continue;
    }

    if (millis() - taskLastCommandPollAt >= FIREBASE_COMMAND_POLL_MS) {
      taskLastCommandPollAt = millis();
      pollFirebaseCommands();
    }

    maybeWriteEnergyHistory();

    // One push path: immediately (throttled) when state changed, otherwise a heartbeat.
    // After a failure, wait FIREBASE_STATE_RETRY_MS instead of hammering the network.
    unsigned long sinceAttempt = millis() - taskLastPushAttemptAt;
    unsigned long interval = lastPushFailed ? FIREBASE_STATE_RETRY_MS
                           : (firebaseStateDirty ? FIREBASE_STATE_MIN_INTERVAL_MS : FIREBASE_STATE_PUSH_MS);
    if (WiFi.status() == WL_CONNECTED && (taskLastPushAttemptAt == 0 || sinceAttempt >= interval)) {
      taskLastPushAttemptAt = millis();
      uint32_t revisionBefore = telemetryRevision;
      lastPushFailed = !pushStateToFirebase();
      if (!lastPushFailed) {
        // Never erase a newer sensor event that occurred while HTTPS was in progress.
        firebaseStateDirty = (telemetryRevision != revisionBefore);
        if (!bluetoothStateDirty && !firebaseStateDirty) stateDirty = false;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
