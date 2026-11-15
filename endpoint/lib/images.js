// Downloads images and dithers them to 1-bit bitmaps for the e-paper display.
//
// Format: uint16 width, uint16 height (both little-endian), then one row at a
// time, ceil(width/8) bytes per row, leftmost pixel in the top bit, 1 = black.

import sharp from "sharp";
import { BROWSER_UA } from "./feed.js";

export const IMG_MAX_W = 364;  // a bit wider than a text column; the firmware scales it down
export const IMG_MAX_H = 260;

// Cover picture for the article list. Has to match COVER_W / COVER_H in the firmware.
export const COVER_W = 64;
export const COVER_H = 96;
export const COVER_BYTES = ((COVER_W + 7) >> 3) * COVER_H;

export function blackCover() {
  return Buffer.alloc(COVER_BYTES, 0xff);
}

async function download(url, timeoutMs = 12000) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(url, {
      headers: { "User-Agent": BROWSER_UA, Accept: "image/*,*/*;q=0.8" },
      redirect: "follow",
      signal: controller.signal,
    });
    if (!response.ok) throw new Error(`HTTP ${response.status} for image ${url}`);
    const type = response.headers.get("content-type") || "";
    if (!/^image\//.test(type) && !/\.(jpe?g|png|webp|avif|gif|bmp|tiff?)(\?|$)/i.test(url)) {
      throw new Error(`Not an image (${type}) ${url}`);
    }
    return Buffer.from(await response.arrayBuffer());
  } finally {
    clearTimeout(timer);
  }
}

function floydSteinberg(gray, w, h) {
  const err = new Float32Array(gray.length);
  const out = Buffer.alloc(((w + 7) >> 3) * h, 0);
  const stride = (w + 7) >> 3;
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = y * w + x;
      const v = gray[i] + err[i];
      const black = v < 128;
      const e = v - (black ? 0 : 255);
      if (black) out[y * stride + (x >> 3)] |= 0x80 >> (x & 7);
      if (x + 1 < w) err[i + 1] += e * 7 / 16;
      if (y + 1 < h) {
        if (x > 0) err[i + w - 1] += e * 3 / 16;
        err[i + w] += e * 5 / 16;
        if (x + 1 < w) err[i + w + 1] += e * 1 / 16;
      }
    }
  }
  return out;
}

// Returns { width, height, bitmap } or null if the image is too small to bother with.
export async function toEpaperBitmap(url, { maxW = IMG_MAX_W, maxH = IMG_MAX_H } = {}) {
  const input = await download(url);
  const base = sharp(input, { animated: false, limitInputPixels: 50_000_000 }).rotate();
  const meta = await base.metadata();
  if (!meta.width || !meta.height) return null;
  if (meta.width < 120 || meta.height < 80) return null; // avatars, icons, badges

  const { data, info } = await base
    .flatten({ background: "#ffffff" })
    .resize({ width: maxW, height: maxH, fit: "inside", withoutEnlargement: false, kernel: "lanczos3" })
    .grayscale()
    .normalise()
    .sharpen({ sigma: 0.8 })
    .raw()
    .toBuffer({ resolveWithObject: true });

  const w = info.width;
  const h = info.height;
  const packed = floydSteinberg(data, w, h);
  const header = Buffer.alloc(4);
  header.writeUInt16LE(w, 0);
  header.writeUInt16LE(h, 2);
  return { width: w, height: h, bitmap: Buffer.concat([header, packed]) };
}

// Center-crops to the cover size. No width/height header on this one, just the rows.
export async function toCoverBitmap(url) {
  const input = await download(url);
  const { data, info } = await sharp(input, { animated: false, limitInputPixels: 50_000_000 })
    .rotate()
    .flatten({ background: "#ffffff" })
    .resize({ width: COVER_W, height: COVER_H, fit: "cover", position: "centre" })
    .grayscale()
    .normalise()
    .sharpen({ sigma: 0.6 })
    .raw()
    .toBuffer({ resolveWithObject: true });
  if (info.width !== COVER_W || info.height !== COVER_H) return null;
  return floydSteinberg(data, info.width, info.height);
}
