// =============================================================================
// Fermentation chamber controller
// =============================================================================
// Top-level wiring only. Each subsystem lives in its own module:
//   Config.h         - pin map and tuning constants
//   Sensors          - DS18B20 probe + BME280
//   Controller       - fan / heater / humidifier logic and safety
//   MenuController    - rotary encoder + button UI and setpoints
//   Display          - SSD1306 OLED rendering

#include <Arduino.h>
#include <Wire.h>

#include "Config.h"
#include "Sensors.h"
#include "Controller.h"
#include "MenuController.h"
#include "Display.h"

static Sensors sensors;
static Controller controller;
static MenuController menu;
static Display display;

static unsigned long lastSensorRead = 0;

// Block forever, retrying init, if a required device is missing.
static void waitForDevice(bool ok, const char *failMessage)
{
    while (!ok)
    {
        Serial.println(failMessage);
        delay(1000);
    }
}

void setup()
{
    Serial.begin(115200);
    Wire.begin(PIN_SDA, PIN_SCL);

    menu.begin();
    controller.begin();

    waitForDevice(display.begin(), "SSD1306 failed");
    waitForDevice(sensors.begin(), "BME280 not found!");

    sensors.read();
    controller.update(menu.setpoints(), sensors.readings());
    menu.requestRedraw();

    Serial.println("Ready.");
}

void loop()
{
    // Update sensors and outputs once per interval.
    if (millis() - lastSensorRead >= SENSOR_INTERVAL_MS)
    {
        lastSensorRead = millis();

        sensors.read();

        const Setpoints sp = menu.setpoints();
        controller.update(sp, sensors.readings());
        menu.setHalted(controller.state().halted());
        menu.requestRedraw(); // refresh display with new data

        const SensorReadings &s = sensors.readings();
        Serial.printf("Temp: %.2f C (set %.1f) | Humid: %.1f%% (set %.0f)\n",
                      s.dsTemp, sp.targetTemp, s.humidity, sp.targetHumidity);
    }

    // Read the button (short press = tap, 5 s hold = stop).
    menu.poll();

    // Hold-to-stop: shut everything off until the user restarts.
    if (menu.consumeStop())
    {
        controller.stop();
        menu.setHalted(true);
        menu.requestRedraw();
    }

    // Handle a restart request from the UI (chamber halted).
    if (menu.consumeRestart())
    {
        controller.restart();
        menu.setHalted(false);
        menu.requestRedraw();
    }

    // Redraw only when something changed (saves flicker).
    if (menu.consumeRedraw())
    {
        display.render(menu.screen(), menu.isEditing(),
                       menu.setpoints(), sensors.readings(), controller.state(),
                       menu.screenLabel(), controller.runStartMs());
    }
}
