/**
* Interactive: Allosaurus Footprint Interactive
* File: esp32.ino
*
* Description: 
* Main ESP32 firmware that reads three stomp-pad mats (industrial 
* pressure safety mats) through H11L1M optoisolators and, on a validated stomp,
* pulses two GPIOs LOW at once to trigger the matching mat's media (BrightSign)
* and QuinLED light strip driver
*
*
* Author: Isai Sanchez  
* Date: 7-31-2026
* Board: ESP32-DevKitC
* Notes:
*   - One video plays at a time: the FIRST stomp wins, and every mat input is then
*   ignored until that mat's video finishes (see Config.h), so a clip is never in
*   danger of being cut off or re-triggered mid-play.
*
*   - Signal chain per mat:
*     dry N.O. contact --> long wire run --> H11L1M optoisolator --> ESP32 input
*     The optocoupler output is open-collector with an external pull-up:
*       idle (unpressed) = HIGH
*       stomped = LOW  (active-low).
*     (see optocoupler schematic for reasoning behind its implementation)
*
* (c) Thanksgiving Point Exhibits Electronics Team — 2025
*/

#include "esp_task_wdt.h"

#include "src/Config.h"
#include "src/MatInput.h"

// One detector per mat. Index i drives index i of every output bank below.
static MatInput mats[MAT_COUNT];

// The output banks that fire together on each validated stomp. Within every
// bank, index i corresponds to MAT_INPUT_PINS[i]. (A BANK_AUDIO entry was
// removed at the design team's request; see Config.h for how to restore it.)
enum OutputBankId : uint8_t { BANK_MEDIA,
                              BANK_LIGHT,
                              BANK_COUNT };

static const uint8_t *const OUTPUT_BANK_PINS[BANK_COUNT] = {
  MEDIA_TRIGGER_PINS,
  LIGHT_TRIGGER_PINS,
};

// Non-blocking pulse bookkeeping per (bank, mat): while active, the line is held
// at its trigger level until OUTPUT_PULSE_MS has elapsed since pulseStart.
static bool pulseActive[BANK_COUNT][MAT_COUNT] = {};
static uint32_t pulseStart[BANK_COUNT][MAT_COUNT] = {};

// Latched fault state per mat, so we log each fail-closed / recovery once (on
// the edge) rather than every loop.
static bool matFaulted[MAT_COUNT] = { false };

// Global retrigger lockout. After a mat fires, EVERY mat input is ignored until
// this deadline (that mat's video length + RETRIGGER_HEADROOM_MS), so a video
// always plays start-to-finish. lockoutActive gates the comparison so a plain
// millis() rollover can't look like an expired lockout.
static bool lockoutActive = false;
static uint32_t lockoutUntil = 0;

// Logic levels for any trigger line, derived from its active polarity (shared
// by all banks).
static inline uint8_t triggerIdleLevel() {
  return OUTPUT_TRIGGER_ACTIVE_LOW ? HIGH : LOW;
}
static inline uint8_t triggerActiveLevel() {
  return OUTPUT_TRIGGER_ACTIVE_LOW ? LOW : HIGH;
}

// Fire every bank's output for mat i at once (media + light). Each line is
// driven active; serviceOutputs() returns them to idle after OUTPUT_PULSE_MS.
static void triggerMat(uint8_t i, uint32_t now) {
  for (uint8_t b = 0; b < BANK_COUNT; ++b) {
    digitalWrite(OUTPUT_BANK_PINS[b][i], triggerActiveLevel());
    pulseActive[b][i] = true;
    pulseStart[b][i] = now;
  }
}

// Return any output line to idle once its pulse width has elapsed. Non-blocking
// so mat scanning is never stalled by an in-flight pulse.
static void serviceOutputs(uint32_t now) {
  for (uint8_t b = 0; b < BANK_COUNT; ++b) {
    for (uint8_t i = 0; i < MAT_COUNT; ++i) {
      if (pulseActive[b][i] && (now - pulseStart[b][i]) >= OUTPUT_PULSE_MS) {
        digitalWrite(OUTPUT_BANK_PINS[b][i], triggerIdleLevel());
        pulseActive[b][i] = false;
      }
    }
  }
}

// Immediately force all of mat i's output lines idle and cancel any in-flight
// pulses. Used when a mat faults so a stuck input can't leave outputs asserted.
static void forceMatIdle(uint8_t i) {
  for (uint8_t b = 0; b < BANK_COUNT; ++b) {
    digitalWrite(OUTPUT_BANK_PINS[b][i], triggerIdleLevel());
    pulseActive[b][i] = false;
  }
}

// Subscribe the main (loop) task to the hardware task watchdog. Handles both
// the ESP32 Arduino core 3.x config-struct API and the 2.x timeout/panic API.
static void watchdogBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  const esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_MS,
    .idle_core_mask = 0,    // don't watch the idle tasks, only ours
    .trigger_panic = true,  // panic + reset on timeout
  };
  esp_task_wdt_deinit();  // the core may have already initialised the TWDT
  esp_task_wdt_init(&wdtConfig);
#else
  esp_task_wdt_init(WDT_TIMEOUT_MS / 1000, true);
#endif
  esp_task_wdt_add(NULL);  // NULL = the currently running (loop) task
}

void setup() {
  Serial.begin(SERIAL_BAUD);

  for (uint8_t i = 0; i < MAT_COUNT; ++i) {
    mats[i].begin(MAT_INPUT_PINS[i]);
  }

  for (uint8_t b = 0; b < BANK_COUNT; ++b) {
    for (uint8_t i = 0; i < MAT_COUNT; ++i) {
      pinMode(OUTPUT_BANK_PINS[b][i], OUTPUT);
      digitalWrite(OUTPUT_BANK_PINS[b][i], triggerIdleLevel());
    }
  }

  watchdogBegin();

  Serial.println(F("Allosaurus Footprint Interactive - ready"));
}

void loop() {
  const uint32_t now = millis();

  // Feed the watchdog. loop() is non-blocking, so reaching here every pass is
  // proof the firmware is still alive; a hang would starve this and reset us.
  esp_task_wdt_reset();

  // Clear the global lockout once the playing video (plus headroom) has elapsed.
  if (lockoutActive && (int32_t)(now - lockoutUntil) >= 0) {
    lockoutActive = false;
  }

  for (uint8_t i = 0; i < MAT_COUNT; ++i) {
    mats[i].update();

    // Fail-closed handling: log each fault/recovery on its edge and, on fault,
    // force all of the mat's outputs idle so a stuck mat can't leave a device
    // asserted. Faults are safety-critical, so they are handled regardless of
    // the retrigger lockout.
    const bool faulted = mats[i].faulted();
    if (faulted != matFaulted[i]) {
      matFaulted[i] = faulted;
      if (faulted) {
        forceMatIdle(i);
        Serial.printf("FAULT: Mat %u stuck active (input GPIO %u) - "
                      "check for a failed-closed mat/short\n",
                      i + 1, MAT_INPUT_PINS[i]);
      } else {
        Serial.printf("RECOVERED: Mat %u back to normal (input GPIO %u)\n",
                      i + 1, MAT_INPUT_PINS[i]);
      }
    }

    // Always consume the event latch so a stomp during a lockout is discarded
    // rather than queued to fire the instant the lockout ends.
    const bool fired = mats[i].eventFired();
    if (fired && !lockoutActive) {
      const uint32_t lockoutMs = VIDEO_LENGTH_MS[i] + RETRIGGER_HEADROOM_MS;
      if (SERIAL_LOG_EVENTS) {
        Serial.printf("Mat %u triggered (input GPIO %u -> media %u / light %u); "
                      "ignoring all mats for %lu ms\n",
                      i + 1, MAT_INPUT_PINS[i], MEDIA_TRIGGER_PINS[i],
                      LIGHT_TRIGGER_PINS[i], (unsigned long)lockoutMs);
      }
      triggerMat(i, now);
      lockoutActive = true;
      lockoutUntil = now + lockoutMs;
    }
  }

  serviceOutputs(now);
}
