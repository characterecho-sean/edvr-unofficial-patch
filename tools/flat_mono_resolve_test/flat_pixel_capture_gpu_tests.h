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
    // Engine capture deliberately uses a nonzero SRV first element. The file
    // retains the whole buffer and the reader must apply the view offset.
    std::vector<float> slots(width*height*2);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        slots[(y*width+x)*2]=x%4==0?-1.0f:float((x%4)*2-1);
        slots[(y*width+x)*2+1]=.01f+float(y)*.01f;
    }
    D3D11_TEXTURE2D_DESC slotDesc{};slotDesc.Width=width;slotDesc.Height=height;
    slotDesc.ArraySize=slotDesc.MipLevels=1;slotDesc.SampleDesc.Count=1;slotDesc.Format=DXGI_FORMAT_R32G32_FLOAT;
    slotDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA slotData{};slotData.pSysMem=slots.data();slotData.SysMemPitch=width*8;
    ComPtr<ID3D11Texture2D> slotTexture;ComPtr<ID3D11ShaderResourceView> slotView;
    check(SUCCEEDED(device->CreateTexture2D(&slotDesc,&slotData,&slotTexture)) &&
        SUCCEEDED(device->CreateShaderResourceView(slotTexture.Get(),nullptr,&slotView)),"engine slot source created");
    uint32_t pool[4][84]{};pool[0][0]=0x12345678u;
    namespace emit=edvr::engine_velocity_emit;
    emit::Pose pose{};pose.w[3]=0x7fff7fffu;pose.w[4]=0xfffe7fffu;
    for(unsigned i=2;i<4;++i) {
        pool[i][1]=pool[i][77]=0x3f800000u;
        pool[i][2]=pool[i][78]=pose.w[3];pool[i][3]=pool[i][79]=pose.w[4];
        pool[i][72]=(i==2?emit::kJoined:emit::kMasked)^emit::markerHash(pose,pose);
    }
    float sceneNow[276][4]{},scenePrevious[276][4]{};
    frame.camera[0][0]=frame.camera[1][1]=frame.camera[2][3]=frame.camera[4][2]=1;
    frame.camera[3][2]=.025f;frame.camera[5][0]=1.25f;
    std::memcpy(frame.previousCamera,frame.camera,sizeof(frame.camera));frame.previousCamera[5][0]=1;
    std::memcpy(sceneNow+270,frame.camera,sizeof(frame.camera));
    std::memcpy(scenePrevious+270,frame.previousCamera,sizeof(frame.previousCamera));
    auto makeBuffer=[&](const void* data,UINT bytes,bool structured) {
        D3D11_BUFFER_DESC d{};d.ByteWidth=bytes;d.BindFlags=structured?D3D11_BIND_SHADER_RESOURCE:D3D11_BIND_CONSTANT_BUFFER;
        d.MiscFlags=structured?D3D11_RESOURCE_MISC_BUFFER_STRUCTURED:0;d.StructureByteStride=structured?336:0;
        D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=data;ComPtr<ID3D11Buffer> out;
        check(SUCCEEDED(device->CreateBuffer(&d,&initial,&out)),"engine buffer fixture created");return out;
    };
    auto poolBuffer=makeBuffer(pool,sizeof(pool),true),nowBuffer=makeBuffer(sceneNow,sizeof(sceneNow),false),previousBuffer=makeBuffer(scenePrevious,sizeof(scenePrevious),false);
    D3D11_SHADER_RESOURCE_VIEW_DESC poolDesc{};poolDesc.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
    poolDesc.Buffer.FirstElement=1;poolDesc.Buffer.NumElements=3;
    ComPtr<ID3D11ShaderResourceView> poolView;
    check(poolBuffer && SUCCEEDED(device->CreateShaderResourceView(poolBuffer.Get(),&poolDesc,&poolView)),"pool view retains nonzero first element");
    if(captureFailures)return captureFailures;
    frame.engine={slotView.Get(),poolView.Get(),nowBuffer.Get(),previousBuffer.Get()};
    const auto before=edvr::flatMonoResolveStats();
    capture.arm(6);check(capture.active(),"manual arm creates output directory");
    const fs::path directory=capture.directory();
    capture.capture(device,context,frame,true,sources);
    // Captures must retain the bytes at the resolve, not contents when polled.
    const uint32_t clearedPool[4][84]{};
    context->UpdateSubresource(poolBuffer.Get(),0,nullptr,clearedPool,0,0);
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
    auto capturedBytes=[&](const char* name,const void* expected,size_t size) {
        std::ifstream file(directory/(std::string("frame_7_")+name+".bin"),std::ios::binary);
        std::vector<unsigned char> actual((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        check(actual.size()==size && std::memcmp(actual.data(),expected,size)==0,"engine snapshot bytes match copied moment, including pool records and scene rows");
    };
    capturedBytes("slots",slots.data(),slots.size()*sizeof(float));capturedBytes("pool",pool,sizeof(pool));
    capturedBytes("scene_now",sceneNow,sizeof(sceneNow));capturedBytes("scene_previous",scenePrevious,sizeof(scenePrevious));
    std::printf("flat pixel fixture: %ls\n",manifest.c_str());
    // A queued set must not cross a manual rearm into the next directory.
    frame.frame=22;capture.capture(device,context,frame,false,sources);
    capture.arm(23);const fs::path rearmed=capture.directory();capture.poll(context,24);
    check(!fs::exists(rearmed/L"frame_22.json"),"rearm cannot publish prior pending frame");
    frame.frame=24;frame.engine={};capture.capture(device,context,frame,true,sources);context->Flush();
    const auto absentManifest=rearmed/L"frame_24.json";
    for(unsigned attempt=0;attempt<120 && !fs::exists(absentManifest);++attempt) {capture.poll(context,25);Sleep(1);}
    std::ifstream absentFile(absentManifest,std::ios::binary);
    const std::string absentJson((std::istreambuf_iterator<char>(absentFile)),std::istreambuf_iterator<char>());
    check(absentJson.find("\"complete\":false")!=std::string::npos && absentJson.find("\"buffers\":[]")!=std::string::npos &&
        !fs::exists(rearmed/L"frame_24_slots.bin"),"absent engine views explicitly recorded without invented resources");
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
