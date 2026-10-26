// F125: aggregate F119's unchanged one-second measurements without keeping a ten-second sample list.
#pragma once
#include "native_on_foot_f119.h"

namespace edvr { namespace nofoot { namespace f119 {
constexpr int kHeadWindowLineLimit = 1090;
// Reporting context is independent of pose-validity/F123 state. A request has no exact runtime-apply acknowledgement: its measurements are visibly mixed
// until the existing geometry-based done/failure result, then a new clean report epoch starts. The reference generation remains the runtime's value.
struct RestContext {
    bool known = false, mixed = false;
    uint32_t reference = 0;
    uint64_t epoch = 0;
    bool different(uint32_t ref) const noexcept { return !known || reference != ref; }
    bool needsCancellation(bool active) const noexcept { return mixed && !active; }
    void observe(uint32_t ref, uint64_t next) noexcept { known = true; reference = ref; epoch = next; }
    void pending(uint64_t next) noexcept { mixed = true; epoch = next; }
    void settled(uint64_t next) noexcept { mixed = false; epoch = next; }
};

struct HeadWindow {
    ClassOut cls[kClasses];
    uint32_t classSeconds[kClasses] = {};
    OwnSecond own;
    DepthSecond depth;
    uint32_t seconds = 0, frames = 0, noPose = 0, dropped = 0, depthSeconds[2] = {}, depthAny = 0, costFrames = 0;
    double costUs = 0.0;

    void fold(const OffsetSecond& off, const OwnSecond& o, const DepthSecond& d, double us, uint32_t timedFrames) noexcept {
        ++seconds; frames += off.frames; noPose += off.noPose; dropped += off.dropped; costUs += us; costFrames += timedFrames;
        for (int i = 0; i < kClasses; ++i) {
            ClassOut s;
            if (!off.summarize(i, &s)) continue;
            ClassOut& c = cls[i];
            if (!c.n) c = s;
            else {
                c.n += s.n; c.max = std::max(c.max, s.max);
                c.rMin = std::min(c.rMin, s.rMin); c.rMax = std::max(c.rMax, s.rMax);
                c.fMin = std::min(c.fMin, s.fMin); c.fMax = std::max(c.fMax, s.fMax);
                c.uMin = std::min(c.uMin, s.uMin); c.uMax = std::max(c.uMax, s.uMax);
                for (int b = 0; b < kBins; ++b) c.hist[b] += s.hist[b];
            }
            ++classSeconds[i];
        }
        if (o.n) {
            if (!own.n) own = o;
            else {
                own.n += o.n; own.hMax = std::max(own.hMax, o.hMax);
                own.upMin = std::min(own.upMin, o.upMin); own.upMax = std::max(own.upMax, o.upMax);
                own.lockH = std::max(own.lockH, o.lockH); own.weaponH = std::max(own.weaponH, o.weaponH); own.viewH = std::max(own.viewH, o.viewH);
                if (std::fabs(o.neckUp) > std::fabs(own.neckUp)) own.neckUp = o.neckUp;
                if (std::fabs(o.holdUp) > std::fabs(own.holdUp)) own.holdUp = o.holdUp;
            }
        }
        for (int e = 0; e < 2; ++e) {
            const EyeSecond& s = d.eye[e]; EyeSecond& t = depth.eye[e];
            depthSeconds[e] += s.read != 0;
            t.read += s.read;
            if (s.read) t.nearM = s.nearM;
            t.nearestWorld = std::min(t.nearestWorld, s.nearestWorld); t.nearestAll = std::min(t.nearestAll, s.nearestAll);
            for (int k = 0; k < kThresholds; ++k) t.within[k] = std::max(t.within[k], s.within[k]);
            t.all20 = std::max(t.all20, s.all20); t.markedCells = std::max(t.markedCells, s.markedCells);
        }
        depthAny += d.eye[0].read || d.eye[1].read;
        for (int k = 0; k < kSkips; ++k) depth.skipped[k] += d.skipped[k];
        depth.baseFrames += d.baseFrames; depth.baseCells20 = std::max(depth.baseCells20, d.baseCells20); depth.baseOwn20 = std::max(depth.baseOwn20, d.baseOwn20);
        depth.contactMin1 = std::min(depth.contactMin1, d.contactMin1); depth.contactMin8 = std::min(depth.contactMin8, d.contactMin8); depth.contactMin32 = std::min(depth.contactMin32, d.contactMin32);
        if (d.contactFrames) {
            depth.contactWeakest = depth.contactFrames ? std::min(depth.contactWeakest, d.contactWeakest) : d.contactWeakest;
            depth.contactFrames += d.contactFrames; depth.contactFar = std::max(depth.contactFar, d.contactFar);
        }
    }
    uint32_t classes() const noexcept { uint32_t n = 0; for (const ClassOut& c : cls) n += c.n != 0; return n; }
    void clear() noexcept { *this = HeadWindow{}; }
};

inline double histogramRank(const ClassOut& c, double q) noexcept {
    const uint32_t rank = std::max(1u, static_cast<uint32_t>(std::ceil(c.n * q)));
    uint32_t n = 0;
    for (int i = 0; i < kBins; ++i) {
        n += c.hist[i];
        if (n >= rank) return i == kBins - 1 ? c.max : kEdgesCm[i];
    }
    return c.max;
}

// One class per line retains every class and histogram. Common window fields repeat and carry one window id, so the reader counts them once.
inline int formatHeadWindow(char* out, size_t cap, const HeadWindow& w, int cls, uint32_t generation, uint32_t recentres, double age, uint64_t window,
                            uint64_t epoch, bool mixed) noexcept {
    char cl[420] = "none", eyes[2][240], depth[500], skipped[320] = "", contact[3][24];
    uint32_t duration = 0;
    if (cls >= 0 && cls < kClasses && w.cls[cls].n) {
        ClassOut c = w.cls[cls]; c.p50 = histogramRank(c, 0.50); c.p95 = histogramRank(c, 0.95);
        formatClass(cl, sizeof(cl), c, true); duration = w.classSeconds[cls];
    }
    formatEye(eyes[0], sizeof(eyes[0]), "L", w.depth.eye[0]); formatEye(eyes[1], sizeof(eyes[1]), "R", w.depth.eye[1]);
    std::snprintf(depth, sizeof(depth), "%s; %s", eyes[0], eyes[1]);
    size_t at = 0;
    for (int k = 0; k < kSkips; ++k) {
        if (!w.depth.skipped[k]) continue;
        const int n = std::snprintf(skipped + at, sizeof(skipped) - at, "%s%s %u", at ? ", " : "", skipName(k), w.depth.skipped[k]);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(skipped) - at) break;
        at += static_cast<size_t>(n);
    }
    const double offsets[3] = {w.depth.contactMin1, w.depth.contactMin8, w.depth.contactMin32};
    for (int i = 0; i < 3; ++i) {
        if (offsets[i] < 1e8) std::snprintf(contact[i], sizeof(contact[i]), "%.1f", offsets[i]);
        else std::snprintf(contact[i], sizeof(contact[i]), "none");
    }
    return std::snprintf(out, cap,
        "native on foot head containment (F119) %u s: frames %u (no pose %u, over limit %u); rest %u age %.1f recentres %u; head offset cm: %s; "
        "own cm h %.1f up %+.1f..%+.1f lock/weapon/view %.1f/%.1f/%.1f neck/hold %+.1f/%+.1f; depth cm: %s; skipped %s; "
        "contact cm 1/8/32 %s/%s/%s frames/weakest/far %u/%u/%u; baseline frames/world/body %u/%u/%u; cost %.0f us/frame; "
        "depth s L/R/any %u/%u/%u of %u; class s %u; window %llu; epoch %llu; %s",
        w.seconds, w.frames, w.noPose, w.dropped, generation, age, recentres, cl,
        w.own.hMax * 100.0, w.own.upMin * 100.0, w.own.upMax * 100.0, w.own.lockH * 100.0, w.own.weaponH * 100.0, w.own.viewH * 100.0,
        w.own.neckUp * 100.0, w.own.holdUp * 100.0, depth, at ? skipped : "none", contact[0], contact[1], contact[2],
        w.depth.contactFrames, w.depth.contactWeakest, w.depth.contactFar, w.depth.baseFrames, w.depth.baseCells20, w.depth.baseOwn20,
        w.costFrames ? w.costUs / w.costFrames : 0.0, w.depthSeconds[0], w.depthSeconds[1], w.depthAny, w.seconds, duration,
        static_cast<unsigned long long>(window), static_cast<unsigned long long>(epoch), mixed ? "mixed rest" : "steady");
}

}}} // namespace edvr::nofoot::f119

namespace edvr { namespace nofoot { namespace logwin {
// Counts belong to their sampled key/scope even when a change arrives before the first full second. Reporting resets retain the primed state.
struct PanelWindow {
    uint32_t seen = 0, hidden = 0, failed = 0, seconds = 0, active = 0;
    bool known = false, key = false, onFoot = false, detail = false;
    void state(bool k, bool on, bool d) noexcept { known = true; key = k; onFoot = on; detail = d; }
    bool changed(bool k, bool on, bool d) const noexcept { return known && (key != k || onFoot != on || detail != d); }
    void take(uint32_t s, uint32_t h, uint32_t f, bool fold) noexcept {
        if (!fold && !s && !h && !f) return;
        if (!onFoot && !s && !h && !f) return;
        ++seconds; active += s != 0; seen += s; hidden += h; failed += f;
    }
    void clear() noexcept { seen = hidden = failed = seconds = active = 0; }
};
}}}
