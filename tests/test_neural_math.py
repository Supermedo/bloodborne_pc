"""CPU reference maths for the neural-rendering host shaders (no GPU, no driver).

Two different kinds of check live here.

The OkLab part is checked against external references: the four matrices are parsed out of
neural_resolve.comp, replayed with real GLSL mat3 semantics, and compared with the published
OkLab transform and its published primary-colour values. That is what catches a matrix built the
wrong way round, which is invisible in a shader that still compiles and still runs.

The resolve part is a contract check: the encode/decode helpers below are the documented behaviour
of neural_prepare.comp / neural_resolve.comp, and the tests assert the properties the pipeline
promises -- exact ratio identity when the model returns its own input, residual reconstruction as
the Lanczos3 interpolation of the *decoded* difference, and the 1:1 kernel collapse that the fast
path relies on.
"""
import math
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADER_DIR = ROOT / 'gpu/shadps4/video_core/host_shaders'
RESOLVE_SOURCE = (SHADER_DIR / 'neural_resolve.comp').read_text(encoding='utf-8')
PREPARE_SOURCE = (SHADER_DIR / 'neural_prepare.comp').read_text(encoding='utf-8')

# Published OkLab definition (Bjoern Ottosson), written row-major exactly as documented.
RGB_TO_LMS = [[0.4122214708, 0.5363325363, 0.0514459929],
              [0.2119034982, 0.6806995451, 0.1073969566],
              [0.0883024619, 0.2817188376, 0.6299787005]]
LMS_TO_LAB = [[0.2104542553, 0.7936177850, -0.0040720468],
              [1.9779984951, -2.4285922050, 0.4505937099],
              [0.0259040371, 0.7827717662, -0.8086757660]]
LAB_TO_LMS = [[1.0, 0.3963377774, 0.2158037573],
              [1.0, -0.1055613458, -0.0638541728],
              [1.0, -0.0894841775, -1.2914855480]]
LMS_TO_RGB = [[4.0767416621, -3.3077115913, 0.2309699292],
              [-1.2684380046, 2.6097574011, -0.3413193965],
              [-0.0041960863, -0.7034186147, 1.7076147010]]

# Published linear-sRGB -> OkLab values for the primaries and the greys.
OKLAB_REFERENCE = {
    'black': ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)),
    'white': ((1.0, 1.0, 1.0), (1.0, 0.0, 0.0)),
    'red': ((1.0, 0.0, 0.0), (0.6279554, 0.2248630, 0.1258463)),
    'green': ((0.0, 1.0, 0.0), (0.8664396, -0.2338875, 0.1794985)),
    'blue': ((0.0, 0.0, 1.0), (0.4520137, -0.0324569, -0.3115281)),
    'mid_gray': ((0.5, 0.5, 0.5), (0.5 ** (1.0 / 3.0), 0.0, 0.0)),
}


def _strip_comments(text):
    return re.sub(r'//[^\n]*', '', text)


def parse_glsl_matrices(source):
    """Replay `const mat3 name = [transpose(]mat3(...)[)]` with real GLSL mat3 semantics.

    GLSL's mat3(a,b,c, d,e,f, g,h,i) builds *columns* from its arguments, so the literal order in
    the source is only the intended matrix when the call is wrapped in transpose().
    """
    matrices = {}
    transposed = {}
    body = _strip_comments(source)
    for match in re.finditer(r'const\s+mat3\s+(\w+)\s*=\s*([^;]*);', body, re.S):
        name, rhs = match.group(1), match.group(2)
        literal = re.search(r'mat3\(([^)]*)\)', rhs)
        if literal is None:
            continue
        numbers = [float(token) for token
                   in re.findall(r'[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?', literal.group(1))]
        if len(numbers) != 9:
            raise AssertionError(f'{name}: expected 9 literals, found {len(numbers)}')
        columns = [numbers[0:3], numbers[3:6], numbers[6:9]]
        # element(row, column) == columns[column][row]
        matrix = [[columns[c][r] for c in range(3)] for r in range(3)]
        transposed[name] = rhs.strip().startswith('transpose')
        matrices[name] = transpose3(matrix) if transposed[name] else matrix
    return matrices, transposed


def transpose3(matrix):
    return [[matrix[c][r] for c in range(3)] for r in range(3)]


def mat_vec(matrix, vector):
    return [sum(matrix[r][c] * vector[c] for c in range(3)) for r in range(3)]


def cbrt_signed(value):
    return math.copysign(abs(value) ** (1.0 / 3.0), value)


def reference_to_oklab(rgb):
    lms = [cbrt_signed(v) for v in mat_vec(RGB_TO_LMS, rgb)]
    return mat_vec(LMS_TO_LAB, lms)


def reference_from_oklab(lab):
    lms = mat_vec(LAB_TO_LMS, lab)
    return mat_vec(LMS_TO_RGB, [v ** 3 for v in lms])


# --- documented encode/decode contract of the two shaders -------------------------------

def srgb_to_linear(x):
    x = min(max(x, 0.0), 1.0)
    return ((x + 0.055) / 1.055) ** 2.4 if x > 0.04045 else x / 12.92


def linear_to_srgb(x):
    x = min(max(x, 0.0), 1.0)
    return 1.055 * (x ** (1.0 / 2.4)) - 0.055 if x > 0.0031308 else 12.92 * x


def compress_hdr(x):
    return x / (1.0 + max(x, 0.0))


def expand_hdr(e):
    return min(e / max(1.0 - e, 1.0 / 65504.0), 65504.0)


def prepare_encode(x, encoding, method, white_point):
    """neural_prepare.comp encode_input(): linear, ratio-only white-point normalisation."""
    normalise = method == 2 and white_point > 0.0 and white_point != 1.0
    if encoding == 0 and not normalise:
        return min(max(x, 0.0), 1.0)
    linear = srgb_to_linear(x) if encoding == 0 else max(x, 0.0)
    if normalise:
        linear /= white_point
    if encoding == 2:
        return linear_to_srgb(compress_hdr(linear))
    return linear_to_srgb(linear)


def model_to_linear(y, encoding):
    e = srgb_to_linear(y)
    return expand_hdr(e) if encoding == 2 else e


def input_to_linear(x, encoding):
    return srgb_to_linear(x) if encoding == 0 else x


def linear_to_output(v, encoding):
    if encoding == 2:
        return min(max(v, 0.0), 65504.0)
    if encoding == 1:
        return min(max(v, 0.0), 1.0)
    return linear_to_srgb(v)


def model_to_output_raw(y, encoding):
    return y if encoding == 0 else model_to_linear(y, encoding)


def clamp_output(v, encoding):
    return min(max(v, 0.0), 65504.0) if encoding == 2 else min(max(v, 0.0), 1.0)


LUMA = (0.2126, 0.7152, 0.0722)


def luma(v):
    return sum(a * b for a, b in zip(v, LUMA))


def per_channel(fn, v):
    return [fn(x) for x in v]


def lanczos3(x):
    x = abs(x)
    if x < 1e-5:
        return 1.0
    if x >= 3.0:
        return 0.0
    px = math.pi * x
    return (math.sin(px) / px) * (math.sin(px / 3.0) / (px / 3.0))


def lanczos_weights(point):
    """The 6x6 tap weights the shader uses, keyed by tap offset from floor(point)."""
    base_x, base_y = math.floor(point[0]), math.floor(point[1])
    weights = {}
    for y in range(-2, 4):
        for x in range(-2, 4):
            weights[(x, y)] = (lanczos3(point[0] - (base_x + x)) *
                               lanczos3(point[1] - (base_y + y)))
    return weights


def ratio_resolve_pixel(original, low, model, encoding, white_point, transfer, colour,
                        max_ratio):
    """neural_resolve.comp ratio_resolve() for one pixel, with low/model already interpolated."""
    w = white_point if white_point > 0.0 else 1.0
    cap = max_ratio if max_ratio > 0.0 else 2.0
    transfer = min(max(transfer, 0.0), 1.0)
    colour = min(max(colour, 0.0), 1.0)

    base = [v / w for v in per_channel(lambda x: input_to_linear(x, encoding), original)]
    low_lin = per_channel(lambda x: model_to_linear(x, encoding), low)
    model_lin = per_channel(lambda x: model_to_linear(x, encoding), model)

    base_luma, low_luma, model_luma = luma(base), luma(low_lin), luma(model_lin)
    upgraded = base
    if model_luma > 1e-5:
        if base_luma < low_luma:
            ratio = base_luma / max(low_luma, 1e-6)
        else:
            ratio = (model_luma + max(0.0, base_luma - low_luma)) / model_luma
        scaled = [v * ratio for v in model_lin]
        mixed = hue_oklab(scaled, model_lin)
        upgraded = [b + (m - b) * transfer for b, m in zip(base, mixed)]

    floor_luma = 1.0 / 512.0
    luma_ratio = min(max((luma(upgraded) + floor_luma) / (base_luma + floor_luma), 0.0), cap)
    linear = [(b * luma_ratio) + (u - b * luma_ratio) * colour for b, u in zip(base, upgraded)]
    return per_channel(lambda x: linear_to_output(x * w, encoding), linear)


def hue_oklab(incorrect, correct):
    a = reference_to_oklab(incorrect)
    b = reference_to_oklab(correct)
    chroma_a = math.hypot(a[1], a[2])
    chroma_b = math.hypot(b[1], b[2])
    scale = 1.0 if chroma_b == 0.0 else chroma_a / chroma_b
    a[1], a[2] = b[1] * scale, b[2] * scale
    return [max(v, 0.0) for v in reference_from_oklab(a)]


def lanczos_sample(grid, work_size, full_size, point):
    """The shader's 6x6 clamped Lanczos3 reconstruction of a work-resolution grid."""
    work_w, work_h = work_size
    weights = lanczos_weights(point)
    base_x, base_y = math.floor(point[0]), math.floor(point[1])
    total = [0.0, 0.0, 0.0]
    weight_sum = 0.0
    for (x, y), w in weights.items():
        qx = min(max(base_x + x, 0), work_w - 1)
        qy = min(max(base_y + y, 0), work_h - 1)
        for c in range(3):
            total[c] += grid[qy][qx][c] * w
        weight_sum += w
    divisor = weight_sum if abs(weight_sum) > 1e-6 else 1.0
    return [v / divisor for v in total]


class OkLabMatrixTests(unittest.TestCase):
    def setUp(self):
        self.matrices, self.transposed = parse_glsl_matrices(RESOLVE_SOURCE)

    def test_all_four_matrices_are_transposed_row_major_literals(self):
        for name in ('rgb_to_lms', 'lms_to_lab', 'lab_to_lms', 'lms_to_rgb'):
            self.assertIn(name, self.matrices, f'{name} not found in neural_resolve.comp')
            self.assertTrue(self.transposed[name],
                            f'{name} must be wrapped in transpose() to be usable as matrix * vector')

    def test_transposed_matrices_match_published_oklab(self):
        for name, published in (('rgb_to_lms', RGB_TO_LMS), ('lms_to_lab', LMS_TO_LAB),
                                ('lab_to_lms', LAB_TO_LMS), ('lms_to_rgb', LMS_TO_RGB)):
            for row in range(3):
                for col in range(3):
                    self.assertAlmostEqual(self.matrices[name][row][col], published[row][col],
                                           places=12, msg=f'{name}[{row}][{col}]')

    def test_untransposed_literals_would_be_the_transpose(self):
        """Guards the exact bug: dropping transpose() silently inverts the transform."""
        for name in ('rgb_to_lms', 'lms_to_lab', 'lab_to_lms', 'lms_to_rgb'):
            wrong = transpose3(self.matrices[name])
            self.assertNotAlmostEqual(wrong[0][1], self.matrices[name][0][1], places=6,
                                      msg=f'{name} is symmetric, the orientation test is vacuous')

    def test_shader_oklab_matches_published_primaries(self):
        def shader_to_oklab(rgb):
            lms = [cbrt_signed(v) for v in mat_vec(self.matrices['rgb_to_lms'], rgb)]
            return mat_vec(self.matrices['lms_to_lab'], lms)

        for name, (rgb, expected) in OKLAB_REFERENCE.items():
            actual = shader_to_oklab(rgb)
            for got, want in zip(actual, expected):
                self.assertAlmostEqual(got, want, places=6, msg=f'{name}: {actual} vs {expected}')

    def test_shader_oklab_roundtrip(self):
        def shader_from_oklab(lab):
            lms = mat_vec(self.matrices['lab_to_lms'], lab)
            return mat_vec(self.matrices['lms_to_rgb'], [v ** 3 for v in lms])

        for name, (rgb, _) in OKLAB_REFERENCE.items():
            lms = [cbrt_signed(v) for v in mat_vec(self.matrices['rgb_to_lms'], rgb)]
            lab = mat_vec(self.matrices['lms_to_lab'], lms)
            back = shader_from_oklab(lab)
            for got, want in zip(back, rgb):
                self.assertAlmostEqual(got, want, places=6, msg=f'{name} roundtrip')

    def test_hue_oklab_keeps_model_hue_and_source_chroma(self):
        incorrect = (0.30, 0.42, 0.60)
        correct = (0.55, 0.35, 0.25)
        result = hue_oklab(incorrect, correct)
        lab_result = reference_to_oklab(result)
        lab_incorrect = reference_to_oklab(incorrect)
        lab_correct = reference_to_oklab(correct)
        self.assertAlmostEqual(math.hypot(lab_result[1], lab_result[2]),
                               math.hypot(lab_incorrect[1], lab_incorrect[2]), places=4)
        # Same hue direction as the model, i.e. the a/b angle is taken from `correct`.
        self.assertAlmostEqual(math.atan2(lab_result[2], lab_result[1]),
                               math.atan2(lab_correct[2], lab_correct[1]), places=4)


class RatioIdentityTests(unittest.TestCase):
    """The ratio path must reproduce the original when the model returns its own input.

    The accurate reason is that `model == low` makes the model term collapse: `upgraded` stays
    equal to `base` because the mix is weighted by transfer_strength (0), and the luma ratio is then
    exactly 1. The proxy only feeds `ratio`, which selects the model's chroma, so the identity does
    not require the proxy to match the original -- a proxy with completely different detail still
    reproduces the original exactly. With transfer_strength > 0 the result is *supposed* to differ.
    """

    FULL = (12, 8)
    WORK = (5, 3)

    def _images(self):
        full_w, full_h = self.FULL
        work_w, work_h = self.WORK
        original = []
        for y in range(full_h):
            row = []
            for x in range(full_w):
                # Chromatic high-contrast content: the hue transfer is only observable when the
                # pixels actually have chroma.
                checker = 0.05 + 0.85 * ((x + y) % 2)
                warm = 0.9 if (x // 2 + y) % 2 else 0.15
                cool = 0.85 if (x + y // 2) % 2 else 0.2
                row.append((min(1.0, checker + 0.1 * (x / (full_w - 1))),
                            min(1.0, warm * 0.8 + 0.1),
                            min(1.0, cool * 0.6 + 0.05)))
            original.append(row)
        # Deliberately smooth and unlike the original, so the proxy carries different detail.
        proxy = []
        for _ in range(work_h):
            row = []
            for x in range(work_w):
                t = x / max(work_w - 1, 1)
                row.append((0.15 + 0.7 * t, 0.25 + 0.5 * (1.0 - t), 0.35 + 0.4 * t))
            proxy.append(row)
        return original, proxy

    def _run(self, encoding, white_point, transfer):
        original, proxy = self._images()
        full_w, full_h = self.FULL
        work_w, work_h = self.WORK
        encoded = [[[prepare_encode(proxy[y][x][c], encoding, 2, white_point) for c in range(3)]
                    for x in range(work_w)] for y in range(work_h)]
        for py in range(full_h):
            for px in range(full_w):
                point = ((px + 0.5) * work_w / full_w - 0.5,
                         (py + 0.5) * work_h / full_h - 0.5)
                low = lanczos_sample(encoded, self.WORK, self.FULL, point)
                model = low  # the model returned its own input
                value = original[py][px]
                result = ratio_resolve_pixel(value, low, model, encoding, white_point, transfer,
                                             1.0, 2.0)
                yield (px, py), value, result

    def test_identity_across_encodings_resolutions_and_white_points(self):
        for encoding in (0, 1, 2):
            for white_point in (1.0, 2.0, 0.5, 0.25, 4.0, 0.0):
                worst = 0.0
                for (px, py), value, result in self._run(encoding, white_point, 0.0):
                    for got, want in zip(result, value):
                        self.assertTrue(math.isfinite(got),
                                        f'non-finite result enc={encoding} wp={white_point}')
                        worst = max(worst, abs(got - want))
                self.assertLessEqual(
                    worst, 1e-9,
                    f'enc={encoding} wp={white_point}: identity broken by {worst:.3e}')

    def test_identity_survives_saturation_from_a_small_white_point(self):
        # white_point 0.25 pushes the normalised signal up to 4x, so the encoded model buffer
        # clamps; the identity must still hold because it only depends on model == low.
        saturated = 0
        for (px, py), value, result in self._run(0, 0.25, 0.0):
            # The model buffer clamps once the normalised linear signal passes 1.0.
            if max(srgb_to_linear(c) / 0.25 for c in value) >= 1.0:
                saturated += 1
            for got, want in zip(result, value):
                self.assertAlmostEqual(got, want, places=9)
        self.assertGreater(saturated, 0, 'expected the test image to saturate the model buffer')

    def test_transfer_strength_breaks_the_identity_on_purpose(self):
        changed = 0
        for (px, py), value, result in self._run(0, 1.0, 1.0):
            if any(abs(got - want) > 1e-6 for got, want in zip(result, value)):
                changed += 1
        self.assertGreater(changed, 0, 'transfer_strength 1 should transfer the model chroma')

    def test_direct_and_residual_ignore_the_white_point(self):
        for encoding in (0, 1, 2):
            for value in (0.0, 0.18, 0.5, 0.9, 1.0):
                a = prepare_encode(value, encoding, 0, 2.0)
                b = prepare_encode(value, encoding, 0, 0.5)
                self.assertEqual(a, b, f'direct encode changed with the white point (enc {encoding})')
                c = prepare_encode(value, encoding, 1, 2.0)
                d = prepare_encode(value, encoding, 1, 0.5)
                self.assertEqual(c, d, f'residual encode changed with the white point (enc {encoding})')

    def test_white_point_changes_the_ratio_encoding(self):
        for encoding in (0, 1, 2):
            self.assertNotAlmostEqual(prepare_encode(0.5, encoding, 2, 1.0),
                                      prepare_encode(0.5, encoding, 2, 2.0), places=6,
                                      msg=f'white point had no effect on enc {encoding}')


class ResidualTests(unittest.TestCase):
    """The residual is Lanczos3 of the *decoded* difference, not the decode of two interpolations."""

    WORK = (6, 4)
    FULL = (13, 9)

    def _buffers(self):
        work_w, work_h = self.WORK
        out = [[None] * work_w for _ in range(work_h)]
        inp = [[None] * work_w for _ in range(work_h)]
        for y in range(work_h):
            for x in range(work_w):
                out[y][x] = (0.9 if (x + y) % 2 else 0.05, 0.35, 0.7 if x else 0.1)
                inp[y][x] = (0.6 if x % 2 else 0.15, 0.5, 0.3)
        return out, inp

    def _weights(self, px, py):
        full_w, full_h = self.FULL
        work_w, work_h = self.WORK
        point = ((px + 0.5) * work_w / full_w - 0.5, (py + 0.5) * work_h / full_h - 0.5)
        return point, lanczos_weights(point)

    def test_per_tap_decode_equals_lanczos_of_decoded_difference(self):
        out, inp = self._buffers()
        work_w, work_h = self.WORK
        for encoding in (0, 1, 2):
            for px in range(self.FULL[0]):
                for py in range(self.FULL[1]):
                    point, weights = self._weights(px, py)
                    base_x, base_y = math.floor(point[0]), math.floor(point[1])
                    shader = [0.0, 0.0, 0.0]
                    weight_sum = 0.0
                    for (x, y), w in weights.items():
                        qx = min(max(base_x + x, 0), work_w - 1)
                        qy = min(max(base_y + y, 0), work_h - 1)
                        for c in range(3):
                            decoded = (model_to_output_raw(out[qy][qx][c], encoding) -
                                       model_to_output_raw(inp[qy][qx][c], encoding))
                            shader[c] += w * decoded
                        weight_sum += w
                    divisor = weight_sum if abs(weight_sum) > 1e-6 else 1.0
                    shader = [v / divisor for v in shader]

                    # Independent formulation: build the decoded difference grid first, then
                    # interpolate that grid. Lanczos3 is linear, so these must agree exactly.
                    grid = [[[model_to_output_raw(out[y][x][c], encoding) -
                              model_to_output_raw(inp[y][x][c], encoding) for c in range(3)]
                             for x in range(work_w)] for y in range(work_h)]
                    reference = lanczos_sample(grid, self.WORK, self.FULL, point)
                    for got, want in zip(shader, reference):
                        self.assertAlmostEqual(got, want, places=10,
                                               msg=f'enc {encoding} pixel {px},{py}')

    def test_decode_after_interpolation_differs_for_linear_encodings(self):
        """The old formulation is a different operator once the transfer curve is non-linear."""
        out, inp = self._buffers()
        work_w, work_h = self.WORK
        for encoding, expect_difference in ((0, False), (1, True), (2, True)):
            worst = 0.0
            for px in range(self.FULL[0]):
                for py in range(self.FULL[1]):
                    point, weights = self._weights(px, py)
                    base_x, base_y = math.floor(point[0]), math.floor(point[1])
                    shader = [0.0, 0.0, 0.0]
                    old = [0.0, 0.0, 0.0]
                    weight_sum = 0.0
                    for (x, y), w in weights.items():
                        qx = min(max(base_x + x, 0), work_w - 1)
                        qy = min(max(base_y + y, 0), work_h - 1)
                        for c in range(3):
                            shader[c] += w * (model_to_output_raw(out[qy][qx][c], encoding) -
                                              model_to_output_raw(inp[qy][qx][c], encoding))
                            old[c] += w * out[qy][qx][c]
                        weight_sum += w
                    divisor = weight_sum if abs(weight_sum) > 1e-6 else 1.0
                    shader = [v / divisor for v in shader]
                    old = [v / divisor for v in old]
                    # old = decode(lanczos(output)) - decode(lanczos(input))
                    old_decoded = [model_to_output_raw(old[c], encoding) for c in range(3)]
                    second = [0.0, 0.0, 0.0]
                    weight_sum = 0.0
                    for (x, y), w in weights.items():
                        qx = min(max(base_x + x, 0), work_w - 1)
                        qy = min(max(base_y + y, 0), work_h - 1)
                        for c in range(3):
                            second[c] += w * inp[qy][qx][c]
                        weight_sum += w
                    second = [model_to_output_raw(v / divisor, encoding) for v in second]
                    for c in range(3):
                        worst = max(worst, abs(shader[c] - (old_decoded[c] - second[c])))
            if expect_difference:
                self.assertGreater(worst, 1e-3,
                                   f'enc {encoding}: the two formulations should differ')
            else:
                self.assertLessEqual(worst, 1e-12,
                                     'enc 0 has an identity decode, so both must agree')


class LanczosFastPathTests(unittest.TestCase):
    def test_kernel_collapses_at_one_to_one(self):
        """At 1:1 only the centre tap is non-zero, so the fast path is exact, not an approximation."""
        for px in range(4):
            weights = lanczos_weights((float(px), float(px + 1)))
            self.assertAlmostEqual(weights[(0, 0)], 1.0, places=12)
            for offset, w in weights.items():
                if offset != (0, 0):
                    self.assertAlmostEqual(w, 0.0, places=12,
                                           msg=f'offset {offset} weight {w} at 1:1')

    def test_shader_short_circuits_the_loop_at_one_to_one(self):
        body = _strip_comments(RESOLVE_SOURCE)
        self.assertIn('bool model_is_1to1()', body)
        lanczos = body[body.index('vec3 lanczos_model'):]
        self.assertIn('model_is_1to1()', lanczos[:lanczos.index('for (')],
                      'lanczos_model must take the single-texel path before the tap loop')
        residual = body[body.index('vec3 residual_resolve'):]
        self.assertIn('model_is_1to1()', residual[:residual.index('for (')],
                      'residual_resolve must take the single-texel path before the tap loop')

    def test_fast_path_fetches_match_the_full_loop(self):
        grid = [[[(x + y) / 10.0, x / 10.0, y / 10.0] for x in range(4)] for y in range(4)]
        for px in range(4):
            for py in range(4):
                full_loop = lanczos_sample(grid, (4, 4), (4, 4), (float(px), float(py)))
                for c in range(3):
                    self.assertAlmostEqual(full_loop[c], grid[py][px][c], places=12)


class HdrRangeTests(unittest.TestCase):
    def test_hdr_decode_stays_finite_at_full_scale(self):
        self.assertTrue(math.isfinite(expand_hdr(1.0)))
        self.assertAlmostEqual(expand_hdr(1.0), 65504.0, places=3)
        for value in (0.0, 0.25, 0.5, 0.75, 0.9, 0.99, 0.999):
            self.assertTrue(math.isfinite(expand_hdr(value)))

    def test_hdr_encode_decode_roundtrip(self):
        for value in (0.0, 0.05, 0.18, 0.5, 1.0, 4.0, 40.0, 1000.0):
            encoded = prepare_encode(value, 2, 0, 1.0)
            decoded = model_to_linear(encoded, 2)
            self.assertAlmostEqual(decoded, value, delta=max(1e-4, value * 1e-4))

    def test_sdr_roundtrip_for_linear_encoding(self):
        for value in (0.0, 0.02, 0.18, 0.5, 0.75, 1.0):
            self.assertAlmostEqual(model_to_linear(prepare_encode(value, 1, 0, 1.0), 1),
                                   value, places=6)


if __name__ == '__main__':
    unittest.main()
