#include "fss_reveal.h"

#include <windows.h>

#include <d3d11_1.h>

#include <cstdarg>
#include <cstdio>

#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "fss_reveal_core.h"
#include "fss_reveal_replay.h"

// See fss_reveal.h and fss_reveal_core.h for the story. What this file adds is the log, in three layers, so that a flight log says which of "the code never ran", "it ran and had nothing to do"
// and "it ran and replayed" happened:
//   1. once a process, "fss reveal fix: armed" (or "not armed: flat profile");
//   2. once a scanner session, the summary: DrawInstanced calls checked, draws of the reveal shaders seen, composites, replays, declines by reason, the state read-back result, and a verdict;
//   3. a few lines per session for the first replays and the first oddities.

namespace edvr {

namespace detail {
bool g_fssRevealWatch = false;
}  // namespace detail

namespace {

using namespace fssreveal;

constexpr uint32_t kSessionGapFrames = 30;   // frames off show before a session is over
constexpr uint32_t kMaxDetailLines = 6;      // per-event lines in one session
constexpr uint32_t kStateChecks = 8;         // replays whose context state is read back and compared

struct State {
    FaultBudget   budget{"fssReveal", 5};
    RevealReplay  rep;
    DrawState     rec[RevealReplay::kRec];
    CbPool        pool[RevealReplay::kRec];
    ID3D11DeviceContext1* ctx1 = nullptr;
    bool          ctx1Tried = false;
    bool          armedNoted = false;
    bool          flat = false;
    // the scanner session
    bool          inSession = false;
    uint32_t      sessionNo = 0, sessionStart = 0, offFrames = 0;
    uint32_t      drawsChecked = 0, drawsOfShaders = 0, detailLines = 0;
};

State g;

void ensureCtx1(ID3D11DeviceContext* ctx) {
    if (!g.ctx1 && !g.ctx1Tried) {
        g.ctx1Tried = true;
        ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g.ctx1));
    }
}

void releaseResources() {
    for (auto& r : g.rec) r.release();
    for (auto& p : g.pool) p.release();
    if (g.ctx1) { g.ctx1->Release(); g.ctx1 = nullptr; }
    g.ctx1Tried = false;
}

void noteDetail(const char* fmt, ...) {
    if (g.detailLines >= kMaxDetailLines) return;
    ++g.detailLines;
    char buf[600];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Log::get().note("%s", buf);
}

void endSession(uint32_t frameNo) {
    if (!g.inSession) return;
    g.inSession = false;
    const RevealReplay& r = g.rep;
    Log::get().note("fss reveal fix: scanner session %u (frames %u-%u) -- DrawInstanced calls checked %u, draws of the reveal shaders %u; frames with a reveal composite %u, with a replay %u (%u replay(s), %u failed); "
                    "state read back after the first replays: %u equal, %u DIFFERENT; downsamples recorded %u (%u dropped, %u failed); composites declined, in frames: no downsample recorded %u, recorded for another "
                    "texture than t1 (hash-only match) %u, t1 unresolved %u, flat %u. %s.",
                    g.sessionNo, g.sessionStart, frameNo, g.drawsChecked, g.drawsOfShaders, r.framesWithComposite, r.framesReplayed, r.replays, r.replayFailures, r.verifiedOk, r.verifiedBad, r.records,
                    r.recordDrops, r.recordFailures, r.framesDeclined[RevealReplay::DeclNoRecord], r.framesDeclined[RevealReplay::DeclOtherTarget], r.framesDeclined[RevealReplay::DeclT1],
                    r.framesDeclined[RevealReplay::DeclFlat], sessionVerdict(r, g.drawsChecked));
}

}  // namespace

void fssRevealConfigure(bool flat) {
    g.flat = flat;
    if (g.armedNoted) return;
    g.armedNoted = true;
    if (flat) Log::get().note("fss reveal fix: not armed (the flat profile has one camera; the black cells there are the game's own scan art).");
    else Log::get().note("fss reveal fix: armed. While the scanner is on show, the first downsample of the ring layer each frame is replayed before the first reveal composite; a summary is written per scanner session.");
}

void fssRevealOnDrawSlow(ID3D11DeviceContext* ctx, const FssRealCalls& rc, uint32_t perInstance, uint32_t instances, uint32_t startVertex, uint32_t startInstance) {
    guardedBudget(g.budget, [&] {
        ++g.drawsChecked;
        const uint64_t vs = bindingShaderHash(BindSlot::Vs);
        if (vs != kCompVs && vs != kDownVs) return;
        const uint64_t ps = bindingShaderHash(BindSlot::Ps);
        if (vs == kDownVs && ps == kDownPs) {
            // a downsample: keep the state it is issued with (constant buffers by copy), keyed by its render target
            ++g.drawsOfShaders;
            ResourceInfo ri;
            void* rtv = bindingGet(BindSlot::Rtv0);
            const void* rt = (rtv && bindingResolveProbe(rtv, &ri)) ? ri.resource : nullptr;
            const int idx = g.rep.record(rt);
            if (idx < 0) return;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            ensureCtx1(ctx);
            bool ok = false;
            if (dev) {
                ok = captureState(dev, ctx, g.ctx1, &g.pool[idx], g.rec[idx]);
                dev->Release();
            }
            if (ok) {
                g.rec[idx].perInstance = perInstance;
                g.rec[idx].instances = instances;
                g.rec[idx].startVertex = startVertex;
                g.rec[idx].startInstance = startInstance;
            } else {
                g.rep.recordFailed(idx);
                noteDetail("fss reveal fix: the state of a downsample draw could not be kept; it will not be replayed.");
            }
            return;
        }
        if (vs == kCompVs && ps == kCompPs) {
            if (g.rep.compSeen) return;   // only the first composite of the frame decides
            ++g.drawsOfShaders;
            ResourceInfo ri;
            void* v = bindingGet(BindSlot::PsSrv1);
            const void* t1 = (v && bindingResolveProbe(v, &ri)) ? ri.resource : nullptr;
            int idx = -1;
            const RevealReplay::Decision d = g.rep.composite(g.flat, t1, &idx);
            if (d != RevealReplay::Replay) {
                if (d != RevealReplay::DeclDone) noteDetail("fss reveal fix: the first reveal composite of a frame was %s.", decisionText(d));
                return;
            }
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            ensureCtx1(ctx);
            ReplayCalls calls;
            calls.vsSetCb = reinterpret_cast<decltype(calls.vsSetCb)>(rc.vsSetCb);
            calls.psSetCb = reinterpret_cast<decltype(calls.psSetCb)>(rc.psSetCb);
            calls.psSetSrv = reinterpret_cast<decltype(calls.psSetSrv)>(rc.psSetSrv);
            calls.vsSetSrv = reinterpret_cast<decltype(calls.vsSetSrv)>(rc.vsSetSrv);
            calls.vsSetShader = reinterpret_cast<decltype(calls.vsSetShader)>(rc.vsSetShader);
            calls.psSetShader = reinterpret_cast<decltype(calls.psSetShader)>(rc.psSetShader);
            calls.omSetRts = reinterpret_cast<decltype(calls.omSetRts)>(rc.omSetRts);
            calls.omSetBlend = reinterpret_cast<decltype(calls.omSetBlend)>(rc.omSetBlend);
            calls.rsSetViewports = reinterpret_cast<decltype(calls.rsSetViewports)>(rc.rsSetViewports);
            calls.drawInstanced = reinterpret_cast<decltype(calls.drawInstanced)>(rc.drawInstanced);
            bool verified = false;
            const bool check = g.rep.replays < kStateChecks;   // the first replays of a session read the whole state back and compare it with what was bound before
            const bool done = dev && replayDraw(dev, ctx, g.ctx1, calls, g.rec[idx], check ? &verified : nullptr);
            if (dev) dev->Release();
            if (done) {
                ++g.rep.replays;
                if (check) { if (verified) ++g.rep.verifiedOk; else ++g.rep.verifiedBad; }
                noteDetail("fss reveal fix: the frame's first downsample into the composite's t1 (%ux%u fmt%u) was re-run before the first reveal composite; %s.", ri.a, ri.b, ri.fmt,
                       check ? (verified ? "the context state read back afterwards equals the state before" : "THE CONTEXT STATE READ BACK AFTERWARDS DIFFERS FROM THE STATE BEFORE") : "state check not repeated");
            } else {
                ++g.rep.replayFailures;
                noteDetail("fss reveal fix: the replay could not be done (no device or no state); nothing was drawn.");
            }
        }
    });
}

void fssRevealFrameBoundary(bool onShow, bool flat, uint32_t frameNo) {
    g.flat = flat;
    // the frame that just ended
    g.rep.endFrame();
    for (auto& r : g.rec) r.release();
    const bool watch = onShow && !flat;
    if (watch) {
        if (!g.inSession) {
            g.inSession = true;
            ++g.sessionNo;
            g.sessionStart = frameNo;
            g.rep.resetTotals();
            g.drawsChecked = g.drawsOfShaders = g.detailLines = 0;
        }
        g.offFrames = 0;
    } else if (g.inSession && ++g.offFrames >= kSessionGapFrames) {
        endSession(frameNo);
        releaseResources();   // the private constant-buffer copies and the context interface are rebuilt on demand
    }
    detail::g_fssRevealWatch = watch;
}

void fssRevealShutdown() {
    endSession(0);
    detail::g_fssRevealWatch = false;
    releaseResources();
}

}  // namespace edvr
