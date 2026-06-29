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
static unsigned long lastUserActivity = 0;
static unsigned long lastWiFiAttempt = 0;
static unsigned long lastMqttAttempt = 0;
static wl_status_t lastWiFiStatus = WL_IDLE_STATUS;
static bool mqttWasConnected = false;
static bool wifiBeginCalled = false;

constexpr unsigned long WIFI_RETRY_MS = 10000;
constexpr unsigned long MQTT_RETRY_MS = 30000;
constexpr unsigned long MQTT_USER_IDLE_MS = 3000;

enum NetworkAlert : uint8_t
{
    NETWORK_ALERT_NONE,
    NETWORK_ALERT_WIFI,
    NETWORK_ALERT_MQTT,
};

static NetworkAlert lastNetworkAlert = NETWORK_ALERT_WIFI;

static const char *networkAlertText(NetworkAlert alert)
{
    switch (alert)
    {
    case NETWORK_ALERT_WIFI:
        return "WiFi!";
    case NETWORK_ALERT_MQTT:
        return "MQTT!";
    default:
        return "";
    }
}

static NetworkAlert currentNetworkAlert()
{
    if (WiFi.status() != WL_CONNECTED)
        return NETWORK_ALERT_WIFI;
    if (!mqtt.connected())
        return NETWORK_ALERT_MQTT;
    return NETWORK_ALERT_NONE;
}

static const char *mqttStateName(int state)
{
    switch (state)
    {
    case MQTT_CONNECTION_TIMEOUT:
        return "connection timeout";
    case MQTT_CONNECTION_LOST:
        return "connection lost";
    case MQTT_CONNECT_FAILED:
        return "tcp connect failed";
    case MQTT_DISCONNECTED:
        return "disconnected";
    case MQTT_CONNECTED:
        return "connected";
    case MQTT_CONNECT_BAD_PROTOCOL:
        return "bad protocol";
    case MQTT_CONNECT_BAD_CLIENT_ID:
        return "bad client id";
    case MQTT_CONNECT_UNAVAILABLE:
        return "server unavailable";
    case MQTT_CONNECT_BAD_CREDENTIALS:
        return "bad credentials";
    case MQTT_CONNECT_UNAUTHORIZED:
        return "unauthorized";
    default:
        return "unknown";
    }
}

static void noteNetworkAlertChange()
{
    const NetworkAlert alert = currentNetworkAlert();
    if (alert != lastNetworkAlert)
    {
        lastNetworkAlert = alert;
        menu.requestRedraw();
    }
}

// Start or retry WiFi without blocking chamber control.
static void serviceWiFi()
{
    const unsigned long now = millis();
    const wl_status_t status = WiFi.status();

    if (status != lastWiFiStatus)
    {
        lastWiFiStatus = status;
        if (status == WL_CONNECTED)
        {
            Serial.printf("WiFi OK: %s RSSI %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
        }
        else
        {
            Serial.printf("WiFi status=%d\n", status);
        }
        noteNetworkAlertChange();
    }

    if (status == WL_CONNECTED)
        return;

    if (!wifiBeginCalled || now - lastWiFiAttempt >= WIFI_RETRY_MS)
    {
        wifiBeginCalled = true;
        lastWiFiAttempt = now;
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        Serial.println("WiFi connect attempt");
        noteNetworkAlertChange();
    }
}

// Topics. Telemetry (MQTT_TOPIC, from Secrets.h) is the live, un-retained
// stream the chart consumes every point of. State is retained/latest-only so a
// freshly opened UI always gets current setpoints + actuator status. Commands
// are transient intents (never retained) so a reboot is not force-fed stale ones.
constexpr char TOPIC_STATE[] = "fermenter/state";
constexpr char TOPIC_CMD_SETPOINT[] = "fermenter/cmd/setpoint";
constexpr char TOPIC_CMD_CONTROL[] = "fermenter/cmd/control";
constexpr size_t MQTT_BUFFER_SIZE = 640;

static const char *controlSensorName(ControlSensor sensor)
{
    switch (sensor)
    {
    case CONTROL_SENSOR_DS:
        return "ds";
    case CONTROL_SENSOR_BME:
        return "bme";
    case CONTROL_SENSOR_AVERAGE:
        return "average";
    default:
        return "ds";
    }
}

static bool readControlSensor(JsonObjectConst obj, const char *key, ControlSensor &out)
{
    JsonVariantConst v = obj[key];
    if (v.is<int>())
    {
        const int raw = v.as<int>();
        if (raw >= 0 && raw < CONTROL_SENSOR_COUNT)
        {
            out = (ControlSensor)raw;
            return true;
        }
        return false;
    }

    const char *s = v.as<const char *>();
    if (!s)
        return false;
    if (strcmp(s, "ds") == 0)
        out = CONTROL_SENSOR_DS;
    else if (strcmp(s, "bme") == 0)
        out = CONTROL_SENSOR_BME;
    else if (strcmp(s, "average") == 0 || strcmp(s, "avg") == 0)
        out = CONTROL_SENSOR_AVERAGE;
    else
        return false;
    return true;
}

static bool readFloat(JsonObjectConst obj, const char *key, float &out)
{
    JsonVariantConst v = obj[key];
    if (!v.is<float>() && !v.is<int>())
        return false;
    out = v.as<float>();
    return isfinite(out);
}

static bool readLong(JsonObjectConst obj, const char *key, long &out)
{
    JsonVariantConst v = obj[key];
    if (!v.is<long>() && !v.is<int>())
        return false;
    out = v.as<long>();
    return true;
}

static bool readInt(JsonObjectConst obj, const char *key, int &out)
{
    JsonVariantConst v = obj[key];
    if (!v.is<int>())
        return false;
    out = v.as<int>();
    return true;
}

// Publish the device's real setpoints + actuator state (retained). This is the
// ground truth the UI mirrors -- it reflects encoder changes too, not just the
// last thing the UI sent.
static void publishState()
{
    if (!mqtt.connected())
        return;

    const Setpoints sp = menu.setpoints();
    const ActuatorState &a = controller.state();
    const char *heaterOverride = a.heaterOverride > 0 ? "on" : (a.heaterOverride < 0 ? "off" : "auto");
    const char *humidOverride = a.humidOverride > 0 ? "on" : (a.humidOverride < 0 ? "off" : "auto");
    char controlTemp[16];
    if (isfinite(a.controlTemp))
        snprintf(controlTemp, sizeof(controlTemp), "%.1f", a.controlTemp);
    else
        snprintf(controlTemp, sizeof(controlTemp), "null");
    char buf[512];
    const int n = snprintf(buf, sizeof(buf),
                           "{\"targetTemp\":%.1f,\"targetHumidity\":%.0f,\"targetCeiling\":%.1f,"
                           "\"dsMaxOverTarget\":%.1f,\"controlSensor\":\"%s\",\"controlTemp\":%s,"
                           "\"runMinutes\":%ld,\"fanManualPct\":%d,\"fanDuty\":%u,"
                           "\"heaterOn\":%s,\"heaterOverride\":\"%s\",\"humidOn\":%s,"
                           "\"humidOverride\":\"%s\",\"halted\":%s}",
                           sp.targetTemp, sp.targetHumidity, sp.targetCeiling,
                           sp.dsMaxOverTarget, controlSensorName(a.controlSensor), controlTemp,
                           sp.runMinutes, sp.fanManualPct, a.fanDuty,
                           a.heaterOn ? "true" : "false", heaterOverride,
                           a.humidOn ? "true" : "false",
                           humidOverride,
                           a.halted() ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(buf))
    {
        Serial.println("State publish skipped: JSON buffer too small");
        return;
    }
    if ((size_t)n + strlen(TOPIC_STATE) + 8 >= MQTT_BUFFER_SIZE)
    {
        Serial.println("State publish skipped: MQTT buffer too small");
        return;
    }
    if (!mqtt.publish(TOPIC_STATE, buf, true))
        Serial.println("State publish failed");
}

// Try to establish MQTT once per retry interval. MQTT must never block the
// chamber loop; local controls and safety continue while the broker is down.
static void serviceMqtt(bool allowConnect)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        if (mqttWasConnected)
        {
            mqttWasConnected = false;
            Serial.println("MQTT offline: WiFi down");
            noteNetworkAlertChange();
        }
        return;
    }

    if (mqtt.connected())
    {
        mqtt.loop();
        if (!mqttWasConnected)
        {
            mqttWasConnected = true;
            noteNetworkAlertChange();
        }
        return;
    }

    if (mqttWasConnected)
    {
        mqttWasConnected = false;
        Serial.printf("MQTT disconnected rc=%d (%s)\n", mqtt.state(), mqttStateName(mqtt.state()));
        noteNetworkAlertChange();
    }

    if (!allowConnect)
        return;

    const unsigned long now = millis();
    if (lastMqttAttempt != 0 && now - lastMqttAttempt < MQTT_RETRY_MS)
        return;
    lastMqttAttempt = now;

    Serial.printf("MQTT connect attempt %s:%d\n", MQTT_BROKER_IP, MQTT_PORT);
    if (mqtt.connect("fermenter-c6"))
    {
        mqttWasConnected = true;
        Serial.println("MQTT OK");
        mqtt.subscribe(TOPIC_CMD_SETPOINT);
        mqtt.subscribe(TOPIC_CMD_CONTROL);
        publishState(); // seed retained state for any subscriber
    }
    else
    {
        Serial.printf("MQTT fail rc=%d (%s)\n", mqtt.state(), mqttStateName(mqtt.state()));
    }
    noteNetworkAlertChange();
}

// Handle inbound commands. Runs in loop() context (via mqtt.loop()), not an
// ISR, so it may touch menu/controller freely. Everything from the network is
// untrusted: clamp setpoints at the door even though Controller also enforces
// the ceiling at runtime -- defense in depth, it drives a heater.
static void onMqtt(char *topic, byte *payload, unsigned int len)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err)
    {
        Serial.printf("MQTT ignored malformed JSON on %s: %s\n", topic, err.c_str());
        return;
    }

    if (!doc.is<JsonObject>())
    {
        Serial.printf("MQTT ignored non-object JSON on %s\n", topic);
        return;
    }

    JsonObjectConst obj = doc.as<JsonObjectConst>();
    bool handled = false;

    if (strcmp(topic, TOPIC_CMD_SETPOINT) == 0)
    {
        float f = 0.0f;
        long l = 0;
        int i = 0;
        ControlSensor sensor = CONTROL_SENSOR_DS;

        if (readFloat(obj, "targetTemp", f))
        {
            menu.setTargetTemp(constrain(f, TEMP_VALID_MIN, DEFAULT_CEILING));
            handled = true;
        }
        if (readFloat(obj, "targetHumidity", f))
        {
            menu.setTargetHumidity(constrain(f, 0.0f, 100.0f));
            handled = true;
        }
        if (readFloat(obj, "targetCeiling", f))
        {
            menu.setTargetCeiling(constrain(f, TEMP_VALID_MIN, TEMP_VALID_MAX));
            handled = true;
        }
        if (readFloat(obj, "dsMaxOverTarget", f))
        {
            menu.setDsMaxOverTarget(constrain(f, 0.0f, TEMP_VALID_MAX));
            handled = true;
        }
        if (readControlSensor(obj, "controlSensor", sensor))
        {
            menu.setControlSensor(sensor);
            handled = true;
        }
        if (readInt(obj, "fanManualPct", i))
        {
            const int pct = constrain(i, FAN_MANUAL_AUTO, FAN_DUTY_MAX_PCT);
            menu.setFanManualPct(pct);
            controller.setFanSpeed(pct);
            handled = true;
        }
        if (readLong(obj, "runMinutes", l))
        {
            const long minutes = l < 0 ? 0 : l;
            menu.setRunMinutes(minutes);
            controller.setRunLimit(minutes);
            handled = true;
        }
    }
    else if (strcmp(topic, TOPIC_CMD_CONTROL) == 0)
    {
        // Mirror the physical knob: stop halts everything until a fresh start;
        // start = restart() (resets the run timer), exactly like the button.
        const char *action = obj["action"].as<const char *>();
        if (action && strcmp(action, "stop") == 0)
        {
            controller.stop();
            menu.setHalted(true);
            handled = true;
        }
        else if (action && strcmp(action, "start") == 0)
        {
            controller.restart();
            menu.setHalted(false);
            handled = true;
        }
        // Humidifier override (does not halt the run): auto follows humidity;
        // on/off force the actuator state until changed again.
        else if (action && strcmp(action, "humidifier_off") == 0)
        {
            controller.setHumidifierOverride(false);
            handled = true;
        }
        else if (action && strcmp(action, "humidifier_on") == 0)
        {
            controller.setHumidifierOverride(true);
            handled = true;
        }
        else if (action && strcmp(action, "humidifier_auto") == 0)
        {
            controller.clearHumidifierOverride();
            handled = true;
        }
        // heater action /on/off
        else if (action && strcmp(action, "heater_off") == 0)
        {
            controller.setHeaterOverride(false);
            handled = true;
        }
        else if (action && strcmp(action, "heater_on") == 0)
        {
            controller.setHeaterOverride(true);
            handled = true;
        }
        else if (action && strcmp(action, "heater_auto") == 0)
        {
            controller.clearHeaterOverride();
            handled = true;
        }
        // fan speed set
        else if (action && strcmp(action, "fan_speed") == 0)
        {
            int pctRaw = 0;
            if (readInt(obj, "speed", pctRaw))
            {
                const int pct = constrain(pctRaw, FAN_MANUAL_AUTO, FAN_DUTY_MAX_PCT);
                menu.setFanManualPct(pct);
                controller.setFanSpeed(pct);
                handled = true;
            }
        }
        else if (action && strcmp(action, "run_limit") == 0)
        {
            long rawMinutes = 0;
            if (readLong(obj, "minutes", rawMinutes))
            {
                const long minutes = rawMinutes < 0 ? 0 : rawMinutes;
                menu.setRunMinutes(minutes);
                controller.setRunLimit(minutes);
                handled = true;
            }
        }
    }
    else
    {
        Serial.printf("MQTT ignored unexpected topic: %s\n", topic);
        return;
    }

    if (!handled)
    {
        Serial.printf("MQTT ignored unsupported command on %s\n", topic);
        return;
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

    menu.begin();
    controller.begin();

    waitForDevice(display.begin(), "SSD1306 failed");
    waitForDevice(sensors.begin(), "BME280 not found!");

    sensors.read();
    controller.update(menu.setpoints(), sensors.readings());
    menu.requestRedraw();

    mqtt.setServer(MQTT_BROKER_IP, MQTT_PORT);
    mqtt.setSocketTimeout(2);
    if (!mqtt.setBufferSize(MQTT_BUFFER_SIZE))
        Serial.println("MQTT buffer resize failed");
    mqtt.setCallback(onMqtt);
    serviceWiFi();

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
        if (mqtt.connected() && !mqtt.publish(MQTT_TOPIC, buf))
            Serial.println("Telemetry publish failed");

        // Echo current setpoints + actuator state (retained) so the UI tracks
        // ground truth, including changes made on the physical encoder.
        publishState();
    }

    // Read the button (short press = tap, 5 s hold = stop).
    menu.poll();
    if (menu.consumeActivity())
        lastUserActivity = millis();

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
                       menu.screenLabel(), controller.runStartMs(),
                       networkAlertText(currentNetworkAlert()));
    }

    serviceWiFi();
    const bool uiIdle = !menu.isEditing() && millis() - lastUserActivity >= MQTT_USER_IDLE_MS;
    serviceMqtt(uiIdle);
}
