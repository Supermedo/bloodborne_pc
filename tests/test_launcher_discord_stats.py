from paths import ROOT
import sys
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
# The launcher module itself, as the other tests import it: a second copy (loaded under another name) binds the
# shared modules (bbport_setup, bbport_signin, ...) to itself, and later tests' mocks on the launcher then miss.
import bbport_launcher_win as launcher  # noqa: E402


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
