# Endpoint

This server is a small Node program that finds a Substack publication, reads its RSS feed, takes the latest posts (eight max), turns the HTML into paragraphs, headings, quotes, and images, and dithers the pictures down to 1-bit bitmaps the e-paper display can draw.

Run it on a computer that's on the same Wi-Fi as the device.

## Run

You need Node.js 18 or newer.

```bash
npm install
```

```bash
npm start
```

The server listens on port 8787. To use a different port, set `PORT`.

To check that it works:

```bash
curl http://127.0.0.1:8787/edition
```

The first request takes a while because the server downloads and converts every article and picture. After that it reuses the result for 30 minutes. `GET /edition?refresh=1` rebuilds it right away, which is what the device does when you hold a knob.

On the device, `API_HOST` in `firmware/secrets.h` should be this computer's local network address and port, like `192.168.1.42:8787`.

## Change the publication

`PROFILE_URL` at the top of `edition.js` is the Substack it reads:

```js
const PROFILE_URL = "https://substack.com/@imovertinker";
```

Change that line to another profile (`https://substack.com/@name`) or publication (`https://name.substack.com`), then restart the server.

## Deploy

Running the server on your laptop is fine for testing, but the device can only fetch articles while the laptop is on and at home. Here are some ways to keep it running.

### A hosting service

Services that keep a normal Node process running are the best fit, because this server holds the built edition in memory. Something like [Render](https://render.com) or [Railway](https://railway.com) would work this way.

On Render and Railway, point the service at this repo, set the root directory to `endpoint`, and use `npm install` to build and `npm start` to start. They set `PORT` for you, and the server already reads it.

Two things to know:

- These services serve over HTTPS, and the firmware makes plain HTTP requests. You'll need the small firmware change below.
- The server may get stopped when nobody is using it. Render's free plan does this after 15 minutes without a request, Fly.io does it by default, and Railway has it as an optional setting. When the server wakes up, the saved edition is gone and it has to build everything again, so the first fetch can take longer than the device waits. If you get the error screen, hold either knob to try again.

Plans and prices change, so check each service's pricing page before you pick one.

### Firmware change for HTTPS

In `firmware/news.cpp`:

1. Add `#include <WiFiClientSecure.h>` near the other includes.
2. In `apiUrl()`, change `"http://"` to `"https://"`.
3. In `httpGetString()` and `httpGetBytes()`, replace `WiFiClient client;` with:

   ```cpp
   WiFiClientSecure client;
   client.setInsecure();
   ```

Then set `API_HOST` to your host name with no port, like `my-scroll.onrender.com`.

`setInsecure()` encrypts the connection but skips checking the server's certificate. That's fine for reading public articles. I haven't tested this change on my own device, since mine fetches over plain HTTP.

Anyone who has the URL can use a deployed server, so don't put anything private on it.

## Routes

You can replace this server with anything that has these four routes.

### `GET /edition`

The catalog, as JSON. Article text is not included here.

```json
{
  "id": "tutorial-202611121530",
  "dateLabel": "Thursday, November 12, 2026",
  "masthead": "Example",
  "articles": [
    {
      "title": "A headline",
      "dek": "A deck, or an empty string",
      "blurb": "Short text next to the cover",
      "author": "A. Writer",
      "publication": "Example",
      "section": "EXAMPLE",
      "published": "Nov 12",
      "backfilled": false,
      "paywalled": false
    }
  ]
}
```

The device shows at most eight articles.

### `GET /article?i=0`

One story as plain text, one block per line. Each line starts with a letter and a tab:

```text
P	A paragraph.
H	A subheading
Q	A quotation
R
I	0	195	260	A caption
```

- `P` is a paragraph, `H` is a heading, and `Q` is a quote.
- `R` is a horizontal rule.
- `I` is an image: its index, width, height, and caption, separated by tabs.

The fonts on the device only have ASCII characters 32 to 126, so everything else is converted or removed.

### `GET /covers`

One 64×96 cover per article, all in a row, in the same order as the catalog. Each cover is 768 bytes: 8 bytes per row, leftmost pixel in the top bit, `1` = black. A story with no usable picture gets a solid black cover.

### `GET /image?a=0&i=0`

A picture inside a story. `a` is the article index and `i` is the image index from the `I` line. The response is the width and height as two little-endian `uint16` values, then the rows, packed the same way as the covers.

## Files

- `server.js` handles the HTTP requests.
- `edition.js` builds the edition and formats each response.
- `lib/feed.js` fetches and parses the RSS feed.
- `lib/extract.js` turns article HTML into text blocks.
- `lib/images.js` downloads and dithers pictures.
