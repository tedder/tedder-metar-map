# firmware notes

## Build

- Target: `idf.py set-target esp32s2` (default for this tree). ESP-IDF 5.5+.
- Source ESP-IDF: `source $IDF_PATH/export.sh` then `idf.py build`.
- `tmm_log.h` includes `<stdbool.h>` — required for `bool` in `tmm_log()` under `-std=gnu17`.

## Flash / partitions

- `sdkconfig.defaults`: 4MB flash; custom `partitions.csv` with `factory` ~1MB and `storage` FAT 512K.
- Recent builds: app image is tight against the factory partition (~few % free). Before adding large features, enlarge `factory` in `partitions.csv` or trim code.

## Config and USB

- FAT mount point: `/fatfs` (must match `CONFIG_TINYUSB_MSC_MOUNT_PATH` and code).
- **TinyUSB MSC** exposes the same wear-levelled FAT to a PC. Firmware and USB host cannot use the volume simultaneously; when in USB mode, file-backed paths are skipped where `tmm_msc_is_usb_mode()` is checked.
- Config files: `config.txt`, `airports.txt`, optional `errors.txt` on FAT.

## Board ID (`boardid`)

- Primary: first 6 bytes of `ESP_EFUSE_OPTIONAL_UNIQUE_ID` as 12 lowercase hex chars.
- Fallback: WiFi STA MAC if eFuse read fails or is all zeros.

## Logging

- Console lines use syslog-style prefix: `local0.<severity>:` (see `tmm_log.c`).
- Optional append to `errors.txt` for selected severities when FAT is mounted and not in USB mode.

## Display

- SSD1306 128×32, I2C: SDA GPIO7, SCL GPIO6 (`oled_ssd1306.h`). Verify on hardware if the panel is garbled (bit order / page layout).

## Web UI (minimal)

- Port 80 after WiFi: `/`, `/cfg` (SSID, password, airports), `/usb` (switch MSC app vs USB), `/debug`.

## ESP32-S3 instead of S2

Same codebase; no S2-only C defines in this project.

1. `idf.py set-target esp32s3` and rebuild.
2. **Pins:** NeoPixel is `LED_STRIP_GPIO` in `main.c` (17 on QT Py S2). OLED pins are `OLED_PIN_SDA` / `OLED_PIN_SCL` in `oled_ssd1306.h`. **Update both to match your S3 board** — Adafruit QT Py ESP32-S3 (and other boards) often differ from S2.
3. **Flash size:** Many S3 modules are 8MB+. Set `CONFIG_ESPTOOLPY_FLASHSIZE_*` appropriately; adjust `partitions.csv` if you need a larger app or storage partition.
4. TinyUSB MSC, efuse optional unique ID, and the rest apply to S3 the same way.

## Optional follow-ups

- URL-decode POST bodies for `/cfg` if passwords or airport lists contain `+` or `%xx`.
- Re-run `idf.py build` after target changes so `dependencies.lock` matches the active chip.
