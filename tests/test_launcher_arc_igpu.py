"""The hardware check's rating of Intel's Arc iGPUs: a tester's Core Ultra 5 125H ran the port at 40-55 FPS at 1080p
native with the settings for it, while the rating said "about 30 FPS"."""
from paths import ROOT
import sys
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_hardware as hardware  # noqa: E402


class ArcIgpuTests(unittest.TestCase):
    def rows(self, name):
        gpu = hardware.match_gpu(name, 16)
        rating = hardware.assess(gpu, hardware.cpu_from_name('Intel(R) Core(TM) Ultra 5 125H'), 32)
        return gpu, [label for _res, _level, label, _settings in rating.rows]

    def test_30_to_60_fps_at_1080p(self):
        for name in ('Intel(R) Arc(TM) Graphics', 'Intel(R) Arc(TM) 140V GPU (16GB)'):
            gpu, rows = self.rows(name)
            self.assertEqual(rows[0], '30–60 FPS', name)
            self.assertIsNone(hardware.upscaler_name(gpu))  # still no upscaler on Intel graphics

    def test_the_newer_arc_is_not_rated_lower(self):
        self.assertGreater(self.rows('Intel(R) Arc(TM) 140V GPU (16GB)')[0].score,
                           self.rows('Intel(R) Arc(TM) Graphics')[0].score)


if __name__ == '__main__':
    unittest.main()
