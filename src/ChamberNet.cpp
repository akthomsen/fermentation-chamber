#include "ChamberNet.h"

#include <ArduinoJson.h>
#include <esp_task_wdt.h>

#include "Config.h"
#include "Controller.h"
#include "MenuController.h"
#include "Secrets.h"

// One-shot connect bound: WiFi association may take a few seconds. This is the
// only place the firmware blocks on the network, and only at boot / user retry.
static constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 8000;
static constexpr size_t MQTT_BUFFER_SIZE = 640;

// Topics. Telemetry (MQTT_TOPIC, from Secrets.h) is the live, un-retained stream
// the chart consumes every point of. State is retained/latest-only so a freshly
// opened UI always gets current setpoints + actuator status. Commands are
// transient intents (never retained) so a reboot is not force-fed stale ones.
static constexpr char TOPIC_STATE[] = "fermenter/state";
static constexpr char TOPIC_CMD_SETPOINT[] = "fermenter/cmd/setpoint";
static constexpr char TOPIC_CMD_CONTROL[] = "fermenter/cmd/control";

namespace
{
const char *mqttStateName(int state)
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

const char *controlSensorName(ControlSensor sensor)
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

bool readControlSensor(JsonObjectConst obj, const char *key, ControlSensor &out)
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

bool readFloat(JsonObjectConst obj, const char *key, float &out)
{
    JsonVariantConst v = obj[key];
    if (!v.is<float>() && !v.is<int>())
        return false;
    out = v.as<float>();
    return isfinite(out);
}

bool readLong(JsonObjectConst obj, const char *key, long &out)
{
    JsonVariantConst v = obj[key];
    if (!v.is<long>() && !v.is<int>())
        return false;
    out = v.as<long>();
    return true;
}

bool readInt(JsonObjectConst obj, const char *key, int &out)
{
    JsonVariantConst v = obj[key];
    if (!v.is<int>())
        return false;
    out = v.as<int>();
    return true;
}
} // namespace

ChamberNet *ChamberNet::instance_ = nullptr;

void ChamberNet::begin(MenuController &menu, Controller &controller)
{
    instance_ = this;
    menu_ = &menu;
    controller_ = &controller;

    mqtt_.setServer(MQTT_BROKER_IP, MQTT_PORT);
    mqtt_.setSocketTimeout(2);
    if (!mqtt_.setBufferSize(MQTT_BUFFER_SIZE))
        Serial.println("MQTT buffer resize failed");
    mqtt_.setCallback(onMessageTramp);
}

void ChamberNet::onMessageTramp(char *topic, byte *payload, unsigned int len)
{
    if (instance_)
        instance_->handleMessage(topic, payload, len);
}

// Recompute the cached connectivity snapshot from the live WiFi/MQTT state.
// Pure: only writes status_ (no network I/O). The detail string carries the
// exact failure reason for the OLED.
void ChamberNet::computeStatus()
{
    status_.wifiConnected = WiFi.status() == WL_CONNECTED;
    status_.mqttConnected = status_.wifiConnected && mqtt_.connected();

    if (status_.wifiConnected)
    {
        const String ip = WiFi.localIP().toString();
        strncpy(status_.ip, ip.c_str(), sizeof(status_.ip) - 1);
        status_.ip[sizeof(status_.ip) - 1] = '\0';
    }
    else
    {
        strcpy(status_.ip, "-");
    }

    if (!status_.wifiConnected)
        snprintf(status_.detail, sizeof(status_.detail), "WiFi down");
    else if (status_.mqttConnected)
        snprintf(status_.detail, sizeof(status_.detail), "connected");
    else
        snprintf(status_.detail, sizeof(status_.detail), "rc%d %s",
                 mqtt_.state(), mqttStateName(mqtt_.state()));
}

// One-shot bring-up of the whole stack (WiFi then MQTT). It blocks while
// connecting -- but only ever at boot or on an explicit user retry -- and paints
// progress on the OLED so the wait is visible. Nothing in steady state blocks on
// the network, so chamber control and the encoder UI keep running with the broker
// down.
void ChamberNet::connect()
{
    status_.connecting = true;

    if (WiFi.status() != WL_CONNECTED)
    {
        snprintf(status_.detail, sizeof(status_.detail), "WiFi...");
        renderProgress();

        Serial.println("WiFi connect attempt");
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(false); // one-shot: no silent background reconnects
        WiFi.begin(WIFI_SSID, WIFI_PASS);

        const unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS)
        {
            delay(100);
            esp_task_wdt_reset(); // harmless before the loop task is subscribed
        }
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi connect FAILED");
        status_.connecting = false;
        computeStatus();
        renderProgress();
        return;
    }
    Serial.printf("WiFi OK: %s RSSI %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());

    status_.wifiConnected = true;
    snprintf(status_.detail, sizeof(status_.detail), "MQTT...");
    renderProgress();

    Serial.printf("MQTT connect attempt %s:%d\n", MQTT_BROKER_IP, MQTT_PORT);
    if (mqtt_.connect("fermenter-c6"))
    {
        Serial.println("MQTT OK");
        mqtt_.subscribe(TOPIC_CMD_SETPOINT);
        mqtt_.subscribe(TOPIC_CMD_CONTROL);
        publishState(); // seed retained state for any subscriber
    }
    else
    {
        Serial.printf("MQTT fail rc=%d (%s)\n", mqtt_.state(), mqttStateName(mqtt_.state()));
    }

    status_.connecting = false;
    computeStatus();
    renderProgress();
}

void ChamberNet::disconnect()
{
    Serial.println("Network disconnect (user)");
    mqtt_.disconnect();
    WiFi.disconnect(true); // drop the association and power the radio down
    WiFi.mode(WIFI_OFF);
    status_.connecting = false;
    computeStatus();
    renderProgress();
}

bool ChamberNet::loop()
{
    const bool prevWifi = status_.wifiConnected;
    const bool prevMqtt = status_.mqttConnected;
    computeStatus();
    const bool changed = (status_.wifiConnected != prevWifi) || (status_.mqttConnected != prevMqtt);
    if (changed)
        Serial.printf("[net] wifi=%d mqtt=%d (%s)\n",
                      status_.wifiConnected, status_.mqttConnected, status_.detail);
    if (status_.mqttConnected)
        mqtt_.loop();
    return changed;
}

// Publish the device's real setpoints + actuator state (retained). This is the
// ground truth the UI mirrors -- it reflects encoder changes too, not just the
// last thing the UI sent.
void ChamberNet::publishState()
{
    if (!mqtt_.connected())
        return;

    const Setpoints sp = menu_->setpoints();
    const ActuatorState &a = controller_->state();
    const char *heaterOverride = a.heaterOverride > 0 ? "on" : (a.heaterOverride < 0 ? "off" : "auto");
    const char *humidOverride = a.humidOverride > 0 ? "on" : (a.humidOverride < 0 ? "off" : "auto");
    char controlTemp[16];
    if (isfinite(a.controlTemp))
        snprintf(controlTemp, sizeof(controlTemp), "%.1f", a.controlTemp);
    else
        snprintf(controlTemp, sizeof(controlTemp), "null");

    // Run-timer readouts the UI mirrors. Time only accrues while a run is live;
    // a not-started/stopped chamber reports zero elapsed and the full limit left.
    long upMinutes = 0;
    long leftMinutes = sp.runMinutes;
    if (!a.notStarted && !a.stopped)
    {
        upMinutes = (long)((millis() - controller_->runStartMs()) / 60000UL);
        leftMinutes = sp.runMinutes > 0 ? sp.runMinutes - upMinutes : 0;
        if (a.runComplete)
        {
            upMinutes = sp.runMinutes;
            leftMinutes = 0;
        }
        if (leftMinutes < 0)
            leftMinutes = 0;
    }

    char buf[600];
    const int n = snprintf(buf, sizeof(buf),
                           "{\"targetTemp\":%.1f,\"targetHumidity\":%.0f,\"targetCeiling\":%.1f,"
                           "\"dsMaxOverTarget\":%.1f,\"controlSensor\":\"%s\",\"controlTemp\":%s,"
                           "\"runMinutes\":%ld,\"upMinutes\":%ld,\"leftMinutes\":%ld,"
                           "\"fanManualPct\":%d,\"fanDuty\":%u,"
                           "\"fanHeatPct\":%d,\"fanHumidPct\":%d,\"fanAutoPct\":%d,"
                           "\"heaterOn\":%s,\"heaterOverride\":\"%s\",\"humidOn\":%s,"
                           "\"humidOverride\":\"%s\",\"halted\":%s,\"runComplete\":%s,"
                           "\"hysteresis\":%.1f,\"fanAfterHeatSec\":%ld,"
                           "\"maxHeatMin\":%ld,\"heatCooldownMin\":%ld}",
                           sp.targetTemp, sp.targetHumidity, sp.targetCeiling,
                           sp.dsMaxOverTarget, controlSensorName(a.controlSensor), controlTemp,
                           sp.runMinutes, upMinutes, leftMinutes,
                           sp.fanManualPct, a.fanDuty,
                           sp.fanHeatPct, sp.fanHumidPct, sp.fanAutoPct,
                           a.heaterOn ? "true" : "false", heaterOverride,
                           a.humidOn ? "true" : "false",
                           humidOverride,
                           a.halted() ? "true" : "false",
                           a.runComplete ? "true" : "false",
                           sp.hysteresis, sp.fanAfterHeatSec,
                           sp.maxHeatMin, sp.heatCooldownMin);
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
    if (!mqtt_.publish(TOPIC_STATE, buf, true))
        Serial.println("State publish failed");
}

void ChamberNet::publishTelemetry(const SensorReadings &s)
{
    if (!mqtt_.connected())
        return;

    // Compact hand-formatted JSON (avoids pulling ArduinoJson into the hot path).
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ts\":%lu,\"dsTemp\":%.2f,\"bmeTemp\":%.2f,\"humidity\":%.1f,\"pressure\":%.1f}",
             millis(), s.dsTemp, s.bmeTemp, s.humidity, s.pressure);
    if (!mqtt_.publish(MQTT_TOPIC, buf))
        Serial.println("Telemetry publish failed");
}

// Handle inbound commands. Runs in loop() context (via mqtt_.loop()), not an ISR,
// so it may touch menu/controller freely. Everything from the network is
// untrusted: clamp setpoints at the door even though Controller also enforces the
// ceiling at runtime -- defense in depth, it drives a heater. Overrides are routed
// through the menu (single source of truth); main reconciles them onto the
// Controller.
void ChamberNet::handleMessage(char *topic, byte *payload, unsigned int len)
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
            menu_->setTargetTemp(constrain(f, TEMP_VALID_MIN, DEFAULT_CEILING));
            handled = true;
        }
        if (readFloat(obj, "targetHumidity", f))
        {
            menu_->setTargetHumidity(constrain(f, 0.0f, 100.0f));
            handled = true;
        }
        if (readFloat(obj, "targetCeiling", f))
        {
            menu_->setTargetCeiling(constrain(f, TEMP_VALID_MIN, TEMP_VALID_MAX));
            handled = true;
        }
        if (readFloat(obj, "dsMaxOverTarget", f))
        {
            menu_->setDsMaxOverTarget(constrain(f, 0.0f, TEMP_VALID_MAX));
            handled = true;
        }
        if (readControlSensor(obj, "controlSensor", sensor))
        {
            menu_->setControlSensor(sensor);
            handled = true;
        }
        if (readInt(obj, "fanManualPct", i))
        {
            const int pct = constrain(i, FAN_MANUAL_AUTO, FAN_DUTY_MAX_PCT);
            menu_->setFanManualPct(pct);
            controller_->setFanSpeed(pct);
            handled = true;
        }
        if (readInt(obj, "fanHeatPct", i))
        {
            menu_->setFanHeatPct(i); // clamps 0..100
            handled = true;
        }
        if (readInt(obj, "fanHumidPct", i))
        {
            menu_->setFanHumidPct(i);
            handled = true;
        }
        if (readInt(obj, "fanAutoPct", i))
        {
            menu_->setFanAutoPct(i);
            handled = true;
        }
        if (readLong(obj, "runMinutes", l))
        {
            const long minutes = l < 0 ? 0 : l;
            menu_->setRunMinutes(minutes);
            controller_->setRunLimit(minutes);
            handled = true;
        }
        // Advanced control tuning (the menu setters clamp to safe ranges).
        if (readFloat(obj, "hysteresis", f))
        {
            menu_->setHysteresis(f);
            handled = true;
        }
        if (readLong(obj, "fanAfterHeatSec", l))
        {
            menu_->setFanAfterHeatSec(l);
            handled = true;
        }
        if (readLong(obj, "maxHeatMin", l))
        {
            menu_->setMaxHeatMin(l);
            handled = true;
        }
        if (readLong(obj, "heatCooldownMin", l))
        {
            menu_->setHeatCooldownMin(l);
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
            controller_->stop();
            menu_->setHalted(true);
            handled = true;
        }
        else if (action && strcmp(action, "start") == 0)
        {
            controller_->restart();
            menu_->setHalted(false);
            menu_->setHeaterOverride(0); // a fresh run starts in full AUTO
            menu_->setHumidOverride(0);
            handled = true;
        }
        // Overrides are intents owned by the menu (single source of truth); main
        // reconciles them onto the Controller. -1 = off, 0 = auto, 1 = on. These
        // do not halt the run.
        else if (action && strcmp(action, "humidifier_off") == 0)
        {
            menu_->setHumidOverride(-1);
            handled = true;
        }
        else if (action && strcmp(action, "humidifier_on") == 0)
        {
            menu_->setHumidOverride(1);
            handled = true;
        }
        else if (action && strcmp(action, "humidifier_auto") == 0)
        {
            menu_->setHumidOverride(0);
            handled = true;
        }
        else if (action && strcmp(action, "heater_off") == 0)
        {
            menu_->setHeaterOverride(-1);
            handled = true;
        }
        else if (action && strcmp(action, "heater_on") == 0)
        {
            menu_->setHeaterOverride(1);
            handled = true;
        }
        else if (action && strcmp(action, "heater_auto") == 0)
        {
            menu_->setHeaterOverride(0);
            handled = true;
        }
        else if (action && strcmp(action, "fan_speed") == 0)
        {
            int pctRaw = 0;
            if (readInt(obj, "speed", pctRaw))
            {
                const int pct = constrain(pctRaw, FAN_MANUAL_AUTO, FAN_DUTY_MAX_PCT);
                menu_->setFanManualPct(pct);
                controller_->setFanSpeed(pct);
                handled = true;
            }
        }
        else if (action && strcmp(action, "run_limit") == 0)
        {
            long rawMinutes = 0;
            if (readLong(obj, "minutes", rawMinutes))
            {
                const long minutes = rawMinutes < 0 ? 0 : rawMinutes;
                menu_->setRunMinutes(minutes);
                controller_->setRunLimit(minutes);
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

    menu_->requestRedraw(); // OLED reflects the remote change too
    publishState();         // echo new truth immediately
}
