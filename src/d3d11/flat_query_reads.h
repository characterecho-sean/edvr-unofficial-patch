// The coverage classification's two questions to the D3D context (flat_query_cut.h explains the shortcuts), as functions the
// runtime and the engine rig both call. The callables keep them free of the runtime's internals: `hashOf` maps a shader
// object to its content hash (the runtime's lookupShaderHash), `flush` puts the game's state back where engine motion's
// substitution is still bound (flatRuntimeSubstitution), `log` says a line.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

#include "flat_query_cut.h"

namespace edvr {

// The depth resource behind the bound depth view. The binding shadow's answer is already in the draw's key (`shadowDepth`:
// the resource behind the depth view the hooked setter recorded); the context is asked as well on a check, or always
// once the shadow has been found wrong, and then the context's answer is the one the draw uses. A depth-only read: engine
// motion's substitution puts nothing on the depth view.
template <class Log>
inline const void* flatQueryDepth(FlatQueryCut& cut, ID3D11DeviceContext* ctx, const void* shadowDepth,
                                  Microsoft::WRL::ComPtr<ID3D11Resource>& hold, Log&& log) {
    const FlatQueryPlan plan = cut.plan(FlatQuery::CoverageDepth);
    if (plan == FlatQueryPlan::Shortcut) return shadowDepth;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> actual;
    ctx->OMGetRenderTargets(0, nullptr, &actual);
    if (actual) actual->GetResource(&hold);
    if (plan == FlatQueryPlan::Sample && cut.compared(FlatQuery::CoverageDepth, hold.Get() == shadowDepth)) {
        char line[400];
        flatQueryFallbackLine(line, sizeof(line), FlatQuery::CoverageDepth, "the resource behind the depth view differed");
        log(line);
    }
    return hold.Get();
}

// Whether the vertex and pixel shaders the context has bound are the ones the draw's key names (`vs`, `ps`: the hashes the
// shadow recorded): yes, from the shadow's own record, unless this is a check or the state fell back. Asked, they are the
// game's only once engine motion's substitution is put back (its patched pixel shader would read as no known shader), so
// `flush` runs first.
template <class Hash, class Flush, class Log>
inline bool flatQueryShaders(FlatQueryCut& cut, ID3D11DeviceContext* ctx, uint64_t vs, uint64_t ps, Hash&& hashOf, Flush&& flush,
                             Log&& log) {
    const FlatQueryPlan plan = cut.plan(FlatQuery::ShaderIdentity);
    if (plan == FlatQueryPlan::Shortcut) return true;
    flush();
    Microsoft::WRL::ComPtr<ID3D11VertexShader> actualVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> actualPs;
    ctx->VSGetShader(&actualVs, nullptr, nullptr);
    ctx->PSGetShader(&actualPs, nullptr, nullptr);
    const bool match = hashOf(actualVs.Get()) == vs && hashOf(actualPs.Get()) == ps;
    if (plan == FlatQueryPlan::Sample && cut.compared(FlatQuery::ShaderIdentity, match)) {
        char line[400];
        flatQueryFallbackLine(line, sizeof(line), FlatQuery::ShaderIdentity, "the bound shaders were not the ones it recorded");
        log(line);
    }
    return match;
}

}  // namespace edvr
