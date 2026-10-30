// F125: frame-thread aggregation of the pin instruments' unchanged one-second snapshots.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
namespace edvr { namespace nofoot { namespace logpin {
template<class T> inline void step(T& d, const T& s) noexcept {
    d.n += s.n;
    d.over1cm += s.over1cm;
    d.sumSq += s.sumSq;
    d.maxAbs = std::max(d.maxAbs, s.maxAbs);
}
template<class T> inline void frame(T& d, const T& s) noexcept {
    d.n += s.n;
    d.frozen += s.frozen;
    d.stale += s.stale;
    d.clamped += s.clamped;
    d.wSum += s.wSum;
    d.raiseSum += s.raiseSum;
    d.appliedSum += s.appliedSum;
    for (uint32_t i = 0; i < 2; ++i) d.movedMm[i] += s.movedMm[i];
    for (uint32_t i = 0; i < 2; ++i) d.ticks[i] += s.ticks[i];
}
template<class T> inline void hud(T& d, const T& s) noexcept {
    d.frames += s.frames;
    d.notStill += s.notStill;
    for (int i = 0; i < 2; ++i) step(d.eye[i], s.eye[i]);
}
template<class T> inline void neck(T& d, const T& s) noexcept {
    d.n += s.n;
    d.legsN += s.legsN;
    d.animN += s.animN;
    d.neckSum += s.neckSum;
    d.viewSum += s.viewSum;
    d.gameSum += s.gameSum;
    d.animSum += s.animSum;
    d.tgtSum += s.tgtSum;
    d.camSum += s.camSum;
    d.vnUpSum += s.vnUpSum;
    d.vnForwardSum += s.vnForwardSum;
    d.vSum += s.vSum;
    d.askedSum += s.askedSum;
    d.legAnimSum += s.legAnimSum;
    d.legSum += s.legSum;
    d.neckMin = std::min(d.neckMin, s.neckMin);
    d.viewMin = std::min(d.viewMin, s.viewMin);
    d.tgtMin = std::min(d.tgtMin, s.tgtMin);
    d.legMin = std::min(d.legMin, s.legMin);
    d.neckMax = std::max(d.neckMax, s.neckMax);
    d.viewMax = std::max(d.viewMax, s.viewMax);
    d.tgtMax = std::max(d.tgtMax, s.tgtMax);
    d.vAbsMax = std::max(d.vAbsMax, s.vAbsMax);
    for (uint32_t i = 0; i < 3; ++i) d.srcN[i] += s.srcN[i];
}
template<class T> inline void body(T& d, const T& s) noexcept {
    d.n += s.n;
    d.capped += s.capped;
    d.legsN += s.legsN;
    d.lifted += s.lifted;
    d.clrN += s.clrN;
    d.inside += s.inside;
    d.wSum += s.wSum;
    d.raiseSum += s.raiseSum;
    d.askedSum += s.askedSum;
    d.legAnimSum += s.legAnimSum;
    d.legSum += s.legSum;
    d.neckSum += s.neckSum;
    d.neckAnimSum += s.neckAnimSum;
    d.clavSum += s.clavSum;
    d.chestSum += s.chestSum;
    d.ssSum += s.ssSum;   // F179a: the standing share's sum and the standing raise asked (cm)
    d.srSum += s.srSum;
    d.raiseMin = std::min(d.raiseMin, s.raiseMin);
    d.legMin = std::min(d.legMin, s.legMin);
    d.clavMin = std::min(d.clavMin, s.clavMin);
    d.chestMin = std::min(d.chestMin, s.chestMin);
    d.raiseMax = std::max(d.raiseMax, s.raiseMax);
}
template<class T> inline void pin(T& d, const T& s) noexcept {
    d.calls += s.calls;
    d.pinned += s.pinned;
    d.ageN += s.ageN;
    d.ageSum += s.ageSum;
    d.speedSum += s.speedSum;
    d.hClamped += s.hClamped;
    d.vClamped += s.vClamped;
    d.cycRows += s.cycRows;
    d.cycCached += s.cycCached;
    d.cycNone += s.cycNone;
    d.ageMax = std::max(d.ageMax, s.ageMax);
    d.speedMax = std::max(d.speedMax, s.speedMax);
    for (uint32_t i = 0; i < sizeof(d.refused) / sizeof(d.refused[0]); ++i) d.refused[i] += s.refused[i];
    for (uint32_t i = 0; i < 2; ++i) d.fresh[i] += s.fresh[i];
    for (uint32_t i = 0; i < 2; ++i) d.repeat[i] += s.repeat[i];
    for (uint32_t i = 0; i < 2; ++i) d.ageSumPar[i] += s.ageSumPar[i];
    for (uint32_t i = 0; i < 2; ++i) d.ageMaxPar[i] = std::max(d.ageMaxPar[i], s.ageMaxPar[i]);
    for (uint32_t i = 0; i < sizeof(d.f95Why) / sizeof(d.f95Why[0]); ++i) d.f95Why[i] += s.f95Why[i];
    for (uint32_t i = 0; i < sizeof(d.f98Src) / sizeof(d.f98Src[0]); ++i) d.f98Src[i] += s.f98Src[i];
    d.f160.merge(s.f160);
    for (uint32_t i = 0; i < 2; ++i) d.f98EyeSum[i] += s.f98EyeSum[i];
    for (uint32_t i = 0; i < 2; ++i) d.f98EyeMin[i] = std::min(d.f98EyeMin[i], s.f98EyeMin[i]);
    for (uint32_t i = 0; i < 2; ++i) d.f98EyeMax[i] = std::max(d.f98EyeMax[i], s.f98EyeMax[i]);
    d.f95TgtMin = std::min(d.f95TgtMin, s.f95TgtMin);
    d.f95TgtMax = std::max(d.f95TgtMax, s.f95TgtMax);
    for (int i = 0; i < 2; ++i) {
        neck(d.f94[i], s.f94[i]); step(d.f95View[i], s.f95View[i]); step(d.f95World[i], s.f95World[i]);
        d.f98Have[i] = s.f98Have[i]; d.f98Neck[i] = s.f98Neck[i];
    }
    for (uint32_t i = 0; i < sizeof(d.f97) / sizeof(d.f97[0]); ++i) body(d.f97[i], s.f97[i]);
    d.f179.merge(s.f179);   // F179b: the neck guard's counters per context
}
template<class T> inline void elbow(T& d, const T& s) noexcept {
    d.n += s.n;
    d.aimN += s.aimN;
    d.leanN += s.leanN;
    d.steps += s.steps;
    d.stepsOver10 += s.stepsOver10;
    d.spanN += s.spanN;
    d.twistLimited += s.twistLimited;
    d.aimSum += s.aimSum;
    d.leanSum += s.leanSum;
    d.leanAnimSum += s.leanAnimSum;
    d.yawSum += s.yawSum;
    d.planeSum += s.planeSum;
    d.planeOver45 += s.planeOver45;
    d.stepSq += s.stepSq;
    d.spanSum += s.spanSum;
    d.downN += s.downN;
    d.aimMin = std::min(d.aimMin, s.aimMin);
    d.downMin = std::min(d.downMin, s.downMin);
    d.aimMax = std::max(d.aimMax, s.aimMax);
    d.yawMax = std::max(d.yawMax, s.yawMax);
    d.planeMax = std::max(d.planeMax, s.planeMax);
    d.stepMax = std::max(d.stepMax, s.stepMax);
    d.spanMax = std::max(d.spanMax, s.spanMax);
    for (uint32_t i = 0; i < 3; ++i) d.src[i] += s.src[i];
    d.f159.merge(s.f159);
    d.f161.merge(s.f161);
    d.f165.merge(s.f165);
    d.f166.merge(s.f166);
    d.f168.merge(s.f168);
    d.f170.merge(s.f170);   // F171: F170's was missing, so its line (printed when f98.f170.ads is nonzero) never printed in a window built here (the flight edvr_gfx_20261029_043951)
    d.f171.merge(s.f171);
    d.f178.merge(s.f178);   // F178: the standing ADS body dials' counters, in the same second
    d.f180.merge(s.f180);   // F180b: the standing ADS spine straighten's counters
    if (s.downStepMax > d.downStepMax) {
        d.downStepMax = s.downStepMax; d.downStepAng = s.downStepAng; d.downStepTwist = s.downStepTwist;
    }
}
// Kept as individual eligible seconds: a teleport must not dominate a pooled RMS, and a median of ten-second medians is not the session's median.
// The hook's raw counters cannot be split retrospectively at a frame-thread context edge. Observe from the first frame, before the first fold, and isolate
// a second containing any edge as mixed. Its next clean second starts from the context most recently observed, without taking an extra hook snapshot/reset.
struct ContextSecond {
    uint64_t first = 0, last = 0;
    bool have = false, mixed = false;
    void observe(uint64_t context) noexcept {
        if (!have) { first = last = context; have = true; }
        else { mixed = mixed || context != last; last = context; }
    }
    bool take() noexcept { const bool was = mixed; mixed = false; first = last; return was; }
};
struct Seconds {
    double v[10] = {}; uint32_t n = 0;
    void add(uint32_t frames, double rms) noexcept { if (frames >= 20 && n < 10) v[n++] = rms; }
    void text(char* out, size_t cap) const noexcept {
        size_t at = 0; out[0] = 0;
        for (uint32_t i = 0; i < n && at < cap; ++i) {
            const int w = std::snprintf(out + at, cap - at, "%s%.2f", i ? "," : "", v[i]);
            if (w < 0 || static_cast<size_t>(w) >= cap - at) break;
            at += static_cast<size_t>(w);
        }
    }
};
// Track adjacent seconds before aggregation, including the edge between two reporting windows.
struct StoredNeck {
    bool have = false; double last = 0.0;
    uint32_t startAfterStored = 0; double jump = 0.0;
    void fold(bool nowHave, double neck, uint32_t starts) noexcept {
        if (have) startAfterStored += starts;
        if (have && nowHave) jump = std::max(jump, std::fabs(neck - last));
        have = nowHave; last = neck;
    }
    void clearWindow() noexcept { startAfterStored = 0; jump = 0.0; }
};
}}} // namespace edvr::nofoot::logpin
