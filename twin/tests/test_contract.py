"""Contract parsing is the deterministic core -- the one thing worth unit testing
in Phase 0. Run with: python -m pytest twin/tests/ (or python -m unittest)."""

import unittest

from fermenter_twin.contract import DeviceState, Telemetry


class TestTelemetry(unittest.TestCase):
    def test_parses_valid_frame(self):
        t = Telemetry.from_json(
            b'{"ts":12345,"dsTemp":27.5,"bmeTemp":26.9,"humidity":82.1,"pressure":1012.3}'
        )
        self.assertIsNotNone(t)
        self.assertEqual(t.ts_ms, 12345)
        self.assertAlmostEqual(t.humidity, 82.1)

    def test_rejects_malformed(self):
        self.assertIsNone(Telemetry.from_json(b"not json"))
        self.assertIsNone(Telemetry.from_json(b'{"ts":1}'))  # missing fields
        self.assertIsNone(Telemetry.from_json(b'{"ts":1,"dsTemp":"x"}'))


class TestDeviceState(unittest.TestCase):
    BASE = (
        '{"targetTemp":28.0,"targetHumidity":85,"targetCeiling":35.0,'
        '"fanDuty":70,"heaterOn":false,"humidOn":true,%s"halted":false}'
    )

    def test_parses_with_inhibit_field(self):
        s = DeviceState.from_json(self.BASE % '"humidInhibited":true,')
        self.assertIsNotNone(s)
        self.assertTrue(s.humid_on)
        self.assertTrue(s.humid_inhibited)

    def test_defaults_inhibit_for_old_firmware(self):
        # Firmware predating the inhibit latch omits the field; default to False.
        s = DeviceState.from_json(self.BASE % "")
        self.assertIsNotNone(s)
        self.assertFalse(s.humid_inhibited)

    def test_rejects_malformed(self):
        self.assertIsNone(DeviceState.from_json(b"{}"))


if __name__ == "__main__":
    unittest.main()
