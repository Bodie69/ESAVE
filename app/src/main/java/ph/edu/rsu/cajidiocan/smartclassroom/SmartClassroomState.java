package ph.edu.rsu.cajidiocan.smartclassroom;

import org.json.JSONObject;

public final class SmartClassroomState {
    public String schemaVersion = "";
    public String mode = "UNKNOWN";
    public String lastSource = "--";
    public long serverTimestampMs = 0;
    public boolean wifi;
    public boolean firebase;
    public boolean bluetooth;
    public int rssi = -127;
    public String ip = "0.0.0.0";
    /** Firmware V25.18+ reports Wi-Fi setup fields; older firmware has Wi-Fi compiled in. */
    public boolean wifiSetupSupported = false;
    public boolean wifiConfigured = true;
    public String wifiSsid = "";
    public boolean wifiSetupAp = false;
    /** Firmware V25.20+: Firebase is stopped for a local Bluetooth session (or the controller is offline). */
    public boolean cloudPaused = false;
    public long uptimeMs = 0L;
    public long telemetrySequence = 0L;
    public int people = 0;
    public boolean occupancyValid = false;
    public boolean entranceBroken;
    public boolean exitBroken;
    public String lastOccupancyEvent = "--";
    public long entranceEvents = 0L;
    public long exitEvents = 0L;
    public long exitUnderflowEvents = 0L;
    public long vacancyDelaySeconds = 0L;
    public String detectionMethod = "--";
    public boolean personClassifierPresent = false;
    public boolean humanOnlyGuaranteed = false;
    public long passageMinMs = 0L;
    public long passageMaxMs = 0L;
    public boolean fastPassEnabled = false;
    public boolean instantLatchEnabled = false;
    public long instantLatchTargetMs = 0L;
    public long minClearGapMs = 0L;
    public long occupancyEventSequence = 0L;
    public String lastPassageDirection = "none";
    public long lastPassageProcessLatencyMs = 0L;
    public long entranceEdgesWindow = 0L;
    public long exitEdgesWindow = 0L;
    public long rejectedPassages = 0L;
    public long entranceRejectedShort = 0L;
    public long entranceRejectedLong = 0L;
    public long exitRejectedShort = 0L;
    public long exitRejectedLong = 0L;
    public boolean entranceSensorNoisy = false;
    public boolean exitSensorNoisy = false;
    public boolean noiseGuardActive = false;
    public long entranceNoiseRejected = 0L;
    public long exitNoiseRejected = 0L;
    public boolean energyValid;
    public double voltage = Double.NaN;
    public double current = Double.NaN;
    public double power = Double.NaN;
    public double energy = Double.NaN;
    public double frequency = Double.NaN;
    public double powerFactor = Double.NaN;
    public long pzemLastGoodAgeSeconds = -1L;
    public long pzemSampleSequence = 0L;
    public long pzemInvalidSamples = 0L;
    public long freeHeap = 0L;
    public long firebaseAuthFailures = 0L;
    public long firebaseHttpFailures = 0L;
    public int lastHttpErrorCode = 0;
    public String lastHttpErrorText = "none";
    public String lastHttpHost = "none";
    public final boolean[] relays = new boolean[6];
    public boolean relayVerificationAvailable = false;
    public final boolean[] relayGpioVerified = new boolean[6];

    /** True if the controller's schema "MAJOR.MINOR" is at least the given version; unknown counts as current. */
    public boolean schemaAtLeast(int major, int minor) {
        if (schemaVersion == null || schemaVersion.trim().isEmpty()) return true;
        String[] parts = schemaVersion.trim().split("\\.");
        try {
            int ma = Integer.parseInt(parts[0]);
            int mi = parts.length > 1 ? Integer.parseInt(parts[1]) : 0;
            return ma > major || (ma == major && mi >= minor);
        } catch (NumberFormatException e) {
            return true;
        }
    }

    private static double numberOrNaN(JSONObject o, String key) {
        if (o == null || o.isNull(key)) return Double.NaN;
        return o.optDouble(key, Double.NaN);
    }

    private static boolean finiteInRange(double value, double min, double max) {
        return !Double.isNaN(value) && !Double.isInfinite(value) && value >= min && value <= max;
    }

    public static SmartClassroomState fromJson(JSONObject root) {
        SmartClassroomState s = new SmartClassroomState();
        s.schemaVersion = root.optString("schema_version", "");
        s.mode = root.optString("mode", "UNKNOWN");
        s.lastSource = root.optString("last_control_source", "--");
        s.uptimeMs = root.optLong("uptime_ms", 0L);
        s.telemetrySequence = root.optLong("telemetry_sequence", 0L);
        s.serverTimestampMs = root.optLong("server_timestamp_ms", 0L);

        JSONObject c = root.optJSONObject("connectivity");
        if (c != null) {
            s.wifi = c.optBoolean("wifi", false);
            s.firebase = c.optBoolean("firebase", false);
            s.bluetooth = c.optBoolean("bluetooth", false);
            s.rssi = c.optInt("wifi_rssi", -127);
            s.ip = c.optString("ip", "0.0.0.0");
            s.wifiSetupSupported = c.has("wifi_configured");
            s.wifiConfigured = c.optBoolean("wifi_configured", true);
            s.wifiSsid = c.optString("wifi_ssid", "");
            s.wifiSetupAp = c.optBoolean("wifi_setup_ap", false);
            s.cloudPaused = c.optBoolean("cloud_paused", false);
        }

        JSONObject o = root.optJSONObject("occupancy");
        if (o != null) {
            s.people = Math.max(0, o.optInt("count", 0));
            s.occupancyValid = o.optBoolean("valid", false) && o.optInt("count", 0) >= 0;
            s.entranceBroken = o.optBoolean("entrance_broken", false);
            s.exitBroken = o.optBoolean("exit_broken", false);
            s.lastOccupancyEvent = o.optString("last_event", "--");
            s.entranceEvents = o.optLong("entrance_events", 0L);
            s.exitEvents = o.optLong("exit_events", 0L);
            s.exitUnderflowEvents = o.optLong("exit_underflow_events", 0L);
            s.vacancyDelaySeconds = o.optLong("vacancy_delay_seconds", 0L);
            s.detectionMethod = o.optString("detection_method", "--");
            s.personClassifierPresent = o.optBoolean("person_classifier_present", false);
            s.humanOnlyGuaranteed = o.optBoolean("human_only_guaranteed", false);
            s.passageMinMs = o.optLong("passage_min_ms", 0L);
            s.passageMaxMs = o.optLong("passage_max_ms", 0L);
            s.fastPassEnabled = o.optBoolean("fast_pass_enabled", false);
            s.instantLatchEnabled = o.optBoolean("instant_latch_enabled", false);
            s.instantLatchTargetMs = o.optLong("instant_latch_target_ms", 0L);
            s.minClearGapMs = o.optLong("min_clear_gap_ms", 0L);
            s.occupancyEventSequence = o.optLong("event_sequence", 0L);
            s.lastPassageDirection = o.optString("last_passage_direction", "none");
            s.lastPassageProcessLatencyMs = o.optLong("last_passage_process_latency_ms", 0L);
            s.entranceEdgesWindow = o.optLong("entrance_edges_window", 0L);
            s.exitEdgesWindow = o.optLong("exit_edges_window", 0L);
            s.entranceRejectedShort = o.optLong("entrance_rejected_short", 0L);
            s.entranceRejectedLong = o.optLong("entrance_rejected_long", 0L);
            s.exitRejectedShort = o.optLong("exit_rejected_short", 0L);
            s.exitRejectedLong = o.optLong("exit_rejected_long", 0L);
            s.entranceSensorNoisy = o.optBoolean("entrance_sensor_noisy", false);
            s.exitSensorNoisy = o.optBoolean("exit_sensor_noisy", false);
            s.noiseGuardActive = o.optBoolean("noise_guard_active", s.entranceSensorNoisy || s.exitSensorNoisy);
            s.entranceNoiseRejected = o.optLong("entrance_noise_rejected", 0L);
            s.exitNoiseRejected = o.optLong("exit_noise_rejected", 0L);
            s.rejectedPassages = s.entranceRejectedShort + s.entranceRejectedLong
                    + s.exitRejectedShort + s.exitRejectedLong;
        }

        JSONObject e = root.optJSONObject("energy");
        if (e != null) {
            boolean reportedValid = e.optBoolean("valid", false);
            s.voltage = numberOrNaN(e, "voltage_v");
            s.current = numberOrNaN(e, "current_a");
            s.power = numberOrNaN(e, "power_w");
            s.energy = numberOrNaN(e, "energy_kwh");
            s.frequency = numberOrNaN(e, "frequency_hz");
            s.powerFactor = numberOrNaN(e, "power_factor");
            s.pzemLastGoodAgeSeconds = e.optLong("last_good_age_seconds", -1L);
            s.pzemSampleSequence = e.optLong("sample_sequence", 0L);
            s.pzemInvalidSamples = e.optLong("invalid_samples", 0L);
            boolean fresh = s.pzemLastGoodAgeSeconds >= 0L && s.pzemLastGoodAgeSeconds <= 6L;
            s.energyValid = reportedValid && fresh
                    && finiteInRange(s.voltage, 50.0, 300.0)
                    && finiteInRange(s.current, 0.0, 120.0)
                    && finiteInRange(s.power, 0.0, 36000.0)
                    && finiteInRange(s.energy, 0.0, 10000000.0)
                    && finiteInRange(s.frequency, 40.0, 70.0)
                    && finiteInRange(s.powerFactor, 0.0, 1.05);
        }

        JSONObject d = root.optJSONObject("diagnostics");
        if (d != null) {
            s.freeHeap = d.optLong("free_heap", 0L);
            s.firebaseAuthFailures = d.optLong("firebase_auth_failures", 0L);
            s.firebaseHttpFailures = d.optLong("firebase_http_failures", 0L);
            s.lastHttpErrorCode = d.optInt("last_http_error_code", 0);
            s.lastHttpErrorText = d.optString("last_http_error_text", "none");
            s.lastHttpHost = d.optString("last_http_host", "none");
        }

        JSONObject r = root.optJSONObject("relays");
        String[] keys = {"light1","light2","light3","light4","fan1","fan2"};
        if (r != null) for (int i = 0; i < keys.length; i++) s.relays[i] = r.optBoolean(keys[i], false);

        JSONObject rv = root.optJSONObject("relay_gpio_verified");
        if (rv != null) {
            s.relayVerificationAvailable = true;
            for (int i = 0; i < keys.length; i++) s.relayGpioVerified[i] = rv.optBoolean(keys[i], false);
        }
        return s;
    }
}
