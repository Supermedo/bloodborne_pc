from paths import ROOT
import os
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import game_check
from test_prepare import fixture


def param_sfo(entries):
    """A minimal param.sfo with UTF-8 string entries."""
    keys = b''.join(k.encode() + b'\0' for k in entries)
    values = [v.encode() + b'\0' for v in entries.values()]
    count = len(entries)
    key_table = 20 + 16 * count
    data_table = key_table + len(keys)
    data = bytearray(struct.pack('<4sIIII', b'\0PSF', 0x101, key_table, data_table, count))
    key_off = data_off = 0
    for key, value in zip(entries, values):
        data += struct.pack('<HHIII', key_off, 0x204, len(value), len(value), data_off)
        key_off += len(key) + 1
        data_off += len(value)
    return bytes(data + keys + b''.join(values))


class GameCheckTests(unittest.TestCase):
    def game(self, title, version, eboot=None):
        path = Path(self.tmp.name)
        (path / 'sce_sys').mkdir(exist_ok=True)
        (path / 'sce_sys/param.sfo').write_bytes(param_sfo({'APP_VER': version, 'TITLE_ID': title}))
        (path / 'eboot.bin').write_bytes(fixture() if eboot is None else eboot)
        return path

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        os.environ.pop('BB_SKIP_GAME_CHECK', None)

    def tearDown(self):
        self.tmp.cleanup()

    def test_base_game_without_the_update(self):
        self.assertEqual(game_check.problem(self.game('CUSA03173', '01.00')),
                         ('missing_update', 'CUSA03173', '01.00'))

    def test_update_metadata_with_an_old_executable(self):
        self.assertEqual(game_check.problem(self.game('CUSA03173', '01.09'))[0], 'wrong_eboot')

    def test_other_edition(self):
        self.assertEqual(game_check.problem(self.game('CUSA00207', '01.09'))[0], 'other_title')

    def test_unreadable_executable(self):
        self.assertEqual(game_check.problem(self.game('CUSA03173', '01.09', b'junk'))[0], 'unreadable')

    def test_supported_image_passes(self):
        game = self.game('CUSA03173', '01.09')
        with mock.patch.object(game_check, 'SUPPORTED_IMAGE', game_check.image_sha256(game)):
            self.assertIsNone(game_check.problem(game))

    def test_skip_switch(self):
        os.environ['BB_SKIP_GAME_CHECK'] = '1'
        self.assertIsNone(game_check.problem(self.game('CUSA03173', '01.00')))

    def test_every_problem_is_explained(self):
        for kind in ('missing_update', 'wrong_eboot', 'other_title', 'unreadable'):
            self.assertIn('CUSA', game_check.explain(kind, 'CUSA03173', '01.00'))


if __name__ == '__main__':
    unittest.main()
