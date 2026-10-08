#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <AnimatedGIF.h>
#include <Preferences.h>
#include <DHT.h>
#include <WebServer.h>
#include <ArduinoJson.h>

#include "animations.h"
#include "dino_game.h"

// --- Board ---
const int touchPin = 21;

// --- Weather sensors (LDR + DHT22) — Pogo reacts to the room ---
#define DHTPIN 27
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);
const int ldrPin = 34;

const unsigned long dhtIntervalMs = 3000;
const unsigned long ldrIntervalMs = 200;
unsigned long lastDhtReadAt = 0;
unsigned long lastLdrReadAt = 0;
float lastKnownTemp = NAN;
float lastKnownHum = NAN;
float ldrSmoothed = -1.0f;

const float HOT_T = 30.0f;      // >= triggers hot reaction
const float COLD_T = 12.0f;     // <= triggers cold reaction
const float HUMID_RH = 80.0f;   // >= triggers humid reaction
const float DARK_LDR = 15.0f;   // smoothed raw ADC below = "dark room"

enum EnvState { ENV_OK, ENV_HOT, ENV_COLD, ENV_HUMID, ENV_DARK };
const unsigned long sensorReactCooldownMs = 30000; // >= 30s between reactions
unsigned long lastSensorReactAt = 0;

// --- Network (network stays on for the web dashboard + sleepy zone clock) ---
const char* ssid = "jasodevi_2";
const char* password = "CLEB4F06F6";
const long gmtOffsetSec = 5 * 3600 + 45 * 60; // Kathmandu = UTC+5:45
const int daylightOffsetSec = 0;              // Nepal has no DST
const char* ntpServer = "pool.ntp.org";

// --- Identity ---
const char* PET_NAME = "Pogo";
const int PET_BIRTH_MONTH = 9;   // 1-12
const int PET_BIRTH_DAY = 19;    // 1-31

// --- Display ---
TFT_eSPI tft = TFT_eSPI();
AnimatedGIF gif;

// GIF centered on screen (computed per animation, used by GIFDraw)
int gGifX = 0;
int gGifY = 0;

// --- Happiness — persistent stat (ESP32 NVS) ---
const char* HAPPY_NS = "mochi";
const float HAPPY_START = 60.0f;
const float HAPPY_MAX = 100.0f;
const float HAPPY_MIN = 0.0f;
const float HAPPY_DECAY_PER_SEC = 0.06f;   // ~1 pt / 17s
const float HAPPY_TAP_BOOST = 6.0f;
const float HAPPY_LONGPRESS_BOOST = 15.0f;
float happy = HAPPY_START;
unsigned long lastHappyTickAt = 0;
const unsigned long happyTickMs = 5000;

// --- Petting reactions (flags consumed after the current GIF finishes) ---
bool sPlayReaction = false;
bool sPlayLove = false;

// Clock availability + periodic WiFi/NTP retry (a failed boot sync must not
// trap Pogo in the sleepy loop forever, and the dashboard needs a link).
bool sClockValid = false;
unsigned long sLastClockRetry = 0;
const unsigned long kNetRetryMs = 30UL * 1000UL; // retry every 30s while unconnected/unsynced

void loadHappiness() {
  Preferences prefs;
  prefs.begin(HAPPY_NS, true);
  happy = prefs.getFloat("happy", HAPPY_START);
  prefs.end();
}

void saveHappiness() {
  Preferences prefs;
  prefs.begin(HAPPY_NS, false);
  prefs.putFloat("happy", happy);
  prefs.end();
}

void addHappiness(float amount) {
  happy += amount;
  if (happy > HAPPY_MAX) happy = HAPPY_MAX;
  if (happy < HAPPY_MIN) happy = HAPPY_MIN;
  saveHappiness();
}

void tickHappiness() {
  unsigned long now = millis();
  if (now - lastHappyTickAt < happyTickMs && lastHappyTickAt != 0) return;
  lastHappyTickAt = now;
  if (happy <= HAPPY_MIN) return;
  happy -= HAPPY_DECAY_PER_SEC * (happyTickMs / 1000.0f);
  if (happy < HAPPY_MIN) happy = HAPPY_MIN;
}

// --- Daily streak — persistent consecutive-days petting ritual (NVS) ---
const char* STREAK_NS = "streak";
int sPetStreak = 0;

uint32_t keyFor(time_t t) {
  struct tm ti;
  localtime_r(&t, &ti);
  return ((uint32_t)(ti.tm_year + 1900) * 10000) +
         ((uint32_t)(ti.tm_mon + 1) * 100) +
         (uint32_t)ti.tm_mday;
}

void loadStreak() {
  Preferences prefs;
  prefs.begin(STREAK_NS, true);
  sPetStreak = prefs.getInt("count", 0);
  prefs.end();
}

void recordPet() {
  if (!sClockValid) return; // need a real date before counting days
  time_t now = time(nullptr);
  uint32_t today = keyFor(now);
  uint32_t yest = keyFor(now - 86400);

  Preferences prefs;
  prefs.begin(STREAK_NS, false);
  uint32_t last = prefs.getULong("day", 0);
  int streak = prefs.getInt("count", 0);
  int best = prefs.getInt("best", 0);

  if (last == today) { prefs.end(); return; } // already petted today

  streak = (last == yest) ? streak + 1 : 1;
  if (streak > best) best = streak;

  prefs.putULong("day", today);
  prefs.putInt("count", streak);
  prefs.putInt("best", best);
  prefs.end();

  sPetStreak = streak;

  // Every 5th day Pogo gets extra affectionate.
  if (streak > 0 && streak % 5 == 0) {
    addHappiness(10);
    sPlayLove = true;
  }
}

// Small affection bonus that grows with the running streak, capped.
float streakBonus() {
  int n = sPetStreak > 10 ? 10 : sPetStreak;
  return n * 0.5f;
}

// --- GIF draw callback (line-based, palette -> RGB565 into TFT, centered) ---

#define BUFFER_SIZE 256
uint16_t usTemp[BUFFER_SIZE];

void GIFDraw(GIFDRAW* pDraw) {
  uint8_t* s;
  uint16_t* d;
  int x, y, iWidth, iCount;

  iWidth = pDraw->iWidth;
  if (pDraw->iX > tft.width()) return;
  if (iWidth + pDraw->iX > tft.width()) iWidth = tft.width() - pDraw->iX;
  uint16_t* usPalette = pDraw->pPalette;

  x = pDraw->iX + gGifX;
  y = pDraw->iY + pDraw->y + gGifY;
  if (y >= tft.height() || x >= tft.width() || iWidth < 1) return;

  s = pDraw->pPixels;
  if (pDraw->ucDisposalMethod == 2) {
    for (int i = 0; i < iWidth; i++) {
      if (s[i] == pDraw->ucTransparent) s[i] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }

  if (pDraw->ucHasTransparency) {
    uint8_t* pEnd;
    uint8_t c, ucTransparent = pDraw->ucTransparent;
    pEnd = s + iWidth;
    x = 0;
    iCount = 0;
    while (x < iWidth) {
      c = ucTransparent - 1;
      d = &usTemp[0];
      while (c != ucTransparent && s < pEnd && iCount < BUFFER_SIZE) {
        c = *s++;
        if (c == ucTransparent) {
          s--;
        } else {
          *d++ = usPalette[c];
          iCount++;
        }
      }
      if (iCount) {
        tft.setAddrWindow(pDraw->iX + gGifX + x, y, iCount, 1);
        tft.pushPixels(usTemp, iCount);
        x += iCount;
        iCount = 0;
      }
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent) x++;
        else s--;
      }
    }
  } else {
    s = pDraw->pPixels;
    if (iWidth <= BUFFER_SIZE) {
      for (iCount = 0; iCount < iWidth; iCount++) usTemp[iCount] = usPalette[*s++];
      tft.setAddrWindow(pDraw->iX + gGifX, y, iWidth, 1);
      tft.pushPixels(usTemp, iWidth);
    } else {
      iCount = 0;
      for (int i = 0; i < BUFFER_SIZE; i++) usTemp[i] = usPalette[*s++];
      tft.setAddrWindow(pDraw->iX + gGifX, y, BUFFER_SIZE, 1);
      tft.pushPixels(usTemp, BUFFER_SIZE);
      iCount = BUFFER_SIZE;
      while (iCount < iWidth) {
        int n = iWidth - iCount;
        if (n > BUFFER_SIZE) n = BUFFER_SIZE;
        for (int i = 0; i < n; i++) usTemp[i] = usPalette[*s++];
        tft.pushPixels(usTemp, n);
        iCount += n;
      }
    }
  }
}

// --- Weather sensing ---

void readSensors() {
  unsigned long now = millis();
  if (lastDhtReadAt == 0 || now - lastDhtReadAt >= dhtIntervalMs) {
    lastDhtReadAt = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) lastKnownTemp = t;
    if (!isnan(h)) lastKnownHum = h;
  }
  if (lastLdrReadAt == 0 || now - lastLdrReadAt >= ldrIntervalMs) {
    lastLdrReadAt = now;
    int raw = analogRead(ldrPin);
    if (ldrSmoothed < 0) ldrSmoothed = raw;
    ldrSmoothed += (raw - ldrSmoothed) * 0.15f;
  }
}

EnvState computeEnv() {
  if (ldrSmoothed >= 0 && ldrSmoothed < DARK_LDR) return ENV_DARK;
  if (!isnan(lastKnownTemp) && lastKnownTemp >= HOT_T) return ENV_HOT;
  if (!isnan(lastKnownTemp) && lastKnownTemp <= COLD_T) return ENV_COLD;
  if (!isnan(lastKnownHum) && lastKnownHum >= HUMID_RH) return ENV_HUMID;
  return ENV_OK;
}

// --- Room HUD — DHT readout, small white text in the black band under the pet ---

const int HUD_TEXT_Y = 220;   // GIF canvases end at y <= 203, so this band is animation-free
const int HUD_TEMP_X = 60;    // temperature, bottom-left
const int HUD_HUM_X  = 220;   // humidity, bottom-right

// Last-painted values: repaint only when a reading actually changes.
float sPaintedTemp = NAN;
float sPaintedHum = NAN;

void drawRoomHud(bool force = false) {
  float t = lastKnownTemp;
  float h = lastKnownHum;
  if (!force && t == sPaintedTemp && h == sPaintedHum) return;
  sPaintedTemp = t;
  sPaintedHum = h;

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  char buf[12];
  if (!isnan(t)) snprintf(buf, sizeof(buf), "%.1fC", t);
  else strcpy(buf, "--C");
  tft.drawString(buf, HUD_TEMP_X, HUD_TEXT_Y);

  if (!isnan(h)) snprintf(buf, sizeof(buf), "%.0f%%", h);
  else strcpy(buf, "--%");
  tft.drawString(buf, HUD_HUM_X, HUD_TEXT_Y);
}

// --- Top clock — time + date centered above the pet ---

const char* const kMonths[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                 "jul", "aug", "sep", "oct", "nov", "dec"};

int sPaintedMinute = -1;   // repaint clock only at minute boundaries

void drawClock(bool force = false) {
  struct tm ti;
  if (!sClockValid || !getLocalTime(&ti, 0)) return;
  int minute = ti.tm_hour * 60 + ti.tm_min;
  if (!force && minute == sPaintedMinute) return;
  sPaintedMinute = minute;

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  char buf[24];
  snprintf(buf, sizeof(buf), "%02d : %02d  %d %s", ti.tm_hour, ti.tm_min, ti.tm_mday, kMonths[ti.tm_mon]);
  tft.drawString(buf, tft.width() / 2, 18);
}

// --- Animation selection ---

void handleTouch(int reading, unsigned long now);
void tickHappiness();

const MochiGif* pickByMood(uint8_t mood) {
  int n = 0;
  for (int i = 0; i < kGifCount; i++) if (kGifs[i].mood == mood) n++;
  if (n == 0) return nullptr;
  int idx = random(n);
  int seen = 0;
  for (int i = 0; i < kGifCount; i++) {
    if (kGifs[i].mood == mood) {
      if (seen == idx) return &kGifs[i];
      seen++;
    }
  }
  return nullptr;
}

int sAnimIndex = 0;
bool sWasSleepZone = false;

// Sequential loop through every animation of the given mood.
const MochiGif* nextSequential(uint8_t mood) {
  for (int i = 0; i < kGifCount; i++) {
    sAnimIndex = (sAnimIndex + 1) % kGifCount;
    if (kGifs[sAnimIndex].mood == mood) return &kGifs[sAnimIndex];
  }
  return nullptr;
}

// Sequential full rotation through ALL animations except sleep zone ones
// (sleepy, drowsy, yawn play only during 22:00–7:00).
const MochiGif* nextSequentialAll() {
  for (int i = 0; i < kGifCount; i++) {
    sAnimIndex = (sAnimIndex + 1) % kGifCount;
    if (kGifs[sAnimIndex].mood != MOOD_SLEEPY) return &kGifs[sAnimIndex];
  }
  return nullptr;
}

void playGif(const MochiGif* g) {
  if (!g) return;
  if (!gif.open((uint8_t*)g->data, g->size, GIFDraw)) return;

  gGifX = (tft.width() - gif.getCanvasWidth()) / 2;
  gGifY = (tft.height() - gif.getCanvasHeight()) / 2;
  if (gGifX < 0) gGifX = 0;
  if (gGifY < 0) gGifY = 0;

  Serial.printf("Playing %s (%dx%d)\n", g->name, gif.getCanvasWidth(), gif.getCanvasHeight());

  tft.fillScreen(TFT_BLACK);
  drawRoomHud(true);
  drawClock(true);
  tft.startWrite();
  while (gif.playFrame(true, NULL)) {
    handleTouch(digitalRead(touchPin), millis());
    tickHappiness();
    readSensors();
    drawRoomHud();
    drawClock();
    yield();
  }
  gif.close();
  tft.endWrite();
  tft.fillScreen(TFT_BLACK);
}

bool isSleepZone() {
  if (!sClockValid) return false; // clock not synced -> stay awake, retry in loop
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return false;
  return (timeinfo.tm_hour >= 22 || timeinfo.tm_hour < 7);
}

const MochiGif* findByName(const char* name) {
  for (int i = 0; i < kGifCount; i++) {
    if (strcmp(kGifs[i].name, name) == 0) return &kGifs[i];
  }
  return nullptr;
}

// --- Random idle personality — Pogo does its own thing while awake ---
const char* kQuirkNames[] = {"giggle", "sneeze", "surprised", "laughing", "happy", "wink"};
const int kQuirkCount = sizeof(kQuirkNames) / sizeof(kQuirkNames[0]);
unsigned long lastQuirkAt = 0;
const unsigned long quirkCooldownMs = 45000;

const MochiGif* pickQuirk() {
  for (int i = 0; i < 5; i++) {
    const MochiGif* g = findByName(kQuirkNames[random(kQuirkCount)]);
    if (g) return g;
  }
  return nullptr;
}

// --- Touch handling ---

const unsigned long debounceMs = 250;
const unsigned long longPressMs = 700;
const unsigned long doubleTapMs = 400;
unsigned long pressStartAt = 0;
bool longPressFired = false;
int lastTouchRead = LOW;
unsigned long lastTapReleaseAt = 0;
bool sPlayGame = false;

void handleTouch(int reading, unsigned long now) {
  bool wasPressed = lastTouchRead == HIGH;
  bool isPressed = reading == HIGH;
  lastTouchRead = reading;

  if (isPressed && !wasPressed) {
    pressStartAt = now;
    longPressFired = false;
    return;
  }

  if (isPressed && !longPressFired && (now - pressStartAt) >= longPressMs) {
    longPressFired = true;
    addHappiness(HAPPY_LONGPRESS_BOOST + streakBonus());
    recordPet();
    sPlayLove = true;
    return;
  }

  if (!isPressed && wasPressed && !longPressFired && (now - pressStartAt) >= debounceMs) {
    bool isDouble = lastTapReleaseAt != 0 && (now - lastTapReleaseAt) <= doubleTapMs;
    lastTapReleaseAt = now;
    if (isDouble) {
      sPlayGame = true;
      sPlayReaction = false;
      sPlayLove = false;
      return; // no happiness for the launch tap itself
    }
    addHappiness(HAPPY_TAP_BOOST + streakBonus());
    recordPet();
    sPlayReaction = true;
  }
}

// --- Network: get WiFi + the clock so sleepy zones work (bounded, never fatal) ---
// WiFi stays ON so the web dashboard stays reachable.

void ensureWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 6000) {
      delay(50);
      yield();
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi connect failed (will retry in loop)");
      return;
    }
    WiFi.setSleep(false); // keep the radio awake so the web server stays snappy
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  }

  if (!sClockValid) {
    configTime(gmtOffsetSec, daylightOffsetSec, ntpServer);
    struct tm timeinfo;
    unsigned long t0 = millis();
    while (!getLocalTime(&timeinfo, 0) && millis() - t0 < 2500) delay(100);
    if (getLocalTime(&timeinfo, 0)) {
      sClockValid = true;
      Serial.printf("Clock synced (hour %d)\n", timeinfo.tm_hour);
    }
  }
}

// --- Web dashboard ---
// Dashboard page is served from a FreeRTOS task pinned to core 0, so it stays
// responsive while GIF playback blocks loop() on core 1. The task only reads
// stats and pokes single-word state; it never touches the TFT, so no locking
// is needed against playGif().

const char kWebPage[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Pogo the Desk Pet</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; font-family: system-ui, sans-serif; background: #12141a; color: #e8eaf0; }
  .wrap { max-width: 480px; margin: 0 auto; padding: 16px; }
  h1 { font-size: 22px; margin: 4px 0 16px; }
  .card { background: #1b1e27; border: 1px solid #2a2e3a; border-radius: 12px; padding: 14px 16px; margin-bottom: 12px; }
  .row { display: flex; justify-content: space-between; margin: 6px 0; }
  .lbl { color: #9aa0b0; }
  .bar { height: 14px; background: #232733; border-radius: 8px; overflow: hidden; margin-top: 8px; }
  .bar > div { height: 100%; border-radius: 8px; width: 0%; transition: width .4s ease; background: linear-gradient(90deg, #f4a261, #e76f51); }
  button { width: 100%; padding: 14px; font-size: 16px; font-weight: 600; border: 0; border-radius: 12px; background: #e76f51; color: #fff; cursor: pointer; }
  button:active { transform: scale(.97); }
  .meta { color: #7b8295; font-size: 12px; text-align: center; margin-top: 12px; }
</style>
</head>
<body>
<div class="wrap">
  <h1>Pogo the Desk Pet</h1>
  <div class="card">
    <div class="row"><span class="lbl">Happiness</span><span><span id="happy">--</span> / 100</span></div>
    <div class="bar"><div id="happyBar"></div></div>
  </div>
  <div class="card">
    <div class="row"><span class="lbl">Temperature</span><span id="temp">--</span></div>
    <div class="row"><span class="lbl">Humidity</span><span id="hum">--</span></div>
    <div class="row"><span class="lbl">Light</span><span id="ldr">--</span></div>
  </div>
  <div class="card">
    <div class="row"><span class="lbl">Petting streak</span><span id="streak">--</span></div>
    <div class="row"><span class="lbl">Local time</span><span id="time">--</span></div>
    <div class="row"><span class="lbl">WiFi RSSI</span><span id="rssi">--</span></div>
  </div>
  <button onclick="pet()">Pet Pogo remotely</button>
  <p class="meta" id="status">Connecting…</p>
</div>
<script>
const $ = id => document.getElementById(id);
async function refresh() {
  try {
    const r = await fetch('/stats.json');
    const d = await r.json();
    $('happy').textContent = d.happy.toFixed(1);
    $('happyBar').style.width = Math.max(0, Math.min(100, d.happy)) + '%';
    $('temp').textContent = Number.isFinite(d.temp) ? d.temp.toFixed(1) + ' °C' : '--';
    $('hum').textContent = Number.isFinite(d.hum) ? d.hum.toFixed(1) + ' %' : '--';
    $('ldr').textContent = d.ldr.toFixed(0);
    $('streak').textContent = d.streak;
    $('time').textContent = d.clock ? new Date(d.ts * 1000).toLocaleString() : 'n/a';
    $('rssi').textContent = d.rssi + ' dBm';
    $('status').textContent = 'Live · refreshed ' + new Date().toLocaleTimeString();
  } catch (e) {
    $('status').textContent = 'Offline — waiting for Pogo to come back…';
  }
}
async function pet() {
  $('status').textContent = 'Petted!';
  try { await fetch('/pet', { method: 'POST' }); } catch (e) {}
}
setInterval(refresh, 2000);
refresh();
</script>
</body>
</html>
)rawliteral";

WebServer server(80);

void handleRoot() {
  server.send_P(200, "text/html", kWebPage);
}

void handleStatsJSON() {
  struct tm timeinfo;
  bool haveTime = getLocalTime(&timeinfo, 0);

  JsonDocument doc;
  doc["happy"] = round(happy * 10.0f) / 10.0f;
  if (!isnan(lastKnownTemp)) doc["temp"] = lastKnownTemp;
  if (!isnan(lastKnownHum)) doc["hum"] = lastKnownHum;
  doc["ldr"] = ldrSmoothed;
  doc["streak"] = sPetStreak;
  doc["clock"] = sClockValid;
  doc["ts"] = haveTime ? (uint32_t)time(nullptr) : 0;
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleRemotePet() {
  addHappiness(HAPPY_TAP_BOOST);
  recordPet();
  sPlayReaction = true;
  server.send(200, "text/plain", "Pogo appreciates the remote pet.");
}

void webTask(void* arg) {
  for (;;) {
    server.handleClient();
    delay(2);
  }
}

void startWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/stats.json", HTTP_GET, handleStatsJSON);
  server.on("/pet", HTTP_POST, handleRemotePet);
  server.begin();
  xTaskCreatePinnedToCore(webTask, "web", 4096, NULL, 1, NULL, 0);
}

void showIdentityCard();

void setup() {
  Serial.begin(115200);

  pinMode(touchPin, INPUT_PULLDOWN);
  dht.begin();
  // Prime the DHT so the first HUD paint shows numbers (first read is often NaN).
  for (int i = 0; i < 3 && isnan(lastKnownTemp); i++) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) lastKnownTemp = t;
    if (!isnan(h)) lastKnownHum = h;
    if (isnan(t) || isnan(h)) delay(300);
  }
  lastDhtReadAt = millis();
  analogSetPinAttenuation(ldrPin, ADC_0db);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  randomSeed(ESP.getEfuseMac());
  gif.begin(BIG_ENDIAN_PIXELS);

  loadHappiness();
  loadStreak();
  ensureWiFi();
  startWebServer();
  showIdentityCard();

  Serial.printf("Happiness: %.1f\n", happy);
}

void showIdentityCard() {
  struct tm timeinfo;
  bool haveNow = getLocalTime(&timeinfo, 0);
  bool isBirthday = haveNow &&
                    timeinfo.tm_mon + 1 == PET_BIRTH_MONTH &&
                    timeinfo.tm_mday == PET_BIRTH_DAY;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);

  String hi = String("Hi! I'm ") + PET_NAME;
  tft.drawString(hi, tft.width() / 2, tft.height() / 2 - 14);

  if (isBirthday) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("HAPPY BIRTHDAY!", tft.width() / 2, tft.height() / 2 + 12);
    Serial.printf("It's %s's birthday today!\n", PET_NAME);
  } else {
    char born[40];
    snprintf(born, sizeof(born), "Born %d/%d", PET_BIRTH_MONTH, PET_BIRTH_DAY);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(born, tft.width() / 2, tft.height() / 2 + 12);
  }

  if (sPetStreak >= 2) {
    char st[32];
    snprintf(st, sizeof(st), "Streak: %d days", sPetStreak);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(st, tft.width() / 2, tft.height() / 2 + 28);
  }

  delay(1500);
  tft.setTextDatum(TL_DATUM);
}

void loop() {
  unsigned long now = millis();
  int reading = digitalRead(touchPin);
  handleTouch(reading, now);
  tickHappiness();
  readSensors();

  // Keep WiFi + the Kathmandu clock healthy (a failed boot sync must not
  // trap Pogo in sleepy mode all day, and the dashboard needs a link).
  if ((WiFi.status() != WL_CONNECTED || !sClockValid) &&
      now - sLastClockRetry >= kNetRetryMs) {
    sLastClockRetry = now;
    ensureWiFi();
  }

  const MochiGif* g = nullptr;

  if (sPlayGame) {
    sPlayGame = false;
    playGame(touchPin);
  } else if (sPlayLove) {
    sPlayLove = false;
    g = nextSequential(MOOD_LOVE);
  } else if (sPlayReaction) {
    sPlayReaction = false;
    g = nextSequential(MOOD_REACTION);
  } else {
    // Sleep zone (22:00–7:00): yawn to fall asleep, then repeat sleepy only.
    // Day: yawn to wake up, then full rotation through all animations.
    bool sleepZone = isSleepZone();
    if (sleepZone && !sWasSleepZone) {
      g = findByName("yawn");
      if (!g) g = findByName("sleepy");
    } else if (!sleepZone && sWasSleepZone) {
      g = findByName("yawn");
      if (!g) g = nextSequentialAll();
    } else if (sleepZone) {
      g = findByName("sleepy");
      if (!g) g = nextSequential(MOOD_SLEEPY);
    } else {
      // Weather-mood pet: Pogo "notices" an uncomfortable room for a moment.
      EnvState env = computeEnv();
      if (env != ENV_OK && env != ENV_DARK &&
          now - lastSensorReactAt >= sensorReactCooldownMs) {
        lastSensorReactAt = now;
        switch (env) {
          case ENV_HOT:
            g = findByName("splash");
            if (!g) g = findByName("rain");
            break;
          case ENV_COLD:
            g = findByName("down");
            if (!g) g = findByName("sick");
            break;
          case ENV_HUMID:
            g = findByName("rain");
            if (!g) g = findByName("splash");
            break;
          default: break;
        }
      }
      if (!g) {
        // Random idle personality: comfortable room, cooldown passed, roll dice.
        if (env == ENV_OK && now - lastQuirkAt >= quirkCooldownMs) {
          if (random(100) < 12) {
            lastQuirkAt = now;
            g = pickQuirk();
          }
        }
      }
      if (!g) {
        if (env == ENV_DARK) {
          // Lights off during the day: Pogo gets drowsy on and off.
          g = (random(2) == 0) ? nextSequential(MOOD_SLEEPY) : nextSequentialAll();
        } else {
          g = nextSequentialAll();
        }
      }
    }
    sWasSleepZone = sleepZone;
  }

  if (!g) g = nextSequential(MOOD_DEFAULT);

  playGif(g);
  delay(20);
}