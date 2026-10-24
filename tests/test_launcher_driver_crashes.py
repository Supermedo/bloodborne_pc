"""A crash inside the graphics driver. A tester's game, started from Steam, crashed in the AMD driver right after
Steam's shader pre-caching layer (Fossilize) failed to record the port's pipelines; RivaTuner ran too."""
from paths import ROOT
import importlib.util
import sys
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT / 'scripts'))
import bbport_launcher_win as L  # noqa: E402
spec = importlib.util.spec_from_file_location('bbport_help_driver', ROOT / 'launcher/bbport_help.py')
help_ = importlib.util.module_from_spec(spec)
spec.loader.exec_module(help_)


class DriverCrashTests(unittest.TestCase):
    def test_steams_pre_caching_layer_is_off_for_the_game(self):
        env = L.game_environment(dict(L.APP_DEFAULTS))
        self.assertEqual(env['DISABLE_VK_LAYER_VALVE_steam_fossilize_1'], '1')
        self.assertNotIn('DISABLE_VK_LAYER_VALVE_steam_overlay_1', env)  # Steam's overlay stays

    def test_a_fault_in_the_driver_leads_to_its_topic(self):
        for line in ('Fault: read access to 0000000000000060 at amdvlk64.dll+0x2b5f3f0 (thread 11120)',
                     'Fault: write access to 0000000000000000 at nvoglv64.dll+0x12345 (thread 4)',
                     'Fault: read access to 0000000000000008 at igvk64.dll+0x99 (thread 7)'):
            self.assertEqual(help_.hints_in_log(line), ['driver'], line)
        self.assertEqual(help_.hints_in_log('Fault: read access to 0 at bb-probe.exe+0x10'), [])
        self.assertIn('RTSS', help_.TOPICS['driver'][2][-1])


if __name__ == '__main__':
    unittest.main()
