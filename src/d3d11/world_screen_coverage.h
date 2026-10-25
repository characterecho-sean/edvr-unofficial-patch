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

namespace edvr {

constexpr uint64_t kWorldScreenVs = 0x889A5279E68F0672ull;
constexpr uint64_t kWorldScreenPs = 0xEBA95E15B0A66102ull;
// The values the stand-in writes (kWorldScreenCoverageHlsl), as the 8-bit masks hold them: the coverage mask's
// attached stroke and the edit mask's mark. The rig reads them back.
constexpr uint32_t kWorldScreenMaskValue = 2;
constexpr uint32_t kWorldScreenEditValue = 255;

// The draw this marks: the exact pair, sampling a learned interface surface (ui depth's composite test), drawn into
// the scene's depth, while an external upscaler runs. Anything else is left exactly as before.
inline bool worldScreenDraw(uint64_t vs, uint64_t ps, bool composite, bool sceneDepth, bool trained) {
    return trained && composite && sceneDepth && vs == kWorldScreenVs && ps == kWorldScreenPs;
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
    void refuse(WorldScreenRefusal r) { ++refused[static_cast<size_t>(r)]; }
    void reset() { *this = WorldScreenWindow{}; }
};

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
    ID3D11ClassInstance* classes_[256] = {};
    UINT classCount_ = 0, stencilRef_ = 0, sampleMask_ = 0;
    float factors_[4] = {};
    bool active_ = false;

    void releaseSaved() {
        for (auto*& t : targets_) if (t) { t->Release(); t = nullptr; }
        for (UINT i = 0; i < classCount_; ++i) if (classes_[i]) { classes_[i]->Release(); classes_[i] = nullptr; }
        classCount_ = 0;
        depth_.Reset(); depthState_.Reset(); blend_.Reset(); ps_.Reset();
    }

public:
    ~WorldScreenCoverage() { releaseSaved(); }
    bool active() const { return active_; }

    bool begin(ID3D11DeviceContext* ctx, ID3D11PixelShader* standIn, ID3D11RenderTargetView* mask,
               ID3D11RenderTargetView* edits, ID3D11DepthStencilState* test, ID3D11BlendState* blend) {
        if (active_ || !ctx || !standIn || !mask || !edits || !test || !blend) return false;
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
        ID3D11RenderTargetView* bound[3] = {mask, nullptr, edits};
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
        releaseSaved();
        active_ = false;
    }
};

}  // namespace edvr
