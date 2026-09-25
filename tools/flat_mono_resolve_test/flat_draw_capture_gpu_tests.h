#pragma once
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <array>
#include <string>
#include <vector>

namespace edvr { inline bool captureFlatProbeShader(char,uint64_t){return true;} }
#include "../../src/d3d11/flat_draw_capture.h"

inline int flatDrawCaptureGpuTests(ID3D11Device* device,ID3D11DeviceContext* context) {
    namespace fs=std::filesystem;
    using Microsoft::WRL::ComPtr;
    int bad=0;auto ck=[&](bool ok,const char* why){if(!ok){std::printf("FAIL: flat draw capture %s\n",why);++bad;}};
    constexpr UINT W=64,H=64;
    D3D11_TEXTURE2D_DESC td{};td.Width=W;td.Height=H;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> color;ComPtr<ID3D11RenderTargetView> rtv;
    ck(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&color))&&
       SUCCEEDED(device->CreateRenderTargetView(color.Get(),nullptr,&rtv)),"render target created");
    td.Format=DXGI_FORMAT_R32_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11DepthStencilView> dsv;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    ck(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&depth))&&
       SUCCEEDED(device->CreateDepthStencilView(depth.Get(),&dd,&dsv)),"depth target created");
    if(!color||!rtv||!depth||!dsv)return bad;
    const char* vsText="float4 main(uint id:SV_VertexID):SV_Position { float2 p[3]={float2(-1,-1),float2(0,-1),float2(-1,1)}; return float4(p[id],0.5,1); }";
    const char* psText="float4 main():SV_Target { return float4(0,1,0,1); }";
    ComPtr<ID3DBlob> vsCode,psCode,error;ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    ck(SUCCEEDED(D3DCompile(vsText,std::strlen(vsText),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vsCode,&error))&&
       SUCCEEDED(D3DCompile(psText,std::strlen(psText),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&psCode,&error)),"fixture shaders compiled");
    if(!vsCode||!psCode)return bad;
    ck(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs))&&
       SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps)),"fixture shaders created");
    uint8_t initial[64]{},later[64]{};for(unsigned i=0;i<64;++i){initial[i]=uint8_t(i+7);later[i]=uint8_t(i+41);}
    D3D11_BUFFER_DESC cbDesc{};cbDesc.ByteWidth=64;cbDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cbData{};cbData.pSysMem=initial;ComPtr<ID3D11Buffer> cb;
    ck(SUCCEEDED(device->CreateBuffer(&cbDesc,&cbData,&cb)),"constant buffer created");
    if(!vs||!ps||!cb)return bad;
    const auto fixtureRoot=fs::path(edvr::Config::get().logDir());
    const auto pointer=fixtureRoot/L"flat_draw_current_fixture.txt";std::error_code ec;fs::remove(pointer,ec);
    edvr::FlatDrawCapture cap;
    cap.present(context,99);ck(!cap.active()&&cap.directory().empty(),"inactive present is inert");
    cap.arm(100);const auto dir=fs::path(cap.directory());
    context->ClearState();
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> rasterState;ck(SUCCEEDED(device->CreateRasterizerState(&raster,&rasterState)),"raster state");
    context->RSSetState(rasterState.Get());
    D3D11_DEPTH_STENCIL_DESC depthStateDesc{};depthStateDesc.DepthEnable=TRUE;
    depthStateDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depthStateDesc.DepthFunc=D3D11_COMPARISON_LESS;
    ComPtr<ID3D11DepthStencilState> depthState;ck(SUCCEEDED(device->CreateDepthStencilState(&depthStateDesc,&depthState)),"depth state");
    context->OMSetDepthStencilState(depthState.Get(),0);context->OMSetBlendState(nullptr,nullptr,0xffffffffu);
    D3D11_VIEWPORT vp{};vp.Width=float(W);vp.Height=float(H);vp.MaxDepth=1;
    context->RSSetViewports(1,&vp);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
    ID3D11Buffer* cbs[3]{cb.Get(),cb.Get(),cb.Get()};context->VSSetConstantBuffers(0,3,cbs);
    ID3D11RenderTargetView* target=rtv.Get();context->OMSetRenderTargets(1,&target,dsv.Get());
    const float red[4]={1,0,0,1};
    for(uint64_t frame=101;frame<=102;++frame){
        context->ClearRenderTargetView(rtv.Get(),red);context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
        if(frame==102)context->UpdateSubresource(cb.Get(),0,nullptr,later,0,0);
        cap.begin(frame,depth.Get(),W,H);
        // Frame 102 deliberately feeds a distinct signed/native argument set
        // to exercise the recorder's ABI fields; the raster call stays the
        // same so the two pixel footprints have an exact reference.
        ck(cap.before(context,1,frame==101?'D':'X',3,frame==101?0:7,frame==101?0:-4,
            frame==101?0:9,0x1234,0x5678,vs.Get(),ps.Get()),"matching scene draw captured");
        context->Draw(3,0);cap.after(context);
        cap.qualify(frame,depth.Get(),color.Get(),W,H);cap.present(context,frame);
        ComPtr<ID3D11RenderTargetView> bound;context->OMGetRenderTargets(1,&bound,nullptr);
        ck(bound.Get()==rtv.Get(),"capture preserves bound render target");
    }
    context->UpdateSubresource(cb.Get(),0,nullptr,initial,0,0);
    context->Flush();
    for(unsigned attempt=0;attempt<120&&(!fs::exists(dir/L"frame_101.json")||!fs::exists(dir/L"frame_102.json"));++attempt){cap.present(context,103+attempt);Sleep(1);}
    ck(fs::exists(dir/L"frame_101.json")&&fs::exists(dir/L"frame_102.json"),"two consecutive manifests written");
    for(uint64_t frame=101;frame<=102;++frame){
        std::ifstream js(dir/(std::string("frame_")+std::to_string(frame)+".json"),std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(js)),std::istreambuf_iterator<char>());
        ck(body.find("\"status\":\"complete\"")!=std::string::npos&&body.find("\"draws_recorded\":1")!=std::string::npos,
           "qualified frame reports one complete draw");
        if(frame==102)ck(body.find("\"kind\":\"X\"")!=std::string::npos&&
            body.find("\"start\":7,\"base\":-4,\"start_instance\":9")!=std::string::npos,
            "signed draw arguments retained without native-command inference");
        std::ifstream cbFile(dir/(std::string("frame_")+std::to_string(frame)+"_cb.bin"),std::ios::binary);
        const std::string cbBytes((std::istreambuf_iterator<char>(cbFile)),std::istreambuf_iterator<char>());
        const auto* expected=frame==101?initial:later;
        ck(cbBytes.size()==192&&std::memcmp(cbBytes.data(),expected,64)==0,"CB snapshot froze draw-time raw bytes");
        std::ifstream pixels(dir/(std::string("frame_")+std::to_string(frame)+"_pixels.bin"),std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(pixels)),std::istreambuf_iterator<char>());
        ck(bytes.size()==8*2*16*16*4,"eight points have before and after native 16x16 windows");
        ck(bytes.find(std::string("\x00\xff\x00\xff",4))!=std::string::npos,"actual draw green appears in capture");
        if(bytes.size()==8*2*16*16*4){
            const std::string redPixel("\xff\x00\x00\xff",4),greenPixel("\x00\xff\x00\xff",4);
            ck(bytes.substr(0,4)==redPixel&&bytes.substr(8*16*16*4,4)==greenPixel,
                "covered window changes only after the actual draw");
            ck(bytes.substr(9*16*16*4,4)==redPixel,"noncovering point remains unchanged");
        }
    }
    cap.arm(200);cap.cancel("test-cancel");ck(!cap.active(),"rearm/cancel clears capture");
    edvr::FlatDrawCapture idle;idle.arm(300);idle.present(context,1201);ck(!idle.active(),"900-frame expiry stops idle arm");
    // Exercise the 512 draw cap without issuing 513 real raster draws. The
    // unsupported native format gives each candidate an explicit refusal,
    // so the cap path stays cheap and cannot accidentally look complete.
    D3D11_TEXTURE2D_DESC unsupportedDesc{};unsupportedDesc.Width=W;unsupportedDesc.Height=H;
    unsupportedDesc.MipLevels=unsupportedDesc.ArraySize=unsupportedDesc.SampleDesc.Count=1;
    unsupportedDesc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;unsupportedDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> unsupported;ComPtr<ID3D11RenderTargetView> unsupportedView;
    ck(SUCCEEDED(device->CreateTexture2D(&unsupportedDesc,nullptr,&unsupported))&&
        SUCCEEDED(device->CreateRenderTargetView(unsupported.Get(),nullptr,&unsupportedView)),"cap target created");
    if(unsupportedView){
        ID3D11RenderTargetView* un=unsupportedView.Get();context->OMSetRenderTargets(1,&un,dsv.Get());
        ID3D11Buffer* none[3]{};context->VSSetConstantBuffers(0,3,none);
        edvr::FlatDrawCapture capped;capped.arm(400);const auto capDir=fs::path(capped.directory());capped.begin(401,depth.Get(),W,H);
        unsigned accepted=0;for(unsigned i=0;i<513;++i){if(capped.before(context,1,'D',3,0,0,0,1,2,vs.Get(),ps.Get())){
            ++accepted;capped.after(context);}}
        ck(accepted==512,"candidate cap rejects draw 513");
        capped.qualify(401,depth.Get(),unsupported.Get(),W,H);capped.present(context,401);
        std::ifstream capJson(capDir/L"frame_401.json",std::ios::binary);
        const std::string capBody((std::istreambuf_iterator<char>(capJson)),std::istreambuf_iterator<char>());
        ck(capBody.find("\"status\":\"partial\"")!=std::string::npos&&
            capBody.find("\"draws_seen\":513")!=std::string::npos&&
            capBody.find("\"draws_recorded\":512")!=std::string::npos&&
            capBody.find("\"overflow\":1")!=std::string::npos,"cap reports partial and exact omitted draw");
        capped.cancel("test-end");
    }
    // Use the game's native depth/slot formats in a second fixture. Each frame
    // records an opaque underlay followed by a nearer, depth-write-off decal.
    // The decal changes color and MRT6 while the depth mirror must remain at
    // the underlay's value. The original frame_101/102 fixture stays intact.
    D3D11_TEXTURE2D_DESC nativeDepthDesc{};nativeDepthDesc.Width=W;nativeDepthDesc.Height=H;
    nativeDepthDesc.MipLevels=nativeDepthDesc.ArraySize=nativeDepthDesc.SampleDesc.Count=1;
    nativeDepthDesc.Format=DXGI_FORMAT_R32G8X24_TYPELESS;
    nativeDepthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC nativeDsvDesc{};nativeDsvDesc.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    nativeDsvDesc.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11Texture2D> nativeDepth;ComPtr<ID3D11DepthStencilView> nativeDsv;
    ck(SUCCEEDED(device->CreateTexture2D(&nativeDepthDesc,nullptr,&nativeDepth))&&
       SUCCEEDED(device->CreateDepthStencilView(nativeDepth.Get(),&nativeDsvDesc,&nativeDsv)),
       "native depth target created");
    D3D11_TEXTURE2D_DESC slotDesc{};slotDesc.Width=W;slotDesc.Height=H;
    slotDesc.MipLevels=slotDesc.ArraySize=slotDesc.SampleDesc.Count=1;
    slotDesc.Format=DXGI_FORMAT_R32G32_FLOAT;slotDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> slot;ComPtr<ID3D11RenderTargetView> slotView;
    ck(SUCCEEDED(device->CreateTexture2D(&slotDesc,nullptr,&slot))&&
       SUCCEEDED(device->CreateRenderTargetView(slot.Get(),nullptr,&slotView)),"MRT6 slot target created");
    std::array<uint8_t,336*4> poolBytes{};
    for(size_t i=0;i<poolBytes.size();++i)poolBytes[i]=uint8_t((i*13+17)&255);
    D3D11_BUFFER_DESC poolDesc{};poolDesc.ByteWidth=UINT(poolBytes.size());
    poolDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;poolDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    poolDesc.StructureByteStride=336;
    D3D11_SUBRESOURCE_DATA poolData{};poolData.pSysMem=poolBytes.data();
    ComPtr<ID3D11Buffer> pool;ComPtr<ID3D11ShaderResourceView> poolView;
    D3D11_SHADER_RESOURCE_VIEW_DESC poolViewDesc{};poolViewDesc.Format=DXGI_FORMAT_UNKNOWN;
    poolViewDesc.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;poolViewDesc.Buffer.FirstElement=0;
    poolViewDesc.Buffer.NumElements=4;
    ck(SUCCEEDED(device->CreateBuffer(&poolDesc,&poolData,&pool))&&
       SUCCEEDED(device->CreateShaderResourceView(pool.Get(),&poolViewDesc,&poolView)),
       "336-byte structured t33 pool created");
    const char* underlayVsText="float4 main(uint id:SV_VertexID):SV_Position { float2 p[3]={float2(-1,-1),float2(0,-1),float2(-1,1)}; return float4(p[id],0.5,1); }";
    const char* decalVsText="float4 main(uint id:SV_VertexID):SV_Position { float2 p[3]={float2(-1,-1),float2(0,-1),float2(-1,1)}; return float4(p[id],0.25,1); }";
    const char* underlayPsText="struct Out { float4 color:SV_Target0; float2 slot:SV_Target6; }; Out main(){ Out o; o.color=float4(0,1,0,1); o.slot=float2(7,0.5); return o; }";
    const char* decalPsText="struct Out { float4 color:SV_Target0; float2 slot:SV_Target6; }; Out main(){ Out o; o.color=float4(0,0,1,1); o.slot=float2(7,0.25); return o; }";
    ComPtr<ID3DBlob> underlayVsCode,decalVsCode,underlayPsCode,decalPsCode;
    ComPtr<ID3D11VertexShader> underlayVs,decalVs;
    ComPtr<ID3D11PixelShader> underlayPs,decalPs;
    ck(SUCCEEDED(D3DCompile(underlayVsText,std::strlen(underlayVsText),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&underlayVsCode,&error))&&
       SUCCEEDED(D3DCompile(decalVsText,std::strlen(decalVsText),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&decalVsCode,&error))&&
       SUCCEEDED(D3DCompile(underlayPsText,std::strlen(underlayPsText),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&underlayPsCode,&error))&&
       SUCCEEDED(D3DCompile(decalPsText,std::strlen(decalPsText),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&decalPsCode,&error)),
       "native motion shaders compiled");
    if(underlayVsCode&&decalVsCode&&underlayPsCode&&decalPsCode){
        ck(SUCCEEDED(device->CreateVertexShader(underlayVsCode->GetBufferPointer(),underlayVsCode->GetBufferSize(),nullptr,&underlayVs))&&
           SUCCEEDED(device->CreateVertexShader(decalVsCode->GetBufferPointer(),decalVsCode->GetBufferSize(),nullptr,&decalVs))&&
           SUCCEEDED(device->CreatePixelShader(underlayPsCode->GetBufferPointer(),underlayPsCode->GetBufferSize(),nullptr,&underlayPs))&&
           SUCCEEDED(device->CreatePixelShader(decalPsCode->GetBufferPointer(),decalPsCode->GetBufferSize(),nullptr,&decalPs)),
           "native motion shaders created");
    }
    if(nativeDepth&&nativeDsv&&slot&&slotView&&pool&&poolView&&underlayVs&&decalVs&&underlayPs&&decalPs){
        D3D11_DEPTH_STENCIL_DESC decalDepthDesc=depthStateDesc;
        decalDepthDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        ComPtr<ID3D11DepthStencilState> decalDepth;
        ck(SUCCEEDED(device->CreateDepthStencilState(&decalDepthDesc,&decalDepth)),"decal depth-write-off state created");
        if(decalDepth){
            edvr::FlatDrawCapture motion;motion.arm(500);const auto motionDir=fs::path(motion.directory());
            context->ClearState();context->RSSetState(rasterState.Get());context->RSSetViewports(1,&vp);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->OMSetBlendState(nullptr,nullptr,0xffffffffu);
            context->VSSetConstantBuffers(0,3,cbs);
            ID3D11ShaderResourceView* t33=poolView.Get();context->VSSetShaderResources(33,1,&t33);
            ID3D11RenderTargetView* scene[7]{};scene[0]=rtv.Get();scene[6]=slotView.Get();
            const float sentinel[4]={-1,0,0,0};
            for(uint64_t frame=501;frame<=502;++frame){
                context->ClearRenderTargetView(rtv.Get(),red);
                context->ClearRenderTargetView(slotView.Get(),sentinel);
                context->ClearDepthStencilView(nativeDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
                motion.begin(frame,nativeDepth.Get(),W,H);
                for(unsigned draw=0;draw<2;++draw){
                    const bool decal=draw!=0;
                    context->OMSetRenderTargets(1,&target,nativeDsv.Get());
                    context->OMSetDepthStencilState(decal?decalDepth.Get():depthState.Get(),0);
                    context->VSSetShader(decal?decalVs.Get():underlayVs.Get(),nullptr,0);
                    context->PSSetShader(decal?decalPs.Get():underlayPs.Get(),nullptr,0);
                    ck(motion.before(context,1,'D',3,0,0,0,
                        decal?0xBBE58E40FE88EC80ull:0x66DE2CADB1F4AE6Bull,
                        decal?0xDB3E8D20CF53FBC0ull:0x864F1F949851B8DEull,
                        decal?decalVs.Get():underlayVs.Get(),decal?decalPs.Get():underlayPs.Get()),
                        "native motion draw recorded before substitution");
                    context->OMSetRenderTargets(7,scene,nativeDsv.Get());
                    motion.motionBefore(context,true,true);
                    context->Draw(3,0);motion.after(context);
                    ID3D11RenderTargetView* bound[7]{};ComPtr<ID3D11DepthStencilView> boundDepth;
                    context->OMGetRenderTargets(7,bound,&boundDepth);
                    ck(bound[0]==rtv.Get()&&bound[6]==slotView.Get()&&boundDepth.Get()==nativeDsv.Get(),
                       "motion capture preserves color, MRT6, and depth bindings");
                    for(auto* view:bound)if(view)view->Release();
                }
                motion.qualify(frame,nativeDepth.Get(),color.Get(),W,H);motion.present(context,frame);
            }
            context->Flush();
            for(unsigned attempt=0;attempt<120&&(!fs::exists(motionDir/L"frame_501.json")||!fs::exists(motionDir/L"frame_502.json"));++attempt){
                motion.present(context,503+attempt);Sleep(1);
            }
            ck(fs::exists(motionDir/L"frame_501.json")&&fs::exists(motionDir/L"frame_502.json"),
               "two native motion manifests written");
            for(uint64_t frame=501;frame<=502;++frame){
                const auto stem=std::string("frame_")+std::to_string(frame);
                std::ifstream js(motionDir/(stem+".json"),std::ios::binary);
                const std::string body((std::istreambuf_iterator<char>(js)),std::istreambuf_iterator<char>());
                ck(body.find("\"status\":\"complete\"")!=std::string::npos&&
                   body.find("\"draws_recorded\":2")!=std::string::npos&&
                   body.find("\"motion_draws\":2")!=std::string::npos&&
                   body.find("\"motion_complete\":true")!=std::string::npos&&
                   body.find("\"depth\":{\"resource\"")!=std::string::npos&&
                   body.find("\"format\":19,\"view_format\":20")!=std::string::npos,
                   "native depth and motion snapshots completed without refusals");
                std::ifstream pools(motionDir/(stem+"_pool.bin"),std::ios::binary);
                const std::string poolRaw((std::istreambuf_iterator<char>(pools)),std::istreambuf_iterator<char>());
                ck(poolRaw.size()==2*poolBytes.size()&&
                   std::memcmp(poolRaw.data(),poolBytes.data(),poolBytes.size())==0&&
                   std::memcmp(poolRaw.data()+poolBytes.size(),poolBytes.data(),poolBytes.size())==0,
                   "both draw-time t33 snapshots preserve all 336-byte records");
            }
            const auto readSurface=[&](ID3D11Texture2D* source,UINT pixelBytes,std::vector<uint8_t>& out){
                D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
                desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;
                desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> staging;
                if(FAILED(device->CreateTexture2D(&desc,nullptr,&staging)))return false;
                context->CopyResource(staging.Get(),source);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return false;
                out.resize(size_t(desc.Width)*desc.Height*pixelBytes);
                for(UINT y=0;y<desc.Height;++y)
                    std::memcpy(out.data()+size_t(y)*desc.Width*pixelBytes,
                                static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,
                                size_t(desc.Width)*pixelBytes);
                context->Unmap(staging.Get(),0);return true;
            };
            std::vector<uint8_t> capturedColor,capturedSlot,capturedDepth;
            ck(readSurface(color.Get(),4,capturedColor)&&readSurface(slot.Get(),8,capturedSlot)&&
               readSurface(nativeDepth.Get(),8,capturedDepth),"captured native outputs read back");
            context->ClearRenderTargetView(rtv.Get(),red);
            context->ClearRenderTargetView(slotView.Get(),sentinel);
            context->ClearDepthStencilView(nativeDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
            context->OMSetRenderTargets(7,scene,nativeDsv.Get());
            for(unsigned draw=0;draw<2;++draw){
                const bool decal=draw!=0;
                context->OMSetDepthStencilState(decal?decalDepth.Get():depthState.Get(),0);
                context->VSSetShader(decal?decalVs.Get():underlayVs.Get(),nullptr,0);
                context->PSSetShader(decal?decalPs.Get():underlayPs.Get(),nullptr,0);
                context->Draw(3,0);
            }
            std::vector<uint8_t> referenceColor,referenceSlot,referenceDepth;
            ck(readSurface(color.Get(),4,referenceColor)&&readSurface(slot.Get(),8,referenceSlot)&&
               readSurface(nativeDepth.Get(),8,referenceDepth),"no-capture reference outputs read back");
            ck(!capturedColor.empty()&&capturedColor==referenceColor&&capturedSlot==referenceSlot&&
               capturedDepth==referenceDepth,"capture leaves color, MRT6, and native depth bit-identical to reference");
            if(!bad){std::ofstream file(fixtureRoot/L"flat_draw_motion_current_fixture.txt",std::ios::binary);
                file<<fs::relative(motionDir,fixtureRoot).generic_u8string()<<"\n";
                ck(file.good(),"native motion fixture pointer written");
                std::printf("flat draw motion fixture: %ls\n",motionDir.c_str());}
            motion.cancel("test-end");
        }
    }
    if(!bad){std::ofstream file(pointer,std::ios::binary);file<<fs::relative(dir,fixtureRoot).generic_u8string()<<"\n";
        ck(file.good(),"fixture pointer written");std::printf("flat draw fixture: %ls\n",dir.c_str());}
    return bad;
}
