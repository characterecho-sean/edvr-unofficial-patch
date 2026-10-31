// F194 (native on-foot VR): look to aim's eye height settles as mouse look's does (behaviour since F195b); the view's height at an edge of look to aim, measured. Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the frame thread and the pin hook use. Journal: the arc doc, F194.
//
// Part 1 (look to aim aiming at the middle of the picture, with its key) was REMOVED on 2026-10-31 (F194b). The tester (Quest 3, rc.4) reported look to aim's aim about 12.9 deg above where they look, and a
// Quest 3's vertical frustum is not symmetric (up 0.9657, down 1.4281 as tangents: the middle of the picture 13.0 deg below the pose's forward axis; a Pimax Crystal Super's, 1.2648 both, is level), so part 1
// took the ray from the head through the middle of the picture as the direction to centre the aim on, in place of the forward axis. The pilot flew it on his Quest 3 (edvr_gfx_20261031_134656) and said it was
// quite a bit worse: "the aim dropped". It was refuted: at four engages after a settled mouse aim the forward axis stood +0.77, +4.46, +4.79 and +5.55 deg from the aim, the ray moved the aim down 6.6 to 11.4
// deg instead, and his gaze offset is about 5 deg on his Quest against the picture's centre 12.2 deg below the axis (and 10.5 to 12 deg on his Pimax, with a symmetric frustum), so no headset value predicts it. F195
// took its place (the engage keeps the aim's pitch where the mouse left it) and flew well; the pilot said to remove the key. Gone with it: the picture-centre pitch, the frustum read and its log line, the ray,
// centreOffset's picture parameter (it is adsHeadAngles again). The runtime's "view fov:" line (src/openxr/view_fov_note.h) stays: a measurement with no key.
//
// Part 2 (look to aim's eye height settles as mouse look's does) is BEHAVIOUR since F195b: its test key (experimental.native_on_foot_look_to_aim_eye_height_as_mouse_look) was RETIRED on 2026-10-31 on the pilot's
// word ("Retire both", after F195 flew OK), and look to aim's own 10 deg settle gate is gone. Look to aim refused the stance's settle (F98c: the only publisher of the raise that holds the view's height over the
// neck) whenever |the carried aim pitch| was 10 deg or more (F109). That pitch is the account of what the game applied for EDVR's asks since the engage (AdsAimMap::pitchSeat), not the game's aim: with the mouse
// aim resting at -11 and a level head an engage that centred the aim carried +11 and the settle was refused with the aim level (the pilot's "my view dropped"). Mouse look's gate is F139's 20 deg on the FP root's
// pitch, nothing drawn only (nofoot::f138::settleRefused), and it already ran after look to aim's for both modes; it is the only gate now, for look to aim as for mouse look. (F195, which keeps the aim's pitch at an
// engage, means the carried pitch no longer starts out at the mouse aim's offset either.)
//
// The instruments (kept): the hook's window of the settle's fate, the frame thread's second, the 1 s line, and the probe of the view's height at an engage and a disengage.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f194 {

// ---- the pin hook's window ----------------------------------------------------------------------------------------------------------------------------------------------------------------------

// One pin call while look to aim drives the aim: the drawn eye over the animated neck (m, not a number: unknown), the raise published (m, not a number: none yet), and the settle's fate (F138/F139's 20 deg gate
// refused it, or a stance was taken).
struct Call {
    double eyeOverNeckM = std::nan(""), raiseM = std::nan("");
    bool f139Refused = false, taken = false;
};
struct Window {
    uint32_t calls = 0, eyeN = 0, raiseN = 0, f139Refused = 0, taken = 0;
    double eyeSum = 0.0, raiseSum = 0.0;
    void add(const Call& c) noexcept {
        ++calls;
        if (std::isfinite(c.eyeOverNeckM)) {
            ++eyeN;
            eyeSum += c.eyeOverNeckM;
        }
        if (std::isfinite(c.raiseM)) {
            ++raiseN;
            raiseSum += c.raiseM;
        }
        if (c.f139Refused) ++f139Refused;
        if (c.taken) ++taken;
    }
    bool any() const noexcept { return calls > 0; }
    void clear() noexcept { *this = Window{}; }
};

// ---- the frame thread's second -------------------------------------------------------------------------------------------------------------------------------------------------------------------

// Per tick while look to aim is engaged: the headset's pitch in LOCAL (the physical head), the carried aim pitch (the account), the game's aim pitch (the FP root, when fresh) and the reported head's pitch from
// the aim (degrees; the seat convention, nose up +; F195: what the engage kept, about 0 with the key off).
struct Second {
    uint32_t ticks = 0, localN = 0, rootN = 0, offN = 0;
    double localSum = 0.0, carriedSum = 0.0, rootSum = 0.0, offSum = 0.0;
    void add(bool localOk, double localDeg, double carriedDeg, bool rootOk, double rootDeg, bool offOk, double offDeg) noexcept {
        ++ticks;
        carriedSum += std::isfinite(carriedDeg) ? carriedDeg : 0.0;
        if (localOk && std::isfinite(localDeg)) { ++localN; localSum += localDeg; }
        if (rootOk && std::isfinite(rootDeg)) { ++rootN; rootSum += rootDeg; }
        if (offOk && std::isfinite(offDeg)) { ++offN; offSum += offDeg; }
    }
    bool any() const noexcept { return ticks > 0; }
    double mean(double sum, uint32_t n) const noexcept { return n ? sum / n : 0.0; }
};

// ---- the lines -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

// The 1 s line while look to aim is engaged. (tools\edvr_log.py reads these formats from this file: keep a quoted copy of a line out of the comments, where the reader's search would find it first. It reads the
// lines of the builds before F194b and before F195b too: the first began with the picture-centre pitch in force and gave the forward axis's and the ray's pitch from the aim; the second had look to aim's own
// 10 deg gate in the settle field, "look to aim's 10 deg gate would have refused (skipped) N of M pin calls, F138/F139's 20 deg gate refused N, taken N".)
inline int formatSecond(char* buf, size_t n, const Second& s, const Window& w) noexcept {
    const double ew = w.eyeN ? static_cast<double>(w.eyeN) : 1.0, rw = w.raiseN ? static_cast<double>(w.raiseN) : 1.0;
    return std::snprintf(buf, n,
                         "native on foot look to aim height (F194) 1 s: the headset's pitch (LOCAL) mean %+.1f deg; "
                         "carried aim pitch mean %+.1f deg (the game's aim %+.1f deg); the reported head's pitch from the aim mean %+.1f deg; "
                         "the settle: F138/F139's 20 deg gate refused %u, taken %u of %u pin calls; the published raise mean %+.1f cm; "
                         "the drawn eye over the animated neck mean %+.1f cm",
                         s.mean(s.localSum, s.localN), s.mean(s.carriedSum, s.ticks), s.mean(s.rootSum, s.rootN), s.mean(s.offSum, s.offN),
                         w.f139Refused, w.taken, w.calls, w.raiseN ? w.raiseSum / rw * 100.0 : 0.0, w.eyeN ? w.eyeSum / ew * 100.0 : 0.0);
}

// ---- the view's height at an engage and a disengage -------------------------------------------------------------------------------------------------------------------------------------------

// The pilot's "my view dropped", as a number. The view's height is the drawn eye's vertical in the character's model (pc.f97EyeDrawn[1]: the job camera with every add EDVR makes, the head's own motion
// included, in the model whose origin stands at the feet), read once a fresh game frame on foot at the hip with the commander's camera: for a still pilot, the frame that best stands still (against the
// ground the model is placed in; the sights and the camera away are not read). At each edge of look to aim one probe takes the frame before and the first frames at or after 0.5 s, 1 s and 3 s,
// and says the change and the carried aim pitch at each. A moment with no frame inside its grace reads "not read": an engage that moves nothing prints a small change, not nothing.
struct HeightProbe {
    static constexpr int kN = 3;
    static constexpr uint64_t kLateMs = 400;    // a frame read at most this long after its moment counts for it
    static constexpr uint64_t kFreshMs = 250;   // a frame this old, or less, is a fresh read
    static constexpr uint64_t kBeforeMs = 500;  // the frame before an edge: the last read inside this long
    bool active = false, engage = true, haveBefore = false, cut = false;
    uint64_t t0 = 0;
    double beforeY = std::nan(""), beforePitch = std::nan("");
    bool got[kN] = {};
    double y[kN] = {}, pitch[kN] = {};
    int next = 0;
    static uint64_t atMs(int i) noexcept { return i == 0 ? 500u : i == 1 ? 1000u : 3000u; }
    void begin(bool isEngage, uint64_t now, bool beforeOk, double beforeHeightM, double carriedDeg) noexcept {
        *this = HeightProbe{};
        active = true;
        engage = isEngage;
        t0 = now;
        haveBefore = beforeOk && std::isfinite(beforeHeightM);
        beforeY = haveBefore ? beforeHeightM : std::nan("");
        beforePitch = carriedDeg;
    }
    // One frame-thread tick: sampleOk, a fresh read (y metres) at this moment; carriedDeg the carried aim pitch. True when the last moment is resolved.
    bool tick(uint64_t now, bool sampleOk, double heightM, double carriedDeg) noexcept {
        if (!active) return false;
        while (next < kN) {
            const uint64_t due = t0 + atMs(next);
            if (now < due) break;
            if (sampleOk && std::isfinite(heightM) && now - due <= kLateMs) {
                got[next] = true;
                y[next] = heightM;
                pitch[next] = carriedDeg;
                ++next;
                break;   // one read serves one moment: a stall that left two due serves the second on the next tick, if it is still inside its grace
            }
            if (now - due > kLateMs) {
                ++next;
                continue;
            }
            break;   // inside the grace, no fresh read yet
        }
        return next >= kN;
    }
    // The next edge came first: what is not resolved reads "not read (cut short)".
    void cutShort() noexcept {
        cut = true;
        next = kN;
    }
    int format(char* buf, size_t n) const noexcept {
        char before[96], after[kN][128];
        if (haveBefore) std::snprintf(before, sizeof(before), "%.3f m, carried aim pitch %+.1f deg", beforeY, beforePitch);
        else std::snprintf(before, sizeof(before), "not read");
        for (int i = 0; i < kN; ++i) {
            if (!got[i]) std::snprintf(after[i], sizeof(after[i]), "%s", cut ? "not read (cut short by the next edge)" : "not read");
            else if (haveBefore) std::snprintf(after[i], sizeof(after[i]), "%+.1f cm (%.3f m), carried aim pitch %+.1f deg", (y[i] - beforeY) * 100.0, y[i], pitch[i]);
            else std::snprintf(after[i], sizeof(after[i]), "%.3f m (no frame before to change from), carried aim pitch %+.1f deg", y[i], pitch[i]);
        }
        return std::snprintf(buf, n,
                             "native on foot look to aim height (F194) %s: the view's height (the drawn eye above the character's origin, the hip frames read): before %s; after 0.5 s %s; after 1 s %s; after 3 s %s",
                             engage ? "engage" : "disengage", before, after[0], after[1], after[2]);
    }
};

}  // namespace f194
}  // namespace nofoot
}  // namespace edvr
