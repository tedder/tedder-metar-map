

import requests
import re

# as a reminder, category:
# VFR > 3000ft & 5mi
# MVFR 1-3k and/or 3-5mi
# IFR 500 to <1000 and/or 1-3mi
# LIFR < 500 and/or < 1mi

# this is fully tested in the unit tests.
def flight_category(viz_miles, cloud_height):
  #print(type(cloud_height), cloud_height)
  if viz_miles < 1 or (cloud_height != None and cloud_height < 500):
    return 'LIFR'
  elif viz_miles < 3 or (cloud_height != None and cloud_height < 1000):
    return 'IFR'
  elif viz_miles < 5 or (cloud_height != None and cloud_height < 3000):
    return 'MVFR'
  else:
    return 'VFR'

def visib_miles(v):
  # already an int, cool!
  if isinstance(v, int): return v
  if isinstance(v, float): return v
  if v == "10+": return 10

  if m := re.match(r'\d+', v):
    print(f"matched: {m}")
    return int(v)
  else:
    print(f"UNKNOWN VISIBILITY: {v}")
  return v

class LED():
  pass

class LEDString():
  def __init__(self, led_length):
    self.leds = [LED()]*led_length


if __name__ == '__main__':
  airports = "KVUO,KPDX,K1W1,KTTD,KS48,K5S9,K4S9,KUAO,K7S3,KHIO,KMMV,KSLE,K7S5,KS12,KS30,KCVO,KONP,KS45,KPFC,KTMK,K3S7,K56S,KAST,K7W1,K2S9,KCLS,KTDO,KKLS,KW27,KSPB,K05S,KCZK,K4S2,KDLS,K35S,KS20,K39P,K55S,K4S6,KYKM,KZ99,KM94,K1S5,KS40,KM50,K9S9,KS33,KSHR".split(",")
  ledstring = LEDString(len(airports))
  print(ledstring.leds)

  ret = requests.get("https://aviationweather.gov/api/data/metar?format=json&ids=KVUO,KPDX,K1W1,KTTD,KS48,K5S9,K4S9,KUAO,K7S3,KHIO,KMMV,KSLE,K7S5,KS12,KS30,KCVO,KONP,KS45,KPFC,KTMK,K3S7,K56S,KAST,K7W1,K2S9,KCLS,KTDO,KKLS,KW27,KSPB,K05S,KCZK,K4S2,KDLS,K35S,KS20,K39P,K55S,K4S6,KYKM,KZ99,KM94,K1S5,KS40,KM50,K9S9,KS33,KSHR")
  metarlist = ret.json()

  for metar in metarlist:
    viz_miles = visib_miles(metar.get('visib'))
    cloud_height = metar.get('clouds')[0]['base']
    icao = metar.get('icaoId')
    fcat = flight_category(viz_miles, cloud_height)
    print(f"{icao}: {fcat:>4s} {viz_miles:4.1f} {cloud_height}")
    #break
