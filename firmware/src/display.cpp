#include "display.h"
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

#include "bluetooth.h"
#include "energy.h"
#include "cloud.h"
#include "occupancy.h"
#include "provisioning.h"
#include "state.h"

// Created by lcdBegin() once the backpack's I2C address is known; null when no LCD answered.
static LiquidCrystal_I2C *lcd = nullptr;

unsigned long lastLcdPageAt = 0;
uint8_t lcdPage = LCD_PAGE_INTRO;

// Relay indexes (0-based) of each zone: zone A = L1, L2, F1; zone B = L3, L4, F2.
static const uint8_t ZONE_A_RELAYS[3] = {0, 1, 4};
static const uint8_t ZONE_B_RELAYS[3] = {2, 3, 5};

// Occupancy/relay values last drawn, so refreshLcdOnChange() only redraws on a real change.
static int shownPeople = -1;
static bool shownValid = false;
static uint8_t shownRelayMask = 0xFF;

static bool i2cDevicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void lcdBegin() {
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  uint8_t address = 0;
  if (i2cDevicePresent(LCD_ADDRESS)) address = LCD_ADDRESS;
  else if (i2cDevicePresent(LCD_ALT_ADDRESS)) address = LCD_ALT_ADDRESS;
  if (!address) {
    Serial.printf("[LCD] not found at 0x%02X or 0x%02X: check SDA %u / SCL %u wiring and power\n",
                  LCD_ADDRESS, LCD_ALT_ADDRESS, LCD_SDA_PIN, LCD_SCL_PIN);
    return;
  }
  Serial.printf("[LCD] found at 0x%02X\n", address);
  lcd = new LiquidCrystal_I2C(address, LCD_COLS, LCD_ROWS);
  lcd->init();
  lcd->backlight();
}

void lcdPrintLine(uint8_t row, String s) {
  if (!lcd) return;
  if (s.length() > LCD_COLS) s = s.substring(0, LCD_COLS);
  while (s.length() < LCD_COLS) s += ' ';
  lcd->setCursor(0, row);
  lcd->print(s);
}

static String centered(const String &s) {
  String out;
  for (int pad = (LCD_COLS - (int)s.length()) / 2; pad > 0; pad--) out += ' ';
  return out + s;
}

static bool zoneOn(const uint8_t relays[3]) {
  return relayState[relays[0]] || relayState[relays[1]] || relayState[relays[2]];
}

// Load label of a relay index: relays 1-4 are lights L1-L4, relays 5-6 are fans F1-F2.
static String loadLabel(uint8_t relay) {
  return relay < 4 ? "L" + String(relay + 1) : "F" + String(relay - 3);
}

// "ZONE A: L1|L2|F1" over "ON | OFF | ON".
static void zoneLines(char zone, const uint8_t relays[3], String &line0, String &line1) {
  line0 = String("ZONE ") + zone + ":";
  line1 = "";
  for (uint8_t i = 0; i < 3; i++) {
    line0 += String(i ? "|" : " ") + loadLabel(relays[i]);
    line1 += String(i ? " | " : "") + (relayState[relays[i]] ? "ON" : "OFF");
  }
}

static uint8_t relayMask() {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < RELAY_COUNT; i++) if (relayState[i]) mask |= 1 << i;
  return mask;
}

void updateLcd() {
  if (!lcd) return;
  // Build both lines under the state lock, then write them after releasing it: a full redraw
  // is ~32 slow I2C character writes (tens of ms) and must not hold up the sensor task.
  String line0, line1;
  {
    StateLock lock;
    if (lcdPage == LCD_PAGE_INTRO) {
      line0 = centered("ESAVE");
      line1 = centered("CAPSTONE 2026");
    } else if (lcdPage == LCD_PAGE_CONNECTIVITY) {
      String wifi = !wifiConfigured ? "NONE" : (WiFi.status() == WL_CONNECTED ? "OK" : "--");
      String bt = !bluetoothActive ? "OFF" : (SerialBT.hasClient() ? "ON" : "--");
      line0 = "WiFi:" + wifi + "  F:" + (firebaseAuthenticated ? "OK" : "--");
      line1 = "BT:" + bt;
    } else if (lcdPage == LCD_PAGE_OCCUPANCY) {
      line0 = "OCCUPANCY: " + String(occupantCount) + (occupancyValid ? "" : " ?");
      line1 = String("A:") + (zoneOn(ZONE_A_RELAYS) ? "ON " : "OFF") + "   B:" + (zoneOn(ZONE_B_RELAYS) ? "ON" : "OFF");
    } else if (lcdPage == LCD_PAGE_ZONE_A) {
      zoneLines('A', ZONE_A_RELAYS, line0, line1);
    } else if (lcdPage == LCD_PAGE_ZONE_B) {
      zoneLines('B', ZONE_B_RELAYS, line0, line1);
    } else if (lcdPage == LCD_PAGE_ENERGY) {
      if (pzemValid) {
        line0 = String(voltage, 1) + "V " + String(currentA, 2) + "A";
        line1 = String(powerW, 0) + "W " + String(energyKWh, 2) + "kWh";
      } else {
        line0 = centered("ROOM 206");
        line1 = "000V | 0.0A";
      }
    } else {
      // Shown only while the setup AP is open (nextLcdPage() skips it otherwise).
      line0 = "WiFi SETUP AP";
      line1 = "192.168.4.1";
    }
    shownPeople = occupantCount;
    shownValid = occupancyValid;
    shownRelayMask = relayMask();
  }
  lcdPrintLine(0, line0);
  lcdPrintLine(1, line1);
}

void nextLcdPage(bool setupPortalActive) {
  uint8_t last = setupPortalActive ? LCD_PAGE_SETUP_AP : LCD_PAGE_ENERGY;
  // The intro is shown once at boot; the rotation then repeats from the connectivity page.
  lcdPage = lcdPage >= last ? LCD_PAGE_CONNECTIVITY : lcdPage + 1;
}

void refreshLcdOnChange() {
  if (!lcd || lcdPage < LCD_PAGE_OCCUPANCY || lcdPage > LCD_PAGE_ZONE_B) return;
  bool changed;
  {
    // Short wait: if another task holds the state, check again on the next loop() pass.
    StateLock lock(stateMutex, pdMS_TO_TICKS(2));
    if (!lock.held) return;
    changed = occupantCount != shownPeople || occupancyValid != shownValid || relayMask() != shownRelayMask;
  }
  if (changed) updateLcd();
}
