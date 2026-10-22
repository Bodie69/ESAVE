# ESAVE — Room 206

Occupancy-driven lighting and fan control with energy monitoring for one classroom.

```
 Entrance IR ─┐                                   ┌─ Firebase Auth (controller + app logins)
 Exit IR ─────┤   ESP32 controller     HTTPS/TLS  │
 PZEM-004T ───┼── (firmware/)     ───────────────►├─ Realtime Database  rooms/room206/...
 6 relays ◄───┤          ▲                        │      state · commands · acks · energy_history
 I2C LCD ◄────┘          │ Bluetooth SPP (local)  └───────────────▲
                         └──────────────── Android app (app/) ────┘
```

- **AUTO mode:** with 1–10 people counted inside, relays CH1, CH2 and CH5 (light1, light2, fan1)
  are ON; with 11 or more, all six relays are ON (dropping back to 10 switches CH3, CH4 and CH6
  off). Everything turns off after the lights-off delay once the room is empty. **MANUAL mode:** each channel is switched from the app.
- The app works over **Firebase** (anywhere with internet) or **Bluetooth** (in the room, no
  internet needed). Every command is confirmed by the ESP32 (ACK) before the app reports success.

| Path | What it is |
|---|---|
| `firmware/` | ESP32 firmware, PlatformIO project (`platformio.ini`, `src/`, `include/`) |
| `app/` | Android app (Java, no AndroidX), unit tests in `app/src/test` |
| `database.rules.json`, `firebase.json` | Realtime Database security rules |
| `firebase-tests/` | Emulator tests for the rules (`npm test`) |
| `artwork/` | Source icon and the script that generates the adaptive launcher icon |

## Prerequisites

| Tool | Needed for | Notes |
|---|---|---|
| Git | everything | `git clone https://github.com/Bodie69/ESAVE.git` |
| [PlatformIO](https://platformio.org/install) (CLI or VS Code extension) | firmware | Downloads the ESP32 platform, toolchain and libraries on first build |
| USB-serial driver for the board (CP210x or CH340) | flashing | The board shows up as a COM port (Windows) or `/dev/ttyUSB*` (Linux) |
| JDK 17 | Android app | Android Studio's bundled JDK works; Gradle 8.14 and AGP 8.11.1 come from the wrapper |
| Android SDK, platform 35 | Android app | Install with Android Studio, or point `sdk.dir` in `local.properties` at an existing SDK |
| Node.js + Java | rules tests only | `firebase-tools` is installed locally by `npm install` |
| A Firebase project | cloud features | Realtime Database + Email/Password Authentication |

## Quick start

1. **Firebase:** create the project, the accounts, the access list and deploy the rules
   ([section 3](#3-firebase)).
2. **Firmware:** copy `firmware/include/secrets.example.h` to `secrets.h`, fill it in, then build
   and flash ([section 2](#2-firmware)).
3. **App:** add the Firebase settings to `local.properties`, then build and install the APK
   ([section 4](#4-android-app)).
4. **Room:** set the controller's Wi-Fi, pair the phone, correct the count
   ([section 5](#5-commissioning-a-room)).

The room keeps working over Bluetooth without step 1, but the app build still needs
placeholder values for `rsu.firebaseApiKey` and `rsu.firebaseDatabaseUrl`.

## 1. Hardware and wiring

Board: **ESP32-WROOM-32** (classic ESP32 — Bluetooth Classic SPP is required, so not S3/C3).

| Function | ESP32 pin | Notes |
|---|---|---|
| Entrance IR receiver OUT | GPIO23 | `INPUT_PULLUP`; NPN/open-collector; common GND; must be 3.3 V-safe |
| Exit IR receiver OUT | GPIO32 | `INPUT_PULLUP`; external 10 k pull-up recommended |
| Relay CH1–CH4 (light1–light4) | GPIO13, 33, 14, 27 | relay board is **active LOW** |
| Relay CH5–CH6 (fan1, fan2) | GPIO26, 25 | active LOW |
| Former relay CH8 | GPIO18 | held OFF at boot; unused |
| PZEM-004T v3 TX → ESP32 RX2 | GPIO16 | UART2, 9600 baud |
| PZEM-004T v3 RX ← ESP32 TX2 | GPIO17 | |
| I2C LCD 16×2 SDA / SCL | GPIO21 / GPIO22 | address `0x27` (change `LCD_ADDRESS` if yours is `0x3F`) |

The old relay IN7 wire must **not** be connected to GPIO23 any more (GPIO23 is the Entrance sensor).

## 2. Firmware

Built with **PlatformIO** (CLI `pio`, or the PlatformIO extension for VS Code). Board, partition
table and library versions are pinned in `firmware/platformio.ini`.

```sh
cd firmware
cp include/secrets.example.h include/secrets.h   # then fill it in (git-ignored; no Wi-Fi here)
pio run                     # build (default env: Arduino-ESP32 core 3.3.11)
pio run -t upload           # build and flash
pio device monitor          # serial log, 115200 baud, decodes crash backtraces
```

On Windows (PowerShell) use `Copy-Item include\secrets.example.h include\secrets.h` for the copy
step. If several serial ports are present, pick one with `pio run -t upload --upload-port COM5`
(and `pio device monitor --port COM5`). If the upload stalls at `Connecting...`, hold the board's
**BOOT** button, tap **EN**, then release BOOT once the upload starts.

| Environment | Platform | Arduino-ESP32 core | Notes |
|---|---|---|---|
| `esp32dev` (default) | pioarduino `55.03.311` | 3.3.11 | Recommended; the version the firmware is verified on. First build downloads the core and toolchain (several GB unpacked). |
| `esp32dev-core2` | `espressif32 @ 6.7.0` | 2.0.16 | Fallback with a much smaller download; core 2.x is end-of-life. `pio run -e esp32dev-core2` |

Libraries (installed automatically): ArduinoJson 7.4.3, PZEM-004T-v30 1.2.1 (GitHub tag; the
registry stops at 1.1.2), LiquidCrystal_I2C 1.1.4. The image is about 1.8 MB, so the project uses
the `huge_app.csv` (3 MB app) partition table.

**Source layout** — one module per subsystem; `include/<module>.h` declares what other modules use,
everything else stays private (`static`) in `src/<module>.cpp`:

| Module | Responsibility |
|---|---|
| `include/config.h` | Pins, timing constants, `FIRMWARE_VERSION`, feature switches, build checks |
| `main.cpp` | `setup()` boot sequence and `loop()` (Bluetooth, PZEM sampling, LCD, watchdog) |
| `state` | Shared state, `StateLock` (recursive mutex), NVS-persisted settings |
| `occupancy` | IR break-beam ISRs, validated passage latching, noise guard, people count |
| `relays` | Relay outputs with GPIO read-back, AUTO occupancy rules |
| `energy` | PZEM-004T sampling and kWh reset |
| `provisioning` | Wi-Fi station with NVS-saved credentials, reconnect backoff, setup AP + captive portal |
| `cloud` | Network diagnostics, certificate-verified HTTPS, Firebase Auth and database requests |
| `telemetry` | State JSON, Firebase state pushes, hourly kWh history |
| `bluetooth` | Bluetooth SPP JSON commands, state pushes, ACKs |
| `commands` | Command execution (shared by Bluetooth and Firebase), Firebase command polling, ACKs |
| `display` | 16x2 LCD pages |
| `tasks` | FreeRTOS tasks (realtime sensor, Firebase network) and the task watchdog |

(The Wi-Fi/Firebase module is called `cloud`, not `network`: core 3 ships a `Network.h`, and on
case-insensitive file systems a local `network.h` would shadow it.)

**Secrets:** `include/secrets.h` holds the Firebase Web API key and database URL, the
**controller's** Firebase account, a 6–16 digit Bluetooth PIN and the 8–63 character
`SETUP_AP_PASSWORD`. Wi-Fi credentials are **not** compiled in (the build fails if `WIFI_SSID` is
still defined): they are entered on the setup access point, see below. The build fails without
`BLUETOOTH_PIN` or `SETUP_AP_PASSWORD`; the firmware refuses to sign in while any `YOUR_...`
placeholder remains.

**Wi-Fi setup (access point):**
- With no saved network the controller opens the WPA2 access point **`ESAVE-Room206-Setup`**
  (password = `SETUP_AP_PASSWORD`). Join it from a phone or laptop; the setup page opens by itself
  (captive portal), or browse to **http://192.168.4.1/**. Pick a network, enter its password,
  *Save and connect*. The page shows whether it connected; the LCD shows `WiFi SETUP AP` while open.
- The network is saved in NVS (namespace `rsu-wifi`) and survives reboots and re-flashing
  (a full `pio run -t erase` clears it).
- To change networks later: in the app, connect by Bluetooth and tap *Settings → Controller Wi-Fi →
  OPEN WI-FI SETUP* (sends `{"cmd":"wifi_setup"}`), or hold the board's **BOOT** button for 3 s. The AP also opens by itself when the saved network has
  been unreachable for 3 minutes. Once online again it closes after 3 minutes without use.
- The page can also *Forget saved network*. The stored password is never shown or sent to the app;
  the app's state gets `connectivity.wifi_configured`, `wifi_ssid` and `wifi_setup_ap`.

**Offline operation:** Wi-Fi and Internet are optional. Without them, IR counting, AUTO lights/fans,
relays, the PZEM meter, the LCD and Bluetooth control from the app all keep working (the setup AP
can be open at the same time). Reconnect attempts back off from 8 s to 60 s so constant scanning
does not compete with Bluetooth. Without NTP time the hourly kWh history is skipped until the
controller is back online; Firebase state and commands resume automatically on reconnect.
If the room is in MANUAL mode and neither Firebase nor a Bluetooth phone reaches the controller
for 3 minutes (`CONTROL_LINK_LOST_AUTO_MS`), it switches back to AUTO (saved in NVS, and reported as
`last_control_source: link_lost_auto`), so lights and fans keep following occupancy.

**Behaviour worth knowing:**
- Firebase connections verify Google's certificates (`include/firebase_roots.h`: GTS Root R1–R4 +
  GlobalSign Root CA). The ESP32 therefore waits for NTP time before contacting Firebase.
- Commands older than 2 minutes (by NTP time) are rejected with `COMMAND_EXPIRED`.
- One cumulative kWh reading per hour (room time, UTC+8) is stored under `energy_history/hourly`
  (not shown in the app; viewable in the Firebase console).
- A task watchdog (30 s) restarts the controller if the sensor task or `loop()` hangs; the
  network task restarts it if it makes no progress for 5 minutes. Relays boot OFF, then AUTO
  restores them from the saved occupancy count.
- After any reboot the occupancy count is flagged *needs correction* (people may have moved
  while it was off); AUTO keeps running on the saved count meanwhile.
- Bluetooth accepts JSON commands only. Diagnostics switches for `secrets.h`:
  `FIREBASE_TLS_INSECURE 1` (disables certificate checks — troubleshooting only) and
  `BLUETOOTH_LEGACY_TEXT_COMMANDS 1` (accept `ALL OFF`-style serial-terminal commands).

## 3. Firebase

1. **Authentication → Sign-in method:** enable *Email/Password*. Create one account for the ESP32
   (used in `secrets.h`) and one per app user. Use a strong password for the controller account.
2. **Realtime Database → Data:** add the access list (copy UIDs from *Authentication → Users*):
   ```
   access/controllers/<ESP32 account UID> = "room206"
   access/operators/<app user UID>/room206 = true
   ```
   One account may be in both lists if the app and the ESP32 share a login.
3. **Deploy the rules** — only *after* step 2, or every request is denied:
   paste `database.rules.json` into *Realtime Database → Rules*, or run
   `firebase login` and then `firebase deploy --only database --project <project-id>` from the
   repository root (without a global install, use `npx firebase` from `firebase-tests/` after
   step 4's `npm install`, adding `--config ../firebase.json`).
4. **Test the rules** (needs Node.js and Java; starts a local emulator, touches nothing online):
   ```sh
   cd firebase-tests && npm install && npm test     # expect: 83/83 passed
   ```
   On Windows the emulator can be left running afterwards; stop the `java` process on port 9000
   before running the tests again.

Database layout under `rooms/room206/`: `state` (ESP32 → app), `commands/*` (app → ESP32),
`acks/by_id/<command_id>` and `acks/last` (ESP32 → app), `energy_history/hourly/<YYYYMMDDHH>`.

## 4. Android app

Add the per-installation settings to `local.properties` (git-ignored; environment variables
`RSU_FIREBASE_API_KEY`, `RSU_FIREBASE_DATABASE_URL`, `RSU_BLUETOOTH_PIN` also work for CI):

```properties
rsu.firebaseApiKey=<Firebase Web API key>
rsu.firebaseDatabaseUrl=https://<instance>.<region>.firebasedatabase.app/
# Optional: shown in the app as a pairing hint. Omit to keep the PIN off phones.
rsu.bluetoothPin=<same as BLUETOOTH_PIN in secrets.h>
```

```sh
./gradlew testDebugUnitTest      # unit tests (controller state parsing)
./gradlew assembleDebug          # debug APK
./gradlew assembleRelease        # R8-shrunk release APK (unsigned without keystore.properties)
./gradlew installDebug           # build and install on a connected phone (USB debugging on)
```

On Windows use `gradlew.bat` (or `.\gradlew` in PowerShell). The APKs land in
`app/build/outputs/apk/<debug|release>/`. Opening the project folder in Android Studio works
too: it creates `local.properties` with `sdk.dir`, and you add the `rsu.*` lines to it.

Release signing: create `keystore.properties` in the project root (git-ignored):

```properties
storeFile=release.jks
storePassword=...
keyAlias=...
keyPassword=...
```

Requirements: Android 8.0+ (API 26). The app asks for *Nearby devices* (Bluetooth) and, on
Android 13+, *Notifications* (for the persistent Bluetooth link notice). The Firebase refresh
token is stored encrypted with an Android Keystore key and is never backed up.

## 5. Commissioning a room

1. Flash the firmware with **both IR beams clear** — the first boot saves the clear-beam levels.
   Re-run *Settings → Occupancy Sensors → CALIBRATE IR* any time the sensors are moved.
2. Join `ESAVE-Room206-Setup` and choose the classroom Wi-Fi at http://192.168.4.1/ (optional:
   the room works over Bluetooth alone).
3. Pair the phone with `ESAVE-Room206` in Android Bluetooth settings using the PIN.
4. In the app, sign in to Firebase (*Settings*), check the dashboard shows *Controller Online*,
   then use *CORRECT COUNT* to set the real number of people in the room.
5. In AUTO mode, lights and fans switch off 30 s after the room is counted empty
   (`VACANCY_DELAY_MS` in `firmware/include/config.h`).

## 6. Troubleshooting

| Symptom | Likely cause |
|---|---|
| App: `RTDB HTTP 401 … Permission denied` | That account's UID is not in `access/operators` (or the ESP32's not in `access/controllers`) |
| Serial: `[HTTP] … failed … ssl=… certificate` | Google changed roots beyond `include/firebase_roots.h`, or NTP time is wrong. Update the bundle from https://pki.goog/repo/certs/; use `FIREBASE_TLS_INSECURE 1` only to confirm |
| App: `ESP32 REJECTED … COMMAND_EXPIRED` | Phone clock more than 2 minutes off; enable automatic date/time |
| Dashboard: firmware older than this app expects | Flash the current firmware (schema 25.16+) |
| Dashboard: IR sensor noise detected | Floating/noisy sensor line: check common GND and 3.3 V-safe OUT, then correct the count |
| LCD shows `W:NONE` / `W:--` | No network saved / saved network unreachable: hold BOOT 3 s and set Wi-Fi at http://192.168.4.1/ |
| LCD shows `PZEM: NO DATA` | No AC on the PZEM or UART wiring (GPIO16/17) |
