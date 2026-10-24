# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's look, after bbport.de: its colours, the Cinzel and Inter fonts,
the emblem and the Yharnam art, and the antialiased images (rounded buttons and fields,
switches, scroll bars, cards) that ttk's clam theme draws the widgets with.

Images are drawn by Pillow at four times their size and scaled down, so their corners stay
smooth at every display scale. ttk blends an image's transparent pixels with the style's own
background, so every style that sits on a card has a Card. twin with the card's colour.
"""
import ctypes
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageTk

if getattr(sys, 'frozen', False):
    ASSETS = Path(getattr(sys, '_MEIPASS', Path(sys.executable).parent)) / 'assets'
else:
    ASSETS = Path(__file__).resolve().parent / 'assets'

# ---- palette (the website's custom properties) ----------------------------------------------
DEEP, BG, SURFACE, CARD, CARD_HOVER = '#08090a', '#0d0e10', '#121316', '#16171a', '#1c1d21'
BORDER, BORDER_STRONG, BORDER_HOVER = '#25272b', '#34373c', '#4a4d53'
TEXT, TEXT_SOFT, TEXT_STRONG, MUTED = '#d9d4ca', '#bdb8ae', '#ece7dd', '#8a8c90'
GOLD, GOLD_LIGHT = '#9c8458', '#c2a878'
BLOOD, BLOOD_HOVER, BLOOD_LIGHT = '#8a1118', '#a3161e', '#c94a44'
AMBER = '#d9a441'

# ---- fonts: replaced by load_fonts() when the bundled ones load -----------------------------
HEAD = HEAD_BOLD = 'Georgia'
BODY = 'Segoe UI'
BODY_MEDIUM = BODY_SEMI = 'Segoe UI Semibold'
ICONS = 'Segoe MDL2 Assets'
LANGUAGE = ''

# Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10) code points.
ICON = {'play': '', 'graphics': '', 'display': '', 'game': '', 'cheats': '',
        'online': '', 'mods': '', 'trophies': '', 'controls': '',
        'advanced': '', 'log': '', 'ok': '', 'warn': '', 'error': '',
        'info': '', 'keyboard': '', 'link': '', 'system': '',
        'folder': '', 'cpu': '', 'ram': '', 'pulse': '', 'help': '', 'picture': '', 'saves': '',
        'language': '', 'gameplay': ''}

SS = 4  # supersampling of the drawn images


def blend(a, b, t):
    """The colour between two #rrggbb colours, t from 0 (a) to 1 (b): fades and gradual fills."""
    one, two = (int(a[i:i + 2], 16) for i in (1, 3, 5)), (int(b[i:i + 2], 16) for i in (1, 3, 5))
    return '#' + ''.join(f'{round(x + (y - x) * t):02x}' for x, y in zip(one, two))


def load_fonts(root, language=''):
    """Registers the bundled fonts for this process only and picks the families Tk sees."""
    global HEAD, HEAD_BOLD, BODY, BODY_MEDIUM, BODY_SEMI, ICONS, LANGUAGE
    import tkinter.font as tkfont
    for path in sorted((ASSETS / 'fonts').glob('*.ttf')):
        try:
            ctypes.windll.gdi32.AddFontResourceExW(str(path), 0x10, 0)  # FR_PRIVATE
        except (AttributeError, OSError):
            pass
    families = set(tkfont.families(root))
    pick = lambda *names: next((n for n in names if n in families), names[-1])
    HEAD = pick('Cinzel SemiBold', 'Georgia')
    HEAD_BOLD = pick('Cinzel Bold', 'Cinzel SemiBold', 'Georgia')
    BODY = pick('Inter', 'Segoe UI')
    BODY_MEDIUM = pick('Inter Medium', 'Inter', 'Segoe UI')
    BODY_SEMI = pick('Inter SemiBold', 'Inter', 'Segoe UI Semibold')
    ICONS = pick('Segoe Fluent Icons', 'Segoe MDL2 Assets')
    LANGUAGE = language
    if language == 'zh':
        family = pick('Microsoft YaHei UI', 'Microsoft YaHei', 'Noto Sans CJK SC', 'Noto Sans SC',
                      'Source Han Sans SC', 'PingFang SC', 'WenQuanYi Micro Hei', BODY)
        HEAD = HEAD_BOLD = BODY = BODY_MEDIUM = BODY_SEMI = family


# ---- drawing --------------------------------------------------------------------------------
def rgba(color, alpha=255):
    color = color.lstrip('#')
    return tuple(int(color[i:i + 2], 16) for i in (0, 2, 4)) + (alpha,)


def mix(a, b, f):
    a, b = rgba(a), rgba(b)
    return '#' + ''.join(f'{round(x + (y - x) * f):02x}' for x, y in zip(a[:3], b[:3]))


def rounded(size, radius, fill=None, outline=None, width=1.0, box=None):
    """A rounded rectangle on a transparent image; `box` (x0, y0, x1, y1) draws a part only."""
    w, h = size
    image = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
    x0, y0, x1, y1 = box or (0, 0, w, h)
    color = lambda c: c and (rgba(c) if isinstance(c, str) else c)
    ImageDraw.Draw(image).rounded_rectangle(
        (x0 * SS, y0 * SS, x1 * SS - 1, y1 * SS - 1), radius * SS, fill=color(fill),
        outline=color(outline), width=max(1, round(width * SS)) if outline else 0)
    return image.resize(size, Image.LANCZOS)


def ellipse(image, box, fill=None, outline=None, width=1.0):
    """Draws an ellipse onto a supersampled image (coordinates in final pixels)."""
    ImageDraw.Draw(image).ellipse([v * SS for v in box], fill=fill and rgba(fill), outline=outline and rgba(outline),
                                  width=max(1, round(width * SS)) if outline else 0)


def canvas(size):
    return Image.new('RGBA', (size[0] * SS, size[1] * SS), (0, 0, 0, 0))


def finish(image, size):
    return image.resize(size, Image.LANCZOS)


def vertical_gradient(size, top, bottom):
    w, h = size
    strip = Image.new('RGBA', (1, h))
    for y in range(h):
        strip.putpixel((0, y), rgba(mix(top, bottom, y / max(1, h - 1))))
    return strip.resize(size)


def cover(image, size, focus=(0.5, 0.5)):
    """`image` scaled to fill `size`, cropped around `focus` (fractions of the source)."""
    w, h = size
    scale = max(w / image.width, h / image.height)
    scaled = image.resize((max(w, round(image.width * scale)), max(h, round(image.height * scale))), Image.LANCZOS)
    left = min(max(0, round(scaled.width * focus[0] - w / 2)), scaled.width - w)
    top = min(max(0, round(scaled.height * focus[1] - h / 2)), scaled.height - h)
    return scaled.crop((left, top, left + w, top + h))


def shade(size, stops, horizontal=True):
    """A black overlay whose alpha runs through `stops` [(position 0..1, alpha 0..255), ...]."""
    w, h = size
    n = w if horizontal else h
    line = Image.new('L', (n, 1) if horizontal else (1, n))
    for i in range(n):
        t = i / max(1, n - 1)
        for (p0, a0), (p1, a1) in zip(stops, stops[1:]):
            if p0 <= t <= p1:
                line.putpixel((i, 0) if horizontal else (0, i), round(a0 + (a1 - a0) * (t - p0) / max(1e-6, p1 - p0)))
                break
    return line.resize(size)


def fade(base, color, stops, horizontal=False):
    """Blends `color` over `base` with the alpha ramp of `stops`."""
    layer = Image.new('RGBA', base.size, rgba(color))
    layer.putalpha(shade(base.size, stops, horizontal))
    return Image.alpha_composite(base, layer)


_art = {}


def art(name):
    if name not in _art:
        try:
            _art[name] = Image.open(ASSETS / name).convert('RGBA')
        except OSError:
            _art[name] = None
    return _art[name]


def emblem(height, glow=True):
    """The community emblem `height` px tall, with a faint blood-red glow behind it."""
    source = art('emblem.png')
    if source is None:
        return None
    width = round(source.width * height / source.height)
    pad = round(height * 0.12) if glow else 0
    image = Image.new('RGBA', (width + 2 * pad, height + 2 * pad), (0, 0, 0, 0))
    mark = source.resize((width, height), Image.LANCZOS)
    if glow:
        halo = Image.new('RGBA', image.size, rgba(BLOOD, 0))
        alpha = Image.new('L', image.size, 0)
        alpha.paste(mark.getchannel('A'), (pad, pad))
        halo.putalpha(alpha.filter(ImageFilter.GaussianBlur(height * 0.06)).point(lambda a: a * 0.55))
        image = Image.alpha_composite(image, halo)
    image.alpha_composite(mark, (pad, pad))
    return image


_layers = {}
BLOOD_TOP, BLOOD_BOTTOM = 244 / 866, 478 / 866  # the blood's extent in the emblem art


def emblem_fill(height, fraction, phase=0.0):
    """The emblem with its chalice filled to `fraction` (0..1), a wave on the blood's surface:
    the setup guide fills it step by step, as the website's emblem rises."""
    import math
    bone, blood = art('emblem-bone.png'), art('emblem-blood.png')
    if bone is None or blood is None:
        return emblem(height, glow=False)
    if height not in _layers:
        width = round(bone.width * height / bone.height)
        _layers[height] = (bone.resize((width, height), Image.LANCZOS), blood.resize((width, height), Image.LANCZOS))
    bone, blood = _layers[height]
    w, h = bone.size
    top, bottom = BLOOD_TOP * h, BLOOD_BOTTOM * h
    level = bottom - (bottom - top) * max(0.0, min(1.0, fraction)) - (2 if fraction >= 1 else 0)
    amp = 0 if fraction <= 0 or fraction >= 1 else h * 0.008
    mask = Image.new('L', (w * 2, h * 2), 0)
    points = [(x, 2 * (level + amp * math.sin(x / (w * 2) * 2 * math.pi * 2 + phase))) for x in range(0, w * 2 + 8, 8)]
    ImageDraw.Draw(mask).polygon(points + [(w * 2, h * 2), (0, h * 2)], fill=255)
    mask = mask.resize((w, h), Image.LANCZOS)
    layer = blood.copy()
    layer.putalpha(Image.composite(blood.getchannel('A'), Image.new('L', (w, h), 0), mask))
    out = bone.copy()
    out.alpha_composite(layer)
    return out


def hero(size, panel):
    """The Play page's banner: Yharnam at night, darker on the left for the title, fading
    into the page at the bottom, and the emblem on the right."""
    w, h = size
    city = art('hero.jpg')
    base = cover(city, size, (0.5, 0.72)) if city else Image.new('RGBA', size, rgba(DEEP))
    base = fade(base, DEEP, [(0, 215), (0.45, 150), (1, 40)], horizontal=True)
    base = fade(base, panel, [(0, 0), (0.55, 0), (1, 255)])
    mark = emblem(round(h * 0.70))
    if mark is not None and w > h * 2.1:
        base.alpha_composite(mark, (w - mark.width - round(h * 0.18), round(h * 0.04)))
    return base


def header(size, panel, name='header.jpg', focus=(0.72, 0.42)):
    """A page's head: the cathedral (or other) art, dark under the title, fading into the page."""
    hall = art(name)
    base = cover(hall, size, focus) if hall else Image.new('RGBA', size, rgba(DEEP))
    base = fade(base, DEEP, [(0, 235), (0.5, 175), (1, 95)], horizontal=True)
    return fade(base, panel, [(0, 0), (0.45, 40), (1, 255)])


# ---- ttk ------------------------------------------------------------------------------------
# Every image is opaque: composited onto the colour it sits on. Tk draws an image with soft
# (partly transparent) pixels by reading the screen back and blending it in software, on every
# redraw; scrolling a page full of them was slow and could leave stale pixels in the corners.
# The images that ttk stretches (buttons, fields, cards, bars) are also made as large as the
# widgets get, so Tk draws nine pieces per widget instead of tiling a small image hundreds of times.

def solid(image, bg):
    """`image` over the colour `bg`, without transparency."""
    base = Image.new('RGBA', image.size, rgba(bg))
    base.alpha_composite(image.convert('RGBA'))
    return base.convert('RGB')


def stretch(image, size, border):
    """A nine-slice image (corners of `border` px) enlarged to `size`: the corners stay, the
    edges and the middle are repeated (they are flat, so nothing changes but the size)."""
    w, h = image.size
    W, H = max(size[0], w), max(size[1], h)
    bl, bt, br, bb = (border,) * 4 if isinstance(border, int) else border
    out = Image.new(image.mode, (W, H))
    xs = [(0, bl, 0, bl), (bl, w - br, bl, W - br), (w - br, w, W - br, W)]
    ys = [(0, bt, 0, bt), (bt, h - bb, bt, H - bb), (h - bb, h, H - bb, H)]
    for sx0, sx1, dx0, dx1 in xs:
        for sy0, sy1, dy0, dy1 in ys:
            if sx1 > sx0 and sy1 > sy0 and dx1 > dx0 and dy1 > dy0:
                part = image.crop((sx0, sy0, sx1, sy1))
                out.paste(part.resize((dx1 - dx0, dy1 - dy0), Image.NEAREST), (dx0, dy0))
    return out


def opaque(image):
    """RGB when nothing in `image` is transparent (Tk then draws it the fast way)."""
    if image.mode == 'RGBA' and image.getchannel('A').getextrema()[0] == 255:
        return image.convert('RGB')
    return image


# The colours widgets sit on, and the style prefix of each.
SURFACES = {'': BG, 'Card.': CARD, 'Bar.': DEEP}


class Theme:
    def __init__(self, root, dpi):
        self.root, self.dpi = root, dpi
        self.keep = []  # Tk holds no reference of its own to these images
        self.cache = {}

    def px(self, value):
        return max(1, round(value * self.dpi))

    def photo(self, image, bg=None):
        image = solid(image, bg) if bg else opaque(image)
        photo = ImageTk.PhotoImage(image, master=self.root)
        self.keep.append(photo)
        return photo

    def cached(self, key, make, bg=None):
        """A PhotoImage made once per key (banners per size); old sizes are dropped."""
        if key not in self.cache:
            if len(self.cache) > 64:
                self.cache.clear()
            image = make()
            if image is not None:
                image = solid(image, bg) if bg else opaque(image)
            self.cache[key] = image and ImageTk.PhotoImage(image, master=self.root)
        return self.cache[key]

    # -- pieces (PIL images) --
    def rounded_px(self, w, h, r, fill, outline=None, width=1.0):
        return rounded((self.px(w), self.px(h)), self.px(r), fill, outline, self.dpi * width)

    def box(self, w, h, r, fill, outline=None, width=1.0, bg=BG, big=None, border=None):
        """A rounded box on `bg`; with `big` (w, h) enlarged for a stretched ttk element."""
        image = solid(self.rounded_px(w, h, r, fill, outline, width), bg)
        if big:
            image = stretch(image, (self.px(big[0]), self.px(big[1])), self.px(border))
        return self.photo(image)

    def switch(self, on, track, knob, bg, gap=10):
        w, h, d = self.px(36), self.px(20), self.px(14)
        size = (w + self.px(gap), h)
        image = canvas(size)
        ImageDraw.Draw(image).rounded_rectangle((0, 0, w * SS - 1, h * SS - 1), h * SS // 2, fill=rgba(track))
        x = w - (h - d) // 2 - d if on else (h - d) // 2
        ellipse(image, (x, (h - d) / 2, x + d, (h + d) / 2), fill=knob)
        return self.photo(finish(image, size), bg)

    def radio(self, on, ring, bg, dot=None, gap=8):
        d = self.px(18)
        size = (d + self.px(gap), d)
        image = canvas(size)
        ellipse(image, (0.75, 0.75, d - 0.75, d - 0.75), fill=SURFACE, outline=ring, width=1.5 * self.dpi)
        if on:
            inset = d * 0.29
            ellipse(image, (inset, inset, d - inset, d - inset), fill=dot or ring)
        return self.photo(finish(image, size), bg)

    def chevron(self, color, bg, up=False, width=26, height=14):
        size = (self.px(width), self.px(height))
        image = canvas(size)
        cx, cy, s = size[0] / 2 - self.px(3), size[1] / 2, self.px(3.6)
        pts = [(cx - s, cy - s / 2), (cx, cy + s / 2), (cx + s, cy - s / 2)]
        if up:
            pts = [(x, 2 * cy - y) for x, y in pts]
        ImageDraw.Draw(image).line([(x * SS, y * SS) for x, y in pts], fill=rgba(color),
                                   width=round(1.6 * self.dpi * SS), joint='curve')
        return self.photo(finish(image, size), bg)

    def slider(self, fill, bg):
        """The knob, over the trough's line so that its corners match what is under it."""
        d = self.px(18)
        image = canvas((d, d))
        ellipse(image, (0, 0, d, d), fill=DEEP)
        ellipse(image, (self.px(2.5), self.px(2.5), d - self.px(2.5), d - self.px(2.5)), fill=fill)
        under = Image.new('RGBA', (d, d), rgba(bg))
        ImageDraw.Draw(under).rectangle((0, self.px(7), d, self.px(11) - 1), fill=rgba(BORDER_STRONG))
        under.alpha_composite(finish(image, (d, d)))
        return self.photo(under.convert('RGB'))

    # -- the whole style --
    def style(self, s):
        px = self.px
        s.theme_use('clam')
        base = (BODY, 10)
        import tkinter.font as tkfont
        for name in ('TkDefaultFont', 'TkTextFont', 'TkMenuFont', 'TkHeadingFont', 'TkTooltipFont'):
            try:
                tkfont.nametofont(name, root=self.root).configure(family=BODY, size=10 if name != 'TkTooltipFont' else 9)
            except Exception:  # an unknown named font on this Tk
                pass
        root = self.root
        root.option_add('*TCombobox*Listbox.background', SURFACE)
        root.option_add('*TCombobox*Listbox.foreground', TEXT)
        root.option_add('*TCombobox*Listbox.selectBackground', BLOOD)
        root.option_add('*TCombobox*Listbox.selectForeground', TEXT_STRONG)
        root.option_add('*TCombobox*Listbox.font', base)
        root.option_add('*TCombobox*Listbox.borderWidth', 0)
        root.option_add('*TCombobox*Listbox.relief', 'flat')
        s.configure('.', background=BG, foreground=TEXT, fieldbackground=SURFACE, bordercolor=BORDER,
                    lightcolor=BORDER, darkcolor=BORDER, troughcolor=SURFACE, focuscolor=GOLD,
                    selectbackground=BLOOD, selectforeground=TEXT_STRONG, insertcolor=TEXT, font=base)
        # clam colours disabled and hovered widgets light grey; every widget keeps its style's
        # own background instead.
        for name in ('.', 'TButton', 'TCheckbutton', 'TRadiobutton', 'TCombobox', 'TEntry', 'TSpinbox',
                     'TScrollbar', 'TScale', 'TProgressbar', 'TLabel', 'TFrame'):
            s.map(name, background=[])
        s.configure('ComboboxPopdownFrame', background=SURFACE, bordercolor=BORDER_STRONG, relief='solid',
                    borderwidth=1)
        label = lambda element: [(element, {'sticky': 'nswe', 'children': [
            ('Button.padding', {'sticky': 'nswe', 'children': [('Button.label', {'sticky': 'nswe'})]})]})]

        # Buttons, drop-downs, fields, switches and radio buttons once per surface colour.
        for prefix, bg in SURFACES.items():
            tag = 'Bb' + prefix.rstrip('.')
            button = lambda fill, line, bg=bg: self.box(40, 34, 8, fill, line, bg=bg, big=(360, 96), border=10)
            s.element_create(f'{tag}.Button.border', 'image', button(CARD, BORDER_STRONG),
                             ('disabled', button(SURFACE, BORDER)), ('pressed', button(SURFACE, GOLD)),
                             ('active', button(CARD_HOVER, GOLD)), border=px(10), padding=px(2), width=px(40), height=px(34), sticky='nsew')
            s.element_create(f'{tag}.Accent.border', 'image', button(BLOOD, '#a8323a'),
                             ('disabled', button('#2a1416', '#3a2024')), ('pressed', button('#6e0d13', BLOOD_LIGHT)),
                             ('active', button(BLOOD_HOVER, BLOOD_LIGHT)), border=px(10), padding=px(2), width=px(40), height=px(34), sticky='nsew')
            for name in ('TButton', 'Tight.TButton', 'Big.TButton'):
                s.layout(prefix + name, label(f'{tag}.Button.border'))
            for name in ('Accent.TButton', 'Tight.Accent.TButton'):
                s.layout(prefix + name, label(f'{tag}.Accent.border'))

            field = lambda fill, line, bg=bg: self.box(40, 34, 7, fill, line, bg=bg, big=(520, 64), border=9)
            s.element_create(f'{tag}.field', 'image', field(SURFACE, BORDER_STRONG),
                             ('disabled', field(BG, BORDER)), ('focus', field(SURFACE, GOLD)),
                             ('hover', field(SURFACE, BORDER_HOVER)), border=px(9), padding=(px(3), px(2)),
                             width=px(40), height=px(34), sticky='nsew')
            s.layout(prefix + 'TEntry', [(f'{tag}.field', {'sticky': 'nswe', 'children': [
                ('Entry.padding', {'sticky': 'nswe', 'children': [('Entry.textarea', {'sticky': 'nswe'})]})]})])
            s.layout(prefix + 'TCombobox', [(f'{tag}.field', {'sticky': 'nswe', 'children': [
                ('Bb.Combobox.downarrow', {'side': 'right', 'sticky': ''}),
                ('Combobox.padding', {'sticky': 'nswe', 'children': [('Combobox.textarea', {'sticky': 'nswe'})]})]})])
            s.layout(prefix + 'TSpinbox', [(f'{tag}.field', {'sticky': 'nswe', 'children': [
                ('null', {'side': 'right', 'sticky': '', 'children': [
                    ('Bb.Spinbox.uparrow', {'side': 'top', 'sticky': ''}),
                    ('Bb.Spinbox.downarrow', {'side': 'bottom', 'sticky': ''})]}),
                ('Spinbox.padding', {'sticky': 'nswe', 'children': [('Spinbox.textarea', {'sticky': 'nswe'})]})]})])

            s.element_create(f'{tag}.switch', 'image', self.switch(False, '#2a2c31', '#8a8c90', bg),
                             ('disabled', 'selected', self.switch(True, '#3a1518', '#7d7470', bg)),
                             ('disabled', self.switch(False, '#1b1c1f', '#45474b', bg)),
                             ('active', 'selected', self.switch(True, BLOOD_HOVER, '#ffffff', bg)),
                             ('selected', self.switch(True, BLOOD, TEXT_STRONG, bg)),
                             ('active', self.switch(False, '#33363b', '#a9abaf', bg)), sticky='')
            s.layout(prefix + 'TCheckbutton', [('Checkbutton.padding', {'sticky': 'nswe', 'children': [
                (f'{tag}.switch', {'side': 'left', 'sticky': ''}),
                ('Checkbutton.label', {'side': 'left', 'sticky': 'nswe'})]})])
            s.element_create(f'{tag}.radio', 'image', self.radio(False, '#5a5d63', bg),
                             ('disabled', 'selected', self.radio(True, BORDER_STRONG, bg)),
                             ('disabled', self.radio(False, BORDER, bg)),
                             ('selected', self.radio(True, BLOOD_LIGHT, bg)),
                             ('active', self.radio(False, '#8a8d93', bg)), sticky='')
            s.layout(prefix + 'TRadiobutton', [('Radiobutton.padding', {'sticky': 'nswe', 'children': [
                (f'{tag}.radio', {'side': 'left', 'sticky': ''}),
                ('Radiobutton.label', {'side': 'left', 'sticky': 'nswe'})]})])

        slot = lambda fill, line: self.box(30, 26, 6, fill, line, big=(240, 64), border=7)  # the binding table
        s.element_create('Bb.Slot.border', 'image', slot(CARD, BORDER),
                         ('disabled', slot(CARD, BORDER)), ('pressed', slot(SURFACE, GOLD)),
                         ('active', slot(CARD_HOVER, GOLD)), border=px(7), padding=px(1), width=px(30), height=px(26), sticky='nsew')
        s.element_create('Bb.Listen.border', 'image', slot(CARD_HOVER, BLOOD_LIGHT), border=px(7), padding=px(1),
                         width=px(30), height=px(26), sticky='nsew')
        s.layout('Slot.TButton', label('Bb.Slot.border'))
        s.layout('Listen.TButton', label('Bb.Listen.border'))

        def play(top, bottom, line):
            w, h, r = px(160), px(54), px(10)
            fill = vertical_gradient((w * SS, h * SS), top, bottom)
            mask = rounded((w, h), r, '#ffffff').getchannel('A').resize((w * SS, h * SS))
            image = Image.new('RGBA', (w * SS, h * SS), (0, 0, 0, 0))
            image.paste(fill, (0, 0), mask)
            image = finish(image, (w, h))
            image.alpha_composite(rounded((w, h), r, None, line, self.dpi))
            return self.photo(stretch(solid(image, DEEP), (px(520), h), (px(14), 0, px(14), 0)))
        s.element_create('Bb.Play.border', 'image', play('#9e1720', '#7a0f15', '#b8484c'),
                         ('disabled', play('#2a1618', '#211214', '#3a2426')),
                         ('pressed', play('#7a0f15', '#650c12', BLOOD_LIGHT)),
                         ('active', play('#b51c26', '#8f131a', GOLD_LIGHT)), border=px(14), padding=px(2), width=px(160), height=px(54), sticky='nsew')
        s.layout('Play.TButton', label('Bb.Play.border'))

        s.configure('TButton', background=BG, foreground=TEXT, padding=(px(14), px(6)), anchor='center',
                    font=(BODY_MEDIUM, 10))
        s.map('TButton', foreground=[('disabled', MUTED), ('active', TEXT_STRONG)])
        s.configure('Accent.TButton', foreground=TEXT_STRONG)
        s.configure('Tight.TButton', padding=(px(4), px(6)), font=(BODY_MEDIUM, 9))  # the side bar's update box
        s.configure('Tight.Accent.TButton', padding=(px(4), px(6)), font=(BODY_MEDIUM, 9))
        s.configure('Big.TButton', padding=(px(18), px(9)), font=(BODY_MEDIUM, 10))
        s.configure('Slot.TButton', padding=(px(6), px(4)), font=(BODY, 9))
        s.map('Slot.TButton', foreground=[('disabled', TEXT), ('active', TEXT_STRONG)])
        s.configure('Listen.TButton', foreground=GOLD_LIGHT, padding=(px(6), px(4)), font=(BODY_MEDIUM, 9))
        s.map('Listen.TButton', foreground=[('disabled', GOLD_LIGHT)])
        s.configure('Play.TButton', background=DEEP, foreground='#f4ece0', font=(HEAD_BOLD, 11 if LANGUAGE == 'zh' else 15),
                    padding=(px(40), px(11)))
        s.map('Play.TButton', foreground=[('disabled', '#6d6360')])

        # The drop-down chevron sits on the field (SURFACE), or on BG while disabled.
        s.element_create('Bb.Combobox.downarrow', 'image', self.chevron(GOLD_LIGHT, SURFACE),
                         ('disabled', self.chevron(MUTED, BG)), sticky='')
        s.element_create('Bb.Spinbox.uparrow', 'image', self.chevron(GOLD_LIGHT, SURFACE, True, 22, 11), sticky='')
        s.element_create('Bb.Spinbox.downarrow', 'image', self.chevron(GOLD_LIGHT, SURFACE, False, 22, 11), sticky='')
        for name in ('TEntry', 'TCombobox', 'TSpinbox'):
            s.configure(name, foreground=TEXT, padding=(px(9), px(5), px(4), px(5)), insertcolor=TEXT,
                        selectbackground=BLOOD, selectforeground=TEXT_STRONG)
            s.map(name, foreground=[('disabled', MUTED)], fieldbackground=[('readonly', SURFACE)],
                  selectbackground=[('readonly', SURFACE)], selectforeground=[('readonly', TEXT)])
        for name in ('TCheckbutton', 'TRadiobutton'):
            s.configure(name, background=BG, foreground=TEXT, padding=(0, px(4)))
            s.map(name, background=[('active', BG)], foreground=[('disabled', MUTED), ('active', TEXT_STRONG)])

        # Slider, progress bar and a thin scroll bar (on the page background).
        trough = solid(rounded((px(30), px(18)), px(2), BORDER_STRONG, box=(0, px(7), px(30), px(11))), BG)
        s.element_create('Bb.Scale.trough', 'image', self.photo(stretch(trough, (px(800), px(18)), (px(6), 0, px(6), 0))),
                         border=(px(6), 0, px(6), 0), width=px(30), height=px(18), sticky='ew')
        s.element_create('Bb.Scale.slider', 'image', self.slider(GOLD_LIGHT, BG),
                         ('pressed', self.slider(TEXT_STRONG, BG)), ('active', self.slider('#d8c194', BG)), sticky='')
        s.layout('Horizontal.TScale', [('Bb.Scale.trough', {'sticky': 'ew', 'children': [
            ('Bb.Scale.slider', {'side': 'left', 'sticky': ''})]})])
        s.configure('Horizontal.TScale', background=BG)
        bar = solid(self.rounded_px(24, 8, 4, BORDER), BG)
        s.element_create('Bb.Progressbar.trough', 'image', self.photo(stretch(bar, (px(800), px(8)), (px(5), 0, px(5), 0))),
                         border=(px(5), 0), width=px(24), height=px(8), sticky='ew')
        # The filled part: rounded on the left like the trough, its right end over the trough's middle.
        under = bar.copy()
        middle = bar.crop((px(5), 0, px(6), bar.height)).resize((px(5), bar.height))
        under.paste(middle, (bar.width - px(5), 0))
        fill = under.convert('RGBA')
        fill.alpha_composite(self.rounded_px(24, 8, 4, GOLD_LIGHT))
        s.element_create('Bb.Progressbar.pbar', 'image', self.photo(stretch(fill.convert('RGB'), (px(800), px(8)),
                                                                             (px(5), 0, px(5), 0))),
                         border=(px(5), 0), width=px(24), height=px(8), sticky='nsew')
        s.layout('Horizontal.TProgressbar', [('Bb.Progressbar.trough', {'sticky': 'ew', 'children': [
            ('Bb.Progressbar.pbar', {'side': 'left', 'sticky': 'ns'})]})])
        s.configure('Horizontal.TProgressbar', background=BG, thickness=px(8))
        thumb = lambda fill: self.photo(stretch(solid(rounded((px(10), px(28)), px(3), fill, box=(px(2), 0, px(8), px(28))), BG),
                                                (px(10), px(1200)), (0, px(5), 0, px(5))))
        s.element_create('Bb.Vertical.Scrollbar.trough', 'image',
                         self.photo(Image.new('RGB', (px(10), px(2400)), rgba(BG)[:3])), width=px(10), height=px(10), sticky='ns')
        s.element_create('Bb.Vertical.Scrollbar.thumb', 'image', thumb(BORDER_STRONG), ('pressed', thumb(GOLD)),
                         ('active', thumb(BORDER_HOVER)), border=(0, px(5)), width=px(10), height=px(28), sticky='nsew')
        s.layout('Vertical.TScrollbar', [('Bb.Vertical.Scrollbar.trough', {'sticky': 'ns', 'children': [
            ('Bb.Vertical.Scrollbar.thumb', {'expand': '1', 'sticky': 'nswe'})]})])
        s.configure('Vertical.TScrollbar', background=BG)

        # Cards: a rounded panel, large enough that Tk never tiles its middle.
        s.element_create('Bb.card', 'image', self.box(48, 48, 12, CARD, BORDER, bg=BG, big=(1400, 900), border=14),
                         border=px(14), width=px(48), height=px(48), sticky='nsew')
        s.layout('Card.TFrame', [('Bb.card', {'sticky': 'nswe'})])
        s.configure('Card.TFrame', background=BG)
        s.configure('TFrame', background=BG)
        s.configure('Inner.TFrame', background=CARD)
        s.configure('TLabel', background=BG, foreground=TEXT)
        s.configure('Muted.TLabel', foreground=MUTED, font=(BODY, 9))
        s.configure('Section.TLabel', foreground=GOLD_LIGHT, font=(HEAD, 12))
        s.configure('Code.TLabel', foreground=GOLD_LIGHT, font=('Consolas', 22, 'bold'))
        s.configure('Warning.TLabel', background='#1e1a12', foreground='#e0b866', padding=(px(12), px(8)))
        for prefix, bg in SURFACES.items():
            if not prefix:
                continue
            for name in ('TButton', 'Accent.TButton', 'Tight.TButton', 'Tight.Accent.TButton', 'Big.TButton',
                         'TCheckbutton', 'TRadiobutton', 'TLabel', 'Muted.TLabel', 'Section.TLabel', 'TEntry',
                         'TCombobox', 'TSpinbox'):
                s.configure(prefix + name, background=bg)
            s.map(prefix + 'TCheckbutton', background=[('active', bg)])
            s.map(prefix + 'TRadiobutton', background=[('active', bg)])
