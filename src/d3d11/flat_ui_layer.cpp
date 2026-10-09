// fix.ui_quality's flat half: the mono adapter. flat_ui_layer.h says what and why; the take, the tonemap re-issue, the
// door and the composite are the VR layer's own (ui_layer.cpp), driven here as eye 0. The pure rules are
// flat_ui_layer_math.h's.
#include "flat_ui_layer.h"

#include "flat_camera_phase.h"     // flatCameraMeasureRowShift: the jitter a draw's camera rows carry
#include "flat_compute_readback.h" // FlatComputeInternalScope
#include "flat_ui_census.h"        // flatUiFamilyOf: the census's own family rule
#include "flat_ui_layer_math.h"
#include "ui_layer.h"

#include "../common/log.h"
#include "../common/runtime_profile.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

namespace edvr {

namespace {

template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;

constexpr size_t kFamilies = static_cast<size_t>(UiLayerFamily::kCount);
constexpr size_t kDecisions = static_cast<size_t>(UiLayerDecision::kCount);
constexpr size_t kRefusals = static_cast<size_t>(FlatUiRefuse::kCount);

// One 30 s window of the adapter's counts (the census's window).
struct Window {
    uint64_t asked[kFamilies] = {}, decided[kFamilies] = {}, taken[kFamilies] = {}, atIssue[kFamilies] = {};
    uint64_t refused[kRefusals] = {};
    uint64_t declined[kDecisions] = {};   // the shared decision's refusals, by UiLayerDecision
    uint64_t jitterPhase = 0, jitterZero = 0;
    uint64_t writeBacks = 0;
    uint64_t toneAdmitted = 0, toneReissued = 0, toneDeclined = 0;
    uint64_t copies = 0, doorsArmed = 0, doorUntreated = 0, doorSize = 0;
    uint64_t composites = 0, compositeNone = 0, bindFailed = 0;
    uint32_t inW = 0, inH = 0, outW = 0, outH = 0;  // the last copy's picture and the display
    double otherX = 0.0, otherY = 0.0;              // the last other-shift draw's measured shift, render pixels
};
Window g_w;
FlatUiLayerAsk g_lastAsk = FlatUiLayerAsk::kNotAsked;

// The view the copy samples in place of its own: over the composite's output, in the copy's own view format.
Ptr<ID3D11ShaderResourceView> g_view;
ID3D11Texture2D* g_viewTex = nullptr;  // identity only (the layer holds the texture)
DXGI_FORMAT g_viewFmt = DXGI_FORMAT_UNKNOWN;
bool g_bindNoted = false, g_firstCompositeNoted = false, g_otherShiftNoted = false;

const char* decisionShort(size_t d) {
    switch (static_cast<UiLayerDecision>(d)) {
    case UiLayerDecision::kNotEyeTarget: return "not-eye-target";
    case UiLayerDecision::kHdrTarget: return "hdr-target";
    case UiLayerDecision::kNoEye: return "no-eye";
    case UiLayerDecision::kTargetSize: return "target-size";
    case UiLayerDecision::kLate: return "late";
    case UiLayerDecision::kToneLate: return "after-tonemap";
    case UiLayerDecision::kNotArmed: return "not-armed";
    case UiLayerDecision::kMrt: return "mrt-or-uav";
    case UiLayerDecision::kDepthStencilTest: return "depth-stencil-test";
    case UiLayerDecision::kBlendRefused: return "blend";
    case UiLayerDecision::kLayerFailed: return "layer-failed";
    case UiLayerDecision::kVerdict: return "verdict";
    default: return "other";
    }
}

}  // namespace

bool flatUiLayerOn() { return runtimeFlatProfile() && uiLayerCrispOn(); }

const char* flatUiLayerState() {
    if (!runtimeFlatProfile()) return "off (not the flat profile)";
    if (uiLayerCrispOn()) return "live";
    if (uiLayerLive()) return "off (the HDR HUD path stood down; see its line)";
    // The shared layer's own reason, verbatim (2026-10-09: a relabelled reason hid the refused jitter key that kept the
    // first flight's layer dead). In flat, "no temporal mode is on (fix.temporal_aa is off)" means the flat AA mode is off.
    const char* why = uiLayerNotLiveReason();
    return why ? why : "off (no reason given)";
}

FlatUiLayerAsk flatUiLayerDecide(ID3D11DeviceContext* ctx, const FlatUiLayerDraw& d) {
    g_lastAsk = FlatUiLayerAsk::kNotAsked;
    if (!ctx || !flatUiLayerOn()) return g_lastAsk;
    const UiLayerFamily family = flatUiFamilyOf(d.vs, d.ps);
    if (!flatUiLayerTakesFamily(family)) return g_lastAsk;
    const size_t fi = static_cast<size_t>(family);
    ++g_w.asked[fi];
    g_lastAsk = FlatUiLayerAsk::kRefused;
    const auto refuse = [&](FlatUiRefuse r) {
        ++g_w.refused[static_cast<size_t>(r)];
        return g_lastAsk;
    };
    if (d.otherWork) return refuse(FlatUiRefuse::kOtherWork);
    if (!d.hdrTarget || !d.width || !d.height) return refuse(FlatUiRefuse::kNotHdrTarget);
    if (!d.upstream) return refuse(FlatUiRefuse::kNotUpstream);
    if (!d.haveRows) return refuse(FlatUiRefuse::kNoRows);
    double ndcX = 0.0, ndcY = 0.0;
    const bool measured = flatCameraMeasureRowShift(d.rows, ndcX, ndcY);
    const FlatUiJitterRead j = flatUiLayerJitterOf(measured, ndcX, ndcY, d.width, d.height, d.phaseX, d.phaseY);
    if (j.kind == FlatUiJitter::kNoRows) return refuse(FlatUiRefuse::kNoRows);
    if (j.kind == FlatUiJitter::kOther) {
        g_w.otherX = j.mx;
        g_w.otherY = j.my;
        if (!g_otherShiftNoted) {
            g_otherShiftNoted = true;
            Log::get().note("flat ui layer: a %s draw (vs %016llX ps %016llX) carries a camera shift of (%.4f, %.4f) render "
                            "pixels, neither the frame's phase (%.4f, %.4f) nor zero: left in the game's frame (counted as "
                            "other-shift; said once).",
                            uiLayerFamilyName(family), static_cast<unsigned long long>(d.vs),
                            static_cast<unsigned long long>(d.ps), j.mx, j.my, static_cast<double>(d.phaseX),
                            static_cast<double>(d.phaseY));
        }
        return refuse(FlatUiRefuse::kOtherShift);
    }
    if (j.kind == FlatUiJitter::kPhase) ++g_w.jitterPhase;
    else ++g_w.jitterZero;
    FlatComputeInternalScope internal;
    uiLayerFlatSetDraw(d.frame, j.jx, j.jy, d.width, d.height);
    if (!uiLayerDecide(ctx, static_cast<int>(family), true, false, 0)) {
        const int decision = uiLayerLastDecision();
        if (decision >= 0 && static_cast<size_t>(decision) < kDecisions) ++g_w.declined[static_cast<size_t>(decision)];
        return g_lastAsk;
    }
    ++g_w.decided[fi];
    g_lastAsk = FlatUiLayerAsk::kDecided;
    return g_lastAsk;
}

bool flatUiLayerBegin(ID3D11DeviceContext* ctx) {
    FlatComputeInternalScope internal;
    return uiLayerBegin(ctx);
}

void flatUiLayerEnd(ID3D11DeviceContext* ctx) {
    FlatComputeInternalScope internal;
    uiLayerEnd(ctx);
}

bool flatUiLayerWriteBackBegin(ID3D11DeviceContext* ctx) {
    FlatComputeInternalScope internal;
    const bool ok = uiLayerWriteBackBegin(ctx);
    if (ok) ++g_w.writeBacks;
    return ok;
}

void flatUiLayerWriteBackEnd(ID3D11DeviceContext* ctx) {
    FlatComputeInternalScope internal;
    uiLayerWriteBackEnd(ctx);
}

// The take's outcome, for the window: Begin's answer per family (the census rows carry the same per shader pair).
void flatUiLayerNoteIssue(uint64_t vs, uint64_t ps, bool taken) {
    const size_t fi = static_cast<size_t>(flatUiFamilyOf(vs, ps));
    if (fi >= kFamilies) return;
    if (taken) ++g_w.taken[fi];
    else ++g_w.atIssue[fi];
}

bool flatUiLayerToneAdmit(ID3D11DeviceContext* ctx, uint64_t frame, uint32_t renderW, uint32_t renderH, char kind,
                          uint32_t count, uint32_t instances, uint32_t startInstance) {
    if (!ctx || count != 3 || instances != 1 || !flatUiLayerOn()) return false;
    FlatComputeInternalScope internal;
    // The admission reads only the frame's sequence from the draw facts (no jitter): the HDR layer's content is keyed on it.
    uiLayerFlatSetDraw(frame, 0.0f, 0.0f, renderW, renderH);
    const bool admitted = uiLayerCrispNoteEyeDraw(ctx, kind, count, instances, startInstance);
    if (admitted) ++g_w.toneAdmitted;
    return admitted;
}

bool flatUiLayerToneBegin(ID3D11DeviceContext* ctx) {
    FlatComputeInternalScope internal;
    const bool ok = uiLayerCrispToneBegin(ctx);
    if (ok) ++g_w.toneReissued;
    else ++g_w.toneDeclined;
    return ok;
}

void flatUiLayerToneEnd(ID3D11DeviceContext* ctx) {
    FlatComputeInternalScope internal;
    uiLayerCrispToneEnd(ctx);
}

void flatUiLayerNoteSceneDraw(ID3D11DeviceContext* ctx, uint32_t count, uint32_t instances, char kind) {
    if (!uiLayerWatching()) return;
    FlatComputeInternalScope internal;
    uiLayerNoteSceneDraw(ctx, count, instances, kind);
}

void flatUiLayerAtCopy(ID3D11DeviceContext* ctx, uint64_t frame, bool treated, uint32_t outW, uint32_t outH,
                       ID3D11ShaderResourceView** original, bool* replaced) {
    if (!ctx || !original || !replaced || !flatUiLayerOn()) return;
    ++g_w.copies;
    FlatComputeInternalScope internal;
    Ptr<ID3D11ShaderResourceView> now;
    ctx->PSGetShaderResources(0, 1, &now);
    if (!now) return;
    Ptr<ID3D11Resource> resource;
    now->GetResource(&resource);
    Ptr<ID3D11Texture2D> picture;
    if (!resource || FAILED(resource.As(&picture))) return;
    D3D11_TEXTURE2D_DESC td{};
    picture->GetDesc(&td);
    g_w.inW = td.Width;
    g_w.inH = td.Height;
    g_w.outW = outW;
    g_w.outH = outH;
    // The door: the next frame's HUD is taken only behind a frame EDVR resolved to the display's size.
    if (flatUiLayerDoorArms(treated, td.Width, td.Height, outW, outH)) {
        uiLayerNoteTemporal(frame, 0, picture.Get());
        uiLayerDoorSeen(frame, 0, picture.Get());
        ++g_w.doorsArmed;
    } else if (!treated) {
        ++g_w.doorUntreated;
    } else {
        ++g_w.doorSize;
    }
    // The composite: this frame's HUD over the picture the copy reads (the resolve's, or the game's own after a refusal).
    const uint32_t region[4] = {0, 0, td.Width, td.Height};
    const float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    ID3D11Texture2D* out = uiLayerComposite(frame, 0, picture.Get(), region, uv);
    if (!out) {
        ++g_w.compositeNone;
        return;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    now->GetDesc(&vd);
    if (!g_view || g_viewTex != out || g_viewFmt != vd.Format) {
        g_view.Reset();
        g_viewTex = nullptr;
        Ptr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (dev && SUCCEEDED(dev->CreateShaderResourceView(out, &vd, &g_view)) && g_view) {
            g_viewTex = out;
            g_viewFmt = vd.Format;
        } else {
            g_view.Reset();
        }
    }
    out->Release();  // the composite's reference; the layer keeps its own
    if (!g_view) {
        ++g_w.bindFailed;
        if (!g_bindNoted) {
            g_bindNoted = true;
            Log::get().note("flat ui layer: a view over the composite's %ux%u output could not be made in the copy's view "
                            "format %d; the frame's HUD is missing from that frame (said once, counted as bind-failed).",
                            td.Width, td.Height, static_cast<int>(vd.Format));
        }
        return;
    }
    // The game's own t0 is kept for the scope to put back: taken here when nothing replaced it yet.
    if (!*replaced && !*original) *original = now.Detach();
    ID3D11ShaderResourceView* bound = g_view.Get();
    ctx->PSSetShaderResources(0, 1, &bound);
    *replaced = true;
    ++g_w.composites;
    if (!g_firstCompositeNoted) {
        g_firstCompositeNoted = true;
        Log::get().note("flat ui layer: first composite -- frame %llu, the cockpit HUD layer over the %ux%u picture the "
                        "game's output copy reads (display %ux%u, %s), bound at the copy's t0 in view format %d.",
                        static_cast<unsigned long long>(frame), td.Width, td.Height, outW, outH,
                        treated ? "resolved by EDVR" : "the game's own, the resolve refused", static_cast<int>(vd.Format));
    }
}

void flatUiLayerRelease() {
    g_view.Reset();
    g_viewTex = nullptr;
    g_viewFmt = DXGI_FORMAT_UNKNOWN;
    FlatComputeInternalScope internal;
    if (uiLayerFlatRelease())
        Log::get().note("flat ui layer: the swap chain is resizing -- every layer, depth target and composite output "
                        "released; the next frame EDVR resolves arms a fresh door.");
}

void flatUiLayerReport(uint64_t windowSeconds) {
    Window& w = g_w;
    Log::get().note(
        "flat ui layer: window=%llus state=%s copies=%llu door-armed=%llu door-refused-untreated=%llu "
        "door-refused-size=%llu (last copy read %ux%u, display %ux%u) tone-admitted=%llu tone-reissued=%llu "
        "tone-declined=%llu composites=%llu composite-none=%llu bind-failed=%llu write-backs=%llu jitter-phase=%llu "
        "jitter-zero=%llu",
        static_cast<unsigned long long>(windowSeconds), flatUiLayerState(), static_cast<unsigned long long>(w.copies),
        static_cast<unsigned long long>(w.doorsArmed), static_cast<unsigned long long>(w.doorUntreated),
        static_cast<unsigned long long>(w.doorSize), w.inW, w.inH, w.outW, w.outH,
        static_cast<unsigned long long>(w.toneAdmitted), static_cast<unsigned long long>(w.toneReissued),
        static_cast<unsigned long long>(w.toneDeclined), static_cast<unsigned long long>(w.composites),
        static_cast<unsigned long long>(w.compositeNone), static_cast<unsigned long long>(w.bindFailed),
        static_cast<unsigned long long>(w.writeBacks), static_cast<unsigned long long>(w.jitterPhase),
        static_cast<unsigned long long>(w.jitterZero));
    const UiLayerFamily families[4] = {UiLayerFamily::kHolo, UiLayerFamily::kFlightHud, UiLayerFamily::kSprite,
                                       UiLayerFamily::kHoloGeneric};
    const size_t f0 = static_cast<size_t>(families[0]), f1 = static_cast<size_t>(families[1]),
                 f2 = static_cast<size_t>(families[2]), f3 = static_cast<size_t>(families[3]);
    Log::get().note(
        "flat ui layer families: holo-panels asked=%llu decided=%llu taken=%llu refused-at-issue=%llu; flight-hud "
        "asked=%llu decided=%llu taken=%llu refused-at-issue=%llu; target-sprite asked=%llu decided=%llu taken=%llu "
        "refused-at-issue=%llu; holograms asked=%llu decided=%llu taken=%llu refused-at-issue=%llu",
        static_cast<unsigned long long>(w.asked[f0]), static_cast<unsigned long long>(w.decided[f0]),
        static_cast<unsigned long long>(w.taken[f0]), static_cast<unsigned long long>(w.atIssue[f0]),
        static_cast<unsigned long long>(w.asked[f1]), static_cast<unsigned long long>(w.decided[f1]),
        static_cast<unsigned long long>(w.taken[f1]), static_cast<unsigned long long>(w.atIssue[f1]),
        static_cast<unsigned long long>(w.asked[f2]), static_cast<unsigned long long>(w.decided[f2]),
        static_cast<unsigned long long>(w.taken[f2]), static_cast<unsigned long long>(w.atIssue[f2]),
        static_cast<unsigned long long>(w.asked[f3]), static_cast<unsigned long long>(w.decided[f3]),
        static_cast<unsigned long long>(w.taken[f3]), static_cast<unsigned long long>(w.atIssue[f3]));
    char declined[400] = "";
    size_t used = 0;
    for (size_t d = 0; d < kDecisions && used < sizeof(declined); ++d) {
        if (!w.declined[d]) continue;
        const int n = std::snprintf(declined + used, sizeof(declined) - used, "%s%s=%llu", used ? " " : "",
                                    decisionShort(d), static_cast<unsigned long long>(w.declined[d]));
        if (n < 0) break;
        used += static_cast<size_t>(n);
    }
    Log::get().note(
        "flat ui layer refusals: other-work=%llu not-hdr-target=%llu not-upstream=%llu no-camera-rows=%llu "
        "other-shift=%llu (last %.3f, %.3f px); the shared decision: %s",
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(FlatUiRefuse::kOtherWork)]),
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(FlatUiRefuse::kNotHdrTarget)]),
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(FlatUiRefuse::kNotUpstream)]),
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(FlatUiRefuse::kNoRows)]),
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(FlatUiRefuse::kOtherShift)]), w.otherX, w.otherY,
        used ? declined : "none");
    g_w = Window{};
}

}  // namespace edvr
