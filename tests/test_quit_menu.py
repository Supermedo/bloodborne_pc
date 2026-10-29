from paths import ROOT  # noqa: F401  (scripts/ on the import path)
from contextlib import redirect_stdout
import io
from pathlib import Path
import random
import struct
import tempfile
import threading
import time
import unittest
from unittest import mock
import zlib

import quit_menu

# An FMG header (version 2) for write_fmg, and a DFLT DCX header for pack_dcx.
FMG =bytes([0, 0, 2, 0]) + bytes(4) + bytes([1, 0, 0, 0]) + bytes(8) + bytes([0xff, 0, 0, 0]) + bytes(16)
DCX = b'DCX\0' + bytes(0x24) + b'DFLT' + bytes(0x20)


def menu_archive(fmgs):
    """A menu.msgbnd.dcx like the game's: a DFLT DCX around a BND4 of FMGs {file id: {id: text}}."""
    bnd = bytearray(0x40 + 0x24 * len(fmgs))
    bnd[:4] = b'BND4'
    struct.pack_into('<I', bnd, 0x0C, len(fmgs))
    struct.pack_into('<Q', bnd, 0x20, 0x24)
    for i, file_id in enumerate(fmgs):
        struct.pack_into('<II', bnd, 0x40 + 0x24 * i + 0x18, len(bnd), file_id)
    files = {file_id: quit_menu.write_fmg(FMG, strings) for file_id, strings in fmgs.items()}
    return quit_menu.pack_dcx(DCX, quit_menu.rebuild_bnd4(bytes(bnd), files))


def fmgs_of(archive):
    bnd = quit_menu.unpack_dcx(archive)
    return {file_id: quit_menu.read_fmg(bnd[o:o + n]) for _e, o, n, file_id in quit_menu.bnd4_files(bnd)}


def bodies():
    """Archive bodies of different sizes and kinds: empty, one byte, UTF-16 text, random bytes,
    and a few MiB with long runs."""
    rng = random.Random(9)
    words = ('Hunter', 'Doll', 'Insight', 'Blood Echoes', 'Rebirth', 'Quit to Desktop', '狩人')
    text = '\0'.join(rng.choice(words) for _ in range(60000)).encode('utf-16-le')
    return [b'', b'x', text[:4096], text, rng.randbytes(200000), bytes(1 << 20) + text + bytes(1 << 20)]


class PackingTests(unittest.TestCase):
    def test_parallel_packing_is_byte_identical_to_serial(self):
        def serial(body):  # what pack_dcx gave one archive after another: the header, then level 9
            packed = zlib.compress(body, 9)
            return DCX[:0x1C] + struct.pack('>II', len(body), len(packed)) + DCX[0x24:] + packed
        data = bodies()
        expected = [serial(body) for body in data]
        for threads in (1, 2, 4, 8):
            with mock.patch.object(quit_menu, 'PACK_THREADS', threads):
                packed = list(quit_menu.in_parallel(lambda body: quit_menu.pack_dcx(DCX, body), data))
            self.assertEqual(packed, expected, f'{threads} threads')
        self.assertEqual([quit_menu.unpack_dcx(archive) for archive in expected], data)

    def test_results_in_order_and_a_failure_stops_there(self):
        def job(i, fail=None):
            time.sleep(0.01 * (6 - i))  # later items finish first
            if i == fail:
                raise ValueError('bad archive')
            return i * 10
        threads = threading.active_count()
        with mock.patch.object(quit_menu, 'PACK_THREADS', 4):
            self.assertEqual(list(quit_menu.in_parallel(job, range(6))), [0, 10, 20, 30, 40, 50])
            got = []
            with self.assertRaisesRegex(ValueError, 'bad archive'):
                for result in quit_menu.in_parallel(lambda i: job(i, fail=3), range(6)):
                    got.append(result)
            self.assertEqual(got, [0, 10, 20])  # as a plain loop: those before the failure, nothing after
            # A caller that stops early: the threads end too.
            results = quit_menu.in_parallel(job, range(6))
            self.assertEqual(next(results), 0)
            results.close()
        self.assertEqual(threading.active_count(), threads)


class TextLayerTests(unittest.TestCase):
    def test_text_layer_is_what_one_language_after_another_gives(self):
        languages = ('deude', 'engus', 'jpnjp', 'zzzzz')  # zzzzz: an unknown folder gets English
        with tempfile.TemporaryDirectory() as temp:
            game, out = Path(temp) / 'game', Path(temp) / 'layer'
            msg = game / 'dvdroot_ps4' / 'msg'
            for i, name in enumerate(languages):
                (msg / name).mkdir(parents=True)
                (msg / name / 'menu.msgbnd.dcx').write_bytes(menu_archive({
                    200: {401300 + k: f'{name} item {k}' * (i + 1) for k in range(300)},
                    201: {401300: f'{name} help', 110005: 'Language'},
                    204: {110000: 'Return to title screen?'},
                    30: {15000100: 'Channel Blood Echoes'},
                }))
            (msg / 'na').mkdir()  # the game has folders without menu texts
            printed = io.StringIO()
            with mock.patch.object(quit_menu, 'PACK_THREADS', 4), redirect_stdout(printed):
                quit_menu.write_text_layer(game, out)
            self.assertIn('menu texts for 4 languages', printed.getvalue())
            self.assertEqual(sorted(p.name for p in (out / 'dvdroot_ps4' / 'msg').iterdir()), sorted(languages))
            for name in languages:
                source = (msg / name / 'menu.msgbnd.dcx').read_bytes()
                written = (out / 'dvdroot_ps4' / 'msg' / name / 'menu.msgbnd.dcx').read_bytes()
                texts = quit_menu.TEXTS.get(name, quit_menu.TEXTS['engus'])
                self.assertEqual(written, quit_menu.menu_texts(source, texts), name)
                fmgs = fmgs_of(written)
                self.assertEqual(fmgs[200][quit_menu.TITLE_ID], texts[0])
                self.assertEqual(fmgs[204][quit_menu.DIALOG_ID], texts[3])
                self.assertEqual(fmgs[30], fmgs_of(source)[30])  # other files unchanged
            self.assertEqual(fmgs_of((out / 'dvdroot_ps4/msg/deude/menu.msgbnd.dcx').read_bytes())[200][
                quit_menu.SYSTEM_ID], 'Zum Desktop beenden')


if __name__ == '__main__':
    unittest.main()
