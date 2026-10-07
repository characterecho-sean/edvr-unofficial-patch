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
    // The freshness stamp the prep shader's EN[276].x reads.
    constexpr uint32_t kCaptureStamp = 77;
    emit::Pose pose{};pose.w[3]=0x7fff7fffu;pose.w[4]=0xfffe7fffu;
    for(unsigned i=2;i<4;++i) {
        pool[i][1]=pool[i][77]=0x3f800000u;
        pool[i][2]=pool[i][78]=pose.w[3];pool[i][3]=pool[i][79]=pose.w[4];
        pool[i][72]=(i==2?emit::kJoined:emit::kMasked)^emit::markerHash(pose,pose,kCaptureStamp);
    }
    float sceneNow[277][4]{},scenePrevious[277][4]{};
    frame.camera[0][0]=frame.camera[1][1]=frame.camera[2][3]=frame.camera[4][2]=1;
    frame.camera[3][2]=.025f;frame.camera[5][0]=1.25f;
    std::memcpy(frame.previousCamera,frame.camera,sizeof(frame.camera));frame.previousCamera[5][0]=1;
    std::memcpy(sceneNow+270,frame.camera,sizeof(frame.camera));
    std::memcpy(scenePrevious+270,frame.previousCamera,sizeof(frame.previousCamera));
    {const uint32_t stamp=kCaptureStamp;std::memcpy(&sceneNow[276][0],&stamp,4);std::memcpy(&scenePrevious[276][0],&stamp,4);}
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
    // A live frame: history stood, and the raster phase and the phase the camera rows carry are
    // nonzero (the upstream camera injector). The writer must record all four phases.
    frame.jitterX=.25f;frame.jitterY=-.375f;frame.previousJitterX=-.125f;frame.previousJitterY=.5f;
    frame.rowsJitterX=.25f;frame.rowsJitterY=-.375f;frame.previousRowsJitterX=-.125f;frame.previousRowsJitterY=.5f;
    frame.staticScene=true; // the 3D main menu's stale-slot policy was on for this frame: the writer must say so
    const auto before=edvr::flatMonoResolveStats();
    capture.arm(6);check(capture.active(),"manual arm creates output directory");
    const fs::path directory=capture.directory();
    // A reset frame is never a sample (2026-09-29): nothing is copied, and the arm stays open
    // for the next frame. The frame right after an F10 arm was one, at phase (0,0).
    {
        edvr::FlatMonoResolveFrame resetFrame=frame;resetFrame.frame=6;
        capture.capture(device,context,resetFrame,true,sources);context->Flush();
        for(unsigned i=0;i<20;++i){capture.poll(context,6);Sleep(1);}
        check(capture.active() && !fs::exists(directory/L"frame_6.json"),"a reset frame is never a sample and the arm stays open");
    }
    capture.capture(device,context,frame,false,sources);
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
    {   // The writer records the phases the rows carry (a replay removes them as the shader does).
        std::ifstream js(manifest,std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(js)),std::istreambuf_iterator<char>());
        check(body.find("\"reset\":false")!=std::string::npos &&
              body.find("\"jitter\":[0.25,-0.375],\"previous_jitter\":[-0.125,0.5],\"rows_jitter\":[0.25,-0.375],\"previous_rows_jitter\":[-0.125,0.5]")!=std::string::npos,
              "the manifest records the raster phases and the phases the camera rows carry");
        check(body.find("\"static_scene\":true")!=std::string::npos && body.find("\"static_scene\":false")==std::string::npos,
              "the manifest records that the frame took the main menu's stale-slot policy");
    }
    {   // The second sample follows the first on the next live frame; later ones keep the old spacing.
        edvr::FlatPixelCapture burst;
        burst.arm(500);const fs::path burstDir=burst.directory();
        auto sampleAt=[&](uint64_t number,bool reset){
            frame.frame=number;burst.capture(device,context,frame,reset,sources);context->Flush();
            for(unsigned attempt=0;attempt<120 && burst.active() && !fs::exists(burstDir/(L"frame_"+std::to_wstring(number)+L".json")) && attempt<120;++attempt)
                {burst.poll(context,number);Sleep(1);}
            return fs::exists(burstDir/(L"frame_"+std::to_wstring(number)+L".json"));
        };
        check(!sampleAt(501,true),"burst: a reset frame is not sample 1");
        check(sampleAt(502,false),"burst: the first live frame is sample 1");
        check(sampleAt(503,false),"burst: the second sample follows on the next live frame");
        check(!sampleAt(504,false),"burst: the third sample waits out the old spacing");
        burst.cancel();
    }
    // A queued set must not cross a manual rearm into the next directory.
    frame.frame=22;capture.capture(device,context,frame,false,sources);
    capture.arm(23);const fs::path rearmed=capture.directory();capture.poll(context,24);
    check(!fs::exists(rearmed/L"frame_22.json"),"rearm cannot publish prior pending frame");
    frame.frame=24;frame.engine={};frame.staticScene=false;capture.capture(device,context,frame,false,sources);context->Flush();
    const auto absentManifest=rearmed/L"frame_24.json";
    for(unsigned attempt=0;attempt<120 && !fs::exists(absentManifest);++attempt) {capture.poll(context,25);Sleep(1);}
    std::ifstream absentFile(absentManifest,std::ios::binary);
    const std::string absentJson((std::istreambuf_iterator<char>(absentFile)),std::istreambuf_iterator<char>());
    check(absentJson.find("\"complete\":false")!=std::string::npos && absentJson.find("\"buffers\":[]")!=std::string::npos &&
        !fs::exists(rearmed/L"frame_24_slots.bin"),"absent engine views explicitly recorded without invented resources");
    check(absentJson.find("\"static_scene\":false")!=std::string::npos && absentJson.find("\"static_scene\":true")==std::string::npos,
        "a frame outside the menu records the policy as off");
    capture.poll(context,923);
    check(!capture.active(),"frame boundary expires arm even with no qualified resolve");
    capture.arm(924);capture.cancel();
    check(!capture.active(),"resize/stop cancel clears diagnostic");
    {   // A small HDR route fixture forces a nonzero ROI origin without a 4K WARP allocation.
        // Two captures are queued before either readback is polled: the second is genuinely
        // the next live frame, and the game's final H changes between them.
        constexpr UINT hw=17,hh=5,cw=9,ch=3,cx=4,cy=1;
        const DXGI_FORMAT hf[]={DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R32_FLOAT,
            DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_R11G11B10_FLOAT};
        const UINT hb[]={4,4,4,1,8,4};
        std::array<std::vector<unsigned char>,6> hp;
        std::array<ComPtr<ID3D11Texture2D>,6> ht;
        ID3D11Texture2D* hs[6]{};
        for(unsigned i=0;i<6;++i) {
            hp[i].resize(hw*hh*hb[i]);
            for(unsigned y=0;y<hh;++y)for(unsigned x=0;x<hw;++x) {
                unsigned char* dst=hp[i].data()+(y*hw+x)*hb[i];
                if(i==0 || i==5) {
                    // Red is 1+small mantissa; green 2; blue .5. Final has a
                    // different mantissa, proving H was captured after the draw.
                    const uint32_t bits=((14u<<5)<<22) | ((16u<<6)<<11) | ((15u<<6)+(i==5?16u:0u)+x+y);
                    std::memcpy(dst,&bits,4);
                } else if(i==1) {
                    const float z=.01f+float(y)*.01f;std::memcpy(dst,&z,4);
                } else if(i==3)dst[0]=x%2?255:0;
                else if(i==4) {
                    const uint16_t half[4]={0x3c00,0x4000,0x3800,0x3c00};std::memcpy(dst,half,8);
                }
            }
            D3D11_TEXTURE2D_DESC d{};d.Width=hw;d.Height=hh;d.ArraySize=d.MipLevels=1;
            d.SampleDesc.Count=1;d.Format=hf[i];d.Usage=D3D11_USAGE_DEFAULT;
            D3D11_SUBRESOURCE_DATA init{};init.pSysMem=hp[i].data();init.SysMemPitch=hw*hb[i];
            check(SUCCEEDED(device->CreateTexture2D(&d,&init,&ht[i])),"HDR route source texture created");
            hs[i]=ht[i].Get();
        }
        std::vector<float> hslots(hw*hh*4,0);
        for(unsigned i=0;i<hw*hh;++i){hslots[i*4+2]=float(i);hslots[i*4+3]=19;}
        D3D11_TEXTURE2D_DESC sd{};sd.Width=hw;sd.Height=hh;sd.ArraySize=sd.MipLevels=1;
        sd.SampleDesc.Count=1;sd.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;sd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA si{};si.pSysMem=hslots.data();si.SysMemPitch=hw*16;
        ComPtr<ID3D11Texture2D> st;ComPtr<ID3D11ShaderResourceView> sv;
        check(SUCCEEDED(device->CreateTexture2D(&sd,&si,&st)) &&
            SUCCEEDED(device->CreateShaderResourceView(st.Get(),nullptr,&sv)),"HDR cropped engine slot texture created");
        if(!st || !sv || !hs[0] || !hs[5])return captureFailures;
        edvr::FlatMonoResolveFrame hf1=frame;hf1.hdr=true;hf1.frame=101;hf1.mode=edvr::FlatMonoResolveMode::Dlaa;
        hf1.renderWidth=hf1.outputWidth=hw;hf1.renderHeight=hf1.outputHeight=hh;
        hf1.engine={sv.Get(),poolView.Get(),nowBuffer.Get(),previousBuffer.Get()};
        edvr::FlatPixelCapture hcap;hcap.arm(100);const fs::path hdir=hcap.directory();
        hcap.capture(device,context,hf1,false,hs,false,cw,ch);
        std::vector<unsigned char> final2=hp[5];
        for(unsigned y=0;y<hh;++y)for(unsigned x=0;x<hw;++x) {
            uint32_t bits=0;std::memcpy(&bits,final2.data()+(y*hw+x)*4,4);bits+=5;
            std::memcpy(final2.data()+(y*hw+x)*4,&bits,4);
        }
        context->UpdateSubresource(ht[5].Get(),0,nullptr,final2.data(),hw*4,0);
        hf1.frame=102;hcap.capture(device,context,hf1,false,hs,false,cw,ch);
        check(hcap.active(),"HDR route accepts two consecutive queued captures");
        context->Flush();
        for(unsigned attempt=0;attempt<240 && (!fs::exists(hdir/L"frame_101.json") || !fs::exists(hdir/L"frame_102.json"));++attempt)
            {hcap.poll(context,103);Sleep(1);}
        check(fs::exists(hdir/L"frame_101.json") && fs::exists(hdir/L"frame_102.json"),
              "two pending HDR readbacks publish consecutive manifests");
        for(unsigned frameNo=101;frameNo<=102;++frameNo) {
            const auto sourceFinal=frameNo==101?hp[5]:final2;
            for(unsigned i=0;i<6;++i) {
                const auto& source=i==5?sourceFinal:hp[i];
                std::vector<unsigned char> expected(cw*ch*hb[i]);
                for(unsigned y=0;y<ch;++y)
                    std::memcpy(expected.data()+y*cw*hb[i],source.data()+((cy+y)*hw+cx)*hb[i],cw*hb[i]);
                std::ifstream file(hdir/(std::string("frame_")+std::to_string(frameNo)+"_"+names[i]+".bin"),std::ios::binary);
                const std::vector<unsigned char> actual((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
                check(actual==expected,"HDR format bytes and nonzero ROI origin match their frame's source");
            }
            std::vector<float> expectedSlots(cw*ch*4);
            for(unsigned y=0;y<ch;++y)std::memcpy(expectedSlots.data()+y*cw*4,hslots.data()+((cy+y)*hw+cx)*4,cw*16);
            std::ifstream slotFile(hdir/(std::string("frame_")+std::to_string(frameNo)+"_slots.bin"),std::ios::binary);
            const std::vector<unsigned char> slotBytes((std::istreambuf_iterator<char>(slotFile)),std::istreambuf_iterator<char>());
            check(slotBytes.size()==expectedSlots.size()*4 && !std::memcmp(slotBytes.data(),expectedSlots.data(),slotBytes.size()),
                "HDR ROI ownership preserves exact primitive and draw-token channels");
            std::ifstream mf(hdir/(std::string("frame_")+std::to_string(frameNo)+".json"),std::ios::binary);
            const std::string body((std::istreambuf_iterator<char>(mf)),std::istreambuf_iterator<char>());
            check(body.find("\"route\":\"hdr\"")!=std::string::npos &&
                  body.find("\"capture_roi\":{\"x\":4,\"y\":1,\"width\":9,\"height\":3}")!=std::string::npos &&
                  body.find("\"final_provenance\":\"scene-H-after-finish-before-tonemap\"")!=std::string::npos &&
                  body.find("\"complete\":true")!=std::string::npos,
                  "HDR manifest identifies ROI, final target provenance and retained engine evidence");
        }
        hcap.cancel();
    }
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
