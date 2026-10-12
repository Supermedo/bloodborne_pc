#!/usr/bin/env python3
"""Checks camera motion vectors against the GPU's object vectors in an upscaler dump.

    nix-shell -p "python3.withPackages (p: [p.numpy])" --run "python3 tools/mv_check.py out/dump_x out/ab_x.log"

For each dumped frame: camera vectors recomputed from the logged matrices (Dump camera lines) as
camera_motion.comp does, with the projection's y scale as logged and negated; the composited
`motion` image; all compared with `objects` (rg pixels, b valid, a depth) on valid pixels whose
depth matches the scene depth. The convention with the smaller error is the right one there.
"""
import re, sys
from pathlib import Path
import numpy as np

def load(d, f, name):
    p = next(Path(d).glob(f'f{f:03d}_{name}_*'))
    w, h = map(int, re.search(r'_(\d+)x(\d+)_', p.name).groups())
    fmt = p.name.rsplit('_', 1)[1][:-4]
    raw = p.read_bytes()
    if fmt == 'f32':
        return np.frombuffer(raw, np.float32).reshape(h, w)
    if fmt == 'rg16f':
        return np.frombuffer(raw, np.float16).astype(np.float32).reshape(h, w, 2)
    if fmt == 'rgba32f':
        return np.frombuffer(raw, np.float32).reshape(h, w, 4)
    raise ValueError(fmt)

def affine(v):
    m = np.eye(4)
    m[:3, :] = np.array(v, dtype=np.float64).reshape(3, 4)
    return m

def camera_vectors(depth, cam, py_sign):
    h, w = depth.shape
    proj = np.array(cam['proj']); prev_proj = np.array(cam['prev_proj'])
    proj[1] *= py_sign; prev_proj[1] *= py_sign
    jitter = np.array(cam['jitter'])
    R = affine(cam['prev_view']) @ np.linalg.inv(affine(cam['view']))
    ys, xs = np.mgrid[0:h, 0:w]
    uv = np.stack([(xs + 0.5) / w, (ys + 0.5) / h], -1)
    suv = uv - jitter / np.array([w, h])
    ndc = suv * 2 - 1
    sky = depth >= 1.0
    z = np.where(sky, 1.0, proj[3] / np.minimum(depth - proj[2], -1e-8))
    v = np.stack([ndc[..., 0] * z / proj[0], ndc[..., 1] * z / proj[1], z, np.where(sky, 0.0, 1.0)], -1)
    pv = v @ R[:3, :].T
    pndc = np.stack([pv[..., 0] * prev_proj[0], pv[..., 1] * prev_proj[1]], -1) / np.maximum(pv[..., 2:3], 1e-4)
    puv = pndc * 0.5 + 0.5
    return (puv - suv) * np.array([w, h])

def main(d, log):
    cams = {}
    for line in open(log, errors='replace'):
        if not line.startswith('Dump camera: frame'):
            continue
        t = line.split()
        f = int(t[3]); vals = {}
        i = 4
        while i < len(t):
            name = t[i]; n = {'view': 12, 'prev_view': 12, 'proj': 4, 'prev_proj': 4, 'jitter': 2, 'prev_jitter': 2}[name]
            vals[name] = [float(x) for x in t[i + 1:i + 1 + n]]; i += 1 + n
        cams[f] = vals  # the last dump's lines win
    frames = sorted(int(p.name[1:4]) for p in Path(d).glob('f*_objects_*'))
    tot = {'logged': [], 'negated': [], 'composite': []}
    for f in frames:
        depth = load(d, f, 'depth'); obj = load(d, f, 'objects'); comp = load(d, f, 'motion')
        valid = (obj[..., 2] > 0.99) & (np.abs(obj[..., 3] - depth) < np.maximum(2e-7, 0.01 * (1.00001669 - depth))) & (depth < 1)
        if valid.sum() < 1000 or f not in cams:
            print(f'frame {f}: {valid.sum()} object pixels, skipped'); continue
        truth = obj[..., :2][valid]
        out = []
        for name, s in (('logged', 1.0), ('negated', -1.0)):
            mv = camera_vectors(depth, cams[f], s)[valid]
            err = np.linalg.norm(mv - truth, axis=1)
            tot[name].append(np.median(err)); out.append(f'{name} median {np.median(err):.3f} px p75 {np.percentile(err, 75):.3f}')
        err = np.linalg.norm(comp[valid] - truth, axis=1)
        tot['composite'].append(np.median(err))
        mag = np.median(np.linalg.norm(truth, axis=1))
        print(f'frame {f}: {valid.sum()} px, |objects| median {mag:.2f} px; ' + '; '.join(out) + f'; dumped motion median {np.median(err):.3f}')
    for k, v in tot.items():
        if v:
            print(f'{k}: median over frames {np.median(v):.3f} px')

main(sys.argv[1], sys.argv[2])
