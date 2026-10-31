# SPDX-License-Identifier: GPL-2.0-or-later
"""Will it run, and how well: the hardware check of bbport.de/install, for this PC.

The tables and the rating are the website's (its HardwareCheck script): a score per graphics
card, per processor generation and the memory size give a tier from "Not supported" to
"4K-ready", the expected frame rate at 1080p, 1440p and 4K, and the suggested upscaler. The
launcher fills them in by itself: the graphics card from bb-gpu-capabilities.exe (Vulkan), the
processor from the registry and CPUID, the memory from Windows. It is a rough guide from the
requirements and the developer's measurements (RX 7800 XT), not a benchmark. "Not supported" is
kept for what stops the port: no Vulkan 1.3, a card far too slow, a processor without AVX. Too
little RAM or video memory is "Below minimum": players run the game with 8 GB and on 3 GB cards.

Texts are English; `tr` translates a template before it is formatted.
"""
import ctypes
import re

# (vendor, group, name, video memory in GB, score, flags); flags: laptop, igpu, no-vulkan13.
GPUS = [
    ('nvidia', 'NVIDIA GeForce', 'RTX 5090', 32, 430, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 5080', 16, 300, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 5070 Ti', 16, 270, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 5070', 12, 215, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 5060 Ti 8 GB', 8, 160, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 5060 Ti 16 GB', 16, 165, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 5060', 8, 140, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 5050', 8, 112, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 4090', 24, 360, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 4080 Super', 16, 285, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 4080', 16, 275, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 4070 Ti Super', 16, 240, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 4070 Ti', 12, 220, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 4070 Super', 12, 205, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 4070', 12, 175, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 4060 Ti 8 GB', 8, 140, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 4060 Ti 16 GB', 16, 142, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 4060', 8, 115, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 3090 Ti', 24, 215, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 3090', 24, 198, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 3080 Ti', 12, 192, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 3080 10 GB', 10, 175, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 3080 12 GB', 12, 182, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 3070 Ti', 8, 148, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 3070', 8, 140, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 3060 Ti', 8, 125, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 3060 12 GB', 12, 100, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 3060 8 GB', 8, 90, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 3050 8 GB', 8, 75, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 3050 6 GB', 6, 62, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 2080 Ti', 11, 155, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 2080 Super', 8, 133, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 2080', 8, 127, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 2070 Super', 8, 118, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 2070', 8, 106, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 2060 Super', 8, 100, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 2060', 6, 88, ''),
    ('nvidia', 'NVIDIA GeForce', 'RTX 2060 12 GB', 12, 92, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1660 Ti', 6, 78, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1660 Super', 6, 76, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1660', 6, 68, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1650 Super', 4, 60, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1650', 4, 48, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1630', 4, 30, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1080 Ti', 11, 118, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1080', 8, 92, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1070 Ti', 8, 86, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1070', 8, 77, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1060 6 GB', 6, 60, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1060 3 GB', 3, 55, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1050 Ti', 4, 38, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 1050', 2, 30, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 980 Ti', 6, 72, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 980', 4, 60, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 970', 4, 52, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 960 2 GB', 2, 36, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 960 4 GB', 4, 36, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 950', 2, 30, ''),
    ('nvidia', 'NVIDIA GeForce', 'GTX 780 Ti', 3, 50, 'no-vulkan13'),
    ('nvidia', 'NVIDIA GeForce', 'GTX 780', 3, 45, 'no-vulkan13'),
    ('nvidia', 'NVIDIA GeForce', 'GTX 770', 2, 38, 'no-vulkan13'),
    ('nvidia', 'NVIDIA GeForce', 'GTX 760', 2, 30, 'no-vulkan13'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 5090 Laptop', 24, 260, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 5080 Laptop', 16, 220, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 5070 Ti Laptop', 12, 175, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 5070 Laptop', 8, 130, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 5060 Laptop', 8, 115, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 5050 Laptop', 8, 95, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 4090 Laptop', 16, 230, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 4080 Laptop', 12, 195, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 4070 Laptop', 8, 125, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 4060 Laptop', 8, 112, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 4050 Laptop', 6, 92, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 3080 Ti Laptop', 16, 150, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 3080 Laptop', 8, 135, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 3070 Ti Laptop', 8, 125, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 3070 Laptop', 8, 115, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 3060 Laptop', 6, 95, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 3050 Ti Laptop', 4, 58, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 3050 Laptop', 4, 52, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 2080 Laptop', 8, 100, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 2070 Laptop', 8, 88, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'RTX 2060 Laptop', 6, 75, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'GTX 1660 Ti Laptop', 6, 65, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'GTX 1650 Ti Laptop', 4, 45, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'GTX 1650 Laptop', 4, 40, 'laptop'),
    ('nvidia', 'NVIDIA GeForce (laptop)', 'GTX 1050 Ti Laptop', 4, 33, 'laptop'),
    ('amd', 'AMD Radeon', 'RX 9070 XT', 16, 275, ''),
    ('amd', 'AMD Radeon', 'RX 9070', 16, 245, ''),
    ('amd', 'AMD Radeon', 'RX 9060 XT 8 GB', 8, 160, ''),
    ('amd', 'AMD Radeon', 'RX 9060 XT 16 GB', 16, 165, ''),
    ('amd', 'AMD Radeon', 'RX 7900 XTX', 24, 290, ''),
    ('amd', 'AMD Radeon', 'RX 7900 XT', 20, 255, ''),
    ('amd', 'AMD Radeon', 'RX 7900 GRE', 16, 220, ''),
    ('amd', 'AMD Radeon', 'RX 7800 XT', 16, 205, ''),
    ('amd', 'AMD Radeon', 'RX 7700 XT', 12, 170, ''),
    ('amd', 'AMD Radeon', 'RX 7600 XT', 16, 128, ''),
    ('amd', 'AMD Radeon', 'RX 7600', 8, 120, ''),
    ('amd', 'AMD Radeon', 'RX 6950 XT', 16, 225, ''),
    ('amd', 'AMD Radeon', 'RX 6900 XT', 16, 215, ''),
    ('amd', 'AMD Radeon', 'RX 6800 XT', 16, 200, ''),
    ('amd', 'AMD Radeon', 'RX 6800', 16, 175, ''),
    ('amd', 'AMD Radeon', 'RX 6750 XT', 12, 152, ''),
    ('amd', 'AMD Radeon', 'RX 6700 XT', 12, 145, ''),
    ('amd', 'AMD Radeon', 'RX 6700', 10, 130, ''),
    ('amd', 'AMD Radeon', 'RX 6650 XT', 8, 120, ''),
    ('amd', 'AMD Radeon', 'RX 6600 XT', 8, 115, ''),
    ('amd', 'AMD Radeon', 'RX 6600', 8, 100, ''),
    ('amd', 'AMD Radeon', 'RX 6500 XT', 4, 50, ''),
    ('amd', 'AMD Radeon', 'RX 6400', 4, 40, ''),
    ('amd', 'AMD Radeon', 'RX 5700 XT', 8, 112, ''),
    ('amd', 'AMD Radeon', 'RX 5700', 8, 100, ''),
    ('amd', 'AMD Radeon', 'RX 5600 XT', 6, 88, ''),
    ('amd', 'AMD Radeon', 'RX 5500 XT 8 GB', 8, 64, ''),
    ('amd', 'AMD Radeon', 'RX 5500 XT 4 GB', 4, 60, ''),
    ('amd', 'AMD Radeon', 'Radeon VII', 16, 115, ''),
    ('amd', 'AMD Radeon', 'RX Vega 64', 8, 95, ''),
    ('amd', 'AMD Radeon', 'RX Vega 56', 8, 85, ''),
    ('amd', 'AMD Radeon', 'RX 590', 8, 70, ''),
    ('amd', 'AMD Radeon', 'RX 580 8 GB', 8, 64, ''),
    ('amd', 'AMD Radeon', 'RX 580 4 GB', 4, 62, ''),
    ('amd', 'AMD Radeon', 'RX 570 4 GB', 4, 58, ''),
    ('amd', 'AMD Radeon', 'RX 570 8 GB', 8, 58, ''),
    ('amd', 'AMD Radeon', 'RX 480', 8, 60, ''),
    ('amd', 'AMD Radeon', 'RX 470', 4, 55, ''),
    ('amd', 'AMD Radeon', 'RX 560', 4, 32, ''),
    ('amd', 'AMD Radeon', 'RX 550', 2, 20, ''),
    ('amd', 'AMD Radeon', 'R9 390 / 390X', 8, 60, 'no-vulkan13'),
    ('amd', 'AMD Radeon', 'R9 290 / 290X', 4, 52, 'no-vulkan13'),
    ('amd', 'AMD Radeon', 'R9 380 / 285', 4, 42, 'no-vulkan13'),
    ('amd', 'AMD Radeon', 'R9 280 / 280X / HD 7970', 3, 40, 'no-vulkan13'),
    ('amd', 'AMD Radeon (laptop)', 'RX 7600M XT', 8, 110, 'laptop'),
    ('amd', 'AMD Radeon (laptop)', 'RX 6800M', 12, 140, 'laptop'),
    ('amd', 'AMD Radeon (laptop)', 'RX 6700M', 10, 115, 'laptop'),
    ('amd', 'AMD Radeon (laptop)', 'RX 6600M', 8, 95, 'laptop'),
    ('amd', 'AMD Radeon (laptop)', 'RX 6500M', 4, 45, 'laptop'),
    ('amd', 'AMD Radeon (laptop)', 'RX 5500M', 4, 50, 'laptop'),
    ('intel', 'Intel Arc', 'Arc B580', 12, 135, ''),
    ('intel', 'Intel Arc', 'Arc B570', 10, 120, ''),
    ('intel', 'Intel Arc', 'Arc A770 16 GB', 16, 115, ''),
    ('intel', 'Intel Arc', 'Arc A770 8 GB', 8, 112, ''),
    ('intel', 'Intel Arc', 'Arc A750', 8, 105, ''),
    ('intel', 'Intel Arc', 'Arc A580', 8, 95, ''),
    ('intel', 'Intel Arc', 'Arc A380', 6, 50, ''),
    ('intel', 'Intel Arc', 'Arc A310', 4, 30, ''),
    ('intel', 'Intel Arc', 'Arc A770M (laptop)', 16, 95, 'laptop'),
    ('intel', 'Intel Arc', 'Arc A730M (laptop)', 12, 80, 'laptop'),
    ('intel', 'Intel Arc', 'Arc A370M (laptop)', 4, 40, 'laptop'),
    ('amd', 'Integrated graphics (AMD)', 'Radeon 890M (Ryzen AI 300)', 0, 60, 'igpu'),
    ('amd', 'Integrated graphics (AMD)', 'Radeon 880M (Ryzen AI 300)', 0, 52, 'igpu'),
    ('amd', 'Integrated graphics (AMD)', 'Radeon 780M (Ryzen 7040, 8040, 8000G)', 0, 50, 'igpu'),
    ('amd', 'Integrated graphics (AMD)', 'Radeon 760M', 0, 42, 'igpu'),
    ('amd', 'Integrated graphics (AMD)', 'Radeon 680M (Ryzen 6000)', 0, 40, 'igpu'),
    ('amd', 'Integrated graphics (AMD)', 'Radeon 660M', 0, 30, 'igpu'),
    ('amd', 'Integrated graphics (AMD)', 'Radeon Vega 8 / Vega 11 (Ryzen 2000–5000 G and U)', 0, 15, 'igpu'),
    ('amd', 'Integrated graphics (AMD)', 'Radeon Graphics 2 CU (Ryzen 7000/9000 desktop)', 0, 7, 'igpu'),
    ('intel', 'Integrated graphics (Intel)', 'Arc 140V / 130V (Core Ultra 200V)', 0, 85, 'igpu'),
    # Measured by a tester (Core Ultra 5 125H): 40-55 FPS at 1080p native with the settings for it; the
    # 200V's Arc is faster.
    ('intel', 'Integrated graphics (Intel)', 'Arc graphics (Core Ultra 100H)', 0, 70, 'igpu'),
    ('intel', 'Integrated graphics (Intel)', 'Iris Xe (11th–13th gen laptops)', 0, 20, 'igpu'),
    ('intel', 'Integrated graphics (Intel)', 'UHD 770 / 730 (12th gen+ desktop)', 0, 10, 'igpu'),
    ('intel', 'Integrated graphics (Intel)', 'UHD 620 / 630', 0, 6, 'igpu'),
]


class Gpu:
    def __init__(self, vendor, group, name, vram, score, flags=''):
        self.vendor, self.group, self.name, self.vram, self.score = vendor, group, name, vram, score
        flags = flags.split()
        self.laptop, self.igpu, self.vulkan13 = 'laptop' in flags, 'igpu' in flags, 'no-vulkan13' not in flags
        rtx = re.search(r'RTX (\d)0', name)
        series = int(rtx.group(1)) * 10 if rtx else 0
        self.dlss = vendor == 'nvidia' and series >= 20
        self.frame_gen = ('DLSS Frame Generation up to 4×' if series >= 50 else
                          'DLSS Frame Generation 2×' if series >= 40 else None) if vendor == 'nvidia' else None
        self.fsr4 = vendor == 'amd' and not self.igpu and bool(re.match(r'RX [79]\d{3}', name))
        self.amd_driver_note = vendor == 'amd' and not self.igpu and bool(re.match(r'RX [69]\d{3}', name))
        self.id = re.sub(r'[^a-z0-9]+', '-', f'{vendor}-{name}'.lower()).strip('-')

    @property
    def label(self):
        group = self.group.split(' (')[0]
        if self.igpu:
            return self.name
        if group.split()[-1] == self.name.split()[0]:
            group = ' '.join(group.split()[:-1])
        return f'{group} {self.name}'


GPU_LIST = [Gpu(*row) for row in GPUS]
GPU_BY_ID = {g.id: g for g in GPU_LIST}

# Processor scores: Intel Core i3/i5/i7/i9 per generation, Ryzen 3/5/7/9 per series (None: no such model).
INTEL_CORE = {14: (142, 192, 222, 235), 13: (138, 188, 215, 230), 12: (132, 168, 195, 205),
              11: (105, 135, 145, 148), 10: (100, 120, 132, 140), 9: (95, 112, 122, 130),
              8: (92, 108, 118, None), 7: (78, 92, 105, 120), 6: (72, 88, 98, None), 5: (62, 78, 88, None),
              4: (62, 78, 88, None), 3: (45, 60, 68, None), 2: (42, 56, 64, None)}
RYZEN = {9000: (None, 215, 235, 242), 8000: (140, 172, 185, None), 7000: (150, 200, 222, 228),
         6000: (None, 140, 155, 160), 5000: (118, 160, 180, 188), 4000: (85, 105, 115, 118),
         3000: (100, 120, 130, 138), 2000: (72, 92, 102, None), 1000: (68, 85, 95, None)}
CORE_ULTRA = {'u2-5': 188, 'u2-7': 215, 'u2-9': 228, 'u1-5': 125, 'u1-7': 140, 'u1-9': 148}
CORE_NO_I = {'3': 135, '5': 165, '7': 195}
# The tables score desktop chips. A laptop's Core i or Ryzen of the same generation holds lower clocks
# under long load: H-class (H, HK, HQ, HS, MQ, QM) about four fifths of it, U-class (U, Y, P, M,
# Intel's G1-G7) about two thirds. HX chips run at desktop power.
LAPTOP_SHARE = {'h': 0.8, 'u': 0.65}


def laptop_class(suffix, intel=True):
    """'h', 'u' or None (a desktop chip, or HX) for the letters after a model number (7820HK: 'HK')."""
    if not suffix or suffix.startswith('HX'):
        return None
    if suffix[0] == 'H' or suffix in ('MQ', 'QM', 'XM', 'MX'):
        return 'h'
    if suffix[0] in 'UYP' or suffix == 'M' or (intel and re.fullmatch(r'G\d', suffix)):
        return 'u'
    return None


class Cpu:
    """A processor: its name, the website's score (None: not in the tables) and support:
    ok, compat (instructions translated: Intel 2nd/3rd gen) or none (no AVX). laptop: 'h' or 'u'
    when the score was lowered for a laptop chip (LAPTOP_SHARE)."""

    def __init__(self, name, score, support='ok', family='', laptop=None):
        self.name, self.score, self.support, self.family, self.laptop = name, score, support, family, laptop
        if laptop and score:
            self.score = round(score * LAPTOP_SHARE[laptop])

    @property
    def label(self):
        """'Intel Core i7-7820HK' for Windows' 'Intel(R) Core(TM) i7-7820HK CPU @ 2.90GHz'."""
        name = re.sub(r'\((R|TM|tm|r)\)', ' ', self.name)
        name = re.sub(r'@.*$|\bw(ith|/) Radeon.*$|\b\d+-Core Processor\b|\bProcessor\b|\bCPU\b|\b\d+(st|nd|rd|th) Gen\b',
                      ' ', name)
        return ' '.join(name.split())


def model_suffix(text):
    """The letters right after a model number ('HK CPU @ 2.90GHz' -> 'HK', 'G7 CPU' -> 'G7')."""
    m = re.match(r'[A-Z]{1,3}\d?(?![A-Za-z])', text)
    return m.group() if m else ''


def cpu_from_name(name):
    """The website's processor entry for a Windows processor name, or a Cpu with score None."""
    n = ' '.join(re.sub(r'\((R|TM|tm|r)\)', ' ', name).split())
    m = re.search(r'\bCore\s+Ultra\s+([579])\s+(\d)\d\d', n)
    if m:
        return Cpu(name, CORE_ULTRA.get(f'u{m.group(2)}-{m.group(1)}', CORE_ULTRA[f'u2-{m.group(1)}']), family='Core Ultra')
    if re.search(r'\bi[3579]\s+(?:CPU\s+)?\d{3}\b', n):
        return Cpu(name, 0, 'none', 'Core i')  # 1st gen (i7 920)
    m = re.search(r'\bi([3579])-(\d{3,5})', n)
    if m:
        tier, digits = '3579'.index(m.group(1)), m.group(2)
        gen = int(digits[:2]) if len(digits) == 5 or (len(digits) == 4 and digits[0] == '1') else \
            int(digits[0]) if len(digits) == 4 else 1
        if gen == 1:
            return Cpu(name, 0, 'none', 'Core i')
        scores = INTEL_CORE.get(min(gen, 14))
        score = scores and (scores[tier] if scores[tier] is not None else max(s for s in scores if s))
        return Cpu(name, score, 'compat' if gen <= 3 else 'ok', 'Core i', laptop_class(model_suffix(n[m.end():])))
    m = re.search(r'\bCore\s+([357])\s+\d{3}', n)
    if m:
        return Cpu(name, CORE_NO_I[m.group(1)], family='Core')
    if re.search(r'\b(Pentium|Celeron)\b|\bIntel\s+N\d{2,3}\b', n):
        new = re.search(r'\bG7\d{3}\b|\bN\d{2,3}\b', n)
        return Cpu(name, 50 if new else 0, 'ok' if new else 'none', 'Pentium')
    if re.search(r'\bCore\s*2\b|\bXeon\b', n):
        return Cpu(name, 0, 'none', 'Xeon')
    if re.search(r'\bRyzen\s+AI\b', n):
        return Cpu(name, 185, family='Ryzen AI')
    m = re.search(r'\bRyzen\s+([3579])\s+(?:PRO\s+)?(\d)\d{3}', n) or \
        re.search(r'\bRyzen\s+Threadripper\s+(?:PRO\s+)?()(\d)\d{3}', n)
    if m:
        tier = '3579'.index(m.group(1)) if m.group(1) else 3
        scores = RYZEN.get(int(m.group(2)) * 1000)
        if scores:
            score = scores[tier] if scores[tier] is not None else max(s for s in scores if s)
            return Cpu(name, score, family='Ryzen', laptop=laptop_class(model_suffix(n[m.end():]), intel=False))
    if re.search(r'\bAthlon\b', n):
        zen = re.search(r'\bAthlon\s+(?:Silver|Gold|PRO)?\s*\d{3,4}', n) and not re.search(r'Athlon\s+(II|X\d|64)', n)
        return Cpu(name, 50 if zen else 0, 'ok' if zen else 'none', 'Athlon')
    if re.search(r'\bFX\b|\bA\d{1,2}-\d|\bPhenom\b|\bSempron\b|\bOpteron\b', n):
        return Cpu(name, 0, 'none', 'FX')
    return Cpu(name, None)


# ---- detection on this PC ----------------------------------------------------------------------
def processor_name():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
            return ' '.join(str(winreg.QueryValueEx(key, 'ProcessorNameString')[0]).split())
    except OSError:
        return ''


def processor_support():
    """ok / compat / none from CPUID (AVX, then the instructions the port translates), or None."""
    try:
        import cpu_compat
        found = cpu_compat._cpuid_windows()
        if not found:
            return None
        cpuid, cleanup = found
        try:
            ecx = cpuid(1)[2]
            avx = ecx >> 28 & 1 and ecx >> 27 & 1  # AVX and OSXSAVE
        finally:
            cleanup()
        if not avx:
            return 'none'
        missing = cpu_compat.missing_host_extensions()
        return None if missing is None else 'compat' if missing else 'ok'
    except Exception:  # an unknown CPU never stops the launcher
        return None


INSTRUCTIONS = ('AVX', 'BMI1', 'MOVBE', 'LZCNT')  # HARD_LIMITS: AVX or nothing; the others are translated


def instructions(cpuid=None):
    """{name: True/False} for INSTRUCTIONS on this CPU, from CPUID as cpu_compat reads it (`cpuid` for
    tests); None when CPUID cannot be read here."""
    cleanup = None
    try:
        import cpu_compat
        if cpuid is None:
            found = cpu_compat._cpuid_windows()
            if not found:
                return None
            cpuid, cleanup = found
        ecx = cpuid(1)[2]
        missing = set(cpu_compat.missing_host_extensions(cpuid) or ())
        have = {'AVX': bool(ecx >> 28 & 1 and ecx >> 27 & 1)}  # AVX and OSXSAVE
        have.update({name: name not in missing for name in cpu_compat.HOST_EXTENSIONS})
        return {name: have[name] for name in INSTRUCTIONS}
    except Exception:  # an unknown CPU never stops the launcher
        return None
    finally:
        if cleanup:
            cleanup()


def instruction_lines(have, tr=str):
    """[(state, text)] for the System page's hard limits: this CPU's instructions, when known."""
    if have is None:
        return []
    missing = [name for name in INSTRUCTIONS if not have.get(name)]
    if not have.get('AVX'):
        return [('bad', tr('Your processor has no AVX: the port cannot run on it.'))]
    if missing:
        return [('warn', tr('Your processor lacks {}: the port translates them (compatibility mode), so expect '
                            'bugs.').format(', '.join(missing)))]
    return [('ok', tr('Your processor has AVX, BMI1, MOVBE and LZCNT: the port runs on it directly.'))]


def detect_cpu():
    name = processor_name()
    cpu = cpu_from_name(name) if name else Cpu('', None)
    support = processor_support()
    if support == 'none' or (support == 'compat' and cpu.support == 'ok'):
        cpu.support = support
    elif support == 'ok' and cpu.support != 'ok':
        # The name looked old (Xeon, a 2nd gen Core) but the CPU has every instruction.
        cpu.support, cpu.score = 'ok', None if cpu.score == 0 else cpu.score
    return cpu


def memory_gb():
    """Installed memory in GB (what is on the sticks, not what Windows has left)."""
    try:
        kb = ctypes.c_ulonglong()
        if ctypes.windll.kernel32.GetPhysicallyInstalledSystemMemory(ctypes.byref(kb)):
            return round(kb.value / 1024 ** 2)
    except (AttributeError, OSError):
        pass

    class Status(ctypes.Structure):
        _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong),
                    ('avail', ctypes.c_ulonglong), ('page_total', ctypes.c_ulonglong),
                    ('page_avail', ctypes.c_ulonglong), ('virtual_total', ctypes.c_ulonglong),
                    ('virtual_avail', ctypes.c_ulonglong), ('extended', ctypes.c_ulonglong)]
    try:
        status = Status()
        status.length = ctypes.sizeof(Status)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
        return round(status.total / 1024 ** 3)
    except (AttributeError, OSError):
        return 0


def _words(text):
    text = re.sub(r'\((r|tm)\)', ' ', text.lower())
    text = ' ' + re.sub(r'[^a-z0-9]+', ' ', text) + ' '
    return re.sub(r' +', ' ', re.sub(r' (graphics|gpu|laptop)(?= )', ' ', text))


def _keys(gpu):
    parts = [_words(p).strip() for p in re.sub(r'\b\d+ GB\b', '', re.sub(r'\(.*?\)', '', gpu.name)).split('/')]
    prefix = ' '.join(parts[0].split(' ')[:-1])
    return [f'{prefix} {p}' if i and p[:1].isdigit() else p for i, p in enumerate(parts) if p]


def match_gpu(name, vram_gb=None):
    """The table's entry for a graphics card name (Vulkan or the browser's), or None. Among
    equal matches the one with the detected video memory wins."""
    words, laptop = _words(name), bool(re.search(r'laptop|mobile|max q', name, re.I))
    best, best_score = None, 0
    for gpu in GPU_LIST:
        for key in _keys(gpu):
            if f' {key} ' not in words:
                continue
            score = len(key) + (0.5 if gpu.laptop == laptop else 0)
            if vram_gb and gpu.vram and abs(gpu.vram - vram_gb) <= 1.5:
                score += 0.25
            if score > best_score:
                best, best_score = gpu, score
    return best


def best_gpu_line(lines):
    """The card a laptop with two plays on: a discrete one first, then the most video memory."""
    if not lines:
        return ''
    return max(lines, key=lambda line: (not parse_gpu_line(line)[1], parse_gpu_line(line)[2]))


def parse_gpu_line(line):
    """bb-gpu-capabilities' 'Name, discrete, 16304 MiB' -> (name, integrated, GB). The tool says
    'integrated or other' for every card that is not discrete (integrated graphics, a virtual or
    software device)."""
    parts = [p.strip() for p in line.split(',')]
    vram = next((int(m.group(1)) for p in parts for m in [re.match(r'(\d+) MiB', p)] if m), 0)
    return parts[0], any(p.startswith('integrated') for p in parts), round(vram / 1024)


# ---- the rating (the website's x()) --------------------------------------------------------------
LEVELS = ['Not recommended', 'Under 30 FPS', 'About 30 FPS', '30–60 FPS', '60–120 FPS', '120+ FPS']
TIERS = ['Not supported', 'Below minimum', 'Minimum', 'Above minimum', 'Recommended', '4K-ready']
SUMMARIES = ["This PC can't run the port.", 'It may start, but expect low frame rates and problems.',
             'Playable at 720p–1080p at about 30 FPS.', 'Playable at 1080p; 1440p with upscaling.',
             'Good for 1440p at 60 FPS or more.', 'Ready for 4K with upscaling, and high frame rates at 1440p.']
LIMITS = {'gpu': 'Your graphics card sets the limit.', 'cpu': 'Your processor sets the limit.',
          'ram': 'Your memory sets the limit.'}
RESOLUTIONS = ('1080p', '1440p', '4K')


def gpu_tier(g):
    if not g.vulkan13 or g.score < 25:
        return 0
    # Under 4 GB of video memory is below the minimum, not a stop: a player's GTX 1060 3 GB played up
    # to the Blood-Starved Beast. The launcher gives such a card the settings for 4 GB cards.
    if (not g.igpu and g.vram < 4) or g.score < 35:
        return 1
    return 2 if g.score < 70 else 3 if g.score < 112 else 4 if g.score < 170 or g.vram < 12 else 5


def resolution_levels(g, tier):
    n, r, i = {0: (0, 0, 0), 1: (1, 1, 0), 2: (2, 1, 0), 3: (3, 2, 1), 4: (4, 4, 3), 5: (5, 5, 4)}[tier]
    if tier == 4 and g.score >= 170:
        n, r = 5, 5
    if g.igpu:
        r, i = min(r, 2), 0
    elif g.vram <= 4:
        r, i = min(r, 1), 0
    elif g.vram <= 6:
        i = min(i, 1)
    elif g.vram < 12:
        i = min(i, 2)
    if g.vendor == 'intel':
        r, i = max(min(r, 1), r - 1), max(min(i, 1), i - 1)
    return n, r, i


def cpu_tier(cpu):
    if cpu.support == 'none':
        return 0
    if cpu.score is None:
        return 2  # not in the tables: counted as the minimum
    return 1 if cpu.score < 85 else 2 if cpu.score < 125 else 3 if cpu.score < 160 else 4


def ram_tier(gb):
    # The game needs memory for its start from the RAM and the page file together (Help: code 3), so
    # less than 16 GB is below the minimum, not a stop: a player's laptop with 8 GB runs it smoothly.
    return 1 if gb < 16 else 5


def upscaler_name(g):
    return None if g.vendor == 'intel' else 'DLSS' if g.dlss else 'FSR 4' if g.fsr4 else 'FSR 3.1'


def quality_for(level, res):
    """The website's suggested quality preset (launcher value) at a level and resolution."""
    if level <= 2:
        return '3'
    if res == '4K':
        return '2' if level >= 4 else '3'
    return '2' if level == 3 else '1'


QUALITY_NAMES = {'1': 'Quality', '2': 'Balanced', '3': 'Performance'}


def settings_text(g, level, res, tr=str):
    if level == 0:
        return ''
    name = upscaler_name(g)
    if name:
        return f'{name} {QUALITY_NAMES[quality_for(level, res)]}'  # the upscaler's own preset name
    return tr('Upscaler off, lower details') if level <= 2 else tr('Upscaler off')


class Rating:
    """tier/tier_label/summary, limit (gpu, cpu, ram or None), rows [(res, level, label, settings)],
    checks [(state ok/warn/bad, part, text)] and notes [text]."""


def assess(g, cpu, ram, tr=str):
    tiers = {'gpu': gpu_tier(g), 'cpu': cpu_tier(cpu), 'ram': ram_tier(ram)}
    capped = {'gpu': tiers['gpu'], 'cpu': 5 if tiers['cpu'] == 4 else tiers['cpu'], 'ram': tiers['ram']}
    tier = min(capped.values())
    # A slow processor caps the frame rate whatever the card can do (below the minimum: under 30 FPS,
    # the minimum: 30–60 FPS). Too little RAM sets no cap: it shows in the tier and the memory check.
    cap = (0, 1, 3, 4, 5)[tiers['cpu']]
    rating = Rating()
    rating.gpu, rating.cpu, rating.ram = g, cpu, ram
    rating.tier, rating.tiers = tier, tiers
    rating.tier_label, rating.summary = tr(TIERS[tier]), tr(SUMMARIES[tier])
    rating.limit = None if tier == 5 else next(k for k in ('gpu', 'cpu', 'ram') if capped[k] == tier)
    # levels: the frame rate to expect, where the processor can set the limit; picture_levels: what
    # the graphics card can do, for the picture settings (with the processor as the limit, a lower
    # picture would not add frames).
    rating.cap, rating.levels, rating.picture_levels = cap, {}, {}
    rating.rows = []
    for res, picture in zip(RESOLUTIONS, resolution_levels(g, tiers['gpu'])):
        level = min(picture, cap)
        rating.levels[res], rating.picture_levels[res] = level, picture
        rating.rows.append((res, level, tr(LEVELS[level]), settings_text(g, picture if level else 0, res, tr)))

    checks = []
    if cpu.support == 'none':
        checks.append(('bad', 'cpu', tr('Not supported: the port needs AVX, BMI1, MOVBE and LZCNT '
                                        '(Intel 4th gen or newer, or any Ryzen).')))
    elif cpu.support == 'compat':
        checks.append(('warn', 'cpu', tr('Intel 2nd/3rd gen runs in compatibility mode, so expect bugs. '
                                         'It is also below the minimum (Core i3-10100).')))
    elif cpu.score is None:
        checks.append(('ok', 'cpu', tr('Not in the website\'s list; it has every instruction the port needs. '
                                       'Counted as the minimum.')))
    else:
        text = tr(('', 'Below the minimum (Core i3-10100 / Ryzen 3 3100): expect low frame rates and stutter.',
                   'Meets the minimum (Core i3-10100 / Ryzen 3 3100).', 'Above the minimum.',
                   'Meets the recommendation (Ryzen 7 5700X / Core i7-12700).')[tiers['cpu']])
        if cpu.laptop:
            text += ' ' + tr('Laptop processor: counted lower than the desktop chip of its generation '
                             '(it holds lower clocks under long load).')
        checks.append((('bad', 'warn', 'ok', 'ok', 'ok')[tiers['cpu']], 'cpu', text))
    if not g.vulkan13:
        checks.append(('bad', 'gpu', tr('Not supported: no Vulkan 1.3 driver for this card.')))
    else:  # under 4 GB of video memory reads "Below the minimum" like a slow card (gpu_tier)
        text = tr(('Too slow for the port.', 'Below the minimum (GTX 1050 Ti / GTX 1650 / RX 570).',
                   'Meets the minimum (GTX 1050 Ti / GTX 1650 / RX 570 / RX 6500 XT).', 'Above the minimum.',
                   'Meets the recommendation (RTX 3060 Ti / RTX 4060 / RX 6700 XT / RX 7600).',
                   'Enough for 4K (RTX 4070 / RTX 5070 / RX 7800 XT / RX 9070 class).')[tiers['gpu']])
        if g.igpu:
            text += ' ' + tr('Integrated graphics use your RAM as video memory.')
        checks.append((('bad', 'warn', 'ok', 'ok', 'ok', 'ok')[tiers['gpu']], 'gpu', text))
    if ram < 16:  # a warning even at 8 GB: with a large enough page file the game can start (ram_tier)
        checks.append(('warn', 'ram', tr('{} GB is below the minimum of 16 GB: the game may close right after PLAY (code 3).').format(ram)))
    elif ram < 32:
        checks.append(('ok', 'ram', tr('{} GB meets the minimum (32 GB recommended). Keep the page file on "System managed".').format(ram)))
    else:
        checks.append(('ok', 'ram', tr('{} GB meets the recommendation.').format(ram)))
    rating.checks = checks

    notes = []
    if tier > 0:
        if not g.igpu and g.vram <= 4:  # profile() sets these for 3 GB cards as well
            notes.append(tr('Settings for 4 GB cards: 720p/1080p, FSR 3.1 Performance, Model detail "As in the game", '
                            'Object motion vectors off, frame generation off, a 60 FPS cap.'))
        if g.vendor == 'intel':
            notes.append(tr('Intel graphics: keep the upscaler off, stretched polygons can still appear with it on.'))
        if g.amd_driver_note:
            notes.append(tr('AMD driver 26.8.1: if the game crashes or shows broken graphics, clear the shader cache '
                            '(Help) or try another driver.'))
        at1440 = rating.levels['1440p']
        if g.frame_gen and at1440 >= 4:
            notes.append(tr('{} is available on your card for even higher frame rates.').format(g.frame_gen))
        elif at1440 >= 4 and g.vram >= 12:
            notes.append(tr('FSR 3.1 frame generation works on your card; it works best from about 60 base FPS.'))
        elif g.igpu or g.vram > 4:
            notes.append(tr('Leave frame generation off: it needs about 60 base FPS and 12–16 GB of video memory '
                            'to work well.'))
        if g.laptop or g.igpu:
            notes.append(tr('Laptops: plug in the charger, set Windows to best performance, and make sure the game '
                            'runs on the dedicated graphics card if you have one.'))
        if g.igpu:
            notes.append(tr('Integrated graphics share your RAM: 16 GB or more is a must.'))
    rating.notes = notes
    rating.frame_gen = ('dlss' if g.frame_gen and rating.levels['1440p'] >= 4 else
                        'fsr3' if rating.levels['1440p'] >= 4 and g.vram >= 12 else None)
    return rating


# ---- what the launcher sets ------------------------------------------------------------------------
def outputs(width, height):
    """{resolution class: output value} for a display: its aspect ratio, at most its height."""
    aspect = width / max(1, height)
    if aspect > 3.0:
        variants = {'1080p': '3840x1080', '1440p': '5120x1440'}
    elif aspect > 2.2:
        variants = {'1080p': '2560x1080', '1440p': '3440x1440', '4K': '5120x2160'}
    else:
        variants = {'1080p': '1920x1080', '1440p': '2560x1440', '4K': '3840x2160'}
    limit = {'1080p': 0, '1440p': 1400, '4K': 2100}
    return {res: value for res, value in variants.items() if height >= limit[res]}


def resolution_class(output):
    """The rating row an output belongs to (by its height)."""
    try:
        height = int(str(output).lower().split('x')[1])
    except (IndexError, ValueError):
        return '1080p'
    return '4K' if height >= 2000 else '1440p' if height >= 1400 else '1080p'


def recommended_class(rating, available):
    for wanted in (4, 3):
        for res in reversed(RESOLUTIONS):
            if res in available and rating.levels[res] >= wanted:
                return res
    return '1080p'


def low_end(rating):
    """The graphics card below the minimum at 1080p: the website's 720p end of the minimum. (A
    processor below the minimum is not helped by fewer pixels.)"""
    return rating.picture_levels['1080p'] <= 1


# The player's goal (the launcher's Play page): it moves the profile along one line, the hardware
# decides where on it. smooth: as many FPS as the game allows (up to 120); balanced: a steady
# 60 FPS with the best picture the card has room for; quiet: 60 FPS with less work for the card
# (laptops, noise, power).
GOALS = ('smooth', 'balanced', 'quiet')
GOAL_FPS = {'smooth': 120, 'balanced': 60, 'quiet': 60}  # the performance test's frame rate goal
# The upscalers of the FSR 4 family (4.0 and the 4.1 methods): they sharpen by themselves.
FSR4_FAMILY = ('fsr4', 'fsr411', 'fsr412')
SHARPEN_WITH_FSR4 = 'FSR 4 sharpens by itself: sharpening on top of it brings flaws along edges.'


def step_preset(preset, steps):
    """A quality preset moved by steps (+1: more upscaling), within Native AA .. Performance."""
    return str(min(3, max(0, int(preset) + steps)))


def profile(rating, res, output, current=None, goal='smooth'):
    """Every setting that decides the performance, for this PC at a resolution class, derived from
    the requirements and the website's check, for the player's goal: [(store, key, value, why)].

    Taste stays the player's: chromatic aberration, depth of field, motion blur and sharpening are not touched,
    except sharpening with FSR 4, which sharpens by itself (RCAS on top of it left flaws along edges with
    FSR 4.1.2), screen-space reflections (not in the original) are only switched off when the card has no room
    for them, and frame generation (input latency) is only switched off where it cannot work well.
    `current` ({key: value}) is what is set now, for those two."""
    # level: the frame rate to expect (frame rate cap, frame generation); picture: what the card can
    # do (preset, effects, model detail). They differ when the processor sets the limit.
    g, level, picture = rating.gpu, rating.levels[res], rating.picture_levels[res]
    four_gb = not g.igpu and g.vram <= 4
    goal = goal if goal in GOALS else 'smooth'
    current = current or {}
    if low_end(rating) and res == '1080p':
        # 720p: about 2.25 times fewer pixels than 1080p
        output, level, picture = '1280x720', min(2, rating.cap), 2
    name = upscaler_name(g)
    upscaler = {'DLSS': 'dlss', 'FSR 4': 'fsr4', 'FSR 3.1': 'fsr3', None: 'off'}[name]
    out = []
    add = lambda store, key, value, why: out.append((store, key, value, why))

    add('ini', 'output_res', output, 'The highest resolution up to your display at which your PC is expected to '
                                     'reach 60 FPS or more (else 30–60, else 1080p).' if output != '1280x720' else
        'Below the minimum at 1080p: 720p, the low end of the minimum requirements.')
    if four_gb:
        add('ini', 'upscaler', 'fsr3', 'The website\'s settings for 4 GB cards: FSR 3.1.')
    elif upscaler == 'off':
        add('ini', 'upscaler', 'off', 'Intel graphics: stretched polygons can still appear with an upscaler on.')
    else:
        add('ini', 'upscaler', upscaler, {'dlss': 'DLSS on a GeForce RTX.', 'fsr4': 'FSR 4 on a Radeon RX 7000/9000.',
                                          'fsr3': 'FSR 3.1: FSR 4 is slow below the Radeon RX 7000/9000.'}[upscaler])
        if upscaler == 'fsr4':
            add('ini', 'sharpen', False, SHARPEN_WITH_FSR4)
    if four_gb:
        add('ini', 'preset', '3', 'The website\'s settings for 4 GB cards: Performance.')
    elif upscaler == 'off':
        pass  # the quality preset is the upscaler's render scale
    elif output == '1280x720':
        add('ini', 'preset', '2', 'Balanced: Performance would render 720p from only 360 lines.')
    elif goal == 'balanced' and picture >= 5:
        add('ini', 'preset', step_preset(quality_for(picture, res), -1),
            'Goal "Balanced": at 60 FPS your card has room for a sharper picture, one quality step up.')
    elif goal == 'quiet':
        add('ini', 'preset', step_preset(quality_for(picture, res), 1),
            'Goal "Quiet": one quality step down, less work for the graphics card.')
    else:
        add('ini', 'preset', quality_for(picture, res), 'The website\'s suggestion for the expected frame rate.')
    cpu_limit = level < picture  # the processor holds the frame rate below what the card could do
    if four_gb:
        add('app', 'fps_mode', '60', 'The website\'s settings for 4 GB cards: a 60 FPS cap.')
    elif goal in ('balanced', 'quiet') and level >= 3:
        add('app', 'fps_mode', '60', {'balanced': 'Goal "Balanced": a steady 60 FPS.',
                                      'quiet': 'Goal "Quiet": 60 FPS; the card rests between frames.'}[goal])
    elif cpu_limit and level <= 3:
        add('app', 'fps_mode', '60' if level == 3 else '30',
            'Your processor sets the limit: a steady cap at what it is expected to hold. Lower graphics '
            'settings would not raise the frame rate.')
    else:
        add('app', 'fps_mode', 'uncap' if level >= 4 else '60' if level == 3 else '30',
            {True: 'Unlocked, capped at the display refresh (at most 120): your PC is expected above 60 FPS.',
             False: 'A steady cap at what your PC is expected to hold.'}[level >= 4])
    add('app', 'frame_cap', '', 'Auto: the display refresh, at most 120 (the game\'s timing breaks above).')
    add('ini', 'effect_game_aa', upscaler == 'off' and not four_gb,
        'The game\'s own anti-aliasing only without an upscaler: the upscaler smooths edges itself.')
    add('ini', 'effect_ssao', picture >= 3, 'Ambient occlusion costs GPU time: on from "Above minimum".')
    add('ini', 'effect_dynamic_shadows', picture >= 3, 'Shadows of dynamic lights: on from "Above minimum".')
    if picture < 5 and current.get('effect_ssr'):
        add('ini', 'effect_ssr', False, 'Screen-space reflections are expensive: off below "4K-ready" headroom.')
    if four_gb:
        lod, why = '0', 'The website\'s settings for 4 GB cards: model detail as in the game.'
    elif goal == 'quiet' and picture >= 3 and not g.igpu:
        lod, why = '0', 'Goal "Quiet": model detail as in the game.'
    elif picture >= 3 and not g.igpu:
        lod, why = '0', 'Model detail as in the game.'
    else:
        lod, why = '1', 'Lower model detail: below "Above minimum" every millisecond counts.'
    add('ini', 'model_lod', lod, why)
    if goal == 'quiet':
        add('ini', 'object_motion', False, 'Goal "Quiet": object motion vectors off (they cost about 5–10% FPS).')
    else:
        add('ini', 'object_motion', picture >= 4 and not four_gb,
            'Object motion vectors cost about 5–10% FPS: on from "Recommended".')
    frame_gen_ok = level >= 4 and (g.frame_gen or g.vram >= 12)
    if current.get('frame_gen', 'off') != 'off' and not frame_gen_ok:
        add('ini', 'frame_gen', 'off', 'Frame generation needs about 60 base FPS and 12–16 GB of video memory.')
    return out


def recommend(rating, res, output, current=None):
    """{(store, key): value} of profile()."""
    return {(store, key): value for store, key, value, _why in profile(rating, res, output, current)}


def vendor_of(name):
    """nvidia, amd or intel from a graphics card's name ('' when unknown)."""
    name = name.lower()
    return ('nvidia' if 'nvidia' in name or 'geforce' in name else
            'amd' if 'amd' in name or 'radeon' in name else
            'intel' if 'intel' in name or ' arc' in f' {name}' else '')


def signature(gpu_line, cpu, ram):
    """The hardware the automatic settings are made for, as detected (never a card chosen by hand):
    'NVIDIA GeForce RTX 4070 12 GB|AMD Ryzen 7 5800X 8-Core Processor|32'; '' when the graphics
    card or the processor was not detected this time (nothing to compare). The display is no part
    of it: 1.9.3 and 1.9.4 added it, and a TV or a second monitor switched off set the player's
    settings back at the next start."""
    if not gpu_line or not cpu or not cpu.name:
        return ''
    name, _integrated, vram = parse_gpu_line(gpu_line)
    return f'{name} {vram} GB|{cpu.name}|{ram}'


def parse_card(text):
    """(name, video memory in GB) of a signature's first part, 'NVIDIA GeForce RTX 4070 12 GB'; or None."""
    m = re.fullmatch(r'(.+) (\d+) GB', text) if isinstance(text, str) else None
    return (m.group(1), int(m.group(2))) if m else None


def signature_parts(text):
    """(card name, video memory, processor, memory in GB) of a stored signature, also one of 1.9.3
    and 1.9.4 with the display after them (left out); None when it is empty or unreadable."""
    parts = text.split('|') if isinstance(text, str) else []
    card = parse_card(parts[0]) if len(parts) >= 3 else None
    if not card:
        return None
    return card[0], card[1], parts[1], int(parts[2]) if parts[2].isdigit() else 0


def stored_signature(text):
    """A stored signature as signature() writes it now (1.9.3 and 1.9.4 had the display after the
    hardware); '' when it is empty or unreadable."""
    parts = signature_parts(text)
    return f'{parts[0]} {parts[1]} GB|{parts[2]}|{parts[3]}' if parts else ''


def same_card(a, b):
    """Whether two (name, video memory in GB) are one graphics card model: the same name, or the same
    entry of the table (a driver that words the name differently changes nothing). The memory may
    differ by 1 GB: drivers report a little more or less from one version to the next."""
    if abs(a[1] - b[1]) > 1:
        return False
    if _words(a[0]) == _words(b[0]):
        return True
    found = match_gpu(a[0], a[1])
    return found is not None and found is match_gpu(b[0], b[1])


def same_memory(a, b):
    """Installed memory in whole GB, the same within rounding: 2 GB, or an eighth of large amounts
    (Windows' fallback count leaves out what the integrated graphics reserve). 0 is unknown."""
    return not a or not b or abs(a - b) <= max(2, max(a, b) / 8)


def new_hardware(before, now, seen=(), integrated=False):
    """Whether the signature `now` is other hardware than `before`, the one the settings were made
    for: another processor, more or less memory, or a graphics card new to this PC.

    Not new: anything not detected, the display, and a card that comes and goes on a PC with two.
    A laptop shows its integrated graphics while the discrete card sleeps or is switched off (MUX,
    eco mode), and an eGPU comes and goes with its cable; 1.9.3 and 1.9.4 set the settings back at
    every such start. So an integrated card detected now (`integrated`) never counts, nor one this PC
    showed before (`seen`: the cards of earlier signatures, 'Name 8 GB')."""
    old, new = signature_parts(before), signature_parts(now)
    if not old or not new:
        return False
    if old[2] and new[2] and old[2] != new[2]:
        return True
    if not same_memory(old[3], new[3]):
        return True
    card = new[:2]
    if integrated or same_card(old[:2], card):
        return False
    return not any(same_card(other, card) for other in map(parse_card, seen) if other)


# ---- the requirements (website) ---------------------------------------------------------------------
REQUIREMENTS = [
    ('OS', 'Windows 10 or 11, 64-bit', 'Windows 11, 64-bit'),
    ('CPU', 'Intel Core i3-10100 or AMD Ryzen 3 3100', 'AMD Ryzen 7 5700X or Intel Core i7-12700'),
    ('RAM', '16 GB', '32 GB'),
    ('GPU', '4 GB of video memory with Vulkan 1.3 (GTX 1050 Ti, GTX 1650, RX 570, RX 6500 XT)',
     '8 GB of video memory or more (RTX 3060 Ti, RTX 4060, RX 6700 XT, RX 7600)'),
    ('Storage', 'About 35 GB for your game files plus 1 GB for the port; an SSD is recommended', 'SSD'),
]
REQUIREMENT_HEADS = ('Minimum (720p–1080p, about 30 FPS)', 'Recommended (1440p, 60–120 FPS)')
FOUR_K = 'For 4K or frame generation: 12–16 GB of video memory (RTX 4070, RTX 5070, RX 7800 XT, RX 9070).'
HARD_LIMITS = ['The CPU needs AVX, BMI1, MOVBE and LZCNT, which means Intel 4th gen (Haswell) or newer, or any Ryzen.',
               'Intel 2nd/3rd gen run in compatibility mode, so expect bugs.',
               'CPUs without AVX (many Pentium/Celeron models) can’t run it at all.']
DRIVER_NOTES = ['DLSS needs a GeForce RTX 20 series or newer. DLSS Frame Generation needs RTX 40 (2×) or RTX 50 '
                '(up to 4×). FSR 3.1 frame generation works on any card and works best from about 60 base FPS.',
                'FSR 4 is slow below Radeon RX 7000/9000; use FSR 3.1 there.',
                'AMD driver 26.8.1: if an RX 6000/9000 crashes or shows broken graphics, clear the shader cache '
                '(Help), or try another driver.',
                'Intel graphics: stretched polygons can still appear with an upscaler on, so turn it off.',
                'Overlays: if the game won’t start or keeps crashing, close MSI Afterburner, RTSS and other overlays.']
