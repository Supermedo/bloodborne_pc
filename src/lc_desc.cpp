// SPDX-License-Identifier: GPL-2.0-or-later
// See lc_desc.h. A port of short_desc.py's item_text / join_items / compose / fit, checked against
// it for every glyph in every language by lc_desc_test.cpp.
#include "lc_desc.h"
#include <algorithm>

namespace lc_desc {
namespace {

constexpr uint32_t kGemWordFmg = 2, kGemWordId = 34004;  // "Blood Gem"
constexpr uint32_t kSepFmg = 35, kSepId = 23105;         // an effect line with a decimal: "." or ","
constexpr uint32_t kShapeFmg = 200, kEffectFmg = 35;

std::u16string get(Lookup lookup, void* context, uint32_t fmg, uint32_t id) {
    if(!fmg || !id) return {};
    const char16_t* p = lookup(context, fmg, id);
    return p ? std::u16string(p) : std::u16string();
}
bool digit(char16_t c) { return (c >= u'0' && c <= u'9') || (c >= 0xff10 && c <= 0xff19); }

// Python's str.strip() on whitespace, for the ":" fallback of rune names.
bool space(char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == 0x3000 || c == 0xa0; }

// 'Caryll Rune: "Deep Sea"' / 'カレルの遺稿「瞳」' -> the last quoted name, with its quotes; else the text
// after the first colon; else the whole name.
std::u16string rune_name(const std::u16string& name) {
    struct Quote { char16_t open; const char16_t* close; };
    static const Quote quotes[] = {{u'“', u"”"}, {u'"', u"\""}, {u'「', u"」"}, {u'«', u"»"},
                                   {u'„', u"“”"}, {u'‘', u"’"}, {u'”', u"”"}};
    std::u16string last;
    for(size_t pos = 0; pos < name.size();) {
        bool matched = false;
        for(const auto& q : quotes) {
            if(name[pos] != q.open) continue;
            const size_t close = name.find_first_of(q.close, pos + 1);
            if(close == std::u16string::npos || close == pos + 1) continue;
            last = name.substr(pos, close - pos + 1);
            pos = close + 1;
            matched = true;
            break;
        }
        if(!matched) ++pos;
    }
    if(!last.empty()) return last;
    const size_t colon = name.find_first_of(u":：");
    if(colon != std::u16string::npos) {
        size_t start = colon + 1;
        while(start < name.size() && space(name[start])) ++start;
        size_t end = name.find(u'\n', start);
        if(end == std::u16string::npos) end = name.size();
        if(end > start) return name.substr(start, end - start);
    }
    return name;
}

std::u16string number(uint32_t hundredths, char16_t sep) {
    std::string s = std::to_string(hundredths / 100);
    const uint32_t frac = hundredths % 100;
    if(frac) {
        s += '.';
        s += char('0' + frac / 10);
        if(frac % 10) s += char('0' + frac % 10);
    }
    std::u16string out;
    for(char c : s) out += c == '.' ? sep : char16_t(c);
    return out;
}

// The first [+-]?\d+([.,]\d+)? in `line`, replaced by `with`.
std::u16string replace_number(const std::u16string& line, const std::u16string& with) {
    for(size_t i = 0; i < line.size(); ++i) {
        size_t start = i, j = i;
        if((line[i] == u'+' || line[i] == u'-') && i + 1 < line.size() && digit(line[i + 1])) j = i + 1;
        else if(!digit(line[i])) continue;
        while(j < line.size() && digit(line[j])) ++j;
        if(j + 1 < line.size() && (line[j] == u'.' || line[j] == u',') && digit(line[j + 1])) {
            j += 1;
            while(j < line.size() && digit(line[j])) ++j;
        }
        return line.substr(0, start) + with + line.substr(j);
    }
    return line;
}

char16_t decimal_separator(Lookup lookup, void* context) {
    const std::u16string probe = get(lookup, context, kSepFmg, kSepId);
    for(size_t i = 1; i + 1 < probe.size(); ++i)
        if(probe[i] == u',' && digit(probe[i - 1]) && digit(probe[i + 1])) return u',';
    return u'.';
}

struct Context { const Language& language; Lookup lookup; void* context; char16_t sep; };

std::u16string item_text(const Item& a, const Context& c) {
    switch(a.kind) {
    case kFarm: {
        std::u16string s = c.language.farm;
        if(a.fmg && a.id) s += u": " + get(c.lookup, c.context, a.fmg, a.id);
        return s;
    }
    case kFalse: return c.language.false_depth;
    case kRune:
    case kWeapon: {
        std::u16string name = get(c.lookup, c.context, a.fmg, a.id);
        if(a.kind == kRune) name = rune_name(name);
        if(a.extra) name += u" " + std::u16string(1, char16_t(u'0' + a.extra));
        return name;
    }
    case kGem: {
        if(a.id) {
            std::u16string line = replace_number(get(c.lookup, c.context, kEffectFmg, a.id), u"+" + number(a.value, c.sep));
            while(!line.empty() && (line.back() == u'.' || line.back() == 0x3002)) line.pop_back();
            return line;
        }
        return get(c.lookup, c.context, kGemWordFmg, kGemWordId) + u" " + number(a.value, c.sep) + u"%";
    }
    case kGameTag: return get(c.lookup, c.context, a.fmg, a.id);
    case kOwnTag: return a.extra == 1 ? c.language.rare : a.extra == 2 ? c.language.glitch : std::u16string();
    case kBoss:
    case kChalice: return get(c.lookup, c.context, a.fmg, a.id);
    }
    return {};
}

bool before(const Item* x, const Item* y) {
    return x->priority != y->priority ? x->priority < y->priority : x->order < y->order;
}

// One group's items; gems of the same shape share it: "Triangle: A / B".
std::u16string join_items(const std::vector<const Item*>& items, const Context& c) {
    std::vector<std::u16string> out;
    bool prev_gem = false;
    uint16_t prev_shape = 0;
    for(const Item* a : items) {
        std::u16string t = item_text(*a, c);
        if(t.empty()) continue;
        if(a->kind == kGem) {
            if(!out.empty() && prev_gem && prev_shape == a->extra) {
                out.back() += u" / " + t;
                continue;
            }
            if(a->extra) t = get(c.lookup, c.context, kShapeFmg, a->extra) + u": " + t;
            prev_gem = true;
            prev_shape = a->extra;
        } else {
            prev_gem = false;
            prev_shape = 0;
        }
        out.push_back(t);
    }
    std::u16string s;
    for(size_t i = 0; i < out.size(); ++i) s += (i ? c.language.comma : u"") + out[i];
    return s;
}

std::u16string layout(const std::vector<const Item*>& items, const Context& c) {
    std::vector<std::u16string> segs;
    for(const Item* a : items)
        if(a->kind == kFarm || a->kind == kFalse) segs.push_back(item_text(*a, c));
    for(uint8_t layer = 1; layer <= 4; ++layer) {
        std::vector<const Item*> group;
        for(const Item* a : items)
            if(a->layer == layer) group.push_back(a);
        std::stable_sort(group.begin(), group.end(), before);
        const std::u16string g = join_items(group, c);
        if(!g.empty()) segs.push_back(u"L" + std::u16string(1, char16_t(u'0' + layer)) + u" " + g);
    }
    std::vector<const Item*> rest, bosses;
    for(const Item* a : items) {
        if(a->layer) continue;
        if(a->kind == kBoss) bosses.push_back(a);
        else if(a->kind != kFarm && a->kind != kFalse && a->kind != kChalice) rest.push_back(a);
    }
    std::stable_sort(rest.begin(), rest.end(), before);
    for(const Item* a : rest) segs.push_back(join_items({a}, c));
    for(const Item* a : items)
        if(a->kind == kChalice) segs.push_back(item_text(*a, c));
    std::stable_sort(bosses.begin(), bosses.end(), before);
    std::u16string names;
    bool any = false;
    for(const Item* a : bosses) {
        const std::u16string t = item_text(*a, c);
        if(t.empty()) continue;
        names += (any ? u" / " : u"") + t;
        any = true;
    }
    if(!bosses.empty()) segs.push_back(names);
    std::u16string s;
    bool first = true;
    for(const auto& seg : segs) {
        if(seg.empty()) continue;
        s += (first ? u"" : u" · ") + seg;
        first = false;
    }
    return s;
}

bool wide(uint32_t c) {
    return (c >= 0x1100 && c <= 0x115f) || c == 0x2329 || c == 0x232a || (c >= 0x2e80 && c <= 0x303e) ||
           (c >= 0x3041 && c <= 0x33ff) || (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) ||
           (c >= 0xa000 && c <= 0xa4cf) || (c >= 0xa960 && c <= 0xa97f) || (c >= 0xac00 && c <= 0xd7a3) ||
           (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe10 && c <= 0xfe19) || (c >= 0xfe30 && c <= 0xfe6f) ||
           (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x20000 && c <= 0x3fffd);
}

}  // namespace

size_t width(const std::u16string& text) {
    size_t w = 0;
    for(size_t i = 0; i < text.size(); ++i) {
        uint32_t c = text[i];
        if(c >= 0xd800 && c < 0xdc00 && i + 1 < text.size()) c = 0x10000 + ((c - 0xd800) << 10) + (text[++i] - 0xdc00);
        w += wide(c) ? 2 : 1;
    }
    return w;
}

uint32_t probe_hash(const std::vector<std::pair<uint32_t, uint32_t>>& probes, Lookup lookup, void* context) {
    uint32_t h = 0x811c9dc5;
    for(const auto& p : probes)
        for(char16_t u : get(lookup, context, p.first, p.second)) h = (h ^ u) * 0x01000193u;
    return h;
}

const Language& detect(const std::vector<Language>& languages, uint32_t hash) {
    for(const auto& l : languages)
        if(l.probe == hash) return l;
    return languages.front();
}

std::u16string compose(const Item* items, size_t count, const Language& language, Lookup lookup, void* context,
                       size_t limit) {
    const Context c{language, lookup, context, decimal_separator(lookup, context)};
    std::vector<const Item*> left;
    for(size_t i = 0; i < count; ++i) left.push_back(items + i);
    std::u16string s = layout(left, c);
    while(width(s) > limit && !left.empty()) {
        // the least important: highest (priority, order)
        auto worst = std::max_element(left.begin(), left.end(), before);
        left.erase(worst);
        s = layout(left, c);
    }
    return s.empty() ? std::u16string(u"—") : s;
}

std::u16string utf16(const char* s, size_t n) {
    std::u16string out;
    for(size_t i = 0; i < n;) {
        uint32_t c = static_cast<unsigned char>(s[i]);
        int extra = 0;
        if(c >= 0xf0) { c &= 7; extra = 3; } else if(c >= 0xe0) { c &= 15; extra = 2; } else if(c >= 0xc0) { c &= 31; extra = 1; }
        ++i;
        for(int k = 0; k < extra && i < n; ++k, ++i) c = (c << 6) | (static_cast<unsigned char>(s[i]) & 63);
        if(c >= 0x10000) {
            c -= 0x10000;
            out += static_cast<char16_t>(0xd800 + (c >> 10));
            out += static_cast<char16_t>(0xdc00 + (c & 1023));
        } else {
            out += static_cast<char16_t>(c);
        }
    }
    return out;
}

}  // namespace lc_desc
