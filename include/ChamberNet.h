#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "NetStatus.h"
#include "Sensors.h"

class MenuController;
class Controller;

// Owns WiFi + MQTT for the chamber. Connects exactly once on demand (at boot or
// on an explicit user retry) and never auto-retries, so the control loop never
// blocks on the network in steady state. The MenuController is the single source
// of truth for every user intent; inbound MQTT commands are applied through it
// (and through the Controller for start/stop), keeping the encoder and the remote
// UI perfectly in sync.
class ChamberNet
{
public:
    // Wire up dependencies and configure the MQTT client. Call once in setup().
    void begin(MenuController &menu, Controller &controller);

    // Optional hook called to repaint connect progress on the OLED during the one
    // intentional blocking connect, so the wait is always visible.
    void setProgressCallback(void (*cb)()) { progressCb_ = cb; }

    void connect();    // one-shot blocking WiFi+MQTT bring-up (shows progress)
    void disconnect(); // user teardown of the whole stack
    bool loop();       // refresh status + pump MQTT; true if status changed

    bool isConnected() const { return status_.mqttConnected; }
    const NetworkStatus &status() const { return status_; }

    // Publish retained device state (setpoints + actuator truth) and the live
    // telemetry sample. Both no-op when MQTT is down.
    void publishState();
    void publishTelemetry(const SensorReadings &s);

private:
    void computeStatus();
    void renderProgress()
    {
        if (progressCb_)
            progressCb_();
    }
    void handleMessage(char *topic, byte *payload, unsigned int len);
    static void onMessageTramp(char *topic, byte *payload, unsigned int len);

    static ChamberNet *instance_;
    WiFiClient net_;
    PubSubClient mqtt_{net_};
    NetworkStatus status_;
    MenuController *menu_ = nullptr;
    Controller *controller_ = nullptr;
    void (*progressCb_)() = nullptr;
};
