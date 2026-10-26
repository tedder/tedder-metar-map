# Changelog

Firmware versions (`FIRMWARE_VERSION` in `c/main/main.c`). Newest first.

## 0.5.3 (2026-10-26)
- New settings `no_report_day_percent` (default 100) and `no_report_night_percent` (default 50):
  brightness of grey no-report airport LEDs as a percent of the map brightness, outside and during
  night dimming; 0 turns them off. Replaces 0.5.2's fixed half brightness at night. On the
  Configure page under Map behavior → "Airports with no report".
- Configure page Save buttons are green when saving is unlocked, and grey with a red outline while
  locked (was a faint fade).

## 0.5.2 (2026-10-22)
- No-report airports at half brightness during night dimming.
- Brightness scaling rounds, so dim greys stay grey.

## 0.5.1 (2026-10-22)
- Weather timeout back to 30 s (service cache misses for long lists can pass 15 s).
- Clearer crash summary; `promote.py` prints the signature check.

## 0.5.0 (2026-10-22)
- Per-map airport list, signed beta/stable OTA, remote config, setup network.
- Settings in NVS; no USB drive.
