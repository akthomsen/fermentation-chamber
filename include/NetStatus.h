#pragma once

#include <Arduino.h>

// =============================================================================
// NetStatus.h - Connectivity snapshot shared between main (owner) and Display.
// =============================================================================
// (Deliberately NOT named Network.h: that collides with the Arduino framework's
// own Network library header and breaks the WiFi stack include chain.)
// A plain value type: main recomputes it from the live WiFi/MQTT state, Display
// only renders it. Keeping it logic-free means the OLED can always show exactly
// what the network is doing -- including the reason a connection failed -- which
// is the whole point of the manual-retry Network screen.
struct NetworkStatus
{
    bool wifiConnected = false;
    bool mqttConnected = false;
    bool connecting = false;            // an attempt is in progress -> show "Connecting..."
    char ip[16] = "-";                  // device IP while WiFi is up
    char detail[32] = "not connected";  // human-readable status / last failure reason
};
