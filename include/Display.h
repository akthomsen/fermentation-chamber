#pragma once

#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include "Sensors.h"
#include "Controller.h"
#include "NetStatus.h"

// Renders the SSD1306 OLED. Owns the panel and knows how to draw each screen,
// but holds no application state of its own - everything to draw is passed in.
class Display
{
public:
    Display();

    // Initialise the panel. Returns false if the display is not found.
    bool begin();

    // Show a two-line full-screen message (boot faults, etc.). Standalone so it
    // can be used before the normal render path is ready.
    void showMessage(const char *line1, const char *line2);

    // Redraw the given screen from the current system state. runStartMs is the
    // millis() timestamp the current run began at, used for the elapsed/left timer.
    void render(int screen, bool editing,
                const Setpoints &sp,
                const SensorReadings &sensors,
                const ActuatorState &act,
                const char *title,
                unsigned long runStartMs,
                const NetworkStatus &net);

private:
    void drawOverview(const Setpoints &sp, const SensorReadings &s, const ActuatorState &act,
                      unsigned long runStartMs);
    void drawSensors(const SensorReadings &s);
    void drawActuators(const Setpoints &sp, const ActuatorState &act);
    void drawNetwork(const NetworkStatus &net);
    void drawSetTemp(const Setpoints &sp, bool editing);
    void drawSetHumid(const Setpoints &sp, bool editing);
    void drawSetCeiling(const Setpoints &sp, bool editing);
    void drawSetDsMax(const Setpoints &sp, bool editing);
    void drawSetControlSensor(const Setpoints &sp, bool editing);
    void drawSetFan(const Setpoints &sp, bool editing);
    void drawSetRun(const Setpoints &sp, const ActuatorState &act, bool editing,
                    unsigned long runStartMs);

    Adafruit_SSD1306 oled_;
};
