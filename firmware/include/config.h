#pragma once
// Hardware pins, timing, identity and build-time feature switches.

#include <Arduino.h>

#include "secrets.h"

// ----------------------------------------------------------------------------- Identity

// Reported to the app as state.schema_version; the app warns when it is older than it expects.
#define FIRMWARE_VERSION "25.22"

static const char *const DEVICE_ID = "room206-controller";
static const char *const ROOM_ID = "room206";
static const char *const BT_NAME = "ESAVE-Room206";

// ----------------------------------------------------------------------------- Build checks / switches

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED) || !defined(CONFIG_BT_SPP_ENABLED)
#error "Bluetooth Classic SPP is not available. Select an original ESP32/WROOM-32 board, not ESP32-S3/C3."
#endif

// With mbedTLS dynamic buffers (custom_sdkconfig in the core 3 env) Bluetooth stays on while
// Firebase runs. Without them (core 2.x) TLS and Bluetooth do not fit together: Bluetooth is off
// while online and a local session stops Firebase first (strict handoff).
#if defined(CONFIG_MBEDTLS_DYNAMIC_BUFFER)
#define BT_CLOUD_COEXIST 1
#else
#define BT_CLOUD_COEXIST 0
#endif

// The pairing PIN must be chosen per installation in secrets.h; there is no built-in default.
#ifndef BLUETOOTH_PIN
#error "Define BLUETOOTH_PIN in secrets.h (6-16 digits, unique to this classroom)"
#endif
static_assert(sizeof(BLUETOOTH_PIN) - 1 >= 6 && sizeof(BLUETOOTH_PIN) - 1 <= 16,
              "BLUETOOTH_PIN must be 6-16 characters");

// Set to 1 in secrets.h ONLY to diagnose a TLS problem: it disables server-certificate checks,
// which lets anyone on the network impersonate Firebase and capture the account password.
#ifndef FIREBASE_TLS_INSECURE
#define FIREBASE_TLS_INSECURE 0
#endif

// Old plain-text Bluetooth commands ("ALL OFF", "LIGHT1 ON"...). The app only uses JSON, so they
// are off by default; set to 1 in secrets.h only for manual serial-terminal testing.
#ifndef BLUETOOTH_LEGACY_TEXT_COMMANDS
#define BLUETOOTH_LEGACY_TEXT_COMMANDS 0
#endif

// ----------------------------------------------------------------------------- Wi-Fi setup AP

// Wi-Fi credentials are not compiled in: they are entered on the setup access point's page
// (http://192.168.4.1/) and saved in NVS. The AP is WPA2-protected with SETUP_AP_PASSWORD.
#ifdef WIFI_SSID
#error "WIFI_SSID/WIFI_PASSWORD in secrets.h are no longer used: remove them and set the network from the setup AP (see README.md)"
#endif
#ifndef SETUP_AP_PASSWORD
#error "Define SETUP_AP_PASSWORD in secrets.h (8-63 characters, unique to this classroom)"
#endif
static_assert(sizeof(SETUP_AP_PASSWORD) - 1 >= 8 && sizeof(SETUP_AP_PASSWORD) - 1 <= 63,
              "SETUP_AP_PASSWORD must be 8-63 characters (WPA2)");

static const char *const SETUP_AP_SSID = "ESAVE-Room206-Setup";
static const uint8_t WIFI_SETUP_AP_MAX_CLIENTS = 2;
// Holding the board's BOOT button (GPIO0) this long opens the setup AP at any time.
static const uint8_t WIFI_SETUP_BUTTON_PIN = 0;
static const unsigned long WIFI_SETUP_BUTTON_HOLD_MS = 3000;
// The AP also opens by itself when the saved network has been unreachable this long.
static const unsigned long WIFI_PORTAL_AFTER_OFFLINE_MS = 180000;
// Once back online, the AP closes after nobody has used it for this long.
static const unsigned long WIFI_PORTAL_IDLE_CLOSE_MS = 180000;
// After new credentials are saved, reconnect attempts continue even with a phone on the AP.
static const unsigned long WIFI_PORTAL_SAVE_RETRY_WINDOW_MS = 60000;

// ----------------------------------------------------------------------------- Pins

// GOGOU break-beam receiver OUT pins. Entrance moved from GPIO35 to GPIO23 (supports
// INPUT_PULLUP on classic ESP32). Disconnect the former relay IN7 wire from GPIO23.
static const uint8_t ENTRANCE_IR_PIN = 23;
static const uint8_t EXIT_IR_PIN = 32;

// Six classroom load channels only: 4 lights + 2 fans. Former CH8 (GPIO18) is forced OFF at boot.
static const uint8_t RELAY_COUNT = 6;
static const uint8_t RELAY_PINS[RELAY_COUNT] = {13, 33, 14, 27, 26, 25};
static const uint8_t UNUSED_RELAY_PINS[1] = {18};
static const char *const RELAY_KEYS[RELAY_COUNT] = {
  "light1", "light2", "light3", "light4", "fan1", "fan2"
};
static const bool RELAY_ACTIVE_LOW = true;

// PZEM-004T v3 UART: PZEM TX -> ESP32 GPIO16; PZEM RX <- ESP32 GPIO17.
static const uint8_t PZEM_RX_PIN = 16;
static const uint8_t PZEM_TX_PIN = 17;

// I2C LCD. lcdBegin() tries LCD_ADDRESS, then LCD_ALT_ADDRESS (the two common PCF8574
// backpack addresses) and logs which one answered.
static const uint8_t LCD_SDA_PIN = 21;
static const uint8_t LCD_SCL_PIN = 22;
static const uint8_t LCD_ADDRESS = 0x27;
static const uint8_t LCD_ALT_ADDRESS = 0x3F;
static const uint8_t LCD_COLS = 16;
static const uint8_t LCD_ROWS = 2;

// ----------------------------------------------------------------------------- Occupancy

// Auto mode: channels 1..6 automatically follow room occupancy and switch off this long
// after the room is counted empty. Bit i = relay CH(i+1).
static const uint8_t AUTO_CHANNEL_MASK = 0b00111111;
// 1..AUTO_LOW_OCCUPANCY_MAX people: only CH1 (light1), CH2 (light2) and CH5 (fan1) are ON.
// More than that: every AUTO channel is ON.
static const int AUTO_LOW_OCCUPANCY_MAX = 10;
static const uint8_t AUTO_LOW_OCCUPANCY_MASK = 0b00010011;
static const unsigned long VACANCY_DELAY_MS = 30000;

static const unsigned long IR_DEBOUNCE_MS = 20;

// A count is accepted only after a complete BREAK -> HOLD -> CLEAR pulse.
// This rejects electrical glitches shorter than 30 ms and abnormally long obstructions.
// Fast human crossings are accepted, but IR break-beam alone cannot prove the object is human.
static const unsigned long PASSAGE_MIN_BREAK_MS = 30;
static const unsigned long PASSAGE_MAX_BREAK_MS = 5000;
static const unsigned long IR_RETRIGGER_GUARD_MS = 20;
static const unsigned long IR_MIN_CLEAR_GAP_MS = 20;
static const uint32_t PASSAGE_MIN_BREAK_US = PASSAGE_MIN_BREAK_MS * 1000UL;
static const uint32_t PASSAGE_MAX_BREAK_US = PASSAGE_MAX_BREAK_MS * 1000UL;
static const uint32_t IR_MIN_CLEAR_GAP_US = IR_MIN_CLEAR_GAP_MS * 1000UL;

// A high burst of sub-minimum IR pulses means the input is electrically noisy/floating.
// Counting is locked out until the line is quiet so noise cannot become a false occupant.
static const unsigned long SENSOR_NOISE_WINDOW_MS = 1000;
static const unsigned long SENSOR_NOISE_LOCKOUT_MS = 5000;
static const uint32_t SENSOR_SHORT_PULSE_ALARM_COUNT = 12;
static const uint32_t SENSOR_EDGE_BURST_ALARM_COUNT = 24;

// ----------------------------------------------------------------------------- Local I/O timing

static const unsigned long BT_STATE_PUSH_MS = 500;
// Without BT_CLOUD_COEXIST, Bluetooth is off while Wi-Fi is connected (its ~80 KB leaves no room
// for Firebase TLS).
// It is stopped once Wi-Fi has been up this long, and restarted once Wi-Fi has been down this
// long. Keep the restart delay above the 10 s after which the TLS session/reserve are freed.
static const unsigned long BT_OFF_AFTER_WIFI_UP_MS = 5000;
static const unsigned long BT_ON_AFTER_WIFI_DOWN_MS = 30000;
// A local Bluetooth session (the app's Bluetooth mode asks through Firebase) stops Firebase and
// keeps Bluetooth on while online: this long waiting for a phone, then while a phone is connected
// plus BT_SESSION_IDLE_MS after it leaves. It also ends BT_SESSION_CONTROL_IDLE_MS after the
// last command (anything but get_state), so a phone left connected cannot keep the cloud off.
static const unsigned long BT_SESSION_WAIT_MS = 120000;
static const unsigned long BT_SESSION_IDLE_MS = 30000;
static const unsigned long BT_SESSION_CONTROL_IDLE_MS = 600000;
// MANUAL mode needs someone to control it. When neither Firebase nor a Bluetooth phone has reached
// the controller for this long (both links down), it switches itself back to AUTO so the lights
// and fans follow occupancy again. Longer than BT_SESSION_WAIT_MS, so a phone that is still
// connecting for a local session is not overridden.
static const unsigned long CONTROL_LINK_LOST_AUTO_MS = 180000;
static const unsigned long PZEM_READ_MS = 1000;
static const unsigned long LCD_PAGE_MS = 3000;

// ----------------------------------------------------------------------------- Network / Firebase

// Wi-Fi reconnect attempts back off from WIFI_RETRY_MS to WIFI_RETRY_MAX_MS while offline.
static const unsigned long WIFI_RETRY_MS = 8000;
static const unsigned long WIFI_RETRY_MAX_MS = 60000;
static const unsigned long FIREBASE_COMMAND_POLL_MS = 1000;

// State is pushed as soon as it changes (throttled to FIREBASE_STATE_MIN_INTERVAL_MS) and
// otherwise as a heartbeat. 3 s keeps the app's 8 s stale / 6 s PZEM-age limits satisfied.
static const unsigned long FIREBASE_STATE_PUSH_MS = 3000;
static const unsigned long FIREBASE_STATE_MIN_INTERVAL_MS = 250;
static const unsigned long FIREBASE_STATE_RETRY_MS = 1000;

// Firebase commands older than this (by requested_at_ms vs NTP time) are rejected instead
// of being executed late, e.g. after the controller was offline.
static const uint64_t FIREBASE_COMMAND_MAX_AGE_MS = 120000ULL;

// Room local time (Philippines, UTC+8, no DST). Used for NTP and energy-history hour keys.
static const long LOCAL_UTC_OFFSET_SEC = 8L * 3600L;
// After a failed history write, wait this long before retrying the same hour.
static const unsigned long ENERGY_HISTORY_RETRY_MS = 30000;

// Minimum heap to attempt an HTTPS request (see tlsHeapAvailable() in cloud.cpp). The prebuilt
// Arduino mbedTLS allocates a ~16.7 KB input AND a ~16.7 KB output record buffer per session,
// plus certificate/handshake working memory, so a handshake needs ~40 KB free.
static const uint32_t TLS_MIN_MAX_ALLOC_BYTES = 17000;
static const uint32_t TLS_MIN_FREE_HEAP_BYTES = 40000;

static const unsigned long HTTP_CONNECT_TIMEOUT_MS = 10000;
static const unsigned long HTTP_REQUEST_TIMEOUT_MS = 12000;
static const unsigned long TLS_HANDSHAKE_TIMEOUT_SEC = 12;
static const unsigned long FIREBASE_AUTH_RETRY_MIN_MS = 10000;
static const unsigned long FIREBASE_AUTH_RETRY_MAX_MS = 60000;
static const unsigned long NETWORK_DIAG_INTERVAL_MS = 30000;
static const unsigned long FIREBASE_TRANSPORT_PROBE_TIMEOUT_MS = 3500;
static const unsigned long FIREBASE_WIFI_RECOVERY_COOLDOWN_MS = 90000;
static const uint8_t FIREBASE_TRANSPORT_FAILURES_BEFORE_WIFI_RECOVERY = 3;

// ----------------------------------------------------------------------------- Watchdog

// Task watchdog: a hung sensor task or loop() reboots the controller (relays boot OFF, then
// AUTO restores them). HTTPS can legitimately block for tens of seconds, so the network task
// is supervised by a separate heartbeat checked from loop() instead.
static const uint32_t TASK_WATCHDOG_TIMEOUT_MS = 30000;

// Task stacks come out of the same heap TLS needs. Free stack per task is logged as [MEM]
// every MEM_DIAG_INTERVAL_MS; keep at least ~1 KB spare in each.
static const uint32_t SENSOR_TASK_STACK_BYTES = 4096;
static const uint32_t NETWORK_TASK_STACK_BYTES = 10240; // ~5 KB used before TLS; handshake needs more
static const unsigned long MEM_DIAG_INTERVAL_MS = 60000;
static const unsigned long NETWORK_TASK_STALL_RESTART_MS = 300000;
