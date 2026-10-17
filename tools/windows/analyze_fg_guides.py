"""Inspect raw captured depth and motion guides alongside FG outputs and manifest."""
import argparse
import json
import struct
import math
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Analyze FG guides diagnostic capture dump.")
    parser.add_argument("directory", type=Path, help="Directory containing capture files (e.g. depth.bin, motion.bin, guides_manifest.json)")
    parser.add_argument("--motion-threshold", type=float,
                        help="Count displacement above this many render pixels (diagnostic only; default half width)")
    args = parser.parse_args()
    if args.motion_threshold is not None and (not math.isfinite(args.motion_threshold) or args.motion_threshold <= 0):
        parser.error("--motion-threshold must be finite and positive")
    return args


def analyze_directory(dir_path: Path, motion_threshold=None):
    manifest_path = dir_path / "guides_manifest.json"
    if not manifest_path.is_file():
        print(f"Error: manifest not found at {manifest_path}")
        return 1

    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    # Render dimensions (depth and motion vectors)
    w = manifest.get("width", 0)
    h = manifest.get("height", 0)
    if not isinstance(w, int) or not isinstance(h, int) or w <= 0 or h <= 0:
        raise ValueError(f"invalid guide render dimensions: {w}x{h}")
    if manifest.get("depth_format") != 100 or manifest.get("motion_format") != 83:
        raise ValueError("expected VK_FORMAT_R32_SFLOAT (100) and VK_FORMAT_R16G16_SFLOAT (83)")

    out_w = manifest.get("output_width", w)
    out_h = manifest.get("output_height", h)
    if not isinstance(out_w, int) or not isinstance(out_h, int) or out_w <= 0 or out_h <= 0:
        raise ValueError(f"invalid output dimensions in manifest: {out_w}x{out_h}")

    failed = False
    render_id = manifest.get("render_frame_index", 0)
    frame_id = manifest.get("frame_index", 0)
    reflex_id = manifest.get("reflex_frame_id", 0)
    generated_count = manifest.get("generated_count", 0)

    print(f"Guide Dump Analysis:")
    print(f"  Frame IDs: render={render_id}, guide={frame_id}, reflex={reflex_id}")
    print(f"  Render dimensions: {w}x{h}, Output dimensions: {out_w}x{out_h}")
    print(f"  Jitter: {manifest.get('jitter')}")
    print(f"  Delta time ms: {manifest.get('frame_ms')}, Reset: {manifest.get('reset')}")
    camera = manifest.get("camera", {})
    print(f"  Camera near={camera.get('near_plane')}, far={camera.get('far_plane')}, vfov={camera.get('vertical_fov')}")
    if generated_count:
        print(f"  Generated frames expected: {generated_count}")

    def raw(name, expected_size):
        path = dir_path / name
        if not path.is_file():
            raise FileNotFoundError(f"missing required file: {name}")
        data = path.read_bytes()
        if len(data) != expected_size:
            raise ValueError(f"{name} size mismatch: got {len(data)}, expected {expected_size}")
        return data

    expected_render_bytes = w * h * 4
    depth_data = raw("depth.bin", expected_render_bytes)
    low, high = math.inf, -math.inf
    nonfinite = outside = 0
    for (value,) in struct.iter_unpack("<f", depth_data):
        if not math.isfinite(value):
            nonfinite += 1
            continue
        low, high = min(low, value), max(high, value)
        outside += not -1e-6 <= value <= 1.0 + 1e-6
    print(f"  Depth (R32): min={low:.6f}, max={high:.6f}, non-finite count={nonfinite}")
    print(f"  Depth values outside 0..1: {outside}")
    if nonfinite or outside:
        failed = True

    motion_data = raw("motion.bin", expected_render_bytes)
    xmin = ymin = math.inf
    xmax = ymax = -math.inf
    nonfinite = large = zero = 0
    threshold = motion_threshold if motion_threshold is not None else w * 0.5
    box = [w, h, -1, -1]
    for index, (x, y) in enumerate(struct.iter_unpack("<ee", motion_data)):
        if not math.isfinite(x) or not math.isfinite(y):
            nonfinite += 1
            continue
        xmin, xmax = min(xmin, x), max(xmax, x)
        ymin, ymax = min(ymin, y), max(ymax, y)
        zero += x == 0 and y == 0
        if math.hypot(x, y) > threshold:
            large += 1
            px, py = index % w, index // w
            box = [min(box[0], px), min(box[1], py), max(box[2], px), max(box[3], py)]
    print(f"  Motion (RG16): vx range=[{xmin:.4f}, {xmax:.4f}], vy range=[{ymin:.4f}, {ymax:.4f}]")
    print(f"  Motion non-finite component count: {nonfinite}")
    print(f"  Zero motion: {zero}/{w*h} pixels ({100.0*zero/(w*h):.4f}%)")
    print(f"  Magnitude > {threshold:g} render pixels: {large}/{w*h} ({100.0*large/(w*h):.4f}%), box={box if large else None}")
    if nonfinite:
        failed = True

    # Validate output-sized RGBA8 frames (color_rgba8.raw, full_rgba8.raw, generated_N_rgba8.raw)
    expected_output_bytes = out_w * out_h * 4
    for raw_rgba in sorted(dir_path.glob("*rgba8*.raw")):
        raw_data = raw_rgba.read_bytes()
        if len(raw_data) != expected_output_bytes:
            print(f"  Error: {raw_rgba.name} size mismatch: got {len(raw_data)} bytes, expected {expected_output_bytes} for {out_w}x{out_h}")
            failed = True
        else:
            print(f"  RGBA8 output ({raw_rgba.name}): {len(raw_data)} bytes verified ({out_w}x{out_h})")

    return 1 if failed else 0


def main():
    args = parse_args()
    return analyze_directory(args.directory, args.motion_threshold)


if __name__ == "__main__":
    raise SystemExit(main())
