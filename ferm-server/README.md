# ferm-server

The server side of the fermentation chamber. It receives MQTT messages from the
ESP32 controller, stores the numeric history in InfluxDB, and serves a live
dashboard. Everything runs as Docker containers on one host (`docker-compose.yml`).

## How it's connected

```
ESP32 ──MQTT──▶ mosquitto ──┬──▶ telegraf ──▶ influxdb   (history, 90d)
                            │
browser ◀─WebSocket─┐       └──▶ app (FastAPI) ──▶ influxdb (reads history)
browser ──HTTP──────┘            app ──MQTT──▶ mosquitto  (relays commands)
```

- **mosquitto** — MQTT broker. The ESP32, Telegraf, and the app all connect on
  `1883`. The browser does *not* speak MQTT; it goes through the app.
- **telegraf** — subscribes to the sensor topics and writes them to InfluxDB.
- **influxdb** — time-series store (org `ferm`, bucket `fermenter`).
- **app** — FastAPI backend. Serves the dashboard, relays live MQTT to browsers
  over a WebSocket, answers history queries from InfluxDB, and publishes
  validated commands back to MQTT.

## What is stored in InfluxDB

Bucket `fermenter`, 90-day retention. Telegraf splits the stream into two
measurements because InfluxDB requires consistent field types per measurement
and the state topic mixes floats, booleans, and strings:

| Measurement | Source topic         | Contents                                                    |
|-------------|----------------------|-------------------------------------------------------------|
| `telemetry` | `fermenter/telemetry`| All-numeric sensor stream: `dsTemp`, `bmeTemp`, `humidity`  |
| `state`     | `fermenter/state`    | Controller state (numeric + bool + string), e.g. `targetTemp`, `targetHumidity`, `heaterOn`, `humidOn`, `fanDuty`, `controlSensor`, `halted`, `runMinutes`, … |

Only `telemetry` is charted by the dashboard (`/api/history` queries `dsTemp`,
`bmeTemp`, `humidity`); `state` is stored for the full record.

## How and where it's configured

### `docker-compose.yml` — wiring and secrets

Ports, the internal network names, and all secrets (via `${…}` from a `.env`
file that lives **only on the server**, not in this repo):

```yaml
  influxdb:
    image: influxdb:2.7
    environment:
      - DOCKER_INFLUXDB_INIT_ORG=ferm
      - DOCKER_INFLUXDB_INIT_BUCKET=fermenter
      - DOCKER_INFLUXDB_INIT_RETENTION=90d
      - DOCKER_INFLUXDB_INIT_ADMIN_TOKEN=${INFLUX_TOKEN}   # secret

  app:
    build: ./app
    ports: ["8000:8000"]
    environment:
      - INFLUX_URL=http://influxdb:8086   # reached by internal DNS name
      - INFLUX_TOKEN=${INFLUX_TOKEN}      # secret
      - MQTT_USER=webui
      - MQTT_PASS=${MQTT_WEBUI_PASS}      # secret
```

Only `mosquitto:1883` and `app:8000` are published. InfluxDB is internal-only —
reach its UI through a tunnel.

### `telegraf/telegraf.conf` — ingest MQTT → InfluxDB

Output target, and one `mqtt_consumer` per measurement. `json_v2` is used so the
mixed-type `state` payload parses:

```toml
[[outputs.influxdb_v2]]
  urls = ["http://influxdb:8086"]
  token = "${INFLUX_TOKEN}"          # from env, injected by compose
  organization = "ferm"
  bucket = "fermenter"

[[inputs.mqtt_consumer]]
  servers  = ["tcp://mosquitto:1883"]
  topics   = ["fermenter/telemetry"]
  username = "telegraf"
  password = "${MQTT_PASS}"          # from env
  data_format = "json_v2"
  [[inputs.mqtt_consumer.json_v2]]
    measurement_name = "telemetry"
    [[inputs.mqtt_consumer.json_v2.object]]
      path = "@this"
      disable_prepend_keys = true
```

A second identical block consumes `fermenter/state` into measurement `state`.

### `mosquitto/config/mosquitto.conf` — broker

Native MQTT only (no WebSocket listener), authenticated. The `passwords` and
`acl` files it points to live **only on the server**:

```conf
listener 1883
protocol mqtt

allow_anonymous false
password_file /mosquitto/config/passwords   # not in repo
acl_file /mosquitto/config/acl              # not in repo
```

## Secrets — not in this repo

The following live only on the server and are intentionally absent here:

- `.env` — `INFLUX_TOKEN`, `INFLUX_PASSWORD`, `MQTT_TELEGRAF_PASS`,
  `MQTT_WEBUI_PASS`
- `mosquitto/config/passwords` and `mosquitto/config/acl`
- the `influxdb/` and `mosquitto/data/` volume data
