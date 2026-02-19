# circup install neopixel adafruit_requests adafruit_datetime adafruit_httpserver adafruit_ssd1306 adafruit_ntp
# https://github.com/adafruit/Adafruit_CircuitPython_NeoPixel
# wget https://raw.githubusercontent.com/adafruit/Adafruit_CircuitPython_framebuf/main/examples/font5x8.bin

# bootup: blue, yellow, green
# after 30sec, "apts: nn; mm mins" on second line

import time
import busio
import struct
import json
import re

import adafruit_httpserver
from adafruit_httpserver import GET, POST, Route
import adafruit_ntp
import sys
import traceback
import adafruit_ssd1306
import digitalio
import ssl
import os
import gc
import ipaddress
import microcontroller

FIRMWARE_VERSION = "2.0.2"
TMM_VERSION = "v1.1.7"

# Build user agent string with CircuitPython version
try:
    cp_version = ".".join(str(v) for v in sys.implementation.version[:3])
    USER_AGENT = f"tedder-metar-map/{FIRMWARE_VERSION} circuitpython/{cp_version}"
except:
    USER_AGENT = f"tedder-metar-map/{FIRMWARE_VERSION} circuitpython/unknown"

METAR_SERVICE_URL = "http://fly.tedder.me/metar"

WEB_PORT = 80

if sys.implementation.name.lower() == "cpython":
    from fakes import *
    from datetime import datetime

    WEB_PORT = 5000  # nonprivileged on a real OS
    watchdog_enabled = False
else:
    import adafruit_requests
    from adafruit_datetime import datetime
    import microcontroller
    import supervisor

    # Watchdog setup
    try:
        from watchdog import WatchDogMode
        supervisor.set_next_code_file("code.py")
        # Set timeout to 3600s (60 minutes) - we'll handle diagnostic reboot differently
        # Boot sequence should complete well under this timeout
        microcontroller.watchdog.timeout = 3600  # 60 minutes for normal operation
        microcontroller.watchdog.mode = WatchDogMode.RESET
        watchdog_enabled = True
        print("[OK] Watchdog enabled (3600s / 60min)")
    except Exception as e:
        watchdog_enabled = False
        print(f"[WARN] Watchdog setup failed: {e}")

try:
    import storage
    import usb_cdc
except ModuleNotFoundError:
    pass


try:
    import board
    import neopixel
    import wifi
    import socketpool
except NotImplementedError:
    # from fakes import adafruit_httpserver
    # from fakes import GET, POST
    # from fakes import wifi
    from fakes import *

    pass

airportlist = []
airportwx = {}
ledstate = []
errors = []
web = None

MAX_ERRORS = 20

debug = 1
# debug 0 == stfu
# debug 10 == all the noise

# Debug flags (32-bit bitmask, persistent in NVM)
# Bits 0-4: Hardware/display errors
# Bit 0: 0x01 = Display init failed
# Bit 1: 0x02 = Display update failed
# Bit 2: 0x04 = WiFi enable failed
# Bit 3: 0x08 = METAR fetch failed
# Bit 4: 0x10 = Reserved
# Bits 5-8: HTTP/Network errors
# Bit 5: 0x20 = HTTP request exception
# Bit 6: 0x40 = NTP datetime failed
# Bit 7: 0x80 = Reserved
# Bit 8: 0x100 = Reserved
# Bits 9-15: System errors
# Bit 9: 0x200 = WiFi connect failed (3+ attempts)
# Bit 10: 0x400 = Main loop exception
# Bit 11: 0x800 = NVM write failed
# Bit 12: 0x1000 = Memory error
# Bit 13: 0x2000 = IPv6 error/incompatibility
# Bit 14: 0x4000 = NeoPixel initialization failed
# Bit 15: 0x8000 = Watchdog reset (set on startup if previous reset was watchdog)

debug_flags = 0
try:
    debug_flags = struct.unpack("<I", microcontroller.nvm[0:4])[0] if hasattr(microcontroller, 'nvm') else 0
except Exception as e:
    print(f"[WARN] NVM read failed ({e}), clearing NVM")
    # Clear potentially corrupted NVM
    try:
        microcontroller.nvm[0:4] = struct.pack("<I", 0)
    except:
        pass
    debug_flags = 0

# Boot counter in NVM bytes 4-7: incremented every boot, cleared only after successful phone_home.
# If phone_home sees boot_count > 1, the device had N-1 failed boot attempts before this one.
boot_count = 0
try:
    boot_count = struct.unpack("<I", microcontroller.nvm[4:8])[0] if hasattr(microcontroller, 'nvm') else 0
    if boot_count > 10000:  # sanity check for corrupted NVM
        boot_count = 0
except Exception:
    boot_count = 0
boot_count += 1
try:
    microcontroller.nvm[4:8] = struct.pack("<I", boot_count)
except Exception:
    pass
print(f"[INFO] Boot count: {boot_count}")

try:
    BOARD_UID = microcontroller.cpu.uid.hex()[:12] if hasattr(microcontroller.cpu.uid, 'hex') else ""
except Exception:
    BOARD_UID = ""

def set_debug_flag(bit):
    global debug_flags
    debug_flags |= (1 << bit)
    try:
        microcontroller.nvm[0:4] = struct.pack("<I", debug_flags)
    except Exception as e:
        print(f"[WARN] NVM write failed: {e}")
        set_debug_flag(11)  # Set bit 11 to indicate NVM write failure

def clear_debug_flag(bit):
    global debug_flags
    debug_flags &= ~(1 << bit)
    try:
        microcontroller.nvm[0:4] = struct.pack("<I", debug_flags)
    except Exception as e:
        print(f"[WARN] NVM write failed: {e}")

def reset_debug_flags():
    global debug_flags
    debug_flags = 0
    try:
        microcontroller.nvm[0:4] = struct.pack("<I", 0)
    except:
        pass

# Capture reset reason from the previous boot
_reset_reason = "unknown"
try:
    _reset_reason = str(supervisor.runtime.reset_reason)
except Exception:
    try:
        _reset_reason = str(microcontroller.cpu.reset_reason)
    except Exception:
        pass
print(f"[INFO] Reset reason: {_reset_reason}")
if "watchdog" in _reset_reason.lower():
    set_debug_flag(15)

def add_error(errstr):
    global errors
    errors.append(errstr)
    if len(errors) > MAX_ERRORS:
        errors = errors[-MAX_ERRORS:]

def feed_watchdog():
    if sys.implementation.name.lower() != "cpython" and watchdog_enabled:
        try:
            microcontroller.watchdog.feed()
        except:
            pass

# board's single pixel
try:
    pixel = neopixel.NeoPixel(board.NEOPIXEL, 1)
    pixel.fill((0, 0, 90))  # light blue
    print("[OK] NeoPixel initialized")
except Exception as e:
    print(f"[WARN] NeoPixel init failed: {e}")
    pixel = None
time.sleep(10)

# Initialize I2C for display
i2c = None
try:
    i2c = busio.I2C(board.SCL, board.SDA, frequency=400000)
    print("[OK] I2C initialized")
except RuntimeError as e:
    print(f"[WARN] I2C init failed: {e}")
    i2c = None

# Initialize display
display = None
if i2c:
    if i2c.try_lock():
        try:
            i2c_devices = i2c.scan()
            print(f"[OK] I2C devices found: {i2c_devices}")
        except Exception as e:
            print(f"[WARN] I2C scan failed: {e}")
        i2c.unlock()

    try:
        display = adafruit_ssd1306.SSD1306_I2C(128, 32, i2c)
        display.fill(0)
        display.show()
        print("[OK] OLED display initialized")
    except Exception as e:
        print(f"[FAIL] OLED init failed: {e}")
        set_debug_flag(0)
        display = None
else:
    print("[WARN] I2C not available, display disabled")

try:
    hpool = socketpool.SocketPool(wifi.radio)
    print("[OK] SocketPool initialized")
except Exception as e:
    print(f"[FAIL] SocketPool init failed: {e}")
    hpool = None

try:
    hserver = adafruit_httpserver.Server(hpool, debug=False)
    print("[OK] HTTP server initialized")
except Exception as e:
    print(f"[FAIL] HTTP server init failed: {e}")
    hserver = None

# Initialize requests before NTP so we can phone_home before the potentially-blocking NTP sync.
# Plain HTTP session is sufficient for phone_home (am.colo uses HTTP).
try:
    requests = adafruit_requests.Session(hpool, ssl.create_default_context())
    print("[OK] Requests session initialized")
except Exception as e:
    print(f"[FAIL] Requests init failed: {e}")
    set_debug_flag(5)
    requests = None

# NTP initialized in boot sequence (after try_wifi) so phone_home() can fire first.
ntp = None

def write_error(errstr):
    try:
        with open("errors0.txt", "a") as f:
            f.write("e0\n")
            f.write(errstr)
            f.write("\n--\n")
            return
    except Exception as ex:
        #oled_write(error=str(errstr), web=web)
        #pixel.fill((255, 0, 0))  # red
        print("couldn't write to error file: ", traceback.format_exception(ex))

    try:
        storage.remount(
            "/", readonly=False, disable_concurrent_write_protection=True
        )
    except Exception as ex:
        pass
    try:
        with open("errors.txt", "a") as f:
            f.write("e1\n")
            f.write(errstr)
            f.write("\n--\n")
    except Exception as ex:
        #oled_write(error=str(errstr), web=web)
        #pixel.fill((255, 0, 0))  # red
        print("couldn't write to error file: ", traceback.format_exception(ex))
    try:
        with open("sd/errors.txt", "a") as f:
            f.write("e2\n")
            f.write(errstr)
            f.write("\n--\n")
    except Exception as ex:
        pass
    try:
        with open("/sd/errorss.txt", "a") as f:
            f.write("e3\n")
            f.write(errstr)
            f.write("\n--\n")
    except Exception as ex:
        pass

def dir_to_str(path, indent=0):
    ret = ""
    idt = "&nbsp;" * (indent*4)
    f = storage.getmount("/")
    for ff in f.ilistdir(path):
        fullpath = path + "/" + ff[0]
        ftype = ff[1]
        if ftype & 16384: # directory
          ret += f"""{idt}<b>{fullpath}</b><br />\n"""
          ret += dir_to_str(fullpath, indent=indent+1)
        elif ftype & 32768: # file
          ret += f"""{idt}<a href="/files?f={fullpath}">{ff[0]}</a> {ff[3]}<br />\n"""
        else: # unknown type
            ret += f"""{idt}{ff[0]} ????type???? {ff}<br />\n"""

    return ret

def get_now_struct():
    global errors
    if not ntp:
        return None
    try:
        return ntp.datetime
    except socketpool.SocketPool.gaierror as ex:
        print(f"[WARN] NTP gaierror: {ex}")
        set_debug_flag(6)
        add_error(str(ex))
        write_error(str(ex))
        return None
    except OSError as ex:
        print(f"[WARN] NTP OSError (timeout): {ex}")
        set_debug_flag(6)
        time.sleep(10)
        return None
    except Exception as ex:
        print(f"[FAIL] NTP error: {ex}")
        set_debug_flag(6)
        return None


def get_now():
    nowstruct = get_now_struct()
    if nowstruct:
        #return time.mktime(ntp.datetime)
        return time.mktime(nowstruct)
    return


last_phone_home_mono = 0  # monotonic time of last phone_home (not NTP-dependent)
last_exception_time = 0  # For non-blocking exception throttling
wifi_ssid = ""  # Store SSID when connected


def phone_home():
    global last_phone_home_mono, boot_count
    if not wifi.radio.connected:
        return

    now_mono = time.monotonic()
    if last_phone_home_mono and (now_mono - last_phone_home_mono) < 3600:
        return
    last_phone_home_mono = now_mono

    uptime_seconds = time.monotonic_ns() // 1_000_000_000
    now_epoch = get_now()

    # Check in to generic-sink with device telemetry
    try:
        # Convert board UID from bytes to hex string
        board_uid_hex = microcontroller.cpu.uid.hex() if hasattr(microcontroller.cpu.uid, 'hex') else microcontroller.cpu.uid

        # Get SSID from environment variable if not already set
        current_ssid = wifi_ssid if wifi_ssid else os.getenv("CIRCUITPY_WIFI_SSID", "")

        device_data = {
            "device_id": "tedder-metar-map-esp32s2",
            "board_uid": board_uid_hex,
            "firmware_version": FIRMWARE_VERSION,
            "uptime_seconds": uptime_seconds,
            "timestamp": now_epoch if now_epoch else 0,
            "wifi_connected": wifi.radio.connected,
            "wifi_ssid": current_ssid,
            "wifi_ip": str(wifi.radio.ipv4_address) if wifi.radio.ipv4_address else "0.0.0.0",
            "ntp_synced": ntp is not None,
            "debug_flags": f"0x{debug_flags:x}",
            "reset_reason": _reset_reason,
            "boot_count": boot_count,
            "user_agent": USER_AGENT
        }

        json_data = json.dumps(device_data)
        headers = {"Content-Type": "application/json"}

        ret = requests.post(
            "http://am.colo.tedder.me:5091/data",
            data=json_data,
            headers=headers
        )
        print(f"[OK] Device check-in sent, response: {ret.status_code}")
        ret.close()
        gc.collect()
        # Successful check-in: clear boot counter so next boot starts fresh at 1
        try:
            microcontroller.nvm[4:8] = struct.pack("<I", 0)
            boot_count = 1
        except Exception:
            pass
    except Exception as ex:
        print(f"[WARN] Device check-in failed: {ex}")
        pass


# save state on the error string
_error = ""
def oled_write(line3="", line4="", error=None, web=None):
    global memory_error_count
    global _error
    global errors
    feed_watchdog()
    if not display:
        return

    try:
        if error:
            _error = error
            add_error(error)
            if web:
                web.errors.append(error)
                if len(web.errors) > MAX_ERRORS:
                    web.errors = web.errors[-MAX_ERRORS:]
        if _error:
            line3 = _error[:21]

        if not line3:
            line3 = f"0x{debug_flags:x} {FIRMWARE_VERSION}"
            if memory_error_count > 0:
                line3 += f";{memory_error_count}me"

        # Line 1: WiFi/IP
        line1 = f"IP: {wifi.radio.ipv4_address}" if wifi.radio.connected else "NO WIFI"

        # Line 2: apts + time combined
        nowstruct = get_now_struct()
        time_str = f"{nowstruct.tm_hour:02d}:{nowstruct.tm_min:02d}" if nowstruct else "--:--"
        line2 = f"apts:{len(airportlist)} {time_str}"

        ages = [aw.get("age_mins") for aw in airportwx.values() if aw.get("age_mins") is not None]
        if ages:
            line2 += f"/{min(ages):.0f}m"

        display.fill(0)
        display.text(line1, 0, 0, 1)
        display.text(line2, 0, 11, 1)
        display.text(line3, 0, 22, 1)
        display.show()
    except Exception as e:
        print(f"[FAIL] OLED update error: {e}")
        set_debug_flag(1)


def load_airports():
    airports = []
    try:
        with open("airports.txt") as fa:
            for a in fa:
                airports.append(a.rstrip().upper())
    except OSError as ex:
        print(ex)
    if len(airports) == 0:
        return ["NEED", "CONFIG"]
    return airports


def tuple_to_hex_str(x):
    return "".join([f"{a:x}" for a in x])


def write_leds(leds_changed=False):
    if not pix:
        return

    tenth = time.monotonic() % 10

    for led_n in range(0, len(airportlist)):
        ll = ledstate[led_n]
        icao = airportlist[led_n]
        # led_n = aa.get("led_n", -1)
        # print(led_n, icao)

        blink_interval = ll.get("blink_interval", 0)
        # blink interval is 1/nth of 10 seconds. In other words, '4' will divide 10 seconds by 4
        # and blink on the truthy intervals (0-2.5, 5-7.5). Don't use odd numbers if you want a
        # smooth blink.

        curr_color = tuple_to_hex_str(ll["actual_color"])
        if blink_interval > 0:
            tenth_interval = 10.0 / blink_interval
            blink_on = tenth % (tenth_interval * 2) > tenth_interval
            c = ll["base_color"]

            if not blink_on:
                c = (0, 0, 0)

            pix[led_n] = c
            ll["actual_color"] = c

            new_color = tuple_to_hex_str(c)
            # print(f"blinky {tenth} {blink_on}")
            if curr_color != new_color:
                # print("new color", icao, led_n, c, blink_on)
                leds_changed = True
        else:
            pix[led_n] = ll["base_color"]
            ll["actual_color"] = ll["base_color"]
    if leds_changed:
        # print("leds changed")
        pix.show()


def is_valid_airport(x):
    if not x:
        return False
    if x == "NULL":
        return False
    if x.startswith("OFF"):
        return False
    if len(x) > 4:
        return False
    return True


memory_error_count = 0

def try_wx():
    global memory_error_count
    global requests
    feed_watchdog()
    airport_ids = [x for x in airportlist if is_valid_airport(x)]
    if not airport_ids:
        return

    url = f"{METAR_SERVICE_URL}?ids={','.join(airport_ids)}&boardid={BOARD_UID}&output=csv&debug=0x{debug_flags:x}"
    print(f"[*] METAR fetch: {url}")
    ret = None

    try:
        gc.collect()
        ret = requests.get(url)
        print(f"[OK] HTTP {ret.status_code}")
        clear_debug_flag(5)
    except adafruit_requests.OutOfRetries as ex:
        print(f"[FAIL] HTTP: Out of retries: {ex}")
        set_debug_flag(5)
        return
    except RuntimeError as ex:
        print(f"[FAIL] HTTP: RuntimeError: {ex}")
        set_debug_flag(5)
        return
    except MemoryError as ex:
        memory_error_count += 1
        print(f"[WARN] Memory error during METAR fetch")
        set_debug_flag(12)
        gc.collect()
        if memory_error_count > 5:
            memory_error_count = 1
            requests = None
            gc.collect()
            requests = adafruit_requests.Session(hpool, ssl.create_default_context())
        return
    except Exception as ex:
        print(f"[FAIL] HTTP: Unexpected error: {ex}")
        set_debug_flag(5)
        return

    try:
        for line in ret.text.strip().split('\n'):
            if not line:
                continue
            parts = line.split(',')
            if len(parts) < 5:
                continue
            icao = parts[0]
            if icao not in airportlist:
                continue
            cat = parts[1] or None
            r, g, b = [int(x) for x in parts[2].split('-')]
            ledc = (r, g, b)
            age_mins = int(parts[3]) if parts[3] else None
            stale = parts[4].strip() == '1'
            led_n = airportlist.index(icao)
            ledstate[led_n]["base_color"] = ledc
            ledstate[led_n]["actual_color"] = ledc
            airportwx[icao] = {
                "cat": cat,
                "last_update": datetime.now(),
                "age_mins": age_mins,
                "stale": stale,
            }
        clear_debug_flag(3)
    except Exception as ex:
        print(f"[FAIL] Response parse error: {ex}")
        set_debug_flag(3)

    if ret:
        try:
            ret.close()
        except:
            pass
    gc.collect()
    print("[OK] METAR fetch complete")
    write_leds(True)


def try_wifi():
    global requests, web
    feed_watchdog()
    did_reconnect = False
    if not wifi.radio.connected:
        ssid = os.getenv("CIRCUITPY_WIFI_SSID", "")
        pw = os.getenv("CIRCUITPY_WIFI_PASSWORD", "")

        if not ssid:
            print("[WARN] WiFi SSID not configured in settings.toml")
            set_debug_flag(2)
            pixel.fill((50, 0, 0))  # dim red
            oled_write("No WiFi config", "Check settings.toml")
            time.sleep(30)
            return

        print(f"[*] WiFi: Attempting {ssid}")

        try:
            wifi.radio.connect(ssid, pw, timeout=30)
            global wifi_ssid
            wifi_ssid = ssid
            did_reconnect = True
            print(f"[OK] WiFi connected to {ssid}")
            # Clear LED when WiFi reconnects (keep debug flag though)
            if pixel:
                pixel.fill((0, 55, 0))  # dim green
        except ConnectionError as ex:
            print(f"[FAIL] WiFi connection failed: {ex}")
            set_debug_flag(9)  # WiFi connect failed
            pixel.fill((50, 0, 0))  # dim red
            oled_write("WiFi failed", str(ex)[:20])
            time.sleep(30)
            return

    if debug > 4:
        print(f"[OK] WiFi IP: {wifi.radio.ipv4_address}")

    # Set DNS with IPv6 error detection
    try:
        if len(os.getenv("OVERRIDE_DNS", "")) > 0:
            wifi.radio.ipv4_dns = ipaddress.ip_address(os.getenv("OVERRIDE_DNS", ""))
        else:
            wifi.radio.ipv4_dns = ipaddress.ip_address("1.1.1.1")
    except (OSError, AttributeError) as ex:
        error_str = str(ex).lower()
        if "ipv6" in error_str or "address family" in error_str or "not supported" in error_str:
            print(f"[FAIL] IPv6 incompatibility detected: {ex}")
            set_debug_flag(13)  # IPv6 error
            if pixel:
                pixel.fill((138, 43, 226))  # Blue-violet for IPv6 issue
            oled_write("IPv6 error", "ISP issue")
        else:
            print(f"[WARN] DNS setting failed: {ex}")

    print(f"[OK] WiFi: {wifi.radio.ipv4_address}, DNS: {wifi.radio.ipv4_dns}, flags: 0x{debug_flags:x}")

    if did_reconnect:
        print("[*] WiFi reconnected - reinitializing network state")
        try:
            requests = None
            gc.collect()
            requests = adafruit_requests.Session(hpool, ssl.create_default_context())
            print("[OK] Requests session reinitialized after reconnect")
        except Exception as ex:
            print(f"[WARN] Requests reinit failed: {ex}")
        if web is not None:
            web.restart(str(wifi.radio.ipv4_address))

    # Retry NTP if not yet synced (required before HTTPS/METAR fetches)
    global ntp
    if ntp is None:
        try:
            ntp = adafruit_ntp.NTP(hpool, tz_offset=0, socket_timeout=10)
            _ = ntp.datetime
            print("[OK] NTP synced")
        except Exception as e:
            print(f"[WARN] NTP retry failed: {e}")
            ntp = None

    oled_write()


def write_config(req, apts=None):
    try:
        storage.remount("/", readonly=False, disable_concurrent_write_protection=True)
    except RuntimeError as ex:
        print(
            type(ex),
            str(ex),
            type(ex.args),
            len(ex.args),
            re.match("Cannot remount.*?when visible via USB.*", ex.args[0]),
        )
        if len(ex.args) and re.match(
            "Cannot remount.*?when visible via USB.*", ex.args[0]
        ):
            print("matched write error")
            return adafruit_httpserver.Response(
                req, "Can't write config while chip is connected via USB to a computer."
            )
        return adafruit_httpserver.Response(req, f"Unknown error: {ex}")

    # apts = req.form_data.get("airports")
    print(f""" {type(apts)} // {apts} // """)
    with open("airports.txt", "w") as fa:
        fa.write(apts)
    # return config(req)


def init_led_string():
    global ledstate
    ledstate = [{}] * len(airportlist)

    for led_n in range(0, len(airportlist)):
        icao = airportlist[led_n]
        ledc = (75, 75, 50)
        if icao.startswith("OFF"):
            ledc = (0, 0, 0)
        ledstate[led_n] = {
            "base_color": ledc,
            "actual_color": ledc,
            "blink_interval": 0,
            "icao": airportlist[led_n],
        }
        if pix:
            pix[led_n] = ledc

        airportwx[icao] = {
            "cat": "",
            "last_update": datetime.now(),
            "report_time": None,
        }


class webserver:
    def __init__(self, server, ip_address):
        print(self, server, ip_address)
        self.errors = []
        self.page_header = """<html><head><style>
  form {margin:0;padding:0}
  input {margin:0;padding:0}
  div.wx { float:left; height: 20px; width: 20px; clear:both; }
  img { max-width: 96%; height: auto; width: 800px }
</style></head><body>"""

        self.navigation_string = """navigation: <a href="/">home</a> | <a href="/airports">airports</a> | <a href="/config">config</a> | <a href="/debug">debug</a> | <a href="/files">files</a><br/>"""
        self._wserver = server
        self._wserver.add_routes(
            [
                Route("/airports", GET, self.airports),
                Route("/", GET, self.show_root),
                Route("/files", GET, self.files),
                Route("/config", [GET, POST], self.config),
                Route("/debug/clear", GET, self.debug_clear),
                Route("/debug", GET, self.debug),
            ]
        )
        # self.server.start("0.0.0.0")
        # if no IP, we probably don't have wifi.
        print("IP: ", ip_address, type(ip_address))
        # yes, the ip can be the string "None"
        if ip_address and ip_address != "None":
            self._wserver.start(ip_address, WEB_PORT)

    def restart(self, ip_address):
        try:
            self._wserver.stop()
            print("[OK] HTTP server stopped for restart")
        except Exception as e:
            print(f"[WARN] HTTP server stop failed: {e}")
        if ip_address and ip_address != "None":
            try:
                self._wserver.start(ip_address, WEB_PORT)
                print(f"[OK] HTTP server restarted on {ip_address}:{WEB_PORT}")
            except Exception as e:
                print(f"[WARN] HTTP server restart failed: {e}")

    def poll(self):
        # print(f"calling poll on {self._wserver}")
        try:
            # host=None if the server is stopped
            if self._wserver.host:
                self._wserver.poll()
        except adafruit_httpserver.exceptions.ServerStoppedError:
            pass

    def airports(self, req):
        global airportlist
        global airportwx
        global led_state
        if req.query_params.get("led_n") and req.query_params.get("icao"):
            led_n = int(req.query_params["led_n"])
            icao = req.query_params.get("icao")
            aptlist = load_airports()
            aptlist[led_n] = icao
            if w_err := write_config(req, "\n".join(aptlist)):
                return w_err

            # reload
            airportwx = {}
            airportlist = load_airports()
            init_led_string()

        if blink_n := req.query_params.get("blink"):
            blink_n = int(blink_n)
            print(f"*********** blinky {blink_n}")

            for led_n in range(0, len(airportlist)):
                if blink_n == led_n:
                    ledstate[led_n]["blink_interval"] = 10
                else:
                    ledstate[led_n]["blink_interval"] = 0

        def body():
            yield f"""{self.page_header}{self.navigation_string}<table>"""

            for led_n in range(0, len(airportlist)):
                icao = airportlist[led_n]
                aw = airportwx.get(icao, {})

                metar_age = ""
                age_mins = aw.get("age_mins")
                if age_mins is not None:
                    metar_age = f"{age_mins} mins"

                blinking = ""
                if ledstate[led_n].get("blink_interval", 0) > 0:
                    blinking = "BLINKING"

                ac = ledstate[led_n].get("base_color")
                # an = aw.get("led_n")
                if ac and len(ac) >= 2:
                    acolor = f"{ac[0]:02x}{ac[1]:02x}{ac[2]:02x}"
                yield f"""
    <tr>"""
                # <div style="margin-bottom: 2px">"""
                # <font color=\"#{acolor}\" size=20>&#9632;</font>"
                # astr += f"""<div style="float:left; height: 20px; width: 20px; background-color:{acolor}; clear:both"></div>"""
                yield f"""<td><div class="wx" style="background-color:{acolor}"></div></td>
    <td>{led_n}</td>
    <td>
    <span><form>
    <input type="hidden" name="led_n" value="{led_n}">
    <input type="textbox" size=6 name="icao" value="{icao}"">
    <input type="submit" value="update">
    </form></td>
    <td>{aw.get("cat")}</td>
    <td>{metar_age}</td>
    <td><a href="/airports?blink={led_n}">blink</a> {blinking}</td>
    </tr>
    """
            yield "</table>"

        return adafruit_httpserver.ChunkedResponse(
            req,
            body,
            content_type="text/html",
        )

    def show_root(self, req):

        #
    #errors: {errors}<br />
        def body():
            yield f"""
    {self.navigation_string}
    hello.<br />
    <br />
    temperature: {microcontroller.cpu.temperature}<br />
    frequency: {microcontroller.cpu.frequency}<br />
    voltage: {microcontroller.cpu.voltage}<br />
    memfree: {gc.mem_free()}<br />
    memerr: {memory_error_count}<br />
    error: {errors}<br />
    """

        return adafruit_httpserver.ChunkedResponse(
                req, body, content_type="text/html")

    def files(self, req):
        rstr = f"""{self.navigation_string}\nget mount<br />"""

        if req.query_params.get("f"):
            try:
                with open(req.query_params.get("f")) as f:
                    print("opened.")
                    return adafruit_httpserver.Response(
                        req,
                        f.read(),
                        content_type="text/plain",
                    )
            except:
                rstr += "open/read failed.<br />"

        rstr += dir_to_str("")



        #for ff in f.ilistdir(""):
        #    rstr += f"""<a href="/files?f={ff[0]}">{ff[0]}</a> {ff}<br />\n"""
        #    print(ff)
        #for ff in f.ilistdir("/sd"):
        #    rstr += f"""<a href="/files?f=/sd/{ff[0]}">/sd/{ff[0]}</a> {ff}<br />\n"""
        #    print(ff)
        return adafruit_httpserver.Response(
            req,
            f"""
{rstr}
""",
            content_type="text/html",
        )

    def config(self, req):
        if req.method == POST:
            err = write_config(req, req.form_data.get("airports"))
            if err:
                return err
            # return adafruit_httpserver.Response(req, f"""{}""")
            # posted_value = request.form_data.get("something")

        # reload
        airportwx = {}
        airportlist = load_airports()
        init_led_string()

        airport_txt = "\n".join(airportlist)
        return adafruit_httpserver.Response(
            req,
            f"""
<html>
<body>
{self.navigation_string}
<form action="" method="post" enctype="multipart/form-data">
<p>airports:</p>
<textarea rows=20 cols=40 name="airports">{airport_txt}</textarea><br />
<input type="submit">
</form>
</body>
</html>
""",
            content_type="text/html",
        )

    def debug(self, req):
        feed_watchdog()
        # Calculate uptime
        uptime_seconds = int(time.monotonic())
        uptime_days = uptime_seconds // 86400
        uptime_hours = (uptime_seconds % 86400) // 3600
        uptime_mins = (uptime_seconds % 3600) // 60
        uptime_secs = uptime_seconds % 60
        uptime_str = f"{uptime_days}d {uptime_hours}h {uptime_mins}m {uptime_secs}s"

        # Decode debug flags - bit by bit
        bits_status = []
        for bit in range(15):
            flag_name = ""
            is_set = bool(debug_flags & (1 << bit))

            if bit == 0:
                flag_name = "Display init failed"
            elif bit == 1:
                flag_name = "Display update failed"
            elif bit == 2:
                flag_name = "WiFi enable failed"
            elif bit == 3:
                flag_name = "METAR fetch failed"
            elif bit == 4:
                flag_name = "Reserved"
            elif bit == 5:
                flag_name = "HTTP request exception"
            elif bit == 6:
                flag_name = "NTP datetime failed"
            elif bit == 7:
                flag_name = "Reserved"
            elif bit == 8:
                flag_name = "Reserved"
            elif bit == 9:
                flag_name = "WiFi connect failed (3+)"
            elif bit == 10:
                flag_name = "Main loop exception"
            elif bit == 11:
                flag_name = "NVM write failed"
            elif bit == 12:
                flag_name = "Memory error"
            elif bit == 13:
                flag_name = "IPv6 incompatibility"
            elif bit == 14:
                flag_name = "NeoPixel initialization failed"

            status = "ON" if is_set else "off"
            bits_status.append(f"Bit {bit:2d} ({status:3s}): {flag_name}")

        bits_html = "<br/>".join(bits_status)

        # Check which error files exist
        error_files = ["errors0.txt", "errors.txt", "sd/errors.txt", "/sd/errorss.txt"]
        error_files_html = ""
        for ef in error_files:
            try:
                with open(ef):
                    error_files_html += f'<li><a href="/files?f={ef}">{ef}</a></li>\n'
            except:
                pass

        if not error_files_html:
            error_files_html = "<li>No error files found</li>"
        else:
            error_files_html = "<ul>\n" + error_files_html + "</ul>"

        return adafruit_httpserver.Response(
            req,
            f"""<html><body>{self.navigation_string}
<h2>Debug Status</h2>
<p>Firmware: {FIRMWARE_VERSION}</p>
<p>Uptime: {uptime_str}</p>
<p>Raw flags: 0x{debug_flags:08x} ({debug_flags})</p>
<pre>{bits_html}</pre>
<h3>Error Files</h3>
{error_files_html}
<p><a href="/debug/clear">Clear Flags</a></p>
<p><a href="/">Home</a></p>
</body></html>""",
            content_type="text/html",
        )

    def debug_clear(self, req):
        feed_watchdog()
        reset_debug_flags()
        return adafruit_httpserver.Response(
            req,
            f"""<html><body>{self.navigation_string}
<h2>Debug Flags Cleared</h2>
<p>All debug flags have been reset to 0x00000000.</p>
<p><a href="/debug">Back to Debug Status</a> | <a href="/">Home</a></p>
</body></html>""",
            content_type="text/html",
        )


pixel.fill((100, 100, 0))  # yellow

try_wifi()

# Early phone_home: fires before NTP so a DNS/NTP hang doesn't silence the device.
# boot_count > 1 here means there were failed boot attempts before this one.
phone_home()

# NTP: socket_timeout covers the UDP recv, but getaddrinfo (DNS) has no timeout in CircuitPython.
# If DNS hangs here, the watchdog resets the device and boot_count records the failed attempt.
try:
    ntp = adafruit_ntp.NTP(hpool, tz_offset=0, socket_timeout=10)
    _ = ntp.datetime
    print("[OK] NTP initialized and time synced")
except Exception as e:
    print(f"[WARN] NTP init failed: {e}")
    ntp = None

# hserver.start(str(wifi.radio.ipv4_address))
web = webserver(hserver, str(wifi.radio.ipv4_address))

# airportlist = ["KPDX", "KHIO", "KTTD", "7S3"]
airportlist = load_airports()
ledstate = [{}] * len(airportlist)

# hserver.serve_forever(str(wifi.radio.ipv4_address))

try:
    storage.remount(
        "/", readonly=False, disable_concurrent_write_protection=True
    )
    with open("/sd/boot.txt", "a") as f:
        f.write("hi.")
except:
  pass

print("hello world")
print(f"fstring test {airportlist}, {len(airportlist)}")
print("starting pix")
try:
    pix = neopixel.NeoPixel(
        board.A6, len(airportlist), pixel_order=neopixel.RGB, brightness=0.2
    )
    print(f"pix is: {pix}")
    init_led_string()
    pix.show()
except Exception as e:
    print(f"[FAIL] NeoPixel initialization failed: {e}")
    print("[WARN] LED string disabled")
    set_debug_flag(14)  # NeoPixel initialization failed
    pix = None
# print(board.A6)

# wifi.radio.start_ap(ssid="mmap-1", password="goldfish")

last_blinky = 0
last_wifi_check = 0

# -150 means it'll first check 30 seconds after startup. not elegant but it works.
last_wx_check = -150

pixel.fill((0, 55, 0))  # dim green

print("[OK] Main loop started")
while True:
    try:
        # Feed watchdog regularly
        feed_watchdog()

        # Process any waiting requests
        try:
            if web:
                web.poll()
        except adafruit_httpserver.exceptions.ServerStoppedError:
            pass
        except Exception as e:
            print(f"[WARN] Web poll error: {e}")

        t = time.monotonic()  # float, nanoseconds

        if t - last_wifi_check > 30:
            last_wifi_check = t
            gc.collect()
            try_wifi()
            oled_write(web=web)
            phone_home()

            # 30sec is a good interval to print errors
            if len(errors):
                print(f"[WARN] Errors recorded: {len(errors)}")

        if t - last_wx_check > 540:
            last_wx_check = t
            gc.collect()
            try_wx()
            write_leds()

        if t - last_blinky > 0.05:
            last_blinky = t
            write_leds()

    except OSError as error:
        print(f"[WARN] OSError in main loop: {error}")
        continue
    except Exception as ex:
        # Non-blocking exception throttling: only log once per 5 seconds
        current_time = time.monotonic()
        if current_time - last_exception_time > 5:
            print(f"[FAIL] Unhandled exception in main loop: {ex}")
            print(traceback.format_exception(ex))
            set_debug_flag(10)  # Main loop exception
            try:
                error = traceback.format_exception(ex)
                error_str = "".join(error) if isinstance(error, list) else str(error)
                write_error(error_str)
            except:
                pass
            last_exception_time = current_time
        continue
