#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Preferences.h>

const int touchPin = 21;

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite petSprite = TFT_eSprite(&tft);

uint16_t COL_BG = TFT_BLACK;
uint16_t COL_FACE = TFT_WHITE;
uint16_t COL_EYE = TFT_BLACK;
uint16_t COL_FRAME = tft.color565(40, 40, 45);
uint16_t COL_ACCENT = tft.color565(160, 160, 165);

// Mochi geometry (sprite pixels, sprite = PET_W x PET_H)
const int PET_W = 200;
const int PET_H = 190;

const float BODY_CX = 100.0f, BODY_CY = 100.0f, BODY_RX = 66.0f, BODY_RY = 60.0f;
const float EAR_L_CX = 48.0f, EAR_R_CX = 152.0f, EAR_CY = 42.0f, EAR_R = 27.0f;

const float EYE_L_X = 79.0f, EYE_R_X = 121.0f, EYE_Y = 92.0f, EYE_RX = 11.0f, EYE_RY_MAX = 13.0f;

// Happiness — persistent stat (ESP32 NVS).
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

// Petting reaction — widen eyes for a moment (still eyes-only)
unsigned long widenUntilMs = 0;
const unsigned long widenShowMs = 400;

// Blink state machine
enum BlinkStage { BLINK_OPEN, BLINK_CLOSING, BLINK_CLOSED, BLINK_OPENING };
BlinkStage blinkStage = BLINK_OPEN;
unsigned long blinkStartAt = 0;
unsigned long openHoldMs = 2500;
const unsigned long CLOSE_MS = 110;
const unsigned long CLOSED_MS = 90;
const unsigned long OPEN_MS = 150;

float smoothstep(float t) {
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  return t * t * (3.0f - 2.0f * t);
}

float blinkScale() {
  unsigned long now = millis();
  switch (blinkStage) {
    case BLINK_OPEN:
      if (now - blinkStartAt >= openHoldMs) {
        blinkStage = BLINK_CLOSING;
        blinkStartAt = now;
      }
      return 1.0f;
    case BLINK_CLOSING: {
      float t = (float)(now - blinkStartAt) / CLOSE_MS;
      if (t >= 1) {
        t = 1;
        blinkStage = BLINK_CLOSED;
        blinkStartAt = now;
      }
      return 1.0f - smoothstep(t);
    }
    case BLINK_CLOSED:
      if (now - blinkStartAt >= CLOSED_MS) {
        blinkStage = BLINK_OPENING;
        blinkStartAt = now;
      }
      return 0.0f;
    case BLINK_OPENING: {
      float t = (float)(now - blinkStartAt) / OPEN_MS;
      if (t >= 1) {
        t = 1;
        blinkStage = BLINK_OPEN;
        blinkStartAt = now;
        openHoldMs = 2000 + random(1500);
      }
      return smoothstep(t);
    }
  }
  return 1.0f;
}

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

void drawEye(float cx, float ry) {
  if (ry <= 0.8f) return;  // fully closed — no pupil visible
  float rx = EYE_RX * (1.0f + (1.0f - ry / EYE_RY_MAX) * 0.06f);  // slight squash-&-stretch
  petSprite.fillEllipse(cx, EYE_Y, rx, ry, COL_EYE);
}

void renderPet() {
  unsigned long now = millis();

  float scale = blinkScale();
  bool widening = now < widenUntilMs;
  float ry = EYE_RY_MAX * scale * (widening ? 1.15f : 1.0f);

  petSprite.fillSprite(COL_BG);

  // Ears first, then body covers their bottoms — merged mochi silhouette with ear bumps
  petSprite.fillCircle(EAR_L_CX, EAR_CY, EAR_R, COL_FACE);
  petSprite.fillCircle(EAR_R_CX, EAR_CY, EAR_R, COL_FACE);
  petSprite.fillEllipse(BODY_CX, BODY_CY, BODY_RX, BODY_RY, COL_FACE);

  drawEye(EYE_L_X, ry);
  drawEye(EYE_R_X, ry);

  petSprite.pushSprite(tft.width() / 2 - PET_W / 2, tft.height() / 2 - PET_H / 2);
}

void handlePetReaction(bool isLongPress) {
  widenUntilMs = millis() + widenShowMs;
  if (isLongPress) {
    addHappiness(HAPPY_LONGPRESS_BOOST);
    widenUntilMs = millis() + 800;
  } else {
    addHappiness(HAPPY_TAP_BOOST);
  }
}

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
    handlePetReaction(true);
    return;
  }

  if (!isPressed && wasPressed && !longPressFired && (now - pressStartAt) >= debounceMs) {
    handlePetReaction(false);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(touchPin, INPUT_PULLDOWN);

  tft.init();
  tft.setRotation(1);

  petSprite.setColorDepth(16);
  petSprite.createSprite(PET_W, PET_H);
  randomSeed(ESP.getEfuseMac());

  loadHappiness();
  blinkStartAt = millis();

  tft.fillScreen(COL_BG);
  tft.drawRoundRect(4, 4, tft.width() - 8, tft.height() - 8, 14, COL_FRAME);
}

void loop() {
  unsigned long now = millis();

  int reading = digitalRead(touchPin);
  handleTouch(reading, now);

  renderPet();
  tickHappiness();

  delay(16);  // ~60fps — smooth liquidy animation
}