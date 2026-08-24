"""监测节点主机侧参考算法测试。运行：python -m unittest tools.test_monitoring_reference"""

import math
import unittest

from tools.monitoring_reference import (
    AlertState,
    AlertTracker,
    band_energy,
    crest_factor,
    peak_to_peak,
    rms,
)


class MonitoringReferenceTests(unittest.TestCase):
    def test_zero_and_constant(self):
        self.assertEqual(rms([0.0, 0.0, 0.0]), 0.0)
        self.assertEqual(peak_to_peak([3.0, 3.0, 3.0]), 0.0)
        self.assertEqual(crest_factor([0.0, 0.0]), 0.0)

    def test_sine_rms_and_crest(self):
        values = [math.sin(2.0 * math.pi * index / 32.0) for index in range(32)]
        self.assertAlmostEqual(rms(values), math.sqrt(0.5), places=6)
        self.assertAlmostEqual(crest_factor(values), math.sqrt(2.0), places=6)

    def test_band_energy_selects_tone(self):
        values = [math.sin(2.0 * math.pi * 100.0 * index / 1024.0)
                  for index in range(1024)]
        in_band = band_energy(values, 1024, 50.0, 400.0)
        out_band = band_energy(values, 1024, 200.0, 300.0)
        self.assertGreater(in_band, 100.0 * out_band)

    def test_alert_requires_three_valid_over_limit_cycles(self):
        tracker = AlertTracker()
        self.assertEqual(tracker.update(True, True, False), AlertState.PENDING)
        self.assertEqual(tracker.update(False, False, True), AlertState.PENDING)
        self.assertEqual(tracker.update(True, True, False), AlertState.PENDING)
        self.assertEqual(tracker.update(True, True, False), AlertState.ACTIVE)

    def test_alert_requires_three_recovery_cycles(self):
        tracker = AlertTracker()
        for _ in range(3):
            tracker.update(True, True, False)
        self.assertEqual(tracker.state, AlertState.ACTIVE)
        self.assertEqual(tracker.update(True, False, False), AlertState.ACTIVE)
        self.assertEqual(tracker.update(True, False, True), AlertState.RECOVERING)
        self.assertEqual(tracker.update(True, False, True), AlertState.RECOVERING)
        self.assertEqual(tracker.update(True, False, True), AlertState.NORMAL)


if __name__ == "__main__":
    unittest.main()
