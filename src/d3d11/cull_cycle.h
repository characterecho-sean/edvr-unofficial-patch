#pragma once

// The terrain-culling arc's automatic probe cycle (docs\terrain-culling.md). TEMPORARY: it goes with advanced.cull_probe
// when the arc closes.
//
// The manual probe flight could not be read by eye: the black squares flicker at the periphery as the head moves. A culler
// whose frustum widens admits more planet-terrain tiles at the outer edges, so a per-frame COUNT of terrain draws should rise
// in exactly the probe group that feeds the culler. `advanced.cull_probe = cycle` drives the groups itself on a fixed schedule
// and counts, per window, the planet-terrain draws each eye was given.
//
//   THE COUNT. A planet-terrain draw is a draw of the patch renderer's colour pass: VS kCelestialPatchVs (72BDD292154158AD,
//   celestial_motion.h), the cube-sphere surface patches the retired terrain hook and the planet patch motion both key on, whose
//   index counts vary with tile LOD. vscreen.cpp's draw path already holds that VS hash; counting is one relaxed add there, and
//   only while a cycle runs. The eye is the scene depth pair's (depthProbeCurrentSceneEyeOf, as celestial_motion.cpp reads it).
//
//   THE SCHEDULE (kSchedule, CullProbe codes): off, all, off, camera, off, ui, off, sky, off, sizes, off, other; 2.0 s windows by
//   QPC. The first 30 frames of every window are discarded: the runtime applies the group it was told and a culler on another
//   thread may lag several frames. A window logs its mean draws and indices per eye per frame and the mean head angular speed
//   over its counted frames, so windows with head motion can be excluded (tools\edvr_log.py --tally cull). When a cycle's last
//   window has been followed by the next cycle's first off window, each group's mean less the mean of its two neighbouring off
//   windows is logged.
//
//   MEASURE (`advanced.cull_probe = measure`): the same windows and the same counting with NO lies, and each window labelled by the
//   cull guard's stage instead of a probe group (off, waiting, adopting, live): the positive control. The old guard
//   (`fix.cull_guard = symmetric`) is proven to remove the squares, so a counter that cannot see its terrain draws rise when the guard
//   goes live is blind, and its silence under the lie probe would mean nothing. The guard cannot alternate every 2 s (it stages, the
//   game rebuilds its targets, then it goes live), so a window is dropped when the stage changes under it and measure keeps
//   counting while the guard runs: only the lying stands down. `edvr_log.py --tally cull` prints live minus off.
//
// Pure bookkeeping with the clock, the log sink and the gates passed in, so the rig (tools\native_frame_test) drives every case.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace edvr::cullcycle {

constexpr unsigned kWindows = 12;
constexpr unsigned kDiscardFrames = 30;
constexpr uint64_t kWindowUs = 2000000;
// advanced.cull_probe codes (EdvrNativeFrameOutput::cullProbe): 0 off, 1 all, 2 camera, 3 ui, 4 sky, 5 sizes, 6 other.
constexpr uint32_t kSchedule[kWindows] = {0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6};
// Build 332841, the pair every build-keyed hook checks (and the runtime's probe is gated on).
constexpr uint32_t kBuildStamp = 1788384820u, kBuildImageSize = 104894464u;

inline const char* groupName(uint32_t group) {
    static const char* const kNames[] = {"off", "all", "camera", "ui", "sky", "sizes", "other"};
    return group < 7 ? kNames[group] : "?";
}
// The cull guard's stage as this half can tell it: 0 off (not configured), 1 waiting (configured, nothing asked of the game yet, or
// inert -- the channel carries stage 0 for both), 2 adopting (the game is asked for bigger targets, still told the truth), 3 live.
inline const char* stageName(uint32_t stage) {
    static const char* const kNames[] = {"off", "waiting", "adopting", "live"};
    return stage < 4 ? kNames[stage] : "?";
}

// ---- the planet-terrain draw counters: the render thread adds, the frame boundary takes ---------------------------------------
inline std::atomic<bool> g_counting{false};
inline std::atomic<uint32_t> g_draws[2]{};
inline std::atomic<uint64_t> g_indices[2]{};
inline bool counting() noexcept { return g_counting.load(std::memory_order_relaxed); }
inline void noteTerrainDraw(int eye, uint32_t count, uint32_t instances) noexcept {
    if (eye != 0 && eye != 1) return;
    g_draws[eye].fetch_add(1, std::memory_order_relaxed);
    g_indices[eye].fetch_add(static_cast<uint64_t>(count) * (instances ? instances : 1u), std::memory_order_relaxed);
}
// Rig only: a nonzero value is "now" in microseconds, and 0 or 1 says whether the executable is build 332841 (-1 reads it).
inline std::atomic<uint64_t> g_clockOverrideUs{0};
inline std::atomic<int> g_buildOverride{-1};

// The angle, degrees, between two rigid poses (row-major 3x4): the rotation R = A^T B turns by atan2(|v|/2, (trace - 1)/2), v
// its antisymmetric part. Not acos of the trace: a frame's turn is a few hundredths of a degree and a float matrix's cosine
// cannot resolve that, while its sine can.
inline double rotationDegrees(const float* a, const float* b) {
    double r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) sum += static_cast<double>(a[k * 4 + i]) * static_cast<double>(b[k * 4 + j]);
            r[i][j] = sum;
        }
    const double vx = r[2][1] - r[1][2], vy = r[0][2] - r[2][0], vz = r[1][0] - r[0][1];
    const double s = 0.5 * std::sqrt(vx * vx + vy * vy + vz * vz);
    const double c = (r[0][0] + r[1][1] + r[2][2] - 1.0) * 0.5;
    return std::atan2(s, c) * (180.0 / 3.14159265358979323846);
}

// A finished window: counted frames and per-frame means. A measure window carries the cull guard's stage, not a group.
struct Window {
    uint64_t number = 0;
    uint32_t group = 0, frames = 0, stage = 0;
    bool measure = false;
    double draws[2] = {}, indices[2] = {}, head = 0.0;
};

class Cycle {
public:
    explicit Cycle(bool measure = false) : measure_(measure) {}
    // One frame boundary. `draws` and `indices` are what the frame that just ended counted, per eye; `head` is the head pose at
    // this boundary (row-major 3x4) or null; `stage` is the cull guard's (stageName), which only a measure run reads. Returns the
    // CullProbe code to tell the runtime for the frame now beginning: the schedule's, or always 0 when measuring.
    template <class Sink>
    uint32_t beginFrame(uint64_t nowUs, const float* head, const uint32_t draws[2], const uint64_t indices[2], Sink&& sink,
                        uint32_t stage = 0) {
        if (!started_) {
            started_ = true;
            slot_ = 0;
            stage_ = stage;
            startUs_ = nowUs;
            clearWindow();
            setPrevious(nowUs, head);
            return measure_ ? 0u : kSchedule[0];
        }
        if (measure_ && stage != stage_) {
            // The frame that just ended straddled the guard changing stage: it, and the window it was in, belong to no stage.
            char line[160];
            std::snprintf(line, sizeof(line), "cull cycle: the cull guard went from %s to %s (the window in progress is dropped)", stageName(stage_),
                          stageName(stage));
            sink(line);
            stage_ = stage;
            startUs_ = nowUs;
            clearWindow();
            setPrevious(nowUs, head);
            return 0u;
        }
        double speed = -1.0;
        if (head && prevHeadValid_ && nowUs > prevUs_ && nowUs - prevUs_ <= 1000000ull)
            speed = rotationDegrees(prevHead_, head) / (static_cast<double>(nowUs - prevUs_) * 1e-6);
        ++seen_;
        if (seen_ > kDiscardFrames) {
            ++counted_;
            for (int e = 0; e < 2; ++e) {
                sumDraws_[e] += draws[e];
                sumIndices_[e] += static_cast<double>(indices[e]);
            }
            if (speed >= 0.0) { sumHead_ += speed; ++headSamples_; }
        }
        setPrevious(nowUs, head);
        if (nowUs - startUs_ >= kWindowUs) closeWindow(nowUs, sink);
        return measure_ ? 0u : kSchedule[slot_];
    }
    bool measuring() const { return measure_; }
    uint64_t windowsClosed() const { return closed_; }
    uint32_t stage() const { return stage_; }
    uint32_t cyclesDone() const { return cycles_; }
    unsigned slot() const { return slot_; }

private:
    void clearWindow() {
        seen_ = counted_ = headSamples_ = 0;
        for (int e = 0; e < 2; ++e) sumDraws_[e] = sumIndices_[e] = 0.0;
        sumHead_ = 0.0;
    }
    void setPrevious(uint64_t nowUs, const float* head) {
        prevUs_ = nowUs;
        prevHeadValid_ = head != nullptr;
        if (head) for (int i = 0; i < 12; ++i) prevHead_[i] = head[i];
    }
    template <class Sink>
    void closeWindow(uint64_t nowUs, Sink&& sink) {
        Window w;
        w.number = closed_ + 1;
        w.measure = measure_;
        w.group = measure_ ? 0u : kSchedule[slot_];
        w.stage = measure_ ? stage_ : 0u;
        w.frames = counted_;
        if (counted_) {
            for (int e = 0; e < 2; ++e) {
                w.draws[e] = sumDraws_[e] / counted_;
                w.indices[e] = sumIndices_[e] / counted_;
            }
        }
        w.head = headSamples_ ? sumHead_ / headSamples_ : 0.0;
        char line[256], label[48];
        if (measure_) std::snprintf(label, sizeof(label), "measure[guard %s]", stageName(w.stage));
        else std::snprintf(label, sizeof(label), "%s", groupName(w.group));
        std::snprintf(line, sizeof(line),
                      "cull cycle: %s window %llu: frames %u, terrain draws L/R mean %.2f/%.2f, indices L/R mean %.1f/%.1f, head %.1f deg/s",
                      label, static_cast<unsigned long long>(w.number), w.frames, w.draws[0], w.draws[1],
                      w.indices[0], w.indices[1], w.head);
        sink(line);
        if (measure_) {
            ++closed_;
            startUs_ = nowUs;
            clearWindow();
            return;
        }
        current_[slot_] = w;
        // A cycle's last group ('other') is paired with the NEXT cycle's first off window, so its pairs are written when
        // that window closes.
        if (slot_ == 0 && haveDone_) { writePairs(w, sink); haveDone_ = false; }
        if (slot_ == kWindows - 1) {
            for (unsigned i = 0; i < kWindows; ++i) done_[i] = current_[i];
            haveDone_ = true;
            ++cycles_;
        }
        ++closed_;
        slot_ = (slot_ + 1) % kWindows;
        startUs_ = nowUs;
        clearWindow();
    }
    template <class Sink>
    void writePairs(const Window& nextOff, Sink&& sink) {
        char line[256];
        bool anyDraws = false;
        for (unsigned i = 0; i < kWindows; ++i) anyDraws = anyDraws || done_[i].draws[0] > 0.0 || done_[i].draws[1] > 0.0;
        if (!anyDraws) {
            std::snprintf(line, sizeof(line),
                          "cull cycle: cycle %u counted no planet-terrain draws in any window (not over terrain, or no scene depth pair yet)",
                          cycles_);
            sink(line);
        }
        for (unsigned p = 1; p < kWindows; p += 2) {
            const Window& g = done_[p];
            const Window& before = done_[p - 1];
            const Window& after = p + 1 < kWindows ? done_[p + 1] : nextOff;
            if (!g.frames || !before.frames || !after.frames) {
                std::snprintf(line, sizeof(line), "cull cycle: cycle %u, %s vs off: n/a (a window had no counted frames)", cycles_,
                              groupName(g.group));
                sink(line);
                continue;
            }
            double d[2], i2[2];
            for (int e = 0; e < 2; ++e) {
                d[e] = g.draws[e] - 0.5 * (before.draws[e] + after.draws[e]);
                i2[e] = g.indices[e] - 0.5 * (before.indices[e] + after.indices[e]);
            }
            std::snprintf(line, sizeof(line),
                          "cull cycle: cycle %u, %s vs off: draws L/R %+.2f/%+.2f, indices L/R %+.1f/%+.1f (windows %llu, %llu, %llu)",
                          cycles_, groupName(g.group), d[0], d[1], i2[0], i2[1], static_cast<unsigned long long>(before.number),
                          static_cast<unsigned long long>(g.number), static_cast<unsigned long long>(after.number));
            sink(line);
        }
    }

    bool measure_ = false, started_ = false, prevHeadValid_ = false, haveDone_ = false;
    unsigned slot_ = 0, seen_ = 0, counted_ = 0, headSamples_ = 0;
    uint32_t stage_ = 0;
    uint32_t cycles_ = 0;
    uint64_t closed_ = 0, startUs_ = 0, prevUs_ = 0;
    double sumDraws_[2] = {}, sumIndices_[2] = {}, sumHead_ = 0.0;
    float prevHead_[12] = {};
    Window current_[kWindows], done_[kWindows];
};

enum class Status : uint32_t { Idle, Running, IgnoredGuard, StoodDownBuild, Measuring };

// The status of a request: the same gates the runtime's probe has, in the same order (a guard wins over a wrong build). The
// runtime has no way to report back, and both gates are things this half reads itself (the config, the executable's PE stamp).
inline Status statusFor(bool requested, bool guardConfigured, bool build332841) {
    if (!requested) return Status::Idle;
    if (guardConfigured) return Status::IgnoredGuard;
    return build332841 ? Status::Running : Status::StoodDownBuild;
}

class Driver {
public:
    // One call per frame boundary (native_frame.cpp's beginFrame). Returns the CullProbe code for this frame: the scheduled
    // group while a cycle runs, 0 otherwise (a measure run never lies). `guardStage` is the cull guard's (stageName). Every change
    // of status is logged once; a request that never ran logs nothing. Measure ignores both gates: it tells no lie, so a cull guard
    // that is running or another build is no reason to stand down.
    template <class Sink>
    uint32_t frame(bool cycleRequested, bool measureRequested, bool guardConfigured, bool build332841, uint32_t guardStage,
                   uint64_t nowUs, const float* head, Sink&& sink) {
        const Status want = measureRequested ? Status::Measuring : statusFor(cycleRequested, guardConfigured, build332841);
        if (want != status_) {
            char line[256];
            if (status_ == Status::Running || status_ == Status::Measuring) {
                std::snprintf(line, sizeof(line), "cull cycle: stopped after %llu windows", static_cast<unsigned long long>(cycle_.windowsClosed()));
                sink(line);
            }
            cycle_ = Cycle(want == Status::Measuring);
            status_ = want;
            g_counting.store(want == Status::Running || want == Status::Measuring, std::memory_order_relaxed);
            for (int e = 0; e < 2; ++e) {
                g_draws[e].store(0, std::memory_order_relaxed);
                g_indices[e].store(0, std::memory_order_relaxed);
            }
            switch (want) {
                case Status::Running:
                    sink("cull cycle: running -- 2.0 s windows, off, all, off, camera, off, ui, off, sky, off, sizes, off, other; the first 30 frames of each "
                         "discarded; planet-terrain draws counted per eye");
                    break;
                case Status::Measuring:
                    sink("cull cycle: measuring -- 2.0 s windows labelled by the cull guard's stage (off, waiting, adopting, live), the first 30 frames of each "
                         "discarded, a window dropped when the stage changes under it; no probe lies are told; planet-terrain draws counted per eye");
                    break;
                case Status::IgnoredGuard: sink("cull cycle: ignored while the cull guard runs"); break;
                case Status::StoodDownBuild: sink("cull cycle: standing down -- not build 332841"); break;
                default: break;
            }
        }
        if (status_ != Status::Running && status_ != Status::Measuring) return 0;
        uint32_t draws[2];
        uint64_t indices[2];
        for (int e = 0; e < 2; ++e) {
            draws[e] = g_draws[e].exchange(0, std::memory_order_relaxed);
            indices[e] = g_indices[e].exchange(0, std::memory_order_relaxed);
        }
        return cycle_.beginFrame(nowUs, head, draws, indices, sink, guardStage);
    }
    Status status() const { return status_; }
    const Cycle& cycle() const { return cycle_; }

private:
    Status status_ = Status::Idle;
    Cycle cycle_;
};

inline Driver g_driver;

}  // namespace edvr::cullcycle
