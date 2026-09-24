#include "lod_governor.h"

#include "journal_watch.h"
#include "kinematic_eval_hook.h"
#include "native_timing.h"
#include "gpu_frame_timing.h"
#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace edvr {

// --- The policy ------------------------------------------------------------------

namespace lodgov {

void Policy::configure(float kMax) noexcept {
    if (!(kMax >= 1.0f)) kMax = 1.0f;   // NaN too
    if (kMax > kMaxCeiling) kMax = kMaxCeiling;
    maxSteps_ = static_cast<int>(std::lround((kMax - 1.0f) * kQuantaPerUnit));
    if (steps_ > maxSteps_) clampPending_ = true;
}

// Everything back to its first state but k_max and the mode, which only
// configure and setFixed set.
void Policy::reset() noexcept {
    const int maxSteps = maxSteps_;
    const bool fixed = fixed_;
    *this = Policy{};
    maxSteps_ = maxSteps;
    fixed_ = fixed;
}

double Policy::partsMean(int field) const noexcept {
    if (!partsCount_) return 0.0;
    double sum = 0.0;
    for (uint32_t i = 0; i < partsCount_; ++i) sum += parts_[(partsNext_ + kEffectFrames - 1 - i) % kEffectFrames][field];
    return sum / partsCount_;
}

double Policy::meanExcessMs() const noexcept {
    if (!count_) return 0.0;
    double sum = 0.0;
    for (uint32_t i = 0; i < count_; ++i) sum += excess_[(next_ + kSampleWindow - 1 - i) % kSampleWindow];
    return sum / count_;
}

double Policy::meanGpuMs() const noexcept {
    double sum = 0.0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < count_; ++i) {
        const double g = gpuMs_[(next_ + kSampleWindow - 1 - i) % kSampleWindow];
        if (g >= 0.0) {
            sum += g;
            ++n;
        }
    }
    return n ? sum / n : -1.0;
}

void Policy::Cohort::add(double r, double t, double p) noexcept {
    if (!n) {
        recordsLo = recordsHi = r;
        testedLo = testedHi = t;
    } else {
        if (r < recordsLo) recordsLo = r;
        if (r > recordsHi) recordsHi = r;
        if (t < testedLo) testedLo = t;
        if (t > testedHi) testedHi = t;
    }
    ++n;
    records += r;
    tested += t;
    passed += p;
}

void Policy::Cohort::done() noexcept {
    if (!n) return;
    records /= n;
    tested /= n;
    passed /= n;
}

// Every frame within 5% (records) and 10% (parts tested) of the side's mean;
// half a count of slack keeps a side of small, constant counts stable.
bool Policy::Cohort::stable() const noexcept {
    return n >= kEffectMin && recordsHi - records <= kStableRecordsShare * records + 0.5 &&
           records - recordsLo <= kStableRecordsShare * records + 0.5 &&
           testedHi - tested <= kStableTestedShare * tested + 0.5 && tested - testedLo <= kStableTestedShare * tested + 0.5;
}

Policy::Cohort Policy::ringCohort() const noexcept {
    Cohort c;
    for (uint32_t i = 0; i < partsCount_; ++i) {
        const double* p = parts_[(partsNext_ + kEffectFrames - 1 - i) % kEffectFrames];
        c.add(p[3], p[0], p[1]);
    }
    c.done();
    return c;
}

double Policy::callerNow() const noexcept {
    return count_ >= kEffectMin ? periodMs_ + meanExcessMs() : -1.0;
}

// Within 2 m of the eye camera now; with either camera unknown, not the same.
bool Policy::sameCam(const float a[3], bool aValid) const noexcept {
    if (!aValid || !camValid_) return false;
    const double dx = double(a[0]) - cam_[0], dy = double(a[1]) - cam_[1], dz = double(a[2]) - cam_[2];
    return dx * dx + dy * dy + dz * dz <= kSameViewM * kSameViewM;
}

namespace {
// The whole effect between two ends: a benefit when the parts tested or
// passed at EDVR's scale fell 1% or more (judged at 200 tested a frame or
// more) or the caller work fell 0.2 ms or more; none when neither did with
// both judged; else not judged.
Effect classifyEffect(double testedBefore, double testedAfter, double passedBefore, double passedAfter,
                      double callerBefore, double callerAfter) noexcept {
    const bool partsJudged = testedBefore >= kJudgeMinTested && testedAfter >= 0.0 && passedAfter >= 0.0;
    const bool callerJudged = callerBefore >= 0.0 && callerAfter >= 0.0;
    const bool partsFell =
        partsJudged && (testedBefore - testedAfter >= kBenefitShare * testedBefore ||
                        (passedBefore > 0.0 && passedBefore - passedAfter >= kBenefitShare * passedBefore));
    const bool callerFell = callerJudged && callerBefore - callerAfter >= kBenefitCallerMs;
    if (partsFell || callerFell) return Effect::Benefit;
    if (partsJudged && callerJudged) return Effect::NoBenefit;
    return Effect::NotJudged;
}
}  // namespace

// At k_max: the target reached when the last second did not trigger; else the
// whole effect against the k = 1 baseline of this view -- the eye camera
// within 2 m of it, a stable scene now, neither the records nor the parts
// tested above the baseline's by more than 2% (the lever only removes) --
// and without one, the last judged step's.
Outcome Policy::ceilingOutcome(EffectFigures* figures) const noexcept {
    EffectFigures f = lastFig_;
    f.baseline = false;
    Effect e = lastEffect_;
    if (base_.valid && sameCam(base_.cam, base_.camValid)) {
        const Cohort now = ringCohort();
        if (now.stable() && now.records <= base_.records * (1.0 + kSceneGrowShare) + 0.5 &&
            now.tested <= base_.tested * (1.0 + kSceneGrowShare) + 0.5) {
            f.tested[0] = base_.tested;
            f.tested[1] = now.tested;
            f.passed[0] = base_.passed;
            f.passed[1] = now.passed;
            f.caller[0] = base_.caller;
            f.caller[1] = callerNow();
            f.baseline = true;
            e = classifyEffect(f.tested[0], f.tested[1], f.passed[0], f.passed[1], f.caller[0], f.caller[1]);
        }
    }
    if (figures) *figures = f;
    if (!lastWin_.decided) return Outcome::Unknown;
    if (!lastWin_.triggered) return Outcome::Reached;
    return e == Effect::Benefit     ? Outcome::Residual
         : e == Effect::NoBenefit ? Outcome::NoBenefit
                                  : Outcome::Unknown;
}

// One valid sample into the ring; once it is full the oldest leaves, and its
// class with it.
void Policy::push(double excessMs, bool miss, bool gpuBound, bool unexplained, double gpuMs) noexcept {
    if (count_ == kSampleWindow) {
        if (miss_[next_]) --missCount_;
        if (gpu_[next_]) --gpuCount_;
        if (unex_[next_]) --unexCount_;
    } else {
        ++count_;
    }
    excess_[next_] = excessMs;
    gpuMs_[next_] = gpuMs;
    miss_[next_] = miss;
    gpu_[next_] = gpuBound;
    unex_[next_] = unexplained;
    if (miss) ++missCount_;
    if (gpuBound) ++gpuCount_;
    if (unexplained) ++unexCount_;
    next_ = (next_ + 1) % kSampleWindow;
}

void Policy::restart(uint64_t nowMs) noexcept {
    winStartMs_ = nowMs;
    gather_ = Gather{};
    lastWin_ = WindowResult{};
    trigRun_ = cleanRun_ = ceilRun_ = 0;
    relaxing_ = kickTrial_ = false;
    good_ = -1;
    lastDownMs_ = 0;
    downWait_ = kCleanWindows;
    partsNext_ = partsCount_ = 0;
    effectOpen_ = inert_ = retryArmed_ = false;
    inertUnits_ = 0;
}

// The evidence is gone: k holds, and nothing gathered before decides after.
void Policy::expire(uint64_t nowMs) noexcept {
    emptyRing();
    haveClock_ = false;
    cycle_ = Cycle{};
    winStartMs_ = nowMs;
    gather_ = Gather{};
    lastWin_ = WindowResult{};
    trigRun_ = cleanRun_ = ceilRun_ = 0;
    effectOpen_ = kickTrial_ = false;
    ++expiries_;
}

// The second is over: what it decides, and the runs it extends or breaks.
// A window of fewer than 20 cycles with a fresh sample decides nothing and
// breaks nothing.
void Policy::closeWindow(uint64_t nowMs) noexcept {
    WindowResult w;
    w.cycles = gather_.cycles;
    w.misses = gather_.misses;
    w.gpuBound = gather_.gpuBound;
    w.unexplained = gather_.unexplained;
    w.anyMisses = gather_.anyMisses;
    w.meanExcessMs = gather_.cycles ? gather_.excessSum / gather_.cycles : 0.0;
    w.decided = gather_.cycles >= kWindowMinCycles;
    if (w.decided) {
        // Without caller work a miss cannot be the CPU's: nothing triggers.
        w.triggered = !holding_ && w.misses * kTriggerDivisor >= w.cycles;
        w.headroom = w.anyMisses == 0 && w.meanExcessMs < -kUnderMarginMs;
        trigRun_ = w.triggered ? trigRun_ + 1 : 0;
        cleanRun_ = w.headroom ? cleanRun_ + 1 : 0;
        ceilRun_ = w.triggered && steps_ == maxSteps_ ? ceilRun_ + 1 : 0;
        // Triggering again ends a kick's relaxation -- not the seconds of the
        // kick's own trial, which the runtime needs to return to full rate.
        if (w.triggered && !kickTrial_) relaxing_ = false;
    }
    lastWin_ = w;
    ++windows_;
    windowClosed_ = true;
    winStartMs_ = nowMs;
    gather_ = Gather{};
}

// A step's benefit is measured from here: the latest 30 frames with builder
// work and 30 samples' caller work before it, the same after it, and where
// the eye camera was.
void Policy::openEffect(Opened kind, bool coarse) noexcept {
    effectOpen_ = true;
    effectKind_ = kind;
    effectCoarse_ = coarse;
    effectRetry_ = kind == Opened::Up && retry_;
    effectBefore_ = ringCohort();
    effectAfter_ = Cohort{};
    effectBeforeCaller_ = callerNow();
    effectCallerSum_ = 0.0;
    effectCallerN_ = 0;
    effectCamValid_ = camValid_;
    std::memcpy(effectCam_, cam_, sizeof(effectCam_));
}

// The open step's benefit, its whole effect (classifyEffect), judged only on
// a stable scene: both sides stable, neither the records nor the parts tested
// risen more than 2% across the step, the eye camera within 2 m of where it
// was -- else not judged, never a reason to hold. An up step without a
// benefit adds to the run (0.25 two units, 0.05 one); 4 units and the lever
// is inert at this view. A retried step with one re-arms.
void Policy::judgeEffect(uint64_t nowMs) noexcept {
    effectOpen_ = false;
    Cohort after = effectAfter_;
    after.done();
    const Cohort& before = effectBefore_;
    const double caller = effectCallerN_ >= kEffectMin ? effectCallerSum_ / effectCallerN_ : -1.0;
    const bool stable = before.stable() && after.stable();
    const bool grew = after.records > before.records * (1.0 + kSceneGrowShare) + 0.5 ||
                      after.tested > before.tested * (1.0 + kSceneGrowShare) + 0.5;
    const bool moved = effectCamValid_ && camValid_ && !sameCam(effectCam_, true);
    lastFig_ = EffectFigures{};
    if (before.n) {
        lastFig_.tested[0] = before.tested;
        lastFig_.passed[0] = before.passed;
    }
    if (after.n) {
        lastFig_.tested[1] = after.tested;
        lastFig_.passed[1] = after.passed;
    }
    lastFig_.caller[0] = effectBeforeCaller_;
    lastFig_.caller[1] = caller;
    lastEffect_ = stable && !grew && !moved
        ? classifyEffect(before.tested, after.tested, before.passed, after.passed, effectBeforeCaller_, caller)
        : Effect::NotJudged;
    if (effectKind_ != Opened::Up) return;   // a kick's or reduced's effect: the outcome only
    ++judged_[lastEffect_ == Effect::Benefit ? 0 : lastEffect_ == Effect::NoBenefit ? 1 : 2];
    if (effectRetry_) {
        if (lastEffect_ == Effect::Benefit && inert_) {
            inert_ = retryArmed_ = false;
            rearmed_ = true;
            ++rearms_;
            inertUnits_ = 0;
        }
        return;
    }
    if (lastEffect_ == Effect::Benefit) {
        inertUnits_ = 0;
        return;
    }
    if (lastEffect_ != Effect::NoBenefit) return;   // not judged: no evidence either way
    if (!inertUnits_) {
        runFig_ = lastFig_;   // the run's first before
        runCoarse_ = runFine_ = 0;
    }
    runFig_.tested[1] = lastFig_.tested[1];
    runFig_.passed[1] = lastFig_.passed[1];
    runFig_.caller[1] = lastFig_.caller[1];
    if (effectCoarse_) ++runCoarse_;
    else ++runFine_;
    inertUnits_ += effectCoarse_ ? 2u : 1u;
    if (inertUnits_ >= kInertUnits && !inert_) {
        inert_ = inertStarted_ = true;
        retryArmed_ = false;
        inertSinceMs_ = nowMs;
        testedAtHold_ = partsMean(0);
        ++inertHolds_;
    }
}

Step Policy::update(const FrameSignals& s) noexcept {
    if (!started_) {
        started_ = true;
        winStartMs_ = s.nowMs;
    }
    cycle_ = Cycle{};
    windowClosed_ = inertStarted_ = rearmed_ = false;
    camValid_ = s.camValid;
    if (s.camValid) std::memcpy(cam_, s.cam, sizeof(cam_));
    // On foot (the game's Status.json, the flag the on-foot frame pacing keys
    // on): the arc measured the cockpit only, so k is held at 1 exactly as
    // outside a settlement -- at once, the settlement, the samples, the clock,
    // the window and its runs, the working point and a pending clamp
    // forgotten -- for as long as it lasts. The k in force when the hold
    // began is kept (a pending one survives a hold that begins at 1) for the
    // return aboard.
    if (s.onFoot) {
        if (!onFoot_) {
            onFoot_ = true;
            if (steps_ > 0) footSteps_ = steps_;
        }
        clampPending_ = inSettlement_ = false;
        sparse_ = 0;
        emptyRing();
        haveClock_ = false;
        restart(s.nowMs);
        if (steps_ > 0) {
            steps_ = 0;
            return Step::Foot;
        }
        return Step::None;
    }
    if (onFoot_) {   // aboard again: the 5 s in which the settlement may be found
        onFoot_ = false;
        boardedMs_ = s.nowMs;
    }
    // This boundary's cycle: the interval since the previous one, when that
    // one may be measured from. A two-slot cycle of any kind counts against
    // the window's cleanness, with or without a new sample (the period is
    // then the last sample's).
    if (s.clockMs >= 0.0) {
        if (haveClock_) {
            cycle_.measured = true;
            cycle_.ms = s.clockMs - lastClockMs_;
        }
        lastClockMs_ = s.clockMs;
        haveClock_ = true;
    } else {
        haveClock_ = false;
    }
    const double period = s.work == Work::Valid ? s.periodMs : periodMs_;
    if (cycle_.measured && period > 0.0 && cycle_.ms > kMissFactor * period) {
        cycle_.missed = true;
        ++gather_.anyMisses;
    }
    // The evidence expired (the lease lost, a stale sample, a sequence
    // unchanged for 2 s, the timing's generation, source or period changed)
    // or the newest sample is invalid: the ring, the second in progress and
    // the runs go, and k holds. The next boundary has no interval.
    if (s.expired || s.work == Work::Invalid) expire(s.nowMs);
    // Density, with hysteresis: in at 200 records a frame; out only after
    // 30 consecutive frames under 150, and then k is 1 at once.
    if (s.records >= kSettlementRecords) {
        inSettlement_ = true;
        sparse_ = 0;
    } else if (inSettlement_ && s.records + kSettlementBand < kSettlementRecords) {
        if (++sparse_ >= kConsecutive) {
            inSettlement_ = false;
            sparse_ = 0;
            emptyRing();
            haveClock_ = false;   // the next boundary has no interval
            restart(s.nowMs);
            if (steps_ > 0) {
                steps_ = 0;
                return Step::Reset;
            }
            return Step::None;
        }
    } else {
        sparse_ = 0;
    }
    // A fresh valid sample joins the ring and, with a measured cycle, the
    // window -- a two-slot cycle classed GPU-bound (the application's GPU
    // render time at or over the period - 0.5 ms), else unexplained (the
    // caller work under the period - 0.3 ms), else the CPU's -- and an open
    // step's caller-work cohort.
    if (s.work == Work::Valid) {
        periodMs_ = s.periodMs;
        holding_ = s.source == WorkSource::App;
        if (cycle_.missed) {
            if (s.gpuValid && s.gpuMs >= s.periodMs - kGpuBoundMarginMs) cycle_.gpuBound = true;
            else if (s.workMs < s.periodMs - kUnexplainedMarginMs) cycle_.unexplained = true;
        }
        const bool ours = cycle_.missed && !cycle_.gpuBound && !cycle_.unexplained;
        push(s.workMs - s.periodMs, ours, cycle_.gpuBound, cycle_.unexplained, s.gpuValid ? s.gpuMs : -1.0);
        if (cycle_.measured) {
            ++gather_.cycles;
            gather_.excessSum += s.workMs - s.periodMs;
            if (cycle_.gpuBound) ++gather_.gpuBound;
            else if (cycle_.unexplained) ++gather_.unexplained;
            else if (cycle_.missed) ++gather_.misses;
        }
        if (effectOpen_) {
            effectCallerSum_ += s.workMs;
            ++effectCallerN_;
        }
    }
    // The lever's effect, frame by frame (frames with builder work only): the
    // latest 30 frames' parts and records, and an open step's after side. The
    // step is judged once 30 fresh samples have followed it (and 30 frames of
    // builder work, if it had any before it).
    if (s.records > 0 || s.tested > 0) {
        double* p = parts_[partsNext_];
        p[0] = s.tested;
        p[1] = s.passed;
        p[2] = s.dropped;
        p[3] = s.records;
        partsNext_ = (partsNext_ + 1) % kEffectFrames;
        if (partsCount_ < kEffectFrames) ++partsCount_;
        if (effectOpen_) effectAfter_.add(s.records, s.tested, s.passed);
    }
    if (effectOpen_ && effectCallerN_ >= kEffectFrames && (effectAfter_.n >= kEffectFrames || effectBefore_.n == 0))
        judgeEffect(s.nowMs);
    // Inert: one step may be retried 30 s after the hold or the last retry,
    // or at once when the view changes -- the parts tested a frame moved by
    // more than 20%.
    if (inert_ && !retryArmed_ && !effectOpen_) {
        const double tested = partsMean(0);
        if (s.nowMs - inertSinceMs_ >= kRetryMs ||
            (testedAtHold_ > 0.0 && std::fabs(tested - testedAtHold_) > kRearmShare * testedAtHold_))
            retryArmed_ = true;
    }
    // Back aboard: the settlement found again within 5 s -- a frame with 200
    // records -- brings the k from before the hold back in one step, no ramp
    // (06:53: re-ramping from 1 in 0.25 steps after re-boarding flickered
    // every structure at each step); not found within 5 s, it starts from 1
    // as ever. reduced's Enter does its own.
    if (footSteps_ > 0) {
        if (fixed_ || s.nowMs - boardedMs_ > kAboardMs) {
            footSteps_ = 0;
        } else if (inSettlement_ && s.records >= kSettlementRecords) {
            const int to = footSteps_ < maxSteps_ ? footSteps_ : maxSteps_;
            footSteps_ = 0;
            if (to > steps_) {
                steps_ = to;
                return Step::Aboard;
            }
        }
    }
    // A lowered k_max takes effect at once, whatever the signals say.
    if (clampPending_) {
        clampPending_ = false;
        if (steps_ > maxSteps_) {
            steps_ = maxSteps_;
            return Step::Clamp;
        }
    }
    // Once a second of wall time: the window closes and decides.
    const bool closed = s.nowMs - winStartMs_ >= kWindowMs;
    if (closed) closeWindow(s.nowMs);
    // reduced: k_max for as long as the settlement lasts -- at once, no ramp,
    // no frame-work steps, no kick (its windows still feed the ceiling line,
    // and the jump's measured effect its outcome).
    if (fixed_) {
        if (inSettlement_ && steps_ != maxSteps_) {
            steps_ = maxSteps_;
            openEffect(Opened::Enter, false);
            return Step::Enter;
        }
        return Step::None;
    }
    if (!closed || !lastWin_.decided) return Step::None;
    const WindowResult& w = lastWin_;
    const bool dense = inSettlement_ && s.records >= kSettlementRecords;
    // A step whose 30 samples are not all in (a frame rate under 30) is
    // judged now on what it has.
    if (effectOpen_) judgeEffect(s.nowMs);
    // The k = 1 baseline of this view: each second at k 1 in the settlement
    // on a stable scene, with the eye camera where it was taken.
    if (steps_ == 0 && inSettlement_) {
        const Cohort c = ringCohort();
        const double caller = callerNow();
        if (c.stable() && caller >= 0.0) {
            base_.valid = true;
            base_.records = c.records;
            base_.tested = c.tested;
            base_.passed = c.passed;
            base_.caller = caller;
            base_.camValid = camValid_;
            std::memcpy(base_.cam, cam_, sizeof(base_.cam));
        }
    }
    // A step down that has held 60 s without a trigger: the next recovery
    // trial waits 5 clean seconds again. Counted from the step, not from the
    // last trigger: a wait of 60 clean seconds is itself 60 s without one.
    if (w.triggered) lastTriggerMs_ = s.nowMs;
    else if (lastDownMs_ && lastTriggerMs_ < lastDownMs_ && s.nowMs - lastDownMs_ >= kBackoffResetMs)
        downWait_ = kCleanWindows;
    // The kick's trial, on its fifth decided window: still triggering at
    // k_max, the kick has not bought the frame back -- the pre-kick k at
    // once, and no kick for 60 s. Otherwise the relaxation goes on.
    if (kickTrial_ && ++trialWindows_ >= kKickTrialWindows) {
        kickTrial_ = false;
        if (w.triggered) {
            steps_ = preKickSteps_ < maxSteps_ ? preKickSteps_ : maxSteps_;
            relaxing_ = false;
            kickBlockedUntilMs_ = s.nowMs + kKickBlockMs;
            trigRun_ = ceilRun_ = 0;
            restoreKick_ = true;
            return Step::Restore;
        }
    }
    if (w.triggered) {
        // A recovery trial that failed: a trigger within 10 s of a step down
        // restores the working point at once, and the next trial waits twice
        // as long (5, 10, 20, 40, 60 clean seconds).
        if (lastDownMs_ && s.nowMs - lastDownMs_ <= kTrialMs && good_ > steps_) {
            steps_ = good_ < maxSteps_ ? good_ : maxSteps_;
            downWait_ = downWait_ * 2 < kCleanWindowsMax ? downWait_ * 2 : kCleanWindowsMax;
            lastDownMs_ = 0;
            relaxing_ = false;
            restoreKick_ = false;
            return Step::Restore;
        }
        // Triggering at or above it, the working point no longer works here.
        if (good_ >= 0 && steps_ >= good_) good_ = -1;
        // Inert at this view: stepping removes nothing, so no step and no
        // kick -- and a held second is not one the steps failed to clear, so
        // the kick's run starts over. A retry, when armed, is one ordinary
        // step below.
        if (inert_ && !retryArmed_) {
            trigRun_ = 0;
            return Step::None;
        }
        // The kick: ten triggering windows in a row below k_max, the steps
        // have not cleared it -- k_max at once, so consecutive frames fit and
        // the runtime returns to full rate; a trial, judged five windows on.
        // At most one per 30 s, none for 60 s after a failed one.
        if (!inert_ && dense && steps_ < maxSteps_ && trigRun_ >= kKickWindows && s.nowMs >= kickBlockedUntilMs_ &&
            (!kicked_ || s.nowMs - lastKickMs_ >= kKickIntervalMs)) {
            preKickSteps_ = steps_;
            relaxTarget_ = steps_ + kCoarseQuanta < maxSteps_ ? steps_ + kCoarseQuanta : maxSteps_;
            steps_ = maxSteps_;
            kicked_ = relaxing_ = kickTrial_ = true;
            trialWindows_ = 0;
            lastKickMs_ = s.nowMs;
            openEffect(Opened::Kick, false);
            return Step::Kick;
        }
        // Up: 0.25 while a quarter of the window's cycles were the CPU's
        // misses or its mean ran more than 1.0 ms over, else 0.05 -- and 0.05
        // within 0.25 below a working point; held to k_max.
        if (dense && steps_ < maxSteps_) {
            upNear_ = good_ > steps_ && good_ - steps_ <= kCoarseQuanta;
            const bool coarse =
                !upNear_ && (w.misses * kCoarseDivisor >= w.cycles || w.meanExcessMs > kCoarseExcessMs);
            upQuanta_ = coarse ? kCoarseQuanta : 1;
            upHeld_ = steps_ + upQuanta_ > maxSteps_;
            steps_ = upHeld_ ? maxSteps_ : steps_ + upQuanta_;
            retry_ = inert_;
            if (retry_) {
                retryArmed_ = false;
                ++retries_;
                inertSinceMs_ = s.nowMs;
                testedAtHold_ = partsMean(0);
            }
            openEffect(Opened::Up, coarse);
            return Step::Up;
        }
        return Step::None;
    }
    // Down, a recovery trial, after downWait_ windows with headroom in a row
    // (5, doubled after each failed trial), and as many again before the
    // next: 0.25 while relaxing after a kick (the last step to the pre-kick
    // k + 0.25 whatever it is), else 0.05. The k before it is the working
    // point. Anything else holds.
    if (steps_ > 0 && cleanRun_ >= downWait_) {
        good_ = steps_;
        lastDownMs_ = s.nowMs;
        relaxStep_ = relaxing_ && steps_ > relaxTarget_;
        downQuanta_ = relaxStep_ ? (steps_ - relaxTarget_ < kCoarseQuanta ? steps_ - relaxTarget_ : kCoarseQuanta) : 1;
        steps_ -= downQuanta_;
        if (!relaxStep_ || steps_ <= relaxTarget_) relaxing_ = false;
        cleanRun_ = 0;
        return Step::Down;
    }
    return Step::None;
}

}  // namespace lodgov

namespace {

// --- The counters: one slot per worker thread -----------------------------------
// Tens of thousands of part tests a frame run on the engine's workers. A
// shared atomic per counter would put a contended read-modify-write on every
// one of them, so each thread owns a slot and bumps it with a plain load and
// store (it is the only writer); the frame boundary sums every slot and keeps
// the previous sum, so a frame's count is the difference (u32, wrap-safe).
// Threads past kSlots share one slot with real atomic adds.
enum : uint32_t {
    cRecords, cParts, cPartForeign, cPartFaults, cPartTableRange, cRecordFaults, cRecordTableRange,
    cPartBase,
};
// Per view class. pActing: tested at EDVR's LOD scale. pDrop: the price --
// observing, a pass that fails at s x k; acting, an engine reject the game's
// setting would have kept (its plane test run and passed). pUnverified: an
// acting reject whose LOD and screen-size terms say dropped but whose plane
// test could not run (no matched FUN_1404F4E10, or it faulted).
enum : uint32_t {
    pSeen, pPassed, pMismatch, pActing, pDrop, pChange, pUnverified, pHist0, pHist1, pHist2, pHist3, kPartFields
};
constexpr uint32_t kClasses = 3;   // eye A, eye B, every other view
enum : uint32_t { rSeen, rActing, rWouldFail, rChange, rMismatch, kRecFields };
constexpr uint32_t cRecBase = cPartBase + kClasses * kPartFields;   // eyes A and B
constexpr uint32_t cRecNotDispatched = cRecBase + 2 * kRecFields;
constexpr uint32_t cRecDispatchMismatch = cRecNotDispatched + 1;
constexpr uint32_t kCounters = cRecDispatchMismatch + 1;

// A whole number of cache lines, so no two threads' slots share one.
constexpr uint32_t kSlotWords = (kCounters + 15) / 16 * 16;
struct alignas(64) Slot {
    std::atomic<uint32_t> v[kSlotWords];
};
static_assert(sizeof(Slot) % 64 == 0, "a slot is whole cache lines");
constexpr uint32_t kSlots = 64;
Slot g_slots[kSlots];
Slot g_shared;
std::atomic<uint32_t> g_slotNext{0};
thread_local Slot* t_slot = nullptr;

Slot* mySlot() noexcept {
    Slot* s = t_slot;
    if (s) return s;
    const uint32_t i = g_slotNext.fetch_add(1, std::memory_order_acq_rel);
    s = i < kSlots ? &g_slots[i] : &g_shared;
    t_slot = s;
    return s;
}

inline void bump(Slot* s, uint32_t c) noexcept {
    if (s == &g_shared) s->v[c].fetch_add(1, std::memory_order_relaxed);
    else s->v[c].store(s->v[c].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void sumSlots(uint32_t out[kCounters]) noexcept {
    for (uint32_t c = 0; c < kCounters; ++c) out[c] = g_shared.v[c].load(std::memory_order_relaxed);
    const uint32_t claimed = g_slotNext.load(std::memory_order_acquire);
    const uint32_t n = claimed < kSlots ? claimed : kSlots;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t c = 0; c < kCounters; ++c) out[c] += g_slots[i].v[c].load(std::memory_order_relaxed);
}

// --- What the workers read and the boundary publishes -----------------------------
std::atomic<bool> g_live{false};                 // the observers do work
std::atomic<uint32_t> g_kBits{0x3F800000u};      // k as float bits, 1.0 while nothing has stepped
std::atomic<uint32_t> g_eyes{0xFFFFu};           // eye A's view bit | eye B's << 8; 0xFF unknown
std::atomic<uintptr_t> g_ctx{0};                 // the render context the builder saw last
std::atomic<uint32_t> g_scaleBits{0};            // its +0x30 as the builder last read it: the scale the tests ran at

inline float fromBits(uint32_t b) noexcept { float f; std::memcpy(&f, &b, 4); return f; }
inline uint32_t toBits(float f) noexcept { return lodgov::floatBits(f); }
inline float currentK() noexcept { return fromBits(g_kBits.load(std::memory_order_relaxed)); }

inline uint32_t classOf(uint32_t bit) noexcept {
    const uint32_t eyes = g_eyes.load(std::memory_order_relaxed);
    if (bit == (eyes & 0xFFu)) return 0;
    if (bit == ((eyes >> 8) & 0xFFu)) return 1;
    return 2;
}

// --- The write: the contexts, the acting switch, the setter's counts ---------------
// A context is written only if the draw-item builder has used it (the
// settlement's; the setter also rebuilds a second context each frame that the
// builder never reads) -- at most kContexts of them, a further one stands
// acting down. Slots fill in order and are cleared only while the governor
// is off. Written twice a frame at most: no cache-line padding.
struct CtxSlot {
    std::atomic<uintptr_t> ptr{0};          // the builder's render context (0: free)
    std::atomic<uint32_t> gameBits{0};      // what the setter stored on its last call: the game's s (0: none yet)
    std::atomic<uint32_t> heldBits{0};      // what EDVR wrote after that call (0: the game's value stands)
    std::atomic<uint32_t> writable{0};      // the page, VirtualQuery'd once: 0 unchecked, 1 writable, 2 refused
    std::atomic<uint32_t> firstState{0};    // the first-write line: 0 none, 1 claimed, 2 to log, 3 logged
    std::atomic<uint32_t> firstGame{0}, firstHeld{0}, firstK{0};
    std::atomic<uint32_t> scaledWindow{0};  // the summary window of its latest scaling (g_window)
};
CtxSlot g_contexts[lodgov::kContexts];
std::atomic<bool> g_contextOverflow{false};
// The contexts the setter was called with (the builder's or not), for the
// summary's count only.
struct SetterSeen {
    std::atomic<uintptr_t> ptr{0};
    std::atomic<uint32_t> calledWindow{0};  // the summary window of its latest setter call
};
SetterSeen g_setterSeen[lodgov::kContexts];
// The summary window's number (startWindow advances it): the setter tags what
// it did with it, so the summary counts contexts per window with no clock.
std::atomic<uint32_t> g_window{1};

std::atomic<bool> g_acting{false};               // configure: auto or reduced, observe 0, the setter hooked
std::atomic<const char*> g_standDown{nullptr};   // why acting stood down for the process (the first cause)
std::atomic<uintptr_t> g_standDownCtx{0};
std::atomic<uint32_t> g_setterCalls{0}, g_setterScaled{0}, g_setterImplausible{0}, g_setterFaults{0};
std::atomic<LodGovernorFrustumFn> g_frustum{nullptr};   // FUN_1404F4E10, matched, or null

inline bool actingNow() noexcept {
    return g_acting.load(std::memory_order_acquire) && !g_standDown.load(std::memory_order_acquire) &&
           !g_contextOverflow.load(std::memory_order_acquire);
}

CtxSlot* findContext(uintptr_t ctx) noexcept {
    if (!ctx) return nullptr;
    for (CtxSlot& s : g_contexts) {
        const uintptr_t p = s.ptr.load(std::memory_order_acquire);
        if (p == ctx) return &s;
        if (!p) return nullptr;
    }
    return nullptr;
}

void registerContext(uintptr_t ctx) noexcept {
    if (!ctx) return;
    for (CtxSlot& s : g_contexts) {
        const uintptr_t p = s.ptr.load(std::memory_order_acquire);
        if (p == ctx) return;
        if (!p) {
            uintptr_t expected = 0;
            if (s.ptr.compare_exchange_strong(expected, ctx, std::memory_order_acq_rel)) return;
            if (expected == ctx) return;
        }
    }
    g_contextOverflow.store(true, std::memory_order_release);
}

void noteSetterContext(uintptr_t ctx, uint32_t window) noexcept {
    for (SetterSeen& s : g_setterSeen) {
        uintptr_t p = s.ptr.load(std::memory_order_acquire);
        if (!p) {
            uintptr_t expected = 0;
            if (s.ptr.compare_exchange_strong(expected, ctx, std::memory_order_acq_rel)) p = ctx;
            else p = expected;
        }
        if (p == ctx) {
            s.calledWindow.store(window, std::memory_order_relaxed);
            return;
        }
    }
}

// Only while off (no observer runs): a fresh table for the next enable.
void resetContexts() noexcept {
    for (CtxSlot& s : g_contexts) {
        s.ptr.store(0, std::memory_order_relaxed);
        s.gameBits.store(0, std::memory_order_relaxed);
        s.heldBits.store(0, std::memory_order_relaxed);
        s.writable.store(0, std::memory_order_relaxed);
        s.firstState.store(0, std::memory_order_relaxed);
        s.scaledWindow.store(0, std::memory_order_relaxed);
    }
    for (SetterSeen& s : g_setterSeen) {
        s.ptr.store(0, std::memory_order_relaxed);
        s.calledWindow.store(0, std::memory_order_relaxed);
    }
    g_contextOverflow.store(false, std::memory_order_release);
    // The builder's last context is one it has used, and its observer
    // registers a context only when it differs from that one: it goes back
    // in at once. (A stale pointer here is harmless: only a setter call on
    // that very context, which proves it live, can write it.)
    registerContext(g_ctx.load(std::memory_order_acquire));
}

void standDown(const char* why, uintptr_t ctx) noexcept {
    const char* expected = nullptr;
    if (g_standDown.compare_exchange_strong(expected, why, std::memory_order_acq_rel))
        g_standDownCtx.store(ctx, std::memory_order_release);
}

// The game's own LOD scale for a context whose +0x30 the tests read as s: s
// itself unless the value in force is exactly the one EDVR wrote after the
// setter's last call.
float gameScaleFor(uintptr_t ctx, float s) noexcept {
    const CtxSlot* slot = findContext(ctx);
    if (!slot) return s;
    const uint32_t held = slot->heldBits.load(std::memory_order_acquire);
    if (!held || held != toBits(s)) return s;
    const uint32_t game = slot->gameBits.load(std::memory_order_acquire);
    return game ? fromBits(game) : s;
}

// --- The engine reads and the one write (SEH: a wild pointer drops the observation, never the flight) --
// POD locals only in the __try functions (/EHs units cannot unwind C++
// objects through __try; cull_gate_probe.cpp's rule).

bool readScale(uintptr_t ctx, float* out) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(ctx + 0x30), 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeScale(uintptr_t ctx, float value) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(ctx + 0x30), &value, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Committed, read-write (or execute-read-write), no guard page, for both ends
// of the four bytes.
bool pageWritable(uintptr_t at) noexcept {
    for (uintptr_t a : {at, at + 3}) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD p = mbi.Protect & 0xFFu;
        if (p != PAGE_READWRITE && p != PAGE_EXECUTE_READWRITE) return false;
    }
    return true;
}

bool contextWritable(CtxSlot& slot, uintptr_t ctx) noexcept {
    uint32_t w = slot.writable.load(std::memory_order_acquire);
    if (!w) {
        w = pageWritable(ctx + 0x30) ? 1u : 2u;
        slot.writable.store(w, std::memory_order_release);
    }
    return w == 1;
}

// The game's value back into a context still holding EDVR's: only if the page
// is still writable and the value there is exactly what EDVR wrote (else the
// engine has stored its own since, and that stands).
bool restoreOne(uintptr_t ctx, uint32_t held, uint32_t game) noexcept {
    if (!pageWritable(ctx + 0x30)) return false;
    __try {
        uint32_t now = 0;
        std::memcpy(&now, reinterpret_cast<const void*>(ctx + 0x30), 4);
        if (now != held) return false;
        std::memcpy(reinterpret_cast<void*>(ctx + 0x30), &game, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t restoreScaled() noexcept {
    uint32_t n = 0;
    for (CtxSlot& s : g_contexts) {
        const uintptr_t ctx = s.ptr.load(std::memory_order_acquire);
        if (!ctx) break;
        const uint32_t held = s.heldBits.exchange(0, std::memory_order_acq_rel);
        const uint32_t game = s.gameBits.load(std::memory_order_acquire);
        if (held && game && s.writable.load(std::memory_order_acquire) == 1 && restoreOne(ctx, held, game)) ++n;
    }
    return n;
}

// FUN_1442B3FC0 after its forward: param_1 = items (the builder's six-pointer
// block: [0] centre, [1] the model's +0x10 copy, [4] the LOD table copy, [5]
// the render context), param_2 = out {u32 LOD, u8 passed}, param_3 = view.
bool readPart(uintptr_t items, uintptr_t out, uintptr_t view, lodgov::PartInputs* in, uintptr_t* ctx) noexcept {
    __try {
        uint64_t block[6];
        std::memcpy(block, reinterpret_cast<const void*>(items), sizeof(block));
        uint8_t pass = 0;
        std::memcpy(&in->engineLod, reinterpret_cast<const void*>(out), 4);
        std::memcpy(&pass, reinterpret_cast<const void*>(out + 4), 1);
        in->enginePass = pass != 0;
        std::memcpy(in->centre, reinterpret_cast<const void*>(block[0]), 16);
        std::memcpy(in->sphere, reinterpret_cast<const void*>(block[1]), 16);
        std::memcpy(in->cam, reinterpret_cast<const void*>(view + 0x540), 16);
        std::memcpy(&in->A, reinterpret_cast<const void*>(view + 0x550), 4);
        std::memcpy(&in->B, reinterpret_cast<const void*>(view + 0x560), 4);
        uint64_t bits = 0;
        std::memcpy(&bits, reinterpret_cast<const void*>(view + 0x570), 8);
        std::memcpy(&in->s, reinterpret_cast<const void*>(block[5] + 0x30), 4);
        in->table = lodgov::LodTable::fromBytes(reinterpret_cast<const uint8_t*>(block[4]));
        unsigned long index = 0;
        in->bit = _BitScanForward64(&index, bits) ? static_cast<uint32_t>(index) : 64u;
        *ctx = static_cast<uintptr_t>(block[5]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The engine's own plane test on the part's sphere, as FUN_1442B3FC0 calls it
// (+0x8F..+0xA6: rcx the view, rdx the centre, r8 the sphere copy); its
// verdict's low 32 bits (-1: outside a plane).
bool planeTest(LodGovernorFrustumFn fn, uintptr_t view, const float* point, const float* interval,
               uint32_t* verdict) noexcept {
    __try {
        *verdict = static_cast<uint32_t>(fn(view, point, interval));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct RecordEye {
    bool seen, mismatch, wouldFail, change;
};
struct RecordOutcome {
    RecordEye eye[2];
    bool acting, dispatchMismatch, notDispatched, tableRange;
    uint32_t scaleBits;
};

// FUN_144308B30 (decomp_4308B30.txt) on the record the builder is about to
// build, from the inputs the traversal handed it (decomp_4312040.txt:154-165:
// centre rec+0x240, radius rec+0x280, table *(rec+0x20), the context's +0x30)
// and against the results the traversal stored and the builder's dispatch
// read (rec+0x208 mask, rec+0x210 nibbles by view bit, node +0x6A LOD count;
// decomp_4320340.txt:64-95). Per view bit: view = ctx + 0x40 + 0x6A0 *
// (u32 at ctx+0x1A840 + 4*bit), camera +0x540, A +0x550, B +0x560. Observing,
// what s x k would do; acting (s is EDVR's), the level against the game's s --
// a view the record lost at EDVR's s is not in rec+0x208 and cannot be told
// from one it was never tested for.
bool shadowRecord(uintptr_t ctx, uintptr_t nibbles, uint32_t eyes, float k, RecordOutcome* o) noexcept {
    __try {
        if (nibbles < 0x210) return false;
        const uintptr_t rec = nibbles - 0x210;
        uint64_t mask = 0, nib[4] = {}, tablePtr = 0, node = 0;
        float centre[4] = {}, radius = 0, s = 0;
        uint16_t lodCount = 0;
        std::memcpy(&mask, reinterpret_cast<const void*>(rec + 0x208), 8);
        std::memcpy(nib, reinterpret_cast<const void*>(rec + 0x210), 32);
        std::memcpy(&tablePtr, reinterpret_cast<const void*>(rec + 0x20), 8);
        std::memcpy(&node, reinterpret_cast<const void*>(rec + 0x18), 8);
        std::memcpy(centre, reinterpret_cast<const void*>(rec + 0x240), 16);
        std::memcpy(&radius, reinterpret_cast<const void*>(rec + 0x280), 4);
        std::memcpy(&lodCount, reinterpret_cast<const void*>(node + 0x6A), 2);
        std::memcpy(&s, reinterpret_cast<const void*>(ctx + 0x30), 4);
        std::memcpy(&o->scaleBits, &s, 4);
        const float sGame = gameScaleFor(ctx, s);
        const bool acting = toBits(s) != toBits(sGame);
        o->acting = acting;
        const lodgov::LodTable table = lodgov::LodTable::fromBytes(reinterpret_cast<const uint8_t*>(tablePtr));
        if (!table.inRange()) {
            o->tableRange = true;
            return true;
        }
        const uint32_t eyeBit[2] = {eyes & 0xFFu, (eyes >> 8) & 0xFFu};
        uint64_t eyeMask = 0;
        for (uint32_t e = 0; e < 2; ++e)
            if (eyeBit[e] < 64) eyeMask |= 1ull << eyeBit[e];
        // What the engine's dispatch saw: any view of the mask within the
        // node's LOD count. The builder runs, so this must hold.
        bool storedDispatch = false;
        for (uint64_t m = mask; m; m &= m - 1) {
            unsigned long bit = 0;
            _BitScanForward64(&bit, m);
            if ((static_cast<uint32_t>(nib[bit >> 4] >> ((bit & 15u) * 4u)) & 0xFu) <= lodCount) storedDispatch = true;
        }
        // Acting, "would the builder still be called at s x k" has no meaning
        // (s is already EDVR's): treated as settled.
        bool dispatch1 = false, dispatchK = acting;
        for (uint64_t m = mask; m; m &= m - 1) {
            unsigned long bit = 0;
            _BitScanForward64(&bit, m);
            const bool isEye = ((eyeMask >> bit) & 1u) != 0;
            // Once the record is known to stay dispatched both ways, only the
            // eyes are still worth a recompute.
            if (dispatch1 && dispatchK && !isEye) {
                if (!(m & eyeMask)) break;
                continue;
            }
            const uint32_t stored = static_cast<uint32_t>(nib[bit >> 4] >> ((bit & 15u) * 4u)) & 0xFu;
            uint32_t slot = 0;
            std::memcpy(&slot, reinterpret_cast<const void*>(ctx + 0x1A840 + uintptr_t(bit) * 4), 4);
            if (slot >= 64) {   // not a view this context holds: nothing to recompute against
                if (isEye) {
                    RecordEye& re = o->eye[bit == eyeBit[0] ? 0 : 1];
                    re.seen = re.mismatch = true;
                }
                continue;
            }
            const uintptr_t view = ctx + 0x40 + uintptr_t(slot) * 0x6A0;
            float cam[4] = {}, A = 0, B = 0;
            std::memcpy(cam, reinterpret_cast<const void*>(view + 0x540), 16);
            std::memcpy(&A, reinterpret_cast<const void*>(view + 0x550), 4);
            std::memcpy(&B, reinterpret_cast<const void*>(view + 0x560), 4);
            const float d = lodgov::engineDistance(centre, cam);
            uint32_t n1 = 0, n2 = 0;
            const bool p1 = lodgov::lodPick(table, lodgov::lodDistance(A, d, radius, s, B), &n1);
            const bool p2 = lodgov::lodPick(table, lodgov::lodDistance(A, d, radius, acting ? sGame : s * k, B), &n2);
            if (p1 && n1 <= lodCount) dispatch1 = true;
            if (!acting && p2 && n2 <= lodCount) dispatchK = true;
            if (isEye) {
                RecordEye& re = o->eye[bit == eyeBit[0] ? 0 : 1];
                re.seen = true;
                if (!p1 || n1 != stored) re.mismatch = true;
                else if (acting) re.change = !p2 || n2 != n1;
                else if (!p2) re.wouldFail = true;
                else if (n2 != n1) re.change = true;
            }
        }
        if (!storedDispatch || !dispatch1) o->dispatchMismatch = true;
        else if (!dispatchK) o->notDispatched = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The render context's view array at the frame boundary, for naming the eyes.
struct ViewInfo {
    float A, B;
    uint32_t bit;
    float cam[3];   // +0x540: the camera the part test measures distance from
};
bool readViews(uintptr_t ctx, ViewInfo* views, uint32_t* count) noexcept {
    __try {
        uint64_t n = 0;
        std::memcpy(&n, reinterpret_cast<const void*>(ctx + 0x1A940), 8);
        if (n > 64) return false;
        for (uint32_t i = 0; i < n; ++i) {
            const uintptr_t view = ctx + 0x40 + uintptr_t(i) * 0x6A0;
            uint64_t bits = 0;
            std::memcpy(&views[i].A, reinterpret_cast<const void*>(view + 0x550), 4);
            std::memcpy(&views[i].B, reinterpret_cast<const void*>(view + 0x560), 4);
            std::memcpy(views[i].cam, reinterpret_cast<const void*>(view + 0x540), 12);
            std::memcpy(&bits, reinterpret_cast<const void*>(view + 0x570), 8);
            unsigned long index = 0;
            views[i].bit = bits && !(bits & (bits - 1)) && _BitScanForward64(&index, bits) ? uint32_t(index) : 64u;
        }
        *count = static_cast<uint32_t>(n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- The runtime state (the caller thread's, and configure's, under g_mutex) -------
enum class Mode { Game, Auto, Reduced };

struct Window {
    uint64_t startMs = 0;
    uint32_t frames = 0, denseFrames = 0, eyeFrames = 0;
    uint32_t bandFrames = 0;     // frames in a settlement with 150-199 records (the density the lever may cut)
    uint32_t workSamples = 0, workOver = 0, workUnder = 0, workInvalid = 0;
    uint32_t callerSamples = 0, appSamples = 0;   // the valid samples by signal (WorkSource)
    uint32_t callerAbsent = 0;   // version 5 frames without valid caller work (inside workInvalid)
    uint32_t staleFrames = 0, expiries = 0;   // frames without fresh timing within 2 s; the evidence expiring
    // The display slots, over the fresh valid samples with a measured cycle:
    // how many, how many took two slots, and whose -- GPU-bound, unexplained,
    // the CPU's in frames with >= 200 records (what a rise needs).
    uint32_t cycles = 0, slotMisses = 0, slotGpuBound = 0, slotUnexplained = 0, denseOurMisses = 0;
    uint32_t kicks = 0, restores = 0, restoresKick = 0, inertHolds = 0, retries = 0, rearms = 0;
    uint32_t judgedAt[3] = {};   // the policy's up-step judgements at the window's start (benefit, none, not judged)
    double ceilingMs = 0;         // wall time at k_max with the trigger holding
    bool ceilingLogged = false;   // the ceiling line, once a window
    double workSum = 0, periodSum = 0;
    uint64_t recordsSum = 0, partsSum = 0;
    uint32_t recordsMax = 0, partsMax = 0;
    float kLow = 1.0f, kHigh = 1.0f;
    uint32_t up = 0, down = 0, resets = 0, clamps = 0, enters = 0;
    uint32_t upCoarse = 0;     // up steps of 0.25 (inside up)
    uint32_t aboard = 0;       // the k from before on foot restored aboard
    uint32_t footFrames = 0;   // frames held at k 1 on foot
    uint64_t sum[kCounters] = {};
    uint32_t dropMax[kClasses] = {};
    uint32_t notDispatchedMax = 0;
    // The setter's counters at the window's start (they only grow).
    uint32_t setterCalls = 0, setterScaled = 0, setterImplausible = 0, setterFaults = 0;
};

struct State {
    Mode mode = Mode::Game;
    bool configured = false;
    bool observe = false;   // advanced.settlement_detail_observe
    std::string modeText = "game";
    float kMaxCfg = lodgov::kDefaultMax;
    const char* attach = "not attempted";
    const char* partStatus = "not requested";
    const char* setterStatus = "not requested";
    bool builderHooked = false;
    bool planesMatched = false;
    lodgov::Policy policy;
    uint32_t prev[kCounters] = {};
    uint64_t lastSeq = 0;
    double firstPeriodMs = 0;
    // The frame-work signal the runtime's timing version fixes (a property of
    // the runtime, kept across off/on), and that version; logged when it is
    // first known and if it ever changes.
    lodgov::WorkSource source = lodgov::WorkSource::None;
    uint32_t timingVersion = 0;
    // Fresh evidence: the boundary time of the last fresh valid sample (0:
    // none, or expired since), and the epoch it belonged to -- the timing
    // generation, source and display period; a change of any expires it.
    uint64_t freshAtMs = 0;
    bool haveEpoch = false;
    uint64_t epochGeneration = 0;
    lodgov::WorkSource epochSource = lodgov::WorkSource::None;
    double epochPeriodMs = 0;
    Window w;
    uint64_t lastStepLogMs = 0;
    uint32_t stepsUnlogged = 0;
    uint32_t eyes = 0xFFFFu;
    float eyePixel = 0;
    uint32_t viewCount = 0;
    bool eyeCamValid = false;   // eye A's camera (+0x540), the view's identity for the lever's effect
    float eyeCam[3] = {};
    bool standDownLogged = false;   // process lifetime, like the stand-down
    bool overflowLogged = false;
    // The cockpit gate: on foot as the last boundary saw it, since when, for
    // how many frames; and whether the missing journal watcher was said.
    bool onFoot = false;
    uint64_t footStartMs = 0;
    uint32_t footFrames = 0;
    bool journalNoted = false;
    uint64_t prevBoundaryMs = 0;   // the previous boundary's wall time (the ceiling clock's steps)
    bool noCallerNoted = false;    // auto holding for want of caller work, said once
};

std::mutex g_mutex;
State g_state;

const char* modeName(Mode m) noexcept {
    return m == Mode::Auto ? "auto" : m == Mode::Reduced ? "reduced" : "game";
}

// Every line's tag: what the governor is doing to the game right now.
const char* modeTag(const State& st) noexcept {
    if (st.observe) return "observe only, never writes";
    if (g_standDown.load(std::memory_order_acquire) || g_contextOverflow.load(std::memory_order_acquire))
        return "acting stood down, observing";
    if (!g_acting.load(std::memory_order_acquire)) return "cannot act, observing";
    return "acting";
}

// The eyes: the two perspective views (B = 0, A > 0) with the finest pixel
// (the smallest A, within 1%), ordered by view bit -- A the lower. At the
// parked Cranfield capture (165433) these are bits 1 and 22, A = 0.000834297;
// every other perspective view there is about nine times coarser and every
// orthographic one has A = 0. Keyed by the view's bit, never its array
// index: 165433's eye B was view 5 in frame 2 and view 6 in frames 3-4.
// Anything else leaves the eyes unnamed.
void identifyEyes(State& st) noexcept {
    const uintptr_t ctx = g_ctx.load(std::memory_order_acquire);
    ViewInfo views[64];
    uint32_t n = 0;
    uint32_t eyes = 0xFFFFu;
    float pixel = 0;
    if (ctx && readViews(ctx, views, &n)) {
        float minA = 0;
        for (uint32_t i = 0; i < n; ++i)
            if (views[i].B == 0.0f && views[i].A > 0.0f && std::isfinite(views[i].A) && views[i].bit < 64 &&
                (minA == 0 || views[i].A < minA))
                minA = views[i].A;
        uint32_t found[2] = {64, 64}, count = 0;
        for (uint32_t i = 0; i < n && minA > 0; ++i)
            if (views[i].B == 0.0f && views[i].A > 0.0f && views[i].bit < 64 && views[i].A <= minA * 1.01f) {
                if (count < 2) found[count] = views[i].bit;
                ++count;
            }
        if (count == 2 && found[0] != found[1]) {
            const uint32_t lo = found[0] < found[1] ? found[0] : found[1];
            const uint32_t hi = found[0] < found[1] ? found[1] : found[0];
            eyes = lo | (hi << 8);
            pixel = minA;
        }
    }
    // Eye A's camera: the view's identity for the lever's effect.
    bool camValid = false;
    float cam[3] = {};
    for (uint32_t i = 0; i < n && eyes != 0xFFFFu; ++i)
        if (views[i].bit == (eyes & 0xFFu)) {
            camValid = std::isfinite(views[i].cam[0]) && std::isfinite(views[i].cam[1]) && std::isfinite(views[i].cam[2]);
            std::memcpy(cam, views[i].cam, sizeof(cam));
            break;
        }
    st.eyes = eyes;
    st.eyePixel = pixel;
    st.viewCount = n;
    st.eyeCamValid = camValid;
    std::memcpy(st.eyeCam, cam, sizeof(cam));
    g_eyes.store(eyes, std::memory_order_release);
}

// The frame-work signal a runtime's timing frame version fixes: version 5 and
// later carry the caller work per cycle; 3 and 4 do not, and the producer's
// application time stands in.
lodgov::WorkSource sourceOf(uint32_t timingVersion) noexcept {
    return timingVersion >= EDVR_NATIVE_TIMING_VERSION_5 ? lodgov::WorkSource::Caller
         : timingVersion >= EDVR_NATIVE_TIMING_VERSION_3 ? lodgov::WorkSource::App
                                                         : lodgov::WorkSource::None;
}

// The signal's name, as the summary and the step lines print it.
const char* workSourceName(lodgov::WorkSource s) noexcept {
    return s == lodgov::WorkSource::Caller ? "caller work per cycle"
         : s == lodgov::WorkSource::App ? "app work (pre-submit only; host older)"
                                        : "no runtime frame yet";
}

// The signal in full, as the configure line and the one-off source line say
// it. Short: the configure line is already most of the log's 1200-byte line.
void workSourceClause(lodgov::WorkSource s, uint32_t timingVersion, char* out, size_t n) noexcept {
    if (s == lodgov::WorkSource::Caller)
        std::snprintf(out, n, "frame work = caller work per cycle (runtime timing v%u: the caller thread from one pose "
                      "wait's return to the next one's entry, submits included)", timingVersion);
    else if (s == lodgov::WorkSource::App)
        std::snprintf(out, n, "frame work = app work (pre-submit only; host older: runtime timing v%u sends no caller "
                      "work, so pose wait end to submit plus the eye treatments stands in)", timingVersion);
    else
        std::snprintf(out, n, "frame work = caller work per cycle if the runtime sends it (timing v5), else app work "
                      "(pre-submit only; host older); the first runtime frame decides and a line names it");
}

// The application's GPU render time, the newest valid sample of the
// "Application-render GPU" instrument (gpu_frame_timing.h; the perf monitor's
// GPU figure reads the same): enabled, a result from the application-render
// source, reason Valid, at most 2 s old (its own age included). None: a
// two-slot cycle is never called GPU-bound.
void readGpu(uint64_t nowMs, lodgov::FrameSignals* sig) noexcept {
    const GpuFrameSnapshot g = gpuFrameSnapshot();
    const bool stampOk = g.capturedAtMs && g.capturedAtMs <= nowMs && g.result.ageMs < g.capturedAtMs;
    const uint64_t at = stampOk ? g.capturedAtMs - g.result.ageMs : 0;
    const double ms = g.result.outerMs;
    sig->gpuValid = g.enabled && g.haveResult && g.result.source == GpuSpanSource::ApplicationRender &&
                    g.result.reason == GpuSpanReason::Valid && at && at <= nowMs && nowMs - at <= 2000 &&
                    std::isfinite(ms) && ms >= 0.0 && ms <= 600000.0;
    sig->gpuMs = sig->gpuValid ? ms : 0.0;
}

// The newest producer sample, once: the runtime's caller work per cycle
// (EdvrNativeTimingFrame version 5, callerWorkMs) against the display period.
// A version 3 or 4 runtime sends none, and NativeTimingSnapshot::applicationMs
// (the monitor's "app CPU", the pre-submit phase only) stands in. A version 5
// frame without valid caller work is an invalid sample: the two figures are
// never mixed in one run. FRESH evidence only (the review of 2026-09-23,
// finding 1): a sample is Valid once, at the boundary that first sees its
// new sequence, and only if it was captured within 2 s. The evidence EXPIRES
// (sig->expired, once a stale spell) on a lost lease, an invalid frame, a
// sample already older than 2 s, a sequence unchanged for more than 2 s, or
// a change of the timing generation, source or display period.
void readWork(State& st, uint64_t nowMs, lodgov::FrameSignals* sig) noexcept {
    const NativeTimingSnapshot t = nativeTimingSnapshot();
    auto expire = [&]() {
        if (st.freshAtMs) sig->expired = true;
        st.freshAtMs = 0;
    };
    if (!t.active) {   // no native timing lease: no sample, and what was held expires
        expire();
        return;
    }
    if (t.invalid || !t.haveCpu) {   // the newest frame failed or none is published
        sig->work = lodgov::Work::Invalid;
        expire();
        return;
    }
    if (!t.sequence || t.sequence == st.lastSeq) {   // nothing new since the last boundary
        if (st.freshAtMs && nowMs - st.freshAtMs > lodgov::kFreshMs) expire();
        return;
    }
    st.lastSeq = t.sequence;
    if (std::isfinite(t.predictedPeriodMs) && t.predictedPeriodMs > 0 && t.predictedPeriodMs <= 10000 &&
        st.firstPeriodMs == 0)
        st.firstPeriodMs = t.predictedPeriodMs;
    double period = st.firstPeriodMs;
    if (t.cpu.version >= EDVR_NATIVE_TIMING_VERSION_4 && std::isfinite(t.cpu.baseDisplayHz) &&
        t.cpu.baseDisplayHz > 0 && t.cpu.baseDisplayHz <= 1000)
        period = 1000.0 / double(t.cpu.baseDisplayHz);
    const bool fresh = t.capturedAtMs && t.capturedAtMs <= nowMs && nowMs - t.capturedAtMs <= lodgov::kFreshMs;
    sig->timingVersion = t.cpu.version;
    sig->source = sourceOf(t.cpu.version);
    double ms = 0;
    bool have = false;
    if (sig->source == lodgov::WorkSource::Caller) {
        ms = t.cpu.callerWorkMs;
        have = t.cpu.callerWorkValid == 1 && std::isfinite(ms) && ms >= 0 && ms <= 600000;
        sig->callerAbsent = !have;
    } else if (sig->source == lodgov::WorkSource::App) {
        ms = t.applicationMs;
        have = t.applicationValid && std::isfinite(ms) && ms >= 0 && ms <= 600000;
    }
    const bool ok = have && fresh && period > 0;
    sig->work = ok ? lodgov::Work::Valid : lodgov::Work::Invalid;
    sig->workMs = ok ? ms : 0;
    sig->periodMs = period;
    if (!ok) {
        expire();
        return;
    }
    // A new epoch -- another timing generation, signal or display period --
    // expires what the old one gathered; this sample begins the new one.
    if (st.haveEpoch && (t.generation != st.epochGeneration || sig->source != st.epochSource ||
                         std::fabs(period - st.epochPeriodMs) > 1e-6))
        sig->expired = true;
    st.haveEpoch = true;
    st.epochGeneration = t.generation;
    st.epochSource = sig->source;
    st.epochPeriodMs = period;
    st.freshAtMs = nowMs;
}

double perFrame(uint64_t total, uint32_t frames) noexcept { return frames ? double(total) / frames : 0.0; }

// A count rounded to a whole number, with thousands separators (4,879).
void grouped(double v, char* out, size_t n) noexcept {
    const unsigned long long u = v > 0.0 ? static_cast<unsigned long long>(std::llround(v)) : 0ull;
    char digits[32];
    const int len = std::snprintf(digits, sizeof(digits), "%llu", u);
    char text[48];
    int j = 0;
    for (int i = 0; i < len && j < int(sizeof(text)) - 2; ++i) {
        if (i && (len - i) % 3 == 0) text[j++] = ',';
        text[j++] = digits[i];
    }
    text[j] = '\0';
    std::snprintf(out, n, "%s", text);
}

// The game's own LOD scale now, for the lines: the value the setter stored for
// the builder's context, else -- nothing having been written without a setter
// call -- the value the builder read. 0 when neither is known.
float gameScaleNow(bool* fromSetter) noexcept {
    *fromSetter = false;
    const CtxSlot* slot = findContext(g_ctx.load(std::memory_order_acquire));
    const uint32_t game = slot ? slot->gameBits.load(std::memory_order_acquire) : 0;
    if (game) {
        *fromSetter = true;
        return fromBits(game);
    }
    const float read = fromBits(g_scaleBits.load(std::memory_order_relaxed));
    return std::isfinite(read) && read > 0.0f ? read : 0.0f;
}

// s x k for the lines: the game's own scale times k, or unknown before any read.
void effectiveText(float k, char* out, size_t n) noexcept {
    bool fromSetter = false;
    const float sGame = gameScaleNow(&fromSetter);
    if (sGame > 0.0f) std::snprintf(out, n, "%.3f", double(sGame) * double(k));
    else std::snprintf(out, n, "unknown");
}

// The outcome at k_max, in the lines' words.
const char* outcomeName(lodgov::Outcome o) noexcept {
    switch (o) {
    case lodgov::Outcome::Reached: return "target reached";
    case lodgov::Outcome::Residual: return "residual benefit";
    case lodgov::Outcome::NoBenefit: return "no observed benefit";
    default: return "unknown (no fresh evidence)";
    }
}

// What a step, or the ceiling, moved: "tested 9,720 -> 9,618, passed 4,879 ->
// 4,872 parts a frame, caller work 12.40 -> 12.30 ms" (both eyes; passed at
// EDVR's scale), a side not measured said so.
void effectText(const lodgov::EffectFigures& f, char* out, size_t n) noexcept {
    char parts[128], caller[80];
    if (f.tested[0] >= 0.0 && f.tested[1] >= 0.0) {
        char t0[32], t1[32], p0[32], p1[32];
        grouped(f.tested[0], t0, sizeof(t0));
        grouped(f.tested[1], t1, sizeof(t1));
        grouped(f.passed[0], p0, sizeof(p0));
        grouped(f.passed[1], p1, sizeof(p1));
        std::snprintf(parts, sizeof(parts), "tested %s -> %s, passed %s -> %s parts a frame", t0, t1, p0, p1);
    } else {
        std::snprintf(parts, sizeof(parts), "parts not measured (under 10 frames of builder work)");
    }
    if (f.caller[0] >= 0.0 && f.caller[1] >= 0.0)
        std::snprintf(caller, sizeof(caller), "caller work %.2f -> %.2f ms", f.caller[0], f.caller[1]);
    else
        std::snprintf(caller, sizeof(caller), "caller work not measured (under 10 fresh samples)");
    std::snprintf(out, n, "%s, %s", parts, caller);
}

// A run of steps in words: "two 0.25 steps", "four 0.05 steps", "three
// steps (one of 0.25, two of 0.05)".
void stepsText(uint32_t coarse, uint32_t fine, char* out, size_t n) noexcept {
    static const char* const kWords[] = {"no", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
    char allWord[16], coarseWord[16], fineWord[16];
    auto word = [](uint32_t v, char* buf, size_t m) {
        if (v < 10) std::snprintf(buf, m, "%s", kWords[v]);
        else std::snprintf(buf, m, "%u", v);
    };
    word(coarse + fine, allWord, sizeof(allWord));
    word(coarse, coarseWord, sizeof(coarseWord));
    word(fine, fineWord, sizeof(fineWord));
    if (!fine) std::snprintf(out, n, "%s 0.25 step%s", coarseWord, coarse == 1 ? "" : "s");
    else if (!coarse) std::snprintf(out, n, "%s 0.05 step%s", fineWord, fine == 1 ? "" : "s");
    else std::snprintf(out, n, "%s steps (%s of 0.25, %s of 0.05)", allWord, coarseWord, fineWord);
}

void logSummary(State& st, uint64_t nowMs) {
    const Window& w = st.w;
    if (!w.frames) return;
    const float kNow = st.policy.k();
    const char* tag = modeTag(st);
    char eyesText[96];
    if (st.eyes != 0xFFFFu)
        std::snprintf(eyesText, sizeof(eyesText), "eye views bits %u / %u (pixel %.6g per metre, %u views; named on %u of "
                      "%u frames)", st.eyes & 0xFFu, (st.eyes >> 8) & 0xFFu, st.eyePixel, st.viewCount, w.eyeFrames,
                      w.frames);
    else
        std::snprintf(eyesText, sizeof(eyesText), "eye views NOT named now (named on %u of %u frames)", w.eyeFrames,
                      w.frames);
    // auto without caller work (a timing v3/v4 runtime) cannot attribute a
    // miss, so it holds: said once, at the first summary, and in each.
    const bool noCaller = !st.policy.fixed() && w.appSamples && !w.callerSamples;
    if (noCaller && !st.noCallerNoted) {
        st.noCallerNoted = true;
        Log::get().note("settlement detail: the runtime sends no caller work (timing v%u), so misses cannot be "
                        "attributed to the CPU: auto holds k at 1", st.timingVersion);
    }
    char stuck[200] = "";
    if (w.kHigh <= 1.0f) {
        if (w.footFrames >= w.frames)
            std::snprintf(stuck, sizeof(stuck), "; k stayed 1: on foot the whole window (the governor is for the cockpit only)");
        else if (!w.recordsSum)
            std::snprintf(stuck, sizeof(stuck), "; k stayed 1: no draw-item builder calls (no settlement records, or the "
                          "builder hook ran nothing)");
        else if (!w.denseFrames)
            std::snprintf(stuck, sizeof(stuck), "; k stayed 1: never 200 builder records in a frame");
        else if (st.policy.fixed())
            stuck[0] = '\0';
        else if (!w.workSamples)
            std::snprintf(stuck, sizeof(stuck), "; k stayed 1: no valid frame-work sample from the native runtime");
        else if (noCaller)
            std::snprintf(stuck, sizeof(stuck), "; k stayed 1: no caller work from the runtime, so no miss can be "
                          "attributed to the CPU: holding");
        else if (!w.denseOurMisses)
            std::snprintf(stuck, sizeof(stuck), "; k stayed 1: no cycle took two display slots as the CPU's in a frame "
                          "with 200 records%s", w.slotGpuBound || w.slotUnexplained ? " (only GPU-bound or unexplained "
                          "ones)" : "");
        else
            std::snprintf(stuck, sizeof(stuck), "; k stayed 1: no second had a tenth of its cycles take two display "
                          "slots as the CPU's (under a tenth is the dead band)");
    }
    // The LOD scale: the game's own (from the setter), what the tests ran at
    // (the builder's read), and the setter's counts. A write path that never
    // ran reads as setter calls 0 or scaled 0 while k rose above 1.
    bool fromSetter = false;
    const float sGame = gameScaleNow(&fromSetter);
    const float held = fromBits(g_scaleBits.load(std::memory_order_relaxed));
    char effective[48];
    if (sGame > 0.0f)
        std::snprintf(effective, sizeof(effective), "%.3f", double(sGame) * double(kNow));
    else
        std::snprintf(effective, sizeof(effective), "unknown (no LOD scale read yet)");
    const uint32_t calls = g_setterCalls.load(std::memory_order_relaxed) - w.setterCalls;
    const uint32_t scaled = g_setterScaled.load(std::memory_order_relaxed) - w.setterScaled;
    const uint32_t implausible = g_setterImplausible.load(std::memory_order_relaxed) - w.setterImplausible;
    const uint32_t faults = g_setterFaults.load(std::memory_order_relaxed) - w.setterFaults;
    uint32_t scaledPointers = 0, builderContexts = 0, setterPointers = 0;
    const uint32_t window = g_window.load(std::memory_order_relaxed);
    for (const CtxSlot& s : g_contexts) {
        if (!s.ptr.load(std::memory_order_acquire)) break;
        ++builderContexts;
        if (s.scaledWindow.load(std::memory_order_relaxed) == window) ++scaledPointers;
    }
    for (const SetterSeen& s : g_setterSeen)
        if (s.ptr.load(std::memory_order_acquire) && s.calledWindow.load(std::memory_order_relaxed) == window)
            ++setterPointers;
    char gameText[80], heldText[32];
    if (sGame > 0.0f)
        std::snprintf(gameText, sizeof(gameText), fromSetter ? "%.3f" : "%.3f (the builder's read: no setter call yet)",
                      double(sGame));
    else
        std::snprintf(gameText, sizeof(gameText), "unknown");
    if (std::isfinite(held) && held > 0.0f) std::snprintf(heldText, sizeof(heldText), "%.3f", double(held));
    else std::snprintf(heldText, sizeof(heldText), "unknown");
    const char* notActing = "";
    if (std::strcmp(tag, "acting") == 0 && w.kHigh > 1.0f) {
        if (!calls)
            notActing = "; NOT ACTING: k rose above 1 but FUN_142819D90's hook ran 0 times, so nothing was written";
        else if (!scaled)
            notActing = "; NOT ACTING: k rose above 1 but no setter call was on a context the draw-item builder used";
    }
    // Which figure the frame work was: one per runtime in practice, both
    // named if a window ever saw both.
    char source[160];
    if (w.callerSamples && w.appSamples)
        std::snprintf(source, sizeof(source), "%s on %u samples and %s on %u",
                      workSourceName(lodgov::WorkSource::Caller), w.callerSamples,
                      workSourceName(lodgov::WorkSource::App), w.appSamples);
    else
        std::snprintf(source, sizeof(source), "%s",
                      workSourceName(w.callerSamples ? lodgov::WorkSource::Caller
                                     : w.appSamples  ? lodgov::WorkSource::App
                                                     : st.source));
    char enters[64] = "";
    if (w.enters && w.aboard)
        std::snprintf(enters, sizeof(enters), ", %u to k_max, %u restored aboard", w.enters, w.aboard);
    else if (w.enters)
        std::snprintf(enters, sizeof(enters), ", %u to k_max", w.enters);
    else if (w.aboard)
        std::snprintf(enters, sizeof(enters), ", %u restored aboard", w.aboard);
    Log::get().note(
        "settlement detail (%s): %.1f s, %u frames: k now %.2f, effective s x k %s (window %.2f..%.2f of max %.2f; %u "
        "up (%u by 0.25), %u down, %u resets, %u clamps%s; held on foot %u frames); builder records/frame %.1f (max "
        "%u, >= 200 on %u frames, 150-199 in a settlement on %u), part tests/frame %.1f (max %u); frame work = %s: "
        "%.2f ms mean vs period %.2f ms over %u samples (over by > 0.30 ms: %u, under by > 1.00 ms: %u, invalid %u, "
        "caller work absent %u; no fresh timing on %u frames, %u expiries); LOD scale: game s %s, held %s (k %.2f); "
        "setter calls %u (scaled %u) on %u pointers (called with %u; builder contexts %u); implausible %u; faults %u; "
        "%s%s%s.",
        tag, double(nowMs - w.startMs) / 1000.0, w.frames, kNow, effective, w.kLow, w.kHigh, st.policy.kMax(), w.up,
        w.upCoarse, w.down, w.resets, w.clamps, enters, w.footFrames, perFrame(w.recordsSum, w.frames), w.recordsMax,
        w.denseFrames, w.bandFrames, perFrame(w.partsSum, w.frames), w.partsMax, source,
        w.workSamples ? w.workSum / w.workSamples : 0.0, w.workSamples ? w.periodSum / w.workSamples : 0.0,
        w.workSamples, w.workOver, w.workUnder, w.workInvalid, w.callerAbsent, w.staleFrames, w.expiries, gameText,
        heldText, kNow, calls, scaled, scaledPointers, setterPointers, builderContexts, implausible, faults, eyesText,
        stuck, notActing);
    // The decisions: the slots and whose, the kicks and the trials undone,
    // the lever's benefit, and at k_max the outcome.
    char outcome[64] = "";
    if (st.policy.maxSteps() > 0 && st.policy.steps() == st.policy.maxSteps())
        std::snprintf(outcome, sizeof(outcome), " (at k_max now: %s)", outcomeName(st.policy.ceilingOutcome()));
    const uint32_t cpuMisses = w.slotMisses - w.slotGpuBound - w.slotUnexplained;
    Log::get().note(
        "settlement detail (%s) decisions: slots missed %u of %u (the CPU's %u, GPU-bound %u, unexplained %u)%s; "
        "kicks %u; restores %u (%u after a failed kick); the next recovery trial after %u clean seconds; up steps' "
        "benefit %u yes, %u no, %u not judged (the scene changing, or too few samples); inert holds %u (retries %u, "
        "re-armed %u); parts a frame: tested %.1f, passed at EDVR's scale %.1f, dropped %.1f; at the ceiling with "
        "misses %.1f s%s.",
        tag, w.slotMisses, w.cycles, cpuMisses, w.slotGpuBound, w.slotUnexplained,
        noCaller ? " (no caller work: holding)" : "", w.kicks, w.restores, w.restoresKick, st.policy.downWait(),
        st.policy.judgedBenefit() - w.judgedAt[0], st.policy.judgedNone() - w.judgedAt[1],
        st.policy.notJudged() - w.judgedAt[2], w.inertHolds, w.retries, w.rearms, st.policy.testedMean(),
        st.policy.passedMean(), st.policy.droppedMean(), w.ceilingMs / 1000.0, outcome);
    if (!w.recordsSum && !w.partsSum) return;   // nothing built: the header says so
    for (uint32_t e = 0; e < 2; ++e) {
        const uint64_t* p = &w.sum[cPartBase + e * kPartFields];
        const uint64_t* r = &w.sum[cRecBase + e * kRecFields];
        const uint32_t bit = e ? (st.eyes >> 8) & 0xFFu : st.eyes & 0xFFu;
        if (p[pActing] || r[rActing]) {
            Log::get().note(
                "settlement detail (%s) eye %c (view bit %u now): parts tested %.1f/frame (%.1f at EDVR's LOD scale), "
                "engine passed %.1f; dropped (the game's setting would have kept it) %.1f/frame (max %u), LOD level "
                "changed from the game's %.1f/frame, plane test not run %llu; dropped angular radius r/d < 0.25 deg "
                "%llu, 0.25-0.5 %llu, 0.5-1 %llu, >= 1 %llu (window totals); records passed %.1f/frame (%.1f at "
                "EDVR's LOD scale), level changed from the game's %.1f, would lose the eye %.1f; a record that lost "
                "the eye at EDVR's scale is not seen; disagreements with the engine at the LOD scale it held: parts "
                "%llu, records %llu.",
                tag, e ? 'B' : 'A', bit, perFrame(p[pSeen], w.frames), perFrame(p[pActing], w.frames),
                perFrame(p[pPassed], w.frames), perFrame(p[pDrop], w.frames), w.dropMax[e],
                perFrame(p[pChange], w.frames), (unsigned long long)p[pUnverified], (unsigned long long)p[pHist0],
                (unsigned long long)p[pHist1], (unsigned long long)p[pHist2], (unsigned long long)p[pHist3],
                perFrame(r[rSeen], w.frames), perFrame(r[rActing], w.frames), perFrame(r[rChange], w.frames),
                perFrame(r[rWouldFail], w.frames), (unsigned long long)p[pMismatch], (unsigned long long)r[rMismatch]);
        } else {
            Log::get().note(
                "settlement detail (%s) eye %c (view bit %u now): parts tested %.1f/frame, engine passed %.1f; at the "
                "shadow k would drop %.1f/frame (max %u), change LOD level %.1f/frame; would-drop angular radius r/d < "
                "0.25 deg %llu, 0.25-0.5 %llu, 0.5-1 %llu, >= 1 %llu (window totals); records passed %.1f/frame, would "
                "lose the eye %.1f, change level %.1f; disagreements with the engine at the LOD scale it held: parts "
                "%llu, records %llu.",
                tag, e ? 'B' : 'A', bit, perFrame(p[pSeen], w.frames), perFrame(p[pPassed], w.frames),
                perFrame(p[pDrop], w.frames), w.dropMax[e], perFrame(p[pChange], w.frames),
                (unsigned long long)p[pHist0], (unsigned long long)p[pHist1], (unsigned long long)p[pHist2],
                (unsigned long long)p[pHist3], perFrame(r[rSeen], w.frames), perFrame(r[rWouldFail], w.frames),
                perFrame(r[rChange], w.frames), (unsigned long long)p[pMismatch], (unsigned long long)r[rMismatch]);
        }
    }
    const uint64_t* o = &w.sum[cPartBase + 2 * kPartFields];
    // Acting, a record left with no view never reaches the builder: what
    // observing counts as "would not be called for" has no acting twin.
    const bool otherActing = o[pActing] != 0;
    char records[128];
    if (otherActing)
        std::snprintf(records, sizeof(records), "records the builder was not called for: not seen at EDVR's scale");
    else
        std::snprintf(records, sizeof(records), "records the builder would not be called for at all %.1f/frame (max %u)",
                      perFrame(w.sum[cRecNotDispatched], w.frames), w.notDispatchedMax);
    Log::get().note(
        "settlement detail (%s) other views: parts tested %.1f/frame, engine passed %.1f, %s %.1f/frame (max %u), %s "
        "%.1f/frame, disagreements %llu; %s, dispatch disagreements %llu; unreadable: parts %llu, records %llu; part "
        "tests from another caller %llu; tables past 7 levels: parts %llu, records %llu.",
        tag, perFrame(o[pSeen], w.frames), perFrame(o[pPassed], w.frames),
        otherActing ? "dropped (the game's setting would have kept it)" : "would drop", perFrame(o[pDrop], w.frames),
        w.dropMax[2], otherActing ? "level changed from the game's" : "change level", perFrame(o[pChange], w.frames),
        (unsigned long long)o[pMismatch], records, (unsigned long long)w.sum[cRecDispatchMismatch],
        (unsigned long long)w.sum[cPartFaults], (unsigned long long)w.sum[cRecordFaults],
        (unsigned long long)w.sum[cPartForeign], (unsigned long long)w.sum[cPartTableRange],
        (unsigned long long)w.sum[cRecordTableRange]);
}

void startWindow(State& st, uint64_t nowMs) {
    g_window.fetch_add(1, std::memory_order_relaxed);
    st.w = Window{};
    st.w.startMs = nowMs;
    st.w.kLow = st.w.kHigh = st.policy.k();
    st.w.judgedAt[0] = st.policy.judgedBenefit();
    st.w.judgedAt[1] = st.policy.judgedNone();
    st.w.judgedAt[2] = st.policy.notJudged();
    st.w.setterCalls = g_setterCalls.load(std::memory_order_relaxed);
    st.w.setterScaled = g_setterScaled.load(std::memory_order_relaxed);
    st.w.setterImplausible = g_setterImplausible.load(std::memory_order_relaxed);
    st.w.setterFaults = g_setterFaults.load(std::memory_order_relaxed);
}

void logStep(State& st, lodgov::Step step, float from, const lodgov::FrameSignals& sig, uint64_t nowMs) {
    // A kick, a restore and the return aboard are never held back by the 5 s
    // rate limit: they say the ordinary steps failed, a trial did, or k
    // jumped back to where it was.
    if (step != lodgov::Step::Kick && step != lodgov::Step::Restore && step != lodgov::Step::Aboard &&
        st.lastStepLogMs && nowMs - st.lastStepLogMs < 5000) {
        ++st.stepsUnlogged;
        return;
    }
    // Every up, down, kick and restore is a window's decision: an up step
    // names how many of the second's cycles took two slots as the CPU's (and
    // the others), their mean caller work, and why its size; a down step the
    // clean seconds, the margin and the k it is a trial against; a kick the
    // seconds in a row and the pre-kick k; a restore the trial that failed.
    char why[420];
    const lodgov::WindowResult& win = st.policy.lastWindow();
    const double mean = win.meanExcessMs;
    if (step == lodgov::Step::Up) {
        const char* size = st.policy.upQuanta() > 1
            ? (win.misses * lodgov::kCoarseDivisor >= win.cycles ? "a quarter or more the CPU's: the coarse step"
                                                                 : "more than 1.00 ms over: the coarse step")
            : st.policy.upNearGood() ? "within 0.25 below the working point: the fine step"
                                     : "under a quarter the CPU's and 1.00 ms over or less: the fine step";
        std::snprintf(why, sizeof(why), "up %.2f: %u of %u cycles in the last second took two display slots as the "
                      "CPU's (%u GPU-bound, %u unexplained), their mean caller work %.2f ms %s (%s%s)%s",
                      double(st.policy.upQuanta()) / lodgov::kQuantaPerUnit, win.misses, win.cycles, win.gpuBound,
                      win.unexplained, std::fabs(mean), mean < 0.0 ? "under" : "over", size,
                      st.policy.upHeld() ? ", held to k_max" : "",
                      st.policy.retrying() ? "; a retry while the lever is inert here" : "");
    } else if (step == lodgov::Step::Aboard) {
        std::snprintf(why, sizeof(why), "back aboard: k restored to %.2f (held on foot %u frames)",
                      double(st.policy.k()), st.footFrames);
    } else if (step == lodgov::Step::Restore && st.policy.restoredAfterKick()) {
        std::snprintf(why, sizeof(why), "restored k %.2f after a failed kick: the fifth second at k_max still had a "
                      "tenth or more of its cycles take two display slots as the CPU's (%u of %u); no kick for 60 s",
                      double(st.policy.k()), win.misses, win.cycles);
    } else if (step == lodgov::Step::Restore) {
        std::snprintf(why, sizeof(why), "restored k %.2f after a failed recovery trial: a second within 10 s of the "
                      "step down had a tenth or more of its cycles take two display slots as the CPU's (%u of %u); "
                      "the next trial after %u clean seconds", double(st.policy.k()), win.misses, win.cycles,
                      st.policy.downWait());
    } else if (step == lodgov::Step::Down && st.policy.relaxStep()) {
        std::snprintf(why, sizeof(why), "down %.2f, relaxing 0.25 a step after the kick toward k %.2f (the pre-kick "
                      "%.2f + 0.25): %u clean seconds, the last one's caller work %.2f ms under the period; a trial "
                      "against k %.2f", double(st.policy.downQuanta()) / lodgov::kQuantaPerUnit,
                      double(st.policy.relaxTargetK()), double(st.policy.preKickK()), st.policy.downWait(),
                      std::fabs(mean), double(st.policy.goodK()));
    } else if (step == lodgov::Step::Down) {
        std::snprintf(why, sizeof(why), "down 0.05: %u clean seconds in a row (no cycle took two display slots), the "
                      "last one's caller work %.2f ms under the period (more than 1.00 ms to spare); a trial: k %.2f "
                      "comes back if a second triggers within 10 s", st.policy.downWait(), std::fabs(mean),
                      double(st.policy.goodK()));
    } else if (step == lodgov::Step::Kick) {
        std::snprintf(why, sizeof(why), "kick: %u seconds in a row with a tenth or more of the cycles taking two slots "
                      "as the CPU's (the last %u of %u), the steps not clearing it: from the pre-kick k %.2f to k_max "
                      "%.2f so consecutive frames fit and the runtime returns to full rate; a trial, judged on the "
                      "fifth second at k_max", st.policy.triggerRun(), win.misses, win.cycles,
                      double(st.policy.preKickK()), double(st.policy.kMax()));
    } else {
        std::snprintf(why, sizeof(why), "%s",
                      step == lodgov::Step::Reset ? "reset: under 150 builder records for 30 frames"
                      : step == lodgov::Step::Enter ? "reduced: in a settlement (>= 200 builder records), k = k_max at once"
                                                    : "clamped to the new advanced.settlement_detail_max");
    }
    char more[48] = "";
    if (st.stepsUnlogged) std::snprintf(more, sizeof(more), " (+%u steps since the last line)", st.stepsUnlogged);
    char work[128] = "no frame-work sample this frame";
    if (sig.work == lodgov::Work::Valid)
        std::snprintf(work, sizeof(work), "frame work = %s: %.2f ms vs period %.2f ms", workSourceName(sig.source),
                      sig.workMs, sig.periodMs);
    // Acting: the scale the next setter call will write for the builder's context.
    char scale[64] = "";
    const char* tag = modeTag(st);
    if (std::strcmp(tag, "acting") == 0) {
        bool fromSetter = false;
        const float sGame = gameScaleNow(&fromSetter);
        if (sGame > 0.0f && fromSetter)
            std::snprintf(scale, sizeof(scale), " -> LOD scale s x k %.3f", double(sGame) * double(st.policy.k()));
        else
            std::snprintf(scale, sizeof(scale), " -> LOD scale s x k unknown (no setter call yet)");
    }
    Log::get().note("settlement detail (%s): k %.2f -> %.2f, %s; %u builder records, %s%s%s.", tag, from,
                    st.policy.k(), why, sig.records, work, scale, more);
    st.lastStepLogMs = nowMs;
    st.stepsUnlogged = 0;
}

// The write's one-off lines, from the caller thread: each context's first
// scaling, and a stand-down (after which the game's value is written back).
void logWriteEvents(State& st) {
    for (CtxSlot& s : g_contexts) {
        const uintptr_t ctx = s.ptr.load(std::memory_order_acquire);
        if (!ctx) break;
        if (s.firstState.load(std::memory_order_acquire) != 2) continue;
        Log::get().note("settlement detail: LOD scale scaled: game s %.3f -> %.3f (k %.2f), ctx 0x%llX.",
                        double(fromBits(s.firstGame.load(std::memory_order_relaxed))),
                        double(fromBits(s.firstHeld.load(std::memory_order_relaxed))),
                        double(fromBits(s.firstK.load(std::memory_order_relaxed))), (unsigned long long)ctx);
        s.firstState.store(3, std::memory_order_release);
    }
    const char* why = g_standDown.load(std::memory_order_acquire);
    if (why && !st.standDownLogged) {
        st.standDownLogged = true;
        const uint32_t restored = restoreScaled();
        Log::get().note("settlement detail: acting STOOD DOWN for this process: %s (ctx 0x%llX); observing only from "
                        "now, the game's LOD scale written back to %u context(s) (and stored by the engine itself "
                        "from the next frame).",
                        why, (unsigned long long)g_standDownCtx.load(std::memory_order_acquire), restored);
    }
    if (g_contextOverflow.load(std::memory_order_acquire) && !st.overflowLogged) {
        st.overflowLogged = true;
        const uint32_t restored = restoreScaled();
        Log::get().note("settlement detail: acting STOOD DOWN: the draw-item builder used more than %u render "
                        "contexts; observing only until fix.settlement_detail is switched off and on (the game's LOD "
                        "scale written back to %u context(s)).",
                        lodgov::kContexts, restored);
    }
}

// nowMs: wall time (the windows, the policy's seconds); clockMs: this
// boundary's QueryPerformanceCounter time in ms (the display slot).
void frameBoundaryAt(uint64_t nowMs, double clockMs) {
    if (!g_live.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    State& st = g_state;
    uint32_t now[kCounters], d[kCounters];
    sumSlots(now);
    for (uint32_t c = 0; c < kCounters; ++c) {
        d[c] = now[c] - st.prev[c];
        st.prev[c] = now[c];
    }
    identifyEyes(st);
    lodgov::FrameSignals sig;
    sig.records = d[cRecords];
    sig.nowMs = nowMs;
    sig.clockMs = clockMs;   // the display slot: the interval to the previous boundary
    sig.camValid = st.eyeCamValid;   // the view's identity for the lever's effect
    std::memcpy(sig.cam, st.eyeCam, sizeof(sig.cam));
    // The lever's effect this frame, both eyes: the parts tested, and those
    // passed at EDVR's scale -- acting, the engine's own passes (its tests ran
    // at s_game x k); observing, those less the shadow's would-drop.
    for (uint32_t e = 0; e < 2; ++e) {
        const uint32_t* p = &d[cPartBase + e * kPartFields];
        sig.tested += p[pSeen];
        sig.passed += p[pActing] ? p[pPassed] : (p[pPassed] > p[pDrop] ? p[pPassed] - p[pDrop] : 0u);
        sig.dropped += p[pDrop];
    }
    // The cockpit gate: the journal watcher's Status.json, read on this same
    // thread each frame before the boundary (device_hook.cpp), exactly as the
    // on-foot frame pacing reads it (native_frame.cpp). Unknown -- menus, the
    // watcher off -- is not on foot.
    sig.onFoot = journalOnFootKnown() && journalOnFoot();
    if (!st.journalNoted && !journalWatchActive()) {
        st.journalNoted = true;
        Log::get().note("settlement detail: the journal watcher is not reading the game's Status.json "
                        "(d3d11.journal_watch off, or the journal folder not found), so on foot cannot be told from "
                        "the cockpit: the governor also runs on foot.");
    }
    readWork(st, nowMs, &sig);
    readGpu(nowMs, &sig);
    // The signal the runtime's timing version fixes, said once when it is
    // first known (unless the configure line already named it) and again only
    // if it changes: a log that never shows this line never had a new frame.
    if (sig.source != lodgov::WorkSource::None &&
        (sig.source != st.source || sig.timingVersion != st.timingVersion)) {
        st.source = sig.source;
        st.timingVersion = sig.timingVersion;
        char clause[400];
        workSourceClause(st.source, st.timingVersion, clause, sizeof(clause));
        Log::get().note("settlement detail: %s.", clause);
    }
    const float from = st.policy.k();
    const lodgov::Step step = st.policy.update(sig);
    const float k = st.policy.k();
    // The setter observer reads k at the engine's next rebuild: the boundary
    // never writes engine memory itself.
    g_kBits.store(toBits(k), std::memory_order_release);
    logWriteEvents(st);
    // One line per transition of the cockpit gate, never rate-limited.
    if (sig.onFoot != st.onFoot) {
        st.onFoot = sig.onFoot;
        if (sig.onFoot) {
            st.footStartMs = nowMs;
            st.footFrames = 0;
            Log::get().note("settlement detail (%s): on foot (the game's Status.json, the flag the on-foot frame "
                            "pacing reads): k %.2f -> %.2f, held at 1 while on foot -- the governor is for the "
                            "cockpit only; on foot is unmeasured.", modeTag(st), from, k);
        } else {
            Log::get().note("settlement detail (%s): no longer on foot (Status.json) after %.1f s, %u frames held at "
                            "k 1; the governor resumes (the k from before, in one step, if a frame has 200 builder "
                            "records within 5 s; else from 1, a second of at least 20 cycles before a step).",
                            modeTag(st), double(nowMs - st.footStartMs) / 1000.0, st.footFrames);
        }
    }
    if (sig.onFoot) ++st.footFrames;
    // The window.
    Window& w = st.w;
    ++w.frames;
    if (sig.onFoot) ++w.footFrames;
    if (sig.expired) ++w.expiries;
    if (!st.freshAtMs || nowMs - st.freshAtMs > lodgov::kFreshMs) ++w.staleFrames;
    // Density the lever itself may have cut: a settlement's frame at 150-199
    // records holds k (under 150 for 30 frames resets it).
    if (st.policy.inSettlement() && sig.records + lodgov::kSettlementBand >= lodgov::kSettlementRecords &&
        sig.records < lodgov::kSettlementRecords)
        ++w.bandFrames;
    // The lever inert at this view: up steps without a benefit -- two of
    // 0.25 or four of 0.05 in a row. Said once a hold, with the run's figures.
    if (st.policy.inertStarted()) {
        ++w.inertHolds;
        char effective[40], figures[256], steps[64];
        effectiveText(k, effective, sizeof(effective));
        effectText(st.policy.runFigures(), figures, sizeof(figures));
        stepsText(st.policy.runCoarse(), st.policy.runFine(), steps, sizeof(steps));
        Log::get().note("settlement detail: the LOD lever is inert at this view: no observed benefit: %s across %s; "
                        "holding k %.2f (s x k %s), no step and no kick; one step is retried every 30 s or when the "
                        "parts tested a frame move 20%%.", figures, steps, double(k), effective);
    }
    // A retried step that showed a benefit: the hold is over.
    if (st.policy.rearmed()) {
        ++w.rearms;
        char figures[256];
        effectText(st.policy.lastFigures(), figures, sizeof(figures));
        Log::get().note("settlement detail: the LOD lever responds again at this view: the retried step moved %s; "
                        "stepping resumes at k %.2f.", figures, double(k));
    }
    if (st.eyes != 0xFFFFu) ++w.eyeFrames;
    if (sig.records >= lodgov::kSettlementRecords) ++w.denseFrames;
    w.recordsSum += d[cRecords];
    w.partsSum += d[cParts];
    if (d[cRecords] > w.recordsMax) w.recordsMax = d[cRecords];
    if (d[cParts] > w.partsMax) w.partsMax = d[cParts];
    if (sig.work == lodgov::Work::Valid) {
        ++w.workSamples;
        if (sig.source == lodgov::WorkSource::Caller) ++w.callerSamples;
        else if (sig.source == lodgov::WorkSource::App) ++w.appSamples;
        w.workSum += sig.workMs;
        w.periodSum += sig.periodMs;
        if (sig.workMs > sig.periodMs + lodgov::kOverMarginMs) ++w.workOver;
        if (sig.workMs < sig.periodMs - lodgov::kUnderMarginMs) ++w.workUnder;
        // The display slots, as the policy classed this frame's cycle.
        const lodgov::Cycle& cycle = st.policy.cycle();
        if (cycle.measured) {
            ++w.cycles;
            if (cycle.missed) {
                ++w.slotMisses;
                if (cycle.gpuBound) ++w.slotGpuBound;
                else if (cycle.unexplained) ++w.slotUnexplained;
                else if (sig.records >= lodgov::kSettlementRecords) ++w.denseOurMisses;
            }
        }
    } else if (sig.work == lodgov::Work::Invalid) {
        ++w.workInvalid;
        if (sig.callerAbsent) ++w.callerAbsent;
    }
    for (uint32_t c = 0; c < kCounters; ++c) w.sum[c] += d[c];
    for (uint32_t cls = 0; cls < kClasses; ++cls)
        if (d[cPartBase + cls * kPartFields + pDrop] > w.dropMax[cls])
            w.dropMax[cls] = d[cPartBase + cls * kPartFields + pDrop];
    if (d[cRecNotDispatched] > w.notDispatchedMax) w.notDispatchedMax = d[cRecNotDispatched];
    if (k < w.kLow) w.kLow = k;
    if (k > w.kHigh) w.kHigh = k;
    switch (step) {
    case lodgov::Step::Up:
        ++w.up;
        if (st.policy.upQuanta() > 1) ++w.upCoarse;
        if (st.policy.retrying()) ++w.retries;
        break;
    case lodgov::Step::Down: ++w.down; break;
    case lodgov::Step::Reset: ++w.resets; break;
    case lodgov::Step::Clamp: ++w.clamps; break;
    case lodgov::Step::Enter: ++w.enters; break;
    case lodgov::Step::Kick: ++w.kicks; break;
    case lodgov::Step::Restore:
        ++w.restores;
        if (st.policy.restoredAfterKick()) ++w.restoresKick;
        break;
    case lodgov::Step::Aboard: ++w.aboard; break;
    default: break;   // Foot: the transition line above says it
    }
    if (step != lodgov::Step::None && step != lodgov::Step::Foot) logStep(st, step, from, sig, nowMs);
    // The lever spent: at k_max with the last window triggering. The summary
    // window counts the time; after five such decision windows in a row a
    // line gives the outcome, once a summary window. Nothing acts.
    if (st.policy.triggered() && st.policy.steps() == st.policy.maxSteps() && st.prevBoundaryMs &&
        nowMs >= st.prevBoundaryMs)
        w.ceilingMs += double(nowMs - st.prevBoundaryMs);
    st.prevBoundaryMs = nowMs;
    if (!w.ceilingLogged && st.policy.ceilingMissing()) {
        w.ceilingLogged = true;
        char effective[40];
        effectiveText(k, effective, sizeof(effective));
        // Both figures: the caller work spans Present, so when the GPU is the
        // wall it reads high too; the GPU's own render time says which.
        const double callerMs = st.policy.periodMs() + st.policy.meanExcessMs();
        const double gpuMs = st.policy.meanGpuMs();
        char gpu[64];
        if (gpuMs >= 0.0)
            std::snprintf(gpu, sizeof(gpu), "GPU %.2f ms%s", gpuMs, gpuMs > callerMs ? ": the GPU is the wall" : "");
        else
            std::snprintf(gpu, sizeof(gpu), "GPU unknown (no application-render sample)");
        // The outcome: a ceiling miss alone does not say the remaining work
        // is not LOD-elastic; the whole effect -- against the k = 1 baseline
        // of this view when there is one, else the last judged step -- does,
        // and both ends are printed so a reader sees what the ceiling removed.
        lodgov::EffectFigures fig;
        const lodgov::Outcome o = st.policy.ceilingOutcome(&fig);
        char outcome[400];
        if (!fig.baseline && fig.tested[0] < 0.0 && fig.caller[0] < 0.0) {
            std::snprintf(outcome, sizeof(outcome), "%s (no k 1 baseline of this view, no step measured)",
                          outcomeName(o));
        } else {
            char figures[256];
            effectText(fig, figures, sizeof(figures));
            std::snprintf(outcome, sizeof(outcome), "%s (%s: %s)", outcomeName(o),
                          fig.baseline ? "against k 1 at this view" : "the last judged step", figures);
        }
        const uint32_t missing = st.policy.misses() + st.policy.gpuBoundMisses() + st.policy.unexplainedMisses();
        Log::get().note("settlement detail: at the ceiling (k %.2f, s x k %s) and still missing %u of the last %u "
                        "display slots (%u the CPU's, %u GPU-bound, %u unexplained): outcome %s; caller work %.2f ms "
                        "mean, %s.",
                        double(k), effective, missing, st.policy.samples(), st.policy.misses(),
                        st.policy.gpuBoundMisses(), st.policy.unexplainedMisses(), outcome, callerMs, gpu);
    }
    if (nowMs - w.startMs >= 30000) {
        logSummary(st, nowMs);
        startWindow(st, nowMs);
    }
}

void configureLine(const State& st) {
    const char* m = modeName(st.mode);
    const bool setterOk = std::strcmp(st.setterStatus, "hooked") == 0;
    char mode[200];
    if (st.observe)
        std::snprintf(mode, sizeof(mode), "%s, observe only: never writes (advanced.settlement_detail_observe = 1)", m);
    else if (!setterOk)
        std::snprintf(mode, sizeof(mode), "%s, but it cannot act: the LOD-scale setter hook stood down; observing, "
                      "never writes", m);
    else
        std::snprintf(mode, sizeof(mode), "%s: acts by scaling the game's LOD scale right after the engine sets it "
                      "each frame (FUN_142819D90): the game's value x k", m);
    const char* stood = g_standDown.load(std::memory_order_acquire)
        ? "; acting STOOD DOWN earlier in this process, observing" : "";
    char policy[400];
    if (st.mode == Mode::Reduced)
        std::snprintf(policy, sizeof(policy), "k = %.2f (advanced.settlement_detail_max) at once from a frame with >= "
                      "200 draw-builder records, 1 after 30 frames under 150 or on foot, no ramp", st.policy.kMax());
    else
        std::snprintf(policy, sizeof(policy), "k in [1, %.2f], auto's policy on the next line", st.policy.kMax());
    char work[400];
    workSourceClause(st.source, st.timingVersion, work, sizeof(work));
    // The hook statuses are the fixed strings kinematic_eval_hook.cpp names;
    // the precision only bounds the line if one ever grows.
    char part[160];
    if (std::strcmp(st.partStatus, "hooked") == 0) std::snprintf(part, sizeof(part), "hooked");
    else std::snprintf(part, sizeof(part), "STOOD DOWN (%.128s)", st.partStatus);
    char setter[160];
    if (setterOk) std::snprintf(setter, sizeof(setter), "hooked");
    else std::snprintf(setter, sizeof(setter), "STOOD DOWN (%.128s)", st.setterStatus);
    Log::get().note(
        "settlement detail: on (%s%s) -- %s; %s. The shadow re-runs the engine's LOD tests (FUN_1442B3FC0 per part, "
        "FUN_144308B30 per record) at the game's scale and at scale x k. Hooks: builder FUN_1442B4420 %s, part test "
        "FUN_1442B3FC0 %s, LOD-scale setter FUN_142819D90 %s, plane test FUN_1404F4E10 %s. Summaries every 30 s.",
        mode, stood, policy, work, st.builderHooked ? "hooked" : "NOT hooked (no records, no record test)", part,
        setter, st.planesMatched ? "matched" : "MISMATCHED (acting drops unverified)");
    // auto's policy, whole, on two lines of its own: the configure line is
    // already most of the log's line in its worst case.
    if (st.mode == Mode::Auto) {
        Log::get().note(
            "settlement detail: auto's policy, decided once a second on the cycles completed in it with a fresh timing "
            "sample (timing older than 2 s expires; a second of fewer than 20 decides nothing). A cycle longer than "
            "1.5 x the period took two display slots: GPU-bound if the application's GPU render time was at least "
            "the period - 0.50 ms, else unexplained if the caller work was under the period - 0.30 ms, else the "
            "CPU's. Up while a frame has >= 200 draw-builder records and a tenth or more of the second's cycles were "
            "the CPU's misses: 0.25 if a quarter or more were or their mean caller work ran > 1.00 ms over, else "
            "0.05 (and 0.05 within 0.25 below a working point); to k_max %.2f at once after 10 such seconds in a "
            "row below it (a kick, at most one per 30 s). Down 0.05 after 5 clean seconds in a row (no cycle taking "
            "two slots, the mean > 1.00 ms under the period), then as many again before the next; 1 after 30 frames "
            "under 150 records, and on foot.",
            st.policy.kMax());
        Log::get().note(
            "settlement detail: auto's trials: a kick is judged on its fifth second at k_max -- still triggering, the "
            "pre-kick k comes back and no kick follows for 60 s; else back 0.25 per 5 clean seconds to the pre-kick "
            "k + 0.25. A step down is a trial: a trigger within 10 s restores the k before it and doubles the clean "
            "seconds the next one waits for (5, 10, 20, 40, 60; 5 again once a step down holds 60 s). Up steps "
            "without a benefit on a stable scene (the parts tested and passed at EDVR's scale falling < 1%%, the "
            "caller work < 0.2 ms; a changing scene is not judged), two of 0.25 or four of 0.05 in a row: the lever "
            "is inert here, no up and no kick, one step retried every 30 s or when the parts tested move 20%%. At "
            "k_max with 5 triggering seconds in a row a line gives the outcome on the same measure, against the k 1 "
            "baseline of the same view when there is one; without caller work (timing v3/v4) auto holds.");
    }
}

bool enableLocked(State& st, uint64_t nowMs) {
    resetContexts();
    kinematicEvalSetLodGovernorObservers(&lodGovernorBuilderObserver, &lodGovernorPartObserver,
                                         &lodGovernorSetterObserver);
    st.attach = kinematicEvalLodGovernorAttach();
    if (std::strcmp(st.attach, "installed") != 0) {
        kinematicEvalSetLodGovernorObservers(nullptr, nullptr, nullptr);
        return false;
    }
    st.builderHooked = kinematicEvalBuilderHooked();
    st.partStatus = kinematicEvalPartTestStatus();
    st.setterStatus = kinematicEvalLodSetterStatus();
    const LodGovernorFrustumFn planes = kinematicEvalFrustumFn();
    g_frustum.store(planes, std::memory_order_release);
    st.planesMatched = planes != nullptr;
    st.overflowLogged = false;
    // Counts start now: the slots' sums so far are the baseline.
    sumSlots(st.prev);
    st.policy.reset();
    st.lastSeq = 0;
    st.lastStepLogMs = 0;
    st.stepsUnlogged = 0;
    st.onFoot = false;   // so the first boundary on foot says so
    st.footStartMs = 0;
    st.footFrames = 0;
    st.journalNoted = false;
    st.prevBoundaryMs = 0;
    st.noCallerNoted = false;
    st.freshAtMs = 0;
    st.haveEpoch = false;
    g_kBits.store(toBits(1.0f), std::memory_order_release);
    identifyEyes(st);   // from the last context seen, if any; else unnamed until the first boundary
    startWindow(st, nowMs);
    g_live.store(true, std::memory_order_release);
    return true;
}

// Returns how many contexts had the game's LOD scale written back.
uint32_t disableLocked(State& st, uint64_t nowMs) {
    if (st.w.frames) logSummary(st, nowMs);   // the partial window, tagged as it ran, before the off line
    g_acting.store(false, std::memory_order_release);
    const uint32_t restored = restoreScaled();
    g_live.store(false, std::memory_order_release);
    kinematicEvalLodGovernorDetach();
    kinematicEvalSetLodGovernorObservers(nullptr, nullptr, nullptr);
    g_kBits.store(toBits(1.0f), std::memory_order_release);
    return restored;
}

// Acting: auto or reduced, observe 0, the setter hooked. Leaving it writes
// the game's value back at once (the engine's next rebuild would anyway);
// returns how many contexts that touched.
uint32_t updateActing(const State& st) noexcept {
    const bool act = st.mode != Mode::Game && !st.observe && std::strcmp(st.setterStatus, "hooked") == 0;
    g_acting.store(act, std::memory_order_release);
    return act ? 0u : restoreScaled();
}

// The configure sweep's decision, testable without a Config.
void applyConfig(const char* modeText, float kMax, bool observe, uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(g_mutex);
    State& st = g_state;
    const std::string text = modeText ? modeText : "";
    Mode mode = Mode::Game;
    bool unknown = false;
    // An empty value is the compiled default (game), as an empty number or
    // switch is theirs (Config::getFloat / getBool).
    if (_stricmp(text.c_str(), "auto") == 0) mode = Mode::Auto;
    else if (_stricmp(text.c_str(), "reduced") == 0) mode = Mode::Reduced;
    else if (!text.empty() && _stricmp(text.c_str(), "game") != 0) unknown = true;
    if (st.configured && mode == st.mode && text == st.modeText && kMax == st.kMaxCfg && observe == st.observe)
        return;   // the 1 Hz re-poll
    const bool first = !st.configured;
    st.configured = true;
    st.modeText = text;
    st.kMaxCfg = kMax;
    st.observe = observe;
    const bool live = g_live.load(std::memory_order_acquire);
    st.policy.configure(kMax);
    st.policy.setFixed(mode == Mode::Reduced);
    if (mode == Mode::Game) {
        st.mode = mode;
        const uint32_t restored = live ? disableLocked(st, nowMs) : 0;
        if (unknown)
            Log::get().note("settlement detail: fix.settlement_detail = \"%s\" is not game, auto or reduced; off -- "
                            "the game's own detail, nothing observed or changed (the game's LOD scale written back "
                            "to %u context(s)).", text.c_str(), restored);
        else if (live || first)
            Log::get().note("settlement detail: off (fix.settlement_detail = game: the game's own detail; nothing is "
                            "observed or changed; the game's LOD scale written back to %u context(s)).", restored);
        return;
    }
    st.mode = mode;
    if (!live && !enableLocked(st, nowMs)) {
        g_acting.store(false, std::memory_order_release);
        Log::get().note("settlement detail: the governor could not attach its engine hooks (%s); off, nothing "
                        "observed or changed (fix.settlement_detail = %s).", st.attach, modeName(mode));
        return;
    }
    updateActing(st);
    // The configure line names the frame-work signal when a runtime frame has
    // already said which (the runtime is fixed for the process); else it says
    // the first frame decides, and frameBoundaryAt logs that.
    if (st.source == lodgov::WorkSource::None) {
        const NativeTimingSnapshot t = nativeTimingSnapshot();
        if (t.active && t.haveCpu && sourceOf(t.cpu.version) != lodgov::WorkSource::None) {
            st.source = sourceOf(t.cpu.version);
            st.timingVersion = t.cpu.version;
        }
    }
    configureLine(st);
}

}  // namespace

// --- The observers ----------------------------------------------------------------------

// The draw-item builder, before its forward (worker threads).
void lodGovernorBuilderObserver(uintptr_t pose, uintptr_t ctx, uintptr_t mask, uintptr_t nibbles) noexcept {
    (void)pose;
    (void)mask;   // the collection's active mask: the builder's own view loop, not the record test
    if (!g_live.load(std::memory_order_relaxed)) return;
    Slot* s = mySlot();
    bump(s, cRecords);
    if (g_ctx.load(std::memory_order_relaxed) != ctx) {
        g_ctx.store(ctx, std::memory_order_release);
        registerContext(ctx);   // the one table the write consults
    }
    RecordOutcome o{};
    if (!shadowRecord(ctx, nibbles, g_eyes.load(std::memory_order_relaxed), currentK(), &o)) {
        bump(s, cRecordFaults);
        return;
    }
    if (g_scaleBits.load(std::memory_order_relaxed) != o.scaleBits)
        g_scaleBits.store(o.scaleBits, std::memory_order_relaxed);
    if (o.tableRange) {
        bump(s, cRecordTableRange);
        return;
    }
    for (uint32_t e = 0; e < 2; ++e) {
        const RecordEye& re = o.eye[e];
        if (!re.seen) continue;
        const uint32_t base = cRecBase + e * kRecFields;
        bump(s, base + rSeen);
        if (o.acting) bump(s, base + rActing);
        if (re.mismatch) bump(s, base + rMismatch);
        else if (re.wouldFail) bump(s, base + rWouldFail);
        else if (re.change) bump(s, base + rChange);
    }
    if (o.dispatchMismatch) bump(s, cRecDispatchMismatch);
    else if (o.notDispatched) bump(s, cRecNotDispatched);
}

// FUN_1442B3FC0, after its forward (worker threads).
void lodGovernorPartObserver(uintptr_t items, uintptr_t out, uintptr_t view, bool fromBuilder) noexcept {
    if (!g_live.load(std::memory_order_relaxed)) return;
    Slot* s = mySlot();
    bump(s, cParts);
    if (!fromBuilder) {   // not the builder's sub-item loop: not a part the builder will draw
        bump(s, cPartForeign);
        return;
    }
    lodgov::PartInputs in;
    uintptr_t ctx = 0;
    if (!readPart(items, out, view, &in, &ctx)) {
        bump(s, cPartFaults);
        return;
    }
    if (!in.table.inRange()) {
        bump(s, cPartTableRange);
        return;
    }
    in.sGame = gameScaleFor(ctx, in.s);
    const uint32_t base = cPartBase + classOf(in.bit) * kPartFields;
    bump(s, base + pSeen);
    if (in.enginePass) bump(s, base + pPassed);
    const lodgov::PartOutcome o = lodgov::shadowPart(in, currentK());
    if (o.acting) bump(s, base + pActing);
    if (o.mismatch) {
        bump(s, base + pMismatch);
    } else if (o.drop) {
        bump(s, base + pDrop);
        bump(s, base + pHist0 + o.bucket);
    } else if (o.change) {
        bump(s, base + pChange);
    } else if (o.checkPlanes) {
        // The one term the LOD arithmetic cannot answer for a reject: the
        // engine's own plane test on the part's sphere.
        const LodGovernorFrustumFn planes = g_frustum.load(std::memory_order_relaxed);
        uint32_t verdict = 0;
        if (!planes || !planeTest(planes, view, in.centre, in.sphere, &verdict)) {
            bump(s, base + pUnverified);
        } else if (verdict != 0xFFFFFFFFu) {
            bump(s, base + pDrop);
            bump(s, base + pHist0 + o.bucket);
        }
    }
}

// FUN_142819D90, after its forward (the engine's thread, twice a frame): the
// engine has just stored the game's LOD scale at ctx+0x30.
void lodGovernorSetterObserver(uintptr_t ctx) noexcept {
    if (!g_live.load(std::memory_order_acquire)) return;
    const uint32_t window = g_window.load(std::memory_order_relaxed);
    g_setterCalls.fetch_add(1, std::memory_order_relaxed);
    noteSetterContext(ctx, window);
    float v = 0;
    if (!readScale(ctx, &v)) {
        g_setterFaults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    CtxSlot* slot = findContext(ctx);
    if (!slot) return;   // not a context the draw-item builder has used: never written
    // The engine has just stored its own value over anything of EDVR's.
    slot->heldBits.store(0, std::memory_order_release);
    if (!(v >= lodgov::kScaleLow && v <= lodgov::kScaleHigh)) {   // NaN too
        g_setterImplausible.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t vBits = toBits(v);
    slot->gameBits.store(vBits, std::memory_order_release);
    if (!actingNow()) return;
    const float k = currentK();
    if (!(k > 1.0f)) return;   // k = 1: the game's value stands
    const float target = v * k;
    if (!contextWritable(*slot, ctx)) {
        standDown("ctx+0x30 is not committed, writable memory (VirtualQuery)", ctx);
        return;
    }
    // Held first, then the store: a reader between the two sees the game's
    // value with a held value that does not match it, and counts it as such.
    const uint32_t targetBits = toBits(target);
    slot->heldBits.store(targetBits, std::memory_order_release);
    if (!writeScale(ctx, target)) {
        slot->heldBits.store(0, std::memory_order_release);
        g_setterFaults.fetch_add(1, std::memory_order_relaxed);
        standDown("writing ctx+0x30 faulted", ctx);
        return;
    }
    slot->scaledWindow.store(window, std::memory_order_relaxed);
    g_setterScaled.fetch_add(1, std::memory_order_relaxed);
    uint32_t expected = 0;
    if (slot->firstState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        slot->firstGame.store(vBits, std::memory_order_relaxed);
        slot->firstHeld.store(targetBits, std::memory_order_relaxed);
        slot->firstK.store(toBits(k), std::memory_order_relaxed);
        slot->firstState.store(2, std::memory_order_release);
    }
}

// --- Configuration, the frame boundary and shutdown ----------------------------------------

void lodGovernorConfigure(Config& cfg) {
    // A shipped fix, off (game) unless the player chooses auto or reduced.
    const std::string mode = cfg.getString("fix.settlement_detail", "game");
    const float kMax = cfg.getFloat("advanced.settlement_detail_max", lodgov::kDefaultMax);
    const bool observe = cfg.getBool("advanced.settlement_detail_observe", false);
    applyConfig(mode.c_str(), kMax, observe, GetTickCount64());
}

void lodGovernorFrameBoundary() {
    if (!g_live.load(std::memory_order_acquire)) return;
    // One QueryPerformanceCounter read a frame: the interval between two
    // boundaries is the frame's cycle, one display slot or two.
    static const double qpcMsPerTick = [] {
        LARGE_INTEGER f;
        return QueryPerformanceFrequency(&f) && f.QuadPart > 0 ? 1000.0 / double(f.QuadPart) : 0.0;
    }();
    LARGE_INTEGER c{};
    const bool clock = qpcMsPerTick > 0.0 && QueryPerformanceCounter(&c);
    frameBoundaryAt(GetTickCount64(), clock ? double(c.QuadPart) * qpcMsPerTick : -1.0);
}

void lodGovernorShutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_live.load(std::memory_order_acquire)) {
        disableLocked(g_state, GetTickCount64());
    } else {
        g_acting.store(false, std::memory_order_release);
        restoreScaled();
    }
}

}  // namespace edvr
