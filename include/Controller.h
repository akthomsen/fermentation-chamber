#pragma once

#include <Arduino.h>
#include "Sensors.h"

// User-adjustable targets that drive the control logic. A plain snapshot value
// type so it can be passed safely from the ISR-owned menu state into update().
struct Setpoints
{
    float targetTemp = 0.0f;     // degrees C
    float targetHumidity = 0.0f; // percent
    float targetCeiling = 0.0f;  // hard over-temp cutoff, degrees C
    long runMinutes = 0;         // shut everything off after this many minutes (0 = no limit)
    int fanManualPct = 0;        // <FAN_DUTY_MIN_PCT = AUTO (temp-proportional); else manual duty %
};

// Resolved on/off state of every actuator, plus the reasons behind it. Read by
// the display; written only by Controller::update().
struct ActuatorState
{
    bool fanOn = false;
    uint8_t fanDuty = 0; // live fan speed, percent (0..100)
    bool heaterOn = false;
    bool humidOn = false;
    bool heaterFault = false;   // sensor invalid / over ceiling -> heater forced off
    bool heaterLockout = false; // tripped max-on time, in forced cooldown
    bool runComplete = false;   // run duration elapsed -> everything off
    bool stopped = false;       // manually stopped by the user -> everything off
    bool notStarted = true;     // powered on but never started this session -> everything off

    // True when the chamber is halted for any reason and needs a start/restart.
    bool halted() const { return runComplete || stopped || notStarted; }
};

// Drives the fan, heater and humidifier from the current readings and setpoints.
// Encapsulates the hysteresis, over-temperature safety and run-timer behaviour.
class Controller
{
public:
    // Configure output pins and drive them to a safe initial state.
    void begin();

    // Recompute and apply every actuator output. Call periodically.
    void update(const Setpoints &sp, const SensorReadings &s);

    // Stop the chamber now: shut every actuator off and stay off until restart().
    void stop();

    // Restart the run: clear the halted latch (finished or stopped) and reset
    // the run timer to zero so a new run can begin without a power cycle.
    void restart();

    const ActuatorState &state() const { return state_; }

    // millis() timestamp the current run started at (for elapsed/left display).
    unsigned long runStartMs() const { return runStartMs_; }

private:
    // Brief full-speed pulse to overcome fan stiction at the start of a run.
    // Blocks for FAN_KICK_MS, so only call it from one-shot paths (begin/restart).
    void kickFan();

    ActuatorState state_;

    // Run-timer baseline: elapsed time is measured from here, not from boot.
    unsigned long runStartMs_ = 0;

    // Heater run-time tracking for the max-on backstop.
    bool heaterWasOn_ = false;
    unsigned long heaterOnSince_ = 0;
    unsigned long lockoutSince_ = 0;
};
