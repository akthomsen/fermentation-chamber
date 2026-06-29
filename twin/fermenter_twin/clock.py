"""Injectable clock so windowing logic is testable and replay is deterministic.

Logic must read time through a Clock, never call time.* directly. Prod uses the
real monotonic clock; tests and replay swap in a fake that advances on demand.
"""

from __future__ import annotations

import time
from typing import Protocol


class Clock(Protocol):
    def now(self) -> float:
        """Monotonic seconds -- the only clock safe to window/order on."""

    def wall(self) -> float:
        """Epoch seconds, for human-readable output only."""


class SystemClock:
    def now(self) -> float:
        return time.monotonic()

    def wall(self) -> float:
        return time.time()


class FakeClock:
    """Manually advanced clock for tests and deterministic replay."""

    def __init__(self, start: float = 0.0, wall: float = 0.0) -> None:
        self._now = start
        self._wall = wall

    def now(self) -> float:
        return self._now

    def wall(self) -> float:
        return self._wall

    def advance(self, seconds: float) -> None:
        self._now += seconds
        self._wall += seconds
