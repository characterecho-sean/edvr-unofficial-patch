#pragma once

#include "../../src/openvr/compat/openvr_v0_9_20.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr::openxr {

// Small, self-contained scene used by the native module fixture. The caller
// owns the immediate context while render() runs; ExecuteCommandList(TRUE)
// restores the caller's bindings after the private deferred work executes.
class ModuleTestScene final {
 public:
  ModuleTestScene() = default;
  ModuleTestScene(const ModuleTestScene&) = delete;
  ModuleTestScene& operator=(const ModuleTestScene&) = delete;

  HRESULT initialize(ID3D11Device* device, uint32_t width, uint32_t height) {
    reset();
    if (!device || !width || !height || width > 2048 || height > 2048) return E_INVALIDARG;
    device_ = device;
    width_ = width; height_ = height;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width; td.Height = height; td.ArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (auto& texture : eyes_) {
      HRESULT hr = device_->CreateTexture2D(&td, nullptr, &texture);
      if (FAILED(hr)) { reset(); return hr; }
    }
    for (unsigned i = 0; i != 2; ++i) {
      HRESULT hr = device_->CreateRenderTargetView(eyes_[i].Get(), nullptr, &views_[i]);
      if (FAILED(hr)) { reset(); return hr; }
    }
    static const char* const source =
      "struct VSIn { float4 p : POSITION; float3 c : COLOR; };"
      "struct VSOut { float4 p : SV_POSITION; float3 c : COLOR; };"
      "VSOut vs(VSIn i) { VSOut o; o.p=i.p; o.c=i.c; return o; }"
      "float4 ps(VSOut i) : SV_TARGET { return float4(i.c,1); }";
    Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, errors;
    HRESULT hr = D3DCompile(source, std::strlen(source), "module_scene.hlsl", nullptr,
      D3D_COMPILE_STANDARD_FILE_INCLUDE, "vs", "vs_4_0", 0, 0, &vs, &errors);
    if (FAILED(hr)) { reset(); return hr; }
    hr = D3DCompile(source, std::strlen(source), "module_scene.hlsl", nullptr,
      D3D_COMPILE_STANDARD_FILE_INCLUDE, "ps", "ps_4_0", 0, 0, &ps, &errors);
    if (FAILED(hr)) { reset(); return hr; }
    hr = device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
    if (SUCCEEDED(hr)) hr = device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_);
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
      {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
      {"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0}};
    if (SUCCEEDED(hr)) hr = device_->CreateInputLayout(elements, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &layout_);
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(Vertex) * 3; bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr)) hr = device_->CreateBuffer(&bd, nullptr, &vertices_);
    if (SUCCEEDED(hr)) hr = device_->CreateDeferredContext(0, &deferred_);
    D3D11_RASTERIZER_DESC raster{}; raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE;
    raster.DepthClipEnable=TRUE;
    if (SUCCEEDED(hr)) hr = device_->CreateRasterizerState(&raster, &rasterizer_);
    D3D11_DEPTH_STENCIL_DESC depth{}; depth.DepthEnable=FALSE; depth.StencilEnable=FALSE;
    depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    depth.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
    depth.BackFace=depth.FrontFace;
    if (SUCCEEDED(hr)) hr = device_->CreateDepthStencilState(&depth, &depthState_);
    if (FAILED(hr)) { reset(); return hr; }
    return S_OK;
  }

  HRESULT render(vr::IVRSystem& system, const vr::TrackedDevicePose_t& renderHead) {
    if (!device_ || !vertices_ || !views_[0] || !views_[1] || !renderHead.bPoseIsValid)
      return E_FAIL;
    float head[4][4]{};
    for (unsigned r=0;r<3;++r) for (unsigned c=0;c<4;++c) head[r][c]=renderHead.mDeviceToAbsoluteTracking.m[r][c];
    if (!finiteRigid(head)) return E_INVALIDARG;
    head[3][0]=head[3][1]=head[3][2]=0; head[3][3]=1;
    for (unsigned eye=0; eye<2; ++eye) {
      const vr::HmdMatrix34_t eh = system.GetEyeToHeadTransform(static_cast<vr::EVREye>(eye));
      float eyeMatrix[4][4]{};
      for (unsigned r=0;r<3;++r) for (unsigned c=0;c<4;++c) eyeMatrix[r][c]=eh.m[r][c];
      if (!finiteRigid(eyeMatrix)) return E_INVALIDARG;
      float eyeToWorld[4][4]{};
      for (unsigned r=0;r<3;++r) for (unsigned c=0;c<4;++c) {
        eyeToWorld[r][c]=head[r][0]*eh.m[0][c]+head[r][1]*eh.m[1][c]+head[r][2]*eh.m[2][c]+head[r][3]*(c==3);
      }
      eyeToWorld[3][3]=1;
      float worldToEye[4][4]{};
      if (!inverseRigid(eyeToWorld, worldToEye)) return E_INVALIDARG;
      const vr::HmdMatrix44_t projection=system.GetProjectionMatrix(static_cast<vr::EVREye>(eye),.025f,100.f,vr::API_DirectX);
      for (unsigned r=0;r<4;++r) for (unsigned c=0;c<4;++c) if (!std::isfinite(projection.m[r][c])) return E_INVALIDARG;
      Vertex transformed[3]{};
      const float world[3][3]={{-.45f,-.35f,-2.f},{.45f,-.35f,-2.f},{0.f,.45f,-2.f}};
      const float colors[3][3]={{1,0,0},{0,1,0},{0,0,1}};
      for (unsigned i=0;i<3;++i) {
        float p[4]={world[i][0],world[i][1],world[i][2],1}, v[4]{};
        for (unsigned r=0;r<4;++r) for (unsigned c=0;c<4;++c) v[r]+=worldToEye[r][c]*p[c];
        float clip[4]{}; for (unsigned r=0;r<4;++r) for (unsigned c=0;c<4;++c) clip[r]+=projection.m[r][c]*v[c];
        for (unsigned c=0;c<3;++c) transformed[i].color[c]=colors[i][c];
        for (unsigned c=0;c<4;++c) if (!std::isfinite(clip[c])) return E_INVALIDARG;
        transformed[i].position[0]=clip[0]; transformed[i].position[1]=clip[1]; transformed[i].position[2]=clip[2];
        transformed[i].position[3]=clip[3];
      }
      if (!deferred_) return E_FAIL;
      ID3D11DeviceContext* deferred=deferred_.Get();
      HRESULT hr=S_OK;
      D3D11_MAPPED_SUBRESOURCE mapped{};
      hr=deferred->Map(vertices_.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
      if(FAILED(hr))return hr; std::memcpy(mapped.pData,transformed,sizeof(transformed)); deferred->Unmap(vertices_.Get(),0);
      const float clear[4]={.03f,.03f,.03f,1}; deferred->ClearRenderTargetView(views_[eye].Get(),clear);
      ID3D11RenderTargetView* view=views_[eye].Get(); deferred->OMSetRenderTargets(1,&view,nullptr);
      D3D11_VIEWPORT viewport{}; viewport.Width=static_cast<float>(width_); viewport.Height=static_cast<float>(height_); viewport.MaxDepth=1;
      deferred->RSSetViewports(1,&viewport); deferred->RSSetState(rasterizer_.Get()); deferred->OMSetDepthStencilState(depthState_.Get(),0);
      const UINT stride=sizeof(Vertex), offset=0; ID3D11Buffer* vb=vertices_.Get();
      deferred->IASetInputLayout(layout_.Get()); deferred->IASetVertexBuffers(0,1,&vb,&stride,&offset);
      deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      deferred->VSSetShader(vs_.Get(),nullptr,0); deferred->PSSetShader(ps_.Get(),nullptr,0);
      deferred->Draw(3,0);
      Microsoft::WRL::ComPtr<ID3D11CommandList> list; hr=deferred->FinishCommandList(FALSE,&list); if(FAILED(hr))return hr;
      ID3D11DeviceContext* immediate=nullptr; device_->GetImmediateContext(&immediate);
      if(!immediate)return E_FAIL; immediate->ExecuteCommandList(list.Get(),TRUE); immediate->Release();
    }
    return S_OK;
  }

  ID3D11Texture2D* eye(unsigned index) const { return index<2?eyes_[index].Get():nullptr; }
  uint32_t width() const { return width_; } uint32_t height() const { return height_; }

 private:
  struct Vertex { float position[4]; float color[3]; };
  static bool inverseRigid(const float in[4][4], float out[4][4]) {
    if (!finiteRigid(in)) return false;
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)out[r][c]=in[c][r];
    for(unsigned r=0;r<3;++r){out[r][3]=0;for(unsigned c=0;c<3;++c)out[r][3]-=out[r][c]*in[c][3];}
    out[3][0]=out[3][1]=out[3][2]=0;out[3][3]=1;return true;
  }
  static bool finiteRigid(const float in[4][4]) {
    for(unsigned r=0;r<3;++r) for(unsigned c=0;c<4;++c) if(!std::isfinite(in[r][c])) return false;
    for(unsigned r=0;r<3;++r) {
      float length=0; for(unsigned c=0;c<3;++c) length+=in[r][c]*in[r][c];
      if(std::fabs(length-1.f)>0.01f) return false;
    }
    for(unsigned a=0;a<3;++a) for(unsigned b=a+1;b<3;++b) {
      float dot=0; for(unsigned c=0;c<3;++c) dot+=in[a][c]*in[b][c];
      if(std::fabs(dot)>0.01f) return false;
    }
    return true;
  }
  void reset(){deferred_.Reset();rasterizer_.Reset();depthState_.Reset();for(auto& v:views_)v.Reset();for(auto& t:eyes_)t.Reset();vertices_.Reset();layout_.Reset();vs_.Reset();ps_.Reset();device_.Reset();width_=height_=0;}
  Microsoft::WRL::ComPtr<ID3D11Device> device_; Microsoft::WRL::ComPtr<ID3D11Texture2D> eyes_[2];
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> views_[2]; Microsoft::WRL::ComPtr<ID3D11Buffer> vertices_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_; Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_; Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> deferred_; Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_; Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState_;
  uint32_t width_=0,height_=0;
};
}
