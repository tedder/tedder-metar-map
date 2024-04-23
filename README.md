## tedder's METAR map

There are many like it, but this one's mine.

Goals:
- reasonably sane code
- should run on a microcontroller (eg esp32)
- can possibly run on a Pi
-  should be able to run it locally for testing, no need to upload to a microcontroller every time.
- hopefully unit tests to catch regressions
- configure in text files or in a web server.
- easy for an end user to configure- this is why it uses text files vs toml or yaml or json.
- not too many custom configurations. just a dead-simple.
- 'phone home' so I can help friends with weird configurations

Future:
- use the light sensor on the carrier board
- calculate/approximate the LED power consumption so it can run off USB power
- fall back to 'wifi AP mode' on failed connect
- OTA updates
- regression tests in github actions

The v1 hardware for this is about $25 ($10ish for the carrier board, $12 for the qtpy). It's simpler than a Pi, it doesn't have as fussy of power requirements as a Pi, and it's much thinner, making it easier to wall-mount.

## how to build

## how to install

### bootstrap

[Install circuitpython](https://learn.adafruit.com/adafruit-qt-py-esp32-s2/circuitpython) on [qtpy esp32-s2](https://www.adafruit.com/product/5325):

1. press reset button
1. when RGB LED turns purple, press reset button again (about a half second later? almost a double-click)
1. note QTPYS2BOOT mounted folder on success
1. copy latest circuitpython uf2 to this folder
1. qtpy will auto-disconnect and remount as CIRCUITPY
1. run `install.sh` to copy the proper files





## troubleshooting

[circuitpython boot states](https://docs.circuitpython.org/en/8.2.x/README.html#:~:text=RGB%20status%20LED%20indicating%20CircuitPython%20state.):
* One green flash - code completed without error.
* Two red flashes - code ended due to an exception.
* Three yellow flashes - safe mode. May be due to CircuitPython internal error.

mu editor, mode->circuitpython, serial, CTRL-D in lower window, watch output
