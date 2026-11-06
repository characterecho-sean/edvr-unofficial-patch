#pragma once
// TEMPORARY instrument (experimental.cockpit_glass_probe; docs\cockpit-glass-legacy-port.md section 5): the pure half of the cockpit
// glass probe. Which shaders are the live canopy, which constant registers the legacy-glass port reads, and how a register is printed.
// No Windows API, so tools\cockpit_glass_probe_test can hold it to its contract.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace glass_probe {

// The live effect CockpitGlass_Forward (Effects2_Win64_SM50.arc), by EDVR shader hash (log.cpp's basis): tools\effects_arc.py
// extract --effect CockpitGlass_Forward. Pixel shaders by gCockpitGlassState (0 intact, 1 ice, 2 condensation) and
// gMaterialQualityLevel (0 and 1 share one); the vertex shader depends on the state alone.
struct CanopyShader {
    uint64_t hash;
    const char* name;
};
constexpr CanopyShader kCanopyPs[9] = {
    {0x70F56E2EB7032E1Aull, "intact q0-1"}, {0x3EAF4DB5B3E21089ull, "intact q2"}, {0x4E4FF61E8A08FC7Eull, "intact q3"},
    {0xF90EA0D834A8B44Eull, "ice q0-1"},    {0x4BF640EED290C885ull, "ice q2"},    {0x7F770D61DEA7CD30ull, "ice q3"},
    {0x3B4EDBBAF1BA87BFull, "condensation q0-1"}, {0x2A4B54153FBF97EEull, "condensation q2"}, {0x42747CC92CF844C4ull, "condensation q3"},
};
constexpr CanopyShader kCanopyVs[3] = {
    {0x8C091FFD08644E02ull, "intact"}, {0x63C6E375A08C78AAull, "ice"}, {0x3245BAA85513D01Bull, "condensation"},
};
constexpr int kCanopyPsCount = 9;
constexpr int kCanopyVsCount = 3;

inline int canopyPsIndex(uint64_t h) {
    for (int i = 0; i < kCanopyPsCount; ++i)
        if (kCanopyPs[i].hash == h) return i;
    return -1;
}
inline int canopyVsIndex(uint64_t h) {
    for (int i = 0; i < kCanopyVsCount; ++i)
        if (kCanopyVs[i].hash == h) return i;
    return -1;
}

// The frame block's registers the port reads (PS b1; register = ((id - 0x222) / 2) / 16, analysis\decomp\
// verify_20261103_cockpit_glass_live_mapping.txt): glass scalars 55-56, exposure 61, HDR scaling 90, light count 245, minimum ambient 248,
// ambient colour 257, moment-shadow resolve 261, camera 275-279, directional lights and shadow cascades 290-326.
struct RegSpan {
    uint16_t first, count;
};
// 227-228 are the light clusters' depth mapping and grid; 249-254 the byte offsets of the light records in t2 (docs\cockpit-glass-legacy-port.md, "Station lights").
constexpr RegSpan kCb1Spans[] = {{55, 2}, {61, 1}, {90, 1}, {227, 2}, {245, 1}, {248, 7}, {257, 1}, {261, 1}, {275, 5}, {290, 37}};
constexpr uint32_t kCb1Regs = 327;   // the copy reads registers 0..326 of the bound range
constexpr uint32_t kCb2Regs = 18;    // the material block, all of it
constexpr uint32_t kLightBytes = 192;   // t1: four 48-byte directional lights

// One 32-bit component. A float whose bits are a small positive integer (a denormal: exponent zero) is an integer the shader reads as
// one, printed "#n"; anything else prints as %.6g.
inline int formatComponent(uint32_t bits, char* out, size_t n) {
    if (bits != 0 && bits < 0x00800000u) return std::snprintf(out, n, "#%u", bits);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return std::snprintf(out, n, "%.6g", static_cast<double>(f));
}

// "[r] x y z w" for registers first..first+count-1 of `data` (bytes long, register 0 at data[0]). A register past the end prints
// "[r] -", so a short buffer reads as short and never as zeros. Returns the length written, truncated to n - 1.
inline size_t formatRegs(const uint8_t* data, size_t bytes, uint32_t first, uint32_t count, char* out, size_t n) {
    if (!out || n == 0) return 0;
    size_t len = 0;
    out[0] = '\0';
    auto put = [&](const char* s) {
        const size_t k = std::strlen(s);
        const size_t room = n - 1 - len;
        const size_t take = k < room ? k : room;
        std::memcpy(out + len, s, take);
        len += take;
        out[len] = '\0';
    };
    char tok[32];
    for (uint32_t r = first; r < first + count; ++r) {
        std::snprintf(tok, sizeof tok, "%s[%u]", r == first ? "" : " ", r);
        put(tok);
        if (!data || (static_cast<size_t>(r) + 1) * 16 > bytes) {
            put(" -");
            continue;
        }
        for (int c = 0; c < 4; ++c) {
            uint32_t bits;
            std::memcpy(&bits, data + static_cast<size_t>(r) * 16 + c * 4, 4);
            tok[0] = ' ';
            formatComponent(bits, tok + 1, sizeof tok - 1);
            put(tok);
        }
    }
    return len;
}

// The key: "on" arms the probe; anything else (the default "off") leaves it off.
inline bool probeKeyOn(const char* v) { return v && std::strcmp(v, "on") == 0; }

// Whether a sample is due: the first canopy draw, then one every interval, never while a readback is still out, at most `cap` a session.
inline bool sampleDue(bool pending, uint32_t taken, uint32_t cap, uint64_t now, uint64_t last, uint64_t interval) {
    if (pending || taken >= cap) return false;
    return taken == 0 || now - last >= interval;
}

// The game's clustered light lists in t0, read from its forward-lit shaders (docs\cockpit-glass-legacy-port.md, "Station lights"). One cluster is 2372 bytes: nine u32 counts, then nine id
// lists. Only the point, spot and probe lists are identified; the others are named by where their count sits.
constexpr uint32_t kClusterStride = 2372;
struct LightList {
    const char* name;
    uint16_t countAt, listAt, capacity;
};
constexpr LightList kLightLists[9] = {
    {"point", 0, 36, 128}, {"spot", 4, 548, 128}, {"count+8", 8, 1060, 64}, {"count+12", 12, 1316, 64}, {"count+16", 16, 1572, 64},
    {"count+20", 20, 1828, 64}, {"count+24", 24, 2084, 32}, {"probe", 28, 2212, 32}, {"count+32", 32, 2340, 8},
};
constexpr int kLightListCount = 9;
constexpr int kListPoint = 0, kListSpot = 1;
constexpr uint32_t kMaxSlices = 64;
constexpr uint32_t kIdSpace = 8192;

inline uint32_t u32At(const uint8_t* d, size_t at) {
    uint32_t v;
    std::memcpy(&v, d + at, 4);
    return v;
}
inline float f32At(const uint8_t* d, size_t at) {
    float v;
    std::memcpy(&v, d + at, 4);
    return v;
}

// The clusters both the grid names and the bytes hold. The two are printed apart, so a grid the buffer does not match reads as that.
inline uint32_t clustersHeld(uint32_t nx, uint32_t ny, uint32_t ns, size_t bytes) {
    const uint64_t grid = static_cast<uint64_t>(nx) * ny * ns;
    const uint64_t have = bytes / kClusterStride;
    return static_cast<uint32_t>(grid < have ? grid : have);
}

// The slice a view depth falls in, as the shaders work it out from cb1[227].zw. -1 for a depth with no slice (not positive, or no slices).
inline int sliceOfDepth(float z, float scale, float bias, uint32_t ns) {
    if (!(z > 0.0f) || ns == 0) return -1;
    const int s = static_cast<int>(std::log(z) * scale + bias);
    return s < 0 ? 0 : s >= static_cast<int>(ns) ? static_cast<int>(ns) - 1 : s;
}

struct ListCensus {
    uint32_t clusters = 0;      // clusters holding at least one id
    uint32_t ids = 0;           // ids over all clusters; one light in many clusters counts in each
    uint32_t most = 0;          // the most in one cluster
    uint32_t overCapacity = 0;  // clusters whose count is past the list's capacity: read as the capacity, and a sign the layout is wrong
    uint32_t distinct = 0;      // distinct lights
    uint32_t idsPast = 0;       // ids past kIdSpace: not counted as distinct
    uint32_t bySlice[kMaxSlices] = {};   // clusters holding any, by slice; a slice past the table counts in the last
};

// One list over every cluster. `wanted` takes up to wantedCap distinct ids first seen in slices lo..hi (the depths the glass lies at), so what is printed is what could reach the glass.
inline ListCensus censusList(const uint8_t* t0, size_t bytes, uint32_t nx, uint32_t ny, uint32_t ns, const LightList& list, int lo, int hi, uint32_t* wanted, uint32_t wantedCap,
                             uint32_t* wantedCount) {
    ListCensus out;
    uint8_t seen[kIdSpace / 8] = {};
    uint32_t got = 0;
    const uint32_t held = clustersHeld(nx, ny, ns, bytes);
    const uint64_t perSlice = static_cast<uint64_t>(nx) * ny;
    for (uint32_t c = 0; t0 && c < held; ++c) {
        const size_t base = static_cast<size_t>(c) * kClusterStride;
        uint32_t n = u32At(t0, base + list.countAt);
        if (!n) continue;
        if (n > list.capacity) {
            ++out.overCapacity;
            n = list.capacity;
        }
        const uint64_t slice = perSlice ? c / perSlice : 0;
        ++out.clusters;
        out.ids += n;
        if (n > out.most) out.most = n;
        ++out.bySlice[slice < kMaxSlices ? slice : kMaxSlices - 1];
        const bool inGlass = static_cast<int64_t>(slice) >= lo && static_cast<int64_t>(slice) <= hi;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t id = u32At(t0, base + list.listAt + i * 4u);
            if (id >= kIdSpace) {
                ++out.idsPast;
                continue;
            }
            uint8_t& byte = seen[id / 8];
            const uint8_t bit = static_cast<uint8_t>(1u << (id % 8));
            if (!(byte & bit)) {
                byte |= bit;
                ++out.distinct;
            }
            if (inGlass && wanted && got < wantedCap) {
                bool have = false;
                for (uint32_t k = 0; k < got && !have; ++k) have = wanted[k] == id;
                if (!have) wanted[got++] = id;
            }
        }
    }
    if (wantedCount) *wantedCount = got;
    return out;
}

// One light's record in t2, as the shaders unpack it. `a` and `b` are the record bases: cb1[249].zw for a point, cb1[250].zw for a spot. False when the record is past the buffer.
struct LightRecord {
    float x = 0, y = 0, z = 0, radius = 0, intensity = 0, r = 0, g = 0, b = 0, sourceRadius = 0, cosOuter = 0, cosInner = 0;
    int shadow = -1, cookie = -1;   // -1 is none
};
inline void unpackColour(uint32_t c, LightRecord* o) {
    o->r = (c & 1023u) / 1023.0f;
    o->g = ((c >> 10) & 1023u) / 1023.0f;
    o->b = ((c >> 20) & 1023u) / 1023.0f;
}
inline bool readPoint(const uint8_t* t2, size_t bytes, uint32_t a, uint32_t b, uint32_t id, LightRecord* o) {
    const uint64_t at = static_cast<uint64_t>(a) + (static_cast<uint64_t>(id) << 4), bt = static_cast<uint64_t>(b) + (static_cast<uint64_t>(id) << 4);
    if (!t2 || at + 16 > bytes || bt + 16 > bytes) return false;
    o->x = f32At(t2, at);
    o->y = f32At(t2, at + 4);
    o->z = f32At(t2, at + 8);
    o->radius = f32At(t2, at + 12);
    o->intensity = f32At(t2, bt);
    unpackColour(u32At(t2, bt + 4), o);
    o->shadow = static_cast<int32_t>(u32At(t2, bt + 12));
    return true;
}
inline bool readSpot(const uint8_t* t2, size_t bytes, uint32_t a, uint32_t b, uint32_t id, LightRecord* o) {
    const uint64_t at = static_cast<uint64_t>(a) + (static_cast<uint64_t>(id) << 5), bt = static_cast<uint64_t>(b) + (static_cast<uint64_t>(id) << 5);
    if (!t2 || at + 28 > bytes || bt + 24 > bytes) return false;
    o->x = f32At(t2, at);
    o->y = f32At(t2, at + 4);
    o->z = f32At(t2, at + 8);
    o->radius = f32At(t2, at + 24);
    o->intensity = f32At(t2, bt);
    unpackColour(u32At(t2, bt + 4), o);
    o->sourceRadius = f32At(t2, bt + 8);
    const uint32_t cone = u32At(t2, bt + 12);
    o->cosOuter = (cone & 0xFFFFu) / 65535.0f;
    o->cosInner = (cone >> 16) / 65535.0f;
    const uint32_t idx = u32At(t2, bt + 20);
    o->shadow = static_cast<int>(idx & 0xFFFFu) - 1;
    o->cookie = static_cast<int>(idx >> 16) - 1;
    return true;
}

}  // namespace glass_probe
}  // namespace edvr
