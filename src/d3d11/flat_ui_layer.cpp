// fix.ui_quality's flat half: the mono adapter. flat_ui_layer.h says what and why; the take, the tonemap re-issue, the
// door and the composite are the VR layer's own (ui_layer.cpp), driven here as eye 0. The pure rules are
// flat_ui_layer_math.h's.
#include "flat_ui_layer.h"

#include "flat_camera_phase.h"     // flatCameraMeasureRowShift: the jitter a draw's camera rows carry
#include "flat_compute_readback.h" // FlatComputeInternalScope
#include "flat_ui_layer_math.h"
#include "exposure_fix.h"         // lookupShaderHash: compare the physical shaders with the draw's cached pair
#include "tonemap_admit.h"        // read-only structural description of the actual game's tone-shaped draw
#include "ui_layer.h"
#include "vscreen.h"            // vScreenPSSetShaderResourcesRaw: the bloom feed binds past the shadow

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
    uint64_t toneCandidates = 0, toneProven = 0, toneAdmitted = 0, toneReissued = 0, toneDeclined = 0, toneByShape = 0;
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
    // The candidates of the tonemaps flatUiMapToneSlotOf names (the maps', the Full System Scanner's): the tone reads the candidate
    // branch saw, and those that proved.
    uint64_t mapToneCandidates = 0, mapToneMatched = 0;
    // The map families' draws by the kind their camera rows measured (flatUiJitterSlot: phase, zero, other, no rows), before
    // the no-cancel rule takes the cancel away (2026-10-10).
    uint64_t mapJitter[2][4] = {};
    // The HUD's bloom feed (flatUiGlowBegin): the first-level draws asked for H', by the pass's verdict (UiGlowFeed).
    uint64_t glow[static_cast<size_t>(UiGlowFeed::kCount)] = {};
    uint32_t glowW = 0, glowH = 0;  // the last fed H'
    // Every HDR-route trigger draw the adapter was asked about (flatUiGlowWanted), those the shape rule took for the bloom chain's first level, and the
    // last one's target and the scene's size: a window with triggers and no half-size ones is a game with Bloom off (or another chain), not a feed that
    // never ran.
    uint64_t glowTriggers = 0, glowHalf = 0;
    uint32_t glowLastTargetW = 0, glowLastTargetH = 0, glowLastHdrW = 0, glowLastHdrH = 0;
};
Window g_w;
bool g_glowNoted = false;  // the once-only first feed line
FlatUiLayerAsk g_lastAsk = FlatUiLayerAsk::kNotAsked;
UiLayerFamily g_decidedFamily = UiLayerFamily::kNone;  // the family of the last kDecided draw (flatUiLayerNoteIssue's)
bool g_mapCanvasNoted = false, g_mapSpriteNoted = false;  // the once-only first take of each map family
bool g_mapToneNoted[2] = {false, false};                  // the once-only first proof from a tonemap of this table: the maps', the scanner's
bool g_mapFirstToneNoted[2] = {false, false};             // the once-only first candidate line of each (proved or not)
FlatUiMapAaTally g_mapAa;                                 // temporal AA off on the maps and the Full System Scanner: the map line's counts
uint64_t g_mapAskFrame = 0, g_mapAsksFrame = 0;           // the frame of the last map ask, and how many map asks it has had
FlatUiToneProof g_proof;
bool g_sourceToneFirstNoted = false;
FlatUiUnknownSourceToneLog g_unknownSourceToneLog;
FlatUiTrail g_trail;   // the back-off trail (log only): what the HUD asked per frame, and each tone-shaped draw of it
FlatUiShapeToneLog g_shapeToneLog;   // the unknown pairs admitted by their physical shape, once each (process-lifetime, like the above)
// First physical RTV for each distinct, HDR-qualified HUD target in the current frame. The E894 and EDHM 0D43 UI-only tone guard
// requires this independent binding check before it may establish proof; references release at Present/device/resize.
struct HudPhysical {
    const void* shadow = nullptr;
    Ptr<ID3D11Resource> actual;
};
HudPhysical g_hudPhysical[4];
uint64_t g_hudPhysicalFrame = 0;
unsigned g_hudPhysicalCount = 0;
void clearHudPhysical(uint64_t frame) {
    for (auto& h : g_hudPhysical) h = HudPhysical{};
    g_hudPhysicalFrame = frame;
    g_hudPhysicalCount = 0;
}
void observeHudPhysical(ID3D11DeviceContext* ctx, const FlatUiLayerDraw& d) {
    if (g_hudPhysicalFrame != d.frame) clearHudPhysical(d.frame);
    for (unsigned i = 0; i < g_hudPhysicalCount; ++i)
        if (g_hudPhysical[i].shadow == d.color) return;
    if (g_hudPhysicalCount == 4) return;
    HudPhysical& h = g_hudPhysical[g_hudPhysicalCount++];
    h.shadow = d.color;
    FlatComputeInternalScope internal;
    ID3D11RenderTargetView* rawRtv = nullptr;
    ctx->OMGetRenderTargets(1, &rawRtv, nullptr);
    Ptr<ID3D11RenderTargetView> rtv; rtv.Attach(rawRtv);
    if (rtv) rtv->GetResource(&h.actual);
}
// Shared physical test for the admitted E894 and EDHM 0D43 pairs and diagnostic-only
// unknown pairs. The caller must hold FlatComputeInternalScope.
bool sourceTonePhysicalShape(ID3D11DeviceContext* ctx, uint64_t frame, uint32_t renderW, uint32_t renderH,
                             uint64_t vs, uint64_t ps, const void* cachedOutput,
                             uint32_t outW, uint32_t outH, bool unknownDiagnostic, ToneAdmit& shape, uint8_t* failed = nullptr) {
    // The first failing test is named for the back-off trail (flat_ui_layer_math.h's FlatUiTrailPhys); failed stays kNotRun when no test ran.
    const auto fail = [&](FlatUiTrailPhys p) {
        if (failed) *failed = static_cast<uint8_t>(p);
        return false;
    };
    if (!ctx || g_hudPhysicalFrame != frame || !g_hudPhysicalCount) return fail(FlatUiTrailPhys::kNoHud);
    Ptr<ID3D11VertexShader> actualVs;
    Ptr<ID3D11PixelShader> actualPs;
    ctx->VSGetShader(&actualVs, nullptr, nullptr);
    ctx->PSGetShader(&actualPs, nullptr, nullptr);
    if (lookupShaderHash(actualVs.Get()) != vs || lookupShaderHash(actualPs.Get()) != ps) return fail(FlatUiTrailPhys::kHash);
    if (!tonemapAdmitStructure(ctx, ps, shape)) return fail(FlatUiTrailPhys::kStructure);
    if (shape.hdrSlotRead != 1) return fail(FlatUiTrailPhys::kSlot);
    if (shape.rtvRes != cachedOutput) return fail(FlatUiTrailPhys::kRtv);
    D3D11_VIEWPORT viewport[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&viewportCount, viewport);
    const bool fullViewport = viewportCount == 1 && viewport[0].TopLeftX == 0.0f && viewport[0].TopLeftY == 0.0f &&
        viewport[0].Width == static_cast<float>(renderW) && viewport[0].Height == static_cast<float>(renderH) &&
        viewport[0].MinDepth == 0.0f && viewport[0].MaxDepth == 1.0f;
    const auto shapeMatches = unknownDiagnostic ? flatUiUnknownSourceToneShape : flatUiSourceToneShape;
    if (!shapeMatches(renderW, renderH, outW, outH, shape.rtvW, shape.rtvH,
                               static_cast<uint32_t>(shape.rtvFmt), shape.hdrW, shape.hdrH,
                               static_cast<uint32_t>(shape.hdrFmt), shape.psT0Is3D && shape.psT0Res != nullptr,
                               shape.hdr2D, fullViewport)) return fail(FlatUiTrailPhys::kShape);
    for (unsigned i = 0; i < g_hudPhysicalCount; ++i)
        if (flatUiSourceToneDirectHud(g_hudPhysical[i].shadow, g_hudPhysical[i].actual.Get(), shape.hdrRes)) {
            if (failed) *failed = static_cast<uint8_t>(FlatUiTrailPhys::kPass);
            return true;
        }
    return fail(FlatUiTrailPhys::kIdentity);
}
// The bloom input an unknown draw must bind at PS t2: a bound 2D view with a resource, or null. The probe's near-miss report and
// the shape admission both read it here, so the two cannot disagree on what the binding is. A 2D resource proves the binding
// shape, not what an unknown shader does with it.
Ptr<ID3D11Resource> toneBloomInput(ID3D11DeviceContext* ctx) {
    Ptr<ID3D11ShaderResourceView> bloom;
    ctx->PSGetShaderResources(2, 1, &bloom);
    if (!bloom) return {};
    D3D11_SHADER_RESOURCE_VIEW_DESC bloomView{};
    bloom->GetDesc(&bloomView);
    if (bloomView.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return {};
    Ptr<ID3D11Resource> bloomResource;
    bloom->GetResource(&bloomResource);
    return bloomResource;
}
uint32_t g_candidateLines = 0;  // eight tone candidates logged, re-armed after a route, render-size or swap-chain change
char g_candidateRoute[48] = "";
uint32_t g_candidateW = 0, g_candidateH = 0;
uint64_t g_takenFrame = 0;  // the frame of the first decided draw, for the map-family note

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
    // The half of the shared layer that governs this family must be live: with fix.ui_quality off and fix.sharp_weapons on, the layer runs for the weapons alone
    // and the HUD stays in the game's frame (not asked, so not counted); the converse for the weapons.
    if (!flatUiFamilyLive(family, uiLayerQualityLive(), uiLayerWeaponsLive(), uiLayerRippleLive())) return g_lastAsk;
    const size_t fi = static_cast<size_t>(family);
    const int ms = mapSlot(family);
    if (ms >= 0) {  // the map asks this frame, counted as they come (the first map-tone candidate reports how many came before it)
        if (g_mapAskFrame != d.frame) { g_mapAskFrame = d.frame; g_mapAsksFrame = 0; }
        ++g_mapAsksFrame;
    }
    ++g_w.asked[fi];
    flatUiTrailNoteAsk(d.frame, d.color);
    g_lastAsk = FlatUiLayerAsk::kRefused;
    const auto refuse = [&](FlatUiRefuse r) {
        ++g_w.refused[static_cast<size_t>(r)];
        flatUiTrailNoteRefuse(d.frame, r);
        if (ms >= 0) ++g_w.mapRefused[ms][static_cast<size_t>(r)];
        return g_lastAsk;
    };
    if (d.otherWork) return refuse(FlatUiRefuse::kOtherWork);
    if (!d.hdrTarget || !d.width || !d.height) return refuse(FlatUiRefuse::kNotHdrTarget);
    // The proof's subject: the target this frame's HUD families draw into, from the first ask, taken or not.
    flatUiToneProofHud(g_proof, d.frame, d.color);
    observeHudPhysical(ctx, d);  // read-only OM binding before any UI take or target substitution
    if (flatUiToneConsumed(g_proof, d.frame, d.color)) return refuse(FlatUiRefuse::kAfterTone);
    if (!flatUiToneProven(g_proof, d.frame, d.color)) return refuse(FlatUiRefuse::kToneUnproven);
    if (!d.upstream) return refuse(FlatUiRefuse::kNotUpstream);
    // The map families take no cancel and are never refused for their rows (flatUiLayerCancelOf): the rows are measured for
    // the per-family counts, and the draw is taken unjittered.
    const bool noCancel = flatUiLayerNoCancel(family);
    if (!d.haveRows && !noCancel) return refuse(FlatUiRefuse::kNoRows);
    double ndcX = 0.0, ndcY = 0.0;
    const bool measured = d.haveRows && flatCameraMeasureRowShift(d.rows, ndcX, ndcY);
    FlatUiJitterRead j = flatUiLayerJitterOf(measured, ndcX, ndcY, d.width, d.height, d.phaseX, d.phaseY);
    if (!d.haveRows) j.kind = FlatUiJitter::kNoRows;
    if (noCancel) {
        if (ms >= 0) ++g_w.mapJitter[ms][flatUiJitterSlot(j.kind)];
        j = flatUiLayerCancelOf(family, j);
    } else {
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
    if (g_takenFrame != d.frame) g_takenFrame = d.frame;
    flatUiTrailNoteTake(d.frame);
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
    if (flatUiLayerOn()) flatUiToneProofCopy(g_proof, frame, source, output);
}

// The back-off trail's hooks (flat_ui_layer.h): the record is g_trail, and each hook is a few loads.
void flatUiTrailNoteAsk(uint64_t frame, const void* hud) { g_trail.noteAsk(frame, hud); }
void flatUiTrailNoteTake(uint64_t frame) { g_trail.noteTake(frame); }
void flatUiTrailNoteRefuse(uint64_t frame, FlatUiRefuse r) { g_trail.noteRefuse(frame, r); }
void flatUiTrailNoteDraw(uint64_t frame, uint64_t vs, uint64_t ps, char kind, uint32_t count, bool uiEntered, bool hdrTrigger,
                         bool hdrTreated) {
    g_trail.noteDraw(frame, vs, ps, kind, count, uiEntered, hdrTrigger, hdrTreated);
}
void flatUiTrailNoteBranch(uint64_t frame, FlatUiTrailBranch b) { g_trail.noteBranch(frame, b); }
void flatUiTrailNotePhys(uint64_t frame, FlatUiTrailPhys p, const void* t1, const void* hudShadow, const void* hudActual) {
    g_trail.notePhys(frame, p, t1, hudShadow, hudActual);
}
void flatUiTrailNoteProven(uint64_t frame, bool proven) { g_trail.noteProven(frame, proven); }
void flatUiTrailNoteAdmission(uint64_t frame, const char* text) { g_trail.noteAdmission(frame, text); }
void flatUiTrailNoteReissue(uint64_t frame, FlatUiTrailReissue r, uint8_t declined, uint64_t seq) {
    g_trail.noteReissue(frame, r, declined, seq);
}
// Logs the previous and this frame of the back-off trail while the session has trails left: twelve, and the twelfth says it is the last.
void flatUiTrailReport() {
    const uint32_t trail = g_trail.claimReport();
    if (!trail) return;
    char line[1000];   // one trail line: its prefix and body (at most 899 characters), inside the logger's line cut
    flatUiTrailLine(line, sizeof line, trail, true, g_trail.prev);
    Log::get().note("%s", line);
    flatUiTrailLine(line, sizeof line, trail, false, g_trail.cur);
    Log::get().note("%s", line);
}
// The physical check's result for the trail: its t1 input, and the frame's first HUD target (its binding shadow and physical RTV).
void noteTrailPhysical(uint64_t frame, uint8_t failed, const ToneAdmit& shape) {
    const void* shadow = g_hudPhysicalCount ? g_hudPhysical[0].shadow : nullptr;
    const void* actual = g_hudPhysicalCount ? g_hudPhysical[0].actual.Get() : nullptr;
    flatUiTrailNotePhys(frame, static_cast<FlatUiTrailPhys>(failed), shape.hdrRes, shadow, actual);
}
bool flatUiLayerSourceToneCandidate(ID3D11DeviceContext* ctx, uint64_t frame, uint32_t renderW, uint32_t renderH,
                                    uint64_t vs, uint64_t ps, const void* cachedOutput,
                                    uint32_t outW, uint32_t outH, const char* route) {
    if (!ctx || !flatUiLayerOn() || !flatUiSourceTonePair(vs, ps) || g_hudPhysicalFrame != frame) return false;
    FlatComputeInternalScope internal;
    ToneAdmit shape{};
    uint8_t failed = 0;
    const bool physical = sourceTonePhysicalShape(ctx, frame, renderW, renderH, vs, ps, cachedOutput, outW, outH, false, shape, &failed);
    noteTrailPhysical(frame, failed, shape);
    if (!physical)
        return false;  // no proof from a cached shadow or an unverified alias
    if (!g_sourceToneFirstNoted) {
        g_sourceToneFirstNoted = true;
        Log::get().note("flat ui layer: first E894/0D43 source tone qualified -- frame %llu VS %016llX PS %016llX, "
                        "physical HUD t1=%p, LDR RTV=%p R=%ux%u, full viewport; flat UI proof only.",
                        static_cast<unsigned long long>(frame), static_cast<unsigned long long>(vs),
                        static_cast<unsigned long long>(ps), shape.hdrRes, shape.rtvRes, renderW, renderH);
    }
    return flatUiLayerToneCandidate(ctx, frame, renderW, renderH, 1, vs, ps, shape.hdrRes, outW, outH, route);
}

// Sean's 2026-10-15 decision: an unknown pair that passes the full physical shape grants flat UI proof, as a named pair does. The
// shape is the strict one (sourceTonePhysicalShape with LDR format 28: the HDR at t1 a physical HUD target, a 3D LUT at t0, the
// full viewport, one RTV, no DSV or blend), and the bloom at PS t2 is a bound 2D view. The admission is the named pair's own proof.
bool flatUiLayerShapeToneCandidate(ID3D11DeviceContext* ctx, uint64_t frame, uint32_t renderW, uint32_t renderH,
                                   uint64_t vs, uint64_t ps, const void* cachedOutput, uint32_t outW, uint32_t outH,
                                   uint32_t startInstance, const char* route, bool& admitted) {
    admitted = false;
    if (!ctx || !flatUiLayerOn() || g_hudPhysicalFrame != frame || !g_hudPhysicalCount || startInstance != 0 ||
        !flatUiShapeToneVs(vs) || flatUiSourceTonePair(vs, ps)) return false;   // a named pair has its own path
    FlatComputeInternalScope internal;
    ToneAdmit shape{};
    uint8_t failed = 0;
    const bool physical = sourceTonePhysicalShape(ctx, frame, renderW, renderH, vs, ps, cachedOutput, outW, outH,
                                                  /*unknownDiagnostic=*/false, shape, &failed);
    noteTrailPhysical(frame, failed, shape);
    if (!physical)
        return false;  // no proof from a cached shadow, an unverified alias or a shape off the strict format
    if (!toneBloomInput(ctx)) return false;
    ++g_w.toneByShape;
    const auto result = g_shapeToneLog.record(vs, ps);
    if (result == FlatUiShapeToneLog::Result::NewPair)
        Log::get().note("flat ui layer: tone pair admitted by its physical shape -- frame %llu VS %016llX PS %016llX: the physical HUD "
                        "target at t1, LDR RTV %p fmt 28 R=%ux%u, a 2D at t2, full viewport. Flat UI proof only; the pair is not in "
                        "any table (2026-10-15 rule).",
                        static_cast<unsigned long long>(frame), static_cast<unsigned long long>(vs),
                        static_cast<unsigned long long>(ps), shape.rtvRes, renderW, renderH);
    else if (result == FlatUiShapeToneLog::Result::CapacityReached)
        Log::get().note("flat ui layer: tone pairs admitted by shape capacity=%u exhausted; later new pairs are admitted without a line",
                        FlatUiShapeToneLog::kCapacity);
    admitted = flatUiLayerToneCandidate(ctx, frame, renderW, renderH, 1, vs, ps, shape.hdrRes, outW, outH, route);
    return true;
}

void flatUiLayerProbeUnknownSourceTone(ID3D11DeviceContext* ctx, uint64_t frame, uint32_t renderW, uint32_t renderH,
                                       uint64_t vs, uint64_t ps, const void* cachedOutput,
                                       uint32_t outW, uint32_t outH, char kind, uint32_t count,
                                       uint32_t instances, uint32_t startInstance) {
    if (!ctx || !flatUiLayerOn() || !g_hudPhysicalCount || flatUiSourceTonePair(vs, ps) ||
        (kind != 'D' && kind != 'N') || count != 3 || instances != 1 || startInstance != 0 ||
        !g_unknownSourceToneLog.shouldInspect(vs, ps)) return;
    FlatComputeInternalScope internal;
    ToneAdmit shape{};
    const auto reject = [&]() {
        if (g_unknownSourceToneLog.reject(vs, ps))
            Log::get().note("flat ui layer: unknown source-tone rejection capacity=%u exhausted; later unknown pairs are not inspected",
                            FlatUiUnknownSourceToneLog::kCapacity);
    };
    if (!sourceTonePhysicalShape(ctx, frame, renderW, renderH, vs, ps, cachedOutput, outW, outH, true, shape)) { reject(); return; }
    // The unknown draw must also bind the measured bloom input at PS t2 (toneBloomInput, which the shape admission reads too).
    const Ptr<ID3D11Resource> bloomResource = toneBloomInput(ctx);
    if (!bloomResource) { reject(); return; }
    const auto result = g_unknownSourceToneLog.record(vs, ps);
    if (result == FlatUiUnknownSourceToneLog::Result::NewPair)
        Log::get().note("flat ui layer: unknown source-tone probe -- frame %llu VS %016llX PS %016llX, physical HUD t1=%p, LDR RTV=%p fmt=%u, bound 2D t2=%p R=%ux%u; a near miss: it passed the diagnostic shape but not the admission's (LDR format 28, a tone VS, start instance 0); no proof",
                        static_cast<unsigned long long>(frame), static_cast<unsigned long long>(vs),
                        static_cast<unsigned long long>(ps), shape.hdrRes, shape.rtvRes,
                        static_cast<unsigned>(shape.rtvFmt),
                        bloomResource.Get(), renderW, renderH);
    else if (result == FlatUiUnknownSourceToneLog::Result::CapacityReached)
        Log::get().note("flat ui layer: unknown source-tone probe capacity=%u exhausted; later unknown pairs are unreported",
                        FlatUiUnknownSourceToneLog::kCapacity);
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
    // The alias the admission may accept: the bound tone input's copy and
    // its own source target. Admission checks that this source holds fresh
    // HUD or weapon content; the first draw of another family cannot select
    // an unrelated alias for the tone.
    const void* takenAlias = nullptr;
    const void* aliasSource = nullptr;
    for (uint32_t i = 0; i < g_proof.count; ++i)
        if (input && input == g_proof.alias[i]) {
            takenAlias = input;
            aliasSource = g_proof.hud[i];
            break;
        }
    const bool proven = flatUiToneProofTone(g_proof, frame, input) != nullptr;
    flatUiTrailNoteProven(frame, proven);
    if (proven) ++g_w.toneProven;
    // The candidates of the tonemaps flatUiMapToneSlotOf names (the maps' and, since 2026-10-22, the Full System Scanner's), counted every
    // time, and the first one of each said once, proved or not, with the pointer it compared and the targets recorded this frame: a null
    // input, no recorded target or a pointer mismatch is then visible in the log, not silent. The map line's tone counts hold both.
    const FlatUiToneScreen toneScreen = flatUiMapToneScreenOf(vs, ps);
    const bool scanner = toneScreen == FlatUiToneScreen::kScanner;
    const bool mapTone = toneScreen != FlatUiToneScreen::kNone && flatUiMapToneSlotOf(vs, ps) == static_cast<uint32_t>(hdrSlot);
    if (mapTone) {
        ++g_w.mapToneCandidates;
        if (proven) ++g_w.mapToneMatched;
        if (!g_mapFirstToneNoted[scanner]) {
            g_mapFirstToneNoted[scanner] = true;
            char asks[48] = "";
            if (!scanner)
                std::snprintf(asks, sizeof(asks), ", map asks this frame %llu",
                              static_cast<unsigned long long>(g_mapAskFrame == frame ? g_mapAsksFrame : 0));
            Log::get().note("flat ui layer: first %s candidate -- frame %llu, input %p, recorded targets %s, match %s%s; said once.",
                            scanner ? "Full System Scanner tone" : "map-tone", static_cast<unsigned long long>(frame), input,
                            used ? targets : "none", flatUiToneMatchWhy(g_proof, frame, input), asks);
        }
    }
    // The first proof a tonemap of this table records (the UI layer's own table): said once for each, with its frame.
    if (proven && mapTone && !g_mapToneNoted[scanner]) {
        g_mapToneNoted[scanner] = true;
        Log::get().note("flat ui layer: %s tone proven by VS %016llX PS %016llX reading slot %d, frame %llu (the proof "
                        "holds for frame %llu); said once.",
                        scanner ? "Full System Scanner" : "map-frame", static_cast<unsigned long long>(vs),
                        static_cast<unsigned long long>(ps), hdrSlot, static_cast<unsigned long long>(frame),
                        static_cast<unsigned long long>(frame + 1));
    }
    bool admitted = false;
    char why[96] = "not asked: no HUD or weapons were taken this frame";
    {
        FlatComputeInternalScope internal;
        uiLayerFlatSetDraw(frame, 0.0f, 0.0f, renderW, renderH);  // the sequence only
        admitted = uiLayerCrispAdmitFlat(ctx, hdrSlot, takenAlias, aliasSource, vs, ps, why, sizeof(why)) == 1;
    }
    if (admitted) ++g_w.toneAdmitted;
    flatUiTrailNoteAdmission(frame, why);
    if (g_candidateLines < 8) {
        ++g_candidateLines;
        Log::get().note("flat ui layer: tone candidate %u: frame %llu vs %016llX ps %016llX, HDR slot t%d reads %p, output %ux%u, "
                        "render %ux%u, route %s; this frame's HDR content targets: %s; proof for the next frame: %s; re-issue: %s.",
                        g_candidateLines, static_cast<unsigned long long>(frame), static_cast<unsigned long long>(vs),
                        static_cast<unsigned long long>(ps), hdrSlot, input, outW, outH, renderW, renderH, route,
                        used ? targets : "none asked yet", proven ? "yes" : used ? "no (reads none of them)" : "no (no HDR content ask yet)",
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

bool flatUiLayerToneNext(ID3D11DeviceContext* ctx) {
    FlatComputeInternalScope internal;
    return uiLayerCrispToneNext(ctx);
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

bool flatUiGlowWanted(bool triggerSrvKnown, bool triggerAmbiguous, uint32_t srvSlot, uint32_t hdrW, uint32_t hdrH, uint32_t targetW,
                      uint32_t targetH, bool starLive) {
    const bool wanted = flatUiGlowFeedAsked(flatUiLayerOn(), starLive, triggerSrvKnown, triggerAmbiguous, srvSlot, hdrW, hdrH, targetW, targetH);
    if (flatUiLayerOn()) {
        ++g_w.glowTriggers;
        if (wanted) ++g_w.glowHalf;
        g_w.glowLastTargetW = targetW;
        g_w.glowLastTargetH = targetH;
        g_w.glowLastHdrW = hdrW;
        g_w.glowLastHdrH = hdrH;
    }
    return wanted;
}

bool flatUiGlowBegin(ID3D11DeviceContext* ctx, uint64_t frame, uint32_t slot, const void* hdr, uint64_t vs, uint64_t ps, uint32_t hdrW,
                     uint32_t hdrH, uint32_t targetW, uint32_t targetH, ID3D11ShaderResourceView** original, UiGlowStar* star) {
    // Asked by the HUD layer (fix.ui_quality) or by the star glow's input (visual.star_glow): either alone makes H'.
    const bool layerOn = flatUiLayerOn();
    if (!ctx || !original || *original || !(layerOn || star)) return false;
    FlatComputeInternalScope internal;
    Ptr<ID3D11ShaderResourceView> game;
    ctx->PSGetShaderResources(slot, 1, &game);
    int verdict = static_cast<int>(UiGlowFeed::kNotReady);
    ID3D11ShaderResourceView* fed = uiLayerGlowFeed(ctx, frame, slot, hdr, &verdict, star);
    if (verdict < 0 || verdict >= static_cast<int>(UiGlowFeed::kCount)) verdict = static_cast<int>(UiGlowFeed::kFault);
    // The HUD's own counts: a feed that was the star light alone (no HUD in the layer this frame) is the star line's, not this layer's.
    const bool hudFed = !star || star->hudIncluded || verdict != static_cast<int>(UiGlowFeed::kFed);
    if (layerOn && hudFed) ++g_w.glow[verdict];
    if (!fed || !game) return false;
    ID3D11ShaderResourceView* bound = fed;
    vScreenPSSetShaderResourcesRaw(ctx, slot, 1, &bound);
    *original = game.Detach();
    g_w.glowW = hdrW;
    g_w.glowH = hdrH;
    if (layerOn && hudFed && !g_glowNoted) {
        g_glowNoted = true;
        Log::get().note("flat ui layer glow: the HUD's light joins the game's bloom -- the first-level draw vs %016llX ps %016llX (target %ux%u of the "
                        "scene's %ux%u) reads H' = H + the HUD layer at t%u from frame %llu on; the chain, its tier and the tone are the game's.",
                        static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps), targetW, targetH, hdrW, hdrH, slot,
                        static_cast<unsigned long long>(frame));
    }
    return true;
}

void flatUiGlowEnd(ID3D11DeviceContext* ctx, uint32_t slot, ID3D11ShaderResourceView* original) {
    if (!original) return;
    if (ctx) {
        FlatComputeInternalScope internal;
        ID3D11ShaderResourceView* game = original;
        vScreenPSSetShaderResourcesRaw(ctx, slot, 1, &game);
    }
    original->Release();
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
    // `treated`: the picture is the one EDVR's resolve made (unjittered), which the shield ripple reads at D minus the jitter; the game's own is left alone.
    ID3D11Texture2D* out = uiLayerComposite(frame, 0, picture.Get(), region, uv, false, treated);
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
        clearHudPhysical(0);
        g_sourceToneFirstNoted = false;
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
    clearHudPhysical(0);
    g_sourceToneFirstNoted = false;
    g_candidateLines = 0;         // and its first tone candidates are logged
    g_takenFrame = 0;
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
void flatUiLayerMapAaCopy(bool ok, bool easu, bool smaa) { g_mapAa.copy(ok, easu, smaa); }

void flatUiLayerFrame(bool mapOpen, uint64_t nextFrame) {
    if (!runtimeFlatProfile()) return;
    clearHudPhysical(nextFrame);  // release the previous frame's physical RTV resources at Present
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
        "jitter-zero=%llu tone-by-shape=%llu",
        static_cast<unsigned long long>(windowSeconds), flatUiLayerState(), static_cast<unsigned long long>(w.copies),
        static_cast<unsigned long long>(w.doorsArmed), static_cast<unsigned long long>(w.doorUntreated),
        static_cast<unsigned long long>(w.doorSize), w.inW, w.inH, w.outW, w.outH,
        static_cast<unsigned long long>(w.toneCandidates), static_cast<unsigned long long>(w.toneProven),
        static_cast<unsigned long long>(w.toneAdmitted), static_cast<unsigned long long>(w.toneReissued),
        static_cast<unsigned long long>(w.toneDeclined), static_cast<unsigned long long>(w.composites),
        static_cast<unsigned long long>(w.compositeNone), static_cast<unsigned long long>(w.bindFailed),
        static_cast<unsigned long long>(w.writeBacks), static_cast<unsigned long long>(w.jitterPhase),
        static_cast<unsigned long long>(w.jitterZero), static_cast<unsigned long long>(w.toneByShape));
    const UiLayerFamily families[4] = {UiLayerFamily::kHolo, UiLayerFamily::kFlightHud, UiLayerFamily::kSprite,
                                       UiLayerFamily::kHoloGeneric};
    const size_t f0 = static_cast<size_t>(families[0]), f1 = static_cast<size_t>(families[1]),
                 f2 = static_cast<size_t>(families[2]), f3 = static_cast<size_t>(families[3]);
    const size_t fc = static_cast<size_t>(UiLayerFamily::kMapCanvas), fs = static_cast<size_t>(UiLayerFamily::kMapSprite);
    // fix.sharp_weapons in flat (2026-10-16): the weapons fire and the shield ripple, each asked/decided/taken/refused-at-issue. Zeros print, and a window in which
    // neither pair reached the adapter says so.
    {
        const size_t fw = static_cast<size_t>(UiLayerFamily::kWeaponsFire), fr = static_cast<size_t>(UiLayerFamily::kShieldRipple);
        if (!w.asked[fw] && !w.asked[fr]) {
            Log::get().note("flat ui layer weapons: not exercised in this window (no weapons-fire or shield-refraction draw was asked; fix.sharp_weapons %s).",
                            uiLayerWeaponsLive() ? "is live" : "is not live: off, or flat anti-aliasing off, or stood down");
        } else {
            Log::get().note(
                "flat ui layer weapons: weapons-fire asked=%llu decided=%llu taken=%llu refused-at-issue=%llu; shield-ripple asked=%llu decided=%llu taken=%llu "
                "refused-at-issue=%llu",
                static_cast<unsigned long long>(w.asked[fw]), static_cast<unsigned long long>(w.decided[fw]), static_cast<unsigned long long>(w.taken[fw]),
                static_cast<unsigned long long>(w.atIssue[fw]), static_cast<unsigned long long>(w.asked[fr]), static_cast<unsigned long long>(w.decided[fr]),
                static_cast<unsigned long long>(w.taken[fr]), static_cast<unsigned long long>(w.atIssue[fr]));
        }
    }
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
    // open-frames and the families are the three maps'; the tone counts (tone-candidates-map, tone-matched-map: the tonemaps
    // flatUiMapToneSlotOf names) and temporal-off with the counts after it also count the Full System Scanner's frames (2026-10-22),
    // so a scanner window reads open-frames=0 with the tone counts and temporal-off above zero.
    Log::get().note(
        "flat ui layer map: open-frames=%llu; canvas asked=%llu taken=%llu refused=%llu (%s); sprite asked=%llu taken=%llu "
        "refused=%llu (%s); tone-candidates-map=%llu tone-matched-map=%llu; temporal-off=%llu jitter-zeroed=%llu spatial=%llu "
        "spatial-refused=%llu no-copy=%llu resets=%llu spatial-easu=%llu spatial-smaa=%llu; canvas rows phase=%llu zero=%llu other=%llu no-rows=%llu; sprite rows "
        "phase=%llu zero=%llu other=%llu no-rows=%llu",
        static_cast<unsigned long long>(w.mapOpenFrames), static_cast<unsigned long long>(w.asked[fc]),
        static_cast<unsigned long long>(w.taken[fc]), static_cast<unsigned long long>(mapRefusedTotal(w, 0, w.atIssue[fc])),
        mapReasons(w, 0, w.atIssue[fc]).c_str(), static_cast<unsigned long long>(w.asked[fs]),
        static_cast<unsigned long long>(w.taken[fs]), static_cast<unsigned long long>(mapRefusedTotal(w, 1, w.atIssue[fs])),
        mapReasons(w, 1, w.atIssue[fs]).c_str(), static_cast<unsigned long long>(w.mapToneCandidates),
        static_cast<unsigned long long>(w.mapToneMatched), static_cast<unsigned long long>(g_mapAa.temporalOff),
        static_cast<unsigned long long>(g_mapAa.jitterZeroed), static_cast<unsigned long long>(g_mapAa.spatial),
        static_cast<unsigned long long>(g_mapAa.spatialRefused), static_cast<unsigned long long>(g_mapAa.noCopy),
        static_cast<unsigned long long>(g_mapAa.resets), static_cast<unsigned long long>(g_mapAa.spatialEasu),
        static_cast<unsigned long long>(g_mapAa.spatialSmaa),
        static_cast<unsigned long long>(w.mapJitter[0][0]), static_cast<unsigned long long>(w.mapJitter[0][1]),
        static_cast<unsigned long long>(w.mapJitter[0][2]), static_cast<unsigned long long>(w.mapJitter[0][3]),
        static_cast<unsigned long long>(w.mapJitter[1][0]), static_cast<unsigned long long>(w.mapJitter[1][1]),
        static_cast<unsigned long long>(w.mapJitter[1][2]), static_cast<unsigned long long>(w.mapJitter[1][3]));
    // The HUD's bloom feed: every first-level draw that asked for H' this window, by what the pass came to. "fed" is the draws that read H'; a
    // window with the layer live and the HUD taken but nothing here is a build that never ran it, or a game with Bloom off (no first level).
    {
        char verdicts[200] = "";
        size_t used2 = 0;
        for (size_t v = 0; v < static_cast<size_t>(UiGlowFeed::kCount) && used2 < sizeof(verdicts); ++v) {
            const int n = std::snprintf(verdicts + used2, sizeof(verdicts) - used2, "%s%s=%llu", used2 ? " " : "",
                                        uiGlowFeedName(static_cast<UiGlowFeed>(v)), static_cast<unsigned long long>(w.glow[v]));
            if (n < 0) break;
            used2 += static_cast<size_t>(n);
        }
        Log::get().note("flat ui layer glow: triggers=%llu half-size=%llu (last trigger target %ux%u of the scene's %ux%u); %s (last H' %ux%u)",
                        static_cast<unsigned long long>(w.glowTriggers), static_cast<unsigned long long>(w.glowHalf), w.glowLastTargetW,
                        w.glowLastTargetH, w.glowLastHdrW, w.glowLastHdrH, verdicts, w.glowW, w.glowH);
    }
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
