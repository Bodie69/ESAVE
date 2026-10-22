#include "telemetry.h"
#include <WiFi.h>
#include <time.h>

#include "bluetooth.h"
#include "energy.h"
#include "cloud.h"
#include "occupancy.h"
#include "provisioning.h"
#include "state.h"

// Local hour (YYYYMMDDHH) whose energy-history reading was last stored. Used only by
// firebaseNetworkTask; persisted so a reboot does not overwrite the hour's first reading.
uint32_t lastEnergyHistoryHourKey = 0;
static unsigned long lastEnergyHistoryAttemptAt = 0;

// Caller must hold stateMutex.
void buildState(JsonDocument &doc, bool forFirebase) {
  doc.clear();
  doc["schema_version"] = FIRMWARE_VERSION;
  doc["device_id"] = DEVICE_ID;
  doc["room_id"] = ROOM_ID;
  doc["device_epoch"] = epochNow();
  doc["uptime_ms"] = millis();
  doc["mode"] = automaticMode ? "AUTO" : "MANUAL";
  doc["last_control_source"] = lastControlSource;
  doc["telemetry_sequence"] = telemetryRevision;
  if (forFirebase) {
    JsonObject sv = doc["server_timestamp_ms"].to<JsonObject>();
    sv[".sv"] = "timestamp";
  } else {
    unsigned long ep = epochNow();
    doc["server_timestamp_ms"] = ep > 0 ? ((uint64_t)ep * 1000ULL) : 0ULL;
  }
  JsonObject conn = doc["connectivity"].to<JsonObject>();
  conn["wifi"] = WiFi.status() == WL_CONNECTED;
  conn["firebase"] = firebaseAuthenticated && (millis() - lastFirebaseOkAt < 15000UL);
  conn["bluetooth"] = SerialBT.hasClient();
  // Local Bluetooth session or offline; never with BT_CLOUD_COEXIST.
  conn["cloud_paused"] = !BT_CLOUD_COEXIST && (bluetoothWanted || bluetoothActive);
  conn["wifi_rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;
  conn["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "0.0.0.0";
  conn["wifi_configured"] = wifiConfigured;
  conn["wifi_ssid"] = wifiSsid;
  conn["wifi_setup_ap"] = wifiPortalActive;
  JsonObject occ = doc["occupancy"].to<JsonObject>();
  occ["count"] = occupantCount;
  occ["occupied"] = occupantCount > 0;
  occ["valid"] = occupancyValid;
  occ["entrance_broken"] = entranceBeam.broken;
  occ["exit_broken"] = exitBeam.broken;
  occ["entrance_raw"] = entranceBeam.stableRaw;
  occ["exit_raw"] = exitBeam.stableRaw;
  occ["entrance_events"] = entranceEvents;
  occ["exit_events"] = exitEvents;
  occ["exit_underflow_events"] = exitUnderflowEvents;
  occ["last_event"] = lastOccupancyEvent;
  occ["vacancy_delay_seconds"] = VACANCY_DELAY_MS / 1000UL;
  occ["detection_method"] = "GOGOU_BREAK_BEAM_FAST_ISR_EDGE_FILTER";
  occ["fast_pass_enabled"] = true;
  occ["event_sequence"] = occupancyEventSequence;
  occ["last_passage_direction"] = lastPassageDirection;
  occ["last_passage_process_latency_ms"] = lastPassageLatencyMs;
  occ["person_classifier_present"] = false;
  occ["human_only_guaranteed"] = false;
  occ["passage_min_ms"] = PASSAGE_MIN_BREAK_MS;
  occ["instant_latch_enabled"] = true;
  occ["instant_latch_target_ms"] = PASSAGE_MIN_BREAK_MS;
  occ["min_clear_gap_ms"] = IR_MIN_CLEAR_GAP_MS;
  occ["passage_max_ms"] = PASSAGE_MAX_BREAK_MS;
  occ["entrance_rejected_short"] = entranceRejectedShort;
  occ["entrance_rejected_long"] = entranceRejectedLong;
  occ["exit_rejected_short"] = exitRejectedShort;
  occ["exit_rejected_long"] = exitRejectedLong;
  occ["entrance_sensor_noisy"] = entranceSensorNoisy;
  occ["exit_sensor_noisy"] = exitSensorNoisy;
  occ["entrance_noise_rejected"] = entranceNoiseRejected;
  occ["exit_noise_rejected"] = exitNoiseRejected;
  occ["noise_guard_active"] = entranceSensorNoisy || exitSensorNoisy;
  occ["entrance_edges_window"] = entranceEdgesWindow;
  occ["exit_edges_window"] = exitEdgesWindow;
  JsonObject energy = doc["energy"].to<JsonObject>();
  energy["valid"] = pzemValid;
  if (pzemValid) {
    energy["voltage_v"] = voltage;
    energy["current_a"] = currentA;
    energy["power_w"] = powerW;
    energy["energy_kwh"] = energyKWh;
    energy["frequency_hz"] = frequencyHz;
    energy["power_factor"] = powerFactor;
  } else {
    energy["voltage_v"] = nullptr;
    energy["current_a"] = nullptr;
    energy["power_w"] = nullptr;
    energy["energy_kwh"] = nullptr;
    energy["frequency_hz"] = nullptr;
    energy["power_factor"] = nullptr;
  }
  // Assigned in separate branches: a single ?: would convert -1 to unsigned 4294967295.
  if (lastPzemGoodAt) energy["last_good_age_seconds"] = (millis() - lastPzemGoodAt) / 1000UL;
  else energy["last_good_age_seconds"] = -1;
  energy["sample_sequence"] = pzemSampleSequence;
  energy["invalid_samples"] = pzemInvalidSamples;
  energy["source"] = "PZEM-004T_V3_DIRECT";
  JsonObject rel = doc["relays"].to<JsonObject>();
  JsonObject relVerify = doc["relay_gpio_verified"].to<JsonObject>();
  for (int i = 0; i < RELAY_COUNT; i++) {
    rel[RELAY_KEYS[i]] = relayState[i];
    relVerify[RELAY_KEYS[i]] = relayGpioOk[i];
  }
  JsonObject diag = doc["diagnostics"].to<JsonObject>();
  diag["free_heap"] = ESP.getFreeHeap();
  diag["firebase_auth_failures"] = firebaseAuthFailures;
  diag["firebase_http_failures"] = firebaseHttpFailures;
  diag["last_http_error_code"] = lastHttpErrorCode;
  diag["last_http_error_text"] = lastHttpErrorText;
  diag["last_http_host"] = lastHttpHost;
  diag["firebase_auth_retry_ms"] = firebaseAuthRetryMs;
  diag["firebase_transport_failures_consecutive"] = firebaseTransportFailuresConsecutive;
  diag["firebase_dns_ok"] = lastFirebaseDnsOk;
  diag["firebase_tcp443_ok"] = lastFirebaseTcp443Ok;
  diag["firebase_tls443_ok"] = lastFirebaseTls443Ok;
  diag["firebase_resolved_ip"] = lastFirebaseResolvedIp;
  diag["firebase_tls_error"] = lastFirebaseTlsError;
  diag["free_heap"] = ESP.getFreeHeap();
  diag["max_alloc_heap"] = ESP.getMaxAllocHeap();
  if (lastBtCommandAt) diag["last_bt_command_age_seconds"] = (millis() - lastBtCommandAt) / 1000UL;
  else diag["last_bt_command_age_seconds"] = -1;
  diag["last_state_change"] = stateDirtyReason;
}

// Stores the cumulative PZEM kWh once per local hour at
// rooms/<room>/energy_history/hourly/<YYYYMMDDHH> = {kwh, epoch, ts}. The app turns the
// differences between readings into hourly/daily usage. Runs in firebaseNetworkTask.
void maybeWriteEnergyHistory() {
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long epoch = epochNow();
  if (!epoch) return; // no NTP time yet: the hour key would be wrong

  time_t localSeconds = (time_t)epoch + LOCAL_UTC_OFFSET_SEC;
  struct tm local;
  gmtime_r(&localSeconds, &local);
  uint32_t hourKey = (uint32_t)(local.tm_year + 1900) * 1000000UL + (uint32_t)(local.tm_mon + 1) * 10000UL
                     + (uint32_t)local.tm_mday * 100UL + (uint32_t)local.tm_hour;
  if (hourKey == lastEnergyHistoryHourKey) return;

  unsigned long now = millis();
  if (lastEnergyHistoryAttemptAt && now - lastEnergyHistoryAttemptAt < ENERGY_HISTORY_RETRY_MS) return;

  bool valid;
  float kwh;
  {
    StateLock lock;
    valid = pzemValid;
    kwh = energyKWh;
  }
  if (!valid) return; // only store real meter readings; retried on the next valid sample

  lastEnergyHistoryAttemptAt = now;

  JsonDocument doc;
  doc["kwh"] = roundf(kwh * 1000.0f) / 1000.0f;
  doc["epoch"] = epoch;
  JsonObject sv = doc["ts"].to<JsonObject>();
  sv[".sv"] = "timestamp";

  String body, response;
  serializeJson(doc, body);
  if (firebaseRequest("PUT", roomPath("energy_history/hourly/") + String(hourKey), body, response)) {
    lastEnergyHistoryHourKey = hourKey;
    lastEnergyHistoryAttemptAt = 0;
    prefs.putULong("kwhHourKey", hourKey);
    Serial.printf("[ENERGY] history %lu = %.3f kWh\n", (unsigned long)hourKey, kwh);
  }
}

bool pushStateToFirebase() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String body, response;

  {
    StateLock lock;
    JsonDocument doc;
    buildState(doc, true);
    serializeJson(doc, body);
  }

  return firebaseRequest("PUT", roomPath("state"), body, response);
}
