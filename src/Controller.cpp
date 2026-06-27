#include "Controller.h"

#include "Config.h"

void Controller::begin()
{
    pinMode(PIN_FAN, OUTPUT);
    pinMode(PIN_HEATER, OUTPUT);
    pinMode(PIN_HUMIDIFIER, OUTPUT);

    digitalWrite(PIN_FAN, FAN_ON); // fan always on while running
    digitalWrite(PIN_HEATER, HEATER_OFF);
    digitalWrite(PIN_HUMIDIFIER, HUMIDIFIER_OFF);

    runStartMs_ = millis();
}

void Controller::stop()
{
    state_.stopped = true; // update() drives every output off on the next call

    // Drive outputs off immediately so a stop takes effect without waiting.
    state_.fanOn = state_.heaterOn = state_.humidOn = false;
    digitalWrite(PIN_FAN, FAN_OFF);
    digitalWrite(PIN_HEATER, HEATER_OFF);
    digitalWrite(PIN_HUMIDIFIER, HUMIDIFIER_OFF);
}

void Controller::restart()
{
    state_ = ActuatorState{};       // clear halted latch and all actuator state
    runStartMs_ = millis();         // run timer starts over from now
    heaterWasOn_ = false;
    heaterOnSince_ = 0;
    lockoutSince_ = 0;
}

void Controller::update(const Setpoints &sp, const SensorReadings &s)
{
    const unsigned long now = millis();
    const unsigned long elapsedMs = now - runStartMs_;

    // Run-duration limit: once the configured time has elapsed, shut everything
    // off and stay off until restart() is called (e.g. from the Run Time menu).
    if (sp.runMinutes > 0 && elapsedMs >= (unsigned long)sp.runMinutes * 60000UL)
        state_.runComplete = true;

    if (state_.halted())
    {
        state_.fanOn = state_.heaterOn = state_.humidOn = false;
        digitalWrite(PIN_FAN, FAN_OFF);
        digitalWrite(PIN_HEATER, HEATER_OFF);
        digitalWrite(PIN_HUMIDIFIER, HUMIDIFIER_OFF);
        return;
    }

    // Fan is always on during the run.
    state_.fanOn = true;
    digitalWrite(PIN_FAN, FAN_ON);

    // Expire the cooldown lockout once enough time has passed.
    if (state_.heaterLockout && now - lockoutSince_ > HEAT_COOLDOWN_MS)
        state_.heaterLockout = false;

    // Normal hysteresis, but shut off slightly BELOW target so residual heat
    // carries the temperature the rest of the way up.
    if (s.dsTemp <= sp.targetTemp - HEATER_OFFSET - HYSTERESIS)
        state_.heaterOn = true;
    else if (s.dsTemp >= sp.targetTemp - HEATER_OFFSET)
        state_.heaterOn = false;

    // Safety: bad sensor reading or above the hard ceiling -> force off.
    state_.heaterFault = (s.dsTemp < TEMP_VALID_MIN) || (s.dsTemp > TEMP_VALID_MAX) ||
                         (s.dsTemp >= sp.targetCeiling);
    if (state_.heaterFault || state_.heaterLockout)
        state_.heaterOn = false;

    // Max continuous-on backstop: if the heater has run too long without the
    // temperature being satisfied, force a cooldown (catches stuck/sluggish states).
    if (state_.heaterOn)
    {
        if (!heaterWasOn_)
            heaterOnSince_ = now; // just turned on
        else if (now - heaterOnSince_ > MAX_HEAT_MS)
        {
            state_.heaterOn = false;
            state_.heaterLockout = true;
            lockoutSince_ = now;
        }
    }
    heaterWasOn_ = state_.heaterOn;

    digitalWrite(PIN_HEATER, state_.heaterOn ? HEATER_ON : HEATER_OFF);

    // Humidifier: add moisture if too dry, stop once above target.
    if (s.humidity < sp.targetHumidity - HYSTERESIS)
        state_.humidOn = true;
    else if (s.humidity > sp.targetHumidity + HYSTERESIS)
        state_.humidOn = false;
    digitalWrite(PIN_HUMIDIFIER, state_.humidOn ? HUMIDIFIER_ON : HUMIDIFIER_OFF);
}
