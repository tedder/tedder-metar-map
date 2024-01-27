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
