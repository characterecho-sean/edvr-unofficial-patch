// aa_off_test -- the AA-off draw path and the cheap instruments it rides on (2026-10-13).
//
//   Cut B: draw_fast_path.h's decisions, the same functions vscreen.cpp's draw thunks and bind hooks call. A model
//          of a run of draws (a claimed draw, a gate-open draw, an unclaimed one) counts the verdict scopes, the
//          engine velocity calls and the shadow records the thunks would make. The model is a transcription of the
//          thunk's branches; the real thunks cannot run without a device, so the rig pins the decisions they take.
//   Cut C: map_wait.h's call counts (exact) and sampled ticks (scaled at take), and map_wait_sample.h's sampler.
//   Cut D: gpu_frame_timing.h's gpuFrameCommandIfLive, with a counting stub for the out-of-line gpuFrameCommand:
//          with the instrument off, no call is made; on, every call is made.
//   Cut E: classifier_memo.h (built 2026-10-27, the ui_depth eye-draw memo): a call hits only when something is stored, both halves
//          of its key equal the stored key and the stored exit is a plain decline; a moved generation or epoch is a miss.
//   Cut F: vs_may_want.h (build 2, #152): the byte stored beside the bound vertex shader's hash. A clear bit means the subscriber's own
//          pure predicate rejects every pixel shader for that vertex shader; hash 0 and the shipped shaders are "may want".
//   Cut H: draw_call_flags.h (build 6, item 3, #184): a draw's own flags and the pure "which second issues for these flags and this verdict" function vscreen.cpp's
//          forwardWithVerdict calls: a skipped or blocked draw issues nothing, a layered draw issues neither coverage nor hologram, the planet coverage needs only the owner,
//          and the pending clear is for the draw that set a pending flag alone.
//
// Exit codes: 0 pass, 1 a check failed, 2 usage. --dry-run touches nothing.
#include "../../src/common/gpu_disjoint_clock.h"
#include "../../src/d3d11/classifier_memo.h"
#include "../../src/d3d11/draw_call_flags.h"
#include "../../src/d3d11/draw_fast_path.h"
#include "../../src/d3d11/frame_span_tile.h"
#include "../../src/d3d11/gpu_frame_timing.h"
#include "../../src/d3d11/map_wait.h"
#include "../../src/d3d11/vs_may_want.h"

// The frame span holds at most kFrameSpanSlots pairs of two timestamps each (gpu_census.cpp); every one must fit the
// clock's per-record lease budget (kLeases, 512), or a pair would be refused rather than held.
static_assert(edvr::kFrameSpanSlots * 2u <= edvr::DisjointClock::kLeases,
              "the frame span's timestamps fit the disjoint clock's lease budget");

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace edvr {

// The stub of gpu_frame_timing.cpp's out-of-line call, counting what the thunks would have made.
static unsigned g_gpuCommandCalls = 0;
void gpuFrameCommand(ID3D11DeviceContext*) noexcept { ++g_gpuCommandCalls; }

namespace detail {
thread_local bool g_gpuFrameInternal = false;
std::atomic<bool> g_gpuFrameCommandLive{false};
std::atomic<bool> g_frameWorkArmed{false};
}  // namespace detail

// The stub of gpu_census.cpp's first-command hook (2026-10-14), counting the calls the armed branch makes.
static unsigned g_frameWorkCalls = 0;
void frameWorkFirstCommand(ID3D11DeviceContext*) noexcept { ++g_frameWorkCalls; }

}  // namespace edvr

namespace {

int g_failed = 0, g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}

// Cut B: the fast-path decision over all 32 input combinations.
void testFastPathTable() {
    unsigned taken = 0, mismatch = 0;
    for (unsigned bits = 0; bits < 32; ++bits) {
        const edvr::DrawPathInputs in{(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0, (bits & 16) != 0};
        const bool expect = !in.claimed && in.aaOff && !in.gateWanted && !in.engineVelocityLive && !in.vrWorld;
        const bool got = edvr::drawFastPathTaken(in);
        taken += got;
        mismatch += got != expect;
    }
    check(mismatch == 0, "fast path: the decision matches its rule for all 32 input combinations");
    check(taken == 1, "fast path: exactly one combination takes it (AA off, no claim, no gate, no engine velocity, no world route)");
    // Each non-AA-off or claimed or gate-open or EV-live or world-route draw is kept on the full path.
    const edvr::DrawPathInputs base{true, false, false, false, false};
    check(edvr::drawFastPathTaken(base), "fast path: the unclaimed AA-off draw is taken");
    edvr::DrawPathInputs c = base; c.claimed = true;
    check(!edvr::drawFastPathTaken(c), "fast path: a fix-claimed draw keeps the full path");
    c = base; c.gateWanted = true;
    check(!edvr::drawFastPathTaken(c), "fast path: an open subscriber gate keeps the full path");
    c = base; c.engineVelocityLive = true;
    check(!edvr::drawFastPathTaken(c), "fast path: live engine velocity (a diagnostic) keeps the full path");
    c = base; c.vrWorld = true;
    check(!edvr::drawFastPathTaken(c), "fast path: the VR world route keeps the full path");
    c = base; c.aaOff = false;
    check(!edvr::drawFastPathTaken(c), "fast path: AA on never takes it (behaviour with AA on is unchanged)");
}

// F201: the marker fix's bind. forwardWithVerdict binds the patched b0 for a draw the UI layer took (the layer's bracket) and, with the temporal path off, for the game's own single issue:
// markerFixBindWanted over all four (layered, temporal path) combinations, and the fast path's claim for a decided draw.
void testMarkerFixBind() {
    check(edvr::markerFixBindWanted(true, true), "marker bind: layered with a temporal mode on: bound (as before)");
    check(!edvr::markerFixBindWanted(false, true), "marker bind: not layered with a temporal mode on (the layer stood down, or no family): not bound, as before (the DLSS path is unchanged)");
    check(edvr::markerFixBindWanted(true, false), "marker bind: layered with the temporal mode off cannot happen, and would be bound");
    check(edvr::markerFixBindWanted(false, false), "marker bind: not layered with the temporal mode off (Anti-aliasing off, no layer): bound, on the game's own issue (F201)");
    // with a temporal mode on the answer is `layered`, bit for bit
    bool same = true;
    for (int l = 0; l < 2; ++l) same = same && edvr::markerFixBindWanted(l != 0, true) == (l != 0);
    check(same, "marker bind: with a temporal mode on the predicate is `layered` exactly");
    // the fast path: a decided draw is claimed while the AA is off; a draw nothing decided still is not; with the AA on nothing changes (the path is never taken there)
    check(!edvr::drawClaimed(false, false) && edvr::drawClaimed(true, false) && edvr::drawClaimed(false, true) && edvr::drawClaimed(true, true), "claim: a verdict or a pending marker decision claims the draw");
    const auto fast = [](bool aaOff, bool verdictClaims, bool pending) {
        // drawPathFast: the pending flag is asked only while the AA is off
        return edvr::drawFastPathTaken(edvr::DrawPathInputs{aaOff, false, false, false, edvr::drawClaimed(verdictClaims, aaOff && pending)});
    };
    check(fast(true, false, false), "claim: AA off, nothing decided: the fast path is taken, as before");
    check(!fast(true, false, true), "claim: AA off, a marker decision pending: the full path, where the bind is (F201)");
    check(!fast(true, true, false) && !fast(true, true, true), "claim: AA off, a verdict claims it: the full path, as before");
    check(!fast(false, false, false) && !fast(false, false, true) && !fast(false, true, true), "claim: AA on never takes the fast path, pending or not (unchanged)");
}

// F201: the wiring, by a scan of the sources (this rig runs from the repository root, as build.bat does): vscreen.cpp's bind uses the predicate and keeps the Begin and End calls, nothing
// returns between them, drawPathFast claims a decided draw, and every write of native on foot's pending decision mirrors the flag the fast path reads.
static std::string slurpFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}
void testMarkerFixWiring() {
    const std::string v = slurpFile("src/d3d11/vscreen.cpp"), n = slurpFile("src/d3d11/native_on_foot.cpp");
    check(!v.empty() && !n.empty(), "marker bind wiring: vscreen.cpp and native_on_foot.cpp open (run from the repository root)");
    const std::string line = "const bool markerFixed = markerFixBindWanted(layered, temporalPassWantsFssChrome()) && owner && nativeOnFootMarkerFixLive() && nativeOnFootMarkerFixBegin(self);";
    check(countOf(v, line) == 1 && countOf(v, "nativeOnFootMarkerFixBegin(self)") == 1 && countOf(v, "layered && owner && nativeOnFootMarkerFixLive()") == 0,
          "marker bind wiring: the bind asks markerFixBindWanted(layered, the temporal path), once, and the old layered-only expression is gone");
    check(countOf(v, "if (markerFixed) nativeOnFootMarkerFixEnd(self);") == 1, "marker bind wiring: End is called once, on markerFixed");
    const size_t begin = v.find(line), end = v.find("if (markerFixed) nativeOnFootMarkerFixEnd(self);");
    bool noReturn = begin != std::string::npos && end != std::string::npos && begin < end;
    for (size_t at = begin; noReturn && at < end;) {
        const size_t eol = v.find('\n', at);
        std::string code = v.substr(at, (eol == std::string::npos ? end : eol) - at);
        const size_t comment = code.find("//");
        if (comment != std::string::npos) code.erase(comment);
        for (size_t r = code.find("return"); r != std::string::npos; r = code.find("return", r + 1)) {
            const bool wordStart = r == 0 || !(std::isalnum(static_cast<unsigned char>(code[r - 1])) || code[r - 1] == '_');
            const bool wordEnd = r + 6 >= code.size() || !(std::isalnum(static_cast<unsigned char>(code[r + 6])) || code[r + 6] == '_');
            if (wordStart && wordEnd) noReturn = false;
        }
        if (eol == std::string::npos) break;
        at = eol + 1;
    }
    check(noReturn, "marker bind wiring: no return between the bind and its End, so End is reached on every path where Begin bound");
    check(countOf(v, "drawClaimed(v != DrawVerdict::kNone, aaOff && nativeOnFootMarkerFixPending())") == 1,
          "marker bind wiring: drawPathFast claims a decided draw, asking the flag only while the AA is off");
    check(countOf(n, "g_fix.pending = true;") == 2 && countOf(n, "g_nativeOnFootMarkerFixPending.store(true, std::memory_order_relaxed);") == 2 &&
              countOf(n, "g_fix.pending = false;") == 2 && countOf(n, "g_nativeOnFootMarkerFixPending.store(false, std::memory_order_relaxed);") == 3,
          "marker bind wiring: every write of the pending decision mirrors the flag the fast path reads (two sets, two clears, and the shutdown's)");
}

// Cut B: the bind records. Only AA off with engine velocity closed skips VsSrv33 and Blend.
void testBindShadows() {
    check(edvr::bindShadowsSkipped(true, false), "bind shadows: skipped with AA off and engine velocity closed");
    check(!edvr::bindShadowsSkipped(true, true), "bind shadows: recorded while engine velocity is live");
    check(!edvr::bindShadowsSkipped(false, false), "bind shadows: recorded with AA on");
    check(!edvr::bindShadowsSkipped(false, true), "bind shadows: recorded with AA on and engine velocity live");
}

// Cut B: a model of the thunks over a run of draws. The model is the branch structure of hookedDraw / hookedDrawIndexed
// / hookedDrawInstanced: the verdict decides the scope; the fast path decides the scope and engine velocity; an
// unclaimed owner draw calls engine velocity on the full path only.
struct DrawTally {
    unsigned scopes = 0;          // forwardWithVerdict scopes entered
    unsigned engineVelocity = 0;  // engineVelocityBeforeDraw calls
    unsigned fastDraws = 0;       // draws issued directly (fast path)
    unsigned realDraws = 0;       // game draws issued, either way
};

DrawTally runDraws(bool aaOff, bool gateWanted, unsigned draws, unsigned claimEvery) {
    DrawTally t;
    for (unsigned i = 0; i < draws; ++i) {
        const bool claimed = claimEvery && (i % claimEvery) == 0;
        const edvr::DrawPathInputs in{aaOff, gateWanted, false, false, claimed};
        const bool fast = edvr::drawFastPathTaken(in);
        if (!fast && !claimed) ++t.engineVelocity;   // the full path's engineVelocityBeforeDraw on an unclaimed owner draw
        if (fast) {
            ++t.fastDraws;
        } else {
            ++t.scopes;
        }
        ++t.realDraws;
    }
    return t;
}

void testThunkModel() {
    // AA off, gate closed, no claims: every draw is fast, no scope, no engine velocity call.
    DrawTally a = runDraws(true, false, 1000, 0);
    check(a.fastDraws == 1000 && a.scopes == 0 && a.engineVelocity == 0 && a.realDraws == 1000,
          "thunk model: AA off, no subscriber, no claims: all 1000 draws skip the scope and engine velocity");
    // AA off, every 10th draw claimed: the claimed draws keep the full path, the rest are fast.
    DrawTally b = runDraws(true, false, 1000, 10);
    check(b.scopes == 100 && b.fastDraws == 900, "thunk model: every claimed draw (100 of 1000) keeps the full path");
    check(b.engineVelocity == 0, "thunk model: no unclaimed draw calls engine velocity while AA is off");
    // AA off with an open gate (a subscriber on): nothing is fast, every draw keeps the full path.
    DrawTally c = runDraws(true, true, 1000, 0);
    check(c.fastDraws == 0 && c.scopes == 1000, "thunk model: an open subscriber gate keeps every draw on the full path");
    // AA on: nothing is fast, whatever the gate and the claims.
    DrawTally d = runDraws(false, false, 1000, 10);
    check(d.fastDraws == 0 && d.scopes == 1000 && d.engineVelocity == 900,
          "thunk model: AA on keeps the full path (unclaimed draws still call engine velocity, as before)");
}

// Cut B: the bind-side model across an AA toggle (on, then off, then on). Recorded binds only when the records are
// wanted; the frame boundary invalidates skipped slots once they are wanted again.
void testBindToggle() {
    unsigned recorded = 0, skipped = 0;
    bool skippedFlag = false;
    // The sequence is AA on, on, off, off, off, on, on: aaOff is the negation.
    const bool aaOffSequence[] = {false, false, true, true, true, false, false};
    for (bool aaOff : aaOffSequence) {
        for (unsigned bind = 0; bind < 10; ++bind) {
            if (edvr::bindShadowsSkipped(aaOff, false)) {
                ++skipped;
                skippedFlag = true;
            } else {
                ++recorded;
            }
        }
        // The frame boundary: skipped slots are invalidated once the records are wanted again.
        if (skippedFlag && !edvr::bindShadowsSkipped(aaOff, false)) skippedFlag = false;
    }
    check(recorded == 40 && skipped == 30, "bind toggle: 40 binds recorded, 30 skipped across on-on-off-off-off-on-on");
    check(!skippedFlag, "bind toggle: back on, the skipped flag is cleared by the frame boundary");
}

// Cut C: the sampler and the scaling.
void testSampler() {
    uint32_t seq = 0;
    unsigned sampled = 0;
    unsigned firstAt = 0;
    for (unsigned i = 1; i <= 1600; ++i) {
        if (edvr::mapWaitSampleNext(seq)) {
            if (!sampled) firstAt = i;
            ++sampled;
        }
    }
    check(sampled == 100, "sampler: 1 in 16 -- 100 of 1600 calls are sampled");
    check(firstAt == 16, "sampler: the first sampled call is the 16th");
    check(edvr::mapWaitScaleSampled(3) == 48, "sampler: a sampled total scales by 16");
    check(edvr::mapWaitScaleSampled(0) == 0, "sampler: an empty total scales to zero");
}

// Cut C: the Map totals through the real map_wait.cpp: calls exact, ticks scaled by the rate at take.
void testMapTotals() {
    edvr::mapWaitTake();   // zero the totals
    for (int i = 0; i < 1600; ++i) edvr::mapWaitCall(D3D11_MAP_WRITE_DISCARD);
    for (int i = 0; i < 100; ++i) edvr::mapWaitCall(D3D11_MAP_READ);
    edvr::mapWaitNote(D3D11_MAP_READ, 1000);     // one sampled read of 1000 ticks
    edvr::mapWaitNote(D3D11_MAP_WRITE, 500);     // one sampled write of 500 ticks
    const edvr::MapWaitTotals t = edvr::mapWaitTake();
    check(t.writeCalls == 1600 && t.readCalls == 100, "map totals: call counts are exact (1600 writes, 100 reads)");
    check(t.readTicks == 16000 && t.writeTicks == 8000, "map totals: the sampled ticks are scaled by 16 at take");
    check(t.longestTicks == 1000, "map totals: the longest call is a measured one, not scaled");
    const edvr::MapWaitTotals again = edvr::mapWaitTake();
    check(again.readCalls == 0 && again.writeCalls == 0 && again.readTicks == 0,
          "map totals: taking zeroes the window");
}

// Cut D: the inline guard. With the instrument off, the thunks make no call; on, every call is made.
void testGpuGuard() {
    using namespace edvr;
    // The work span's arm (2026-10-14): one more relaxed load in the same gate. Armed, every command reaches the first-command
    // hook; disarmed, none does, and the frame's first command is the one that begins the span.
    detail::g_frameWorkArmed.store(false, std::memory_order_relaxed);
    g_frameWorkCalls = 0;
    for (int i = 0; i < 100; ++i) gpuFrameCommandIfLive(nullptr);
    check(g_frameWorkCalls == 0, "work span: disarmed, no command reaches the first-command hook");
    detail::g_frameWorkArmed.store(true, std::memory_order_relaxed);
    for (int i = 0; i < 100; ++i) gpuFrameCommandIfLive(nullptr);
    check(g_frameWorkCalls == 100, "work span: armed, every command reaches the first-command hook");
    detail::g_frameWorkArmed.store(false, std::memory_order_relaxed);
    detail::g_gpuFrameCommandLive.store(false, std::memory_order_relaxed);
    g_gpuCommandCalls = 0;
    for (int i = 0; i < 1000; ++i) gpuFrameCommandIfLive(nullptr);
    check(g_gpuCommandCalls == 0, "gpu guard: with app_gpu_timing off, 1000 thunk calls make no gpuFrameCommand call");
    detail::g_gpuFrameCommandLive.store(true, std::memory_order_relaxed);
    g_gpuCommandCalls = 0;
    for (int i = 0; i < 1000; ++i) gpuFrameCommandIfLive(nullptr);
    check(g_gpuCommandCalls == 1000, "gpu guard: with app_gpu_timing on, every thunk call is made");
    detail::g_gpuFrameInternal = true;   // inside EDVR's own GPU timing call: never re-entered
    g_gpuCommandCalls = 0;
    gpuFrameCommandIfLive(nullptr);
    check(g_gpuCommandCalls == 0, "gpu guard: the reentrancy flag suppresses the call");
    detail::g_gpuFrameInternal = false;
    detail::g_gpuFrameCommandLive.store(false, std::memory_order_relaxed);
}

// The overlay's GPU TIME tile (2026-10-14): the application mean when it has samples; with the application clock off, the
// frame span when a pair has completed; "--" with its reason before either.
void testGpuTimeTile() {
    char v[16], sub[160];
    bool shown = edvr::gpuTimeTileText(true, 12.34, false, 0.0, "reason", v, sizeof(v), sub, sizeof(sub));
    check(shown && std::strcmp(v, "12.3") == 0 && std::strstr(sub, "application render") != nullptr,
          "tile: with app_gpu_timing on and samples, the application mean is shown");
    // Application clock off (no app samples) and one frame span completed: a number, labelled as the span.
    shown = edvr::gpuTimeTileText(false, 0.0, true, 4.26, "reason", v, sizeof(v), sub, sizeof(sub));
    check(shown && std::strcmp(v, "4.3") == 0, "tile: app clock off with a completed frame span shows that number");
    check(std::strstr(sub, "frame GPU work span (first command to last; includes GPU idle inside the frame)") != nullptr,
          "tile: the work span carries the required label (first command to last; includes GPU idle inside the frame)");
    // Nothing completed yet: "--" with the reason, never a number.
    shown = edvr::gpuTimeTileText(false, 0.0, false, 0.0, "frame GPU span: no sample yet", v, sizeof(v), sub, sizeof(sub));
    check(!shown && std::strcmp(v, "--") == 0 && std::strcmp(sub, "frame GPU span: no sample yet") == 0,
          "tile: '--' appears only when no sample has completed yet, with its reason");
    // A tiny buffer truncates, never overruns.
    char tiny[2];
    edvr::gpuTimeTileText(false, 0.0, true, 123.4, "", tiny, sizeof(tiny), sub, sizeof(sub));
    check(std::strlen(tiny) < sizeof(tiny), "tile: a short value buffer is truncated, not overrun");
}

// The work span against the frame period (2026-10-14), in a simulated slack frame: 90 Hz, the GPU idle until the game's first command,
// then busy briefly, then idle until the next boundary. The work span is first command to door end; the boundary-to-boundary figure
// is the frame period. The arithmetic only (frameWorkSpanMs); the GPU's own timestamps are not simulated here.
void testFrameWorkSlack() {
    const double period = 1000.0 / 90.0;   // 11.11 ms at 90 Hz
    // One slack frame: the first command at 6.0 ms into the frame, the door end at 8.5 ms. The span is the work, not the period.
    const double slack = edvr::frameWorkSpanMs(6.0, 8.5);
    check(slack > 0.0 && slack < period, "work span: a slack frame's span is less than the frame period (11.1 ms at 90 Hz)");
    check(std::fabs(slack - 2.5) < 1e-9, "work span: the span is the door end minus the first command");
    // A sequence of slack frames: every span stays under the period, and the boundary-to-boundary figure would be the period each time.
    bool allUnder = true;
    for (int i = 0; i < 100; ++i) {
        const double first = 4.0 + (i % 5) * 0.5;      // the GPU starts the frame's work between 4.0 and 6.0 ms
        const double end = first + 2.0 + (i % 3) * 0.7; // and works for 2.0 to 3.4 ms
        const double span = edvr::frameWorkSpanMs(first, end);
        allUnder = allUnder && span > 0.0 && span < period;
    }
    check(allUnder, "work span: every frame of a 100-frame slack run is under the frame period");
    check(edvr::frameWorkSpanMs(8.0, 7.0) == 0.0, "work span: an out-of-order pair clamps to zero, not a negative frame");
}

// The in-headset fps overlay's line (2026-10-14): it chose its GPU figure on its own and only the tile learned the work span, so a flight with
// the application clock off read "gpu -- ms". Now both call chooseGpuTime; this runs the choice and the line the overlay prints.
void testOverlayLine() {
    char line[120];
    using edvr::GpuTimeSource;
    // Clock off, a span in the series: a number, from the work span.
    edvr::GpuTimeChoice c = edvr::chooseGpuTime(false, false, true, 4.26, true, 3.0);
    check(c.source == GpuTimeSource::WorkSpan && std::fabs(c.ms - 4.26) < 1e-9, "overlay: clock off with the series holding spans picks the work span mean");
    edvr::overlayLineText(line, sizeof(line), 89.6, c, "cpu", true, 2.34);
    check(std::strcmp(line, "90 fps   gpu 4.3 ms   cpu 2.3 ms") == 0, "overlay: clock off with a span prints a number in the line");
    // Clock off, the series aged out but a pair completed: the last pair.
    c = edvr::chooseGpuTime(false, false, false, 0.0, true, 5.04);
    check(c.source == GpuTimeSource::WorkSpan && std::fabs(c.ms - 5.04) < 1e-9, "overlay: clock off, series empty, a completed pair: the last pair");
    // Clock off, no span anywhere: "--", never a number.
    c = edvr::chooseGpuTime(false, false, false, 0.0, false, 0.0);
    edvr::overlayLineText(line, sizeof(line), 90.0, c, "cpu", false, 0.0);
    check(c.source == GpuTimeSource::None && std::strcmp(line, "90 fps   gpu -- ms   cpu -- ms") == 0,
          "overlay: clock off with no span prints '--'");
    // Clock on keeps today's behaviour: the application mean when ready and sampled, else '--' (the work span is never read).
    c = edvr::chooseGpuTime(true, true, true, 12.34, true, 4.0);
    check(c.source == GpuTimeSource::ApplicationRender && std::fabs(c.ms - 12.34) < 1e-9, "overlay: clock on and ready shows the application render mean");
    c = edvr::chooseGpuTime(true, false, true, 12.34, true, 4.0);
    check(c.source == GpuTimeSource::None, "overlay: clock on but the snapshot not ready is '--', not the span");
    c = edvr::chooseGpuTime(true, true, false, 0.0, true, 4.0);
    check(c.source == GpuTimeSource::None, "overlay: clock on with no application samples is '--', not the span");
    edvr::overlayLineText(line, sizeof(line), 72.0, edvr::chooseGpuTime(true, true, true, 9.96, false, 0.0), "cpu (pre-submit)", true, 6.0);
    check(std::strcmp(line, "72 fps   gpu 10.0 ms   cpu (pre-submit) 6.0 ms") == 0, "overlay: clock on prints the application mean and the stand-in label");
    // A short buffer truncates, never overruns.
    char tiny[8];
    edvr::overlayLineText(tiny, sizeof(tiny), 90.0, c, "cpu", true, 1.0);
    check(std::strlen(tiny) < sizeof(tiny), "overlay: a short line buffer is truncated, not overrun");
    // The census field the log parser reads (tools\edvr_log.py --gpu-work-span): its exact text.
    char field[160];
    edvr::formatFrameWorkSpanField(field, sizeof(field), 0, 0, 0, 0.0);
    check(std::strcmp(field, "frame work span: completed 0, skipped 0, invalid 0, last - ms") == 0, "census field: nothing completed prints 'last -'");
    edvr::formatFrameWorkSpanField(field, sizeof(field), 5400, 3, 2, 4.2567);
    check(std::strcmp(field, "frame work span: completed 5400, skipped 3, invalid 2, last 4.257 ms") == 0, "census field: counts and the last pair in ms");
}

// ---- Cut E: classifier_memo.h -------------------------------------------------------------------------------------------------------

// A hit needs something stored, both halves of the key equal AND the stored exit to be a plain decline; every other call is a miss.
void testMemoHit() {
    using namespace edvr::classifiermemo;
    Memo m;
    const Key k{5, 7};
    check(!hit(m, k), "memo: nothing stored is a miss");
    check(!hit(m, Key{0, 0}), "memo: nothing stored is a miss even for the zero key");
    store(m, k, Exit::kPlainDecline);
    check(hit(m, k), "memo: both halves equal after a plain decline is a hit");
    check(!hit(m, Key{6, 7}), "memo: a moved generation is a miss");
    check(!hit(m, Key{5, 8}), "memo: a moved epoch is a miss");
    check(!hit(m, Key{6, 8}), "memo: both moved is a miss");
    check(!hit(m, Key{4, 7}) && !hit(m, Key{5, 6}), "memo: a key that moved backwards is a miss as well, never a hit");
    check(!hit(m, Key{0, 0}), "memo: the zero key does not match a stored one");
    // The stored exit decides what an equal key means.
    store(m, k, Exit::kOther);
    check(!hit(m, k), "memo: an equal key after an exit that was not a plain decline is a miss");
    check(!hit(m, Key{6, 7}) && !hit(m, Key{5, 8}), "memo: a moved key after a non-plain exit is a miss as well");
    store(m, k, Exit::kPlainDecline);
    check(hit(m, k), "memo: storing the plain decline again restores the hit");
    store(m, Key{9, 9}, Exit::kPlainDecline);
    check(!hit(m, k) && hit(m, Key{9, 9}), "memo: a later store replaces the earlier key");
    invalidate(m);
    check(!hit(m, Key{9, 9}) && !m.stored, "memo: invalidate forgets the answer, so the same key misses");
    // Both classifiers keep a memo of their own: storing in one leaves the other alone.
    Memo eye, holo;
    store(eye, k, Exit::kPlainDecline);
    check(!hit(holo, k) && hit(eye, k), "memo: two memos are independent");
    // Over a grid of keys against one stored key, exactly the stored key after a plain decline hits.
    unsigned hits = 0;
    for (uint32_t g = 3; g < 8; ++g) {
        for (uint32_t e = 3; e < 8; ++e) {
            for (int plain = 0; plain < 2; ++plain) {
                Memo c;
                store(c, Key{5, 5}, plain ? Exit::kPlainDecline : Exit::kOther);
                if (hit(c, Key{g, e})) ++hits;
            }
        }
    }
    check(hits == 1, "memo: over a grid of keys and exits exactly one cell is a hit");
}

// A model of a run of draws through the memo, as uiDepthOnEyeDraw drives it: key, hit test, and on a miss the full run's exit stored.
void testMemoRun() {
    using namespace edvr::classifiermemo;
    Memo m;
    unsigned fullRuns = 0;
    const auto call = [&](Key now, Exit exitIfRun) {
        if (hit(m, now)) return true;
        ++fullRuns;
        store(m, now, exitIfRun);
        return false;
    };
    const Key a{10, 3}, b{11, 3}, c{11, 4};
    check(!call(a, Exit::kPlainDecline), "run: the first call finds nothing stored and runs in full");
    check(call(a, Exit::kPlainDecline) && call(a, Exit::kPlainDecline), "run: the repeats hit");
    check(!call(b, Exit::kOther), "run: a draw whose bindings moved runs in full, and its exit is not a plain decline");
    check(!call(b, Exit::kOther), "run: its repeat finds the key but not a plain decline, so it runs in full again");
    check(!call(a, Exit::kPlainDecline), "run: back to the plain draw, the generation has moved");
    check(call(a, Exit::kPlainDecline), "run: ...and it hits again");
    check(!call(c, Exit::kPlainDecline), "run: a bump of both runs in full");
    check(!call(Key{11, 5}, Exit::kPlainDecline), "run: an epoch move alone runs in full");
    check(fullRuns == 6, "run: of nine calls three hit and six ran in full");
}

void usage() {
    std::puts("usage: aa_off_test --self-test | --dry-run");
}

}  // namespace

// Cut F: the may-want byte against the subscribers' own predicates.
void testVsMayWant() {
    using namespace edvr;
    check(vsMayWantByte(0) == kVsMayWantAll, "may want: hash 0 (a shader EDVR never hashed) wants every subscriber");
    // The shipped shaders: each subscriber's vertex shader keeps its own bit set.
    for (const ScreenMotionPair& p : kScreenMotionUiPairs) {
        check((vsMayWantByte(p.vs) & kVsMayScreenUi) != 0, "may want: each screen motion UI vertex shader keeps its bit");
        check(screenMotionUiPair(p.vs, p.ps), "may want: the pair table answers its own rows");
    }
    check((vsMayWantByte(kScreenMotionRecognizePair.vs) & kVsMayScreenRecognize) != 0, "may want: the recognised screen's vertex shader keeps its bit");
    for (uint64_t vs : kWeaponMotionFamilyVs) check((vsMayWantByte(vs) & kVsMayWeaponMotion) != 0, "may want: each weapon motion vertex shader keeps its bit");
    check((vsMayWantByte(kUiVsPanel) & kVsMayUiFamily) != 0 && (vsMayWantByte(kUiVsLoader) & kVsMayUiFamily) != 0,
          "may want: the panel and loader vertex shaders always run the family check (its probe records them)");
    check((vsMayWantByte(kUiVsScopeMark) & kVsMayUiFamily) != 0, "may want: the scope mark's vertex shader always runs the family check (runtime arming)");
    check((vsMayWantByte(kUiVsOnFootHudA) & kVsMayUiFamily) != 0 && (vsMayWantByte(kUiVsOnFootHudB) & kVsMayUiFamily) != 0,
          "may want: the on-foot HUD's vertex shaders always run the family check (runtime on-foot state)");
    // Random shaders: a clear bit implies the subscriber's predicate rejects the vertex shader whatever the pixel shader.
    uint64_t x = 0x9E3779B97F4A7C15ull;
    auto next = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    unsigned clear[4] = {0, 0, 0, 0}, wrong = 0;
    for (unsigned i = 0; i < 20000; ++i) {
        const uint64_t vs = next(), ps = next();
        const uint8_t m = vsMayWantByte(vs);
        if (!(m & kVsMayScreenUi)) { ++clear[0]; wrong += screenMotionUiPair(vs, ps); }
        if (!(m & kVsMayScreenRecognize)) { ++clear[1]; wrong += screenMotionRecognizePair(vs, ps); }
        if (!(m & kVsMayWeaponMotion)) { ++clear[2]; wrong += weaponMotionFamilyHash(vs); }
        if (!(m & kVsMayUiFamily)) ++clear[3];
    }
    check(wrong == 0, "may want: a clear bit never hides a vertex shader the subscriber's predicate accepts (random shaders)");
    check(clear[0] == 20000 && clear[1] == 20000 && clear[2] == 20000 && clear[3] == 20000, "may want: an unknown shader clears every bit");
    // One-token mutants of the shipped shaders: the bit is a function of the vertex shader alone, so a neighbour must not inherit it.
    unsigned inherited = 0;
    for (const ScreenMotionPair& p : kScreenMotionUiPairs)
        for (int bit = 0; bit < 64; ++bit) inherited += ((vsMayWantByte(p.vs ^ (1ull << bit)) & kVsMayScreenUi) != 0) != screenMotionUiPairVs(p.vs ^ (1ull << bit));
    check(inherited == 0, "may want: the bit tracks the predicate exactly (mutants of the screen motion shaders)");
    // The skip helpers read one bit each.
    check(!vsMaySkip(kVsMayWantAll, kVsMayScreenUi) && vsMaySkip(0, kVsMayScreenUi),
          "may want: a bit set never skips, a bit clear always does");
}

// Cut H: a draw's own flags (draw_call_flags.h).
void testDrawCallFlags() {
    using namespace edvr;
    DrawCallFlags none;
    check(!none.uiDepth && !none.holoDepth && !none.composite && !none.planetPending && !none.worldScreenPending, "flags: a fresh DrawCallFlags is all false (what every early return of beginPanelOverride leaves)");
    check(!drawCallOwnsPending(none), "flags: a draw that set no pending flag owns none (a foreign-context draw never clears the module's)");
    DrawCallFlags planet; planet.planetPending = true;
    DrawCallFlags screen; screen.worldScreenPending = true;
    DrawCallFlags depth; depth.uiDepth = true; depth.holoDepth = true; depth.composite = true;
    check(drawCallOwnsPending(planet) && drawCallOwnsPending(screen) && !drawCallOwnsPending(depth), "flags: only a planet or world-screen pending flag makes a draw the owner of the pending clear");
    DrawCallFlags all; all.uiDepth = all.holoDepth = all.composite = all.planetPending = all.worldScreenPending = true;
    // An ordinary owner draw with every flag set: every second issue is asked for.
    DrawReissues r = drawReissuesFor(all, true, false);
    check(r.uiDepthCoverage && r.hologram && r.planet && r.worldScreen, "reissues: an owner draw, not layered, issues the coverage, the hologram passes, the planet and the world screen");
    // Layered: the take is not in the eye's colour, so the coverage, the hologram passes and the world screen are skipped; the planet coverage has no layered test.
    r = drawReissuesFor(all, true, true);
    check(!r.uiDepthCoverage && !r.hologram && r.planet && !r.worldScreen, "reissues: a layered draw skips the coverage, the hologram passes and the world screen, and keeps the planet's (as before)");
    // A foreign-context draw: its flags are false, so nothing is issued on that context (#184's first stale consumer).
    r = drawReissuesFor(none, false, false);
    check(!r.uiDepthCoverage && !r.hologram && !r.planet && !r.worldScreen, "reissues: a draw with no flags of its own issues nothing (a foreign context never takes another draw's)");
    // The planet and the world screen need the owner.
    r = drawReissuesFor(all, false, false);
    check(r.uiDepthCoverage && r.hologram && !r.planet && !r.worldScreen, "reissues: the planet and world-screen coverage need the owner context");
    // Every combination of the five flags, the owner and layered: a second issue never appears without its own flag.
    unsigned wrong = 0;
    for (unsigned bits = 0; bits < 128; ++bits) {
        DrawCallFlags f;
        f.uiDepth = bits & 1; f.holoDepth = bits & 2; f.composite = bits & 4; f.planetPending = bits & 8; f.worldScreenPending = bits & 16;
        const bool owner = bits & 32, layered = bits & 64;
        const DrawReissues x = drawReissuesFor(f, owner, layered);
        wrong += (x.uiDepthCoverage && !f.uiDepth) + (x.hologram && !f.holoDepth) + (x.planet && !(f.planetPending && owner)) + (x.worldScreen && !(f.worldScreenPending && owner && !layered));
        wrong += (f.uiDepth && !layered && !x.uiDepthCoverage) + (f.holoDepth && !layered && !x.hologram) + (f.planetPending && owner && !x.planet);
    }
    check(wrong == 0, "reissues: over all 128 flag, owner and layered combinations a second issue is made exactly when its own flag and its conditions hold");
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("aa_off_test: dry run, nothing written");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        usage();
        return 2;
    }
    testFastPathTable();
    testMarkerFixBind();
    testMarkerFixWiring();
    testBindShadows();
    testThunkModel();
    testBindToggle();
    testSampler();
    testMapTotals();
    testGpuGuard();
    testGpuTimeTile();
    testFrameWorkSlack();
    testOverlayLine();
    testMemoHit();
    testMemoRun();
    testVsMayWant();
    testDrawCallFlags();
    std::printf("aa_off_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
