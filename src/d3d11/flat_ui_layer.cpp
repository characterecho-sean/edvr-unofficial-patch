// fix.ui_quality's flat half: the mono adapter. flat_ui_layer.h says what and why; the take, the tonemap re-issue, the
// door and the composite are the VR layer's own (ui_layer.cpp), driven here as eye 0. The pure rules are
// flat_ui_layer_math.h's.
#include "flat_ui_layer.h"

#include "flat_camera_phase.h"     // flatCameraMeasureRowShift: the jitter a draw's camera rows carry
#include "flat_compute_readback.h" // FlatComputeInternalScope
#include "flat_ui_layer_math.h"
#include "ui_layer.h"

#include "../common/log.h"
#include "../common/runtime_profile.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {

namespace {

template <class T>
using Ptr = Microsoft::WRL::ComPtr<T>;

constexpr size_t kFamilies = static_cast<size_t>(UiLayerFamily::kCount);
constexpr size_t kDecisions = static_cast<size_t>(UiLayerDecision::kCount);
constexpr size_t kRefusals = static_cast<size_t>(FlatUiRefuse::kCount);

// One 30 s window of the adapter's counts (flatUiLayerFrame).
struct Window {
    uint64_t asked[kFamilies] = {}, decided[kFamilies] = {}, taken[kFamilies] = {}, atIssue[kFamilies] = {};
    uint64_t refused[kRefusals] = {};
    uint64_t declined[kDecisions] = {};   // the shared decision's refusals, by UiLayerDecision
    uint64_t jitterPhase = 0, jitterZero = 0;
    uint64_t writeBacks = 0;
    uint64_t toneCandidates = 0, toneProven = 0, toneAdmitted = 0, toneReissued = 0, toneDeclined = 0;
    uint64_t copies = 0, doorsArmed = 0, doorUntreated = 0, doorSize = 0;
    uint64_t composites = 0, compositeNone = 0, bindFailed = 0;
    uint32_t inW = 0, inH = 0, outW = 0, outH = 0;  // the last copy's picture and the display
    double otherX = 0.0, otherY = 0.0;              // the last other-shift draw's measured shift, render pixels
    // The System Map's two families (2026-10-10): the frames a map was open in, and per family (0 canvas, 1 sprite) the
    // adapter's refusals and the shared decision's declines, by reason. Their asked/decided/taken/refused-at-issue are the
    // family arrays above.
    uint64_t mapOpenFrames = 0;
    uint64_t mapRefused[2][kRefusals] = {};
    uint64_t mapDeclined[2][kDecisions] = {};
    // The map tonemap's candidates (flatUiMapToneSlotOf): the tone reads the candidate branch saw, and those that proved.
    uint64_t mapToneCandidates = 0, mapToneMatched = 0;
};
Window g_w;
FlatUiLayerAsk g_lastAsk = FlatUiLayerAsk::kNotAsked;
UiLayerFamily g_decidedFamily = UiLayerFamily::kNone;  // the family of the last kDecided draw (flatUiLayerNoteIssue's)
bool g_mapCanvasNoted = false, g_mapSpriteNoted = false;  // the once-only first take of each map family
bool g_mapToneNoted = false;                              // the once-only first proof from a map frame's tonemap
bool g_mapFirstToneNoted = false;                         // the once-only first map-tone candidate line (proved or not)
FlatUiMapAaTally g_mapAa;                                 // temporal AA off on the Galaxy Map and the Orrery: the map line's counts
uint64_t g_mapAskFrame = 0, g_mapAsksFrame = 0;           // the frame of the last map ask, and how many map asks it has had
FlatUiToneProof g_proof;
uint32_t g_candidateLines = 0;  // eight tone candidates logged, re-armed after a route, render-size or swap-chain change
char g_candidateRoute[48] = "";
uint32_t g_candidateW = 0, g_candidateH = 0;
uint64_t g_takenFrame = 0;          // the frame the shared decision last took a HUD draw in...
const void* g_takenTarget = nullptr;  // ...and its target (the admission's copy is that target's)

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

// The map families' window slot (Window::mapRefused / mapDeclined), or -1 for every other family.
int mapSlot(UiLayerFamily f) {
    if (f == UiLayerFamily::kMapCanvas) return 0;
    if (f == UiLayerFamily::kMapSprite) return 1;
    return -1;
}

// One map family's refusals this window, by reason: the adapter's, the shared decision's, and the refusals at the game's
// issue ("at-issue"). "other-work=2 late=1 at-issue=3", or "none".
std::string mapReasons(const Window& w, int slot, uint64_t atIssue) {
    std::string out;
    const auto add = [&out](const char* name, uint64_t n) {
        if (!n) return;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s%s=%llu", out.empty() ? "" : " ", name, static_cast<unsigned long long>(n));
        out += buf;
    };
    for (size_t r = 0; r < kRefusals; ++r) add(flatUiRefuseName(static_cast<FlatUiRefuse>(r)), w.mapRefused[slot][r]);
    for (size_t d = 0; d < kDecisions; ++d) add(decisionShort(d), w.mapDeclined[slot][d]);
    add("at-issue", atIssue);
    return out.empty() ? "none" : out;
}

// The same refusals as one number (mapReasons' list, summed).
uint64_t mapRefusedTotal(const Window& w, int slot, uint64_t atIssue) {
    uint64_t n = atIssue;
    for (size_t r = 0; r < kRefusals; ++r) n += w.mapRefused[slot][r];
    for (size_t d = 0; d < kDecisions; ++d) n += w.mapDeclined[slot][d];
    return n;
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
    // The map families first (only with a map open, on the HDR target); otherwise the shared family rule, as it always was.
    const UiLayerFamily family = flatUiFamilyFor(d.vs, d.ps, d.mapOpen, d.hdrTarget);
    if (!flatUiLayerTakesFamily(family)) return g_lastAsk;
    const size_t fi = static_cast<size_t>(family);
    const int ms = mapSlot(family);
    if (ms >= 0) {  // the map asks this frame, counted as they come (the first map-tone candidate reports how many came before it)
        if (g_mapAskFrame != d.frame) { g_mapAskFrame = d.frame; g_mapAsksFrame = 0; }
        ++g_mapAsksFrame;
    }
    ++g_w.asked[fi];
    g_lastAsk = FlatUiLayerAsk::kRefused;
    const auto refuse = [&](FlatUiRefuse r) {
        ++g_w.refused[static_cast<size_t>(r)];
        if (ms >= 0) ++g_w.mapRefused[ms][static_cast<size_t>(r)];
        return g_lastAsk;
    };
    if (d.otherWork) return refuse(FlatUiRefuse::kOtherWork);
    if (!d.hdrTarget || !d.width || !d.height) return refuse(FlatUiRefuse::kNotHdrTarget);
    // The proof's subject: the target this frame's HUD families draw into, from the first ask, taken or not.
    flatUiToneProofHud(g_proof, d.frame, d.color);
    if (flatUiToneConsumed(g_proof, d.frame, d.color)) return refuse(FlatUiRefuse::kAfterTone);
    if (!flatUiToneProven(g_proof, d.frame, d.color)) return refuse(FlatUiRefuse::kToneUnproven);
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
        if (decision >= 0 && static_cast<size_t>(decision) < kDecisions) {
            ++g_w.declined[static_cast<size_t>(decision)];
            if (ms >= 0) ++g_w.mapDeclined[ms][static_cast<size_t>(decision)];
        }
        return g_lastAsk;
    }
    ++g_w.decided[fi];
    g_decidedFamily = family;  // flatUiLayerNoteIssue counts this draw under it
    if (g_takenFrame != d.frame) {
        g_takenFrame = d.frame;
        g_takenTarget = d.color;
    }
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

// The take's outcome, for the window: Begin's answer per family.
void flatUiLayerNoteIssue(uint64_t vs, uint64_t ps, bool taken) {
    const size_t fi = static_cast<size_t>(g_decidedFamily);
    if (fi >= kFamilies) return;
    if (!taken) {
        ++g_w.atIssue[fi];
        return;
    }
    ++g_w.taken[fi];
    // The first take of each map family is said once, with its frame and pair (the first composite's note, for the maps).
    const int ms = mapSlot(g_decidedFamily);
    if (ms < 0) return;
    bool& noted = ms == 0 ? g_mapCanvasNoted : g_mapSpriteNoted;
    if (!noted) {
        noted = true;
        Log::get().note("flat ui layer: the first %s taken -- frame %llu, vs %016llX ps %016llX, into the flat HDR layer "
                        "while a map is open (GuiFocus 6, 7 or 8); said once.",
                        uiLayerFamilyName(g_decidedFamily), static_cast<unsigned long long>(g_takenFrame),
                        static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps));
    }
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

void flatUiLayerNoteCopy(uint64_t frame, const void* source, const void* output) {
    if (!flatUiLayerOn()) return;
    flatUiToneProofCopy(g_proof, frame, source, output);
}

bool flatUiLayerToneCandidate(ID3D11DeviceContext* ctx, uint64_t frame, uint32_t renderW, uint32_t renderH, int hdrSlot,
                              uint64_t vs, uint64_t ps, const void* input, uint32_t outW, uint32_t outH, const char* route) {
    if (!ctx || !flatUiLayerOn()) return false;
    ++g_w.toneCandidates;
    // The candidate log re-arms for eight more lines after a route change or a render-size change (a resize re-arms it in
    // flatUiLayerRelease), so the first candidates after any change are always on record.
    if (!route) route = "?";
    if (std::strcmp(route, g_candidateRoute) != 0 || renderW != g_candidateW || renderH != g_candidateH) {
        std::snprintf(g_candidateRoute, sizeof(g_candidateRoute), "%s", route);
        g_candidateW = renderW;
        g_candidateH = renderH;
        g_candidateLines = 0;
    }
    flatUiToneProofFrame(g_proof, frame);
    char targets[200] = "";
    size_t used = 0;
    for (uint32_t i = 0; i < g_proof.count && used < sizeof(targets); ++i) {
        const int n = std::snprintf(targets + used, sizeof(targets) - used, "%s%p (copy %p)", i ? ", " : "", g_proof.hud[i],
                                    g_proof.alias[i]);
        if (n < 0) break;
        used += static_cast<size_t>(n);
    }
    // The alias the admission may accept: the copy of the target this frame's HUD was taken from.
    const void* takenAlias = nullptr;
    for (uint32_t i = 0; i < g_proof.count; ++i)
        if (g_takenFrame == frame && g_proof.hud[i] == g_takenTarget) takenAlias = g_proof.alias[i];
    const bool proven = flatUiToneProofTone(g_proof, frame, input) != nullptr;
    if (proven) ++g_w.toneProven;
    // The map tonemap's candidates (flatUiMapToneSlotOf), counted every time, and the first one said once, proved or not, with
    // the pointer it compared and the targets recorded this frame: a null input, no recorded target or a pointer mismatch
    // is then visible in the log, not silent.
    if (flatUiMapToneSlotOf(vs, ps) != ~0u && flatUiMapToneSlotOf(vs, ps) == static_cast<uint32_t>(hdrSlot)) {
        ++g_w.mapToneCandidates;
        if (proven) ++g_w.mapToneMatched;
        if (!g_mapFirstToneNoted) {
            g_mapFirstToneNoted = true;
            Log::get().note("flat ui layer: first map-tone candidate -- frame %llu, input %p, recorded targets %s, match %s, map "
                            "asks this frame %llu; said once.",
                            static_cast<unsigned long long>(frame), input, used ? targets : "none",
                            flatUiToneMatchWhy(g_proof, frame, input),
                            static_cast<unsigned long long>(g_mapAskFrame == frame ? g_mapAsksFrame : 0));
        }
    }
    // The first proof a map frame's tonemap records (flatUiMapToneSlotOf: the UI layer's own table): said once, with its frame.
    if (proven && !g_mapToneNoted && flatUiMapToneSlotOf(vs, ps) == static_cast<uint32_t>(hdrSlot)) {
        g_mapToneNoted = true;
        Log::get().note("flat ui layer: map-frame tone proven by VS %016llX PS %016llX reading slot %d, frame %llu (the proof "
                        "holds for frame %llu); said once.",
                        static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps), hdrSlot,
                        static_cast<unsigned long long>(frame), static_cast<unsigned long long>(frame + 1));
    }
    bool admitted = false;
    char why[96] = "not asked: no HUD was taken this frame";
    {
        FlatComputeInternalScope internal;
        uiLayerFlatSetDraw(frame, 0.0f, 0.0f, renderW, renderH);  // the sequence only
        admitted = uiLayerCrispAdmitFlat(ctx, hdrSlot, takenAlias, vs, ps, why, sizeof(why)) == 1;
    }
    if (admitted) ++g_w.toneAdmitted;
    if (g_candidateLines < 8) {
        ++g_candidateLines;
        Log::get().note("flat ui layer: tone candidate %u: frame %llu vs %016llX ps %016llX, HDR slot t%d reads %p, output %ux%u, "
                        "render %ux%u, route %s; this frame's HUD targets: %s; proof for the next frame: %s; re-issue: %s.",
                        g_candidateLines, static_cast<unsigned long long>(frame), static_cast<unsigned long long>(vs),
                        static_cast<unsigned long long>(ps), hdrSlot, input, outW, outH, renderW, renderH, route,
                        used ? targets : "none asked yet", proven ? "yes" : used ? "no (reads none of them)" : "no (no HUD ask yet)",
                        why);
    }
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

namespace {
Ptr<ID3D11Device> g_device;  // the device the shared layer's children were made on (identity; held so it cannot recur)
}  // namespace

void flatUiLayerNoteDevice(ID3D11Device* device) {
    if (!device || device == g_device.Get()) return;
    if (g_device) {
        g_proof = FlatUiToneProof{};
        g_view.Reset();
        g_viewTex = nullptr;
        g_viewFmt = DXGI_FORMAT_UNKNOWN;
        {
            FlatComputeInternalScope internal;
            uiLayerDeviceReset();
        }
        Log::get().note("flat ui layer: the D3D11 device changed -- every device child of the shared layer (blend cache, "
                        "seeder, deferred contexts, coverage and composite shaders, parameter buffer, timers, layers) "
                        "released; they are made again on the new device at their next use.");
    }
    g_device = device;
}

void flatUiLayerRelease() {
    g_proof = FlatUiToneProof{};  // a fresh chain proves itself again
    g_candidateLines = 0;         // and its first tone candidates are logged
    g_takenFrame = 0;
    g_takenTarget = nullptr;
    g_view.Reset();
    g_viewTex = nullptr;
    g_viewFmt = DXGI_FORMAT_UNKNOWN;
    FlatComputeInternalScope internal;
    if (uiLayerFlatRelease())
        Log::get().note("flat ui layer: the swap chain is resizing -- every layer, depth target and composite output "
                        "released; the next frame EDVR resolves arms a fresh door.");
}

void flatUiLayerMapAaFrame(bool engaged, bool jitterWanted) {
    if (!runtimeFlatProfile()) return;
    g_mapAa.frame(engaged, jitterWanted);
}
void flatUiLayerMapAaCopy(bool ok) { g_mapAa.copy(ok); }

void flatUiLayerFrame(bool mapOpen) {
    if (!runtimeFlatProfile()) return;
    if (mapOpen) ++g_w.mapOpenFrames;
    static uint64_t windowStartMs = 0;
    const uint64_t now = GetTickCount64();
    if (!windowStartMs) windowStartMs = now;
    if (now - windowStartMs < 30000) return;
    flatUiLayerReport((now - windowStartMs) / 1000);
    windowStartMs = now;
}

void flatUiLayerReport(uint64_t windowSeconds) {
    Window& w = g_w;
    Log::get().note(
        "flat ui layer: window=%llus state=%s copies=%llu door-armed=%llu door-refused-untreated=%llu "
        "door-refused-size=%llu (last copy read %ux%u, display %ux%u) tone-candidates=%llu tone-proven=%llu "
        "tone-admitted=%llu tone-reissued=%llu "
        "tone-declined=%llu composites=%llu composite-none=%llu bind-failed=%llu write-backs=%llu jitter-phase=%llu "
        "jitter-zero=%llu",
        static_cast<unsigned long long>(windowSeconds), flatUiLayerState(), static_cast<unsigned long long>(w.copies),
        static_cast<unsigned long long>(w.doorsArmed), static_cast<unsigned long long>(w.doorUntreated),
        static_cast<unsigned long long>(w.doorSize), w.inW, w.inH, w.outW, w.outH,
        static_cast<unsigned long long>(w.toneCandidates), static_cast<unsigned long long>(w.toneProven),
        static_cast<unsigned long long>(w.toneAdmitted), static_cast<unsigned long long>(w.toneReissued),
        static_cast<unsigned long long>(w.toneDeclined), static_cast<unsigned long long>(w.composites),
        static_cast<unsigned long long>(w.compositeNone), static_cast<unsigned long long>(w.bindFailed),
        static_cast<unsigned long long>(w.writeBacks), static_cast<unsigned long long>(w.jitterPhase),
        static_cast<unsigned long long>(w.jitterZero));
    const UiLayerFamily families[4] = {UiLayerFamily::kHolo, UiLayerFamily::kFlightHud, UiLayerFamily::kSprite,
                                       UiLayerFamily::kHoloGeneric};
    const size_t f0 = static_cast<size_t>(families[0]), f1 = static_cast<size_t>(families[1]),
                 f2 = static_cast<size_t>(families[2]), f3 = static_cast<size_t>(families[3]);
    const size_t fc = static_cast<size_t>(UiLayerFamily::kMapCanvas), fs = static_cast<size_t>(UiLayerFamily::kMapSprite);
    Log::get().note(
        "flat ui layer families: holo-panels asked=%llu decided=%llu taken=%llu refused-at-issue=%llu; flight-hud "
        "asked=%llu decided=%llu taken=%llu refused-at-issue=%llu; target-sprite asked=%llu decided=%llu taken=%llu "
        "refused-at-issue=%llu; holograms asked=%llu decided=%llu taken=%llu refused-at-issue=%llu; map-canvas "
        "asked=%llu decided=%llu taken=%llu refused-at-issue=%llu; map-sprite asked=%llu decided=%llu taken=%llu "
        "refused-at-issue=%llu",
        static_cast<unsigned long long>(w.asked[f0]), static_cast<unsigned long long>(w.decided[f0]),
        static_cast<unsigned long long>(w.taken[f0]), static_cast<unsigned long long>(w.atIssue[f0]),
        static_cast<unsigned long long>(w.asked[f1]), static_cast<unsigned long long>(w.decided[f1]),
        static_cast<unsigned long long>(w.taken[f1]), static_cast<unsigned long long>(w.atIssue[f1]),
        static_cast<unsigned long long>(w.asked[f2]), static_cast<unsigned long long>(w.decided[f2]),
        static_cast<unsigned long long>(w.taken[f2]), static_cast<unsigned long long>(w.atIssue[f2]),
        static_cast<unsigned long long>(w.asked[f3]), static_cast<unsigned long long>(w.decided[f3]),
        static_cast<unsigned long long>(w.taken[f3]), static_cast<unsigned long long>(w.atIssue[f3]),
        static_cast<unsigned long long>(w.asked[fc]), static_cast<unsigned long long>(w.decided[fc]),
        static_cast<unsigned long long>(w.taken[fc]), static_cast<unsigned long long>(w.atIssue[fc]),
        static_cast<unsigned long long>(w.asked[fs]), static_cast<unsigned long long>(w.decided[fs]),
        static_cast<unsigned long long>(w.taken[fs]), static_cast<unsigned long long>(w.atIssue[fs]));
    // The System Map's line (2026-10-10): the frames a map was open in, and each map family's asked/taken/refused, with the
    // refusals by reason (the adapter's and the shared decision's; "at-issue" is a refusal at the game's issue). Zeros print.
    Log::get().note(
        "flat ui layer map: open-frames=%llu; canvas asked=%llu taken=%llu refused=%llu (%s); sprite asked=%llu taken=%llu "
        "refused=%llu (%s); tone-candidates-map=%llu tone-matched-map=%llu; temporal-off=%llu jitter-zeroed=%llu spatial=%llu "
        "spatial-refused=%llu no-copy=%llu resets=%llu",
        static_cast<unsigned long long>(w.mapOpenFrames), static_cast<unsigned long long>(w.asked[fc]),
        static_cast<unsigned long long>(w.taken[fc]), static_cast<unsigned long long>(mapRefusedTotal(w, 0, w.atIssue[fc])),
        mapReasons(w, 0, w.atIssue[fc]).c_str(), static_cast<unsigned long long>(w.asked[fs]),
        static_cast<unsigned long long>(w.taken[fs]), static_cast<unsigned long long>(mapRefusedTotal(w, 1, w.atIssue[fs])),
        mapReasons(w, 1, w.atIssue[fs]).c_str(), static_cast<unsigned long long>(w.mapToneCandidates),
        static_cast<unsigned long long>(w.mapToneMatched), static_cast<unsigned long long>(g_mapAa.temporalOff),
        static_cast<unsigned long long>(g_mapAa.jitterZeroed), static_cast<unsigned long long>(g_mapAa.spatial),
        static_cast<unsigned long long>(g_mapAa.spatialRefused), static_cast<unsigned long long>(g_mapAa.noCopy),
        static_cast<unsigned long long>(g_mapAa.resets));
    g_mapAa.clearCounts();
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
        "other-shift=%llu (last %.3f, %.3f px) tone-unproven=%llu after-tone=%llu; the shared decision: %s",
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(FlatUiRefuse::kOtherWork)]),
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(FlatUiRefuse::kNotHdrTarget)]),
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(FlatUiRefuse::kNotUpstream)]),
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(FlatUiRefuse::kNoRows)]),
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(FlatUiRefuse::kOtherShift)]), w.otherX, w.otherY,
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(FlatUiRefuse::kToneUnproven)]),
        static_cast<unsigned long long>(w.refused[static_cast<size_t>(FlatUiRefuse::kAfterTone)]),
        used ? declined : "none");
    g_w = Window{};
}

}  // namespace edvr
