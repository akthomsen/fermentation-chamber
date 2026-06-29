#!/usr/bin/env python3
"""Minimal MQTT subscriber for the fermentation-chamber telemetry POC.

Runs on the same laptop as the Mosquitto broker (hence localhost). Prints each
telemetry message as it arrives. Persisting (CSV/SQLite) is a trivial later step.

    pip install paho-mqtt
    python3 tools/subscriber.py
"""
import json
import paho.mqtt.client as mqtt


def on_connect(client, userdata, flags, rc, props=None):
    print("connected rc", rc)
    client.subscribe("fermenter/telemetry")


def on_message(client, userdata, msg):
    data = json.loads(msg.payload)
    print(f"{data['dsTemp']:.2f}C  {data['humidity']:.1f}%  {data['pressure']:.1f}hPa")


def main():
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    c.on_connect = on_connect
    c.on_message = on_message
    c.connect("localhost", 1883)
    c.loop_forever()


if __name__ == "__main__":
    main()
