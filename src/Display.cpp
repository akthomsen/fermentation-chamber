#include "Display.h"

#include <string.h>
#include <Wire.h>
#include "Config.h"

namespace
{
// Minutes elapsed since the run started.
long elapsedMinutes(unsigned long runStartMs)
{
    return (long)((millis() - runStartMs) / 60000UL);
}

// Minutes left before the run finishes (clamped at 0).
long remainingMinutes(const Setpoints &sp, unsigned long runStartMs)
{
    long left = sp.runMinutes - elapsedMinutes(runStartMs);
    return left < 0 ? 0 : left;
}

const char *controlSensorLabel(ControlSensor sensor)
{
    switch (sensor)
    {
    case CONTROL_SENSOR_DS:
        return "DS";
    case CONTROL_SENSOR_BME:
        return "BME";
    case CONTROL_SENSOR_AVERAGE:
        return "AVG";
    default:
        return "?";
    }
}

const char *overrideLabel(int8_t ov)
{
    return ov > 0 ? "On" : (ov < 0 ? "Off" : "Auto");
}

// One row of a group list: short label + current value.
void formatGroupItem(ItemKind k, const Setpoints &sp, const ActuatorState &act, char *out, size_t n)
{
    switch (k)
    {
    case IK_VIEW_SENSORS:
        snprintf(out, n, "Sensors");
        break;
    case IK_VIEW_ACT:
        snprintf(out, n, "Actuators");
        break;
    case IK_EDIT_TEMP:
        snprintf(out, n, "Temp    %.1fC", sp.targetTemp);
        break;
    case IK_EDIT_HUMID:
        snprintf(out, n, "Humid   %.0f%%", sp.targetHumidity);
        break;
    case IK_EDIT_MAXTEMP:
        snprintf(out, n, "MaxTemp %.1fC", sp.targetCeiling);
        break;
    case IK_EDIT_DSMAX:
        snprintf(out, n, "DS Max +%.1fC", sp.dsMaxOverTarget);
        break;
    case IK_EDIT_CONTROL:
        snprintf(out, n, "Control %s", controlSensorLabel(sp.controlSensor));
        break;
    case IK_EDIT_HYST:
        snprintf(out, n, "Hyst    %.1fC", sp.hysteresis);
        break;
    case IK_EDIT_FANHEAT:
        snprintf(out, n, "FanHeat %lds", sp.fanAfterHeatSec);
        break;
    case IK_EDIT_MAXON:
        snprintf(out, n, "MaxOn   %ldm", sp.maxHeatMin);
        break;
    case IK_EDIT_COOLDOWN:
        snprintf(out, n, "Cooldn  %ldm", sp.heatCooldownMin);
        break;
    case IK_CTRL_HEATER:
        snprintf(out, n, "Heater  %s", overrideLabel(act.heaterOverride));
        break;
    case IK_CTRL_HUMID:
        snprintf(out, n, "Humid   %s", overrideLabel(act.humidOverride));
        break;
    case IK_CTRL_FAN:
        if (sp.fanManualPct < 0)
            snprintf(out, n, "Fan     AUTO");
        else
            snprintf(out, n, "Fan     %d%%", sp.fanManualPct);
        break;
    case IK_BACK:
    default:
        snprintf(out, n, "< Back");
        break;
    }
}

// Common prompt under an open editor: turn changes, push returns to the list.
void drawValuePrompt(Adafruit_SSD1306 &oled, int y)
{
    oled.setTextSize(1);
    oled.setCursor(0, y);
    oled.print("turn=set  push=back");
}
} // namespace

Display::Display()
    : oled_(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET)
{
}

bool Display::begin()
{
    if (!oled_.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS))
        return false;
    oled_.setTextColor(SSD1306_WHITE);
    return true;
}

void Display::showMessage(const char *line1, const char *line2)
{
    oled_.clearDisplay();
    oled_.setTextColor(SSD1306_WHITE);
    oled_.setTextSize(2);
    oled_.setCursor(0, 12);
    oled_.println(line1);
    oled_.setCursor(0, 36);
    oled_.println(line2);
    oled_.display();
}

void Display::drawTopBar(const char *title, const NetworkStatus &net, bool showAlert)
{
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);
    oled_.setCursor(0, 0);
    oled_.printf("[ %s ]", title);

    // Corner alert chip flags the first broken link from any screen. Suppressed on
    // the Network page, which already spells the state out in full.
    if (showAlert)
    {
        const char *alert = !net.wifiConnected ? "WiFi!" : !net.mqttConnected ? "MQTT!" : "";
        if (alert[0])
        {
            const int16_t x = SCREEN_WIDTH - (int16_t)strlen(alert) * 6;
            oled_.setCursor(x < 0 ? 0 : x, 0);
            oled_.print(alert);
        }
    }
    oled_.drawFastHLine(0, 12, SCREEN_WIDTH, SSD1306_WHITE);
}

void Display::renderNetwork(const NetworkStatus &net)
{
    oled_.clearDisplay();
    drawTopBar("Network", net, false);
    drawNetwork(net);
    oled_.display();
}

void Display::render(const MenuController &menu,
                     const Setpoints &sp,
                     const SensorReadings &sensors,
                     const ActuatorState &act,
                     unsigned long runStartMs,
                     const NetworkStatus &net)
{
    oled_.clearDisplay();

    const bool onNetworkPage = (menu.topPage() == TOP_NETWORK && !menu.entered());
    drawTopBar(menu.title(), net, !onNetworkPage);

    if (menu.isEditing())
    {
        if (!menu.entered())
        {
            // The only directly-editable top page is Run Time.
            drawSetRun(sp, act, true, runStartMs);
        }
        else
        {
            switch (menu.currentItem())
            {
            case IK_VIEW_SENSORS:
                drawSensors(sensors);
                break;
            case IK_VIEW_ACT:
                drawActuators(sp, act);
                break;
            case IK_EDIT_TEMP:
                drawSetTemp(sp);
                break;
            case IK_EDIT_HUMID:
                drawSetHumid(sp);
                break;
            case IK_EDIT_MAXTEMP:
                drawSetCeiling(sp);
                break;
            case IK_EDIT_DSMAX:
                drawSetDsMax(sp);
                break;
            case IK_EDIT_CONTROL:
                drawSetControlSensor(sp);
                break;
            case IK_EDIT_HYST:
                drawSetHysteresis(sp);
                break;
            case IK_EDIT_FANHEAT:
                drawSetFanAfterHeat(sp);
                break;
            case IK_EDIT_MAXON:
                drawSetMaxOn(sp);
                break;
            case IK_EDIT_COOLDOWN:
                drawSetCooldown(sp);
                break;
            case IK_CTRL_HEATER:
                drawOverrideCtrl(act.heaterOverride);
                break;
            case IK_CTRL_HUMID:
                drawOverrideCtrl(act.humidOverride);
                break;
            case IK_CTRL_FAN:
                drawSetFan(sp);
                break;
            default:
                break;
            }
        }
    }
    else if (menu.entered())
    {
        drawGroupList(menu, sp, act);
    }
    else
    {
        switch (menu.topPage())
        {
        case TOP_OVERVIEW:
            drawOverview(sp, sensors, act, runStartMs);
            break;
        case TOP_STATES:
        case TOP_SETTINGS:
        case TOP_ACTUATORS:
            drawCover(menu.title());
            break;
        case TOP_RUNTIME:
            drawSetRun(sp, act, false, runStartMs);
            break;
        case TOP_NETWORK:
            drawNetwork(net);
            break;
        default:
            break;
        }
    }

    oled_.display();
}

void Display::drawOverview(const Setpoints &sp, const SensorReadings &s, const ActuatorState &act,
                           unsigned long runStartMs)
{
    oled_.setTextSize(1);
    oled_.setCursor(0, 16);
    oled_.printf("DS  %.1f / %.1f C", s.dsTemp, sp.targetTemp);
    oled_.setCursor(0, 28);
    oled_.printf("BME %.1f / %.1f C", s.bmeTemp, sp.targetTemp);
    oled_.setCursor(0, 40);
    oled_.printf("Hum %.0f / %.0f %%", s.humidity, sp.targetHumidity);
    oled_.setCursor(0, 52);

    if (act.notStarted)
    {
        if (sp.runMinutes > 0)
            oled_.printf("READY goal %ld:%02ld", sp.runMinutes / 60, sp.runMinutes % 60);
        else
            oled_.print("READY (no limit)");
    }
    else if (act.stopped)
    {
        oled_.print("STOPPED");
    }
    else if (act.runComplete)
    {
        oled_.print("FINISHED");
    }
    else if (sp.runMinutes > 0)
    {
        // Running with a time limit: Uptime, Left and Goal on one line so the
        // whole run is visible at a glance. Labels are UPPERCASE on purpose: a
        // lowercase 'l' is indistinguishable from '1' in the 5x7 font (so "l13:00"
        // reads as "113:00"). Single letters keep it within 128 px for long runs.
        const long up = elapsedMinutes(runStartMs);
        const long left = remainingMinutes(sp, runStartMs);
        oled_.printf("U%ld:%02ld L%ld:%02ld G%ld:%02ld",
                     up / 60, up % 60, left / 60, left % 60,
                     sp.runMinutes / 60, sp.runMinutes % 60);
    }
    else
    {
        const long up = elapsedMinutes(runStartMs);
        oled_.printf("Up %ld:%02ld  no limit", up / 60, up % 60);
    }
}

void Display::drawSensors(const SensorReadings &s)
{
    oled_.setTextSize(1);
    oled_.setCursor(0, 16);
    oled_.printf("DS Temp : %.1f C", s.dsTemp);
    oled_.setCursor(0, 28);
    oled_.printf("BME Temp: %.1f C", s.bmeTemp);
    oled_.setCursor(0, 40);
    oled_.printf("Humidity: %.1f %%", s.humidity);
    oled_.setCursor(0, 52);
    oled_.printf("Pressure: %.0f hPa", s.pressure);
}

void Display::drawActuators(const Setpoints &sp, const ActuatorState &act)
{
    const char *heaterStat = act.heaterFault ? "FAULT"
                             : act.heaterLockout ? "COOLDN"
                             : act.heaterOn ? "ON"
                                            : "OFF";
    const char *fanMode = sp.fanManualPct >= 0 ? "M" : "A";
    oled_.setTextSize(1);
    oled_.setCursor(0, 16);
    oled_.printf("Fan       : %d%% %s", act.fanDuty, fanMode);
    oled_.setCursor(0, 28);
    oled_.printf("Heater    : %s", heaterStat);
    oled_.setCursor(0, 40);
    oled_.printf("Humidifier: %s", act.humidOn ? "ON" : "OFF");
    oled_.setCursor(0, 52);
    if (act.notStarted)
        oled_.print("READY - push start");
    else if (act.stopped)
        oled_.print("RUN STOPPED");
    else if (act.runComplete)
        oled_.print("RUN FINISHED");
    else
        oled_.printf("Ctrl %s: %.1f C", controlSensorLabel(act.controlSensor), act.controlTemp);
}

void Display::drawCover(const char *name)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 26);
    oled_.print(name);
    oled_.setTextSize(1);
    oled_.setCursor(0, 52);
    oled_.print("push to open");
}

void Display::drawGroupList(const MenuController &menu, const Setpoints &sp, const ActuatorState &act)
{
    constexpr uint8_t MAX_VISIBLE = 6; // rows that fit below the title bar
    oled_.setTextSize(1);
    const uint8_t n = menu.groupItemCount();
    const uint8_t sel = menu.itemIndex();

    // Scroll window: keep the highlighted item on screen for longer lists.
    uint8_t first = 0;
    if (n > MAX_VISIBLE)
    {
        if (sel >= MAX_VISIBLE)
            first = sel - MAX_VISIBLE + 1;
        if (first > n - MAX_VISIBLE)
            first = n - MAX_VISIBLE;
    }

    for (uint8_t row = 0; row < MAX_VISIBLE && (first + row) < n; row++)
    {
        const uint8_t i = first + row;
        const int y = 15 + row * 8;
        oled_.setCursor(0, y);
        oled_.print(i == sel ? ">" : " ");
        oled_.setCursor(8, y);
        char buf[22];
        formatGroupItem(menu.groupItem(i), sp, act, buf, sizeof(buf));
        oled_.print(buf);
    }
}

void Display::drawSetTemp(const Setpoints &sp)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 24);
    oled_.printf("%.1f C", sp.targetTemp);
    drawValuePrompt(oled_, 56);
}

void Display::drawSetHumid(const Setpoints &sp)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 24);
    oled_.printf("%.0f %%", sp.targetHumidity);
    drawValuePrompt(oled_, 56);
}

void Display::drawSetCeiling(const Setpoints &sp)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 24);
    oled_.printf("%.1f C", sp.targetCeiling);
    drawValuePrompt(oled_, 56);
}

void Display::drawSetDsMax(const Setpoints &sp)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 22);
    oled_.printf("+%.1f C", sp.dsMaxOverTarget);
    oled_.setTextSize(1);
    oled_.setCursor(0, 44);
    oled_.printf("DS limit %.1f C", sp.targetTemp + sp.dsMaxOverTarget);
    drawValuePrompt(oled_, 56);
}

void Display::drawSetControlSensor(const Setpoints &sp)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 24);
    oled_.print(controlSensorLabel(sp.controlSensor));
    drawValuePrompt(oled_, 56);
}

void Display::drawSetHysteresis(const Setpoints &sp)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 22);
    oled_.printf("%.1f C", sp.hysteresis);
    oled_.setTextSize(1);
    oled_.setCursor(0, 44);
    oled_.print("control dead-band");
    drawValuePrompt(oled_, 56);
}

void Display::drawSetFanAfterHeat(const Setpoints &sp)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 22);
    oled_.printf("%ld s", sp.fanAfterHeatSec);
    oled_.setTextSize(1);
    oled_.setCursor(0, 44);
    oled_.print("fan hold after heat");
    drawValuePrompt(oled_, 56);
}

void Display::drawSetMaxOn(const Setpoints &sp)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 22);
    oled_.printf("%ld min", sp.maxHeatMin);
    oled_.setTextSize(1);
    oled_.setCursor(0, 44);
    oled_.print("max heater-on time");
    drawValuePrompt(oled_, 56);
}

void Display::drawSetCooldown(const Setpoints &sp)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 22);
    oled_.printf("%ld min", sp.heatCooldownMin);
    oled_.setTextSize(1);
    oled_.setCursor(0, 44);
    oled_.print("cooldown after trip");
    drawValuePrompt(oled_, 56);
}

void Display::drawSetFan(const Setpoints &sp)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 24);
    if (sp.fanManualPct < 0)
        oled_.print("AUTO");
    else
        oled_.printf("%d %%", sp.fanManualPct);
    drawValuePrompt(oled_, 56);
}

void Display::drawOverrideCtrl(int8_t override)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 24);
    oled_.print(override > 0 ? "ON" : (override < 0 ? "OFF" : "AUTO"));
    drawValuePrompt(oled_, 56);
}

void Display::drawSetRun(const Setpoints &sp, const ActuatorState &act, bool editing,
                         unsigned long runStartMs)
{
    const bool running = !act.halted();

    // Big number: while a run is active it shows time REMAINING (and the knob
    // edits that), so editing is intuitive -- the value on screen is the value
    // you change. While halted it shows the full duration set for the next run.
    oled_.setTextSize(2);
    oled_.setCursor(0, 22);
    if (running && sp.runMinutes > 0)
    {
        long left = remainingMinutes(sp, runStartMs);
        oled_.printf("%ldh %02ldm", left / 60, left % 60);
    }
    else if (sp.runMinutes <= 0)
        oled_.print("OFF");
    else
        oled_.printf("%ldh %02ldm", sp.runMinutes / 60, sp.runMinutes % 60);

    oled_.setTextSize(1);
    oled_.setCursor(0, 44);
    if (act.notStarted)
        oled_.print("READY");
    else if (act.stopped)
        oled_.print("STOPPED");
    else if (act.runComplete)
        oled_.print("FINISHED");
    else
    {
        // Running: show uptime (time since start), the true start-to-finish clock,
        // unaffected by edits to the remaining time.
        long up = elapsedMinutes(runStartMs);
        oled_.printf("up %ldh %02ldm", up / 60, up % 60);
    }

    // While halted the button starts/restarts the run instead of editing.
    oled_.setCursor(0, 56);
    if (act.notStarted)
        oled_.print("push to start");
    else if (act.halted())
        oled_.print("push to restart");
    else
        oled_.print(editing ? "turn=set  push=back" : "push to edit");
}

void Display::drawNetwork(const NetworkStatus &net)
{
    oled_.setTextSize(1);
    oled_.setCursor(0, 16);
    oled_.printf("WiFi: %s", net.wifiConnected ? net.ip : "DOWN");
    oled_.setCursor(0, 28);
    oled_.printf("MQTT: %s", net.mqttConnected ? "OK" : "DOWN");

    // Reason / progress line: the exact failure reason (e.g. "rc-2 ...") is the
    // whole point of this screen -- it tells the user WHY the link is down.
    oled_.setCursor(0, 40);
    oled_.print(net.detail);

    oled_.setCursor(0, 52);
    if (net.connecting)
        oled_.print("Connecting...");
    else if (net.mqttConnected)
        oled_.print("push to disconnect");
    else
        oled_.print("push to connect");
}
