import flask

GET = "GET"
POST = "POST"


# class Route:
#    def __init__(self, path, methods=[GET], func=None, append_slash=False):
#        self.path = path
#        print(f"init methods: {type(methods)} {methods}")
#        if not isinstance(methods, list):
#          methods = [methods]
#        self.methods = methods
#        self.func = func
#        self.append_slash = append_slash
#

# class adafruit_httpserver(object):
#    def __init__(self):
#        self._server = flask.Flask(__name__)
#        self.server_started = False
#
#    #  def route(path, methods=[GET]):
#    #    print(f"route: {path} // {methods}")
#    #    self.server.add_url_rule(path, methods=methods, view_func=fn)
#    #    def wrapper(path, /, methods=''):
#    #      #return fn()
#    #      return
#    #    return wrapper
#
#    @classmethod
#    def poll(self):
#        return True
#
#    #@classmethod
#    class Server(object):
#        def __init__(self, pool, debug=False):
#            self._server = flask.Flask(__name__)
#            self.server_started = False
#            print(f"haz class: {self}")
#            #super().__init__()
#            #cls.__init__()
#
#        def add_routes(self, routes=[]):
#            for r in routes:
#                print(f"route: {r.path} // {r.methods} {type(r.methods)}")
#                self._server.add_url_rule(
#                    r.path, methods=r.methods, view_func=r.func # , append_slash=r.append_slash
#                )
#
#        #@classmethod
#        def start(self, ip_str=0):
#            print(self, self._server, ip_str)
#            self._server.run() #debug=True)

import requests


class adafruit_requests(object):
    @staticmethod
    def Session(pool, ssl_context):
        return requests.session()


import socket


class socketpool(object):
    @staticmethod
    def SocketPool(wifi_radio):
        return socket


class wifi(object):
    class radio(object):
        connected = 1
        ipv4_address = "0.0.0.0"

        @classmethod
        def start_ap(cls, ssid, password):
            print(f"starting API with ssid={ssid}")


class board(object):
    A6 = 1


class neopixel(object):
    RGB = "rgb"

    def __init__(self):
        print("NI")

    # @classmethod
    # class NeoPixel(cls, board, led_count, pixel_order=None, brightness=1.0):
    class NeoPixel(object):
        _pixels = []

        def __init__(self, board, led_count, pixel_order=None, brightness=1.0):
            print("NNI")
            self._pixels = [None] * led_count
            self.led_count = led_count

        def __getitem__(self, index: int):
            return "1"

        def __setitem__(self, index: int, val):
            # print(f"setting {index} to {val}")
            self._pixels[index] = val

        def show(self):
            print(f"** showing {self.led_count} pixels: {self._pixels}")
            for p in self._pixels:
                print(f"- {p}")

        # board.A6, len(airportlist), pixel_order=neopixel.RGB, brightness=0.2


class microcontroller:
    class cpu:
        temperature = 1
        frequency = 60000000
        voltage = 2.0


import os


class storage:
    @staticmethod
    def ilistdir(path=""):
        os.listdir(path)

    @staticmethod
    def remount(path="", readonly=True, disable_concurrent_write_protection=False):
        pass
