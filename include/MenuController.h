#pragma once

#include <Arduino.h>
#include "Controller.h" // for Setpoints / ControlSensor

// Two-level encoder menu.
//
//   TOP (turn to move, push to enter)        GROUPS (push an item to open it)
//     Overview                                 States   : Sensors, Actuators
//     States    >                              Settings : Temp, Humidity, Max
//     Settings  >                                         Temp, DS Max, Control
//     Actuators >                              Actuators: Heater, Humidifier, Fan
//     Run Time
//     Network
//
// Groups open into a scrollable item list; each list ends with a "< Back" item
// that returns to the top. Editable items (settings) and actuator controls open
// full-screen; push again closes them back to the list. The 3 s button hold is a
// global STOP from anywhere, unchanged.
//
// The encoder is read on interrupts (timing critical); the button is polled so a
// short press and a long hold can be told apart. State shared with the ISR is
// volatile. attachInterrupt() needs a plain function, so a static trampoline
// forwards to the single active instance.

// Top-level pages. STATES / SETTINGS / ACTUATORS are groups you push to open.
enum TopPage : uint8_t
{
    TOP_OVERVIEW = 0,
    TOP_STATES,
    TOP_SETTINGS,
    TOP_ACTUATORS,
    TOP_RUNTIME,
    TOP_NETWORK,
    TOP_COUNT
};

// What an item inside a group does when opened.
enum ItemKind : uint8_t
{
    IK_BACK = 0,
    IK_VIEW_SENSORS,
    IK_VIEW_ACT,
    IK_EDIT_TEMP,
    IK_EDIT_HUMID,
    IK_EDIT_MAXTEMP,
    IK_EDIT_DSMAX,
    IK_EDIT_CONTROL,
    IK_CTRL_HEATER,
    IK_CTRL_HUMID,
    IK_CTRL_FAN,
};

class MenuController
{
public:
    MenuController();

    // Configure pins and attach the encoder interrupts.
    void begin();

    // Read the button and act on short/long presses. Call every loop iteration.
    void poll();

    // ---- Navigation state, read by Display ----
    TopPage topPage() const { return (TopPage)topSel_; }
    bool entered() const { return entered_; }
    bool isEditing() const { return editing_; }
    uint8_t itemIndex() const { return itemSel_; }
    uint8_t groupItemCount() const;      // items in the current group (incl. Back)
    ItemKind groupItem(uint8_t i) const; // kind of item i in the current group
    ItemKind currentItem() const;        // kind of the highlighted item
    const char *title() const;           // header text for the current view

    // Snapshot of the setpoints, safe to hand to the Controller (ISR may update
    // the underlying fields concurrently; aligned 32-bit stores are atomic here).
    Setpoints setpoints() const;

    // Current actuator-override intents (-1 = off, 0 = auto, 1 = on). main is the
    // single applier: it reconciles these onto the Controller each loop.
    int8_t heaterOverride() const { return heaterOverride_; }
    int8_t humidOverride() const { return humidOverride_; }

    // ---- External (e.g. MQTT) setters. The menu is the single source of truth
    //      for every user intent, so remote commands flow through here too. ----
    void setTargetTemp(float c) { targetTemp_ = c; }
    void setTargetHumidity(float p) { targetHumidity_ = p; }
    void setTargetCeiling(float c) { targetCeiling_ = c; }
    void setDsMaxOverTarget(float c) { dsMaxOverTarget_ = c; }
    void setControlSensor(ControlSensor s) { controlSensor_ = s < CONTROL_SENSOR_COUNT ? s : CONTROL_SENSOR_DS; }
    void setFanManualPct(int pct) { fanManualPct_ = pct; }
    void setRunMinutes(long m) { runMinutes_ = m; }
    void setHeaterOverride(int8_t v) { heaterOverride_ = clampOverride(v); }
    void setHumidOverride(int8_t v) { humidOverride_ = clampOverride(v); }

    // ---- Redraw / events ----
    bool consumeRedraw();
    void requestRedraw() { menuChanged_ = true; }

    // Whether the chamber is halted (finished/stopped/not started): the Run Time
    // page offers a restart instead of edit. Call each loop.
    void setHalted(bool h) { halted_ = h; }

    // Live run elapsed-minutes (now - runStart): lets the Run Time knob edit time
    // REMAINING while a run is active instead of the raw total.
    void setRunElapsed(long m) { runElapsedMin_ = m < 0 ? 0 : m; }

    bool consumeRestart();        // user asked to (re)start the run
    bool consumeStop();           // user held the button to stop
    bool consumeNetworkAction();  // user pushed on the Network page

private:
    void onEncoder(); // called from the encoder ISR
    void onShortPress();
    void applyEdit(int dir, int mult);          // adjust the open item's value
    const ItemKind *groupItems(uint8_t &count) const;
    static int8_t clampOverride(int8_t v) { return v < -1 ? -1 : (v > 1 ? 1 : v); }

    static void IRAM_ATTR encoderTrampoline();
    static MenuController *instance_;

    // --- Navigation ---
    volatile uint8_t topSel_ = TOP_OVERVIEW; // current top page
    volatile bool entered_ = false;          // inside a group
    volatile uint8_t itemSel_ = 0;           // highlighted item in the group
    volatile bool editing_ = false;          // an item editor / view is open
    volatile bool menuChanged_ = true;       // redraw needed
    volatile bool activityPending_ = false;  // encoder/button used since last loop

    bool halted_ = false;
    volatile long runElapsedMin_ = 0;
    bool restartRequested_ = false;
    bool stopRequested_ = false;
    bool networkActionRequested_ = false;

    // --- Setpoints / intents (changed with the encoder or remotely) ---
    volatile float targetTemp_;
    volatile float targetHumidity_;
    volatile float targetCeiling_;
    volatile float dsMaxOverTarget_;
    volatile ControlSensor controlSensor_;
    volatile int fanManualPct_; // FAN_MANUAL_AUTO (-1) = AUTO; 0..100 = manual duty %
    volatile long runMinutes_;
    volatile int8_t heaterOverride_ = 0; // -1 off, 0 auto, 1 on
    volatile int8_t humidOverride_ = 0;  // -1 off, 0 auto, 1 on

    // --- Encoder decode state ---
    volatile unsigned long lastEncoderTime_ = 0;
    volatile uint8_t encState_ = 0;
    volatile int8_t encAccum_ = 0;

    // --- Button poll state ---
    bool btnDown_ = false;
    bool longFired_ = false;
    unsigned long btnDownSince_ = 0;
};
