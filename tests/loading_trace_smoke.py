"""Exercise the loading report through Python or the packaged --script runtime; no game files."""
import csv
import ctypes
import json
import os
from pathlib import Path
import runpy
import sys
import tempfile
import time
import zipfile


def main():
    if '--child' in sys.argv:
        kernel = ctypes.WinDLL('kernel32')
        kernel.GetCurrentThread.restype = ctypes.c_void_p
        kernel.SetThreadDescription.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
        kernel.SetThreadDescription(kernel.GetCurrentThread(), 'loading-fixture')
        assert os.environ['BB_FRAME_STATS'] == os.environ['BB_WAIT_TRACE'] == '1'
        print('fixture-start', flush=True)
        for key, data in [('BB_FRAME_LOG', 't_s,frame_ms\n0,16\n'),
                          ('BB_WAIT_LOG', 'fixture wait\n--\n'),
                          ('BB_READBACK_LOG', 't_s,wait_ms\n0,0\n')]:
            Path(os.environ[key]).write_text(data, encoding='utf-8')
        end = time.perf_counter() + .3
        while time.perf_counter() < end:
            sum(range(10000))
        time.sleep(1.8)  # allow a bounded retry when enumeration precedes thread naming
        print('fixture-end', flush=True)
        return
    root = Path(__file__).resolve().parent.parent
    module = runpy.run_path(str(root / 'scripts/loading_trace.py'))
    with tempfile.TemporaryDirectory(prefix='loading-\u0442\u0435\u0441\u0442-') as temp:
        previous = os.environ.get('BB_DATA_DIR')
        os.environ['BB_DATA_DIR'] = temp
        try:
            command = [sys.executable]
            if getattr(sys, 'frozen', False):
                command.append('--script')
            command += [str(Path(__file__).resolve()), '--child']
            report = module['capture'](root, command, names=(Path(sys.executable).name.lower(),))
        finally:
            if previous is None:
                os.environ.pop('BB_DATA_DIR', None)
            else:
                os.environ['BB_DATA_DIR'] = previous
        with zipfile.ZipFile(report) as archive:
            assert archive.testzip() is None
            assert set(archive.namelist()) == set(module['REPORT_FILES'])
            metadata = json.loads(archive.read('metadata.json'))
            assert metadata['exit_code'] == 0, archive.read('game.log')
            assert metadata['processes'] and metadata['sample_count'] >= 5, metadata
            rows = list(csv.DictReader(archive.read('process.csv').decode().splitlines()))
            assert rows and all(int(row['pid']) in metadata['processes'] for row in rows)
            assert any(int(row['write_bytes']) > 0 for row in rows)
            threads = list(csv.DictReader(archive.read('threads.csv').decode().splitlines()))
            assert any(row['name'] == 'loading-fixture' for row in threads), threads
            assert any(float(row['user_ms']) > 0 for row in threads)
            assert b'fixture-start' in archive.read('game.log')
            assert b'fixture-end' in archive.read('game.log')
        print('Loading trace smoke passed: Unicode paths, process/thread CPU and I/O, log export.', flush=True)


if __name__ == '__main__':
    main()
