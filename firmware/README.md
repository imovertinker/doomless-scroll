# Firmware

The code that goes on the ESP32. It joins your Wi-Fi, gets articles from the server in [`endpoint`](../endpoint), and draws them on the e-paper display.

This folder is an Arduino sketch, so the folder name and the `.ino` file name have to match. If you rename the folder, rename `firmware.ino` too.

## Wiring

I used an ESP32-S3 dev board, a Waveshare 7.5-inch black-and-white e-paper display (800×480) with the Waveshare e-Paper Driver HAT, and two rotary encoders. The display and both encoders run on 3.3V.

If you use different pins, change them in `hw.h`.

### E-paper Driver HAT

| HAT pin | ESP32-S3 |
| ------- | -------- |
| VCC     | 3V3      |
| GND     | GND      |
| DIN     | GPIO 11  |
| CLK     | GPIO 12  |
| CS      | GPIO 10  |
| DC      | GPIO 9   |
| RST     | GPIO 8   |
| BUSY    | GPIO 18  |
| PWR     | GPIO 17  |

### Left knob

| Encoder pin | ESP32-S3 |
| ----------- | -------- |
| +           | 3V3      |
| GND         | GND      |
| CLK         | GPIO 38  |
| DT          | GPIO 39  |
| SW          | GPIO 40  |

### Right knob

| Encoder pin | ESP32-S3 |
| ----------- | -------- |
| +           | 3V3      |
| GND         | GND      |
| CLK         | GPIO 41  |
| DT          | GPIO 42  |
| SW          | GPIO 47  |

### Power

| From                  | To        |
| --------------------- | --------- |
| USB 5V                | Switch, outer pin |
| Switch, middle pin    | ESP32 5V  |
| USB GND               | ESP32 GND |

Use a USB-A port on the power bank to power the project, and keep the USB-C port for charging the power bank. Only the 5V wire goes through the switch. If on and off end up backwards, move the 5V wire to the switch's other outer pin.

## Arduino setup

1. Open `firmware.ino` in the Arduino IDE.
2. Install ESP32 board support through Boards Manager (version 3 or newer), then select `ESP32S3 Dev Module`.
3. Install two libraries through Library Manager:
   - [GxEPD2](https://github.com/ZinggJM/GxEPD2)
   - Adafruit GFX Library

Wi-Fi, HTTP, and JSON parsing all come with the ESP32 board package.

## Wi-Fi and server address

Fill in `secrets.h`:

```cpp
#define WIFI_SSID      "your-wifi"
#define WIFI_PASSWORD  "your-password"

// The computer running the endpoint and its port
#define API_HOST       "192.168.1.42:8787"
```

`API_HOST` is the local network address of the computer running the server, without `http://` in front. Don't use `127.0.0.1` or `localhost`. On the ESP32 that means the ESP32 itself.

The ESP32 and the computer need to be on the same Wi-Fi.

## Upload

Select your port and upload. The serial monitor runs at 115200 baud and prints what the sketch is doing, which helps a lot if something isn't working.

The first load can take a while, because the server is downloading and converting all the articles. If the fetch fails, the screen shows an error. Hold either knob to try again.

## Controls

- Left knob: moves the star on the list. Click to open a story. While you're reading, turning it jumps to the previous or next story, and a click returns to the list.
- Right knob: turns pages.
- Hold either knob: re-fetches the edition.

The controls are handled in `service()` at the bottom of `news.cpp`, if you want to change them.

## Fitting your case

My wooden lid covers the edges of the screen, so the sketch only draws inside two rectangles: a small one at the top for the title, and a big one for everything else. They're the `SLOT_*` and `SAFE_*` numbers at the top of `news.cpp`. Change them to match your case.

The title at the top says `DOOMLESS SCROLL`. You can change it in `drawSlotTitle()`.

## Files

- `firmware.ino` sets up the display and the knobs.
- `news.cpp` is the article reader: fetching, layout, and controls.
- `hw.h` has the pin numbers.
- `secrets.h` has your Wi-Fi details and server address.
- `fonts.h` has the fonts (Playfair Display and Inter), converted to bitmaps. It's a generated file.

## What the server has to return

The sketch makes plain HTTP requests to `http://API_HOST`. If you write your own server, it needs these four routes. They're described in the [endpoint README](../endpoint/README.md#routes).

- `GET /edition`
- `GET /article?i=0`
- `GET /covers`
- `GET /image?a=0&i=0`

The list screen has eight slots, so anything past eight articles is ignored.
