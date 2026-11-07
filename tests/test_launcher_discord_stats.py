from paths import ROOT
import importlib.util
import sys
import unittest


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sys.path.insert(0, str(ROOT / 'launcher'))
launcher = load('bb_launcher_discord_stats', 'launcher/bbport_launcher_win.py')


class DiscordStatsTests(unittest.TestCase):
    """The stats switch beside the Discord status (src/runtime_discord.c, BB_DISCORD_STATS)."""

    def test_on_by_default(self):
        self.assertIs(launcher.APP_DEFAULTS['discord_stats'], True)

    def test_environment(self):
        for wanted, value in ((True, '1'), (False, '0')):
            env = launcher.game_environment({**launcher.APP_DEFAULTS, 'discord_stats': wanted})
            self.assertEqual(env['BB_DISCORD_STATS'], value)
        settings = dict(launcher.APP_DEFAULTS)
        del settings['discord_stats']  # an older settings.json shows the stats
        self.assertEqual(launcher.game_environment(settings)['BB_DISCORD_STATS'], '1')


if __name__ == '__main__':
    unittest.main()
