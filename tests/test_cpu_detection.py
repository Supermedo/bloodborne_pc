"""The legacy CPU profile turns on by itself only where BMI1, LZCNT or MOVBE are missing."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import cpu_compat  # noqa: E402


def fake_cpuid(bmi1, lzcnt, movbe, top=0xd, extended=0x80000008):
    def cpuid(leaf, subleaf=0):
        if leaf == 0:
            return (top, 0, 0, 0)
        if leaf == 0x80000000:
            return (extended, 0, 0, 0)
        if leaf == 1:
            return (0, 0, movbe << 22, 0)
        if leaf == 7:
            return (0, bmi1 << 3, 0, 0)
        if leaf == 0x80000001:
            return (0, 0, lzcnt << 5, 0)
        return (0, 0, 0, 0)
    return cpuid


class Detection(unittest.TestCase):
    def test_ivy_bridge_lacks_all_three(self):
        self.assertEqual(cpu_compat.missing_host_extensions(fake_cpuid(0, 0, 0)), ['BMI1', 'LZCNT', 'MOVBE'])

    def test_haswell_and_later_need_nothing(self):
        self.assertEqual(cpu_compat.missing_host_extensions(fake_cpuid(1, 1, 1)), [])

    def test_absent_leaves_count_as_missing(self):
        self.assertEqual(cpu_compat.missing_host_extensions(fake_cpuid(1, 1, 1, top=1, extended=0x80000000)),
                         ['BMI1', 'LZCNT'])

    @unittest.skipUnless(os.name == 'nt', 'native CPUID through VirtualAlloc')
    def test_this_cpu_answers(self):
        missing = cpu_compat.missing_host_extensions()
        self.assertIsInstance(missing, list)
        self.assertTrue(set(missing) <= set(cpu_compat.HOST_EXTENSIONS))


class Activation(unittest.TestCase):
    def enabled(self, ini=None, env=None, missing=None):
        with tempfile.TemporaryDirectory() as folder:
            config = Path(folder) / 'bbport.ini'
            if ini is not None:
                config.write_text(ini, encoding='utf-8')
            values = {'BB_CONFIG': str(config), **(env or {})}
            with patch.dict(os.environ, values), patch.object(cpu_compat, 'missing_host_extensions',
                                                              return_value=missing):
                if 'BB_LEGACY_CPU' not in values:
                    os.environ.pop('BB_LEGACY_CPU', None)
                return cpu_compat.enabled()

    def test_automatic_from_cpuid(self):
        self.assertTrue(self.enabled(missing=['MOVBE']))
        self.assertFalse(self.enabled(missing=[]))
        self.assertTrue(self.enabled(missing=None))  # unknown CPU: the translations are safe

    def test_ini_and_environment_override(self):
        self.assertFalse(self.enabled(ini='legacy_cpu=0\n', missing=['BMI1']))
        self.assertTrue(self.enabled(ini='upscaler=off\nlegacy_cpu=1\n', missing=[]))
        self.assertFalse(self.enabled(ini='legacy_cpu=1\n', env={'BB_LEGACY_CPU': '0'}, missing=['BMI1']))

    def test_partial_profile_is_the_default(self):
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop('BB_LEGACY_CPU_ALLOW_PARTIAL', None)
            self.assertTrue(cpu_compat.partial_allowed())
            os.environ['BB_LEGACY_CPU_ALLOW_PARTIAL'] = '0'
            self.assertFalse(cpu_compat.partial_allowed())


if __name__ == '__main__':
    unittest.main()
