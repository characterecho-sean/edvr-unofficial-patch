#pragma once

#include "../../src/d3d11/original_draw_identity_capture.h"

#include <stdexcept>
#include <vector>

namespace identity_capture_test {
using Microsoft::WRL::ComPtr;

inline void require(bool v, const char* message, unsigned& checks) {
    ++checks; if (!v) throw std::runtime_error(message);
}
inline ComPtr<ID3DBlob> shader(const char* text, const char* entry, const char* target,
                              unsigned& checks) {
    ComPtr<ID3DBlob> code, errors;
    require(SUCCEEDED(D3DCompile(text,std::strlen(text),"identity-capture-test",nullptr,nullptr,
        entry,target,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors)),"compile identity test shader",checks);
    return code;
}

// Called by the original_draw_probe WARP rig. The production poll is always
// nonblocking; Flush below is test synchronization after that behavior is
// checked, and is never part of the capture helper.
inline void run(ID3D11Device* device, ID3D11DeviceContext* ctx, unsigned& checks) {
    using namespace edvr;
    const auto vsCode=shader("float4 main(uint2 i:INSTANCEANDMODELDATAINDEX):SV_Position{return float4(0,0,0,1);}","main","vs_5_0",checks);
    D3D11_INPUT_ELEMENT_DESC element={"INSTANCEANDMODELDATAINDEX",0,DXGI_FORMAT_R32G32_UINT,0,0,D3D11_INPUT_PER_INSTANCE_DATA,1};
    ComPtr<ID3D11InputLayout> layout; require(SUCCEEDED(device->CreateInputLayout(&element,1,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),&layout)),"create identity layout",checks);
    // Creation hash is deliberately unrelated: compatible layouts are reused
    // across shader families; runtime selection validates the actual VS.
    originalDrawIdentityRememberLayout(layout.Get(),&element,1,0x123456789ABCDEF0ull); ctx->IASetInputLayout(layout.Get());

    struct Pair { uint32_t a,b; }; Pair ids[]={{99,99},{0,100},{2,101},{99,102},{1,103}};
    D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(ids);b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{ids};ComPtr<ID3D11Buffer> vb;require(SUCCEEDED(device->CreateBuffer(&b,&init,&vb)),"create identity VB",checks);
    UINT stride=8,offset=8;ID3D11Buffer* rawVb=vb.Get();ctx->IASetVertexBuffers(0,1,&rawVb,&stride,&offset);

    std::vector<uint8_t> models(5*kOriginalDrawIdentityModelBytes);
    for(UINT record=0;record<5;++record)for(UINT byte=0;byte<kOriginalDrawIdentityModelBytes;++byte)models[record*kOriginalDrawIdentityModelBytes+byte]=uint8_t(record*17+byte);
    std::memset(models.data()+2*kOriginalDrawIdentityModelBytes,0,4); // unskinned view element 0
    const uint32_t bone=7;std::memcpy(models.data()+4*kOriginalDrawIdentityModelBytes,&bone,4); // skinned view element 2
    b={};b.ByteWidth=UINT(models.size());b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_SHADER_RESOURCE;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.StructureByteStride=kOriginalDrawIdentityModelBytes;
    init={models.data()};ComPtr<ID3D11Buffer> pool;require(SUCCEEDED(device->CreateBuffer(&b,&init,&pool)),"create identity pool",checks);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_UNKNOWN;sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sd.Buffer.FirstElement=2;sd.Buffer.NumElements=3;
    ComPtr<ID3D11ShaderResourceView> poolView;require(SUCCEEDED(device->CreateShaderResourceView(pool.Get(),&sd,&poolView)),"create offset pool view",checks);
    ID3D11ShaderResourceView* rawPool=poolView.Get();ctx->VSSetShaderResources(33,1,&rawPool);

    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> image;ComPtr<ID3D11RenderTargetView> imageRtv;require(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&image)),"create untouched image",checks);require(SUCCEEDED(device->CreateRenderTargetView(image.Get(),nullptr,&imageRtv)),"create untouched RTV",checks);
    const float green[]={0,1,0,1};ctx->ClearRenderTargetView(imageRtv.Get(),green);

    // Seed every compute binding touched by capture and verify exact COM
    // identities survive. The dummy shader deliberately has no side effects.
    const auto csCode=shader("[numthreads(1,1,1)]void main(){}","main","cs_5_0",checks);
    ComPtr<ID3D11ComputeShader> oldCs;require(SUCCEEDED(device->CreateComputeShader(csCode->GetBufferPointer(),csCode->GetBufferSize(),nullptr,&oldCs)),"create saved CS",checks);
    b={};b.ByteWidth=16;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> oldCb;require(SUCCEEDED(device->CreateBuffer(&b,nullptr,&oldCb)),"create saved CB",checks);
    b={};b.ByteWidth=16;b.BindFlags=D3D11_BIND_SHADER_RESOURCE;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.StructureByteStride=4;
    ComPtr<ID3D11Buffer> stateSrvBuffer,stateUavBuffer;ComPtr<ID3D11ShaderResourceView> oldSrv0,oldSrv1;ComPtr<ID3D11UnorderedAccessView> oldUav;
    require(SUCCEEDED(device->CreateBuffer(&b,nullptr,&stateSrvBuffer)),"create state SRV buffer",checks);require(SUCCEEDED(device->CreateShaderResourceView(stateSrvBuffer.Get(),nullptr,&oldSrv0)),"create state SRV0",checks);oldSrv1=oldSrv0;
    b.BindFlags=D3D11_BIND_UNORDERED_ACCESS;require(SUCCEEDED(device->CreateBuffer(&b,nullptr,&stateUavBuffer)),"create state UAV buffer",checks);require(SUCCEEDED(device->CreateUnorderedAccessView(stateUavBuffer.Get(),nullptr,&oldUav)),"create state UAV",checks);
    ID3D11Buffer* rawCb=oldCb.Get();ID3D11ShaderResourceView* oldSrvs[]={oldSrv0.Get(),oldSrv1.Get()};ID3D11UnorderedAccessView* rawUav=oldUav.Get();UINT preserve=~0u;
    ctx->CSSetShader(oldCs.Get(),nullptr,0);ctx->CSSetConstantBuffers(0,1,&rawCb);ctx->CSSetShaderResources(0,2,oldSrvs);ctx->CSSetUnorderedAccessViews(0,1,&rawUav,&preserve);

    OriginalDrawIdentityCaptureResources shared;OriginalDrawIdentityCapture capture;
    require(capture.capture(shared,device,ctx,3,0),"capture selected instances",checks);
    ComPtr<ID3D11ComputeShader> gotCs;ComPtr<ID3D11Buffer> gotCb;ID3D11ShaderResourceView* gotSrvs[2]{};ComPtr<ID3D11UnorderedAccessView> gotUav;
    ctx->CSGetShader(&gotCs,nullptr,nullptr);ctx->CSGetConstantBuffers(0,1,&gotCb);ctx->CSGetShaderResources(0,2,gotSrvs);ctx->CSGetUnorderedAccessViews(0,1,&gotUav);
    require(gotCs.Get()==oldCs.Get()&&gotCb.Get()==oldCb.Get()&&gotSrvs[0]==oldSrv0.Get()&&gotSrvs[1]==oldSrv1.Get()&&gotUav.Get()==oldUav.Get(),"restore compute state",checks);
    for(auto* p:gotSrvs)if(p)p->Release();
    ComPtr<ID3D11InputLayout> gotLayout;ComPtr<ID3D11Buffer> gotVb;UINT gotStride=0,gotOffset=0;ComPtr<ID3D11ShaderResourceView> gotPool;
    ctx->IAGetInputLayout(&gotLayout);ctx->IAGetVertexBuffers(0,1,&gotVb,&gotStride,&gotOffset);ctx->VSGetShaderResources(33,1,&gotPool);
    require(gotLayout.Get()==layout.Get()&&gotVb.Get()==vb.Get()&&gotStride==stride&&gotOffset==offset&&gotPool.Get()==poolView.Get(),"source IA and VS bindings unchanged",checks);

    OriginalDrawIdentitySnapshot snap;auto polled=capture.poll(ctx,snap);require(polled==OriginalDrawIdentityPoll::Pending||polled==OriginalDrawIdentityPoll::Ready,"first poll is async",checks);
    if(polled==OriginalDrawIdentityPoll::Pending){ctx->Flush();for(unsigned i=0;i<200&&polled==OriginalDrawIdentityPoll::Pending;++i){Sleep(1);polled=capture.poll(ctx,snap);}}
    require(polled==OriginalDrawIdentityPoll::Ready,"identity readback ready",checks);
    require(snap.count==3&&snap.records[0].instanceAndModelDataIndex[0]==0&&snap.records[1].instanceAndModelDataIndex[0]==2,"sample offsets and IDs",checks);
    require(snap.records[0].valid&&!snap.records[0].skinned&&snap.records[1].valid&&snap.records[1].skinned,"model payload and skin flag",checks);
    require(!snap.records[2].valid,"out of range model ID invalid",checks);
    require(std::memcmp(snap.records[1].modelRecord.data(),models.data()+4*kOriginalDrawIdentityModelBytes,kOriginalDrawIdentityModelBytes)==0,"t33 FirstElement honored",checks);
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> imageRead;require(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&imageRead)),"create image readback",checks);ctx->CopyResource(imageRead.Get(),image.Get());ctx->Flush();D3D11_MAPPED_SUBRESOURCE imageMap{};require(SUCCEEDED(ctx->Map(imageRead.Get(),0,D3D11_MAP_READ,0,&imageMap)),"map untouched image",checks);const auto* pixel=static_cast<const uint8_t*>(imageMap.pData);require(pixel[0]==0&&pixel[1]==255&&pixel[2]==0&&pixel[3]==255,"capture leaves image unchanged",checks);ctx->Unmap(imageRead.Get(),0);

    // Reuse the exact same two game resource pointers but overwrite both
    // payloads. A later capture must observe bytes, not infer identity from
    // stable bindings or from the model index alone.
    ids[1].b=777;models[2*kOriginalDrawIdentityModelBytes+20]^=0x5a;
    ctx->UpdateSubresource(vb.Get(),0,nullptr,ids,0,0);ctx->UpdateSubresource(pool.Get(),0,nullptr,models.data(),0,0);
    OriginalDrawIdentityCapture second;require(second.capture(shared,device,ctx,1,0),"recapture reused bindings",checks);
    OriginalDrawIdentitySnapshot changed;auto changedPoll=second.poll(ctx,changed);if(changedPoll==OriginalDrawIdentityPoll::Pending){ctx->Flush();for(unsigned i=0;i<200&&changedPoll==OriginalDrawIdentityPoll::Pending;++i){Sleep(1);changedPoll=second.poll(ctx,changed);}}
    require(changedPoll==OriginalDrawIdentityPoll::Ready,"reused binding readback ready",checks);
    require(changed.records[0].instanceAndModelDataIndex[0]==snap.records[0].instanceAndModelDataIndex[0]&&changed.records[0].instanceAndModelDataIndex[1]!=snap.records[0].instanceAndModelDataIndex[1]&&changed.records[0].modelRecord[20]!=snap.records[0].modelRecord[20],"reused pointers expose changed payload",checks);

    OriginalDrawIdentityCapture rejected;require(!rejected.capture(shared,device,ctx,33,0)&&rejected.reason()==OriginalDrawIdentityReason::InvalidArgument,"too many instances rejected",checks);
    ID3D11ShaderResourceView* noPool=nullptr;ctx->VSSetShaderResources(33,1,&noPool);
    require(!rejected.capture(shared,device,ctx,1,0)&&rejected.reason()==OriginalDrawIdentityReason::MissingModelPool,"missing pool rejected",checks);
    D3D11_INPUT_ELEMENT_DESC wrong=element;wrong.InstanceDataStepRate=2;ComPtr<ID3D11InputLayout> wrongLayout;require(SUCCEEDED(device->CreateInputLayout(&wrong,1,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),&wrongLayout)),"create wrong layout",checks);
    originalDrawIdentityRememberLayout(wrongLayout.Get(),&wrong,1,0xEB5234DB6ADB491Dull);ctx->IASetInputLayout(wrongLayout.Get());
    require(!rejected.capture(shared,device,ctx,1,0)&&rejected.reason()==OriginalDrawIdentityReason::IneligibleLayout,"bad layout rejected",checks);
}
} // namespace identity_capture_test
