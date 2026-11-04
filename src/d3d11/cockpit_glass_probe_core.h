#pragma once
// TEMPORARY instrument (experimental.cockpit_glass_probe; docs\cockpit-glass-legacy-port.md section 5): the pure half of the cockpit
// glass probe. Which shaders are the live canopy, which constant registers the legacy-glass port reads, and how a register is printed.
// No Windows API, so tools\cockpit_glass_probe_test can hold it to its contract.
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
constexpr RegSpan kCb1Spans[] = {{55, 2}, {61, 1}, {90, 1}, {245, 1}, {248, 1}, {257, 1}, {261, 1}, {275, 5}, {290, 37}};
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

}  // namespace glass_probe
}  // namespace edvr
