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
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "Config.h"
#include "Sensors.h"
#include "Controller.h"
#include "MenuController.h"
#include "Display.h"
#include "Secrets.h"

static Sensors sensors;
static Controller controller;
static MenuController menu;
static Display display;

static WiFiClient net;
static PubSubClient mqtt(net);

static unsigned long lastSensorRead = 0;

// Connect to WiFi, blocking until associated.
static void connectWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(300);
        Serial.print(".");
    }
    Serial.printf("\nWiFi OK: %s\n", WiFi.localIP().toString().c_str());
}

// Topics. Telemetry (MQTT_TOPIC, from Secrets.h) is the live, un-retained
// stream the chart consumes every point of. State is retained/latest-only so a
// freshly opened UI always gets current setpoints + actuator status. Commands
// are transient intents (never retained) so a reboot is not force-fed stale ones.
constexpr char TOPIC_STATE[] = "fermenter/state";
constexpr char TOPIC_CMD_SETPOINT[] = "fermenter/cmd/setpoint";
constexpr char TOPIC_CMD_CONTROL[] = "fermenter/cmd/control";

// Publish the device's real setpoints + actuator state (retained). This is the
// ground truth the UI mirrors -- it reflects encoder changes too, not just the
// last thing the UI sent.
static void publishState()
{
    const Setpoints sp = menu.setpoints();
    const ActuatorState &a = controller.state();
    char buf[224];
    snprintf(buf, sizeof(buf),
             "{\"targetTemp\":%.1f,\"targetHumidity\":%.0f,\"targetCeiling\":%.1f,"
             "\"fanDuty\":%u,\"heaterOn\":%s,\"humidOn\":%s,\"halted\":%s}",
             sp.targetTemp, sp.targetHumidity, sp.targetCeiling, a.fanDuty,
             a.heaterOn ? "true" : "false", a.humidOn ? "true" : "false",
             a.halted() ? "true" : "false");
    mqtt.publish(TOPIC_STATE, buf, true); // retained
}

// (Re)establish the MQTT connection, blocking until connected.
static void mqttReconnect()
{
    while (!mqtt.connected())
    {
        if (mqtt.connect("fermenter-c6"))
        {
            Serial.println("MQTT OK");
            mqtt.subscribe(TOPIC_CMD_SETPOINT);
            mqtt.subscribe(TOPIC_CMD_CONTROL);
            publishState(); // seed retained state for any subscriber
        }
        else
        {
            Serial.printf("MQTT fail rc=%d\n", mqtt.state());
            delay(1000);
        }
    }
}

// Handle inbound commands. Runs in loop() context (via mqtt.loop()), not an
// ISR, so it may touch menu/controller freely. Everything from the network is
// untrusted: clamp setpoints at the door even though Controller also enforces
// the ceiling at runtime -- defense in depth, it drives a heater.
static void onMqtt(char *topic, byte *payload, unsigned int len)
{
    JsonDocument doc;
    if (deserializeJson(doc, payload, len))
        return; // ignore malformed JSON

    if (strcmp(topic, TOPIC_CMD_SETPOINT) == 0)
    {
        if (doc["targetTemp"].is<float>())
            menu.setTargetTemp(constrain(doc["targetTemp"].as<float>(),
                                         TEMP_VALID_MIN, DEFAULT_CEILING));
        if (doc["targetHumidity"].is<float>())
            menu.setTargetHumidity(constrain(doc["targetHumidity"].as<float>(), 0.0f, 100.0f));
    }
    else if (strcmp(topic, TOPIC_CMD_CONTROL) == 0)
    {
        // Mirror the physical knob: stop halts everything until a fresh start;
        // start = restart() (resets the run timer), exactly like the button.
        const char *action = doc["action"];
        if (action && strcmp(action, "stop") == 0)
        {
            controller.stop();
            menu.setHalted(true);
        }
        else if (action && strcmp(action, "start") == 0)
        {
            controller.restart();
            menu.setHalted(false);
        }
    }

    menu.requestRedraw(); // OLED reflects the remote change too
    publishState();       // echo new truth immediately
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
    Wire.begin(PIN_SDA, PIN_SCL);

    connectWiFi();
    mqtt.setServer(MQTT_BROKER_IP, MQTT_PORT);
    mqtt.setCallback(onMqtt);

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
    // Keep the MQTT link alive and service its buffers each iteration.
    if (!mqtt.connected())
        mqttReconnect();
    mqtt.loop();

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
        Serial.printf("Temp: %.2f C (set %.1f) | Humid: %.1f%% (set %.0f) | Fan: %u%%\n",
                      s.dsTemp, sp.targetTemp, s.humidity, sp.targetHumidity,
                      controller.state().fanDuty);

        // Publish telemetry as compact JSON (hand-formatted to avoid pulling in
        // ArduinoJson for the POC; fits PubSubClient's default 256-byte buffer).
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"ts\":%lu,\"dsTemp\":%.2f,\"bmeTemp\":%.2f,\"humidity\":%.1f,\"pressure\":%.1f}",
                 millis(), s.dsTemp, s.bmeTemp, s.humidity, s.pressure);
        mqtt.publish(MQTT_TOPIC, buf);

        // Echo current setpoints + actuator state (retained) so the UI tracks
        // ground truth, including changes made on the physical encoder.
        publishState();
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
