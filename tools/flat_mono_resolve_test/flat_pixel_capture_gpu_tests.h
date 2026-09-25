#pragma once
#include "../../src/d3d11/flat_pixel_capture.h"
#include <filesystem>
#include <fstream>

inline int flatPixelCaptureGpuTests(ID3D11Device* device,ID3D11DeviceContext* context) {
    namespace fs=std::filesystem;
    using Microsoft::WRL::ComPtr;
    int captureFailures=0;
    auto check=[&](bool ok,const char* what){if(!ok){std::printf("FAIL: flat pixels GPU %s\n",what);++captureFailures;}};
    const fs::path fixtureRoot=edvr::Config::get().logDir();
    const auto pointer=fixtureRoot/L"current_fixture.txt";
    std::error_code removeError;fs::remove(pointer,removeError);
    check(!removeError,"old fixture pointer removed before this producer run");
    edvr::FlatPixelCapture capture;
    capture.poll(context,1);
    check(!capture.active() && capture.directory().empty(),"inactive poll creates no capture state");
    constexpr unsigned width=17,height=3;
    const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT,
        DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_TYPELESS};
    const char* names[]={"color","depth","motion","rejection","raw","final"};
    std::array<std::vector<unsigned char>,6> payload;
    std::array<ComPtr<ID3D11Texture2D>,6> textures;
    ID3D11Texture2D* sources[6]{};
    for(unsigned i=0;i<6;++i) {
        const unsigned bpp=i==3?1:4;payload[i].resize(width*height*bpp);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            uint32_t pixel=0;
            if(i==0 || i==5)pixel=0xff000000u | (x+1) | ((y+1)<<8);
            if(i==1) {const float depth=.01f+float(y)*.01f;std::memcpy(&pixel,&depth,4);}
            if(i==3)pixel=255;
            if(i==4)pixel=0xff00ff00u;
            std::memcpy(payload[i].data()+(y*width+x)*bpp,&pixel,bpp);
        }
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.ArraySize=d.MipLevels=1;
        d.SampleDesc.Count=1;d.Format=formats[i];d.Usage=D3D11_USAGE_DEFAULT;
        D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=payload[i].data();initial.SysMemPitch=width*bpp;
        check(SUCCEEDED(device->CreateTexture2D(&d,&initial,textures[i].GetAddressOf())),"fixture source texture created");
        if(!textures[i])return captureFailures;
        sources[i]=textures[i].Get();
    }
    edvr::FlatMonoResolveFrame frame{};frame.frame=7;frame.renderWidth=frame.outputWidth=width;
    frame.renderHeight=frame.outputHeight=height;frame.mode=edvr::FlatMonoResolveMode::Dlss;frame.configuredDlssPreset=11;
    const auto before=edvr::flatMonoResolveStats();
    capture.arm(6);check(capture.active(),"manual arm creates output directory");
    const fs::path directory=capture.directory();
    capture.capture(device,context,frame,true,sources);
    // The test may flush to advance WARP; the production capture never does.
    context->Flush();
    const auto manifest=directory/L"frame_7.json";
    for(unsigned attempt=0;attempt<120 && !fs::exists(manifest);++attempt) {capture.poll(context,8);Sleep(1);}
    check(fs::exists(manifest) && !fs::exists(directory/L"frame_7.json.tmp"),"complete manifest committed atomically");
    for(unsigned i=0;i<6;++i) {
        const auto path=directory/(std::string("frame_7_")+names[i]+".bin");
        std::ifstream file(path,std::ios::binary);
        std::vector<unsigned char> actual((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        check(actual==payload[i],"all native rows packed exactly, excluding staging RowPitch padding");
    }
    std::printf("flat pixel fixture: %ls\n",manifest.c_str());
    // A queued set must not cross a manual rearm into the next directory.
    frame.frame=22;capture.capture(device,context,frame,false,sources);
    capture.arm(23);const fs::path rearmed=capture.directory();capture.poll(context,24);
    check(!fs::exists(rearmed/L"frame_22.json"),"rearm cannot publish prior pending frame");
    capture.poll(context,923);
    check(!capture.active(),"frame boundary expires arm even with no qualified resolve");
    capture.arm(924);capture.cancel();
    check(!capture.active(),"resize/stop cancel clears diagnostic");
    const auto after=edvr::flatMonoResolveStats();
    check(before.acceptedResets==after.acceptedResets && before.acceptedContinues==after.acceptedContinues &&
        before.fullResets==after.fullResets && before.invalidations==after.invalidations,"manual diagnostic never changes temporal history");
    if(!captureFailures) {
        std::ofstream file(pointer,std::ios::binary);
        file<<fs::relative(manifest,fixtureRoot).generic_u8string()<<"\n";
        file.close();check(bool(file),"current producer fixture pointer written");
    }
    return captureFailures;
}
