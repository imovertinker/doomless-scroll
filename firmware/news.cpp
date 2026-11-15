// The article reader. The home screen is a list of stories laid out like a
// book fair flyer (called the "catalog" in this file).
//
// Left knob: moves the star on the list. Click to open a story.
//            While reading, turning it jumps to the previous or next story
//            and a click goes back to the list.
// Right knob: turns pages.
// Holding either knob re-fetches the edition.
//
// Everything comes from the server at http://API_HOST (see secrets.h).

#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <cJSON.h>
#include "hw.h"
#include "font_fwd.h"
#include "secrets.h"

namespace news {

#define HOLD_THRESHOLD_MS 600
#define DETENT_STEPS      4

#define SCREEN_W   800
#define SCREEN_H   480

// My wooden lid covers part of the 800x480 screen. These are the two openings.
// Change them to match your case.
// The small cutout at the top, for the title:
#define SLOT_X     292
#define SLOT_Y      40
#define SLOT_W     215
#define SLOT_H      23

// The big cutout. Everything else is drawn inside this.
#define SAFE_X      48
#define SAFE_Y     100
#define SAFE_W     704
#define SAFE_H     352
#define SAFE_R     (SAFE_X + SAFE_W)          // 752
#define SAFE_B     (SAFE_Y + SAFE_H)          // 452
#define CX         (SCREEN_W / 2)

#define GUTTER      16
#define COL_W      ((SAFE_W - GUTTER) / 2)    // 344. Server images are wider; they are scaled to fit.
#define COL_X(c)   (SAFE_X + (c) * (COL_W + GUTTER))
#define FOOTER_H    16
#define BODY_BOTTOM (SAFE_B - FOOTER_H)

// Fonts (Playfair Display for display type, Inter for text).
#define F_HEADLINE  (&SerifBold30)
#define F_DEK       (&Sans20)
#define F_KICKER    (&SansBold16)
#define F_SUBHEAD   (&SansBold16)
#define F_BODY      (&Sans14)        // swap for &Sans20 for large print (fewer words per page)
#define F_SMALL     (&Sans14)

#define PARA_INDENT   16
#define SUBHEAD_GAP    8
#define IMAGE_GAP      8

#define MAX_ARTICLES     8   // the catalog page has eight slots; extra stories in the edition are ignored
#define MAX_PAGES       200
#define MAX_PAGE_IMAGES   4
#define MAX_BLOCKS      1500

// Cover pictures. The server has to send this same 64x96 size, 1 = black.
#define MARK_BOX     72                    // selection star, beside the cover (multiple of 8)
#define COVER_W      64
#define COVER_H      96
#define COVER_STRIDE ((COVER_W + 7) / 8)
#define COVER_BYTES  (COVER_STRIDE * COVER_H)

// Catalog cell styles. The slot table is with the flyer layout below.
enum SlotStyle : uint8_t { S_RIGHT, S_LEFT, S_RIGHT_BIG, S_LEFT_BIG, S_BANNER, S_BELOW };
struct Slot
{
  int16_t   x, y, w, h;      // window-relative cell
  SlotStyle style;
  bool      panel;           // rounded border around the cell
  uint8_t   scalePct;        // cover size, 100 = the 64x96 the server sends
  int8_t    tiltDeg;         // positive leans the top of the card to the right
};
// Where a cover lands: the tilted, scaled card's centre and its bounding box (with halo).
struct CoverGeom { float cx, cy, scale, c, s; int bx, by, bw, bh; };

struct Article
{
  String title, dek, author, publication, section, published;
  bool   backfilled = false;
  bool   paywalled  = false;
  String blurb;                      // short teaser shown on the catalog
  String body;                       // P/H/Q/R/I block lines
  int    blockCount = 0;
};

// One open article at a time. Offsets are rebuilt for whichever article is open.
uint32_t blockOff[MAX_BLOCKS];

struct Edition
{
  bool    valid = false;
  String  id, dateLabel, masthead;
  int     articleCount = 0;
  Article articles[MAX_ARTICLES];
};

// A position in the edition: where a page starts.
struct Pos
{
  int8_t   article;
  int16_t  block;
  uint32_t ch;        // char offset inside a text block (0 = start of block)
  int16_t  deferred;  // image block that did not fit on the previous page (-1 = none)
};

struct Block
{
  char        type;      // 'P','H','Q','R','I'
  const char *text;      // for P/H/Q: the text; for I: the caption
  uint32_t    len;
  int         imgIdx, imgW, imgH;
};

struct ImgSlot
{
  int8_t   article;
  int      idx, w, h;
  int      x, y, dw, dh; // where the bitmap is drawn
  uint8_t *data;         // packed 1bpp rows, NULL if not fetched
};

struct PageCtx
{
  bool    draw;
  int     pageNo;
  ImgSlot imgs[MAX_PAGE_IMAGES];
  int     imgCount;
};

Edition edition;
Pos     pageStart[MAX_PAGES];
int     pageCount = 0;
int     currentPage = 0;
bool    browsing = true;             // catalog, until a cover is opened
int     catalogIndex = 0;
bool    uiSettled = false;          // a refresh just finished; that knob press is not a new click or hold
uint8_t *coverBits = nullptr;        // articleCount * COVER_BYTES, or null

// Catalog layout.
// A fixed page, like a Scholastic flyer. Left column: one big cover with its copy
// underneath and a small pick tucked below. Then two columns of three whose rows
// don't line up, copy on the left or right of each cover, two title sizes, a few
// boxed panels, covers at different sizes, some tilted a few degrees, and one
// "special" whose black banner lies over the top of its picture.

static const Slot kSlots[MAX_ARTICLES] = {   // fills SAFE_W x SAFE_H = 704 x 352
  {   0,   0, 192, 264, S_BELOW,     true,  160, -2 },   // the big one
  { 192,   0, 256, 108, S_RIGHT,     false, 100,  0 },
  { 448,   0, 256, 134, S_BANNER,    true,   90, -3 },
  { 192, 108, 256, 126, S_LEFT,      false, 115, -5 },
  { 448, 134, 256, 110, S_RIGHT,     false,  95,  4 },
  { 192, 234, 256, 118, S_RIGHT_BIG, true,  105,  5 },
  { 448, 244, 256, 108, S_LEFT,      false,  95, -5 },
  {   0, 264, 192,  88, S_RIGHT,     false,  80,  0 },   // the small pick
};

#define BANNER_H  44
#define CARD_HALO  2     // white margin around the card, so it sits over whatever is beneath


// Selection mark: a hand-drawn five-pointed star. One pentagram stroke that
// doesn't quite close at the top. The box is on a byte boundary so a selection
// change is one partial refresh. 0 = leave the page, 1 = white halo, 2 = black.
static const float kStar[] = {
  37.4f,  9.1f,    // top
  50.6f, 59.8f,    // lower right
   9.4f, 26.0f,    // upper left
  63.7f, 27.3f,    // upper right
  18.0f, 59.7f,    // lower left
  35.5f, 14.2f,    // toward the top, stopping short so the angle stays open
};

int takeDetent(volatile int32_t &steps);
void clearKnobs();
void noteUiSettled();
void led(uint8_t r, uint8_t g, uint8_t b);
void indexBlocks(Article &a);
void freeCovers();
void fetchCovers();
void resetEdition(Edition &ed);
bool connectWiFi(uint32_t timeoutMs = 20000);
String apiUrl(const String &pathAndQuery);
int httpGetString(const String &pathAndQuery, String &out, uint32_t timeoutMs = 45000);
uint8_t *httpGetBytes(const String &pathAndQuery, size_t &len, uint32_t timeoutMs = 30000);
String jstr(cJSON *o, const char *key);
bool jbool(cJSON *o, const char *key);
bool fetchEdition(Edition &ed, bool refresh);
inline bool hasGlyph(const GFXfont *font, char c);
inline int spaceAdvance(const GFXfont *font);
inline int glyphAdvance(const GFXfont *font, char c);
int textAdvanceN(const GFXfont *font, const char *s, uint32_t n);
int textAdvance(const GFXfont *font, const String &s);
int fontLineHeight(const GFXfont *font);
void drawTextN(int x, int y, const char *s, uint32_t n, const GFXfont *font);
void drawText(int x, int y, const String &s, const GFXfont *font);
void drawTextRight(int xRight, int y, const String &s, const GFXfont *font);
void drawTextCentered(int cx, int y, const String &s, const GFXfont *font);
uint32_t fitLine(const GFXfont *font, const char *t, uint32_t len, uint32_t start, int maxW, uint32_t *next);
int drawWrapped(int x, int y, int maxW, const String &s, const GFXfont *font, int maxLines, bool draw);
int wrappedLines(int maxW, const String &s, const GFXfont *font);
String strN(const char *s, uint32_t n);
void hrule(int x, int y, int w, int thickness = 1);
bool getBlock(const Article &a, int b, Block &out);
void drawSlotTitle();
void imageDisplaySize(int srcW, int srcH, int maxW, int maxH, int &dstW, int &dstH);
void drawBitmapFit(int x, int y, const uint8_t *src, int srcW, int srcH, int dstW, int dstH);
int drawRunningHeader(const PageCtx &ctx, const Article &a);
int drawArticleHeader(const PageCtx &ctx, const Article &a, int y);
void drawFooter(const PageCtx &ctx, const Article &a, bool articleContinues);
const ImgSlot *findImage(const PageCtx &ctx, int article, int idx);
Pos flowPage(Pos start, PageCtx &ctx);
void paginateArticle(int article);
void freeImages(PageCtx &ctx);
void fetchPageImages(PageCtx &ctx);
void renderPage(int pageNo, bool full = true);
void showMessageScreen(const String &title, const String &detail);
bool slotTextLeft(const Slot &s);
CoverGeom coverGeom(int index);
int coverSample(int index, const CoverGeom &g, int px, int py);
void drawCover(int index);
int starPixel(int x, int y);
void starBox(int index, int &x, int &y);
void drawMark(int index);
void markBitmap(int index, uint8_t *bm, bool on);
void paintMark(int index, bool on, bool previous);
void shiftSelection(int previous);
void renderCatalog();
void releaseBodiesExcept(int keep);
bool ensureArticleBody(int i);
void openArticle(int i);
void loadEdition(bool refresh);
int pageOfArticle(int article, int index);
int pagesInArticle(int article);
void turnPage(int dir);
void moveCatalog(int dir);
void turnArticle(int dir);
void onSelectClick();
void onHold();
void enter();
void service();

int takeDetent(volatile int32_t &steps)
{
  noInterrupts();
  int32_t s = steps;
  if (s >= DETENT_STEPS || s <= -DETENT_STEPS) steps = 0;
  interrupts();
  if (s >= DETENT_STEPS) return 1;
  if (s <= -DETENT_STEPS) return -1;
  return 0;
}

void clearKnobs()
{
  noInterrupts();
  enc1Steps = 0;
  enc2Steps = 0;
  interrupts();
}

void noteUiSettled()
{
  uiSettled = true;
  clearKnobs();
}

void led(uint8_t r, uint8_t g, uint8_t b)
{
  rgbLedWrite(RGB_LED_PIN, r, g, b);
}

void indexBlocks(Article &a)
{
  a.blockCount = 0;
  const char *s = a.body.c_str();
  uint32_t n = a.body.length(), i = 0;
  while (i < n && a.blockCount < MAX_BLOCKS)
  {
    if (s[i] != '\n') blockOff[a.blockCount++] = i;
    while (i < n && s[i] != '\n') i++;
    i++;
  }
}

void freeCovers()
{
  free(coverBits);
  coverBits = nullptr;
}

void resetEdition(Edition &ed)
{
  ed.valid = false;
  ed.id = ""; ed.dateLabel = ""; ed.masthead = "";
  ed.articleCount = 0;
  for (int i = 0; i < MAX_ARTICLES; i++)
  {
    Article &a = ed.articles[i];
    a.title = ""; a.dek = ""; a.author = ""; a.publication = ""; a.section = ""; a.published = "";
    a.blurb = "";
    a.backfilled = a.paywalled = false;
    a.body = "";
    a.blockCount = 0;
  }
}

bool connectWiFi(uint32_t timeoutMs)
{
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.printf("Connecting to WiFi \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    if (millis() - start > timeoutMs)
    {
      Serial.printf("\nWiFi FAILED (status %d)\n", WiFi.status());
      return false;
    }
    delay(250);
    Serial.print('.');
  }
  Serial.printf("\nWiFi connected. IP: %s RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

String apiUrl(const String &pathAndQuery)
{
  return String("http://") + API_HOST + pathAndQuery;
}

int httpGetString(const String &pathAndQuery, String &out, uint32_t timeoutMs)
{
  if (!connectWiFi(10000)) return -1;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.setReuse(false);
  String url = apiUrl(pathAndQuery);
  Serial.printf("GET %s\n", url.c_str());
  if (!http.begin(client, url)) return -2;
  int code = http.GET();
  if (code == HTTP_CODE_OK) out = http.getString();
  else Serial.printf("HTTP %d %s\n", code, http.errorToString(code).c_str());
  http.end();
  return code;
}

uint8_t *httpGetBytes(const String &pathAndQuery, size_t &len, uint32_t timeoutMs)
{
  len = 0;
  if (!connectWiFi(10000)) return nullptr;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(timeoutMs);
  http.setReuse(false);
  String url = apiUrl(pathAndQuery);
  Serial.printf("GET %s\n", url.c_str());
  if (!http.begin(client, url)) return nullptr;
  int code = http.GET();
  if (code != HTTP_CODE_OK)
  {
    Serial.printf("HTTP %d\n", code);
    http.end();
    return nullptr;
  }
  int size = http.getSize();
  uint8_t *buf = nullptr;
  if (size > 0)
  {
    buf = (uint8_t *)malloc(size);
    if (!buf)
    {
      Serial.println("  no heap for the body");
      http.end();
      return nullptr;
    }
    WiFiClient *stream = http.getStreamPtr();
    size_t got = 0;
    uint32_t last = millis();
    while (got < (size_t)size && millis() - last < timeoutMs)
    {
      size_t avail = stream->available();
      if (avail)
      {
        int n = stream->readBytes(buf + got, min(avail, (size_t)size - got));
        if (n > 0) { got += n; last = millis(); }
      }
      else if (!http.connected()) break;
      else delay(2);
    }
    if (got != (size_t)size) { free(buf); buf = nullptr; }
    else len = got;
  }
  else
  {
    String s = http.getString();
    buf = (uint8_t *)malloc(s.length());
    if (buf) { memcpy(buf, s.c_str(), s.length()); len = s.length(); }
  }
  http.end();
  return buf;
}

String jstr(cJSON *o, const char *key)
{
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return (v && cJSON_IsString(v) && v->valuestring) ? String(v->valuestring) : String();
}

bool jbool(cJSON *o, const char *key)
{
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return v && cJSON_IsTrue(v);
}

bool fetchEdition(Edition &ed, bool refresh)
{
  freeCovers();
  String json;
  int code = httpGetString(refresh ? "/edition?refresh=1" : "/edition", json, 60000);
  if (code != HTTP_CODE_OK) return false;

  cJSON *root = cJSON_Parse(json.c_str());
  if (!root) { Serial.println("Edition JSON parse failed"); return false; }

  resetEdition(ed);
  ed.id        = jstr(root, "id");
  ed.dateLabel = jstr(root, "dateLabel");
  ed.masthead  = jstr(root, "masthead");
  cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "articles");
  int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
  if (n > MAX_ARTICLES) n = MAX_ARTICLES;
  for (int i = 0; i < n; i++)
  {
    cJSON *o = cJSON_GetArrayItem(arr, i);
    Article &a = ed.articles[i];
    a.title       = jstr(o, "title");
    a.dek         = jstr(o, "dek");
    a.blurb       = jstr(o, "blurb");
    a.author      = jstr(o, "author");
    a.publication = jstr(o, "publication");
    a.section     = jstr(o, "section");
    a.published   = jstr(o, "published");
    a.backfilled  = jbool(o, "backfilled");
    a.paywalled   = jbool(o, "paywalled");
    Serial.printf("  [%d] %s | %s\n", i, a.section.c_str(), a.title.c_str());
  }
  ed.articleCount = n;
  ed.valid = true;
  cJSON_Delete(root);
  return true;
}

void fetchCovers()
{
  if (coverBits || edition.articleCount <= 0) return;
  int n = edition.articleCount;
  coverBits = (uint8_t *)malloc((size_t)n * COVER_BYTES);
  size_t len = 0;
  uint8_t *buf = httpGetBytes("/covers", len);
  if (coverBits && buf && len >= (size_t)n * COVER_BYTES) memcpy(coverBits, buf, (size_t)n * COVER_BYTES);
  else if (coverBits) memset(coverBits, 0xFF, (size_t)n * COVER_BYTES);
  free(buf);
  if (!coverBits) Serial.printf("No heap for covers (%u free)\n", ESP.getFreeHeap());
  else Serial.printf("Covers: %u bytes, heap %u\n", (unsigned)((size_t)n * COVER_BYTES), ESP.getFreeHeap());
}

inline bool hasGlyph(const GFXfont *font, char c)
{
  uint8_t u = (uint8_t)c;
  return u >= pgm_read_byte(&font->first) && u <= pgm_read_byte(&font->last);
}

inline int spaceAdvance(const GFXfont *font)
{
  return hasGlyph(font, ' ') ? pgm_read_byte(&font->glyph[' ' - pgm_read_byte(&font->first)].xAdvance)
                             : pgm_read_byte(&font->yAdvance) / 3;
}

inline int glyphAdvance(const GFXfont *font, char c)
{
  if (c == ' ') return spaceAdvance(font);
  if (!hasGlyph(font, c)) return hasGlyph(font, '?') ? glyphAdvance(font, '?') : 0;
  return pgm_read_byte(&font->glyph[(uint8_t)c - pgm_read_byte(&font->first)].xAdvance);
}

int textAdvanceN(const GFXfont *font, const char *s, uint32_t n)
{
  int w = 0;
  for (uint32_t i = 0; i < n; i++) w += glyphAdvance(font, s[i]);
  return w;
}

int textAdvance(const GFXfont *font, const String &s)
{ return textAdvanceN(font, s.c_str(), s.length()); }

int fontLineHeight(const GFXfont *font)
{ return pgm_read_byte(&font->yAdvance); }

void drawTextN(int x, int y, const char *s, uint32_t n, const GFXfont *font)
{
  display.setFont(font);
  display.setCursor(x, y);
  for (uint32_t i = 0; i < n; i++)
  {
    char c = s[i];
    if (hasGlyph(font, c)) { display.write((uint8_t)c); continue; }
    // GFX draws nothing for a missing glyph and doesn't move the cursor, so do it by hand.
    x = display.getCursorX() + glyphAdvance(font, c);
    if (c != ' ' && hasGlyph(font, '?')) display.write((uint8_t)'?');
    else display.setCursor(x, y);
  }
}

void drawText(int x, int y, const String &s, const GFXfont *font)
{ drawTextN(x, y, s.c_str(), s.length(), font); }

void drawTextRight(int xRight, int y, const String &s, const GFXfont *font)
{ drawText(xRight - textAdvance(font, s), y, s, font); }

void drawTextCentered(int cx, int y, const String &s, const GFXfont *font)
{ drawText(cx - textAdvance(font, s) / 2, y, s, font); }

uint32_t fitLine(const GFXfont *font, const char *t, uint32_t len, uint32_t start, int maxW, uint32_t *next)
{
  int w = 0;
  uint32_t i = start, breakEnd = start, breakNext = start;
  while (i < len)
  {
    char c = t[i];
    if (c == ' ')
    {
      breakEnd = i;
      breakNext = i + 1;
    }
    int cw = glyphAdvance(font, c);
    if (w + cw > maxW)
    {
      if (c == ' ') { *next = i + 1; return i; }
      if (breakEnd > start) { *next = breakNext; return breakEnd; }
      if (i == start) i++;               // a glyph wider than the column: never return an empty line
      *next = i;                         // hard break inside a very long word
      return i;
    }
    w += cw;
    i++;
  }
  *next = len;
  return len;
}

int drawWrapped(int x, int y, int maxW, const String &s, const GFXfont *font, int maxLines, bool draw)
{
  const char *t = s.c_str();
  uint32_t len = s.length(), pos = 0, next;
  int lineH = fontLineHeight(font), lines = 0;
  while (pos < len && lines < maxLines)
  {
    uint32_t end = fitLine(font, t, len, pos, maxW, &next);
    bool lastAllowed = (lines == maxLines - 1) && next < len;
    if (draw)
    {
      if (lastAllowed)
      {
        // Truncated: make room for an ellipsis on the final line.
        int dots = textAdvance(font, "...");
        uint32_t e = end;
        while (e > pos && textAdvanceN(font, t + pos, e - pos) + dots > maxW) e--;
        while (e > pos && t[e - 1] == ' ') e--;
        drawTextN(x, y, t + pos, e - pos, font);
        drawTextN(x + textAdvanceN(font, t + pos, e - pos), y, "...", 3, font);
      }
      else drawTextN(x, y, t + pos, end - pos, font);
    }
    y += lineH;
    lines++;
    pos = next;
  }
  return y;
}

int wrappedLines(int maxW, const String &s, const GFXfont *font)
{
  const char *t = s.c_str();
  uint32_t len = s.length(), pos = 0, next;
  int lines = 0;
  while (pos < len) { fitLine(font, t, len, pos, maxW, &next); pos = next; lines++; }
  return lines;
}

String strN(const char *s, uint32_t n)
{
  String out;
  out.reserve(n);
  out.concat(s, n);
  return out;
}

void hrule(int x, int y, int w, int thickness)
{
  display.fillRect(x, y, w, thickness, GxEPD_BLACK);
}

bool getBlock(const Article &a, int b, Block &out)
{
  if (b < 0 || b >= a.blockCount) return false;
  const char *s = a.body.c_str() + blockOff[b];
  const char *end = strchr(s, '\n');
  uint32_t lineLen = end ? (uint32_t)(end - s) : strlen(s);

  out.type = s[0];
  out.text = "";
  out.len = 0;
  out.imgIdx = out.imgW = out.imgH = 0;

  if (out.type == 'I')
  {
    // I \t idx \t w \t h \t caption
    int idx = 0, w = 0, h = 0;
    const char *p = s + 2;
    idx = atoi(p); p = (const char *)memchr(p, '\t', lineLen - (p - s)); if (!p) return true; p++;
    w = atoi(p);   p = (const char *)memchr(p, '\t', lineLen - (p - s)); if (!p) return true; p++;
    h = atoi(p);   p = (const char *)memchr(p, '\t', lineLen - (p - s));
    out.imgIdx = idx; out.imgW = w; out.imgH = h;
    if (p) { p++; out.text = p; out.len = lineLen - (p - s); }
    return true;
  }
  if (lineLen >= 2)
  {
    out.text = s + 2;
    out.len = lineLen - 2;
  }
  return true;
}

void drawSlotTitle()
{
  const String title = "DOOMLESS SCROLL";
  const GFXfont *font = F_KICKER;
  int16_t bx, by;
  uint16_t bw, bh;
  display.setFont(font);
  display.getTextBounds(title, 0, 0, &bx, &by, &bw, &bh);
  int line = fontLineHeight(font);
  int baseline = SLOT_Y + ((int)SLOT_H - (int)bh) / 2 - by - line - line / 3;
  drawTextCentered(CX, baseline, title, font);
}

void imageDisplaySize(int srcW, int srcH, int maxW, int maxH, int &dstW, int &dstH)
{
  dstW = max(1, srcW);
  dstH = max(1, srcH);
  if (maxW < 1) maxW = 1;
  if (maxH < 1) maxH = 1;
  if (dstW > maxW)
  {
    dstH = max(1, (int)((int32_t)dstH * maxW / dstW));
    dstW = maxW;
  }
  if (dstH > maxH)
  {
    dstW = max(1, (int)((int32_t)dstW * maxH / dstH));
    dstH = maxH;
  }
}

void drawBitmapFit(int x, int y, const uint8_t *src, int srcW, int srcH, int dstW, int dstH)
{
  if (!src || srcW < 1 || srcH < 1 || dstW < 1 || dstH < 1) return;
  if (dstW == srcW && dstH == srcH)
  {
    display.drawBitmap(x, y, (uint8_t *)src, srcW, srcH, GxEPD_BLACK);
    return;
  }
  int srcStride = (srcW + 7) / 8;
  int dstStride = (dstW + 7) / 8;
  uint8_t *dst = (uint8_t *)malloc((size_t)dstStride * dstH);
  if (!dst)
  {
    display.drawRect(x, y, dstW, dstH, GxEPD_BLACK);
    return;
  }
  memset(dst, 0, (size_t)dstStride * dstH);
  for (int dy = 0; dy < dstH; dy++)
  {
    const uint8_t *row = src + (dy * srcH / dstH) * srcStride;
    uint8_t *out = dst + dy * dstStride;
    for (int dx = 0; dx < dstW; dx++)
    {
      int sx = dx * srcW / dstW;
      if (row[sx >> 3] & (0x80 >> (sx & 7))) out[dx >> 3] |= 0x80 >> (dx & 7);
    }
  }
  display.drawBitmap(x, y, dst, dstW, dstH, GxEPD_BLACK);
  free(dst);
}

int drawRunningHeader(const PageCtx &ctx, const Article &a)
{
  int y = SAFE_Y + 14;
  if (ctx.draw)
  {
    String section = a.section.length() ? a.section : a.publication;
    drawText(SAFE_X, y, section, F_KICKER);
    drawTextRight(SAFE_R, y, "Continued", F_SMALL);
    hrule(SAFE_X, y + 6, SAFE_W, 1);
  }
  return y + 6 + 12;
}

int drawArticleHeader(const PageCtx &ctx, const Article &a, int y)
{
  const int x = SAFE_X;
  const int fullW = SAFE_W;

  String kicker = a.section;
  String pubUpper = a.publication;
  pubUpper.toUpperCase();
  if (a.publication.length() && pubUpper != a.section) kicker += "  |  " + a.publication;
  if (a.backfilled) kicker += "  |  FROM THE ARCHIVE";
  y += fontLineHeight(F_KICKER) - 6;
  if (ctx.draw) drawWrapped(x, y, fullW, kicker, F_KICKER, 1, true);
  y += 4;

  int hl = fontLineHeight(F_HEADLINE);
  int lines = max(1, min(2, wrappedLines(fullW, a.title, F_HEADLINE)));
  y += hl - 8;
  drawWrapped(x, y, fullW, a.title, F_HEADLINE, 2, ctx.draw);
  y += (lines - 1) * hl + 4;

  if (a.dek.length())
  {
    int dl = fontLineHeight(F_SMALL);
    int dlines = max(1, min(2, wrappedLines(fullW, a.dek, F_SMALL)));
    y += dl - 4;
    drawWrapped(x, y, fullW, a.dek, F_SMALL, 2, ctx.draw);
    y += (dlines - 1) * dl + 2;
  }

  String byline;
  if (a.author.length()) byline = "By " + a.author;
  if (a.published.length()) byline += (byline.length() ? "   |   " : "") + a.published;
  if (a.paywalled) byline += "   |   Preview";
  if (byline.length())
  {
    y += fontLineHeight(F_SMALL) - 4;
    if (ctx.draw) drawWrapped(x, y, fullW, byline, F_SMALL, 1, true);
  }
  y += 5;
  if (ctx.draw) hrule(x, y, fullW, 1);
  return y + 8;
}

void drawFooter(const PageCtx &ctx, const Article &a, bool articleContinues)
{
  (void)articleContinues;
  if (!ctx.draw || pageCount == 0) return;
  int article = pageStart[ctx.pageNo].article;
  int index = 0, count = 0;
  for (int i = 0; i < pageCount; i++)
  {
    if (pageStart[i].article != article) continue;
    if (i == ctx.pageNo) index = count;
    count++;
  }
  int y = SAFE_B - 4;
  hrule(SAFE_X, SAFE_B - FOOTER_H, SAFE_W, 1);
  String who = a.publication.length() ? a.publication : a.section;
  while (who.length() > 3 && textAdvance(F_SMALL, who) > 200)
    who = who.substring(0, who.length() - 4) + "...";
  drawText(SAFE_X, y, who, F_SMALL);
  drawTextCentered(CX, y, "Page " + String(index + 1) + " of " + String(max(1, count)), F_SMALL);
  drawTextRight(SAFE_R, y, String(article + 1) + " of " + String(edition.articleCount), F_SMALL);
}

const ImgSlot *findImage(const PageCtx &ctx, int article, int idx)
{
  for (int i = 0; i < ctx.imgCount; i++)
    if (ctx.imgs[i].article == article && ctx.imgs[i].idx == idx) return &ctx.imgs[i];
  return nullptr;
}

Pos flowPage(Pos start, PageCtx &ctx)
{
  Article &a = edition.articles[start.article];
  bool articleStart = (start.block == 0 && start.ch == 0);

  if (ctx.draw) drawSlotTitle();
  int y0 = articleStart ? drawArticleHeader(ctx, a, SAFE_Y) : drawRunningHeader(ctx, a);

  int bodyTop = y0;

  if (ctx.draw && BODY_BOTTOM > bodyTop + 4)
    display.fillRect(SAFE_X + COL_W + GUTTER / 2, bodyTop, 1, BODY_BOTTOM - bodyTop - 4, GxEPD_BLACK);

  int col = 0;
  int y = bodyTop;
  Pos pos = start;
  pos.deferred = -1;
  int16_t deferredHere = -1;          // image pushed off this page, to be placed first on the next one
  bool articleDone = false;

  auto nextColumn = [&]() -> bool {   // returns false when the page is full
    col++;
    y = bodyTop;
    return col < 2;
  };

  // Places an image block at the current position if it fits; false = does not fit here.
  auto placeImage = [&](const Block &b) -> bool {
    String cap = b.len ? strN(b.text, b.len) : String();
    int capLines = b.len ? min(3, wrappedLines(COL_W, cap, F_SMALL)) : 0;
    int capH = capLines ? 4 + capLines * fontLineHeight(F_SMALL) : 0;
    int dw, dh;
    imageDisplaySize(b.imgW, b.imgH, COL_W, 10000, dw, dh);
    int need = dh + capH + IMAGE_GAP;
    if (y + need > BODY_BOTTOM)
    {
      // Opening pages are short because the headline spans both columns, so send the
      // picture to the next page instead of squashing it. Shrink only when even a
      // continued page cannot hold the width-fitted image.
      if (y > bodyTop || articleStart) return false;
      int maxH = BODY_BOTTOM - y - capH - IMAGE_GAP;
      if (maxH < 24) return false;
      imageDisplaySize(b.imgW, b.imgH, COL_W, maxH, dw, dh);
      need = dh + capH + IMAGE_GAP;
    }
    int x = COL_X(col);
    if (ctx.draw)
    {
      int ix = x + (COL_W - dw) / 2;
      const ImgSlot *img = findImage(ctx, start.article, b.imgIdx);
      if (img && img->data) drawBitmapFit(ix, y, img->data, img->w, img->h, dw, dh);
      else display.drawRect(ix, y, dw, dh, GxEPD_BLACK);
      if (capLines)
        drawWrapped(x, y + dh + 4 + fontLineHeight(F_SMALL) - 4, COL_W, cap, F_SMALL, 3, true);
    }
    else if (ctx.imgCount < MAX_PAGE_IMAGES)
    {
      ImgSlot &s = ctx.imgs[ctx.imgCount++];
      s.article = start.article; s.idx = b.imgIdx; s.w = b.imgW; s.h = b.imgH;
      s.x = x + (COL_W - dw) / 2; s.y = y; s.dw = dw; s.dh = dh; s.data = nullptr;
    }
    y += need;
    return true;
  };

  // An image carried over from the previous page goes at the top of the first column.
  if (start.deferred >= 0)
  {
    Block d;
    if (getBlock(a, start.deferred, d) && d.type == 'I' && !placeImage(d))
      Serial.printf("Dropped image block %d of article %d (never fit)\n", start.deferred, start.article);
  }

  while (!articleDone)
  {
    Block b;
    if (!getBlock(a, pos.block, b))
    {
      articleDone = true;
      break;
    }
    int x = COL_X(col);

    if (b.type == 'R')
    {
      if (y + 18 > BODY_BOTTOM) { if (!nextColumn()) break; x = COL_X(col); }
      if (ctx.draw) hrule(x + COL_W / 2 - 40, y + 9, 80, 1);
      y += 18;
      pos.block++; pos.ch = 0;
      continue;
    }

    if (b.type == 'I')
    {
      if (!placeImage(b))
      {
        if (y > bodyTop)
        {
          // Not enough room left in this column: try the next one, else the next page.
          if (!nextColumn()) break;
          if (placeImage(b)) { pos.block++; pos.ch = 0; continue; }
        }
        // Does not fit even in an empty column (tall article header). Defer it to the next page
        // and keep flowing text so this page is not left blank.
        if (deferredHere < 0) deferredHere = pos.block;
        else Serial.printf("Dropped image block %d of article %d (two deferred)\n", pos.block, start.article);
      }
      pos.block++; pos.ch = 0;
      continue;
    }

    // Text block: P, H or Q
    const GFXfont *font = (b.type == 'H') ? F_SUBHEAD : F_BODY;
    int lh = fontLineHeight(font);
    int asc = lh - 4;
    int indentX = (b.type == 'Q') ? 14 : 0;
    int width = COL_W - indentX;

    if (b.type == 'H' && pos.ch == 0)
    {
      if (y + SUBHEAD_GAP + lh * 2 > BODY_BOTTOM) { if (!nextColumn()) break; x = COL_X(col); }
      else y += SUBHEAD_GAP;
    }

    uint32_t p = pos.ch, next;
    int quoteTop = y;
    while (p < b.len)
    {
      if (y + lh > BODY_BOTTOM)
      {
        if (b.type == 'Q' && ctx.draw && y > quoteTop) display.fillRect(x + 4, quoteTop, 2, y - quoteTop - 2, GxEPD_BLACK);
        pos.ch = p;
        if (!nextColumn()) goto pageFull;
        x = COL_X(col);
        quoteTop = y;
      }
      int firstIndent = (b.type == 'P' && p == 0) ? PARA_INDENT : 0;
      uint32_t end = fitLine(font, b.text, b.len, p, width - firstIndent, &next);
      if (ctx.draw) drawTextN(x + indentX + firstIndent, y + asc, b.text + p, end - p, font);
      y += lh;
      p = next;
    }
    if (b.type == 'Q' && ctx.draw && y > quoteTop) display.fillRect(x + 4, quoteTop, 2, y - quoteTop - 2, GxEPD_BLACK);
    if (b.type == 'H') y += 2;
    if (b.type == 'P' || b.type == 'Q') y += 3;   // small paragraph spacing
    pos.block++; pos.ch = 0;
  }

  if (articleDone || pos.block >= a.blockCount)
  {
    // The article ended on this page. If an image was deferred, use any remaining room for it.
    if (deferredHere >= 0)
    {
      Block d;
      if (getBlock(a, deferredHere, d))
      {
        if (!placeImage(d) && nextColumn()) placeImage(d);
      }
    }
    // End mark
    if (ctx.draw)
    {
      int x = COL_X(col);
      if (y + 12 <= BODY_BOTTOM) display.fillRect(x + COL_W - 10, y + 2, 8, 8, GxEPD_BLACK);
    }
    drawFooter(ctx, a, false);
    Pos n = { (int8_t)(start.article + 1), 0, 0, -1 };
    return n;
  }

pageFull:
  drawFooter(ctx, a, true);
  pos.deferred = deferredHere;
  return pos;
}

void paginateArticle(int article)
{
  pageCount = 0;
  if (!edition.valid || article < 0 || article >= edition.articleCount) return;
  if (edition.articles[article].blockCount <= 0) return;
  Pos p = { (int8_t)article, 0, 0, -1 };
  while (p.article == article && pageCount < MAX_PAGES)
  {
    pageStart[pageCount++] = p;
    PageCtx ctx;
    ctx.draw = false; ctx.pageNo = pageCount - 1; ctx.imgCount = 0;
    Pos n = flowPage(p, ctx);
    if (n.article == p.article && n.block == p.block && n.ch == p.ch && n.deferred == p.deferred)
    {
      Serial.printf("Pagination stalled at article %d block %d; skipping block\n", p.article, p.block);
      n.block++; n.ch = 0; n.deferred = -1;
      if (n.block >= edition.articles[article].blockCount) n.article = article + 1;
    }
    p = n;
  }
  Serial.printf("Article %d: %d pages, heap %u\n", article, pageCount, ESP.getFreeHeap());
}

void freeImages(PageCtx &ctx)
{
  for (int i = 0; i < ctx.imgCount; i++) { free(ctx.imgs[i].data); ctx.imgs[i].data = nullptr; }
}

void fetchPageImages(PageCtx &ctx)
{
  for (int i = 0; i < ctx.imgCount; i++)
  {
    ImgSlot &s = ctx.imgs[i];
    size_t len = 0;
    uint8_t *buf = httpGetBytes(String("/image?a=") + s.article + "&i=" + s.idx, len, 12000);
    if (!buf)
    {
      Serial.printf("  image %d/%d skipped\n", s.article, s.idx);
      continue;
    }
    if (len < 4) { free(buf); continue; }
    int w = buf[0] | (buf[1] << 8), h = buf[2] | (buf[3] << 8);
    size_t expect = 4 + ((w + 7) / 8) * (size_t)h;
    if (w != s.w || h != s.h || len < expect)
    {
      Serial.printf("Image %d/%d: unexpected %dx%d (%u bytes), wanted %dx%d\n", s.article, s.idx, w, h, (unsigned)len, s.w, s.h);
      if (len >= expect && w <= s.w && h <= s.h) { s.w = w; s.h = h; }
      else { free(buf); continue; }
    }
    memmove(buf, buf + 4, len - 4);
    s.data = buf;
  }
}

void textWindow(int &x, int &y, int &w, int &h)
{
  x = SAFE_X & ~7;
  int right = SAFE_R;
  if (right & 7) right += 8 - (right & 7);
  if (right > SCREEN_W) right = SCREEN_W;
  w = right - x;
  y = (SAFE_Y - 16) & ~7;
  if (y < 0) y = 0;
  int bottom = SAFE_B + 16;
  if (bottom > SCREEN_H) bottom = SCREEN_H;
  h = bottom - y;
}

void paintImageSlot(const ImgSlot &s)
{
  if (!s.data || s.dw < 1 || s.dh < 1) return;
  int x = s.x & ~7;
  int right = s.x + s.dw;
  if (right & 7) right += 8 - (right & 7);
  if (x < 0) x = 0;
  if (right > SCREEN_W) right = SCREEN_W;
  int w = right - x;
  int y = s.y < 0 ? 0 : s.y;
  int h = s.dh;
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (w < 8 || h < 1) return;
  display.setPartialWindow(x, y, w, h);
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    drawBitmapFit(s.x, s.y, s.data, s.w, s.h, s.dw, s.dh);
  }
  while (display.nextPage());
}

// This panel's partial update compares the new image to a second RAM copy of
// the last one. That copy is not updated by a full refresh (writeImageAgain
// is empty on the 7.5"), so the next partial leaves the previous letters in
// place. Write the page now on the glass into that copy, without flashing.
void rememberText(PageCtx &ctx, int pageNo)
{
  int x, y, w, h;
  textWindow(x, y, w, h);
  display.setPartialWindow(x, y, w, h);
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    flowPage(pageStart[pageNo], ctx);
  }
  while (display.nextPageToPrevious());
}

void renderPage(int pageNo, bool full)
{
  if (pageCount == 0) return;
  pageNo = ((pageNo % pageCount) + pageCount) % pageCount;
  currentPage = pageNo;

  led(0, 0, 40);
  PageCtx ctx;
  ctx.draw = false; ctx.pageNo = pageNo; ctx.imgCount = 0;
  flowPage(pageStart[pageNo], ctx);      // dry run, to find which images this page needs

  ctx.draw = true;
  if (full)
    display.setFullWindow();
  else
  {
    int x, y, w, h;
    textWindow(x, y, w, h);
    display.setPartialWindow(x, y, w, h);
  }
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    flowPage(pageStart[pageNo], ctx);
  }
  while (display.nextPage());
  noteUiSettled();
  Serial.printf("Page %d/%d shown%s. heap %u\n", pageNo + 1, pageCount,
                full ? "" : " (partial)", ESP.getFreeHeap());

  // Pictures arrive after the text, in their own rectangles.
  fetchPageImages(ctx);
  for (int i = 0; i < ctx.imgCount; i++) if (ctx.imgs[i].data) paintImageSlot(ctx.imgs[i]);
  rememberText(ctx, pageNo);

  freeImages(ctx);
  display.powerOff();
  led(0, 0, 0);
  noteUiSettled();
}

void showMessageScreen(const String &title, const String &detail)
{
  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    drawSlotTitle();
    drawTextCentered(CX, SAFE_Y + 56, title, F_DEK);
    drawWrapped(SAFE_X, SAFE_Y + 96, SAFE_W, detail, F_SMALL, 6, true);
  }
  while (display.nextPage());
  display.powerOff();
  noteUiSettled();
}

bool slotTextLeft(const Slot &s)
{ return s.style == S_LEFT || s.style == S_LEFT_BIG; }

CoverGeom coverGeom(int index)
{
  const Slot &sl = kSlots[index];
  CoverGeom g;
  g.scale = sl.scalePct / 100.0f;
  float a = sl.tiltDeg * (float)M_PI / 180.0f;
  g.c = cosf(a);
  g.s = sinf(a);
  float w = COVER_W * g.scale, h = COVER_H * g.scale;
  g.bw = (int)ceilf(fabsf(w * g.c) + fabsf(h * g.s)) + 2 * CARD_HALO;
  g.bh = (int)ceilf(fabsf(w * g.s) + fabsf(h * g.c)) + 2 * CARD_HALO;
  int cellX = SAFE_X + sl.x, cellY = SAFE_Y + sl.y;
  if (sl.style == S_BELOW) { g.bx = cellX + (sl.w - g.bw) / 2; g.by = cellY + 8; }   // centred, copy underneath
  else if (slotTextLeft(sl))
  {
    g.bx = cellX + sl.w - 8 - g.bw;
    g.by = cellY + (sl.h - g.bh) / 2;
  }
  else
  {
    g.bx = cellX + 8;
    if (sl.style == S_BANNER) g.by = cellY + BANNER_H - 8 - CARD_HALO;  // top of the card tucks under the banner
    else g.by = cellY + (sl.h - g.bh) / 2;
  }
  g.cx = g.bx + g.bw / 2.0f;
  g.cy = g.by + g.bh / 2.0f;
  return g;
}

// What the catalog paints at screen pixel (px, py) for this cover:
// 0 = nothing (outside the card), 1 = white, 2 = black.
// The card is resampled from the 64x96 bitmap: four sub-samples per screen pixel,
// then a 2x2 ordered threshold, so scaled or tilted dithers still read as tone.
// At 100% and 0 degrees it reproduces the bitmap exactly.
int coverSample(int index, const CoverGeom &g, int px, int py)
{
  float u = px + 0.5f - g.cx, v = py + 0.5f - g.cy;
  float sx = (u * g.c + v * g.s) / g.scale + COVER_W / 2.0f;
  float sy = (-u * g.s + v * g.c) / g.scale + COVER_H / 2.0f;
  float halo = CARD_HALO / g.scale, edge = 1.0f / g.scale;
  if (sx < -halo || sx >= COVER_W + halo || sy < -halo || sy >= COVER_H + halo) return 0;
  if (sx < 0 || sx >= COVER_W || sy < 0 || sy >= COVER_H) return 1;              // halo
  if (sx < edge || sx >= COVER_W - edge || sy < edge || sy >= COVER_H - edge) return 2;  // card edge
  const uint8_t *bits = (coverBits && index >= 0 && index < edition.articleCount) ? coverBits + (size_t)index * COVER_BYTES : nullptr;
  if (!bits) return 2;                                                           // no picture: solid black
  int n = 0;
  for (int k = 0; k < 4; k++)
  {
    float du = (k & 1) ? 0.25f : -0.25f, dv = (k & 2) ? 0.25f : -0.25f;
    int ix = (int)(sx + (du * g.c + dv * g.s) / g.scale);
    int iy = (int)(sy + (-du * g.s + dv * g.c) / g.scale);
    if (ix < 0) ix = 0; if (ix >= COVER_W) ix = COVER_W - 1;
    if (iy < 0) iy = 0; if (iy >= COVER_H) iy = COVER_H - 1;
    if (bits[iy * COVER_STRIDE + (ix >> 3)] & (0x80 >> (ix & 7))) n++;
  }
  static const uint8_t threshold[4] = { 1, 3, 4, 2 };
  return n >= threshold[((py & 1) << 1) | (px & 1)] ? 2 : 1;
}

void drawCover(int index)
{
  CoverGeom g = coverGeom(index);
  for (int py = g.by; py < g.by + g.bh; py++)
    for (int px = g.bx; px < g.bx + g.bw; px++)
    {
      int s = coverSample(index, g, px, py);
      if (s) display.drawPixel(px, py, s == 2 ? GxEPD_BLACK : GxEPD_WHITE);
    }
}

int starPixel(int x, int y)
{
  float px = x + 0.5f, py = y + 0.5f, best = 1e9f;
  const unsigned n = sizeof(kStar) / sizeof(kStar[0]) / 2;
  for (unsigned i = 0; i + 1 < n; i++)
  {
    float ax = kStar[2 * i], ay = kStar[2 * i + 1], bx = kStar[2 * i + 2], by = kStar[2 * i + 3];
    float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
    if (len2 < 0.01f) continue;
    float t = ((px - ax) * vx + (py - ay) * vy) / len2;
    if (t < 0) t = 0; if (t > 1) t = 1;
    float dx = px - (ax + t * vx), dy = py - (ay + t * vy), d = dx * dx + dy * dy;
    if (d < best) best = d;
  }
  if (best <= 1.45f * 1.45f) return 2;
  if (best <= 2.6f * 2.6f) return 1;
  return 0;
}

void starBox(int index, int &x, int &y)
{
  const Slot &sl = kSlots[index];
  CoverGeom g = coverGeom(index);
  int cellX = SAFE_X + sl.x, cellY = SAFE_Y + sl.y;
  if (slotTextLeft(sl)) x = g.bx & ~7;                                        // on the cover, so the copy can use the left
  else x = cellX + 8;                                                         // still a multiple of 8
  if (sl.style == S_BANNER) y = cellY + BANNER_H + 2;                         // below the banner, left of the cover
  else if (sl.style == S_BELOW) y = g.by + 8;
  else y = g.by + 2;
  if (y + MARK_BOX > cellY + sl.h - 1) y = cellY + sl.h - 1 - MARK_BOX;
}

void drawMark(int index)
{
  int bx, by;
  starBox(index, bx, by);
  for (int y = 0; y < MARK_BOX; y++)
    for (int x = 0; x < MARK_BOX; x++)
    {
      int p = starPixel(x, y);
      if (p) display.drawPixel(bx + x, by + y, p == 2 ? GxEPD_BLACK : GxEPD_WHITE);
    }
}

void markBitmap(int index, uint8_t *bm, bool on)
{
  memset(bm, 0xFF, MARK_BOX * MARK_BOX / 8);
  CoverGeom g = coverGeom(index);
  int bx, by;
  starBox(index, bx, by);
  for (int y = 0; y < MARK_BOX; y++)
    for (int x = 0; x < MARK_BOX; x++)
    {
      bool black = coverSample(index, g, bx + x, by + y) == 2;
      if (on)
      {
        int p = starPixel(x, y);
        if (p) black = (p == 2);
      }
      if (black) bm[y * (MARK_BOX / 8) + (x >> 3)] &= (uint8_t)~(0x80 >> (x & 7));
    }
}

void paintMark(int index, bool on, bool previous)
{
  uint8_t bm[MARK_BOX * MARK_BOX / 8];
  markBitmap(index, bm, on);
  int x, y;
  starBox(index, x, y);
  if (previous) display.epd2.writeImageToPrevious(bm, x, y, MARK_BOX, MARK_BOX);
  else display.epd2.writeImage(bm, x, y, MARK_BOX, MARK_BOX);
}

void shiftSelection(int previous)
{
  int x0, y0, x1, y1;
  starBox(previous, x0, y0);
  starBox(catalogIndex, x1, y1);
  int x = x0 < x1 ? x0 : x1;
  int y = y0 < y1 ? y0 : y1;
  int w = (x0 > x1 ? x0 : x1) + MARK_BOX - x;
  int h = (y0 > y1 ? y0 : y1) + MARK_BOX - y;
  // Current RAM only. Everything between the two stars is left as it was, so the
  // differential update changes only the stars and leaves the rest of the page alone.
  // One partial is ~1.6s; a full refresh is ~4.2s and flashes the panel.
  paintMark(previous, false, false);
  paintMark(catalogIndex, true, false);
  display.epd2.refresh(x, y, w, h);
  // These controllers swap the two RAM buffers on refresh. Write the marks again so
  // both buffers match the screen; otherwise the next move redraws the old star.
  paintMark(previous, false, false);
  paintMark(catalogIndex, true, false);
  paintMark(previous, false, true);
  paintMark(catalogIndex, true, true);
  noteUiSettled();
  Serial.printf("Catalog selection %d/%d\n", catalogIndex + 1, edition.articleCount);
}

void renderCatalog()
{
  browsing = true;
  if (edition.articleCount <= 0) return;
  if (catalogIndex < 0 || catalogIndex >= edition.articleCount) catalogIndex = 0;
  fetchCovers();

  led(0, 0, 40);
  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    drawSlotTitle();
    int n = edition.articleCount;
    if (n > MAX_ARTICLES) n = MAX_ARTICLES;
    for (int i = 0; i < n; i++)
    {
      const Article &a = edition.articles[i];
      const Slot &s = kSlots[i];
      int cellX = SAFE_X + s.x, cellY = SAFE_Y + s.y;
      int cellR = cellX + s.w, cellB = cellY + s.h;
      bool left = slotTextLeft(s);
      bool big = (s.style == S_RIGHT_BIG || s.style == S_LEFT_BIG);
      String title = String(i + 1) + ". " + a.title;

      if (s.panel) display.drawRoundRect(cellX + 1, cellY + 1, s.w - 2, s.h - 2, 8, GxEPD_BLACK);

      CoverGeom g = coverGeom(i);
      int mx, my;
      starBox(i, mx, my);
      int starR = mx + MARK_BOX + 4;
      int textX, textW, blurbY;

      if (s.style == S_BANNER)
      {
        textX = g.bx + g.bw + 6;
        if (textX < starR) textX = starR;
        textW = cellR - textX - 4;
        blurbY = cellY + BANNER_H + 22;
      }
      else if (s.style == S_BELOW)
      {
        textX = cellX + 8;
        textW = s.w - 16;
        int next = drawWrapped(textX, g.by + g.bh + 18, textW, title, F_KICKER, 2, true);
        blurbY = next + 1;
      }
      else
      {
        const GFXfont *tf = big ? F_DEK : F_KICKER;
        if (left) { textX = cellX + 8; textW = g.bx - 8 - textX; }
        else
        {
          textX = g.bx + g.bw + 6;
          if (textX < starR) textX = starR;
          textW = cellR - textX - 4;
        }
        int next = drawWrapped(textX, cellY + (big ? 24 : 20), textW, title, tf, big ? 3 : 2, true);
        blurbY = next + 1;
      }
      if (textW < 24) textW = 24;
      int lineH = fontLineHeight(F_SMALL);
      int maxLines = (cellB - 3 - blurbY) / lineH + 1;   // blurbY is a baseline
      if (maxLines < 1) maxLines = 1;
      if (a.blurb.length()) drawWrapped(textX, blurbY, textW, a.blurb, F_SMALL, maxLines, true);

      drawCover(i);

      if (s.style == S_BANNER)
      {
        // Black banner over the top of the picture, title knocked out in white.
        display.fillRoundRect(cellX + 2, cellY + 2, s.w - 4, BANNER_H, 6, GxEPD_BLACK);
        display.setTextColor(GxEPD_WHITE);
        drawWrapped(cellX + 8, cellY + 20, s.w - 16, title, F_KICKER, 2, true);
        display.setTextColor(GxEPD_BLACK);
      }
      if (i == catalogIndex) drawMark(i);
    }
  }
  while (display.nextPage());
  display.powerOff();
  led(0, 0, 0);
  noteUiSettled();
  Serial.printf("Catalog, selection %d/%d. heap %u\n", catalogIndex + 1, edition.articleCount, ESP.getFreeHeap());
}

void releaseBodiesExcept(int keep)
{
  for (int i = 0; i < edition.articleCount; i++)
  {
    if (i == keep) continue;
    edition.articles[i].body = "";
    edition.articles[i].blockCount = 0;
  }
}

bool ensureArticleBody(int i)
{
  Article &a = edition.articles[i];
  if (a.blockCount > 0) return true;
  int c = httpGetString(String("/article?i=") + i, a.body, 60000);
  if (c != HTTP_CODE_OK) a.body = "P\t[Could not load this article.]\n";
  indexBlocks(a);
  Serial.printf("  opened [%d] %s | %u bytes, %d blocks | heap %u\n", i, a.title.c_str(),
                a.body.length(), a.blockCount, ESP.getFreeHeap());
  return a.blockCount > 0;
}

void openArticle(int i)
{
  if (!edition.valid || i < 0 || i >= edition.articleCount) { noteUiSettled(); return; }
  catalogIndex = i;
  led(0, 0, 40);
  releaseBodiesExcept(i);
  if (!ensureArticleBody(i))
  {
    renderCatalog();
    return;
  }
  paginateArticle(i);
  if (pageCount == 0)
  {
    renderCatalog();
    return;
  }
  browsing = false;
  renderPage(0);
}

void loadEdition(bool refresh)
{
  led(40, 20, 0);
  Serial.printf("Fetch edition. heap %u\n", ESP.getFreeHeap());
  if (!fetchEdition(edition, refresh))
  {
    led(60, 0, 0);
    showMessageScreen("Could not fetch the edition",
                      String("WiFi: ") + (WiFi.status() == WL_CONNECTED ? "connected" : "down") +
                      ". Check WIFI_SSID and API_HOST in secrets.h. Hold either knob to retry.");
    edition.valid = false;
    pageCount = 0;
    return;
  }
  if (edition.articleCount == 0)
  {
    showMessageScreen("No stories", "The server returned an empty edition. Hold either knob to retry.");
    return;
  }
  catalogIndex = 0;
  pageCount = 0;
  renderCatalog();
}

int pageOfArticle(int article, int index)
{
  int seen = 0;
  for (int i = 0; i < pageCount; i++)
  {
    if (pageStart[i].article != article) continue;
    if (seen == index) return i;
    seen++;
  }
  return -1;
}

int pagesInArticle(int article)
{
  int n = 0;
  for (int i = 0; i < pageCount; i++) if (pageStart[i].article == article) n++;
  return n;
}

void turnPage(int dir)
{
  if (browsing || pageCount == 0 || dir == 0) return;
  int article = pageStart[currentPage].article;
  int n = pagesInArticle(article);
  if (n <= 1) return;
  int idx = 0;
  for (int i = 0; i < currentPage; i++) if (pageStart[i].article == article) idx++;
  int next = (idx + (dir > 0 ? 1 : -1)) % n;
  if (next < 0) next += n;
  int p = pageOfArticle(article, next);
  if (p >= 0 && p != currentPage) renderPage(p, false);
}

void moveCatalog(int dir)
{
  int n = edition.articleCount;
  if (n <= 1 || dir == 0) return;
  int previous = catalogIndex;
  catalogIndex = (catalogIndex + (dir > 0 ? 1 : -1) + n) % n;
  if (catalogIndex != previous) shiftSelection(previous);
}

void turnArticle(int dir)
{
  if (!edition.articleCount || dir == 0) return;
  if (browsing) { moveCatalog(dir); return; }
  if (pageCount == 0) return;
  int n = edition.articleCount;
  int next = (catalogIndex + (dir > 0 ? 1 : -1) + n) % n;
  if (next != catalogIndex) openArticle(next);
}

void onSelectClick()
{
  if (!edition.valid || edition.articleCount <= 0) { noteUiSettled(); return; }
  if (browsing) openArticle(catalogIndex);
  else renderCatalog();
}

void onHold()
{
  loadEdition(true);
  clearKnobs();
}

void service()
{
  static bool     prevDown  = false;
  static uint32_t pressedAt  = 0;
  static bool     holdFired  = false;
  static bool     selectWas  = false;

  bool rightDown = digitalRead(ENC2_SW) == LOW;
  bool leftDown = digitalRead(ENC1_SW) == LOW;
  bool down = leftDown || rightDown;
  static uint32_t seenEpoch = 0;
  if (seenEpoch != inputEpoch)
  {
    seenEpoch = inputEpoch;
    prevDown = down;
    pressedAt = millis();
    holdFired = false;
    selectWas = false;
  }

  if (uiSettled)
  {
    prevDown = down;
    holdFired = true;
    selectWas = false;
    uiSettled = false;
  }
  else
  {
    uint32_t now = millis();
    if (down && !prevDown)
    {
      pressedAt = now;
      holdFired = false;
      selectWas = leftDown;
    }
    if (down && !holdFired && now - pressedAt >= HOLD_THRESHOLD_MS)
    {
      holdFired = true;
      onHold();
    }
    if (!down && prevDown && !holdFired && selectWas) onSelectClick();
    if (uiSettled)
    {
      // Re-read: the handler blocked, so the sample from the top of the loop is stale.
      down = digitalRead(ENC2_SW) == LOW || digitalRead(ENC1_SW) == LOW;
      prevDown = down;
      holdFired = true;
      selectWas = false;
      uiSettled = false;
    }
    else prevDown = down;
  }

  int left = takeDetent(enc1Steps);
  if (left) { turnArticle(left); return; }
  int right = takeDetent(enc2Steps);
  if (right) turnPage(right);
  delay(5);
}

void enter()
{
  led(0, 0, 0);
  loadEdition(false);
  clearKnobs();
}

} // namespace news
