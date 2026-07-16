#pragma once
#include <Arduino.h>

/*
 * MatInput — non-blocking, debounced press detector for a single active-low
 * optocoupler input (H11L1M open-collector output with an external pull-up:
 * idle = HIGH, mat stomped = LOW).
 *
 * The class owns nothing but its debounce state machine. It answers one
 * question — "did a clean, validated stomp just happen?" — with no dependency
 * on Serial, the media outputs, or any other module. Instantiate one per mat
 * and give each its own GPIO via begin().
 *
 * USAGE:
 *   MatInput mat;
 *   void setup() { mat.begin(MAT_INPUT_PINS[0]); }
 *   void loop()  {
 *     mat.update();                 // call as often as possible; never blocks
 *     if (mat.eventFired()) { ... } // fires exactly once per validated stomp
 *   }
 *
 * Timing is tuned via the three knobs in Config.h (debounce window, minimum
 * on-time, re-arm/cooldown) — each rejects a different failure mode.
 *
 * BOOT SAFETY: begin() starts in WaitForRelease, so a mat already held down at
 * power-on never produces a spurious event; the detector arms only after a
 * confirmed clean release.
 */
class MatInput {
public:
  // Bind this detector to a GPIO and reset its state machine. Call once in
  // setup(). Polarity and pull-up behaviour come from Config.h (shared across
  // all mats, which are identical hardware).
  void begin(uint8_t pin);

  // Advance the state machine. Non-blocking; uses millis(). Call every loop.
  void update();

  // True exactly once per validated stomp. Reading it clears the latch, so the
  // caller consumes each event exactly once (poll it every loop).
  bool eventFired();

  // Debounced level: true while a stomp is held (after the debounce window).
  bool pressed() const { return pressed_; }

  // Instantaneous, polarity-adjusted raw read. Handy for a startup self-test or
  // diagnostics before the state machine has armed.
  bool rawActive() const;

private:
  enum class State : uint8_t {
    WaitForRelease, // boot-safe entry + post-press cooldown; needs clean release
    Armed,          // idle, ready to detect a press
    PressDebounce,  // active edge seen; validating it is stable
    PressConfirm,   // debounced-pressed; waiting to reach minimum on-time
    Held            // event fired; waiting for release
  };

  uint8_t pin_ = 0;
  State state_ = State::WaitForRelease;
  bool pressed_ = false;
  bool event_ = false;
  uint32_t t_mark_ = 0; // ms timestamp of the last significant transition
};
