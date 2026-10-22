#!/usr/bin/env python3
"""Change single NVS settings (namespace tmm_cfg) on a map over USB, keeping all the others.

Reads the NVS partition, decodes tmm_cfg, applies the changes, rebuilds the partition and writes
it back. Other namespaces (PHY calibration, WiFi driver cache) are dropped; the firmware recreates
them on the next boot. A backup of the original partition goes to ~/tmp/tmm-nvs/ (mode 700; it
holds WiFi passwords). Passwords are never printed.

  tools/nvs_set.py --port /dev/cu.usbmodem101 test_hooks=u8:1
  tools/nvs_set.py --port /dev/cu.usbmodem101 led_order=u16:1 --delete ota_int   # 1 = GRB (tools/tmm_env.py)
  tools/nvs_set.py --port /dev/cu.usbmodem101 --show          # decode only, change nothing
Types: u8 u16 u32 i8 i16 i32 string.
"""
import argparse
import csv
import json
import os
import shlex
import sys
import tempfile
import time

import tmm_env as E

NS = E.NVS_NAMESPACE
NVS_OFF, NVS_SIZE = 0x9000, 0x6000
BACKUP_DIR = os.path.expanduser("~/tmp/tmm-nvs")
ENC = {"uint8_t": "u8", "uint16_t": "u16", "uint32_t": "u32", "int8_t": "i8", "int16_t": "i16",
       "int32_t": "i32", "string": "string"}


def idf(cmd):
    return E.idf_run(cmd)


def show(rows):
    for k, enc, v in rows:
        if k.startswith("pass") and v:
            v = "********"
        elif k == "airports":
            v = f"{len(v.split())} LEDs: " + " ".join(v.split()[:8]) + (" ..." if len(v.split()) > 8 else "")
        print(f"  {k:12} {enc:6} {v}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("set", nargs="*", metavar="KEY=TYPE:VALUE")
    ap.add_argument("--delete", action="append", default=[], metavar="KEY")
    ap.add_argument("--show", action="store_true", help="decode and print, change nothing")
    a = ap.parse_args()

    os.makedirs(BACKUP_DIR, exist_ok=True)
    os.chmod(BACKUP_DIR, 0o700)
    backup = os.path.join(BACKUP_DIR, time.strftime("nvs-%Y%m%d-%H%M%S.bin"))
    port = shlex.quote(a.port)
    idf(f"python -m esptool --port {port} read-flash {hex(NVS_OFF)} {hex(NVS_SIZE)} {shlex.quote(backup)}")
    os.chmod(backup, 0o600)
    nvs_tool = os.path.join(E.idf_path(), "components/nvs_flash/nvs_partition_tool/nvs_tool.py")
    entries = json.loads(idf(f"python {shlex.quote(nvs_tool)} -f json -d minimal {shlex.quote(backup)}"))
    rows = []
    for e in entries:
        if e["namespace"] != NS or e.get("state", "Written") != "Written":
            continue
        enc = ENC.get(e["encoding"])
        if not enc:
            sys.exit(f"unsupported type {e['encoding']} for {e['key']}")
        rows = [r for r in rows if r[0] != e["key"]] + [(e["key"], enc, str(e["data"]))]
    print(f"backup {backup}\ncurrent {NS}:")
    show(rows)
    if a.show:
        return
    if not a.set and not a.delete:
        sys.exit("nothing to change")

    for item in a.set:
        k, _, tv = item.partition("=")
        enc, _, v = tv.partition(":")
        if not k or enc not in ENC.values() or len(k) > 15:
            sys.exit(f"bad setting {item!r} (KEY=TYPE:VALUE, key <= 15 chars)")
        rows = [r for r in rows if r[0] != k] + [(k, enc, v)]
    rows = [r for r in rows if r[0] not in a.delete]
    rows = [r for r in rows if not (r[1] == "string" and r[2] == "")]   # empty = absent to the firmware

    with tempfile.TemporaryDirectory(prefix="tmm-nvs-") as tmp:
        csv_path, bin_path = os.path.join(tmp, "nvs.csv"), os.path.join(tmp, "nvs.bin")
        with open(csv_path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["key", "type", "encoding", "value"])
            w.writerow([NS, "namespace", "", ""])
            for k, enc, v in rows:
                w.writerow([k, "data", enc, v])
        idf(f"python -m esp_idf_nvs_partition_gen generate {shlex.quote(csv_path)} {shlex.quote(bin_path)} {hex(NVS_SIZE)}")
        idf(f"python -m esptool --port {port} write-flash {hex(NVS_OFF)} {shlex.quote(bin_path)}")
    print("written:")
    show(rows)
    print(f"restore with: python -m esptool --port {a.port} write-flash {hex(NVS_OFF)} {backup}")


if __name__ == "__main__":
    main()
