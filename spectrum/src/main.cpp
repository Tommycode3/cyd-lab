// CYD Spectrum — Bluetooth A2DP music visualizer (ESP32-2432S028)
// Phone/Spotify --A2DP--> ESP32 --> internal DAC (CYD speaker) + live FFT spectrum.
// Track title/artist read over Bluetooth (AVRCP). No WiFi.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <arduinoFFT.h>
#include "BluetoothA2DPSink.h"

// ---------------- Display ----------------
TFT_eSPI tft = TFT_eSPI();
#define BG        TFT_BLACK
#define AREA_TOP  46
#define AREA_BOT  236
#define AREA_H    (AREA_BOT - AREA_TOP)   // 190 px
#define NUM_BARS  32
#define BARW      (320 / NUM_BARS)        // 10 px
#define GAP       2                       // -> 8 px bar

// ---------------- LED (active low) ----------------
#define LED_R 4
#define LED_G 16
#define LED_B 17

// ---------------- Audio / FFT ----------------
#define SAMPLES        512
#define SAMPLING_FREQ  44100.0
double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLING_FREQ);

volatile int16_t sbuf[SAMPLES];
volatile int  sidx = 0;
volatile bool sready = false;

int bandEdge[NUM_BARS + 1];
int   barH[NUM_BARS]   = {0};
int   prevH[NUM_BARS]  = {0};
int   peak[NUM_BARS]   = {0};
int   prevPeak[NUM_BARS] = {0};
uint16_t prevCol[NUM_BARS] = {0};

// display / log-magnitude mapping (tune if bars too small/large)
#define LOG_FLOOR  2.3     // below this = silence
#define LOG_CEIL   6.2     // full-height magnitude
#define FALL       7       // px/frame the bar falls
#define PEAK_FALL  1       // px/frame the peak cap falls

// ---------------- Bluetooth ----------------
BluetoothA2DPSink a2dp_sink;
volatile bool connected = false;
String g_title = "", g_artist = "";
volatile bool metaDirty = true, connDirty = true;

// ===================================================================
void read_data_stream(const uint8_t *data, uint32_t len) {
  if (sready) return;                       // consumer still busy: skip chunk
  const int16_t *s = (const int16_t *)data;
  uint32_t frames = len / 4;                // stereo 16-bit -> 4 bytes/frame
  for (uint32_t i = 0; i < frames && sidx < SAMPLES; i++) {
    int32_t l = s[2 * i], r = s[2 * i + 1];
    sbuf[sidx++] = (int16_t)((l + r) >> 1);
  }
  if (sidx >= SAMPLES) { sready = true; sidx = 0; }
}

void avrc_metadata_callback(uint8_t id, const uint8_t *text) {
  if (id == ESP_AVRC_MD_ATTR_TITLE)  { g_title  = String((const char *)text); metaDirty = true; }
  if (id == ESP_AVRC_MD_ATTR_ARTIST) { g_artist = String((const char *)text); metaDirty = true; }
}

void conn_state_callback(esp_a2d_connection_state_t state, void *) {
  connected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  connDirty = true;
  if (!connected) { g_title = ""; g_artist = ""; }
}

// ---------------- UI helpers ----------------
uint16_t barColor(int h) {
  float r = (float)h / AREA_H;
  if (r > 0.82) return TFT_RED;
  if (r > 0.58) return 0xFD20;   // amber
  if (r > 0.30) return 0x07E0;   // green
  return 0x0400;                 // dim green
}

void drawWaitScreen() {
  tft.fillScreen(BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, BG);
  tft.drawString("CYD SPECTRUM", 160, 90, 4);
  tft.setTextColor(TFT_WHITE, BG);
  tft.drawString("Bluetooth: connect to", 160, 130, 4);
  tft.setTextColor(0x07FF, BG);
  tft.drawString("\"CYD-Music\"", 160, 158, 4);
  tft.setTextColor(TFT_DARKGREY, BG);
  tft.drawString("then start Spotify", 160, 190, 2);
}

void drawFrame() {
  tft.fillScreen(BG);
  tft.drawFastHLine(0, AREA_BOT + 1, 320, 0x2104);
  for (int b = 0; b < NUM_BARS; b++) {
    barH[b] = prevH[b] = peak[b] = prevPeak[b] = 0;
    prevCol[b] = 0xFFFF;
  }
  metaDirty = true;
}

void drawMeta() {
  tft.fillRect(0, 0, 320, AREA_TOP - 2, BG);
  tft.setTextDatum(TL_DATUM);
  String t = g_title.length()  ? g_title  : String("Now playing...");
  String a = g_artist.length() ? g_artist : String("");
  // truncate to width
  if (t.length() > 30) t = t.substring(0, 29) + "~";
  if (a.length() > 44) a = a.substring(0, 43) + "~";
  tft.setTextColor(TFT_WHITE, BG);
  tft.drawString(t, 6, 4, 4);
  tft.setTextColor(0x07FF, BG);
  tft.drawString(a, 6, 30, 2);
}

// ===================================================================
void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println("\n[CYD Spectrum] boot");

  pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  digitalWrite(LED_G, HIGH); digitalWrite(LED_B, HIGH);   // off (active low)
  ledcSetup(0, 5000, 8); ledcAttachPin(LED_R, 0);
  ledcWrite(0, 255);                                      // red off

  tft.init();
  tft.setRotation(1);
  drawWaitScreen();

  // log-spaced FFT band edges (bins 2 .. SAMPLES/2-1)
  const double lo = 2.0, hi = (SAMPLES / 2) - 1;
  for (int i = 0; i <= NUM_BARS; i++) {
    bandEdge[i] = (int)(lo * pow(hi / lo, (double)i / NUM_BARS));
    if (i > 0 && bandEdge[i] <= bandEdge[i - 1]) bandEdge[i] = bandEdge[i - 1] + 1;
  }

  // ---- Bluetooth A2DP sink -> internal DAC (GPIO26 = CYD speaker) ----
  static i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_MSB,
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  a2dp_sink.set_i2s_config(i2s_config);
  a2dp_sink.set_stream_reader(read_data_stream, true);    // true = still play audio
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink.set_on_connection_state_changed(conn_state_callback);
  a2dp_sink.start("CYD-Music");
  Serial.println("[bt] A2DP sink 'CYD-Music' started");
}

// ===================================================================
bool viz = false;

void loop() {
  // connection state transitions
  if (connDirty) {
    connDirty = false;
    if (connected && !viz)      { drawFrame(); viz = true;  Serial.println("[bt] connected"); }
    else if (!connected && viz) { drawWaitScreen(); viz = false; Serial.println("[bt] disconnected"); }
  }

  if (viz && metaDirty) { metaDirty = false; drawMeta(); }

  if (!viz || !sready) { delay(2); return; }

  // ---- copy snapshot, release buffer, run FFT ----
  for (int i = 0; i < SAMPLES; i++) { vReal[i] = (double)sbuf[i]; vImag[i] = 0.0; }
  sready = false;

  FFT.dcRemoval();
  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  // ---- bins -> bars (peak per log band, compressed) ----
  int bassAcc = 0;
  for (int b = 0; b < NUM_BARS; b++) {
    int loB = bandEdge[b], hiB = bandEdge[b + 1];
    double m = 0;
    for (int k = loB; k < hiB; k++) if (vReal[k] > m) m = vReal[k];
    double lv = log10(m + 1.0);
    int h = (int)((lv - LOG_FLOOR) / (LOG_CEIL - LOG_FLOOR) * AREA_H);
    if (h < 0) h = 0; if (h > AREA_H) h = AREA_H;

    if (h >= barH[b]) barH[b] = h;
    else { barH[b] -= FALL; if (barH[b] < h) barH[b] = h; if (barH[b] < 0) barH[b] = 0; }

    if (barH[b] >= peak[b]) peak[b] = barH[b];
    else { peak[b] -= PEAK_FALL; if (peak[b] < barH[b]) peak[b] = barH[b]; }

    if (b < 4) bassAcc += barH[b];
  }

  // ---- render (delta, flicker-free) ----
  for (int b = 0; b < NUM_BARS; b++) {
    int x = b * BARW, w = BARW - GAP;
    int nb = barH[b], pb = prevH[b];
    uint16_t col = barColor(nb);

    if (col != prevCol[b]) {                       // color bucket changed: repaint
      if (nb > 0)       tft.fillRect(x, AREA_BOT - nb, w, nb, col);
      if (nb < AREA_H)  tft.fillRect(x, AREA_TOP, w, AREA_H - nb, BG);
    } else if (nb > pb) {
      tft.fillRect(x, AREA_BOT - nb, w, nb - pb, col);          // grew
    } else if (nb < pb) {
      tft.fillRect(x, AREA_BOT - pb, w, pb - nb, BG);           // shrank
    }

    // peak cap (2 px, white)
    if (peak[b] != prevPeak[b]) {
      int oy = AREA_BOT - prevPeak[b];
      tft.fillRect(x, oy - 1, w, 2, (prevPeak[b] <= nb) ? col : BG);
      int py = AREA_BOT - peak[b];
      tft.fillRect(x, py - 1, w, 2, TFT_WHITE);
    }
    prevH[b] = nb; prevCol[b] = col; prevPeak[b] = peak[b];
  }

  // bass -> red LED glow (active low PWM)
  int bass = bassAcc / 4 * 255 / AREA_H;
  if (bass > 255) bass = 255;
  ledcWrite(0, 255 - bass);
}
