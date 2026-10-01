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
constexpr uint8_t MAT_INPUT_PINS[] = {
    13, // hatchling
    14, // juvenile
    15  // adult
};
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

// ===================== Fault detection (fail-closed) ======================
/*
 * A mat "fails closed" when its contact welds shut, the mat is physically stuck
 * down, or the long wire run shorts: the optocoupler output is then held LOW
 * (active) indefinitely. No real stomp lasts anywhere near this long, so if a
 * mat's input stays CONTINUOUSLY active for MAT_STUCK_MS we flag it FAULTED —
 * the detector stops emitting stomp events for that mat and the sketch forces
 * its media output back to idle and logs the fault.
 *
 * The mat auto-recovers once its line reads inactive (HIGH) continuously for
 * MAT_FAULT_RECOVER_MS, at which point it re-arms through the normal boot-safe
 * WaitForRelease path.
 *
 * MAT_STUCK_MS must be comfortably longer than the longest legitimate stomp.
 */
constexpr uint32_t MAT_STUCK_MS = 10000;       // continuous-active -> fault
constexpr uint32_t MAT_FAULT_RECOVER_MS = 500; // continuous-clear -> recovered

// ===================== Downstream trigger outputs =========================
/*
 * Each validated stomp fires the output banks simultaneously, one output per
 * mat in each bank: media (BrightSign) and light strip. Every bank behaves
 * identically — the line idles HIGH and is pulsed LOW for OUTPUT_PULSE_MS,
 * because the downstream devices trigger on a LOW input.
 *
 * Within every bank, index i is driven by MAT_INPUT_PINS[i] (same index), so
 * mat 1 -> {MEDIA[0], LIGHT[0]}, and so on.
 *
 * Wiring: each ESP32 GPIO -> the downstream device's GPIO input, plus a COMMON
 * GROUND to every device. ESP32 outputs are 3.3V logic; confirm each input is
 * 3.3V-referenced before connecting.
 *
 * AUDIO: an audio bank was added at first to test a polyphonic audio module
 * (sparkfun + robertsonic's WAV trigger) but has since been removed
 * GPIO25/26/27 are left reserved for it — do not reuse them. To bring audio
 * back, re-add AUDIO_TRIGGER_PINS[] = {25, 26, 27} plus its static_assert and
 * a BANK_AUDIO entry in the sketch's OUTPUT_BANK_PINS table.
 */

constexpr uint8_t MEDIA_TRIGGER_PINS[] = {
    21, // hatchling
    22, // juvenile
    23  // adult
}; // BrightSign media player
constexpr uint8_t LIGHT_TRIGGER_PINS[] = {
    16, // hatchling
    17, // juvenile
    18  // adult
}; // QuinLED controller

static_assert(sizeof(MEDIA_TRIGGER_PINS) / sizeof(MEDIA_TRIGGER_PINS[0]) ==
                  MAT_COUNT,
              "media bank needs exactly one output per mat");
static_assert(sizeof(LIGHT_TRIGGER_PINS) / sizeof(LIGHT_TRIGGER_PINS[0]) ==
                  MAT_COUNT,
              "light bank needs exactly one output per mat");

constexpr bool OUTPUT_TRIGGER_ACTIVE_LOW = true; // all banks fire on LOW
constexpr uint16_t OUTPUT_PULSE_MS = 200;        // LOW pulse width per trigger

// ===================== Retrigger lockout (per-mat video lengths) ==========
/*
 * Each mat plays an animation video of a different length. On the FIRST
 * validated stomp, that mat's clip is triggered and EVERY mat input is then
 * ignored until the clip has finished, so a video always plays start-to-finish
 * without danger of it being cut off or re-triggered. The lockout is global
 * (covers all mats) and is measured from the moment of the trigger:
 *
 *     lockout = VIDEO_LENGTH_MS[i] + RETRIGGER_HEADROOM_MS
 *
 * The headroom absorbs player start-up latency and transitioning and adds a
 * short beat before the exhibit re-arms.
 *
 * NOTE: this is a different mechanism from MAT_REARM_MS above. MAT_REARM_MS is
 * a low-level debounce cooldown (min continuous off-time before ONE mat's
 * detector re-arms); RETRIGGER_HEADROOM_MS is the high-level pad added on top
 * of the video length for the GLOBAL lockout.
 *
 * VIDEO_LENGTH_MS[i] pairs with MAT_INPUT_PINS[i] (same index). Set these to
 * the real clip durations.
 */
constexpr uint32_t VIDEO_LENGTH_MS[] = {
    13000, // hatchling footprint mat clip length
    17000, // juvenile footprint mat clip length
    22000, // adult footprint mat clip length
};
static_assert(sizeof(VIDEO_LENGTH_MS) / sizeof(VIDEO_LENGTH_MS[0]) == MAT_COUNT,
              "one video length per mat");

constexpr uint32_t RETRIGGER_HEADROOM_MS =
    1000; // headroom pad added to each video length

// ===================== Watchdog ===========================================
/*
 * Hardware task watchdog (TWDT). loop() is fully non-blocking, so it feeds the
 * watchdog every pass in well under a millisecond. If anything ever wedges the
 * main task past WDT_TIMEOUT_MS (a hung peripheral call, a stray infinite
 * loop), the watchdog panics and resets the board — the exhibit self-recovers
 * instead of hanging until someone power-cycles it.
 */
constexpr uint32_t WDT_TIMEOUT_MS = 5000;

// ===================== Serial =============================================
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr bool SERIAL_LOG_EVENTS = true; // concise per-stomp log line
