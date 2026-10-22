#!/usr/bin/env python3
"""Antenna / radio check for a new board: runs an upload sink on this machine, starts
POST /radio_check on the unit, waits for the result and prints it. Exit 0 pass or warn (weak only at
17 dBm), 1 fail, 2 unsure, 3 could not run. The unit steps TX power 17/11/8 dBm twice on its current network (no rejoin), pings
its gateway 20 times per step and uploads 256 KB here per step; ~1 min. A bad antenna gets worse as
power goes up.

  tools/radio_check.py metarmap.local            # or the map's IP (status page address)
  tools/radio_check.py metarmap.local --no-sink  # pings only
  tools/radio_check.py metarmap.local --sink http://<wired host>:8399   # sink on a wired machine
The built-in sink runs on this machine: if it is on WiFi, every upload crosses the air twice and
reads slower; a wired sink gives cleaner numbers.
Run it with the unit where it gets a decent signal (better than about -70 dBm)."""
import argparse
import http.server
import json
import socket
import sys
import threading
import time
import urllib.parse
import urllib.request


class Sink(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        got = 0
        while got < n:
            chunk = self.rfile.read(min(65536, n - got))
            if not chunk:
                break
            got += len(chunk)
        body = str(got).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass


def local_ip_toward(host):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, 80))
        return s.getsockname()[0]
    finally:
        s.close()


def call(method, url, data=None, origin=None):
    req = urllib.request.Request(url, data=data, method=method)
    if origin:
        req.add_header("Origin", origin)
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("unit", help="unit IP address")
    ap.add_argument("--no-sink", action="store_true", help="pings only, no upload test")
    ap.add_argument("--sink", metavar="URL", help="use this sink (http://LAN-IPv4:port, POST /up) instead of one here")
    ap.add_argument("--json", metavar="FILE", help="also save the unit's result JSON here")
    a = ap.parse_args()
    base = f"http://{a.unit}"

    form = b""
    if a.sink:
        form = urllib.parse.urlencode({"sink": a.sink}).encode()
        print(f"sink {a.sink}")
    elif not a.no_sink:
        srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Sink)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        sink = f"http://{local_ip_toward(a.unit)}:{srv.server_address[1]}"
        form = urllib.parse.urlencode({"sink": sink}).encode()
        print(f"sink {sink}")

    code, js = call("POST", f"{base}/radio_check", form, origin=base)
    if code != 200 or not js.get("started"):
        print(f"could not start (HTTP {code}): {js.get('why', js)}")
        return 3
    t0 = time.time()
    while time.time() - t0 < 180:
        time.sleep(2)
        code, js = call("GET", f"{base}/radio_check")
        if js.get("state") == "done":
            break
    else:
        print("no result after 3 min")
        return 3
    if a.json:
        with open(a.json, "w") as f:
            json.dump(js, f, indent=1)
    print(f"{'dBm':>4} {'ping':>6} {'avg ms':>6} {'max ms':>6} {'up kB/s':>8}")
    for r in js["rows"]:
        up = "-" if r["up_kBps"] < 0 else r["up_kBps"]
        drop = f"  dropped (reason {r['dropped']})" if r["dropped"] else ""
        print(f"{r['dbm']:>4} {r['recv']:>3}/{r['sent']:<2} {r['avg_ms']:>6} {r['max_ms']:>6} {up:>8}{drop}")
    print(f"{js['result'].upper()}: {js['why']} ({js['ms'] / 1000:.0f} s; back at {js['tx_dbm']} dBm)")
    return {"pass": 0, "warn": 0, "fail": 1}.get(js["result"], 2)


if __name__ == "__main__":
    sys.exit(main())
