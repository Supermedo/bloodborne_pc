"""tools/fsr4cap/dll_info.py: which AMD DLLs the FSR 4.1.1 build accepts (synthetic files)."""
from paths import ROOT
import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("dll_info", ROOT / "tools/fsr4cap/dll_info.py")
dll_info = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dll_info)


FFX_API = ["ffxConfigure", "ffxCreateContext", "ffxDestroyContext", "ffxDispatch", "ffxQuery"]


def pe_exporting(names):
    """PE32+ headers and one section holding an export directory that names `names`."""
    table = 0x1000 + 40  # RVA of the name pointers, after the directory
    strings, pointers = b"", []
    for name in names:
        pointers.append(table + 4 * len(names) + len(strings))
        strings += name.encode() + b"\0"
    section = (struct.pack("<IIHHIIIIIII", 0, 0, 0, 0, 0, 1, len(names), len(names), 0, table, 0)
               + struct.pack(f"<{len(names)}I", *pointers) + strings)
    header = bytearray(0x200)
    header[0:2] = b"MZ"
    struct.pack_into("<I", header, 0x3C, 0x40)
    header[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", header, 0x44, 0x8664, 1, 0, 0, 0, 240, 0x2022)
    struct.pack_into("<H", header, 0x58, 0x20B)
    struct.pack_into("<II", header, 0x58 + 112, 0x1000, len(section))
    struct.pack_into("<8sIIII", header, 0x58 + 240, b".edata", len(section), 0x1000, len(section), 0x200)
    return bytes(header) + section


def fake_dll(path, version, models=(), exports=()):
    """A file with a VS_FIXEDFILEINFO of `version`, shader names of `models` and, when given,
    a PE export directory of `exports`."""
    a, b, c, d = version
    fixed = struct.pack("<IIII", 0xFEEF04BD, 0x10000, (a << 16) | b, (c << 16) | d)
    names = b"".join(f"\0fsr4_model_{m}_pass{i}\0fsr4_model_{m}_prepass\0".encode()
                     for m in models for i in (1, 2))
    head = pe_exporting(exports) if exports else b"MZ" + bytes(64)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(head + names + bytes(16) + fixed + bytes(40))
    return path


class DllInfoTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_official_4_1_1_needs_no_loader(self):
        # AMD's DLL exports the FidelityFX API: fsr4cap.exe loads it directly (one DLL is enough).
        upscaler = fake_dll(self.root / "dist/amd_fidelityfx_upscaler_dx12.dll", (4, 1, 1, 2740),
                            ["v07_fp8_no_scale"], FFX_API)
        self.assertEqual(dll_info.exports(upscaler.read_bytes()), set(FFX_API))
        status, problem, details = dll_info.inspect(upscaler)
        self.assertEqual((status, problem), (0, None))
        self.assertIsNone(details["loader"])
        # A loader picked anyway (the launcher asked for one before) is not needed either.
        status, _, details = dll_info.inspect(upscaler, upscaler)
        self.assertEqual(status, 0)
        self.assertIsNone(details["loader"])

    def test_not_a_pe_file_exports_nothing(self):
        self.assertEqual(dll_info.exports(b"MZ" + bytes(64)), set())
        self.assertEqual(dll_info.exports(b""), set())

    def test_without_exports_the_optiscaler_loader_one_folder_up(self):
        upscaler = fake_dll(self.root / "FSR4_LATEST/amd_fidelityfx_upscaler_dx12.dll", (4, 1, 1, 2740),
                            ["v07_fp8_no_scale"])
        loader = fake_dll(self.root / "amd_fidelityfx_dx12.dll", (2, 3, 0, 2740))
        status, problem, details = dll_info.inspect(upscaler)
        self.assertEqual((status, problem), (0, None))
        self.assertEqual(details["version"], "4.1.1.2740")
        self.assertEqual(Path(details["loader"]), loader)

    def test_community_int8_build_is_refused_before_a_capture(self):
        upscaler = fake_dll(self.root / "amd_fidelityfx_upscaler_dx12.dll", (4, 0, 2, 0), ["v07_i8"])
        status, problem, details = dll_info.inspect(upscaler)
        self.assertEqual(status, dll_info.UNSUPPORTED)
        self.assertIn("v07_i8", problem)
        self.assertEqual(details["models"], ["v07_i8"])

    def test_loader_given_instead_of_the_upscaler(self):
        loader = fake_dll(self.root / "amd_fidelityfx_loader_dx12.dll", (2, 3, 0, 0))
        status, problem, _ = dll_info.inspect(loader)
        self.assertEqual(status, dll_info.UNSUPPORTED)
        self.assertIn("no FSR 4 model", problem)

    def test_missing_or_old_loader(self):
        upscaler = fake_dll(self.root / "game/amd_fidelityfx_upscaler_dx12.dll", (4, 1, 1, 0),
                            ["v07_fp8_no_scale"])
        self.assertEqual(dll_info.inspect(upscaler)[0], dll_info.NO_LOADER)
        fake_dll(self.root / "game/amd_fidelityfx_loader_dx12.dll", (1, 1, 4, 0))
        status, problem, _ = dll_info.inspect(upscaler)
        self.assertEqual(status, dll_info.NO_LOADER)
        self.assertIn("2.x", problem)

    def test_official_4_0_is_refused_although_its_model_matches(self):
        # Pragmata's 4.0.3: the model name of 4.1.1, but AMD offers it on RDNA4 only.
        upscaler = fake_dll(self.root / "amd_fidelityfx_upscaler_dx12.dll", (4, 0, 3, 604),
                            ["v07_fp8_no_scale"])
        fake_dll(self.root / "amd_fidelityfx_loader_dx12.dll", (2, 1, 0, 604))
        status, problem, details = dll_info.inspect(upscaler)
        self.assertEqual(status, dll_info.UNSUPPORTED)
        self.assertEqual(details["kind"], "fsr4_0")
        self.assertIn("RDNA4", problem)

    def test_upscaler_chosen_as_its_own_loader(self):
        upscaler = fake_dll(self.root / "amd_fidelityfx_upscaler_dx12.dll", (4, 1, 1, 0),
                            ["v07_fp8_no_scale"])
        status, _, details = dll_info.inspect(upscaler, upscaler)
        self.assertEqual(status, dll_info.NO_LOADER)
        self.assertEqual(details["kind"], "loader_is_upscaler")

    def test_loader_named_by_the_user(self):
        upscaler = fake_dll(self.root / "a/amd_fidelityfx_upscaler_dx12.dll", (4, 1, 1, 0),
                            ["v07_fp8_no_scale"])
        loader = fake_dll(self.root / "elsewhere/loader.dll", (2, 3, 0, 0))
        status, _, details = dll_info.inspect(upscaler, loader)
        self.assertEqual(status, 0)
        self.assertEqual(Path(details["loader"]), loader)


if __name__ == "__main__":
    unittest.main()
