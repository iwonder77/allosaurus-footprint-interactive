#include "MatInput.h"
#include "Config.h"

void MatInput::begin(uint8_t pin) {
  pin_ = pin;
  pinMode(pin_, MAT_ENABLE_INTERNAL_PULLUP ? INPUT_PULLUP : INPUT);
  // Always start needing a clean release: this covers both the cooldown
  // contract and the "powered on while someone is standing on the mat" case.
  state_ = State::WaitForRelease;
  pressed_ = false;
  event_ = false;
  t_mark_ = millis();
  active_since_ = t_mark_;
}

bool MatInput::rawActive() const {
  const int level = digitalRead(pin_);
  return MAT_ACTIVE_LOW ? (level == LOW) : (level == HIGH);
}

void MatInput::update() {
  const uint32_t now = millis();
  const bool active = rawActive();

  // Track how long the line has been continuously active, independent of the
  // press FSM: every inactive read pushes the "active since" mark forward, so
  // while active (now - active_since_) is the uninterrupted active duration.
  if (!active) {
    active_since_ = now;
  }

  // Fault handling takes priority over the normal press logic.
  if (state_ == State::Faulted) {
    // Recover only after the line reads inactive continuously for the recovery
    // window; any activity restarts that timer. On recovery, fall back through
    // the boot-safe path so a lingering press still can't count.
    if (active) {
      t_mark_ = now;
    } else if (now - t_mark_ >= MAT_FAULT_RECOVER_MS) {
      state_ = State::WaitForRelease;
      t_mark_ = now;
    }
    return;
  }

  // Enter the fault state if the line has been active far longer than any real
  // stomp. Drop any in-flight press/event so a stuck mat goes fully quiet.
  if (active && (now - active_since_ >= MAT_STUCK_MS)) {
    pressed_ = false;
    event_ = false;
    t_mark_ = now;
    state_ = State::Faulted;
    return;
  }

  switch (state_) {
  case State::WaitForRelease:
    // Require CONTINUOUS inactivity for the re-arm window. Any blip of
    // activity restarts the timer, so a chattery release can't sneak through.
    if (active) {
      t_mark_ = now; // still/again active -> restart
    } else if (now - t_mark_ >= MAT_REARM_MS) {
      state_ = State::Armed;
    }
    break;

  case State::Armed:
    if (active) { // first active edge of a press
      t_mark_ = now;
      state_ = State::PressDebounce;
    }
    break;

  case State::PressDebounce:
    if (!active) {
      state_ = State::Armed; // dropped during debounce -> chatter
    } else if (now - t_mark_ >= MAT_DEBOUNCE_MS) {
      pressed_ = true; // believed pressed (debounced level)
      state_ = State::PressConfirm;
    }
    break;

  case State::PressConfirm:
    // t_mark_ is still the first-edge time, so min-on is measured from the
    // start of the press (hence MAT_MIN_ON_MS must be >= MAT_DEBOUNCE_MS).
    if (!active) {
      pressed_ = false; // released before min-on -> too short
      t_mark_ = now;
      state_ = State::WaitForRelease;
    } else if (now - t_mark_ >= MAT_MIN_ON_MS) {
      event_ = true; // VALIDATED PRESS — latch the event
      state_ = State::Held;
    }
    break;

  case State::Held:
    if (!active) { // press finished
      pressed_ = false;
      t_mark_ = now; // begin cooldown
      state_ = State::WaitForRelease;
    }
    break;

  case State::Faulted:
    // Handled above (before the switch); listed so the switch stays exhaustive.
    break;
  }
}

bool MatInput::eventFired() {
  if (event_) {
    event_ = false;
    return true;
  }
  return false;
}
