# Config options

Firmware 0.5.0. Settings live in NVS (namespace `tmm_cfg`) on each map. Three ways to set them:

- **Setup page**: `http://metarmap.local/config` (or the map's IP), or join the setup network
  `metarmap-XXXXXX` (the page opens by itself). Saving on the home network needs a press of the
  BOOT button first (see [Saving and the BOOT button](#saving-and-the-boot-button)).
- **USB**: `c/tools/provision.py` writes an NVS image while flashing (replaces all settings);
  `c/tools/nvs_set.py` changes single keys.
- **Config file** (maintainer): a JSON file at `<config base>/<boardid>.json`, fetched about 15 s
  after boot and every `config_interval_minutes`. Later checks send a HEAD first and skip the
  download when the ETag hasn't changed. It is applied only when `rev` is higher than the last
  applied rev. The whole file is checked first: any bad value rejects the file (nothing changes)
  and sets debug flag `0x200`. `c/tools/push_config.py` builds, checks and uploads it.

Unknown keys are ignored (and logged), so a newer config file never breaks older firmware.

## Airport list

| Config key | NVS key | Default | Values |
|---|---|---|---|
| `airports` | `airports` | empty | list of strings, or one string; at most 200 entries |

One entry per LED, in strip order: LED 1 is the first LED after the board. An entry is an airport
code (3–4 letters/digits, e.g. `KPDX`, `S48`) or a dark LED: `OFF`, anything starting with `OFF`
(`OFF_LEGEND`), `NULL` or `-`. In a single string, entries are separated by newlines, commas or
spaces. The same airport may appear on more than one LED. LEDs past the end of the list stay off.

The setup page has the list as a text box, one entry per line, plus a **Find** button per LED that
blinks just that LED. Saving it asks the weather service once and lists any codes with no current
report, so typos show up right away.

## Settings

| Config key | NVS key | Default | Range / values | Notes |
|---|---|---|---|---|
| `brightness` | `bri_idx` (u8) | `4` (25 %) | 0–8 | 0 off; 1–7 = 3, 7, 15, 25, 40, 55, 75 %; 8 full. Also −/+ on the status page |
| `ota_channel` | `ota_ch` | `stable` | `stable`, `beta` | which firmware file to follow |
| `metar_url_prefix` | `url_prefix` | `https://fly.tedder.me/metar?ids=` | `http(s)://…`, < 128 chars | weather service; the airport list is appended |
| `ota_interval_minutes` | `ota_int` | `360` (6 h) | 15–1440 | firmware update check interval |
| `config_interval_minutes` | `cfg_int` | `60` | 5–1440 | config check interval; takes effect after the current wait |
| `wifi_add` | `ssid1`/`pass1`, `ssid2`/`pass2` | none | list of `{"ssid","pass"}`, ≤ 2 | adds backup networks; never touches the first network or the one in use |
| `wifi_remove` | same | — | list of SSIDs | removes backup networks; never the one in use |

The first WiFi network (`ssid`/`pass`) is set on the setup page or with `provision.py --ssid/--pass`.
The map stays on the network it joined; after 3 failed reconnects it moves to the next saved one.

### Behavior settings

Each is optional; `null` goes back to the default (and removes it from NVS). NVS values are u16.

| Config key | NVS key | Default | Range / values | What it does |
|---|---|---|---|---|
| `metar_poll_seconds` | `poll_sec` | `300` | 60–3600 | weather check interval (the service caches about 5 min) |
| `wifi_fallback_minutes` | `fb_min` | `5` | 1–120 | minutes without WiFi before the fallback setup network opens |
| `metar_stale_minutes` | `stale_min` | `60` | 10–1440 | minutes without fresh weather before every airport LED shows the "no report" color |
| `metar_stale_reboot_hours` | `stale_rb_hr` | `12` | 1–48 | hours without fresh weather before a self-reboot (not with an empty airport list) |
| `status_pixel` | `status_pix` | `true` | `true`/`false` | onboard status light (setup, unlock and button colors show either way) |
| `verbose_telemetry` | `verbose_tel` | `true` | `true`/`false` | `false` leaves `t=` and `localip=` out of weather requests |
| `wifi_tx_power_dbm` | `tx_dbm` | `17` | 8–17 | WiFi transmit power cap. Unset, the map steps down (3 dB per step, floor 8) after strong-signal join timeouts and remembers the level that worked; setting it turns that off |

### LED hardware (applied after a reboot)

The map saves these and reboots itself after applying a config file. On the setup page they are
under "LED string". Over USB: `provision.py --led-order GRB --led-model sk6812 --led-gpio 5`.

| Config key | NVS key | Default | Values |
|---|---|---|---|
| `led_order` | `led_order` (index) | `RGB` | `RGB`, `GRB`, `BRG`, `BGR`, `RBG`, `GBR`, `RGBW`, `GRBW` |
| `led_model` | `led_model` (index) | `ws2812` | `ws2812`, `sk6812`, `ws2811` |
| `led_gpio` | `led_gpio` | `5` | 0–46 (QT Py ESP32-S2 pin A3 / GPIO 5 by default) |

`RGBW`/`GRBW` are 4-channel strips; the white channel is left off. Check a new strip with the LED
test (below): **Colors** must show red, green, blue, white in that order (wrong colors = wrong
`led_order`); **Order** steps one white LED from LED 1 to the end of the list.

### Night dimming

```json
"night_dim": {"start": "22:00", "end": "07:00", "brightness": 1, "tz": "PST8PDT,M3.2.0,M11.1.0"}
```

- Between `start` and `end` (local time, may cross midnight) the brightness is `brightness`
  (0–8; 0 = off). `false` turns night dimming off; `null` goes back to the default.
- Default: on, 22:00–07:00, brightness 1 (3 %), Pacific time.
- `tz` is a POSIX TZ string (no spaces, under 48 chars). Pacific `PST8PDT,M3.2.0,M11.1.0`,
  Mountain `MST7MDT,M3.2.0,M11.1.0`, Arizona `MST7`, Central `CST6CDT,M3.2.0,M11.1.0`, Eastern
  `EST5EDT,M3.2.0,M11.1.0`, UTC `UTC0`.
- Time comes from `pool.ntp.org`. Until the clock is set, the map stays at normal brightness.
- NVS: `nd_start`, `nd_end` (minutes), `nd_bri`, `tz`; `nd_off` = 1 when turned off.

### Colors

```json
"colors": {"VFR": "00ff40", "LIFR": null}
```

Overrides the service's color for a flight category (`VFR`, `MVFR`, `IFR`, `LIFR`) with `rrggbb`.
`null` for one category, or `"colors": null` for all, goes back to the service colors (VFR
`007f00`, MVFR `00007f`, IFR `7f0000`, LIFR `ff16c7`). An airport with no current report shows a
dim grey (`4b4b32`, from the service). NVS: `col_vfr` … `col_lifr` (u32).

## Forced replacements

These need `"force": true` at the top level of the file; without it the whole file is rejected.
URLs must be `https://`. On the setup page, the two base URLs need the map's four-word admin
phrase (set over USB with `provision.py --admin-phrase`); 5 wrong tries lock it for 10 minutes.

| Config key | NVS key | Default | Values | Notes |
|---|---|---|---|---|
| `wifi_primary` | `ssid`/`pass` | (setup page) | `{"ssid": "…", "pass": "…"}` | replaces the first network; the old one is kept as a backup if a slot is free |
| `firmware_base_url` | `fw_base_url` | `https://tedder.me/esp32/tedder-metar-map/bin` | `https://…`, < 128 chars; `""` = default | firmware files `<base>/stable.bin`, `<base>/beta.bin` |
| `config_base_url` | `cfg_base_url` | `https://tedder.me/esp32/tedder-metar-map/cfg` | `https://…`, < 128 chars; `""` = default | config file `<base>/<boardid>.json` |

A wrong base URL can cut a map off from updates or config. Recovery is the setup page or USB.

## One-time actions

Run once per `rev`, after settings are saved.

| Config key | Value | Effect |
|---|---|---|
| `clear_flags` | `true` | clear debug flags |
| `ota_now` | `true` | check for a firmware update now |
| `reboot` | `true` | reboot (the rev is saved first, so it doesn't loop) |

## Example file

```json
{
  "rev": 4,
  "airports": ["KPDX", "KTTD", "OFF", "KHIO", "S48"],
  "led_order": "GRB",
  "ota_channel": "beta",
  "wifi_add": [{"ssid": "Guest", "pass": "example-pass"}],
  "clear_flags": true
}
```

Each file holds only that revision's changes: a setting changed on the map's own page stays until
a later file changes that same setting. Files are limited to 4 KB.

```bash
cd c
tools/push_config.py 0123456789ab brightness=3 'colors={"VFR":"00ff40"}'
tools/push_config.py 0123456789ab airports=@my-airports.txt
tools/push_config.py 0123456789ab --show        # current file and the last telemetry
```

## Saving and the BOOT button

Saves from the status and setup pages must come from the page itself (same-site check). On the
home network, saving settings, WiFi or the airport list also needs a short press of the BOOT
button first: one press unlocks saving for 15 minutes (the status light turns cyan). In the setup
network that you opened by hand (9 s hold), no press is needed. The LED test patterns and the
brightness −/+ buttons never need a press.

| BOOT button | Effect |
|---|---|
| short press (< 9 s) | unlock saving for 15 minutes; the status light flashes blue to acknowledge |
| hold 9 s | open the setup network `metarmap-XXXXXX`; in setup mode, reboot instead |

## LED test patterns

`POST /pattern` with `mode=colors|order|categories|one|off`, optional `seconds=5..300` (default
30) and, for `one`, `led=N`. Or the buttons on the status page and **Find** on the airport page.

| Mode | Shows |
|---|---|
| `colors` | all LEDs red, green, blue, white, 1.5 s each |
| `order` | one white LED stepping from LED 1 to the last, 0.5 s each |
| `categories` | VFR, MVFR, IFR, LIFR colors (with overrides), 2 s each |
| `one` | LED N blinking white, the rest off |
| `off` | stop |

## What the LEDs and status light show

| Map LEDs | Meaning |
|---|---|
| first LEDs white, one more per step | starting up: WiFi, address, first weather request |
| airport colors | normal |
| all airport LEDs dim grey | weather older than `metar_stale_minutes` |
| all blue, LED 1 red/green | setup network open (LED 1 green once a phone has joined) |
| all blue, blinking | saved WiFi unreachable; setup network open, still retrying |
| startup LEDs amber or red | WiFi join problem (amber: the access point doesn't answer, blinking once nothing is left to try; red: wrong password) |

| Status light (on the board) | Meaning |
|---|---|
| slow green breathing | all good |
| amber breathing | weather is old |
| amber blinking | connecting / no WiFi |
| blue | setup network open (blinking: fallback) |
| cyan | saving unlocked (BOOT button) |
| blue flash | button press seen |

## Radio check (antenna test)

For new boards and antenna swaps. `POST /radio_check` (same-site check, no button press) with
optional `sink=http://<LAN IPv4>[:port]`; `GET /radio_check` returns JSON (`state`, `result`
pass/warn/fail/unsure, `why`, `rows`). On its current network the map steps TX power through 17,
11 and 8 dBm, twice; each step pings the gateway 20 times and, with a sink, uploads 256 KB. It
takes about a minute. **Fail**: the link drops, more than 10 % of pings are lost at 11 or 8 dBm,
or 11 is clearly worse than 8. **Warn**: only 17 dBm has problems. **Unsure**: uploads are under
50 kB/s everywhere (too far from the access point). `c/tools/radio_check.py <map address>` runs
it and prints the result.

## Debug flags

Sent as `debug=0x…` with every weather request and shown on `http://<map>/debug`. The weather
and update flags clear on the next success of the same kind; the others clear 24 h after they
were last set, or with `clear_flags` / the **Clear** button.

| Bit | Name | Set when |
|---|---|---|
| `0x01` | HTTP_TIMEOUT | weather request failed (timeout or connection) |
| `0x02` | HTTP_NON_200 | weather service answered with an error |
| `0x04` | PARSE_ERROR | weather answer could not be read |
| `0x08` | WIFI_DISCONNECT | WiFi dropped |
| `0x10` | OTA_FAILED | firmware update failed (any reason) |
| `0x20` | WDT_FIRED | last reboot was a crash or watchdog |
| `0x40` | HTTPD_START_FAIL | local web server didn't start |
| `0x80` | HTTPD_DEAD | local web server stopped responding |
| `0x100` | STALE_FETCH_REBOOT | rebooted after `metar_stale_reboot_hours` without weather |
| `0x200` | CFG_REJECTED | config file rejected |
| `0x400` | OTA_BAD_IMAGE | update downloaded but not signed with the project key, or corrupt |
| `0x800` | OTA_DOWNLOAD | update could not be downloaded or written |
| `0x1000` | OTA_ROLLED_BACK | a new update failed to start; went back to the old version |
| `0x2000` | CFG_FETCH_FAIL | config file could not be fetched (a missing file is not an error) |
| `0x4000` | LED_INIT_FAIL | the LED strip driver didn't start (check `led_gpio`) |

After a crash, the map keeps a one-line summary (task, address, backtrace) and shows it on
`/debug`. `/log` lists warnings and errors since boot.

## What the map reports back

Each weather request carries `boardid`, `fw`, `ch` (OTA channel), `debug`, and with
`verbose_telemetry` also `localip` and `t=` (dot-separated base36, format version 1):

`1.rssi.free_heap_kb.min_free_heap_kb.boots.uptime_min.reset_reason.wifi_drops.fetch_fails.config_rev.config_status.tx_dbm.tx_cap.tx_fixed.tx_downs.tx_ups.tx_up_fails.last_disc_reason.wifi_slot.fetch_ms.wifi_ch.leds.reports`

- `rssi`: signal as a positive number (`48` = −48 dBm; 0 = not connected).
- `tx_dbm`: transmit power in use; `tx_cap`: the `wifi_tx_power_dbm` setting; `tx_fixed`: 1 when
  set by hand. `tx_downs`, `tx_ups`, `tx_up_fails`: power step-downs and step-up tests since boot.
- `last_disc_reason`: last WiFi disconnect reason (0 none; 2 the access point didn't answer, 201
  network not found, 15/202 wrong password).
- `wifi_slot`: saved network in use (0 first, 1–2 backups). `fetch_ms`: duration of the previous
  weather request. `wifi_ch`: the access point's channel.
- `leds`: entries in the airport list; `reports`: airports with a current report in the last reply.

| `config_status` | Meaning |
|---|---|
| 0 | not checked yet |
| 1 | no config file (normal) |
| 2 | current: this rev was applied, or is already applied |
| 3 | rejected (status page "Remote config" line; debug flag `0x200`) |
| 4 | fetch error |
