# Fermentation-chamber digital twin

A thin runtime that observes the chamber over MQTT and runs runtime monitors
against the live signal. The broker is the integration seam: the **firmware**,
this **twin**, and the **dashboard** (`web/index.html`) are three independent
peers that meet on the bus — none proxies for another.

v1 is deliberately **model-free**. The place where a physical/causal model would
attach (predicted state → residuals → model-based monitors) is left explicit but
empty, so attaching even a trivial model later is additive.

## The MQTT contract

| Topic | Retained | Publisher(s) | Subscriber(s) | Payload |
|---|---|---|---|---|
| `fermenter/telemetry` | no | firmware | twin, dashboard | `{ts, dsTemp, bmeTemp, humidity, pressure}` |
| `fermenter/state` | yes | firmware | twin, dashboard | `{targetTemp, targetHumidity, targetCeiling, fanDuty, heaterOn, humidOn, humidInhibited, halted}` |
| `fermenter/cmd/setpoint` | no | dashboard | firmware | `{targetTemp?, targetHumidity?}` |
| `fermenter/cmd/control` | no | dashboard, twin | firmware | `{action: start \| stop \| humidifier_off \| humidifier_on}` |
| `fermenter/alert` | yes | twin | dashboard | `{code, severity, message, ts, active}` |

`ts` is the device's `millis()` since boot — not epoch, wraps at ~49 days,
resets to 0 on reboot. **Never window on it.** Window on the host clock captured
in `Snapshot.recv_time`.

`humidInhibited` is the load-bearing addition: it lets a subscriber tell
"humidifier off because satisfied" from "off because latched (empty reservoir)".

## Module map

Each layer is one responsibility and is swappable. Status as of this scaffold:

| Module | Responsibility | Status |
|---|---|---|
| `contract.py` | typed payloads + parsing/validation (the only place that knows the wire format) | **done** |
| `clock.py` | injectable monotonic clock (testable windowing, deterministic replay) | **done** |
| `config.py` | broker/topics/thresholds from env — no magic numbers in logic | **done** |
| `source.py` | `Source` protocol → fused `Snapshot` stream; `MqttSource` (live). `ReplaySource` (offline log) | MqttSource done; replay TODO |
| `app.py` | wiring (the analogue of `main.cpp`) | Phase 0 logger |
| `state.py` | `TwinState`: latest snapshot + bounded history; **observed only**, with an empty `predicted` slot (the model seam) | Phase 1 |
| `service.py` | `Service` protocol + `ServiceManager`; add a capability = one class + one registration | Phase 1 |
| `events.py` | `Alert` / `CommandRequest` — services emit *intent*, never side effects | Phase 1 |
| `dispatch.py` | routes events to alert sinks + a command publisher; mediates (`observe`/`act` mode, rate-limit, de-dup) | Phase 3 |

## Build order

- **Phase 0 — ingest spine (this scaffold).** `contract` + `MqttSource` logging
  fused snapshots. Confirms both topics are ingested and paired. Supersedes
  `tools/subscriber.py`.
- **Phase 1 — observe-only spine.** `TwinState` history + `ServiceManager`
  running a no-op service end to end.
- **Phase 2 — reservoir monitor (detect only).** Alert to console. Validate
  against a real run and a deliberately empty tank.
- **Phase 3 — act.** `dispatch` + command publisher; flip the monitor's command
  path from `observe` to `act` behind the config flag.
- **Phase 4 — model seam.** Populate `predicted`/residual, an STL-monitor base
  class, alert persistence, more services.

## The first service (Phase 2): reservoir-empty monitor

A model-free runtime monitor over a bounded-response property: *whenever the
humidifier is held on (run active), humidity should eventually rise.* A sustained
violation — disc continuously commanded on across a window yet humidity flat — is
the empty-reservoir symptom. Implementation fits a slope over the window rather
than differencing two noisy samples, and **gates the on-streak on `heater_on`
false** (heating drops *relative* humidity at constant moisture — the biggest
false-positive source). On sustained violation it publishes `humidifier_off` +
a retained alert, then re-arms its fire-once latch when it later observes the
humidifier actually running again (manual resume; the twin never auto-re-enables).

This is a *symptom* monitor: flat-humidity-while-on also fits a cut vent, a
scaled disc, a stuck sensor, an open door, or already-saturated air.
Disambiguating cause is exactly the job of the model/STL seam in Phase 4.

## Running (Phase 0)

```sh
pip install -r requirements.txt
TWIN_BROKER_HOST=<broker-ip> python -m fermenter_twin   # from this twin/ dir
python -m pytest tests/                                 # contract unit tests
```
