// =============================================================================
// Fermentation chamber controller
// =============================================================================
// Top-level wiring only. Each subsystem lives in its own module:
//   Config.h        - pin map and tuning constants
//   Sensors         - DS18B20 probe + BME280
//   Controller      - fan / heater / humidifier logic and safety
//   MenuController   - rotary encoder + button UI and setpoints
//   Display         - SSD1306 OLED rendering
//   ChamberNet   - WiFi + MQTT (connect-once, manual retry, never blocks)

#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "Config.h"
#include "Sensors.h"
#include "Controller.h"
#include "MenuController.h"
#include "Display.h"
#include "ChamberNet.h"

static Sensors sensors;
static Controller controller;
static MenuController menu;
static Display display;
static ChamberNet net;

static unsigned long lastSensorRead = 0;

// Task-watchdog timeout. If loop() stalls this long (e.g. a locked I2C bus while
// the heater is ON) the chip resets and controller.begin() drives everything OFF
// again on reboot. Comfortably above the one intentional blocking window (a boot
// or user-triggered connect), which feeds the watchdog while it waits.
constexpr unsigned long WDT_TIMEOUT_MS = 12000;

// Map an esp_reset_reason() to a short name so the boot log records WHY the
// device last reset (power-on vs. crash vs. watchdog). The primary breadcrumb
// for debugging a field fault after the fact.
static const char *resetReasonName(esp_reset_reason_t r)
{
    switch (r)
    {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_SW:
        return "sw-reset";
    case ESP_RST_PANIC:
        return "panic/crash";
    case ESP_RST_INT_WDT:
        return "int-watchdog";
    case ESP_RST_TASK_WDT:
        return "task-watchdog";
    case ESP_RST_WDT:
        return "other-watchdog";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_DEEPSLEEP:
        return "deep-sleep";
    default:
        return "unknown";
    }
}

// Repaint the network page during the blocking connect so the wait is visible.
static void renderNetProgress()
{
    display.renderNetwork(net.status());
}

// Reconcile a menu override intent (-1 off, 0 auto, 1 on) onto the Controller.
// Only acts on a change so the off-side-effects don't re-fire every loop.
static void applyOverride(int8_t want, int8_t current,
                          void (Controller::*setOn)(bool), void (Controller::*clear)())
{
    if (want == current)
        return;
    if (want > 0)
        (controller.*setOn)(true);
    else if (want < 0)
        (controller.*setOn)(false);
    else
        (controller.*clear)();
}

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
    delay(50);
    const esp_reset_reason_t rr = esp_reset_reason();
    Serial.printf("\n[boot] reset reason: %d (%s)\n", (int)rr, resetReasonName(rr));

    Wire.begin(PIN_SDA, PIN_SCL);

    menu.begin();
    controller.begin(); // drive every actuator to a known OFF state first

    waitForDevice(display.begin(), "SSD1306 failed");

    // BME is required. Block (every actuator is already OFF, so this is safe) but
    // surface the fault on the OLED too, not just Serial, so a headless unit is
    // still diagnosable.
    while (!sensors.begin())
    {
        Serial.println("BME280 not found!");
        display.showMessage("BME280", "not found");
        delay(1000);
    }

    sensors.read();
    controller.update(menu.setpoints(), sensors.readings());
    menu.requestRedraw();

    net.begin(menu, controller);
    net.setProgressCallback(renderNetProgress);
    net.connect(); // one attempt at boot; no automatic retries afterward

    // Arm the task watchdog AFTER the intentional boot-time blocking.
    esp_task_wdt_config_t wdtCfg = {
        .timeout_ms = WDT_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    if (esp_task_wdt_init(&wdtCfg) == ESP_ERR_INVALID_STATE)
        esp_task_wdt_reconfigure(&wdtCfg); // already inited by the Arduino core
    esp_task_wdt_add(NULL);

    Serial.println("Ready.");
}

void loop()
{
    const unsigned long loopStart = millis();

    // Update sensors and outputs once per interval.
    if (loopStart - lastSensorRead >= SENSOR_INTERVAL_MS)
    {
        lastSensorRead = loopStart;

        sensors.read();

        const Setpoints sp = menu.setpoints();
        controller.update(sp, sensors.readings());
        menu.setHalted(controller.state().halted());
        menu.setRunElapsed((long)((millis() - controller.runStartMs()) / 60000UL));
        menu.requestRedraw(); // refresh display with new data

        const SensorReadings &s = sensors.readings();
        Serial.printf("Temp: %.2f C (set %.1f) | Humid: %.1f%% (set %.0f) | Fan: %u%%\n",
                      s.dsTemp, sp.targetTemp, s.humidity, sp.targetHumidity,
                      controller.state().fanDuty);

        net.publishTelemetry(s); // live chart stream
        net.publishState();      // retained ground-truth for the UI
    }

    // Read the button (short press = tap, 3 s hold = stop).
    menu.poll();

    // Hold-to-stop: shut everything off until the user restarts.
    if (menu.consumeStop())
    {
        controller.stop();
        menu.setHalted(true);
        menu.requestRedraw();
    }

    // Handle a (re)start request from the UI (chamber halted).
    if (menu.consumeRestart())
    {
        controller.restart();
        menu.setHalted(false);
        menu.setHeaterOverride(0); // a fresh run starts in full AUTO
        menu.setHumidOverride(0);
        menu.requestRedraw();
    }

    // Apply the menu's actuator-override intents (encoder + MQTT both write them)
    // onto the Controller.
    applyOverride(menu.heaterOverride(), controller.state().heaterOverride,
                  &Controller::setHeaterOverride, &Controller::clearHeaterOverride);
    applyOverride(menu.humidOverride(), controller.state().humidOverride,
                  &Controller::setHumidifierOverride, &Controller::clearHumidifierOverride);

    // Redraw only when something changed (saves flicker).
    if (menu.consumeRedraw())
    {
        display.render(menu, menu.setpoints(), sensors.readings(),
                       controller.state(), controller.runStartMs(), net.status());
    }

    // Track network status + pump MQTT (never auto-retries); redraw on change.
    if (net.loop())
        menu.requestRedraw();

    // A single push on the Network page toggles connect/retry vs. disconnect.
    if (menu.consumeNetworkAction())
    {
        if (net.isConnected())
            net.disconnect();
        else
            net.connect();
    }

    esp_task_wdt_reset(); // pet the watchdog: a clean loop iteration completed
}
