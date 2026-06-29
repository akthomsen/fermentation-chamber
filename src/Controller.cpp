#include "Controller.h"

#include "Config.h"

namespace
{
    bool validTemp(float t)
    {
        return !isnan(t) && t >= TEMP_VALID_MIN && t <= TEMP_VALID_MAX;
    }

    bool resolveControlTemp(ControlSensor sensor, const SensorReadings &s, float &out)
    {
        const bool dsValid = validTemp(s.dsTemp);
        const bool bmeValid = validTemp(s.bmeTemp);

        switch (sensor)
        {
        case CONTROL_SENSOR_DS:
            if (!dsValid)
                return false;
            out = s.dsTemp;
            return true;
        case CONTROL_SENSOR_BME:
            if (!bmeValid)
                return false;
            out = s.bmeTemp;
            return true;
        case CONTROL_SENSOR_AVERAGE:
            if (dsValid && bmeValid)
            {
                out = (s.dsTemp + s.bmeTemp) * 0.5f;
                return true;
            }
            if (dsValid)
            {
                out = s.dsTemp;
                return true;
            }
            if (bmeValid)
            {
                out = s.bmeTemp;
                return true;
            }
            return false;
        default:
            return false;
        }
    }
} // namespace

void Controller::begin()
{
    pinMode(PIN_HEATER, OUTPUT);
    pinMode(PIN_HUMIDIFIER, OUTPUT);

    // Fan is PWM-driven via LEDC (core 3.x API). Attaching a pin auto-allocates
    // a channel; we then write an 8-bit duty (0..255) with ledcWrite().
    ledcAttach(PIN_FAN, FAN_PWM_FREQ, FAN_PWM_RES_BITS);
    ledcWrite(PIN_FAN, 0); // fan off until the run is started
    digitalWrite(PIN_HEATER, HEATER_OFF);
    digitalWrite(PIN_HUMIDIFIER, HUMIDIFIER_OFF);

    // Boot idle: nothing runs on power-up. The user starts the run from the Run
    // Time screen (which calls restart() and kicks the fan). This avoids the
    // chamber heating/circulating the instant it gets power. notStarted defaults
    // true, but set it explicitly so begin() always leaves a known idle state.
    state_.notStarted = true;
    runStartMs_ = millis();
}

void Controller::kickFan()
{
    ledcWrite(PIN_FAN, 255);
    delay(FAN_KICK_MS);
}

void Controller::stop()
{
    state_.stopped = true; // update() drives every output off on the next call

    // Drive outputs off immediately so a stop takes effect without waiting.
    state_.fanOn = state_.heaterOn = state_.humidOn = false;
    state_.fanDuty = 0;
    ledcWrite(PIN_FAN, 0);
    digitalWrite(PIN_HEATER, HEATER_OFF);
    digitalWrite(PIN_HUMIDIFIER, HUMIDIFIER_OFF);
}

void Controller::restart()
{
    state_ = ActuatorState{};  // clear halted latch and all actuator state
    state_.notStarted = false; // ActuatorState defaults notStarted=true; the run is now live
    runStartMs_ = millis();    // run timer starts over from now
    humidOverride_ = 0;
    state_.humidOverride = humidOverride_;
    heaterOverride_ = 0;
    state_.heaterOverride = heaterOverride_;
    heaterWasOn_ = false;
    heaterOnSince_ = 0;
    lockoutSince_ = 0;
    fanFullUntilMs_ = 0;

    kickFan(); // spin the fan back up cleanly after a halt
}

void Controller::setHumidifierOverride(bool on)
{
    humidOverride_ = on ? 1 : -1;
    state_.humidOverride = humidOverride_;

    if (!on)
    {
        state_.humidOn = false;
        digitalWrite(PIN_HUMIDIFIER, HUMIDIFIER_OFF);
    }
}

void Controller::clearHumidifierOverride()
{
    humidOverride_ = 0;
    state_.humidOverride = humidOverride_;
}

void Controller::setHeaterOverride(bool on)
{
    heaterOverride_ = on ? 1 : -1;
    state_.heaterOverride = heaterOverride_;

    if (!on)
    {
        state_.heaterOn = false;
        heaterWasOn_ = false;
        fanFullUntilMs_ = millis() + FAN_AFTER_HEAT_MS;
        digitalWrite(PIN_HEATER, HEATER_OFF);
    }
}

void Controller::clearHeaterOverride()
{
    heaterOverride_ = 0;
    state_.heaterOverride = heaterOverride_;
}

void Controller::setFanSpeed(int pct)
{
    if (pct < FAN_MANUAL_AUTO)
        pct = FAN_MANUAL_AUTO;
    else if (pct > FAN_DUTY_MAX_PCT)
        pct = FAN_DUTY_MAX_PCT;

    fanManualPct_ = pct;
    if (pct >= 0)
    {
        state_.fanOn = pct > 0;
        state_.fanDuty = (uint8_t)pct;
        ledcWrite(PIN_FAN, (pct * 255) / 100);
    }
}

void Controller::setRunLimit(long minutes)
{
    runLimitMinutes_ = minutes < 0 ? 0 : minutes;
}

void Controller::update(const Setpoints &sp, const SensorReadings &s)
{
    const unsigned long now = millis();
    const unsigned long elapsedMs = now - runStartMs_;

    state_.humidOverride = humidOverride_;
    state_.heaterOverride = heaterOverride_;

    // Run-duration limit: once the configured time has elapsed, shut everything
    // off and stay off until restart() is called (e.g. from the Run Time menu).
    if (sp.runMinutes > 0 && elapsedMs >= (unsigned long)sp.runMinutes * 60000UL)
        state_.runComplete = true;

    if (state_.halted())
    {
        state_.fanOn = state_.heaterOn = state_.humidOn = false;
        state_.fanDuty = 0;
        ledcWrite(PIN_FAN, 0);
        digitalWrite(PIN_HEATER, HEATER_OFF);
        digitalWrite(PIN_HUMIDIFIER, HUMIDIFIER_OFF);
        return;
    }

    // Expire the cooldown lockout once enough time has passed.
    if (state_.heaterLockout && now - lockoutSince_ > HEAT_COOLDOWN_MS)
        state_.heaterLockout = false;

    state_.controlSensor = sp.controlSensor < CONTROL_SENSOR_COUNT ? sp.controlSensor : CONTROL_SENSOR_DS;

    float controlTemp = NAN;
    const bool tempValid = resolveControlTemp(state_.controlSensor, s, controlTemp);
    state_.controlTemp = tempValid ? controlTemp : NAN;
    const bool dsValid = validTemp(s.dsTemp);

    // Hysteresis band: turn ON once we fall HYSTERESIS below target, and don't
    // turn OFF again until we reach target. The two thresholds must differ or the
    // dead-band collapses to zero width and the heater short-cycles (chatters).
    // Shutting off at target lets residual heat carry the rest of the way up.
    if (tempValid && controlTemp <= sp.targetTemp - HYSTERESIS)
        state_.heaterOn = true;
    else if (controlTemp >= sp.targetTemp)
        state_.heaterOn = false;

    if (heaterOverride_ < 0)
        state_.heaterOn = false;
    else if (heaterOverride_ > 0)
        state_.heaterOn = true;

    // Safety: selected sensor invalid, selected control source above the hard
    // ceiling, or the DS probe above its target-relative guard -> force off.
    const float dsLimit = sp.targetTemp + (sp.dsMaxOverTarget < 0.0f ? 0.0f : sp.dsMaxOverTarget);
    const bool dsOverLimit = dsValid && s.dsTemp >= dsLimit;
    state_.heaterFault = !tempValid || (tempValid && controlTemp >= sp.targetCeiling) || dsOverLimit;
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
    if (heaterWasOn_ && !state_.heaterOn)
        fanFullUntilMs_ = now + FAN_AFTER_HEAT_MS;
    heaterWasOn_ = state_.heaterOn;

    digitalWrite(PIN_HEATER, state_.heaterOn ? HEATER_ON : HEATER_OFF);

    // Humidifier: add moisture if too dry, stop once above target. Resolved
    // before the fan so the fan can react to the humidifier's final state.
    if (humidOverride_ < 0)
        state_.humidOn = false;
    else if (humidOverride_ > 0)
        state_.humidOn = true;
    else if (s.humidity < sp.targetHumidity - HYSTERESIS)
        state_.humidOn = true;
    else if (s.humidity > sp.targetHumidity + HYSTERESIS)
        state_.humidOn = false;
    digitalWrite(PIN_HUMIDIFIER, state_.humidOn ? HUMIDIFIER_ON : HUMIDIFIER_OFF);

    // Fan speed (PWM duty %). The fan now lives INSIDE the chamber, so it only
    // recirculates air -- it cannot cool. Its job is to keep the temperature and
    // humidity uniform and to distribute heat/moisture from the other actuators.
    // In AUTO it therefore runs continuously for the whole run, with the speed
    // set by what is being conditioned (priority high to low):
    //   heater on/recently off -> full speed, distribute heat so it never pools
    //   no usable control temp -> full speed as a failsafe
    //   manual fan setting     -> requested 0..100%
    //   humidifier on          -> 70%, spread moisture
    //   otherwise              -> circulation floor, just keep the air mixed
    int fanPct;
    if (state_.heaterOn || (long)(fanFullUntilMs_ - now) > 0)
        fanPct = FAN_DUTY_MAX_PCT; // heater wins
    else if (!tempValid)
        fanPct = FAN_DUTY_MAX_PCT; // blind -> full circulation
    else if (sp.fanManualPct >= 0)
        fanPct = sp.fanManualPct > FAN_DUTY_MAX_PCT ? FAN_DUTY_MAX_PCT : sp.fanManualPct;
    else if (state_.humidOn)
        fanPct = FAN_DUTY_HUMID_PCT;
    else
        fanPct = FAN_DUTY_MIN_PCT; // always-on circulation floor

    state_.fanOn = fanPct > 0;
    state_.fanDuty = (uint8_t)fanPct;
    ledcWrite(PIN_FAN, (fanPct * 255) / 100); // 8-bit duty
}
