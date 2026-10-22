# tedder-metar-map firmware (ESP-IDF)

Overview, build and flash steps: `../README.md`. Implementation notes: `NOTES.md`.

## Filesystem: FAT + wear levelling + TinyUSB MSC

The flash partition `storage` uses **subtype `fat`** with wear levelling. The firmware mounts it at **`/fatfs`** and can expose it to a host PC as a **USB mass-storage volume** via **TinyUSB MSC** (`espressif/esp_tinyusb`). The app and the USB host cannot use the volume at the same time: use the web page `/usb` to switch between **app mode** (firmware reads/writes config) and **USB disk mode** (PC sees the drive; edit `config.txt`, `airports.txt`, etc.).

If MSC is active or FAT mount fails, the firmware falls back to compile-time WiFi defaults (empty) and a small built-in airport list.

## METAR service

HTTPS GET to `fly.tedder.me/metar` with `ids` (comma-separated airports), `boardid` (12-char lowercase hex from the chip's optional unique ID, falling back to the WiFi STA MAC), `output=csv`, `debug` (flags), `crash` (reset reason), `fetches` and `fetch_fails`. Each CSV line is `ICAO,category,r-g-b,age_minutes,stale`.

## Features

- **WS2812** on GPIO 5 via `espressif/led_strip` (RMT)
- **SSD1306** 128×32 over I2C (SDA GPIO 7, SCL GPIO 6) via `esp_lcd` + small 5×7 font
- **Task watchdog** 60 minutes
- **HTTP** server on port 80: `/`, `/airports`, `/config`, `/usb`, `/debug`, `/log`, plus `POST /ota` and `POST /bootloader`
- **Logging**: syslog-style lines to console (`local0.<severity>: ...`); warnings and above also go to `errors.txt` on FAT
- **OTA** via `esp_https_ota` (`OTA_ENABLED` in `main/main.c`); statsbox POST is a stub (`STATSBOX_ENABLED 0`)

## TLS

`main/certs/AmazonRootCA1.pem` holds two certificates: Amazon Root CA 1 and the Amazon RSA 2048 M04 intermediate, because CloudFront does not send the intermediate in the handshake. If the distribution moves to a different intermediate, add it here.
