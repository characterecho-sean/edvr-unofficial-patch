#include "exposure_actions.h"

#include <cmath>
#include <cstring>

#include "../../common/log.h"
#include "../../common/plugin_cost.h"
#include "../../common/timing.h"
#include "../../d3d11/binding_shadow.h"
#include "../../d3d11/sunglare_fix.h"

namespace edvr::plugins::exposure {
namespace {

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

BindSlot uavSlot(uint32_t i) {
    return static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::CsUav0) + i);
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

}  // namespace

void exposurePluginShareExposure(
    ExposureActionState* s, ID3D11DeviceContext* ctx,
    ID3D11UnorderedAccessView* const* first,
    ID3D11UnorderedAccessView* const* second) {
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
void exposurePluginDamp(ExposureActionState* s, ID3D11DeviceContext* ctx,
                        ID3D11UnorderedAccessView* firstEyeStrip) {
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

}  // namespace edvr::plugins::exposure
