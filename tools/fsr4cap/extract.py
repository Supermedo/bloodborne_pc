#!/usr/bin/env python3
# bbport: builds the FSR 4.1.1 asset set for vk_fsr411.cpp from fsr4cap captures
# (capture_<render>_<output> directories written by capture_all.sh; fp8/capture_* for the DLL's
# FP8 matrix variant).
#
#   extract.py <dxil-spirv> <capture root> <output dir>
# (spirv-dis, spirv-as from SPIRV-Tools on PATH: the postpass is rewritten by postpass_lds.py.)
#
# Per (tier, model) set: the shaders of one frame translated to SPIR-V (dxil-spirv with
# --class-bindings: binding = register + 32 * class, SRV/UAV/CBV/sampler), named after the pass,
# and the model's initializer (weights). Tiers: t1080 (output up to 1920x1080), t2160 (larger).
# Models: m0 (quality ratios up to 2.0), m1 (ultra performance, 3.0).
# Variants: INT8 in <output dir>/<set> (every GPU); the FP8 matrix variant in fp8/<set>, translated
# for FP8 cooperative matrices (VK_EXT_shader_float8, RDNA4) as vkd3d-proton does there, and in
# fp8emu/<set> with FP8 emulated through FP16 matrices as vkd3d-proton does on RDNA3
# (DXIL_SPIRV_CONFIG=wmma_rdna3_workaround): slower, for testing the variant without RDNA4.
#
# It also checks the rules vk_fsr411.cpp uses against every capture: the dispatch sequence, the
# group counts, the tensor size table and the frame constants. A mismatch is an error.
import glob, hashlib, math, os, re, struct, subprocess, sys

dxil_spirv, root, out = sys.argv[1:4]
FLAGS = ['--enable-shader-i8-dot', '--ssbo-uav', '--ssbo-srv', '--class-bindings', '--use-reflection-names',
         '--mixed-float-dot-product']  # as vkd3d-proton: dot2 of halves into float (VALVE extension)
PREFIX = 'fsr4_model_v07_fp8_no_scale_'
SEQUENCE = ['spd', 'prepass', 'pass0_post'] + [f'pass{k}{s}' for k in range(1, 13) for s in ('', '_post')] + ['postpass', 'rcas']
LEVEL = {1: 1, 2: 1, 3: 2, 4: 2, 5: 2, 6: 3, 7: 3, 8: 3, 9: 3, 10: 2, 11: 2, 12: 1}
POST_LEVEL = [1, 1, 1, 2, 2, 2, 3, 3, 3, 2, 2, 1, 1]  # tensor a _post pass clears (pass0_post: [0])
# Per variant (vk_fsr411.cpp kInt8 / kFp8): tensor elements per group of the model passes at their
# level; output pixels per group of the prepass and postpass; how the _post passes find the
# border: the tensor's width is the tier's aligned to post_align, kx right columns up to
# post_align + 1.
VARIANTS = {
    'int8': {'tile': {k: (64, 1) for k in range(1, 13)}, 'prepass': (16, 16), 'postpass': (32, 32),
             'post_align': 4},
    'fp8': {'tile': {1: (16, 8), 2: (16, 8), 3: (32, 1), 4: (16, 4), 5: (16, 4), 6: (32, 1), 7: (16, 2),
                     8: (16, 2), 9: (16, 1), 10: (16, 4), 11: (16, 1), 12: (16, 8)},
            'prepass': (64, 2), 'postpass': (64, 2), 'post_align': 32},
}
POST_LOCAL_SIZE = 32
# Output folder, extra dxil-spirv flags and environment of each translation of a variant.
TRANSLATIONS = {
    'int8': [('', [], {})],
    'fp8': [('fp8', ['--full-wmma', '1', '0'], {}),
            ('fp8emu', [], {'DXIL_SPIRV_CONFIG': 'wmma_rdna3_workaround'})],
}
errors = 0

mismatches = []

def fail(msg):
    global errors
    errors += 1
    mismatches.append(msg)
    print('MISMATCH', msg)

def shader_name(path):
    data = open(path, 'rb').read()
    for s in re.findall(rb'[A-Za-z_][A-Za-z0-9_]{5,80}', data):
        s = s.decode()
        if s.startswith(PREFIX):
            return s[len(PREFIX):]
        if s.startswith('fsr_rcas'):
            return 'rcas'
    return 'spd'

def ceil_div(a, b):
    return (a + b - 1) // b

def tensor_sizes(aw, ah):
    d = [1, 0, 1, 1, 2, 2, 2, 3, 3, 3, 2, 2, 1, 1, 0, 0, 0]
    return [(aw >> s, ah >> s) for s in d]

def expected_groups(name, rw, rh, ow, oh, variant):
    rules = VARIANTS[variant]
    aw, ah = (ow + 7) & ~7, (oh + 7) & ~7
    if name == 'spd':
        return (ceil_div(rw, 64), ceil_div(rh, 64), 1)
    if name == 'rcas':
        return (ceil_div(aw, 16), ceil_div(ah, 16), 1)
    if name in ('prepass', 'postpass'):
        tx, ty = rules[name]
        return (ceil_div(aw, tx), ceil_div(ah, ty), 1)
    m = re.fullmatch(r'pass(\d+)', name)
    if m:
        level = LEVEL[int(m[1])]
        tx, ty = rules['tile'][int(m[1])]
        return (ceil_div(aw >> level, tx), ceil_div(ah >> level, ty), 1)
    # _post: clears the border of a tensor (the shader stops threads past it).
    level = POST_LEVEL[int(re.fullmatch(r'pass(\d+)_post', name)[1])]
    w, h = aw >> level, ah >> level
    t2160 = ow > 1920 or oh > 1080
    tw, th = (3840 if t2160 else 1920) >> level, (2160 if t2160 else 1080) >> level
    align = rules['post_align']
    kx = min((tw + align - 1) // align * align + 1 - w, align + 1)
    ky = min(th + 1 - h, 1)
    threads = (w + 1 + kx) + h + h * kx + (w + 1 + kx) * ky
    return (ceil_div(threads, POST_LOCAL_SIZE), 1, 1)

variants = {}
captures = [('int8', c) for c in sorted(glob.glob(os.path.join(root, 'capture_*')))]
captures += [('fp8', c) for c in sorted(glob.glob(os.path.join(root, 'fp8', 'capture_*')))]
for variant, cap in captures:
    sets = variants.setdefault(variant, {})
    m = re.search(r'capture_(\d+)x(\d+)_(\d+)x(\d+)$', cap)
    rw, rh, ow, oh = map(int, m.groups())
    tier = 't1080' if ow <= 1920 and oh <= 1080 else 't2160'
    model = 'm1' if ow / rw > 2.5 else 'm0'
    trace_path = os.path.join(cap, 'trace.txt')
    trace = open(trace_path).read() if os.path.isfile(trace_path) else ''
    # Frame 1 (pipelines exist), or frame 0 of a one-frame run.
    mark = 'MARK frame 1\n' if 'MARK frame 1\n' in trace else 'MARK frame 0\n'
    if mark not in trace:
        # The upscaler never ran: an older Proton (vkd3d-proton) loads the DLL's FSR 2/3 providers
        # only (issue #4), or the DLL is not 4.1.x. capture_all.sh stops on it first.
        fail(f'{cap}: no frame recorded (see fsr4cap.log: the 4.1.1 upscaler did not run)')
        continue
    frame = re.split(r'MARK (?:frame \d+|end)\n', trace.split(mark)[1])[0]
    disp = re.findall(r'DISPATCH #\d+ cs=(\w+) root=\w+ groups=(\d+),(\d+),(\d+)\n((?:  .*\n)*)', frame)
    names = [shader_name(os.path.join(cap, f'cs_{h}.dxil')) for h, *_ in disp]
    if names != SEQUENCE:
        fail(f'{cap}: sequence {names}')
        continue
    key = f'{tier}_{model}'
    entry = sets.setdefault(key, {'shaders': {}, 'init': None, 'caps': []})
    entry['caps'].append(os.path.basename(cap))
    for (h, gx, gy, gz, body), name in zip(disp, names):
        groups = (int(gx), int(gy), int(gz))
        want = expected_groups(name, rw, rh, ow, oh, variant)
        if want != groups:
            fail(f'{cap} {name}: groups {groups}, rule {want}')
        prev = entry['shaders'].get(name)
        if prev and prev != h:
            fail(f'{cap} {name}: shader {h} differs from {prev} in the same set')
        entry['shaders'][name] = h
        entry.setdefault('dxil', {})[name] = os.path.join(cap, f'cs_{h}.dxil')
        cbv = re.search(r'CBV r\d+\+\d+ data=(data_\w+\.bin)', body)
        data = open(os.path.join(cap, cbv[1]), 'rb').read() if cbv else b''
        if data and re.fullmatch(r'pass\d+(_post)?', name):
            u = struct.unpack('<68I', data[:272])
            got = [u[i:i + 2] for i in range(0, 68, 4)]
            if got != tensor_sizes((ow + 7) & ~7, (oh + 7) & ~7):
                fail(f'{cap} {name}: tensor sizes {got}')
        if name == 'prepass':
            f = struct.unpack('<16f', data[:64])
            u = struct.unpack('<10I', data[64:104])
            want_f = [1 / ow, 1 / oh, ow / rw, oh / rh, rw / ow, rh / oh, None, None, 1 / rw, 1 / rh, ow, oh, ow, oh, 0, 0]
            for i, w in enumerate(want_f):
                if w is not None and abs(f[i] - w) > 1e-6 * max(1, abs(w)):
                    fail(f'{cap} prepass constant {i}: {f[i]} != {w}')
            if (u[0], u[1], u[3], u[4]) != (ow, oh, rw, rh):
                fail(f'{cap} prepass sizes {u[:5]}')
    init = re.search(r'COPYBUFFER r\d+\+0 <- r\d+\+0 size 131072 data=(data_\w+\.bin)', trace)
    if not init:
        fail(f'{cap}: no model initializer upload recorded')
        continue
    blob = open(os.path.join(cap, init[1]), 'rb').read()
    if entry['init'] and entry['init'] != blob:
        fail(f'{cap}: initializer differs within {key}')
    entry['init'] = blob

# Nothing is written from captures that break the rules: vk_fsr411.cpp would replay them wrongly,
# and postpass_lds.py only knows the postpass of the expected variant (issue #12).
if errors:
    print(f'{errors} mismatches: no assets written.')
    if any('groups' in line for line in mismatches):
        print('The DLL dispatched other group counts than bbport replays (its INT8 variant in '
              'capture_*, the FP8 matrix one in fp8/): another DLL version? Report the GPU, the '
              'DLL version and this output.')
    sys.exit(6)


def translate(path, spv, flags, env):
    """DXIL -> SPIR-V; returns the shader's wave size heuristic (vkd3d-proton makes it the
    required subgroup size) and whether it uses cooperative matrices."""
    run_env = dict(os.environ, **env)
    subprocess.run([dxil_spirv, path, *FLAGS, *flags, '--output', spv], check=True,
                   stderr=subprocess.DEVNULL, env=run_env)
    asm = subprocess.run([dxil_spirv, path, *FLAGS, *flags, '--asm'], check=True, capture_output=True,
                         text=True, env=run_env).stdout
    wave = re.search(r'// HeuristicWaveSize\((\d+)\)', asm)
    return (int(wave[1]) if wave else 0), 'OpCapability CooperativeMatrixKHR' in asm


def write_set(d, entry, variant, flags, env):
    os.makedirs(d, exist_ok=True)
    for name, path in entry['dxil'].items():
        spv = os.path.join(d, f'{name}.spv')
        wave, matrices = translate(path, spv, flags, env)
        # vk_fsr411.cpp requires wave32 for the passes with cooperative matrices: these must be the
        # ones for which vkd3d-proton picks wave32 (WMMA is a wave32 operation on RDNA).
        if (wave == 32) != matrices or wave not in (0, 32):
            sys.exit(f'{d}/{name}: wave size heuristic {wave}, cooperative matrices {matrices}: '
                     f'vk_fsr411.cpp would pick another subgroup size')
        if name == 'postpass' and variant == 'int8':
            # Stores through workgroup memory (bit-exact, ~2.3x faster): postpass_lds.py.
            os.replace(spv, os.path.join(d, 'postpass_orig.spv'))
            asm = subprocess.run(['spirv-dis', os.path.join(d, 'postpass_orig.spv')], check=True,
                                 capture_output=True, text=True).stdout
            run = subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), 'postpass_lds.py')],
                                 input=asm, capture_output=True, text=True)
            if run.returncode != 0:
                sys.exit(f'postpass_lds.py failed on {d}/postpass:\n{run.stderr}')
            lds = run.stdout
            subprocess.run(['spirv-as', '--target-env', 'spv1.3', '-', '-o', spv], input=lds, check=True,
                           text=True)
    open(os.path.join(d, 'initializer.bin'), 'wb').write(entry['init'])


for variant, sets in sorted(variants.items()):
    for folder, flags, env in TRANSLATIONS[variant]:
        for key, entry in sorted(sets.items()):
            write_set(os.path.join(out, folder, key), entry, variant, flags, env)
            print(f'{os.path.join(folder, key)}: {len(entry["dxil"])} shaders, initializer '
                  f'{hashlib.sha256(entry["init"]).hexdigest()[:12]}, from {len(entry["caps"])} captures')
if errors:
    print(f'{errors} mismatches')
    sys.exit(6)
print('all rules match the captures')
