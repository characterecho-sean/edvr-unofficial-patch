#pragma once
#include "../../src/d3d11/eye_engine_capture.h"
#include <cstring>

namespace capture_tests {
inline void run(ID3D11Device* device,ID3D11DeviceContext* ctx,void (*check)(bool,const char*)) {
    using Microsoft::WRL::ComPtr;
    namespace capture=edvr::eye_engine_capture;
    ComPtr<ID3D11Texture2D> staged;
    check(capture::stage(ctx,false,nullptr,&staged)==capture::Result::NotBound && !staged,
          "capture: absent engine binding is unavailable, not a fabricated clear map");
    check(capture::stage(ctx,true,nullptr,&staged)==capture::Result::MissingView && !staged,
          "capture: bound engine with absent G6 is explicitly unavailable");
    float values[8]={-1,0,-1,0,-1,0,-1,0};
    D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=2;d.ArraySize=d.MipLevels=1;
    d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32G32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{values,16,0};
    ComPtr<ID3D11Texture2D> source;ComPtr<ID3D11ShaderResourceView> srv;
    check(SUCCEEDED(device->CreateTexture2D(&d,&initial,&source)),"capture: ownership texture created");
    check(source && SUCCEEDED(device->CreateShaderResourceView(source.Get(),nullptr,&srv)),"capture: ownership view created");
    if(!srv)return;
    check(capture::stage(ctx,false,srv.Get(),&staged)==capture::Result::NotBound && !staged,
          "capture: an available view not consumed by preparation is not exported");
    auto verify=[&](const char* label){
        staged.Reset();
        check(capture::stage(ctx,true,srv.Get(),&staged)==capture::Result::Staged && staged,label);
        if(!staged)return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr=ctx->Map(staged.Get(),0,D3D11_MAP_READ,0,&mapped);
        check(SUCCEEDED(hr),"capture: staged ownership readable");
        if(FAILED(hr))return;
        bool equal=true;
        for(unsigned y=0;y<2;++y)equal=equal && !std::memcmp(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch,values+y*4,16);
        ctx->Unmap(staged.Get(),0);
        check(equal,"capture: original slot and depth bits preserved across every row");
    };
    verify("capture: consumed all-clear ownership is available data");
    values[0]=1473;values[1]=0.000123f;values[6]=1459;values[7]=0.25f;
    ctx->UpdateSubresource(source.Get(),0,nullptr,values,16,0);
    verify("capture: nonclear ownership snapshot exported from actual view");
    uint32_t words[84];for(unsigned i=0;i<84;++i)words[i]=0xFF123400u+i;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(words);bd.StructureByteStride=336;
    bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA bi{words,0,0};ComPtr<ID3D11Buffer> pool,poolStage;
    check(SUCCEEDED(device->CreateBuffer(&bd,&bi,&pool)),"capture: actual private-pool fixture created");
    check(capture::stageBuffer(ctx,false,pool.Get(),&poolStage)==capture::Result::NotBound && !poolStage,"capture: unconsumed pool not fabricated");
    check(capture::stageBuffer(ctx,true,nullptr,&poolStage)==capture::Result::MissingView,"capture: absent EN stamp explicitly unavailable");
    check(capture::stageBuffer(ctx,true,pool.Get(),&poolStage)==capture::Result::Staged,"capture: actual private pool bytes staged");
    D3D11_MAPPED_SUBRESOURCE bm{};
    if(poolStage && SUCCEEDED(ctx->Map(poolStage.Get(),0,D3D11_MAP_READ,0,&bm))){
        check(!std::memcmp(bm.pData,words,sizeof(words)),"capture: actual pool marker/material/stamp bits preserved");ctx->Unmap(poolStage.Get(),0);
    }else check(false,"capture: actual pool staging readable");
}
}
