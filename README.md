# Doomless Scroll

Code for the doomless scroll device: an e-paper screen in a wooden case with two knobs, for reading articles without picking up your phone. The full build tutorial is on [my Substack](https://imovertinker.substack.com).

There are two folders:

- [`endpoint`](endpoint) has the server. It's a small Node program you run on your computer. It reads a Substack, grabs the latest posts, and converts the text and pictures into something the device can draw.
- [`firmware`](firmware) is the code to put on the ESP32. It's an Arduino sketch that joins your Wi-Fi, asks the server for articles, and draws them on the e-paper display.

## Quick start

1. Wire everything up. The pin list is in the [firmware README](firmware/README.md).
2. Start the server:

   ```bash
   cd endpoint
   npm install
   npm start
   ```

3. Fill in your Wi-Fi name, password, and your computer's address in `firmware/secrets.h`.
4. Open `firmware/firmware.ino` in the Arduino IDE, select `ESP32S3 Dev Module`, and upload.

The device and the computer running the server need to be on the same Wi-Fi.

## How the two parts interact

The ESP32 makes four requests to the server:

1. `GET /edition` gets the catalog, with titles, decks, blurbs, authors, and dates
2. `GET /article?i=0` gets one story as plain text, fetched when you open it
3. `GET /covers` gets one tiny black-and-white portrait per story
4. `GET /image?a=0&i=0` gets a picture inside the story, fetched after the text is on the screen

You can replace the server with anything that has these four routes. The exact formats are in the [endpoint README](endpoint/README.md).

## Controls

- Left knob: moves the star on the list. Click to open a story. While you're reading, turning it jumps to the previous or next story, and a click returns to the list.
- Right knob: turns pages.
- Hold either knob: re-fetches the edition.
