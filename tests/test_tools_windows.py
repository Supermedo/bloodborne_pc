"""CPU unit tests for Windows diagnostic tools in tools/windows/."""
import csv
import json
import math
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

# Resolve ROOT repo-relative from this file: tests/ -> root
ROOT = Path(__file__).resolve().parent.parent
TOOLS_WIN = ROOT / "tools" / "windows"


class AnalyzeFgGuidesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def create_fixture(self, rw=16, rh=8, ow=32, oh=16, depth_vals=None, motion_vals=None, out_rgba_sizes=None):
        manifest = {
            "width": rw,
            "height": rh,
            "output_width": ow,
            "output_height": oh,
            "depth_format": 100,
            "motion_format": 83,
            "render_frame_index": 42,
            "frame_index": 42,
            "reflex_frame_id": 100,
            "frame_ms": 16.6667,
            "reset": False,
            "generated_count": 1,
            "jitter": [0.0, 0.0],
            "camera": {"near_plane": 0.1, "far_plane": 1000.0, "vertical_fov": 0.785},
        }
        (self.dir / "guides_manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

        # R32 depth
        if depth_vals is None:
            depth_vals = [0.5] * (rw * rh)
        depth_bytes = struct.pack(f"<{len(depth_vals)}f", *depth_vals)
        (self.dir / "depth.bin").write_bytes(depth_bytes)

        # RG16 motion (half float 'e')
        if motion_vals is None:
            motion_vals = [0.0] * (rw * rh * 2)
        motion_bytes = struct.pack(f"<{len(motion_vals)}e", *motion_vals)
        (self.dir / "motion.bin").write_bytes(motion_bytes)

        # Output RGBA8 files
        if out_rgba_sizes is None:
            out_rgba_sizes = {
                "color_rgba8.raw": ow * oh * 4,
                "full_rgba8.raw": ow * oh * 4,
                "generated_1_rgba8.raw": ow * oh * 4,
            }
        for name, size in out_rgba_sizes.items():
            (self.dir / name).write_bytes(b"\x80" * size)

    def test_valid_guides_and_outputs(self):
        self.create_fixture()
        res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_fg_guides.py"), str(self.dir)],
                             capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"STDOUT: {res.stdout}\nSTDERR: {res.stderr}")
        self.assertIn("Render dimensions: 16x8, Output dimensions: 32x16", res.stdout)
        self.assertIn("Depth (R32): min=0.500000, max=0.500000", res.stdout)
        self.assertIn("color_rgba8.raw): 2048 bytes verified", res.stdout)

    def test_reject_nonfinite_depth(self):
        self.create_fixture(depth_vals=[math.nan] + [0.5] * (16 * 8 - 1))
        res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_fg_guides.py"), str(self.dir)],
                             capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("non-finite count=1", res.stdout)

    def test_reject_out_of_range_depth(self):
        self.create_fixture(depth_vals=[1.5] + [0.5] * (16 * 8 - 1))
        res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_fg_guides.py"), str(self.dir)],
                             capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("Depth values outside 0..1: 1", res.stdout)

    def test_reject_nonfinite_motion(self):
        self.create_fixture(motion_vals=[math.inf] + [0.0] * (16 * 8 * 2 - 1))
        res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_fg_guides.py"), str(self.dir)],
                             capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("Motion non-finite component count: 1", res.stdout)

    def test_reject_rgba_output_size_mismatch(self):
        # Expected is 32*16*4 = 2048, give wrong size
        self.create_fixture(out_rgba_sizes={"color_rgba8.raw": 1000})
        res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_fg_guides.py"), str(self.dir)],
                             capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("size mismatch", res.stdout)

    def test_reject_missing_manifest(self):
        res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_fg_guides.py"), str(self.dir)],
                             capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("manifest not found", res.stdout)


class AnalyzePresentmonTests(unittest.TestCase):
    def test_presentmon_v2_schema(self):
        # PresentMon v2 with TimeInMs and PascalCase headers
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as f:
            writer = csv.writer(f)
            writer.writerow(["Application", "ProcessID", "SwapChainAddress", "TimeInMs", "MsBetweenDisplayChange", "MsBetweenPresents", "PresentMode"])
            for t in range(0, 1000, 16):
                writer.writerow(["bbport.exe", "1234", "0xABCD", str(t), "16.666", "16.666", "Composed: Independent Flip"])
            f_path = Path(f.name)

        try:
            res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_presentmon.py"), str(f_path), "--after-ms", "100", "--before-ms", "800"],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0)
            self.assertIn("bbport.exe", res.stdout)
            self.assertIn("actual display cadence", res.stdout)
            self.assertIn("display interval ms", res.stdout)
        finally:
            f_path.unlink(missing_ok=True)

    def test_presentmon_v1_actual_header_schema(self):
        # PresentMon 2.6 v1 actual schema with TimeInSeconds and mixed/camelCase headers
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as f:
            writer = csv.writer(f)
            writer.writerow([
                "Application", "ProcessID", "SwapChainAddress", "Runtime", "SyncInterval",
                "PresentFlags", "Dropped", "TimeInSeconds", "msInPresentAPI", "msBetweenPresents",
                "AllowsTearing", "PresentMode", "msUntilRenderComplete", "msUntilDisplayed",
                "msBetweenDisplayChange", "msFlipDelay", "msUntilRenderStart", "msGPUActive", "msSinceInput"
            ])
            # 80s to 120s window with ~60 FPS intervals
            for frame_idx in range(5000, 7500):
                t_sec = frame_idx * 0.016666667
                writer.writerow([
                    "bb-probe.exe", "9208", "0x000000005923C3A0", "DXGI", "1", "0", "0",
                    f"{t_sec:.8f}", "0.5", "16.667", "0", "Hardware Composed: Independent Flip",
                    "8.0", "16.0", "16.667", "0.0", "0.0", "8.0", "0.0"
                ])
            f_path = Path(f.name)

        try:
            res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_presentmon.py"), str(f_path),
                                  "--after-ms", "85000", "--before-ms", "115000"],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0)
            self.assertIn("bb-probe.exe PID 9208 swapchain 0x000000005923C3A0", res.stdout)
            self.assertIn("actual display cadence: 60.00 FPS", res.stdout)
            self.assertIn("Hardware Composed: Independent Flip", res.stdout)
        finally:
            f_path.unlink(missing_ok=True)


class AnalyzeReflexTests(unittest.TestCase):
    def load_module(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("reflex_analysis", TOOLS_WIN / "analyze_reflex.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_cpu_owned_latency_uses_clock_units_and_unique_frame_ids(self):
        module = self.load_module()
        # The printed decimal is informational: compute from nanosecond clocks,
        # deduplicate IDs, reject negative intervals and trim by present time.
        text = "\n".join([
            "reflex cpu: id=1 sampled_ns=1000000000 present_ns=1040000000 latency_ms=999.000",
            "reflex cpu: id=1 sampled_ns=1000000000 present_ns=1040000000 latency_ms=999.000",
            "reflex cpu: id=2 sampled_ns=2000000000 present_ns=2030000000 latency_ms=30.000",
            "reflex cpu: id=3 sampled_ns=3000000000 present_ns=2990000000 latency_ms=0.000",
        ])
        all_rows = module.summarize(text, 0)
        self.assertEqual(all_rows["cpu_owned_frames"], 2)
        self.assertEqual(all_rows["cpu_input_to_present_ms"]["p99"], 40)
        tail = module.summarize(text, 1)
        self.assertEqual(tail["cpu_owned_frames"], 1)
        self.assertEqual(tail["cpu_input_to_present_ms"]["p50"], 30)

    def test_cpu_repeated_presents_expose_first_last_and_repeat_counts(self):
        module = self.load_module()
        # One input cycle presents three times at distinct timestamps.
        text = "\n".join([
            "reflex cpu: id=7 sampled_ns=1000000000 present_ns=1010000000 latency_ms=10.000",
            "reflex cpu: id=7 sampled_ns=1000000000 present_ns=1020000000 latency_ms=20.000",
            "reflex cpu: id=7 sampled_ns=1000000000 present_ns=1030000000 latency_ms=30.000",
        ])
        data = module.summarize(text, 0)
        self.assertEqual(data["cpu_input_cycles"], 1)
        self.assertEqual(data["cpu_owned_frames"], 1)
        self.assertEqual(data["cpu_repeated_present_records"], 2)
        self.assertEqual(data["cpu_first_present_ms"]["p50"], 10)
        self.assertEqual(data["cpu_last_present_ms"]["p50"], 30)
        # Backward-compatible field keeps describing the last presentation.
        self.assertEqual(data["cpu_input_to_present_ms"]["p50"], 30)

    def test_cpu_exact_duplicate_records_dedupe(self):
        module = self.load_module()
        # A repeated query of the same cycle must not look like a second present.
        text = "\n".join([
            "reflex cpu: id=4 sampled_ns=1000000000 present_ns=1004000000 latency_ms=4.000",
            "reflex cpu: id=4 sampled_ns=1000000000 present_ns=1004000000 latency_ms=4.000",
        ])
        data = module.summarize(text, 0)
        self.assertEqual(data["cpu_input_cycles"], 1)
        self.assertEqual(data["cpu_owned_frames"], 1)
        self.assertEqual(data["cpu_repeated_present_records"], 0)
        self.assertEqual(data["cpu_first_present_ms"]["p50"], 4)
        self.assertEqual(data["cpu_last_present_ms"]["p50"], 4)

    def test_cpu_invalid_samples_and_timestamps_excluded(self):
        module = self.load_module()
        text = "\n".join([
            "reflex cpu: id=3 sampled_ns=3000000000 present_ns=2990000000 latency_ms=0.000",
            "reflex cpu: id=5 sampled_ns=0 present_ns=1000000 latency_ms=1.000",
            "reflex cpu: id=6 sampled_ns=6000000000 present_ns=6030000000 latency_ms=30.000",
        ])
        data = module.summarize(text, 0)
        # Only the cycle with a valid sample and present>=sample is measured.
        self.assertEqual(data["cpu_input_cycles"], 1)
        self.assertEqual(data["cpu_owned_frames"], 1)
        self.assertEqual(data["cpu_first_present_ms"]["p50"], 30)
        self.assertEqual(data["cpu_last_present_ms"]["p50"], 30)

    def test_cpu_tail_trims_by_present_time(self):
        module = self.load_module()
        # id=9's first present (10 ms) predates the tail window; its later
        # present anchors the window, so only the in-window present is reported.
        text = "\n".join([
            "reflex cpu: id=9 sampled_ns=1000000000 present_ns=1010000000 latency_ms=10.000",
            "reflex cpu: id=9 sampled_ns=1000000000 present_ns=4000000000 latency_ms=3000.000",
        ])
        full = module.summarize(text, 0)
        self.assertEqual(full["cpu_first_present_ms"]["p50"], 10)
        self.assertEqual(full["cpu_last_present_ms"]["p50"], 3000)
        self.assertEqual(full["cpu_repeated_present_records"], 1)
        tail = module.summarize(text, 2)
        # Window is [2.0s, 4.0s]: the 10 ms present is trimmed, so the honest
        # first-present value inside the window is 3000 ms.
        self.assertEqual(tail["cpu_input_cycles"], 1)
        self.assertEqual(tail["cpu_owned_frames"], 1)
        self.assertEqual(tail["cpu_first_present_ms"]["p50"], 3000)
        self.assertEqual(tail["cpu_last_present_ms"]["p50"], 3000)
        self.assertEqual(tail["cpu_repeated_present_records"], 0)

    def test_valid_reflex_timing_log_parsing(self):
        lines = []
        for i in range(1, 20):
            # Timestamps in microseconds (16 ms spacing)
            sim0 = i * 16000
            inp = sim0 + 500
            sim1 = sim0 + 2000
            ren0 = sim1 + 1000
            ren1 = ren0 + 4000
            pre0 = ren1 + 500
            pre1 = pre0 + 100
            gpu0 = ren0 + 2000
            gpu1 = pre0 + 3000
            lines.append(f"reflex timing: id={i} input={inp} sim={sim0}..{sim1} render={ren0}..{ren1} present={pre0}..{pre1} gpu={gpu0}..{gpu1}")

        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write("\n".join(lines))
            f_path = Path(f.name)

        try:
            res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_reflex.py"), str(f_path), "--tail-seconds", "60"],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0)
            data = json.loads(res.stdout.split("Driver timestamps")[0])
            self.assertEqual(data["frames"], 19)
            self.assertEqual(data["invalid_reports"], 0)
            self.assertIn("simulation_to_present_ms", data)
        finally:
            f_path.unlink(missing_ok=True)


class AnalyzeStabilityTests(unittest.TestCase):
    def test_valid_stability_csv_parsing(self):
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as f:
            writer = csv.writer(f)
            writer.writerow(["seconds", "private_bytes", "working_set", "handles", "threads", "gpu_global_mib", "gpu_util_percent"])
            for s in range(300, 600, 30):
                writer.writerow([str(s), str(100 * 1024 * 1024), str(150 * 1024 * 1024), "500", "32", "2048", "75.0"])
            f_path = Path(f.name)

        try:
            res = subprocess.run([sys.executable, str(TOOLS_WIN / "analyze_stability.py"), str(f_path), "--after", "300", "--before", "600"],
                                 capture_output=True, text=True)
            self.assertEqual(res.returncode, 0)
            self.assertIn("private_bytes:", res.stdout)
            self.assertIn("working_set:", res.stdout)
            self.assertIn("slope", res.stdout)
        finally:
            f_path.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
