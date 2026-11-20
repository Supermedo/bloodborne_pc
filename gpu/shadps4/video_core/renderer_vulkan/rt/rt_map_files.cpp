// SPDX-License-Identifier: GPL-2.0-or-later
#include "rt_map_files.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <miniz.h>

#include "rt_geo_cache.h"

namespace Vulkan::Rt::MapFiles {
namespace {

// rt_geo_cache.h GeoMeshAlphaTest, GeoMeshBlend, GeoMeshMoon, GeoMeshSky.
constexpr uint32_t GeoFlagAlphaTest = 1, GeoFlagBlend = 2, GeoFlagMoon = 4, GeoFlagSky = 8;

// Bounds-checked little-endian reads; `ok` drops to false on the first read past the end.
struct Reader {
    const uint8_t* b;
    size_t n;
    bool ok = true;
    template <typename T>
    T At(size_t o) {
        T v{};
        if (o > n || n - o < sizeof(T)) {
            ok = false;
            return v;
        }
        std::memcpy(&v, b + o, sizeof(T));
        return v;
    }
    uint32_t BE32(size_t o) {
        const uint32_t v = At<uint32_t>(o);
        return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
    }
    bool Has(size_t o, size_t len) const {
        return o <= n && n - o >= len;
    }
    // A zero-terminated name, narrowed to bytes: UTF-16 code units above 0x7F and Shift-JIS
    // double-byte characters become '?' (only ASCII is ever compared).
    std::string Str(size_t o, bool wide) {
        std::string s;
        if (wide) {
            for (size_t p = o; Has(p, 2) && s.size() < 1024; p += 2) {
                const uint16_t c = At<uint16_t>(p);
                if (!c) {
                    return s;
                }
                s.push_back(c < 0x80 ? char(c) : '?');
            }
        } else {
            for (size_t p = o; p < n && s.size() < 1024; ++p) {
                const uint8_t c = b[p];
                if (!c) {
                    return s;
                }
                if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
                    s.push_back('?');
                    ++p;
                    continue;
                }
                s.push_back(c < 0x80 ? char(c) : '?');
            }
        }
        ok = false;
        return s;
    }
};

std::string Lower(std::string s) {
    for (char& c : s) {
        c = char(std::tolower(uint8_t(c)));
    }
    return s;
}

std::string BaseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// --- matrices (double, as the Python reference computes them)
using M34 = std::array<std::array<double, 4>, 3>;

M34 Mul(const M34& a, const M34& b) {
    M34 out{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            double v = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
            if (j == 3) {
                v += a[i][3];
            }
            out[i][j] = v;
        }
    }
    return out;
}

// Scale, then rotations about X, Z, Y (radians), then translation: mapcache._compose 'XZY'.
M34 Compose(const double t[3], const double r[3], const double s[3]) {
    M34 m{{{s[0], 0, 0, 0}, {0, s[1], 0, 0}, {0, 0, s[2], 0}}};
    const auto rx = [](double a) {
        const double c = std::cos(a), n = std::sin(a);
        return M34{{{1, 0, 0, 0}, {0, c, -n, 0}, {0, n, c, 0}}};
    };
    const auto ry = [](double a) {
        const double c = std::cos(a), n = std::sin(a);
        return M34{{{c, 0, n, 0}, {0, 1, 0, 0}, {-n, 0, c, 0}}};
    };
    const auto rz = [](double a) {
        const double c = std::cos(a), n = std::sin(a);
        return M34{{{c, -n, 0, 0}, {n, c, 0, 0}, {0, 0, 1, 0}}};
    };
    m = Mul(rx(r[0]), m);
    m = Mul(rz(r[2]), m);
    m = Mul(ry(r[1]), m);
    m[0][3] += t[0];
    m[1][3] += t[1];
    m[2][3] += t[2];
    return m;
}

bool IsIdentity(const M34& m) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (std::fabs(m[i][j] - (i == j ? 1.0 : 0.0)) > 1e-6) {
                return false;
            }
        }
    }
    return true;
}

// --- MTD alpha (tools/mapcache/study/mtd_params.py + alpha.py)
const uint8_t* Find(const uint8_t* b, size_t n, size_t from, const void* what, size_t len) {
    if (from >= n || len > n - from) {
        return nullptr;
    }
    const uint8_t* end = b + n - len + 1;
    for (const uint8_t* p = b + from; p < end; ++p) {
        p = static_cast<const uint8_t*>(std::memchr(p, *static_cast<const uint8_t*>(what),
                                                    size_t(end - p)));
        if (!p) {
            return nullptr;
        }
        if (std::memcmp(p, what, len) == 0) {
            return p;
        }
    }
    return nullptr;
}

// The first int of an MTD parameter, or false when it is absent.
bool MtdParam(const uint8_t* b, size_t n, const char* name, int32_t& first) {
    std::string key = name;
    key.push_back(char(0xA3));
    const uint8_t* i = Find(b, n, 0, key.data(), key.size());
    if (!i) {
        return false;
    }
    const uint8_t marker = 0xC5;
    const uint8_t* j = Find(b, n, size_t(i - b) + key.size() - 1 + 8, &marker, 1);
    if (!j) {
        return false;
    }
    Reader r{b, n};
    const int32_t count = r.At<int32_t>(size_t(j - b) + 4);
    if (count <= 0) {
        return false;
    }
    first = r.At<int32_t>(size_t(j - b) + 8);
    return r.ok;
}

bool Matches(const std::string& s, const char* stem) {
    // stem followed by '_', '.' or the end
    for (size_t at = s.find(stem); at != std::string::npos; at = s.find(stem, at + 1)) {
        const size_t e = at + std::strlen(stem);
        if (e == s.size() || s[e] == '_' || s[e] == '.') {
            return true;
        }
    }
    return false;
}

} // namespace

bool ReadDcx(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    std::vector<uint8_t> raw;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0) {
        raw.resize(size_t(size));
    }
    const bool read = size > 0 && std::fread(raw.data(), 1, raw.size(), f) == raw.size();
    std::fclose(f);
    if (!read) {
        return false;
    }
    if (raw.size() < 4 || std::memcmp(raw.data(), "DCX\0", 4) != 0) {
        out = std::move(raw);
        return true;
    }
    Reader r{raw.data(), raw.size()};
    if (!r.Has(0x4C, 0) || std::memcmp(raw.data() + 0x18, "DCS\0", 4) != 0 ||
        std::memcmp(raw.data() + 0x24, "DCP\0", 4) != 0 ||
        std::memcmp(raw.data() + 0x28, "DFLT", 4) != 0 ||
        std::memcmp(raw.data() + 0x44, "DCA\0", 4) != 0) {
        return false;
    }
    const uint32_t usize = r.BE32(0x1C), csize = r.BE32(0x20), ca = r.BE32(0x48);
    const size_t start = 0x44 + size_t(ca);
    if (!r.ok || !r.Has(start, csize) || usize > (1u << 30)) {
        return false;
    }
    out.resize(usize);
    mz_ulong len = usize;
    if (mz_uncompress(out.data(), &len, raw.data() + start, csize) != MZ_OK || len != usize) {
        out.clear();
        return false;
    }
    return true;
}

bool MtdTable::Load(const std::string& dvdroot) {
    rows.clear();
    std::vector<uint8_t> b;
    if (!ReadDcx(dvdroot + "/mtd/allmaterialbnd.mtdbnd.dcx", b) || b.size() < 0x40 ||
        std::memcmp(b.data(), "BND4", 4) != 0) {
        return false;
    }
    Reader r{b.data(), b.size()};
    const int32_t count = r.At<int32_t>(0x0C);
    const int64_t fhs = r.At<int64_t>(0x20);
    const bool wide = b[0x30] != 0;
    const uint8_t fmt = b[0x31];
    if (count < 0 || count > 100000 || fhs <= 0 || fhs > 0x100) {
        return false;
    }
    for (int32_t i = 0; i < count && r.ok; ++i) {
        const size_t o = 0x40 + size_t(fhs) * size_t(i);
        const int64_t csize = r.At<int64_t>(o + 8);
        size_t p = o + 16;
        if (fmt & 0x24) {
            p += 8;
        }
        const int64_t data_at = fhs == 0x2C ? r.At<int64_t>(p) : r.At<int32_t>(p);
        p += fhs == 0x2C ? 8 : 4;
        const int32_t name_at = r.At<int32_t>(p + 4);
        const std::string name = Lower(BaseName(r.Str(size_t(name_at), wide)));
        if (!r.ok || data_at < 0 || csize < 0 || !r.Has(size_t(data_at), size_t(csize))) {
            return false;
        }
        const uint8_t* d = b.data() + data_at;
        int32_t blend = 0, ref = 0;
        int8_t kind = -1; // no g_BlendMode: the name decides
        if (MtdParam(d, size_t(csize), "g_BlendMode", blend)) {
            if (!MtdParam(d, size_t(csize), "g_AlphaRef", ref)) {
                ref = 0;
            }
            kind = (blend == 0 || blend == 1) && (ref > 0 || blend == 1) ? 1
                   : blend == 2                                       ? 2
                   : blend >= 3                                       ? 3
                                                                      : 0;
        }
        rows.emplace_back(name, kind);
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end(),
                           [](const auto& a, const auto& c) { return a.first == c.first; }),
               rows.end());
    return !rows.empty();
}

uint32_t MtdTable::MeshFlags(const std::string& material, const std::string& mtd_path) const {
    const std::string base = Lower(BaseName(mtd_path));
    const auto it = std::lower_bound(rows.begin(), rows.end(), base,
                                     [](const auto& row, const std::string& k) { return row.first < k; });
    int kind = it != rows.end() && it->first == base ? it->second : -1;
    if (kind < 0) {
        kind = Matches(base, "_e") || base.find("edge") != std::string::npos   ? 1
               : Matches(base, "_al") || base.find("_alp") != std::string::npos ? 2
                                                                                 : 0;
    }
    uint32_t flags = kind == 1 ? GeoFlagAlphaTest : kind != 0 ? GeoFlagBlend : 0u;
    const std::string name = Lower(material + " " + mtd_path);
    if (name.find("moon") != std::string::npos || name.find("sun") != std::string::npos) {
        flags |= GeoFlagMoon;
    }
    if (name.find("sky") != std::string::npos) {
        flags |= GeoFlagSky;
    }
    return flags;
}

bool ReadMsb(const std::vector<uint8_t>& data, std::vector<MsbPart>& parts) {
    parts.clear();
    Reader r{data.data(), data.size()};
    if (data.size() < 0x10 || std::memcmp(data.data(), "MSB ", 4) != 0) {
        return false;
    }
    std::vector<std::string> models;
    std::vector<int64_t> part_at;
    size_t o = 0x10;
    for (int guard = 0; o && guard < 64 && r.ok; ++guard) {
        const int32_t count = r.At<int32_t>(o + 4);
        const int64_t name_at = r.At<int64_t>(o + 8);
        if (count <= 0 || count > 1000000) {
            return false;
        }
        const std::string pname = r.Str(size_t(name_at), true);
        std::vector<int64_t> offs(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i) {
            offs[size_t(i)] = r.At<int64_t>(o + 16 + 8 * size_t(i));
        }
        const int64_t next = offs.back();
        offs.pop_back();
        if (pname == "MODEL_PARAM_ST") {
            for (const int64_t e : offs) {
                models.push_back(r.Str(size_t(e + r.At<int64_t>(size_t(e))), true));
            }
        } else if (pname == "PARTS_PARAM_ST") {
            part_at = offs;
        }
        o = size_t(next);
    }
    if (!r.ok) {
        return false;
    }
    for (const int64_t e64 : part_at) {
        const size_t e = size_t(e64);
        MsbPart p;
        p.type = r.At<int32_t>(e + 20);
        const int32_t model = r.At<int32_t>(e + 28);
        for (int a = 0; a < 3; ++a) {
            p.pos[a] = r.At<float>(e + 40 + 4 * size_t(a));
            p.rot[a] = r.At<float>(e + 52 + 4 * size_t(a));
            p.scale[a] = r.At<float>(e + 64 + 4 * size_t(a));
        }
        for (size_t k = 0; k < 8; ++k) {
            p.draw_groups[k] = r.At<uint32_t>(e + 0x4C + 4 * k);
            p.display_groups[k] = r.At<uint32_t>(e + 0x6C + 4 * k);
        }
        p.entity_id = r.At<int32_t>(e + size_t(r.At<int64_t>(e + 0xB0)));
        if (model >= 0 && size_t(model) < models.size()) {
            p.model = models[size_t(model)];
        }
        if (!r.ok) {
            return false;
        }
        parts.push_back(std::move(p));
    }
    return true;
}

std::array<float, 12> PartMatrix(const float pos[3], const float rot_deg[3], const float scale[3]) {
    constexpr double Rad = 3.14159265358979323846 / 180.0;
    const double t[3] = {pos[0], pos[1], pos[2]};
    const double rr[3] = {rot_deg[0] * Rad, rot_deg[1] * Rad, rot_deg[2] * Rad};
    const double s[3] = {scale[0], scale[1], scale[2]};
    const M34 m = Compose(t, rr, s);
    std::array<float, 12> out{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[size_t(i * 4 + j)] = float(m[i][j]);
        }
    }
    return out;
}

std::string ModelPath(const std::string& dvdroot, const std::string& map_id,
                      const std::string& model) {
    return dvdroot + "/map/" + map_id + "/" + map_id + "_" + model.substr(1) + ".flver.dcx";
}

namespace {

constexpr uint32_t FsLod1 = 0x01000000, FsLod2 = 0x02000000, FsEdge = 0x40000000,
                   FsMotionBlur = 0x80000000;

int LayoutTypeSize(uint32_t type) {
    switch (type) {
    case 0x01: return 8;
    case 0x02: return 12;
    case 0x03: return 16;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x15: case 0x2F: return 4;
    case 0x16: case 0x18: case 0x1A: case 0x2E: return 8;
    default: return -1;
    }
}

struct FaceSet {
    uint32_t flags;
    bool strip;
    uint32_t index_size, count, at;
};

// mapcache.triangulate; false on an index past the vertices or a broken list.
bool Triangulate(Reader& r, const FaceSet& fs, uint32_t vcount, std::vector<uint32_t>& out) {
    out.clear();
    const uint32_t restart = fs.index_size == 16 ? 0xFFFFu : 0xFFFFFFFFu;
    const auto idx = [&](uint32_t i) {
        return fs.index_size == 16 ? uint32_t(r.At<uint16_t>(fs.at + 2 * size_t(i)))
                                   : r.At<uint32_t>(fs.at + 4 * size_t(i));
    };
    if (fs.strip) {
        bool flip = false;
        for (uint32_t i = 0; i + 2 < fs.count; ++i) {
            const uint32_t a = idx(i), b = idx(i + 1), c = idx(i + 2);
            if (a == restart || b == restart || c == restart) {
                flip = false;
                continue;
            }
            if (a != b && b != c && a != c) {
                if (flip) {
                    out.insert(out.end(), {c, b, a});
                } else {
                    out.insert(out.end(), {a, b, c});
                }
            }
            flip = !flip;
        }
    } else {
        if (fs.count % 3) {
            return false;
        }
        for (uint32_t i = 0; i < fs.count; i += 3) {
            const uint32_t a = idx(i), b = idx(i + 1), c = idx(i + 2);
            if (a != b && b != c && a != c) {
                out.insert(out.end(), {a, b, c});
            }
        }
    }
    for (const uint32_t v : out) {
        if (v >= vcount) {
            return false;
        }
    }
    return r.ok;
}

} // namespace

bool ReadModel(const std::vector<uint8_t>& data, const MtdTable& mtd, bool want_soup,
               std::vector<ModelMesh>& out) {
    out.clear();
    Reader r{data.data(), data.size()};
    if (data.size() < 0x80 || std::memcmp(data.data(), "FLVER\0", 6) != 0 ||
        std::memcmp(data.data() + 6, "L\0", 2) != 0) {
        return false;
    }
    const int32_t ver = r.At<int32_t>(8), doff = r.At<int32_t>(12), ndum = r.At<int32_t>(20),
                  nmat = r.At<int32_t>(24), nbone = r.At<int32_t>(28), nmesh = r.At<int32_t>(32),
                  nvb = r.At<int32_t>(36);
    if ((ver >> 16) != 2 || ndum < 0 || nmat < 0 || nbone < 0 || nmesh < 0 || nvb < 0 ||
        nbone > 4096 || nmesh > 65536) {
        return false;
    }
    const uint32_t hdr_index_size = data[0x48];
    const bool wide = data[0x49] != 0;
    const int32_t nfs = r.At<int32_t>(0x50), nlay = r.At<int32_t>(0x54);
    size_t o = 0x80 + 0x40 * size_t(ndum);
    std::vector<std::pair<std::string, std::string>> mats(static_cast<size_t>(nmat));
    for (int32_t i = 0; i < nmat; ++i) {
        mats[size_t(i)] = {r.Str(size_t(r.At<int32_t>(o + 0x20 * size_t(i))), wide),
                           r.Str(size_t(r.At<int32_t>(o + 0x20 * size_t(i) + 4)), wide)};
    }
    o += 0x20 * size_t(nmat);
    std::vector<M34> local(static_cast<size_t>(nbone));
    std::vector<int16_t> parent(static_cast<size_t>(nbone));
    for (int32_t i = 0; i < nbone; ++i) {
        const size_t e = o + 0x80 * size_t(i);
        double t[3], rr[3], s[3];
        for (int a = 0; a < 3; ++a) {
            t[a] = r.At<float>(e + 4 * size_t(a));
            rr[a] = r.At<float>(e + 16 + 4 * size_t(a));
            s[a] = r.At<float>(e + 32 + 4 * size_t(a));
        }
        parent[size_t(i)] = r.At<int16_t>(e + 28);
        local[size_t(i)] = Compose(t, rr, s);
    }
    o += 0x80 * size_t(nbone);
    std::vector<M34> world(static_cast<size_t>(nbone));
    for (int32_t i = 0; i < nbone; ++i) {
        M34 m = local[size_t(i)];
        int p = parent[size_t(i)];
        for (int guard = 0; p >= 0 && p < nbone && guard < 256; ++guard) {
            m = Mul(local[size_t(p)], m);
            p = parent[size_t(p)];
        }
        world[size_t(i)] = m;
    }
    const size_t mesh_at = o;
    o += 0x30 * size_t(nmesh);
    const size_t fs_at = o;
    o += 0x20 * size_t(nfs);
    const size_t vb_at = o;
    o += 0x20 * size_t(nvb);
    const size_t lay_at = o;
    if (!r.ok) {
        return false;
    }
    std::vector<FaceSet> sets;
    std::vector<uint32_t> tris, lod0;
    std::vector<int32_t> remap;
    for (int32_t mi = 0; mi < nmesh; ++mi) {
        const size_t h = mesh_at + 0x30 * size_t(mi);
        const bool dynamic = r.At<uint8_t>(h) != 0;
        const int32_t mat_i = r.At<int32_t>(h + 4), def_bone = r.At<int32_t>(h + 16),
                      fs_cnt = r.At<int32_t>(h + 32), fs_off = r.At<int32_t>(h + 36),
                      vb_cnt = r.At<int32_t>(h + 40), vb_off = r.At<int32_t>(h + 44);
        if (!r.ok) {
            r.ok = true; // this mesh only: mapcache.read_flver sets its error
            continue;
        }
        // The checks of mapcache.read_flver: a mesh failing one is left out.
        if (vb_cnt != 1 || dynamic) {
            continue;
        }
        const int32_t vbi = r.At<int32_t>(size_t(vb_off));
        if (vbi < 0 || vbi >= nvb) {
            continue;
        }
        const size_t vh = vb_at + 0x20 * size_t(vbi);
        const int32_t lay_i = r.At<int32_t>(vh + 4), vsize = r.At<int32_t>(vh + 8),
                      vcount = r.At<int32_t>(vh + 12), blen = r.At<int32_t>(vh + 24),
                      boff = r.At<int32_t>(vh + 28);
        if (lay_i < 0 || lay_i >= nlay || vsize <= 0 || vcount < 0) {
            continue;
        }
        const size_t lh = lay_at + 0x10 * size_t(lay_i);
        const int32_t members = r.At<int32_t>(lh), moff = r.At<int32_t>(lh + 12);
        int32_t end = 0, positions = 0, poff = 0;
        uint32_t ptype = 0;
        bool layout_ok = members >= 0 && members < 64;
        for (int32_t j = 0; layout_ok && j < members; ++j) {
            const size_t m = size_t(moff) + 0x14 * size_t(j);
            const int32_t soff = r.At<int32_t>(m + 4);
            const uint32_t type = r.At<uint32_t>(m + 8), sem = r.At<uint32_t>(m + 12);
            const int size = LayoutTypeSize(type);
            if (size < 0 || soff != end) {
                layout_ok = false;
                break;
            }
            end = soff + size;
            if (sem == 0) {
                ++positions;
                poff = soff;
                ptype = type;
            }
        }
        if (!r.ok) {
            r.ok = true; // this mesh only: mapcache.read_flver sets its error
            continue;
        }
        if (!layout_ok || end != vsize || int64_t(blen) != int64_t(vsize) * vcount ||
            positions != 1 || (ptype != 0x02 && ptype != 0x03)) {
            continue;
        }
        const M34* node = nullptr;
        if (nbone == 1) {
            node = &world[0];
        } else if (def_bone >= 0 && def_bone < nbone) {
            node = &world[size_t(def_bone)];
        } else if (nbone != 0) {
            continue;
        }
        if (node && IsIdentity(*node)) {
            node = nullptr;
        }
        if (vsize % 4 || poff % 4 || !r.Has(size_t(doff) + size_t(boff), size_t(blen))) {
            continue;
        }
        sets.clear();
        bool sets_ok = fs_cnt >= 0 && fs_cnt < 64;
        int lod0_count = 0;
        for (int32_t k = 0; sets_ok && k < fs_cnt; ++k) {
            const int32_t id = r.At<int32_t>(size_t(fs_off) + 4 * size_t(k));
            if (id < 0 || id >= nfs) {
                sets_ok = false;
                break;
            }
            const size_t f = fs_at + 0x20 * size_t(id);
            FaceSet fs{r.At<uint32_t>(f), r.At<uint8_t>(f + 4) != 0, uint32_t(r.At<int32_t>(f + 24)),
                       uint32_t(r.At<int32_t>(f + 8)), 0};
            const int32_t ioff = r.At<int32_t>(f + 12), ilen = r.At<int32_t>(f + 16);
            if (!fs.index_size) {
                fs.index_size = hdr_index_size;
            }
            if ((fs.index_size != 16 && fs.index_size != 32) ||
                int64_t(ilen) != int64_t(fs.count) * fs.index_size / 8 || (fs.flags & FsEdge) ||
                !r.Has(size_t(doff) + size_t(ioff), size_t(ilen))) {
                sets_ok = false;
                break;
            }
            fs.at = uint32_t(doff + ioff);
            lod0_count += fs.flags == 0 ? 1 : 0;
            sets.push_back(fs);
        }
        if (!r.ok) {
            r.ok = true; // this mesh only: mapcache.read_flver sets its error
            continue;
        }
        if (!sets_ok || lod0_count != 1) {
            continue;
        }
        const FaceSet* zero = nullptr;
        const FaceSet* best = nullptr;
        int best_level = -1;
        ModelMesh mesh;
        for (const FaceSet& fs : sets) {
            zero = fs.flags == 0 ? &fs : zero;
            if (fs.flags & ~(FsLod1 | FsLod2)) {
                continue;
            }
            const int level = fs.flags & FsLod2 ? 2 : fs.flags & FsLod1 ? 1 : 0;
            mesh.lods[level] = fs.count;
            if (!(fs.flags & (FsMotionBlur | FsEdge)) && level > best_level) {
                best_level = level;
                best = &fs;
            }
        }
        if (!Triangulate(r, *zero, uint32_t(vcount), lod0) || !best ||
            !Triangulate(r, *best, uint32_t(vcount), tris) || tris.empty()) {
            continue;
        }
        // The used vertices, in the node's space; the box over them.
        const size_t base = size_t(doff) + size_t(boff) + size_t(poff);
        const auto vertex = [&](uint32_t v, float out3[3]) {
            const size_t at = base + size_t(v) * size_t(vsize);
            const double x = r.At<float>(at), y = r.At<float>(at + 4), z = r.At<float>(at + 8);
            if (node) {
                const M34& m = *node;
                out3[0] = float(m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3]);
                out3[1] = float(m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3]);
                out3[2] = float(m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3]);
            } else {
                out3[0] = float(x);
                out3[1] = float(y);
                out3[2] = float(z);
            }
        };
        for (int a = 0; a < 3; ++a) {
            mesh.box[a] = 3.0e38f;
            mesh.box[a + 3] = -3.0e38f;
        }
        remap.assign(size_t(vcount), 0);
        for (const uint32_t v : tris) {
            remap[v] = 1;
        }
        float p[3];
        for (int32_t v = 0; v < vcount; ++v) {
            if (!remap[size_t(v)]) {
                continue;
            }
            vertex(uint32_t(v), p);
            for (int a = 0; a < 3; ++a) {
                mesh.box[a] = std::min(mesh.box[a], p[a]);
                mesh.box[a + 3] = std::max(mesh.box[a + 3], p[a]);
            }
        }
        if (want_soup) {
            mesh.soup.resize(tris.size() * 3);
            for (size_t i = 0; i < tris.size(); ++i) {
                vertex(tris[i], mesh.soup.data() + i * 3);
            }
        }
        if (!r.ok) {
            r.ok = true; // this mesh only: mapcache.read_flver sets its error
            continue;
        }
        const auto& mat = mat_i >= 0 && mat_i < nmat ? mats[size_t(mat_i)]
                                                     : std::pair<std::string, std::string>{"?", "?"};
        mesh.flags = mtd.MeshFlags(mat.first, mat.second);
        mesh.index_count = uint32_t(tris.size());
        out.push_back(std::move(mesh));
    }
    return true;
}

} // namespace Vulkan::Rt::MapFiles

namespace Vulkan::Rt {

// Game mode of a GeoMap (rt_geo_cache.h). The scan thread reads the MSB and every map-piece model
// of the map (a few worker threads, the files are zlib): the tables of build_cache.py without
// the triangles. The loader thread reads a model again when a BLAS build asks for its triangles.
struct GeoGame {
    std::string dvdroot, map_id;
    MapFiles::MtdTable mtd;

    std::thread scan;
    std::atomic<int> scan_state{0}; // 0 running, 1 done, -1 failed
    std::atomic<bool> quit{false};
    std::vector<GeoModel> models;
    std::vector<GeoMesh> meshes;
    std::vector<GeoPlace> places;
    std::vector<GeoGroups> collisions;
    std::vector<uint32_t> mesh_model;
    std::string report;

    // Loader: models asked for, the soups read (by mesh), models that failed.
    std::mutex mu;
    std::condition_variable wake;
    std::thread loader;
    std::deque<uint32_t> queue;
    std::unordered_set<uint32_t> queued, failed;
    std::unordered_map<uint32_t, std::vector<float>> ready;
    std::deque<uint32_t> ready_order;
    size_t ready_bytes = 0;
    std::vector<GeoModel> loader_models; // a copy for the loader thread

    ~GeoGame() {
        {
            std::lock_guard lock(mu);
            quit = true;
        }
        wake.notify_all();
        if (scan.joinable()) {
            scan.join();
        }
        if (loader.joinable()) {
            loader.join();
        }
    }

    static std::string ModelName(const GeoModel& m) {
        return std::string(m.name, strnlen(m.name, sizeof(m.name)));
    }

    void Scan() {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<uint8_t> bytes;
        std::vector<MapFiles::MsbPart> parts;
        if (!mtd.Load(dvdroot)) {
            report = "material table " + dvdroot + "/mtd/allmaterialbnd.mtdbnd.dcx not read";
            scan_state = -1;
            return;
        }
        if (!MapFiles::ReadDcx(dvdroot + "/map/MapStudio/" + map_id + ".msb.dcx", bytes) ||
            !MapFiles::ReadMsb(bytes, parts)) {
            report = "map file " + map_id + ".msb.dcx not read";
            scan_state = -1;
            return;
        }
        std::set<std::string> names;
        for (const auto& p : parts) {
            if (p.type == 0 && !p.model.empty()) {
                names.insert(p.model);
            }
        }
        const std::vector<std::string> list(names.begin(), names.end());
        std::vector<std::vector<MapFiles::ModelMesh>> read(list.size());
        std::vector<uint8_t> read_ok(list.size(), 0);
        std::atomic<size_t> next{0};
        const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 4u, 1u, 4u);
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t) {
            pool.emplace_back([&] {
                std::vector<uint8_t> data;
                for (size_t i = next++; !quit && i < list.size(); i = next++) {
                    read_ok[i] =
                        MapFiles::ReadDcx(MapFiles::ModelPath(dvdroot, map_id, list[i]), data) &&
                        MapFiles::ReadModel(data, mtd, false, read[i]);
                }
            });
        }
        for (auto& t : pool) {
            t.join();
        }
        if (quit) {
            scan_state = -1;
            return;
        }
        std::unordered_map<std::string, uint32_t> index;
        std::vector<std::array<float, 6>> model_box;
        uint32_t failures = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            failures += read_ok[i] ? 0u : 1u;
            if (!read_ok[i] || read[i].empty()) {
                continue;
            }
            GeoModel model{};
            std::memcpy(model.name, list[i].data(), std::min<size_t>(list[i].size(), 8));
            model.first_mesh = uint32_t(meshes.size());
            model.mesh_count = uint32_t(read[i].size());
            std::array<float, 6> box{3.0e38f, 3.0e38f, 3.0e38f, -3.0e38f, -3.0e38f, -3.0e38f};
            for (const auto& m : read[i]) {
                GeoMesh mesh{};
                mesh.index_count = m.index_count;
                mesh.flags = m.flags;
                mesh.lod0_indices = m.lods[0];
                mesh.lod1_indices = m.lods[1];
                mesh.lod2_indices = m.lods[2];
                std::memcpy(mesh.box, m.box, sizeof(mesh.box));
                for (size_t a = 0; a < 3; ++a) {
                    box[a] = std::min(box[a], m.box[a]);
                    box[a + 3] = std::max(box[a + 3], m.box[a + 3]);
                }
                meshes.push_back(mesh);
                mesh_model.push_back(uint32_t(models.size()));
            }
            index.emplace(list[i], uint32_t(models.size()));
            models.push_back(model);
            model_box.push_back(box);
        }
        for (const auto& p : parts) {
            if (p.type != 0) {
                continue;
            }
            const auto it = index.find(p.model);
            if (it == index.end()) {
                continue;
            }
            GeoPlace place{};
            place.model = it->second;
            const auto m = MapFiles::PartMatrix(p.pos, p.rot, p.scale);
            std::memcpy(place.m, m.data(), sizeof(place.m));
            const GeoModel& model = models[it->second];
            bool sky = false;
            for (uint32_t k = 0; k < model.mesh_count; ++k) {
                sky = sky || (meshes[model.first_mesh + k].flags & (GeoMeshSky | GeoMeshMoon));
            }
            place.flags = (p.entity_id != -1 && p.entity_id != 0 ? GeoPlaceEvent : 0u) |
                          (sky ? GeoPlaceSky : 0u);
            const auto& b = model_box[it->second];
            for (size_t a = 0; a < 3; ++a) {
                place.box[a] = 3.0e38f;
                place.box[a + 3] = -3.0e38f;
            }
            for (int c = 0; c < 8; ++c) {
                const double x = b[c & 1 ? 3 : 0], y = b[c & 2 ? 4 : 1], z = b[c & 4 ? 5 : 2];
                for (size_t a = 0; a < 3; ++a) {
                    const float w = float(double(m[a * 4]) * x + double(m[a * 4 + 1]) * y +
                                          double(m[a * 4 + 2]) * z + double(m[a * 4 + 3]));
                    place.box[a] = std::min(place.box[a], w);
                    place.box[a + 3] = std::max(place.box[a + 3], w);
                }
            }
            std::memcpy(place.groups.data(), p.draw_groups.data(), sizeof(place.groups));
            places.push_back(place);
        }
        std::set<GeoGroups> sets;
        for (const auto& p : parts) {
            bool any = false;
            for (const uint32_t g : p.display_groups) {
                any = any || g != 0;
            }
            if (p.type == 5 && any) {
                sets.insert(p.display_groups);
            }
        }
        collisions.assign(sets.begin(), sets.end());
        const double sec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        char line[256];
        std::snprintf(line, sizeof(line),
                      "%zu models, %zu meshes, %zu placements, %zu collision group sets, %u models "
                      "not read, %.1f s on %u threads",
                      models.size(), meshes.size(), places.size(), collisions.size(), failures, sec,
                      threads);
        report = line;
        loader_models = models;
        if (places.empty()) {
            scan_state = -1;
            return;
        }
        loader = std::thread([this] { Load(); });
        scan_state = 1;
    }

    // The loader thread: reads each model asked for and keeps its meshes' triangles until taken.
    void Load() {
        constexpr size_t MaxReadyBytes = size_t(256) << 20;
        std::vector<uint8_t> data;
        std::vector<MapFiles::ModelMesh> read;
        for (;;) {
            uint32_t mi;
            {
                std::unique_lock lock(mu);
                wake.wait(lock, [&] { return quit.load() || !queue.empty(); });
                if (quit) {
                    return;
                }
                mi = queue.front();
                queue.pop_front();
            }
            const GeoModel& model = loader_models[mi];
            const bool ok =
                MapFiles::ReadDcx(MapFiles::ModelPath(dvdroot, map_id, ModelName(model)), data) &&
                MapFiles::ReadModel(data, mtd, true, read) && read.size() == model.mesh_count;
            std::lock_guard lock(mu);
            queued.erase(mi);
            if (!ok) {
                failed.insert(mi);
                continue;
            }
            for (uint32_t k = 0; k < model.mesh_count; ++k) {
                const uint32_t mesh = model.first_mesh + k;
                if (ready.contains(mesh)) {
                    continue;
                }
                ready_bytes += read[k].soup.size() * sizeof(float);
                ready.emplace(mesh, std::move(read[k].soup));
                ready_order.push_back(mesh);
            }
            // Not taken (the camera moved on, or a mesh the cache does not build): oldest out.
            while (ready_bytes > MaxReadyBytes && !ready_order.empty()) {
                const auto it = ready.find(ready_order.front());
                ready_order.pop_front();
                if (it != ready.end()) {
                    ready_bytes -= it->second.size() * sizeof(float);
                    ready.erase(it);
                }
            }
        }
    }
};

void GeoMap::StartGame(const std::string& dvdroot, const std::string& map_id) {
    Close();
    name = map_id;
    game = std::make_shared<GeoGame>();
    game->dvdroot = dvdroot;
    game->map_id = map_id;
    GeoGame* g = game.get();
    g->scan = std::thread([g] { g->Scan(); });
}

bool GeoMap::PollGame() {
    if (!game || !places.empty() || game->scan_state.load() != 1) {
        return false;
    }
    models = std::move(game->models);
    meshes = std::move(game->meshes);
    places = std::move(game->places);
    collisions = std::move(game->collisions);
    mesh_model = std::move(game->mesh_model);
    BuildCells();
    return true;
}

bool GeoMap::GameFailed() const {
    return game && game->scan_state.load() == -1;
}

std::string GeoMap::GameReport() const {
    if (!game || game->scan_state.load() == 0) {
        return {};
    }
    return game->report;
}

int GeoMap::GameSoup(uint32_t mesh_index, float* out) {
    if (mesh_index >= mesh_model.size()) {
        return -1;
    }
    GeoGame& g = *game;
    const uint32_t mi = mesh_model[mesh_index];
    std::lock_guard lock(g.mu);
    const auto it = g.ready.find(mesh_index);
    if (it != g.ready.end()) {
        const bool fits = it->second.size() == size_t(meshes[mesh_index].index_count) * 3;
        if (fits) {
            std::memcpy(out, it->second.data(), it->second.size() * sizeof(float));
        }
        g.ready_bytes -= it->second.size() * sizeof(float);
        g.ready.erase(it);
        return fits ? 1 : -1;
    }
    if (g.failed.contains(mi)) {
        return -1;
    }
    if (g.queued.insert(mi).second) {
        g.queue.push_back(mi);
        g.wake.notify_one();
    }
    return 0;
}

} // namespace Vulkan::Rt
