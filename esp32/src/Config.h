#pragma once
#include <Arduino.h>

/*
 * Config.h — single source of truth for pins, polarity, and timing.
 *
 * Everything you might change on the bench lives here so the logic modules
 * (MatInput) stay untouched. Constants are `constexpr` (compile-time, zero
 * RAM).
 *
 * Target board: ESP32 (see sketch.yaml -> esp32:esp32:esp32).
 */

// ===================== Mat triggers (optocoupler inputs) ==================
/*
 * Each mat is a dry, normally-open contact switch on a long wire run, isolated
 * by an H11L1M. The optocoupler output is open-collector with an EXTERNAL
 * pull-up to 3.3V, so:  idle (unpressed) = HIGH, stomped = LOW  => active-low.
 *
 * Index order defines the mat numbering used everywhere else: MAT_INPUT_PINS[i]
 * is answered by MEDIA_TRIGGER_PINS[i] (same index).
 */
constexpr uint8_t MAT_INPUT_PINS[] = {13, 14, 15};
constexpr uint8_t MAT_COUNT =
    sizeof(MAT_INPUT_PINS) / sizeof(MAT_INPUT_PINS[0]);

// pressed = LOW because the external pull-up idles the line HIGH.
constexpr bool MAT_ACTIVE_LOW = true;

// The isolation stage already provides an external pull-up, so we use a plain
// INPUT. Set true only as a failsafe if you suspect the external pull-up; the
// internal ~45k in parallel is harmless but shifts the idle threshold slightly.
constexpr bool MAT_ENABLE_INTERNAL_PULLUP = false;

/*
 * Three INDEPENDENT timing knobs. They are intentionally NOT collapsed into one
 * value — each rejects a different failure mode. Defaults are sane starting
 * points; tune on the real mat with SERIAL_LOG_EVENTS on (double-triggers =
 * increase, missed stomps = decrease).
 *
 *   MAT_DEBOUNCE_MS : the input must stay continuously active for this long
 *                     before we BELIEVE the press began. Rejects fast edge
 *                     chatter from the contact/optocoupler. Sets when pressed()
 *                     flips true.
 *
 *   MAT_MIN_ON_MS   : the press must remain active at least this long (measured
 *                     from the first edge) before it COUNTS as one event.
 *                     Rejects short spikes that survive debounce. This is also
 *                     the event latency — set it small for snappy triggering.
 *                     MUST BE >= MAT_DEBOUNCE_MS.
 *
 *   MAT_REARM_MS    : after release, the input must read inactive continuously
 *                     for this long before the NEXT press can register. This is
 *                     the cooldown that prevents a single bouncy release from
 *                     re-triggering, and it also gates boot-while-pressed.
 */
constexpr uint16_t MAT_DEBOUNCE_MS = 10; // edge stability window
constexpr uint16_t MAT_MIN_ON_MS = 30;   // min dwell to count (>= debounce)
constexpr uint16_t MAT_REARM_MS = 80;    // min off-time before re-arming

// ===================== Media player triggers (BrightSign) =================
/*
 * One digital output per mat drives a BrightSign GPIO input. The BrightSign is
 * configured to fire its clip when the line reads LOW, so each output idles
 * HIGH and is pulsed LOW for MEDIA_PULSE_MS on a validated stomp.
 *
 * Wiring: ESP32 GPIO -> BrightSign GPIO input, plus a COMMON GROUND between the
 * two boards. ESP32 outputs are 3.3V logic; confirm the BrightSign input is
 * 3.3V-referenced before connecting.
 *
 * MEDIA_TRIGGER_PINS[i] is triggered by MAT_INPUT_PINS[i] (same index).
 */
constexpr uint8_t MEDIA_TRIGGER_PINS[] = {21, 22, 23};
static_assert(sizeof(MEDIA_TRIGGER_PINS) / sizeof(MEDIA_TRIGGER_PINS[0]) ==
                  MAT_COUNT,
              "Each mat needs exactly one media-trigger output");

constexpr bool MEDIA_TRIGGER_ACTIVE_LOW = true; // BrightSign fires on LOW
constexpr uint16_t MEDIA_PULSE_MS = 200;        // LOW pulse width per trigger

// ===================== Serial =============================================
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr bool SERIAL_LOG_EVENTS = true; // concise per-stomp log line
