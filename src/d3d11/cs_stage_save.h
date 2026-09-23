#pragma once
// The compute stage as the game left it, saved before the temporal pass's
// dispatches and put back after them, whichever of its branches ran.
//
// One place for the slot counts, because the counts are the bug class: the
// pass bound t0..t20 (and, with engine-record velocity, t21/t22 and b1/b2)
// while the save covered t0..t18 and b0, so every slot above the save came
// back NULL to a game that may never rebind it (the 2026-09-23 review of
// engine motion, item 5; t19/t20 had the same gap before it). A new binding
// past these counts fails the static_assert beside its dispatch in
// temporal_pass.cpp, not a flight; tools/engine_velocity_test binds sentinels
// in every slot and checks each one comes back by identity.
#include <d3d11.h>

namespace edvr {

struct CsStageSave {
    static constexpr UINT kSrvs = 23;      // t0..t22 (t21/t22: engine-record velocity)
    static constexpr UINT kUavs = 7;       // u0..u6 (u7, the trace target, is saved at its own site)
    static constexpr UINT kCbs = 3;        // b0 the pass's own, b1/b2 engine-record velocity
    static constexpr UINT kSamplers = 1;

    ID3D11ComputeShader* shader = nullptr;
    ID3D11ShaderResourceView* srv[kSrvs] = {};
    ID3D11UnorderedAccessView* uav[kUavs] = {};
    ID3D11Buffer* cb[kCbs] = {};
    ID3D11SamplerState* sampler[kSamplers] = {};
    bool held = false;

    CsStageSave() = default;
    CsStageSave(const CsStageSave&) = delete;
    CsStageSave& operator=(const CsStageSave&) = delete;
    ~CsStageSave() { release(); }

    void save(ID3D11DeviceContext* ctx) {
        release();
        ctx->CSGetShader(&shader, nullptr, nullptr);
        ctx->CSGetShaderResources(0, kSrvs, srv);
        ctx->CSGetUnorderedAccessViews(0, kUavs, uav);
        ctx->CSGetConstantBuffers(0, kCbs, cb);
        ctx->CSGetSamplers(0, kSamplers, sampler);
        held = true;
    }
    // Everything back exactly, then the references dropped. A null saved slot
    // is put back as null: that is what the game had.
    void restore(ID3D11DeviceContext* ctx) {
        if (!held) return;
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->CSSetShaderResources(0, kSrvs, srv);
        ctx->CSSetUnorderedAccessViews(0, kUavs, uav, nullptr);
        ctx->CSSetConstantBuffers(0, kCbs, cb);
        ctx->CSSetSamplers(0, kSamplers, sampler);
        release();
    }

private:
    void release() {
        if (shader) shader->Release();
        shader = nullptr;
        for (auto*& v : srv) { if (v) v->Release(); v = nullptr; }
        for (auto*& v : uav) { if (v) v->Release(); v = nullptr; }
        for (auto*& v : cb) { if (v) v->Release(); v = nullptr; }
        for (auto*& v : sampler) { if (v) v->Release(); v = nullptr; }
        held = false;
    }
};

}  // namespace edvr
