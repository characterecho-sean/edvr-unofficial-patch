// The supercruise bars' private pass, the parts a rig runs as they are (tools/supercruise_bars_test, on WARP): the strip
// shader's constants, the two rasterizer states it draws with, and the binding that puts them in for one issue of the game's
// draw and gives the game's own back. supercruise_bars.h says what and why; supercruise_bars.cpp is the module.
//
// THE BINDING, the shape and the failure rules of orbital_width_patch.h's and ui_holo_remap.h's: everything the game had in
// the three places is held (a reference each), the private objects go in, and restore() owes the game its own back. A
// restore that failed twice leaves the saved references held and needsRestore() true, and settle() -- run at the next frame
// boundary -- puts them back only into a place that still holds EDVR's own object (a place the game rebound already holds its
// own state). The binding shadow of vscreen is never told: it does not track the geometry stage or the rasterizer state, and
// the game's draw is issued through the hooked context exactly as it was.
//
// WHAT IS SWAPPED. (1) The geometry shader: the game binds none for these draws, and the binding declines (kGameHasGs) if it
// ever does -- a game geometry shader is not ours to replace. (2) The geometry stage's constant buffer slot 0, which the game
// never binds either: the strip's four numbers. (3) The rasterizer state: the strip is triangles, so the game's cull mode
// (a state it set for lines, which no cull mode ever touches) would apply to them; the private states cull nothing, fill
// solid, clip depth, and keep ONLY the game's scissor enable (the layer remaps the scissor rectangle the game set).
#pragma once

#include "../common/guard.h"

#include <d3d11.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace supercruise_bars {

inline constexpr uint32_t kGsSlot = 0;   // the geometry stage's constant buffer slot the strip shader reads (the game binds none)
inline constexpr float kClipW = 0.01f;   // clip-space w a segment is cut at: a hundredth of the projection's unit, a point on the camera's plane
inline constexpr float kMaxTent = 16.0f; // the tent's half-width is clamped to this many layer pixels (a layer 16 times the render)

template <class T>
inline void release(T*& value) {
    T* p = value;
    value = nullptr;
    if (p) guarded("supercruise.bars.release", [&] { p->Release(); });
}

// The strip shader's constant buffer, as the HLSL declares it: viewport width and height, the tent's half-width, the cut.
struct StripParams {
    float viewportW = 0.0f, viewportH = 0.0f, halfWidth = 0.0f, clipW = kClipW;
    bool operator==(const StripParams& o) const { return std::memcmp(this, &o, sizeof(StripParams)) == 0; }
};
static_assert(sizeof(StripParams) == 16, "the strip shader's cbuffer is one 16-byte register");

// The tent's half-width in the pixels of the viewport the strip is drawn into: one pixel of the game's own line is
// layerViewportW / renderViewportW of them, which is the weight (the width a unit-peak tent integrates to) the line keeps.
// Clamped to [1, kMaxTent]: a layer no wider than the render never reaches here (the layer declines those draws), and
// anything past sixteen times is not a layer this was made for. 0 for a dead input; `clamped` says the clamp acted.
inline float tentHalfWidth(float layerViewportW, float renderViewportW, bool* clamped = nullptr) {
    if (clamped) *clamped = false;
    if (!std::isfinite(layerViewportW) || !std::isfinite(renderViewportW) || !(layerViewportW > 0.0f) ||
        !(renderViewportW > 0.0f))
        return 0.0f;
    const float r = layerViewportW / renderViewportW;
    if (r < 1.0f) {
        if (clamped) *clamped = true;
        return 1.0f;
    }
    if (r > kMaxTent) {
        if (clamped) *clamped = true;
        return kMaxTent;
    }
    return r;
}

// One 16-byte buffer per device, made on first use and rewritten (UpdateSubresource) only when the four numbers change, never
// per draw. A failed create or write is not retried on that device: get() answers null and the caller declines.
class Constants {
    ID3D11Device* device_ = nullptr;  // identity only: the buffer holds its device alive, so the address cannot be reused
    ID3D11Buffer* buffer_ = nullptr;
    StripParams written_;
    bool failed_ = false;

public:
    Constants() = default;
    Constants(const Constants&) = delete;
    Constants& operator=(const Constants&) = delete;
    ~Constants() { reset(); }

    ID3D11Buffer* get(ID3D11DeviceContext* ctx, const StripParams& p) {
        if (!ctx || !std::isfinite(p.viewportW) || !std::isfinite(p.viewportH) || !std::isfinite(p.halfWidth) ||
            !std::isfinite(p.clipW) || !(p.viewportW > 0.0f) || !(p.viewportH > 0.0f) || !(p.halfWidth > 0.0f))
            return nullptr;
        ID3D11Device* d = nullptr;
        if (!guarded("supercruise.bars.constants.device", [&] { ctx->GetDevice(&d); }) || !d) {
            release(d);
            return nullptr;
        }
        if (d != device_) {
            reset();
            device_ = d;
        }
        ID3D11Buffer* result = nullptr;
        if (!failed_) {
            if (!buffer_) {
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = sizeof(StripParams);
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                const D3D11_SUBRESOURCE_DATA init{&p, 0, 0};
                ID3D11Buffer* made = nullptr;
                HRESULT hr = E_FAIL;
                const bool ran = guarded("supercruise.bars.constants.create", [&] { hr = d->CreateBuffer(&bd, &init, &made); });
                if (!ran || FAILED(hr) || !made) {
                    release(made);
                    failed_ = true;
                } else {
                    buffer_ = made;
                    written_ = p;
                }
            } else if (!(p == written_)) {
                if (guarded("supercruise.bars.constants.write", [&] { ctx->UpdateSubresource(buffer_, 0, nullptr, &p, 0, 0); }))
                    written_ = p;
                else
                    failed_ = true;
            }
            if (!failed_) result = buffer_;
        }
        release(d);
        return result;
    }

    bool failed() const { return failed_; }
    ID3D11Buffer* buffer() const { return buffer_; }
    StripParams written() const { return written_; }
    void reset() {
        release(buffer_);
        device_ = nullptr;
        written_ = StripParams{};
        failed_ = false;
    }
};

// The two states the strip is drawn with, [scissor off, scissor on]: no cull, solid, depth clip, the scissor enable the game's
// own state had. Made on first use and kept; a failed create is not retried on that device.
class RasterStates {
    ID3D11Device* device_ = nullptr;
    ID3D11RasterizerState* state_[2] = {};
    bool failed_[2] = {};

public:
    RasterStates() = default;
    RasterStates(const RasterStates&) = delete;
    RasterStates& operator=(const RasterStates&) = delete;
    ~RasterStates() { reset(); }

    ID3D11RasterizerState* get(ID3D11DeviceContext* ctx, bool scissor) {
        if (!ctx) return nullptr;
        const int i = scissor ? 1 : 0;
        ID3D11Device* d = nullptr;
        if (!guarded("supercruise.bars.raster.device", [&] { ctx->GetDevice(&d); }) || !d) {
            release(d);
            return nullptr;
        }
        if (d != device_) {
            reset();
            device_ = d;
        }
        if (!state_[i] && !failed_[i]) {
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            rd.ScissorEnable = scissor ? TRUE : FALSE;
            ID3D11RasterizerState* made = nullptr;
            HRESULT hr = E_FAIL;
            const bool ran = guarded("supercruise.bars.raster.create", [&] { hr = d->CreateRasterizerState(&rd, &made); });
            if (!ran || FAILED(hr) || !made) {
                release(made);
                failed_[i] = true;
            } else {
                state_[i] = made;
            }
        }
        ID3D11RasterizerState* result = state_[i];
        release(d);
        return result;
    }

    bool failed() const { return failed_[0] || failed_[1]; }
    void reset() {
        release(state_[0]);
        release(state_[1]);
        device_ = nullptr;
        failed_[0] = failed_[1] = false;
    }
};

using SetGs = void (*)(ID3D11DeviceContext*, ID3D11GeometryShader*, ID3D11ClassInstance* const*, uint32_t);
inline void directSetGs(ID3D11DeviceContext* c, ID3D11GeometryShader* gs, ID3D11ClassInstance* const* classes, uint32_t n) {
    c->GSSetShader(gs, classes, n);
}

class Binding {
    ID3D11GeometryShader* gs_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11RasterizerState* rs_ = nullptr;
    const void* ourGs_ = nullptr;
    const void* ourCb_ = nullptr;
    const void* ourRs_ = nullptr;
    ID3D11ClassInstance* classes_[D3D11_SHADER_MAX_INTERFACES] = {};
    UINT count_ = D3D11_SHADER_MAX_INTERFACES;
    bool gsSaved_ = false, cbSaved_ = false, rsSaved_ = false, modified_ = false;

    void drop() {
        release(gs_);
        release(cb_);
        release(rs_);
        for (auto& p : classes_) release(p);
        count_ = D3D11_SHADER_MAX_INTERFACES;
        gsSaved_ = cbSaved_ = rsSaved_ = modified_ = false;
        ourGs_ = ourCb_ = ourRs_ = nullptr;
    }
    bool settleGs(ID3D11DeviceContext* c, SetGs setGs) {
        ID3D11GeometryShader* bound = nullptr;
        const bool read = guarded("supercruise.bars.settle.get.gs", [&] { c->GSGetShader(&bound, nullptr, nullptr); });
        const bool ours = bound && static_cast<const void*>(bound) == ourGs_;
        release(bound);
        if (!read) return false;
        if (ours && !guarded("supercruise.bars.settle.gs", [&] { setGs(c, gs_, count_ ? classes_ : nullptr, count_); })) return false;
        release(gs_);
        for (auto& p : classes_) release(p);
        count_ = D3D11_SHADER_MAX_INTERFACES;
        gsSaved_ = false;
        return true;
    }
    bool settleBuffer(ID3D11DeviceContext* c) {
        ID3D11Buffer* bound = nullptr;
        const bool read = guarded("supercruise.bars.settle.get.cb", [&] { c->GSGetConstantBuffers(kGsSlot, 1, &bound); });
        const bool ours = bound && static_cast<const void*>(bound) == ourCb_;
        release(bound);
        if (!read) return false;
        if (ours && !guarded("supercruise.bars.settle.cb", [&] { c->GSSetConstantBuffers(kGsSlot, 1, &cb_); })) return false;
        release(cb_);
        cbSaved_ = false;
        return true;
    }
    bool settleRaster(ID3D11DeviceContext* c) {
        ID3D11RasterizerState* bound = nullptr;
        const bool read = guarded("supercruise.bars.settle.get.rs", [&] { c->RSGetState(&bound); });
        const bool ours = bound && static_cast<const void*>(bound) == ourRs_;
        release(bound);
        if (!read) return false;
        if (ours && !guarded("supercruise.bars.settle.rs", [&] { c->RSSetState(rs_); })) return false;
        release(rs_);
        rsSaved_ = false;
        return true;
    }

public:
    Binding() = default;
    Binding(const Binding&) = delete;
    Binding& operator=(const Binding&) = delete;
    ~Binding() { drop(); }

    bool needsRestore() const { return modified_; }
    void clear() { drop(); }

    enum class Begin { kBound, kGameHasGs, kRefused };

    // Bind the strip shader, its constants and the private rasterizer state for the issue that follows. `pickRaster` answers the
    // private state for the game's own scissor enable (RasterStates::get). kGameHasGs: the game has a geometry shader bound, or
    // class instances: nothing was touched. kRefused: a call faulted, or a private object was missing: nothing of ours stays bound.
    template <class PickRaster>
    Begin begin(ID3D11DeviceContext* c, ID3D11GeometryShader* strip, ID3D11Buffer* constants, PickRaster&& pickRaster,
                SetGs setGs = directSetGs) {
        if (!c || !strip || !constants || modified_ || gsSaved_ || cbSaved_ || rsSaved_) return Begin::kRefused;
        Begin result = Begin::kRefused;
        const bool ran = guarded("supercruise.bars.bind", [&] {
            c->GSGetShader(&gs_, classes_, &count_);
            gsSaved_ = true;
            if (gs_ || count_ != 0) {
                result = Begin::kGameHasGs;
                return;
            }
            c->GSGetConstantBuffers(kGsSlot, 1, &cb_);
            cbSaved_ = true;
            c->RSGetState(&rs_);
            rsSaved_ = true;
            bool scissor = false;
            if (rs_) {
                D3D11_RASTERIZER_DESC rd{};
                rs_->GetDesc(&rd);
                scissor = rd.ScissorEnable != FALSE;
            }
            ID3D11RasterizerState* ours = pickRaster(scissor);
            if (!ours) return;
            ourGs_ = strip;
            ourCb_ = constants;
            ourRs_ = ours;
            modified_ = true;  // any of the three setters can partially publish before a fault
            c->GSSetConstantBuffers(kGsSlot, 1, &constants);
            c->RSSetState(ours);
            setGs(c, strip, nullptr, 0);
            result = Begin::kBound;
        });
        if (!ran) result = Begin::kRefused;
        if (result != Begin::kBound && !modified_) drop();  // nothing of ours is bound: let go of what the getters took
        return result;
    }

    bool restore(ID3D11DeviceContext* c, SetGs setGs = directSetGs) {
        if (!modified_) return true;
        bool ok = true;
        if (gsSaved_) ok = guarded("supercruise.bars.restore.gs", [&] { setGs(c, gs_, count_ ? classes_ : nullptr, count_); }) && ok;
        if (cbSaved_) ok = guarded("supercruise.bars.restore.cb", [&] { c->GSSetConstantBuffers(kGsSlot, 1, &cb_); }) && ok;
        if (rsSaved_) ok = guarded("supercruise.bars.restore.rs", [&] { c->RSSetState(rs_); }) && ok;
        if (ok) drop();
        return ok;
    }
    struct RestoreResult {
        bool restored, retried;
    };
    RestoreResult finish(ID3D11DeviceContext* c, SetGs setGs = directSetGs) {
        if (restore(c, setGs)) return {true, false};
        return {restore(c, setGs), true};  // one bounded attempt, the saved references retained
    }
    bool settle(ID3D11DeviceContext* c, SetGs setGs = directSetGs) {
        if (!modified_) return true;
        if (!c) return false;
        bool ok = true;
        if (gsSaved_) ok = settleGs(c, setGs) && ok;
        if (cbSaved_) ok = settleBuffer(c) && ok;
        if (rsSaved_) ok = settleRaster(c) && ok;
        if (ok) modified_ = false;
        return ok;
    }
};

}  // namespace supercruise_bars
}  // namespace edvr
