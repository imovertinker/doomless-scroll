// Builds the "edition": the latest posts from one Substack, converted into
// what the firmware expects. server.js serves the results.

import * as cheerio from "cheerio";
import { BROWSER_UA, fetchFeed } from "./lib/feed.js";
import { extractArticle, toAscii } from "./lib/extract.js";
import { COVER_BYTES, blackCover, toCoverBitmap, toEpaperBitmap } from "./lib/images.js";

// The Substack to read. Either a profile URL (https://substack.com/@name)
// or the publication's own address (https://name.substack.com) works.
const PROFILE_URL = "https://substack.com/@imovertinker";
const MAX_ARTICLES = 8;
const TTL_MS = 30 * 60 * 1000;

const clean = (s) => String(s ?? "").replace(/[\t\r\n]+/g, " ").trim();

function dateLabel(d = new Date()) {
  return new Intl.DateTimeFormat("en-US", {
    weekday: "long", month: "long", day: "numeric", year: "numeric",
  }).format(d);
}

function shortDate(d) {
  if (!d) return "";
  return new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric" }).format(d);
}

function blurbOf(summaryHtml, blocks) {
  let text = toAscii(cheerio.load(String(summaryHtml ?? ""), null, false).text()).replace(/\s+/g, " ").trim();
  if (!text) text = blocks.find((b) => b.text && (b.t === "p" || b.t === "h"))?.text || "";
  const cap = 160;
  if (text.length <= cap) return text;
  const cut = text.slice(0, cap + 1);
  const space = cut.lastIndexOf(" ");
  return (space > 40 ? cut.slice(0, space) : text.slice(0, cap)).trim();
}

async function publicationOrigin(profileUrl) {
  const url = new URL(profileUrl);
  const host = url.hostname.replace(/^www\./, "");
  const handle = host === "substack.com" ? url.pathname.match(/^\/@([A-Za-z0-9_.-]+)/)?.[1] : null;
  if (!handle) return url.origin;
  const response = await fetch(`https://substack.com/api/v1/user/${encodeURIComponent(handle)}/public_profile`, {
    headers: { Accept: "application/json", "User-Agent": BROWSER_UA },
  });
  if (!response.ok) throw new Error(`Profile lookup HTTP ${response.status}`);
  const pub = (await response.json()).primaryPublication;
  if (pub?.custom_domain) return `https://${pub.custom_domain}`;
  if (pub?.subdomain) return `https://${pub.subdomain}.substack.com`;
  throw new Error(`@${handle} has no publication`);
}

async function attachImages(blocks) {
  const imgs = blocks.filter((b) => b.t === "img").slice(0, 6);
  const keep = new Set(imgs);
  let next = blocks.filter((b) => b.t !== "img" || keep.has(b));
  await Promise.all(imgs.map(async (block) => {
    try {
      const bmp = await toEpaperBitmap(block.src);
      if (!bmp) throw new Error("too small");
      block.w = bmp.width;
      block.h = bmp.height;
      block.bitmap = bmp.bitmap;
    } catch {
      block.drop = true;
    }
  }));
  next = next.filter((b) => b.t !== "img" || !b.drop);
  let i = 0;
  for (const b of next) if (b.t === "img") b.i = i++;
  return next;
}

async function buildArticle(item, index, publication) {
  let extracted;
  try {
    extracted = await extractArticle(item);
  } catch (error) {
    extracted = {
      dek: "",
      author: toAscii(item.author || ""),
      blocks: [{ t: "p", text: `[Could not load article: ${toAscii(error.message)}]` }],
      paywalled: false,
    };
  }
  const blocks = await attachImages(extracted.blocks);
  if (!blocks.some((b) => b.t === "p" || b.t === "h" || b.t === "q")) {
    blocks.push({ t: "p", text: toAscii(item.title) || "Untitled" });
  }
  const title = toAscii(item.title) || "Untitled";
  const coverSrc = item.imageUrl || blocks.find((b) => b.t === "img")?.src || "";
  let cover = null;
  if (coverSrc) {
    try {
      cover = await toCoverBitmap(coverSrc);
    } catch {
      cover = null;
    }
  }
  if (!cover || cover.length !== COVER_BYTES) cover = blackCover();
  return {
    index,
    title,
    dek: /[A-Za-z]{3,}/.test(extracted.dek || "") ? extracted.dek : "",
    blurb: blurbOf(item.summaryHtml, blocks),
    author: extracted.author || "",
    publication,
    section: publication.toUpperCase(),
    published: shortDate(item.publishedAt),
    backfilled: false,
    paywalled: !!extracted.paywalled,
    blocks,
    cover,
  };
}

async function buildEdition() {
  console.log(`Fetching ${PROFILE_URL}`);
  const origin = await publicationOrigin(PROFILE_URL);
  const feed = await fetchFeed(`${origin}/feed`);
  const publication = toAscii(feed.title) || "Substack";
  const items = feed.items
    .filter((it) => it.link && it.publishedAt && !/\/podcast\//i.test(it.link))
    .sort((a, b) => b.publishedAt - a.publishedAt)
    .slice(0, MAX_ARTICLES);
  const articles = await Promise.all(items.map((item, index) => buildArticle(item, index, publication)));
  return {
    id: `tutorial-${new Date().toISOString().slice(0, 16).replace(/[-:T]/g, "")}`,
    dateLabel: dateLabel(),
    masthead: publication,
    articles,
  };
}

let cache = null;

export async function getEdition({ refresh = false } = {}) {
  if (!refresh && cache && Date.now() - cache.at < TTL_MS) return cache.edition;
  const edition = await buildEdition();
  cache = { at: Date.now(), edition };
  return edition;
}

export function manifestFor(edition) {
  return {
    id: edition.id,
    dateLabel: edition.dateLabel,
    masthead: edition.masthead,
    articles: edition.articles.map((a) => ({
      title: a.title,
      dek: a.dek,
      blurb: a.blurb,
      author: a.author,
      publication: a.publication,
      section: a.section,
      published: a.published,
      backfilled: a.backfilled,
      paywalled: a.paywalled,
    })),
  };
}

export function articleText(article) {
  const lines = [];
  for (const b of article.blocks) {
    switch (b.t) {
      case "p": lines.push(`P\t${clean(b.text)}`); break;
      case "h": lines.push(`H\t${clean(b.text)}`); break;
      case "q": lines.push(`Q\t${clean(b.text)}`); break;
      case "hr": lines.push("R"); break;
      case "img": lines.push(`I\t${b.i}\t${b.w}\t${b.h}\t${clean(b.caption)}`); break;
      default: break;
    }
  }
  if (article.paywalled) lines.push("P\t[This post is for paid subscribers; only the free preview is shown.]");
  return lines.join("\n") + "\n";
}

export function coverSheet(edition) {
  return Buffer.concat(edition.articles.map((a) => (a.cover?.length === COVER_BYTES ? a.cover : blackCover())));
}

export function imageBytes(edition, articleIndex, imageIndex) {
  const block = edition.articles[articleIndex]?.blocks.find((b) => b.t === "img" && b.i === imageIndex);
  return block?.bitmap ?? null;
}
