"""The wire contract the twin speaks to the firmware over MQTT.

This is the *single* place that knows the on-the-wire format. Parsing and
validation live here so a malformed MQTT payload can never reach the logic
layers -- everything downstream works with the typed records below.

Topics (defined by the firmware on the `wifi`/`twin` branch):

  fermenter/telemetry  unretained, ~1 Hz   -> Telemetry
  fermenter/state      retained,   ~1 Hz   -> DeviceState
  fermenter/cmd/*      transient                (published, not parsed here)
"""

from __future__ import annotations

import json
from dataclasses import dataclass


def _num(d: dict, key: str) -> float:
    """Pull a finite number out of a decoded JSON object or raise KeyError."""
    v = d[key]
    if not isinstance(v, (int, float)) or isinstance(v, bool):
        raise ValueError(f"{key!r} is not a number: {v!r}")
    return float(v)


@dataclass(frozen=True)
class Telemetry:
    """One live sensor frame off `fermenter/telemetry`.

    `ts_ms` is the device's millis() since boot -- NOT epoch. It wraps at
    ~49 days and resets to 0 on every reboot, so it is fine as an opaque label
    but must never be used to window or order across reboots. Window on the
    host clock captured in Snapshot.recv_time instead.
    """

    ts_ms: int
    ds_temp: float
    bme_temp: float
    humidity: float
    pressure: float

    @classmethod
    def from_json(cls, raw: bytes | str) -> "Telemetry | None":
        try:
            d = json.loads(raw)
            return cls(
                ts_ms=int(_num(d, "ts")),
                ds_temp=_num(d, "dsTemp"),
                bme_temp=_num(d, "bmeTemp"),
                humidity=_num(d, "humidity"),
                pressure=_num(d, "pressure"),
            )
        except (ValueError, TypeError, KeyError, json.JSONDecodeError):
            return None


@dataclass(frozen=True)
class DeviceState:
    """Latest retained actuator/setpoint truth off `fermenter/state`.

    `humid_on` vs `humid_inhibited` is the load-bearing distinction: the disc is
    off either because humidity is satisfied (`humid_on` false, not inhibited)
    or because the inhibit latch is held (`humid_inhibited` true). The reservoir
    monitor and the dashboard both need to tell these apart.
    """

    target_temp: float
    target_humidity: float
    target_ceiling: float
    fan_duty: int
    heater_on: bool
    humid_on: bool
    humid_inhibited: bool
    halted: bool

    @classmethod
    def from_json(cls, raw: bytes | str) -> "DeviceState | None":
        try:
            d = json.loads(raw)
            return cls(
                target_temp=_num(d, "targetTemp"),
                target_humidity=_num(d, "targetHumidity"),
                target_ceiling=_num(d, "targetCeiling"),
                fan_duty=int(_num(d, "fanDuty")),
                heater_on=bool(d["heaterOn"]),
                humid_on=bool(d["humidOn"]),
                # Tolerate older firmware that predates the inhibit latch.
                humid_inhibited=bool(d.get("humidInhibited", False)),
                halted=bool(d["halted"]),
            )
        except (ValueError, TypeError, KeyError, json.JSONDecodeError):
            return None


@dataclass(frozen=True)
class Snapshot:
    """A telemetry frame fused with the latest known device state.

    `recv_time` is a host monotonic clock (seconds) and is the ONLY timestamp
    safe to window/order on -- see the note on Telemetry.ts_ms. `wall_time` is
    host epoch seconds, for human-readable logs only.

    `state` may be None for telemetry that arrives before the first retained
    state message; consumers that need actuator context must handle that.
    """

    recv_time: float
    wall_time: float
    telemetry: Telemetry
    state: DeviceState | None
