# 7-Segment LED Clock

A large wall or desk clock with four 7-segment digits built from addressable RGB LEDs, driven by an ESP8266. A DS3231 real-time clock keeps the time. The clock runs its own Wi-Fi hotspot with a small web page for setting the colour, brightness, 12/24-hour mode and daylight saving, and for syncing the time from your phone.

<!-- PHOTOS: add a photo of the finished clock here, e.g. ![The clock](docs/clock.jpg) -->

## Features

- **Colour and brightness:** set them from the web page. Changes fade smoothly into place.
- **Settings survive power cuts:** colour, 12/24-hour mode and DST are saved to the ESP8266's flash (LittleFS). To limit flash wear, a colour change is only saved after it has stayed put for 15 seconds.
- **Keeps time with no network:** the DS3231 RTC holds the time, so it doesn't need the internet.
- **Daylight saving:** applies US rules (second Sunday in March to first Sunday in November), and can be turned off.
- **One-tap time sync:** "Sync Device Time" sends your phone's current time to the clock.
- **Fade-in on power-up.**
- **Network discovery:** the clock announces itself over SSDP as "LED Clock".

## Settings page

![Settings page](docs/web-ui.png)

To open it:
1. Join the clock's Wi-Fi hotspot.
2. Browse to `http://192.168.4.1/`, the ESP8266's default hotspot address.

The page has three sliders (brightness, hue and saturation), the 12/24-hour and DST toggles, and the sync button.

## Hardware

| Part | Notes |
|---|---|
| ESP8266 board (e.g. Wemos D1 mini / NodeMCU) | |
| WS2812B (NeoPixel) LEDs | **3 per segment**, wired as two strips: |
| | - Hours strip on **GPIO 2** (D4): 1 colon LED + 2 digits = 43 LEDs |
| | - Minutes strip on **GPIO 13** (D7): 1 colon LED + 2 digits, sized for 45 LEDs |
| DS3231 RTC module | I²C on the default pins: SDA = GPIO 4 (D2), SCL = GPIO 5 (D1) |
| 5 V power supply | Sized for the LEDs at your chosen brightness |

Each digit is 7 segments × 3 LEDs. The segment order inside each digit is set by the `displayHourDigit…` / `displayMinuteDigit…` functions in `clock.ino`.

## Building

Arduino IDE with the **ESP8266 board package**, plus these libraries:

| Library | Version |
|---|---|
| FastLED | **3.6.0** (newer releases fail to compile with this sketch) |
| ArduinoJson | **5.x** (5.13.4 tested; the sketch uses the v5 API) |
| DS3231 by NorthernWidget | 1.2.0 |

Steps:
1. Copy `clock/secrets.example.h` to `clock/secrets.h` and set the hotspot name and password (8+ characters). `secrets.h` is git-ignored.
2. Upload `clock/clock.ino`.
3. Upload the `clock/data/` folder to LittleFS with the *ESP8266 LittleFS Data Upload* tool. This holds the settings page and the default `config.json`.

## Files

| Path | Purpose |
|---|---|
| `clock/clock.ino` | Firmware: LEDs, RTC, DST, web server, settings storage |
| `clock/data/index.html` | Settings page, served from LittleFS (Bootstrap is inlined, so it works offline) |
| `clock/data/config.json` | Default settings |
| `clock/secrets.example.h` | Template for the hotspot name and password |

## Web API

The settings page uses these endpoints, which you can also call directly:

| Method & path | Body / query | Does |
|---|---|---|
| `GET /config` | | Current colour, 12/24 mode |
| `PUT /color` | `?h=0-360&s=0-100&v=0-100` | Fade to a new colour |
| `GET`/`PUT /clock` | `12` or `24` | 12/24-hour mode |
| `GET`/`PUT /dst` | `1` or `0` | DST on/off |
| `PUT /time` | `?year&month&day&dow&hour&min&sec` | Set the RTC |

## Known issues

- The DST start and end dates are worked out from *today's* day of the week, not from the weekday of March 1 and November 1. So the switch can happen on the wrong day in some years.
- In 24-hour mode, during DST, the hour after 23:59 can show as `24` instead of `00`.
