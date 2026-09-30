#include "fixed_shader_source.h"
#include "temporal_shader_bytecode.h"
#pragma once

// The UI layer's depth-stencil seed (ui_layer.cpp): the game's depth and
// stencil copied into a target of another size, through a deferred context,
// nearest-sample with the jitter applied. Written for the deferred UI replay
// (retired 2026-09-23); the layer is its reader now. Self contained, so
// tools/ui_layer_seed_test and tools/ui_quality_test exercise the exact
// D3D11 path without linking the game DLL.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>
#include <cstring>

namespace edvr_layer_seed {
using Microsoft::WRL::ComPtr;
inline constexpr auto kVs = edvr::kUiSeedVs;
inline constexpr auto kPsDepthStencil = edvr::kUiSeedPsDepthStencil;
inline constexpr auto kPsDepthOnly = edvr::kUiSeedPsDepthOnly;
inline constexpr auto kPsDepthNoStencil = edvr::kUiSeedPsDepthNoStencil;









class Seeder {
  ComPtr<ID3D11VertexShader> vs_; ComPtr<ID3D11PixelShader> ps_,psNoStencil_; ComPtr<ID3D11Buffer> cb_;
  ComPtr<ID3D11SamplerState> point_; ComPtr<ID3D11DepthStencilState> all_,depthOnly_,stencilOnly_,stencilBits_[8]; bool specified_=false;
  ComPtr<ID3D11Texture2D> output_; ComPtr<ID3D11DepthStencilView> outputView_; DXGI_FORMAT format_=DXGI_FORMAT_UNKNOWN;
  UINT inW_=0,inH_=0,outW_=0,outH_=0;
  struct C { UINT inSize[2], outSize[2]; float jitter[2]; UINT writeBit; };

  void state(ID3D11Device* d) {
    D3D11_DEPTH_STENCIL_DESC x{}; x.DepthEnable=TRUE; x.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL; x.DepthFunc=D3D11_COMPARISON_ALWAYS;
    x.StencilEnable=TRUE; x.StencilReadMask=0xff; x.StencilWriteMask=0xff; x.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS}; x.BackFace=x.FrontFace;
    if(FAILED(d->CreateDepthStencilState(&x,&all_))) throw std::runtime_error("CreateDepthStencilState");
    x.StencilEnable=FALSE; x.StencilWriteMask=0; if(FAILED(d->CreateDepthStencilState(&x,&depthOnly_))) throw std::runtime_error("CreateDepthOnlyState");
    x.DepthEnable=FALSE; x.StencilEnable=TRUE; x.StencilReadMask=0xff; x.StencilWriteMask=0xff; x.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_REPLACE,D3D11_COMPARISON_ALWAYS}; x.BackFace=x.FrontFace; if(FAILED(d->CreateDepthStencilState(&x,&stencilOnly_))) throw std::runtime_error("CreateStencilOnlyState"); for(UINT i=0;i<8;i++){x.StencilWriteMask=1u<<i;if(FAILED(d->CreateDepthStencilState(&x,&stencilBits_[i])))throw std::runtime_error("CreateStencilBitState");}
    D3D11_SAMPLER_DESC s{}; s.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT; s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; s.MaxLOD=D3D11_FLOAT32_MAX;
    if(FAILED(d->CreateSamplerState(&s,&point_))) throw std::runtime_error("CreateSamplerState");
    D3D11_BUFFER_DESC b{}; b.ByteWidth=(sizeof(C)+15u)&~15u; b.Usage=D3D11_USAGE_DYNAMIC; b.BindFlags=D3D11_BIND_CONSTANT_BUFFER; b.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    if(FAILED(d->CreateBuffer(&b,nullptr,&cb_))) throw std::runtime_error("CreateBuffer");
  }
public:
  void init(ID3D11Device* d) {
    if(FAILED(d->CreateVertexShader(edvr::kUiSeedVsBytecode,sizeof(edvr::kUiSeedVsBytecode),nullptr,&vs_))) throw std::runtime_error("CreateVertexShader");
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o{}; if(SUCCEEDED(d->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2,&o,sizeof(o)))) specified_=o.PSSpecifiedStencilRefSupported!=FALSE;
    const void* pb=specified_?static_cast<const void*>(edvr::kUiSeedDepthStencilBytecode):edvr::kUiSeedDepthOnlyBytecode; const size_t pbSize=specified_?sizeof(edvr::kUiSeedDepthStencilBytecode):sizeof(edvr::kUiSeedDepthOnlyBytecode); if(FAILED(d->CreatePixelShader(pb,pbSize,nullptr,&ps_))) throw std::runtime_error("CreatePixelShader"); state(d);
    if(FAILED(d->CreatePixelShader(edvr::kUiSeedDepthNoStencilBytecode,sizeof(edvr::kUiSeedDepthNoStencilBytecode),nullptr,&psNoStencil_))) throw std::runtime_error("CreateDepthOnlyPixelShader");
  }
  bool usesSpecifiedStencilRef() const { return specified_; }
  bool ensure(ID3D11Device* d, UINT w, UINT h, DXGI_FORMAT dsvFormat) {
    DXGI_FORMAT resource=DXGI_FORMAT_UNKNOWN;
    if(dsvFormat==DXGI_FORMAT_D24_UNORM_S8_UINT) resource=DXGI_FORMAT_R24G8_TYPELESS;
    else if(dsvFormat==DXGI_FORMAT_D32_FLOAT_S8X24_UINT) resource=DXGI_FORMAT_R32G8X24_TYPELESS;
    else if(dsvFormat==DXGI_FORMAT_D32_FLOAT) resource=DXGI_FORMAT_R32_TYPELESS;
    else return false;
    if(output_ && w==outW_ && h==outH_ && format_==dsvFormat) return true;
    D3D11_TEXTURE2D_DESC td{}; td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=1;td.Format=resource;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    if(FAILED(d->CreateTexture2D(&td,nullptr,&output_))) return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.Format=dsvFormat;vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    if(FAILED(d->CreateDepthStencilView(output_.Get(),&vd,&outputView_))) { output_.Reset(); return false; }
    outW_=w;outH_=h;format_=dsvFormat;return true;
  }
  ID3D11DepthStencilView* view() const { return outputView_.Get(); }
  bool record(ID3D11DeviceContext* freshDeferred, ID3D11ShaderResourceView* inputDepth, ID3D11ShaderResourceView* inputStencil,
              UINT inputWidth, UINT inputHeight, float jitterX, float jitterY, UINT stencilMask=255, bool needsDepth=true) {
    if(!freshDeferred || freshDeferred->GetType()!=D3D11_DEVICE_CONTEXT_DEFERRED || !outputView_) return false;
    seed(freshDeferred,inputDepth,inputStencil,outputView_.Get(),inputWidth,inputHeight,outW_,outH_,jitterX,jitterY,stencilMask,needsDepth); return true;
  }
  void seed(ID3D11DeviceContext* c, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* stencil, ID3D11DepthStencilView* out,
            UINT inW,UINT inH,UINT outW,UINT outH,float jitterX,float jitterY,UINT stencilMask=255,bool needsDepth=true) {
    if(!c||!depth||!out||!vs_||!ps_) throw std::runtime_error("invalid seeder arguments"); inW_=inW;inH_=inH;outW_=outW;outH_=outH;
    if(stencil)c->ClearDepthStencilView(out,D3D11_CLEAR_STENCIL,0,0);
    c->RSSetState(nullptr);c->IASetInputLayout(nullptr);c->GSSetShader(nullptr,nullptr,0);c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);c->SetPredication(nullptr,FALSE);
    D3D11_MAPPED_SUBRESOURCE m{}; if(FAILED(c->Map(cb_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m))) throw std::runtime_error("Map constants"); C x{{inW,inH},{outW,outH},{jitterX,jitterY},0}; memcpy(m.pData,&x,sizeof(x)); c->Unmap(cb_.Get(),0);
    D3D11_VIEWPORT vp{0,0,(float)outW,(float)outH,0,1}; c->RSSetViewports(1,&vp); c->OMSetRenderTargets(0,nullptr,out); const bool hasStencil=stencil!=nullptr; const bool fallback=hasStencil&&!specified_; c->OMSetDepthStencilState((fallback?depthOnly_:(hasStencil?all_:depthOnly_)).Get(),0); c->VSSetShader(vs_.Get(),nullptr,0); c->PSSetShader((fallback?psNoStencil_:(hasStencil?ps_:psNoStencil_)).Get(),nullptr,0); c->VSSetConstantBuffers(0,1,cb_.GetAddressOf()); c->PSSetConstantBuffers(0,1,cb_.GetAddressOf()); ID3D11ShaderResourceView* sr[]={depth,stencil}; c->PSSetShaderResources(0,hasStencil?2:1,sr); c->PSSetSamplers(0,1,point_.GetAddressOf()); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); if(needsDepth || (hasStencil && specified_ && stencilMask))c->Draw(3,0);
    ID3D11ShaderResourceView* n[2]={nullptr,nullptr}; c->PSSetShaderResources(0,2,n);
    if(!specified_ && hasStencil) { /* The fallback requires one pass per set stencil bit. */
      for(UINT bit=1,bi=0;bit<256;bit<<=1,bi++) { if(!(stencilMask&bit))continue; if(FAILED(c->Map(cb_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m))) throw std::runtime_error("Map fallback constants"); x.writeBit=bit; memcpy(m.pData,&x,sizeof(x)); c->Unmap(cb_.Get(),0); c->OMSetDepthStencilState(stencilBits_[bi].Get(),bit); c->OMSetRenderTargets(0,nullptr,out); c->PSSetShader(ps_.Get(),nullptr,0); c->PSSetShaderResources(0,2,sr); c->Draw(3,0); c->PSSetShaderResources(0,2,n); }
    }
  }
};
}
