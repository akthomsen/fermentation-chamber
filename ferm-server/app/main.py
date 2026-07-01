import asyncio
import json
import logging
import os
import pathlib
import sqlite3
import time
from contextlib import asynccontextmanager

import paho.mqtt.client as mqtt
from fastapi import FastAPI, HTTPException, Query, WebSocket, WebSocketDisconnect, Request
from fastapi.staticfiles import StaticFiles
from influxdb_client import InfluxDBClient
from pydantic import BaseModel
from pywebpush import WebPushException, webpush

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


# --- web push -----------------------------------------------------------------
# The browser registers once for Web Push; thereafter the backend pushes
# notifications straight to the browser's push service, even with the page shut.
# VAPID is the signing keypair that authorises us to that service: the public
# key is handed to the browser, the private key never leaves here. If the keys
# are absent the app still runs — push endpoints just report "not configured" —
# so the dashboard is never held hostage to notification setup.
VAPID_PUBLIC_KEY  = os.environ.get("VAPID_PUBLIC_KEY", "")
VAPID_PRIVATE_KEY = os.environ.get("VAPID_PRIVATE_KEY", "")
VAPID_SUBJECT     = os.environ.get("VAPID_SUBJECT", "mailto:admin@example.com")
PUSH_ENABLED      = bool(VAPID_PUBLIC_KEY and VAPID_PRIVATE_KEY)
PUSH_DB_PATH      = os.environ.get("PUSH_DB_PATH", "/app/data/app.db")


def _db() -> sqlite3.Connection:
    # A fresh connection per call keeps this trivially thread-safe: the sync
    # endpoints run in FastAPI's threadpool and the alert path runs in an
    # executor, so there is no single connection to share across threads.
    pathlib.Path(PUSH_DB_PATH).parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(PUSH_DB_PATH)
    conn.execute(
        "CREATE TABLE IF NOT EXISTS subscriptions("
        "endpoint TEXT PRIMARY KEY, sub TEXT NOT NULL, created REAL NOT NULL)"
    )
    return conn


def save_subscription(sub: dict) -> None:
    # Keyed by endpoint so re-subscribing the same browser updates in place
    # rather than piling up duplicates.
    with _db() as conn:
        conn.execute(
            "INSERT INTO subscriptions(endpoint, sub, created) VALUES(?, ?, ?) "
            "ON CONFLICT(endpoint) DO UPDATE SET sub=excluded.sub",
            (sub["endpoint"], json.dumps(sub), time.time()),
        )


def delete_subscription(endpoint: str) -> None:
    with _db() as conn:
        conn.execute("DELETE FROM subscriptions WHERE endpoint=?", (endpoint,))


def load_subscriptions() -> list[dict]:
    with _db() as conn:
        rows = conn.execute("SELECT sub FROM subscriptions").fetchall()
    return [json.loads(r[0]) for r in rows]


def _send_one(sub: dict, payload: dict) -> bool:
    try:
        webpush(
            subscription_info=sub,
            data=json.dumps(payload),
            vapid_private_key=VAPID_PRIVATE_KEY,
            vapid_claims={"sub": VAPID_SUBJECT},
        )
        return True
    except WebPushException as e:
        # 404/410 mean the browser dropped the subscription (uninstalled, expired
        # push, cleared data). Prune it so it stops counting as a failure.
        status = getattr(e.response, "status_code", None)
        if status in (404, 410):
            delete_subscription(sub.get("endpoint", ""))
            log.info("push subscription gone (%s), removed", status)
        else:
            log.warning("push failed: %s", e)
        return False


def send_push_to_all(payload: dict) -> dict:
    """Blocking: fan a notification out to every stored subscription. Called
    from the threadpool (endpoints) or an executor (alert path), never on the
    event loop."""
    if not PUSH_ENABLED:
        log.warning("push requested but VAPID keys are not configured")
        return {"sent": 0, "failed": 0, "detail": "push not configured"}
    subs = load_subscriptions()
    sent = sum(1 for s in subs if _send_one(s, payload))
    return {"sent": sent, "failed": len(subs) - sent}


# Alert -> push, de-duplicated. fermenter/alert is retained and singular (one
# active fault at a time), so a naive "push on every message" would re-fire on
# broker reconnects and on each unchanged repeat. We push only when the fault is
# new, escalates in severity, or a cooldown has lapsed (a gentle reminder that a
# fault is still live) — never a storm from one stuck sensor.
_SEV_RANK = {"info": 0, "warning": 1, "critical": 2}
_alert_state = {"code": None, "sev": -1, "at": 0.0}
ALERT_COOLDOWN = float(os.environ.get("ALERT_COOLDOWN_SEC", "900"))


def notify_alert(payload: str) -> None:
    a = None
    if payload:
        try:
            a = json.loads(payload)
        except Exception:
            a = None
    # Empty payload / active:false clears the fault (matches the frontend banner).
    if not a or a.get("active") is False:
        _alert_state.update(code=None, sev=-1, at=0.0)
        return
    code = a.get("code") or "ALERT"
    sev = _SEV_RANK.get(a.get("severity"), 1)
    now = time.time()
    if not (code != _alert_state["code"]
            or sev > _alert_state["sev"]
            or now - _alert_state["at"] > ALERT_COOLDOWN):
        return
    _alert_state.update(code=code, sev=sev, at=now)
    title = {2: "🔴 Fermenter critical", 1: "⚠️ Fermenter warning"}.get(
        sev, "Fermentation chamber")
    send_push_to_all({
        "title": title,
        "body": a.get("message") or code,
        "tag": code,
        "severity": a.get("severity") or "warning",
    })


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
            # A fault also becomes a phone notification. webpush blocks (it does
            # sync HTTP to the push service), so hand it to the executor and let
            # the fan-out below proceed without waiting on the network.
            if topic == "fermenter/alert":
                asyncio.get_running_loop().run_in_executor(None, notify_alert, payload)
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


def _series(hours: float, measurement: str, fields: list[str]) -> dict:
    """Pivot the given fields of one measurement into parallel arrays:
    {"t": [...timestamps], field: [...values], ...}. Missing samples come
    back as None so the frontend keeps every series aligned to `t`."""
    seconds = int(round(hours * 3600))

    field_filter = " or ".join(f'r._field == "{f}"' for f in fields)
    flux = f'''
    from(bucket: "{BUCKET}")
      |> range(start: -{seconds}s)
      |> filter(fn: (r) => r._measurement == "{measurement}")
      |> filter(fn: (r) => {field_filter})
      |> pivot(rowKey: ["_time"], columnKey: ["_field"], valueColumn: "_value")
      |> sort(columns: ["_time"])
    '''
    out: dict = {"t": []}
    for f in fields:
        out[f] = []
    try:
        for table in influx.query_api().query(flux):
            for rec in table.records:
                out["t"].append(rec["_time"].timestamp())
                for f in fields:
                    out[f].append(rec.values.get(f))
    except Exception:
        # One measurement's query failing (e.g. a schema/type quirk) must not
        # blank the others — return what we have and surface the cause.
        log.exception("history query failed for measurement %s", measurement)
    return out


@app.get("/api/history")
def history(hours: float = Query(1, ge=0.099, le=168)):
    # Two measurements, two timelines: telemetry (sensors) and state
    # (actuators). Telegraf writes them separately, so the browser backfills
    # each chart group from its own series.
    return {
        "telemetry": _series(hours, "telemetry", ["dsTemp", "bmeTemp", "humidity"]),
        "state": _series(hours, "state", ["fanDuty", "heaterOn", "humidOn"]),
    }


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


# --- web push registration ---------------------------------------------------
# The browser subscribes once (button in the dashboard), then the backend owns
# delivery. These are sync `def` endpoints so FastAPI runs them in its
# threadpool — safe for the blocking sqlite/webpush calls they make.
class PushSub(BaseModel):
    endpoint: str
    keys: dict
    expirationTime: float | None = None


@app.get("/api/push/vapid-public-key")
def vapid_public_key():
    if not PUSH_ENABLED:
        raise HTTPException(503, "push not configured")
    return {"key": VAPID_PUBLIC_KEY}


@app.post("/api/push/subscribe")
def push_subscribe(sub: PushSub):
    save_subscription(sub.model_dump(exclude_none=True))
    return {"ok": True}


@app.post("/api/push/unsubscribe")
def push_unsubscribe(sub: PushSub):
    delete_subscription(sub.endpoint)
    return {"ok": True}


@app.post("/api/push/test")
def push_test():
    # Verify the whole round-trip without waiting for a real fault.
    return send_push_to_all({
        "title": "Fermentation chamber",
        "body": "Test notification — push is working.",
        "tag": "test",
        "severity": "info",
    })


# Serve the dashboard. Explicit routes above win over this mount.
app.mount("/", StaticFiles(directory="web", html=True), name="web")
