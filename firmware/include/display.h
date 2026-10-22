#pragma once
// 16x2 I2C LCD status pages.

#include <Arduino.h>

#include "config.h"

// Pages in display order. LCD_PAGE_INTRO is shown only at boot.
enum LcdPage : uint8_t {
  LCD_PAGE_INTRO,
  LCD_PAGE_CONNECTIVITY,
  LCD_PAGE_OCCUPANCY,
  LCD_PAGE_ZONE_A,
  LCD_PAGE_ZONE_B,
  LCD_PAGE_ENERGY,
  LCD_PAGE_SETUP_AP,
};

extern unsigned long lastLcdPageAt;
extern uint8_t lcdPage;

// Starts I2C, finds the LCD at LCD_ADDRESS or LCD_ALT_ADDRESS and logs the result. Without an
// LCD every other call here is a no-op, so the controller runs the same headless.
void lcdBegin();
void lcdPrintLine(uint8_t row, String s);
void updateLcd();
// Advances lcdPage; the setup AP page is included only while the setup portal is open.
void nextLcdPage(bool setupPortalActive);
// Called every loop() pass: redraws the occupancy/zone pages as soon as the occupant count or
// a relay changes instead of waiting for the next page flip.
void refreshLcdOnChange();
