"""Driver discovery at the AppImage's GTK/game entry point, without a GPU."""
from paths import ROOT
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("bbport_vulkan", ROOT / "launcher/bbport_vulkan.py")
vulkan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vulkan)


class PackagedVulkanTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.icds = self.root / "icds"
        self.libs = self.root / "host-libs"
        self.icds.mkdir()
        self.libs.mkdir()
        self.env = {"BB_DATA_DIR": str(self.root / "data"),
                    "BB_BUNDLED_VK_DRIVER_FILES": "/bundled/radeon.json:/bundled/intel.json",
                    "LD_LIBRARY_PATH": "/bundled/lib"}

    def library(self, name, bits=64):
        path = self.libs / name
        path.write_bytes(b"\x7fELF" + bytes([2 if bits == 64 else 1, 1]) +
                         b"\0" * 12 + b"\x3e\0")
        return path

    def manifest(self, library="libGLX_nvidia.so.0"):
        path = self.icds / "nvidia_icd.json"
        path.write_text(json.dumps({"file_format_version": "1.0.0", "ICD": {
            "library_path": library, "api_version": "1.3.0"}}))
        return path

    def configure(self):
        return vulkan.configure(self.env, (self.icds,), (self.libs,))

    def test_amd_intel_keep_bundled_drivers(self):
        self.configure()
        self.assertEqual(self.env["VK_DRIVER_FILES"], self.env["BB_BUNDLED_VK_DRIVER_FILES"])
        self.assertEqual(self.env["LD_LIBRARY_PATH"], "/bundled/lib")
        self.assertFalse((self.root / "data").exists())

    def test_explicit_and_legacy_overrides_are_preserved(self):
        for key in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES"):
            with self.subTest(key=key):
                self.env[key] = "/user/lavapipe.json"
                before = dict(self.env)
                self.configure()
                self.assertEqual(self.env, before)
                del self.env[key]

    def test_host_nvidia_and_compiler_are_exposed_without_host_libc(self):
        driver = self.library("libGLX_nvidia.so.580.1")
        (self.libs / "libGLX_nvidia.so.0").symlink_to(driver.name)
        compiler = self.library("libnvidia-glvkspirv.so.580.1")
        self.library("libc.so.6")
        self.manifest()
        self.assertIn("host NVIDIA", self.configure())
        driver_json = Path(self.env["VK_DRIVER_FILES"].split(":")[0])
        data = json.loads(driver_json.read_text())
        self.assertEqual(data["ICD"]["api_version"], "1.3.0")
        self.assertEqual(Path(data["ICD"]["library_path"]).resolve(), driver)
        cache = Path(self.env["LD_LIBRARY_PATH"].split(":")[0])
        self.assertEqual((cache / compiler.name).resolve(), compiler)
        self.assertFalse((cache / "libc.so.6").exists())
        self.assertTrue(self.env["VK_DRIVER_FILES"].endswith(":/bundled/radeon.json:/bundled/intel.json"))
        # Repeated starts reuse valid links and do not accumulate per-launch caches.
        before = dict(self.env)
        self.env = {k: v for k, v in before.items() if k != "VK_DRIVER_FILES"}
        self.env["LD_LIBRARY_PATH"] = "/bundled/lib"
        self.configure()
        self.assertEqual(self.env, before)

    def test_stale_links_from_an_older_driver_are_repaired(self):
        driver = self.library("libGLX_nvidia.so.0")
        compiler = self.library("libnvidia-glvkspirv.so.580.2")
        self.manifest()
        self.configure()
        cache = Path(self.env["LD_LIBRARY_PATH"].split(":")[0])
        # What an update of the driver leaves behind under the same cache key.
        (cache / "libnvidia-ngx.so.1").symlink_to(self.libs / "libnvidia-ngx.so.580.1")
        (cache / compiler.name).unlink()
        (cache / compiler.name).symlink_to(self.libs / "libnvidia-glvkspirv.so.580.1")
        self.env["LD_LIBRARY_PATH"] = "/bundled/lib"
        del self.env["VK_DRIVER_FILES"]
        self.configure()
        self.assertEqual(Path(self.env["LD_LIBRARY_PATH"].split(":")[0]), cache)
        self.assertEqual((cache / compiler.name).resolve(), compiler)
        self.assertFalse((cache / "libnvidia-ngx.so.1").is_symlink())
        self.assertEqual({link.name for link in cache.iterdir()},
                         {driver.name, compiler.name})

    def test_ostree_driver_update_gets_a_new_cache(self):
        # ostree keeps the unversioned name and a 1970 mtime across updates.
        driver = self.library("libGLX_nvidia.so.0")
        os.utime(driver, ns=(0, 0))
        self.manifest()
        self.configure()
        first = Path(self.env["LD_LIBRARY_PATH"].split(":")[0])
        driver.unlink()
        driver.write_bytes(b"\x7fELF\x02\x01" + b"\0" * 12 + b"\x3e\0" + b"new driver")
        os.utime(driver, ns=(0, 0))
        self.env["LD_LIBRARY_PATH"] = "/bundled/lib"
        del self.env["VK_DRIVER_FILES"]
        self.configure()
        self.assertNotEqual(Path(self.env["LD_LIBRARY_PATH"].split(":")[0]), first)

    def test_absolute_and_relative_icd_paths(self):
        driver = self.library("libGLX_nvidia.so.0")
        for name in (str(driver), "../host-libs/" + driver.name):
            with self.subTest(name=name):
                self.manifest(name)
                data, selected = vulkan.host_nvidia((self.icds,), (self.libs,))
                self.assertEqual(selected, driver)

    def test_32_bit_and_malformed_icds_are_skipped(self):
        self.library("libGLX_nvidia.so.0", bits=32)
        self.manifest()
        (self.icds / "broken_nvidia.json").write_text("not JSON")
        self.configure()
        self.assertEqual(self.env["VK_DRIVER_FILES"], self.env["BB_BUNDLED_VK_DRIVER_FILES"])

    def test_additional_user_icds_are_not_lost(self):
        self.env["VK_ADD_DRIVER_FILES"] = "/user/extra.json"
        self.configure()
        self.assertEqual(self.env["VK_DRIVER_FILES"],
                         "/user/extra.json:/bundled/radeon.json:/bundled/intel.json")

    def test_host_driver_can_be_supplied_outside_hidden_nix_store(self):
        self.library("libGLX_nvidia.so.0")
        self.manifest("/nix/store/hidden/lib/libGLX_nvidia.so.0")
        self.env["BB_NVIDIA_LIB_DIR"] = str(self.libs)
        vulkan.configure(self.env, (self.icds,), ())
        self.assertIn("nvidia_icd.json", self.env["VK_DRIVER_FILES"])
