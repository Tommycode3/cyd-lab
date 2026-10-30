// Desk Companion — CYD ESP32-2432S028
// NTP clock + live weather (Open-Meteo, no API key, IP geolocation) + touch Pomodoro.
// Touch: tap anywhere = start/pause focus · hold >1.2s = reset.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

// ---------------- Touch (XPT2046 on VSPI) ----------------
#define T_IRQ  36
#define T_MOSI 32
#define T_MISO 39
#define T_CLK  25
#define T_CS   33
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(T_CS, T_IRQ);

// ---------------- Onboard RGB LED (active LOW) ----------------
#define LED_R 4
#define LED_G 16
#define LED_B 17

TFT_eSPI tft = TFT_eSPI();

// ---------------- Colors ----------------
#define C_BG      0x0000            // black
#define C_CARD    0x10A2            // dark slate
#define C_CLOCK   TFT_WHITE
#define C_DIM     0x8410            // grey
#define C_ACCENT  0xFD20            // amber/orange
#define C_BREAK   0x05FF            // cyan
#define C_OK      0x2FE8            // green

// ---------------- Locale (UTC offset comes from ip-api / Open-Meteo) ----------------
const char* WDAY[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
const char* MON[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

// ---------------- Weather ----------------
struct Weather {
  bool ok = false;
  float temp = 0, tmax = 0, tmin = 0;
  int code = -1;
  String city = "";
} wx;
double gLat = 0, gLon = 0;
long gOffset = 0;          // UTC offset in seconds (incl. DST), from ip-api
bool haveLoc = false;
unsigned long lastWxMs = 0;
const unsigned long WX_PERIOD = 15UL * 60UL * 1000UL;  // 15 min

// ---------------- Pomodoro ----------------
enum PState { P_IDLE, P_FOCUS, P_PAUSED, P_BREAK };
PState pstate = P_IDLE;
const unsigned long FOCUS_MS = 25UL * 60UL * 1000UL;
const unsigned long BREAK_MS = 5UL  * 60UL * 1000UL;
unsigned long phaseTotal = FOCUS_MS;
unsigned long endMs = 0;       // when running: absolute end
unsigned long remainMs = 0;    // when paused: frozen remaining

// ---------------- Redraw caches ----------------
String cTime = "", cDate = "", cWxTop = "", cWxBot = "", cPomo = "", cBar = "";
int lastBarW = -1, lastBarCol = -1;

// ===================================================================
void setLed(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}

void applyClock() {
  configTime(gOffset, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
}

void banner(const char* msg, uint16_t col = C_DIM) {
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(col, C_BG);
  tft.drawString(msg, 160, 120, 4);
}

// ---- HTTP helpers -------------------------------------------------
bool httpGET(const String& url, bool secure, String& out) {
  HTTPClient http;
  bool ok = false;
  if (secure) {
    WiFiClientSecure* c = new WiFiClientSecure();
    c->setInsecure();
    if (http.begin(*c, url)) {
      http.setTimeout(8000);
      int code = http.GET();
      if (code == 200) { out = http.getString(); ok = true; }
      else Serial.printf("[http] %s -> %d\n", url.c_str(), code);
      http.end();
    }
    delete c;
  } else {
    WiFiClient c;
    if (http.begin(c, url)) {
      http.setTimeout(8000);
      int code = http.GET();
      if (code == 200) { out = http.getString(); ok = true; }
      else Serial.printf("[http] %s -> %d\n", url.c_str(), code);
      http.end();
    }
  }
  return ok;
}

void fetchLocation() {
  String body;
  if (!httpGET("http://ip-api.com/json/?fields=status,city,lat,lon,offset,timezone", false, body)) return;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  if (String(doc["status"] | "") != "success") return;
  gLat = doc["lat"] | 0.0;
  gLon = doc["lon"] | 0.0;
  gOffset = doc["offset"] | 0L;         // seconds from UTC, DST already applied
  wx.city = String((const char*)(doc["city"] | "?"));
  haveLoc = (gLat != 0.0 || gLon != 0.0);
  Serial.printf("[loc] %s  %.4f,%.4f  off=%lds tz=%s\n",
                wx.city.c_str(), gLat, gLon, gOffset,
                (const char*)(doc["timezone"] | "?"));
}

void fetchWeather() {
  if (!haveLoc) fetchLocation();
  if (!haveLoc) return;
  char url[256];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min"
    "&timezone=auto&forecast_days=1", gLat, gLon);
  String body;
  if (!httpGET(String(url), true, body)) return;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  wx.temp = doc["current"]["temperature_2m"] | 0.0f;
  wx.code = doc["current"]["weather_code"] | -1;
  wx.tmax = doc["daily"]["temperature_2m_max"][0] | 0.0f;
  wx.tmin = doc["daily"]["temperature_2m_min"][0] | 0.0f;
  // Open-Meteo returns the authoritative UTC offset for the location — use it
  // to correct the clock even if ip-api's offset was missing/flaky.
  long off = doc["utc_offset_seconds"] | gOffset;
  if (off != gOffset) {
    gOffset = off; applyClock();
    Serial.printf("[tz] offset %lds (open-meteo)\n", gOffset);
  }
  wx.ok = true;
  lastWxMs = millis();
  Serial.printf("[wx] %.1fC code=%d hi=%.0f lo=%.0f\n", wx.temp, wx.code, wx.tmax, wx.tmin);
}

const char* wxLabel(int c) {
  if (c < 0) return "--";
  if (c == 0) return "Clear";
  if (c <= 2) return "Partly cl.";
  if (c == 3) return "Cloudy";
  if (c <= 48) return "Fog";
  if (c <= 57) return "Drizzle";
  if (c <= 67) return "Rain";
  if (c <= 77) return "Snow";
  if (c <= 82) return "Showers";
  if (c <= 86) return "Snow";
  return "Storm";
}

// ---- Layout drawing ----------------------------------------------
void drawFrame() {
  tft.fillScreen(C_BG);
  // Pomodoro card
  tft.fillRoundRect(6, 158, 308, 76, 8, C_CARD);
  // static hint
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_DIM, C_CARD);
  tft.drawString("Tap = focus/pause   .   Hold = reset", 160, 226, 2);
  cTime = cDate = cWxTop = cWxBot = cPomo = cBar = "";
  lastBarW = -1; lastBarCol = -1;
}

void field(String& cache, const String& val, int x, int y, uint8_t font,
           uint16_t col, uint16_t bg, uint8_t datum, int clrW, int clrH) {
  if (cache == val) return;
  tft.fillRect(x - clrW / 2, y - clrH / 2, clrW, clrH, bg);
  tft.setTextDatum(datum);
  tft.setTextColor(col, bg);
  tft.drawString(val, x, y, font);
  cache = val;
}

void updateClockDate() {
  struct tm t;
  if (!getLocalTime(&t, 50)) {
    field(cTime, "--:--", 160, 52, 7, C_DIM, C_BG, MC_DATUM, 220, 60);
    return;
  }
  char hm[6], sec[3], date[24];
  snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
  snprintf(sec, sizeof(sec), "%02d", t.tm_sec);
  snprintf(date, sizeof(date), "%s %d %s", WDAY[t.tm_wday], t.tm_mday, MON[t.tm_mon]);
  field(cTime, String(hm), 148, 52, 7, C_CLOCK, C_BG, MC_DATUM, 230, 60);
  // seconds (small, to the right of the clock)
  static String cSec = "";
  if (cSec != String(sec)) {
    tft.fillRect(270, 34, 44, 34, C_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_ACCENT, C_BG);
    tft.drawString(String(sec), 272, 40, 4);
    cSec = String(sec);
  }
  field(cDate, String(date), 160, 96, 4, C_DIM, C_BG, MC_DATUM, 300, 30);
}

void updateWeather() {
  String top, bot;
  if (wx.ok) {
    char a[40], b[40];
    snprintf(a, sizeof(a), "%s  %.0f C", wx.city.c_str(), wx.temp);
    snprintf(b, sizeof(b), "%s   ^%.0f  v%.0f", wxLabel(wx.code), wx.tmax, wx.tmin);
    top = a; bot = b;
  } else if (WiFi.status() == WL_CONNECTED) {
    top = "weather..."; bot = "";
  } else {
    top = "offline"; bot = "";
  }
  field(cWxTop, top, 160, 124, 4, C_BREAK, C_BG, MC_DATUM, 320, 28);
  field(cWxBot, bot, 160, 146, 2, C_DIM,   C_BG, MC_DATUM, 320, 20);
}

unsigned long remainingMs() {
  if (pstate == P_FOCUS || pstate == P_BREAK) {
    long r = (long)endMs - (long)millis();
    return r > 0 ? (unsigned long)r : 0;
  }
  if (pstate == P_PAUSED) return remainMs;
  return FOCUS_MS;  // idle preview
}

void updatePomodoro() {
  unsigned long rem = remainingMs();
  unsigned int mm = rem / 60000UL;
  unsigned int ss = (rem % 60000UL) / 1000UL;
  char t[16]; snprintf(t, sizeof(t), "%02u:%02u", mm, ss);

  uint16_t col; String tag;
  switch (pstate) {
    case P_FOCUS:  col = C_ACCENT; tag = "FOCUS ";  break;
    case P_PAUSED: col = C_DIM;    tag = "PAUSED ";  break;
    case P_BREAK:  col = C_BREAK;  tag = "BREAK ";  break;
    default:       col = C_OK;     tag = "";        break;
  }
  String label = (pstate == P_IDLE) ? String("Pomodoro 25:00") : (tag + String(t));
  field(cPomo, label, 160, 190, 4, col, C_CARD, MC_DATUM, 300, 34);

  // progress bar
  float frac = 0;
  if (pstate == P_FOCUS || pstate == P_PAUSED) frac = 1.0f - (float)rem / (float)FOCUS_MS;
  else if (pstate == P_BREAK) frac = 1.0f - (float)rem / (float)BREAK_MS;
  int barW = (int)(frac * 296.0f);
  if (barW != lastBarW || (int)col != lastBarCol) {
    tft.fillRoundRect(12, 168, 296, 8, 3, 0x2104);
    if (barW > 3) tft.fillRoundRect(12, 168, barW, 8, 3, col);
    lastBarW = barW; lastBarCol = col;
  }
}

// ---- Pomodoro control --------------------------------------------
void startPhase(PState s, unsigned long dur) {
  pstate = s; phaseTotal = dur; endMs = millis() + dur;
  if (s == P_FOCUS) setLed(1, 0, 0);
  else if (s == P_BREAK) setLed(0, 0, 1);
}
void resetPomodoro() { pstate = P_IDLE; setLed(0, 1, 0); }

void toggle() {
  switch (pstate) {
    case P_IDLE:  startPhase(P_FOCUS, FOCUS_MS); break;
    case P_FOCUS: remainMs = remainingMs(); pstate = P_PAUSED; setLed(0,0,0); break;
    case P_PAUSED: endMs = millis() + remainMs; pstate = P_FOCUS; setLed(1,0,0); break;
    case P_BREAK: resetPomodoro(); break;
  }
}

void flashDone(uint16_t col) {
  for (int i = 0; i < 3; i++) {
    tft.fillRect(6, 158, 308, 76, col); delay(120);
    tft.fillRoundRect(6, 158, 308, 76, 8, C_CARD); delay(120);
  }
  cPomo = ""; cBar = ""; lastBarW = -1;  // force redraw
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_DIM, C_CARD);
  tft.drawString("Tap = focus/pause   .   Hold = reset", 160, 226, 2);
}

// ---- Touch handling ----------------------------------------------
bool touchActive = false;
unsigned long touchStart = 0;
void handleTouch() {
  bool now = ts.touched();
  if (now && !touchActive) { touchActive = true; touchStart = millis(); }
  if (!now && touchActive) {
    touchActive = false;
    unsigned long dur = millis() - touchStart;
    if (dur > 1200) resetPomodoro();
    else toggle();
  }
}

// ===================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[Desk Companion] boot");

  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  setLed(0, 0, 0);

  tft.init();
  tft.setRotation(1);            // landscape 320x240
  tft.fillScreen(C_BG);

  touchSPI.begin(T_CLK, T_MISO, T_MOSI, T_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  banner("Connecting WiFi...", C_BREAK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("AP 'CYD-Setup' if prompted", 160, 150, 2);

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  bool connected = wm.autoConnect("CYD-Setup");
  Serial.printf("[wifi] %s\n", connected ? WiFi.localIP().toString().c_str() : "offline");

  if (connected) {
    banner("Syncing time...", C_BREAK);
    for (int i = 0; i < 5 && !haveLoc; i++) {   // ip-api can be flaky; retry
      fetchLocation();
      if (!haveLoc) delay(1200);
    }
    applyClock();                                // UTC first; corrected below
    struct tm t;
    for (int i = 0; i < 20 && !getLocalTime(&t, 500); i++) { /* wait NTP */ }
    fetchWeather();                              // sets authoritative offset
    if (getLocalTime(&t, 10))
      Serial.printf("[time] %02d:%02d:%02d (offset %lds)\n",
                    t.tm_hour, t.tm_min, t.tm_sec, gOffset);
  }

  resetPomodoro();     // LED green idle
  drawFrame();
}

void loop() {
  static unsigned long lastSec = 0;
  handleTouch();

  // phase completion
  if ((pstate == P_FOCUS || pstate == P_BREAK) && remainingMs() == 0) {
    if (pstate == P_FOCUS) { flashDone(C_ACCENT); startPhase(P_BREAK, BREAK_MS); }
    else { flashDone(C_BREAK); resetPomodoro(); }
  }

  unsigned long ms = millis();
  if (ms - lastSec >= 250) {   // refresh 4x/s for smooth seconds + timer
    lastSec = ms;
    updateClockDate();
    updatePomodoro();
    updateWeather();
  }

  // self-heal: if location/timezone never came up, keep retrying every 30s
  static unsigned long lastLocTry = 0;
  if (WiFi.status() == WL_CONNECTED && !haveLoc && (ms - lastLocTry > 30000)) {
    lastLocTry = ms;
    fetchLocation();
    if (haveLoc) { applyClock(); fetchWeather(); }
  }

  // periodic weather refresh
  if (WiFi.status() == WL_CONNECTED && (ms - lastWxMs > WX_PERIOD)) {
    fetchWeather();
  }

  delay(5);
}
