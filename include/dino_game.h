// Playable Chrome-dino style mini-game for Pogo.
// Launched by double-tap; tap to jump; best score persisted in NVS.

#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>

extern TFT_eSPI tft;

const char GAME_NS[] = "dino";
const char GAME_HI_KEY[] = "hi";

const int DINO_GRID_W = 28;
const int DINO_GRID_H = 24;
const int DINO_SCALE = 2;

// Dino silhouette (feet on last row), same art as assets/dino_bitmap.txt.
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

static unsigned long gameHighScore() {
  Preferences p;
  p.begin(GAME_NS, true);
  unsigned long hi = p.getULong(GAME_HI_KEY, 0);
  p.end();
  return hi;
}

static void gameSaveScore(unsigned long score) {
  Preferences p;
  p.begin(GAME_NS, false);
  unsigned long hi = p.getULong(GAME_HI_KEY, 0);
  if (score > hi) p.putULong(GAME_HI_KEY, score);
  p.end();
}

// Draw dino row-by-row as compact horizontal runs to minimise TFT SPI calls.
static void gameDrawDino(int x, int groundY, int lift) {
  for (int row = 0; row < DINO_GRID_H; row++) {
    int runStart = -1;
    for (int col = 0; col <= DINO_GRID_W; col++) {
      bool on = col < DINO_GRID_W && pgm_read_byte(&DINO_BITMAP[row][col]);
      if (on && runStart < 0) {
        runStart = col;
      } else if (!on && runStart >= 0) {
        int px = x + runStart * DINO_SCALE;
        int py = groundY - 2 - lift - (DINO_GRID_H - row) * DINO_SCALE;
        tft.fillRect(px, py, (col - runStart) * DINO_SCALE, DINO_SCALE, TFT_WHITE);
        runStart = -1;
      }
    }
  }
}

void playGame(int pin) {
  unsigned long hi = gameHighScore();
  int w = tft.width();
  int groundY = tft.height() - 30;

  // Brief "get ready" screen.
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.drawString("TAP TO JUMP!", w / 2, tft.height() / 2 - 20);
  delay(1400);
  tft.setTextDatum(TL_DATUM);

  float dinoX = 40;
  float lift = 0.0f;      // height above ground (0 = grounded)
  float liftVy = 0.0f;
  const float GRAV = 0.55f;
  const float JUMP_V = 7.6f;

  float obsX = w + 40;
  int obsW = 10;
  int obsH = 22;

  float speed = 3.4f;
  unsigned long score = 0;
  unsigned long lastFrame = 0;
  const unsigned long frameMs = 30;
  bool over = false;
  bool jumpArmed = true; // rising-edge only jump

  while (!over) {
    unsigned long now = millis();
    if (now - lastFrame < frameMs) continue;
    lastFrame = now;

    int reading = digitalRead(pin);
    if (reading == HIGH && jumpArmed) jumpArmed = false;
    if (reading == LOW && !jumpArmed) jumpArmed = true;
    if (reading == HIGH && lift == 0 && liftVy == 0) liftVy = -JUMP_V; // grounded only

    // Physics
    liftVy += GRAV;
    lift += liftVy;
    if (lift < 0) { lift = 0; liftVy = 0; }

    // Obstacle scroll
    obsX -= speed;
    if (obsX + obsW < 0) {
      obsX = w + random(30, 90);
      obsW = random(7, 13);
      obsH = random(16, 30);
    }

    score++;
    if (score % 150 == 0) speed += 0.35f;

    // Collision (forgiving hitboxes)
    int oL = (int)obsX + 2;
    int oR = (int)obsX + obsW - 1;
    int oT = groundY - obsH + 2;
    int oB = groundY - 1;
    int dL = (int)dinoX + 7;
    int dR = (int)dinoX + DINO_GRID_W * DINO_SCALE - 4;
    int dB = groundY - (int)lift - 2;
    int dT = dB - DINO_GRID_H * DINO_SCALE + 6;
    if (dR >= oL && dL <= oR && dB >= oT && dT <= oB) over = true;

    // Draw
    tft.fillScreen(TFT_BLACK);
    tft.drawFastHLine(0, groundY, w, TFT_WHITE);
    tft.drawFastHLine(0, groundY + 3, w, tft.color565(70, 70, 70));

    // Ground dashes scrolling with speed
    int dash = 12;
    int off = (int)(score * 0.7f) % dash;
    for (int x = -off; x < w; x += dash) {
      tft.drawFastHLine(x, groundY + 5, 6, tft.color565(55, 55, 55));
    }

    gameDrawDino((int)dinoX, groundY, (int)lift);

    // Cactus
    tft.fillRect((int)obsX, groundY - obsH, obsW, obsH, TFT_WHITE);
    tft.fillRect((int)obsX - 4, groundY - obsH + 5, 4, 5, TFT_WHITE);
    tft.fillRect((int)obsX + obsW, groundY - obsH + 9, 4, 5, TFT_WHITE);

    char buf[20];
    snprintf(buf, sizeof(buf), "%lu", score);
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(4, 2);
    tft.print(buf);
    if (hi > 0) {
      snprintf(buf, sizeof(buf), "HI %lu", hi);
      tft.setCursor(w - 66, 2);
      tft.print(buf);
    }
  }

  // Game over + high score
  gameSaveScore(score);
  bool newRecord = score > hi;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("GAME OVER", w / 2, tft.height() / 2 - 24);
  char buf[20];
  snprintf(buf, sizeof(buf), "%lu", score);
  tft.drawString(buf, w / 2, tft.height() / 2 + 4);
  if (newRecord) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("NEW RECORD!", w / 2, tft.height() / 2 + 24);
  }
  tft.setTextDatum(TL_DATUM);
  delay(2600);
}