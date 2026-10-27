// F138 (native on-foot VR): the view holds its height over the neck while the look is pitched. Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the pin hook and the frame thread use. Journal: the arc doc, 2026-10-27, F137 flown / F138.
//
// The F137 flight (unarmed, looking up, mouse look and look to aim): the game's own first-person camera sinks against the animated neck as the look pitches up (the game's
// camera over the neck +5.6 cm level, -8.6 cm at +40 deg under look to aim, -14 to -49 cm at +37 to +70 deg under mouse look), while the pilot's headset rose or stood
// still, so it is not his head; the view then sits at the neck, inside the torso. F94a's raise holds the view the key's height over the neck from a settled level stance;
// this adds back what the camera sank since that stance, in the same raise (so the P1 hook takes it out of the pin's camera by the alignment serial, as it does the raise,
// and the body is placed from the game's own camera as before).
//
// The reference is taken in the pin hook on every call where the settle publishes an UNARMED target from an upright stance: the published target, G of that call, the
// animated neck's height, the headset's LOCAL height and the LOCAL pose's generation. G of a call is the game's camera (EDVR's offsets out, not pitch-normalised) less the
// animated neck less the physical headset's height in LOCAL: what the game's camera does over the neck beyond what the pilot's own head does (his real head motion stays in
// the view). While the look is pitched (w = smoothstep(20, 30 deg, |aim pitch|) since F139, the FP root's pitch) the raise's target is the stance's plus w times the sink (the
// reference's G less this call's), clamped to +-40 cm. Looking up applies under mouse look and look to aim; looking down only under mouse look (under look to aim the
// F109 gate held the raise whatever the pitch, as flown: w is 0 there, and those calls are not the instrument's). The settle's refusal for the aim's pitch (from 20 deg
// since F139) applies with nothing drawn only: with an item drawn the settle takes the pitched camera as it did before F138 (the armed target is an absolute height). The end of a pitched episode
// publishes the stance's target once only when this tracker published a target during it and nothing is drawn at the edge; otherwise it publishes nothing ("not needed").
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f138 {

// F139: the pilot's resting mouse aim was -10.5 to -12 deg with his head level, so the 10 deg refusal held every unarmed settle and F138 never had a reference ("no reference"
// on 2823 calls); the flown bins put the camera's sink at -1 to +3.5 cm at 10 to 20 deg and +5 cm at 20 to 30. The settle's refusal (so the reference's own gate) and the weight
// start at 20 deg; the instrument (kPitchedDeg) still counts a call from 10.
constexpr double kWeightLoDeg = 20.0, kWeightHiDeg = 30.0;   // w: 0 up to 20 deg of aim pitch, 1 from 30 (smoothstep)
constexpr double kSettleGateDeg = 20.0;                      // the settle refuses (nothing drawn) from this |aim pitch|; look to aim's own gate (F109, 10 deg) stays in the hook
constexpr double kPitchedDeg = 10.0;                         // a call at this |aim pitch| or more is the instrument's
constexpr double kStanceGuardM = 0.03;                       // the animated neck within this of the reference's: the same stance (a crouch, the fists' hunch move it)
constexpr double kSinkMinM = -0.40, kSinkMaxM = 0.40;        // the sink added, clamped (looking down unarmed the camera rose 12 to 17 cm over the neck)

inline double smoothstep(double lo, double hi, double x) noexcept {
    if (!std::isfinite(x)) return 0.0;
    const double t = std::min(1.0, std::max(0.0, (x - lo) / (hi - lo)));
    return t * t * (3.0 - 2.0 * t);
}
// The pitch's weight, mode aside: 0 for an aim pitch that is not a number.
inline double pitchWeight(double aimPitchDeg) noexcept {
    return std::isfinite(aimPitchDeg) ? smoothstep(kWeightLoDeg, kWeightHiDeg, std::fabs(aimPitchDeg)) : 0.0;
}
// Looking down under look to aim stays as flown (the F109 gate held the raise whatever the pitch): not F138's.
inline bool lookToAimDown(double aimPitchDeg, bool lookToAim) noexcept { return lookToAim && aimPitchDeg < 0.0; }
// The weight of the pitched look: the pitch's, 0 for a look down under look to aim.
inline double weight(double aimPitchDeg, bool lookToAim) noexcept { return lookToAimDown(aimPitchDeg, lookToAim) ? 0.0 : pitchWeight(aimPitchDeg); }
// The instrument counts a call at |aim pitch| of kPitchedDeg or more (not a look down under look to aim); the settle refuses from gateDeg (the hook passes kSettleGateDeg, F139:
// 20 deg), with nothing drawn only.
inline bool pitched(double aimPitchDeg, bool lookToAim) noexcept {
    return std::isfinite(aimPitchDeg) && std::fabs(aimPitchDeg) >= kPitchedDeg && !lookToAimDown(aimPitchDeg, lookToAim);
}
inline bool settleRefused(double aimPitchDeg, double gateDeg, bool itemDrawn) noexcept {
    return !itemDrawn && std::isfinite(aimPitchDeg) && std::fabs(aimPitchDeg) >= gateDeg;
}

// G: the game's camera (E, raw) over the animated neck, less the physical headset's height in LOCAL (all metres).
inline double gOf(double rawEyeY, double neckY, double localHeadY) noexcept { return rawEyeY - neckY - localHeadY; }
inline double clampSink(double sinkM) noexcept { return std::min(kSinkMaxM, std::max(kSinkMinM, sinkM)); }
inline double sinkOf(double refG, double gNow) noexcept { return clampSink(refG - gNow); }
inline double targetOf(double refTarget, double w, double sinkM) noexcept { return refTarget + w * sinkM; }
inline bool stanceHolds(double neckY, double refNeckY) noexcept { return std::isfinite(neckY) && std::isfinite(refNeckY) && std::fabs(neckY - refNeckY) <= kStanceGuardM; }

// Why a pitched call (w > 0) did not add the sink; kApplied added it, kItemDrawn measured it and published nothing.
enum Reason : int {
    kApplied = 0, kItemDrawn = 1, kNoReference = 2, kPoseInvalid = 3, kGeneration = 4, kStance = 5, kAds = 6, kCameraAway = 7, kSettleOwned = 8, kInputs = 9, kReasons = 10
};
inline const char* reasonName(int r) noexcept {
    switch (r) {
        case kApplied: return "applied";
        case kItemDrawn: return "an item drawn";
        case kNoReference: return "no reference";
        case kPoseInvalid: return "LOCAL pose invalid";
        case kGeneration: return "LOCAL pose generation changed";
        case kStance: return "stance changed";
        case kAds: return "ADS";
        case kCameraAway: return "camera away";
        case kSettleOwned: return "the settle published";
        case kInputs: return "an input is not a number";
        default: return "?";
    }
}

struct Stance {
    bool have = false;
    double target = 0.0;       // the raise's target the settle published (m)
    double g = 0.0;            // G of that call
    double neckY = 0.0;        // the animated neck's height
    double localY = 0.0;       // the headset's LOCAL height
    uint32_t generation = 0;   // the LOCAL pose's generation
};
struct In {
    double aimPitchDeg = std::nan("");   // the FP root's pitch, nose up + (not a number: unknown this call)
    bool settleOwned = false;            // the existing code published a target this call (the settle's, or ADS's zero): it owns the raise
    bool lookToAim = false;              // look to aim drives the aim (engaged and not held): a look down stays as flown
    bool itemDrawn = false;              // armed, or the weapon eye's weight up
    bool ads = false;                    // aiming down sights (the detach weight above 0)
    bool cameraAway = false;
    bool poseValid = false;              // the LOCAL pose is valid
    uint32_t generation = 0;
    double rawEyeY = 0.0, neckY = 0.0, localHeadY = 0.0;
};
struct Out {
    bool pitched = false;        // |aim pitch| at the instrument's size
    bool considered = false;     // w > 0: a reason applies
    double w = 0.0;
    int reason = kApplied;
    bool sinkValid = false;      // the sink was measured (applied, or measured only)
    double sinkM = 0.0;          // clamped
    double headRiseM = 0.0;      // the headset's own height since the stance (m); valid with sinkValid
    bool publish = false;        // the hook publishes publishM as the raise's target
    double publishM = 0.0;
    double addedM = 0.0;         // w times the sink, when applied
    bool returned = false;       // the end of an episode this tracker published in, nothing drawn: the stance's target, once
    bool returnRefused = false;  // ... the stance changed, or no reference: nothing published
    bool notNeeded = false;      // the end of an episode in which it published nothing (an item drawn, the settle's target, none added): nothing to give back
};

struct Tracker {
    Stance ref;
    bool added = false;     // this tracker published a target (the sink added) during the episode: the end of it gives the stance's back
    bool episode = false;   // a pitched call (w > 0) since the last edge
    void reset() noexcept {
        ref = Stance{};
        added = false;
        episode = false;
    }
    // A settled unarmed upright stance was published (the hook calls this beside f94TargetPublish). A pose that is not valid drops the reference: a stale one must not be used.
    void noteStance(double target, double rawEyeY, double neckY, double localHeadY, uint32_t generation, bool poseValid) noexcept {
        Stance s;
        s.have = poseValid && std::isfinite(target) && std::isfinite(rawEyeY) && std::isfinite(neckY) && std::isfinite(localHeadY);
        if (s.have) {
            s.target = target;
            s.g = gOf(rawEyeY, neckY, localHeadY);
            s.neckY = neckY;
            s.localY = localHeadY;
            s.generation = generation;
        }
        ref = s;
        added = false;     // the settle published a target of its own
        episode = false;
    }
    Out step(const In& in) noexcept {
        Out o;
        if (!std::isfinite(in.aimPitchDeg)) return o;   // the aim is unknown this call (G absent): nothing changes, nothing flaps
        o.pitched = pitched(in.aimPitchDeg, in.lookToAim);
        o.w = weight(in.aimPitchDeg, in.lookToAim);
        if (in.settleOwned) added = false;               // its publication replaced anything added
        if (!(o.w > 0.0)) {
            // the episode is over (the look came level, or turned down under look to aim): the first call gives the raise back to the stance's, exactly, only when this tracker
            // published in the episode and nothing is drawn now (an armed target is an absolute height of its own); not while the camera is away or in the sights
            if (episode && !in.cameraAway && !in.ads) {
                episode = false;
                if (added && !in.settleOwned && !in.itemDrawn) {
                    added = false;
                    if (ref.have && stanceHolds(in.neckY, ref.neckY)) {
                        o.publish = true;
                        o.publishM = ref.target;
                        o.returned = true;
                    } else {
                        o.returnRefused = true;
                    }
                } else {
                    added = false;
                    o.notNeeded = true;
                }
            }
            return o;
        }
        o.considered = true;
        episode = true;
        if (in.cameraAway) o.reason = kCameraAway;
        else if (in.ads) o.reason = kAds;
        else if (in.settleOwned) o.reason = kSettleOwned;
        else if (!in.poseValid) o.reason = kPoseInvalid;
        else if (!std::isfinite(in.rawEyeY) || !std::isfinite(in.neckY) || !std::isfinite(in.localHeadY)) o.reason = kInputs;
        else if (!ref.have) o.reason = kNoReference;
        else if (in.generation != ref.generation) o.reason = kGeneration;   // a recentre: the reference's LOCAL height is another frame's
        else if (!stanceHolds(in.neckY, ref.neckY)) o.reason = kStance;
        else {
            o.sinkValid = true;
            o.sinkM = sinkOf(ref.g, gOf(in.rawEyeY, in.neckY, in.localHeadY));
            o.headRiseM = in.localHeadY - ref.localY;
            if (in.itemDrawn) {
                o.reason = kItemDrawn;   // measured only
            } else {
                o.reason = kApplied;
                o.publish = true;
                o.addedM = o.w * o.sinkM;
                o.publishM = targetOf(ref.target, o.w, o.sinkM);
                added = true;
            }
        }
        return o;
    }
};

// ---- the instrument -------------------------------------------------------------------------------------------------------------------------------------------
// One pitched pin call's figures, and the window the hook adds them to (one writer) and the frame thread takes once a second.
constexpr int kBins = 18, kBinWidthDeg = 10, kBinLoDeg = -90;   // [-90,-80) ... [80,90]
inline int binOf(double aimDeg) noexcept {
    if (!std::isfinite(aimDeg)) return 0;
    const int b = static_cast<int>(std::floor((aimDeg - kBinLoDeg) / kBinWidthDeg));
    return b < 0 ? 0 : b >= kBins ? kBins - 1 : b;
}
struct Call {
    double aimDeg = 0.0, headDeg = 0.0;
    bool headValid = false;
    bool lookToAim = false, itemDrawn = false;
    Out out;
    double carriedUpCm = 0.0, carriedFwdCm = 0.0, viewOverNeckCm = 0.0;   // EDVR's carried raise (the neck reference's and F137's up), F137's forward, the view as drawn over the neck
    bool settleRefusedAim = false;                                       // the settle refused this call for the aim's pitch (F138's gate; look to aim's is F109's, counted apart)
};
struct Bin {
    uint32_t n = 0, sinkN = 0, headN = 0;
    double sinkSum = 0.0, upSum = 0.0, fwdSum = 0.0, headSum = 0.0;
};
struct Group {   // one mode and one item class, by 10 deg of aim pitch
    Bin bin[kBins];
    bool any() const noexcept {
        for (const Bin& b : bin)
            if (b.n) return true;
        return false;
    }
    void merge(const Group& o) noexcept {
        for (int i = 0; i < kBins; ++i) {
            bin[i].n += o.bin[i].n;
            bin[i].sinkN += o.bin[i].sinkN;
            bin[i].headN += o.bin[i].headN;
            bin[i].sinkSum += o.bin[i].sinkSum;
            bin[i].upSum += o.bin[i].upSum;
            bin[i].fwdSum += o.bin[i].fwdSum;
            bin[i].headSum += o.bin[i].headSum;
        }
    }
};
struct Window {
    uint32_t calls = 0, modeCalls[2] = {}, itemCalls = 0;   // pitched calls; by mode (0 mouse look, 1 look to aim); with an item drawn
    double aimSum = 0.0, aimMin = 1e9, aimMax = -1e9, headSum = 0.0;
    uint32_t headN = 0;
    uint32_t sinkN = 0, headRiseN = 0;
    double sinkSum = 0.0, sinkMax = -1e9, headRiseSum = 0.0;
    uint32_t consideredN = 0;   // w > 0
    double wSum = 0.0, wMax = 0.0;
    uint32_t appliedN = 0, measuredN = 0, why[kReasons] = {};
    double addedSum = 0.0, addedMax = -1e9;
    double upSum = 0.0, fwdSum = 0.0, viewSum = 0.0;
    uint32_t inside = 0, insideOf = 0;
    uint32_t settleRefusedAim = 0;
    uint32_t returned = 0, returnRefused = 0, notNeeded = 0;
    Group group[2][2];   // [look to aim][item drawn]
    void clear() noexcept { *this = Window{}; }
    void add(const Call& c) noexcept {
        if (!c.out.pitched) return;
        ++calls;
        ++modeCalls[c.lookToAim ? 1 : 0];
        if (c.itemDrawn) ++itemCalls;
        aimSum += c.aimDeg;
        aimMin = std::min(aimMin, c.aimDeg);
        aimMax = std::max(aimMax, c.aimDeg);
        if (c.headValid) {
            headSum += c.headDeg;
            ++headN;
        }
        if (c.out.sinkValid) {
            ++sinkN;
            sinkSum += 100.0 * c.out.sinkM;
            sinkMax = std::max(sinkMax, 100.0 * c.out.sinkM);
            ++headRiseN;
            headRiseSum += 100.0 * c.out.headRiseM;
        }
        if (c.out.considered) {
            ++consideredN;
            wSum += c.out.w;
            wMax = std::max(wMax, c.out.w);
            const int r = c.out.reason >= 0 && c.out.reason < kReasons ? c.out.reason : kInputs;
            ++why[r];
            if (r == kApplied) {
                ++appliedN;
                addedSum += 100.0 * c.out.addedM;
                addedMax = std::max(addedMax, 100.0 * c.out.addedM);
            } else if (r == kItemDrawn) {
                ++measuredN;
            }
        }
        upSum += c.carriedUpCm;
        fwdSum += c.carriedFwdCm;
        viewSum += c.viewOverNeckCm;
        if (c.settleRefusedAim) ++settleRefusedAim;
        Bin& b = group[c.lookToAim ? 1 : 0][c.itemDrawn ? 1 : 0].bin[binOf(c.aimDeg)];
        ++b.n;
        if (c.out.sinkValid) {
            ++b.sinkN;
            b.sinkSum += 100.0 * c.out.sinkM;
            ++b.headN;
            b.headSum += 100.0 * c.out.headRiseM;
        }
        b.upSum += c.carriedUpCm;
        b.fwdSum += c.carriedFwdCm;
    }
    void addInside(bool isInside) noexcept {   // the close of a pitched call: F97a's test on the drawn eye
        ++insideOf;
        if (isInside) ++inside;
    }
    void addReturn(const Out& o) noexcept {
        if (o.returned) ++returned;
        if (o.returnRefused) ++returnRefused;
        if (o.notNeeded) ++notNeeded;
    }
    bool any() const noexcept { return calls > 0 || returned > 0 || returnRefused > 0 || notNeeded > 0; }
    static double mean(double sum, uint32_t n) noexcept { return n ? sum / static_cast<double>(n) : 0.0; }
    // "mouse look", "look to aim", or both counts
    void modeText(char* buf, size_t cap) const noexcept {
        if (modeCalls[0] && modeCalls[1]) std::snprintf(buf, cap, "mixed (mouse look %u calls and look to aim %u)", modeCalls[0], modeCalls[1]);
        else std::snprintf(buf, cap, "%s", modeCalls[1] ? "look to aim" : "mouse look");
    }
};

// The reasons a pitched call did not add the sink with their counts ("no reference 3, stance changed 2"), "none" when every considered call added it or only measured it.
inline void notAppliedText(const uint32_t why[kReasons], char* buf, size_t cap) noexcept {
    size_t n = 0;
    if (cap) buf[0] = '\0';
    for (int r = kNoReference; r < kReasons; ++r) {
        if (!why[r]) continue;
        const int w = std::snprintf(buf + n, cap - n, "%s%s %u", n ? ", " : "", reasonName(r), why[r]);
        if (w < 0 || static_cast<size_t>(w) >= cap - n) break;
        n += static_cast<size_t>(w);
    }
    if (n == 0 && cap) std::snprintf(buf, cap, "none");
}
// A group's 10 s bins: "[lo,hi) n sink up fwd head" for each bin with a call, "--" where a mean has no data. Stops before cap; returns the length (0: no bin has a call).
inline size_t binsText(const Group& g, char* buf, size_t cap) noexcept {
    size_t n = 0;
    if (cap) buf[0] = '\0';
    for (int i = 0; i < kBins; ++i) {
        const Bin& b = g.bin[i];
        if (!b.n) continue;
        char sink[16], head[16];
        if (b.sinkN) std::snprintf(sink, sizeof(sink), "%+.1f", b.sinkSum / static_cast<double>(b.sinkN));
        else std::snprintf(sink, sizeof(sink), "--");
        if (b.headN) std::snprintf(head, sizeof(head), "%+.1f", b.headSum / static_cast<double>(b.headN));
        else std::snprintf(head, sizeof(head), "--");
        const double nn = static_cast<double>(b.n);
        const int lo = kBinLoDeg + i * kBinWidthDeg;
        const int w = std::snprintf(buf + n, cap - n, "%s[%d,%d) %u %s %+.1f %+.1f %s", n ? "; " : "", lo, lo + kBinWidthDeg, b.n, sink, b.upSum / nn, b.fwdSum / nn, head);
        if (w < 0 || static_cast<size_t>(w) >= cap - n) {
            buf[n] = '\0';
            break;
        }
        n += static_cast<size_t>(w);
    }
    return n;
}

}  // namespace f138
}  // namespace nofoot
}  // namespace edvr
