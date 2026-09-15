#pragma once

// Deferred UI depth/stencil copy. This header is self contained so the
// test harness can exercise the exact
// D3D11 path without linking the game DLL.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>
#include <cstring>

namespace edvr_deferred_depth {
using Microsoft::WRL::ComPtr;

static const char* kVs = R"(
struct V { float4 p:SV_POSITION; };
V main(uint id:SV_VertexID) {
  float2 p = id==0 ? float2(-1,-1) : (id==1 ? float2(-1,3) : float2(3,-1));
  V v; v.p=float4(p,0,1); return v;
})";

static const char* kPsDepthStencil = R"(
cbuffer C:register(b0) { uint2 inSize; uint2 outSize; float2 jitter; uint writeBit; };
Texture2D<float> D:register(t0); Texture2D<uint4> S:register(t1);
struct O { float d:SV_Depth; uint s:SV_StencilRef; };
O main(float4 p:SV_POSITION) {
  int2 q=int2(floor(p.xy*float2(inSize)/float2(outSize)+float2(jitter)));
  q=clamp(q,int2(0,0),int2(inSize)-1); uint s=S.Load(int3(q,0)).y;
  O o; o.d=D.Load(int3(q,0)); o.s=s; return o;
})";

static const char* kPsDepthOnly = R"(
cbuffer C:register(b0) { uint2 inSize; uint2 outSize; float2 jitter; uint writeBit; };
Texture2D<float> D:register(t0); Texture2D<uint4> S:register(t1);
struct O { float d:SV_Depth; };
O main(float4 p:SV_POSITION) {
  int2 q=int2(floor(p.xy*float2(inSize)/float2(outSize)+float2(jitter)));
  q=clamp(q,int2(0,0),int2(inSize)-1); uint s=S.Load(int3(q,0)).y;
  if ((s & writeBit)==0) discard; O o; o.d=D.Load(int3(q,0)); return o;
})";
static const char* kPsDepthNoStencil = R"(
cbuffer C:register(b0) { uint2 inSize; uint2 outSize; float2 jitter; uint writeBit; };
Texture2D<float> D:register(t0); float main(float4 p:SV_POSITION):SV_Depth {
  int2 q=int2(floor(p.xy*float2(inSize)/float2(outSize)+float2(jitter)));
  q=clamp(q,int2(0,0),int2(inSize)-1); return D.Load(int3(q,0));
})";

class Seeder {
  ComPtr<ID3D11VertexShader> vs_; ComPtr<ID3D11PixelShader> ps_,psNoStencil_; ComPtr<ID3D11Buffer> cb_;
  ComPtr<ID3D11SamplerState> point_; ComPtr<ID3D11DepthStencilState> all_,depthOnly_,stencilOnly_,stencilBits_[8]; bool specified_=false;
  ComPtr<ID3D11Texture2D> output_; ComPtr<ID3D11DepthStencilView> outputView_; DXGI_FORMAT format_=DXGI_FORMAT_UNKNOWN;
  UINT inW_=0,inH_=0,outW_=0,outH_=0;
  struct C { UINT inSize[2], outSize[2]; float jitter[2]; UINT writeBit; };
  static ComPtr<ID3DBlob> compile(const char* src,const char* entry,const char* profile) {
    ComPtr<ID3DBlob> b,e; HRESULT h=D3DCompile(src,strlen(src),"ui_deferred_depth",nullptr,nullptr,entry,profile,0,0,&b,&e);
    if(FAILED(h)) throw std::runtime_error(e?std::string((char*)e->GetBufferPointer(),e->GetBufferSize()):"D3DCompile failed"); return b;
  }
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
    auto vb=compile(kVs,"main","vs_5_0"); if(FAILED(d->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vs_))) throw std::runtime_error("CreateVertexShader");
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o{}; if(SUCCEEDED(d->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2,&o,sizeof(o)))) specified_=o.PSSpecifiedStencilRefSupported!=FALSE;
    auto pb=compile(specified_?kPsDepthStencil:kPsDepthOnly,"main","ps_5_0"); if(FAILED(d->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&ps_))) throw std::runtime_error("CreatePixelShader"); state(d);
    auto pn=compile(kPsDepthNoStencil,"main","ps_5_0"); if(FAILED(d->CreatePixelShader(pn->GetBufferPointer(),pn->GetBufferSize(),nullptr,&psNoStencil_))) throw std::runtime_error("CreateDepthOnlyPixelShader");
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
