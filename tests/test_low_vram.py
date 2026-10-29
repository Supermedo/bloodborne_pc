from paths import ROOT
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('bbport_run', ROOT / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)


class LowVramTests(unittest.TestCase):
    def caps(self, directory, output):
        """A stand-in for bb-gpu-capabilities --vram printing `output`."""
        if os.name == 'nt':
            tool = Path(directory) / 'caps.cmd'
            tool.write_text(f'@echo {output}\n')
        else:
            tool = Path(directory) / 'caps'
            tool.write_text(f'#!/bin/sh\necho {output}\n')
            tool.chmod(0o755)
        return tool

    def test_discrete_cards_up_to_6_gb(self):
        with tempfile.TemporaryDirectory() as temp:
            self.assertEqual(run.low_vram_mb({}, self.caps(temp, '4096 1')), 4096)
            self.assertEqual(run.low_vram_mb({}, self.caps(temp, '6144 1')), 6144)
            self.assertEqual(run.low_vram_mb({}, self.caps(temp, '8192 1')), 0)
            # Integrated GPUs follow the driver's budget already.
            self.assertEqual(run.low_vram_mb({}, self.caps(temp, '512 0')), 0)

    def test_setting_forces_or_disables_it(self):
        with tempfile.TemporaryDirectory() as temp:
            self.assertEqual(run.low_vram_mb({'BB_LOW_VRAM': '1'}, self.caps(temp, '24138 1')), 24138)
            self.assertEqual(run.low_vram_mb({'BB_LOW_VRAM': '0'}, self.caps(temp, '4096 1')), 0)

    def test_unknown_memory(self):
        with tempfile.TemporaryDirectory() as temp:
            missing = Path(temp) / 'missing.exe'
            self.assertEqual(run.low_vram_mb({}, missing), 0)
            self.assertEqual(run.low_vram_mb({'BB_LOW_VRAM': '1'}, missing), 4096)
            self.assertEqual(run.low_vram_mb({}, self.caps(temp, 'garbage')), 0)


if __name__ == '__main__':
    unittest.main()
