## tedder's METAR map

There are many like it, but this one's mine.

A wall map with one LED per airport. Each LED shows the airport's flight category
(VFR, MVFR, IFR, LIFR) from the current METAR. The controller is an
[Adafruit QT Py ESP32-S2](https://www.adafruit.com/product/5325) running ESP-IDF
firmware (`c/`). Weather comes from a small service (`fly.tedder.me/metar`) that
fetches METARs for a list of airports and returns one CSV line per airport with the
category and the LED color, so the device does no METAR parsing.

The v1 hardware is about $25 ($10ish for the carrier board, $12 for the QT Py). It's
simpler than a Pi, has less fussy power requirements, and is thin enough to wall-mount.

The original CircuitPython version has been removed. It's in the git history (last
version at commit `6763be1`) if you need it.

## Hardware

| Part | Pin |
|------|-----|
| WS2812 LED string | GPIO 5 (up to 74 LEDs, set in `c/main/main.c`) |
| SSD1306 OLED 128×32 (optional) | I2C, SDA GPIO 7 / SCL GPIO 6 |

4MB flash, 2MB PSRAM (PSRAM is needed for TLS and the FAT filesystem to fit together).

## Build

Needs [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) 5.5 or later (builds on 6.0.2).

```
cd c
idf.py set-target esp32s2
idf.py build
```

If `sdkconfig.defaults` changes, delete `sdkconfig` and rebuild.

## Flash

Put the QT Py in bootloader mode: hold BOOT while plugging in USB. Then:

```
idf.py -p /dev/cu.usbmodemXXXX flash
```

"Device not configured" at the very end of a flash is harmless; everything has been
written by then.

## Configure

Settings live in files on the device's FAT partition:

| File | Contents |
|------|----------|
| `config.txt` | `WIFI_SSID=...` and `WIFI_PASS=...` (see `c/config.txt.example`) |
| `airports.txt` | one ICAO code per line, in LED order. `OFF`, `OFF_LEGEND` or `NULL` leave that LED dark. See `c/airports.txt.example`. |
| `errors.txt` | log written by the firmware |

Two ways to edit them:

- **Web page:** `http://<device IP>/config` (WiFi and airports).
- **USB drive:** on `http://<device IP>/usb`, switch storage to USB mode. The device
  then shows up on the computer as a drive named METARMAP. Edit the files, eject,
  and switch back to app mode (or reboot).

## Web pages

| Path | What |
|------|------|
| `/` | status: firmware version, board ID, fetch counts |
| `/airports` | each LED: airport, category, report age |
| `/config` | WiFi and airport list |
| `/debug` | debug flags and last error |
| `/log` | `errors.txt` |
| `/usb` | switch storage between the firmware and USB |

## Updates

The firmware checks for a new build over HTTPS (`esp_https_ota`) every 8 weather
fetches (about 72 minutes) and installs it if the version differs.

## License

MIT, see `LICENSE`.
