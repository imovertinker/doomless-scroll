// The content server for the Doomless Scroll. The ESP32 calls these four routes:
//   GET /edition          the list of stories (add ?refresh=1 to rebuild it)
//   GET /article?i=0      one story as plain text
//   GET /covers           a small cover picture for each story
//   GET /image?a=0&i=0    a picture from inside a story

import http from "node:http";
import { articleText, coverSheet, getEdition, imageBytes, manifestFor } from "./edition.js";

const port = Number(process.env.PORT) || 8787;

function send(res, status, body, type) {
  res.writeHead(status, {
    "Content-Type": type,
    "Content-Length": Buffer.byteLength(body),
  });
  res.end(body);
}

function sendJson(res, status, body) {
  send(res, status, JSON.stringify(body), "application/json; charset=utf-8");
}

function indexParam(url, name) {
  const raw = url.searchParams.get(name);
  if (raw == null || !/^\d+$/.test(raw)) return null;
  return Number(raw);
}

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, "http://127.0.0.1");
    if (req.method !== "GET") {
      res.writeHead(405, { Allow: "GET" });
      res.end();
      return;
    }
    if (url.pathname === "/") {
      sendJson(res, 200, {
        edition: "/edition",
        article: "/article?i=0",
        covers: "/covers",
        image: "/image?a=0&i=0",
      });
      return;
    }
    if (!["/edition", "/article", "/covers", "/image"].includes(url.pathname)) {
      sendJson(res, 404, { error: "Not found" });
      return;
    }

    const refresh = url.pathname === "/edition" && url.searchParams.get("refresh") === "1";
    const edition = await getEdition({ refresh });

    if (url.pathname === "/edition") {
      console.log(`Edition ${edition.id}, ${edition.articles.length} articles`);
      sendJson(res, 200, manifestFor(edition));
      return;
    }
    if (url.pathname === "/article") {
      const i = indexParam(url, "i");
      const article = i == null ? null : edition.articles[i];
      if (!article) return sendJson(res, 404, { error: "No such article" });
      send(res, 200, articleText(article), "text/plain; charset=us-ascii");
      return;
    }
    if (url.pathname === "/covers") {
      const sheet = coverSheet(edition);
      send(res, 200, sheet, "application/octet-stream");
      return;
    }
    if (url.pathname === "/image") {
      const a = indexParam(url, "a");
      const i = indexParam(url, "i");
      const bytes = a == null || i == null ? null : imageBytes(edition, a, i);
      if (!bytes) return sendJson(res, 404, { error: "No such image" });
      send(res, 200, bytes, "application/octet-stream");
      return;
    }
  } catch (error) {
    console.error(error);
    if (!res.headersSent) sendJson(res, 500, { error: error.message });
  }
});

server.listen(port, "0.0.0.0", () => {
  console.log(`Doomless Scroll server http://0.0.0.0:${port}`);
});
