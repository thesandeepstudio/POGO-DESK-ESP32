#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <AnimatedGIF.h>
#include <Preferences.h>

#include "animations.h"

// --- Board ---
const int touchPin = 21;

// --- Network (for the sleepy zone clock only) ---
const char* ssid = "jasodevi_2";
const char* password = "CLEB4F06F6";
const long gmtOffsetSec = 5 * 3600 + 45 * 60;
const int daylightOffsetSec = 0;
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
  tft.startWrite();
  while (gif.playFrame(true, NULL)) {
    handleTouch(digitalRead(touchPin), millis());
    tickHappiness();
    yield();
  }
  gif.close();
  tft.endWrite();
  tft.fillScreen(TFT_BLACK);
}

bool isSleepZone() {
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

// --- Touch handling ---

const unsigned long debounceMs = 250;
const unsigned long longPressMs = 700;
unsigned long pressStartAt = 0;
bool longPressFired = false;
int lastTouchRead = LOW;

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
    addHappiness(HAPPY_LONGPRESS_BOOST);
    sPlayLove = true;
    return;
  }

  if (!isPressed && wasPressed && !longPressFired && (now - pressStartAt) >= debounceMs) {
    addHappiness(HAPPY_TAP_BOOST);
    sPlayReaction = true;
  }
}

// --- Boot: get the clock so sleepy zones work (bounded, never fatal) ---

void syncClock() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 6000) delay(50);

  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffsetSec, daylightOffsetSec, ntpServer);
    struct tm timeinfo;
    t0 = millis();
    while (!getLocalTime(&timeinfo, 0) && millis() - t0 < 2500) delay(100);
    if (getLocalTime(&timeinfo, 0)) {
      Serial.printf("Clock synced (hour %d)\n", timeinfo.tm_hour);
    }
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void syncClock();
void showIdentityCard();

void setup() {
  Serial.begin(115200);

  pinMode(touchPin, INPUT_PULLDOWN);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  randomSeed(ESP.getEfuseMac());
  gif.begin(BIG_ENDIAN_PIXELS);

  loadHappiness();
  syncClock();
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

  delay(1500);
  tft.setTextDatum(TL_DATUM);
}

void loop() {
  unsigned long now = millis();
  int reading = digitalRead(touchPin);
  handleTouch(reading, now);
  tickHappiness();

  const MochiGif* g = nullptr;

  if (sPlayLove) {
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
      g = nextSequentialAll();
    }
    sWasSleepZone = sleepZone;
  }

  if (!g) g = nextSequential(MOOD_DEFAULT);

  playGif(g);
  delay(20);
}