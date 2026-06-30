import asyncio
import json
import logging
import os
from contextlib import asynccontextmanager

import paho.mqtt.client as mqtt
from fastapi import FastAPI, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.staticfiles import StaticFiles
from influxdb_client import InfluxDBClient
from pydantic import BaseModel

log = logging.getLogger("uvicorn.error")

URL    = os.environ["INFLUX_URL"]
TOKEN  = os.environ["INFLUX_TOKEN"]
ORG    = os.environ["INFLUX_ORG"]
BUCKET = os.environ["INFLUX_BUCKET"]

# The backend speaks MQTT on the browser's behalf. The broker credential lives
# here (from the environment), never in the page that the browser downloads.
MQTT_HOST = os.environ.get("MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USER = os.environ["MQTT_USER"]
MQTT_PASS = os.environ["MQTT_PASS"]

# Read side: topics relayed verbatim to connected browsers.
RELAY_TOPICS = ("fermenter/telemetry", "fermenter/state", "fermenter/alert")


# --- live relay: broker -> browsers -----------------------------------------
# paho runs its network loop on a background thread; FastAPI's WebSockets live
# on the asyncio loop. on_message bridges the two by handing each frame to an
# asyncio.Queue via call_soon_threadsafe; the consume() task fans it out.
class Relay:
    def __init__(self):
        self.client: mqtt.Client | None = None
        self.queue: asyncio.Queue | None = None
        self.clients: set[WebSocket] = set()
        # Last payload per topic, replayed to each new client so it renders
        # immediately instead of waiting for the next broker message. This
        # stands in for MQTT's retained delivery, which the browser no longer
        # receives directly.
        self.cache: dict[str, str] = {}

    async def consume(self):
        assert self.queue is not None
        while True:
            topic, payload = await self.queue.get()
            self.cache[topic] = payload
            frame = json.dumps({"topic": topic, "payload": payload})
            for ws in list(self.clients):
                try:
                    await ws.send_text(frame)
                except Exception:
                    self.clients.discard(ws)


relay = Relay()


@asynccontextmanager
async def lifespan(app: FastAPI):
    loop = asyncio.get_running_loop()
    relay.queue = asyncio.Queue()

    def on_connect(client, userdata, flags, rc):
        if rc == 0:
            for topic in RELAY_TOPICS:
                client.subscribe(topic)
            log.info("mqtt connected, subscribed to %s", ", ".join(RELAY_TOPICS))
        else:
            log.warning("mqtt connect failed rc=%s", rc)

    def on_message(client, userdata, msg):
        try:
            payload = msg.payload.decode()
        except Exception:
            payload = ""
        loop.call_soon_threadsafe(relay.queue.put_nowait, (msg.topic, payload))

    client = mqtt.Client(client_id="ferm-backend")
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=30)
    client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_start()
    relay.client = client

    consumer = asyncio.create_task(relay.consume())
    try:
        yield
    finally:
        consumer.cancel()
        client.loop_stop()
        client.disconnect()


app = FastAPI(lifespan=lifespan)
influx = InfluxDBClient(url=URL, token=TOKEN, org=ORG)


@app.get("/api/history")
def history(hours: int = Query(6, ge=1, le=168)):
    flux = f'''
    from(bucket: "{BUCKET}")
      |> range(start: -{hours}h)
      |> filter(fn: (r) => r._measurement == "telemetry")
      |> filter(fn: (r) => r._field == "dsTemp" or r._field == "bmeTemp" or r._field == "humidity")
      |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
      |> sort(columns: ["_time"])
    '''
    out = {"t": [], "dsTemp": [], "bmeTemp": [], "humidity": []}
    for table in influx.query_api().query(flux):
        for rec in table.records:
            out["t"].append(rec["_time"].timestamp())
            out["dsTemp"].append(rec.values.get("dsTemp"))
            out["bmeTemp"].append(rec.values.get("bmeTemp"))
            out["humidity"].append(rec.values.get("humidity"))
    return out


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws.accept()
    # Replay last-known state/alert/telemetry so a fresh page renders at once.
    for topic in RELAY_TOPICS:
        if topic in relay.cache:
            await ws.send_text(json.dumps({"topic": topic, "payload": relay.cache[topic]}))
    relay.clients.add(ws)
    try:
        while True:
            await ws.receive_text()  # browser never sends; blocks until disconnect
    except WebSocketDisconnect:
        pass
    finally:
        relay.clients.discard(ws)


# --- command mediation: browser -> backend -> broker -------------------------
# Every command crosses this checkpoint, where it is range-clamped and
# whitelisted before being published. The device clamps too; this is
# defence-in-depth and the natural seam for a future safety-property layer.
CONTROL_ACTIONS = {
    "heater_auto", "heater_on", "heater_off",
    "humidifier_auto", "humidifier_on", "humidifier_off",
    "start", "stop",
}

# field -> (kind, lo, hi); ranges mirror the dashboard input bounds.
SETPOINT_FIELDS = {
    "targetTemp":      ("f", 0, 45),
    "targetHumidity":  ("f", 0, 100),
    "targetCeiling":   ("f", 0, 70),
    "dsMaxOverTarget": ("f", 0, 70),
    "hysteresis":      ("f", 0.1, 10),
    "fanAfterHeatSec": ("i", 0, 3600),
    "maxHeatMin":      ("i", 1, 600),
    "heatCooldownMin": ("i", 0, 600),
    "fanHeatPct":      ("i", 0, 100),
    "fanHumidPct":     ("i", 0, 100),
    "fanAutoPct":      ("i", 0, 100),
    "fanManualPct":    ("i", -1, 100),
    "runMinutes":      ("i", 0, 6_000_000),
}


def sanitize_setpoint(data: dict) -> dict:
    out: dict = {}
    for key, value in data.items():
        if key == "controlSensor":
            if value in ("ds", "bme", "average"):
                out[key] = value
            continue
        spec = SETPOINT_FIELDS.get(key)
        if spec is None:
            continue  # silently drop unknown keys
        kind, lo, hi = spec
        try:
            num = float(value)
        except (TypeError, ValueError):
            continue
        if kind == "i":
            out[key] = max(lo, min(hi, int(round(num))))
        else:
            out[key] = max(lo, min(hi, num))
    return out


class Command(BaseModel):
    setpoint: dict | None = None
    controls: list[str] | None = None


@app.post("/api/command")
def command(cmd: Command):
    client = relay.client
    if client is None or not client.is_connected():
        raise HTTPException(503, "broker unavailable")

    # Validate the whole batch before publishing any of it, so a bad action
    # rejects the request atomically instead of half-applying.
    if cmd.controls:
        for action in cmd.controls:
            if action not in CONTROL_ACTIONS:
                raise HTTPException(400, f"unknown action: {action}")

    setpoint = sanitize_setpoint(cmd.setpoint) if cmd.setpoint else {}

    if not setpoint and not cmd.controls:
        raise HTTPException(400, "empty command")

    if setpoint:
        client.publish("fermenter/cmd/setpoint", json.dumps(setpoint))
        log.info("cmd setpoint %s", setpoint)
    for action in cmd.controls or []:
        client.publish("fermenter/cmd/control", json.dumps({"action": action}))
        log.info("cmd control %s", action)

    return {"ok": True}


# Serve the dashboard. Explicit routes above win over this mount.
app.mount("/", StaticFiles(directory="web", html=True), name="web")
