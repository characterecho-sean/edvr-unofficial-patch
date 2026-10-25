// The in-world screens' coverage (F105; docs\design-native-onfoot-vr-2026-10-13.md, journal 2026-10-24 "F105"): the
// terminals' screens on foot, and every other screen the game draws the same way. Header-only: the pair and its test,
// the 20 s window and its line, and the pass's D3D half, which the private UI depth rig (world_screen_coverage_test.h)
// drives on WARP. ui_depth.cpp classifies the draw and owns the masks; vscreen.cpp issues the draw again between
// begin() and end().
//
// WHY. The game draws a terminal's interface as the EMISSIVE term of a lit surface in its G-buffer pass: VS
// 889A5279E68F0672 with PS EBA95E15B0A66102, four colour targets, the interface sampled from a texture array at t2.
// That is a surface, not a composite into the eye, so the UI layer cannot take it, and ui depth had no coverage
// stand-in for it (the flight log's "ui depth: a new interface family -- vs 889A5279E68F0672 ps EBA95E15B0A66102 ...
// left alone -- this composite still swims"). Its pixels were never in the coverage mask, so the UI resolve -- the pass
// that holds interface pixels to the current frame after DLSS, because the transformer presets ignore the bias mask --
// never touched them, and DLSS alone reconstructed one-pixel interface text under the jitter. It does not manage it:
// in eye dump 221109514003 (Epic, Quest 3 over Virtual Desktop, 1536x1632 render, DLSS preset K) the raw frame's
// tooltip is crisp in all 16 frames and DLSS's output is dim, striped and doubled; 0.4 to 0.9% of the screen's output
// pixels lie outside the raw 2x2 range by more than 8/255, against 0.04% on the terminal's body beside it, and the
// motion vectors on the screen match the content's measured shift to 0.2 px. Replayed offline, the UI resolve's
// rebuild (Catmull-Rom of the raw frame at the unjittered position, bounded by the raw 2x2) gives solid single text in
// every frame; its clamp alone does not.
//
// WHAT. Right after the game's draw of the pair, with the game's state still bound, the draw is issued once more
// through kWorldScreenCoverageHlsl into the eye's coverage mask (2/255: an attached stroke, so the engine-record motion
// stays the pixel's; the floating value 1 would hand the pixel the camera's path) and the eye's edit mask (1), against
// the game's own depth with GREATER_EQUAL and no writes: the same vertices give the same depth, so the pass marks
// exactly where the screen won. The UI resolve then holds the screen to the current frame, and rebuilds it from the
// current frame wherever DLSS departs from it by more than its tolerance. No game target is written and the game's
// depth is only read.
//
// LIMITS. The masks are marked in the G-buffer pass, so geometry drawn later in front of a screen (a hand, a weapon)
// is marked where it covers the screen, and the resolve treats those pixels as the screen's. The pass marks only while
// an external upscaler runs (ui depth's trained flag); EDVR's own temporal AA is left as it was. The pair is exact: a
// game build that changes either shader leaves the screens as they were, and the 20 s line then counts nothing seen.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <d3d11.h>
#include <wrl/client.h>
#include "vscreen.h"   // vScreenSetRenderTargetsRaw: the rebind past the binding shadow
#include "shader_swap.h"                 // shaderSwapCreateCs: F108's census shader
#include "temporal_shader_bytecode.h"    // kWorldScreenCensusBytecode (tools\temporal_shader_build)

// F108 (docs\design-native-onfoot-vr-2026-10-13.md, journal 2026-10-25 "F108"): F105 held every screen pixel every frame,
// so the eye got the render-resolution rebuild over the whole interface, and the text read low resolution: in eye dump
// 045208629001 (Pimax Crystal Super, 2016x2017 render, 2x DLSS preset K) the resolve replaced DLSS's output at 52 to 75%
// of the interface's detail pixels, and DLSS's own output from the same frames reads "PHAEACIA / MILITARY" cleanly where
// the eye got broken strokes. Now the screen is held only where its interface changed: UiContent digests the interface
// texture (a Texture2DArray) per 4x4 block and slice, and the tracked stand-in (kWorldScreenTrackedHlsl) reads the game
// shader's own slice and uv, marks the edit mask only where those blocks changed in the last 32 frames, and on a change's
// first frame sets bit 2 of the coverage value (6), where the motion pass restarts NVIDIA's history (the size*2 value,
// which preset K honours: docs\terrain-history-shimmer-2026-09-17.md). At rest the screen is DLSS's. An interface texture
// UiContent cannot track keeps F105's rule (held everywhere), counted and named. Each pass also writes the screen's state
// (1 at rest, 2 edited, 3 a change's first frame) into a per-eye target the census counts every frame a screen was drawn.

namespace edvr {

constexpr uint64_t kWorldScreenVs = 0x889A5279E68F0672ull;
constexpr uint64_t kWorldScreenPs = 0xEBA95E15B0A66102ull;
// The values the stand-ins write, as the 8-bit masks hold them: the coverage mask's attached stroke, the same with bit 2
// on an edit's first frame (F108), the edit mask's mark (F105's held rule writes it everywhere), and the census state.
// The rig reads them back.
constexpr uint32_t kWorldScreenMaskValue = 2;
constexpr uint32_t kWorldScreenFreshMaskValue = 6;
constexpr uint32_t kWorldScreenEditValue = 255;
constexpr uint32_t kWorldScreenStateRest = 1, kWorldScreenStateEdited = 2, kWorldScreenStateFresh = 3;
// UiContent's edit expiry (the age falls 1/32 a frame): a census frame this many frames or more after the last change
// began is at rest.
constexpr uint32_t kWorldScreenRestFrames = 32;

// The draw this marks: the exact pair, sampling a learned interface surface (ui depth's composite test), drawn into
// the scene's depth, while an external upscaler runs. Anything else is left exactly as before.
inline bool worldScreenDraw(uint64_t vs, uint64_t ps, bool composite, bool sceneDepth, bool trained) {
    return trained && composite && sceneDepth && vs == kWorldScreenVs && ps == kWorldScreenPs;
}

// F108: why a draw of the pair was not taken at the classification, the first of F105's tests it failed. A draw with
// no learned surface bound may be no interface at all (the shader's solid-colour instances); the other two are screens
// left to DLSS alone (the carrier's 04:59:55 `left alone` line, 2026-10-25, was one, its reason then unlogged).
enum class WorldScreenClassRefusal : uint8_t { kNone, kNoSurface, kNotSceneDepth, kNotTrained, kCount };
inline WorldScreenClassRefusal worldScreenClassRefusal(bool composite, bool sceneDepth, bool trained) {
    if (!composite) return WorldScreenClassRefusal::kNoSurface;
    if (!sceneDepth) return WorldScreenClassRefusal::kNotSceneDepth;
    if (!trained) return WorldScreenClassRefusal::kNotTrained;
    return WorldScreenClassRefusal::kNone;
}
inline const char* worldScreenClassRefusalName(WorldScreenClassRefusal r) {
    switch (r) {
        case WorldScreenClassRefusal::kNoSurface: return "no learned surface";
        case WorldScreenClassRefusal::kNotSceneDepth: return "not the scene's depth";
        case WorldScreenClassRefusal::kNotTrained: return "no external upscaler";
        default: return "none";
    }
}

// Why a classified draw was not marked.
enum class WorldScreenRefusal : uint8_t { kNoEye, kNoMask, kShader, kState, kCount };
inline const char* worldScreenRefusalName(WorldScreenRefusal r) {
    switch (r) {
        case WorldScreenRefusal::kNoEye: return "no eye";
        case WorldScreenRefusal::kNoMask: return "no mask";
        case WorldScreenRefusal::kShader: return "no shader";
        case WorldScreenRefusal::kState: return "state";
        default: return "?";
    }
}

// The window, zeros included: no line at all is the code never having run (no external upscaler, or a build without
// it); seen 0 is no screen drawn; seen above marked is a refusal, named.
struct WorldScreenWindow {
    uint32_t frames = 0;
    uint32_t seen = 0;      // draws of the pair classified
    uint32_t marked = 0;    // ...issued into the masks
    uint32_t eye[2] = {};   // marked, by eye
    uint32_t refused[static_cast<size_t>(WorldScreenRefusal::kCount)] = {};
    // F108: of the marked, those held only where the interface changed, and those held everywhere (F105's rule) because
    // UiContent could not track the interface texture, by UiContent's decision (its enum's value; the line names them).
    uint32_t tracked = 0, held = 0;
    uint32_t heldWhy[16] = {};
    // F108: draws of the pair not taken at the classification, by the first test they failed.
    uint32_t classRefused[static_cast<size_t>(WorldScreenClassRefusal::kCount)] = {};
    void refuse(WorldScreenRefusal r) { ++refused[static_cast<size_t>(r)]; }
    void reset() { *this = WorldScreenWindow{}; }
};

// F108's census: one row a frame in which a screen was drawn (the per-eye state targets counted, both eyes summed), read
// back a few frames later and added in frame order. A frame is DURING a change from the frame a change began (a fresh
// pixel) until kWorldScreenRestFrames frames later, and AT REST otherwise; the edited fraction at rest is the hold's
// leftover (near 0 when it works, 1 under F105's rule). The last change survives a window, so the boundary does not
// call the frames after a change at rest.
struct WorldScreenCensus {
    bool haveChange = false;
    uint64_t lastChange = 0;
    uint32_t frames = 0, editFrames = 0, changeFrames = 0, restFrames = 0, duringFrames = 0, skipped = 0;
    uint64_t restScreen = 0, restEdited = 0, duringScreen = 0, duringEdited = 0, freshPx = 0;
    void add(uint64_t frame, uint32_t screen, uint32_t edited, uint32_t fresh) {
        if (!screen) return;
        ++frames;
        if (edited) ++editFrames;
        if (fresh) {
            ++changeFrames;
            freshPx += fresh;
            haveChange = true;
            lastChange = frame;
        }
        if (haveChange && frame >= lastChange && frame - lastChange < kWorldScreenRestFrames) {
            ++duringFrames;
            duringScreen += screen;
            duringEdited += edited;
        } else {
            ++restFrames;
            restScreen += screen;
            restEdited += edited;
        }
    }
    void resetWindow() {
        const bool keep = haveChange;
        const uint64_t last = lastChange;
        *this = WorldScreenCensus{};
        haveChange = keep;
        lastChange = last;
    }
};

// F108's line (the Log adds the time); tools\edvr_log.py --health's `world screens (F108)` reads it by its first words.
// heldWhy: UiContent's decisions for the held draws, as text ("none" when every marked draw was tracked).
inline int worldScreenFormatF108(char* out, size_t size, const WorldScreenWindow& w, const WorldScreenCensus& c, const char* heldWhy) {
    return std::snprintf(out, size,
        "ui depth world screens (F108): %u frames; draws tracked %u, held without change tracking %u (%s); not taken at the "
        "classification: no learned surface %u, not the scene's depth %u, no external upscaler %u; census %u frames with a "
        "screen (%u skipped): frames with an edit %u, a change began %u; at rest %u frames, edited %llu of %llu px; during a "
        "change %u frames, edited %llu of %llu px; history restarts %llu px in %u frames.",
        w.frames, w.tracked, w.held, heldWhy ? heldWhy : "?",
        w.classRefused[static_cast<size_t>(WorldScreenClassRefusal::kNoSurface)],
        w.classRefused[static_cast<size_t>(WorldScreenClassRefusal::kNotSceneDepth)],
        w.classRefused[static_cast<size_t>(WorldScreenClassRefusal::kNotTrained)],
        c.frames, c.skipped, c.editFrames, c.changeFrames, c.restFrames, static_cast<unsigned long long>(c.restEdited),
        static_cast<unsigned long long>(c.restScreen), c.duringFrames, static_cast<unsigned long long>(c.duringEdited),
        static_cast<unsigned long long>(c.duringScreen), static_cast<unsigned long long>(c.freshPx), c.changeFrames);
}

// The line (the Log adds the time); tools\edvr_log.py --health reads it by its first words.
inline int worldScreenFormatWindow(char* out, size_t size, const WorldScreenWindow& w) {
    return std::snprintf(out, size,
        "ui depth world screens (F105): %u frames, seen %u, marked %u (eye 0 %u, eye 1 %u); refused: no eye %u, no mask %u, "
        "no shader %u, state %u. Marked screens are held to the current frame by the UI resolve after DLSS "
        "(vs %016llX ps %016llX, the terminals' interface drawn as a lit surface).",
        w.frames, w.seen, w.marked, w.eye[0], w.eye[1],
        w.refused[static_cast<size_t>(WorldScreenRefusal::kNoEye)], w.refused[static_cast<size_t>(WorldScreenRefusal::kNoMask)],
        w.refused[static_cast<size_t>(WorldScreenRefusal::kShader)], w.refused[static_cast<size_t>(WorldScreenRefusal::kState)],
        static_cast<unsigned long long>(kWorldScreenVs), static_cast<unsigned long long>(kWorldScreenPs));
}

// The pass's D3D half: save what it changes, bind the masks over the game's own depth with the given states and the
// stand-in, and put everything back. begin() refuses (and changes nothing) when the draw's state is one it could not
// put back exactly: unordered-access views beside the colour targets, stream-out, predication, no depth.
class WorldScreenCoverage {
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    ID3D11RenderTargetView* targets_[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    Ptr<ID3D11DepthStencilView> depth_;
    Ptr<ID3D11DepthStencilState> depthState_;
    Ptr<ID3D11BlendState> blend_;
    Ptr<ID3D11PixelShader> ps_;
    Ptr<ID3D11ShaderResourceView> savedAges_;   // F108: the game's t14, when the tracked stand-in binds the edit ages there
    ID3D11ClassInstance* classes_[256] = {};
    UINT classCount_ = 0, stencilRef_ = 0, sampleMask_ = 0;
    float factors_[4] = {};
    bool active_ = false, agesBound_ = false;

    void releaseSaved() {
        for (auto*& t : targets_) if (t) { t->Release(); t = nullptr; }
        for (UINT i = 0; i < classCount_; ++i) if (classes_[i]) { classes_[i]->Release(); classes_[i] = nullptr; }
        classCount_ = 0;
        depth_.Reset(); depthState_.Reset(); blend_.Reset(); ps_.Reset(); savedAges_.Reset();
        agesBound_ = false;
    }

public:
    static constexpr UINT kAgesSlot = 14;   // the tracked stand-in's UiEditAges (kWorldScreenTrackedHlsl); the game's shader declares t0..t5
    ~WorldScreenCoverage() { releaseSaved(); }
    bool active() const { return active_; }

    // mask, state and edits: the eye's coverage mask, census state and edit mask, bound at targets 0, 1 and 2. ages: UiContent's
    // edit ages for the tracked stand-in (null for F105's held one), bound at t14 and put back at end().
    bool begin(ID3D11DeviceContext* ctx, ID3D11PixelShader* standIn, ID3D11RenderTargetView* mask, ID3D11RenderTargetView* state,
               ID3D11RenderTargetView* edits, ID3D11ShaderResourceView* ages, ID3D11DepthStencilState* test, ID3D11BlendState* blend) {
        if (active_ || !ctx || !standIn || !mask || !state || !edits || !test || !blend) return false;
        Ptr<ID3D11Predicate> predicate; BOOL value = FALSE; ctx->GetPredication(&predicate, &value);
        if (predicate) return false;
        ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
        bool busy = false;
        for (auto* p : uavs) if (p) { busy = true; p->Release(); }
        ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT] = {}; ctx->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so);
        for (auto* p : so) if (p) { busy = true; p->Release(); }
        if (busy) return false;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets_, &depth_);
        if (!depth_) { releaseSaved(); return false; }
        ctx->OMGetDepthStencilState(&depthState_, &stencilRef_);
        ctx->OMGetBlendState(&blend_, factors_, &sampleMask_);
        classCount_ = 256;
        ctx->PSGetShader(&ps_, classes_, &classCount_);
        if (ages) {
            ctx->PSGetShaderResources(kAgesSlot, 1, &savedAges_);
            ctx->PSSetShaderResources(kAgesSlot, 1, &ages);
            agesBound_ = true;
        }
        ID3D11RenderTargetView* bound[3] = {mask, state, edits};
        vScreenSetRenderTargetsRaw(ctx, 3, bound, depth_.Get());
        const FLOAT ones[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        ctx->OMSetBlendState(blend, ones, 0xFFFFFFFFu);
        ctx->OMSetDepthStencilState(test, 0);
        ctx->PSSetShader(standIn, nullptr, 0);
        active_ = true;
        return true;
    }

    void end(ID3D11DeviceContext* ctx) {
        if (!active_) return;
        vScreenSetRenderTargetsRaw(ctx, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets_, depth_.Get());
        ctx->OMSetDepthStencilState(depthState_.Get(), stencilRef_);
        ctx->OMSetBlendState(blend_.Get(), factors_, sampleMask_);
        ctx->PSSetShader(ps_.Get(), classes_, classCount_);
        if (agesBound_) {
            ID3D11ShaderResourceView* back = savedAges_.Get();
            ctx->PSSetShaderResources(kAgesSlot, 1, &back);
        }
        releaseSaved();
        active_ = false;
    }
};

// F108's census on the GPU: each eye's screen-state target counted by kWorldScreenCensusCs into three words (screen
// pixels, edited, an edit's first frame), copied into a ring of four staging buffers and read back in frame order with
// no wait. A frame whose slot is still in flight skips its census (counted). The compute state it touches is put back.
class WorldScreenCensusPass {
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
public:
    static constexpr unsigned kSlots = 4;
private:
    Ptr<ID3D11ComputeShader> cs_;
    Ptr<ID3D11Buffer> counts_, staging_[kSlots];
    Ptr<ID3D11UnorderedAccessView> uav_;
    uint64_t frame_[kSlots] = {};
    bool pending_[kSlots] = {};
    unsigned next_ = 0, oldest_ = 0;
    bool tried_ = false, failed_ = false;

    bool make(ID3D11DeviceContext* ctx) {
        if (tried_) return !failed_;
        tried_ = true;
        failed_ = true;
        Ptr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        cs_.Attach(shaderSwapCreateCs(ctx, kWorldScreenCensusBytecode, sizeof(kWorldScreenCensusBytecode), "world_screen_census_cs",
                                      "ui depth world screens (F108)"));
        if (!cs_) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &counts_))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(dev->CreateUnorderedAccessView(counts_.Get(), &ud, &uav_))) return false;
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = 16;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& s : staging_) if (FAILED(dev->CreateBuffer(&sd, nullptr, &s))) return false;
        failed_ = false;
        return true;
    }

public:
    bool failed() const { return tried_ && failed_; }

    // Count the n eyes' state targets for the frame. False when it could not (the shader or a buffer missing, or the ring
    // full: the caller counts that frame skipped).
    bool count(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* const* states, unsigned n, uint64_t frame) {
        if (!ctx || !states || !n || !make(ctx)) return false;
        if (pending_[next_]) return false;
        Ptr<ID3D11ComputeShader> savedCs;
        ID3D11ClassInstance* classes[256] = {};
        UINT nc = 256;
        ctx->CSGetShader(&savedCs, classes, &nc);
        ID3D11ShaderResourceView* savedSrv = nullptr;
        ID3D11UnorderedAccessView* savedUav = nullptr;
        ctx->CSGetShaderResources(0, 1, &savedSrv);
        ctx->CSGetUnorderedAccessViews(0, 1, &savedUav);
        const UINT zero[4] = {};
        ctx->ClearUnorderedAccessViewUint(uav_.Get(), zero);
        ctx->CSSetShader(cs_.Get(), nullptr, 0);
        ID3D11UnorderedAccessView* uav = uav_.Get();
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        for (unsigned i = 0; i < n; ++i) {
            ID3D11ShaderResourceView* s = states[i];
            if (!s) continue;
            Ptr<ID3D11Resource> r;
            s->GetResource(&r);
            Ptr<ID3D11Texture2D> t;
            if (!r || FAILED(r.As(&t))) continue;
            D3D11_TEXTURE2D_DESC td{};
            t->GetDesc(&td);
            ctx->CSSetShaderResources(0, 1, &s);
            ctx->Dispatch((td.Width + 15) / 16, (td.Height + 15) / 16, 1);
        }
        ID3D11ShaderResourceView* noSrv = nullptr;
        ID3D11UnorderedAccessView* noUav = nullptr;
        ctx->CSSetShaderResources(0, 1, &noSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
        ctx->CSSetShaderResources(0, 1, &savedSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &savedUav, nullptr);
        ctx->CSSetShader(savedCs.Get(), classes, nc);
        for (UINT i = 0; i < nc; ++i) if (classes[i]) classes[i]->Release();
        if (savedSrv) savedSrv->Release();
        if (savedUav) savedUav->Release();
        ctx->CopyResource(staging_[next_].Get(), counts_.Get());
        pending_[next_] = true;
        frame_[next_] = frame;
        next_ = (next_ + 1) % kSlots;
        return true;
    }

    // Read back what is ready, oldest first, stopping at the first slot still in flight: sink(frame, screen, edited, fresh).
    template <class Sink>
    void drain(ID3D11DeviceContext* ctx, Sink&& sink) {
        while (ctx && pending_[oldest_]) {
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr = ctx->Map(staging_[oldest_].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
            if (SUCCEEDED(hr) && m.pData) {
                const uint32_t* v = static_cast<const uint32_t*>(m.pData);
                const uint32_t screen = v[0], edited = v[1], fresh = v[2];
                ctx->Unmap(staging_[oldest_].Get(), 0);
                sink(frame_[oldest_], screen, edited, fresh);
            }
            pending_[oldest_] = false;
            oldest_ = (oldest_ + 1) % kSlots;
        }
    }

    void reset() {
        cs_.Reset();
        counts_.Reset();
        uav_.Reset();
        for (auto& s : staging_) s.Reset();
        for (auto& p : pending_) p = false;
        next_ = oldest_ = 0;
        tried_ = failed_ = false;
    }
};

}  // namespace edvr
