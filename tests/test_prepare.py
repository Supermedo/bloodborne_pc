from paths import ROOT
from contextlib import redirect_stdout
import io
import json
import struct
import unittest
from unittest import mock
import tempfile
from pathlib import Path
from prepare import parse_self, inspect_libc, nid, entries, segment, prepare


def fixture():
    data = bytearray(0x210)
    data[:4] = b'O\x15=\x1d'
    struct.pack_into('<H', data, 24, 1)
    struct.pack_into('<QQQQ', data, 32, 0x800, 0x200, 16, 16)
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 64,
                     b'\x7fELF\x02\x01\x01\x09', 0xfe10, 62, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', data, 128, 1, 5, 0x1000, 0, 0, 16, 32, 0x1000)
    data[0x200:] = bytes(range(16))
    return data


def module(code, symbols, relocations=()):
    """A plaintext SELF with `code` at address 0 and the dynamic data segment: string and symbol
    tables (symbols: [(name, info, section, value, size)]) and relocations [(target, info, addend)]."""
    strings, syms = bytearray(), bytearray()
    for name, info, section, value, size in symbols:
        syms += struct.pack('<IBBHQQ', len(strings), info, 0, section, value, size)
        strings += name.encode() + b'\0'
    blob, places = bytearray(), []
    for table in (strings, syms, b''.join(struct.pack('<QQq', *r) for r in relocations)):
        blob += bytes(-len(blob) % 16)
        places.append((len(blob), len(table)))
        blob += table
    blob += bytes(-len(blob) % 16)
    (str_at, str_size), (sym_at, sym_size), (rel_at, rel_size) = places
    dynamic = len(blob)
    for tag, value in ((0x61000035, str_at), (0x61000037, str_size), (0x61000039, sym_at), (0x6100003f, sym_size),
                       (0x61000029, rel_at), (0x6100002d, rel_size), (0x6100002f, rel_at), (0x61000031, 0), (0, 0)):
        blob += struct.pack('<QQ', tag, value)
    code_at = 0x200
    blob_at = code_at + len(code) + (-len(code) % 16)
    data = bytearray(blob_at + len(blob))
    data[:4] = b'O\x15=\x1d'
    struct.pack_into('<H', data, 24, 2)
    struct.pack_into('<QQQQ', data, 32, 0x800, code_at, len(code), len(code))
    struct.pack_into('<QQQQ', data, 64, 0x100800, blob_at, len(blob), len(blob))
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 96,
                     b'\x7fELF\x02\x01\x01\x09', 0xfe10, 62, 1, 0, 64, 0, 0, 64, 56, 3, 0, 0, 0)
    for i, (kind, flags, offset, size, memsz) in enumerate(((1, 5, 0x1000, len(code), len(code)),
                                                           (0x61000000, 4, 0x2000, len(blob), 0),
                                                           (2, 4, 0x2000 + dynamic, len(blob) - dynamic,
                                                            len(blob) - dynamic))):
        struct.pack_into('<IIQQQQQQ', data, 160 + 56 * i, kind, flags, offset, 0, 0, size, memsz, 8)
    data[code_at:code_at + len(code)] = code
    data[blob_at:] = blob
    return bytes(data)


def param_sfo(title):
    key, value = b'TITLE\0', title.encode() + b'\0'
    return struct.pack('<4sIIII', b'\0PSF', 0x101, 36, 36 + len(key), 1) + \
        struct.pack('<HHIII', 0, 0x204, len(value), len(value), 0) + key + value


class PrepareTests(unittest.TestCase):
    def test_prepare_writes_the_image_without_walking_the_game_files(self):
        init_env = nid('_init_env') + '#C#A'
        code = bytes(range(32))
        with tempfile.TemporaryDirectory() as tmp:
            game, out = Path(tmp) / 'game', Path(tmp) / 'out'
            for folder in ('sce_module', 'sce_sys', 'dvdroot_ps4/msg/engus'):
                (game / folder).mkdir(parents=True)
            # The eboot imports _init_env (a jump slot) and has one base-relative relocation.
            (game / 'eboot.bin').write_bytes(module(code, [(init_env, 0x12, 0, 0, 0)], [(16, 8, 0x10), (24, 7, 0)]))
            (game / 'sce_module/libc.prx').write_bytes(module(b'\xc3' + bytes(15), [(init_env, 0x12, 1, 0, 1)]))
            (game / 'sce_sys/param.sfo').write_bytes(param_sfo('Bloodborne'))
            (game / 'dvdroot_ps4/msg/engus/menu.msgbnd.dcx').write_bytes(b'DCX\0')
            # Walking dvdroot_ps4 took 5.5 s of every game start for numbers nothing read.
            with mock.patch.object(Path, 'rglob', side_effect=AssertionError('prepare walks the game files')), \
                    redirect_stdout(io.StringIO()):
                prepare(game, out)
            report = json.loads((out / 'analysis.json').read_text(encoding='utf-8'))
            self.assertEqual(report['import_name_hints'], {init_env: '_init_env'})  # src/runtime.c points there
            self.assertTrue(report['libc_evidence']['init_env_is_ret'])
            self.assertEqual(report['sfo'], {'TITLE': 'Bloodborne'})
            self.assertEqual(report['relocation_counts'], {'8': 1, '7': 1})
            self.assertFalse({'resources', 'resource_bytes'} & report.keys())
            boot = (out / 'boot.bin').read_bytes()
            self.assertEqual(boot[:8], b'BBPROBE2')
            self.assertEqual(struct.unpack_from('<QQQQQQ', boot, 8), (len(code), 0, 1, 2, 1, 1))
            self.assertEqual(boot[-len(code):], code)
            self.assertEqual((out / 'entry.bin').read_bytes(), code)
            self.assertEqual((out / 'eboot.elf').read_bytes()[0x1000:0x1000 + len(code)], code)


class SelfTests(unittest.TestCase):
    def test_libc_ret_contract_is_verified_from_symbol_and_code(self):
        def libc(instruction):
            data=bytearray(0x320)
            data[:4]=b'O\x15=\x1d'
            struct.pack_into('<H',data,24,2)
            struct.pack_into('<QQQQ',data,32,0x800,0x200,16,16)
            struct.pack_into('<QQQQ',data,64,0x100800,0x220,256,256)
            struct.pack_into('<16sHHIQQQIHHHHHH',data,96,b'\x7fELF\x02\x01\x01\x09',0xfe18,62,1,0,64,0,0,64,56,3,0,0,0)
            struct.pack_into('<IIQQQQQQ',data,160,1,5,0x1000,0,0,16,16,0x1000)
            struct.pack_into('<IIQQQQQQ',data,216,0x61000000,4,0x2000,0,0,256,0,16)
            struct.pack_into('<IIQQQQQQ',data,272,2,4,0x20b0,0,0,80,80,8)
            data[0x200]=instruction
            name=b'bzQExy189ZI#C#A\0'
            data[0x220:0x220+len(name)]=name
            struct.pack_into('<IBBHQQ',data,0x260,0,0x12,0,1,0,1)
            for i,(tag,value) in enumerate([(0x61000035,0),(0x61000037,32),(0x61000039,64),(0x6100003f,24),(0,0)]):
                struct.pack_into('<QQ',data,0x2d0+16*i,tag,value)
            return data
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'libc.prx'
            path.write_bytes(libc(0xc3))
            proof=inspect_libc(path)
            self.assertTrue(proof['init_env_is_ret'])
            self.assertEqual(proof['bytes'],'c3')
            path.write_bytes(libc(0x90))
            self.assertFalse(inspect_libc(path)['init_env_is_ret'])
        self.assertEqual(nid('_init_env'),'bzQExy189ZI')

    def test_file_offset_is_not_virtual_address(self):
        elf, header, ph, segments, missing = parse_self(fixture())
        self.assertEqual(elf[0x1000:0x1010], bytes(range(16)))
        self.assertEqual(ph[0]['vaddr'], 0)
        self.assertEqual(missing, [])

    def test_truncated_payload_rejected(self):
        with self.assertRaisesRegex(ValueError, 'out of bounds'):
            parse_self(fixture()[:-1])

    def test_encrypted_or_compressed_payload_rejected(self):
        for flag in (2, 8):
            data = fixture()
            struct.pack_into('<Q', data, 32, 0x800 | flag)
            with self.assertRaisesRegex(ValueError, 'encrypted/compressed'):
                parse_self(data)

    def test_missing_required_segment_rejected(self):
        data = fixture()
        struct.pack_into('<Q', data, 32, 0)
        with self.assertRaisesRegex(ValueError, 'required segment'):
            parse_self(data)

    def test_bad_program_header_index_rejected(self):
        data = fixture()
        struct.pack_into('<Q', data, 32, 0x100800)
        with self.assertRaisesRegex(ValueError, 'segment index'):
            parse_self(data)

    def test_a_missing_table_names_the_file(self):
        """Not the bare KeyError number a player saw ("prepare failed: 1627390005")."""
        with self.assertRaisesRegex(ValueError, r'^eboot\.bin is not the original file: its string table is missing'):
            entries({0x61000037: 4}, 0x61000035, 0x61000037, 'eboot.bin')
        with self.assertRaisesRegex(ValueError, r'^sce_module/libc\.prx is not the original file: its dynamic section'):
            segment([{'type': 1}], 2, 'sce_module/libc.prx')
        self.assertEqual(entries({1: 8, 2: 16}, 1, 2, 'eboot.bin'), (8, 16))


if __name__ == '__main__':
    unittest.main()
