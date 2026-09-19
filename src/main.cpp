#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <DHT.h>
#include "BenzinBoldClock.h"
#include "BenzinBoldSmall.h"

const char* ssid = "jasodevi_2";
const char* password = "CLEB4F06F6";

// Nepal Standard Time = UTC+5:45
const long gmtOffsetSec = 5 * 3600 + 45 * 60;
const int daylightOffsetSec = 0;
const char* ntpServer = "pool.ntp.org";

const int touchPin = 21;

#define DHTPIN 27
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);
const unsigned long dhtIntervalMs = 2000;
unsigned long lastDhtReadAt = 0;
String lastDhtStr = "";
float lastKnownTemp = NAN;
float lastKnownHum = NAN;

// TODO: confirm this is the correct REST path + JSON field names by checking
// https://nepseapi.surajrimal.dev/docs -- adjust fetchNepseIndex() below once confirmed.
const char* nepseUrl = "https://nepseapi.surajrimal.dev/NepseIndex";
const unsigned long nepseIntervalMs = 60000;  // 60s -- be gentle on the free hosted API
unsigned long lastNepseFetchAt = 0;
bool nepseHasData = false;
float nepseIndex = NAN;
float nepseChange = 0;
float nepseChangePercent = 0;
bool nepseMarketOpen = false;

const int NEPSE_HIST_LEN = 60;
float nepseHist[NEPSE_HIST_LEN];
int nepseHistHead = 0;
int nepseHistCount = 0;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite clockSprite = TFT_eSprite(&tft);
TFT_eSprite ldrSprite = TFT_eSprite(&tft);

const int ldrPin = 34;
const int LDR_W = 100;
const int LDR_H = 22;
float ldrSmoothed = -1;
int lastLdrDisplayed = -9999;
unsigned long lastLdrUpdateAt = 0;
const unsigned long ldrUpdateIntervalMs = 400;

const int DARK_ROOM_THRESHOLD = 15;
bool darkThemeActive = false;
unsigned long lastThemeCheckAt = 0;
const unsigned long themeCheckIntervalMs = 5000;

TFT_eSprite nepseSprite = TFT_eSprite(&tft);
TFT_eSprite petSprite = TFT_eSprite(&tft);
TFT_eSprite gameSprite = TFT_eSprite(&tft);

uint16_t COL_BG;
uint16_t COL_TEXT;
uint16_t COL_ACCENT;
uint16_t COL_DIM;
uint16_t COL_GOOD;
uint16_t COL_BAD;
uint16_t COL_PET_BODY;
uint16_t COL_PET_SHADE;
uint16_t COL_PET_HILITE;
uint16_t COL_PET_BLUSH;
uint16_t COL_PET_SWEAT;

String lastTimeStr = "";
int lastTouchRead = LOW;

enum Screen { SCR_CLOCK = 0, SCR_DHT = 1, SCR_NEPSE = 2, SCR_PET = 3, SCR_GAME = 4, SCR_COUNT = 5 };
Screen currentScreen = SCR_CLOCK;
const unsigned long screenDurations[SCR_COUNT] = {6000, 6000, 6000, 6000, 8000};
unsigned long screenStartAt = 0;

const char* dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* monthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

const int SPRITE_W = 250;
const int SPRITE_H = 90;

const int NEPSE_W = 240;
const int NEPSE_H = 190;
const int NEPSE_CHART_Y = 70;
const int NEPSE_CHART_H = NEPSE_H - NEPSE_CHART_Y - 10;
const int NEPSE_CHART_X = 10;
const int NEPSE_CHART_W = NEPSE_W - 20;

// Pixel-art pet
const int PET_GRID = 26;
const int PET_SCALE = 6;
const int PET_W = 200;
const int PET_H = 190;
const int PET_ORIGIN_X = (PET_W - PET_GRID * PET_SCALE) / 2;
const int PET_ORIGIN_Y = 12;

enum PetExpr { PET_HAPPY, PET_HOT, PET_COLD, PET_HUMID, PET_SLEEPY, PET_EXPR_NONE };
PetExpr lastDrawnPetExpr = PET_EXPR_NONE;
unsigned long lastPetCheckAt = 0;
const unsigned long petCheckIntervalMs = 2000;

// Dino runner (idle/decorative, auto-plays, no input)
const int GAME_W = 220;
const int GAME_H = 150;
const int GAME_GROUND_Y = GAME_H - 30;
const int GAME_DINO_X = 30;
const float GAME_SCROLL_SPEED = 4.0f;
const unsigned long GAME_FRAME_MS = 90;
const unsigned long GAME_JUMP_MS = 550;
const int GAME_JUMP_HEIGHT = 34;
const int GAME_JUMP_TRIGGER_DIST = 46;

unsigned long lastGameFrameAt = 0;
float gameGroundScroll = 0;
float gameObstacleX = GAME_W;
int gameObstacleW = 8;
int gameObstacleH = 18;
bool gameJumping = false;
unsigned long gameJumpStartAt = 0;

// Pixel-accurate dino silhouette, converted from a reference image of the
// classic Chrome offline-game T-Rex. Feet sit on the last row.
const int DINO_GRID_W = 28;
const int DINO_GRID_H = 24;
const int DINO_SCALE = 2;
const int DINO_DRAW_OFFSET_X = -12;

const uint8_t DINO_BITMAP[DINO_GRID_H][DINO_GRID_W] PROGMEM = {
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,0,1,1,1,1,1,1,1,1,1,1,1},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
  {1,1,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},
  {1,1,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
  {1,1,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
  {1,1,1,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0},
  {1,1,1,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0},
  {1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,1,1,0,0,0,0,0},
  {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
  {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
  {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
  {0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
  {0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,1,1,1,1,1,1,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,1,1,1,1,1,1,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0}
};

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
  lastLdrDisplayed = -9999;
  lastLdrUpdateAt = 0;
}

void drawDhtScreen() {
  tft.fillScreen(COL_BG);
  drawFrame();
  lastDhtStr = "";      // force immediate redraw
  lastDhtReadAt = 0;    // force immediate read
  lastLdrDisplayed = -9999;
  lastLdrUpdateAt = 0;
}

void drawNepseScreen() {
  tft.fillScreen(COL_BG);
  drawFrame();
  lastNepseFetchAt = 0;  // force immediate fetch/redraw
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
    lastKnownTemp = temp;
    lastKnownHum = hum;
  }
  String dhtStr = String(tempBuf) + " " + String(humBuf);

  if (dhtStr != lastDhtStr) {
    clockSprite.fillSprite(COL_BG);

    clockSprite.setFreeFont(&BenzinBoldClock);
    clockSprite.setTextColor(COL_TEXT, COL_BG);
    clockSprite.drawString(tempBuf, SPRITE_W / 2, 30);

    clockSprite.setFreeFont(&BenzinBoldSmall);
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
    char dateBuf[16];
    snprintf(dateBuf, sizeof(dateBuf), "%s %02d %s",
              dayNames[timeinfo.tm_wday], timeinfo.tm_mday,
              monthNames[timeinfo.tm_mon]);

    clockSprite.fillSprite(COL_BG);

    clockSprite.setFreeFont(&BenzinBoldClock);
    clockSprite.setTextColor(COL_TEXT, COL_BG);
    clockSprite.drawString(timeStr, SPRITE_W / 2, 30);

    clockSprite.setFreeFont(&BenzinBoldSmall);
    clockSprite.setTextColor(COL_ACCENT, COL_BG);
    clockSprite.drawString(dateBuf, SPRITE_W / 2, 70);

    clockSprite.pushSprite(tft.width() / 2 - SPRITE_W / 2, tft.height() / 2 - SPRITE_H / 2);

    lastTimeStr = timeStr;
  }
}

bool fetchNepseIndex() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();  // skip cert validation -- fine for this read-only hobby use

  HTTPClient http;
  if (!http.begin(client, nepseUrl)) return false;
  http.setTimeout(8000);

  int code = http.GET();
  bool ok = false;

  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      // TODO: verify these field names against the real API response.
      if (!doc["index"].isNull()) {
        nepseIndex = doc["index"].as<float>();
        nepseChange = doc["change"] | 0.0f;
        nepseChangePercent = doc["perChange"] | 0.0f;
        const char* status = doc["marketStatus"] | "";
        if (strlen(status) == 0) status = doc["status"] | "";
        nepseMarketOpen = (strcasecmp(status, "open") == 0) || (doc["isOpen"] | false);
        ok = true;
      } else {
        Serial.println("NEPSE JSON missing expected 'index' field, got:");
        Serial.println(payload);
      }
    } else {
      Serial.printf("NEPSE JSON parse error: %s\n", err.c_str());
    }
  } else {
    Serial.printf("NEPSE HTTP GET failed, code=%d\n", code);
  }

  http.end();
  return ok;
}

void pushNepseHistory(float value) {
  nepseHist[nepseHistHead] = value;
  nepseHistHead = (nepseHistHead + 1) % NEPSE_HIST_LEN;
  if (nepseHistCount < NEPSE_HIST_LEN) nepseHistCount++;
}

void drawNepseChart() {
  if (nepseHistCount < 2) return;

  int start = (nepseHistHead - nepseHistCount + NEPSE_HIST_LEN) % NEPSE_HIST_LEN;
  float vmin = 1e9, vmax = -1e9;
  for (int i = 0; i < nepseHistCount; i++) {
    float v = nepseHist[(start + i) % NEPSE_HIST_LEN];
    if (v < vmin) vmin = v;
    if (v > vmax) vmax = v;
  }
  float range = vmax - vmin;
  if (range < 0.5f) {
    float mid = (vmax + vmin) / 2;
    vmin = mid - 0.25f;
    vmax = mid + 0.25f;
    range = 0.5f;
  } else {
    float pad = range * 0.15f;
    vmin -= pad;
    vmax += pad;
    range = vmax - vmin;
  }

  int prevX = -1, prevY = -1;
  for (int i = 0; i < nepseHistCount; i++) {
    float v = nepseHist[(start + i) % NEPSE_HIST_LEN];
    int px = NEPSE_CHART_X + (int)(((float)i / (NEPSE_HIST_LEN - 1)) * NEPSE_CHART_W);
    int py = NEPSE_CHART_Y + NEPSE_CHART_H - (int)(((v - vmin) / range) * NEPSE_CHART_H);
    if (prevX >= 0) nepseSprite.drawLine(prevX, prevY, px, py, COL_TEXT);
    prevX = px;
    prevY = py;
  }
}

void updateNepseScreen() {
  unsigned long now = millis();
  if (now - lastNepseFetchAt < nepseIntervalMs && lastNepseFetchAt != 0) return;
  lastNepseFetchAt = now;

  bool ok = fetchNepseIndex();
  if (ok) {
    nepseHasData = true;
    pushNepseHistory(nepseIndex);
  }

  nepseSprite.fillSprite(COL_BG);

  // Index value, top-left
  nepseSprite.setFreeFont(&FreeSansBold18pt7b);
  nepseSprite.setTextDatum(TL_DATUM);
  if (nepseHasData) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", nepseIndex);
    nepseSprite.setTextColor(COL_TEXT, COL_BG);
    nepseSprite.drawString(buf, 10, 8);

    char chgBuf[24];
    snprintf(chgBuf, sizeof(chgBuf), "%+.2f (%+.2f%%)", nepseChange, nepseChangePercent);
    nepseSprite.setFreeFont(&FreeSansBold9pt7b);
    nepseSprite.setTextColor(nepseChange >= 0 ? COL_GOOD : COL_BAD, COL_BG);
    nepseSprite.drawString(chgBuf, 10, 44);
  } else {
    nepseSprite.setTextColor(COL_DIM, COL_BG);
    nepseSprite.drawString("--.--", 10, 8);
  }

  // Market status dot, top-right
  int dotX = NEPSE_W - 18;
  int dotY = 20;
  nepseSprite.fillCircle(dotX, dotY, 7, nepseMarketOpen ? COL_GOOD : COL_BAD);
  nepseSprite.setFreeFont(&FreeSans9pt7b);
  nepseSprite.setTextDatum(TR_DATUM);
  nepseSprite.setTextColor(COL_ACCENT, COL_BG);
  nepseSprite.drawString(nepseMarketOpen ? "OPEN" : "CLOSED", NEPSE_W - 30, 12);

  drawNepseChart();

  nepseSprite.pushSprite(tft.width() / 2 - NEPSE_W / 2, tft.height() / 2 - NEPSE_H / 2);
}

PetExpr determinePetExpression() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10) && (timeinfo.tm_hour >= 22 || timeinfo.tm_hour < 6)) {
    return PET_SLEEPY;
  }
  if (!isnan(lastKnownTemp)) {
    if (lastKnownTemp > 30.0f) return PET_HOT;
    if (lastKnownTemp < 15.0f) return PET_COLD;
  }
  if (!isnan(lastKnownHum) && lastKnownHum > 80.0f) return PET_HUMID;
  return PET_HAPPY;
}

bool petIsBody(int x, int y) {
  float dx = x - 13.0f, dy = y - 15.0f;
  float rx = 10.5f, ry = 9.5f;
  return ((dx * dx) / (rx * rx) + (dy * dy) / (ry * ry)) <= 1.0f;
}

bool petIsEar(int x, int y) {
  float d1x = x - 6.0f, d1y = y - 5.0f;
  float d2x = x - 20.0f, d2y = y - 5.0f;
  return (d1x * d1x + d1y * d1y <= 9.0f) || (d2x * d2x + d2y * d2y <= 9.0f);
}

bool petInShape(int x, int y) {
  if (x < 0 || y < 0 || x >= PET_GRID || y >= PET_GRID) return false;
  return petIsBody(x, y) || petIsEar(x, y);
}

void petPixel(int gx, int gy, uint16_t color) {
  int x = PET_ORIGIN_X + gx * PET_SCALE;
  int y = PET_ORIGIN_Y + gy * PET_SCALE;
  petSprite.fillRect(x, y, PET_SCALE, PET_SCALE, color);
}

void drawPetBody() {
  for (int y = 0; y < PET_GRID; y++) {
    for (int x = 0; x < PET_GRID; x++) {
      if (!petInShape(x, y)) continue;

      bool edge = !petInShape(x - 1, y) || !petInShape(x + 1, y) ||
                  !petInShape(x, y - 1) || !petInShape(x, y + 1);

      uint16_t color;
      if (edge) {
        color = COL_TEXT;
      } else if ((x + y) < 16) {
        color = COL_PET_HILITE;
      } else if ((x + y) > 30) {
        color = COL_PET_SHADE;
      } else {
        color = COL_PET_BODY;
      }
      petPixel(x, y, color);
    }
  }
}

void drawPetEyesOpen(int centerY) {
  int eyeXs[2] = {9, 17};
  for (int e = 0; e < 2; e++) {
    int ex = eyeXs[e];
    for (int dy = 0; dy < 4; dy++) {
      for (int dx = -1; dx <= 1; dx++) {
        petPixel(ex + dx, centerY + dy, TFT_WHITE);
      }
    }
    petPixel(ex, centerY + 2, TFT_BLACK);
  }
}

void drawPetEyesClosed(int centerY) {
  int eyeXs[2] = {9, 17};
  for (int e = 0; e < 2; e++) {
    int ex = eyeXs[e];
    for (int dx = -1; dx <= 1; dx++) {
      petPixel(ex + dx, centerY + 2, TFT_BLACK);
    }
  }
}

void drawPetEyesDroopy(int centerY) {
  int eyeXs[2] = {9, 17};
  for (int e = 0; e < 2; e++) {
    int ex = eyeXs[e];
    for (int dx = -1; dx <= 1; dx++) {
      petPixel(ex + dx, centerY + 1, TFT_BLACK);  // heavy lid
      petPixel(ex + dx, centerY + 3, TFT_WHITE);
    }
    petPixel(ex, centerY + 3, TFT_BLACK);
  }
}

void drawPetMouth(const int offsets[][2], int count, int anchorX, int anchorY, uint16_t color) {
  for (int i = 0; i < count; i++) {
    petPixel(anchorX + offsets[i][0], anchorY + offsets[i][1], color);
  }
}

void renderPetSprite(PetExpr expr) {
  petSprite.fillSprite(COL_BG);
  drawPetBody();

  const int mouthX = 13, mouthY = 19;

  switch (expr) {
    case PET_HAPPY: {
      drawPetEyesOpen(12);
      int smile[][2] = {{-3, 0}, {-2, 1}, {-1, 2}, {0, 2}, {1, 2}, {2, 1}, {3, 0}};
      drawPetMouth(smile, 7, mouthX, mouthY, COL_TEXT);
      petPixel(5, 16, COL_PET_BLUSH);
      petPixel(21, 16, COL_PET_BLUSH);
      break;
    }
    case PET_HOT: {
      drawPetEyesOpen(12);
      for (int dy = 0; dy < 3; dy++)
        for (int dx = -1; dx <= 1; dx++)
          petPixel(mouthX + dx, mouthY + dy, COL_BAD);
      petPixel(21, 2, COL_PET_SWEAT);
      petPixel(21, 3, COL_PET_SWEAT);
      petPixel(22, 4, COL_PET_SWEAT);
      break;
    }
    case PET_COLD: {
      drawPetEyesOpen(12);
      int zigzag[][2] = {{-2, 1}, {-1, 0}, {0, 1}, {1, 0}, {2, 1}};
      drawPetMouth(zigzag, 5, mouthX, mouthY, COL_TEXT);
      break;
    }
    case PET_HUMID: {
      drawPetEyesDroopy(11);
      int flat[][2] = {{-2, 2}, {-1, 2}, {0, 2}, {1, 2}, {2, 2}};
      drawPetMouth(flat, 5, mouthX, mouthY, COL_TEXT);
      break;
    }
    case PET_SLEEPY:
    default: {
      drawPetEyesClosed(12);
      petPixel(mouthX, mouthY + 1, COL_TEXT);
      petPixel(mouthX + 1, mouthY + 1, COL_TEXT);
      petSprite.setTextFont(2);
      petSprite.setTextColor(COL_ACCENT, COL_BG);
      petSprite.setTextDatum(TL_DATUM);
      petSprite.drawString("Z", PET_ORIGIN_X + PET_GRID * PET_SCALE - 10, PET_ORIGIN_Y - 4);
      petSprite.drawString("z", PET_ORIGIN_X + PET_GRID * PET_SCALE + 2, PET_ORIGIN_Y + 6);
      break;
    }
  }
}

void drawPetScreen() {
  tft.fillScreen(COL_BG);
  drawFrame();
  lastDrawnPetExpr = PET_EXPR_NONE;
  lastPetCheckAt = 0;
}

void updatePetScreen() {
  unsigned long now = millis();
  if (now - lastPetCheckAt < petCheckIntervalMs && lastPetCheckAt != 0) return;
  lastPetCheckAt = now;

  PetExpr expr = determinePetExpression();
  if (expr == lastDrawnPetExpr) return;
  lastDrawnPetExpr = expr;

  renderPetSprite(expr);
  petSprite.pushSprite(tft.width() / 2 - PET_W / 2, tft.height() / 2 - PET_H / 2);
}

void drawDino(int baseX, int baseY) {
  // baseY = ground/feet line; the bitmap's last row sits on this line.
  int originX = baseX + DINO_DRAW_OFFSET_X;
  int originY = baseY - DINO_GRID_H * DINO_SCALE;

  for (int row = 0; row < DINO_GRID_H; row++) {
    for (int col = 0; col < DINO_GRID_W; col++) {
      if (pgm_read_byte(&DINO_BITMAP[row][col])) {
        int x = originX + col * DINO_SCALE;
        int y = originY + row * DINO_SCALE;
        gameSprite.fillRect(x, y, DINO_SCALE, DINO_SCALE, COL_TEXT);
      }
    }
  }
}

void drawCactus(int x, int w, int h, int groundY) {
  gameSprite.fillRect(x, groundY - h, w, h, COL_TEXT);
  gameSprite.fillRect(x - 4, groundY - h + 6, 4, 5, COL_TEXT);
  gameSprite.fillRect(x + w, groundY - h + 10, 4, 5, COL_TEXT);
}

void resetGameObstacle() {
  gameObstacleX = GAME_W + random(20, 80);
  gameObstacleW = random(6, 11);
  gameObstacleH = random(14, 24);
}

void drawGameScreen() {
  tft.fillScreen(COL_BG);
  drawFrame();
  lastGameFrameAt = 0;
  gameGroundScroll = 0;
  gameJumping = false;
  resetGameObstacle();
}

void updateGameScreen() {
  unsigned long now = millis();
  if (now - lastGameFrameAt < GAME_FRAME_MS && lastGameFrameAt != 0) return;
  lastGameFrameAt = now;

  gameObstacleX -= GAME_SCROLL_SPEED;
  if (gameObstacleX + gameObstacleW < 0) {
    resetGameObstacle();
  }

  float distToObstacle = gameObstacleX - GAME_DINO_X;
  if (!gameJumping && distToObstacle > 0 && distToObstacle < GAME_JUMP_TRIGGER_DIST) {
    gameJumping = true;
    gameJumpStartAt = now;
  }

  int dinoYOffset = 0;
  if (gameJumping) {
    unsigned long elapsed = now - gameJumpStartAt;
    if (elapsed >= GAME_JUMP_MS) {
      gameJumping = false;
    } else {
      float t = (float)elapsed / GAME_JUMP_MS;
      dinoYOffset = -(int)(GAME_JUMP_HEIGHT * sinf(PI * t));
    }
  }

  gameGroundScroll += GAME_SCROLL_SPEED;

  gameSprite.fillSprite(COL_BG);
  gameSprite.drawFastHLine(0, GAME_GROUND_Y, GAME_W, COL_TEXT);

  int dashSpacing = 16;
  int off = (int)gameGroundScroll % dashSpacing;
  for (int x = -off; x < GAME_W; x += dashSpacing) {
    gameSprite.drawFastHLine(x, GAME_GROUND_Y + 3, 6, COL_DIM);
  }

  drawCactus((int)gameObstacleX, gameObstacleW, gameObstacleH, GAME_GROUND_Y);
  drawDino(GAME_DINO_X, GAME_GROUND_Y + dinoYOffset);

  gameSprite.pushSprite(tft.width() / 2 - GAME_W / 2, tft.height() / 2 - GAME_H / 2);
}

void updateLdrLabel() {
  unsigned long now = millis();
  if (now - lastLdrUpdateAt < ldrUpdateIntervalMs && lastLdrUpdateAt != 0) return;
  lastLdrUpdateAt = now;

  int raw = analogRead(ldrPin);
  if (ldrSmoothed < 0) ldrSmoothed = raw;
  ldrSmoothed += (raw - ldrSmoothed) * 0.15f;
  int display = (int)ldrSmoothed;

  if (display == lastLdrDisplayed) return;
  lastLdrDisplayed = display;

  char buf[16];
  snprintf(buf, sizeof(buf), "LDR %d", display);

  ldrSprite.fillSprite(COL_BG);
  ldrSprite.setTextFont(2);
  ldrSprite.setTextColor(COL_ACCENT, COL_BG);
  ldrSprite.drawString(buf, LDR_W / 2, LDR_H / 2);
  ldrSprite.pushSprite(tft.width() / 2 - LDR_W / 2, 14);
}

void applyLightTheme() {
  COL_BG = TFT_WHITE;
  COL_TEXT = TFT_BLACK;
  COL_ACCENT = tft.color565(70, 70, 70);
  COL_DIM = tft.color565(210, 210, 210);
}

void applyDarkTheme() {
  COL_BG = tft.color565(15, 15, 18);
  COL_TEXT = tft.color565(235, 235, 230);
  COL_ACCENT = tft.color565(160, 160, 165);
  COL_DIM = tft.color565(55, 55, 60);
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

  if (currentScreen == SCR_CLOCK) {
    drawClockFrame();
  } else if (currentScreen == SCR_DHT) {
    drawDhtScreen();
  }
}

void setup() {
  Serial.begin(115200);

  applyLightTheme();
  COL_GOOD = tft.color565(30, 140, 60);
  COL_BAD = tft.color565(190, 40, 40);
  COL_PET_BODY = tft.color565(255, 196, 140);
  COL_PET_SHADE = tft.color565(225, 150, 90);
  COL_PET_HILITE = tft.color565(255, 225, 190);
  COL_PET_BLUSH = tft.color565(255, 140, 160);
  COL_PET_SWEAT = tft.color565(120, 180, 255);

  pinMode(touchPin, INPUT_PULLDOWN);
  dht.begin();
  analogSetPinAttenuation(ldrPin, ADC_0db);

  tft.init();
  tft.setRotation(1);

  connectWiFi();
  syncTime();

  clockSprite.setColorDepth(16);
  clockSprite.createSprite(SPRITE_W, SPRITE_H);
  clockSprite.setTextDatum(MC_DATUM);

  ldrSprite.setColorDepth(16);
  ldrSprite.createSprite(LDR_W, LDR_H);
  ldrSprite.setTextDatum(MC_DATUM);

  nepseSprite.setColorDepth(16);
  nepseSprite.createSprite(NEPSE_W, NEPSE_H);

  petSprite.setColorDepth(16);
  petSprite.createSprite(PET_W, PET_H);

  gameSprite.setColorDepth(16);
  gameSprite.createSprite(GAME_W, GAME_H);
  randomSeed(analogRead(34));

  drawClockFrame();
  screenStartAt = millis();
}

void switchScreen(Screen next) {
  currentScreen = next;
  screenStartAt = millis();
  switch (currentScreen) {
    case SCR_CLOCK: drawClockFrame(); break;
    case SCR_DHT: drawDhtScreen(); break;
    case SCR_NEPSE: drawNepseScreen(); break;
    case SCR_PET: drawPetScreen(); break;
    case SCR_GAME: drawGameScreen(); break;
    default: break;
  }
}

const unsigned long debounceMs = 250;
unsigned long lastToggleAt = 0;

void loop() {
  int reading = digitalRead(touchPin);
  unsigned long now = millis();

  if (reading == HIGH && lastTouchRead == LOW && (now - lastToggleAt) > debounceMs) {
    lastToggleAt = now;
    switchScreen(currentScreen == SCR_CLOCK ? SCR_DHT : SCR_CLOCK);
  }
  lastTouchRead = reading;

  unsigned long elapsed = now - screenStartAt;
  if (elapsed >= screenDurations[currentScreen]) {
    switchScreen(currentScreen == SCR_CLOCK ? SCR_DHT : SCR_CLOCK);
  }

  switch (currentScreen) {
    case SCR_CLOCK: updateClock(); break;
    case SCR_DHT: updateDhtScreen(); break;
    default: break;
  }
  updateLdrLabel();
  updateTheme();

  delay(50);
}
