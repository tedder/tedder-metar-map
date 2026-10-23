#!/usr/bin/env python3
"""Promote the beta firmware to stable: copies bin/beta.bin to bin/stable.bin on S3.

Checks first: beta.bin is a release build (no -rbtest/-sigtest version suffix), it is signed
with the project key (signing-pub.pem), and it differs from the current stable.bin. Prints both
versions and the stable units from fleet.md (TMM_FLEET). Without --yes it only reports.
Needs TMM_S3 and AWS credentials (tools/tmm_env.py).

  tools/promote.py            # report only
  tools/promote.py --yes      # copy beta -> stable (max-age=30); stable units update within 6 h
"""
import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile

import tmm_env as E
from tmm_env import aws


def bin_loc():
    bucket, key = E.s3_split(E.s3_base())
    return bucket, (key + "/bin").lstrip("/")


def fetch(name, path):
    """Download bin/<name>.bin; returns its ETag, or None when it doesn't exist."""
    bucket, prefix = bin_loc()
    r = aws(["s3api", "get-object", "--bucket", bucket, "--key", f"{prefix}/{name}.bin", path,
             "--query", "ETag", "--output", "text"], check=False)
    return r.stdout.strip().strip('"') if r.returncode == 0 else None


def app_version(path):
    """esp_app_desc_t.version: image header (24) + first segment header (8) + 16."""
    with open(path, "rb") as f:
        d = f.read(80)
    if len(d) < 80 or d[0] != 0xE9 or d[32:36] != bytes.fromhex("3254cdab"):
        return None
    return d[48:80].split(b"\0")[0].decode("ascii", "replace")


def signed_ok(path):
    cmd = (f"source {shlex.quote(os.path.join(E.idf_path(), 'export.sh'))} >/dev/null 2>&1 && "
           f"python -m espsecure verify-signature --version 2 --keyfile {shlex.quote(E.PUB_KEY)} {shlex.quote(path)}")
    r = subprocess.run(["bash", "-c", cmd], text=True, capture_output=True)
    lines = (r.stdout + r.stderr).strip().splitlines()
    pick = [x for x in lines if "verif" in x.lower() and "success" in x.lower()] if r.returncode == 0 else lines[-1:]
    return r.returncode == 0, pick[:1] or [""]


def stable_units():
    return [f"unit {n} {bid} ({c[2][:40]})" for n, bid, c in E.fleet_rows()
            if len(c) >= 5 and "stable" in c[4].lower()]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--yes", action="store_true", help="do the copy")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="tmm-promote-") as tmp:
        beta, stable = os.path.join(tmp, "beta.bin"), os.path.join(tmp, "stable.bin")
        b_etag = fetch("beta", beta)
        if not b_etag:
            sys.exit("no beta.bin on S3")
        s_etag = fetch("stable", stable)
        bv = app_version(beta)
        sv = app_version(stable) if s_etag else None
        ok, msg = signed_ok(beta)
        print(f"beta.bin   {bv} (ETag {b_etag}); signature: {'OK' if ok else 'FAILED'} {msg[0]}")
        print(f"stable.bin {sv if s_etag else '(none yet)'}" + (f" (ETag {s_etag})" if s_etag else ""))
        units = stable_units()
        print("stable units (fleet.md): " + ("; ".join(units) if units else "none listed"))
        problems = []
        if not bv:
            problems.append("beta.bin is not an ESP app image")
        elif re.search(r"-(rbtest|sigtest)", bv):
            problems.append(f"beta.bin is a test build ({bv}); rebuild the real image first")
        if not ok:
            problems.append("beta.bin is not signed with our key: stable units would refuse it")
        if s_etag == b_etag:
            problems.append("stable.bin already has these bytes")
        if problems:
            sys.exit("not promoted:\n  " + "\n  ".join(problems))
        if not a.yes:
            print("report only; run with --yes to copy beta -> stable")
            return
    bucket, prefix = bin_loc()
    aws(["s3", "cp", f"s3://{bucket}/{prefix}/beta.bin", f"s3://{bucket}/{prefix}/stable.bin",
         "--cache-control", "max-age=30", "--content-type", "application/octet-stream",
         "--metadata-directive", "REPLACE", "--only-show-errors"])
    r = aws(["s3api", "head-object", "--bucket", bucket, "--key", f"{prefix}/stable.bin",
             "--query", "ETag", "--output", "text"])
    print(f"promoted {bv}: stable.bin ETag {r.stdout.strip()} (beta {b_etag}); "
          "stable units take it at their next update check (every 6 h by default)")


if __name__ == "__main__":
    main()
