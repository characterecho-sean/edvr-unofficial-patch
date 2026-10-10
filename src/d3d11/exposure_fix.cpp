#include "exposure_fix.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "../common/vtable_hook.h"
#include "binding_shadow.h"
#include "device_hook.h"  // contextHookModeFor
#include "draw_census.h"  // drawCensusDispatch: the census records compute
#include "engine_velocity.h"  // F2: the palette chain's dispatch feeds the second skin's join
#include "skin_join.h"        // kChainHash
#include "flat_runtime.h"
#include "flat_temporal.h"  // flat discovery and capture-only dispatch forwarding
#include "object_probe.h"   // objectProbeNoteDispatch: the skin ledger's view of the palette chain (armed eye runs only)
#include "vr_world_route.h"  // g_vrWorldInternal: the VR world route's own dispatches pass straight through
#include "../common/runtime_profile.h"
#include "../common/plugin_cost.h"
#include "gpu_frame_timing.h"
                          // writers through THIS module's Dispatch hook,
                          // because slot 41 is already ours and a second
                          // patch on it would be a second thing to reclaim
#include "sunglare_fix.h"  // sunglareLastSeenMs, the damper's sun scope
#include "../plugins/exposure/exposure_shape.h"

namespace edvr {
namespace {

// Direct-context API sites owned by the exposure damper. Keep these outside
// the NV and CockpitVisuals ranges in the shared fixed coverage mask.
enum class ExposureApiSite : uint16_t {
    DampGetDevice = 96,
    DampCopyReadback = 97,
    DampMapReadback = 98,
    DampUnmapReadback = 99,
    DampWriteFirstEye = 100,
    DampWriteSecondEye = 101,
};

constexpr uint16_t exposureApiSite(ExposureApiSite site) noexcept {
    return static_cast<uint16_t>(site);
}
static_assert(exposureApiSite(ExposureApiSite::DampGetDevice) == 96 &&
              exposureApiSite(ExposureApiSite::DampWriteSecondEye) == 101 &&
              exposureApiSite(ExposureApiSite::DampWriteSecondEye) <
                  EDVR_PLUGIN_COST_MAX_SITE_ID &&
              exposureApiSite(ExposureApiSite::DampGetDevice) <
                  exposureApiSite(ExposureApiSite::DampCopyReadback) &&
              exposureApiSite(ExposureApiSite::DampCopyReadback) <
                  exposureApiSite(ExposureApiSite::DampMapReadback) &&
              exposureApiSite(ExposureApiSite::DampMapReadback) <
                  exposureApiSite(ExposureApiSite::DampUnmapReadback) &&
              exposureApiSite(ExposureApiSite::DampUnmapReadback) <
                  exposureApiSite(ExposureApiSite::DampWriteFirstEye) &&
              exposureApiSite(ExposureApiSite::DampWriteFirstEye) <
                  exposureApiSite(ExposureApiSite::DampWriteSecondEye),
              "Exposure API site IDs are stable and unique.");

// ID3D11DeviceContext vtable indices.
//
// A frozen COM ABI: IUnknown occupies 0-2, ID3D11DeviceChild 3-6, and the
// ID3D11DeviceContext methods follow in d3d11.h declaration order. These are
// documented rather than guessed, and are still range-checked against the
// probed vtable before use.
constexpr size_t kSlotDispatch             = 41;
constexpr size_t kSlotDispatchIndirect     = 42;
constexpr size_t kSlotCSSetUAVs            = 68;
constexpr size_t kSlotCSSetShader          = 69;
// Drops every binding without naming any of them, which is why the shadows
// above have to be told about it.
constexpr size_t kSlotClearState           = 110;
constexpr size_t kHighestSlotUsed          = 110;

// The slots the reclaim pass may vouch for, and the evidence that is allowed
// to earn it -- which is NOT vscreen's evidence, and the difference is the
// point. vscreen's slots fire every presented frame, so silence-while-
// presenting proves bypass there. These three are COMPUTE: this game runs
// whole loading screens with Present at four figures and not one dispatch
// (the give-up machinery below was rebuilt around exactly that measurement),
// so silence-while-presenting proves nothing here, and a chainer that
// composed politely during play would be adopted during the first hyperspace
// jump -- the call loop, deferred rather than prevented. What CAN prove
// bypass is silence while EYES ARE BEING DRAWN: the exposure pass is how
// those eyes get tonemapped, and scene compute does not idle while scene
// draws flow. So the quiet streak only advances on passes where vscreen
// counted eye draws (plumbed through from its reclaim pass), and there is
// deliberately no everHit precondition: with scene evidence in the gate, a
// slot bypassed BEFORE its first dispatch -- the field timing, the toolkit
// re-points at XR session init -- is still healable, where an everHit test
// would have written it off for the session.
// ClearState is absent: shared with vscreen, refused by reclaim on its own
// grounds, and quiet for whole legitimate sessions besides.
enum ReclaimHit : uint32_t {
    kHitDispatch = 0,   // 41
    kHitCsUavs,         // 68
    kHitCsShader,       // 69
    kHitCount
};
constexpr size_t kReclaimableSlots[kHitCount] = {kSlotDispatch, kSlotCSSetUAVs,
                                                 kSlotCSSetShader};
constexpr uint8_t kQuietPassesToVouch = 3;

typedef void(STDMETHODCALLTYPE* PFN_SetShader)(ID3D11DeviceContext*, void*,
                                               ID3D11ClassInstance* const*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D11DeviceContext*, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DispatchIndirect)(ID3D11DeviceContext*,
                                                      ID3D11Buffer*, UINT);
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
    PFN_DispatchIndirect realDispatchIndirect = nullptr;
    PFN_CSSetUAVs realCSSetUAVs = nullptr;
    PFN_ClearState realClearState = nullptr;

    // The bound shader and UAVs live in binding_shadow, shared with vscreen.
    // They used to live here, with the opposite policy: this file nulled the
    // pointers every frame while vscreen kept them, so an engine that filters a
    // redundant re-bind would have left this fix reading nullptr for the rest of
    // the session -- silently inert, with the give-up notice blaming the game
    // for being stock. One module, one policy: keep the pointers, expire the
    // answers.

    // Per-slot proof the hooked thunks are being CALLED, for the reclaim
    // pass. Incremented before the foreign-context test, because raw
    // invocation is the evidence, and a chainer forwarding the game's calls
    // keeps them climbing -- which is exactly what makes its slot unsafe to
    // take back. No everHit here, on purpose: the scene gate in
    // exposureFixReclaimHooks replaces it, and does the one thing it could
    // not -- heal a slot that was bypassed before its first call.
    uint32_t thunkHits[kHitCount] = {};
    uint8_t  quietPasses[kHitCount] = {};

    bool     enabled = false;
    uint64_t targetHash = 0;      // pinned by config, or learned by detection
    bool     pinned = false;      // true if the hash came from config
    uint32_t copyMask = 0xF;
    bool     copyBtoA = false;

    // The dispatch-skip probe (advanced.census_skip_dispatch): compute
    // shaders named by content hash are NOT forwarded while the spec is set.
    // The census_skip idea completed -- draws could be probed by hash since
    // the geyser hunt, but the FSS black-square stack turned out to be built
    // by COMPUTE (a per-eye 16x16-tile uint mask), and a system with no
    // draw to skip needs its dispatches skippable to be localised live.
    // Empty is off and the only shipped state; each firing is counted and
    // the first is said aloud.
    uint64_t dispatchSkip[4] = {};
    uint8_t  dispatchSkipOcc[4] = {};     // 0 = every occurrence; N = only the
                                          // Nth per frame ("HASH:2" = the
                                          // second eye's dispatch alone)
    uint8_t  dispatchOccSeen[4] = {};     // per-frame occurrence counters
    uint32_t dispatchSkipCount = 0;
    uint64_t dispatchSkipped = 0;
    bool     dispatchSkipNoted = false;
    char     dispatchSkipSpec[96] = {};   // raw spec, to log only on change

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

    // The damper. The exposure peek's sweep (the retired measurement
    // instrument, removed 2026-09-23) decoded the 8-byte state buffer: float
    // [0] a constant luminance floor (-9.9658 in every sample), float [1]
    // the adaptation value in log2 stops -- and a four-stop swing under
    // an ordinary head pitch at a star, because the metering runs on the
    // head-tracked view and the simulated pupils land on top of the real
    // ones. Each frame the freshly computed value is read back (an 8-byte
    // ping-pong copy, one frame of lag a slow signal never notices), a
    // slow mean tracks where it lives, and mean + (1-k)(v - mean) is
    // written over the game's state for both eyes. The game's own
    // adaptation loop then runs FROM the damped value -- swing compressed
    // by k, genuine scene changes still drifting the mean over seconds.
    float          dampK = 0;             // experimental.exposure_damping, 0..1
    float          dampTau = 45.0f;       // experimental.exposure_damping_tau, secs
    ID3D11Texture2D* dampStaging[2] = {}; // owned; strip ping-pong pair
    int            dampCur = 0;
    bool           dampPrevValid = false;
    bool           dampHaveMean = false;
    float          dampMean[6] = {};      // per-texel slow means
    uint64_t       dampSeedMs = 0;        // when the means were (re)seeded
    uint64_t       dampDevSinceMs = 0;    // 0 = gain currently inside band
    uint64_t       dampSnaps = 0;
    void*          dampLastStrip = nullptr;  // identity only, never
                                             // dereferenced: the strip
                                             // must be the SAME texture
                                             // object, settled for
                                             // kDampSettleMs, before the
                                             // damper acts
    uint64_t       dampStableSinceMs = 0;
    uint64_t       dampStepMs = 0;        // wall clock of the last step --
                                          // the blend is dt/tau, so the
                                          // mean's speed survives
                                          // reprojection halving the rate
    uint64_t       dampWrites = 0;
    uint64_t       dampWritesAtNote = 0;
    uint64_t       dampLastNoteMs = 0;
};

// Consecutive frames a detected candidate must run exactly twice before the
// fix acts on it.
constexpr uint32_t kConfirmFrames = 5;

// The damper's constants. The strip as measured 2026-08-21: 6x1, R32
// float, texels [raw luminance, smoothed luminance, gain, gain again,
// curve, direct-sun term]. Texels 0-4 are damped; texel 5 passes raw --
// it is the sun-occlusion intensity the glare cards read, and holding it
// would leave glare shining through cockpit struts. The state-buffer
// damper this replaces measured beta of ~1: the game re-derives its
// state within a frame, so only the strip -- pure output, read by the
// tonemaps at frame end -- can hold the image.
constexpr uint32_t kStripW = 6;
constexpr uint32_t kStripFmtA = 39;  // R32_TYPELESS
constexpr uint32_t kStripFmtB = 41;  // R32_FLOAT
constexpr uint32_t kDampRawTexel = 5;
constexpr uint32_t kDampGainTexel = 2;

// The transient-versus-sustained discriminator. A head pose swings the
// gain briefly and returns; a real scene change -- the menu hangar on
// launch, a station slot, a jump -- moves it far and KEEPS it there. A
// gain more than a quarter away from the mean continuously for a second
// and a half snaps the means to reality; the launch that taught this
// held the hangar blown out for most of a minute, because the mean had
// seeded from the game's arbitrary pre-adapted first frame and tau is
// deliberately glacial. A fast-blend window right after seeding covers
// the same first seconds.
constexpr float    kSnapDeviation = 0.25f;
constexpr uint64_t kSnapAfterMs = 1500;
constexpr uint64_t kFastSeedMs = 3000;
constexpr float    kFastSeedBoost = 10.0f;

// The menu lesson (Q3 launch, 2026-08-21): menus run MULTIPLE passes of
// the exposure shape, so "the second dispatch's strip" is not reliably
// an eye there -- the damper read a zero-gain UI strip and faithfully
// wrote zero gain over the real eyes, which IS the blowout it was
// blamed for. Every measured eye gain sits far above this floor; a
// reading at or below it is some other instance, and the damper stands
// aside rather than propagate it.
constexpr float    kDampGainFloor = 0.5f;

// How long the strip's identity must hold before the damper's FIRST
// write. Two frames was not enough: the Q3 menu's pass sequence is
// QUASI-stable -- the same strip for a handful of frames, then a
// shuffle -- so intermittent writes slipped through as a left-eye
// flicker. Gameplay holds one identity for hours; a menu shuffle never
// survives two seconds.
constexpr uint64_t kDampSettleMs = 2000;

// The sun scope: the damper acts only while the glare train has drawn
// within this window. The breathing it exists for happens AT a star;
// with no sun around, stock adaptation is the correct behaviour, and
// menus never see the damper at all -- which retires the whole family
// of pass-instance ambiguities the menu kept teaching, one flicker at
// a time.
constexpr uint64_t kDampSunWindowMs = 5000;

// Frames to wait before reporting that detection found nothing. Long enough to
// cover menus and loading, where the pass legitimately does not run.
constexpr uint64_t kGiveUpFrames = 5000;

State* g_state = nullptr;
FaultBudget g_budget("exposureFix", 5);

BindSlot uavSlot(uint32_t i) {
    return static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::CsUav0) + i);
}

#if !defined(EDVR_EXPOSURE_DAMP_TEST)
uint64_t hashOf(void* shader) {
    return lookupShaderHash(shader);
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

// One damping step, run right after the second eye's dispatch with the
// pass's UAVs still bound and share_exposure's copy already landed:
// queue this frame's strip readback, consume last frame's, filter, and
// write the damped strip over both eyes' copies -- after the passes,
// before the tonemaps at frame end that read it.
#endif  // !EDVR_EXPOSURE_DAMP_TEST

void exposureDamp(ID3D11DeviceContext* ctx,
                  ID3D11UnorderedAccessView* firstEyeStrip) {
    State* s = g_state;

    // The sun scope, checked first: no glare train in the last few
    // seconds means no sun worth holding for, and the damper does not
    // touch anything. The pending readback is dropped rather than kept
    // -- resuming later re-settles from scratch.
    const uint64_t seen = sunglareLastSeenMs();
    if (seen == 0 || nowMs() - seen > kDampSunWindowMs) {
        s->dampPrevValid = false;
        s->dampLastStrip = nullptr;
        return;
    }

    ID3D11UnorderedAccessView* view =
        static_cast<ID3D11UnorderedAccessView*>(bindingGet(uavSlot(1)));
    if (!view || !firstEyeStrip) return;
    ID3D11Resource* resB = nullptr;
    view->GetResource(&resB);
    if (!resB) return;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resB->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        resB->Release();
        return;
    }
    D3D11_TEXTURE2D_DESC td{};
    static_cast<ID3D11Texture2D*>(resB)->GetDesc(&td);
    const uint32_t fmt = static_cast<uint32_t>(td.Format);
    if (td.Width != kStripW || td.Height != 1 ||
        (fmt != kStripFmtA && fmt != kStripFmtB)) {
        resB->Release();
        return;   // not the measured strip; stand aside entirely
    }

    // Identity gate: act only when this frame's strip is the SAME texture
    // object it has been for kDampSettleMs. Gameplay holds one identity
    // for hours and settles once; a menu shuffling several same-shaped
    // instances -- even quasi-stably, which is what got past the
    // two-frame version of this gate as a left-eye flicker -- never
    // survives the settle. The pending readback dies with any change; it
    // was copied from whatever the previous instance was.
    const uint64_t nowGate = nowMs();
    if (resB != s->dampLastStrip) {
        s->dampLastStrip = resB;
        s->dampStableSinceMs = nowGate;
        s->dampPrevValid = false;
        resB->Release();
        return;
    }
    if (nowGate - s->dampStableSinceMs < kDampSettleMs) {
        s->dampPrevValid = false;
        resB->Release();
        return;
    }

    // Sample only after the damper's early declines. The false path adds one
    // existing owner/context/frame check per eligible invocation and no D3D
    // queries, clocks, or allocations.
    const bool costSample = edvrPluginCostApiSampleContext(ctx) != 0;
    if (!s->dampStaging[0]) {
        ID3D11Device* dev = nullptr;
        if (costSample) {
            edvrPluginCostNoteD3dCall(
                static_cast<uint8_t>(plugin_cost::Owner::Exposure),
                exposureApiSite(ExposureApiSite::DampGetDevice),
                static_cast<uint8_t>(plugin_cost::ApiClass::ReadQuery));
        }
        ctx->GetDevice(&dev);
        if (!dev) {
            resB->Release();
            return;
        }
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        dev->CreateTexture2D(&sd, nullptr, &s->dampStaging[0]);
        dev->CreateTexture2D(&sd, nullptr, &s->dampStaging[1]);
        dev->Release();
        if (!s->dampStaging[0] || !s->dampStaging[1]) {
            resB->Release();
            return;
        }
    }

    ID3D11Resource* resA = nullptr;
    firstEyeStrip->GetResource(&resA);

    // Queue this frame's readback FIRST, before the damped write below
    // lands on the same texture -- the staging captures the game's own
    // freshly derived parameters, so the filter runs on the measurement
    // and never chews its own output.
    const int prev = s->dampCur ^ 1;
    if (costSample) {
        edvrPluginCostNoteD3dCall(
            static_cast<uint8_t>(plugin_cost::Owner::Exposure),
            exposureApiSite(ExposureApiSite::DampCopyReadback),
            static_cast<uint8_t>(plugin_cost::ApiClass::Transfer));
    }
    ctx->CopyResource(s->dampStaging[s->dampCur], resB);
    s->dampCur ^= 1;

    if (s->dampPrevValid) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (costSample) {
            edvrPluginCostNoteD3dCall(
                static_cast<uint8_t>(plugin_cost::Owner::Exposure),
                exposureApiSite(ExposureApiSite::DampMapReadback),
                static_cast<uint8_t>(plugin_cost::ApiClass::ReadQuery));
        }
        if (SUCCEEDED(ctx->Map(s->dampStaging[prev], 0, D3D11_MAP_READ, 0,
                               &m)) &&
            m.pData) {
            float raw[kStripW];
            memcpy(raw, m.pData, sizeof(raw));
            if (costSample) {
                edvrPluginCostNoteD3dCall(
                    static_cast<uint8_t>(plugin_cost::Owner::Exposure),
                    exposureApiSite(ExposureApiSite::DampUnmapReadback),
                    static_cast<uint8_t>(plugin_cost::ApiClass::Transfer));
            }
            ctx->Unmap(s->dampStaging[prev], 0);

            bool sane = raw[kDampGainTexel] > kDampGainFloor;
            for (uint32_t i = 0; i < kStripW; ++i) {
                if (!(raw[i] >= -1e6f && raw[i] <= 1e6f)) sane = false;
            }
            if (sane) {
                const uint64_t now = nowMs();
                if (!s->dampHaveMean) {
                    memcpy(s->dampMean, raw, sizeof(raw));
                    s->dampHaveMean = true;
                    s->dampStepMs = now;
                    s->dampSeedMs = now;
                    s->dampDevSinceMs = 0;
                }
                // The scene-change snap: gain far from the mean and
                // STAYING far means the scene itself moved, and holding
                // the old mean would hold the wrong brightness -- the
                // launch hangar stayed blown out until this existed.
                const float dev =
                    fabsf(raw[kDampGainTexel] - s->dampMean[kDampGainTexel]);
                const float ref = fabsf(s->dampMean[kDampGainTexel]);
                if (dev > kSnapDeviation * (ref > 1.0f ? ref : 1.0f)) {
                    if (s->dampDevSinceMs == 0) s->dampDevSinceMs = now;
                    if (now - s->dampDevSinceMs >= kSnapAfterMs) {
                        memcpy(s->dampMean, raw, sizeof(raw));
                        s->dampSeedMs = now;
                        s->dampDevSinceMs = 0;
                        ++s->dampSnaps;
                        Log::get().note("exposure damping: scene change -- "
                                        "means snapped to the new scene "
                                        "(gain %.1f, snap %llu).",
                                        raw[kDampGainTexel],
                                        static_cast<unsigned long long>(
                                            s->dampSnaps));
                    }
                } else {
                    s->dampDevSinceMs = 0;
                }
                // The mean's blend is wall-clock over tau -- a
                // few-second constant matched a held head pose and the
                // mean chased every pitch, which was the first field
                // trial's leak. Right after a seed or a snap the blend
                // runs boosted, so the first seconds of a new scene
                // settle at stock-like speed.
                float alpha =
                    static_cast<float>(now - s->dampStepMs) / 1000.0f /
                    s->dampTau;
                if (now - s->dampSeedMs < kFastSeedMs) {
                    alpha *= kFastSeedBoost;
                }
                if (alpha > 0.2f) alpha = 0.2f;
                s->dampStepMs = now;

                float out[kStripW];
                for (uint32_t i = 0; i < kStripW; ++i) {
                    s->dampMean[i] += alpha * (raw[i] - s->dampMean[i]);
                    out[i] = i == kDampRawTexel
                                 ? raw[i]
                                 : s->dampMean[i] +
                                       (1.0f - s->dampK) *
                                           (raw[i] - s->dampMean[i]);
                }
                const UINT pitch = kStripW * 4;
                if (resA) {
                    if (costSample) {
                        edvrPluginCostNoteD3dCall(
                            static_cast<uint8_t>(plugin_cost::Owner::Exposure),
                            exposureApiSite(ExposureApiSite::DampWriteFirstEye),
                            static_cast<uint8_t>(plugin_cost::ApiClass::Transfer));
                    }
                    ctx->UpdateSubresource(resA, 0, nullptr, out, pitch, 0);
                }
                if (resB != resA) {
                    if (costSample) {
                        edvrPluginCostNoteD3dCall(
                            static_cast<uint8_t>(plugin_cost::Owner::Exposure),
                            exposureApiSite(ExposureApiSite::DampWriteSecondEye),
                            static_cast<uint8_t>(plugin_cost::ApiClass::Transfer));
                    }
                    ctx->UpdateSubresource(resB, 0, nullptr, out, pitch, 0);
                }
                ++s->dampWrites;
                if (s->dampWrites == 1 ||
                    now - s->dampLastNoteMs >= 5000) {
                    Log::get().note(
                        "exposure damping: %llu write(s) since last note, "
                        "k=%.2f, gain %.1f, gain mean %.1f.",
                        static_cast<unsigned long long>(
                            s->dampWrites - s->dampWritesAtNote),
                        s->dampK, raw[2], s->dampMean[2]);
                    s->dampLastNoteMs = now;
                    s->dampWritesAtNote = s->dampWrites;
                }
            }
        }
    }

    s->dampPrevValid = true;
    if (resA) resA->Release();
    resB->Release();
}

#if !defined(EDVR_EXPOSURE_DAMP_TEST)
// Is this call for the context we installed on? In-place vtable patching
// hooks every object of the class, so a deferred context or a wrapper mod's
// internal one lands here too and must leave untouched.
inline bool foreignContext(ID3D11DeviceContext* self) {
    return self != g_state->ownerCtx;
}

void STDMETHODCALLTYPE hookedCSSetShader(ID3D11DeviceContext* self, void* shader,
                                         ID3D11ClassInstance* const* inst, UINT n) {
    if (g_flatComputeInternal) { g_state->realCSSetShader(self, shader, inst, n); return; }
    ++g_state->thunkHits[kHitCsShader];
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
    if (g_flatComputeInternal) { g_state->realCSSetUAVs(self, start, n, uavs, counts); return; }
    ++g_state->thunkHits[kHitCsUavs];
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
    if (flatRuntimeActive()) flatRuntimeUavs(start, n, uavs);
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
    // The VR world route's resolver isolates the context (SwapDeviceContextState, ClearState) around its own work: the game's
    // bindings are not forgotten by that (vscreen's hook on this slot steps aside for the same scope, and this one must not
    // run first and wipe the shadow).
    if (g_vrWorldInternal) { g_state->realClearState(self); return; }
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

    const bool match = plugins::exposure::shapeLooksLikeExposure();
    s->everExamined.insert(h);
    s->shapeVerdict[h] = match;
    if (match) {
        Log::get().note("exposure fix: candidate compute shader %016llX matches the "
                        "exposure-state shape; confirming across frames",
                        static_cast<unsigned long long>(h));
    }
    return match;
}

// Record-only: the census names GPU-driven compute (group counts live in
// the argument buffer, so n= cannot be known CPU-side), and everything else
// -- skips, syncs, the exposure pass itself -- stays Dispatch-only. Hooked
// at all because round sixteen of the black squares proved a reconstruction
// chain ran passes no census line ever showed, and DispatchIndirect was one
// of the three ways that could be true.
void STDMETHODCALLTYPE hookedDispatchIndirect(ID3D11DeviceContext* self,
                                               ID3D11Buffer* args, UINT off) {
    if (runtimeFlatProfile()) {
        if (self == g_state->ownerCtx && flatTemporalCapturing()) flatTemporalDispatch(self, 0, 0, 0, args, off);
        FlatRuntimeDispatchScope flatDispatch(self);
        g_state->realDispatchIndirect(self, args, off);
        return;
    }
    if (g_vrWorldInternal) { g_state->realDispatchIndirect(self, args, off); return; }   // the world route's own (vr_world_route.h)
    gpuFrameCommand(self);
    State* s = g_state;
    if (drawCensusArmed()) {
        drawCensusDispatch(self, 0, 0, 0, foreignContext(self), args, off);
    }
    if (!foreignContext(self)) s->computeThisFrame = true;

    s->realDispatchIndirect(self, args, off);
}

// F2: is the bound compute shader the skinning palette chain (skin_join.h kChainHash)? Asked of every owner dispatch while the second skin
// is wanted, so the verdict is kept by shader object AND by the registry's generation (skinjoin::ChainVerdicts): an address a registration has since
// re-used is asked again, and a steady dispatch is a map hit and an atomic load, never the registry's lock. Owner context only.
bool skinChainBound() {
    void* cs = bindingGet(BindSlot::Cs);
    if (!cs) return false;
    static skinjoin::ChainVerdicts verdicts;
    return verdicts.bound(cs, shaderRegistryGeneration(), [](const void* shader) { return hashOf(const_cast<void*>(shader)); });
}

void STDMETHODCALLTYPE hookedDispatch(ID3D11DeviceContext* self, UINT x, UINT y, UINT z) {
    if (runtimeFlatProfile()) {
        ++g_state->thunkHits[kHitDispatch];
        if (self == g_state->ownerCtx && flatTemporalCapturing()) flatTemporalDispatch(self, x, y, z);
        FlatRuntimeDispatchScope flatDispatch(self);
        g_state->realDispatch(self, x, y, z);
        return;
    }
    if (g_vrWorldInternal) { g_state->realDispatch(self, x, y, z); return; }   // the world route's own: not the game's exposure pass
    gpuFrameCommand(self);
    State* s = g_state;
    ++s->thunkHits[kHitDispatch];
    if (g_vrWorldWatchWrites && !foreignContext(self)) vrWorldRouteNoteDispatch();   // a UAV write into H after the resolve is the latch's
    if (foreignContext(self)) {
        // Recorded, then passed straight through. Deferred contexts reach
        // this thunk (in-place patching hooks the class), and until round
        // sixteen of the black squares they were passed through UNRECORDED
        // -- which is exactly where that hunt's invisible reconstruction
        // middle could hide. Every read the census makes is off `self`, so
        // the record is honest for any context; only the fixes and probes
        // below stay owner-only.
        if (drawCensusArmed()) drawCensusDispatch(self, x, y, z, true, nullptr, 0);
        if (objectProbeLedgerActive()) objectProbeNoteDispatch(self, x, y, z, true);   // the skin ledger: one bool unarmed
        s->realDispatch(self, x, y, z);
        return;
    }

    // The census's view of compute, recorded before the forward the way the
    // draw hooks record before theirs, so the q= ordinals across draws,
    // copies and dispatches share one timeline. One bool call per dispatch
    // when no census runs, which is almost always -- the same bargain every
    // other census hook strikes. Recording, not classification: this line
    // exists because the FSS body could legally be built by a compute writer
    // and no capture before 2026-08-25 could have seen it.
    if (drawCensusArmed()) drawCensusDispatch(self, x, y, z, false, nullptr, 0);
    // The skin ledger (skin_ledger.h) reads the palette chain's inputs here, before the game's dispatch runs: an
    // armed eye run only, one bool load otherwise, and what it copies is the game's own state, untouched.
    if (objectProbeLedgerActive()) objectProbeNoteDispatch(self, x, y, z, false);
    // F2 (the second skin, VR only): the palette chain's dispatch feeds the identity join before the game's dispatch runs. One atomic load
    // when the feature is off; the shader test is a lookup kept by shader object.
    if (engineVelocitySkinWanted() && skinChainBound()) engineVelocityNoteChainDispatch(self, x);

    // The dispatch-skip probe, after the census record (a census taken
    // while probing must record what the game SUBMITTED -- the draw skips'
    // rule) and before anything else acts. A skipped dispatch still proves
    // the game is rendering a scene, so the flag is set on the way out.
    if (s->dispatchSkipCount) {
        const uint64_t h = hashOf(bindingGet(BindSlot::Cs));
        for (uint32_t i = 0; i < s->dispatchSkipCount; ++i) {
            if (s->dispatchSkip[i] != h) continue;
            const uint8_t seen = ++s->dispatchOccSeen[i];
            if (s->dispatchSkipOcc[i] == 0 || s->dispatchSkipOcc[i] == seen) {
                ++s->dispatchSkipped;
                s->computeThisFrame = true;
                if (!s->dispatchSkipNoted) {
                    s->dispatchSkipNoted = true;
                    Log::get().note(
                        "dispatch skip: first hit -- ch=%016llX occurrence "
                        "%u not forwarded; counting silently from here.",
                        static_cast<unsigned long long>(h), seen);
                }
                return;
            }
            break;   // matched hash, untargeted occurrence: forward normally
        }
    }

    // Classification runs INSIDE the guard.
    //
    // It was called here, bare, one line above the guarded region it feeds.
    // isExposureDispatch reaches the exposure plugin's shape classifier, which
    // makes COM calls
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
                                "[advanced] in %s if you want to skip detection.",
                                static_cast<unsigned long long>(hashOf(bindingGet(BindSlot::Cs))),
                                Config::get().iniName());
            }
            shareExposure(self, s->firstEye, second);
            if (s->dampK > 0.0f) exposureDamp(self, s->firstEye[1]);
        }
    });
}

}  // namespace

void exposureConfigure(Config& cfg) {
    State* s = g_state;
    if (!s) return;

    // The dispatch-skip probe's spec: up to four 16-digit hex hashes (the
    // census's ch= column), comma separated; "ch:" prefixes tolerated since
    // that is how the column spells them. Refused whole on any parse doubt,
    // the census_skip discipline.
    {
        const std::string spec =
            cfg.getString("advanced.census_skip_dispatch", "");
        if (spec.length() < sizeof(s->dispatchSkipSpec) &&
            spec != s->dispatchSkipSpec) {
            memcpy(s->dispatchSkipSpec, spec.c_str(), spec.length() + 1);
            s->dispatchSkipCount = 0;
            s->dispatchSkipNoted = false;
            const char* p = spec.c_str();
            bool ok = true;
            while (*p && s->dispatchSkipCount < 4) {
                while (*p == ' ' || *p == ',' || *p == '\t') ++p;
                if (!*p) break;
                if ((p[0] == 'c' || p[0] == 'C') && (p[1] == 'h' || p[1] == 'H') &&
                    p[2] == ':') {
                    p += 3;
                }
                char* end = nullptr;
                const unsigned long long h = _strtoui64(p, &end, 16);
                if (end == p || h == 0) {
                    ok = false;
                    break;
                }
                // ":N" narrows the skip to the Nth occurrence per frame --
                // "HASH:2" is the second eye's dispatch alone, which is how
                // a per-eye pair gets probed one eye at a time.
                uint8_t occ = 0;
                if (*end == ':') {
                    const char* oq = end + 1;
                    const unsigned long o = strtoul(oq, &end, 10);
                    if (end == oq || o == 0 || o > 9) {
                        ok = false;
                        break;
                    }
                    occ = static_cast<uint8_t>(o);
                }
                s->dispatchSkipOcc[s->dispatchSkipCount] = occ;
                s->dispatchSkip[s->dispatchSkipCount++] = h;
                p = end;
            }
            while (*p == ' ' || *p == ',' || *p == '\t') ++p;
            if (!ok || *p) {
                Log::get().note(
                    "dispatch skip: \"%s\" is not up to four 16-digit hex "
                    "hashes (the census's ch= column); the whole spec is "
                    "refused rather than half-applied.",
                    spec.c_str());
                s->dispatchSkipCount = 0;
            } else if (s->dispatchSkipCount) {
                Log::get().note(
                    "dispatch skip ARMED: %u compute shader(s) will NOT be "
                    "forwarded while this is set (%llu skipped under earlier "
                    "specs this session). The scene may look very wrong -- "
                    "that is the probe working. Clear the setting to restore.",
                    s->dispatchSkipCount,
                    static_cast<unsigned long long>(s->dispatchSkipped));
            } else {
                Log::get().note(
                    "dispatch skip: cleared (%llu dispatch(es) were skipped "
                    "while it was set).",
                    static_cast<unsigned long long>(s->dispatchSkipped));
            }
        }
    }

    const float wasK = s->dampK;
    float k = cfg.getFloat("experimental.exposure_damping", 0.0f);
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    s->dampK = k;
    float tau = cfg.getFloat("experimental.exposure_damping_tau", 45.0f);
    if (tau < 1.0f) tau = 1.0f;
    if (tau > 600.0f) tau = 600.0f;
    s->dampTau = tau;
    if (s->dampK != wasK) {
        if (s->dampK > 0.0f) {
            Log::get().note("exposure damping: ON, k=%.2f -- the adaptation "
                            "swing is compressed to %.0f%% about a slow "
                            "running mean. 0 restores stock; 1 holds the "
                            "mean outright.",
                            s->dampK, (1.0f - s->dampK) * 100.0f);
        } else {
            Log::get().note("exposure damping: off; the game's adaptation "
                            "is stock from the next frame.");
            s->dampPrevValid = false;
            s->dampHaveMean = false;
        }
    }
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
    // The skip probe counts occurrences per frame.
    for (uint32_t i = 0; i < 4; ++i) s->dispatchOccSeen[i] = 0;

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

bool exposureDampingActive() { return g_state && g_state->dampK > 0.0f; }

void installExposureFix(ID3D11Device* device, HookMode mode) {
    if (!device || g_state) return;

    Config& cfg = Config::get();

    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx) return;

    g_state = new State();
    shaderRegistryBegin();
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
        shaderRegistryEnd();
        delete g_state;
        g_state = nullptr;
        return;
    }

    // The mechanism, decided once per device by the caller and shared with the
    // vScreen hooks so the two never disagree about this one object. Between
    // attach and the first replace, which is the only window setMode allows.
    //
    // THE RETURN VALUE IS READ, because setMode can refuse -- the live mode's
    // block may not allocate -- and a refusal leaves the hook in InPlace while
    // every line downstream goes on describing the mode that was asked for. The
    // install line below prints mode() and is therefore honest either way; this
    // says plainly that the two differ, because "EDVR is in shared mode" is the
    // single most load-bearing fact in an issue #21 log.
    if (!s.hook.setMode(mode) && mode != HookMode::InPlace) {
        Log::get().note(
            "exposure fix: the context hook could NOT take the mode it was "
            "given, so it is patching the shared table in place instead. Every "
            "line below says what it actually did; if advanced.context_hook_mode "
            "asked for private or live, this session is not testing it.");
    }
    // Same implementation module as vScreen's hook on the same object -- the
    // two must agree about this as well, or one of them would take a slot back
    // from the runtime while the other conceded it (issue #21).
    s.hook.setImplementationModule(systemD3D11Module());

    s.hook.replace(kSlotCSSetShader, &hookedCSSetShader,
                   reinterpret_cast<void**>(&s.realCSSetShader));
    s.hook.replace(kSlotCSSetUAVs, &hookedCSSetUAVs,
                   reinterpret_cast<void**>(&s.realCSSetUAVs));
    s.hook.replace(kSlotDispatch, &hookedDispatch,
                   reinterpret_cast<void**>(&s.realDispatch));
    s.hook.replace(kSlotDispatchIndirect, &hookedDispatchIndirect,
                   reinterpret_cast<void**>(&s.realDispatchIndirect));
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
        shaderRegistryEnd();
        delete g_state;
        g_state = nullptr;
        return;
    }

    Log::get().note("exposure fix installed on %p (%zu methods), currently %s, "
                    "target %s, hooking %s",
                    static_cast<void*>(ctx), s.hook.executablePrefix(),
                    s.enabled ? "ON" : "off",
                    s.pinned ? "pinned by config" : "detected automatically",
                    s.hook.mode() == HookMode::CopyVptr
                        ? "by private vtable copy"
                    : s.hook.mode() == HookMode::LiveCopy
                        ? "by live private vtable (stubs that read the context's "
                          "own slot at each call)"
                        : "in place");

    // WHICH VARIANT THE TABLE WAS ON AT INSTALL, named by module and offset.
    //
    // From THIS hook and not vScreen's: the exposure hook installs first, so its
    // m_vtable is the runtime's real embedded table in every mode, while
    // vScreen's is this hook's private buffer the moment a private mode is in
    // play -- and printing that would name EDVR's own stubs as the runtime's
    // variant. The body lives in device_hook now because the two context probes
    // need it too and never ran this function; see logContextTableVariants.
    logContextTableVariants(s.hook.originalVTable(), s.hook.executablePrefix(),
                            "exposure context");
    exposureConfigure(cfg);
    ctx->Release();
}

void** exposureFixContextTable(size_t* spanOut) {
    State* s = g_state;
    if (!s || !s->hook.committed()) return nullptr;
    if (spanOut) *spanOut = s->hook.executablePrefix();
    return s->hook.originalVTable();
}

void exposureFixReclaimHooks(bool sceneRendered) {
    State* s = g_state;
    if (!s) return;
    // The vouch list, gated on the SCENE and not the clock: a quiet pass
    // advances the streak only when vscreen counted eye draws in the same
    // window, because compute goes legitimately silent through loading
    // screens while Present runs at four figures -- see the slot-table
    // comment. Passes without scene evidence FREEZE the streak rather than
    // reset it: a bypass does not un-bypass itself during a loading screen,
    // and resetting would let every hyperspace jump hand the intruder three
    // more free seconds.
    size_t quiet[kHitCount];
    size_t n = 0;
    for (uint32_t i = 0; i < kHitCount; ++i) {
        if (s->thunkHits[i] != 0) {
            s->quietPasses[i] = 0;
            s->thunkHits[i] = 0;
        } else if (sceneRendered && s->quietPasses[i] < 255) {
            ++s->quietPasses[i];
        }
        if (s->quietPasses[i] >= kQuietPassesToVouch) {
            quiet[n++] = kReclaimableSlots[i];
        }
    }
    s->hook.reclaim("exposure context", quiet, n);
}

// The fast patrol, per frame, nothing vouched. vScreenReclaimTick's comment
// carries the argument; this is the same pass on the other hook of the same
// object, and the two must run at the same cadence or the runtime's rewrite
// leaves one of them out of the table for a second while the other is back in.
void exposureFixReclaimTick() {
    State* s = g_state;
    if (!s) return;
    // A private table has no slot to lose; see vScreenReclaimTick.
    if (s->hook.mode() != HookMode::InPlace) return;
    s->hook.reclaim("exposure context", nullptr, 0);
}

void shutdownExposureFix() {
    if (!g_state) return;
    g_state->enabled = false;
    for (int i = 0; i < 2; ++i) {
        if (g_state->dampStaging[i]) {
            g_state->dampStaging[i]->Release();
            g_state->dampStaging[i] = nullptr;
        }
    }
    g_state->hook.uninstall();
    shaderRegistryEnd();
}

#else
}  // namespace
#endif  // !EDVR_EXPOSURE_DAMP_TEST
}  // namespace edvr
