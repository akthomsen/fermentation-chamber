#include "Controller.h"

#include "Config.h"

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
    state_ = ActuatorState{};       // clear halted latch and all actuator state
    state_.notStarted = false;      // ActuatorState defaults notStarted=true; the run is now live
    runStartMs_ = millis();         // run timer starts over from now
    heaterWasOn_ = false;
    heaterOnSince_ = 0;
    lockoutSince_ = 0;

    kickFan(); // spin the fan back up cleanly after a halt
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
        state_.fanDuty = 0;
        ledcWrite(PIN_FAN, 0);
        digitalWrite(PIN_HEATER, HEATER_OFF);
        digitalWrite(PIN_HUMIDIFIER, HUMIDIFIER_OFF);
        return;
    }

    // Expire the cooldown lockout once enough time has passed.
    if (state_.heaterLockout && now - lockoutSince_ > HEAT_COOLDOWN_MS)
        state_.heaterLockout = false;

    // Control on the average of the two temperature sensors. A sensor is
    // ignored if it reads NaN or sits outside the valid band (a DS18B20 returns
    // -127 when unplugged), so a single failed sensor neither drags the average
    // off nor masks the other one. With no usable sensor the heater is faulted.
    const bool dsValid = !isnan(s.dsTemp) && s.dsTemp >= TEMP_VALID_MIN && s.dsTemp <= TEMP_VALID_MAX;
    float tempSum = 0.0f;
    int tempCount = 0;
    if (dsValid)
    {
        tempSum += s.dsTemp;
        ++tempCount;
    }
    if (!isnan(s.bmeTemp) && s.bmeTemp >= TEMP_VALID_MIN && s.bmeTemp <= TEMP_VALID_MAX)
    {
        tempSum += s.bmeTemp;
        ++tempCount;
    }
    const bool tempValid = tempCount > 0;
    const float controlTemp = tempValid ? tempSum / tempCount : 0.0f;

    // Normal hysteresis, but shut off slightly BELOW target so residual heat
    // carries the temperature the rest of the way up.
    if (tempValid && controlTemp <= sp.targetTemp - HEATER_OFFSET - HYSTERESIS)
        state_.heaterOn = true;
    else if (controlTemp >= sp.targetTemp - HEATER_OFFSET)
        state_.heaterOn = false;

    // Safety: no usable sensor or above the hard ceiling -> force off.
    state_.heaterFault = !tempValid || (controlTemp >= sp.targetCeiling);
    if (state_.heaterFault || state_.heaterLockout)
        state_.heaterOn = false;

    // DS probe guard: never heat while the DS sensor reads at/above target, even
    // if a cooler BME drags the average below target and would otherwise call for
    // heat. The DS probe alone can veto heating, never demand it.
    if (dsValid && s.dsTemp >= sp.targetTemp)
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

    // Humidifier: add moisture if too dry, stop once above target. Resolved
    // before the fan so the fan can react to the humidifier's final state.
    if (s.humidity < sp.targetHumidity - HYSTERESIS)
        state_.humidOn = true;
    else if (s.humidity > sp.targetHumidity + HYSTERESIS)
        state_.humidOn = false;
    digitalWrite(PIN_HUMIDIFIER, state_.humidOn ? HUMIDIFIER_ON : HUMIDIFIER_OFF);

    // Fan speed (PWM duty %). The fan now lives INSIDE the chamber, so it only
    // recirculates air -- it cannot cool. Its job is to keep the temperature and
    // humidity uniform and to distribute heat/moisture from the other actuators.
    // In AUTO it therefore runs continuously for the whole run, with the speed
    // set by what is being conditioned (priority high to low):
    //   heater on    -> full speed, distribute heat so it never pools at the element
    //   humidifier on -> 70%, spread moisture (heater wins if both are on)
    //   otherwise     -> circulation floor, just keep the air mixed
    // Manual override wins over all of it (any 0..100%, so manual can also stop
    // the fan); with no usable sensor AUTO runs full as a failsafe.
    int fanPct;
    if (sp.fanManualPct >= 0)
        fanPct = sp.fanManualPct > FAN_DUTY_MAX_PCT ? FAN_DUTY_MAX_PCT : sp.fanManualPct;
    else if (!tempValid)
        fanPct = FAN_DUTY_MAX_PCT; // blind -> full circulation
    else if (state_.heaterOn)
        fanPct = FAN_DUTY_MAX_PCT; // heater wins
    else if (state_.humidOn)
        fanPct = FAN_DUTY_HUMID_PCT;
    else
        fanPct = FAN_DUTY_MIN_PCT; // always-on circulation floor

    state_.fanOn = fanPct > 0;
    state_.fanDuty = (uint8_t)fanPct;
    ledcWrite(PIN_FAN, (fanPct * 255) / 100); // 8-bit duty
}
