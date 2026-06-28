#include "Display.h"

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

void Display::render(int screen, bool editing,
                     const Setpoints &sp,
                     const SensorReadings &sensors,
                     const ActuatorState &act,
                     const char *title,
                     unsigned long runStartMs)
{
    oled_.clearDisplay();

    // --- Top bar: screen title + divider ---
    oled_.setTextSize(1);
    oled_.setTextColor(SSD1306_WHITE);
    oled_.setCursor(0, 0);
    oled_.printf("[ %s ]", title);
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
    case SCREEN_SET_TEMP:
        drawSetTemp(sp, editing);
        break;
    case SCREEN_SET_HUMID:
        drawSetHumid(sp, editing);
        break;
    case SCREEN_SET_CEIL:
        drawSetCeiling(sp, editing);
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
        oled_.print("Time READY");
    }
    else if (act.stopped)
    {
        oled_.print("Time STOPPED");
    }
    else if (act.runComplete)
    {
        oled_.print("Time FINISHED");
    }
    else if (sp.runMinutes > 0)
    {
        long left = remainingMinutes(sp, runStartMs);
        oled_.printf("Left %ldh %02ldm", left / 60, left % 60);
    }
    else
    {
        long up = elapsedMinutes(runStartMs);
        oled_.printf("Up   %ldh %02ldm", up / 60, up % 60);
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
        oled_.printf("Ceiling   : %.1f C", sp.targetCeiling);
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
    oled_.setTextSize(2);
    oled_.setCursor(0, 22);
    if (sp.runMinutes <= 0)
        oled_.print("OFF");
    else
        oled_.printf("%ldh %02ldm", sp.runMinutes / 60, sp.runMinutes % 60);

    oled_.setTextSize(1);
    oled_.setCursor(0, 44);
    if (act.notStarted)
    {
        oled_.print("READY");
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
        long left = remainingMinutes(sp, runStartMs);
        oled_.printf("left: %ldh %02ldm", left / 60, left % 60);
    }

    // While halted the button starts/restarts the run instead of editing.
    oled_.setCursor(0, 56);
    if (act.notStarted)
        oled_.print("push to start");
    else if (act.halted())
        oled_.print("push to restart");
    else
        oled_.print(editing ? "EDIT: turn knob" : "push to edit");
}
