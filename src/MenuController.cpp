#include "MenuController.h"

#include "Config.h"

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
    // The button is polled (see poll()) so a short press and a long hold differ.
}

// ---- Group item tables ------------------------------------------------------
const ItemKind *MenuController::groupItems(uint8_t &count) const
{
    switch (topSel_)
    {
    case TOP_STATES:
    {
        static const ItemKind a[] = {IK_VIEW_SENSORS, IK_VIEW_ACT, IK_BACK};
        count = sizeof(a) / sizeof(a[0]);
        return a;
    }
    case TOP_SETTINGS:
    {
        static const ItemKind a[] = {IK_EDIT_TEMP, IK_EDIT_HUMID, IK_EDIT_MAXTEMP,
                                     IK_EDIT_DSMAX, IK_EDIT_CONTROL, IK_BACK};
        count = sizeof(a) / sizeof(a[0]);
        return a;
    }
    case TOP_ACTUATORS:
    {
        static const ItemKind a[] = {IK_CTRL_HEATER, IK_CTRL_HUMID, IK_CTRL_FAN, IK_BACK};
        count = sizeof(a) / sizeof(a[0]);
        return a;
    }
    default:
        count = 0;
        return nullptr;
    }
}

uint8_t MenuController::groupItemCount() const
{
    uint8_t c;
    groupItems(c);
    return c;
}

ItemKind MenuController::groupItem(uint8_t i) const
{
    uint8_t c;
    const ItemKind *a = groupItems(c);
    return (a && i < c) ? a[i] : IK_BACK;
}

ItemKind MenuController::currentItem() const
{
    return groupItem(itemSel_);
}

const char *MenuController::title() const
{
    if (entered_)
    {
        if (editing_)
        {
            switch (currentItem())
            {
            case IK_VIEW_SENSORS: return "Sensors";
            case IK_VIEW_ACT: return "Actuators";
            case IK_EDIT_TEMP: return "Set Temp";
            case IK_EDIT_HUMID: return "Set Humid";
            case IK_EDIT_MAXTEMP: return "Max Temp";
            case IK_EDIT_DSMAX: return "DS Max";
            case IK_EDIT_CONTROL: return "Control";
            case IK_CTRL_HEATER: return "Heater";
            case IK_CTRL_HUMID: return "Humidifier";
            case IK_CTRL_FAN: return "Fan";
            default: return "Back";
            }
        }
        switch (topSel_)
        {
        case TOP_STATES: return "States";
        case TOP_SETTINGS: return "Settings";
        case TOP_ACTUATORS: return "Actuators";
        default: return "";
        }
    }

    switch (topSel_)
    {
    case TOP_OVERVIEW: return "Overview";
    case TOP_STATES: return "States";
    case TOP_SETTINGS: return "Settings";
    case TOP_ACTUATORS: return "Actuators";
    case TOP_RUNTIME: return "Run Time";
    case TOP_NETWORK: return "Network";
    default: return "";
    }
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

bool MenuController::consumeNetworkAction()
{
    if (!networkActionRequested_)
        return false;
    networkActionRequested_ = false;
    return true;
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
    activityPending_ = true;

    // A fast spin takes bigger steps while editing a value.
    const unsigned long now = micros();
    const int mult = (now - lastEncoderTime_ < 60000) ? 5 : 1;
    lastEncoderTime_ = now;

    if (editing_)
    {
        applyEdit(dir, mult);
        menuChanged_ = true;
        return;
    }

    if (entered_)
    {
        // Scroll items within the open group.
        const uint8_t count = groupItemCount();
        if (count > 0)
            itemSel_ = (uint8_t)((itemSel_ + dir + count) % count);
        menuChanged_ = true;
        return;
    }

    // Scroll the top-level pages.
    topSel_ = (uint8_t)((topSel_ + dir + TOP_COUNT) % TOP_COUNT);
    menuChanged_ = true;
}

void MenuController::applyEdit(int dir, int mult)
{
    // The Run Time page is editable directly at the top level; everything else
    // editable lives inside a group.
    if (!entered_)
    {
        if (topSel_ != TOP_RUNTIME)
            return;

        if (!halted_)
        {
            // Run active: the knob adjusts time REMAINING. Keep the stored total
            // as elapsed + remaining so the run ends the right amount of time from
            // NOW and uptime keeps counting from the real start. Works from OFF
            // too (remaining clamps up from 0), so a fresh duration can be dialled
            // in mid-run.
            long remaining = runMinutes_ - runElapsedMin_;
            if (remaining < 0)
                remaining = 0;
            remaining += dir * 5 * mult;
            if (remaining < 0)
                remaining = 0;
            runMinutes_ = runElapsedMin_ + remaining;
        }
        else
        {
            runMinutes_ = runMinutes_ + dir * 5 * mult;
            if (runMinutes_ < 0)
                runMinutes_ = 0;
        }
        return;
    }

    switch (currentItem())
    {
    case IK_EDIT_TEMP:
        targetTemp_ = targetTemp_ + dir * 0.5f * mult;
        break;
    case IK_EDIT_HUMID:
        targetHumidity_ = targetHumidity_ + dir * 1.0f * mult;
        break;
    case IK_EDIT_MAXTEMP:
        targetCeiling_ = targetCeiling_ + dir * 0.5f * mult;
        break;
    case IK_EDIT_DSMAX:
        dsMaxOverTarget_ = dsMaxOverTarget_ + dir * 0.5f * mult;
        if (dsMaxOverTarget_ < 0.0f)
            dsMaxOverTarget_ = 0.0f;
        break;
    case IK_EDIT_CONTROL:
        controlSensor_ = (ControlSensor)((controlSensor_ + dir + CONTROL_SENSOR_COUNT) % CONTROL_SENSOR_COUNT);
        break;
    case IK_CTRL_HEATER:
        heaterOverride_ = clampOverride((int8_t)(heaterOverride_ + dir));
        break;
    case IK_CTRL_HUMID:
        humidOverride_ = clampOverride((int8_t)(humidOverride_ + dir));
        break;
    case IK_CTRL_FAN:
        // One knob spans AUTO plus a manual 0..100% band in 5% steps. Turning up
        // from AUTO enters manual at 0%; turning down past 0% returns to AUTO.
        if (fanManualPct_ < 0)
        {
            if (dir > 0)
                fanManualPct_ = 0;
        }
        else
        {
            int v = fanManualPct_ + dir * 5 * mult;
            if (v > FAN_DUTY_MAX_PCT)
                v = FAN_DUTY_MAX_PCT;
            else if (v < 0)
                v = FAN_MANUAL_AUTO;
            fanManualPct_ = v;
        }
        break;
    default:
        break; // views (Sensors/Actuators) and Back ignore the knob
    }
}

void MenuController::poll()
{
    const bool pressed = (digitalRead(PIN_ENC_SW) == LOW); // active-low (INPUT_PULLUP)
    const unsigned long now = millis();

    if (pressed && !btnDown_)
    {
        btnDown_ = true;
        longFired_ = false;
        btnDownSince_ = now;
        activityPending_ = true;
    }
    else if (pressed && btnDown_)
    {
        // Still held: fire the global STOP once the hold threshold is reached.
        if (!longFired_ && now - btnDownSince_ >= STOP_HOLD_MS)
        {
            longFired_ = true;
            stopRequested_ = true;
            editing_ = false; // drop out of any open editor
            menuChanged_ = true;
            activityPending_ = true;
        }
    }
    else if (!pressed && btnDown_)
    {
        // Released: a short, clean press is a tap (the long hold already acted).
        btnDown_ = false;
        const unsigned long held = now - btnDownSince_;
        activityPending_ = true;
        if (!longFired_ && held >= BUTTON_DEBOUNCE_MS)
            onShortPress();
    }
}

void MenuController::onShortPress()
{
    // Inside an open editor/view: push closes it back to the group list.
    if (editing_)
    {
        editing_ = false;
        menuChanged_ = true;
        return;
    }

    // Inside a group: open the highlighted item, or go back.
    if (entered_)
    {
        if (currentItem() == IK_BACK)
            entered_ = false;
        else
            editing_ = true;
        menuChanged_ = true;
        return;
    }

    // Top level.
    switch (topSel_)
    {
    case TOP_STATES:
    case TOP_SETTINGS:
    case TOP_ACTUATORS:
        entered_ = true;
        itemSel_ = 0;
        break;
    case TOP_RUNTIME:
        // While halted the button (re)starts the run; otherwise it edits the time.
        if (halted_)
            restartRequested_ = true;
        else
            editing_ = true;
        break;
    case TOP_NETWORK:
        networkActionRequested_ = true;
        break;
    default:
        break; // Overview: nothing to do
    }
    menuChanged_ = true;
}
