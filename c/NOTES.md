# firmware notes

## Build

- Target esp32s2 (QT Py ESP32-S2: 4MB flash, 2MB PSRAM, single core). ESP-IDF 5.5+, tested on 6.0.2.
- `PROJECT_VER` comes from `FIRMWARE_VERSION` in `main/main.c` (CMakeLists.txt reads it). OTA
  compares versions, so bump it for every release.
- Test images (CMake cache options, they stick in the build dir): `-DTMM_ROLLBACK_TEST=1` aborts
  at boot and must be rolled back (version `<v>-rbtest`); `-DTMM_VER_SUFFIX=-x`; `-DTMM_TX_DEFAULT=N`;
  `-DTMM_TX_TEST=1` short autotune timers. Use a separate `-B build-xxx` dir for each.
- With `TMM_S3` in the environment, each build uploads a changed image to `$TMM_S3/bin/beta.bin`
  (`tools/ota_upload.sh`, skipped for test images and with `-DTMM_NO_UPLOAD=1`).

## Partitions and boot

- `partitions.csv`: NVS 24 KB, two 1.875 MB OTA slots, otadata, 64 KB core dump. No factory app.
- Rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`): a new image is marked valid after its first
  good weather fetch; a crash or reset before that boots the previous slot, and the next boot
  sets debug flag `OTA_ROLLED_BACK`. Bootloader setting: a board flashed before 0.5.0 needs one
  USB flash to get it.
- Signed apps without hardware secure boot (RSA-3072, V2 scheme): nothing burned in eFuses, USB
  can always reflash. OTA accepts only images signed with the key of the running image (trust
  comes from the running app's own signature block). `signing-pub.pem` is the public half.

## OTA

- Channels `<base>/stable.bin` and `<base>/beta.bin`; `ota_channel` picks one. Checks every
  `ota_interval_minutes` (6 h): HEAD first and skip when the ETag is unchanged; otherwise read
  only the image header (partial download, 32 KB ranges) and stop when the version matches.
- Version differs (newer or older) → install, except a version that already rolled back on this
  map. Promotion is a copy beta → stable (`tools/promote.py`).

## Remote config

- `<config base>/<boardid>.json`, checked 15 s after boot and every `config_interval_minutes`
  (HEAD + ETag). Applied once per `rev`; the whole file is validated before anything changes.
  Same apply path as the setup page (`cfg_apply_obj`). Keys: `../docs/config-options.md`.

## NVS (namespace `tmm_cfg`)

`ssid`/`pass`, `ssid1..2`/`pass1..2`, `airports` (one token per line, dark LEDs as `OFF`),
`bri_idx`, `ota_ch`, `url_prefix`, `ota_int`, `cfg_int`, `cfg_rev`, `fw_base_url`,
`cfg_base_url`, `admin_hash`, settings from `k_settings` (u16), night dimming (`nd_*`, `tz`),
colors (`col_*`), `tx_auto` (learned TX level), `boots`, `crash`, `rb_seen`, bench-only
`test_hooks` / `fb_sec`.

## Board ID (`boardid`)

First 6 bytes of `ESP_EFUSE_OPTIONAL_UNIQUE_ID` as 12 lowercase hex chars; the WiFi STA MAC if
that is unreadable or zero. The setup network and hostname use the last 6 hex chars.

## LEDs

- `led_strip` over RMT. The S2 has 4 TX channels with 64 symbols each: the map strip takes two
  blocks (128 symbols), the onboard status pixel one. `led_order` maps to the driver's color
  component format; `RGBW`/`GRBW` send 4 bytes per LED with white 0.
- The strip is always `LED_MAX` (200) pixels long, so LEDs past the airport list are driven off.
  A frame is only sent when it changed, or once a second.
- Onboard NeoPixel: data GPIO 39, power GPIO 38 (driven high at boot).
- If the strip driver fails (bad `led_gpio`), debug flag `LED_INIT_FAIL` is set and the web
  pages still work, so the setting can be fixed.

## Tasks

`led` (render, 6 KB), `poll` (weather, 8 KB), `ota` (8 KB), `cfg` (8 KB), `wdt` (stale-weather
reboot, web server health), `btn` (BOOT button), `dns` (captive DNS in setup mode), `oled`
(only when an OLED answers). The event loop task stack is raised to 3.5 KB because the WiFi
handler logs and the `/log` hook copies warnings.

## Logging

- Console on the USB CDC port. `log_vprintf` passes everything through, and copies W/E lines
  into a 6 KB PSRAM ring served at `/log`. It looks at the format string first (log v1 sends one
  call per line, starting with the level letter) so only W/E lines are formatted twice.
- Core dumps go to the `coredump` partition; the next boot stores a one-line summary (task, PC,
  backtrace) in NVS `crash` and erases the dump. Decode the addresses with the ELF of that
  version (`TMM_ELF_ARCHIVE` keeps one per uploaded build).

## Web pages

`/` status, `/airports` list with Find buttons, `/config` setup page, `/debug` flags and crash,
`/log`. POST: `/save_wifi`, `/save_airports`, `/save_settings`, `/save_url`, `/save_channel`,
`/save_admin`, `/bri_step`, `/clear_flags`, `/pattern`, `/radio_check`, `/action`. GET
`/unlock` (lock state for the page). Bench units (`test_hooks=1`, USB only): `/test/button`,
`/test/config`, `/test/check`, `/test/weather`.

Saves need a same-site request (Origin/Referer naming this map; skipped in setup mode), and on
the home network a BOOT press within the last 15 minutes.

## OLED

SSD1306 128×32 on I2C, SDA GPIO 7, SCL GPIO 6 (`oled_ssd1306.h`), three text lines refreshed
every 2 s. Optional; probed at boot.
