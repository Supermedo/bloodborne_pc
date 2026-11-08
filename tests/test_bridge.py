from paths import ROOT
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


run = load('bb_run', 'run.py')


class OnlineOverridesTests(unittest.TestCase):
    """run.py's host overrides for online play."""

    def test_shadnet_pool_sends_game_functions_over_http_only(self):
        with tempfile.TemporaryDirectory() as temp:
            env = {'BB_SHADNET_WEBAPI': 'http://srv.shadps4.net:31315'}
            run.online_overrides(env, Path(temp))
            overrides = json.loads((Path(temp) / 'host_overrides.json').read_text(encoding='utf-8'))
        self.assertFalse(any('31315' in v for v in overrides.values()))


if __name__ == '__main__':
    unittest.main()
