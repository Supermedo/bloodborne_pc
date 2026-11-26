"""Short, localized glyph descriptions built from the game's own text (+ 4 tags we translate).

Usage: short_desc.py <msg dir (all languages)> <catalog.bin v3> <out data.json>

For every catalog glyph, the English free-text description is read for game names (bosses,
chalice weapons, runes) and keywords, and turned into references: game text IDs where the game
has the word (so every language gets the official translation), a few tags of our own otherwise.
The parts are ordered by importance and trimmed from the end to fit a width limit (CJK = 2).
"""
import collections, json, re, struct, sys, unicodedata
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from fmg_lookup import dcx, bnd4, fmg

MSG = Path(sys.argv[1])
LANGS = ['engus', 'enggb', 'deude', 'frafr', 'itait', 'spaes', 'spaar', 'porpt', 'porbr', 'nldnl', 'dandk',
         'swese', 'norno', 'finfi', 'polpl', 'rusru', 'turtr', 'jpnjp', 'korkr', 'zhocn', 'zhotw']
LANG_NAMES = {'engus': 'English (US)', 'enggb': 'English (UK)', 'deude': 'Deutsch', 'frafr': 'Français',
              'itait': 'Italiano', 'spaes': 'Español (España)', 'spaar': 'Español (Latinoamérica)',
              'porpt': 'Português (Portugal)', 'porbr': 'Português (Brasil)', 'nldnl': 'Nederlands',
              'dandk': 'Dansk', 'swese': 'Svenska', 'norno': 'Norsk', 'finfi': 'Suomi', 'polpl': 'Polski',
              'rusru': 'Русский', 'turtr': 'Türkçe', 'jpnjp': '日本語', 'korkr': '한국어',
              'zhocn': '简体中文', 'zhotw': '繁體中文'}
LIMIT = 150

# Our own words (no game text for these).
OWN = {
    'farm': {'engus': 'Farm', 'enggb': 'Farm', 'deude': 'Farmen', 'frafr': 'Farm', 'itait': 'Farm',
             'spaes': 'Farmeo', 'spaar': 'Farmeo', 'porpt': 'Farm', 'porbr': 'Farm', 'nldnl': 'Farmen',
             'dandk': 'Farm', 'swese': 'Farmning', 'norno': 'Farming', 'finfi': 'Farmaus', 'polpl': 'Farmienie',
             'rusru': 'Фарм', 'turtr': 'Farm', 'jpnjp': '稼ぎ', 'korkr': '파밍', 'zhocn': '刷取', 'zhotw': '刷取'},
    'rare': {'engus': 'Rare layout', 'enggb': 'Rare layout', 'deude': 'Seltenes Layout', 'frafr': 'Disposition rare',
             'itait': 'Layout raro', 'spaes': 'Diseño raro', 'spaar': 'Diseño raro', 'porpt': 'Layout raro',
             'porbr': 'Layout raro', 'nldnl': 'Zeldzame indeling', 'dandk': 'Sjældent layout', 'swese': 'Sällsynt layout',
             'norno': 'Sjelden utforming', 'finfi': 'Harvinainen pohja', 'polpl': 'Rzadki układ',
             'rusru': 'Редкая планировка', 'turtr': 'Nadir düzen', 'jpnjp': 'レア配置', 'korkr': '희귀 구조',
             'zhocn': '稀有布局', 'zhotw': '稀有佈局'},
    'glitch': {'engus': 'Glitch', 'enggb': 'Glitch', 'deude': 'Glitch', 'frafr': 'Bug', 'itait': 'Glitch',
               'spaes': 'Bug', 'spaar': 'Bug', 'porpt': 'Bug', 'porbr': 'Bug', 'nldnl': 'Glitch', 'dandk': 'Fejl',
               'swese': 'Bugg', 'norno': 'Feil', 'finfi': 'Bugi', 'polpl': 'Błąd', 'rusru': 'Баг', 'turtr': 'Hata',
               'jpnjp': 'バグ', 'korkr': '버그', 'zhocn': '漏洞', 'zhotw': '漏洞'},
    'false': {'engus': 'False depth', 'enggb': 'False depth', 'deude': 'Falsche Tiefe', 'frafr': 'Fausse profondeur',
              'itait': 'Falsa profondità', 'spaes': 'Falsa profundidad', 'spaar': 'Falsa profundidad',
              'porpt': 'Falsa profundidade', 'porbr': 'Falsa profundidade', 'nldnl': 'Valse diepte',
              'dandk': 'Falsk dybde', 'swese': 'Falskt djup', 'norno': 'Falsk dybde', 'finfi': 'Väärä syvyys',
              'polpl': 'Fałszywa głębia', 'rusru': 'Ложная глубина', 'turtr': 'Sahte derinlik', 'jpnjp': '偽深度',
              'korkr': '가짜 깊이', 'zhocn': '假深度', 'zhotw': '假深度'},
}
# Official game words: (archive, fmg, id).
GAME_WORD = {'echoes': ('menu', 2, 34001), 'bloodstone': ('menu', 2, 34003), 'gem': ('menu', 2, 34004),
             'rune': ('menu', 2, 34005), 'material': ('menu', 2, 34006), 'treasure': ('menu', 2, 34014),
             'hidden': ('menu', 2, 34029), 'messenger': ('menu', 30, 70000003)}

# Blood gems: the effect line exactly as the game's gem screen shows it (menu FMG 35, a template whose
# number we replace), and the gem shape (menu FMG 200). Tempering = Physical ATK UP, Adept = ATK UP.
GEM_SHAPE = {'radial': 14203, 'triangle': 14202, 'waning': 14205, 'droplet': 14204, 'circle': 14201, 'circular': 14201}
# How notes name a gem's effect -> the effect line id (longest phrasings first). Tempering = Physical ATK UP,
# Adept = ATK UP (all types), Bloodtinge = Blood ATK UP, Nourishing = ATK UP (notes: "Nourishing 23.6% ATK up").
GEM_STATS = [
    (r'phys(?:ical)?\.?\s+(?:atk\s+)?(?:up\s+)?at\s+full\s+hp', 48101),
    (r'phys(?:ical)?\.?\s+(?:atk\s+)?(?:up\s+)?near\s+death', 49101),
    (r'(?:atk|attack)\s+(?:up\s+)?near\s+death', 52101),
    (r'(?:atk|attack)\s+(?:up\s+)?(?:when\s+)?at\s+full\s+hp', 51101),
    (r'charged?\s+(?:atk|attack)s?(?:\s+up)?', 42101),
    (r'(?:atk\s+)?vs\.?\s*beasts?', 33101), (r'(?:atk\s+)?vs\.?\s*(?:the\s+)?kin', 35101),
    (r'all\s+damage', 27101), (r'fire\s+atk(?:\s+up)?', 24101), (r'bolt\s+atk(?:\s+up)?', 25101),
    (r'blood\s+atk(?:\s+up)?', 22101), (r'arcane\s+atk(?:\s+up)?', 23101),
    (r'phys(?:ical)?\.?(?:\s+atk)?(?:\s+up)?', 26101), (r'phy', 26101), (r'tempering', 26101),
    (r'adept', 27101), (r'bloodtinge', 22101), (r'blt', 22101), (r'bt', 22101),
    (r'arcane', 23101), (r'arcana', 23101), (r'arc', 23101), (r'fire', 24101), (r'bolt', 25101),
    (r'nourishing', 27101), (r'nourish', 27101), (r'blunt', 20101), (r'thrust', 21101), (r'striking', 20101), (r'finestrike', 21101),
    (r'beasthunter', 33101), (r'kinhunter', 35101), (r'atk(?:\s+up)?', 27101), (r'attack', 27101)]
STAT = '(' + '|'.join(p for p, _ in GEM_STATS) + ')'
SHAPE = r'(radial|triangle|waning|droplet|circle|circular)'
NUM = r'(\d+(?:[.,]\d+)?)'
GEM_NUM_FIRST = re.compile(r'(?<![\d.,])(\+)?' + NUM + r'\s*(%)?\s*(?:' + SHAPE + r's?\s+)?(?:(?:odd|hard|heavy|abyssal|sharp|cursed)\s+)*' + STAT + r'\b', re.I)
GEM_STAT_FIRST = re.compile(r'\b' + STAT + r'\b[^%\d|/,;.){}]{0,25}?\+?' + NUM + r'\s*%', re.I)
GEM_SHAPE_ONLY = re.compile(r'(?<![\d.,])' + NUM + r'\s*%', re.I)
GEMMY = re.compile(r'gem|radial|triangle|waning|droplet|tempering|adept|bloodtinge|\bblt\b|nourishing|abyssal', re.I)

def effect_of(phrase):
    for p, eff in GEM_STATS:
        if re.fullmatch(p, phrase, re.I):
            return eff
    return None

def gems(d):
    """[(layer, shape key, effect id or None, value)] in note order, best value per effect, at most 3."""
    if not GEMMY.search(d):
        return []
    found, used = [], set()
    # Number-first wins: in '+21% Physical +12% Physical ATK at Full HP', 'Physical' must not take '+12%'.
    for m in GEM_NUM_FIRST.finditer(d):
        plus, num, pct, shape, stat = m.groups()
        if not (pct or (not plus and ('.' in num or ',' in num or shape))):
            continue
        found.append((m.start(), effect_of(stat), shape, num))
        used.add(m.start(2))
    for m in GEM_STAT_FIRST.finditer(d):
        if m.start(2) in used:
            continue
        found.append((m.start(), effect_of(m.group(1)), None, m.group(2)))
        used.add(m.start(2))
    if not found:  # "27.2% radials", "drops 28.7%": a percentage in a gem note, effect not named
        loose = [(m.start(), None, None, m.group(1)) for m in GEM_SHAPE_ONLY.finditer(d)
                 if 10 <= float(m.group(1).replace(',', '.')) <= 60]
        found = sorted(loose, key=lambda f: -float(f[3].replace(',', '.')))[:1]
    best = {}
    for pos, eff, shape, num in sorted(found):
        val = round(float(num.replace(',', '.')), 2)
        if not 1 <= val <= 60:
            continue
        shape = shape.lower().rstrip('s') if shape else None
        if not shape:  # nearest shape word before, else the first one in the note
            before = list(re.finditer(SHAPE, d[:pos], re.I))
            m = before[-1] if before else re.search(SHAPE, d, re.I)
            shape = m.group(1).lower() if m else None
        if eff not in best or val > best[eff][4]:
            best[eff] = (pos, layer_before(d, pos), shape, eff, val)
    return [b[1:] for b in sorted(best.values())[:3]]

tables = {}
def table(lang, arc, fid):
    key = (lang, arc)
    if key not in tables:
        tables[key] = bnd4(dcx((MSG / lang / f'{arc}.msgbnd.dcx').read_bytes()))
    return fmg(tables[key][fid][1])
cache = {}
def text(lang, arc, fid, i):
    k = (lang, arc, fid)
    if k not in cache:
        cache[k] = table(lang, arc, fid)
    return (cache[k].get(i) or '').strip()

# English names -> references.
names = {}
def add(name, ref):
    name = name.strip().lower()
    if len(name) >= 3:
        names.setdefault(name, ref)
bosses = cache.setdefault(('engus', 'item', 18), table('engus', 'item', 18))
GENERIC = {'the', 'a', 'an', 'hunter', 'lady', 'old', 'great', 'nameless', 'living', 'heroine', 'defeated'}
for i, t in bosses.items():
    if not t or t.startswith(('*', 'en')):
        continue
    ref = ('boss', 'item', 18, i)
    low = t.lower()
    add(low, ref); add(low + 's', ref)
    if low.startswith('the '): add(low[4:], ref)
    if ',' in low: add(low.split(',')[0], ref)
    words = [w for w in low.replace(',', '').split() if w != 'the']
    if 'of' in words[1:]:
        head = ' '.join(words[:words.index('of')])
        if head not in GENERIC: add(head, ref)
for short, full in {'bps': 'beast-possessed soul', 'bepis': 'beast-possessed soul',
                    'bloodletting': 'bloodletting beast', 'descendant': 'pthumerian descendant',
                    'emissary': 'celestial emissary', 'darkbeast': 'loran darkbeast'}.items():
    if full in names: add(short, names[full])
weapons = cache.setdefault(('engus', 'item', 11), table('engus', 'item', 11))
for i, t in weapons.items():  # chalice-dungeon variants: "Uncanny X", "Ludwig's Lost Holy Blade", ...
    if t and re.search(r'(^|\s)(Uncanny|Lost)\s', t) and '+' not in t:
        add(t, ('weapon', 'item', 11, i))
goods = cache.setdefault(('engus', 'item', 10), table('engus', 'item', 10))
for i, t in goods.items():
    m = re.match(r'Caryll Rune: "(.+)"', t or '')
    if m:
        add(m.group(1), ('rune', 'item', 10, i))
patterns = sorted(names, key=len, reverse=True)
NAME_RE = re.compile('|'.join(r'(?<![a-z])' + re.escape(p) + r'(?![a-z])' for p in patterns), re.I)

def layer_before(d, pos):
    m = None
    for m in re.finditer(r'\bL\s?([1-4])\b|\blayer\s?([1-4])\b', d[:pos], re.I):
        pass
    return (m.group(1) or m.group(2)) if m else None

def analyse(d, chalice):
    low = d.lower()
    parts = []  # (priority, kind, payload)
    if 'farm' in low:
        subj = 'echoes' if 'echo' in low else 'bloodstone' if 'bloodstone' in low or 'blood stone' in low else \
               'gem' if 'gem' in low or 'madness' in low or 'radial' in low or 'triangle' in low or 'waning' in low else \
               'material' if 'ritual' in low or 'material' in low or 'bastard' in low else None
        parts.append(('farm', subj))
    if 'false depth' in low:
        parts.append(('false', None))
    loot, seen = [], set()
    for m in NAME_RE.finditer(d):
        ref = names[m.group(0).lower()]
        if ref[0] in ('weapon', 'rune') and ref not in seen:
            seen.add(ref)
            tier = re.match(r'\s*(?:tier|t)\s?([1-3])', d[m.end():], re.I)
            loot.append(('loot', (ref, layer_before(d, m.start()), tier.group(1) if tier else None)))
    # What people run dungeons for comes first: runes, then gems, then chalice weapons.
    parts += [p for p in loot if p[1][0][0] == 'rune']
    g = gems(d)
    if g:
        parts.append(('gem', g))
    parts += [p for p in loot if p[1][0][0] == 'weapon']
    for key, pat in (('messenger', r'messenger|\bbath\b|\bshop'), ('rare', r'rare layout|\brare\b|odd layout|unusual layout|special layout'),
                     ('hidden', r'illusory|hidden'), ('treasure', r'bonus room|bonus area|treasure room'),
                     ('glitch', r'glitch|bugged|exploit')):
        m = re.search(pat, low)
        if m:
            parts.append((key, layer_before(d, m.start())))
    boss_list, layer_of = [], {}
    for m in NAME_RE.finditer(d):
        ref = names[m.group(0).lower()]
        if ref[0] == 'boss':
            if ref not in boss_list:
                boss_list.append(ref)
            layer_of[ref] = layer_of.get(ref) or layer_before(d, m.start())
    boss_list = [(ref, layer_of[ref]) for ref in boss_list]
    parts.append(('chalice', chalice))
    if boss_list:  # last: players know the bosses; trimmed first, one by one
        parts.append(('bosses', boss_list))
    return parts

def word(lang, key):
    if key in OWN:
        return OWN[key][lang]
    arc, fid, i = GAME_WORD[key]
    return text(lang, arc, fid, i)

def width(s):
    return sum(2 if unicodedata.east_asian_width(c) in 'WF' else 1 for c in s)

TAG_KINDS = ('messenger', 'rare', 'hidden', 'treasure', 'glitch')
PRIORITY = {'farm': 0, 'false': 0, 'rune': 1, 'gem': 2, 'weapon': 3, 'tag': 4, 'chalice': 5, 'boss': 6}

def atoms(parts):
    """Split the parts into single items: (layer or None, priority, note order, kind, payload)."""
    out = []
    for kind, payload in parts:
        n = len(out)
        if kind in ('farm', 'false'):
            out.append((None, 0, n, kind, payload))
        elif kind == 'loot':
            out.append((payload[1], PRIORITY[payload[0][0]], n, payload[0][0], payload))
        elif kind == 'gem':
            for k, g in enumerate(payload):
                out.append((g[0], 2, n + k, 'gem', g))
        elif kind in TAG_KINDS:
            out.append((payload, 4, n, 'tag', kind))
        elif kind == 'bosses':
            for k, (ref, layer) in enumerate(payload):
                out.append((layer, 6, n + k, 'boss', ref))
        elif kind == 'chalice':
            out.append((None, 5, n, 'chalice', payload))
    return out

def item_text(a, lang):
    layer, pri, _, kind, p = a
    if kind == 'farm':
        return word(lang, 'farm') + (': ' + word(lang, p) if p else '')
    if kind == 'false':
        return word(lang, 'false')
    if kind in ('rune', 'weapon'):
        (k, arc, fid, i), _, tier = p
        name = text(lang, arc, fid, i)
        if k == 'rune':  # 'Caryll Rune: "Deep Sea"' / 'カレルの遺稿「瞳」' -> the quoted name, with its quotes
            q = re.findall(r'(“[^”]+”|"[^"]+"|「[^」]+」|«[^»]+»|„[^“”]+[“”]|‘[^’]+’|”[^”]+”)', name)
            if q:
                name = q[-1]
            else:
                m = re.search(r'[:：]\s*(.+)$', name)
                name = m.group(1) if m else name
        return name + (f' {tier}' if tier else '')
    if kind == 'gem':
        _, shape, eff, val = p
        sep = ',' if re.search(r'\d,\d', text(lang, 'menu', 35, 23105)) else '.'
        if eff:
            return re.sub(r'[+-]?\d+(?:[.,]\d+)?', f'+{val:g}'.replace('.', sep),
                          text(lang, 'menu', 35, eff), count=1).rstrip('.。')
        return word(lang, 'gem') + ' ' + f'{val:g}'.replace('.', sep) + '%'
    if kind == 'tag':
        return word(lang, p)
    if kind == 'boss':
        return text(lang, p[1], p[2], p[3])
    if kind == 'chalice':
        return text(lang, 'item', 10, p)

def join_items(items, lang):
    """One group's items; gems of the same shape share it: 'Triangle: A / B'."""
    comma = '、' if lang in ('jpnjp', 'zhocn', 'zhotw') else ', '
    out = []
    prev_gem, shape_of_prev = False, None
    for a in items:
        t = item_text(a, lang)
        if not t:
            continue
        if a[3] == 'gem':
            shape = a[4][1]
            if out and prev_gem and shape_of_prev == shape:
                out[-1] += ' / ' + t
                continue
            if shape:
                t = text(lang, 'menu', 200, GEM_SHAPE[shape]) + ': ' + t
            prev_gem, shape_of_prev = True, shape
        else:
            prev_gem, shape_of_prev = False, None
        out.append(t)
    return comma.join(out)

def compose(items, lang):
    """Farm / false depth, then L1..L4 groups, then items without a layer, the chalice, unplaced bosses."""
    order = lambda a: (a[1], a[2])
    segs = [item_text(a, lang) for a in items if a[3] in ('farm', 'false')]
    for layer in sorted({a[0] for a in items if a[0]}):
        g = join_items(sorted((a for a in items if a[0] == layer), key=order), lang)
        if g:
            segs.append(f'L{layer} ' + g)
    rest = sorted((a for a in items if not a[0] and a[3] not in ('farm', 'false', 'chalice', 'boss')), key=order)
    segs += [join_items([a], lang) for a in rest]
    segs += [item_text(a, lang) for a in items if a[3] == 'chalice']
    bosses = [item_text(a, lang) for a in sorted((a for a in items if a[3] == 'boss' and not a[0]), key=order)]
    if bosses:
        segs.append(' / '.join(b for b in bosses if b))
    return ' · '.join(s for s in segs if s)

def fit(items, lang):
    """Drop the least important item (bosses, last first; then the chalice; then the rest) until it fits.
    Returns the text and what was dropped: a set of 'chalice', 'boss', 'other'."""
    items = list(items)
    dropped = set()
    s = compose(items, lang)
    while width(s) > LIMIT and items:
        worst = max(items, key=lambda a: (a[1], a[2]))
        items.remove(worst)
        dropped.add({'chalice': 'chalice', 'boss': 'boss'}.get(worst[3], 'other'))
        s = compose(items, lang)
    return (s or '—'), dropped

data = Path(sys.argv[2]).read_bytes()
assert data[:8] == b'LCCAT003'
count = struct.unpack_from('<I', data, 8)[0]
blob = data[12 + 164 * count:]
entries = []
for n in range(count):
    r = data[12 + 164 * n: 12 + 164 * (n + 1)]
    glyph = r[:8].decode()
    form = struct.unpack_from('<31I', r, 16)
    off, ln = struct.unpack_from('<II', r, 156)
    entries.append((glyph, form[3], blob[off:off + ln].decode('utf-8')))

out = {'langs': [{'code': l, 'name': LANG_NAMES[l]} for l in LANGS], 'limit': LIMIT, 'rows': []}
stats = {l: collections.Counter() for l in LANGS}
widths = {l: [] for l in LANGS}
for glyph, chalice, d in entries:
    parts = analyse(d, chalice)
    items = atoms(parts)
    row = {'g': glyph, 'src': d, 'kinds': [k for k, _ in parts], 's': {}, 'w': {}, 't': {}}
    for lang in LANGS:
        s, dropped = fit(items, lang)
        row['s'][lang] = s
        row['w'][lang] = width(s)
        row['t'][lang] = ''.join(sorted(k[0] for k in dropped))  # b = some boss, c = chalice, o = other
        widths[lang].append(width(s))
        stats[lang]['dropped_any'] += bool(dropped)
        stats[lang]['dropped_boss'] += 'boss' in dropped
        stats[lang]['dropped_other'] += 'other' in dropped
        stats[lang]['empty'] += s == '—'
    out['rows'].append(row)
Path(sys.argv[3]).write_text(json.dumps(out, ensure_ascii=False, separators=(',', ':')), encoding='utf-8')

kinds = collections.Counter(k for r in out['rows'] for k in set(r['kinds']))
out['stats'] = {l: {'median': sorted(widths[l])[count // 2], 'max': max(widths[l]),
                    'trimmed': stats[l]['dropped_any'], 'boss': stats[l]['dropped_boss']} for l in LANGS}
out['parts'] = dict(kinds.most_common())
Path(sys.argv[3]).write_text(json.dumps(out, ensure_ascii=False, separators=(',', ':')), encoding='utf-8')
print('entries', count, '| parts present:', dict(kinds.most_common()))
for lang in LANGS:
    w = sorted(widths[lang])
    st = stats[lang]
    print(f'{lang}: median width {w[len(w)//2]:3d}, max {w[-1]:3d} | trimmed {st["dropped_any"]:4d} '
          f'({st["dropped_any"]/count:.0%}): some boss {st["dropped_boss"]:4d} ({st["dropped_boss"]/count:.0%}), '
          f'tags/loot {st["dropped_other"]:3d} | empty {st["empty"]}')
