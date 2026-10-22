# METAR map users guide

Your map has one light per airport. Every few minutes it asks a weather service for the latest
report (METAR) at each airport and colors that light by flight category.

| Color | Category | Ceiling / visibility |
|---|---|---|
| green | VFR | ceiling above 3,000 ft and visibility over 5 miles |
| blue | MVFR | ceiling 1,000–3,000 ft or visibility 3–5 miles |
| red | IFR | ceiling 500–999 ft or visibility 1–3 miles |
| magenta | LIFR | ceiling below 500 ft or visibility under 1 mile |
| dim grey | no current report | the airport isn't reporting, or the map's weather is old |
| off | | no airport on this light (a spacer or a legend light) |

The map needs only WiFi and power. Firmware updates install by themselves: the map checks
every 6 hours and restarts once when there is a new version. If a new version doesn't work,
the map goes back to the old one on its own.

## The controller board

The small board on the back (an Adafruit QT Py ESP32-S2) has:

- a **USB-C port**: power, and the only cable it needs;
- a **BOOT button**: the button next to the USB port;
- a **status light**: one small color LED on the board.

## First setup: connect it to WiFi

1. Plug the map in. If it doesn't know any WiFi yet (or can't find its network), all the map's
   lights turn **blue**, and the first light is **red**.
2. On your phone, open WiFi settings and join the network **`metarmap-XXXXXX`** (the last six
   characters differ per map). The first light turns **green** once your phone has joined.
3. A setup page opens by itself. If it doesn't, open a browser and go to `http://192.168.4.1/`.
4. Under **WiFi**, type your network name exactly (capital letters matter) and the password,
   and press **Save**. The map tries
   that network before saving; if the name or password is wrong it says so and nothing changes.
5. The map restarts and joins your WiFi. The lights fill in white, one by one, as it connects,
   then show the weather. The first weather appears within about a minute.

You can save up to three networks (the first plus two backups). The map stays on the network
it joined and moves to the next saved one after three failed reconnects.

The map does not support "captive" WiFi that needs a sign-in page (hotels, some guest networks),
or 5 GHz-only networks. Use a 2.4 GHz network with a normal password.

## The status page

On a phone or computer on the same WiFi, open **`http://metarmap.local/`**. (Some Android phones
and older Windows PCs don't support `.local` names; then use the map's address from your router's
list of devices.)

- **Weather**: how many airports are in each category, and when the map last fetched weather.
- **What the LEDs show**: brightness **−** and **+**, and LED tests.
- **Airports**: each light's airport, its category and the report's age, with a **Find** button
  that blinks just that light.
- **Configure**: WiFi, the airport list, night dimming and other settings.

## Saving changes: press BOOT first

To stop anyone else on your WiFi from changing the map, saving settings, WiFi or the airport list
needs a **short press of the BOOT button** first. One press unlocks saving for 15 minutes; the
status light turns **cyan** while it is unlocked. The Configure page shows whether saving is
unlocked and how long is left.

Brightness −/+, the LED tests and Find work without a press.

## Brightness and night dimming

- Brightness has nine steps: off, 3, 7, 15, 25, 40, 55, 75 and 100 %. New maps start at 25 %.
- Night dimming is on by default: from 22:00 to 07:00 (Pacific time) the map drops to 3 %. Set
  the hours, the night brightness and your time zone on the Configure page, or turn it off.
- During the night, pressing − or + on the status page cancels dimming until the morning.

## Changing the airports

On **Configure → Airports**, the list has one line per light, in the order the lights are wired:
line 1 is the first light after the controller board.

- An airport is its 3- or 4-character code: `KPDX`, `KTTD`, `S48`. US airports use the K prefix
  when they have one (`KPDX`, not `PDX`).
- `OFF` leaves a light dark (a spacer, or a light used for a legend).
- The same airport can be on more than one light.
- Lights past the end of the list stay off.

When you save, the map asks the weather service once and lists any codes that have no current
report. That catches typos; it also lists small airports that don't report weather at all.

To find out which line a light is, use **Find** on the Airports page: that light blinks white.

## What the lights mean when something is wrong

| Map lights | Meaning | What to do |
|---|---|---|
| the first few lights white, not moving on | starting up, or can't reach WiFi / the weather service | wait a minute; check the WiFi |
| those first lights **amber** | the WiFi router doesn't answer the map | move the router or the map closer, or restart the router |
| those first lights **red** | wrong WiFi password | hold BOOT 9 s and enter the password again |
| all lights blue, blinking | the saved WiFi has been unreachable for 5 minutes; setup network open, still retrying | fix the WiFi, or join `metarmap-XXXXXX` to set a new one |
| all airport lights dim grey | the map's weather is over an hour old | check the internet connection; it keeps trying, and restarts itself after 12 hours |

| Status light (on the board) | Meaning |
|---|---|
| slow green breathing | all good |
| amber breathing | weather is old |
| amber blinking | connecting to WiFi / no WiFi |
| blue | setup network open (blinking: the map is also still trying the saved WiFi) |
| cyan | saving unlocked |
| short blue flash | button press seen |

## Moving to a new WiFi network

**Hold BOOT for 9 seconds** (until the map lights turn blue). The setup network
`metarmap-XXXXXX` opens; join it and follow "First setup" above. Holding BOOT for 9 seconds
again while in setup mode restarts the map without changes.

If the map can't find its saved network for 5 minutes it opens the setup network by itself, so
after a move you can usually just wait, then join `metarmap-XXXXXX`.

## LED tests

On the status page, **LED test**:

- **Colors**: every light red, then green, then blue, then white. If red and green (or another
  pair) are swapped, the map's color order setting is wrong for its LED strip: change
  **Configure → LED string → Color order**. White that looks pink or blue usually means a weak
  power supply.
- **Order**: one white light walks from light 1 to the last. A light that stays dark, or a dot
  that jumps around, means a wiring problem between two lights.
- **Categories**: every light shows green, blue, red, magenta in turn.

## Restart and reset

- **Configure → Actions → Restart** restarts the map. So does unplugging it.
- **Reset settings to defaults** puts night dimming, timing, LED string, colors and the other
  settings back to their defaults. WiFi, the airport list, brightness and the update channel
  are kept.

## Privacy

Each weather request tells the weather service the map's board ID, firmware version and a few
health numbers (WiFi signal, uptime, error counts, and the map's address on your home network),
so problems can be spotted and fixed without a visit. The map sends nothing else and doesn't
listen to anything outside your home network.
