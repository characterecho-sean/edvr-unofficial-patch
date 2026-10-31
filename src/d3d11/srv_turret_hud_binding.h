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
// translation in the eye's space).
#pragma once

#include "../common/guard.h"
#include "srv_turret_math.h"

#include <d3d11.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace srv_turret {

inline constexpr uint32_t kGsSlot = 0;   // the geometry stage's constant buffer slot the shader reads (the game binds none)

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
    const void* ourGs_ = nullptr;
    const void* ourCb_ = nullptr;
    ID3D11ClassInstance* classes_[D3D11_SHADER_MAX_INTERFACES] = {};
    UINT count_ = D3D11_SHADER_MAX_INTERFACES;
    bool gsSaved_ = false, cbSaved_ = false, modified_ = false;

    void drop() {
        release(gs_);
        release(cb_);
        for (auto& p : classes_) release(p);
        count_ = D3D11_SHADER_MAX_INTERFACES;
        gsSaved_ = cbSaved_ = modified_ = false;
        ourGs_ = ourCb_ = nullptr;
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
    Begin begin(ID3D11DeviceContext* c, ID3D11GeometryShader* shader, ID3D11Buffer* constants, SetGs setGs = directSetGs) {
        if (!c || !shader || !constants || modified_ || gsSaved_ || cbSaved_) return Begin::kRefused;
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
            modified_ = true;  // either setter can partially publish before a fault
            c->GSSetConstantBuffers(kGsSlot, 1, &constants);
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
        if (ok) modified_ = false;
        return ok;
    }
};

}  // namespace srv_turret
}  // namespace edvr
