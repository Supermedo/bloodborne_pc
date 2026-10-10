from paths import ROOT
import importlib.util
import tempfile
import unittest
from pathlib import Path

from test_prepare import fixture

spec = importlib.util.spec_from_file_location('bbrun', ROOT / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)


class PatchedEbootTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.game = Path(self.temp.name)
        (self.game / 'eboot.bin').write_bytes(fixture())

    def test_a_clean_image_is_not_the_patched_one(self):
        self.assertEqual(run.image_sha256(self.game), run.image_sha256(self.game))
        self.assertNotEqual(run.image_sha256(self.game), run.LANCE_60FPS_IMAGE)


if __name__ == '__main__':
    unittest.main()
