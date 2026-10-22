#!/usr/bin/env python3
"""Capture the device's USB serial log for N seconds (no reset). Masks WiFi passwords.
Usage (IDF python env): python tools/serial_log.py /dev/cu.usbmodem101 45 [outfile] [--reset]
--reset pulses RTS (like idf.py monitor) so the log starts at boot."""
import re
import sys
import time

import serial

args = [a for a in sys.argv[1:] if a != "--reset"]
port, secs = args[0], float(args[1])
out = open(args[2], "w") if len(args) > 2 else None
s = serial.Serial(port, 115200, timeout=0.5)
if "--reset" in sys.argv:
    s.dtr = False
    s.rts = True
    time.sleep(0.2)
    s.rts = False
end = time.time() + secs
buf = b""
while time.time() < end:
    buf += s.read(4096)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        t = re.sub(r"\x1b\[[0-9;]*m", "", line.decode("utf-8", "replace")).rstrip()
        t = re.sub(r"(pass\w*[=:]\s*)\S+", r"\1********", t, flags=re.I)
        print(t, flush=True)
        if out:
            out.write(t + "\n")
            out.flush()   # a kill (timeout, Ctrl-C) must not lose the buffered log
