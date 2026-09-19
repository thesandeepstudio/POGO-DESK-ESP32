#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Preferences.h>

const int touchPin = 21;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite petSprite = TFT_eSprite(&tft);

uint16_t COL_BG;
uint16_t COL_TEXT;
uint16_t COL_ACCENT;
uint16_t COL_DIM;
uint16_t COL_PET_BODY;
uint16_t COL_PET_SHADE;
uint16_t COL_PET_HILITE;
uint16_t COL_PET_BLUSH;
uint16_t COL_PET_SWEAT;

// Pixel-art pet
const int PET_GRID = 26;
const int PET_SCALE = 6;
const int PET_W = 200;
const int PET_H = 190;
const int PET_ORIGIN_X = (PET_W - PET_GRID * PET_SCALE) / 2;
const int PET_ORIGIN_Y = 12;

enum PetExpr { PET_HAPPY, PET_SAD, PET_EXPR_NONE };
PetExpr lastDrawnPetExpr = PET_EXPR_NONE;
unsigned long lastPetCheckAt = 0;
const unsigned long petCheckIntervalMs = 2000;

// Happiness — persistent stat (ESP32 NVS). Decays over time, boosted by petting.
const char* HAPPY_NS = "mochi";
const float HAPPY_START = 60.0f;
const float HAPPY_MAX = 100.0f;
const float HAPPY_MIN = 0.0f;
const float HAPPY_DECAY_PER_SEC = 0.06f;        // ~1 pt / 17s — tune to taste
const float HAPPY_TAP_BOOST = 6.0f;
const float HAPPY_LONGPRESS_BOOST = 15.0f;
bool happyLoaded = false;
float happy = HAPPY_START;
unsigned long lastHappyTickAt = 0;
const unsigned long happyTickMs = 5000;

// Petting feedback
enum PetReaction { PET_REACT_NONE, PET_REACT_TAP, PET_REACT_HEART };
PetReaction activeReaction = PET_REACT_NONE;
unsigned long reactionUntilMs = 0;
const unsigned long reactionShowMs = 1500;

void applyLightTheme() {
  COL_BG = TFT_WHITE;
  COL_TEXT = TFT_BLACK;
  COL_ACCENT = tft.color565(70, 70, 70);
  COL_DIM = tft.color565(210, 210, 210);
}

void drawFrame() {
  int w = tft.width();
  int h = tft.height();
  tft.drawRoundRect(4, 4, w - 8, h - 8, 14, COL_DIM);
}

void loadHappiness() {
  Preferences prefs;
  prefs.begin(HAPPY_NS, true);
  float saved = prefs.getFloat("happy", HAPPY_START);
  prefs.end();
  happy = saved;
  happyLoaded = true;
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

PetExpr determinePetExpression() {
  if (activeReaction != PET_REACT_NONE) return PET_HAPPY;
  if (happy < 25.0f) return PET_SAD;
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

void drawPetHeartEyes() {
  int eyeXs[2] = {9, 17};
  for (int e = 0; e < 2; e++) {
    int ex = eyeXs[e];
    petPixel(ex - 1, 12, COL_TEXT);
    petPixel(ex + 1, 12, COL_TEXT);
    petPixel(ex, 13, COL_TEXT);
    petPixel(ex - 2, 13, COL_TEXT);
    petPixel(ex + 2, 13, COL_TEXT);
    petPixel(ex - 1, 14, COL_TEXT);
    petPixel(ex + 1, 14, COL_TEXT);
    petPixel(ex, 15, COL_TEXT);
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

  if (activeReaction == PET_REACT_HEART) {
    drawPetHeartEyes();
    int smile[][2] = {{-3, 0}, {-2, 1}, {-1, 2}, {0, 2}, {1, 2}, {2, 1}, {3, 0}};
    drawPetMouth(smile, 7, mouthX, mouthY, COL_TEXT);
    petPixel(5, 16, COL_PET_BLUSH);
    petPixel(21, 16, COL_PET_BLUSH);
    return;
  }

  switch (expr) {
    case PET_HAPPY:
    default: {
      drawPetEyesOpen(12);
      int smile[][2] = {{-3, 0}, {-2, 1}, {-1, 2}, {0, 2}, {1, 2}, {2, 1}, {3, 0}};
      drawPetMouth(smile, 7, mouthX, mouthY, COL_TEXT);
      petPixel(5, 16, COL_PET_BLUSH);
      petPixel(21, 16, COL_PET_BLUSH);
      break;
    }
    case PET_SAD: {
      drawPetEyesDroopy(11);
      int frown[][2] = {{-2, 3}, {-1, 2}, {0, 2}, {1, 2}, {2, 3}};
      drawPetMouth(frown, 5, mouthX, mouthY, COL_TEXT);
      petPixel(7, 16, COL_PET_SWEAT);
      petPixel(7, 17, COL_PET_SWEAT);
      petPixel(6, 18, COL_PET_SWEAT);
      break;
    }
  }
}

void updatePetScreen() {
  unsigned long now = millis();
  if (now - lastPetCheckAt < petCheckIntervalMs && lastPetCheckAt != 0) return;
  lastPetCheckAt = now;

  if (activeReaction != PET_REACT_NONE && now >= reactionUntilMs) {
    activeReaction = PET_REACT_NONE;
    lastDrawnPetExpr = PET_EXPR_NONE;
  }

  PetExpr expr = determinePetExpression();
  if (expr == lastDrawnPetExpr && activeReaction == PET_REACT_NONE) return;
  lastDrawnPetExpr = expr;

  renderPetSprite(expr);
  petSprite.pushSprite(tft.width() / 2 - PET_W / 2, tft.height() / 2 - PET_H / 2);
}

const unsigned long debounceMs = 250;
const unsigned long longPressMs = 700;
unsigned long pressStartAt = 0;
bool longPressFired = false;
int lastTouchRead = LOW;

void handlePetReaction(bool isLongPress) {
  if (isLongPress) {
    addHappiness(HAPPY_LONGPRESS_BOOST);
    activeReaction = PET_REACT_HEART;
  } else {
    addHappiness(HAPPY_TAP_BOOST);
    activeReaction = PET_REACT_TAP;
  }
  reactionUntilMs = millis() + reactionShowMs;
  lastDrawnPetExpr = PET_EXPR_NONE;
  lastPetCheckAt = 0;
}

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
    handlePetReaction(true);
    return;
  }

  if (!isPressed && wasPressed && !longPressFired && (now - pressStartAt) >= debounceMs) {
    handlePetReaction(false);
  }
}

void setup() {
  Serial.begin(115200);

  applyLightTheme();
  COL_PET_BODY = tft.color565(255, 196, 140);
  COL_PET_SHADE = tft.color565(225, 150, 90);
  COL_PET_HILITE = tft.color565(255, 225, 190);
  COL_PET_BLUSH = tft.color565(255, 140, 160);
  COL_PET_SWEAT = tft.color565(120, 180, 255);

  pinMode(touchPin, INPUT_PULLDOWN);

  tft.init();
  tft.setRotation(1);

  petSprite.setColorDepth(16);
  petSprite.createSprite(PET_W, PET_H);
  randomSeed(analogRead(34));

  loadHappiness();

  tft.fillScreen(COL_BG);
  drawFrame();
  lastDrawnPetExpr = PET_EXPR_NONE;
  lastPetCheckAt = 0;
}

void loop() {
  unsigned long now = millis();

  int reading = digitalRead(touchPin);
  handleTouch(reading, now);

  updatePetScreen();
  tickHappiness();

  delay(50);
}