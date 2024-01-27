# circup install neopixel adafruit_requests adafruit_datetime
# https://github.com/adafruit/Adafruit_CircuitPython_NeoPixel

print("hello")
# import adafruit_pixelbuf

import time

try:
    import adafruit_requests
except ModuleNotFoundError:
    import requests

    class adafruit_requests(object):
        @staticmethod
        def Session(pool, ssl_context):
            return requests.session()


try:
    from adafruit_datetime import datetime
except ModuleNotFoundError:
    from datetime import datetime

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
    import adafruit_httpserver
    from adafruit_httpserver import GET, POST
except NotImplementedError:

    class socketpool(object):
        @staticmethod
        def SocketPool(wifi_radio):
            return

    # from fakes import adafruit_httpserver
    # from fakes import GET, POST
    # from fakes import wifi
    from fakes import *

    pass


import ssl
import os
import microcontroller
import re
import digitalio as dio


print("hello startup")
last_wifi_check = 0
last_wx_check = 0

debug = 14
# debug 0 == stfu
# debug 10 == all the noise


hpool = socketpool.SocketPool(wifi.radio)
hserver = adafruit_httpserver.Server(hpool, debug=True)

requests = adafruit_requests.Session(hpool, ssl.create_default_context())


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
def flight_category(viz_miles, cloud_height):
    # print(type(cloud_height), cloud_height)
    if viz_miles < 1 or (cloud_height != None and cloud_height < 500):
        return "LIFR"
    elif viz_miles < 3 or (cloud_height != None and cloud_height < 1000):
        return "IFR"
    elif viz_miles < 5 or (cloud_height != None and cloud_height < 3000):
        return "MVFR"
    else:
        return "VFR"


def visib_miles(v):
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
        print(f"UNKNOWN VISIBILITY: {v}")
    return v


def write_led(icao, flight_cat, report_time):
    led_n = airportlist.index(icao)
    ledc = led_color(flight_cat)
    if debug > 1:
        print(f"{led_n}={icao}={flight_cat}={ledc}")
    pix[led_n] = led_color(flight_cat)

    # time: time.time.strftime("%Y-%m-%d %H:%M:%S")
    airportwx[icao] = {
        "cat": flight_cat,
        "last_update": datetime.now(),
        "report_time": report_time,
        "led_color": ledc,
    }


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
    viz_miles = visib_miles(metar.get("visib"))
    cloud_height = metar.get("clouds")[0]["base"]
    icao = metar.get("icaoId")
    fcat = flight_category(viz_miles, cloud_height)
    if debug > 2:
        print(f"{icao}: {fcat:>4s} {viz_miles:4.1f} {cloud_height}")

    write_led(icao, fcat, metar.get("reportTime"))


def try_wx():
    airport_ids = ",".join(airportlist)
    try:
        ret = requests.get(
            f"https://aviationweather.gov/api/data/metar?format=json&ids={airport_ids}"
        )
    except RuntimeError as ex:
        print(ex)
        return
    for metar in ret.json():
        if debug > 8:
            print(metar)
        process_airport(metar)
    # print(f"wx ret: {ret}")
    # print(f"wx json: {ret.json()}")


def try_wifi():
    if not wifi.radio.connected:
        ssid = os.getenv("CIRCUITPY_WIFI_SSID")
        pw = os.getenv("CIRCUITPY_WIFI_PASSWORD")
        print("connecting", ssid, pw)

        wifi.radio.connect(ssid, pw, timeout=30)
    if debug > 4:
        print("wifi: ", wifi.radio.ipv4_address)


def write_config(req):
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
            return adafruit_httpserver.Response(
                req, "Can't write config while chip is connected via USB to a computer."
            )
        return adafruit_httpserver.Response(req, f"Unknown error: {ex}")

    apts = req.form_data.get("airports")
    print(f""" {type(apts)} // {apts} // """)
    with open("airports.txt", "w") as fa:
        fa.write(apts)
    # return config(req)


def init_led_string():
    for icao in airportlist:
        led_n = airportlist.index(icao)
        ledc = (150, 150, 100)
        pix[led_n] = ledc
        airportwx[icao] = {
            "cat": "",
            "last_update": datetime.now(),
            "report_time": "",
            "led_color": ledc,
        }


@hserver.route("/airports")  # , ["GET"])
def show_airports(req):
    astr = ""
    print(airportlist)
    for a in airportlist:
        print(a)
        aw = airportwx.get(a, {})

        ac = aw.get("led_color")
        if len(ac) >= 2:
            acolor = f"{ac[0]:02x}{ac[1]:02x}{ac[2]:02x}"
        astr += f"""<div style="margin-bottom: 2px">"""
        # <font color=\"#{acolor}\" size=20>&#9632;</font>"
        astr += f"""<div style="float:left; height: 20px; width: 20px; background-color:{acolor}; clear:both"></div>"""

        astr += f""" {a} {aw.get("cat")} <!-- {aw.get("last_update")} """
        astr += (
            f"""{aw.get("report_time")} <font color=\"#{acolor}\">{acolor}</font> """
        )
        astr += f"""{aw.get("led_color")} --></div>\n"""

    return adafruit_httpserver.Response(
        req,
        f"""
airports:<br />
{astr}
""",
        content_type="text/html",
    )


@hserver.route("/")
def show_root(req):
    return adafruit_httpserver.Response(
        req,
        f"""
hello.

temperature: {microcontroller.cpu.temperature}
frequency: {microcontroller.cpu.frequency}
voltage: {microcontroller.cpu.voltage}

""",
    )


@hserver.route("/files")
def files(req):
    rstr = ""
    rstr += "get mount\n"
    f = storage.getmount("/")
    for ff in f.ilistdir(""):
        rstr += f"{ff}\n"
        print(ff)
    return adafruit_httpserver.Response(
        req,
        f"""
hello.
{rstr}
""",
    )


@hserver.route("/config", [GET, POST])
def config(req):
    if req.method == POST:
        err = write_config(req)
        if err:
            return err
        # return adafruit_httpserver.Response(req, f"""{}""")
        # posted_value = request.form_data.get("something")

    airportlist = load_airports()

    airport_txt = "\n".join(airportlist)
    return adafruit_httpserver.Response(
        req,
        f"""
<html>
<body>
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


try_wifi()
hserver.start(str(wifi.radio.ipv4_address))
# hserver.start("0.0.0.0")

# airportlist = ["KPDX", "KHIO", "KTTD", "7S3"]
airportlist = load_airports()

airportwx = {}

# hserver.serve_forever(str(wifi.radio.ipv4_address))

print("hello world")
print(f"fstring test {airportlist}")
print("starting pix")
pix = neopixel.NeoPixel(
    board.A6, len(airportlist), pixel_order=neopixel.RGB, brightness=0.2
)
print(f"pix is: {pix}")
init_led_string()
pix.show()
# print(board.A6)

wifi.radio.start_ap(ssid="mmap-1", password="goldfish")

while True:
    try:
        # Process any waiting requests
        hserver.poll()
        if time.time() - last_wifi_check > 30:
            last_wifi_check = time.time()
            try_wifi()

        if time.time() - last_wx_check > 180:
            last_wx_check = time.time()
            try_wx()
            pix.show()

    except OSError as error:
        print(error)
        continue
