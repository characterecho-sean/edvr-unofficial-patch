#pragma once
#include <cstddef>
#include "../../src/d3d11/dxbc_flat_overlay.h"

inline int flatLoopOutputGpuTests(ID3D11Device* device,ID3D11DeviceContext* context) {
    using Microsoft::WRL::ComPtr;
    static_assert(offsetof(edvr::FlatTraceEvent,key)+offsetof(edvr::FlatContractObservation,vs)==120,"packet trace VS offset");
    static_assert(offsetof(edvr::FlatContractObservation,ps)==128,"packet trace PS offset");
    static_assert(offsetof(edvr::FlatContractObservation,sequence)==196,"packet trace q offset");
    static_assert(offsetof(edvr::FlatTraceEvent,kind)==464&&sizeof(edvr::FlatTraceEvent)==504,"packet trace event layout");
    int failures=0;
    auto check=[&](bool ok,const char* why){if(!ok){std::printf("FAIL: loop-output GPU %s\n",why);++failures;}};
    // The production observation intentionally starts with q=0; only the
    // separate drawSequence gets the reducer's q. Pin runtime wiring as well
    // as the serializer's offsets and original observation isolation.
    FILE* runtime=nullptr;fopen_s(&runtime,"src/d3d11/flat_runtime.cpp","rb");
    char line[4096]{};unsigned traceWiring=0;
    if(runtime){while(std::fgets(line,sizeof(line),runtime)){
        if(std::strstr(line,"flatTraceRecord(s.traceRing, d, foreignWork.load(std::memory_order_acquire), hdrSrvKnown ? hdrSrv : nullptr, s.prefix.sequence);")){traceWiring=1;break;}}
        std::fclose(runtime);}
    check(traceWiring==1,"normal runtime trace stamps reducer q separately from the original observation");
    {
        auto ring=std::make_unique<edvr::FlatTraceRing>();
        edvr::FlatRuntimePrefix prefix{};
        edvr::FlatRuntimeDraw observation{};observation.key.vs=0x7F9B650EC1A1E570ull;observation.key.ps=0xCBB1A87D6023B2A8ull;
        std::vector<std::pair<uint64_t,uint32_t>> expected;
        for(uint64_t frame:{49882ull,49883ull}) {
            prefix.frame=frame;prefix.sequence=0;
            edvr::flatTraceBeginFrame(*ring,frame,nullptr,16,16,28);
            if(frame==49882) {
                edvr::FlatRuntimeDraw preceding{};preceding.key.vs=1;preceding.key.ps=2;
                edvr::flatRuntimeObserve(prefix,preceding);
                edvr::flatTraceRecord(*ring,preceding,false,nullptr,prefix.sequence);
                // Camera captures consume the SAME reducer sequence space.
                ++prefix.sequence;
                edvr::flatTraceMark(*ring,edvr::kFlatTraceEventCameraCapture,nullptr);
            }
            const auto packetQ=prefix.sequence+1;
            expected.emplace_back(frame,packetQ);
            edvr::flatRuntimeObserve(prefix,observation);
            edvr::flatTraceRecord(*ring,observation,false,nullptr,prefix.sequence);
            edvr::flatTraceSeal(*ring,false,0);
        }
        edvr::flatTraceBeginFrame(*ring,49884,nullptr,16,16,28);
        std::vector<unsigned char> serialized;
        edvr::flatTraceDump(*ring,[&](const void* bytes,size_t count){const auto* first=static_cast<const unsigned char*>(bytes);serialized.insert(serialized.end(),first,first+count);return static_cast<uint32_t>(count);});
        uint64_t frame=0;unsigned markers=0;
        std::vector<std::pair<uint64_t,uint32_t>> joined;
        const bool parsed=edvr::flatTraceParse(serialized.data(),serialized.size(),
            [&](const edvr::FlatTraceFrameHeader& header){frame=header.frame;},
            [&](const edvr::FlatTraceEvent& event){
                markers+=event.kind==edvr::kFlatTraceEventCameraCapture;
                if(event.kind==edvr::kFlatTraceEventDraw&&event.key.vs==observation.key.vs&&event.key.ps==observation.key.ps)
                    joined.emplace_back(frame,event.drawSequence);
            });
        check(parsed&&markers==1&&joined==expected&&expected[0].second==3&&expected[1].second==1,
              "real reducer and ring serialization join packet frame/q/shaders across interleaved capture and frame reset");
        check(observation.key.sequence==0&&observation.key.vs==0x7F9B650EC1A1E570ull&&observation.key.ps==0xCBB1A87D6023B2A8ull,
              "reducing and serializing correlation leaves the original observation unchanged");
    }
    auto compile=[&](const char* source,const char* profile){ComPtr<ID3DBlob> code,error;
        const auto hr=D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"main",profile,0,0,&code,&error);
        if(FAILED(hr)&&error)std::printf("loop-output shader: %s\n",static_cast<const char*>(error->GetBufferPointer()));
        check(SUCCEEDED(hr),"fixture compiles");return code;};
    std::vector<BYTE> captured(66988);FILE* file=nullptr;
    const auto opened=fopen_s(&file,"tools/flat_mono_resolve_test/fixtures/ps_CBB1A87D6023B2A8.dxbc","rb");
    const bool read=opened==0&&file&&std::fread(captured.data(),1,captured.size(),file)==captured.size()&&std::fgetc(file)==EOF;
    if(file)std::fclose(file);check(read,"captured plasma creation bytes read exactly");
    uint64_t hash=1469598103934665603ull;for(auto byte:captured){hash^=byte;hash*=1099511628211ull;}
    check(read&&hash==0xCBB1A87D6023B2A8ull,"captured plasma fixture has exact EDVR hash");
    std::vector<BYTE> normal,replay;std::string why;
    check(read&&edvr::flatOverlayPatchPs(captured.data(),captured.size(),normal,why),"captured nested-loop plasma qualifies generically");
    if(!normal.empty())check(edvr::flatOverlayCoverageFromPatchedPs(normal.data(),normal.size(),replay,why),"captured plasma replay derives without another unsupported opcode");
    ComPtr<ID3D11PixelShader> actual,actualNormal,actualReplay;
    check(read&&SUCCEEDED(device->CreatePixelShader(captured.data(),captured.size(),nullptr,&actual))&&
          !normal.empty()&&SUCCEEDED(device->CreatePixelShader(normal.data(),normal.size(),nullptr,&actualNormal))&&
          !replay.empty()&&SUCCEEDED(device->CreatePixelShader(replay.data(),replay.size(),nullptr,&actualReplay)),
          "captured original, normal coverage and replay PS all create on WARP");

    auto vertex=compile("float4 main(uint id:SV_VertexID):SV_Position{float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[id],.5,1);}","vs_5_0");
    auto pixel=compile("cbuffer Limits:register(b0){uint4 limit;}float4 main(float4 p:SV_Position):SV_Target{"
        "uint x=(uint)p.x;float sum=0;[loop]for(uint i=0;i<limit.x;++i){if(i==1)continue;"
        "[loop]for(uint j=0;j<limit.y;++j){if(j==1)continue;if(j>2)break;sum+=(float)(i+j+1);}"
        "if(i>x%3+2)break;}if(x%4==0)discard;return float4(sum*.01,.25,.5,.75);}","ps_5_0");
    if(!vertex||!pixel)return failures;
    auto wordsFor=[&](const void* bytes,size_t size){auto chunks=edvr::dxbc_container::parseContainer(bytes,size,0x50u);
        std::vector<uint32_t> result;for(auto& chunk:chunks)if(chunk.tag==0x58454853u||chunk.tag==0x52444853u){result.resize(chunk.bytes.size()/4);std::memcpy(result.data(),chunk.bytes.data(),chunk.bytes.size());}return result;};
    const auto originalWords=wordsFor(pixel->GetBufferPointer(),pixel->GetBufferSize());
    auto opcodeCount=[&](const std::vector<uint32_t>& words,uint32_t op){unsigned n=0;for(size_t at=2;at<words.size();at+=edvr::dxbc_container::instructionLength(words,at))n+=(words[at]&0x7ffu)==op;return n;};
    check(opcodeCount(originalWords,48)>=2&&opcodeCount(originalWords,7)>0&&opcodeCount(originalWords,3)>0&&opcodeCount(originalWords,13)>0,
          "dynamic fixture actually contains nested loops, continue, conditional break and discard");
    check(edvr::flatOverlayPatchPs(pixel->GetBufferPointer(),pixel->GetBufferSize(),normal,why),"dynamic nested-loop discard shader qualifies");
    // Removing only the private declaration and terminal mark must recover
    // every original instruction bit for bit, including loop branch operands.
    if(!normal.empty()){
        auto restored=wordsFor(normal.data(),normal.size());std::vector<uint32_t> stripped{restored[0],0};
        for(size_t at=2;at<restored.size();at+=edvr::dxbc_container::instructionLength(restored,at)){
            const auto len=edvr::dxbc_container::instructionLength(restored,at);const auto op=restored[at]&0x7ffu;
            if((op==101&&len==3&&restored[at+2]==7)||(op==54&&len==5&&restored[at+2]==7&&at+len+1==restored.size()))continue;
            stripped.insert(stripped.end(),restored.begin()+at,restored.begin()+at+len);}
        stripped[1]=static_cast<uint32_t>(stripped.size());check(stripped==originalWords,"private tail injection preserves all original loop instructions byte for byte");
    }
    auto refuse=[&](const std::vector<uint32_t>& changed,const char* message){auto chunks=edvr::dxbc_container::parseContainer(pixel->GetBufferPointer(),pixel->GetBufferSize(),0x50u);
        for(auto& chunk:chunks)if(chunk.tag==0x58454853u||chunk.tag==0x52444853u){chunk.bytes.resize(changed.size()*4);std::memcpy(chunk.bytes.data(),changed.data(),chunk.bytes.size());}
        const auto bytes=edvr::dxbc_container::makeContainer(chunks);std::vector<BYTE> out;
        check(!edvr::flatOverlayPatchPs(bytes.data(),bytes.size(),out,why)&&out.empty(),message);};
    auto mutate=[&](uint32_t from,uint32_t to){auto words=originalWords;for(size_t at=2;at<words.size();at+=edvr::dxbc_container::instructionLength(words,at))if((words[at]&0x7ffu)==from){words[at]=(words[at]&~0x7ffu)|to;break;}return words;};
    refuse(mutate(48,21),"unmatched ENDIF in place of LOOP is refused");
    refuse(mutate(22,21),"crossed LOOP/ENDIF is refused");
    refuse(mutate(21,22),"crossed IF/ENDLOOP is refused");
    refuse(mutate(22,7),"unclosed LOOP is refused");
    refuse(mutate(48,2),"BREAK outside LOOP is refused");
    refuse(mutate(48,7),"CONTINUE outside LOOP is refused");
    refuse(mutate(48,62),"nonterminal RET is refused");
    refuse(mutate(62,63),"conditional RETC remains refused");
    auto malformed=originalWords;for(size_t at=2;at<malformed.size();at+=edvr::dxbc_container::instructionLength(malformed,at))if((malformed[at]&0x7ffu)==48){malformed[at]=(malformed[at]&~0x7f000000u)|0x02000000u;break;}
    refuse(malformed,"malformed LOOP instruction length is refused");
    if(normal.empty())return failures;
    check(edvr::flatOverlayCoverageFromPatchedPs(normal.data(),normal.size(),replay,why),"dynamic loop replay variant derives");
    ComPtr<ID3D11PixelShader> ps,coverage,replayPs;ComPtr<ID3D11VertexShader> vs;
    check(SUCCEEDED(device->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&ps))&&
          SUCCEEDED(device->CreatePixelShader(normal.data(),normal.size(),nullptr,&coverage))&&
          !replay.empty()&&SUCCEEDED(device->CreatePixelShader(replay.data(),replay.size(),nullptr,&replayPs))&&
          SUCCEEDED(device->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&vs)),"dynamic shaders create");
    constexpr UINT w=16,h=16;
    D3D11_TEXTURE2D_DESC cd{};cd.Width=w;cd.Height=h;cd.ArraySize=cd.MipLevels=cd.SampleDesc.Count=1;cd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;cd.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colors[2],depths[2],mask,replayMask,privateDepth;ComPtr<ID3D11RenderTargetView> rtvs[2],maskRtv,replayRtv;ComPtr<ID3D11DepthStencilView> dsvs[2],privateDsv;
    for(UINT i=0;i<2;++i){check(SUCCEEDED(device->CreateTexture2D(&cd,nullptr,&colors[i]))&&SUCCEEDED(device->CreateRenderTargetView(colors[i].Get(),nullptr,&rtvs[i])),"dynamic color target creates");
        auto dd=cd;dd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        check(SUCCEEDED(device->CreateTexture2D(&dd,nullptr,&depths[i]))&&SUCCEEDED(device->CreateDepthStencilView(depths[i].Get(),nullptr,&dsvs[i])),"dynamic depth/stencil target creates");}
    auto md=cd;md.Format=DXGI_FORMAT_R32_FLOAT;
    check(SUCCEEDED(device->CreateTexture2D(&md,nullptr,&mask))&&SUCCEEDED(device->CreateRenderTargetView(mask.Get(),nullptr,&maskRtv)),"private coverage target creates");
    check(SUCCEEDED(device->CreateTexture2D(&md,nullptr,&replayMask))&&SUCCEEDED(device->CreateRenderTargetView(replayMask.Get(),nullptr,&replayRtv)),"RT0 replay coverage target creates");
    auto privateDesc=cd;privateDesc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;privateDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    check(SUCCEEDED(device->CreateTexture2D(&privateDesc,nullptr,&privateDepth))&&SUCCEEDED(device->CreateDepthStencilView(privateDepth.Get(),nullptr,&privateDsv)),"pre-original private replay DSV creates");
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;UINT limits[]={6,5,0,0};D3D11_SUBRESOURCE_DATA init{limits,0,0};ComPtr<ID3D11Buffer> cb;
    check(SUCCEEDED(device->CreateBuffer(&bd,&init,&cb)),"dynamic loop limits buffer creates");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;device->CreateRasterizerState(&rd,&raster);
    D3D11_DEPTH_STENCIL_DESC zd{};zd.DepthEnable=TRUE;zd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;zd.DepthFunc=D3D11_COMPARISON_LESS;zd.StencilEnable=TRUE;zd.StencilReadMask=zd.StencilWriteMask=0xff;
    zd.FrontFace.StencilFunc=zd.BackFace.StencilFunc=D3D11_COMPARISON_EQUAL;zd.FrontFace.StencilFailOp=zd.BackFace.StencilFailOp=D3D11_STENCIL_OP_KEEP;
    zd.FrontFace.StencilDepthFailOp=zd.BackFace.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;zd.FrontFace.StencilPassOp=zd.BackFace.StencilPassOp=D3D11_STENCIL_OP_INCR_SAT;
    ComPtr<ID3D11DepthStencilState> tested;device->CreateDepthStencilState(&zd,&tested);
    D3D11_BLEND_DESC blendDesc{};blendDesc.IndependentBlendEnable=TRUE;blendDesc.RenderTarget[0].RenderTargetWriteMask=0xf;blendDesc.RenderTarget[7].RenderTargetWriteMask=1;
    ComPtr<ID3D11BlendState> blend;device->CreateBlendState(&blendDesc,&blend);
    if(!ps||!coverage||!replayPs||!vs||!rtvs[0]||!rtvs[1]||!dsvs[0]||!dsvs[1]||!maskRtv||!replayRtv||!privateDsv||!cb||!raster||!tested||!blend)return failures+1;
    auto readTexture=[&](ID3D11Texture2D* source){D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> staging;std::vector<BYTE> bytes;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&staging)))return bytes;context->CopyResource(staging.Get(),source);D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return bytes;
        bytes.resize(w*h*4);for(UINT y=0;y<h;++y)std::memcpy(bytes.data()+y*w*4,static_cast<BYTE*>(mapped.pData)+y*mapped.RowPitch,w*4);context->Unmap(staging.Get(),0);return bytes;};
    const float zero[4]={};for(UINT i=0;i<2;++i){context->ClearRenderTargetView(rtvs[i].Get(),zero);context->ClearDepthStencilView(dsvs[i].Get(),3,.8f,3);}
    for(UINT pass=0;pass<3;++pass){context->ClearState();context->ClearRenderTargetView(maskRtv.Get(),zero);context->ClearRenderTargetView(replayRtv.Get(),zero);
        context->CopyResource(privateDepth.Get(),depths[0].Get());
        const auto beforeColor=readTexture(colors[0].Get()),beforeDepth=readTexture(depths[0].Get());
        context->OMSetRenderTargets(1,replayRtv.GetAddressOf(),privateDsv.Get());context->OMSetBlendState(blend.Get(),nullptr,~0u);
        context->OMSetDepthStencilState(tested.Get(),pass==0?2:pass==1?3:4);context->RSSetState(raster.Get());D3D11_VIEWPORT replayViewport{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&replayViewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(replayPs.Get(),nullptr,0);context->PSSetConstantBuffers(0,1,cb.GetAddressOf());context->Draw(3,0);
        check(beforeColor==readTexture(colors[0].Get())&&beforeDepth==readTexture(depths[0].Get()),"RT0 replay with private pre-original DSV leaves game color/depth/stencil untouched");
        for(UINT i=0;i<2;++i){context->ClearState();ID3D11RenderTargetView* views[8]={};views[0]=rtvs[i].Get();if(i)views[7]=maskRtv.Get();context->OMSetRenderTargets(i?8:1,views,dsvs[i].Get());
            context->OMSetBlendState(blend.Get(),nullptr,~0u);context->OMSetDepthStencilState(tested.Get(),pass==0?2:pass==1?3:4);context->RSSetState(raster.Get());D3D11_VIEWPORT viewport{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&viewport);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(i?coverage.Get():ps.Get(),nullptr,0);context->PSSetConstantBuffers(0,1,cb.GetAddressOf());context->Draw(3,0);}
        check(readTexture(colors[0].Get())==readTexture(colors[1].Get())&&readTexture(depths[0].Get())==readTexture(depths[1].Get()),"nested loop normal coverage preserves original color, depth and stencil for stencil-fail, pass and depth-fail draws");
        const auto values=readTexture(mask.Get());bool exact=values.size()==w*h*4;
        for(UINT y=0;y<h&&exact;++y)for(UINT x=0;x<w;++x){float value=0;std::memcpy(&value,values.data()+(y*w+x)*4,4);exact&=value==((pass==1&&x%4!=0)?1.f:0.f);}
        check(exact,"private coverage exactly matches surviving loop/discard fragments after depth and stencil tests");
        check(exact&&values==readTexture(replayMask.Get()),"RT0 replay coverage matches normal MRT7 coverage exactly for nested-loop discard and depth/stencil outcomes");}
    context->ClearState();return failures;
}
