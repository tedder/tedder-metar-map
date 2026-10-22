## tedder's METAR map

There are many like it, but this one's mine.

A wall map with one LED per airport. Each LED shows the airport's flight category
(VFR, MVFR, IFR, LIFR) from the current METAR. The controller is an
[Adafruit QT Py ESP32-S2](https://www.adafruit.com/product/5325) running ESP-IDF
firmware (`c/`). Weather comes from a small service (`fly.tedder.me/metar`) that
fetches METARs for a list of airports and returns one CSV line per airport with the
category and the LED color, so the map does no METAR parsing.

The v1 hardware is about $25 ($10ish for the carrier board, $12 for the QT Py). It's
simpler than a Pi, has less fussy power requirements, and is thin enough to wall-mount.

The original CircuitPython version has been removed. It's in the git history (last
version at commit `6763be1`) if you need it.

- **Using a map** (setup, WiFi, airport list, what the lights mean): [docs/users-guide.md](docs/users-guide.md)
- **Every setting**, the remote config file, debug flags, telemetry: [docs/config-options.md](docs/config-options.md)
- **Firmware internals**: [c/NOTES.md](c/NOTES.md)

## Hardware

| Part | Pin |
|------|-----|
| WS2812/SK6812 LED string (RGB or RGBW, any color order) | GPIO 5 (A3) by default; up to 200 LEDs |
| SSD1306 OLED 128×32 (optional) | I2C, SDA GPIO 7 / SCL GPIO 6 |
| BOOT button | on the QT Py; unlocks saving, opens setup |
| status NeoPixel | on the QT Py |

4MB flash, 2MB PSRAM. The LED count comes from the airport list, and the color order,
chip type and data pin are settings, so one firmware fits every map.

## Features

- Setup network `metarmap-XXXXXX` with a captive page for WiFi and the airport list; up to
  three saved networks; falls back to the setup network when WiFi is gone for 5 minutes.
- Status page at `http://metarmap.local/`: weather per LED, brightness, LED tests, Find an LED.
- Signed OTA updates with a beta and a stable channel, partial downloads, and rollback: a new
  version that can't fetch weather goes back to the old one by itself.
- Per-map remote config file (maintainer), so settings can change without visiting the map.
- Night dimming (NTP + time zone), per-category color overrides, brightness 0–8.
- WiFi transmit power autotune and an antenna check for flaky boards.
- Crash summary and a warning/error log kept in RAM for the status page.

## Build

Needs [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) 5.5 or later (built and tested
on 6.0.2).

Release images are signed. With the signing key (`c/signing-key.pem`, not in git):

```
cd c
idf.py build
```

Without it (anyone else, and CI) build an unsigned image:

```
cd c
idf.py -B build-ci -DSDKCONFIG=build-ci/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci" build
```

An unsigned image works, but takes no OTA updates (OTA trusts the key the running image was
signed with). For your own maps, make your own key, point `firmware_base_url` at your own
server, and sign with that.

Host unit tests (airport list and weather parsing, captive DNS, TX power autotune, radio
check): `make -C c/test`.

If `sdkconfig.defaults` changes, delete `sdkconfig` and rebuild.

## Flash

A new board, or one that ran firmware older than 0.5.0 (the partition table changed), needs
one USB flash. Hold BOOT while plugging in USB, then:

```
cd c
idf.py -p /dev/cu.usbmodemXXXX flash
```

or, to flash a released image and preload WiFi and the airport list in one step:

```
tools/provision.py --port /dev/cu.usbmodemXXXX --airports my-airports.txt --ssid "WiFi" --pass "..."
```

After that, updates arrive over WiFi.

## Maintainer tools (`c/tools/`)

Site-specific settings come from environment variables (`TMM_S3`, `AWS_PROFILE`, ...;
see `c/tools/tmm_env.py`).

| Tool | What |
|------|------|
| `provision.py` | flash a released or local image plus an NVS image with WiFi, airports and LED settings |
| `nvs_set.py` | change single NVS keys over USB, keeping the rest |
| `push_config.py` | write a map's remote config file (checked with the firmware's rules) |
| `promote.py` | copy the beta image to stable after checking version and signature |
| `ota_upload.sh` | build step: upload a changed image to the beta channel |
| `admin_phrase.py` | per-map admin phrase for changing the update URLs on the setup page |
| `radio_check.py` | run the antenna check and print the result |
| `serial_log.py` | capture the USB serial log |

## License

MIT, see `LICENSE`.
