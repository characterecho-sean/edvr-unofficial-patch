#pragma once
// The draw hooks' own time, split by step (built 2026-10-27). A TEMPORARY instrument for the AA-on draw-hook increment
// (docs\engine-render-performance-2026-09-19.md, the 2026-10-27 entry).
//
// WHY. The pad leg of 2026-10-27 measured the "draw hook CPU:" line at 2.44 ms a frame with temporal AA off and 4.26 ms with it on,
// at about 22k eye draws a frame, and about 1.4 ms of the 1.8 ms increment is in no part the earlier legs priced. That line clocks
// a draw whole (a clock read before the thunk's work and one after, less the game's own draw), so it cannot say WHICH step of the
// thunk holds the time. This one clocks the steps: one flight says where it is.
//
// TWO CLOCK MODES, ON DIFFERENT DRAWS, so the line above keeps its meaning exactly. On a sampled frame (one in sixteen,
// perf_monitor.h) the draw thunks clock every 64th draw whole, as they have since 2026-09-22 (ordinal % 64 == 0: the same clock
// reads at the same places, the same perfMonitorDrawTicks report). This header adds a STEPS mode for the draws between those, the
// ordinal % 64 == 32 draws of the same frames: the clock keeps the time of its last mark and adds each interval to the step the
// mark closes. Every other draw is off, and pays what it paid before plus one branch per mark.
//
// A MARK'S LABEL IS THE BLOCK THAT ENDS AT IT. mark(step) reads the clock once, adds now - last to that step's ticks, counts one
// mark for the step, and sets last = now. So a function that returns early between two marks needs no care: what ran until the
// next mark goes to that next mark's step. A step with no mark in a draw's run is simply not in it (the report prints "-" for a
// step that never closed in the window, never 0.000: the code never ran is not a cheap step).
//
// The thunk-level steps, in the order a draw meets them (vscreen.cpp):
//   pre            the clock's start to just before beginPanelOverride (the thunk's own preamble)
//   v:*            beginPanelOverride (the draw's verdict), cut by sub-marks inside it, each named after what its block serves:
//                    v:hud/fss     the on-foot HUD issuer probe, the roll-free pending clear, the FSS chrome tracker
//                    v:context     the glare clamp reset, the foreign-context return, the optimized station's sun skip, the per-draw flags
//                    v:particles   the draw gate, the binding audit, the particle billboards, the witchspace stars, the space dust
//                    v:eye-test    the render target's eye test (rebound only), the quad probe, the depth probe's note
//                    v:offscreen   the whole off-screen branch (a draw that is not into an eye texture), up to its return
//                    v:ui-depth    the eye branch's first half: the depth probe's eye note, the interface depth (ui_depth), the
//                                  hologram depth, the intro movie's panel and the splash's composite, the glare camera nomination
//                    v:census      the draw census, the crisp HUD note, the object probe, the skip probes
//                    v:fixes       the eye recognisers: night vision, RemLok, holo, target indicator, scrim, backdrop, FSS panel, the
//                                  glare train, the curved screen
//                    v:rest        everything else in the call (the panel distance fix's tail, a verdict's early return)
//   engine-vel     the owner's pre-draw work: the body-cull begin, engineVelocityBeforeDraw, engineVelocityNoteSkinDraw
//   probes         the pixel probe's before-call, the VR world route's detector, the glare clamp
//   f:*            forwardWithVerdict up to the game's draw: f:scopes (the entry scopes: the blend probe, the effect capture, the
//                  interface depth, the roll-free block, the skip return), f:ui-layer (fix.ui_quality's family decision and the
//                  composite census: it closes twice on a composite draw, so its calls run a little over one a draw), f:after-ui
//                  (the after-UI branch, which runs per draw while the layer is watching), f:verdict-begin (the loader panel, the quad
//                  skip and the curve checks, the verdict's Begin, the orbit lines, the supercruise bars), f:before (the layer's
//                  bracket, the altered-draw class, the marker fix, the surface strip: the rest up to the draw lambda's entry)
//   GAME           the FIRST real draw call the lambda makes: the game's own, excluded from EDVR's sum
//   reissue        every later real call the lambda makes (the splash dim's second issue): EDVR's
//   weapon         the X thunk's weaponMotionDraw (the old whole clock counts it with the game's draw)
//   p:*            the X thunk's work after the draw: p:rigid (the rigid-draw query: temporalPassWantsRigidDraw,
//                  depthProbeCurrentSceneEyeOf, VSGetConstantBuffers, temporalPassNoteRigidDraw), p:screen (screenMotionRecognize,
//                  screenMotionUiDraw, screenMotionDraw), p:rest (the census, the footprint, the predicates)
//   f:layer-end    forwardWithVerdict after the draw: the layer's end and second issues, the marker fix's end, the crisp HUD and world
//                  screen reissues, the issue-blocked return
//   f:ui-depth     the coverage reissues: the interface, hologram, planet and world screen depth passes
//   f:after        the rest of forwardWithVerdict (the verdict's End, the splash dim) to its return, and the scopes' destructors
//   tail           to the clock's end: endPanelOverride, pixelProbeAfterEye, the intro curve's end, the body-cull end
// On the D, I and N thunks' AA-off fast path (draw_fast_path.h) a draw goes pre, v:*, probes, GAME, tail.
//
// THE CLOCK READ IS PART OF EVERY INTERVAL. Each mark adds one clock read's cost to the step it closes, so the report is NET of it:
// at each window close the floor is calibrated (the fastest of 4 batches of 512 marks into a scratch run, through the same markRun
// the draws use), and each step's ticks lose marks x floor. The floor is the warm, back-to-back figure; a mark in a cold thunk
// costs more, so the net figures can still carry a little of it. The calls and the marks per clocked draw are in the line, so that is
// checkable against the draw count.
//
// SUB-MARKS reach the clock through a thread-local pointer set only while a steps-mode clock is alive (the caller saves and
// restores the previous value), behind a process-wide count of live steps-mode clocks (Env::stepsLive): every draw but the one in
// 64 that is clocked by step pays one plain load per sub-mark and no thread-local read. That includes the whole-clocked draws of
// a sampled frame, so the sub-marks add next to nothing to the "draw hook CPU" line's own figure.
//
// Header only and pure (no Windows, no Direct3D), so tools\aa_off_test drives every function here with a fake clock.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace drawstep {

// ---- the steps --------------------------------------------------------------------
enum Step : uint8_t {
    kPre = 0,        // pre: the X thunk's clock starts at the top of the thunk (Phase 2b), so for X this is the prologue, the fast-path decision (arm ON) and the args stashes; D, I and N start theirs after the prologue
    kVHudFss,        // v:hud/fss
    kVContext,       // v:context
    kVParticles,     // v:particles
    kVEyeTest,       // v:eye-test
    kVOffscreen,     // v:offscreen
    kVUiDepth,       // v:ui-depth
    kVCensus,        // v:census
    kVFixes,         // v:fixes
    kVRest,          // v:rest
    kEngineVelocity, // engine-vel
    kProbes,         // probes
    kFwScopes,       // f:scopes
    kFwUiLayer,      // f:ui-layer
    kFwAfterUi,      // f:after-ui
    kFwVerdict,      // f:verdict-begin
    kFwBefore,       // f:before
    kFast,           // fast: the repeat-draw fast path's work before the game's own draw (Phase 2, 2026-10-27), in place of v:*, engine-vel, probes, f:*
    kGameDraw,       // GAME: the game's own first real call, not EDVR's
    kReissue,        // reissue
    kWeapon,         // weapon
    kPostRigid,      // p:rigid
    kPostScreen,     // p:screen
    kPostRest,       // p:rest
    kFwLayerEnd,     // f:layer-end
    kFwUiDepth,      // f:ui-depth
    kFwAfter,        // f:after
    kTail,           // tail
    kStepCount
};

// The names the log line prints. Short because 28 of them share a line the log truncates near 1170 characters.
inline const char* stepName(unsigned step) {
    static const char* const kNames[kStepCount] = {
        "pre", "v:hud/fss", "v:context", "v:particles", "v:eye-test", "v:offscreen", "v:ui-depth", "v:census", "v:fixes", "v:rest",
        "engine-vel", "probes", "f:scopes", "f:ui-layer", "f:after-ui", "f:verdict-begin", "f:before", "fast", "GAME", "reissue", "weapon",
        "p:rigid", "p:screen", "p:rest", "f:layer-end", "f:ui-depth", "f:after", "tail"};
    return step < kStepCount ? kNames[step] : "?";
}

// ---- the draw kinds ---------------------------------------------------------------
constexpr unsigned kKinds = 4;   // D, I, N, X: the four VR draw thunks
inline unsigned kindIndex(char kind) {
    switch (kind) {
    case 'D': return 0;
    case 'I': return 1;
    case 'N': return 2;
    default:  return 3;   // 'X'
    }
}
inline char kindLetter(unsigned index) { return index < kKinds ? "DINX"[index] : '?'; }

// ---- which draws are clocked how -------------------------------------------------------
enum class Mode : uint8_t {
    kOff = 0,
    kWhole,   // the existing clock: ordinal % stride == 0
    kSteps    // this instrument: ordinal % stride == stride / 2
};

// `ordinal` is the thread's count of draws on sampled frames (it advances on no other), already incremented for this draw.
inline Mode modeFor(uint32_t ordinal, uint32_t stride) {
    if (stride < 2) return Mode::kOff;
    const uint32_t r = ordinal % stride;
    return r == 0 ? Mode::kWhole : (r == stride / 2 ? Mode::kSteps : Mode::kOff);
}

// ---- one clocked draw's accumulation -------------------------------------------------------
// The members are value-initialised so a Run is constant-initialised as a thread_local (no guard on each thread-local read).
struct StepAccum {
    int64_t  ticks[kStepCount] = {};
    uint32_t marks[kStepCount] = {};
    void clear() {
        std::memset(ticks, 0, sizeof(ticks));
        std::memset(marks, 0, sizeof(marks));
    }
};

// EDVR's part of one draw: every step but the game's own draw, and the marks that closed them.
inline int64_t edvrTicks(const StepAccum& a) {
    int64_t sum = 0;
    for (unsigned i = 0; i < kStepCount; ++i)
        if (i != kGameDraw) sum += a.ticks[i];
    return sum;
}
inline uint64_t edvrMarks(const StepAccum& a) {
    uint64_t sum = 0;
    for (unsigned i = 0; i < kStepCount; ++i)
        if (i != kGameDraw) sum += a.marks[i];
    return sum;
}

// A steps-mode clock's run. `last` is the time of the last mark (the clock's start before any); `outer` is the run this one replaced as
// the thread's sub-mark target, put back when the clock ends.
struct Run {
    StepAccum acc;
    int64_t   last = 0;
    Run*      outer = nullptr;
};

// The mark: one clock read, the interval since the last mark added to the step it closes. Returns the time it read. `now` is any
// callable returning the clock's ticks (the production binding is the real clock; the rig's is a scripted one).
template <class Now>
inline int64_t markRun(Run& run, Step step, Now now) {
    const int64_t t = now();
    run.acc.ticks[step] += t - run.last;
    ++run.acc.marks[step];
    run.last = t;
    return t;
}

// ---- the draw thunks' clock ------------------------------------------------------------------
// Env supplies, as statics:
//   kStride                    the whole clock's stride (perf_monitor.h kPerfMonitorDrawTimeStride)
//   bool sampled()             is this a sampled frame
//   uint32_t nextOrdinal()     the drawing thread's draw ordinal, incremented and returned (advances on sampled frames only)
//   int64_t now()              the clock
//   Run& run()                 the drawing thread's storage for a steps-mode run
//   Run*& sub()                the drawing thread's sub-mark target (null unless a steps-mode clock is alive)
//   void stepsLive(int delta)  +1 as a steps-mode clock starts, -1 as it ends: the process-wide count the sub-marks test before
//                              they read the thread-local, so a whole-mode draw on a sampled frame pays a plain load per sub-mark
//   void reportWhole(int64_t whole, int64_t real)    a whole-mode draw's totals (perfMonitorDrawTicks)
//   void reportSteps(char kind, const StepAccum&)    a steps-mode draw's run
template <class Env>
class DrawClockT {
    struct EnvNow {
        int64_t operator()() const { return Env::now(); }
    };

public:
    // Whole mode: the thunks' own r0 reads test this, as they did before the steps mode existed.
    bool on = false;

    explicit DrawClockT(char kind) : kind_(kind) {
        // An unsampled frame's draw: one load, a branch, and the constructor's stores. The ordinal advances on sampled frames only.
        if (!Env::sampled()) return;
        mode_ = modeFor(Env::nextOrdinal(), Env::kStride);
        if (mode_ == Mode::kOff) return;
        t0_ = Env::now();
        if (mode_ == Mode::kWhole) {
            on = true;
            return;
        }
        run_ = &Env::run();
        run_->acc.clear();
        run_->last = t0_;
        run_->outer = Env::sub();
        Env::sub() = run_;
        Env::stepsLive(1);
    }
    DrawClockT(const DrawClockT&) = delete;
    DrawClockT& operator=(const DrawClockT&) = delete;

    Mode mode() const { return mode_; }

    // Whole mode. Only the first call forwards the game's draw; coverage reissues are EDVR work, and subtracting every call hid their
    // CPU and driver cost.
    void realCall(int64_t start) {
        if (!forwarded_) real_ += Env::now() - start;
        forwarded_ = true;
    }

    // Steps mode: close the step that ends here. A branch and nothing else in every other mode.
    void mark(Step step) {
        if (mode_ == Mode::kSteps) markRun(*run_, step, EnvNow{});
    }
    // The game's draw call has just returned. The first real call is the game's; any later one is a reissue and EDVR's.
    void stepDraw() {
        if (mode_ != Mode::kSteps) return;
        markRun(*run_, forwarded_ ? kReissue : kGameDraw, EnvNow{});
        forwarded_ = true;
    }
    // Entering the draw lambda, and forwardWithVerdict's return: what ran since the last mark is the forward scope's, before the first
    // real draw or after it. (A skip that never calls the lambda leaves the whole stretch to "before".)
    void markForward() {
        if (mode_ == Mode::kSteps) markRun(*run_, forwarded_ ? kFwAfter : kFwBefore, EnvNow{});
    }

    ~DrawClockT() {
        if (on) {
            Env::reportWhole(Env::now() - t0_, real_);
        } else if (mode_ == Mode::kSteps) {
            markRun(*run_, kTail, EnvNow{});
            Env::reportSteps(kind_, run_->acc);
            Env::sub() = run_->outer;
            Env::stepsLive(-1);
        }
    }

private:
    Mode    mode_ = Mode::kOff;
    bool    forwarded_ = false;
    char    kind_;
    int64_t t0_ = 0;
    int64_t real_ = 0;
    Run*    run_ = nullptr;
};

// ---- the window ---------------------------------------------------------------------------
// What the steps-mode draws of one 1800-frame window add up to. Plain accumulation from the drawing thread, read and reset at the frame
// boundary: perfMonitorDrawTicks' own arrangement.
struct Window {
    int64_t  ticks[kStepCount];
    uint64_t marks[kStepCount];
    uint64_t clocked;                  // steps-mode draws folded in
    uint64_t kindDraws[kKinds];
    int64_t  kindTicks[kKinds];        // EDVR's ticks by kind: every step but the game's draw
    uint64_t kindMarks[kKinds];        // ...and the marks that closed them (their floor comes off)

    void reset() { std::memset(this, 0, sizeof(*this)); }
    void add(char kind, const StepAccum& a) {
        const unsigned k = kindIndex(kind);
        for (unsigned i = 0; i < kStepCount; ++i) {
            ticks[i] += a.ticks[i];
            marks[i] += a.marks[i];
        }
        ++clocked;
        ++kindDraws[k];
        kindTicks[k] += edvrTicks(a);
        kindMarks[k] += edvrMarks(a);
    }
};

// The cost of one mark, in ns: the fastest of `batches` batches of `marksPerBatch` marks into a scratch run, through markRun itself
// so the clock call, the add and the count are all in the figure. A batch a preemption or a slow clock inflated is not the floor.
template <class Now>
inline double calibrateMarkNs(Now now, int64_t ticksPerSecond, unsigned marksPerBatch = 512, unsigned batches = 4) {
    if (ticksPerSecond <= 0 || marksPerBatch == 0 || batches == 0) return 0.0;
    Run scratch;
    scratch.acc.clear();
    scratch.last = now();
    for (unsigned i = 0; i < 64; ++i) markRun(scratch, kPre, now);   // warm: the clock's page, the code
    int64_t best = 0;
    for (unsigned b = 0; b < batches; ++b) {
        const int64_t t0 = now();
        scratch.last = t0;
        for (unsigned i = 0; i < marksPerBatch; ++i) markRun(scratch, kPre, now);
        const int64_t span = scratch.last - t0;
        if (b == 0 || span < best) best = span;
    }
    if (best < 0) best = 0;
    return static_cast<double>(best) * 1e9 / static_cast<double>(ticksPerSecond) / static_cast<double>(marksPerBatch);
}

// ---- the report -----------------------------------------------------------------------------
struct FormatInfo {
    uint32_t endFrame = 0;         // the frame the window closes on
    uint32_t sampledFrames = 0;    // sampled frames in the window
    uint32_t stride = 64;          // the whole clock's stride, the scale from clocked draws to a frame
    int64_t  ticksPerSecond = 0;
    double   floorNs = 0.0;        // one mark's cost
    bool     aaOn = false;         // temporal AA on in the VR profile
};

// The window's figures, per sampled frame, scaled from the clocked draws by the stride and net of the clock floor. A step with no marks
// is not seen (printed "-"). A net that comes out below zero is a step shorter than its own clock read: 0.000, not negative.
struct Totals {
    bool   seen[kStepCount];
    double netMs[kStepCount];
    double callsPerFrame[kStepCount];
    double edvrMs;                 // every step but the game's draw
    double gameMs;                 // the game's own draw, as clocked
    double marksPerDraw;
    bool   kindSeen[kKinds];
    double kindDrawsPerFrame[kKinds];
    double kindMs[kKinds];
};

inline double netTicks(int64_t ticks, uint64_t marks, double floorTicks) {
    const double net = static_cast<double>(ticks) - static_cast<double>(marks) * floorTicks;
    return net > 0.0 ? net : 0.0;
}

inline bool summarise(const Window& w, const FormatInfo& in, Totals* out) {
    std::memset(out, 0, sizeof(*out));
    if (in.sampledFrames == 0 || in.ticksPerSecond <= 0 || in.stride == 0) return false;
    const double freq = static_cast<double>(in.ticksPerSecond);
    const double floorTicks = in.floorNs * freq / 1e9;
    const double toMs = static_cast<double>(in.stride) * 1000.0 / freq / static_cast<double>(in.sampledFrames);
    const double toPerFrame = static_cast<double>(in.stride) / static_cast<double>(in.sampledFrames);
    uint64_t allMarks = 0;
    for (unsigned i = 0; i < kStepCount; ++i) {
        allMarks += w.marks[i];
        out->seen[i] = w.marks[i] != 0;
        if (!out->seen[i]) continue;
        out->netMs[i] = netTicks(w.ticks[i], w.marks[i], floorTicks) * toMs;
        out->callsPerFrame[i] = static_cast<double>(w.marks[i]) * toPerFrame;
        if (i == kGameDraw) out->gameMs += out->netMs[i];
        else out->edvrMs += out->netMs[i];
    }
    out->marksPerDraw = w.clocked ? static_cast<double>(allMarks) / static_cast<double>(w.clocked) : 0.0;
    for (unsigned k = 0; k < kKinds; ++k) {
        out->kindSeen[k] = w.kindDraws[k] != 0;
        if (!out->kindSeen[k]) continue;
        out->kindDrawsPerFrame[k] = static_cast<double>(w.kindDraws[k]) * toPerFrame;
        out->kindMs[k] = netTicks(w.kindTicks[k], w.kindMarks[k], floorTicks) * toMs;
    }
    return true;
}

namespace textutil {
// Appends to a fixed buffer: truncates, never overruns, always NUL-terminated.
struct Out {
    char*  buf;
    size_t cap;
    size_t len = 0;
    Out(char* b, size_t c) : buf(b), cap(c) {
        if (cap) buf[0] = '\0';
    }
    void add(const char* fmt, ...) {
        if (cap == 0 || len + 1 >= cap) return;
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(buf + len, cap - len, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        const size_t room = cap - 1 - len;
        len += static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room;
    }
};
// A frame's milliseconds never run to six digits and a call count never to nine: the clamp bounds the line's length, not the data.
inline double clampMs(double v) { return v > 99999.999 ? 99999.999 : (v < 0.0 ? 0.0 : v); }
inline double clampCount(double v) { return v > 99999999.0 ? 99999999.0 : (v < 0.0 ? 0.0 : v); }
}  // namespace textutil

// The first line: the window, the floor, every step's net ms per sampled frame, EDVR's sum, the game's draw. The log line holds about
// 1170 characters (Log::note's 1200 less its stamp and truncation mark), so the report is two lines and each stays under 1150 at its worst.
inline size_t formatStepsLine(char* out, size_t cap, const Window& w, const FormatInfo& in) {
    textutil::Out o(out, cap);
    Totals t;
    const bool ok = summarise(w, in, &t);
    o.add("draw hook CPU by step: 1800-frame window ending %u; %u sampled frames, %llu draws clocked by step (every %uth draw, "
          "offset %u from the line above's). A mark is one clock read: about %.0f ns, %.1f per clocked draw, taken off every figure. "
          "ms per frame, scaled by %u from the sampled frames (- never ran): ",
          in.endFrame, in.sampledFrames, static_cast<unsigned long long>(w.clocked), in.stride, in.stride / 2,
          textutil::clampCount(in.floorNs), textutil::clampCount(ok ? t.marksPerDraw : 0.0), in.stride);
    for (unsigned i = 0; i < kStepCount; ++i) {
        if (ok && t.seen[i]) o.add("%s %.3f", stepName(i), textutil::clampMs(t.netMs[i]));
        else o.add("%s -", stepName(i));
        o.add(i + 1 < kStepCount ? ", " : "; ");
    }
    if (ok) {
        o.add("EDVR %.3f ms (the line above counts weapon as the game's, so it reads near this less weapon); "
              "game draw %.3f ms (the game's own). ",
              textutil::clampMs(t.edvrMs), textutil::clampMs(t.gameMs));
    } else {
        o.add("EDVR -; game draw - (no sampled frame in the window). ");
    }
    o.add("Temporal AA %s.", in.aaOn ? "on" : "off");
    return o.len;
}

// The second line: how often each step ran, and the draw kinds.
inline size_t formatCallsLine(char* out, size_t cap, const Window& w, const FormatInfo& in) {
    textutil::Out o(out, cap);
    Totals t;
    const bool ok = summarise(w, in, &t);
    o.add("draw hook CPU by step, calls and kinds: window ending %u. Calls per frame (marks, scaled by %u): ", in.endFrame, in.stride);
    for (unsigned i = 0; i < kStepCount; ++i) {
        if (ok && t.seen[i]) o.add("%s %.0f", stepName(i), textutil::clampCount(t.callsPerFrame[i]));
        else o.add("%s -", stepName(i));
        o.add(i + 1 < kStepCount ? ", " : "; ");
    }
    o.add("by draw kind, draws per frame and EDVR ms per frame: ");
    for (unsigned k = 0; k < kKinds; ++k) {
        if (ok && t.kindSeen[k]) {
            o.add("%c %.0f draws %.3f ms", kindLetter(k), textutil::clampCount(t.kindDrawsPerFrame[k]), textutil::clampMs(t.kindMs[k]));
        } else {
            o.add("%c -", kindLetter(k));
        }
        o.add(k + 1 < kKinds ? ", " : ". ");
    }
    o.add("Names: v: is the verdict (beginPanelOverride), f: the forward scope around the game's draw, p: the work after the "
          "game's draw (X draws only); GAME is the game's own draw.");
    return o.len;
}

}  // namespace drawstep
}  // namespace edvr
