#pragma once

#include <cstdint>

#include <driver/gpio.h>

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- BOOT button (Waveshare ESP32-C6-Touch-LCD-1.28, active LOW) ---
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
constexpr unsigned long kBootResetHoldMs = 3000UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;

// --- Display: onboard GC9A01A 1.28" round 240×240 (SPI)
// Pins from the Waveshare BSP (examples/.../esp32_c6_touch_lcd_1_28.h)
// and the Arduino HelloWorld sketch shipped for this board.
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_20;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_10;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_19;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_21;
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_11;
constexpr gpio_num_t kDisplayPinBl = GPIO_NUM_7;
/** TF-card CS shares the LCD SPI bus. Hold it high so a card cannot fight the panel. */
constexpr gpio_num_t kSdCardCsPin = GPIO_NUM_15;

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

/** Vendor examples clock the panel at 80 MHz; 40 MHz leaves margin on this SPI bus. */
constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9A01 modules need invert + BGR. Waveshare's panel init writes MADCTL 0x08 (BGR)
// and display-inversion on (command 0x21).
constexpr bool kDisplayInvert = true;
constexpr bool kDisplayRgbOrder = true;

// --- CST816S capacitive touch (I2C, shared with the onboard IMU and RTC) ---
constexpr gpio_num_t kTouchPinSda = GPIO_NUM_18;
constexpr gpio_num_t kTouchPinScl = GPIO_NUM_8;
constexpr gpio_num_t kTouchPinInt = GPIO_NUM_4;
constexpr gpio_num_t kTouchPinRst = GPIO_NUM_23;
constexpr uint8_t kTouchI2cAddr = 0x15;
constexpr uint32_t kTouchI2cHz = 400000;

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 52.3676;
constexpr double kDefaultRadarLon = 4.9041;

/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 5000;
/** Redraw cadence; aircraft are dead-reckoned along their track/speed so motion
 *  is smooth. ~4 Hz = 250 ms (panel scanout caps at ~24 Hz; the full-frame
 *  recompose+present is the practical limit ~10-15 Hz). */
constexpr unsigned long kRadarRedrawIntervalMs = 250;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;
/**
 * Extra serial lines for ADS-B fetches and radar drawing: request URL, drop
 * reasons, heap around the frame sprite, and why the screen is empty.
 * Leave false for normal use; set true and rebuild to debug a blank radar.
 */
constexpr bool kDebugLog = false;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
