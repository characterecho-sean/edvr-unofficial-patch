// The SRV turret HUD's private pass, the parts a rig runs as they are (tools/srv_turret_hud_test, on WARP): the geometry shader's constant buffer and the binding that
// puts the shader and its constants in for one issue of the game's draw and gives the game's own back. srv_turret.h says what and why; srv_turret.cpp is the module.
//
// THE BINDING is the shape and the failure rules of supercruise_bars_binding.h's, less its rasterizer state (the pass draws triangles with the game's own state
// and needs no other): everything the game had in the two places is held (a reference each), the private objects go in, and restore() owes the game its own back.
// A restore that failed twice leaves the saved references held and needsRestore() true, and settle() -- run at the next frame boundary -- puts them back only into a
// place that still holds EDVR's own object (a place the game rebound already holds its own state). The binding shadow of vscreen is never told: it does not track the
// geometry stage, and the game's draw is issued through the hooked context exactly as it was.
//
// WHAT IS SWAPPED. (1) The geometry shader: the game binds none for these draws, and the binding declines (kGameHasGs) if it ever does -- a game geometry shader is
// not ours to replace. (2) The geometry stage's constant buffer slot 0, which the game never binds either: srv_turret_math.h's TurretParams, four registers (the move, the eye's tangents, the anchor and the
// translation in the eye's space). (3) For signature H only (the Scorpion's hologram), GS slot 1: the draw's own vertex constants (its VS slot 1, the view constants of which rows 277 to 279 are the
// camera's axes), taken with their range if the game bound one (VSGetConstantBuffers1) and set with the same range when it starts past the first constant (from the first constant the shader reads the same registers either way); the game binds nothing at GS slot 1 and gets that back too.
#pragma once

#include "../common/guard.h"
#include "srv_turret_math.h"

#include <d3d11.h>
#include <d3d11_1.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace srv_turret {

inline constexpr uint32_t kGsSlot = 0;   // the geometry stage's constant buffer slot the shader reads (the game binds none)
inline constexpr uint32_t kGsGameSlot = 1;   // ... and the one signature H reads the draw's vertex constants at (kGameCbSlot of the vertex stage)

// A constant buffer of the draw's own: for signature H the vertex stage's slot kGameCbSlot, for the disc probe a pixel-stage register's buffer; the buffer and the range the game bound it with (first constant and count, in 16-byte constants;
// ranged is false when the context cannot say, and the whole buffer is meant). A reference is held until drop() or the destructor.
struct ExtraCb {
    ID3D11Buffer* buffer = nullptr;
    UINT first = 0, count = 0;
    bool ranged = false;
    ExtraCb() = default;
    ExtraCb(const ExtraCb&) = delete;
    ExtraCb& operator=(const ExtraCb&) = delete;
    ~ExtraCb() { drop(); }
    void drop() {
        ID3D11Buffer* b = buffer;
        buffer = nullptr;
        if (b) guarded("srv.turret.extra.release", [&] { b->Release(); });
    }
};
// Read the draw's constants at `slot` of the vertex stage (pixel = false) or the pixel stage. False when nothing is bound there or a call faulted. A context with the 11.1 interface says the
// range the game bound; one without is the whole buffer.
inline bool readGameCb(ID3D11DeviceContext* c, uint32_t slot, ExtraCb& out, bool pixel = false) {
    out.drop();
    out.first = out.count = 0;
    out.ranged = false;
    if (!c) return false;
    guarded("srv.turret.extra.get", [&] {
        ID3D11DeviceContext1* c1 = nullptr;
        if (SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) && c1) {
            UINT first = 0, count = 0;
            if (pixel)
                c1->PSGetConstantBuffers1(slot, 1, &out.buffer, &first, &count);
            else
                c1->VSGetConstantBuffers1(slot, 1, &out.buffer, &first, &count);
            if (out.buffer) {
                out.first = first;
                out.count = count;
                out.ranged = true;
            }
            c1->Release();
        } else if (pixel) {
            c->PSGetConstantBuffers(slot, 1, &out.buffer);
        } else {
            c->VSGetConstantBuffers(slot, 1, &out.buffer);
        }
    });
    return out.buffer != nullptr;
}

// A region of a buffer copied into a staging buffer (the module hands the copy that goes past the vscreen hooks; the rig's is the context's own).
using CopyRegion = void (*)(ID3D11DeviceContext*, ID3D11Resource* dst, UINT dstX, ID3D11Resource* src, const D3D11_BOX* box);
inline void directCopyRegion(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dstX, ID3D11Resource* src, const D3D11_BOX* box) { c->CopySubresourceRegion(dst, 0, dstX, 0, 0, src, 0, box); }
// One pixel-shader constant register (16 bytes at `row` of the buffer bound at pixel-stage `slot`, counted from the first constant the game bound it with) copied into `stage` at byte `to`.
// False when nothing is bound there, the register lies past the buffer or the range, or a call faulted; `stage` is then untouched.
inline bool copyPsRegister(ID3D11DeviceContext* ctx, ID3D11Buffer* stage, uint32_t slot, uint32_t row, UINT to, CopyRegion copy = directCopyRegion) {
    if (!ctx || !stage || !copy) return false;
    ExtraCb cb;
    if (!readGameCb(ctx, slot, cb, true)) return false;
    D3D11_BUFFER_DESC bd{};
    bool sized = false;
    guarded("srv.turret.copy.desc", [&] {
        cb.buffer->GetDesc(&bd);
        sized = true;
    });
    const uint64_t left = (static_cast<uint64_t>(cb.ranged ? cb.first : 0) + row) * 16u;
    if (!sized || left + 16u > bd.ByteWidth || (cb.ranged && cb.count && row >= cb.count)) return false;
    const D3D11_BOX box{static_cast<UINT>(left), 0, 0, static_cast<UINT>(left) + 16u, 1, 1};
    return guarded("srv.turret.copy.region", [&] { copy(ctx, stage, to, cb.buffer, &box); });
}

// THE SCISSOR. The game sets a scissor rectangle for the radar's draws from the radar's screen bounds at its OLD place, and the moved geometry is cropped to it (a straight edge across the disc and
// the hologram). While a bound draw is issued the rectangles are the whole render target (render target 0's size at its view's mip, else the first viewport's extent), when the bound rasterizer state
// has ScissorEnable; the rasterizer state object is never changed, and the game's own rectangles (any number up to 16, or none) are set back exactly after.
class Scissor {
public:
    static constexpr UINT kMax = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    enum class Open { kOff, kOpened, kFailed };
    // What open() found at the game's draw (for the log).
    struct Seen {
        bool enabled = false;       // the bound rasterizer state has ScissorEnable
        UINT rects = 0;             // the rectangles the game had bound
        D3D11_RECT first{};         // ... the first of them
        LONG targetW = 0, targetH = 0;   // the extent they were opened to
    };

private:
    D3D11_RECT saved_[kMax] = {};
    D3D11_RECT ours_{};
    UINT count_ = 0, setCount_ = 0;
    bool owed_ = false;

public:
    bool owed() const { return owed_; }
    void clear() { owed_ = false; count_ = setCount_ = 0; }

    // The bound rasterizer state's ScissorEnable and, when set, the rectangles opened to the whole target. kOff: the test is off (nothing touched); kOpened: the rectangles are the whole target and the
    // game's are owed back (restore); kFailed: a call faulted, or there was no extent to open to (anything set is owed back as well).
    // apply = false only looks (seen is filled, nothing is set, the answer is kOff): the game-scissor key still says what it found.
    Open open(ID3D11DeviceContext* c, Seen& seen, bool apply = true) {
        seen = Seen{};
        if (!c || owed_) return Open::kFailed;
        Open result = Open::kOff;
        const bool ran = guarded("srv.turret.scissor.open", [&] {
            ID3D11RasterizerState* rs = nullptr;
            c->RSGetState(&rs);
            bool enabled = false;
            if (rs) {
                D3D11_RASTERIZER_DESC rd{};
                rs->GetDesc(&rd);
                enabled = rd.ScissorEnable != FALSE;
                rs->Release();
            }
            if (!enabled) return;
            seen.enabled = true;
            UINT n = 0;
            c->RSGetScissorRects(&n, nullptr);
            if (n > kMax) n = kMax;
            UINT take = n;
            if (n) c->RSGetScissorRects(&take, saved_);
            count_ = n;
            seen.rects = n;
            if (n) seen.first = saved_[0];
            LONG w = 0, h = 0;
            ID3D11RenderTargetView* rtv = nullptr;
            c->OMGetRenderTargets(1, &rtv, nullptr);
            if (rtv) {
                D3D11_RENDER_TARGET_VIEW_DESC vd{};
                rtv->GetDesc(&vd);
                UINT mip = 0;
                if (vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) mip = vd.Texture2D.MipSlice;
                else if (vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) mip = vd.Texture2DArray.MipSlice;
                ID3D11Resource* res = nullptr;
                rtv->GetResource(&res);
                if (res) {
                    ID3D11Texture2D* tex = nullptr;
                    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
                        D3D11_TEXTURE2D_DESC td{};
                        tex->GetDesc(&td);
                        w = static_cast<LONG>((td.Width >> mip) ? (td.Width >> mip) : 1u);
                        h = static_cast<LONG>((td.Height >> mip) ? (td.Height >> mip) : 1u);
                        tex->Release();
                    }
                    res->Release();
                }
                rtv->Release();
            }
            if (w <= 0 || h <= 0) {   // no target view: the viewport's extent
                UINT nv = 1;
                D3D11_VIEWPORT vp{};
                c->RSGetViewports(&nv, &vp);
                if (nv) {
                    w = static_cast<LONG>(std::ceil(vp.TopLeftX + vp.Width));
                    h = static_cast<LONG>(std::ceil(vp.TopLeftY + vp.Height));
                }
            }
            if (w <= 0 || h <= 0) {
                result = Open::kFailed;
                return;
            }
            seen.targetW = w;
            seen.targetH = h;
            if (!apply) return;
            ours_ = D3D11_RECT{0, 0, w, h};
            D3D11_RECT full[kMax];
            setCount_ = n ? n : 1;
            for (UINT i = 0; i < setCount_; ++i) full[i] = ours_;
            owed_ = true;   // the setter can partially publish before a fault
            c->RSSetScissorRects(setCount_, full);
            result = Open::kOpened;
        });
        if (!ran) result = owed_ ? Open::kOpened : Open::kFailed;
        return result;
    }
    // The game's rectangles back, exactly (none: none). False when the setter faulted: still owed.
    bool restore(ID3D11DeviceContext* c) {
        if (!owed_) return true;
        if (!c) return false;
        if (!guarded("srv.turret.scissor.restore", [&] { c->RSSetScissorRects(count_, count_ ? saved_ : nullptr); })) return false;
        owed_ = false;
        return true;
    }
    // At a frame boundary after a restore failed: the game's rectangles go back only if the bound ones are still ours (the game rebinding its own is left alone).
    bool settle(ID3D11DeviceContext* c) {
        if (!owed_) return true;
        if (!c) return false;
        bool ours = false;
        if (!guarded("srv.turret.scissor.settle.get", [&] {
                UINT n = 0;
                c->RSGetScissorRects(&n, nullptr);
                if (n != setCount_ || n > kMax) return;
                D3D11_RECT now[kMax] = {};
                UINT take = n;
                c->RSGetScissorRects(&take, now);
                ours = true;
                for (UINT i = 0; i < n; ++i) ours = ours && now[i].left == ours_.left && now[i].top == ours_.top && now[i].right == ours_.right && now[i].bottom == ours_.bottom;
            }))
            return false;
        if (!ours) {
            owed_ = false;
            return true;
        }
        return restore(c);
    }
};

// The stream-output declaration of an output signature, for the GPU instrument's twin of its shader (srv_turret.cpp makeTwins, and the rig's twin): one entry for each element, in the
// signature's order, into stream 0 and output slot 0, then the shader's own `pre` element (the clip position before the move), so a vertex is written out as sigFloats + 4 floats with
// the moved clip position at Family::posFloat and the game's at Family::preFloat. Returns the entries; `out` holds kMaxSigElements.
inline UINT soDeclaration(uint32_t sig, D3D11_SO_DECLARATION_ENTRY (&out)[kMaxSigElements]) {
    const Signature& s = kSignatures[sig];
    for (uint32_t i = 0; i < s.count; ++i) out[i] = D3D11_SO_DECLARATION_ENTRY{0, s.el[i].name, s.el[i].index, 0, static_cast<BYTE>(s.el[i].comps), 0};
    out[s.count] = D3D11_SO_DECLARATION_ENTRY{0, kPreSemantic, 0, 0, static_cast<BYTE>(kPreFloats), 0};
    return s.count + 1;
}

template <class T>
inline void release(T*& value) {
    T* p = value;
    value = nullptr;
    if (p) guarded("srv.turret.release", [&] { p->Release(); });
}

// One 64-byte buffer per eye, made on first use and rewritten (UpdateSubresource) only when a number changes, never per draw. A failed create or write is not
// retried on that device: get() answers null and the caller declines.
class Constants {
    ID3D11Device* device_ = nullptr;  // identity only: the buffer holds its device alive, so the address cannot be reused
    ID3D11Buffer* buffer_ = nullptr;
    TurretParams written_;
    bool failed_ = false;

public:
    Constants() = default;
    Constants(const Constants&) = delete;
    Constants& operator=(const Constants&) = delete;
    ~Constants() { reset(); }

    ID3D11Buffer* get(ID3D11DeviceContext* ctx, const TurretParams& p) {
        // paramsUsable (srv_turret_math.h): finite; a pull is a positive gap and a push a negative one, up to 2 m; the window of anchor distances starts above the gap and above 0 (the
        // divisor d - gap stays positive); a frustum; an anchor and a translation within 2 m; and a move that does something (a gap, or a translation with a gap of 0).
        if (!ctx || !paramsUsable(p)) return nullptr;
        ID3D11Device* d = nullptr;
        if (!guarded("srv.turret.constants.device", [&] { ctx->GetDevice(&d); }) || !d) {
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
                bd.ByteWidth = sizeof(TurretParams);
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                const D3D11_SUBRESOURCE_DATA init{&p, 0, 0};
                ID3D11Buffer* made = nullptr;
                HRESULT hr = E_FAIL;
                const bool ran = guarded("srv.turret.constants.create", [&] { hr = d->CreateBuffer(&bd, &init, &made); });
                if (!ran || FAILED(hr) || !made) {
                    release(made);
                    failed_ = true;
                } else {
                    buffer_ = made;
                    written_ = p;
                }
            } else if (!(p == written_)) {
                if (guarded("srv.turret.constants.write", [&] { ctx->UpdateSubresource(buffer_, 0, nullptr, &p, 0, 0); }))
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
    TurretParams written() const { return written_; }
    void reset() {
        release(buffer_);
        device_ = nullptr;
        written_ = TurretParams{};
        failed_ = false;
    }
};

using SetGs = void (*)(ID3D11DeviceContext*, ID3D11GeometryShader*, ID3D11ClassInstance* const*, uint32_t);
inline void directSetGs(ID3D11DeviceContext* c, ID3D11GeometryShader* gs, ID3D11ClassInstance* const* classes, uint32_t n) {
    c->GSSetShader(gs, classes, n);
}

class Binding {
    ID3D11GeometryShader* gs_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11Buffer* cb1_ = nullptr;   // the game's GS slot 1 (it binds none; held only when signature H's extra constants went in)
    const void* ourGs_ = nullptr;
    const void* ourCb_ = nullptr;
    const void* ourCb1_ = nullptr;
    ID3D11ClassInstance* classes_[D3D11_SHADER_MAX_INTERFACES] = {};
    UINT count_ = D3D11_SHADER_MAX_INTERFACES;
    bool gsSaved_ = false, cbSaved_ = false, cb1Saved_ = false, modified_ = false;

    void drop() {
        release(gs_);
        release(cb_);
        release(cb1_);
        for (auto& p : classes_) release(p);
        count_ = D3D11_SHADER_MAX_INTERFACES;
        gsSaved_ = cbSaved_ = cb1Saved_ = modified_ = false;
        ourGs_ = ourCb_ = ourCb1_ = nullptr;
    }
    bool settleGs(ID3D11DeviceContext* c, SetGs setGs) {
        ID3D11GeometryShader* bound = nullptr;
        const bool read = guarded("srv.turret.settle.get.gs", [&] { c->GSGetShader(&bound, nullptr, nullptr); });
        const bool ours = bound && static_cast<const void*>(bound) == ourGs_;
        release(bound);
        if (!read) return false;
        if (ours && !guarded("srv.turret.settle.gs", [&] { setGs(c, gs_, count_ ? classes_ : nullptr, count_); })) return false;
        release(gs_);
        for (auto& p : classes_) release(p);
        count_ = D3D11_SHADER_MAX_INTERFACES;
        gsSaved_ = false;
        return true;
    }
    bool settleBuffer(ID3D11DeviceContext* c) {
        ID3D11Buffer* bound = nullptr;
        const bool read = guarded("srv.turret.settle.get.cb", [&] { c->GSGetConstantBuffers(kGsSlot, 1, &bound); });
        const bool ours = bound && static_cast<const void*>(bound) == ourCb_;
        release(bound);
        if (!read) return false;
        if (ours && !guarded("srv.turret.settle.cb", [&] { c->GSSetConstantBuffers(kGsSlot, 1, &cb_); })) return false;
        release(cb_);
        cbSaved_ = false;
        return true;
    }
    bool settleBuffer1(ID3D11DeviceContext* c) {
        ID3D11Buffer* bound = nullptr;
        const bool read = guarded("srv.turret.settle.get.cb1", [&] { c->GSGetConstantBuffers(kGsGameSlot, 1, &bound); });
        const bool ours = bound && static_cast<const void*>(bound) == ourCb1_;
        release(bound);
        if (!read) return false;
        if (ours && !guarded("srv.turret.settle.cb1", [&] { c->GSSetConstantBuffers(kGsGameSlot, 1, &cb1_); })) return false;
        release(cb1_);
        cb1Saved_ = false;
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

    // Bind the shader and its constants for the issue that follows. kGameHasGs: the game has a geometry shader bound, or class instances: nothing was touched.
    // kRefused: a call faulted, or a private object was missing: nothing of ours stays bound.
    // `extra` (signature H) is the draw's vertex constants, set at GS slot 1 with the range the game bound them with.
    Begin begin(ID3D11DeviceContext* c, ID3D11GeometryShader* shader, ID3D11Buffer* constants, SetGs setGs = directSetGs, const ExtraCb* extra = nullptr) {
        if (!c || !shader || !constants || modified_ || gsSaved_ || cbSaved_ || cb1Saved_) return Begin::kRefused;
        if (extra && !extra->buffer) return Begin::kRefused;
        Begin result = Begin::kRefused;
        const bool ran = guarded("srv.turret.bind", [&] {
            c->GSGetShader(&gs_, classes_, &count_);
            gsSaved_ = true;
            if (gs_ || count_ != 0) {
                result = Begin::kGameHasGs;
                return;
            }
            c->GSGetConstantBuffers(kGsSlot, 1, &cb_);
            cbSaved_ = true;
            ourGs_ = shader;
            ourCb_ = constants;
            if (extra) {
                c->GSGetConstantBuffers(kGsGameSlot, 1, &cb1_);
                cb1Saved_ = true;
                ourCb1_ = extra->buffer;
            }
            modified_ = true;  // any setter can partially publish before a fault
            c->GSSetConstantBuffers(kGsSlot, 1, &constants);
            if (extra) {
                ID3D11DeviceContext1* c1 = nullptr;
                if (extra->ranged && extra->first != 0 && SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) && c1) {   // a range that starts at 0 is the whole buffer to the shader
                    ID3D11Buffer* b = extra->buffer;
                    c1->GSSetConstantBuffers1(kGsGameSlot, 1, &b, &extra->first, &extra->count);
                    c1->Release();
                } else {
                    ID3D11Buffer* b = extra->buffer;
                    c->GSSetConstantBuffers(kGsGameSlot, 1, &b);
                }
            }
            setGs(c, shader, nullptr, 0);
            result = Begin::kBound;
        });
        if (!ran) result = Begin::kRefused;
        if (result != Begin::kBound && !modified_) drop();  // nothing of ours is bound: let go of what the getters took
        return result;
    }

    bool restore(ID3D11DeviceContext* c, SetGs setGs = directSetGs) {
        if (!modified_) return true;
        bool ok = true;
        if (gsSaved_) ok = guarded("srv.turret.restore.gs", [&] { setGs(c, gs_, count_ ? classes_ : nullptr, count_); }) && ok;
        if (cbSaved_) ok = guarded("srv.turret.restore.cb", [&] { c->GSSetConstantBuffers(kGsSlot, 1, &cb_); }) && ok;
        if (cb1Saved_) ok = guarded("srv.turret.restore.cb1", [&] { c->GSSetConstantBuffers(kGsGameSlot, 1, &cb1_); }) && ok;
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
        if (cb1Saved_) ok = settleBuffer1(c) && ok;
        if (ok) modified_ = false;
        return ok;
    }
};

}  // namespace srv_turret
}  // namespace edvr
