package ph.edu.rsu.cajidiocan.smartclassroom;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.json.JSONObject;
import org.junit.Test;

/** Parsing of the controller state JSON written by firmware buildState() (Firebase and Bluetooth). */
public class SmartClassroomStateTest {
    private static final double EPS = 1e-9;

    /** A state document shaped like firmware V25.16 buildState() with a valid PZEM sample. */
    private static JSONObject firmwareState() throws Exception {
        return new JSONObject("{"
                + "\"schema_version\":\"25.16\",\"room_id\":\"room206\",\"mode\":\"AUTO\","
                + "\"last_control_source\":\"auto_occupied\",\"uptime_ms\":123456,\"telemetry_sequence\":42,"
                + "\"server_timestamp_ms\":1792130000000,"
                + "\"connectivity\":{\"wifi\":true,\"firebase\":true,\"bluetooth\":false,\"wifi_rssi\":-55,\"ip\":\"192.168.1.20\"},"
                + "\"occupancy\":{\"count\":3,\"valid\":true,\"entrance_broken\":false,\"exit_broken\":true,"
                + "\"entrance_events\":10,\"exit_events\":7,\"exit_underflow_events\":1,\"vacancy_delay_seconds\":300,"
                + "\"last_event\":\"entrance_fast_validated\",\"entrance_rejected_short\":2,\"entrance_rejected_long\":1,"
                + "\"exit_rejected_short\":3,\"exit_rejected_long\":0,\"entrance_sensor_noisy\":false,\"exit_sensor_noisy\":false,"
                + "\"noise_guard_active\":false,\"human_only_guaranteed\":false},"
                + "\"energy\":{\"valid\":true,\"voltage_v\":229.5,\"current_a\":1.234,\"power_w\":250.1,\"energy_kwh\":12.345,"
                + "\"frequency_hz\":60,\"power_factor\":0.95,\"last_good_age_seconds\":0,\"sample_sequence\":99,\"invalid_samples\":0},"
                + "\"relays\":{\"light1\":true,\"light2\":false,\"light3\":true,\"light4\":false,\"fan1\":true,\"fan2\":false},"
                + "\"relay_gpio_verified\":{\"light1\":true,\"light2\":true,\"light3\":true,\"light4\":true,\"fan1\":true,\"fan2\":false},"
                + "\"diagnostics\":{\"free_heap\":120000,\"firebase_auth_failures\":2,\"firebase_http_failures\":5,"
                + "\"last_http_error_code\":-1,\"last_http_error_text\":\"connection refused\"}"
                + "}");
    }

    @Test public void parsesFullFirmwareState() throws Exception {
        SmartClassroomState s = SmartClassroomState.fromJson(firmwareState());
        assertEquals("25.16", s.schemaVersion);
        assertEquals("AUTO", s.mode);
        assertEquals(42L, s.telemetrySequence);
        assertEquals(1792130000000L, s.serverTimestampMs);
        assertTrue(s.wifi);
        assertEquals(-55, s.rssi);
        assertEquals(3, s.people);
        assertTrue(s.occupancyValid);
        assertTrue(s.exitBroken);
        assertEquals(300L, s.vacancyDelaySeconds);
        assertEquals(6L, s.rejectedPassages);
        assertTrue(s.energyValid);
        assertEquals(250.1, s.power, EPS);
        assertEquals(12.345, s.energy, EPS);
        assertArrayEquals(new boolean[]{true, false, true, false, true, false}, s.relays);
        assertTrue(s.relayVerificationAvailable);
        assertFalse(s.relayGpioVerified[5]);
        assertEquals(-1, s.lastHttpErrorCode);
        assertEquals(5L, s.firebaseHttpFailures);
    }

    @Test public void pzemNullValuesAreInvalid() throws Exception {
        JSONObject j = firmwareState();
        j.put("energy", new JSONObject("{\"valid\":false,\"voltage_v\":null,\"power_w\":null,\"last_good_age_seconds\":-1}"));
        SmartClassroomState s = SmartClassroomState.fromJson(j);
        assertFalse(s.energyValid);
        assertTrue(Double.isNaN(s.voltage));
        assertTrue(Double.isNaN(s.power));
        assertEquals(-1L, s.pzemLastGoodAgeSeconds);
    }

    @Test public void stalePzemSampleIsInvalidEvenIfFirmwareSaysValid() throws Exception {
        JSONObject j = firmwareState();
        j.getJSONObject("energy").put("last_good_age_seconds", 7);
        assertFalse(SmartClassroomState.fromJson(j).energyValid);
    }

    @Test public void outOfRangeElectricalValueIsInvalid() throws Exception {
        JSONObject j = firmwareState();
        j.getJSONObject("energy").put("voltage_v", 400.0);
        assertFalse(SmartClassroomState.fromJson(j).energyValid);
    }

    @Test public void negativeCountIsClampedAndMarkedInvalid() throws Exception {
        JSONObject j = firmwareState();
        j.getJSONObject("occupancy").put("count", -2);
        SmartClassroomState s = SmartClassroomState.fromJson(j);
        assertEquals(0, s.people);
        assertFalse(s.occupancyValid);
    }

    @Test public void noiseGuardDefaultsFromNoisyFlags() throws Exception {
        JSONObject j = firmwareState();
        JSONObject o = j.getJSONObject("occupancy");
        o.remove("noise_guard_active");
        o.put("exit_sensor_noisy", true);
        assertTrue(SmartClassroomState.fromJson(j).noiseGuardActive);
    }

    @Test public void missingSectionsUseSafeDefaults() throws Exception {
        SmartClassroomState s = SmartClassroomState.fromJson(new JSONObject("{\"mode\":\"MANUAL\"}"));
        assertEquals("MANUAL", s.mode);
        assertEquals(0, s.people);
        assertFalse(s.occupancyValid);
        assertFalse(s.energyValid);
        assertFalse(s.relayVerificationAvailable);
        assertArrayEquals(new boolean[6], s.relays);
    }

    @Test public void wifiSetupFieldsFromV2518Firmware() throws Exception {
        SmartClassroomState old = SmartClassroomState.fromJson(firmwareState());
        assertFalse("V25.16 has no Wi-Fi setup", old.wifiSetupSupported);
        assertTrue("older firmware is not reported as unconfigured", old.wifiConfigured);

        JSONObject j = firmwareState();
        JSONObject c = j.getJSONObject("connectivity");
        c.put("wifi_configured", false);
        c.put("wifi_ssid", "");
        c.put("wifi_setup_ap", true);
        SmartClassroomState s = SmartClassroomState.fromJson(j);
        assertTrue(s.wifiSetupSupported);
        assertFalse(s.wifiConfigured);
        assertTrue(s.wifiSetupAp);
        assertTrue(MainActivity.wifiSetupStatusText(s).contains("No network saved"));
        assertTrue(MainActivity.wifiSetupStatusText(s).contains(MainActivity.WIFI_SETUP_AP_SSID));

        c.put("wifi_configured", true);
        c.put("wifi_ssid", "RSU-Staff");
        c.put("wifi_setup_ap", false);
        s = SmartClassroomState.fromJson(j);
        assertEquals("RSU-Staff", s.wifiSsid);
        assertEquals("RSU-Staff  ·  192.168.1.20", MainActivity.wifiSetupStatusText(s));
    }

    @Test public void schemaVersionComparison() throws Exception {
        SmartClassroomState s = SmartClassroomState.fromJson(firmwareState());
        assertTrue(s.schemaAtLeast(25, 16));
        assertTrue(s.schemaAtLeast(25, 14));
        assertFalse(s.schemaAtLeast(25, 17));
        assertFalse(s.schemaAtLeast(26, 0));
        s.schemaVersion = "25.9";
        assertFalse("25.9 is older than 25.14, not newer", s.schemaAtLeast(25, 14));
        s.schemaVersion = "26.0";
        assertTrue(s.schemaAtLeast(25, 16));
        s.schemaVersion = "";
        assertTrue("unknown version is not blocked", s.schemaAtLeast(25, 16));
        s.schemaVersion = "dev-build";
        assertTrue(s.schemaAtLeast(25, 16));
    }
}
