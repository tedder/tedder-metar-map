#!/usr/bin/env python3
"""Flash + provision a map over USB, no setup network needed.

Flashes an already-built firmware (default: the released bin for --ota-channel, downloaded and
signature-checked; nothing is built) plus an NVS partition with the map's settings (namespace
tmm_cfg, the keys the firmware reads at boot). WiFi is NOT tested here, so this works for a
network that isn't in range (a map going to another house).

Examples (run from c/):
  tools/provision.py --port /dev/cu.usbmodem101 --airports airports.txt --ssid "Their WiFi" --pass secret --tz eastern
  tools/provision.py --port /dev/cu.usbmodem101 --airports airports.txt --led-order GRB --firmware local
  tools/provision.py --dry-run --airports airports.txt --ssid Test --pass 12345678   # build the image only
  tools/provision.py --port ... --airports airports.txt --nvs-only                   # keep the current app

The airport file has one entry per LED in strip order (see airports.txt.example); OFF, NULL or -
for a dark LED. Writing NVS replaces ALL stored settings on the map (boot counter, config rev,
brightness...). The first flash of a board that ran firmware older than 0.5.0 must be a full
flash (not --nvs-only): the partition table changed.
Bootloader mode if auto-reset doesn't work: hold BOOT, tap RESET.
"""
import argparse
import csv
import os
import re
import shlex
import sys
import tempfile

import tmm_env as E

TZ_NAMES = {"pacific": "PST8PDT,M3.2.0,M11.1.0", "mountain": "MST7MDT,M3.2.0,M11.1.0", "arizona": "MST7",
            "central": "CST6CDT,M3.2.0,M11.1.0", "eastern": "EST5EDT,M3.2.0,M11.1.0",
            "alaska": "AKST9AKDT,M3.2.0,M11.1.0", "hawaii": "HST10", "utc": "UTC0"}
LED_MAX = 200


def partition(name):
    """(offset, size) of a partition from partitions.csv."""
    with open(os.path.join(E.PROJ, "partitions.csv")) as f:
        for row in csv.reader(line for line in f if not line.lstrip().startswith("#")):
            row = [c.strip() for c in row]
            if row and row[0] == name:
                return int(row[3], 0), int(row[4], 0)
    sys.exit(f"partition {name!r} not found in partitions.csv")


def flash_layout():
    """(esptool options, [(offset, path)] for everything but the app, app offset) from build/flash_args."""
    path = os.path.join(E.BUILD, "flash_args")
    if not os.path.exists(path):
        sys.exit("no build/flash_args: build once (idf.py build) for the bootloader and partition table")
    lines = open(path).read().split("\n")
    opts, parts, app_off = lines[0].strip(), [], None
    for line in lines[1:]:
        if not line.strip():
            continue
        off, p = line.split(None, 1)
        if p.strip() == f"{E.APP_NAME}.bin":
            app_off = int(off, 0)
        else:
            parts.append((int(off, 0), os.path.join(E.BUILD, p.strip())))
    return opts, parts, app_off


def firmware_bin(which, tmp):
    """Path of the app image to flash; prints its version. Released bins must carry our signature."""
    import promote
    if which in ("stable", "beta"):
        path = os.path.join(tmp, f"{which}.bin")
        if not promote.fetch(which, path):
            sys.exit(f"could not download bin/{which}.bin")
        ok, msg = promote.signed_ok(path)
        if not ok:
            sys.exit(f"bin/{which}.bin signature check failed: {msg[0]}")
    else:
        path = os.path.join(E.BUILD, f"{E.APP_NAME}.bin") if which == "local" else which
        if not os.path.exists(path):
            sys.exit(f"firmware {path} not found")
    print(f"firmware: {which} -> {path}, version {promote.app_version(path)}")
    return path


def read_airports(path):
    """Airport file -> canonical NVS text (one entry per line, dark LEDs as OFF), same rules as
    the firmware's airports_parse."""
    toks = re.split(r"[\s,]+", open(path).read().upper())
    out = []
    for t in (t for t in toks if t):
        if t.startswith("OFF") or t in ("NULL", "-"):
            out.append("OFF")
        elif re.fullmatch(r"[A-Z0-9]{3,4}", t):
            out.append(t)
        else:
            sys.exit(f"{path}: LED {len(out) + 1}: {t!r} is not an airport code or OFF")
    if len(out) > LED_MAX:
        sys.exit(f"{path}: {len(out)} entries, the firmware takes at most {LED_MAX}")
    return "\n".join(out), len(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port, e.g. /dev/cu.usbmodem101")
    ap.add_argument("--airports", required=True, metavar="FILE", help="airport list, one entry per LED")
    ap.add_argument("--ssid", help="primary WiFi (slot 0); omit to ship without WiFi (setup network on first boot)")
    ap.add_argument("--pass", dest="password", default="", help="primary WiFi password")
    ap.add_argument("--extra", action="append", default=[], metavar="SSID:PASS",
                    help="additional network (slots 1-2), may repeat")
    ap.add_argument("--ota-channel", choices=["stable", "beta"], default="stable")
    ap.add_argument("--firmware", help="stable, beta (released bin from S3, signature checked), local "
                    f"(build/{E.APP_NAME}.bin) or a path; default: the --ota-channel bin. Nothing is built.")
    ap.add_argument("--brightness", type=int, default=4, help="0-8 (0 = off, 8 = full; default 4)")
    ap.add_argument("--tz", help="night dimming time zone: " + ", ".join(TZ_NAMES) +
                    " or a POSIX TZ string (default Pacific)")
    ap.add_argument("--metar-url-prefix", help="weather URL prefix (default: firmware default)")
    ap.add_argument("--led-order", choices=E.LED_ORDERS, help="color order on the wire (default RGB)")
    ap.add_argument("--led-model", choices=E.LED_MODELS, help="LED chip (default ws2812)")
    ap.add_argument("--led-gpio", type=int, help="LED data pin (default 5)")
    ap.add_argument("--tx-power", type=int, help="WiFi TX power cap in dBm, 8-17 (default: automatic)")
    ap.add_argument("--test-hooks", action="store_true", help="bench unit: enable /test/ endpoints")
    ap.add_argument("--fallback-seconds", type=int,
                    help="with --test-hooks: open the fallback setup network after N s (30-3600)")
    ap.add_argument("--admin-phrase", action="store_true",
                    help="store this board's admin phrase hash (tools/admin_phrase.py)")
    ap.add_argument("--boardid", help="board ID for --admin-phrase with --dry-run (otherwise read over USB)")
    ap.add_argument("--nvs-only", action="store_true", help="write settings only, keep the current firmware")
    ap.add_argument("--dry-run", action="store_true", help="build the NVS image and stop")
    a = ap.parse_args()

    airports, n_leds = read_airports(a.airports)
    if a.ssid is not None and not (1 <= len(a.ssid.encode()) <= 32):
        sys.exit("ssid must be 1-32 bytes")
    if a.password and not (8 <= len(a.password) <= 63):
        sys.exit("WPA password must be 8-63 characters (or empty for an open network)")
    if not 0 <= a.brightness <= 8:
        sys.exit("--brightness must be 0-8")
    if len(a.extra) > 2:
        sys.exit("at most 2 --extra networks")
    if a.fallback_seconds is not None and not (a.test_hooks and 30 <= a.fallback_seconds <= 3600):
        sys.exit("--fallback-seconds needs --test-hooks and 30-3600")
    if a.led_gpio is not None and not 0 <= a.led_gpio <= 46:
        sys.exit("--led-gpio must be 0-46")
    if a.tx_power is not None and not 8 <= a.tx_power <= 17:
        sys.exit("--tx-power must be 8-17")
    if not a.dry_run and not a.port:
        sys.exit("--port is required unless --dry-run")

    rows = [("airports", "string", airports), ("bri_idx", "u8", str(a.brightness))]
    if a.ssid:
        rows += [("ssid", "string", a.ssid), ("pass", "string", a.password)]
    for i, e in enumerate(a.extra, start=1):
        ss, _, pw = e.partition(":")
        rows += [(f"ssid{i}", "string", ss), (f"pass{i}", "string", pw)]
    if a.ota_channel == "beta":
        rows.append(("ota_ch", "string", "beta"))
    if a.tz:
        tz = TZ_NAMES.get(a.tz.lower(), a.tz)
        if " " in tz or len(tz) >= 48:
            sys.exit("--tz: a name above or a POSIX TZ string without spaces, under 48 chars")
        rows.append(("tz", "string", tz))
    if a.metar_url_prefix:
        rows.append(("url_prefix", "string", a.metar_url_prefix))
    if a.test_hooks:
        rows.append(("test_hooks", "u8", "1"))
    if a.fallback_seconds is not None:
        rows.append(("fb_sec", "u16", str(a.fallback_seconds)))
    if a.tx_power is not None:
        rows.append(("tx_dbm", "u16", str(a.tx_power)))
    if a.led_order:
        rows.append(("led_order", "u16", str(E.LED_ORDERS.index(a.led_order))))
    if a.led_model:
        rows.append(("led_model", "u16", str(E.LED_MODELS.index(a.led_model))))
    if a.led_gpio is not None:
        rows.append(("led_gpio", "u16", str(a.led_gpio)))

    boardid = a.boardid
    if a.port and not boardid:
        boardid = E.read_boardid(a.port)
    if a.admin_phrase:
        import admin_phrase
        if not boardid:
            sys.exit("--admin-phrase needs --port (or --boardid with --dry-run)")
        rows.append(("admin_hash", "string",
                     admin_phrase.nvs_hash(boardid, admin_phrase.phrase(boardid, admin_phrase.secret()))))
        print(f"admin phrase hash for {boardid} added (phrase: tools/admin_phrase.py {boardid})")

    app = None
    if not a.nvs_only and not a.dry_run:
        opts, boot_parts, app_off = flash_layout()
        app = firmware_bin(a.firmware or a.ota_channel, tempfile.mkdtemp(prefix="tmm-fw-"))
    nvs_off, nvs_size = partition("nvs")
    # The image holds the WiFi password: keep it in a private temp dir, deleted afterwards.
    with tempfile.TemporaryDirectory(prefix="tmm-provision-") as tmp:
        csv_path = os.path.join(tmp, "nvs.csv")
        bin_path = os.path.join(tmp, "nvs.bin")
        with open(csv_path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["key", "type", "encoding", "value"])
            w.writerow([E.NVS_NAMESPACE, "namespace", "", ""])
            for k, enc, v in rows:
                w.writerow([k, "data", enc, v])
        E.idf_run(f"python -m esp_idf_nvs_partition_gen generate {shlex.quote(csv_path)} "
                  f"{shlex.quote(bin_path)} {hex(nvs_size)}")
        print(f"NVS image ({hex(nvs_size)} bytes @ {hex(nvs_off)}):")
        for k, _, v in rows:
            if k == "airports":
                v = f"{n_leds} LEDs: " + " ".join(airports.split("\n")[:8]) + (" ..." if n_leds > 8 else "")
            elif k.startswith("pass") and v:
                v = "********"
            print(f"  {k:10} = {v}")
        if a.dry_run:
            return

        port = shlex.quote(a.port)
        print(f"boardid {boardid}  (setup network metarmap-{boardid[6:]})")
        parts = [(nvs_off, bin_path)]
        if a.nvs_only:
            opts = "--flash-size keep"
        else:
            parts = boot_parts + [(app_off, app)] + parts
        args = " ".join(f"{hex(o)} {shlex.quote(p)}" for o, p in parts)
        E.idf_run(f"python -m esptool --port {port} write-flash {opts} {args}", capture=False)

    print(f"\nDone. fleet.md row:\n| ? | `{boardid}` | <where> | {n_leds} LEDs | {a.ota_channel} | "
          f"<fw> | provisioned {'(WiFi ' + a.ssid + ')' if a.ssid else '(no WiFi)'} |")


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
