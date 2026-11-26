# SPDX-License-Identifier: GPL-2.0-or-later
"""Seamless co-op's difficulty: what each step hands BBSC, and that a step saved before its values changed plays
the new ones (Nick, 2026-11-02: less damage per guest, normal enemies less tough, Easy as the game's own co-op).

BBSC's settings as its mod.json (bbsc/mod.json) names them."""
from paths import ROOT
import sys
import unittest

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_seamless as seamless  # noqa: E402

SETTINGS = {
    'scale_enemy_hp': {'id': 'scale_enemy_hp', 'type': 'int', 'default': 40, 'min': 0, 'max': 300, 'env': 'BBSC_SCALE_ENEMY_HP'},
    'scale_boss_hp': {'id': 'scale_boss_hp', 'type': 'int', 'default': 100, 'min': 0, 'max': 300, 'env': 'BBSC_SCALE_BOSS_HP'},
    'scale_damage': {'id': 'scale_damage', 'type': 'int', 'default': 15, 'min': 0, 'max': 200, 'env': 'BBSC_SCALE_DAMAGE'},
    'scale_enemy_stagger': {'id': 'scale_enemy_stagger', 'type': 'int', 'default': 15, 'min': 0, 'max': 200,
                            'env': 'BBSC_SCALE_ENEMY_STAGGER'},
    'scale_boss_stagger': {'id': 'scale_boss_stagger', 'type': 'int', 'default': 20, 'min': 0, 'max': 200,
                           'env': 'BBSC_SCALE_BOSS_STAGGER'},
}
KEYS = [key for _id, key in seamless.SCALES]


def saved(difficulty, *values):
    return {'bbsc_difficulty': difficulty, **{key: str(value) for key, value in zip(KEYS, values)}}


class DifficultyTests(unittest.TestCase):
    def test_each_step_hands_bbsc_its_values(self):
        expected = {
            'easy': {'BBSC_SCALE_ENEMY_HP': '10', 'BBSC_SCALE_BOSS_HP': '50', 'BBSC_SCALE_DAMAGE': '0',
                     'BBSC_SCALE_ENEMY_STAGGER': '5', 'BBSC_SCALE_BOSS_STAGGER': '10'},
            # The boss poise stays BBSC's own 20, so it is not passed
            'normal': {'BBSC_SCALE_ENEMY_HP': '30', 'BBSC_SCALE_BOSS_HP': '80', 'BBSC_SCALE_DAMAGE': '5',
                       'BBSC_SCALE_ENEMY_STAGGER': '10'},
            'hard': {'BBSC_SCALE_ENEMY_HP': '45', 'BBSC_SCALE_BOSS_HP': '110', 'BBSC_SCALE_DAMAGE': '12',
                     'BBSC_SCALE_ENEMY_STAGGER': '20', 'BBSC_SCALE_BOSS_STAGGER': '30'},
            'very_hard': {'BBSC_SCALE_ENEMY_HP': '60', 'BBSC_SCALE_BOSS_HP': '140', 'BBSC_SCALE_DAMAGE': '20',
                          'BBSC_SCALE_ENEMY_STAGGER': '30', 'BBSC_SCALE_BOSS_STAGGER': '40'},
        }
        for difficulty, env in expected.items():
            with self.subTest(difficulty):
                self.assertEqual(seamless.environment(saved(difficulty, *seamless.DIFFICULTIES[difficulty]), SETTINGS), env)

    def test_a_step_saved_with_its_old_values_plays_the_new_ones(self):
        # Normal as it was saved before 2.11.2026: BBSC's own defaults, so nothing was passed then
        env = seamless.environment(saved('normal', 40, 100, 15, 15, 20), SETTINGS)
        self.assertEqual(env['BBSC_SCALE_DAMAGE'], '5')
        self.assertEqual(env['BBSC_SCALE_BOSS_HP'], '80')
        env = seamless.environment(saved('very_hard', 100, 200, 40, 40, 50), SETTINGS)
        self.assertEqual(env['BBSC_SCALE_DAMAGE'], '20')
        self.assertEqual(env['BBSC_SCALE_ENEMY_HP'], '60')

    def test_custom_keeps_the_players_numbers_within_bbscs_limits(self):
        env = seamless.environment(saved('custom', 40, 500, 15, 0, 20), SETTINGS)
        self.assertEqual(env, {'BBSC_SCALE_BOSS_HP': '300', 'BBSC_SCALE_ENEMY_STAGGER': '0'})

    def test_damage_grows_least(self):
        # With four guests (BBSC: 1 + percent x guests) no step more than doubles the damage
        for difficulty, values in seamless.DIFFICULTIES.items():
            with self.subTest(difficulty):
                self.assertLessEqual(1 + values[2] * 4 / 100, 2.0)
                self.assertLess(values[2], values[0])


if __name__ == '__main__':
    unittest.main()
