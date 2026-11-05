#pragma once
// visual.cockpit_glass (docs\cockpit-glass-legacy-port.md): the pure half. The key, which bound pixel shader is the one the legacy look replaces, the
// replacement's input signature (which must be the game's own, so it links with the game's canopy vertex shader), and how the log lines are worded.
// No Windows API, so tools\cockpit_glass_test can hold it to its contract.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace cockpit_glass {

// The live canopy pixel shader the replacement stands in for, by EDVR shader hash (log.cpp's basis): CockpitGlass_Forward, gCockpitGlassState 0
// (intact), gMaterialQualityLevel 3. The only canopy pixel shader the probe flight saw draw. Ice, condensation and quality 0 to 2 are other
// hashes and stay stock (doc section 6, decision 3).
constexpr uint64_t kReplacedPsHash = 0x4E4FF61E8A08FC7Eull;
// Its vertex shader, which the replacement is linked with.
constexpr uint64_t kCanopyVsHash = 0x8C091FFD08644E02ull;

// The ice and condensation pixel shaders at quality 3, over which the legacy dirt is drawn as an overlay (their bindings were checked against the intact shader's:
// docs\cockpit-glass-legacy-port.md section 11). The quality 0-2 variants (F90EA0D834A8B44E, 4BF640EED290C885, 3B4EDBBAF1BA87BF, 2A4B54153FBF97EE) bind other slot layouts
// and stay as the game draws them. The vertex shaders are the ice one 63C6E375A08C78AA and the condensation one 3245BAA85513D01B.
constexpr uint64_t kIcePsQ3Hash = 0x7F770D61DEA7CD30ull;
constexpr uint64_t kCondensationPsQ3Hash = 0x42747CC92CF844C4ull;
enum class Overlay : int8_t { None = -1, Ice = 0, Condensation = 1 };
inline Overlay overlayFor(uint64_t psHash) {
    if (psHash == kIcePsQ3Hash) return Overlay::Ice;
    if (psHash == kCondensationPsQ3Hash) return Overlay::Condensation;
    return Overlay::None;
}

enum class Look : uint8_t { Odyssey, Legacy };

inline bool equalsNoCase(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        const char x = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
        const char y = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

// The key: "odyssey" (the game as it ships) or "legacy". Anything else, or no value, is odyssey; `recognised` says whether it was one of the two, so the
// caller can say so once instead of silently reading a typo as the default.
inline Look parseLook(const char* v, bool* recognised) {
    if (v && equalsNoCase(v, "legacy")) {
        if (recognised) *recognised = true;
        return Look::Legacy;
    }
    if (recognised) *recognised = !v || equalsNoCase(v, "odyssey");
    return Look::Odyssey;
}

// Whether the pixel shader being bound is handed the replacement: only with the key on, a replacement made, and the game binding the intact quality-3
// canopy shader. Every other bind goes through unchanged.
inline bool swapWanted(Look look, bool haveReplacement, uint64_t boundHash) {
    return look == Look::Legacy && haveReplacement && boundHash == kReplacedPsHash;
}

// visual.cockpit_glass_dirt: how much dirt the legacy glass shows, in percent of the look as shipped (100 is the shader's own dirt strength, 0 a clean pane). It is read
// only while `legacy` is on; with `odyssey` the game draws its own dirt and the key does nothing, which is why the menus dim its row then (dirtRowDim).
constexpr int kDirtPercentDefault = 100;
constexpr int kDirtPercentMin = 0;
constexpr int kDirtPercentMax = 300;
// The shader's dirt level (cbuffer Level, x) for a percent, held to the key's range so a bad file value cannot push the dirt past what the menu offers.
inline float dirtLevelOf(int percent) {
    const int p = percent < kDirtPercentMin ? kDirtPercentMin : (percent > kDirtPercentMax ? kDirtPercentMax : percent);
    return static_cast<float>(p) / 100.0f;
}
// The pixel-shader constant-buffer slot the level lives in (b13, the last of D3D11's fourteen): the game's canopy shaders bind b1 and b2.
constexpr unsigned kLevelSlot = 13;
// Whether the Legacy glass dirt row is dimmed (and ignores steps and typing): the dirt is only drawn by the legacy shader.
inline bool dirtRowDim(Look look) { return look != Look::Legacy; }

// A window line is due once the interval has passed since the last.
inline bool windowDue(uint64_t now, uint64_t last, uint64_t interval) { return now - last >= interval; }

// "cockpit glass: window 30 s legacy binds=N canopy-seen=M ice-overlays=K replacement=ready". binds is how many canopy pixel-shader binds were handed the replacement;
// canopy-seen is how many the game made at all; ice-overlays is how many ice or condensation draws were drawn over with the legacy dirt. With the key on, binds=0 and canopy-seen>0 is a replacement that was refused or missing; both 0 is a
// canopy the game never drew (or a different shader, which the probe's 'other-ps' says).
inline int formatWindow(char* out, size_t n, double seconds, uint32_t binds, uint32_t seen, uint32_t overlays, const char* replacement) {
    return std::snprintf(out, n, "cockpit glass: window %.0f s legacy binds=%u canopy-seen=%u ice-overlays=%u replacement=%s", seconds, binds, seen,
                         overlays, replacement);
}

// What the replacement says about itself in the log.
enum class Replacement : uint8_t { NotTried, Ready, Refused };
inline const char* replacementName(Replacement r) {
    switch (r) {
        case Replacement::Ready: return "ready";
        case Replacement::Refused: return "refused";
        default: return "not-made-yet";
    }
}

// The live pixel shader's input signature, from its disassembly (analysis\decomp\verify_20261103_cockpit_glass_live.txt): the replacement has to
// carry the same names, indexes, registers and masks, element for element, or it does not link with the live vertex shader.
struct InputElement {
    const char* name;
    uint8_t index;
    uint8_t reg;
    uint8_t mask;       // component bits, x = 1
    bool isUint;        // the face flag is an unsigned integer, constant across the primitive
    bool isPosition;    // SV_POSITION
};
constexpr InputElement kLiveInputs[] = {
    {"__USER_KEY_COCKPITGLASSMASKS", 0, 0, 0x7, false, false},
    {"__USER_VERTEX_FACEINVARIANT", 0, 1, 0x1, true, false},
    {"__USER_VERTEX_M_LIGHTINGNORMAL", 0, 2, 0x7, false, false},
    {"__USER_VERTEX_M_LIGHTINGPOSITION", 0, 3, 0x7, false, false},
    {"__USER_VERTEX_M_LIGHTINGTANGENT", 0, 4, 0x7, false, false},
    {"__USER_VERTEX_M_TEXCOORD", 0, 5, 0x3, false, false},
    {"SV_Position", 0, 6, 0xF, false, true},
};
constexpr size_t kLiveInputCount = sizeof(kLiveInputs) / sizeof(kLiveInputs[0]);

// The ice and condensation vertex shaders' output (the overlays carry it): the same, but the masks are four components.
constexpr InputElement kWideInputs[] = {
    {"__USER_KEY_COCKPITGLASSMASKS", 0, 0, 0xF, false, false},
    {"__USER_VERTEX_FACEINVARIANT", 0, 1, 0x1, true, false},
    {"__USER_VERTEX_M_LIGHTINGNORMAL", 0, 2, 0x7, false, false},
    {"__USER_VERTEX_M_LIGHTINGPOSITION", 0, 3, 0x7, false, false},
    {"__USER_VERTEX_M_LIGHTINGTANGENT", 0, 4, 0x7, false, false},
    {"__USER_VERTEX_M_TEXCOORD", 0, 5, 0x3, false, false},
    {"SV_Position", 0, 6, 0xF, false, true},
};

// The registers the live shader declares: the frame block (b1, 327 registers, dynamically indexed) and the material block (b2, 18).
constexpr uint32_t kFrameRegisters = 327;
constexpr uint32_t kMaterialRegisters = 18;

}  // namespace cockpit_glass
}  // namespace edvr
