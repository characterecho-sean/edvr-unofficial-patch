// The flat HDR route's crash-safe breadcrumbs (docs\design-flat-temporal-aa-2026-09-23.md, section 81; the macOS arc,
// docs\macos-dxmt-2026-09-30.md).
//
// WHY THIS EXISTS. Under CrossOver with DXMT (a D3D11-to-Metal layer, Apple M4 Max, flat profile) Elite exits cleanly
// about 30 s into a session, at the first frame the HDR route would treat, with DLSS and with FSR alike and never with
// experimental.temporal_aa_before_post off. There is no UNHANDLED line, only "gfx: process exit", and the gfx log does
// not flush its last lines on the way out: what it shows is the route's first selection, a decline
// (engine-source-not-ready), engine motion's "substitution starts", and then nothing. Which step of the treatment
// ends the process is therefore not in the log. It is in edvr_breadcrumbs.txt, if the steps write there.
//
// THAT EXIT IS SOLVED, AND THE CRUMBS STAY FOR DXMT ONLY (docs\macos-dxmt-2026-09-30.md). The trail named the step: DXMT
// aborts in ID3D11DeviceContext1::SwapDeviceContextState, and the resolver now isolates by an explicit capture there
// (flat_context_state.h). The Mac flight that carried it ran the route's treatment for minutes, DLSS and FSR both. The
// crumbs are still the way to see what DXMT does with the next call EDVR makes, so they stay, written on DXMT and on
// nothing else:
//
// THE GATE. Nothing here writes unless the flat runtime has found the device to be DXMT's and said so with
// hdrCrumbEnable(true). The finding is the markers' (flat_context_isolation.h: the device's or the context's private
// interface, the module's version resource, an adapter name that begins Apple), made once, at the first Present with a
// temporal mode on, before the route's key is first read. The gate is that detection and nothing else:
// advanced.flat_context_isolation=capture on a Windows device changes how the resolver isolates the game's state and
// turns no crumb on. With the gate shut every writer below returns before it touches any state: the armed line,
// admitted, reached and declined, every span (the resolver's, the backends', the explicit capture's eleven groups), the
// frame end, and the Present hook's pair. edvr_breadcrumbs.txt on Windows holds none of them, and the budget is never
// started. What stays on every device is no crumb: the 5 s line's step counts (FlatMonoResolveStats::hdrCaptured and the
// rest, the route's own window) are plain counters read into a log line.
//// HOW THE FILE IS WRITTEN (answered, not assumed). breadcrumb() (src\common\proxy.cpp) builds the path by hand, opens
// edvr_breadcrumbs.txt for FILE_APPEND_DATA, calls WriteFile ONCE with the whole line, and closes the handle, every
// call. No buffer of ours stands between the call and the operating system: no stdio, no log ring, no flusher thread.
// When breadcrumb() returns the line is in the OS's (under Wine, the host's) file cache, and a process that dies
// afterwards, by any route, leaves it there. It is not FILE_FLAG_WRITE_THROUGH, which only matters if the machine
// itself goes down. So these crumbs use it as it is, and the last line of the file names the last step that began.
//
// THE RULES, every one of them behind THE GATE above. (1) The first kHdrCrumbFrames frames of a session that REACH the
// resolver write a crumb before and after every step, from the route's selection to the frame's Present. A frame the route
// admits but declines before it gets there (engine motion not ready yet, say) does not use one of the three up: it writes
// "admitted" and "declined" and its own frame end, for at most kHdrCrumbDeclined such frames, so a run of early declines
// cannot spend the trail before the first real treatment and cannot hide it. (2) The whole session writes at most
// kHdrCrumbCap crumbs, the last of them a line saying the budget ran out. (3) After the third frame, and after the budget,
// every site costs one relaxed load and one branch: nothing is formatted, nothing is written. (4) Crumbs change nothing the
// route does.
//
// THE LINE. "gfx: hdr-treat K/3 <step> [begin|end] [detail]", after the stamp breadcrumb() puts in front of every
// line. K is the frame's number among those that reach the resolver (1, 2 or 3); before a frame has reached it, it is
// the number the frame will have if it does, so two declined frames and a third that goes through all read 1/3 until
// the third is numbered by reaching. "frame=" in the admitted and reached lines tells them apart. An "end" line
// carries the result (hr=0x.., ok=..); E_PENDING (0x8000000A) in an hr field means that call was never reached.
//
// ONE LINE TELLS THE TRAIL FROM NO TRAIL. "gfx: hdr-treat armed key=auto frames=3 declined=3 cap=192" is written once when
// the route is switched on (the key read as auto, at startup or later; three a session at most, outside the budget) and the gate
// is open. A file with no such line came from a build without these crumbs, or from a device that is not DXMT's. A file with it
// and no "admitted" after it is a session that ended before the route took a frame. One with "admitted" and nothing after names
// the step the route was in.
//
// THE CRUMBS, in the order a normal frame writes them (function names; the line numbers move):
//
//   admitted frame=F backend=B                 FlatRuntimeDrawScope::treatHdr, first statement: the route took the frame
//   declined why=W                             treatHdr's decline(): the copy route serves the frame (also after reaching)
//   [create-depth-srv begin|end]               depthView(): the SRV over the scene depth, the first time only
//   reached frame=F backend=B step=S           before flatMonoResolve (S=resolve) or the spatial recovery (S=spatial-recovery)
//   [create-context-state, create-compute-shaders, create-constants-sampler,
//    create-hdr-vs, create-hdr-ps-finish, create-hdr-ps-spatial,
//    create-texture (one per image: role, format, size, hr srv uav), create-rtv]
//                                              flat_mono_resolve.cpp initialize/initializeHdr/resources/hdrTargetView,
//                                              the first time (and after a resize), at the preflight or at the resolve
//   capture-state begin|end by=swap|capture    the game's pipeline state taken out (Isolate's constructor): swapped out, or on DXMT
//                                              (whose swap aborts the process) read out by the explicit capture
//     [capture-ia, capture-vs, capture-hs, capture-ds, capture-gs, capture-ps, capture-cs, capture-so, capture-om,
//      capture-rs, capture-predication  begin|end]
//                                              the explicit capture's eleven groups (flat_context_state.h), written for the
//                                              session's first capture only; each end line says what the game had bound
//   [backend-available begin|end]              the SDK's own initialisation, the first ask only
//   copy-h begin|end                           the GPU span's timestamps, the constants upload, CopyResource H -> private copy
//   prep begin|end                             the prep dispatch: bindings, Dispatch, unbind
//   backend begin|end                          the evaluation: taa dispatch, or dlaaEvaluate / fsr3Evaluate, which write
//     [backend-query begin|end]                  (DLSS) the runtime's optimal-settings questions, when it makes the feature
//     [backend-create begin|end]                 the feature or the context, the first time (FSR: its three shared
//     [create-texture begin|end]                 surfaces first)
//     backend-evaluate begin|end                 the NGX evaluate call, or AMD's dispatch
//   finish-bind begin|end                      ClearState, the bindings, and H bound as the render target
//   finish-draw begin|end                      the pixel-shader draw into H
//   restore-state begin|end by=swap|capture    the game's pipeline state put back (Isolate's destructor)
//     [restore-ia, restore-vs, ... restore-rs, restore-predication  begin|end]
//                                              the explicit capture's groups going back, the session's first restore only
//   before-present begin|end                   engine motion's state back, the census span closed (flatRuntimeBeforePresent)
//   present begin|end hr=.. removed=..         the real Present (hookedPresent); removed is GetDeviceRemovedReason
//   frame-end begin hr=..  ...  frame-end end  everything flatRuntimePresent does for the frame just ended, the
//     [preflight begin|end]                      next frame's preflight among it, with its creations above
//
// The bracketed ones happen only when the thing they bracket does. The spatial recovery (a refused backend, or a frame
// whose jitter did not land) writes the same capture-state, copy-h, finish-bind, finish-draw and restore-state crumbs.
//
// WHAT IT COSTS IN CRUMBS. A frame that reaches the resolver writes 24 (admitted, reached, eleven pairs), the first of them 6
// more (the target view, the feature or context, DLSS's size queries), a declined one 4, the preflight that makes the route's
// objects (the first time, at the Present of the first admitted frame) about 30 and the depth view 2. Three reaching frames, two
// declined ones and that preflight come to about 115 (FSR 4 more, for its three shared surfaces). A device that isolates by the
// explicit capture adds 44 once: eleven pairs at the session's first capture and eleven at its first restore, about 160 in all.
// That is why the cap is 192 and not the 60 the per-frame steps alone would make: the one-time creations are the steps a Metal
// layer is likeliest to refuse, and the capture's Get and Set calls are the first of those it has answered to.
//
// The state is one struct, and the gate a load. Nothing here allocates, locks or throws; every write is one
// breadcrumb() call, and a crumb that cannot be written is simply not there.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../common/format_support_decode.h"
#include "../common/proxy.h"

namespace edvr {

// Frames that reach the resolver and write every step. The design asks for three: the first tells whether the route's
// first treatment ends the process, the next two whether a second and third with history do.
constexpr uint32_t kHdrCrumbFrames = 3;
// Admitted frames that never got there and still write (admitted, declined, frame end). Past this they are silent, and a
// later frame that does reach the resolver writes again from there.
constexpr uint32_t kHdrCrumbDeclined = 3;
// Every crumb of the session, the one-time creations, the declined frames and the explicit capture's groups included (see WHAT
// IT COSTS above); the last of these is the "budget spent" line.
constexpr uint32_t kHdrCrumbCap = 192;

struct HdrCrumbState {
    std::atomic<bool> enabled{false};    // THE GATE: the device is DXMT's (hdrCrumbEnable); shut in every process that has not been told
    std::atomic<bool> live{false};       // a frame that writes is in progress: from its admission or reach to its Present
    std::atomic<uint32_t> written{0};    // crumbs written or refused for the budget, this session
    uint32_t reached = 0;                // frames that reached the resolver, this session
    uint32_t declinedFrames = 0;         // live frames that were declined before reaching it
    uint32_t armedSaid = 0;              // "armed" lines written (outside the budget)
    bool frameReached = false;           // the frame in progress has reached the resolver
    bool frameDeclined = false;          // ... has been counted as declined
    bool spent = false;                  // the budget ran out
    bool captureCrumbed = false;         // the explicit capture's per-group crumbs have been written for a capture (once a session)
    bool restoreCrumbed = false;         // ... and for a restore
};
// One per process. Inline so the resolver, the backends and the runtime share it, and the rigs that compile the resolver alone
// get their own. Plain data with a trivial destructor, so static destruction under the loader lock touches nothing.
inline HdrCrumbState g_hdrCrumbs;

// THE GATE (the header's second paragraph): open only on a DXMT device. The flat runtime opens it once, from the markers
// (flatCrumbsWantedFor, flat_context_isolation.h); a process nobody has told has it shut, which is every Windows process, and
// every writer below tests it before it touches anything else. Shutting it ends a frame in progress.
inline bool hdrCrumbEnabled() noexcept { return g_hdrCrumbs.enabled.load(std::memory_order_relaxed); }
inline void hdrCrumbEnable(bool on) noexcept {
    HdrCrumbState& c = g_hdrCrumbs;
    c.enabled.store(on, std::memory_order_relaxed);
    if (!on) c.live.store(false, std::memory_order_relaxed);
}

// The gate every site tests: a frame that writes is in progress. It can only become true with the gate open (hdrCrumbAdmit and
// hdrCrumbReach, the two places that raise it, test the gate first), and shutting the gate lowers it.
inline bool hdrCrumbLive() noexcept { return g_hdrCrumbs.live.load(std::memory_order_relaxed); }

// Test only: a session starts once per process. The gate is not session state (it says which device this is), so a reset leaves it
// as it is.
inline void hdrCrumbReset() noexcept {
    HdrCrumbState& c = g_hdrCrumbs;
    c.live.store(false, std::memory_order_relaxed);
    c.written.store(0, std::memory_order_relaxed);
    c.reached = c.declinedFrames = c.armedSaid = 0;
    c.frameReached = c.frameDeclined = c.spent = false;
    c.captureCrumbed = c.restoreCrumbed = false;
}

// The K of "K/3": the frame's number among those that reach the resolver, or the number it will have if it does.
inline uint32_t hdrCrumbSlot() noexcept {
    const HdrCrumbState& c = g_hdrCrumbs;
    const uint32_t n = c.frameReached ? c.reached : c.reached + 1;
    return n > kHdrCrumbFrames ? kHdrCrumbFrames : (n ? n : 1);
}

// The explicit capture's per-group crumbs (flat_context_state.h) are for the session's first capture and first restore, which
// are the first calls a Metal layer has answered to: true once each, for the first call made while a frame that writes is in
// progress and the caller's own gate (`on`, the resolver's flag for a call that belongs to the route) is open.
inline bool hdrCrumbFirstCapture(bool on) noexcept {
    HdrCrumbState& c = g_hdrCrumbs;
    if (!on || !hdrCrumbLive() || c.captureCrumbed) return false;
    c.captureCrumbed = true;
    return true;
}
inline bool hdrCrumbFirstRestore(bool on) noexcept {
    HdrCrumbState& c = g_hdrCrumbs;
    if (!on || !hdrCrumbLive() || c.restoreCrumbed) return false;
    c.restoreCrumbed = true;
    return true;
}

// The one writer. `edge` is "begin", "end" or null for a line that stands alone; `detail` may be null or empty. The
// cap-th crumb of a session is the line that says the budget is spent, and closes the gate.
inline void hdrCrumbEmit(const char* step, const char* edge, const char* detail) noexcept {
    HdrCrumbState& c = g_hdrCrumbs;
    if (!hdrCrumbEnabled()) return;   // the gate, at the one writer too: a caller that skipped its own test still writes nothing, and counts nothing
    const uint32_t ordinal = c.written.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal > kHdrCrumbCap) return;
    char line[232];
    if (ordinal == kHdrCrumbCap) {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "gfx: hdr-treat budget of %u crumbs spent; none follow",
                    static_cast<unsigned>(kHdrCrumbCap));
        c.spent = true;
        c.live.store(false, std::memory_order_relaxed);
    } else {
        _snprintf_s(line, sizeof(line), _TRUNCATE, "gfx: hdr-treat %u/%u %s%s%s%s%s", static_cast<unsigned>(hdrCrumbSlot()),
                    static_cast<unsigned>(kHdrCrumbFrames), step ? step : "?", edge ? " " : "", edge ? edge : "",
                    (detail && *detail) ? " " : "", (detail && *detail) ? detail : "");
    }
    breadcrumb(line);
}
inline void hdrCrumbWrite(const char* step, const char* edge) noexcept { hdrCrumbEmit(step, edge, nullptr); }
template <class... A>
inline void hdrCrumbWrite(const char* step, const char* edge, const char* fmt, A... args) noexcept {
    char detail[176];
    _snprintf_s(detail, sizeof(detail), _TRUNCATE, fmt, args...);
    hdrCrumbEmit(step, edge, detail);
}

// One step of the treatment: "<step> begin [detail]" now, "<step> end [result]" when it goes out of scope or close() is
// called. `on` is the caller's own gate (the resolver's flag for a call that belongs to the route, the backends'
// `hdr` argument, or hdrCrumbLive() for the runtime's own steps); the session's gate is tested here as well. A span
// that is not armed costs that one test. The end line is written on every path out of the scope, a `return` included;
// a fault that unwinds past the scope does not write one, which is what the last begin in the file then means.
class HdrCrumbSpan {
public:
    HdrCrumbSpan(bool on, const char* step) noexcept : step_(step), armed_(on && hdrCrumbLive()) {
        if (armed_) { end_[0] = '\0'; hdrCrumbWrite(step_, "begin"); }
    }
    template <class... A>
    HdrCrumbSpan(bool on, const char* step, const char* fmt, A... args) noexcept : step_(step), armed_(on && hdrCrumbLive()) {
        if (armed_) { end_[0] = '\0'; hdrCrumbWrite(step_, "begin", fmt, args...); }
    }
    HdrCrumbSpan(const HdrCrumbSpan&) = delete;
    HdrCrumbSpan& operator=(const HdrCrumbSpan&) = delete;
    ~HdrCrumbSpan() noexcept { close(); }
    // What the end line says (an HRESULT, a verdict). Set before the scope ends.
    template <class... A>
    void result(const char* fmt, A... args) noexcept {
        if (armed_) _snprintf_s(end_, sizeof(end_), _TRUNCATE, fmt, args...);
    }
    // The end line now, for a step that does not end where its scope does.
    void close() noexcept {
        if (!armed_) return;
        armed_ = false;
        hdrCrumbEmit(step_, "end", end_);
    }
    bool armed() const noexcept { return armed_; }

private:
    const char* step_;
    bool armed_;
    char end_[112];
};

// ---- the frame's life -----------------------------------------------------------------------------------------------

// A DXGI format's name, for the crumbs that say what a resource is.
inline const char* hdrCrumbFormat(uint32_t format) noexcept {
    const char* name = dxgiFormatName(format);
    return name ? name : "?";
}

// The route is switched on (ONE LINE TELLS THE TRAIL FROM NO TRAIL, above). Outside the budget, and at most three a session:
// the key can be flipped off and on, and a line per flip would be a line the trail does not need. True when it wrote.
inline bool hdrCrumbArmed(const char* key) noexcept {
    HdrCrumbState& c = g_hdrCrumbs;
    if (!hdrCrumbEnabled() || c.armedSaid >= 3) return false;
    ++c.armedSaid;
    char line[160];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "gfx: hdr-treat armed key=%s frames=%u declined=%u cap=%u", key ? key : "?",
                static_cast<unsigned>(kHdrCrumbFrames), static_cast<unsigned>(kHdrCrumbDeclined), static_cast<unsigned>(kHdrCrumbCap));
    breadcrumb(line);
    return true;
}

// The route admitted the frame (treatHdr's first statement). Once the third frame has reached the resolver, or the budget
// is spent, this is one compare and a return.
inline void hdrCrumbAdmit(uint64_t frame, const char* backend) noexcept {
    HdrCrumbState& c = g_hdrCrumbs;
    if (!hdrCrumbEnabled() || c.reached >= kHdrCrumbFrames || c.spent) return;
    c.frameReached = c.frameDeclined = false;
    if (c.declinedFrames >= kHdrCrumbDeclined) { c.live.store(false, std::memory_order_relaxed); return; }
    c.live.store(true, std::memory_order_relaxed);
    hdrCrumbWrite("admitted", nullptr, "frame=%llu backend=%s", static_cast<unsigned long long>(frame), backend ? backend : "?");
}
// The treatment declined the frame. A frame that had not reached the resolver is counted once toward the allowance.
inline void hdrCrumbDeclined(const char* why) noexcept {
    if (!hdrCrumbLive()) return;
    HdrCrumbState& c = g_hdrCrumbs;
    hdrCrumbWrite("declined", nullptr, "why=%s", why ? why : "?");
    if (!c.frameReached && !c.frameDeclined) { c.frameDeclined = true; ++c.declinedFrames; }
}
// The frame is about to be handed to the resolver (`step` says which call): it is numbered from here, and writes from here
// even if its admission was silent because the allowance for declined frames was spent. Once per frame.
inline void hdrCrumbReach(uint64_t frame, const char* backend, const char* step) noexcept {
    HdrCrumbState& c = g_hdrCrumbs;
    if (!hdrCrumbEnabled() || c.frameReached || c.reached >= kHdrCrumbFrames || c.spent) return;
    c.frameReached = true;
    ++c.reached;
    c.live.store(true, std::memory_order_relaxed);
    hdrCrumbWrite("reached", nullptr, "frame=%llu backend=%s step=%s", static_cast<unsigned long long>(frame),
                  backend ? backend : "?", step ? step : "?");
}
// A frame that has reached the resolver: the steps that follow the treatment (the real Present among them) write for it.
// A declined frame's own Present writes only its frame end.
inline bool hdrCrumbPresentSide() noexcept {
    return g_hdrCrumbs.live.load(std::memory_order_relaxed) && g_hdrCrumbs.frameReached;
}

// The frame's end, at the top of flatRuntimePresent (which the real Present has already returned to, `hr` its result):
// "frame-end begin", everything the Present does for the frame, "frame-end end", and then the gate closes until the route
// admits another frame. Every path out of flatRuntimePresent ends it.
class HdrCrumbFrameEnd {
public:
    explicit HdrCrumbFrameEnd(long hr) noexcept : armed_(hdrCrumbLive()) {
        if (armed_) hdrCrumbWrite("frame-end", "begin", "hr=0x%08X", static_cast<unsigned>(hr));
    }
    HdrCrumbFrameEnd(const HdrCrumbFrameEnd&) = delete;
    HdrCrumbFrameEnd& operator=(const HdrCrumbFrameEnd&) = delete;
    ~HdrCrumbFrameEnd() noexcept {
        if (!armed_) return;
        hdrCrumbWrite("frame-end", "end");
        g_hdrCrumbs.live.store(false, std::memory_order_relaxed);
        g_hdrCrumbs.frameReached = g_hdrCrumbs.frameDeclined = false;
    }

private:
    bool armed_;
};

}  // namespace edvr
