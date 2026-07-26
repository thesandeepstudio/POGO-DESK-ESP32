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

const int touchPin = 21;
const unsigned long debounceMs = 250;

#define DHTPIN 27
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);
const unsigned long dhtIntervalMs = 2000;
unsigned long lastDhtReadAt = 0;
String lastDhtStr = "";

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite clockSprite = TFT_eSprite(&tft);

uint16_t COL_BG;
uint16_t COL_TEXT;
uint16_t COL_ACCENT;
uint16_t COL_DIM;

String lastTimeStr = "";
bool showTouchScreen = false;
int lastTouchRead = LOW;
unsigned long lastToggleAt = 0;

const unsigned long clockDurationMs = 5000;
const unsigned long dhtDurationMs = 10000;
unsigned long screenStartAt = 0;

const char* dayNames[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char* monthNames[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                             "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

const int SPRITE_W = 220;
const int SPRITE_H = 90;

void drawFrame() {
  int w = tft.width();
  int h = tft.height();
  tft.drawRoundRect(4, 4, w - 8, h - 8, 14, COL_DIM);
}

void statusScreen(const char* msg) {
  tft.fillScreen(COL_BG);
  drawFrame();
  tft.setFreeFont(&FreeSans12pt7b);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(msg, tft.width() / 2, tft.height() / 2);
}

void connectWiFi() {
  statusScreen("Connecting WiFi...");
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
  statusScreen("Syncing time...");
  configTime(gmtOffsetSec, daylightOffsetSec, ntpServer);
  struct tm timeinfo;
  Serial.print("Syncing NTP time");
  while (!getLocalTime(&timeinfo)) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(" synced");
}

void drawClockFrame() {
  tft.fillScreen(COL_BG);
  drawFrame();
  lastTimeStr = "";  // force immediate redraw
}

void drawDhtScreen() {
  tft.fillScreen(COL_BG);
  drawFrame();
  lastDhtStr = "";      // force immediate redraw
  lastDhtReadAt = 0;    // force immediate read
}

void updateDhtScreen() {
  unsigned long now = millis();
  if (now - lastDhtReadAt < dhtIntervalMs && lastDhtReadAt != 0) return;
  lastDhtReadAt = now;

  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  char tempBuf[16];
  char humBuf[16];
  if (isnan(temp) || isnan(hum)) {
    strcpy(tempBuf, "--.-C");
    strcpy(humBuf, "--.-% RH");
  } else {
    snprintf(tempBuf, sizeof(tempBuf), "%.1fC", temp);
    snprintf(humBuf, sizeof(humBuf), "%.1f%% RH", hum);
  }
  String dhtStr = String(tempBuf) + " " + String(humBuf);

  if (dhtStr != lastDhtStr) {
    clockSprite.fillSprite(COL_BG);

    clockSprite.setFreeFont(&FreeSansBold24pt7b);
    clockSprite.setTextColor(COL_TEXT, COL_BG);
    clockSprite.drawString(tempBuf, SPRITE_W / 2, 30);

    clockSprite.setFreeFont(&FreeSans9pt7b);
    clockSprite.setTextColor(COL_ACCENT, COL_BG);
    clockSprite.drawString(humBuf, SPRITE_W / 2, 70);

    clockSprite.pushSprite(tft.width() / 2 - SPRITE_W / 2, tft.height() / 2 - SPRITE_H / 2);

    lastDhtStr = dhtStr;
  }
}

void updateClock() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  char timeBuf[9];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
            timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  String timeStr(timeBuf);

  if (timeStr != lastTimeStr) {
    char dateBuf[24];
    snprintf(dateBuf, sizeof(dateBuf), "%s, %d %s %d",
              dayNames[timeinfo.tm_wday], timeinfo.tm_mday,
              monthNames[timeinfo.tm_mon], timeinfo.tm_year + 1900);

    clockSprite.fillSprite(COL_BG);

    clockSprite.setFreeFont(&FreeSansBold24pt7b);
    clockSprite.setTextColor(COL_TEXT, COL_BG);
    clockSprite.drawString(timeStr, SPRITE_W / 2, 30);

    clockSprite.setFreeFont(&FreeSans9pt7b);
    clockSprite.setTextColor(COL_ACCENT, COL_BG);
    clockSprite.drawString(dateBuf, SPRITE_W / 2, 70);

    clockSprite.pushSprite(tft.width() / 2 - SPRITE_W / 2, tft.height() / 2 - SPRITE_H / 2);

    lastTimeStr = timeStr;
  }
}

void setup() {
  Serial.begin(115200);

  COL_BG = TFT_WHITE;
  COL_TEXT = TFT_BLACK;
  COL_ACCENT = tft.color565(70, 70, 70);
  COL_DIM = tft.color565(210, 210, 210);

  pinMode(touchPin, INPUT_PULLDOWN);
  dht.begin();

  tft.init();
  tft.setRotation(1);

  connectWiFi();
  syncTime();

  clockSprite.setColorDepth(16);
  clockSprite.createSprite(SPRITE_W, SPRITE_H);
  clockSprite.setTextDatum(MC_DATUM);

  drawClockFrame();
  screenStartAt = millis();
}

void switchScreen(bool toDht) {
  showTouchScreen = toDht;
  screenStartAt = millis();
  if (toDht) {
    drawDhtScreen();
  } else {
    drawClockFrame();
  }
}

void loop() {
  int reading = digitalRead(touchPin);
  unsigned long now = millis();

  if (reading == HIGH && lastTouchRead == LOW && (now - lastToggleAt) > debounceMs) {
    lastToggleAt = now;
    switchScreen(!showTouchScreen);
  }
  lastTouchRead = reading;

  unsigned long elapsed = now - screenStartAt;
  if (!showTouchScreen && elapsed >= clockDurationMs) {
    switchScreen(true);
  } else if (showTouchScreen && elapsed >= dhtDurationMs) {
    switchScreen(false);
  }

  if (showTouchScreen) {
    updateDhtScreen();
  } else {
    updateClock();
  }

  delay(50);
}
