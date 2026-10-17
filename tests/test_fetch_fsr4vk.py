# SPDX-License-Identifier: GPL-2.0-or-later
# Adapted from 0xCydral/bloodborne_pc at 66576efbbd272d0c97e04a9bdb5631455d530eae.
"""scripts/fetch_fsr4vk.py: pinned download, resume, hash checks (a local server, synthetic zip)."""
from paths import ROOT
import contextlib
import hashlib
import http.server
import importlib.util
import io
import tempfile
import os
import threading
import unittest
from unittest import mock
import zipfile
from pathlib import Path

spec = importlib.util.spec_from_file_location("fetch_fsr4vk", ROOT / "scripts/fetch_fsr4vk.py")
fetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch)

DLL = "amd_fidelityfx_upscaler_vk.dll"
MEMBERS = {
    DLL: b"MZ" + bytes(range(256)) * 40,
    "LICENSES/GPL-3.0.txt": b"gpl",
    "LICENSES/AMD-FidelityFX-SDK-MIT.md": b"mit",
    "LICENSES/Zstandard-BSD.txt": b"bsd",
    "PATCH.md": b"the patch",  # rides in the zip, but is not a pinned file
}


def make_zip(members):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_STORED) as archive:
        for name, data in members.items():
            archive.writestr(name, data)
    return buffer.getvalue()


class Server(http.server.ThreadingHTTPServer):
    """Serves one body and honours Range; `cut_after` ends the first full response early."""

    def __init__(self, body, cut_after=None):
        super().__init__(("127.0.0.1", 0), Handler)
        self.body, self.cut_after, self.ranges = body, cut_after, []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        body, start = self.server.body, 0
        header = self.headers.get("Range")
        self.server.ranges.append(header)
        if header:
            start = int(header.split("=")[1].split("-")[0])
            self.send_response(206)
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(len(body) - start))
        self.end_headers()
        data = body[start:]
        if self.server.cut_after is not None and not header:
            data = data[:self.server.cut_after]
            self.server.cut_after = None
        self.wfile.write(data)


class FetchFsr4VkTest(unittest.TestCase):
    def patch(self, **values):
        """Patches module pins for this test only; they are restored when it ends."""
        for name, value in values.items():
            patcher = mock.patch.object(fetch, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def pin(self, body, members, port, **overrides):
        """Points the module's pins at a local server and a synthetic zip."""
        values = dict(
            ZIP_URL=f"http://127.0.0.1:{port}/fsr4vk.zip",
            ZIP_SIZE=len(body),
            ZIP_SHA256=hashlib.sha256(body).hexdigest(),
            FILES=tuple((name, name, len(data), hashlib.sha256(data).hexdigest())
                        for name, data in members.items() if name != "PATCH.md"))
        values.update(overrides)
        self.patch(**values)

    def serve(self, body, cut_after=None):
        server = Server(body, cut_after)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(lambda: (server.shutdown(), server.server_close()))
        return server

    def test_download_extracts_and_verifies(self):
        body = make_zip(MEMBERS)
        server = self.serve(body)
        self.pin(body, MEMBERS, server.server_address[1])
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / "fsr4vk"
            self.assertFalse(fetch.files_ok(target))
            fetch.fetch(target)
            self.assertTrue(fetch.files_ok(target))
            self.assertEqual((target / DLL).read_bytes(), MEMBERS[DLL])
            self.assertEqual((target / "PATCH.md").read_bytes(), MEMBERS["PATCH.md"])
            self.assertIn("github.com/dvj5411/fsr4vk", (target / "SOURCE.txt").read_text())
            self.assertFalse((target / ".download").exists())
            requests = len(server.ranges)
            fetch.fetch(target)  # verified files: no second download
            self.assertEqual(len(server.ranges), requests)

    def test_resumes_an_interrupted_download(self):
        body = make_zip(MEMBERS)
        server = self.serve(body, cut_after=len(body) // 3)
        self.pin(body, MEMBERS, server.server_address[1])
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder)
            fetch.fetch(target)
            self.assertTrue(fetch.files_ok(target))
            self.assertEqual(server.ranges[0], None)
            self.assertEqual(server.ranges[1], f"bytes={len(body) // 3}-")

    def test_wrong_zip_is_rejected_and_deleted(self):
        body = make_zip(MEMBERS)
        server = self.serve(body)
        self.pin(body, MEMBERS, server.server_address[1], ZIP_SHA256="0" * 64)
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(fetch.FetchError):
                fetch.fetch(Path(folder))
            self.assertFalse(list(Path(folder).rglob("*.part")))
            self.assertFalse(fetch.files_ok(Path(folder)))

    def test_changed_member_is_rejected(self):
        body = make_zip(MEMBERS)
        server = self.serve(body)
        self.pin(body, MEMBERS, server.server_address[1])
        self.patch(FILES=tuple((n, d, size, "f" * 64 if n == DLL else sha) for n, d, size, sha in fetch.FILES))
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(fetch.FetchError):
                fetch.fetch(Path(folder))
            self.assertFalse((Path(folder) / DLL).exists())

    def test_an_outdated_dll_is_replaced(self):
        """The original upstream DLL (or any other) in the folder: files_ok is False, so fetch downloads
        and swaps it, and the old SOURCE.txt goes with it."""
        body = make_zip(MEMBERS)
        server = self.serve(body)
        self.pin(body, MEMBERS, server.server_address[1])
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder)
            (target / DLL).write_bytes(b"MZ the original upstream build")
            (target / "SOURCE.txt").write_text("fsr4vk v0.4.3 from upstream")
            self.assertFalse(fetch.files_ok(target))
            fetch.fetch(target)
            self.assertEqual(len(server.ranges), 1)
            self.assertTrue(fetch.files_ok(target))
            self.assertEqual((target / DLL).read_bytes(), MEMBERS[DLL])
            self.assertIn("variable descriptor count", (target / "SOURCE.txt").read_text())
            self.assertFalse(list(target.rglob("*.tmp")))

    def test_a_dll_windows_will_not_replace_leaves_no_temporary_file(self):
        """The game has the old DLL loaded: the swap is refused and the half-written copy must not stay."""
        body = make_zip(MEMBERS)
        server = self.serve(body)
        self.pin(body, MEMBERS, server.server_address[1])
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder)
            (target / DLL).write_bytes(b"MZ the original upstream build")
            with mock.patch.object(fetch.os, "replace", side_effect=PermissionError(5, "Access is denied")):
                with self.assertRaises(PermissionError):
                    fetch.fetch(target)
            self.assertFalse(list(target.rglob("*.tmp")))
            self.assertEqual((target / DLL).read_bytes(), b"MZ the original upstream build")

    def test_patch_md_is_not_required(self):
        body = make_zip(MEMBERS)
        server = self.serve(body)
        self.pin(body, MEMBERS, server.server_address[1])
        with tempfile.TemporaryDirectory() as folder:
            fetch.fetch(Path(folder))
            (Path(folder) / "PATCH.md").unlink()
            self.assertTrue(fetch.files_ok(Path(folder)))






    def test_persistent_folder_and_explicit_overrides(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch.dict(os.environ):
            os.environ.pop("BB_FSR4VK_DIR", None)
            os.environ.pop("BB_DATA_DIR", None)
            port = Path(folder)
            self.assertEqual(fetch.default_dir(port), port / "fsr4vk")
            (port / "bin").mkdir()
            (port / "bin" / "bb-probe.exe").touch()
            self.assertEqual(fetch.default_dir(port), port / "fsr4vk")
            os.environ["BB_DATA_DIR"] = str(port / "data")
            self.assertEqual(fetch.default_dir(port), port / "data" / "fsr4vk")
            os.environ["BB_FSR4VK_DIR"] = str(port / "override")
            self.assertEqual(fetch.default_dir(port), port / "override")


if __name__ == "__main__":
    unittest.main()
