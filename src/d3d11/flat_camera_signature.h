// The flat injector's kind-3 camera SIGNATURES: a log-only instrument (2026-10-14).
//
// The flat injector writes its jitter into EVERY kind-3 camera refresh (flat_camera_phase.h,
// flatCameraAdmit: no role, aspect or target test). The 5 s census names the camera OBJECTS and how
// often each was refreshed and injected, and nothing else: it cannot say whether an auxiliary kind-3
// camera (a cube-map face, a probe, a narrow square zoom camera, a 16:9 secondary view) shares the
// scene camera's object or sits beside it. The review that asked for this instrument is
// reviews\flat-sun-occlusion-jitter-2026-10-13.md; its next-flight list names the discriminating
// reading: a kind-3 signature other than the scene's with injected calls above zero means auxiliary
// cameras are being jittered in flat.
//
// What it does. One kind-3 call in sixteen is SAMPLED. The sampled call's frustum (aspect, vertical
// fov, near, far) is read once through the VR route's SEH-guarded readFrustum, quantised, and counted
// by signature together with the call's caller RVA and whether the call was injected. The first eight
// distinct signatures of a 5 s window are kept; a ninth is counted in `overflow`. The window's line is
// `flat camera signatures 5s: ...` (flatCameraFormatSignatures). It changes no behaviour and has no key.
//
// Why a hash and not a stride. Every sixteenth call would be blind to any camera that sits at a fixed
// place in a repeating call order of sixteen (or a divisor of it); the game's refresh order is exactly
// that kind of repeating list. The sampler is the top four bits of a Weyl sequence over the kind-3 call
// index: one multiply, equidistributed, and no call order of small period can hide from it (the rig
// walks the periods that matter).
//
// No call-path I/O, no allocation: the table is a fixed array on the owner thread (the thread that
// runs Present, which is the only one that reaches the flat path past the off-thread test), reset by the
// 5 s tick, printed by the tick.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

constexpr uint32_t kFlatCameraSignatureSampleOneIn = 16;   // a power of two: the sampler tests log2 bits
constexpr uint32_t kFlatCameraSignatureSampleBits = 4;     // log2(kFlatCameraSignatureSampleOneIn)
static_assert((1u << kFlatCameraSignatureSampleBits) == kFlatCameraSignatureSampleOneIn,
              "the sampler tests the top kFlatCameraSignatureSampleBits bits of a 32-bit hash");

// Is kind-3 call number `index` (counted from 0, never reset) sampled? 1 in 16, with no period.
inline bool flatCameraSignatureSampled(uint64_t index) noexcept {
    return (static_cast<uint32_t>(index) * 0x9E3779B1u) >> (32 - kFlatCameraSignatureSampleBits) == 0u;
}

// A value to two significant digits: m * 10^e with 10 <= |m| <= 99, or m == 0 for zero, or m == kNotFinite.
struct FlatCameraSigNum {
    static constexpr int16_t kNotFinite = 32767;
    int16_t m = 0, e = 0;
    bool operator==(const FlatCameraSigNum& o) const { return m == o.m && e == o.e; }
    bool finite() const { return m != kNotFinite; }
    double value() const { return static_cast<double>(m) * std::pow(10.0, static_cast<double>(e)); }
};
inline FlatCameraSigNum flatCameraSigNum(float v) noexcept {
    FlatCameraSigNum out;
    if (!std::isfinite(v)) { out.m = FlatCameraSigNum::kNotFinite; return out; }
    if (v == 0.0f) return out;
    const double a = std::fabs(static_cast<double>(v));
    int e = static_cast<int>(std::floor(std::log10(a))) - 1;
    double scaled = a / std::pow(10.0, e);
    // log10 can land one off at an exact power of ten: bring the mantissa into [10, 100).
    while (scaled < 10.0) { --e; scaled = a / std::pow(10.0, e); }
    while (scaled >= 100.0) { ++e; scaled = a / std::pow(10.0, e); }
    long m = std::lround(scaled);
    if (m >= 100) { m = 10; ++e; }   // 99.6 rounds up into the next decade
    out.m = static_cast<int16_t>(v < 0.0f ? -m : m);
    out.e = static_cast<int16_t>(e);
    return out;
}

// A value to 0.01, as an integer count of hundredths; INT32_MIN for a value that is not finite.
constexpr int32_t kFlatCameraSigNotFinite = INT32_MIN;
inline int32_t flatCameraSigHundredths(float v) noexcept {
    if (!std::isfinite(v)) return kFlatCameraSigNotFinite;
    double h = std::round(static_cast<double>(v) * 100.0);
    if (h > 1.0e9) h = 1.0e9;
    if (h < -1.0e9) h = -1.0e9;
    return static_cast<int32_t>(h);
}

struct FlatCameraSignature {
    int32_t aspect100 = 0, fov100 = 0;   // hundredths: aspect to 0.01, vertical fov to 0.01 rad
    FlatCameraSigNum nearZ, farZ;        // two significant digits
    uint32_t callerRva = 0;              // the refresh's call site, an offset from the game module; 0 when unknown
    bool operator==(const FlatCameraSignature& o) const {
        return aspect100 == o.aspect100 && fov100 == o.fov100 && nearZ == o.nearZ && farZ == o.farZ && callerRva == o.callerRva;
    }
};
inline FlatCameraSignature flatCameraSignatureOf(float aspect, float fov, float nearZ, float farZ, uint64_t callerRva) noexcept {
    FlatCameraSignature s;
    s.aspect100 = flatCameraSigHundredths(aspect);
    s.fov100 = flatCameraSigHundredths(fov);
    s.nearZ = flatCameraSigNum(nearZ);
    s.farZ = flatCameraSigNum(farZ);
    s.callerRva = callerRva > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(callerRva);
    return s;
}

struct FlatCameraSignatureEntry {
    FlatCameraSignature sig;
    uint64_t calls = 0, injected = 0;   // SAMPLED calls with this signature, and how many of them were injected
};

class FlatCameraSignatureTable {
public:
    static constexpr size_t kCapacity = 8;
    // One kind-3 call reached the flat path. True when it is the sampled one: the caller then reads the frustum
    // and MUST call note() exactly once for it, on whichever way the call ends.
    bool noteKind3Call() noexcept {
        ++kind3Calls_;
        if (!flatCameraSignatureSampled(index_++)) return false;
        ++sampled_;
        return true;
    }
    void note(const FlatCameraSignature& sig, bool injected) noexcept {
        for (size_t i = 0; i < used_; ++i) if (entry_[i].sig == sig) {
            ++entry_[i].calls;
            if (injected) ++entry_[i].injected;
            ++recorded_;
            return;
        }
        if (used_ < kCapacity) {
            entry_[used_].sig = sig;
            entry_[used_].calls = 1;
            entry_[used_].injected = injected ? 1u : 0u;
            ++used_;
            ++recorded_;
            return;
        }
        ++overflow_;
        ++recorded_;
    }
    size_t used() const { return used_; }
    const FlatCameraSignatureEntry& at(size_t i) const { return entry_[i]; }
    uint64_t kind3Calls() const { return kind3Calls_; }
    uint64_t sampled() const { return sampled_; }
    uint64_t recorded() const { return recorded_; }   // equals sampled() when every sampled call was noted
    uint64_t overflow() const { return overflow_; }   // sampled calls whose signature was not among the first eight
    // The window ends. The sampler's position carries on (a reset sequence would sample the same call numbers
    // every window).
    void reset() noexcept {
        for (size_t i = 0; i < kCapacity; ++i) entry_[i] = FlatCameraSignatureEntry{};
        used_ = 0;
        kind3Calls_ = sampled_ = recorded_ = overflow_ = 0;
    }
private:
    FlatCameraSignatureEntry entry_[kCapacity] = {};
    size_t used_ = 0;
    uint64_t kind3Calls_ = 0, sampled_ = 0, recorded_ = 0, overflow_ = 0;
    uint64_t index_ = 0;
};

// The window's line. Printed every window while the hook is installed, INCLUDING a window with no kind-3 call
// (kind3-calls=0 sampled=0 sigs=[]): a line that is absent means this code never ran, and a line that says
// sampled=0 beside kind3-calls above zero means it ran and the sampler picked nothing that window.
inline int flatCameraFormatSignatures(char* out, size_t size, const FlatCameraSignatureTable& t) {
    size_t n = 0;
    auto put = [&](const char* fmt, auto... args) {
        if (n >= size) return;
        const int w = std::snprintf(out + n, size - n, fmt, args...);
        if (w > 0) n += static_cast<size_t>(w);
    };
    put("flat camera signatures 5s: kind3-calls=%llu sampled=%llu (1 in %u; calls and injected below count sampled calls) "
        "distinct=%u overflow=%llu sigs=[",
        static_cast<unsigned long long>(t.kind3Calls()), static_cast<unsigned long long>(t.sampled()),
        kFlatCameraSignatureSampleOneIn, static_cast<unsigned>(t.used()), static_cast<unsigned long long>(t.overflow()));
    for (size_t i = 0; i < t.used(); ++i) {
        const FlatCameraSignatureEntry& e = t.at(i);
        put("%s", i ? " | " : "");
        if (e.sig.aspect100 == kFlatCameraSigNotFinite) put("aspect=nan"); else put("aspect=%.2f", e.sig.aspect100 / 100.0);
        if (e.sig.fov100 == kFlatCameraSigNotFinite) put(" fov=nan"); else put(" fov=%.2f", e.sig.fov100 / 100.0);
        if (!e.sig.nearZ.finite()) put(" near=nan"); else put(" near=%g", e.sig.nearZ.value());
        if (!e.sig.farZ.finite()) put(" far=nan"); else put(" far=%g", e.sig.farZ.value());
        if (e.sig.callerRva) put(" rva=+0x%X", e.sig.callerRva); else put(" rva=unknown");
        put(" calls=%llu injected=%llu", static_cast<unsigned long long>(e.calls), static_cast<unsigned long long>(e.injected));
    }
    put("]");
    return static_cast<int>(n);
}

} // namespace edvr
