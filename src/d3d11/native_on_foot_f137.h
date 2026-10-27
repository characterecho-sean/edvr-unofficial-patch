// F137 (native on-foot VR): a virtual neck under mouse look. Header only, no game or D3D11 state, so native_frame_test runs the very functions the frame thread uses.
// Journal: the arc doc, 2026-10-27, F137.
//
// Under look to aim the aim follows the head, so the head's own pitch is the aim's pitch and the headset's translation carries the neck: a head pitched 60 deg down drops
// the eye about 15 cm and moves it about 9 cm forward (a rest vector from the neck to the eye of up 15.5 cm and forward 9 cm, turned about the neck). Under mouse look the
// game pitches the camera and the head stays level: the camera is the aim turned by the reported head, but the eye position stays the level head's, behind and above where
// a head pitched by the aim would put it. The pilot sees a torso hunched under a head that has not moved (the F136 flight: the chest lean was not the visible cause).
//
// The virtual neck adds what the game's aim pitch would have made of the neck. With v the neck-to-eye rest vector (forward kPinRestBackM, up kPinRestUpM: the constants
// pinEyeHeightNormalized uses; the name "back" there is the eye's offset from the neck toward the face), R(p) the rotation by p nose up in the (forward, up) plane,
//     R(p) (f, u) = (f cos p - u sin p, f sin p + u cos p),
// h the head's own pitch (its natural LOCAL pitch, nose up +) and a the game's aim pitch (the FP root's forward elevation, nose up +), the camera's pitch is about h + a
// and the missing translation, in the body's (forward, up) plane (no sideways part), is
//     d = R(h + a) v - R(h) v.
// At a = 0 it is 0. At h = 0, a = -60 deg: forward +8.9 cm, up -15.5 cm (down). Its length is capped at 25 cm.
//
// It is published as part of the eye alignment, which is a SEATED vector (seated = D body, D = diag(-1, -1, +1): seatedFromBody), so the view moves down and forward as a
// head pitched by the aim would; the view's rotation is untouched (it already comes from the game). It is the VIEW only: the body pin takes d out of the game's camera
// again, by the serial the pose carried (so the body does not follow it), and the head containment (F119/F123) reads the physical head as the pose's render plus the
// alignment that pose carried, in which d cancels. Weight: a ramp of 0.15 s in and out, times 1 less the ADS lock's weight (the sight lock owns the eye), and 0 while look
// to aim drives the aim.
#pragma once

#include "native_on_foot_core.h"   // kPinRestUpM, kPinRestBackM, seatedFromBody

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f137 {

constexpr double kRampS = 0.15;                    // the weight's ramp, in and out
constexpr double kCapM = 0.25;                     // the largest |d|
constexpr uint64_t kAimMaxAgeMs = 300;             // the pin hook's aim pitch is read while it is no older than this

// The rest neck-to-eye vector turned nose up by pitchRad: its forward and up parts (m).
inline void rotateRest(double pitchRad, double* forward, double* up) noexcept {
    const double c = std::cos(pitchRad), s = std::sin(pitchRad);
    *forward = kPinRestBackM * c - kPinRestUpM * s;
    *up = kPinRestUpM * c + kPinRestBackM * s;
}

struct Offset {
    bool ok = false;
    double forward = 0.0, up = 0.0;   // body axes, metres: forward +, up + (down is negative)
    double length() const noexcept { return std::sqrt(forward * forward + up * up); }
};
// d = R(h + a) v - R(h) v, its length capped at kCapM. Not ok without two finite pitches.
inline Offset offset(double headPitchRad, double aimPitchRad) noexcept {
    Offset o;
    if (!std::isfinite(headPitchRad) || !std::isfinite(aimPitchRad)) return o;
    double f0 = 0.0, u0 = 0.0, f1 = 0.0, u1 = 0.0;
    rotateRest(headPitchRad, &f0, &u0);
    rotateRest(headPitchRad + aimPitchRad, &f1, &u1);
    o.forward = f1 - f0;
    o.up = u1 - u0;
    const double len = o.length();
    if (len > kCapM) {
        o.forward *= kCapM / len;
        o.up *= kCapM / len;
    }
    o.ok = true;
    return o;
}
// The seated eye-alignment vector of a body offset (forward, up): (0, -up, +forward), the same map the other eye terms use (seatedFromBody at psi 0).
inline void seatedOf(double forwardM, double upM, double out[3]) noexcept {
    const double body[3] = {0.0, upM, forwardM};
    seatedFromBody(body, 0.0, out);
    for (int i = 0; i < 3; ++i) out[i] += 0.0;   // -0 becomes +0
}

// Why the virtual neck is not applied, in the order the tick asks; kOn is applied.
enum Reason : int { kOn = 0, kLookToAim = 1, kOutOfScope = 2, kNoAim = 3, kNoHead = 4, kReasons = 5 };
inline const char* reasonName(int r) noexcept {
    switch (r) {
        case kLookToAim: return "look to aim drives the aim";
        case kOutOfScope: return "outside the body pin's scope";
        case kNoAim: return "no fresh aim pitch from the pin hook";
        case kNoHead: return "no head pose from the runtime";
        default: return "applied";
    }
}
struct In {
    bool scope = false;        // native stereo on foot, on the commander's first-person camera, the body pin's scope (the other view terms' scope)
    bool lookToAim = false;    // look to aim drives the aim (engaged and not held by F131: g_ltaDrop)
    bool aimFresh = false;     // the aim pitch is the pin hook's of the last kAimMaxAgeMs
    bool headValid = false;    // the head's natural LOCAL pitch is known
    double aimPitchRad = 0.0;
    double headPitchRad = 0.0;
    double adsWeight = 0.0;    // the ADS lock's weight this tick (0..1)
    double dt = 0.0;           // s
};
inline int reasonOf(const In& in) noexcept {
    if (!in.scope) return kOutOfScope;
    if (in.lookToAim) return kLookToAim;
    if (!in.aimFresh || !std::isfinite(in.aimPitchRad)) return kNoAim;
    if (!in.headValid || !std::isfinite(in.headPitchRad)) return kNoHead;
    return kOn;
}
struct Out {
    int reason = kOutOfScope;
    double w = 0.0;                         // ramp times (1 - ADS weight)
    double forwardM = 0.0, upM = 0.0;       // the applied offset (weighted), body axes
    double seated[3] = {};                  // ... as the eye alignment's seated vector
};
struct VirtualNeck {
    double ramp = 0.0;
    Offset last;   // the last offset worked out: the ramp out carries it while the inputs are gone (look to aim engaged, a stale aim), so the weight alone leaves
    void reset() noexcept {
        ramp = 0.0;
        last = Offset{};
    }
    Out step(const In& in) noexcept {
        Out o;
        o.reason = reasonOf(in);
        const double d = !(in.dt > 0.0) ? 0.0 : in.dt > 0.25 ? 0.25 : in.dt;
        ramp = o.reason == kOn ? std::min(1.0, ramp + d / kRampS) : std::max(0.0, ramp - d / kRampS);
        // the offset follows the inputs while the neck is applied; the ramp out (look to aim engaged, a stale aim, the pin out of scope) carries the last one, so only the
        // weight leaves
        if (o.reason == kOn) {
            const Offset live = offset(in.headPitchRad, in.aimPitchRad);
            if (live.ok) last = live;
        }
        const double ads = std::isfinite(in.adsWeight) ? std::min(1.0, std::max(0.0, in.adsWeight)) : 0.0;
        o.w = ramp * (1.0 - ads);
        if (!(o.w > 0.0) || !last.ok) {
            o.w = 0.0;
            return o;
        }
        o.forwardM = o.w * last.forward;
        o.upM = o.w * last.up;
        seatedOf(o.forwardM, o.upM, o.seated);
        return o;
    }
};

// The virtual neck's part of every eye alignment published, by serial (the seated y and z; x is 0): the frame thread writes (f89AlignPublish), the P1 hook reads on
// another thread. A slot is its serial and one atomic word with the two floats' bits; a reader checks the serial before and after the word, so a slot overwritten
// under it is a miss, never a mixed value. The newest pair is one word too. Serial 0 is the zero alignment before any publication.
inline uint64_t packYZ(double y, double z) noexcept {
    const float fy = static_cast<float>(y), fz = static_cast<float>(z);
    uint32_t by = 0, bz = 0;
    std::memcpy(&by, &fy, sizeof(by));
    std::memcpy(&bz, &fz, sizeof(bz));
    return (static_cast<uint64_t>(by) << 32) | bz;
}
inline void unpackYZ(uint64_t v, double* y, double* z) noexcept {
    const uint32_t by = static_cast<uint32_t>(v >> 32), bz = static_cast<uint32_t>(v);
    float fy = 0.0f, fz = 0.0f;
    std::memcpy(&fy, &by, sizeof(fy));
    std::memcpy(&fz, &bz, sizeof(fz));
    *y = fy;
    *z = fz;
}
struct SeatedHistory {
    static constexpr uint32_t kN = 256;
    struct Slot {
        std::atomic<uint32_t> serial{~0u};
        std::atomic<uint64_t> yz{0};
    };
    Slot slot[kN];
    std::atomic<uint64_t> newestYZ{0};
    void record(uint32_t serial, double seatedY, double seatedZ) noexcept {
        Slot& s = slot[serial % kN];
        const uint64_t v = packYZ(seatedY, seatedZ);
        s.serial.store(~0u, std::memory_order_release);   // invalid while the word changes
        s.yz.store(v, std::memory_order_release);
        s.serial.store(serial, std::memory_order_release);
        newestYZ.store(v, std::memory_order_release);
    }
    struct Found {
        bool reported = false;   // the runtime reported a serial (a word above 0)
        bool matched = false;    // ... and the history still holds it
        double y = 0.0, z = 0.0;
    };
    // word: the frame input's applied-alignment word (0: not reported, else the serial plus 1).
    Found find(uint32_t word) const noexcept {
        Found f;
        if (word == 0) return f;
        f.reported = true;
        const uint32_t serial = word - 1u;
        if (serial == 0) {
            f.matched = true;
            return f;
        }
        const Slot& s = slot[serial % kN];
        if (s.serial.load(std::memory_order_acquire) != serial) return f;
        const uint64_t v = s.yz.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.serial.load(std::memory_order_relaxed) != serial) return f;
        double y = 0.0, z = 0.0;
        unpackYZ(v, &y, &z);
        f.matched = std::isfinite(y) && std::isfinite(z);
        if (f.matched) {
            f.y = y;
            f.z = z;
        }
        return f;
    }
    void newest(double* y, double* z) const noexcept { unpackYZ(newestYZ.load(std::memory_order_acquire), y, z); }
};

// A second of ticks with the weight up, for the 1 s line.
struct Second {
    uint32_t ticks = 0;
    double aimDegSum = 0.0, headDegSum = 0.0, forwardCmSum = 0.0, upCmSum = 0.0, maxCm = 0.0, wSum = 0.0, wMax = 0.0;
    void add(const Out& o, double aimPitchRad, double headPitchRad) noexcept {
        if (!(o.w > 0.0)) return;
        ++ticks;
        aimDegSum += aimPitchRad / kAdsDeg;
        headDegSum += headPitchRad / kAdsDeg;
        forwardCmSum += 100.0 * o.forwardM;
        upCmSum += 100.0 * o.upM;
        maxCm = std::max(maxCm, 100.0 * std::sqrt(o.forwardM * o.forwardM + o.upM * o.upM));
        wSum += o.w;
        wMax = std::max(wMax, o.w);
    }
    bool any() const noexcept { return ticks > 0; }
    double mean(double sum) const noexcept { return ticks ? sum / static_cast<double>(ticks) : 0.0; }
    void clear() noexcept { *this = Second{}; }
};

}  // namespace f137
}  // namespace nofoot
}  // namespace edvr
