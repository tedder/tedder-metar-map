#!/usr/bin/env python3
"""Per-map admin phrase: four words from the EFF short word list, separated by spaces.

  phrase = 4 words picked by HMAC-SHA256(secret, "tmm-admin:" + boardid)
  hash   = SHA-256("tmm-admin-v1:" + boardid + ":" + phrase)   (hex; stored in NVS "admin_hash")

The secret is read with the 1Password CLI from TMM_ADMIN_SECRET_REF (an op:// reference). The
map stores only the hash, written over USB. The phrase unlocks changing the firmware/config
URLs on /config:
  tools/admin_phrase.py 0123456789ab                 # print the phrase (to type on /config)
  tools/admin_phrase.py 1 --nvs-hash                 # print the hash for NVS
  tools/provision.py ... --admin-phrase              # writes admin_hash while provisioning
  tools/nvs_set.py --port PORT admin_hash=string:$(tools/admin_phrase.py 1 --nvs-hash)

The map lowercases the typed phrase and collapses spaces before hashing (same as normalize()).
Word list: EFF short word list 1 (eff.org/dice, CC BY 3.0 US), tools/eff_short_wordlist_1.txt.
"""
import argparse
import hashlib
import hmac
import os
import subprocess
import sys

import tmm_env as E
from tmm_env import resolve

WORDS = os.path.join(E.PROJ, "tools", "eff_short_wordlist_1.txt")


def secret():
    env = os.environ.get("TMM_ADMIN_SECRET")           # for tests only; normally from 1Password
    if env:
        return env.encode()
    ref = os.environ.get("TMM_ADMIN_SECRET_REF")
    if not ref:
        sys.exit("set TMM_ADMIN_SECRET_REF to the 1Password reference (op://vault/item/field) of the secret")
    r = subprocess.run(["op", "read", ref], text=True, capture_output=True)
    if r.returncode:
        sys.exit(f"could not read the secret from 1Password ({ref}): {r.stderr.strip()}")
    return r.stdout.strip().encode()


def phrase(board, key):
    words = open(WORDS).read().split()
    assert len(words) == 1296, "word list must have 1296 words"
    n = int.from_bytes(hmac.new(key, ("tmm-admin:" + board).encode(), hashlib.sha256).digest()[:8], "big")
    return " ".join(words[(n // 1296 ** i) % 1296] for i in range(4))


def normalize(p):
    return " ".join(p.lower().split())


def nvs_hash(board, p):
    return hashlib.sha256(f"tmm-admin-v1:{board}:{normalize(p)}".encode()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("unit", help="fleet.md unit number or 12-hex boardid")
    ap.add_argument("--nvs-hash", action="store_true", help="print the NVS admin_hash instead of the phrase")
    a = ap.parse_args()
    board = resolve(a.unit)
    p = phrase(board, secret())
    print(nvs_hash(board, p) if a.nvs_hash else p)


if __name__ == "__main__":
    main()
