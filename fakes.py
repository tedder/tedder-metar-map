GET = 'GET'
POST = 'POST'

class adafruit_httpserver(object):

  def route(path, methods=[]):
    print(f"route: {path} // {methods}")
    def wrapper(path, /, methods=''):
      #return fn()
      return
    return wrapper

  @classmethod
  def poll(cls):
    return True
 
  @classmethod
  def Server(cls, pool, debug=False):
    print(f"haz class: {cls}")
    return cls

  def start(ip_str):
    pass

class wifi(object):
  class radio(object):
    connected = 1
    ipv4_address = '10.1.2.3'
    @classmethod
    def start_ap(cls, ssid, password):
      print(f"starting API with ssid={ssid}")

class board(object):
  A6 = 1

class neopixel(object):
  RGB = 'rgb'
  def __init__(self):
    print("NI")

  #@classmethod
  #class NeoPixel(cls, board, led_count, pixel_order=None, brightness=1.0):
  class NeoPixel(object):
    _pixels = []
    def __init__(self, board, led_count, pixel_order=None, brightness=1.0):
      print("NNI")
      self._pixels = [None]*led_count
      self.led_count = led_count

    def __getitem__(self, index: int):
      return '1'
    def __setitem__(self, index: int, val):
      print(f"setting {index} to {val}")
      self._pixels[index] = val
    def show(self):
      print(f"showing {self.led_count} pixels: {self._pixels}")
      for p in self._pixels:
        print(f" {p}")

    #board.A6, len(airportlist), pixel_order=neopixel.RGB, brightness=0.2
