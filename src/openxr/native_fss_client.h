#pragma once
#include "../common/native_fss.h"
#include "geometry_snapshot.h"
#include <cstring>
#include <wrl/client.h>

namespace edvr::openxr {
class NativeFssClient final {
 public:
  HRESULT acquire(HMODULE provider,ID3D11Device* device,uint64_t generation) {
    if(table_.context)return E_PENDING;
    if(!provider||!device||!generation)return E_INVALIDARG;
    auto entry=GetProcAddress(provider,"edvrAcquireNativeFss");
    if(!owned(provider,entry))return E_NOINTERFACE;
    EdvrNativeFssRequest request{sizeof(request),EDVR_NATIVE_FSS_VERSION_1,device,generation};
    EdvrNativeFssTable candidate{sizeof(candidate),EDVR_NATIVE_FSS_VERSION_1};
    const auto r=reinterpret_cast<decltype(&edvrAcquireNativeFss)>(entry)(&request,&candidate);
    if(r!=S_OK)return FAILED(r)?r:E_NOINTERFACE;
    if(candidate.size!=sizeof(candidate)||candidate.version!=EDVR_NATIVE_FSS_VERSION_1||!candidate.context||
       !owned(provider,candidate.beginFrame)||!owned(provider,candidate.treatEye)||
       !owned(provider,candidate.invalidate)||!owned(provider,candidate.close))return E_NOINTERFACE;
    table_=candidate;generation_=generation;return S_OK;
  }
  bool acquired()const{return table_.context!=nullptr;}
  HRESULT begin(const GeometryInput& geometry,uint64_t reference) {
    if(!acquired())return S_FALSE;
    GeometrySnapshot snapshot{};if(!makeGeometrySnapshot(geometry,snapshot))return E_INVALIDARG;
    EdvrNativeFssFrame f{sizeof(f),EDVR_NATIVE_FSS_VERSION_1};
    f.generation=generation_;f.referenceGeneration=reference;f.sequence=geometry.sequence;
    for(unsigned e=0;e<2;++e) {
      const auto& raw=snapshot.raw[e];
      f.frusta[e][0]=raw.left;f.frusta[e][1]=raw.right;f.frusta[e][2]=raw.top;f.frusta[e][3]=raw.bottom;
      std::memcpy(f.eyeToHead[e],snapshot.eyeToHead[e].m,sizeof(f.eyeToHead[e]));
    }
    return table_.beginFrame(table_.context,&f);
  }
  HRESULT treat(uint64_t sequence,unsigned eye,ID3D11Texture2D* source,const vr::VRTextureBounds_t* bounds,
      Microsoft::WRL::ComPtr<ID3D11Texture2D>& output) {
    output.Reset();if(!acquired())return S_FALSE;
    const float box[4]={bounds?bounds->uMin:0,bounds?bounds->vMin:0,bounds?bounds->uMax:1,bounds?bounds->vMax:1};
    ID3D11Texture2D* raw=nullptr;
    const auto r=table_.treatEye(table_.context,sequence,eye,source,box,&raw);output.Attach(raw);
    if(r!=S_OK)output.Reset();else if(!output)return E_UNEXPECTED;
    return r;
  }
  HRESULT invalidate(){return acquired()?table_.invalidate(table_.context):S_FALSE;}
  HRESULT close(){if(!acquired())return S_FALSE;const auto r=table_.close(table_.context);if(SUCCEEDED(r))table_={};return r;}
 private:
  template<class T>static bool owned(HMODULE provider,T address) {
    if(!address)return false;HMODULE actual=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(address),&actual)&&actual==provider;
  }
  EdvrNativeFssTable table_{};uint64_t generation_=0;
};
}
