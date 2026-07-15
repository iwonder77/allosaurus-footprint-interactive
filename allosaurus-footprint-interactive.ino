/*
 * Allosaurus Footprint Interactive — ESP32 firmware
 *
 * Reads three stomp-pad mats (industrial pressure safety mats) through H11L1M
 * optoisolators and, on a validated stomp, pulses a dedicated GPIO LOW to
 * trigger the matching clip on a BrightSign media player.
 *
 * Signal chain per mat:
 *   dry NO contact --long wire run--> H11L1M optoisolator --> ESP32 input
 *   The optocoupler output is open-collector with an external pull-up:
 *     idle (unpressed) = HIGH, stomped = LOW  (active-low).
 *
 * This top-level sketch only wires modules together: MatInput does the
 * debouncing and Config.h holds every pin/timing constant.
 */

#include "src/Config.h"
#include "src/MatInput.h"

// One detector per mat. Index i maps MAT_INPUT_PINS[i] -> MEDIA_TRIGGER_PINS[i].
static MatInput mats[MAT_COUNT];

// Non-blocking media-pulse bookkeeping: while active, the line is held at its
// trigger level until MEDIA_PULSE_MS has elapsed since mediaPulseStart.
static bool mediaPulseActive[MAT_COUNT] = {false};
static uint32_t mediaPulseStart[MAT_COUNT] = {0};

// Logic levels for a media-trigger line, derived from its active polarity.
static inline uint8_t mediaIdleLevel() {
  return MEDIA_TRIGGER_ACTIVE_LOW ? HIGH : LOW;
}
static inline uint8_t mediaActiveLevel() {
  return MEDIA_TRIGGER_ACTIVE_LOW ? LOW : HIGH;
}

// Start a trigger pulse on media channel i (drives the line to its active
// level; serviceMedia() returns it to idle after MEDIA_PULSE_MS).
static void triggerMedia(uint8_t i, uint32_t now) {
  digitalWrite(MEDIA_TRIGGER_PINS[i], mediaActiveLevel());
  mediaPulseActive[i] = true;
  mediaPulseStart[i] = now;
}

// Return any media line to idle once its pulse width has elapsed. Non-blocking
// so mat scanning is never stalled by an in-flight pulse.
static void serviceMedia(uint32_t now) {
  for (uint8_t i = 0; i < MAT_COUNT; ++i) {
    if (mediaPulseActive[i] && (now - mediaPulseStart[i]) >= MEDIA_PULSE_MS) {
      digitalWrite(MEDIA_TRIGGER_PINS[i], mediaIdleLevel());
      mediaPulseActive[i] = false;
    }
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);

  for (uint8_t i = 0; i < MAT_COUNT; ++i) {
    mats[i].begin(MAT_INPUT_PINS[i]);

    // Park each media line at its idle (non-triggering) level before enabling
    // the output, so we never emit a spurious trigger at boot.
    digitalWrite(MEDIA_TRIGGER_PINS[i], mediaIdleLevel());
    pinMode(MEDIA_TRIGGER_PINS[i], OUTPUT);
    digitalWrite(MEDIA_TRIGGER_PINS[i], mediaIdleLevel());
  }

  Serial.println(F("Allosaurus Footprint Interactive - ready"));
}

void loop() {
  const uint32_t now = millis();

  for (uint8_t i = 0; i < MAT_COUNT; ++i) {
    mats[i].update();

    if (mats[i].eventFired()) {
      if (SERIAL_LOG_EVENTS) {
        Serial.printf("Mat %u triggered (input GPIO %u -> media GPIO %u)\n",
                      i + 1, MAT_INPUT_PINS[i], MEDIA_TRIGGER_PINS[i]);
      }
      triggerMedia(i, now);
    }
  }

  serviceMedia(now);
}
