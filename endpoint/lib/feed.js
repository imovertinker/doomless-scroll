// Fetches an RSS or Atom feed and returns its items in one common shape.

import * as cheerio from "cheerio";

export const BROWSER_UA =
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";

export async function fetchText(url, { timeoutMs = 12000, headers = {} } = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(url, {
      headers: { "User-Agent": BROWSER_UA, Accept: "*/*", ...headers },
      redirect: "follow",
      signal: controller.signal,
    });
    if (!response.ok) {
      const error = new Error(`HTTP ${response.status} for ${url}`);
      error.status = response.status;
      throw error;
    }
    return await response.text();
  } finally {
    clearTimeout(timer);
  }
}

function text($el) {
  return ($el.first().text() ?? "").trim();
}

// Titles sometimes come with HTML tags or entities in them.
function plainTitle(s) {
  if (!/[<&]/.test(s)) return s;
  return cheerio.load(s, null, false).text().replace(/\s+/g, " ").trim();
}

function parseDate(value) {
  if (!value) return null;
  const t = Date.parse(value);
  return Number.isFinite(t) ? new Date(t) : null;
}

// Returns { title, link, generator, items: [{ title, link, author, publishedAt, contentHtml, summaryHtml, imageUrl }] }
export function parseFeed(xml) {
  const $ = cheerio.load(xml, { xml: { xmlMode: true, decodeEntities: true } });

  const isAtom = $("feed").length > 0 && $("rss").length === 0;
  const items = [];

  if (isAtom) {
    const feedTitle = text($("feed > title"));
    const generator = text($("feed > generator"));
    $("feed > entry").each((_, el) => {
      const e = $(el);
      let link = e.find("link[rel='alternate']").attr("href") || e.find("link").first().attr("href") || "";
      items.push({
        title: plainTitle(text(e.find("> title"))),
        link: link.trim(),
        author: text(e.find("> author > name")),
        publishedAt: parseDate(text(e.find("> published")) || text(e.find("> updated"))),
        contentHtml: e.find("> content").first().text() || "",
        summaryHtml: e.find("> summary").first().text() || "",
        imageUrl: e.find("link[rel='enclosure'][type^='image']").attr("href") || "",
      });
    });
    return { title: feedTitle, link: $("feed > link[rel='alternate']").attr("href") || "", generator, items };
  }

  const channel = $("rss > channel");
  const feedTitle = text(channel.find("> title"));
  const generator = text(channel.find("> generator"));
  channel.find("> item").each((_, el) => {
    const e = $(el);
    items.push({
      title: plainTitle(text(e.find("> title"))),
      link: text(e.find("> link")) || e.find("> guid").first().text().trim(),
      author: text(e.find("> dc\\:creator")) || text(e.find("> author")),
      publishedAt: parseDate(text(e.find("> pubDate")) || text(e.find("> dc\\:date"))),
      contentHtml: e.find("> content\\:encoded").first().text() || "",
      summaryHtml: e.find("> description").first().text() || "",
      imageUrl:
        e.find("> enclosure[type^='image']").attr("url") ||
        e.find("> media\\:content[medium='image'], > media\\:content[type^='image']").attr("url") ||
        e.find("> media\\:thumbnail").attr("url") ||
        "",
    });
  });
  return { title: feedTitle, link: text(channel.find("> link")), generator, items };
}

export async function fetchFeed(url) {
  const xml = await fetchText(url, { headers: { Accept: "application/rss+xml, application/atom+xml, application/xml, text/xml, */*" } });
  const feed = parseFeed(xml);
  feed.url = url;
  return feed;
}
