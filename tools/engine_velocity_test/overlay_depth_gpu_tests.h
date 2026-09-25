#pragma once

// Pixel-level WARP gate for the guarded flat decal export. The snapshot is a
// separate SRV, never the bound MRT6 resource. A nonzero viewport origin makes
// SV_Position.xy's absolute texel addressing observable.
#include "shader_tests.h"

namespace overlay_depth_gpu_tests {
using Microsoft::WRL::ComPtr;

constexpr UINT kWidth = 32, kHeight = 16;
constexpr char kVs[] = R"HLSL(
struct Out { nointerpolation uint id : __USER_VERTEX_FACEINVARIANT; float4 p : SV_Position; };
Out main(uint vertex : SV_VertexID) {
    Out o;
    float2 corner = vertex == 0 ? float2(-1,-1) : vertex == 1 ? float2(-1,3) : float2(3,-1);
    o.p = float4(corner,0.25,1);
    o.id = 0x80000003u;
    return o;
}
)HLSL";
constexpr char kPs[] = R"HLSL(
struct In { nointerpolation uint id : __USER_VERTEX_FACEINVARIANT; float4 p : SV_Position; };
struct Out { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float4 d : SV_Target3; };
Out main(In i) {
    Out o;
    o.a = float4(frac(i.p.x / 32), frac(i.p.y / 16), 0.25, 1);
    o.b = float4((i.id & 7u) / 7.0, 0.5, 0.75, 1);
    o.c = float4(i.p.z, frac(i.p.x / 8), frac(i.p.y / 8), 1);
    o.d = float4(0.125, 0.375, 0.625, 1);
    return o;
}
)HLSL";
constexpr char kPsOccupied[] = R"HLSL(
Texture2D<float4> busy : register(t3);
struct In { nointerpolation uint id : __USER_VERTEX_FACEINVARIANT; float4 p : SV_Position; };
float4 main(In i) : SV_Target0 { return busy.Load(int3(i.p.xy,0)) + float4(i.id & 1u,0,0,0); }
)HLSL";

struct Frame { std::vector<BYTE> game[4], slots; };

inline std::vector<BYTE> read(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                              ID3D11Texture2D* source, UINT bytesPerPixel) {
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&desc, nullptr, &staging))) return {};
    ctx->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    std::vector<BYTE> result(size_t(kWidth) * kHeight * bytesPerPixel);
    for (UINT y = 0; y < kHeight; ++y)
        std::memcpy(result.data() + size_t(y) * kWidth * bytesPerPixel,
                    static_cast<const BYTE*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                    size_t(kWidth) * bytesPerPixel);
    ctx->Unmap(staging.Get(), 0);
    return result;
}

inline bool draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11VertexShader* vs,
                 ID3D11PixelShader* ps, ID3D11ShaderResourceView* snapshot,
                 Frame& result) {
    ctx->ClearState();
    ComPtr<ID3D11Texture2D> targets[7];
    ComPtr<ID3D11RenderTargetView> rtv[7];
    ID3D11RenderTargetView* bound[7]{};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kWidth; td.Height = kHeight; td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    const float clear[4] = {-1,0,0,0};
    for (UINT i = 0; i < 7; ++i) {
        if (i == 4 || i == 5) continue;
        td.Format = i == 6 ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &targets[i])) ||
            FAILED(dev->CreateRenderTargetView(targets[i].Get(), nullptr, &rtv[i]))) return false;
        ctx->ClearRenderTargetView(rtv[i].Get(), clear);
        bound[i] = rtv[i].Get();
    }
    td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &depth)) ||
        FAILED(dev->CreateDepthStencilView(depth.Get(), nullptr, &dsv))) return false;
    ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0, 0);
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    ComPtr<ID3D11DepthStencilState> depthState;
    if (FAILED(dev->CreateDepthStencilState(&ds, &depthState))) return false;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rasterState;
    if (FAILED(dev->CreateRasterizerState(&raster, &rasterState))) return false;
    const D3D11_VIEWPORT vp{4,3,24,10,0,1};
    ctx->OMSetRenderTargets(7, bound, dsv.Get());
    ctx->OMSetDepthStencilState(depthState.Get(), 0);
    ctx->RSSetState(rasterState.Get()); ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0);
    if (snapshot) ctx->PSSetShaderResources(edvr::kEngineVelocityOverlaySnapshotSlot, 1, &snapshot);
    ctx->Draw(3, 0);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    for (UINT i = 0; i < 4; ++i) result.game[i] = read(dev, ctx, targets[i].Get(), 16);
    result.slots = read(dev, ctx, targets[6].Get(), 8);
    return !result.game[0].empty() && !result.slots.empty();
}

inline void run(ID3D11Device* dev, ID3D11DeviceContext* ctx, void (*check)(bool,const char*)) {
    const shader_tests::Harness h{dev,ctx,check};
    auto vsBlob = shader_tests::compile(h,kVs,"vs_5_0");
    auto psBlob = shader_tests::compile(h,kPs,"ps_5_0");
    if (!vsBlob || !psBlob) return;
    std::vector<BYTE> psBytes(static_cast<const BYTE*>(psBlob->GetBufferPointer()),
                              static_cast<const BYTE*>(psBlob->GetBufferPointer()) + psBlob->GetBufferSize());
    // These synthetic signatures are deliberately simple and checked before
    // use: FACEINVARIANT v0.x and VS/PS position register 1.
    ComPtr<ID3D11ShaderReflection> vsReflection, psReflection;
    check(SUCCEEDED(D3DReflect(vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),
                               IID_PPV_ARGS(&vsReflection))) &&
          SUCCEEDED(D3DReflect(psBlob->GetBufferPointer(),psBlob->GetBufferSize(),
                               IID_PPV_ARGS(&psReflection))),
          "synthetic overlay signatures reflect");
    if (!vsReflection || !psReflection) return;
    D3D11_SHADER_DESC vsDescription{},psDescription{};
    vsReflection->GetDesc(&vsDescription);psReflection->GetDesc(&psDescription);
    bool vsPosition=false,psPosition=false,psIdentity=false;
    for (UINT i=0;i<vsDescription.OutputParameters;++i) {
        D3D11_SIGNATURE_PARAMETER_DESC entry{};vsReflection->GetOutputParameterDesc(i,&entry);
        vsPosition |= entry.SystemValueType==D3D_NAME_POSITION && entry.Register==1;
    }
    for (UINT i=0;i<psDescription.InputParameters;++i) {
        D3D11_SIGNATURE_PARAMETER_DESC entry{};psReflection->GetInputParameterDesc(i,&entry);
        psPosition |= entry.SystemValueType==D3D_NAME_POSITION && entry.Register==1;
        psIdentity |= entry.Register==0 && entry.ComponentType==D3D_REGISTER_COMPONENT_UINT32 &&
                      entry.SemanticName && std::strcmp(entry.SemanticName,"__USER_VERTEX_FACEINVARIANT")==0;
    }
    check(vsPosition && psPosition && psIdentity,
          "guard fixture's position and pool-code input registers are exact");
    edvr::EngineVelocityInputs inputs{};
    inputs.identityRegister = 0; inputs.identityComponent = 0; inputs.positionRegister = 1;
    std::string reason;
    std::vector<BYTE> normal, guarded;
    check(edvr::engineVelocityPatchPs(psBytes.data(),psBytes.size(),inputs,normal,reason),
          "normal synthetic decal PS patches");
    check(edvr::engineVelocityPatchPs(psBytes.data(),psBytes.size(),inputs,guarded,reason,true),
          "guarded synthetic decal PS patches");
    if (normal.empty() || guarded.empty()) return;
    auto occupiedBlob = shader_tests::compile(h,kPsOccupied,"ps_5_0");
    if (occupiedBlob) {
        std::vector<BYTE> occupied(static_cast<const BYTE*>(occupiedBlob->GetBufferPointer()),
                                   static_cast<const BYTE*>(occupiedBlob->GetBufferPointer()) + occupiedBlob->GetBufferSize());
        std::vector<BYTE> refused;
        check(!edvr::engineVelocityPatchPs(occupied.data(),occupied.size(),inputs,refused,reason,true) &&
              reason.find("t3 occupied") != std::string::npos,
              "guarded patch refuses the game's occupied t3");
    }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> stockPs, normalPs, guardedPs;
    check(SUCCEEDED(dev->CreateVertexShader(vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),nullptr,&vs)) &&
          SUCCEEDED(dev->CreatePixelShader(psBlob->GetBufferPointer(),psBlob->GetBufferSize(),nullptr,&stockPs)) &&
          SUCCEEDED(dev->CreatePixelShader(normal.data(),normal.size(),nullptr,&normalPs)) &&
          SUCCEEDED(dev->CreatePixelShader(guarded.data(),guarded.size(),nullptr,&guardedPs)),
          "stock, normal and guarded shaders create on WARP");
    if (!vs || !stockPs || !normalPs || !guardedPs) return;
    struct Pair { float code, depth; };
    std::vector<Pair> texels(size_t(kWidth)*kHeight);
    for (UINT y=0;y<kHeight;++y) for (UINT x=0;x<kWidth;++x)
        texels[size_t(y)*kWidth+x] = {x%3==0 ? 7.0f : x%3==1 ? 9.0f : -1.0f, 0.5f};
    D3D11_TEXTURE2D_DESC td{};
    td.Width=kWidth;td.Height=kHeight;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32G32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{texels.data(),UINT(kWidth*sizeof(Pair)),0};
    ComPtr<ID3D11Texture2D> snapshot;
    ComPtr<ID3D11ShaderResourceView> snapshotView;
    check(SUCCEEDED(dev->CreateTexture2D(&td,&init,&snapshot)) &&
          SUCCEEDED(dev->CreateShaderResourceView(snapshot.Get(),nullptr,&snapshotView)),
          "private snapshot SRV created separately from MRT6");
    if (!snapshotView) return;
    Frame stock, ordinary, guardedFrame;
    check(draw(dev,ctx,vs.Get(),stockPs.Get(),nullptr,stock) &&
          draw(dev,ctx,vs.Get(),normalPs.Get(),nullptr,ordinary) &&
          draw(dev,ctx,vs.Get(),guardedPs.Get(),snapshotView.Get(),guardedFrame),
          "all three decal draws read back");
    for (UINT i=0;i<4;++i)
        check(stock.game[i]==ordinary.game[i] && stock.game[i]==guardedFrame.game[i],
              "guarded depth selection preserves original game MRT bytes");
    UINT matched=0,different=0,cleared=0,normalPixels=0,outside=0;
    for (UINT y=0;y<kHeight;++y) for (UINT x=0;x<kWidth;++x) {
        const size_t at=(size_t(y)*kWidth+x)*8;
        float regular[2], selected[2];
        std::memcpy(regular,ordinary.slots.data()+at,8);
        std::memcpy(selected,guardedFrame.slots.data()+at,8);
        const bool covered=x>=4&&x<28&&y>=3&&y<13;
        if (!covered) { outside += selected[0]==-1.0f && regular[0]==-1.0f; continue; }
        if (regular[0]==7.0f && regular[1]==0.25f) ++normalPixels;
        if (x%3==0) matched += selected[0]==7.0f && selected[1]==0.5f;
        else if (x%3==1) different += selected[0]==7.0f && selected[1]==0.25f;
        else cleared += selected[0]==7.0f && selected[1]==0.25f;
    }
    check(normalPixels==240 && matched==80 && different==80 && cleared==80 && outside==kWidth*kHeight-240,
          "per-pixel guard keeps matching substrate depth and rejects other/empty owner");
    std::printf("  guarded overlay: %u matching-owner, %u different-owner, %u empty-substrate pixels; game MRT0-3 exact\n",
                matched,different,cleared);
    ctx->ClearState();
}
} // namespace overlay_depth_gpu_tests
