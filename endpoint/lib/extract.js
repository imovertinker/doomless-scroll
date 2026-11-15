// Turns a feed item into something the device can draw: plain ASCII text blocks and image links.

import * as cheerio from "cheerio";
import { fetchText } from "./feed.js";

// The fonts on the device only have ASCII 32-126, so swap or strip everything else.

const REPLACEMENTS = [
  [/[\u2018\u2019\u201A\u201B\u2032]/g, "'"],
  [/[\u201C\u201D\u201E\u201F\u2033]/g, '"'],
  [/[\u2013]/g, "-"],
  [/[\u2014\u2015]/g, " -- "],
  [/[\u2026]/g, "..."],
  [/[\u00A0\u2000-\u200B\u202F\u205F\u3000]/g, " "],
  [/[\u2022\u25CF\u25E6]/g, "*"],
  [/[\u00AB]/g, '"'],
  [/[\u00BB]/g, '"'],
  [/\u00E6/g, "ae"],
  [/\u00C6/g, "AE"],
  [/\u0153/g, "oe"],
  [/\u0152/g, "OE"],
  [/\u00DF/g, "ss"],
  [/\u00F8/g, "o"],
  [/\u00D8/g, "O"],
  [/\u0142/g, "l"],
  [/\u0141/g, "L"],
  [/\u00D7/g, "x"],
  [/[\u2190\u2192]/g, "->"],
  [/\u00A9/g, "(c)"],
  [/\u2122/g, "(TM)"],
  [/\u00AE/g, "(R)"],
];

export function toAscii(input) {
  let s = String(input ?? "");
  for (const [re, rep] of REPLACEMENTS) s = s.replace(re, rep);
  s = s.normalize("NFKD").replace(/[\u0300-\u036f]/g, "");
  s = s.replace(/[^\x20-\x7E]/g, "");
  return s.replace(/[ \t]+/g, " ").replace(/ ?\n ?/g, "\n").trim();
}

function squash(s) {
  return String(s ?? "").replace(/\s+/g, " ").trim();
}

const SKIP_SELECTOR = [
  "script", "style", "noscript", "iframe", "form", "button", "svg", "nav", "aside",
  ".subscription-widget-wrap", ".subscription-widget", ".button-wrapper", ".share-dialog",
  ".embedded-post-wrap", ".embedded-publication-wrap", ".captioned-button-wrap", ".poll-embed",
  ".digest-post-embed", ".footnote-anchor", ".latex-rendered", ".pullquote-share",
  ".image-link-expand", ".preamble", ".paywall-jump", ".comments-page-sort-menu-wrapper",
  "[data-component-name='SubscribeWidgetToDOM']", "[data-component-name='ButtonCreateButton']",
  "[data-component-name='ShareToDOM']", ".c-related", ".newsletter-signup", ".article-share",
  ".related-posts", ".related-content", ".recirc", ".outbrain",
  "#comments", ".more-stories",
].join(",");

const TEXT_TAGS = new Set(["p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "pre", "dd", "dt", "td", "th"]);
const HEADING_TAGS = new Set(["h1", "h2", "h3", "h4", "h5", "h6"]);

function bestImageSrc($img) {
  const plain = $img.attr("src") || $img.attr("data-src") || "";
  if (/^https?:\/\//.test(plain)) return plain; // Substack's src is already the largest rendition
  const srcset = $img.attr("srcset") || $img.attr("data-srcset");
  if (srcset) {
    // Candidates are "<url> <descriptor>" separated by commas, but URLs may themselves contain commas
    // (Substack CDN). Split on commas that are followed by whitespace + a URL.
    let best = null;
    for (const part of srcset.split(/,\s+(?=https?:\/\/|\/)/)) {
      const m = part.trim().match(/^(\S+)(?:\s+(\d+)[wx])?$/);
      if (!m) continue;
      const w = m[2] ? parseInt(m[2], 10) : 0;
      if (!best || w > best.w) best = { url: m[1], w };
    }
    if (best?.url) return best.url;
  }
  return plain;
}

function absolutize(src, baseUrl) {
  try {
    return (baseUrl ? new URL(src, baseUrl) : new URL(src)).href;
  } catch {
    return "";
  }
}

const BODY_SELECTORS = [
  "[itemprop='articleBody']",
  ".available-content",
  ".body.markup",
  ".post-content",
  ".entry-content",
  ".article__body",
];

function containsNode(parent, child) {
  let n = child?.parent;
  while (n) {
    if (n === parent) return true;
    n = n.parent;
  }
  return false;
}

// Some pages nest the same content inside a wrapper, so a section shows up
// twice in a row. Drop the repeats.
export function dedupeBlocks(blocks) {
  let cur = blocks.slice();
  for (let guard = 0; guard < 8; guard++) {
    const keys = cur.map((b) => (b.t === "img" ? `img:${b.src || ""}` : `${b.t}:${b.text || ""}`));
    const n = keys.length;
    const drop = new Array(n).fill(false);
    let removedSpan = false;
    for (let src = 0; src < n; ) {
      if (drop[src]) { src++; continue; }
      const maxSpan = Math.floor((n - src) / 2);
      let best = 0;
      for (let span = maxSpan; span >= 2; span--) {
        if (drop[src + span]) continue;
        let chars = 0;
        let same = true;
        for (let k = 0; k < span; k++) {
          if (drop[src + k] || drop[src + span + k] || keys[src + k] !== keys[src + span + k]) { same = false; break; }
          chars += (cur[src + k].text || "").length;
        }
        if (!same || chars < 160) continue;
        best = span;
        break;
      }
      if (!best) { src++; continue; }
      for (let k = 0; k < best; k++) drop[src + best + k] = true;
      removedSpan = true;
      src += best;
    }
    if (removedSpan) {
      cur = cur.filter((_, i) => !drop[i]);
      continue;
    }
    let removed = false;
    const next = [];
    for (const b of cur) {
      const prev = next[next.length - 1];
      if (prev && prev.t === b.t && prev.t !== "img" && prev.text && prev.text === b.text && prev.text.length >= 80) {
        removed = true;
        continue;
      }
      next.push(b);
    }
    if (!removed) return next;
    cur = next;
  }
  return cur;
}

export function htmlToBlocks(html, { baseUrl = "" } = {}) {
  if (!html) return [];
  const $ = cheerio.load(html, null, false);
  $(SKIP_SELECTOR).remove();

  // Start from the article body if we can find it, not the whole page.
  // If body containers are nested, take the innermost one.
  const total = squash($.root().text()).length;
  const candidates = [];
  for (const sel of BODY_SELECTORS) {
    $(sel).each((_, el) => {
      const n = squash($(el).text()).length;
      if (n >= 80) candidates.push({ el, n });
    });
  }
  const inner = candidates.filter((c) => !candidates.some((o) => o.el !== c.el && containsNode(c.el, o.el)));
  inner.sort((a, b) => b.n - a.n);
  const root = inner[0] && inner[0].n >= total * 0.4 ? inner[0].el : $.root()[0];

  const blocks = [];
  let lastImg = null;

  const pushText = (kind, raw) => {
    const t = toAscii(squash(raw));
    if (!t) return;
    // leftover Substack subscribe/share buttons
    if (/^(subscribe now|share|leave a comment|share this post|thanks for reading.*subscribe.*)$/i.test(t)) return;
    blocks.push({ t: kind, text: t });
    lastImg = null;
  };

  const pushImage = (src, alt) => {
    const url = absolutize(src, baseUrl);
    if (!url || /\.(gif|svg)(\?|$)/i.test(url) || /pixel|tracking|emoji|1x1/i.test(url)) return;
    if (blocks.some((b) => b.t === "img" && b.src === url)) return;
    lastImg = { t: "img", src: url, caption: toAscii(squash(alt || "")) };
    blocks.push(lastImg);
  };

  const walk = (node, quote) => {
    for (const child of node.childNodes ?? []) {
      if (child.type !== "tag") continue;
      const el = $(child);
      const tag = child.tagName?.toLowerCase();
      if (!tag) continue;

      if (tag === "img") {
        pushImage(bestImageSrc(el), el.attr("alt"));
        continue;
      }
      if (tag === "figcaption") {
        const cap = toAscii(squash(el.text()));
        if (lastImg && cap) lastImg.caption = cap;
        else if (cap) blocks.push({ t: "cap", text: cap });
        continue;
      }
      if (tag === "hr") {
        blocks.push({ t: "hr" });
        continue;
      }
      if (tag === "blockquote") {
        walk(child, true);
        continue;
      }
      if (TEXT_TAGS.has(tag)) {
        // images nested inside a paragraph
        el.find("img").each((_, img) => pushImage(bestImageSrc($(img)), $(img).attr("alt")));
        const kind = HEADING_TAGS.has(tag) ? "h" : quote ? "q" : "p";
        let txt = el.text();
        if (tag === "li") txt = `* ${txt}`;
        if (tag === "pre") {
          for (const line of txt.split("\n")) if (line.trim()) blocks.push({ t: "q", text: toAscii(line) });
          continue;
        }
        pushText(kind, txt);
        continue;
      }
      // Any other container: if it's just text, treat it as a paragraph. Otherwise look inside.
      const hasBlockChild = el.children().toArray().some((c) => {
        const t = c.tagName?.toLowerCase();
        return TEXT_TAGS.has(t) || t === "div" || t === "figure" || t === "blockquote" || t === "ul" || t === "ol" || t === "img" || t === "picture" || t === "section" || t === "article";
      });
      if (!hasBlockChild) {
        const direct = el.text();
        if (squash(direct).length > 0 && !["a", "span", "em", "strong", "b", "i", "picture", "source", "figure"].includes(tag)) {
          pushText(quote ? "q" : "p", direct);
          continue;
        }
        if (["a", "span", "em", "strong", "b", "i"].includes(tag) && squash(direct)) {
          pushText(quote ? "q" : "p", direct);
          continue;
        }
      }
      walk(child, quote);
    }
  };

  walk(root, false);

  // Attach loose captions to the image before them, and drop doubled-up or trailing rules.
  const out = [];
  for (const b of blocks) {
    if (b.t === "cap") {
      const prev = out[out.length - 1];
      if (prev?.t === "img" && !prev.caption) prev.caption = b.text;
      continue;
    }
    if (b.t === "hr" && (out.length === 0 || out[out.length - 1].t === "hr")) continue;
    out.push(b);
  }
  while (out.length && out[out.length - 1].t === "hr") out.pop();
  return dedupeBlocks(out);
}

async function substackArticle(item, { feedHtml }) {
  let blocks = [];
  let paywalled = false;
  let dek = "";
  let author = item.author || "";
  let lead = item.imageUrl || "";

  try {
    const u = new URL(item.link);
    const m = u.pathname.match(/^\/p\/([^/]+)/);
    if (m) {
      const json = JSON.parse(await fetchText(`${u.origin}/api/v1/posts/${m[1]}`, { timeoutMs: 12000, headers: { Accept: "application/json" } }));
      if (json?.body_html) blocks = htmlToBlocks(json.body_html, { baseUrl: item.link });
      if (json?.subtitle) dek = json.subtitle;
      if (Array.isArray(json?.publishedBylines) && json.publishedBylines.length) {
        author = json.publishedBylines.map((b) => b?.name).filter(Boolean).join(", ") || author;
      }
      if (json?.cover_image) lead = json.cover_image;
      if (json?.audience && json.audience !== "everyone") paywalled = true;
      if (json?.should_send_free_preview === false && json?.audience === "only_paid") paywalled = true;
    }
  } catch {
    // the post API failed, so use the feed body below
  }

  const fromFeed = htmlToBlocks(feedHtml, { baseUrl: item.link });
  if (fromFeed.length > blocks.length) blocks = fromFeed;

  if (lead && !blocks.some((b) => b.t === "img")) blocks.unshift({ t: "img", src: lead, caption: "" });
  return { dek: toAscii(squash(dek)), author: toAscii(squash(author)), blocks, paywalled };
}

function genericArticle(item, { feedHtml }) {
  const blocks = htmlToBlocks(feedHtml, { baseUrl: item.link });
  if (item.imageUrl && !blocks.some((b) => b.t === "img")) blocks.unshift({ t: "img", src: item.imageUrl, caption: "" });
  // The summary is often just the first paragraph again, so only use it as the dek when it's short.
  const summary = toAscii(squash(cheerio.load(item.summaryHtml || "", null, false).text()));
  const dek = summary && summary.length <= 220 && !blocks.some((b) => b.text === summary) ? summary : "";
  return { dek, author: toAscii(squash(item.author || "")), blocks, paywalled: false };
}

// kind is "substack" or "generic". Generic uses whatever HTML is in the feed.
export async function extractArticle(item, kind = "substack") {
  const feedHtml = item.contentHtml || item.summaryHtml || "";
  const result = kind === "substack" ? await substackArticle(item, { feedHtml }) : genericArticle(item, { feedHtml });
  return { ...result, blocks: dedupeBlocks(result.blocks) };
}
