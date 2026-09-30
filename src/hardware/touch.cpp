#include "hardware/touch.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"

namespace {

bool s_ready = false;
bool s_touching = false;
unsigned long s_down_ms = 0;
unsigned long s_last_tap_ms = 0;

constexpr unsigned long kTapMinMs = 40;
constexpr unsigned long kTapMaxMs = 800;
constexpr unsigned long kTapCooldownMs = 280;

bool readRegs(uint8_t reg, uint8_t* dst, size_t len) {
  Wire.beginTransmission(config::kTouchI2cAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<int>(config::kTouchI2cAddr),
                       static_cast<int>(len)) != static_cast<int>(len)) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    dst[i] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

void resetController() {
  pinMode(static_cast<uint8_t>(config::kTouchPinRst), OUTPUT);
  pinMode(static_cast<uint8_t>(config::kTouchPinInt), INPUT);
  digitalWrite(static_cast<uint8_t>(config::kTouchPinRst), LOW);
  delay(10);
  digitalWrite(static_cast<uint8_t>(config::kTouchPinRst), HIGH);
  delay(50);
}

}  // namespace

void touchInit() {
  resetController();
  Wire.begin(static_cast<int>(config::kTouchPinSda),
             static_cast<int>(config::kTouchPinScl));
  Wire.setClock(config::kTouchI2cHz);

  uint8_t chip_id = 0;
  // CST816 chip-id register. A missing ACK means touch stays disabled.
  if (!readRegs(0xA7, &chip_id, 1) || chip_id == 0x00 || chip_id == 0xFF) {
    Serial.println("Touch: CST816S not found — BOOT button still cycles range");
    s_ready = false;
    return;
  }
  s_ready = true;
  Serial.printf("Touch: CST816S id 0x%02X\n", chip_id);
}

bool touchConsumeTap() {
  if (!s_ready) {
    return false;
  }

  uint8_t raw[2] = {0, 0};
  // 0x01 gesture, 0x02 finger count.
  if (!readRegs(0x01, raw, sizeof(raw))) {
    return false;
  }

  const bool down = raw[1] > 0;
  const unsigned long now = millis();
  bool tap = false;

  if (down && !s_touching) {
    s_down_ms = now;
  } else if (!down && s_touching) {
    const unsigned long held = now - s_down_ms;
    if (held >= kTapMinMs && held <= kTapMaxMs &&
        now - s_last_tap_ms >= kTapCooldownMs) {
      tap = true;
      s_last_tap_ms = now;
    }
  }
  s_touching = down;
  return tap;
}
