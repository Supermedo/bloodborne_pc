"""run.py run_script: a script given a time limit that is still running then is stopped and counts as a
failure (an optional one returns None), so the start goes on without it (mods.py's game view with the
port's own layers: a player's 1.9.4 never got past it)."""
from paths import ROOT
import importlib.util
import subprocess
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('bbport_run_script_timeout', ROOT / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)


class RunScriptTimeoutTests(unittest.TestCase):
    def test_an_optional_script_past_its_time_limit_is_a_failure(self):
        hang = subprocess.TimeoutExpired(['mods.py'], 60)
        with mock.patch.object(run.subprocess, 'run', side_effect=hang) as started, \
                mock.patch('builtins.print') as printed:
            self.assertIsNone(run.run_script('mods.py', 'game', capture=True, optional=True, timeout=60))
        self.assertEqual(started.call_args.kwargs['timeout'], 60)
        self.assertIn('still running after 60 s', printed.call_args.args[0])

    def test_a_required_script_has_no_time_limit(self):
        done = subprocess.CompletedProcess(['mods.py'], 0, stdout=b'view\n', stderr=b'')
        with mock.patch.object(run.subprocess, 'run', return_value=done) as started:
            self.assertEqual(run.run_script('mods.py', 'game', capture=True, timeout=60), 'view\n')
        self.assertIsNone(started.call_args.kwargs['timeout'])

    def test_the_game_view_with_the_ports_layers_has_a_time_limit(self):
        source = (ROOT / 'run.py').read_text(encoding='utf-8')
        call = source[source.index("view = run_script('mods.py', *mod_args, '--layer'"):]
        self.assertIn('timeout=60', call[:call.index(')\n')])


if __name__ == '__main__':
    unittest.main()
