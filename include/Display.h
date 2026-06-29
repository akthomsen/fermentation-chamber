#pragma once

#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include "Sensors.h"
#include "Controller.h"
#include "MenuController.h"
#include "NetStatus.h"

// Renders the SSD1306 OLED. Owns the panel and knows how to draw each screen,
// but holds no application state of its own - what to draw is read from the
// MenuController's navigation state plus the data passed in.
class Display
{
public:
    Display();

    // Initialise the panel. Returns false if the display is not found.
    bool begin();

    // Show a two-line full-screen message (boot faults, etc.). Standalone so it
    // can be used before the normal render path is ready.
    void showMessage(const char *line1, const char *line2);

    // Draw the network status page on its own (used to show live progress during
    // the boot/manual connect, independent of the menu's current page).
    void renderNetwork(const NetworkStatus &net);

    // Redraw whatever the menu is currently showing. runStartMs is the millis()
    // timestamp the current run began at, for the elapsed/left timer.
    void render(const MenuController &menu,
                const Setpoints &sp,
                const SensorReadings &sensors,
                const ActuatorState &act,
                unsigned long runStartMs,
                const NetworkStatus &net);

private:
    void drawTopBar(const char *title, const NetworkStatus &net, bool showAlert);

    void drawOverview(const Setpoints &sp, const SensorReadings &s, const ActuatorState &act,
                      unsigned long runStartMs);
    void drawSensors(const SensorReadings &s);
    void drawActuators(const Setpoints &sp, const ActuatorState &act);
    void drawCover(const char *name);
    void drawGroupList(const MenuController &menu, const Setpoints &sp, const ActuatorState &act);
    void drawSetTemp(const Setpoints &sp);
    void drawSetHumid(const Setpoints &sp);
    void drawSetCeiling(const Setpoints &sp);
    void drawSetDsMax(const Setpoints &sp);
    void drawSetControlSensor(const Setpoints &sp);
    void drawSetHysteresis(const Setpoints &sp);
    void drawSetFanAfterHeat(const Setpoints &sp);
    void drawSetMaxOn(const Setpoints &sp);
    void drawSetCooldown(const Setpoints &sp);
    void drawSetFan(const Setpoints &sp);
    void drawOverrideCtrl(int8_t override);
    void drawSetRun(const Setpoints &sp, const ActuatorState &act, bool editing,
                    unsigned long runStartMs);
    void drawNetwork(const NetworkStatus &net);

    Adafruit_SSD1306 oled_;
};
