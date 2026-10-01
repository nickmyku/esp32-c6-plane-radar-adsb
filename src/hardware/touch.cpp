#include "hardware/touch.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"

namespace {

bool s_ready = false;
bool s_touching = false;
unsigned long s_down_ms = 0;
unsigned long s_last_tap_ms = 0;
unsigned long s_retry_after_ms = 0;
int s_fail_streak = 0;

constexpr unsigned long kTapMinMs = 40;
constexpr unsigned long kTapMaxMs = 800;
constexpr unsigned long kTapCooldownMs = 280;
/** Back off after a NACK so a stuck bus does not flood the log every loop. */
constexpr unsigned long kFailBackoffMs = 100;
/** GPIO-reset the controller after this many failed polls. */
constexpr int kFailResetThreshold = 4;

constexpr uint8_t kRegGesture = 0x01;
constexpr uint8_t kRegChipId = 0xA7;
/** DisAutoSleep. Any non-zero value keeps the controller in dynamic mode. */
constexpr uint8_t kRegDisAutoSleep = 0xFE;
constexpr uint8_t kDisAutoSleep = 0x01;

void wireBegin() {
  Wire.begin(static_cast<int>(config::kTouchPinSda),
             static_cast<int>(config::kTouchPinScl));
  Wire.setClock(config::kTouchI2cHz);
}

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(config::kTouchI2cAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool readRegs(uint8_t reg, uint8_t* dst, size_t len) {
  Wire.beginTransmission(config::kTouchI2cAddr);
  Wire.write(reg);
  // STOP, then a new START. endTransmission(false) makes requestFrom call
  // i2cWriteReadNonStop(), and the ESP-IDF master reports a CST816 NACK from
  // that combined transfer as ESP_ERR_INVALID_STATE (259). The bus FSM is
  // then left non-idle, so every later poll logs the same error.
  if (Wire.endTransmission(true) != 0) {
    return false;
  }
  if (Wire.requestFrom(config::kTouchI2cAddr, len) != len) {
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

void recoverBus(bool hard_reset) {
  // Drop the IDF master so a non-idle FSM cannot keep returning
  // ESP_ERR_INVALID_STATE. A GPIO reset also wakes a sleeping CST816;
  // DisAutoSleep is cleared by that reset and has to be written again.
  Wire.end();
  if (hard_reset) {
    Serial.println("Touch: I2C stuck — resetting CST816S");
    resetController();
  }
  wireBegin();
  if (hard_reset) {
    writeReg(kRegDisAutoSleep, kDisAutoSleep);
  }
}

}  // namespace

void touchInit() {
  resetController();
  wireBegin();

  uint8_t chip_id = 0;
  // CST816 chip-id register. A missing ACK means touch stays disabled.
  if (!readRegs(kRegChipId, &chip_id, 1) || chip_id == 0x00 || chip_id == 0xFF) {
    Serial.println("Touch: CST816S not found — BOOT button still cycles range");
    s_ready = false;
    return;
  }
  // Default auto-sleep is ~2 s. After that the controller NACKs polls, which
  // this core logs as ESP_ERR_INVALID_STATE and then refuses further transfers.
  if (!writeReg(kRegDisAutoSleep, kDisAutoSleep)) {
    Serial.println("Touch: failed to disable auto-sleep");
  }
  s_ready = true;
  Serial.printf("Touch: CST816S id 0x%02X\n", chip_id);
}

bool touchConsumeTap() {
  if (!s_ready) {
    return false;
  }

  const unsigned long now = millis();
  if (static_cast<long>(now - s_retry_after_ms) < 0) {
    return false;
  }

  uint8_t raw[2] = {0, 0};
  // 0x01 gesture, 0x02 finger count.
  if (!readRegs(kRegGesture, raw, sizeof(raw))) {
    ++s_fail_streak;
    const bool hard_reset = s_fail_streak >= kFailResetThreshold;
    recoverBus(hard_reset);
    if (hard_reset) {
      s_fail_streak = 0;
    }
    s_retry_after_ms = millis() + kFailBackoffMs;
    return false;
  }
  s_fail_streak = 0;

  // 0xFF is "no data" on some CST816T parts once auto-sleep is disabled.
  const bool down = raw[1] > 0 && raw[1] != 0xFF;
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
