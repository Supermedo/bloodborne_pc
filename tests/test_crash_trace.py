from paths import ROOT
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('crash_trace', ROOT / 'scripts/crash_trace.py')
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)


class CrashTraceTests(unittest.TestCase):
    def test_report_contains_only_session_dump_and_status(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / 'logs/crash-trace-session'
            report.mkdir(parents=True)
            (report / 'session.log').write_text('Native process exited: 0xc0000409\n', encoding='utf-8')
            (report / 'bb-crash-123.dmp').write_bytes(b'dump fixture')
            for name in ('userdata.bin', 'bbport.ini', 'eboot.bin'):
                (report / name).write_text('private')
            (root / 'Bloodborne.exe').write_bytes(b'launcher fixture')
            target = trace.collect(report, -1073740791, root)
            with zipfile.ZipFile(target) as archive:
                self.assertEqual(set(archive.namelist()), {'session.log', 'metadata.json', 'bb-crash-123.dmp'})
                meta = json.loads(archive.read('metadata.json'))
                self.assertEqual(meta['launch_status_hex'], '0xc0000409')
                self.assertEqual(meta['dump_count'], 1)
                self.assertEqual(len(meta['binary_sha256']['Bloodborne.exe']), 64)
            with self.assertRaises(FileExistsError):
                trace.collect(report, 0, root)
