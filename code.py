# circup install neopixel adafruit_requests adafruit_datetime adafruit_httpserver adafruit_ssd1306 adafruit_ntp
# https://github.com/adafruit/Adafruit_CircuitPython_NeoPixel
# wget https://raw.githubusercontent.com/adafruit/Adafruit_CircuitPython_framebuf/main/examples/font5x8.bin

# bootup: blue, yellow, green
# after 30sec, "apts: nn; mm mins" on second line

import time
import busio

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

TMM_VERSION = "v1.1.5"
WEB_PORT = 80
if sys.implementation.name.lower() == "cpython":
    from fakes import *
    from datetime import datetime

    WEB_PORT = 5000  # nonprivileged on a real OS
else:
    import adafruit_requests
    from adafruit_datetime import datetime
    import microcontroller

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

debug = 1
# debug 0 == stfu
# debug 10 == all the noise

# board's single pixel
pixel = neopixel.NeoPixel(board.NEOPIXEL, 1)
pixel.fill((0, 0, 90))  # light blue
time.sleep(10)


# print("pins: ", board.SCL, board.SDA)
# print(dir(board))

# i2c = busio.I2C(board.SCL, board.SDA)
# i2c = board.I2C()
# SCL1 is for the stemma/qwiic connector
# i2c = busio.I2C(board.SCL1, board.SDA1)
i2c = None
try:
    # i2c = board.STEMMA_I2C()
    i2c = busio.I2C(board.SCL, board.SDA, frequency=400000)
except RuntimeError:
    pass

if i2c and i2c.try_lock():
    isc = i2c.scan()
    print("isc", type(isc))
    for i in isc:
        print(type(i), i)
    i2c.unlock()
display = None
if i2c:
    try:
        display = adafruit_ssd1306.SSD1306_I2C(128, 32, i2c)
        display.fill(0)
        display.show()
    except ValueError:
        pass

# print("display: ", display)

hpool = socketpool.SocketPool(wifi.radio)
hserver = adafruit_httpserver.Server(hpool, debug=True)


ntp = adafruit_ntp.NTP(hpool, tz_offset=0)

requests = adafruit_requests.Session(hpool, ssl.create_default_context())


def get_now_struct():
    try:
        # print("ntp datetime", time.mktime(ntp.datetime) )
        # print("hhmm", ntp.datetime.tm_hour, ntp.datetime.tm_min)
        return ntp.datetime
    except socketpool.SocketPool.gaierror as ex:
        pass
    except OSError as ex:
        pass
    return


def get_now():
    nowstruct = get_now_struct()
    if nowstruct:
        return time.mktime(ntp.datetime)
    return


last_phone_home = 0


def phone_home():
    global last_phone_home
    if not wifi.radio.connected:
        return

    now_epoch = get_now()
    if last_phone_home and now_epoch and (now_epoch - last_phone_home) < 3600:
        return
    else:
        last_phone_home = now_epoch

    try:
        #ret =
        #requests.post(
        #    "https://api.tedder.me/metar-map/status",
            # data=f"""board_uid={str(microcontroller.Processor.uid)} version={TMM_VERSION}"""
            # ",
            # "airportlist": airportlist,
            # "airportwx": airportwx,
            # "ledstate": ledstate,
            # "errors": errors,
            # "ip": wifi.radio.ipv4_address,
            # "board_id": board.board_id,
        #)
        #print("post ret: ", ret)
        pass
    except Exception as ex:
        print("phonehome failed", ex)
        pass


# save state on the error string
_error = ""
def oled_write(line3="", line4="", error=None, web=None):
    global memory_error_count
    global _error
    global errors
    if not display:
        return

    if error:
        _error = error
        errors.append(error)
        if web:
            web.errors.append(error)
    if _error:
        line3 = _error[:30]
        line4 = _error[30:]

    if not line3:
        nowstruct = get_now_struct()
        if nowstruct:
            line3 = f"t: {nowstruct.tm_hour:02d}:{nowstruct.tm_min:02d}"
    if not line4:
        line4 = TMM_VERSION
        if memory_error_count > 0:
          line4 += f"; {memory_error_count} mec"

    oled_txt = ["NO WIFI", "", line3, line4]
    if wifi.radio.connected:
        oled_txt[0] = f"IP: {wifi.radio.ipv4_address}"

    oled_txt[1] = f"apts: {len(airportlist)}"

    now_epoch = get_now()
    newest_metar = 0
    for icao, aw in airportwx.items():
        # for led_n in range(0, len(airportlist)):
        # icao = airportlist[led_n]
        aw = airportwx.get(icao, {})
        metar_time = aw.get("report_time", None)
        if metar_time:
            # print("rt", metar_time, type(metar_time))
            newest_metar = max(newest_metar, metar_time)

    if now_epoch and newest_metar and newest_metar > 0:
        newest_metar_mins = (now_epoch - newest_metar) / 60
        oled_txt[1] += f"; {newest_metar_mins:.0f} mins"

    display.fill(0)
    display.text(oled_txt[0], 0, 0, 1)
    display.text(oled_txt[1], 0, 8, 1)
    display.text(oled_txt[2], 0, 16, 1)
    display.text(oled_txt[3], 0, 24, 1)
    display.show()


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


# as a reminder, category:
# VFR > 3000ft & 5mi
# MVFR 1-3k and/or 3-5mi
# IFR 500 to <1000 and/or 1-3mi
# LIFR < 500 and/or < 1mi


# this is fully tested in the unit tests.
# TODO: test with None values of viz_miles
# TODO: test with array of clouds
# aim 7-1-14, the ceiling is BKN or OVC. In other words, SKC (0/0), FEW (<=2/8), and SCT (<=4/8) can be ignored.
# OVX = vertical visibility/fog problems
#  curl --silent 'https://aviationweather.gov/api/data/metar?format=json&ids=' | jq ".[].clouds|.[].cover" | egrep -v "SCT|BKN|OVC|FEW|CLR|CAVOK|OVX"


def is_actual_cloud_layer(layer):
    # print("hello islayer", layer)
    cov = layer.get("cover")
    # print("cov", cov)
    if not cov:
        print(f"** no value for cover. layer: {layer}")
        return False
    if cov in ["SKC", "FEW", "SCT", "CLR", "CAVOK"]:
        # print("not cov")
        return False
    if cov in ["OVC", "BKN", "OVX"]:
        # ovx is nonstandard, it's basically "low vertical visiblity".
        return True
    print(f"** unknown cover value: {layer}")
    return False


def get_lowest_cloud_height(clouds: list):
    if not clouds or not len(clouds):
        return None

    # print("clouds: ", clouds)

    # circuitpython doesn't support the default option of the next function, this is cleaner.
    actual_cloud_layers = [layer for layer in clouds if is_actual_cloud_layer(layer)]
    first_actual_cloud_layer = None
    if actual_cloud_layers:
        first_actual_cloud_layer = actual_cloud_layers[0]
    # print(f"first layer: {first_actual_cloud_layer}")
    if not first_actual_cloud_layer:
        return None
    return first_actual_cloud_layer["base"]


def flight_category(viz_miles, clouds: list):
    cloud_height = get_lowest_cloud_height(clouds)

    # print(type(cloud_height), cloud_height)
    if (viz_miles != None and viz_miles < 1) or (
        cloud_height != None and cloud_height < 500
    ):
        return "LIFR"
    elif (viz_miles != None and viz_miles < 3) or (
        cloud_height != None and cloud_height < 1000
    ):
        return "IFR"
    elif (viz_miles != None and viz_miles < 5) or (
        cloud_height != None and cloud_height < 3000
    ):
        return "MVFR"
    else:
        return "VFR"


def visib_miles(v, icao=""):
    if v is None:
        print(f"VISIBILITY NONE {icao}")
        return v
    # already an int, cool!
    if isinstance(v, int):
        return v
    if isinstance(v, float):
        return v
    if v == "10+":
        return 10

    if m := re.match(r"\d+", v):
        print(f"matched: {m}")
        return int(v)
    else:
        print(f"UNKNOWN VISIBILITY: {v}  {icao}")
    return v


def tuple_to_hex_str(x):
    return "".join([f"{a:x}" for a in x])


def write_leds(leds_changed=False):
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


def led_color(flight_cat):
    if flight_cat == "VFR":
        return (0, 127, 0)  # green
    elif flight_cat == "MVFR":
        return (0, 0, 127)  # blue
    elif flight_cat == "IFR":
        return (127, 0, 0)  # red
    elif flight_cat == "LIFR":
        return (0, 127, 127)  # magenta
    # else:
    return (255, 255, 0)  # yellow


def process_airport(metar):
    icao = metar.get("icaoId")
    if debug > 8:
        print("metar: ", metar)
    viz_miles = visib_miles(metar.get("visib"), icao)
    if debug > 4:
        print("clouds: ", metar.get("clouds"))

    flight_cat = flight_category(viz_miles, metar.get("clouds", None))
    if debug > 6:
        print(f"{icao}: {fcat:>4s} {viz_miles:4.1f} {cloud_height}")

    # write_led(icao, fcat, metar.get("reportTime"))
    led_n = airportlist.index(icao)
    ledc = led_color(flight_cat)
    if debug > 4:
        print(f"{led_n}={icao}={flight_cat}={ledc}")

    airportwx[icao] = {
        "cat": flight_cat,
        "last_update": datetime.now(),
        "report_time": metar.get("obsTime", None),
    }
    print(f"{icao} led_n: {led_n} // lslen: {len(ledstate)}")
    ledstate[led_n]["base_color"] = ledc
    ledstate[led_n]["actual_color"] = ledc


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


BATCH_SIZE = 10

memory_error_count = 0

def try_wx():
    global memory_error_count
    global requests
    airport_ids = [x for x in airportlist if is_valid_airport(x)]

    # batch BATCH_SIZE at a time
    while True:
        if len(airport_ids) == 0:
            break
        this_airport_ids = ",".join(airport_ids[:BATCH_SIZE])
        airport_ids = airport_ids[BATCH_SIZE:]
        print("my ids", this_airport_ids)
        ret = None

        try:
            avurl = f"https://aviationweather.gov/api/data/metar?format=json&ids={this_airport_ids}"
            print(avurl)
            gc.collect()
            ret = requests.get(avurl)
        except adafruit_requests.OutOfRetries as ex:
            print("out of retries, no biggie.", ex)
            return
        except RuntimeError as ex:
            print("req error", ex)
            return
        except MemoryError as ex:
            memory_error_count += 1
            print(f"memory error at url {avurl}")
            gc.collect()
            if memory_error_count > 5:
                # reset to 1, not 0, that way we'll still indicate an error
                memory_error_count = 1
                requests = None
                gc.collect()
                requests = adafruit_requests.Session(hpool, ssl.create_default_context())

            return
        for metar in ret.json():
            if debug > 8:
                print(metar)
            process_airport(metar)

        gc.collect()
        # print(f"wx ret: {ret}")
        # print(f"wx json: {ret.json()}")
    write_leds(True)


def try_wifi():
    if not wifi.radio.connected:
        ssid = os.getenv("CIRCUITPY_WIFI_SSID", "nossid")
        pw = os.getenv("CIRCUITPY_WIFI_PASSWORD", "nopassword")
        print("connecting", ssid, pw)

        try:
            wifi.radio.connect(ssid, pw, timeout=30)
        except ConnectionError as ex:
            print("wifi failed, ex: ", ex)
            pixel.fill((50, 0, 0))  # dim red
            oled_write("wifi failed", str(ex))
            time.sleep(30)  # sleep, it'll implicitly try again
    if debug > 4:
        print("wifi: ", wifi.radio.ipv4_address)

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
        ledc = (150, 150, 100)
        if icao.startswith("OFF"):
            ledc = (0, 0, 0)
        ledstate[led_n] = {
            "base_color": ledc,
            "actual_color": ledc,
            "blink_interval": 0,
            "icao": airportlist[led_n],
        }
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

        self.navigation_string = """navigation: <a href="/">home</a> | <a href="/airports">airports</a> | <a href="/config">config</a> | <a href="/files">files</a><br/>"""
        self._wserver = server
        self._wserver.add_routes(
            [
                Route("/airports", GET, self.airports),
                Route("/", GET, self.show_root),
                Route("/files", GET, self.files),
                Route("/config", [GET, POST], self.config),
            ]
        )
        # self.server.start("0.0.0.0")
        # if no IP, we probably don't have wifi.
        print("IP: ", ip_address, type(ip_address))
        # yes, the ip can be the string "None"
        if ip_address and ip_address != "None":
            self._wserver.start(ip_address, WEB_PORT)

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

        now_epoch = get_now()

        def body():
            yield f"""{self.page_header}{self.navigation_string}<table>"""

            for led_n in range(0, len(airportlist)):
                #        a in airportlist:
                icao = airportlist[led_n]
                aw = airportwx.get(icao, {})

                metar_age = ""
                metar_time = aw.get("report_time", None)
                if metar_time and now_epoch:
                    age_mins = (now_epoch - metar_time) / 60
                    metar_age = f"{age_mins:.1f} mins"

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
        f = storage.getmount("/")
        for ff in f.ilistdir(""):
            rstr += f"{ff}<br />\n"
            print(ff)
        return adafruit_httpserver.Response(
            req,
            f"""
hello.
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


pixel.fill((100, 100, 0))  # yellow

try_wifi()
# hserver.start(str(wifi.radio.ipv4_address))
web = webserver(hserver, str(wifi.radio.ipv4_address))

# airportlist = ["KPDX", "KHIO", "KTTD", "7S3"]
airportlist = load_airports()
ledstate = [{}] * len(airportlist)

# hserver.serve_forever(str(wifi.radio.ipv4_address))

print("hello world")
print(f"fstring test {airportlist}, {len(airportlist)}")
print("starting pix")
pix = neopixel.NeoPixel(
    board.A6, len(airportlist), pixel_order=neopixel.RGB, brightness=0.2
)
print(f"pix is: {pix}")
init_led_string()
pix.show()
# print(board.A6)

# wifi.radio.start_ap(ssid="mmap-1", password="goldfish")

last_blinky = 0
last_wifi_check = 0

# -150 means it'll first check 30 seconds after startup. not elegant but it works.
last_wx_check = -150

pixel.fill((0, 55, 0))  # dim green


while True:
    try:
        # Process any waiting requests
        if web:
            web.poll()
        t = time.monotonic()  # float, nanoseconds

        if t - last_wifi_check > 30:
            last_wifi_check = t
            gc.collect()
            try_wifi()
            oled_write(web=web)
            phone_home()

        if t - last_wx_check > 180:
            last_wx_check = t
            gc.collect()
            # print("trying wx")
            try_wx()
            write_leds()

        if t - last_blinky > 0.05:
            last_blinky = t
            write_leds()

    except OSError as error:
        print(error)
        continue
    except Exception as ex:
        try:
            storage.remount(
                "/", readonly=False, disable_concurrent_write_protection=True
            )
        except:
            pass
        try:
            with open("errors.txt", "w+") as f:
                f.write(traceback.format_exception(ex))
        except:
            oled_write(error=str(traceback.format_exception(ex)), web=web)
            pixel.fill((255, 0, 0))  # red
            print("couldn't write to error file.")
            print(traceback.format_exception(ex))
        continue
