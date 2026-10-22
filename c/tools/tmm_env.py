"""Shared settings for the maintainer tools. Everything site-specific comes from the environment,
so nothing private lives in this repo:

  TMM_S3            s3://bucket/prefix that holds bin/{stable,beta}.bin and cfg/<boardid>.json
                    (the firmware fetches the same files over HTTPS)
  AWS_PROFILE       AWS credentials for TMM_S3 (standard AWS CLI variable)
  TMM_LOG_S3        optional: CloudFront standard-log prefix (parquet) for the weather host, used
                    by push_config.py to find a map's last poll
  TMM_FLEET         optional: path to a fleet.md whose table rows start "| <n> | `<boardid>` |",
                    so tools accept a unit number instead of a boardid
  TMM_ADMIN_SECRET_REF  optional: 1Password reference (op://...) for the admin phrase secret
  TMM_ADMIN_SECRET  test only: the secret itself
  IDF_PATH          ESP-IDF checkout (default ~/esp/esp-idf, then ~/git/esp-idf)
"""
import os
import re
import shlex
import subprocess
import sys

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(PROJ, "build")
APP_NAME = "tedder-metar-map"
NVS_NAMESPACE = "tmm_cfg"
PUB_KEY = os.path.join(PROJ, "signing-pub.pem")

# Same order as the firmware's k_led_order_opts / k_led_model_opts (NVS stores the index, u16).
LED_ORDERS = ["RGB", "GRB", "BRG", "BGR", "RBG", "GBR", "RGBW", "GRBW"]
LED_MODELS = ["ws2812", "sk6812", "ws2811"]


def idf_path():
    for p in (os.environ.get("IDF_PATH"), "~/esp/esp-idf", "~/git/esp-idf"):
        if p and os.path.exists(os.path.join(os.path.expanduser(p), "export.sh")):
            return os.path.expanduser(p)
    sys.exit("ESP-IDF not found: set IDF_PATH")


def idf_run(cmd, capture=True):
    """Run a shell command inside the ESP-IDF environment (python with esptool, nvs tools)."""
    full = f"source {shlex.quote(os.path.join(idf_path(), 'export.sh'))} >/dev/null 2>&1 && {cmd}"
    r = subprocess.run(["bash", "-c", full], cwd=PROJ, text=True, capture_output=capture)
    if r.returncode != 0:
        if capture:
            sys.stderr.write(r.stdout[-1500:] + r.stderr[-1500:])
        sys.exit(f"failed: {cmd}")
    return r.stdout if capture else ""


def s3_base():
    b = os.environ.get("TMM_S3", "").rstrip("/")
    if not b.startswith("s3://"):
        sys.exit("set TMM_S3 to s3://bucket/prefix (holds bin/ and cfg/)")
    return b


def s3_split(url):
    """s3://bucket/key -> (bucket, key)."""
    bucket, _, key = url[len("s3://"):].partition("/")
    return bucket, key


def aws(args, check=True):
    r = subprocess.run(["aws"] + args, text=True, capture_output=True)
    if check and r.returncode:
        sys.exit(f"aws {' '.join(args)} failed:\n{r.stderr}")
    return r


def fleet_rows():
    """(unit, boardid, cells) for each fleet table row; [] without TMM_FLEET."""
    path = os.environ.get("TMM_FLEET")
    if not path:
        return []
    rows = []
    for line in open(os.path.expanduser(path)):
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        m = re.fullmatch(r"`([0-9a-f]{12})`", cells[1]) if len(cells) > 1 else None
        if cells and cells[0].isdigit() and m:
            rows.append((cells[0], m.group(1), cells))
    return rows


def resolve(unit):
    """A 12-hex boardid, or a fleet.md unit number -> boardid."""
    if re.fullmatch(r"[0-9a-f]{12}", unit):
        return unit
    for n, bid, _ in fleet_rows():
        if n == unit:
            return bid
    sys.exit(f"unit {unit!r}: not a 12-hex boardid" +
             ("" if os.environ.get("TMM_FLEET") else " (set TMM_FLEET to use unit numbers)"))


def read_boardid(port):
    """Board ID the firmware uses: first 6 bytes of the eFuse optional unique ID, else the WiFi MAC."""
    out = idf_run(f"python -m espefuse --port {shlex.quote(port)} summary --format json", capture=True)
    try:
        import json
        val = json.loads(out[out.index("{"):])["OPTIONAL_UNIQUE_ID"]["value"]
        hexs = re.sub(r"[^0-9a-f]", "", str(val).lower().replace("0x", ""))
        if len(hexs) >= 12 and int(hexs[:12], 16):
            return hexs[:12]
    except (ValueError, KeyError):
        pass
    out = idf_run(f"python -m esptool --port {shlex.quote(port)} read-mac", capture=True)
    m = re.search(r"MAC:\s*([0-9a-f:]{17})", out, re.I)
    if not m:
        sys.exit("could not read the board ID (eFuse unique ID or MAC)")
    return m.group(1).replace(":", "").lower()
