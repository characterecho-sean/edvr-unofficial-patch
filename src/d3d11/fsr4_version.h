// The pure decisions behind fsr4_engine.cpp, with no AMD SDK, no device and no project .cpp, so tools\fsr4_engine_test can pin
// them in its --self-test on a machine with no AMD GPU: which of the versions AMD's runtime offers FSR 4 may run (decision D7,
// docs\fsr-upscaler-design-2026-09-16.md, "2026-10-23: route 2"), how a refusal reads, which texture formats the D3D12 side
// is given, the creation and dispatch flags, the camera planes and the frame time. Header-only, namespace edvr.
//
// D7: fsr4 runs only when AMD's runtime offers a 4.x provider for the device. It never runs an FSR 3.1.5 provider in fsr4's
// name; a GPU AMD routes to FSR 3.1.5 (RDNA 2, RDNA 3.5, every non-AMD GPU) gets a refusal that names what it was offered.
#pragma once

#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <dxgiformat.h>

#include "hdr_backend_flags.h"   // the flat route's FSR creation flags, whose bits AMD's FSR 4 flags share (checked by the engine)

namespace edvr {

// ---- versions ----

struct Fsr4Version {
    uint32_t major = 0, minor = 0, patch = 0;
};

inline bool fsr4IsDigit(char c) { return c >= '0' && c <= '9'; }

// Reads a run of at most six digits at s[i], leaving i past it. False on none, or on more (a number that large is not a version).
inline bool fsr4ReadNumber(const char* s, size_t& i, uint32_t& value) {
    size_t digits = 0;
    uint32_t v = 0;
    while (fsr4IsDigit(s[i])) {
        if (++digits > 6) return false;
        v = v * 10u + static_cast<uint32_t>(s[i] - '0');
        ++i;
    }
    if (!digits) return false;
    value = v;
    return true;
}

// "3.1.5", "4.1", "FSR 4.1.1", "AMD FSR v4.1.1 (ML)": the first "digits.digits[.digits]" in the name, whatever precedes it. A
// missing patch reads as 0. A name with no such run ("", "unknown", "4", "1..2", null) is not a version: false, out untouched.
inline bool fsr4ParseVersion(const char* name, Fsr4Version* out = nullptr) {
    if (!name) return false;
    for (size_t start = 0; name[start]; ++start) {
        if (!fsr4IsDigit(name[start])) continue;
        // Not the middle of a number, and not the tail of a dotted run that began earlier (and failed): "1234567.1.1" is no 1.1.
        if (start > 0 && (fsr4IsDigit(name[start - 1]) || name[start - 1] == '.')) continue;
        size_t i = start;
        Fsr4Version v;
        if (!fsr4ReadNumber(name, i, v.major) || name[i] != '.') continue;
        ++i;
        if (!fsr4ReadNumber(name, i, v.minor)) continue;
        if (name[i] == '.' && fsr4IsDigit(name[i + 1])) {
            ++i;
            if (!fsr4ReadNumber(name, i, v.patch)) continue;
        }
        if (out) *out = v;
        return true;
    }
    return false;
}

inline int fsr4CompareVersions(const Fsr4Version& a, const Fsr4Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

// Index of the highest offered version whose major is at least 4, or -1 when AMD offers none (D7: then fsr4 refuses). Names that
// are not versions are ignored; of two equal versions the first wins. picked, when given, receives the winner.
inline int fsr4PickVersion(const std::vector<std::string>& names, Fsr4Version* picked = nullptr) {
    int best = -1;
    Fsr4Version bestVersion;
    for (size_t i = 0; i < names.size(); ++i) {
        Fsr4Version v;
        if (!fsr4ParseVersion(names[i].c_str(), &v) || v.major < 4) continue;
        if (best < 0 || fsr4CompareVersions(v, bestVersion) > 0) {
            best = static_cast<int>(i);
            bestVersion = v;
        }
    }
    if (best >= 0 && picked) *picked = bestVersion;
    return best;
}

inline std::string fsr4FormatVersion(const Fsr4Version& v) {
    char text[40];
    std::snprintf(text, sizeof(text), "%u.%u.%u", v.major, v.minor, v.patch);
    return text;
}

// "3.1.5, 4.1.1": each offered name as major.minor.patch, in the order AMD gave them. A name that is not a version is shown as
// it came (at most 24 characters; "?" when empty), so a log never hides what the runtime said.
inline std::string fsr4FormatVersionList(const std::vector<std::string>& names) {
    std::string text;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) text += ", ";
        Fsr4Version v;
        if (fsr4ParseVersion(names[i].c_str(), &v)) {
            text += fsr4FormatVersion(v);
        } else if (names[i].empty()) {
            text += "?";
        } else {
            text += names[i].substr(0, 24);
        }
    }
    return text;
}

// The refusal when AMD's runtime offers no 4.x provider (D7). underWine adds the Proton launch option: Valve's Wine routes the
// version query to AMD's driver-side FSR 4 file only with FSR4_UPGRADE=1.
inline std::string fsr4RefusalText(const std::vector<std::string>& offered, bool underWine) {
    std::string text = offered.empty()
        ? std::string("AMD's runtime offers no FSR version for this GPU")
        : "AMD's runtime offers only FSR " + fsr4FormatVersionList(offered) + " for this GPU";
    text += "; FSR 4 needs a Radeon RX 7000 or 9000 series GPU (set temporal_aa = fsr for FSR 3.1)";
    if (underWine)
        text += " -- under Proton, launch Elite with FSR4_UPGRADE=1 %command% on a Proton that carries AMD's FSR 4 driver file";
    return text;
}

// Whether the provider a made context reports is a 4.x one (D7, belt and braces after the override). A name that parses is judged
// by its major; one that does not is accepted only when its id is the id the context was asked for.
inline bool fsr4ProviderIsFour(const char* name, uint64_t providerId, uint64_t requestedId) {
    Fsr4Version v;
    if (fsr4ParseVersion(name, &v)) return v.major >= 4;
    return requestedId != 0 && providerId == requestedId;
}

// ---- the context key and its failure latch ----

// Everything a context and its shared textures are made for. A change remakes both. The formats are the typed ones the shared
// textures take (fsr4TypedFormat), stored as numbers so this header needs no device.
struct Fsr4ContextKey {
    uint32_t inW = 0, inH = 0, outW = 0, outH = 0;
    bool infiniteDepth = false, hdr = false;
    uint32_t colourFormat = 0, depthFormat = 0, motionFormat = 0, outFormat = 0;

    bool operator==(const Fsr4ContextKey& o) const {
        return inW == o.inW && inH == o.inH && outW == o.outW && outH == o.outH &&
               infiniteDepth == o.infiniteDepth && hdr == o.hdr &&
               colourFormat == o.colourFormat && depthFormat == o.depthFormat &&
               motionFormat == o.motionFormat && outFormat == o.outFormat;
    }
};

// One failed context key is remembered at a time, as XessFeatureFailureLatch does for XeSS: a new key gets a fresh attempt, and
// a release or profile switch clears it.
class Fsr4FailureLatch {
public:
    bool matches(const Fsr4ContextKey& key) const { return failed_ && key_ == key; }
    const char* reason() const { return reason_; }
    void clear() { failed_ = false; reason_[0] = '\0'; }
    void clearIfDifferent(const Fsr4ContextKey& key) {
        if (failed_ && !(key_ == key)) clear();
    }
    void record(const Fsr4ContextKey& key, const char* reason) {
        key_ = key;
        std::snprintf(reason_, sizeof(reason_), "%s", reason ? reason : "no reason given");
        failed_ = true;
    }

private:
    Fsr4ContextKey key_{};
    char reason_[384]{};
    bool failed_ = false;
};

// ---- texture formats ----

// FSR 4 lacks some typeless conversions, so the shared textures the D3D12 side sees are always typed. The callers' textures may
// be typeless or a depth format of the same family; the copy into the shared texture is a plain bit copy between formats of one
// typeless family, which D3D11 allows. DXGI_FORMAT_UNKNOWN means FSR 4 has no input of that kind in that format.
enum class Fsr4Role { Colour, Depth, Motion, Output };

inline DXGI_FORMAT fsr4TypedFormat(Fsr4Role role, DXGI_FORMAT source) {
    switch (role) {
        case Fsr4Role::Colour:
            switch (source) {
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   // the sRGB-encoded bytes are copied as they are; AMD is told they are sRGB
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                    return DXGI_FORMAT_B8G8R8A8_UNORM;
                case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                case DXGI_FORMAT_R10G10B10A2_UNORM:
                    return DXGI_FORMAT_R10G10B10A2_UNORM;
                case DXGI_FORMAT_R11G11B10_FLOAT:
                    return DXGI_FORMAT_R11G11B10_FLOAT;
                case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                case DXGI_FORMAT_R16G16B16A16_FLOAT:
                    return DXGI_FORMAT_R16G16B16A16_FLOAT;
                case DXGI_FORMAT_R32G32B32A32_TYPELESS:
                case DXGI_FORMAT_R32G32B32A32_FLOAT:
                    return DXGI_FORMAT_R32G32B32A32_FLOAT;
                default:
                    return DXGI_FORMAT_UNKNOWN;
            }
        case Fsr4Role::Depth:   // AMD wants 32-bit float depth; D32_FLOAT and R32_TYPELESS share R32_FLOAT's family
            switch (source) {
                case DXGI_FORMAT_R32_TYPELESS:
                case DXGI_FORMAT_R32_FLOAT:
                case DXGI_FORMAT_D32_FLOAT:
                    return DXGI_FORMAT_R32_FLOAT;
                default:
                    return DXGI_FORMAT_UNKNOWN;
            }
        case Fsr4Role::Motion:
            switch (source) {
                case DXGI_FORMAT_R16G16_TYPELESS:
                case DXGI_FORMAT_R16G16_FLOAT:
                    return DXGI_FORMAT_R16G16_FLOAT;
                case DXGI_FORMAT_R32G32_TYPELESS:
                case DXGI_FORMAT_R32G32_FLOAT:
                    return DXGI_FORMAT_R32G32_FLOAT;
                default:
                    return DXGI_FORMAT_UNKNOWN;
            }
        case Fsr4Role::Output:   // written through a typed UAV, so only the formats every D3D12 GPU stores to
            switch (source) {
                case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                case DXGI_FORMAT_R10G10B10A2_UNORM:
                    return DXGI_FORMAT_R10G10B10A2_UNORM;
                case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                case DXGI_FORMAT_R16G16B16A16_FLOAT:
                    return DXGI_FORMAT_R16G16B16A16_FLOAT;
                default:
                    return DXGI_FORMAT_UNKNOWN;
            }
    }
    return DXGI_FORMAT_UNKNOWN;
}

// ---- flags ----

// AMD's FidelityFX SDK v2.3.0 ffx_upscale.h bits that are not in hdr_backend_flags.h (which holds the FSR 3.1 DX11 port's). The
// engine static_asserts each against the real enum, as it does the shared ones.
constexpr uint32_t kFsr4FlagNonLinearColourspace = 1u << 8;   // FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE
constexpr uint32_t kFsr4DispatchNonLinearSrgb = 1u << 1;      // FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB

// Creation flags: route 1's (reversed Z always, infinite depth when asked, the HDR bit on the HDR route; hdr_backend_flags.h)
// with no AMD debug checking, and two of FSR 4's own: automatic exposure on both routes, because AMD documents the exposure
// input as optional only when that flag is set and recommends it (route 1 runs the LDR route with neither, which FSR 3.1
// accepts); and on the display-referred LDR route the non-linear colour space, which FSR 4 asks of colour that is not linear.
inline constexpr uint32_t fsr4CreateFlags(bool infiniteDepth, bool hdr) {
    return flatFsrCreateFlags(infiniteDepth, false, hdr) | kFsrFlagAutoExposure | (hdr ? 0u : kFsr4FlagNonLinearColourspace);
}

// Dispatch flags: LDR colour is declared sRGB; HDR scene radiance is linear and needs none.
inline constexpr uint32_t fsr4DispatchFlags(bool hdr) { return hdr ? 0u : kFsr4DispatchNonLinearSrgb; }

// ---- camera and time ----

// fsr3_engine.cpp's constants, kept: a 90 degree stand-in for an unknown field of view, and the planes the game has been seen to
// use for unknown ones.
constexpr float kFsr4DefaultFovYRadians = 1.5707963f;
constexpr float kFsr4DefaultNearZ = 0.025f;
constexpr float kFsr4DefaultFarZ = 50000.0f;
constexpr float kFsr4NominalFrameMs = 11.1f;

// AMD's cameraNear/cameraFar under reversed Z, exactly as fsr3_engine.cpp computes them: the larger of the pair goes in as
// cameraNear (AMD's own check warns otherwise; its depth transform takes the min and max itself), an infinite projection hands
// FLT_MAX as the far plane and so FLT_MAX as cameraNear, and planes that are unknown (0, or not finite) take the documented
// defaults. False, with a reason, only for an infinite projection with no finite positive near plane.
inline bool fsr4CameraPlanes(float nearZ, float farZ, bool infiniteDepth, float* camNear, float* camFar, const char** why) {
    float n = std::isfinite(nearZ) ? nearZ : 0.0f;
    float f = std::isfinite(farZ) ? farZ : 0.0f;
    if (infiniteDepth) {
        if (!(n > 0.0f)) {
            if (why) *why = "infinite depth requires a finite positive near plane";
            return false;
        }
        f = FLT_MAX;   // AMD requires cameraNear = FLT_MAX for an infinite reversed projection
    }
    if (n <= 0.0f || f <= 0.0f) {
        n = kFsr4DefaultNearZ;
        f = kFsr4DefaultFarZ;
    }
    *camNear = n > f ? n : f;
    *camFar = n > f ? f : n;
    return true;
}

// frameTimeDelta in milliseconds. 0 (or anything not a positive finite number) is "unknown" and reads as a nominal 90 Hz frame;
// a real value keeps the clamp that stops a hitch or a debugger break reaching AMD's history weighting (fsr3_engine.cpp).
inline float fsr4FrameTimeMs(float frameMs) {
    if (!std::isfinite(frameMs) || frameMs <= 0.0f) return kFsr4NominalFrameMs;
    return frameMs < 0.1f ? 0.1f : (frameMs > 100.0f ? 100.0f : frameMs);
}

}  // namespace edvr
