# Plane Radar for Waveshare ESP32-C6-Touch-LCD-1.28

Port of [MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar) (MIT) to the [Waveshare ESP32-C6-Touch-LCD-1.28](https://www.waveshare.com/esp32-c6-touch-lcd-1.28.htm).

The original firmware targets an ESP32-C3 Super Mini wired to a separate 1.28″ GC9A01 module. This board is an ESP32-C6 with the same 240×240 round GC9A01A panel, a CST816S touch controller, and 16 MB of flash already on the PCB. Radar behavior is unchanged: a Wi-Fi setup portal, then a live ADS-B picture from [adsb.fi](https://opendata.adsb.fi/).

## What it does

1. **Wi-Fi setup** (if needed) — captive portal on AP **`PlaneRadar-Setup`**
2. **Radar** — live aircraft around the location saved in the portal

After Wi-Fi is saved, the device reconnects on its own. Aircraft positions update about every 5 seconds and the screen redraws at 4 Hz.

## Controls

| Action | Effect |
|--------|--------|
| **Tap the screen** | Cycle range preset (5 → 10 → 15 → 25 km); saved to flash |
| **Short BOOT press** (GPIO 9) | Same range cycle |
| **Hold BOOT 3 s** | Clear Wi-Fi, location, and units; reboot into the setup portal |

Holding BOOT at power-on also forces a credential reset.

## Wi-Fi setup portal

**First-time setup** (no saved Wi-Fi):

1. Connect to **`PlaneRadar-Setup`**
2. Open **`http://plane-radar.local`** or **`http://192.168.4.1`**
3. Set home Wi-Fi, latitude, longitude, distance units, and the runway overlay, then save

The same portal stays available later at **`http://plane-radar.local`** or the device’s LAN IP.

## Flash

The merged image is `firmware/plane-radar-esp32-c6-touch-lcd-1.28.bin`.

| Setting | Value |
|---------|--------|
| Chip | ESP32-C6 |
| Flash size | 16 MB |
| Flash mode | DIO |
| Flash frequency | 40 MHz |
| Offset | `0x0` |

Those match the header of Waveshare’s factory image for this board. The Type-C port is the ESP32-C6 USB Serial/JTAG controller.

With [esptool](https://github.com/espressif/esptool):

```bash
esptool.py --chip esp32c6 -b 921600 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB \
  0x0 firmware/plane-radar-esp32-c6-touch-lcd-1.28.bin
```

If auto-reset does not enter download mode, hold **BOOT**, tap **RESET**, then release **BOOT**.

A browser flasher such as [esptool-js](https://espressif.github.io/esptool-js/) can write the same file at offset **0x0**. Select the ESP32-C6 and a 16 MB flash.

Serial logs are 115200 baud on the USB port.

## Board wiring (onboard)

Nothing is wired by hand. The firmware uses the Waveshare BSP pinout:

| Function | GPIO |
|----------|------|
| LCD CS | 10 |
| LCD SCK | 11 |
| LCD MOSI | 21 |
| LCD DC | 19 |
| LCD RST | 20 |
| LCD backlight | 7 |
| TF card CS (held high) | 15 |
| Touch SDA / SCL | 18 / 8 |
| Touch INT / RST | 4 / 23 |
| BOOT | 9 |

The panel is inverted BGR, the same GC9A01 setup the original project uses, and it matches Waveshare’s init command `0x36 = 0x08` plus inversion on.

## Build

```bash
pio run -e c6touch
pio run -t merge -e c6touch
```

The merged file is also written to `.pio/build/c6touch/firmware-merged.bin`. `scripts/merge-firmware.sh` copies it to `release/plane-radar-merged.bin`.

The PlatformIO environment uses [pioarduino](https://github.com/pioarduino/platform-espressif32) so the Arduino-ESP32 3.x core (required by this chip) is available. Official PlatformIO’s espressif32 platform does not build Arduino sketches for the ESP32-C6.

## Configuration

Hardware pins and timing live in `include/config.h`. Range presets live in `include/ui/radar_range.h`.

## License

MIT. Original copyright (c) 2026 MatixYo. See `LICENSE`.
