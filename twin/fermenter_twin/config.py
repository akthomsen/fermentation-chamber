"""Runtime configuration, sourced from the environment.

No magic numbers in the logic layers -- they read from here, the same way the
firmware keeps its tuning in Config.h. Defaults match a local Mosquitto broker.
"""

from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True)
class TwinConfig:
    broker_host: str = "localhost"
    broker_port: int = 1883
    client_id: str = "fermenter-twin"

    topic_telemetry: str = "fermenter/telemetry"
    topic_state: str = "fermenter/state"
    topic_cmd_control: str = "fermenter/cmd/control"
    topic_alert: str = "fermenter/alert"

    @classmethod
    def from_env(cls) -> "TwinConfig":
        return cls(
            broker_host=os.environ.get("TWIN_BROKER_HOST", cls.broker_host),
            broker_port=int(os.environ.get("TWIN_BROKER_PORT", cls.broker_port)),
            client_id=os.environ.get("TWIN_CLIENT_ID", cls.client_id),
        )
