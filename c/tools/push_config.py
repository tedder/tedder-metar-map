#!/usr/bin/env python3
"""Change a map's remote config file ($TMM_S3/cfg/<boardid>.json).

Each file holds only this revision's changes (plus "rev"); nothing is carried over from the
previous file, so a setting changed on the map's own page stays until a later file changes
that same setting. rev goes past both the old file and the unit's last reported rev (CloudFront
logs, or its status page when reachable). Values are checked with the firmware's rules first.

  tools/push_config.py 1 --show                         # current file + latest telemetry
  tools/push_config.py 1 brightness=3 led_order=GRB
  tools/push_config.py 1 airports=@airports.txt                 # @file: the list from a file
  tools/push_config.py 2 'night_dim={"start":"22:00","end":"07:00","brightness":2,"tz":"EST5EDT,M3.2.0,M11.1.0"}'
  tools/push_config.py 0123456789ab metar_poll_seconds=null     # null = back to the default
  tools/push_config.py 1 ota_now=true --yes

Values are JSON when they parse as JSON (numbers, true/false, null, objects), else strings.
The unit is a fleet.md unit number (TMM_FLEET) or a 12-hex boardid. Key list:
docs/config-options.md. Needs TMM_S3 and AWS credentials; TMM_LOG_S3 for the last-poll lookup.
"""
import argparse
import glob
import json
import os
import re
import sys
import tempfile
import time

import tmm_env as E
from tmm_env import aws, resolve

LOG_DIR = os.path.expanduser("~/tmp/tmm-logs/weather")
MAX_BYTES = 4096
LED_MAX = 200

FORCED = {"wifi_primary", "firmware_base_url", "config_base_url"}

# Same ranges as the firmware (cfg_apply_text, k_settings). (kind, lo, hi) or (kind, options).
INT = {"brightness": (0, 8), "ota_interval_minutes": (15, 1440), "config_interval_minutes": (5, 1440),
       "metar_poll_seconds": (60, 3600), "wifi_fallback_minutes": (1, 120), "metar_stale_minutes": (10, 1440),
       "metar_stale_reboot_hours": (1, 48), "wifi_tx_power_dbm": (8, 17), "led_gpio": (0, 46)}
BOOL = {"status_pixel", "verbose_telemetry", "clear_flags", "ota_now", "reboot", "force"}
ENUM = {"led_order": E.LED_ORDERS, "led_model": E.LED_MODELS, "ota_channel": ["stable", "beta", ""]}
RESETTABLE = set(INT) - {"brightness", "ota_interval_minutes", "config_interval_minutes"} | \
    {"status_pixel", "verbose_telemetry"} | set(ENUM) - {"ota_channel"}
OTHER = {"airports", "metar_url_prefix", "wifi_add", "wifi_remove", "wifi_primary", "firmware_base_url",
         "config_base_url", "night_dim", "colors"}
CATS = {"VFR", "MVFR", "IFR", "LIFR"}


def cfg_url(board):
    return f"{E.s3_base()}/cfg/{board}.json"


def read_file(board):
    r = aws(["s3", "cp", cfg_url(board), "-"], check=False)
    if r.returncode:
        return {}
    return json.loads(r.stdout or "{}")


def latest_telemetry(board, hours=2):
    """Newest weather poll for this board in the last `hours` of CloudFront logs: (time, query)."""
    log_s3 = os.environ.get("TMM_LOG_S3")
    if not log_s3:
        return None
    try:
        import pyarrow.parquet as pq
    except ImportError:
        print(f"NOTE: no pyarrow in {sys.executable}; logs not read (use /opt/homebrew/bin/python3)")
        return None
    os.makedirs(LOG_DIR, exist_ok=True)
    now = time.time()
    for h in range(hours + 1):
        stamp = time.strftime("%Y-%m-%d-%H", time.gmtime(now - h * 3600))
        aws(["s3", "cp", log_s3, LOG_DIR, "--recursive", "--exclude", "*", "--include", f"*.{stamp}.*",
             "--only-show-errors"], check=False)
    best = None
    for f in sorted(glob.glob(os.path.join(LOG_DIR, "*.parquet")))[-(hours + 1) * 12:]:
        t = pq.read_table(f, columns=["date", "time", "cs_uri_query"]).to_pylist()
        for row in t:
            q = row["cs_uri_query"] or ""
            if f"boardid={board}" in q:
                ts = f"{row['date']} {row['time']}"
                if not best or ts > best[0]:
                    best = (ts, q)
    return best


def lan_rev(q):
    """cfg rev from the map's status page when it is reachable here (localip= in its last poll).
    The logs lag ~5 min, and a bench unit's test hook can move the rev without the file."""
    m = re.search(r"localip=([0-9.]+)", q or "")
    if not m:
        return None
    try:
        import urllib.request
        with urllib.request.urlopen(f"http://{m.group(1)}/", timeout=3) as r:
            page = r.read().decode("utf-8", "replace")
        if "metarmap-" not in page:
            return None
        rv = re.search(r"config rev (\d+)", page)
        return int(rv.group(1)) if rv else None
    except OSError:
        return None


def decode_t(q):
    m = re.search(r"&t=([0-9a-z.]+)", q)
    if not m:
        return {}
    names = ["ver", "rssi", "heap_kb", "min_heap_kb", "boots", "uptime_min", "reset_reason",
             "wifi_drops", "fetch_fails", "cfg_rev", "cfg_status", "tx_dbm", "tx_cap", "tx_fixed",
             "tx_downs", "tx_ups", "tx_up_fails", "last_disc_reason", "wifi_slot", "fetch_ms",
             "wifi_ch", "leds", "reports"]
    return dict(zip(names, (int(x, 36) for x in m.group(1).split("."))))


def check(cfg, warn):
    """Problems with a config dict, as the firmware would see them ([] = fine). Unknown keys
    (the firmware logs and ignores them) go to `warn`."""
    bad = []
    for k, v in cfg.items():
        if k == "rev":
            continue
        if v is None:
            if k not in RESETTABLE:
                bad.append(f"{k}: null only resets settings")
        elif k in INT:
            lo, hi = INT[k]
            if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
                bad.append(f"{k}: integer {lo}..{hi}")
        elif k in BOOL:
            if not isinstance(v, bool):
                bad.append(f"{k}: true or false")
        elif k in ENUM:
            if not isinstance(v, str) or v.lower() not in [o.lower() for o in ENUM[k]]:
                bad.append(f"{k}: one of {ENUM[k]}")
        elif k == "airports":
            toks = v if isinstance(v, list) else re.split(r"[\s,]+", v) if isinstance(v, str) else None
            toks = [t for t in toks if isinstance(t, str) and t.strip()] if toks is not None else None
            if toks is None or (isinstance(v, list) and len(toks) != len(v)):
                bad.append("airports: list of strings or one string")
            elif len(toks) > LED_MAX:
                bad.append(f"airports: {len(toks)} entries, at most {LED_MAX}")
            else:
                for i, t in enumerate(toks):
                    t = t.strip().upper()
                    if not (t.startswith("OFF") or t in ("NULL", "-") or re.fullmatch(r"[A-Z0-9]{3,4}", t)):
                        bad.append(f"airports: LED {i + 1}: {t!r} is not an airport code or OFF")
                        break
        elif k == "metar_url_prefix":
            if not isinstance(v, str) or not re.match(r"https?://", v) or len(v) > 127:
                bad.append("metar_url_prefix: http(s) URL under 128 chars")
        elif k in ("firmware_base_url", "config_base_url"):
            if not isinstance(v, str) or (v and not v.startswith("https://")) or len(v) > 127 or " " in v:
                bad.append(f"{k}: https URL or empty")
        elif k in ("wifi_add", "wifi_primary"):
            nets = v if k == "wifi_add" else [v]
            if not isinstance(nets, list) or len(nets) > 2:
                bad.append(f"{k}: list of at most 2 networks")
                continue
            for n in nets:
                pw = n.get("pass", "") if isinstance(n, dict) else None
                if not isinstance(n, dict) or not isinstance(n.get("ssid"), str) or not 1 <= len(n["ssid"]) <= 32 \
                        or not isinstance(pw, str) or not (pw == "" or 8 <= len(pw) <= 63):
                    bad.append(f"{k}: each needs ssid (1-32) and pass (empty or 8-63)")
        elif k == "wifi_remove":
            if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
                bad.append("wifi_remove: list of SSIDs")
        elif k == "night_dim":
            if v is False:
                continue
            ok = isinstance(v, dict) and all(isinstance(v.get(x), str) for x in ("start", "end"))
            hm = [re.fullmatch(r"(\d{1,2}):(\d{2})", v[x]) if ok else None for x in ("start", "end")]
            ok = ok and all(m and int(m.group(1)) < 24 and int(m.group(2)) < 60 for m in hm) \
                and v["start"] != v["end"] and isinstance(v.get("brightness"), int) and 0 <= v["brightness"] <= 8 \
                and isinstance(v.get("tz"), str) and 0 < len(v["tz"]) < 48 and " " not in v["tz"]
            if not ok:
                bad.append('night_dim: false or {"start":"HH:MM","end":"HH:MM","brightness":0-8,"tz":"POSIX TZ"}')
        elif k == "colors":
            if v is False:
                continue
            if not isinstance(v, dict) or any(c.upper() not in CATS or not (x is None or (
                    isinstance(x, str) and re.fullmatch(r"[0-9a-fA-F]{6}", x))) for c, x in v.items()):
                bad.append('colors: {"VFR"|"MVFR"|"IFR"|"LIFR": "rrggbb" or null}')
        else:
            warn.append(f"{k}: unknown key (the firmware ignores it)")
    if (FORCED & set(cfg)) and cfg.get("force") is not True:
        bad.append(f"{sorted(FORCED & set(cfg))} need force=true")
    return bad


def parse_value(text):
    if text.startswith("@"):            # @file: an airport list, one entry per LED
        return [t for t in re.split(r"[\s,]+", open(text[1:]).read()) if t]
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("unit", help="fleet.md unit number or boardid")
    ap.add_argument("set", nargs="*", metavar="KEY=VALUE")
    ap.add_argument("--show", action="store_true", help="print the file and the latest telemetry, change nothing")
    ap.add_argument("--rev", type=int, help="use this rev instead of the automatic one")
    ap.add_argument("--no-logs", action="store_true", help="skip the CloudFront log lookup")
    ap.add_argument("--yes", action="store_true", help="upload without asking")
    a = ap.parse_args()

    board = resolve(a.unit)
    cur = read_file(board)
    tel = None if a.no_logs else latest_telemetry(board)
    dev_rev = decode_t(tel[1]).get("cfg_rev") if tel else None
    lan = lan_rev(tel[1]) if tel else None
    if lan is not None:
        dev_rev = max(dev_rev or 0, lan)
    if a.show or not a.set:
        print(f"{cfg_url(board)}:\n{json.dumps(cur, indent=2) if cur else '(no file)'}")
        if tel:
            q = tel[1]
            fw = re.search(r"fw=([^&]+)", q)
            dbg = re.search(r"debug=([^&]+)", q)
            print(f"latest poll {tel[0]} UTC: fw {fw and fw.group(1)}, debug {dbg and dbg.group(1)}, {decode_t(q) or 'no t='}")
        else:
            print("" if a.no_logs else "no poll from this map in the last 2 h of logs" if os.environ.get("TMM_LOG_S3")
                  else "(TMM_LOG_S3 not set: last poll not looked up)")
        return

    new = {}
    for item in a.set:
        k, eq, v = item.partition("=")
        if not eq:
            sys.exit(f"bad argument {item!r}: use KEY=VALUE")
        new[k] = parse_value(v)
    base = max(int(cur.get("rev", 0)), int(dev_rev or 0))
    new = {"rev": a.rev or base + 1, **new}
    warnings = []
    problems = check(new, warnings)
    body = json.dumps(new, separators=(",", ":"))
    if len(body.encode()) > MAX_BYTES:
        problems.append(f"file is {len(body.encode())} bytes, firmware limit {MAX_BYTES}")
    shown = json.dumps(new, indent=2)
    shown = re.sub(r'("pass":\s*)"[^"]*"', r'\1"********"', shown)
    print(f"{cfg_url(board)} (file rev {cur.get('rev', 'none')}, unit last reported rev {dev_rev}):\n{shown}")
    for w in warnings:
        print(f"warning: {w}")
    if problems:
        sys.exit("not uploaded:\n  " + "\n  ".join(problems))
    if not a.yes:
        try:
            ok = input("upload? [y/N] ").strip().lower() == "y"
        except EOFError:
            ok = False
        if not ok:
            sys.exit("not uploaded")
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        f.write(body)
    try:
        aws(["s3", "cp", f.name, cfg_url(board), "--cache-control", "max-age=30",
             "--content-type", "application/json", "--only-show-errors"])
    finally:
        os.unlink(f.name)
    print(f"uploaded rev {new['rev']}; the map applies it at its next config check (every 60 min by default)")


if __name__ == "__main__":
    main()
