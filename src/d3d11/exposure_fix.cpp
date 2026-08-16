#include "exposure_fix.h"

#include <windows.h>

#include <cstdlib>
#include <string>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/vtable_hook.h"
#include "binding_shadow.h"

namespace edvr {
namespace {

// ID3D11DeviceContext vtable indices.
//
// A frozen COM ABI: IUnknown occupies 0-2, ID3D11DeviceChild 3-6, and the
// ID3D11DeviceContext methods follow in d3d11.h declaration order. These are
// documented rather than guessed, and are still range-checked against the
// probed vtable before use.
constexpr size_t kSlotDispatch             = 41;
constexpr size_t kSlotCSSetUAVs            = 68;
constexpr size_t kSlotCSSetShader          = 69;
// Drops every binding without naming any of them, which is why the shadows
// above have to be told about it.
constexpr size_t kSlotClearState           = 110;
constexpr size_t kHighestSlotUsed          = 110;

typedef void(STDMETHODCALLTYPE* PFN_SetShader)(ID3D11DeviceContext*, void*,
                                               ID3D11ClassInstance* const*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D11DeviceContext*, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_CSSetUAVs)(ID3D11DeviceContext*, UINT, UINT,
                                               ID3D11UnorderedAccessView* const*,
                                               const UINT*);
typedef void(STDMETHODCALLTYPE* PFN_ClearState)(ID3D11DeviceContext*);

struct State {
    VTableHook    hook;
    // The context these hooks were installed for. Identity only -- compared,
    // never dereferenced. In-place vtable patching hooks the class, so
    // deferred contexts and a wrapper mod's internal ones reach our thunks
    // too and must pass straight through. See vtable_hook.h.
    ID3D11DeviceContext* ownerCtx = nullptr;
    PFN_SetShader realCSSetShader = nullptr;
    PFN_Dispatch  realDispatch = nullptr;
    PFN_CSSetUAVs realCSSetUAVs = nullptr;
    PFN_ClearState realClearState = nullptr;

    // The bound shader and UAVs live in binding_shadow, shared with vscreen.
    // They used to live here, with the opposite policy: this file nulled the
    // pointers every frame while vscreen kept them, so an engine that filters a
    // redundant re-bind would have left this fix reading nullptr for the rest of
    // the session -- silently inert, with the give-up notice blaming the game
    // for being stock. One module, one policy: keep the pointers, expire the
    // answers.

    bool     enabled = false;
    uint64_t targetHash = 0;      // pinned by config, or learned by detection
    bool     pinned = false;      // true if the hash came from config
    uint32_t copyMask = 0xF;
    bool     copyBtoA = false;

    // Shape detection.
    //
    // A bytecode hash identifies one compiled shader and changes whenever the
    // game's shaders are rebuilt, so pinning one means the fix breaks on every
    // update until somebody re-derives it. The pass's SHAPE is far more stable:
    // it writes a small structured buffer of exposure state and a tiny
    // parameter texture, and it runs once per eye. Detecting that costs one
    // evaluation per distinct compute shader and then nothing.
    std::unordered_map<uint64_t, bool> shapeVerdict;
    // Hashes the shape test has ever run on. A counter cannot do this job: the
    // prune below removes negatives every frame, so the map "forgets" a shader
    // and the next frame's probe counts it again -- the give-up notice, which
    // calls itself the thing to report, would print tens of thousands where it
    // means a handful. This set is never pruned; it holds one 64-bit hash per
    // distinct compute shader the game creates.
    std::unordered_set<uint64_t> everExamined;
    uint32_t detectStreak = 0;    // consecutive frames the candidate ran twice
    bool     announced = false;
    uint64_t frames = 0;
    bool     gaveUpNotice = false;

    uint32_t seenThisFrame = 0;
    // Whether the game did ANY compute work this frame. The give-up notice
    // counts these frames rather than all frames -- see exposureFixFrameBoundary.
    bool     computeThisFrame = false;
    ID3D11UnorderedAccessView* firstEye[4] = {nullptr, nullptr, nullptr, nullptr};
    uint64_t applied = 0;
    bool     rejected = false;

    std::unordered_map<void*, uint64_t> shaderHashes;
    CRITICAL_SECTION lock{};
    bool lockReady = false;
};

// Consecutive frames a detected candidate must run exactly twice before the
// fix acts on it.
constexpr uint32_t kConfirmFrames = 5;

// Frames to wait before reporting that detection found nothing. Long enough to
// cover menus and loading, where the pass legitimately does not run.
constexpr uint64_t kGiveUpFrames = 5000;

State* g_state = nullptr;
FaultBudget g_budget("exposureFix", 5);

BindSlot uavSlot(uint32_t i) {
    return static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::CsUav0) + i);
}

uint64_t hashOf(void* shader) {
    if (!shader || !g_state || !g_state->lockReady) return 0;
    uint64_t out = 0;
    EnterCriticalSection(&g_state->lock);
    auto it = g_state->shaderHashes.find(shader);
    if (it != g_state->shaderHashes.end()) out = it->second;
    LeaveCriticalSection(&g_state->lock);
    return out;
}

// Two resources may only be copied if they are the same kind and size. The two
// eyes' equivalents always are; anything else means the configured shader hash
// no longer identifies what it did when it was verified, and the copy is
// skipped rather than applied to an unrelated resource.
bool copyCompatible(ID3D11Resource* a, ID3D11Resource* b) {
    if (!a || !b || a == b) return false;
    D3D11_RESOURCE_DIMENSION da = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    D3D11_RESOURCE_DIMENSION db = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    a->GetType(&da);
    b->GetType(&db);
    if (da != db) return false;

    if (da == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC x{}, y{};
        static_cast<ID3D11Buffer*>(a)->GetDesc(&x);
        static_cast<ID3D11Buffer*>(b)->GetDesc(&y);
        return x.ByteWidth == y.ByteWidth && x.StructureByteStride == y.StructureByteStride;
    }
    if (da == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC x{}, y{};
        static_cast<ID3D11Texture2D*>(a)->GetDesc(&x);
        static_cast<ID3D11Texture2D*>(b)->GetDesc(&y);
        return x.Width == y.Width && x.Height == y.Height && x.Format == y.Format &&
               x.MipLevels == y.MipLevels && x.ArraySize == y.ArraySize;
    }
    return false;
}

void shareExposure(ID3D11DeviceContext* ctx, ID3D11UnorderedAccessView* const* first,
                   ID3D11UnorderedAccessView* const* second) {
    State* s = g_state;
    uint32_t copied = 0, skipped = 0;

    for (uint32_t slot = 0; slot < 4; ++slot) {
        if ((s->copyMask & (1u << slot)) == 0) continue;
        if (!first[slot] || !second[slot]) continue;

        ID3D11Resource* a = nullptr;
        ID3D11Resource* b = nullptr;
        first[slot]->GetResource(&a);
        second[slot]->GetResource(&b);

        if (copyCompatible(a, b)) {
            if (s->copyBtoA) ctx->CopyResource(a, b);
            else             ctx->CopyResource(b, a);
            ++copied;
        } else {
            ++skipped;
        }
        if (a) a->Release();
        if (b) b->Release();
    }

    if (++s->applied == 1) {
        if (copied == 0) {
            s->rejected = true;
            Log::get().note("exposure fix DISABLED: no compatible resource pairs "
                            "(mask 0x%X, %u skipped). The configured shader is not the "
                            "exposure pass on this game build.", s->copyMask, skipped);
        } else {
            Log::get().note("exposure fix ACTIVE: sharing %u slot(s) %s each frame",
                            copied,
                            s->copyBtoA ? "second eye -> first" : "first eye -> second");
        }
    }
}

// Does the bound UAV set look like per-eye exposure state?
//
// Slot 0 is a small structured buffer holding the luminance range; slot 1 is a
// tiny texture holding the tonemap parameters the rest of the frame reads. Both
// are unusual enough that nothing else in the frame matches, and neither depends
// on the shader's bytecode, so this survives the game being rebuilt.
bool shapeLooksLikeExposure() {
    // Slot 0 is a small structured buffer holding the luminance range; slot 1 is
    // a tiny texture holding the tonemap parameters the rest of the frame reads.
    // Both are unusual enough that nothing else in the frame matches, and
    // neither depends on the shader's bytecode, so this survives a rebuild.
    //
    // Resolved through binding_shadow, which owns the guard, the budget and the
    // GetType-first rule. A view that cannot be resolved -- because it is no
    // longer live -- reads as "not the exposure pass", which is the safe answer.
    ResourceInfo buf;
    if (!bindingResolve(bindingGet(uavSlot(0)), &buf) || !buf.isBuffer) return false;
    if (buf.a == 0 || buf.a > 256) return false;

    ResourceInfo strip;
    if (!bindingResolve(bindingGet(uavSlot(1)), &strip) || !strip.isTexture2D) return false;
    // A parameter strip: a few texels, one row.
    if (strip.b != 1 || strip.a == 0 || strip.a > 64) return false;

    return true;
}

// Is this call for the context we installed on? In-place vtable patching
// hooks every object of the class, so a deferred context or a wrapper mod's
// internal one lands here too and must leave untouched.
inline bool foreignContext(ID3D11DeviceContext* self) {
    return self != g_state->ownerCtx;
}

void STDMETHODCALLTYPE hookedCSSetShader(ID3D11DeviceContext* self, void* shader,
                                         ID3D11ClassInstance* const* inst, UINT n) {
    if (foreignContext(self)) {
        g_state->realCSSetShader(self, shader, inst, n);
        return;
    }
    bindingSet(BindSlot::Cs, shader);
    g_state->realCSSetShader(self, shader, inst, n);
}

void STDMETHODCALLTYPE hookedCSSetUAVs(ID3D11DeviceContext* self, UINT start, UINT n,
                                       ID3D11UnorderedAccessView* const* uavs,
                                       const UINT* counts) {
    if (foreignContext(self)) {
        g_state->realCSSetUAVs(self, start, n, uavs, counts);
        return;
    }
    for (UINT i = 0; i < n && uavs; ++i) {
        const UINT slot = start + i;
        if (slot < 4) {
            bindingSet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::CsUav0) + slot),
                       uavs[i]);
        }
    }
    g_state->realCSSetUAVs(self, start, n, uavs, counts);
}

// Everything is unbound, so forget what we thought was bound.
//
// These shadows were written in the two hooks above and never cleared -- not at
// the frame boundary, not anywhere -- while ClearState was not hooked at all. So
// after the game cleared state and released those views, curUav still named
// them and the next unseen compute shader ran the shape probe over freed
// memory. vscreen.cpp hit the same thing and hooks this for the same reason.
void STDMETHODCALLTYPE hookedClearState(ID3D11DeviceContext* self) {
    if (foreignContext(self)) {
        g_state->realClearState(self);
        return;
    }
    bindingForgetAll();
    g_state->realClearState(self);
}

// Is this dispatch the exposure pass? Pinned hash if configured, otherwise
// shape detection, cached per shader so the cost is one evaluation each.
bool isExposureDispatch() {
    State* s = g_state;
    if (!s->enabled || s->rejected) return false;

    const uint64_t h = hashOf(bindingGet(BindSlot::Cs));
    if (h == 0) return false;
    if (s->targetHash != 0) return h == s->targetHash;
    if (s->pinned) return false;   // pinned but not matching: do nothing

    auto it = s->shapeVerdict.find(h);
    if (it != s->shapeVerdict.end()) return it->second;

    const bool match = shapeLooksLikeExposure();
    s->everExamined.insert(h);
    s->shapeVerdict[h] = match;
    if (match) {
        Log::get().note("exposure fix: candidate compute shader %016llX matches the "
                        "exposure-state shape; confirming across frames",
                        static_cast<unsigned long long>(h));
    }
    return match;
}

void STDMETHODCALLTYPE hookedDispatch(ID3D11DeviceContext* self, UINT x, UINT y, UINT z) {
    State* s = g_state;
    if (foreignContext(self)) {
        s->realDispatch(self, x, y, z);
        return;
    }

    // Classification runs INSIDE the guard.
    //
    // It was called here, bare, one line above the guarded region it feeds.
    // isExposureDispatch reaches shapeLooksLikeExposure, which makes COM calls
    // through the curUav shadow -- and that shadow is only as fresh as the last
    // CSSetUnorderedAccessViews we saw. After a ClearState (now hooked below,
    // but a command list can still do it) those pointers can name released
    // views, and the probe would run on them with no SEH at all. The budget is
    // the same one the copy uses: if we cannot classify, we cannot act, so
    // there is nothing to keep alive separately.
    bool isTarget = false;
    guardedBudget(g_budget, [&] { isTarget = isExposureDispatch(); });

    // Any compute work at all means the game is rendering a scene, which is the
    // only condition under which the exposure pass could appear. Menus and
    // loading screens do not count -- see the frame counter at the boundary.
    s->computeThisFrame = true;

    s->realDispatch(self, x, y, z);
    if (!isTarget) return;

    guardedBudget(g_budget, [&] {
        ++s->seenThisFrame;
        if (s->seenThisFrame == 1) {
            for (uint32_t i = 0; i < 4; ++i) {
                s->firstEye[i] = static_cast<ID3D11UnorderedAccessView*>(
                    bindingGet(uavSlot(i)));
            }
        } else if (s->seenThisFrame == 2) {
            // Running exactly twice a frame is the other half of the signature:
            // once per eye. A shader that merely has the right resource shape
            // but runs once, or five times, is something else. Detection waits
            // for a few consecutive frames of that before touching anything;
            // a pinned hash is trusted immediately.
            if (!s->pinned && s->detectStreak < kConfirmFrames) return;

            ID3D11UnorderedAccessView* second[4];
            for (uint32_t i = 0; i < 4; ++i) {
                second[i] = static_cast<ID3D11UnorderedAccessView*>(bindingGet(uavSlot(i)));
            }
            if (!s->announced) {
                s->announced = true;
                // The key is advanced.exposure_shader. It said fix.b1_exposure_cs,
                // which is this repo's predecessor's name for it and is read by
                // nothing here -- so anyone following the instruction was
                // silently ignored, on the support path where it matters most.
                Log::get().note("exposure fix: confirmed compute shader %016llX runs "
                                "once per eye. Pin it with exposure_shader under "
                                "[advanced] in edvr.ini if you want to skip detection.",
                                static_cast<unsigned long long>(hashOf(bindingGet(BindSlot::Cs))));
            }
            shareExposure(self, s->firstEye, second);
        }
    });
}

}  // namespace

void registerShaderHash(void* shader, uint64_t hash) {
    if (!g_state || !shader || !g_state->lockReady) return;
    EnterCriticalSection(&g_state->lock);
    g_state->shaderHashes[shader] = hash;
    LeaveCriticalSection(&g_state->lock);
}

void exposureFixFrameBoundary() {
    State* s = g_state;
    if (!s) return;
    // Exactly two dispatches means one per eye. Anything else breaks the streak,
    // so a shader that only sometimes runs twice never gets promoted.
    if (s->seenThisFrame == 2) {
        if (s->detectStreak < kConfirmFrames) ++s->detectStreak;
    } else if (s->seenThisFrame != 0) {
        s->detectStreak = 0;
    }
    s->seenThisFrame = 0;
    for (uint32_t i = 0; i < 4; ++i) s->firstEye[i] = nullptr;

    // Forget what was bound, once a frame.
    //
    // ClearState is hooked now, but it is not the only way these go stale: a
    // command list replayed onto this context resets the bindings without
    // passing through any hook we own. This bounds that to a frame, which is
    // what vscreen.cpp settled on for the same reason. It costs one re-probe
    // per shader per frame while detection is still running, and nothing
    // afterwards.

    // Drop the NO answers while detection is still looking.
    //
    // shapeVerdict was written once per shader and never revisited, so the real
    // exposure pass being probed once in a transient binding state -- the first
    // dispatch after a clear, say -- blacklisted it for the whole session. The
    // fix then never engaged, and the give-up notice went on to report that the
    // game is stock, which is a different and wrong thing.
    //
    // Yes answers are kept: those are confirmed across frames anyway, and a
    // shader that matched the shape once does not stop having matched it.
    if (!s->announced && !s->gaveUpNotice && s->targetHash == 0) {
        for (auto it = s->shapeVerdict.begin(); it != s->shapeVerdict.end();) {
            it = it->second ? std::next(it) : s->shapeVerdict.erase(it);
        }
    }

    // Say so when detection comes up empty. Otherwise a build where the shape
    // stopped matching produces a log identical to one where the user never got
    // into VR, and there is no way to tell those apart from a bug report.
    // Count frames in which the game did compute work, NOT frames since launch.
    //
    // Counting every frame fired this notice during a loading screen: 5000
    // frames went by in three seconds, and the target was found 68 ms later. The
    // log then read "NOT ENGAGED ... the game is stock" directly above the line
    // announcing detection -- exactly the thing that produces a bug report about
    // a fix that is working.
    //
    // A frame with no compute work is a frame in which the exposure pass could
    // not have run, so it is not evidence of anything.
    if (s->computeThisFrame) ++s->frames;
    s->computeThisFrame = false;
    if (!s->announced && !s->gaveUpNotice && s->frames >= kGiveUpFrames) {
        s->gaveUpNotice = true;
        Log::get().note(
            "exposure fix: NOT ENGAGED after %llu frames -- no compute pass matched "
            "the exposure shape (%zu distinct compute shaders examined). Nothing has "
            "been touched and the game is stock. If this is a VR session at a bright "
            "star and the eyes still differ, the pass has changed shape and the fix "
            "needs updating; this log is the thing to report.",
            static_cast<unsigned long long>(s->frames),
            s->everExamined.size());
    }
}

void toggleExposureFix() {
    State* s = g_state;
    // Deliberately does NOT require a pinned hash. It used to, from when one was
    // mandatory, and making detection the default silently disabled the toggle:
    // with nothing pinned, targetHash is zero and this returned immediately.
    if (!s || !s->hook.committed()) return;

    s->enabled = !s->enabled;
    if (s->enabled) s->rejected = false;
    Log::get().note("exposure fix toggled %s (applied %llu times so far, target %s)",
                    s->enabled ? "ON" : "OFF",
                    static_cast<unsigned long long>(s->applied),
                    s->pinned ? "pinned" : (s->announced ? "detected" : "not yet found"));
}

bool exposureFixEnabled() { return g_state && g_state->enabled; }

void installExposureFix(ID3D11Device* device) {
    if (!device || g_state) return;

    Config& cfg = Config::get();

    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx) return;

    g_state = new State();
    InitializeCriticalSection(&g_state->lock);
    g_state->lockReady = true;
    // An empty hash means "find it yourself", which is the default and the
    // reason this survives a game update.
    const std::string hashText = cfg.getString("advanced.exposure_shader", "");
    if (!hashText.empty()) {
        g_state->targetHash = strtoull(hashText.c_str(), nullptr, 16);
        g_state->pinned = g_state->targetHash != 0;
    }
    g_state->enabled = cfg.getBool("fix.share_exposure", true);
    // Not settings. All four of the pass's outputs have to be shared -- sharing
    // only the first was tried and did nothing visible, because the value the
    // tonemap actually reads is in another one. Direction was measured too:
    // first eye to second keeps the scene bright, the reverse dims everything.
    g_state->copyMask = 0xF;
    g_state->copyBtoA = false;

    State& s = *g_state;
    s.ownerCtx = ctx;
    if (!s.hook.attach(ctx) || s.hook.executablePrefix() <= kHighestSlotUsed) {
        Log::get().note("exposure fix: context vtable unusable; not installing");
        s.hook.uninstall();
        ctx->Release();
        // Delete and null, as vscreen does on its own failure paths. Leaving it
        // set meant exposureFixFrameBoundary ran all session for a fix that was
        // never installed, and at 5000 frames announced "NOT ENGAGED ... the
        // game is stock" -- a report about a fix that had never been there.
        //
        // The critical section is initialised above this point, so it has to go
        // back before the object does.
        if (g_state->lockReady) {
            DeleteCriticalSection(&g_state->lock);
            g_state->lockReady = false;
        }
        delete g_state;
        g_state = nullptr;
        return;
    }

    s.hook.replace(kSlotCSSetShader, &hookedCSSetShader,
                   reinterpret_cast<void**>(&s.realCSSetShader));
    s.hook.replace(kSlotCSSetUAVs, &hookedCSSetUAVs,
                   reinterpret_cast<void**>(&s.realCSSetUAVs));
    s.hook.replace(kSlotDispatch, &hookedDispatch,
                   reinterpret_cast<void**>(&s.realDispatch));
    s.hook.replace(kSlotClearState, &hookedClearState,
                   reinterpret_cast<void**>(&s.realClearState));

    if (!s.hook.commit()) {
        Log::get().note("exposure fix: vtable commit failed; not installing");
        s.hook.uninstall();
        ctx->Release();
        // Delete and null, as vscreen does on its own failure paths. Leaving it
        // set meant exposureFixFrameBoundary ran all session for a fix that was
        // never installed, and at 5000 frames announced "NOT ENGAGED ... the
        // game is stock" -- a report about a fix that had never been there.
        //
        // The critical section is initialised above this point, so it has to go
        // back before the object does.
        if (g_state->lockReady) {
            DeleteCriticalSection(&g_state->lock);
            g_state->lockReady = false;
        }
        delete g_state;
        g_state = nullptr;
        return;
    }

    Log::get().note("exposure fix installed on %p (%zu methods), currently %s, "
                    "target %s",
                    static_cast<void*>(ctx), s.hook.executablePrefix(),
                    s.enabled ? "ON" : "off",
                    s.pinned ? "pinned by config" : "detected automatically");
    ctx->Release();
}

void shutdownExposureFix() {
    if (!g_state) return;
    g_state->enabled = false;
    g_state->hook.uninstall();
    if (g_state->lockReady) {
        DeleteCriticalSection(&g_state->lock);
        g_state->lockReady = false;
    }
}

}  // namespace edvr

