# Desk Pet 🐾

A little environment-aware, time-aware virtual pet that lives on a round touch display, powered by an ESP32.

## Concept

The pet's mood/expression is driven by three things:

1. **Time of day** — wakes up, stays alert during the day, calms down in the evening, sleeps at night
2. **Room comfort** — DHT sensor reads temperature/humidity, pet reacts (happy, hot/fanning, cold/shivering, humid/sluggish)
3. **Happiness** — boosted by touch (petting), decays slowly if neglected

**Priority order when picking the current expression:**
`Sleep (night) > Extreme discomfort (too hot/cold/humid) > Happiness-based idle mood`

## Hardware

| Component | Notes |
|---|---|
| ESP32 | Any dev board variant |
| GC9A01 round display | 1.28", 240x240, round TFT |
| Touch sensor | Single touch pad/pin |
| DHT sensor | DHT11 or DHT22 (confirm which) |

### Wiring
_TODO: fill in once pins are confirmed_

| Display Pin | ESP32 GPIO |
|---|---|
| SCLK | |
| MOSI | |
| CS | |
| DC | |
| RST | |
| BLK | |

| Other | ESP32 GPIO |
|---|---|
| Touch | |
| DHT Data | |

## Software Setup

1. Install [VS Code](https://code.visualstudio.com/)
2. Install the **PlatformIO IDE** extension (Extensions tab → search "PlatformIO IDE")
3. New Project → select your ESP32 board → framework: **Arduino**
4. Add libraries to `platformio.ini`:
   ```ini
   lib_deps =
       bodmer/TFT_eSPI
       adafruit/DHT sensor library
   ```
5. Configure `TFT_eSPI`'s `User_Setup.h` with the exact wiring pins above (this is the most common setup snag — do this before writing any pet code)

## Features / Behavior Spec

### Power-on sequence
- Screen boots dim/black
- Eyes slowly open (2–3 frame animation)
- Blink, stretch/yawn pose
- "Syncing..." indicator while WiFi + NTP time sync happens in background
- Settles into real current state (checks time + DHT + happiness)

### Time-of-day states
| Time | Behavior |
|---|---|
| 6–11am | Morning: wakes, stretches, extra cheerful |
| 11am–6pm | Day: alert, active idle animation |
| 6–10pm | Evening: calmer, slower blinking |
| 10pm–6am | Night: sleeping, eyes closed, "Zzz", dimmed screen |

### Comfort states (from DHT)
| Condition | Expression |
|---|---|
| Comfortable range | Content, relaxed idle bounce |
| Too hot | Fanning itself, sweat drop |
| Too cold | Shivering, huddled |
| Too humid | Droopy, sluggish |

### Touch interaction
| Gesture | Effect |
|---|---|
| Tap | Happy animation, Happiness +small boost |
| Long-press | Heart-eyes animation, Happiness +big boost, resets "last pet" timer |
| Neglect (no touch for X hours) | Happiness decays, pet looks sad/lonely regardless of comfort |

## Build Order

- [ ] **1. Display + sprites** — GC9A01 working, draw base pet with expressions (happy, sleepy, hot, cold, sad, neutral) + wake-up frames
- [ ] **2. Power-on wake sequence** — plays once on boot, bridges into WiFi/NTP sync
- [ ] **3. State machine** — combines time-of-day + comfort + happiness → current expression
- [ ] **4. DHT reading loop** — map readings to comfort thresholds
- [ ] **5. Touch handling** — tap vs long-press detection, Happiness boost + reaction animation
- [ ] **6. NTP time sync** — WiFi connect + time.h sync for accurate time-of-day states
- [ ] **7. Polish** — smooth idle animations (blinking loop, breathing bounce), transition easing between expressions

## Notes / Decisions Log

- Dropped Spotify/music integration — pet is fully environment + time driven, no phone/API dependency
- Comfort is calculated live (not stored/decaying) — always reflects current DHT reading
- Happiness is the only persistent/decaying stat
