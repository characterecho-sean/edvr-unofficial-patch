#include "exposure_fix.h"

#include <windows.h>

#include <cstdlib>
#include <string>
#include <unordered_map>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/vtable_hook.h"

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
constexpr size_t kHighestSlotUsed          = 69;

typedef void(STDMETHODCALLTYPE* PFN_SetShader)(ID3D11DeviceContext*, void*,
                                               ID3D11ClassInstance* const*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D11DeviceContext*, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_CSSetUAVs)(ID3D11DeviceContext*, UINT, UINT,
                                               ID3D11UnorderedAccessView* const*,
                                               const UINT*);

struct State {
    VTableHook    hook;
    PFN_SetShader realCSSetShader = nullptr;
    PFN_Dispatch  realDispatch = nullptr;
    PFN_CSSetUAVs realCSSetUAVs = nullptr;

    // Shadow state, so a dispatch knows what was bound before it.
    void* curCS = nullptr;
    void* curUav[4] = {nullptr, nullptr, nullptr, nullptr};

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
bool shapeLooksLikeExposure(void* const* uavs) {
    auto sizeOf = [](void* view, bool wantBuffer, uint32_t* a, uint32_t* b) -> bool {
        if (!view) return false;
        ID3D11Resource* res = nullptr;
        static_cast<ID3D11UnorderedAccessView*>(view)->GetResource(&res);
        if (!res) return false;

        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        bool ok = false;
        if (wantBuffer && dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d{};
            static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
            *a = d.ByteWidth;
            *b = d.StructureByteStride;
            ok = true;
        } else if (!wantBuffer && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d{};
            static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
            *a = d.Width;
            *b = d.Height;
            ok = true;
        }
        res->Release();
        return ok;
    };

    uint32_t bytes = 0, stride = 0;
    if (!sizeOf(uavs[0], true, &bytes, &stride)) return false;
    // A handful of floats of state. Bounded rather than exact, so a build that
    // adds a field to it still matches.
    if (bytes == 0 || bytes > 64) return false;

    uint32_t w = 0, h = 0;
    if (!sizeOf(uavs[1], false, &w, &h)) return false;
    // A parameter strip: a few texels, one row.
    if (h != 1 || w == 0 || w > 64) return false;

    return true;
}

void STDMETHODCALLTYPE hookedCSSetShader(ID3D11DeviceContext* self, void* shader,
                                         ID3D11ClassInstance* const* inst, UINT n) {
    g_state->curCS = shader;
    g_state->realCSSetShader(self, shader, inst, n);
}

void STDMETHODCALLTYPE hookedCSSetUAVs(ID3D11DeviceContext* self, UINT start, UINT n,
                                       ID3D11UnorderedAccessView* const* uavs,
                                       const UINT* counts) {
    State* s = g_state;
    for (UINT i = 0; i < n && uavs; ++i) {
        const UINT slot = start + i;
        if (slot < 4) s->curUav[slot] = uavs[i];
    }
    s->realCSSetUAVs(self, start, n, uavs, counts);
}

// Is this dispatch the exposure pass? Pinned hash if configured, otherwise
// shape detection, cached per shader so the cost is one evaluation each.
bool isExposureDispatch() {
    State* s = g_state;
    if (!s->enabled || s->rejected) return false;

    const uint64_t h = hashOf(s->curCS);
    if (h == 0) return false;
    if (s->targetHash != 0) return h == s->targetHash;
    if (s->pinned) return false;   // pinned but not matching: do nothing

    auto it = s->shapeVerdict.find(h);
    if (it != s->shapeVerdict.end()) return it->second;

    const bool match = shapeLooksLikeExposure(s->curUav);
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
    const bool isTarget = isExposureDispatch();

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
                s->firstEye[i] = static_cast<ID3D11UnorderedAccessView*>(s->curUav[i]);
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
                second[i] = static_cast<ID3D11UnorderedAccessView*>(s->curUav[i]);
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
                                static_cast<unsigned long long>(hashOf(s->curCS)));
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
            static_cast<unsigned long long>(s->frames), s->shapeVerdict.size());
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
    if (!s.hook.attach(ctx) || s.hook.executablePrefix() <= kHighestSlotUsed) {
        Log::get().note("exposure fix: context vtable unusable; not installing");
        s.hook.uninstall();
        ctx->Release();
        return;
    }

    s.hook.replace(kSlotCSSetShader, &hookedCSSetShader,
                   reinterpret_cast<void**>(&s.realCSSetShader));
    s.hook.replace(kSlotCSSetUAVs, &hookedCSSetUAVs,
                   reinterpret_cast<void**>(&s.realCSSetUAVs));
    s.hook.replace(kSlotDispatch, &hookedDispatch,
                   reinterpret_cast<void**>(&s.realDispatch));

    if (!s.hook.commit()) {
        Log::get().note("exposure fix: vtable commit failed; not installing");
        s.hook.uninstall();
        ctx->Release();
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

