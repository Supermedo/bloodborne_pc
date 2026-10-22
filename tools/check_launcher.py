"""The launcher check: every launcher test, the completeness check first (tests/test_launcher_completeness.py),
with a short summary. Run it after every larger change, and before a release:

    python tools/check_launcher.py           everything (on Windows the live checks open a launcher window
                                             for a minute, on copies of the settings)
    python tools/check_launcher.py --quick   only the checks without a window

Exit code 0 when nothing failed but the known problems listed in KNOWN."""
import sys
import time
import unittest
from pathlib import Path

TESTS = Path(__file__).resolve().parent.parent / 'tests'
sys.path.insert(0, str(TESTS))
# The completeness check first; then every launcher test.
MODULES = ['test_launcher_completeness'] + sorted(p.stem for p in TESTS.glob('test_launcher_*.py')
                                                  if p.stem != 'test_launcher_completeness') + [
    'test_input_defaults', 'test_object_motion_cache_launcher', 'test_shared_graphics_settings',
    'test_fsr411_launcher_backend', 'test_fg401_launcher', 'test_low_vram', 'test_respec', 'test_rebirth',
    'test_vulkan_check', 'test_run_stale_views', 'test_package_files']  # missing ones are skipped
WINDOW = {'test_launcher_completeness.LiveTests', 'test_launcher_search_live', 'test_launcher_window'}
# Failures that are known and tracked elsewhere: test id (start) -> why. Empty while nothing is.
KNOWN = {}


def known(test_id):
    return next((why for start, why in KNOWN.items() if test_id.startswith(start)), None)


def main():
    quick = '--quick' in sys.argv
    loader, suite = unittest.TestLoader(), unittest.TestSuite()
    for name in MODULES:
        if not (TESTS / f'{name}.py').is_file():
            continue
        module = loader.loadTestsFromName(name)
        if quick:
            module = unittest.TestSuite(t for group in module for t in (group if isinstance(group, unittest.TestSuite)
                                                                       else [group])
                                        if not any(t.id().startswith(w) for w in WINDOW))
        suite.addTest(module)
    start = time.time()
    result = unittest.TextTestRunner(stream=open('nul' if sys.platform == 'win32' else '/dev/null', 'w'),
                                     verbosity=0).run(suite)
    problems = [(test.id(), text) for test, text in result.failures + result.errors]
    new = [(test_id, text) for test_id, text in problems if not known(test_id)]
    print(f'Launcher check: {result.testsRun} tests in {time.time() - start:.0f} s'
          f'{" (quick: without a window)" if quick else ""}, {len(result.skipped)} skipped')
    for test_id, text in problems:
        why = known(test_id)
        last = [line for line in text.strip().splitlines() if line.strip()][-1]
        print(f'  {"known " if why else "FAILED"}  {test_id}')
        print(f'          {why or last[:300]}')
    print('Result:', 'OK' if not problems else
          f'{len(new)} new problem(s), {len(problems) - len(new)} known' if new else
          f'OK apart from {len(problems)} known problem(s)')
    return 1 if new else 0


if __name__ == '__main__':
    sys.exit(main())
