"""Snapshot sources: where the twin gets its observations from.

Everything downstream consumes the `Source` protocol -- a stream of fused
`Snapshot`s -- so the live broker and a recorded log are interchangeable. That
is the reproducibility seam: the entire twin can be run offline against a
recorded run, deterministically, by swapping MqttSource for a ReplaySource.
"""

from __future__ import annotations

import queue
from typing import Iterator, Protocol

import paho.mqtt.client as mqtt

from .clock import Clock, SystemClock
from .config import TwinConfig
from .contract import DeviceState, Snapshot, Telemetry


class Source(Protocol):
    def snapshots(self) -> Iterator[Snapshot]:
        """Yield fused snapshots until the source is closed."""

    def close(self) -> None: ...


class MqttSource:
    """Live source: subscribes to telemetry + state and fuses them.

    The two topics are merged by holding the latest retained DeviceState and
    pairing it with every incoming Telemetry frame. Telemetry is the heartbeat
    (one Snapshot per frame); state just updates the context carried alongside.
    """

    _SENTINEL = object()  # pushed by close() to unblock the snapshots() generator

    def __init__(self, config: TwinConfig, clock: Clock | None = None) -> None:
        self._config = config
        self._clock = clock or SystemClock()
        self._latest_state: DeviceState | None = None
        self._queue: "queue.Queue[Snapshot | object]" = queue.Queue()

        self._client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2, client_id=config.client_id
        )
        self._client.on_connect = self._on_connect
        self._client.on_message = self._on_message

    def _on_connect(self, client, userdata, flags, reason_code, properties) -> None:
        client.subscribe(self._config.topic_telemetry)
        client.subscribe(self._config.topic_state)

    def _on_message(self, client, userdata, msg) -> None:
        if msg.topic == self._config.topic_state:
            state = DeviceState.from_json(msg.payload)
            if state is not None:
                self._latest_state = state  # atomic rebind; no telemetry emitted
        elif msg.topic == self._config.topic_telemetry:
            tel = Telemetry.from_json(msg.payload)
            if tel is not None:
                self._queue.put(
                    Snapshot(
                        recv_time=self._clock.now(),
                        wall_time=self._clock.wall(),
                        telemetry=tel,
                        state=self._latest_state,
                    )
                )

    def snapshots(self) -> Iterator[Snapshot]:
        self._client.connect(self._config.broker_host, self._config.broker_port)
        self._client.loop_start()
        try:
            while True:
                item = self._queue.get()
                if item is self._SENTINEL:
                    return
                yield item  # type: ignore[misc]
        finally:
            self._client.loop_stop()

    def close(self) -> None:
        self._queue.put(self._SENTINEL)
        self._client.disconnect()
