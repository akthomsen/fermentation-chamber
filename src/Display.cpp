#include "Display.h"

#include <string.h>
#include <Wire.h>
#include "Config.h"
#include "MenuController.h" // for MenuScreen

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

// Common "push to edit / EDIT: turn knob" prompt on the bottom line.
void drawEditPrompt(Adafruit_SSD1306 &oled, bool editing, int y)
{
    oled.setTextSize(1);
    oled.setCursor(0, y);
    oled.print(editing ? "EDIT: turn knob" : "push to edit");
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

void Display::render(int screen, bool editing,
                     const Setpoints &sp,
                     const SensorReadings &sensors,
                     const ActuatorState &act,
                     const char *title,
                     unsigned long runStartMs,
                     const NetworkStatus &net)
{
    oled_.clearDisplay();

    // --- Top bar: screen title + divider ---
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);
    oled_.setCursor(0, 0);
    oled_.printf("[ %s ]", title);

    // Corner alert chip flags the first broken link, so any screen shows at a
    // glance that connectivity is down. Suppressed on the Network screen itself,
    // which already spells the state out in full.
    const char *alert = !net.wifiConnected ? "WiFi!" : !net.mqttConnected ? "MQTT!" : "";
    if (screen != SCREEN_NETWORK && alert[0])
    {
        const int16_t x = SCREEN_WIDTH - (int16_t)strlen(alert) * 6;
        oled_.setCursor(x < 0 ? 0 : x, 0);
        oled_.print(alert);
    }
    oled_.drawFastHLine(0, 12, SCREEN_WIDTH, SSD1306_WHITE);

    switch (screen)
    {
    case SCREEN_OVERVIEW:
        drawOverview(sp, sensors, act, runStartMs);
        break;
    case SCREEN_SENSORS:
        drawSensors(sensors);
        break;
    case SCREEN_ACTUATORS:
        drawActuators(sp, act);
        break;
    case SCREEN_NETWORK:
        drawNetwork(net);
        break;
    case SCREEN_SET_TEMP:
        drawSetTemp(sp, editing);
        break;
    case SCREEN_SET_HUMID:
        drawSetHumid(sp, editing);
        break;
    case SCREEN_SET_CEIL:
        drawSetCeiling(sp, editing);
        break;
    case SCREEN_SET_DS_MAX:
        drawSetDsMax(sp, editing);
        break;
    case SCREEN_SET_CONTROL_SENSOR:
        drawSetControlSensor(sp, editing);
        break;
    case SCREEN_SET_FAN:
        drawSetFan(sp, editing);
        break;
    case SCREEN_SET_RUN:
        drawSetRun(sp, act, editing, runStartMs);
        break;
    default:
        break;
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
        // Running with a time limit: uptime (u), remaining (l) and goal (g) on
        // one line so the whole run is visible at a glance. Single-letter labels
        // keep it within 128 px even for multi-hour runs.
        const long up = elapsedMinutes(runStartMs);
        const long left = remainingMinutes(sp, runStartMs);
        oled_.printf("u%ld:%02ld l%ld:%02ld g%ld:%02ld",
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

void Display::drawSetTemp(const Setpoints &sp, bool editing)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 24);
    oled_.printf("%.1f C", sp.targetTemp);
    drawEditPrompt(oled_, editing, 56);
}

void Display::drawSetHumid(const Setpoints &sp, bool editing)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 24);
    oled_.printf("%.0f %%", sp.targetHumidity);
    drawEditPrompt(oled_, editing, 56);
}

void Display::drawSetCeiling(const Setpoints &sp, bool editing)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 24);
    oled_.printf("%.1f C", sp.targetCeiling);
    drawEditPrompt(oled_, editing, 56);
}

void Display::drawSetDsMax(const Setpoints &sp, bool editing)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 22);
    oled_.printf("+%.1f C", sp.dsMaxOverTarget);
    oled_.setTextSize(1);
    oled_.setCursor(0, 44);
    oled_.printf("DS limit %.1f C", sp.targetTemp + sp.dsMaxOverTarget);
    drawEditPrompt(oled_, editing, 56);
}

void Display::drawSetControlSensor(const Setpoints &sp, bool editing)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 24);
    oled_.print(controlSensorLabel(sp.controlSensor));
    drawEditPrompt(oled_, editing, 56);
}

void Display::drawSetFan(const Setpoints &sp, bool editing)
{
    oled_.setTextSize(2);
    oled_.setCursor(0, 24);
    if (sp.fanManualPct < 0)
        oled_.print("AUTO");
    else
        oled_.printf("%d %%", sp.fanManualPct);
    drawEditPrompt(oled_, editing, 56);
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
        oled_.print(editing ? "EDIT: left" : "push to edit");
}
