#pragma once
#include <cstdio>
#include "../../src/d3d11/flat_runtime_model.h"
#include "../../src/d3d11/flat_projection_recipes.h"

inline int flatProjectionViewportTests() {
    using namespace edvr;
    int failures=0;
    auto expect=[&](bool ok,const char* name) {
        if(!ok) { std::printf("FAIL: projection viewport %s\n",name);++failures; }
    };
    const void* output=reinterpret_cast<void*>(0x100);
    const void* depth=reinterpret_cast<void*>(0x200);
    FlatContractObservation hdr{};
    hdr.color=reinterpret_cast<void*>(0x300);hdr.rtv=reinterpret_cast<void*>(0x301);
    hdr.depth=depth;hdr.dsv=reinterpret_cast<void*>(0x201);
    hdr.width=hdr.depthWidth=1920;hdr.height=hdr.depthHeight=1080;hdr.format=26;
    hdr.viewportCount=1;hdr.viewport[2]=1920;hdr.viewport[3]=1080;
    auto accepts=[&](const FlatContractObservation& k) {
        return flatRuntimeProjectionViewport(k.viewportCount,k.viewport,1920,1080,
            flatRuntimeProjectionHdr(k,output,depth));
    };
    // Verified f6c59ba6 Epic frame 44833, q365/367/368: these already have
    // recipes; their full-XY, 0..0 HDR viewport failed qualification.
    const uint64_t pairs[][2]={
        {0xF8FA801F2CB1E27Cull,0x84965D3C050FB01Bull},
        {0x68DDDEF04D9894AFull,0x06332CA168B6DA63ull},
        {0xF7A6E916F14A3B1Aull,0x06332CA168B6DA63ull}
    };
    for(const auto& pair:pairs) {
        hdr.vs=pair[0];hdr.ps=pair[1];
        expect(flatProjectionDrawRecipes(hdr.vs,hdr.ps).count && accepts(hdr),
               "captured known HDR recipe accepts depth-clamped viewport");
    }
    expect(!flat_mono_detail::fullViewport(hdr,1920,1080),
           "depth-clamped HDR still cannot name a motion source");
    expect(!flatRuntimeProjectionHdr(hdr,output,nullptr) && !flatRuntimeProjectionHdr(hdr,output,output),
           "missing or mismatched owned depth cannot relax viewport");
    for(uint32_t format:{9u,23u,28u,60u}) {
        auto k=hdr;k.format=format;
        expect(!accepts(k),"only HDR format26 admits depth clamp");
        k.viewport[5]=1;
        expect(accepts(k),"ordinary 0..1 viewport remains valid for other roles");
    }
    for(unsigned change=0;change<8;++change) {
        auto k=hdr;
        switch(change) {
        case 0:k.color=output;break;
        case 1:k.color=nullptr;break;
        case 2:k.rtv=nullptr;break;
        case 3:k.depth=nullptr;break;
        case 4:k.dsv=nullptr;break;
        case 5:--k.depthWidth;break;
        case 6:--k.depthHeight;break;
        case 7:k.width=0;break;
        }
        expect(!accepts(k),"output, absent views and depth layout cannot relax viewport");
    }
    for(uint32_t count:{0u,2u,16u}) {
        auto k=hdr;k.viewportCount=count;
        expect(!accepts(k),"HDR still requires exactly one viewport");
    }
    for(unsigned component=0;component<6;++component) {
        auto k=hdr;
        if(component<2)k.viewport[component]=1;
        else if(component<4)k.viewport[component]-=1;
        else k.viewport[component]=0.5f;
        expect(!accepts(k),"offset, partial extent and intermediate depth ranges rejected");
    }
    auto regular=hdr;regular.viewport[5]=1;
    expect(accepts(regular) && flat_mono_detail::fullViewport(regular,1920,1080),
           "ordinary HDR remains eligible for projection and motion source");
    expect(!flatRuntimeProjectionViewport(1,hdr.viewport,0,1080,true) &&
           !flatRuntimeProjectionViewport(1,hdr.viewport,1920,0,true),
           "zero expected extent rejected");
    return failures;
}
