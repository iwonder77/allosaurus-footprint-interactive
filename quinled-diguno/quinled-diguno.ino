/**
 * Allosaurus Footprint Interactive — LED strip controller
 * File: quinled-diguno.ino
 * Board: QuinLED Dig-Uno (ESP32-based; LED outputs are level-shifted to 5V)
 * Strip: WS2815 addressable, 152 pixels
 *
 * Behaviour:
 *   - IDLE: the whole strip holds a constant warm white / amber glow.
 *   - On a stomp (GPIO15 pulled LOW by the optocoupler mat-trigger board) a fast
 *     red "snake" wipes down the strip: a leading pixel advances and every pixel
 *     it passes stays red, overtaking the amber glow. When the head reaches the
 *     final pixel the strip holds full red for FULL_RED_HOLD_MS (ignoring any
 *     further stomps), then cross-fades smoothly back to the amber idle glow.
 *
 * The GPIO15 input is debounced with the same active-low state machine used by
 * the esp32 mat sketch (debounce window + minimum on-time + re-arm cooldown,
 * plus boot-safety), so contact/optocoupler chatter can't double-fire.
 */

#include <FastLED.h>

// ===================== LED strip ==========================================
constexpr uint16_t NUM_LEDS = 152;
constexpr uint8_t LED_DATA_PIN = 3;  // QuinLED Dig-Uno LED data output
#define LED_TYPE WS2815
#define COLOR_ORDER GRB
constexpr uint8_t BRIGHTNESS_PCT = 75;  // master brightness, percent

// Idle glow and snake colors. IDLE_COLOR is a warm, slightly-amber white; tune
// the green/blue channels down for more amber, up for cooler white.
const CRGB IDLE_COLOR = CRGB(255, 120, 40);
const CRGB SNAKE_COLOR = CRGB::Red;

// ===================== Snake animation ====================================
constexpr float SNAKE_SPEED_PPS = 100.0f;  // leading-pixel speed, pixels/sec (higher = faster)
constexpr uint8_t TARGET_FPS = 120;        // render cap

// One trigger runs for TOTAL_CYCLE_MS: the snake wipe, the full-red hold, and
// the closing fade back to idle. The hold is whatever time is left after the
// wipe and fade, so wipe + hold + fade always sums to this total — raise
// SNAKE_SPEED_PPS and the hold grows, lower it and the hold shrinks. Stomps on
// MAT_INPUT_PIN are ignored for the whole cycle.
constexpr uint32_t TOTAL_CYCLE_MS = 10000;

// Smooth cross-fade from full red back to the amber idle glow at the end of the
// cycle. This is carved out of TOTAL_CYCLE_MS, not added on top.
constexpr uint32_t FADE_TO_IDLE_MS = 1000;

// Nominal time for the head to travel the strip (head goes from pixel 0 to
// NUM_LEDS-1 at SNAKE_SPEED_PPS); the rest of the budget is the full-red hold.
constexpr uint32_t SNAKE_WIPE_MS =
    (uint32_t)((NUM_LEDS - 1) / SNAKE_SPEED_PPS * 1000.0f);
static_assert(SNAKE_WIPE_MS + FADE_TO_IDLE_MS <= TOTAL_CYCLE_MS,
              "Snake wipe + fade exceed TOTAL_CYCLE_MS; raise the total or speed");
constexpr uint32_t FULL_RED_HOLD_MS =
    TOTAL_CYCLE_MS - SNAKE_WIPE_MS - FADE_TO_IDLE_MS;

constexpr uint16_t FRAME_INTERVAL_MS = 1000 / TARGET_FPS;

// ===================== Mat trigger input (optocoupler) ====================
// GPIO15 idles HIGH (external pull-up on the opto board) and is pulled LOW when
// the mat is stomped. GPIO15 is an ESP32 strapping pin that must be HIGH at
// boot — idle-HIGH satisfies that, so this wiring is boot-safe.
#define MAT_INPUT_PIN 32
constexpr bool MAT_ACTIVE_LOW = true;
constexpr bool MAT_USE_INTERNAL_PULLUP = true;  // `false` if external pull-up present

// Three independent debounce knobs (see the esp32 sketch's Config.h for the
// rationale of each). MAT_MIN_ON_MS must be >= MAT_DEBOUNCE_MS.
constexpr uint16_t MAT_DEBOUNCE_MS = 10;  // edge stability window
constexpr uint16_t MAT_MIN_ON_MS = 30;    // min dwell to count
constexpr uint16_t MAT_REARM_MS = 1000;   // min off-time before re-arming

/*
 * MatTrigger — non-blocking, debounced press detector for one active-low input.
 * Fires eventFired() exactly once per validated stomp. begin() starts in
 * WaitForRelease so a mat held at power-on never produces a spurious trigger.
 */
class MatTrigger {
public:
  void begin(uint8_t pin) {
    pin_ = pin;
    pinMode(pin_, MAT_USE_INTERNAL_PULLUP ? INPUT_PULLUP : INPUT);
    state_ = WaitForRelease;
    event_ = false;
    t_mark_ = millis();
  }

  // Advance the state machine. Non-blocking; call every loop.
  void update() {
    const uint32_t now = millis();
    const bool active = rawActive();

    switch (state_) {
      case WaitForRelease:  // boot-safe entry + cooldown; needs continuous release
        if (active) {
          t_mark_ = now;
        } else if (now - t_mark_ >= MAT_REARM_MS) {
          state_ = Armed;
        }
        break;

      case Armed:
        if (active) {  // first active edge
          t_mark_ = now;
          state_ = PressDebounce;
        }
        break;

      case PressDebounce:
        if (!active) {
          state_ = Armed;  // dropped during debounce -> chatter
        } else if (now - t_mark_ >= MAT_DEBOUNCE_MS) {
          state_ = PressConfirm;
        }
        break;

      case PressConfirm:  // min-on measured from the first edge
        if (!active) {
          t_mark_ = now;
          state_ = WaitForRelease;  // released too soon -> too short
        } else if (now - t_mark_ >= MAT_MIN_ON_MS) {
          event_ = true;  // VALIDATED STOMP — latch the event
          state_ = Held;
        }
        break;

      case Held:
        if (!active) {
          t_mark_ = now;  // begin cooldown
          state_ = WaitForRelease;
        }
        break;
    }
  }

  // True exactly once per validated stomp; reading it clears the latch.
  bool eventFired() {
    if (event_) {
      event_ = false;
      return true;
    }
    return false;
  }

private:
  bool rawActive() const {
    const int level = digitalRead(pin_);
    return MAT_ACTIVE_LOW ? (level == LOW) : (level == HIGH);
  }

  enum State : uint8_t { WaitForRelease,
                         Armed,
                         PressDebounce,
                         PressConfirm,
                         Held };

  uint8_t pin_ = 0;
  State state_ = WaitForRelease;
  bool event_ = false;
  uint32_t t_mark_ = 0;
};

// ===================== Runtime state ======================================
CRGB leds[NUM_LEDS];
MatTrigger mat;

enum Mode : uint8_t { IDLE,
                      SNAKE,
                      SNAKE_HOLD,
                      FADE_TO_IDLE };
Mode mode = IDLE;

float snakeHead = 0.0f;    // leading-pixel position, fractional
uint32_t lastFrameMs = 0;  // last rendered frame timestamp
uint32_t holdStart = 0;    // full-red dwell start timestamp
uint32_t fadeStart = 0;    // fade-to-idle start timestamp

// Paint the whole strip with the amber idle glow.
void renderIdle() {
  fill_solid(leds, NUM_LEDS, IDLE_COLOR);
  FastLED.show();
}

// Begin the red snake wipe from the first pixel.
void startSnake(uint32_t now) {
  mode = SNAKE;
  snakeHead = 0.0f;
  lastFrameMs = now;
}

// Advance and draw one snake frame: pixels at/behind the head are red, pixels
// ahead of it keep the amber idle glow. Transitions to SNAKE_HOLD at the end.
void updateSnake(uint32_t now) {
  const uint32_t dtMs = now - lastFrameMs;
  if (dtMs < FRAME_INTERVAL_MS) return;
  lastFrameMs = now;

  snakeHead += SNAKE_SPEED_PPS * (dtMs / 1000.0f);

  int head = (int)snakeHead;
  bool reachedEnd = false;
  if (head >= NUM_LEDS - 1) {
    head = NUM_LEDS - 1;
    reachedEnd = true;
  }

  for (int i = 0; i < NUM_LEDS; ++i) {
    leds[i] = (i <= head) ? SNAKE_COLOR : IDLE_COLOR;
  }
  FastLED.show();

  if (reachedEnd) {
    mode = SNAKE_HOLD;
    holdStart = now;
  }
}

// Cross-fade the whole strip from full red back to the amber idle glow over
// FADE_TO_IDLE_MS, then settle into IDLE. The strip is uniform here, so one
// blended color fills every pixel.
void updateFade(uint32_t now) {
  const uint32_t dtMs = now - lastFrameMs;
  if (dtMs < FRAME_INTERVAL_MS) return;
  lastFrameMs = now;

  const uint32_t elapsed = now - fadeStart;
  if (elapsed >= FADE_TO_IDLE_MS) {
    mode = IDLE;
    renderIdle();
    return;
  }

  // 0 -> full red, 255 -> full idle color.
  const uint8_t amount = (uint8_t)(elapsed * 255UL / FADE_TO_IDLE_MS);
  fill_solid(leds, NUM_LEDS, blend(SNAKE_COLOR, IDLE_COLOR, amount));
  FastLED.show();
}

void setup() {
  FastLED.addLeds<LED_TYPE, LED_DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness((255 * BRIGHTNESS_PCT) / 100);

  mat.begin(MAT_INPUT_PIN);

  renderIdle();
}

void loop() {
  const uint32_t now = millis();

  mat.update();
  const bool stomped = mat.eventFired();

  switch (mode) {
    case IDLE:
      if (stomped) startSnake(now);  // stomps during animation are ignored
      break;

    case SNAKE:
      updateSnake(now);
      break;

    case SNAKE_HOLD:
      // Full-red hold: `stomped` is still consumed above but deliberately
      // ignored here, so no input can retrigger until the timer elapses.
      if (now - holdStart >= FULL_RED_HOLD_MS) {
        mode = FADE_TO_IDLE;
        fadeStart = now;
        lastFrameMs = now;
      }
      break;

    case FADE_TO_IDLE:
      // Smooth return to idle; stomps remain ignored until the fade completes.
      updateFade(now);
      break;
  }
}
