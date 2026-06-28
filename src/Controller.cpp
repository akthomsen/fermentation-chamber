#include "Controller.h"

#include "Config.h"

void Controller::begin()
{
    pinMode(PIN_HEATER, OUTPUT);
    pinMode(PIN_HUMIDIFIER, OUTPUT);

    // Fan is PWM-driven via LEDC (core 3.x API). Attaching a pin auto-allocates
    // a channel; we then write an 8-bit duty (0..255) with ledcWrite().
    ledcAttach(PIN_FAN, FAN_PWM_FREQ, FAN_PWM_RES_BITS);
    digitalWrite(PIN_HEATER, HEATER_OFF);
    digitalWrite(PIN_HUMIDIFIER, HUMIDIFIER_OFF);

    kickFan(); // pulse to full so the fan reliably spins up; update() sets the real duty next tick

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
    float tempSum = 0.0f;
    int tempCount = 0;
    if (!isnan(s.dsTemp) && s.dsTemp >= TEMP_VALID_MIN && s.dsTemp <= TEMP_VALID_MAX)
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

    // Fan speed (PWM duty %). Manual override wins; otherwise AUTO ramps the
    // speed with how far the aggregate temperature sits from target. The duty
    // never drops below the floor during a run, so air keeps circulating and the
    // fan cannot stall. Manual values are already clamped to [floor, max].
    int fanPct;
    if (sp.fanManualPct >= FAN_DUTY_MIN_PCT)
    {
        fanPct = sp.fanManualPct > FAN_DUTY_MAX_PCT ? FAN_DUTY_MAX_PCT : sp.fanManualPct;
    }
    else if (!tempValid)
    {
        fanPct = FAN_DUTY_MAX_PCT; // blind -> full circulation
    }
    else
    {
        const float deltaC = fabsf(controlTemp - sp.targetTemp);
        const float frac = constrain(deltaC / FAN_RAMP_SPAN_C, 0.0f, 1.0f);
        fanPct = FAN_DUTY_MIN_PCT + (int)((FAN_DUTY_MAX_PCT - FAN_DUTY_MIN_PCT) * frac + 0.5f);
    }
    state_.fanOn = true;
    state_.fanDuty = (uint8_t)fanPct;
    ledcWrite(PIN_FAN, (fanPct * 255) / 100); // 8-bit duty

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
