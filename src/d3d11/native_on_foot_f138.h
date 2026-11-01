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
//
// F170, after the flight edvr_gfx_20261028_215408: a pilot who holsters and looks straight up before the first unarmed settle of the flight (it needs 0.3 s of stillness, level) has no
// reference, so the sink is never added and the view stays inside the torso (INSIDE 90 of 90 frames, no reference on 1533 calls). The tracker now also keeps a RUNNING reference: every
// call where nothing is drawn, the aim is under the settle's gate, the stance is upright and the pose is valid takes the raise's target in force and G, smoothed over about 0.3 s, with the
// same stance guard. A settled reference wins when it is usable; the running one stands in when there is none or the settled one is refused (the LOCAL pose's generation moved, the stance
// changed). It clears with the reset (the on-foot camera's edge). The 1 s line names the reference each call used: settled, running or none. And the 1 s line no longer repeats a window
// nothing has written to since it was taken (Mailbox).
//
// F171, after the flight edvr_gfx_20261029_043951 (build d64b85ac): the running reference drove the raise to its clamp. Unarmed, mouse look, aim +52 to +85 deg, the reference used was
// "running" on every applied call, the sink read -25.9 to -40.0 cm (negative), the raise it added ran to -50.0 cm on 90 of 90 ticks, and the game's camera over the neck read +14 to +40 cm
// against it. A call whose reference is the running one now MEASURES the sink and publishes nothing (it does not set `added`): its own reason, "running reference, measured only". The
// settled path is unchanged. The line also says the reference's age, target and G, G now, and the mean target published (referenceDetailText). Why the camera moved against the raise is
// F171's other instrument (native_on_foot_f171c.h).
#pragma once

#include <algorithm>
#include <atomic>
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
constexpr double kSettleGateDeg = 20.0;                      // the settle refuses (nothing drawn) from this |aim pitch|, in both modes (look to aim's own 10 deg gate, F109's, was retired in F195b)
constexpr double kPitchedDeg = 10.0;                         // a call at this |aim pitch| or more is the instrument's
constexpr double kStanceGuardM = 0.03;                       // the animated neck within this of the reference's: the same stance (a crouch, the fists' hunch move it)
constexpr double kSinkMinM = -0.40, kSinkMaxM = 0.40;        // the sink added, clamped (looking down unarmed the camera rose 12 to 17 cm over the neck)
constexpr double kRunSmoothS = 0.3;                          // F170: the running reference follows G and the target over about this long (its 95% time)
constexpr double kRunMaxGapS = 1.0;                          // F170: a gap between two samples longer than this starts the running reference over (the stream paused)

inline double smoothstep(double lo, double hi, double x) noexcept {
    if (!std::isfinite(x)) return 0.0;
    const double t = std::min(1.0, std::max(0.0, (x - lo) / (hi - lo)));
    return t * t * (3.0 - 2.0 * t);
}
// The pitch's weight, mode aside: 0 for an aim pitch that is not a number.
inline double pitchWeight(double aimPitchDeg) noexcept {
    return std::isfinite(aimPitchDeg) ? smoothstep(kWeightLoDeg, kWeightHiDeg, std::fabs(aimPitchDeg)) : 0.0;
}
// Looking down under look to aim stays as flown (F109's own gate held the raise whatever the pitch until F195b retired it; the weight stays 0 there): not F138's.
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

// Why a pitched call (w > 0) did not add the sink; kApplied added it, kItemDrawn measured it and published nothing. F171: kRunningMeasured also measured it and published nothing: the
// reference was the running one.
enum Reason : int {
    kApplied = 0, kItemDrawn = 1, kNoReference = 2, kPoseInvalid = 3, kGeneration = 4, kStance = 5, kAds = 6, kCameraAway = 7, kSettleOwned = 8, kInputs = 9, kRunningMeasured = 10, kReasons = 11
};
// F170: which reference a call used. Settled: the unarmed stance the settle published; running: the smoothed one taken whenever nothing was drawn and the aim was level; none.
enum Source : int { kSrcNone = 0, kSrcSettled = 1, kSrcRunning = 2, kSources = 3 };
inline const char* sourceName(int s) noexcept {
    switch (s) {
        case kSrcSettled: return "settled";
        case kSrcRunning: return "running";
        default: return "none";
    }
}
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
        case kRunningMeasured: return "running reference, measured only";
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
    double tS = std::nan("");  // F171: when it was taken (s, the call's clock; not a number: unknown). The running one: when it started over, not its last sample
};
struct In {
    double tS = std::nan("");            // F171: the call's time (s), for the reference's age
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
    int source = kSrcNone;       // F170: the reference this call used (meaningful with reached)
    bool reached = false;        // F170: the call got as far as choosing a reference (applied, measured only, no reference, generation changed, stance changed)
    bool refValid = false;       // F171: a reference was chosen: the next four are its figures
    double refAgeS = std::nan("");   // F171: s since it was taken (not a number: unknown)
    double refTargetM = 0.0, refGM = 0.0, gNowM = 0.0;   // F171: its target and G, and G as this call forms it
    bool publish = false;        // the hook publishes publishM as the raise's target
    double publishM = 0.0;
    double addedM = 0.0;         // w times the sink, when applied
    bool returned = false;       // the end of an episode this tracker published in, nothing drawn: the stance's target, once
    bool returnRefused = false;  // ... the stance changed, or no reference: nothing published
    bool notNeeded = false;      // the end of an episode in which it published nothing (an item drawn, the settle's target, none added): nothing to give back
};

struct Tracker {
    Stance ref;             // the settled unarmed stance (noteStance)
    Stance run;             // F170: the running reference, smoothed (noteRunning)
    double runT = 0.0;      // F170: the time of the running reference's last sample (s)
    bool added = false;     // this tracker published a target (the sink added) during the episode: the end of it gives the stance's back
    bool episode = false;   // a pitched call (w > 0) since the last edge
    void reset() noexcept {
        ref = Stance{};
        run = Stance{};
        runT = 0.0;
        added = false;
        episode = false;
    }
    // A settled unarmed upright stance was published (the hook calls this beside f94TargetPublish). A pose that is not valid drops the reference: a stale one must not be used.
    void noteStance(double target, double rawEyeY, double neckY, double localHeadY, uint32_t generation, bool poseValid, double tS = std::nan("")) noexcept {
        Stance s;
        s.have = poseValid && std::isfinite(target) && std::isfinite(rawEyeY) && std::isfinite(neckY) && std::isfinite(localHeadY);
        if (s.have) {
            s.target = target;
            s.g = gOf(rawEyeY, neckY, localHeadY);
            s.neckY = neckY;
            s.localY = localHeadY;
            s.generation = generation;
            s.tS = tS;   // F171: the reference's age is read against it
        }
        ref = s;
        added = false;     // the settle published a target of its own
        episode = false;
    }
    // F170: one call where nothing is drawn, the aim is under the settle's gate, the stance is upright and not crouched (the hook decides that): the raise's target in force, G, the
    // animated neck and the headset's LOCAL height at time tS (s), smoothed into the running reference over about kRunSmoothS. The first sample, a new LOCAL generation, a neck that left
    // the stance (kStanceGuardM) and a gap of kRunMaxGapS or more start it over from this sample. A pose that is not valid drops it. Nothing is taken while this tracker's own sunk
    // target is in force (added): the target it would read is not the stance's.
    void noteRunning(double tS, double target, double rawEyeY, double neckY, double localHeadY, uint32_t generation, bool poseValid) noexcept {
        if (!poseValid) {
            run = Stance{};
            return;
        }
        if (added || !std::isfinite(tS) || !std::isfinite(target) || !std::isfinite(rawEyeY) || !std::isfinite(neckY) || !std::isfinite(localHeadY)) return;
        const double g = gOf(rawEyeY, neckY, localHeadY);
        const double dt = tS - runT;
        if (run.have && generation == run.generation && stanceHolds(neckY, run.neckY) && dt <= kRunMaxGapS) {
            if (!(dt > 0.0)) return;   // the same instant again: nothing to smooth
            const double a = 1.0 - std::exp(-dt / (kRunSmoothS / 3.0));
            run.target += a * (target - run.target);
            run.g += a * (g - run.g);
            run.neckY += a * (neckY - run.neckY);
            run.localY += a * (localHeadY - run.localY);
            runT = tS;
            return;
        }
        run.have = true;
        run.target = target;
        run.g = g;
        run.neckY = neckY;
        run.localY = localHeadY;
        run.generation = generation;
        run.tS = tS;   // F171: the age of a running reference counts from here, where it started over
        runT = tS;
    }
    // F170: the reference a call uses, and why none. The settled one when it is usable (the same LOCAL pose generation, the same stance), else the running one when that is, else none
    // with the reason the first reference that exists was refused (no reference when neither exists).
    const Stance* pick(uint32_t generation, double neckY, int* source, int* why) const noexcept {
        const auto usable = [&](const Stance& s) noexcept {
            if (!s.have) return static_cast<int>(kNoReference);
            if (generation != s.generation) return static_cast<int>(kGeneration);   // a recentre: the reference's LOCAL height is another frame's
            if (!stanceHolds(neckY, s.neckY)) return static_cast<int>(kStance);
            return static_cast<int>(kApplied);
        };
        const int settled = usable(ref);
        if (settled == kApplied) {
            *source = kSrcSettled;
            *why = kApplied;
            return &ref;
        }
        const int running = usable(run);
        if (running == kApplied) {
            *source = kSrcRunning;
            *why = kApplied;
            return &run;
        }
        *source = kSrcNone;
        *why = ref.have ? settled : run.have ? running : static_cast<int>(kNoReference);
        return nullptr;
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
                    // the target given back is the stance's (a recentre does not change it, so no generation test here, as before F170): the settled one's, else the running one's
                    const Stance* use = ref.have && stanceHolds(in.neckY, ref.neckY) ? &ref : run.have && stanceHolds(in.neckY, run.neckY) ? &run : nullptr;
                    if (use) {
                        o.publish = true;
                        o.publishM = use->target;
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
        else {
            int src = kSrcNone, why = kNoReference;
            const Stance* use = pick(in.generation, in.neckY, &src, &why);   // F170: the settled reference, else the running one
            o.reached = true;
            o.source = src;
            if (!use) {
                o.reason = why;
            } else {
                o.sinkValid = true;
                o.gNowM = gOf(in.rawEyeY, in.neckY, in.localHeadY);
                o.sinkM = sinkOf(use->g, o.gNowM);
                o.headRiseM = in.localHeadY - use->localY;
                o.refValid = true;   // F171: the reference's figures
                o.refTargetM = use->target;
                o.refGM = use->g;
                o.refAgeS = std::isfinite(in.tS) && std::isfinite(use->tS) ? in.tS - use->tS : std::nan("");
                if (in.itemDrawn) {
                    o.reason = kItemDrawn;   // measured only
                } else if (src == kSrcRunning) {
                    o.reason = kRunningMeasured;   // F171: measured only. The flight edvr_gfx_20261029_043951 ran the raise to its clamp on a running reference's sink; nothing is published, `added` stays as it was
                } else {
                    o.reason = kApplied;
                    o.publish = true;
                    o.addedM = o.w * o.sinkM;
                    o.publishM = targetOf(use->target, o.w, o.sinkM);
                    added = true;
                }
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
    bool settleRefusedAim = false;                                       // the settle refused this call for the aim's pitch (F138/F139's gate)
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
    uint32_t srcN[kSources] = {};   // F170: the considered calls that got as far as choosing a reference, by the reference they used (none, settled, running)
    double addedSum = 0.0, addedMax = -1e9;
    double upSum = 0.0, fwdSum = 0.0, viewSum = 0.0;
    uint32_t inside = 0, insideOf = 0;
    uint32_t settleRefusedAim = 0;
    uint32_t returned = 0, returnRefused = 0, notNeeded = 0;
    // F171: the reference the considered calls chose, as figures: the calls that had one, its age (s; the calls it was known for), target and G (cm), G as the call formed it (cm), the
    // source of the last one, and the target the hook published (cm; the calls that published, an episode's return among them)
    uint32_t refN = 0, refAgeN = 0, pubN = 0;
    int refSrcLast = kSrcNone;
    double refAgeSum = 0.0, refTargetSum = 0.0, refGSum = 0.0, gNowSum = 0.0, pubSum = 0.0;
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
            if (c.out.reached) ++srcN[c.out.source >= 0 && c.out.source < kSources ? c.out.source : kSrcNone];
            if (c.out.refValid) {   // F171
                ++refN;
                refSrcLast = c.out.source >= 0 && c.out.source < kSources ? c.out.source : kSrcNone;
                refTargetSum += 100.0 * c.out.refTargetM;
                refGSum += 100.0 * c.out.refGM;
                gNowSum += 100.0 * c.out.gNowM;
                if (std::isfinite(c.out.refAgeS)) {
                    ++refAgeN;
                    refAgeSum += c.out.refAgeS;
                }
            }
            if (r == kApplied) {
                ++appliedN;
                addedSum += 100.0 * c.out.addedM;
                addedMax = std::max(addedMax, 100.0 * c.out.addedM);
            } else if (r == kItemDrawn) {
                ++measuredN;
            }
        }
        if (c.out.publish) {   // F171: what the hook published this call (the sunk target, or the stance's given back)
            ++pubN;
            pubSum += 100.0 * c.out.publishM;
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
// F170: the reference the second's considered calls used, always the three counts so the line reads the same every second: "settled 0, running 29, none 0" (a call that did not get
// as far as choosing one, a camera away or ADS, is in none of them).
inline void referenceText(const uint32_t srcN[kSources], char* buf, size_t cap) noexcept {
    std::snprintf(buf, cap, "settled %u, running %u, none %u", srcN[kSrcSettled], srcN[kSrcRunning], srcN[kSrcNone]);
}
// F171: the reference's figures over the second's considered calls that had one (the means of its age, target and G, and of G as the calls formed it), the source of the last of them, and
// the mean target the hook published in the second (an episode's return among them) with the calls that published. Always every field, so the line reads the same every second.
inline void referenceDetailText(const Window& w, char* buf, size_t cap) noexcept {
    const auto mean = [](double sum, uint32_t n) noexcept { return n ? sum / static_cast<double>(n) : 0.0; };
    std::snprintf(buf, cap, "reference detail (F171; the calls that had one %u): last source %s, age %.1f s, target %+.1f cm, G %+.1f cm, G now %+.1f cm; target published %+.1f cm in %u calls", w.refN,
                  sourceName(w.refN ? w.refSrcLast : kSrcNone), mean(w.refAgeSum, w.refAgeN), mean(w.refTargetSum, w.refN), mean(w.refGSum, w.refN), mean(w.gNowSum, w.refN),
                  mean(w.pubSum, w.pubN), w.pubN);
}

// F170: the window the hook adds to (one writer) and the frame thread takes once a second. Taking a window asks the writer to clear it before its next write; a second in which the writer
// wrote nothing leaves the window as it was taken, and the next take would read it again and print the same line (the flight edvr_gfx_20261028_215408: from 21:58:40 to 21:59:00 every
// line was the one second's "0 of 29 pitched calls | not applied: no reference 27" while the pilot looked level). A take that finds the clear still pending reads an empty window.
// A write that lands between a take's copy and its request is dropped with the next clear (a call or two of a diagnostic; the line is never printed twice).
struct Mailbox {
    Window win;
    std::atomic<uint32_t> seq{0};
    std::atomic<bool> clearPending{false};
    // the hook (one writer)
    template <class Fn>
    void write(Fn&& fn) noexcept {
        seq.fetch_add(1, std::memory_order_acq_rel);
        if (clearPending.exchange(false, std::memory_order_relaxed)) win.clear();
        fn(win);
        seq.fetch_add(1, std::memory_order_release);
    }
    // the frame thread: the window since the last take, empty when nothing was written since. False when the copy tore twice (nothing is asked to clear: the next take has both).
    bool take(Window* out) noexcept {
        for (int tries = 0; tries < 2; ++tries) {
            const uint32_t s1 = seq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            std::memcpy(out, &win, sizeof(win));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) != s1) continue;
            if (clearPending.load(std::memory_order_acquire)) *out = Window{};   // nothing written since the last take: that window was printed
            else clearPending.store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
};

// The 1 s line of a window with a pitched call (or an episode's end): the look, the camera's sink, what the raise added or why not, the reference each call used (F170) and its figures
// (F171), what EDVR carried, INSIDE and the settle's refusals. item is the item's name as the line should say it (F195b: look to aim's own gate is gone, and its count with it). The log's lines
// end at 1200 bytes, 1166 of them the message: a typical second runs to about 870 and the rig's worst one (every figure at its limit, a 74-character item) to 1163.
inline int formatPitched(char* buf, size_t n, const Window& w, const char* item) noexcept {
    char mode[96], why[260], reference[96], detail[200];
    w.modeText(mode, sizeof(mode));
    notAppliedText(w.why, why, sizeof(why));
    referenceText(w.srcN, reference, sizeof(reference));
    referenceDetailText(w, detail, sizeof(detail));
    const auto mean = Window::mean;
    return std::snprintf(buf, n,
                         "native on foot pitched view (F138) 1 s: mode %s, item %s, aim pitch %+.1f deg (min %+.1f max %+.1f), head pitch %+.1f deg; the camera's sink against the "
                         "stance %+.1f cm (max %+.1f), the head's own height since the stance %+.1f cm; w %.2f/%.2f; added %+.1f cm (max %+.1f) in %u of %u pitched calls | "
                         "measured only: an item drawn %u calls | not applied: %s; reference used (F170; the calls that chose one): %s; %s; EDVR carried up %+.1f forward %+.1f cm (the raise, F137); "
                         "the view over the neck %+.1f cm; INSIDE %u of %u; the settle refused for the aim's pitch %u calls; returned to the stance's raise %u times "
                         "(refused %u, not needed %u)",
                         mode, item ? item : "none", mean(w.aimSum, w.calls), w.calls ? w.aimMin : 0.0, w.calls ? w.aimMax : 0.0, mean(w.headSum, w.headN), mean(w.sinkSum, w.sinkN),
                         w.sinkN ? w.sinkMax : 0.0, mean(w.headRiseSum, w.headRiseN), mean(w.wSum, w.consideredN), w.wMax, mean(w.addedSum, w.appliedN), w.appliedN ? w.addedMax : 0.0,
                         w.appliedN, w.calls, w.measuredN, why, reference, detail, mean(w.upSum, w.calls), mean(w.fwdSum, w.calls), mean(w.viewSum, w.calls), w.inside, w.insideOf,
                         w.settleRefusedAim, w.returned, w.returnRefused, w.notNeeded);
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
