// CYD Spotify — WIRED controller (ESP32-2432S028)
// Talks to a host-side bridge (tools/spotify_bridge.py) over USB serial. No WiFi.
// Protocol (host -> CYD), '\n'-terminated, fields split by 0x1F:
//   T <title> <artist> <album> <durationMs> <progressMs> <playing>   track meta
//   P <progressMs> <playing>                                         progress resync
//   J <len>\n <len raw JPEG bytes>                                   album art
// CYD -> host:  N (next)  B (back)  S (toggle)   |  #... debug lines
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

#define SEP '\x1f'
#define BAUD 460800

// ---- touch ----
#define T_CLK 25
#define T_CS  33
#define T_DIN 32
#define T_DO  39
#define T_IRQ 36
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(T_CS, T_IRQ);
#define TS_MINX 300
#define TS_MAXX 3800
#define TS_MINY 300
#define TS_MAXY 3800

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite pb = TFT_eSprite(&tft);

// ---- state ----
struct NP { String title, artist, album; long duration = 0, basisProg = 0; unsigned long basisMs = 0; bool playing = false; } np;
uint8_t *curImg = nullptr; int curLen = 0;
bool firstShown = false, lastPlaying = true;

// ---- layout ----
#define COV_X 18
#define COV_Y 20
#define COV_S 148
#define TXT_X 182
#define PBY 182
#define PBH 22
#define PB_X 14
#define PB_W 292
#define CTRL_Y 216

// ---- palette ----
uint8_t aR = 40, aG = 200, aB = 110; uint16_t accent = 0x2FE8;
bool accum = false; uint32_t sumR, sumG, sumB, pxN;
String dArtist = ""; int dArtistY = 0, dTw = 0;

static inline uint16_t rgb565(int r, int g, int b) { r = constrain(r, 0, 255); g = constrain(g, 0, 255); b = constrain(b, 0, 255); return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
uint16_t dimA(float f) { return rgb565(aR * f, aG * f, aB * f); }

bool tjpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
  if (accum) { int n = w * h; for (int i = 0; i < n; i++) { uint16_t q = (bmp[i] >> 8) | (bmp[i] << 8); sumR += (q >> 11) & 0x1f; sumG += (q >> 5) & 0x3f; sumB += q & 0x1f; } pxN += n; }
  if (y >= tft.height() || y + h <= 0) return 1;
  tft.pushImage(x, y, w, h, bmp); return 1;
}
void msToClock(long ms, char *out) { if (ms < 0) ms = 0; long s = ms / 1000; sprintf(out, "%ld:%02ld", s / 60, s % 60); }
String fit(const String &s, int maxW) { if (tft.textWidth(s) <= maxW) return s; String r = s; while (r.length() > 1 && tft.textWidth(r + "..") > maxW) r.remove(r.length() - 1); return r + ".."; }
void twoLines(const String &s, int maxW, String &l1, String &l2) {
  l1 = ""; l2 = ""; String line = ""; int start = 0;
  for (int i = 0; i <= (int)s.length(); i++) {
    if (i == (int)s.length() || s[i] == ' ') {
      String word = s.substring(start, i); String trial = line.length() ? line + " " + word : word;
      if (tft.textWidth(trial) <= maxW) line = trial; else { l1 = line; l2 = s.substring(line.length() ? line.length() + 1 : 0); return; }
      start = i + 1;
    }
  }
  l1 = line;
}
void deriveAccent() {
  int avg = (aR + aG + aB) / 3;
  int r = avg + (int)((aR - avg) * 1.8f), g = avg + (int)((aG - avg) * 1.8f), b = avg + (int)((aB - avg) * 1.8f);
  int luma = (r * 77 + g * 150 + b * 29) >> 8; if (luma < 1) luma = 1; float f = 210.0f / luma;
  aR = constrain((int)(r * f), 0, 255); aG = constrain((int)(g * f), 0, 255); aB = constrain((int)(b * f), 0, 255);
  accent = rgb565(aR, aG, aB);
}
void drawControls(bool playing, int hi = -1) {
  int cx[3] = {90, 160, 230}, cy = CTRL_Y + 12;
  tft.fillRect(0, CTRL_Y - 4, 320, 240 - (CTRL_Y - 4), TFT_BLACK);
  for (int b = 0; b < 3; b++) {
    if (b == hi) tft.fillCircle(cx[b], cy, 18, dimA(0.4f));
    uint16_t c = rgb565(235, 235, 240); int x = cx[b];
    if (b == 0) { tft.fillTriangle(x + 7, cy - 8, x + 7, cy + 8, x - 1, cy, c); tft.fillTriangle(x - 1, cy - 8, x - 1, cy + 8, x - 9, cy, c); tft.fillRect(x - 11, cy - 8, 3, 16, c); }
    else if (b == 1) { if (playing) { tft.fillRect(x - 7, cy - 9, 5, 18, c); tft.fillRect(x + 2, cy - 9, 5, 18, c); } else tft.fillTriangle(x - 6, cy - 9, x - 6, cy + 9, x + 9, cy, c); }
    else { tft.fillTriangle(x - 7, cy - 8, x - 7, cy + 8, x + 1, cy, c); tft.fillTriangle(x + 1, cy - 8, x + 1, cy + 8, x + 9, cy, c); tft.fillRect(x + 8, cy - 8, 3, 16, c); }
  }
}
void drawBar(long prog, long dur, bool playing) {
  pb.fillSprite(TFT_BLACK);
  char el[12], tot[12]; msToClock(prog, el); msToClock(dur, tot);
  pb.setTextDatum(TL_DATUM); pb.setTextColor(rgb565(230, 230, 230)); pb.drawString(el, PB_X, 1, 2);
  pb.setTextDatum(TR_DATUM); pb.setTextColor(rgb565(150, 150, 150)); pb.drawString(tot, PB_X + PB_W, 1, 2);
  float fr = dur > 0 ? (float)prog / dur : 0; fr = fr < 0 ? 0 : (fr > 1 ? 1 : fr);
  int fw = (int)(fr * PB_W);
  pb.fillRoundRect(PB_X, 16, PB_W, 5, 2, rgb565(54, 54, 62));
  if (fw > 2) pb.fillRoundRect(PB_X, 16, fw, 5, 2, playing ? accent : rgb565(150, 150, 150));
  int kx = PB_X + fw; kx = constrain(kx, PB_X + 4, PB_X + PB_W - 4);
  pb.fillCircle(kx, 18, 4, TFT_WHITE);
  pb.pushSprite(0, PBY);
}
void drawCoverFrame() {
  for (int i = 4; i >= 1; i--) { float f = (5 - i) / 5.0f * 0.30f; tft.drawRoundRect(COV_X - 1 - i, COV_Y - 1 - i, COV_S + 2 + 2 * i, COV_S + 2 + 2 * i, 10 + i, dimA(f)); }
  tft.drawRoundRect(COV_X - 1, COV_Y - 1, COV_S + 2, COV_S + 2, 8, rgb565(235, 235, 240));
}
void renderText() {
  tft.fillScreen(TFT_BLACK);
  if (curImg) { accum = false; TJpgDec.setJpgScale(1); TJpgDec.drawJpg(COV_X, COV_Y, curImg, curLen); }
  else { tft.fillRoundRect(COV_X, COV_Y, COV_S, COV_S, 12, rgb565(26, 26, 32)); tft.setTextDatum(MC_DATUM); tft.setTextColor(rgb565(80, 80, 92)); tft.setFreeFont(&FreeSans9pt7b); tft.drawString("...", COV_X + COV_S / 2, COV_Y + COV_S / 2); }
  drawCoverFrame();
  int tw = 320 - TXT_X - 8; dTw = tw;
  tft.setTextDatum(TL_DATUM); tft.setFreeFont(&FreeSansBold12pt7b);
  String l1, l2; twoLines(np.title, tw, l1, l2);
  int ty = l2.length() ? 42 : 56;
  tft.setTextColor(TFT_WHITE); tft.drawString(l1, TXT_X, ty);
  if (l2.length()) tft.drawString(l2, TXT_X, ty + 26);
  int ay = l2.length() ? 100 : 92; dArtistY = ay; dArtist = fit(np.artist, tw);
  tft.setFreeFont(&FreeSansBold9pt7b); tft.setTextColor(accent); tft.drawString(dArtist, TXT_X, ay);
  tft.setFreeFont(&FreeSans9pt7b); tft.setTextColor(rgb565(175, 175, 182)); tft.drawString(fit(np.album, tw), TXT_X, ay + 24);
  drawControls(np.playing);
}
void applyCover(uint8_t *buf, int len) {
  if (curImg) free(curImg);
  curImg = buf; curLen = len;
  sumR = sumG = sumB = pxN = 0; accum = true;
  TJpgDec.setJpgScale(1); TJpgDec.drawJpg(COV_X, COV_Y, curImg, curLen); accum = false;
  if (pxN) { aR = (sumR / pxN) << 3; aG = (sumG / pxN) << 2; aB = (sumB / pxN) << 3; }
  deriveAccent();
  drawCoverFrame();
  tft.fillRect(TXT_X, dArtistY - 2, dTw + 4, 22, TFT_BLACK);
  tft.setTextDatum(TL_DATUM); tft.setFreeFont(&FreeSansBold9pt7b); tft.setTextColor(accent); tft.drawString(dArtist, TXT_X, dArtistY);
}
void splash(const char *l1, const char *l2, uint16_t c2) {
  tft.fillScreen(TFT_BLACK); tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b); tft.setTextColor(rgb565(30, 215, 96)); tft.drawString(l1, 160, 104);
  tft.setFreeFont(&FreeSans9pt7b); tft.setTextColor(c2); tft.drawString(l2, 160, 140);
}

// ---- serial protocol ----
String rxLine = "";
void handleLine(const String &ln) {
  if (ln.length() < 1) return;
  char cmd = ln[0];
  if (cmd == 'T') {
    // T<SEP>title<SEP>artist<SEP>album<SEP>dur<SEP>prog<SEP>playing
    String f[7]; int n = 0, start = 2;
    for (int i = 2; i <= (int)ln.length() && n < 7; i++) {
      if (i == (int)ln.length() || ln[i] == SEP) { f[n++] = ln.substring(start, i); start = i + 1; }
    }
    np.title = f[0]; np.artist = f[1]; np.album = f[2];
    np.duration = f[3].toInt(); np.basisProg = f[4].toInt(); np.basisMs = millis();
    np.playing = f[5].toInt() != 0; lastPlaying = np.playing;
    renderText(); firstShown = true;
  } else if (cmd == 'P') {
    String f[2]; int n = 0, start = 2;
    for (int i = 2; i <= (int)ln.length() && n < 2; i++) { if (i == (int)ln.length() || ln[i] == SEP) { f[n++] = ln.substring(start, i); start = i + 1; } }
    np.basisProg = f[0].toInt(); np.basisMs = millis(); np.playing = f[1].toInt() != 0;
  }
}
void readImage(int len) {
  if (len <= 0 || len > 220000) return;
  uint8_t *buf = (uint8_t *)malloc(len); if (!buf) return;
  int got = 0; unsigned long t = millis();
  while (got < len && millis() - t < 4000) {
    int a = Serial.available();
    if (a > 0) { got += Serial.readBytes(buf + got, min(a, len - got)); t = millis(); }
  }
  if (got == len) { applyCover(buf, len); Serial.printf("#img ok %d\n", len); }
  else { free(buf); Serial.printf("#img short %d/%d\n", got, len); }
}
void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (rxLine.startsWith("J")) { int len = rxLine.substring(2).toInt(); rxLine = ""; readImage(len); return; }
      handleLine(rxLine); rxLine = "";
    } else if (c != '\r') { rxLine += c; if (rxLine.length() > 400) rxLine = ""; }
  }
}

// ---- touch ----
bool touchDown = false;
void handleTouch() {
  if (!ts.touched()) { touchDown = false; return; }
  if (touchDown) return; touchDown = true;
  TS_Point p = ts.getPoint();
  int sx = map(p.x, TS_MINX, TS_MAXX, 0, 320);
  int sy = map(p.y, TS_MINY, TS_MAXY, 0, 240);
  Serial.printf("#touch %d %d -> %d %d\n", p.x, p.y, sx, sy);
  if (sy < 200) return;
  int b = sx < 120 ? 2 : (sx < 213 ? 3 : 1);   // 2 prev, 3 toggle, 1 next
  int hi = (b == 2) ? 0 : (b == 3) ? 1 : 2;
  if (b == 3) {                                  // optimistic play/pause
    np.playing = !np.playing;
    if (np.playing) np.basisMs = millis(); else np.basisProg += (long)(millis() - np.basisMs);
    lastPlaying = np.playing; drawControls(np.playing, 1);
    Serial.print("S\n");
  } else { drawControls(np.playing, hi); Serial.print(b == 2 ? "B\n" : "N\n"); }
}

// ===================================================================
void setup() {
  Serial.begin(BAUD);
  tft.init(); tft.setRotation(1); tft.invertDisplay(true);
  TJpgDec.setSwapBytes(true); TJpgDec.setCallback(tjpgOutput);
  touchSPI.begin(T_CLK, T_DO, T_DIN, T_CS); ts.begin(touchSPI); ts.setRotation(1);
  pb.setColorDepth(16); pb.createSprite(320, PBH);
  tft.invertDisplay(true);
  splash("CYD Spotify", "connect to host...", TFT_CYAN);
  Serial.print("#ready\n");
}

unsigned long lastFrame = 0;
void loop() {
  readSerial();
  handleTouch();
  if (firstShown && np.playing != lastPlaying && !touchDown) { lastPlaying = np.playing; drawControls(np.playing); }
  if (firstShown && millis() - lastFrame >= 33) {
    lastFrame = millis();
    long prog = np.basisProg + (np.playing ? (long)(millis() - np.basisMs) : 0);
    if (prog < 0) prog = 0; if (np.duration > 0 && prog > np.duration) prog = np.duration;
    drawBar(prog, np.duration, np.playing);
  }
}
