"""Phase 0 entry point: ingest both topics, fuse, and log each snapshot.

This is wiring only -- the analogue of main.cpp. It proves the ingestion spine
(both topics subscribed, telemetry paired with retained state) before any
TwinState / Service / dispatch layers are built on top. Run it against a live
broker to confirm the contract round-trips:

    python -m fermenter_twin

It supersedes the throwaway tools/subscriber.py.
"""

from __future__ import annotations

import logging
import signal

from .config import TwinConfig
from .contract import Snapshot
from .source import MqttSource

log = logging.getLogger("fermenter_twin")


def _format(snap: Snapshot) -> str:
    t = snap.telemetry
    s = snap.state
    base = f"hum={t.humidity:5.1f}%  ds={t.ds_temp:5.2f}C  bme={t.bme_temp:5.2f}C"
    if s is None:
        return base + "  state=<none yet>"
    reason = (
        "ON" if s.humid_on
        else "INHIBITED" if s.humid_inhibited
        else "off"
    )
    return (
        f"{base}  set={s.target_humidity:.0f}%  humid={reason}"
        f"  heater={'ON' if s.heater_on else 'off'}"
        f"  {'HALTED' if s.halted else 'running'}"
    )


def main() -> None:
    logging.basicConfig(
        level=logging.INFO, format="%(asctime)s %(message)s", datefmt="%H:%M:%S"
    )
    source = MqttSource(TwinConfig.from_env())
    signal.signal(signal.SIGINT, lambda *_: source.close())
    signal.signal(signal.SIGTERM, lambda *_: source.close())

    log.info("twin ingesting (Phase 0: log fused snapshots) -- Ctrl-C to stop")
    for snap in source.snapshots():
        log.info(_format(snap))
    log.info("twin stopped")


if __name__ == "__main__":
    main()
