# circup install neopixel adafruit_requests adafruit_datetime adafruit_httpserver adafruit_ssd1306 adafruit_ntp
# https://github.com/adafruit/Adafruit_CircuitPython_NeoPixel
# wget https://raw.githubusercontent.com/adafruit/Adafruit_CircuitPython_framebuf/main/examples/font5x8.bin

import time
import busio
import struct
import re
import sys
import traceback
import ssl
import os
import gc
import ipaddress
import microcontroller
import supervisor
import board
import neopixel
import wifi
import socketpool
import storage
import adafruit_httpserver
from adafruit_httpserver import GET, POST, Route
import adafruit_ntp
import adafruit_ssd1306
import adafruit_requests
from adafruit_datetime import datetime

try:
    from watchdog import WatchDogMode
    _has_watchdog = True
except ImportError:
    _has_watchdog = False

# ── Constants ─────────────────────────────────────────────────────────────────

FIRMWARE_VERSION = "2.0.10"
TMM_VERSION = "v1.1.7"
METAR_SERVICE_URL = "http://fly.tedder.me/metar"
WEB_PORT = 80
MAX_ERRORS = 20

try:
    cp_version = ".".join(str(v) for v in sys.implementation.version[:3])
    USER_AGENT = f"tedder-metar-map/{FIRMWARE_VERSION} circuitpython/{cp_version}"
except Exception:
    USER_AGENT = f"tedder-metar-map/{FIRMWARE_VERSION} circuitpython/unknown"

if sys.implementation.version[0] < 10:
    try:
        import neopixel, board
        neopixel.NeoPixel(board.NEOPIXEL, 1).fill((255, 80, 0))
    except Exception:
        pass
    raise RuntimeError(f"CircuitPython 10.0+ required, got {cp_version}")

# ── Global state ──────────────────────────────────────────────────────────────

debug = 1
debug_flags = 0
ntp = None
BOARD_UID = ""
memory_error_count = 0
last_exception_time = 0
watchdog_enabled = False
airportlist = []
airportwx = {}
ledstate = []
errors = []
web = None
pix = None
display = None
hpool = None
hserver = None
requests = None
pixel = None

# Debug flags (32-bit bitmask, persistent in NVM bytes 0-3)
# Bit 0:  0x0001 = Display init failed
# Bit 1:  0x0002 = Display update failed
# Bit 2:  0x0004 = WiFi enable failed
# Bit 3:  0x0008 = METAR fetch failed
# Bit 5:  0x0020 = HTTP request exception
# Bit 6:  0x0040 = NTP datetime failed
# Bit 9:  0x0200 = WiFi connect failed (3+ attempts)
# Bit 10: 0x0400 = Main loop exception
# Bit 11: 0x0800 = NVM write failed
# Bit 12: 0x1000 = Memory error
# Bit 13: 0x2000 = IPv6 error/incompatibility
# Bit 14: 0x4000 = NeoPixel initialization failed
# Bit 15: 0x8000 = Watchdog reset (set on startup if previous reset was watchdog)

# ── Functions ─────────────────────────────────────────────────────────────────

def set_debug_flag(bit):
    global debug_flags
    debug_flags |= (1 << bit)
    try:
        microcontroller.nvm[0:4] = struct.pack("<I", debug_flags)
    except Exception as e:
        print(f"[WARN] NVM write failed: {e}")

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
    except Exception:
        pass

def add_error(errstr):
    global errors
    errors.append(errstr)
    if len(errors) > MAX_ERRORS:
        errors = errors[-MAX_ERRORS:]

def feed_watchdog():
    if watchdog_enabled:
        try:
            microcontroller.watchdog.feed()
        except Exception:
            pass

def write_error(errstr):
    try:
        with open("errors0.txt", "a") as f:
            f.write("e0\n")
            f.write(errstr)
            f.write("\n--\n")
            return
    except Exception as ex:
        print("couldn't write to error file: ", traceback.format_exception(ex))

    try:
        storage.remount("/", readonly=False, disable_concurrent_write_protection=True)
    except Exception:
        pass
    try:
        with open("errors.txt", "a") as f:
            f.write("e1\n")
            f.write(errstr)
            f.write("\n--\n")
    except Exception as ex:
        print("couldn't write to error file: ", traceback.format_exception(ex))

def dir_to_str(path, indent=0):
    ret = ""
    idt = "&nbsp;" * (indent * 4)
    f = storage.getmount("/")
    for ff in f.ilistdir(path):
        fullpath = path + "/" + ff[0]
        ftype = ff[1]
        if ftype & 16384:
            ret += f"""{idt}<b>{fullpath}</b><br />\n"""
            ret += dir_to_str(fullpath, indent=indent + 1)
        elif ftype & 32768:
            ret += f"""{idt}<a href="/files?f={fullpath}">{ff[0]}</a> {ff[3]}<br />\n"""
        else:
            ret += f"""{idt}{ff[0]} ????type???? {ff}<br />\n"""
    return ret

def get_now_struct():
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
        return time.mktime(nowstruct)
    return None

_error = ""
def oled_write(line3="", error=None, web=None):
    global _error
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

        line1 = f"IP: {wifi.radio.ipv4_address}" if wifi.radio.connected else "NO WIFI"

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
        blink_interval = ll.get("blink_interval", 0)
        curr_color = tuple_to_hex_str(ll["actual_color"])
        if blink_interval > 0:
            tenth_interval = 10.0 / blink_interval
            blink_on = tenth % (tenth_interval * 2) > tenth_interval
            c = ll["base_color"] if blink_on else (0, 0, 0)
            pix[led_n] = c
            ll["actual_color"] = c
            if curr_color != tuple_to_hex_str(c):
                leds_changed = True
        else:
            pix[led_n] = ll["base_color"]
            ll["actual_color"] = ll["base_color"]
    if leds_changed:
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
        ret = requests.get(url, timeout=30)
        print(f"[OK] HTTP {ret.status_code}")
        clear_debug_flag(5)
    except adafruit_requests.OutOfRetries as ex:
        print(f"[FAIL] HTTP: Out of retries: {ex}")
        set_debug_flag(5)
        requests = None
        return
    except RuntimeError as ex:
        print(f"[FAIL] HTTP: RuntimeError: {ex}")
        set_debug_flag(5)
        requests = None
        return
    except MemoryError:
        memory_error_count += 1
        print(f"[WARN] Memory error during METAR fetch")
        set_debug_flag(12)
        gc.collect()
        requests = None
        return
    except Exception as ex:
        print(f"[FAIL] HTTP: Unexpected error: {ex}")
        set_debug_flag(5)
        requests = None
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
        except Exception:
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
            if pixel:
                pixel.fill((50, 0, 0))
            oled_write("No WiFi config")
            time.sleep(30)
            return

        print(f"[*] WiFi: Attempting {ssid}")
        try:
            wifi.radio.connect(ssid, pw, timeout=30)
            did_reconnect = True
            print(f"[OK] WiFi connected to {ssid}")
            if pixel:
                pixel.fill((0, 55, 0))
        except ConnectionError as ex:
            print(f"[FAIL] WiFi connection failed: {ex}")
            set_debug_flag(9)
            if pixel:
                pixel.fill((50, 0, 0))
            oled_write("WiFi failed")
            time.sleep(30)
            return

    if debug > 4:
        print(f"[OK] WiFi IP: {wifi.radio.ipv4_address}")

    # CP 9.2+ fixed IPv6 RDNSS overwriting DHCP DNS, so prefer DHCP-provided DNS.
    # Fall back to 1.1.1.1 only if DHCP didn't provide one, or use OVERRIDE_DNS if set.
    override = os.getenv("OVERRIDE_DNS", "")
    if override:
        try:
            wifi.radio.ipv4_dns = ipaddress.ip_address(override)
        except Exception as ex:
            print(f"[WARN] DNS override failed: {ex}")
    elif wifi.radio.ipv4_dns is None:
        try:
            wifi.radio.ipv4_dns = ipaddress.ip_address("1.1.1.1")
        except Exception as ex:
            print(f"[WARN] DNS fallback failed: {ex}")

    print(f"[OK] WiFi: {wifi.radio.ipv4_address}, DNS: {wifi.radio.ipv4_dns}, flags: 0x{debug_flags:x}")

    if did_reconnect or requests is None:
        print("[*] WiFi reconnected or requests lost - reinitializing network state")
        try:
            requests = None
            gc.collect()
            requests = adafruit_requests.Session(hpool, ssl.create_default_context())
            print("[OK] Requests session reinitialized")
        except Exception as ex:
            print(f"[WARN] Requests reinit failed: {ex}")
        if web is not None:
            web.restart(str(wifi.radio.ipv4_address))

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
        print(type(ex), str(ex))
        if len(ex.args) and re.match("Cannot remount.*?when visible via USB.*", ex.args[0]):
            print("matched write error")
            return adafruit_httpserver.Response(
                req, "Can't write config while chip is connected via USB to a computer."
            )
        return adafruit_httpserver.Response(req, f"Unknown error: {ex}")

    print(f""" {type(apts)} // {apts} // """)
    with open("airports.txt", "w") as fa:
        fa.write(apts)

def init_led_string():
    global ledstate
    ledstate = [{}] * len(airportlist)
    for led_n in range(0, len(airportlist)):
        icao = airportlist[led_n]
        ledc = (0, 0, 90)   # dim blue = waiting for first fetch
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
            "age_mins": None,
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
        self._wserver.add_routes([
            Route("/airports", GET, self.airports),
            Route("/", GET, self.show_root),
            Route("/files", GET, self.files),
            Route("/config", [GET, POST], self.config),
            Route("/debug/clear", GET, self.debug_clear),
            Route("/debug", GET, self.debug),
        ])
        print("IP: ", ip_address, type(ip_address))
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
        try:
            if self._wserver.host:
                self._wserver.poll()
        except adafruit_httpserver.exceptions.ServerStoppedError:
            pass

    def airports(self, req):
        if req.query_params.get("led_n") and req.query_params.get("icao"):
            led_n = int(req.query_params["led_n"])
            icao = req.query_params.get("icao")
            aptlist = load_airports()
            aptlist[led_n] = icao
            if w_err := write_config(req, "\n".join(aptlist)):
                return w_err
            global airportwx, airportlist
            airportwx = {}
            airportlist = load_airports()
            init_led_string()

        if blink_n := req.query_params.get("blink"):
            blink_n = int(blink_n)
            for led_n in range(0, len(airportlist)):
                ledstate[led_n]["blink_interval"] = 10 if blink_n == led_n else 0

        def body():
            yield f"""{self.page_header}{self.navigation_string}<table>"""
            for led_n in range(0, len(airportlist)):
                icao = airportlist[led_n]
                aw = airportwx.get(icao, {})
                metar_age = ""
                age_mins = aw.get("age_mins")
                if age_mins is not None:
                    metar_age = f"{age_mins} mins"
                blinking = "BLINKING" if ledstate[led_n].get("blink_interval", 0) > 0 else ""
                ac = ledstate[led_n].get("base_color")
                acolor = f"{ac[0]:02x}{ac[1]:02x}{ac[2]:02x}" if ac and len(ac) >= 3 else "000000"
                yield f"""
    <tr>
    <td><div class="wx" style="background-color:#{acolor}"></div></td>
    <td>{led_n}</td>
    <td><span><form>
    <input type="hidden" name="led_n" value="{led_n}">
    <input type="textbox" size=6 name="icao" value="{icao}">
    <input type="submit" value="update">
    </form></td>
    <td>{aw.get("cat")}</td>
    <td>{metar_age}</td>
    <td><a href="/airports?blink={led_n}">blink</a> {blinking}</td>
    </tr>"""
            yield "</table>"

        return adafruit_httpserver.ChunkedResponse(req, body, content_type="text/html")

    def show_root(self, req):
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
        return adafruit_httpserver.ChunkedResponse(req, body, content_type="text/html")

    def files(self, req):
        rstr = f"""{self.navigation_string}\nget mount<br />"""
        if req.query_params.get("f"):
            try:
                with open(req.query_params.get("f")) as f:
                    return adafruit_httpserver.Response(req, f.read(), content_type="text/plain")
            except Exception:
                rstr += "open/read failed.<br />"
        rstr += dir_to_str("")
        return adafruit_httpserver.Response(req, f"\n{rstr}\n", content_type="text/html")

    def config(self, req):
        if req.method == POST:
            err = write_config(req, req.form_data.get("airports"))
            if err:
                return err
        global airportwx, airportlist
        airportwx = {}
        airportlist = load_airports()
        init_led_string()
        airport_txt = "\n".join(airportlist)
        return adafruit_httpserver.Response(req, f"""
<html><body>
{self.navigation_string}
<form action="" method="post" enctype="multipart/form-data">
<p>airports:</p>
<textarea rows=20 cols=40 name="airports">{airport_txt}</textarea><br />
<input type="submit">
</form>
</body></html>""", content_type="text/html")

    def debug(self, req):
        feed_watchdog()
        uptime_seconds = int(time.monotonic())
        uptime_str = f"{uptime_seconds//86400}d {(uptime_seconds%86400)//3600}h {(uptime_seconds%3600)//60}m {uptime_seconds%60}s"
        flag_names = {
            0: "Display init failed", 1: "Display update failed",
            2: "WiFi enable failed", 3: "METAR fetch failed",
            5: "HTTP request exception", 6: "NTP datetime failed",
            9: "WiFi connect failed (3+)", 10: "Main loop exception",
            11: "NVM write failed", 12: "Memory error",
            13: "IPv6 incompatibility", 14: "NeoPixel init failed",
            15: "Watchdog reset",
        }
        bits_status = []
        for bit in range(16):
            is_set = bool(debug_flags & (1 << bit))
            status = "ON" if is_set else "off"
            name = flag_names.get(bit, "Reserved")
            bits_status.append(f"Bit {bit:2d} ({status:3s}): {name}")
        bits_html = "<br/>".join(bits_status)
        error_files_html = ""
        for ef in ["errors0.txt", "errors.txt"]:
            try:
                with open(ef):
                    error_files_html += f'<li><a href="/files?f={ef}">{ef}</a></li>\n'
            except Exception:
                pass
        error_files_html = "<ul>\n" + error_files_html + "</ul>" if error_files_html else "<li>No error files found</li>"
        return adafruit_httpserver.Response(req, f"""<html><body>{self.navigation_string}
<h2>Debug Status</h2>
<p>Firmware: {FIRMWARE_VERSION}</p>
<p>Uptime: {uptime_str}</p>
<p>Raw flags: 0x{debug_flags:08x} ({debug_flags})</p>
<pre>{bits_html}</pre>
<h3>Error Files</h3>
{error_files_html}
<p><a href="/debug/clear">Clear Flags</a></p>
<p><a href="/">Home</a></p>
</body></html>""", content_type="text/html")

    def debug_clear(self, req):
        feed_watchdog()
        reset_debug_flags()
        return adafruit_httpserver.Response(req, f"""<html><body>{self.navigation_string}
<h2>Debug Flags Cleared</h2>
<p>All debug flags have been reset to 0x00000000.</p>
<p><a href="/debug">Back to Debug Status</a> | <a href="/">Home</a></p>
</body></html>""", content_type="text/html")


# ── Hardware initialization ───────────────────────────────────────────────────

# NVM: read debug_flags (bytes 0-3)
try:
    debug_flags = struct.unpack("<I", microcontroller.nvm[0:4])[0]
except Exception as e:
    print(f"[WARN] NVM read failed ({e}), clearing NVM")
    try:
        microcontroller.nvm[0:4] = struct.pack("<I", 0)
    except Exception:
        pass
    debug_flags = 0

# Watchdog
if _has_watchdog:
    try:
        supervisor.set_next_code_file("code.py")
        microcontroller.watchdog.timeout = 3600
        microcontroller.watchdog.mode = WatchDogMode.RESET
        watchdog_enabled = True
        print("[OK] Watchdog enabled (3600s)")
    except Exception as e:
        print(f"[WARN] Watchdog setup failed: {e}")

# Board UID
try:
    BOARD_UID = microcontroller.cpu.uid.hex()[:12]
except Exception:
    BOARD_UID = ""

# Reset reason — set watchdog debug flag if applicable
try:
    _reset_reason = str(supervisor.runtime.reset_reason)
except Exception:
    try:
        _reset_reason = str(microcontroller.cpu.reset_reason)
    except Exception:
        _reset_reason = "unknown"
print(f"[INFO] Reset reason: {_reset_reason}")
if "watchdog" in _reset_reason.lower():
    set_debug_flag(15)

# Board status NeoPixel
try:
    pixel = neopixel.NeoPixel(board.NEOPIXEL, 1)
    pixel.fill((0, 0, 90))
    print("[OK] NeoPixel initialized")
except Exception as e:
    print(f"[WARN] NeoPixel init failed: {e}")
time.sleep(10)

# I2C
i2c = None
try:
    i2c = busio.I2C(board.SCL, board.SDA, frequency=400000)
    print("[OK] I2C initialized")
except RuntimeError as e:
    print(f"[WARN] I2C init failed: {e}")

# Display
if i2c:
    if i2c.try_lock():
        try:
            print(f"[OK] I2C devices found: {i2c.scan()}")
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
else:
    print("[WARN] I2C not available, display disabled")

# SocketPool
try:
    hpool = socketpool.SocketPool(wifi.radio)
    print("[OK] SocketPool initialized")
except Exception as e:
    print(f"[FAIL] SocketPool init failed: {e}")

# HTTP server
try:
    hserver = adafruit_httpserver.Server(hpool, debug=False)
    print("[OK] HTTP server initialized")
except Exception as e:
    print(f"[FAIL] HTTP server init failed: {e}")

# Requests session
try:
    requests = adafruit_requests.Session(hpool, ssl.create_default_context())
    print("[OK] Requests session initialized")
except Exception as e:
    print(f"[FAIL] Requests init failed: {e}")
    set_debug_flag(5)

# ── Boot sequence ─────────────────────────────────────────────────────────────

if pixel:
    pixel.fill((100, 100, 0))  # yellow

try_wifi()

try:
    ntp = adafruit_ntp.NTP(hpool, tz_offset=0, socket_timeout=10)
    _ = ntp.datetime
    print("[OK] NTP initialized and time synced")
except Exception as e:
    print(f"[WARN] NTP init failed: {e}")
    ntp = None

web = webserver(hserver, str(wifi.radio.ipv4_address))

airportlist = load_airports()
ledstate = [{}] * len(airportlist)

try:
    storage.remount("/", readonly=False, disable_concurrent_write_protection=True)
    with open("/sd/boot.txt", "a") as f:
        f.write("hi.")
except Exception:
    pass

print(f"[INFO] Airport list: {airportlist} ({len(airportlist)} airports)")

try:
    pix = neopixel.NeoPixel(board.A6, len(airportlist), pixel_order=neopixel.RGB, brightness=0.2)
    print(f"[OK] LED strip initialized: {pix}")
    init_led_string()
    pix.show()
except Exception as e:
    print(f"[FAIL] NeoPixel initialization failed: {e}")
    set_debug_flag(14)

last_blinky = 0
last_wifi_check = 0
last_wx_check = -495  # triggers first fetch ~45s after startup

if pixel:
    pixel.fill((0, 55, 0))  # dim green

# ── Main loop ─────────────────────────────────────────────────────────────────

print("[OK] Main loop started")
while True:
    try:
        feed_watchdog()

        try:
            if web:
                web.poll()
        except adafruit_httpserver.exceptions.ServerStoppedError:
            pass
        except Exception as e:
            print(f"[WARN] Web poll error: {e}")

        t = time.monotonic()

        if t - last_wifi_check > 30:
            last_wifi_check = t
            gc.collect()
            try_wifi()
            oled_write(web=web)
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
        current_time = time.monotonic()
        if current_time - last_exception_time > 5:
            print(f"[FAIL] Unhandled exception in main loop: {ex}")
            print(traceback.format_exception(ex))
            set_debug_flag(10)
            try:
                error_str = "".join(traceback.format_exception(ex))
                write_error(error_str)
            except Exception:
                pass
            last_exception_time = current_time
        continue
