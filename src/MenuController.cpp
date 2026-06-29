#include "MenuController.h"

#include "Config.h"

// Labels shown in the screen title bar, indexed by MenuScreen.
static const char *const kMenuLabels[SCREEN_COUNT] = {
    "Overview", "Sensors", "Actuators", "Set Temp", "Set Humid", "Max Temp", "DS Max", "Control", "Set Fan", "Run Time"};

// Quadrature decode table: maps a 4-bit (prev<<2 | now) state to a step.
// Invalid transitions (contact bounce) decode to 0, so noise is ignored.
static const int8_t kQuadDecode[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

MenuController *MenuController::instance_ = nullptr;

MenuController::MenuController()
    : targetTemp_(DEFAULT_TARGET_TEMP),
      targetHumidity_(DEFAULT_TARGET_HUMIDITY),
      targetCeiling_(DEFAULT_CEILING),
      dsMaxOverTarget_(DEFAULT_DS_MAX_OVER_TARGET),
      controlSensor_(CONTROL_SENSOR_DS),
      fanManualPct_(FAN_MANUAL_AUTO), // AUTO (conditioning-driven circulation)
      runMinutes_(DEFAULT_RUN_MINUTES)
{
    instance_ = this;
}

void MenuController::begin()
{
    pinMode(PIN_ENC_CLK, INPUT);
    pinMode(PIN_ENC_DT, INPUT);
    pinMode(PIN_ENC_SW, INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), encoderTrampoline, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT), encoderTrampoline, CHANGE);
    // The button is polled (see poll()), not interrupt-driven, so a short press
    // and a long hold can be told apart.
}

const char *MenuController::screenLabel() const
{
    return kMenuLabels[menuIndex_];
}

Setpoints MenuController::setpoints() const
{
    Setpoints sp;
    sp.targetTemp = targetTemp_;
    sp.targetHumidity = targetHumidity_;
    sp.targetCeiling = targetCeiling_;
    sp.dsMaxOverTarget = dsMaxOverTarget_;
    sp.controlSensor = controlSensor_;
    sp.fanManualPct = fanManualPct_;
    sp.runMinutes = runMinutes_;
    return sp;
}

bool MenuController::consumeRedraw()
{
    if (!menuChanged_)
        return false;
    menuChanged_ = false;
    return true;
}

bool MenuController::consumeRestart()
{
    if (!restartRequested_)
        return false;
    restartRequested_ = false;
    return true;
}

bool MenuController::consumeStop()
{
    if (!stopRequested_)
        return false;
    stopRequested_ = false;
    return true;
}

bool MenuController::isEditableScreen(int screen) const
{
    return screen == SCREEN_SET_TEMP || screen == SCREEN_SET_HUMID ||
           screen == SCREEN_SET_CEIL || screen == SCREEN_SET_FAN ||
           screen == SCREEN_SET_DS_MAX || screen == SCREEN_SET_CONTROL_SENSOR ||
           screen == SCREEN_SET_RUN;
}

void IRAM_ATTR MenuController::encoderTrampoline()
{
    if (instance_)
        instance_->onEncoder();
}

void IRAM_ATTR MenuController::onEncoder()
{
    encState_ = ((encState_ << 2) | (digitalRead(PIN_ENC_CLK) << 1) | digitalRead(PIN_ENC_DT)) & 0x0F;
    encAccum_ = encAccum_ + kQuadDecode[encState_];

    int dir = 0;
    if (encAccum_ >= 4) // one full detent clockwise
        dir = +1;
    else if (encAccum_ <= -4) // one full detent counter-clockwise
        dir = -1;
    if (dir == 0)
        return;
    encAccum_ = 0;

    if (!editing_)
    {
        // Scroll between screens.
        menuIndex_ = (menuIndex_ + dir + SCREEN_COUNT) % SCREEN_COUNT;
        menuChanged_ = true;
        return;
    }

    // Editing a setpoint: a fast spin takes bigger steps.
    const unsigned long now = micros();
    const int mult = (now - lastEncoderTime_ < 60000) ? 5 : 1;
    lastEncoderTime_ = now;

    switch (menuIndex_)
    {
    case SCREEN_SET_TEMP:
        targetTemp_ = targetTemp_ + dir * 0.5f * mult;
        break;
    case SCREEN_SET_HUMID:
        targetHumidity_ = targetHumidity_ + dir * 1.0f * mult;
        break;
    case SCREEN_SET_CEIL:
        targetCeiling_ = targetCeiling_ + dir * 0.5f * mult;
        break;
    case SCREEN_SET_DS_MAX:
        dsMaxOverTarget_ = dsMaxOverTarget_ + dir * 0.5f * mult;
        if (dsMaxOverTarget_ < 0.0f)
            dsMaxOverTarget_ = 0.0f;
        break;
    case SCREEN_SET_CONTROL_SENSOR:
        controlSensor_ = (ControlSensor)((controlSensor_ + dir + CONTROL_SENSOR_COUNT) % CONTROL_SENSOR_COUNT);
        break;
    case SCREEN_SET_FAN:
    {
        // One knob spans AUTO plus a full manual 0..100% band in 5% steps.
        // Turning up from AUTO enters manual at 0%; turning down past 0% returns
        // to AUTO.
        if (fanManualPct_ < 0)
        {
            if (dir > 0)
                fanManualPct_ = 0; // leave AUTO into manual
        }
        else
        {
            int v = fanManualPct_ + dir * 5 * mult; // 5% steps
            if (v > FAN_DUTY_MAX_PCT)
                v = FAN_DUTY_MAX_PCT;
            else if (v < 0)
                v = FAN_MANUAL_AUTO; // below 0% -> AUTO
            fanManualPct_ = v;
        }
        break;
    }
    case SCREEN_SET_RUN:
        runMinutes_ = runMinutes_ + dir * 5 * mult; // 5 min steps
        if (runMinutes_ < 0)
            runMinutes_ = 0;
        break;
    default:
        break;
    }

    menuChanged_ = true;
}

void MenuController::poll()
{
    const bool pressed = (digitalRead(PIN_ENC_SW) == LOW); // active-low (INPUT_PULLUP)
    const unsigned long now = millis();

    if (pressed && !btnDown_)
    {
        // Press begins.
        btnDown_ = true;
        longFired_ = false;
        btnDownSince_ = now;
    }
    else if (pressed && btnDown_)
    {
        // Still held: fire the stop once the hold threshold is reached.
        if (!longFired_ && now - btnDownSince_ >= STOP_HOLD_MS)
        {
            longFired_ = true;
            stopRequested_ = true;
            editing_ = false;
            menuChanged_ = true;
        }
    }
    else if (!pressed && btnDown_)
    {
        // Released: a short, clean press counts as a tap (long hold already acted).
        btnDown_ = false;
        const unsigned long held = now - btnDownSince_;
        if (!longFired_ && held >= BUTTON_DEBOUNCE_MS)
            onShortPress();
    }
}

void MenuController::onShortPress()
{
    // On the Run Time screen, once the chamber is halted the button restarts it
    // rather than entering edit mode.
    if (menuIndex_ == SCREEN_SET_RUN && halted_)
    {
        restartRequested_ = true;
        editing_ = false;
        menuChanged_ = true;
        return;
    }

    // Only the setpoint screens are editable; button toggles edit mode there.
    if (isEditableScreen(menuIndex_))
        editing_ = !editing_;
    menuChanged_ = true;
}
