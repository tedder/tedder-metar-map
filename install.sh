#!/bin/bash -exv

pip3 install circup --break-system-packages

circup install --upgrade neopixel adafruit_requests adafruit_datetime adafruit_httpserver adafruit_ssd1306 adafruit_ntp
cp settings.toml /Volumes/CIRCUITPY/settings.toml
cp airports.txt /Volumes/CIRCUITPY/airports.txt
cp font5x8.bin code.py /Volumes/CIRCUITPY/

