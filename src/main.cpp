#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <DHT.h>

const char* ssid = "jasodevi_2";
const char* password = "CLEB4F06F6";

// Nepal Standard Time = UTC+5:45
const long gmtOffsetSec = 5 * 3600 + 45 * 60;
const int daylightOffsetSec = 0;
const char* ntpServer = "pool.ntp.org";

#define DHTPIN 27
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);
const unsigned long dhtIntervalMs = 2000;
unsigned long lastDhtReadAt = 0;
float lastKnownTemp = NAN;
float lastKnownHum = NAN;

const int ldrPin = 34;
float ldrSmoothed = -1;
unsigned long lastLdrReadAt = 0;
const unsigned long ldrReadIntervalMs = 400;

const int DARK_ROOM_THRESHOLD = 15;
bool darkThemeActive = false;
unsigned long lastThemeCheckAt = 0;
const unsigned long themeCheckIntervalMs = 5000;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite timeCardSprite = TFT_eSprite(&tft);
TFT_eSprite dateCardSprite = TFT_eSprite(&tft);
TFT_eSprite tempCardSprite = TFT_eSprite(&tft);

uint16_t COL_BG;
uint16_t COL_CARD_ORANGE;
uint16_t COL_CARD_YELLOW;

const int BORDER_THICKNESS = 8;

const char* dayNamesShort[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// Bento layout -- computed at boot from the actual screen size (see layoutCards())
// so it fits whichever rotation is active instead of assuming portrait 240x280.
const int MARGIN = 14;
const int GAP = 14;
int TIME_CARD_W, TIME_CARD_H;
int BOTTOM_CARD_W, BOTTOM_CARD_H;
int BOTTOM_ROW_Y;

void layoutCards() {
  int w = tft.width();
  int h = tft.height();

  TIME_CARD_W = w - 2 * MARGIN;
  BOTTOM_CARD_W = (TIME_CARD_W - GAP) / 2;

  int usableH = h - 2 * MARGIN - GAP;
  TIME_CARD_H = (int)(usableH * 0.55f);
  BOTTOM_CARD_H = usableH - TIME_CARD_H;

  BOTTOM_ROW_Y = MARGIN + TIME_CARD_H + GAP;
}

String lastTimeStr = "";
String lastDateStr = "";
float lastTempDisplayed = NAN;

void applyLightTheme() {
  COL_BG = TFT_WHITE;
}

void applyDarkTheme() {
  COL_BG = tft.color565(20, 20, 24);
}

void connectWiFi() {
  tft.fillScreen(COL_BG);
  tft.setTextColor(TFT_BLACK, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSans12pt7b);
  tft.drawString("Connecting WiFi...", tft.width() / 2, tft.height() / 2);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(" connected");
}

void syncTime() {
  tft.fillScreen(COL_BG);
  tft.drawString("Syncing time...", tft.width() / 2, tft.height() / 2);

  configTime(gmtOffsetSec, daylightOffsetSec, ntpServer);
  struct tm timeinfo;
  Serial.print("Syncing NTP time");
  while (!getLocalTime(&timeinfo)) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(" synced");
}

void drawHome() {
  int w = tft.width();
  int h = tft.height();
  tft.fillRoundRect(0, 0, w, h, 24, TFT_BLACK);
  tft.fillRoundRect(BORDER_THICKNESS, BORDER_THICKNESS, w - 2 * BORDER_THICKNESS,
                     h - 2 * BORDER_THICKNESS, 18, COL_BG);
  lastTimeStr = "";
  lastDateStr = "";
  lastTempDisplayed = NAN;
}

void updateTimeCard() {
  struct tm ti;
  if (!getLocalTime(&ti, 10)) return;

  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", ti.tm_hour, ti.tm_min);
  String timeStr(buf);
  if (timeStr == lastTimeStr) return;
  lastTimeStr = timeStr;

  timeCardSprite.fillSprite(COL_BG);
  timeCardSprite.fillRoundRect(0, 0, TIME_CARD_W, TIME_CARD_H, 24, COL_CARD_ORANGE);
  timeCardSprite.setFreeFont(&FreeSansBold24pt7b);
  timeCardSprite.setTextColor(TFT_BLACK, COL_CARD_ORANGE);
  timeCardSprite.setTextDatum(MC_DATUM);
  timeCardSprite.drawString(timeStr, TIME_CARD_W / 2, TIME_CARD_H / 2);
  timeCardSprite.pushSprite(MARGIN, MARGIN);
}

void updateDateCard() {
  struct tm ti;
  if (!getLocalTime(&ti, 10)) return;

  char buf[8];
  snprintf(buf, sizeof(buf), "%s|%d", dayNamesShort[ti.tm_wday], ti.tm_mday);
  String dateStr(buf);
  if (dateStr == lastDateStr) return;
  lastDateStr = dateStr;

  dateCardSprite.fillSprite(COL_BG);
  dateCardSprite.fillRoundRect(0, 0, BOTTOM_CARD_W, BOTTOM_CARD_H, 20, TFT_BLACK);
  dateCardSprite.setTextDatum(TL_DATUM);

  const int padX = 16;
  dateCardSprite.setFreeFont(&FreeSansBold12pt7b);
  dateCardSprite.setTextColor(TFT_WHITE, TFT_BLACK);
  dateCardSprite.drawString(dayNamesShort[ti.tm_wday], padX, (int)(BOTTOM_CARD_H * 0.14f));

  char dayNum[4];
  snprintf(dayNum, sizeof(dayNum), "%d", ti.tm_mday);
  dateCardSprite.setFreeFont(&FreeSansBold24pt7b);
  dateCardSprite.drawString(dayNum, padX, (int)(BOTTOM_CARD_H * 0.42f));

  dateCardSprite.pushSprite(MARGIN, BOTTOM_ROW_Y);
}

void updateTempCard() {
  unsigned long now = millis();
  if (now - lastDhtReadAt >= dhtIntervalMs || lastDhtReadAt == 0) {
    lastDhtReadAt = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) lastKnownTemp = t;
    if (!isnan(h)) lastKnownHum = h;
  }

  if (isnan(lastKnownTemp) || lastKnownTemp == lastTempDisplayed) return;
  lastTempDisplayed = lastKnownTemp;

  tempCardSprite.fillSprite(COL_BG);
  tempCardSprite.fillRoundRect(0, 0, BOTTOM_CARD_W, BOTTOM_CARD_H, 20, COL_CARD_YELLOW);
  tempCardSprite.setTextDatum(TL_DATUM);
  tempCardSprite.setTextColor(TFT_BLACK, COL_CARD_YELLOW);

  const int padX = 16;
  tempCardSprite.setFreeFont(&FreeSansBold9pt7b);
  tempCardSprite.drawString("Temp", padX, (int)(BOTTOM_CARD_H * 0.10f));

  char buf[8];
  snprintf(buf, sizeof(buf), "%.1f", lastKnownTemp);
  tempCardSprite.setFreeFont(&FreeSansBold18pt7b);
  tempCardSprite.drawString(buf, padX, (int)(BOTTOM_CARD_H * 0.36f));

  if (!isnan(lastKnownHum)) {
    char humBuf[12];
    snprintf(humBuf, sizeof(humBuf), "%.0f%% RH", lastKnownHum);
    tempCardSprite.setFreeFont(&FreeSansBold9pt7b);
    tempCardSprite.drawString(humBuf, padX, (int)(BOTTOM_CARD_H * 0.82f));
  }

  tempCardSprite.pushSprite(MARGIN + BOTTOM_CARD_W + GAP, BOTTOM_ROW_Y);
}

void updateLdr() {
  unsigned long now = millis();
  if (now - lastLdrReadAt < ldrReadIntervalMs && lastLdrReadAt != 0) return;
  lastLdrReadAt = now;

  int raw = analogRead(ldrPin);
  if (ldrSmoothed < 0) ldrSmoothed = raw;
  ldrSmoothed += (raw - ldrSmoothed) * 0.15f;
}

void updateTheme() {
  unsigned long now = millis();
  if (now - lastThemeCheckAt < themeCheckIntervalMs && lastThemeCheckAt != 0) return;
  lastThemeCheckAt = now;

  bool wantDark = ldrSmoothed >= 0 && ldrSmoothed < DARK_ROOM_THRESHOLD;
  if (wantDark == darkThemeActive) return;
  darkThemeActive = wantDark;

  if (darkThemeActive) {
    applyDarkTheme();
  } else {
    applyLightTheme();
  }
  drawHome();
}

void setup() {
  Serial.begin(115200);

  applyLightTheme();
  COL_CARD_ORANGE = tft.color565(0xFF, 0x6A, 0x00);
  COL_CARD_YELLOW = tft.color565(0xFF, 0xF6, 0x00);

  dht.begin();
  analogSetPinAttenuation(ldrPin, ADC_0db);

  tft.init();
  tft.setRotation(1);
  layoutCards();

  connectWiFi();
  syncTime();

  timeCardSprite.setColorDepth(16);
  timeCardSprite.createSprite(TIME_CARD_W, TIME_CARD_H);

  dateCardSprite.setColorDepth(16);
  dateCardSprite.createSprite(BOTTOM_CARD_W, BOTTOM_CARD_H);

  tempCardSprite.setColorDepth(16);
  tempCardSprite.createSprite(BOTTOM_CARD_W, BOTTOM_CARD_H);

  drawHome();
}

void loop() {
  updateLdr();
  updateTheme();
  updateTimeCard();
  updateDateCard();
  updateTempCard();

  delay(50);
}
