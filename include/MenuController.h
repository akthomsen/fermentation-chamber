#pragma once

#include <Arduino.h>
#include "Controller.h" // for Setpoints

// Screens the encoder scrolls through. The first three are read-only views;
// the SET_* screens are editable setpoints (push the knob to toggle editing).
enum MenuScreen : int
{
    SCREEN_OVERVIEW = 0,
    SCREEN_SENSORS,
    SCREEN_ACTUATORS,
    SCREEN_SET_TEMP,
    SCREEN_SET_HUMID,
    SCREEN_SET_CEIL,
    SCREEN_SET_FAN,
    SCREEN_SET_RUN,
    SCREEN_COUNT
};

// Rotary-encoder + push-button user interface. Navigates between screens and
// edits the setpoints.
//
// The encoder is read on interrupts (it is timing-critical); the button is
// polled from poll() so we can tell a short press (toggle/restart) from a long
// hold (stop). State shared with the encoder ISR is volatile.
//
// Because attachInterrupt() needs plain function pointers, the encoder installs
// a static trampoline that forwards to a single active instance. Only one
// MenuController may exist at a time.
class MenuController
{
public:
    MenuController();

    // Configure pins and attach the encoder interrupts.
    void begin();

    // Read the button and act on short/long presses. Call every loop iteration.
    void poll();

    // Current navigation state.
    int screen() const { return menuIndex_; }
    bool isEditing() const { return editing_; }
    const char *screenLabel() const;

    // Snapshot the setpoints into a plain value type, safe to hand to the
    // Controller without worrying about concurrent ISR updates.
    Setpoints setpoints() const;

    // Apply a setpoint from outside the encoder (e.g. a remote MQTT command).
    // Single aligned 32-bit stores are atomic on the C6, so these are safe to
    // call from loop() context even though the encoder ISR also writes the
    // same fields -- no critical section required.
    void setTargetTemp(float c) { targetTemp_ = c; }
    void setTargetHumidity(float p) { targetHumidity_ = p; }
    void setTargetCeiling(float c) { targetCeiling_ = c; }
    void setFanManualPct(int pct) { fanManualPct_ = pct; }
    void setRunMinutes(long minutes) { runMinutes_ = minutes; }

    // Returns true (and clears the flag) when the display needs redrawing.
    bool consumeRedraw();

    // Force a redraw on the next consumeRedraw(), e.g. after new sensor data.
    void requestRedraw() { menuChanged_ = true; }

    // Tell the menu whether the chamber is halted (run finished or stopped), so
    // the Run Time screen offers a restart instead of edit. Call each loop.
    void setHalted(bool halted) { halted_ = halted; }

    // Returns true (and clears it) when the user asked to restart the run.
    bool consumeRestart();

    // Returns true (and clears it) when the user held the button to stop.
    bool consumeStop();

private:
    void onEncoder(); // called from the encoder ISR
    void onShortPress();
    bool isEditableScreen(int screen) const;

    static void IRAM_ATTR encoderTrampoline();
    static MenuController *instance_;

    // --- Navigation / UI state ---
    volatile int menuIndex_ = SCREEN_OVERVIEW;
    volatile bool menuChanged_ = true; // redraw display
    volatile bool editing_ = false;    // editing a setpoint with the knob?
    bool halted_ = false;              // mirror of Controller::state().halted()
    bool restartRequested_ = false;    // short-pressed restart on the run screen
    bool stopRequested_ = false;       // held the button to stop

    // --- Setpoints (changed with the encoder) ---
    volatile float targetTemp_;
    volatile float targetHumidity_;
    volatile float targetCeiling_;
    volatile int fanManualPct_; // FAN_MANUAL_AUTO (-1) = AUTO; 0..100 = manual duty %
    volatile long runMinutes_;

    // --- Encoder decode state ---
    volatile unsigned long lastEncoderTime_ = 0;
    volatile uint8_t encState_ = 0;
    volatile int8_t encAccum_ = 0;

    // --- Button poll state ---
    bool btnDown_ = false;            // debounced "currently pressed"
    bool longFired_ = false;          // stop already triggered for this hold
    unsigned long btnDownSince_ = 0;  // when the current press began
};
