#pragma once
#include "../../src/d3d11/flat_resolve_input_capture.h"
#include <filesystem>
#include <fstream>
#include <iterator>

inline int flatResolveInputGpuTests(ID3D11Device* device,ID3D11DeviceContext* context) {
    namespace fs=std::filesystem;
    using edvr::FlatMonoResolveFrame;
    using edvr::FlatMonoResolveMode;
    int failed=0;
    auto expect=[&](bool ok,const char* what) {if(!ok) {std::printf("FAIL: resolve inputs %s\n",what);++failed;}};
    constexpr UINT width=16,height=12;
    std::array<std::vector<uint8_t>,9> bytes;
    const UINT bpp[]={4,8,4,1,1,16};
    for(unsigned i=0;i<6;++i) {
        bytes[i].resize(width*height*bpp[i]);
        for(size_t j=0;j<bytes[i].size();++j)bytes[i][j]=uint8_t(j*13+i*7);
    }
    // Marks on both edges prove that the diagnostic saves full planes, not an ROI.
    for(unsigned i=3;i<5;++i) {
        std::fill(bytes[i].begin(),bytes[i].end(),uint8_t(0));bytes[i][0]=255;bytes[i].back()=128;bytes[i][width+3]=1;
    }
    const DXGI_FORMAT formats[]={DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R32G32B32A32_FLOAT};
    std::array<ComPtr<ID3D11Texture2D>,6> textures;
    std::array<ComPtr<ID3D11ShaderResourceView>,6> views;
    for(unsigned i=0;i<textures.size();++i) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=formats[i];desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|(i==1?D3D11_BIND_DEPTH_STENCIL:D3D11_BIND_RENDER_TARGET);
        D3D11_SUBRESOURCE_DATA initial{bytes[i].data(),width*bpp[i],width*height*bpp[i]};
        expect(SUCCEEDED(device->CreateTexture2D(&desc,&initial,&textures[i])),"fixture texture creates");
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};view.Format=i==1?DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:formats[i];view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels=UINT(-1);
        if(textures[i])expect(SUCCEEDED(device->CreateShaderResourceView(textures[i].Get(),&view,&views[i])),"all-mips one-level view creates");
    }
    for(const auto& view:views)if(!view)return failed+1;
    std::array<ComPtr<ID3D11Buffer>,3> buffers;
    for(unsigned i=0;i<buffers.size();++i) {
        auto& payload=bytes[i+6];payload.resize(i?5376:1008);
        for(size_t j=0;j<payload.size();++j)payload[j]=uint8_t(j*17+i*9);
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT(payload.size());desc.BindFlags=i?D3D11_BIND_CONSTANT_BUFFER:D3D11_BIND_SHADER_RESOURCE;
        if(!i) {desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.StructureByteStride=336;}
        D3D11_SUBRESOURCE_DATA initial{payload.data(),0,0};expect(SUCCEEDED(device->CreateBuffer(&desc,&initial,&buffers[i])),"engine buffer creates");
    }
    ComPtr<ID3D11ShaderResourceView> poolView;
    D3D11_SHADER_RESOURCE_VIEW_DESC poolDesc{};poolDesc.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;poolDesc.Buffer.FirstElement=1;poolDesc.Buffer.NumElements=2;
    if(buffers[0])expect(SUCCEEDED(device->CreateShaderResourceView(buffers[0].Get(),&poolDesc,&poolView)),"pool view keeps nonzero first element");
    for(const auto& buffer:buffers)if(!buffer)return failed+1;
    if(!poolView)return failed+1;
    FlatMonoResolveFrame f{};f.frame=60000;f.hdr=true;f.reset=true;f.mode=FlatMonoResolveMode::Dlss;f.deltaMs=16;
    f.renderWidth=f.outputWidth=width;f.renderHeight=f.outputHeight=height;camera(f.camera);camera(f.previousCamera);
    f.color=views[0].Get();f.depth=views[1].Get();f.cleanColor=views[2].Get();f.overlayCoverage=views[3].Get();f.untrustedCameraCoverage=views[4].Get();
    f.engine={views[5].Get(),poolView.Get(),buffers[1].Get(),buffers[2].Get()};
    f.staticScene=true;f.steadyDetail=true;f.previousRowsJitterX=.125f;
    edvr::FlatResolveInputCapture capture;
    capture.capture(device,context,f);capture.poll(context,f.frame);capture.cancel();
    expect(capture.copyOperations()==0 && capture.directory().empty(),"unarmed capture/poll/cancel is inert");
    auto read=[&](ID3D11Texture2D* source,UINT stride) {
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;std::vector<uint8_t> result;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&stage)))return result;
        context->CopyResource(stage.Get(),source);D3D11_MAPPED_SUBRESOURCE map{};
        if(FAILED(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&map)))return result;
        result.resize(stride*desc.Height);
        for(UINT y=0;y<desc.Height;++y)std::memcpy(result.data()+y*stride,static_cast<const uint8_t*>(map.pData)+y*map.RowPitch,stride);
        context->Unmap(stage.Get(),0);return result;
    };
    capture.arm(f.frame-1);const fs::path folder=capture.directory();
    context->PSSetShaderResources(7,1,views[0].GetAddressOf());
    const auto beforeH=read(textures[0].Get(),width*4),beforeD=read(textures[1].Get(),width*8);
    capture.capture(device,context,f);
    ComPtr<ID3D11ShaderResourceView> stillBound;context->PSGetShaderResources(7,1,&stillBound);
    expect(stillBound.Get()==views[0].Get() && read(textures[0].Get(),width*4)==beforeH && read(textures[1].Get(),width*8)==beforeD,"snapshot changes neither H/depth nor bindings");
    // Later mutations cannot overwrite the owned queued snapshot.
    std::vector<uint8_t> later(width*height,31);
    context->UpdateSubresource(textures[3].Get(),0,nullptr,later.data(),width,width*height);
    for(unsigned n=0;n<200;++n) {capture.poll(context,f.frame+1);Sleep(1);}
    const char* names[]={"color","depth","clean_color","overlay_coverage","untrusted_coverage","slots","pool","scene_now","scene_previous"};
    for(unsigned i=0;i<bytes.size();++i) {
        std::ifstream stream(folder/("inputs_60000_1_"+std::string(names[i])+"_0.bin"),std::ios::binary);
        std::vector<uint8_t> actual((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());
        expect(actual==bytes[i],"full-plane logical bytes preserve all edges and pre-mutation state");
    }
    auto text=[](const fs::path& p) {std::ifstream file(p,std::ios::binary);return std::string((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());};
    const auto body=text(folder/"inputs_frame_60000_1.json");
    expect(body.find("\"stage\":\"prebackend\"")!=std::string::npos && body.find("\"backend_status\":\"not-run\"")!=std::string::npos && body.find("\"complete\":true")!=std::string::npos && body.find("\"reset\":true")!=std::string::npos,"manifest describes input evidence, not a backend result, including reset");
    expect(body.find("\"static_scene\":true")!=std::string::npos && body.find("\"steady_detail\":true")!=std::string::npos && body.find("\"previous_rows_jitter\":[0.125,0]")!=std::string::npos && body.find("\"first_element\":1")!=std::string::npos,"prep flags, previous row phase and full pool view range remain attributable");
    expect(body.find("\"resource_format\":2")!=std::string::npos && body.find("\"row_stride\":256")!=std::string::npos,"RGBA ownership snapshot accounts all four float components exactly");
    ++f.frame;capture.capture(device,context,f);++f.frame;capture.capture(device,context,f);
    for(unsigned n=0;n<200 && capture.active();++n) {capture.poll(context,f.frame);Sleep(1);}
    expect(!capture.active() && capture.copyOperations()==18 && !fs::exists(folder/"inputs_frame_60002_3.json"),"two bounded attempts stop after pending work completes");
    capture.arm(60100);f.frame=60101;capture.capture(device,context,f);capture.poll(context,60221);
    expect(text(fs::path(capture.directory())/"inputs_frame_60101_1.json").find("\"status\":\"timeout\"")!=std::string::npos,"expired readback publishes explicit partial status");capture.cancel();
    uint64_t attemptBytes=0;for(const auto& payload:bytes)attemptBytes+=payload.size();
    edvr::FlatResolveInputCapture limited(2*attemptBytes-1);f.frame=60400;limited.arm(f.frame-1);
    limited.capture(device,context,f);++f.frame;limited.capture(device,context,f);
    const auto limitedFolder=fs::path(limited.directory());
    expect(limited.copyOperations()==9 && text(limitedFolder/"inputs_frame_60401_2.json").find("\"status\":\"budget-cap\"")!=std::string::npos,"second attempt over budget queues no partial engine or image set");
    for(unsigned n=0;n<200 && limited.active();++n) {limited.poll(context,f.frame+1);Sleep(1);}
    expect(!limited.active() && text(limitedFolder/"inputs_frame_60400_1.json").find("\"complete\":true")!=std::string::npos,"budget refusal still polls the complete first attempt");
    edvr::FlatResolveInputCapture blocked;f.frame=60800;blocked.arm(f.frame-1);blocked.capture(device,context,f);
    const auto blockedManifest=fs::path(blocked.directory())/"inputs_frame_60800_1.json";
    fs::remove(blockedManifest);fs::create_directory(blockedManifest);
    const auto beforeFailureLogs=resolveInputLines.size();
    for(unsigned n=0;n<200;++n) {blocked.poll(context,f.frame+1);Sleep(1);}
    bool failureLogged=false,falseComplete=false;
    for(size_t i=beforeFailureLogs;i<resolveInputLines.size();++i) {
        const auto& line=resolveInputLines[i];
        if(line.find("frame=60800")!=std::string::npos) {
            failureLogged|=line.find("metadata publication failed")!=std::string::npos || line.find("status=metadata-write-failed")!=std::string::npos;
            falseComplete|=line.find("status=complete")!=std::string::npos;
        }
    }
    expect(failureLogged && !falseComplete && fs::is_directory(blockedManifest),"blocked metadata publication logs failure and never claims a completed frame");blocked.cancel();

    // Exercise the actual resolver arm/capture/refusal/poll path for every SDK.
    const fs::path root=fs::path(edvr::Config::get().logDir())/"flat_pixels";
    for(const auto mode:{FlatMonoResolveMode::Dlaa,FlatMonoResolveMode::Dlss,FlatMonoResolveMode::Fsr}) {
        edvr::flatMonoResolveReset();std::vector<fs::path> before;
        for(const auto& entry:fs::directory_iterator(root))before.push_back(entry.path());
        f.mode=mode;f.frame+=200;edvr::flatMonoResolveArmPixels(f.frame-1);
        fs::path actualFolder;
        for(const auto& entry:fs::directory_iterator(root))if(entry.path().filename().wstring().find(L"resolve_inputs_")==0 && std::find(before.begin(),before.end(),entry.path())==before.end())actualFolder=entry.path();
        const auto hdrBefore=read(textures[0].Get(),width*4);const int callsBefore=backendCalls;
        ComPtr<ID3D11ShaderResourceView> output;const char* reason=nullptr;
        const bool result=edvr::flatMonoResolve(device,context,f,&output,&reason);
        expect(!result && reason && std::strstr(reason,"untrusted-coverage-requires-native-HDR-TAA") && backendCalls==callsBefore && read(textures[0].Get(),width*4)==hdrBefore,"configured SDK refuses honestly without TAA fallback or H mutation");
        for(unsigned n=0;n<200;++n) {edvr::flatMonoResolvePollPixels(context,f.frame+1);Sleep(1);}
        const auto manifestPath=actualFolder/("inputs_frame_"+std::to_string(f.frame)+"_1.json");
        const auto resultText=text(manifestPath);
        const char* name=mode==FlatMonoResolveMode::Dlaa?"dlaa":mode==FlatMonoResolveMode::Dlss?"dlss":"fsr";
        expect(!actualFolder.empty() && resultText.find("\"complete\":true")!=std::string::npos && resultText.find(std::string("\"mode\":\"")+name+"\"")!=std::string::npos,"production preguard capture survives SDK refusal and Present poll with configured mode");
        edvr::flatMonoResolveReset();
    }
    // Keep one fixture for the Python reader gate without checking in payloads.
    std::ofstream pointer(fs::path(edvr::Config::get().logDir())/"flat_resolve_inputs_fixture.txt",std::ios::binary);
    pointer<<folder.u8string();
    return failed;
}
