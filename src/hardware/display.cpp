#include "hardware/display.h"

#include <Arduino.h>

#include "config.h"
#include "hardware/display_font.h"

LGFX tft;

void displayInit() {
  // The TF slot shares SCK/MOSI with the panel. Deselect it before the first transfer.
  pinMode(static_cast<uint8_t>(config::kSdCardCsPin), OUTPUT);
  digitalWrite(static_cast<uint8_t>(config::kSdCardCsPin), HIGH);

  pinMode(static_cast<uint8_t>(config::kDisplayPinBl), OUTPUT);
  digitalWrite(static_cast<uint8_t>(config::kDisplayPinBl), HIGH);

  tft.init();
  tft.setRotation(0);
  tft.setBrightness(255);
  tft.setTextWrap(false);
  displayFontInit();
}
