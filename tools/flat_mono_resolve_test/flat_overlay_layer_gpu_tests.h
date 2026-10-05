#pragma once
#include "../../src/d3d11/flat_overlay_layer.h"
#include "../../src/d3d11/flat_replay_query_tracker.h"

inline int flatOverlayLayerGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using Microsoft::WRL::ComPtr;
    int failures = 0;
    auto expect = [&](bool ok, const char* why) {
        if (!ok) { std::printf("FAIL: flat overlay GPU %s\n", why); ++failures; }
    };
    {
        using Tracker=edvr::FlatReplayQueryTracker;int countQuery=0,timingQuery=0,unknownCounter=0,other=0;
        Tracker tracker;
        tracker.begin(&countQuery,Tracker::Kind::Count);
        tracker.begin(&timingQuery,Tracker::Kind::Timing);
        tracker.end(&timingQuery,Tracker::Kind::Timing);
        expect(!tracker.safe(),"ending a timing query cannot clear another active count-bearing query");
        tracker.end(&countQuery,Tracker::Kind::Count);
        expect(tracker.safe(),"matching End closes the observed count-bearing bracket");
        tracker.begin(&unknownCounter,Tracker::Kind::Unknown);
        expect(!tracker.safe(),"unknown asynchronous counter blocks replay until its own End");
        tracker.end(&unknownCounter,Tracker::Kind::Unknown);
        expect(tracker.safe(),"matching unknown-counter End safely closes its bracket");
        tracker.end(&other,Tracker::Kind::Count);
        tracker.begin(&countQuery,Tracker::Kind::Count);tracker.end(&countQuery,Tracker::Kind::Count);
        expect(!tracker.safe(),"unmatched End remains uncertain despite later correctly paired queries");
        Tracker capacity;int identities[65]{};
        for(auto& identity:identities)capacity.begin(&identity,Tracker::Kind::Count);
        for(auto& identity:identities)capacity.end(&identity,Tracker::Kind::Count);
        expect(!capacity.safe(),"query-table overflow cannot be cleared by later Ends");
    }
    constexpr UINT w = 16, h = 16;
    auto compile = [&](const char* source, const char* profile) {
        ComPtr<ID3DBlob> bytecode, errors;
        const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
                                      "main", profile, 0, 0, &bytecode, &errors);
        expect(SUCCEEDED(hr), "fixture shader compiles");
        return bytecode;
    };
    auto vsBytes = compile("float4 main(uint id:SV_VertexID):SV_Position {"
        "float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};"
        "return float4(p[id],0.5,1);}", "vs_5_0");
    auto psBytes = compile("float4 main(float4 p:SV_Position):SV_Target {"
        "if(p.x<4) discard; return float4(.8,.1,.2,.5);}", "ps_5_0");
    auto variantBytes = compile("float4 main():SV_Target {return float4(.1,.8,.2,.5);}", "ps_5_0");
    auto neutralBytes = compile("float4 main():SV_Target {return float4(1,0,0,0);}", "ps_5_0");
    auto linkedBytes = compile("float4 main():SV_Target {return float4(.2,.7,.4,.5);}", "ps_5_0");
    auto occupiedBytes = compile("float4 main():SV_Target7 {return 1;}", "ps_5_0");
    auto depthBytes = compile("struct O {float4 c:SV_Target;float d:SV_Depth;};"
        "O main(){O o;o.c=1;o.d=.5;return o;}", "ps_5_0");
    auto structuredBytes = compile("StructuredBuffer<float4> source:register(t1);"
        "float4 main(float4 p:SV_Position):SV_Target {float4 c;"
        "[branch] if(p.x<8)c=source[0];else c=source[1];return c;}", "ps_5_0");
    auto rawBytes = compile("ByteAddressBuffer source:register(t1);"
        "float4 main(float4 p:SV_Position):SV_Target {uint4 v;"
        "[branch] if(p.x<8)v=source.Load4(0);else v=source.Load4(16);return asfloat(v);}", "ps_5_0");
    auto structuredWriteBytes = compile("RWStructuredBuffer<uint4> target:register(u1);"
        "float4 main():SV_Target {target[0]=uint4(1,2,3,4);return 1;}", "ps_5_0");
    auto rawWriteBytes = compile("RWByteAddressBuffer target:register(u1);"
        "float4 main():SV_Target {target.Store(0,1);return 1;}", "ps_5_0");
    auto atomicBytes = compile("RWByteAddressBuffer target:register(u1);"
        "float4 main():SV_Target {uint old;target.InterlockedAdd(0,1,old);return old;}", "ps_5_0");
    if (!vsBytes || !psBytes || !variantBytes || !neutralBytes || !linkedBytes || !occupiedBytes || !depthBytes ||
        !structuredBytes || !rawBytes || !structuredWriteBytes || !rawWriteBytes || !atomicBytes) return failures;
    std::vector<BYTE> patched;
    std::string patchWhy;
    expect(edvr::flatOverlayPatchPs(psBytes->GetBufferPointer(), psBytes->GetBufferSize(), patched, patchWhy) &&
           !patched.empty(), "pure discard shader gains a private output");
    expect(!edvr::flatOverlayPatchPs(occupiedBytes->GetBufferPointer(), occupiedBytes->GetBufferSize(),
                                     patched, patchWhy) && patched.empty(), "existing MRT7 is refused");
    expect(!edvr::flatOverlayPatchPs(depthBytes->GetBufferPointer(), depthBytes->GetBufferSize(),
                                     patched, patchWhy) && patched.empty(), "depth-writing PS is refused");
    auto hasOpcode=[](ID3DBlob* blob,uint32_t opcode) {
        const auto parsed=edvr::dxbc_container::parseContainer(blob->GetBufferPointer(),blob->GetBufferSize(),0x50u);
        for(const auto& chunk:parsed)if(chunk.tag==0x58454853u || chunk.tag==0x52444853u) {
            std::vector<uint32_t> words(chunk.bytes.size()/4);
            std::memcpy(words.data(),chunk.bytes.data(),chunk.bytes.size());
            for(size_t at=2;at<words.size();at+=edvr::dxbc_container::instructionLength(words,at))
                if((words[at]&0x7ffu)==opcode)return true;
        }
        return false;
    };
    expect(hasOpcode(structuredBytes.Get(),162) && hasOpcode(structuredBytes.Get(),167) &&
           hasOpcode(structuredBytes.Get(),31) &&
           edvr::flatOverlayPatchPs(structuredBytes->GetBufferPointer(),structuredBytes->GetBufferSize(),patched,patchWhy),
           "branched structured SRV declaration and load are patchable");
    expect(hasOpcode(rawBytes.Get(),161) && hasOpcode(rawBytes.Get(),165) &&
           edvr::flatOverlayPatchPs(rawBytes->GetBufferPointer(),rawBytes->GetBufferSize(),patched,patchWhy),
           "branched raw SRV declaration and load are patchable");
    expect(!edvr::flatOverlayPatchPs(structuredWriteBytes->GetBufferPointer(),structuredWriteBytes->GetBufferSize(),
           patched,patchWhy) && patched.empty(),"structured UAV write is refused");
    expect(!edvr::flatOverlayPatchPs(rawWriteBytes->GetBufferPointer(),rawWriteBytes->GetBufferSize(),
           patched,patchWhy) && patched.empty(),"raw UAV write is refused");
    expect(!edvr::flatOverlayPatchPs(atomicBytes->GetBufferPointer(),atomicBytes->GetBufferSize(),
           patched,patchWhy) && patched.empty(),"UAV atomic is refused");
    auto chunks=edvr::dxbc_container::parseContainer(psBytes->GetBufferPointer(),psBytes->GetBufferSize(),0x50u);
    bool conditionalReturn=false;
    for(auto& chunk:chunks)if(chunk.tag==0x58454853u || chunk.tag==0x52444853u) {
        std::vector<uint32_t> words(chunk.bytes.size()/4);
        std::memcpy(words.data(),chunk.bytes.data(),chunk.bytes.size());
        for(size_t at=2;at<words.size();at+=edvr::dxbc_container::instructionLength(words,at))
            if((words[at]&0x7ffu)==62u) {words[at]=(words[at]&~0x7ffu)|63u;conditionalReturn=true;break;}
        std::memcpy(chunk.bytes.data(),words.data(),chunk.bytes.size());
    }
    const auto conditionalBytes=edvr::dxbc_container::makeContainer(chunks);
    expect(conditionalReturn && !edvr::flatOverlayPatchPs(conditionalBytes.data(),conditionalBytes.size(),
           patched,patchWhy) && patched.empty(),"conditional-return bytecode is refused");

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps, variant, neutral, linked;
    expect(SUCCEEDED(device->CreateVertexShader(vsBytes->GetBufferPointer(),vsBytes->GetBufferSize(),nullptr,&vs)) &&
           SUCCEEDED(device->CreatePixelShader(psBytes->GetBufferPointer(),psBytes->GetBufferSize(),nullptr,&ps)) &&
           SUCCEEDED(device->CreatePixelShader(variantBytes->GetBufferPointer(),variantBytes->GetBufferSize(),nullptr,&variant)) &&
           SUCCEEDED(device->CreatePixelShader(neutralBytes->GetBufferPointer(),neutralBytes->GetBufferSize(),nullptr,&neutral)) &&
           SUCCEEDED(device->CreatePixelShader(linkedBytes->GetBufferPointer(),linkedBytes->GetBufferSize(),nullptr,&linked)),
           "fixture shaders create");
    if (!vs || !ps || !variant || !neutral || !linked) return failures;
    edvr::FlatOverlayLayer::rememberPixelShader(ps.Get(),psBytes->GetBufferPointer(),psBytes->GetBufferSize(),false);
    edvr::FlatOverlayLayer::rememberPixelShader(variant.Get(),variantBytes->GetBufferPointer(),variantBytes->GetBufferSize(),false);
    edvr::FlatOverlayLayer::rememberPixelShader(neutral.Get(),neutralBytes->GetBufferPointer(),neutralBytes->GetBufferSize(),false);
    edvr::FlatOverlayLayer::rememberPixelShader(linked.Get(),linkedBytes->GetBufferPointer(),linkedBytes->GetBufferSize(),true);

    D3D11_TEXTURE2D_DESC cd{};
    cd.Width=w;cd.Height=h;cd.MipLevels=cd.ArraySize=cd.SampleDesc.Count=1;
    cd.Format=DXGI_FORMAT_R11G11B10_FLOAT;
    cd.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> baseline, instrumented;
    ComPtr<ID3D11RenderTargetView> baselineRtv, instrumentedRtv;
    expect(SUCCEEDED(device->CreateTexture2D(&cd,nullptr,&baseline)) &&
           SUCCEEDED(device->CreateTexture2D(&cd,nullptr,&instrumented)) &&
           SUCCEEDED(device->CreateRenderTargetView(baseline.Get(),nullptr,&baselineRtv)) &&
           SUCCEEDED(device->CreateRenderTargetView(instrumented.Get(),nullptr,&instrumentedRtv)),
           "HDR targets create");
    D3D11_TEXTURE2D_DESC dd=cd;
    dd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> baselineDepth, instrumentedDepth;
    ComPtr<ID3D11DepthStencilView> baselineDsv, instrumentedDsv;
    expect(SUCCEEDED(device->CreateTexture2D(&dd,nullptr,&baselineDepth)) &&
           SUCCEEDED(device->CreateTexture2D(&dd,nullptr,&instrumentedDepth)) &&
           SUCCEEDED(device->CreateDepthStencilView(baselineDepth.Get(),nullptr,&baselineDsv)) &&
           SUCCEEDED(device->CreateDepthStencilView(instrumentedDepth.Get(),nullptr,&instrumentedDsv)),
           "depth/stencil targets create");
    if (!baselineRtv || !instrumentedRtv || !baselineDsv || !instrumentedDsv) return failures;

    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;
    rd.DepthClipEnable=TRUE;rd.ScissorEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    expect(SUCCEEDED(device->CreateRasterizerState(&rd,&raster)), "scissor raster state");
    D3D11_DEPTH_STENCIL_DESC zd{};
    zd.DepthEnable=TRUE;zd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    zd.DepthFunc=D3D11_COMPARISON_LESS;zd.StencilEnable=TRUE;
    zd.StencilReadMask=zd.StencilWriteMask=0xff;
    zd.FrontFace.StencilFunc=zd.BackFace.StencilFunc=D3D11_COMPARISON_EQUAL;
    zd.FrontFace.StencilFailOp=zd.BackFace.StencilFailOp=D3D11_STENCIL_OP_KEEP;
    zd.FrontFace.StencilDepthFailOp=zd.BackFace.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;
    zd.FrontFace.StencilPassOp=zd.BackFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
    ComPtr<ID3D11DepthStencilState> depthState;
    expect(SUCCEEDED(device->CreateDepthStencilState(&zd,&depthState)), "depth/stencil test state");
    D3D11_DEPTH_STENCIL_DESC readOnlyDesc=zd;
    readOnlyDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
    ComPtr<ID3D11DepthStencilState> readOnlyState;
    expect(SUCCEEDED(device->CreateDepthStencilState(&readOnlyDesc,&readOnlyState)),
           "read-only depth/stencil state");
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable=TRUE;
    bd.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;
    expect(SUCCEEDED(device->CreateBlendState(&bd,&blend)), "alpha blend state");
    D3D11_BLEND_DESC noColorDesc=bd;
    noColorDesc.RenderTarget[0].RenderTargetWriteMask=0;
    ComPtr<ID3D11BlendState> noColor;
    expect(SUCCEEDED(device->CreateBlendState(&noColorDesc,&noColor)), "prefill color-write-disabled state");
    D3D11_DEPTH_STENCIL_DESC prefillDesc=zd;
    prefillDesc.StencilEnable=FALSE;
    ComPtr<ID3D11DepthStencilState> prefillDepth;
    expect(SUCCEEDED(device->CreateDepthStencilState(&prefillDesc,&prefillDepth)), "depth prefill state");
    D3D11_DEPTH_STENCIL_DESC stencilDesc=zd;
    stencilDesc.DepthEnable=FALSE;
    stencilDesc.StencilReadMask=0xff;stencilDesc.StencilWriteMask=0xff;
    stencilDesc.FrontFace.StencilFunc=stencilDesc.BackFace.StencilFunc=D3D11_COMPARISON_ALWAYS;
    stencilDesc.FrontFace.StencilPassOp=stencilDesc.BackFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;
    ComPtr<ID3D11DepthStencilState> prefillStencil;
    expect(SUCCEEDED(device->CreateDepthStencilState(&stencilDesc,&prefillStencil)), "stencil prefill state");
    D3D11_DEPTH_STENCIL_DESC unionDesc=zd;
    unionDesc.DepthEnable=FALSE;unionDesc.StencilEnable=FALSE;
    ComPtr<ID3D11DepthStencilState> unionState;
    expect(SUCCEEDED(device->CreateDepthStencilState(&unionDesc,&unionState)), "mask union state");
    if (!raster || !depthState || !readOnlyState || !blend || !noColor || !prefillDepth || !prefillStencil || !unionState) return failures;
    context->ClearState();
    const float clear[4]={.1f,.2f,.3f,1};
    context->ClearRenderTargetView(baselineRtv.Get(),clear);
    context->ClearDepthStencilView(baselineDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8f,3);
    D3D11_VIEWPORT vp{};vp.Width=float(w);vp.Height=float(h);vp.MaxDepth=1;
    D3D11_RECT full{0,0,LONG(w),LONG(h)};
    auto bind = [&](ID3D11RenderTargetView* rtv,ID3D11DepthStencilView* dsv,ID3D11PixelShader* shader) {
        context->OMSetRenderTargets(1,&rtv,dsv);
        context->OMSetBlendState(blend.Get(),nullptr,~0u);
        context->OMSetDepthStencilState(depthState.Get(),3);
        context->RSSetState(raster.Get());context->RSSetViewports(1,&vp);context->RSSetScissorRects(1,&full);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(shader,nullptr,0);
    };
    auto bytes = [&](ID3D11Texture2D* source,UINT pixelBytes) {
        D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
        d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
        ComPtr<ID3D11Texture2D> staging;
        std::vector<BYTE> out(size_t(d.Width)*d.Height*pixelBytes);
        if (FAILED(device->CreateTexture2D(&d,nullptr,&staging))) return std::vector<BYTE>{};
        context->CopyResource(staging.Get(),source);
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map))) return std::vector<BYTE>{};
        for (UINT y=0;y<d.Height;++y)
            std::memcpy(out.data()+size_t(y)*d.Width*pixelBytes,
                        static_cast<const BYTE*>(map.pData)+size_t(y)*map.RowPitch,d.Width*pixelBytes);
        context->Unmap(staging.Get(),0);
        return out;
    };
    // Make depth reject the bottom half and stencil reject the upper-right.
    // The PS itself discards x<4, leaving only x=[4,8), y=[0,8) as coverage.
    bind(baselineRtv.Get(),baselineDsv.Get(),ps.Get());
    D3D11_RECT bottom{0,8,LONG(w),LONG(h)};
    context->RSSetScissorRects(1,&bottom);
    context->OMSetBlendState(noColor.Get(),nullptr,~0u);
    context->OMSetDepthStencilState(prefillDepth.Get(),3);
    context->Draw(3,0);
    D3D11_RECT upperRight{8,0,LONG(w),8};
    context->RSSetScissorRects(1,&upperRight);
    context->OMSetDepthStencilState(prefillStencil.Get(),1);
    context->Draw(3,0);
    context->CopyResource(instrumented.Get(),baseline.Get());
    context->CopyResource(instrumentedDepth.Get(),baselineDepth.Get());
    const auto cleanBefore=bytes(baseline.Get(),4);

    edvr::FlatOverlayLayer layer;
    edvr::FlatOverlayBlendDiagnostic ordinaryBlendSample;
    layer.beginFrame(10);
    bind(baselineRtv.Get(),baselineDsv.Get(),ps.Get());context->Draw(3,0);
    bind(instrumentedRtv.Get(),instrumentedDsv.Get(),ps.Get());
    const char* reason=nullptr;
    expect(layer.beginDraw(context,10,instrumented.Get(),instrumentedDsv.Get(),&reason,
        false,false,false,&ordinaryBlendSample),
           "supported draw binds private MRT7");
    expect(!ordinaryBlendSample.captured && ordinaryBlendSample.failures==0,
           "ordinary accepted blend leaves an explicit empty diagnostic sample");
    if (!reason) { context->Draw(3,0);layer.endDraw(context); }
    expect(layer.ready(10,instrumented.Get()) && layer.markedDraws()==1,
           "completed overlay is ready for this frame and HDR target");
    ComPtr<ID3D11PixelShader> restoredPs;context->PSGetShader(&restoredPs,nullptr,nullptr);
    ComPtr<ID3D11BlendState> restoredBlend;FLOAT factors[4]{};UINT sampleMask=0;
    context->OMGetBlendState(&restoredBlend,factors,&sampleMask);
    ID3D11RenderTargetView* rawRt=nullptr;ID3D11DepthStencilView* rawDs=nullptr;
    context->OMGetRenderTargets(1,&rawRt,&rawDs);
    expect(restoredPs.Get()==ps.Get() && restoredBlend.Get()==blend.Get() &&
           rawRt==instrumentedRtv.Get() && rawDs==instrumentedDsv.Get() && sampleMask==~0u,
           "original PS, blend, RTV, DSV and sample mask restored");
    if(rawRt)rawRt->Release();if(rawDs)rawDs->Release();
    const auto baselineColor=bytes(baseline.Get(),4), overlayColor=bytes(instrumented.Get(),4);
    const auto baselineZ=bytes(baselineDepth.Get(),4), overlayZ=bytes(instrumentedDepth.Get(),4);
    const auto cleanColor=layer.cleanHdr()?bytes(layer.cleanHdr(),4):std::vector<BYTE>{};
    expect(!baselineColor.empty() && baselineColor==overlayColor &&
           !baselineZ.empty() && baselineZ==overlayZ,
           "private output leaves original color, blend, depth and stencil byte-identical");
    expect(!cleanColor.empty() && cleanColor!=overlayColor &&
           cleanColor==cleanBefore,
           "clean backend HDR is the pre-overlay image, distinct from live HDR");
    ComPtr<ID3D11Resource> coverage;
    if(layer.coverageView())layer.coverageView()->GetResource(&coverage);
    ComPtr<ID3D11Texture2D> mask; if(coverage)coverage.As(&mask);
    const auto covered=mask?bytes(mask.Get(),1):std::vector<BYTE>{};
    UINT marked=0;
    for(BYTE v:covered)marked+=v==255;
    expect(covered.size()==w*h && marked==32 && covered.size()==w*h && covered[0]==0 && covered[5]==255 &&
           covered[10]==0 && covered[10*w+5]==0,
           "coverage marks only fragments passing PS discard, depth and stencil");

    // The second shader is a different supported colour variant. Its private
    // writes union with the first mask; a new frame starts with a fresh mask.
    D3D11_RECT upperRightUnion{8,0,LONG(w),8};
    bind(instrumentedRtv.Get(),instrumentedDsv.Get(),variant.Get());
    context->OMSetDepthStencilState(unionState.Get(),3);
    context->RSSetScissorRects(1,&upperRightUnion);
    expect(layer.beginDraw(context,10,instrumented.Get(),instrumentedDsv.Get(),&reason),
           "second colour variant is admitted on the same HDR target");
    if (!reason) {context->Draw(3,0);layer.endDraw(context);}
    const auto unionMask=mask?bytes(mask.Get(),1):std::vector<BYTE>{};
    UINT unionPixels=0;for(BYTE v:unionMask)unionPixels+=v==255;
    expect(layer.markedDraws()==2 && unionPixels==96 && unionMask.size()==w*h &&
           unionMask[5]==255 && unionMask[10]==255,
           "two private fragment masks union without erasing prior marks");
    const auto priorLive=bytes(instrumented.Get(),4);
    layer.beginFrame(11);
    D3D11_RECT freshRect{0,0,4,8};
    bind(instrumentedRtv.Get(),instrumentedDsv.Get(),variant.Get());
    context->OMSetDepthStencilState(unionState.Get(),3);
    context->RSSetScissorRects(1,&freshRect);
    expect(layer.beginDraw(context,11,instrumented.Get(),instrumentedDsv.Get(),&reason),
           "same COM HDR and views are reusable next frame");
    if (!reason) {context->Draw(3,0);layer.endDraw(context);}
    ComPtr<ID3D11Resource> freshResource;
    if(layer.coverageView())layer.coverageView()->GetResource(&freshResource);
    ComPtr<ID3D11Texture2D> freshMask; if(freshResource)freshResource.As(&freshMask);
    const auto fresh=freshMask?bytes(freshMask.Get(),1):std::vector<BYTE>{};
    UINT freshPixels=0;for(BYTE v:fresh)freshPixels+=v==255;
    expect(layer.ready(11,instrumented.Get()) && layer.cleanHdr() &&
           bytes(layer.cleanHdr(),4)==priorLive && freshPixels==32 && fresh.size()==w*h &&
           fresh[1]==255 && fresh[5]==0 && fresh[10]==0,
           "next frame refreshes clean HDR and clears old private coverage");

    // A neutral alpha-blended draw can have no visible colour delta while
    // still covering every pixel. It must export coverage without changing H,
    // depth, or stencil; absence of a colour delta is not proof of no draw.
    context->ClearRenderTargetView(instrumentedRtv.Get(),clear);
    context->ClearDepthStencilView(instrumentedDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8f,3);
    const auto neutralBefore=bytes(instrumented.Get(),4),neutralDepthBefore=bytes(instrumentedDepth.Get(),4);
    edvr::FlatOverlayLayer neutralLayer;
    neutralLayer.beginFrame(20);
    bind(instrumentedRtv.Get(),instrumentedDsv.Get(),neutral.Get());
    context->OMSetDepthStencilState(readOnlyState.Get(),3);
    expect(neutralLayer.beginDraw(context,20,instrumented.Get(),instrumentedDsv.Get(),&reason),
           "neutral alpha-zero draw admits private coverage");
    if(!reason) {context->Draw(3,0);neutralLayer.endDraw(context);}
    ComPtr<ID3D11Resource> neutralResource;
    if(neutralLayer.coverageView())neutralLayer.coverageView()->GetResource(&neutralResource);
    ComPtr<ID3D11Texture2D> neutralMask;if(neutralResource)neutralResource.As(&neutralMask);
    const auto neutralMarks=neutralMask?bytes(neutralMask.Get(),1):std::vector<BYTE>{};
    UINT neutralCount=0;for(BYTE v:neutralMarks)neutralCount+=v==255;
    expect(neutralLayer.ready(20,instrumented.Get()) && neutralCount==w*h &&
           bytes(instrumented.Get(),4)==neutralBefore &&
           bytes(instrumentedDepth.Get(),4)==neutralDepthBefore,
           "alpha-zero draw marks every passing fragment despite byte-identical HDR/depth/stencil");

    // The captured late PS uses read-only structured loads behind IF/ELSE.
    // Raw SRV loads are the adjacent legal shape. Compare real raster output
    // against the unmodified PS, not just the rewritten DXBC's structure.
    ComPtr<ID3D11PixelShader> structuredPs, rawPs;
    expect(SUCCEEDED(device->CreatePixelShader(structuredBytes->GetBufferPointer(),
           structuredBytes->GetBufferSize(),nullptr,&structuredPs)) &&
           SUCCEEDED(device->CreatePixelShader(rawBytes->GetBufferPointer(),
           rawBytes->GetBufferSize(),nullptr,&rawPs)),
           "read-only structured and raw PS variants create");
    const float values[8]={.75f,.125f,.25f,1,.125f,.75f,.25f,1};
    D3D11_SUBRESOURCE_DATA bufferInit{};bufferInit.pSysMem=values;
    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth=sizeof(values);bufferDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    bufferDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufferDesc.StructureByteStride=16;
    ComPtr<ID3D11Buffer> structuredBuffer, rawBuffer;
    ComPtr<ID3D11ShaderResourceView> structuredSrv, rawSrv;
    D3D11_SHADER_RESOURCE_VIEW_DESC structuredView{};
    structuredView.Format=DXGI_FORMAT_UNKNOWN;
    structuredView.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
    structuredView.Buffer.NumElements=2;
    expect(SUCCEEDED(device->CreateBuffer(&bufferDesc,&bufferInit,&structuredBuffer)) &&
           SUCCEEDED(device->CreateShaderResourceView(structuredBuffer.Get(),&structuredView,&structuredSrv)),
           "structured SRV fixture creates");
    bufferDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    bufferDesc.StructureByteStride=0;
    D3D11_SHADER_RESOURCE_VIEW_DESC rawView{};
    rawView.Format=DXGI_FORMAT_R32_TYPELESS;
    rawView.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
    rawView.BufferEx.NumElements=8;
    rawView.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
    expect(SUCCEEDED(device->CreateBuffer(&bufferDesc,&bufferInit,&rawBuffer)) &&
           SUCCEEDED(device->CreateShaderResourceView(rawBuffer.Get(),&rawView,&rawSrv)),
           "raw byte-address SRV fixture creates");
    if(structuredPs && rawPs && structuredSrv && rawSrv) {
        edvr::FlatOverlayLayer::rememberPixelShader(structuredPs.Get(),structuredBytes->GetBufferPointer(),
            structuredBytes->GetBufferSize(),false);
        edvr::FlatOverlayLayer::rememberPixelShader(rawPs.Get(),rawBytes->GetBufferPointer(),
            rawBytes->GetBufferSize(),false);
        struct ReadCase {ID3D11PixelShader* shader;ID3D11ShaderResourceView* srv;uint64_t frame;};
        const ReadCase cases[]={{structuredPs.Get(),structuredSrv.Get(),30},
                                {rawPs.Get(),rawSrv.Get(),31}};
        for(const auto& read:cases) {
            context->ClearRenderTargetView(baselineRtv.Get(),clear);
            context->ClearRenderTargetView(instrumentedRtv.Get(),clear);
            context->ClearDepthStencilView(baselineDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8f,3);
            context->ClearDepthStencilView(instrumentedDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8f,3);
            bind(baselineRtv.Get(),baselineDsv.Get(),read.shader);
            context->OMSetDepthStencilState(readOnlyState.Get(),3);
            ID3D11ShaderResourceView* srv=read.srv;
            context->PSSetShaderResources(1,1,&srv);
            context->Draw(3,0);
            bind(instrumentedRtv.Get(),instrumentedDsv.Get(),read.shader);
            context->OMSetDepthStencilState(readOnlyState.Get(),3);
            context->PSSetShaderResources(1,1,&srv);
            edvr::FlatOverlayLayer readLayer;
            readLayer.beginFrame(read.frame);
            const bool began=readLayer.beginDraw(context,read.frame,instrumented.Get(),instrumentedDsv.Get(),&reason);
            expect(began,"read-only buffer PS binds private MRT7");
            if(began) {context->Draw(3,0);readLayer.endDraw(context);}
            ComPtr<ID3D11Resource> readCoverage;
            if(readLayer.coverageView())readLayer.coverageView()->GetResource(&readCoverage);
            ComPtr<ID3D11Texture2D> readMask;if(readCoverage)readCoverage.As(&readMask);
            const auto marks=readMask?bytes(readMask.Get(),1):std::vector<BYTE>{};
            UINT count=0;for(BYTE v:marks)count+=v==255;
            expect(readLayer.ready(read.frame,instrumented.Get()) && count==w*h &&
                   bytes(baseline.Get(),4)==bytes(instrumented.Get(),4) &&
                   bytes(baselineDepth.Get(),4)==bytes(instrumentedDepth.Get(),4),
                   "read-only buffer shader keeps original raster colour/depth/stencil and exports coverage");
        }
        ID3D11ShaderResourceView* none=nullptr;
        context->PSSetShaderResources(1,1,&none);
    }

    // A linked shader and occupied MRT7 are closed-world refusals; no draw is
    // silently reclassified as protected when the private target cannot bind.
    edvr::FlatOverlayLayer refused;
    refused.beginFrame(11);
    bind(instrumentedRtv.Get(),instrumentedDsv.Get(),linked.Get());
    expect(!refused.beginDraw(context,11,instrumented.Get(),instrumentedDsv.Get(),&reason) &&
           reason && std::strstr(reason,"linkage"), "linked PS refuses before game draw");
    edvr::FlatOverlayLayer occupied;
    occupied.beginFrame(12);
    bind(instrumentedRtv.Get(),instrumentedDsv.Get(),ps.Get());
    ID3D11RenderTargetView* rts[8]{};rts[0]=instrumentedRtv.Get();rts[7]=baselineRtv.Get();
    context->OMSetRenderTargets(8,rts,instrumentedDsv.Get());
    expect(!occupied.beginDraw(context,12,instrumented.Get(),instrumentedDsv.Get(),&reason) &&
           reason, "occupied MRT7 refuses before game draw");
    edvr::FlatOverlayLayer uavBound;
    uavBound.beginFrame(13);
    bind(instrumentedRtv.Get(),instrumentedDsv.Get(),ps.Get());
    D3D11_TEXTURE2D_DESC ud=cd;
    ud.Format=DXGI_FORMAT_R32_UINT;ud.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> uavTexture;
    ComPtr<ID3D11UnorderedAccessView> uav;
    expect(SUCCEEDED(device->CreateTexture2D(&ud,nullptr,&uavTexture)) &&
           SUCCEEDED(device->CreateUnorderedAccessView(uavTexture.Get(),nullptr,&uav)),
           "OM UAV refusal fixture creates");
    if(uav) {
        ID3D11RenderTargetView* rt=instrumentedRtv.Get();
        ID3D11UnorderedAccessView* rawUav=uav.Get();
        context->OMSetRenderTargetsAndUnorderedAccessViews(1,&rt,instrumentedDsv.Get(),1,1,&rawUav,nullptr);
        expect(!uavBound.beginDraw(context,13,instrumented.Get(),instrumentedDsv.Get(),&reason) &&
               reason && std::strstr(reason,"UAV"),
               "PS UAV binding refuses private MRT injection before the game draw");
    }
    edvr::FlatOverlayLayer predicated;
    predicated.beginFrame(14);
    bind(instrumentedRtv.Get(),instrumentedDsv.Get(),ps.Get());
    D3D11_QUERY_DESC predicateDesc{};predicateDesc.Query=D3D11_QUERY_OCCLUSION_PREDICATE;
    ComPtr<ID3D11Predicate> predicate;
    expect(SUCCEEDED(device->CreatePredicate(&predicateDesc,&predicate)),
           "predication refusal fixture creates");
    if(predicate) {
        context->SetPredication(predicate.Get(),TRUE);
        expect(!predicated.beginDraw(context,14,instrumented.Get(),instrumentedDsv.Get(),&reason) &&
               reason && std::strstr(reason,"predication"),
               "predicated draw refuses private coverage injection");
        context->SetPredication(nullptr,FALSE);
    }
    // Diagnostics read the exact live GetDesc state at a real existing guard
    // failure, retain only the first sample, and leave admission/state intact.
    {
        D3D11_BLEND_DESC dualDesc{};
        for(auto& t:dualDesc.RenderTarget) {
            t.SrcBlend=t.SrcBlendAlpha=D3D11_BLEND_ONE;
            t.DestBlend=t.DestBlendAlpha=D3D11_BLEND_ZERO;
            t.BlendOp=t.BlendOpAlpha=D3D11_BLEND_OP_ADD;
            t.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        }
        dualDesc.RenderTarget[0].BlendEnable=TRUE;
        dualDesc.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC1_COLOR;
        ComPtr<ID3D11BlendState> dualBlend;
        expect(SUCCEEDED(device->CreateBlendState(&dualDesc,&dualBlend)),
               "actual active dual-source diagnostic fixture creates");
        if(dualBlend) {
            bind(instrumentedRtv.Get(),instrumentedDsv.Get(),ps.Get());
            context->OMSetBlendState(dualBlend.Get(),nullptr,~0u);
            D3D11_BLEND_DESC actual{};dualBlend->GetDesc(&actual);
            ComPtr<ID3D11VertexShader> actualVs;context->VSGetShader(&actualVs,nullptr,nullptr);
            const auto beforeColor=bytes(instrumented.Get(),4);
            const auto beforeDepth=bytes(instrumentedDepth.Get(),4);
            edvr::FlatOverlayBlendDiagnostic sample;
            edvr::FlatOverlayLayer dualLayer;dualLayer.beginFrame(20);
            expect(!dualLayer.beginDraw(context,20,instrumented.Get(),instrumentedDsv.Get(),
                &reason,false,false,false,&sample) && reason &&
                std::strcmp(reason,"dual-source-blend")==0,
                "sampled active dual-source blend still refuses before game draw");
            expect(sample.captured && sample.failures==1 && sample.frame==20 &&
                sample.ps==ps.Get() && sample.vs==actualVs.Get() &&
                sample.hdr==instrumented.Get() && sample.dsv==instrumentedDsv.Get() &&
                sample.activeRtvMask==1 && sample.activeRtvCount==1 &&
                std::memcmp(&sample.blend,&actual,sizeof(actual))==0 &&
                sample.effectiveSlots==1 && sample.effectiveChannels[0]==7 &&
                !sample.psOutputSignatureKnown,
                "first refusal captures actual descriptor, bindings and effective RGB dependency");
            ComPtr<ID3D11BlendState> stillBlend;context->OMGetBlendState(&stillBlend,nullptr,nullptr);
            ComPtr<ID3D11PixelShader> stillPs;context->PSGetShader(&stillPs,nullptr,nullptr);
            ID3D11RenderTargetView* stillRtv=nullptr;ID3D11DepthStencilView* stillDsv=nullptr;
            context->OMGetRenderTargets(1,&stillRtv,&stillDsv);
            expect(stillBlend.Get()==dualBlend.Get() && stillPs.Get()==ps.Get() &&
                stillRtv==instrumentedRtv.Get() && stillDsv==instrumentedDsv.Get() &&
                beforeColor==bytes(instrumented.Get(),4) && beforeDepth==bytes(instrumentedDepth.Get(),4),
                "refusal diagnostic preserves game OM/PS bindings and color/depth/stencil bytes");
            if(stillRtv)stillRtv->Release();if(stillDsv)stillDsv->Release();
            dualLayer.beginFrame(21);
            expect(!dualLayer.beginDraw(context,21,instrumented.Get(),instrumentedDsv.Get(),
                &reason,false,false,false,&sample) && sample.failures==2 && sample.frame==20,
                "another refusing frame increments count without replacing first window sample");
            sample={};dualLayer.beginFrame(22);
            expect(!dualLayer.beginDraw(context,22,instrumented.Get(),instrumentedDsv.Get(),
                &reason,false,false,false,&sample) && sample.failures==1 && sample.frame==22,
                "report-window reset rearms the first actual refusal sample");
        }
        // WARP canonicalizes API-inactive SRC1 fields; production reads
        // GetDesc, so an input descriptor is not evidence of a live refusal.
        dualDesc.RenderTarget[0].BlendEnable=FALSE;
        ComPtr<ID3D11BlendState> disabledBlend;
        expect(SUCCEEDED(device->CreateBlendState(&dualDesc,&disabledBlend)),
               "disabled SRC1 fixture creates");
        if(disabledBlend) {
            D3D11_BLEND_DESC actual{};disabledBlend->GetDesc(&actual);
            expect(actual.RenderTarget[0].SrcBlend==D3D11_BLEND_ONE,
                   "WARP actual GetDesc canonicalizes disabled SRC1 factor");
        }
        edvr::FlatOverlayBlendDiagnostic classification;
        classification.blend=dualDesc;classification.activeRtvMask=1;
        edvr::flatOverlayClassifyBlendDiagnostic(classification);
        expect(classification.effectiveSlots==0,"disabled blend has no effective SRC1 dependency");
        classification.blend.RenderTarget[0].SrcBlend=D3D11_BLEND_ONE;
        classification.blend.RenderTarget[1].BlendEnable=TRUE;
        classification.blend.RenderTarget[1].SrcBlend=D3D11_BLEND_SRC1_ALPHA;
        classification.activeRtvMask=3;
        edvr::flatOverlayClassifyBlendDiagnostic(classification);
        expect(classification.effectiveSlots==0,"nonindependent blend ignores dormant RT1 descriptor");
        classification.blend.IndependentBlendEnable=TRUE;
        edvr::flatOverlayClassifyBlendDiagnostic(classification);
        expect(classification.effectiveSlots==2 && classification.effectiveChannels[1]==7,
               "independent active slot classifies contributing RGB factors");
        classification.activeRtvMask=1;
        edvr::flatOverlayClassifyBlendDiagnostic(classification);
        expect(classification.effectiveSlots==0,"unbound RTV contributes no diagnostic dependency");
        classification.activeRtvMask=3;
        classification.blend.RenderTarget[1].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALPHA;
        edvr::flatOverlayClassifyBlendDiagnostic(classification);
        expect(classification.effectiveSlots==0,"unwritten RGB channels contribute no SRC1 dependency");
        classification.blend.RenderTarget[1].SrcBlendAlpha=D3D11_BLEND_SRC1_ALPHA;
        edvr::flatOverlayClassifyBlendDiagnostic(classification);
        expect(classification.effectiveChannels[1]==8,"written alpha classifies its own SRC1 factor");
        classification.blend.RenderTarget[1].BlendOpAlpha=D3D11_BLEND_OP_MAX;
        edvr::flatOverlayClassifyBlendDiagnostic(classification);
        expect(classification.effectiveSlots==0,"MIN/MAX ignores blend factors in diagnostic classification");
    }
    // True dual-source color stays on the untouched original draw. Coverage
    // replays against a copied private stencil/depth resource before it.
    {
        auto dualBytes=compile("struct O{float4 c:SV_Target0;float4 b:SV_Target1;};"
            "O main(float4 p:SV_Position){if(p.x<4)discard;O o;o.c=float4(.2,.3,.4,.5);o.b=float4(.7,.6,.5,.4);return o;}","ps_5_0");
        auto leftBytes=compile("struct O{float4 c:SV_Target0;float4 b:SV_Target1;};"
            "O main(float4 p:SV_Position){if(p.x>=4)discard;O o;o.c=float4(.3,.2,.4,.5);o.b=float4(.7,.6,.5,.4);return o;}","ps_5_0");
        ComPtr<ID3D11PixelShader> dual,left;
        expect(dualBytes && leftBytes && SUCCEEDED(device->CreatePixelShader(dualBytes->GetBufferPointer(),dualBytes->GetBufferSize(),nullptr,&dual)) &&
            SUCCEEDED(device->CreatePixelShader(leftBytes->GetBufferPointer(),leftBytes->GetBufferSize(),nullptr,&left)),"true dual-output replay shaders create");
        if(dual && left) {
            edvr::FlatOverlayLayer::rememberPixelShader(dual.Get(),dualBytes->GetBufferPointer(),dualBytes->GetBufferSize(),false);
            edvr::FlatOverlayLayer::rememberPixelShader(left.Get(),leftBytes->GetBufferPointer(),leftBytes->GetBufferSize(),false);
            D3D11_BLEND_DESC bd{};auto& t=bd.RenderTarget[0];
            t.BlendEnable=TRUE;t.SrcBlend=t.SrcBlendAlpha=D3D11_BLEND_ONE;
            t.DestBlend=D3D11_BLEND_SRC1_COLOR;t.DestBlendAlpha=D3D11_BLEND_SRC1_ALPHA;
            t.BlendOp=t.BlendOpAlpha=D3D11_BLEND_OP_ADD;t.RenderTargetWriteMask=15;
            ComPtr<ID3D11BlendState> dualBlend;expect(SUCCEEDED(device->CreateBlendState(&bd,&dualBlend)),"true dual-source blend state creates");
            D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;ds.DepthFunc=D3D11_COMPARISON_LESS;
            ds.StencilEnable=TRUE;ds.StencilReadMask=1;ds.StencilWriteMask=4;
            ds.FrontFace.StencilFunc=D3D11_COMPARISON_EQUAL;
            ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;
            ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;ds.BackFace=ds.FrontFace;
            ComPtr<ID3D11DepthStencilState> stencilWrites;expect(SUCCEEDED(device->CreateDepthStencilState(&ds,&stencilWrites)),"replay stencil-write-mask4 state creates");
            if(dualBlend && stencilWrites) {
                context->ClearRenderTargetView(baselineRtv.Get(),clear);context->ClearRenderTargetView(instrumentedRtv.Get(),clear);
                context->ClearDepthStencilView(baselineDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8f,5);
                context->ClearDepthStencilView(instrumentedDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8f,5);
                const auto preColor=bytes(instrumented.Get(),4),preDepth=bytes(instrumentedDepth.Get(),4);
                edvr::FlatOverlayLayer replay;replay.beginFrame(80);
                const auto draw=[&](ID3D11PixelShader* shader) {
                    bind(baselineRtv.Get(),baselineDsv.Get(),shader);context->OMSetBlendState(dualBlend.Get(),nullptr,~0u);context->OMSetDepthStencilState(stencilWrites.Get(),1);context->Draw(3,0);
                    bind(instrumentedRtv.Get(),instrumentedDsv.Get(),shader);context->OMSetBlendState(dualBlend.Get(),nullptr,~0u);context->OMSetDepthStencilState(stencilWrites.Get(),1);
                    const char* why=nullptr;
                    expect(!replay.beginDraw(context,80,instrumented.Get(),instrumentedDsv.Get(),&why) && why && std::strcmp(why,"dual-source-blend")==0,"normal MRT7 guard still refuses true dual-source");
                    const bool began=replay.beginReplayDraw(context,80,instrumented.Get(),instrumentedDsv.Get(),&why);
                    expect(began,"guarded private DSV/RT0 replay begins after dual-source refusal");
                    if(!began)return;
                    context->Draw(3,0);expect(replay.finishReplayDraw(context),"private replay restores game before original draw");
                    ComPtr<ID3D11PixelShader> restored;ComPtr<ID3D11BlendState> restoredBlend;
                    context->PSGetShader(&restored,nullptr,nullptr);context->OMGetBlendState(&restoredBlend,nullptr,nullptr);
                    ID3D11RenderTargetView* restoredRtv=nullptr;ID3D11DepthStencilView* restoredDsv=nullptr;context->OMGetRenderTargets(1,&restoredRtv,&restoredDsv);
                    expect(restored.Get()==shader && restoredBlend.Get()==dualBlend.Get() && restoredRtv==instrumentedRtv.Get() && restoredDsv==instrumentedDsv.Get(),"dual-source original PS/blend/RTV/DSV restored exactly");
                    if(restoredRtv)restoredRtv->Release();if(restoredDsv)restoredDsv->Release();
                    context->Draw(3,0);replay.endReplayOriginalDraw(context);
                };
                draw(dual.Get());
                expect(bytes(baseline.Get(),4)==bytes(instrumented.Get(),4) && bytes(baselineDepth.Get(),4)==bytes(instrumentedDepth.Get(),4),"dual-source replay leaves original color/depth/stencil byte-identical");
                expect(replay.ready(80,instrumented.Get()) && replay.markedDraws()==1 && bytes(replay.cleanHdr(),4)==preColor,"replay retains first pre-overlay clean HDR and completed original receipt");
                ComPtr<ID3D11Resource> coveredResource;replay.coverageView()->GetResource(&coveredResource);ComPtr<ID3D11Texture2D> coveredTexture;coveredResource.As(&coveredTexture);
                auto coveredBytes=bytes(coveredTexture.Get(),1);bool exact=coveredBytes.size()==w*h;
                for(UINT y=0;y<h && exact;++y)for(UINT x=0;x<w;++x)exact &= coveredBytes[y*w+x]==(x<4?0:255);
                expect(exact,"private coverage excludes actual dual-source PS discard");
                draw(left.Get());coveredBytes=bytes(coveredTexture.Get(),1);
                expect(replay.markedDraws()==2 && std::all_of(coveredBytes.begin(),coveredBytes.end(),[](BYTE b){return b==255;}) && bytes(replay.cleanHdr(),4)==preColor,"later dual-source replay unions coverage without refreshing clean HDR");
                expect(bytes(baseline.Get(),4)==bytes(instrumented.Get(),4) && bytes(baselineDepth.Get(),4)==bytes(instrumentedDepth.Get(),4),"second replay reproduces current stencil transitions privately");
                edvr::FlatOverlayLayer depthRefused;depthRefused.beginFrame(81);context->OMSetDepthStencilState(prefillDepth.Get(),1);
                const auto beforeRefusal=bytes(instrumentedDepth.Get(),4);
                expect(!depthRefused.beginReplayDraw(context,81,instrumented.Get(),instrumentedDsv.Get(),&reason) && reason && std::strcmp(reason,"replay-game-depth-write")==0 && bytes(instrumentedDepth.Get(),4)==beforeRefusal,"replay refuses original depth writes before private copies or draws");
            }
        }
        // An undeclared original o7 use cannot become private coverage. Only
        // the exact injected terminal MOV o7.x,1 is remapped to o0.
        std::vector<BYTE> normal,replay;std::string why;
        expect(edvr::flatOverlayPatchPs(psBytes->GetBufferPointer(),psBytes->GetBufferSize(),normal,why) && edvr::flatOverlayCoverageFromPatchedPs(normal.data(),normal.size(),replay,why),"qualified normal coverage bytecode derives replay variant");
        auto badChunks=edvr::dxbc_container::parseContainer(normal.data(),normal.size(),0x50);
        bool changed=false;
        for(auto& chunk:badChunks)if(chunk.tag==0x58454853u || chunk.tag==0x52444853u) {
            std::vector<uint32_t> words(chunk.bytes.size()/4);std::memcpy(words.data(),chunk.bytes.data(),chunk.bytes.size());
            for(size_t at=2;at<words.size();) { const auto length=edvr::dxbc_container::instructionLength(words,at);
                if((words[at]&0x7ffu)==54 && length>=3 && ((words[at+1]>>12)&255u)==2 && words[at+2]<7) { words[at+2]=7;changed=true;break; }at+=length; }
            std::memcpy(chunk.bytes.data(),words.data(),chunk.bytes.size());
        }
        auto bad=edvr::dxbc_container::makeContainer(badChunks);
        expect(changed && !edvr::flatOverlayCoverageFromPatchedPs(bad.data(),bad.size(),replay,why) && replay.empty(),"replay refuses undeclared original MRT7 operand");
        for(unsigned mutation=0;mutation<3;++mutation) {
            auto chunks=edvr::dxbc_container::parseContainer(normal.data(),normal.size(),0x50);bool altered=false;
            for(auto& chunk:chunks)if(chunk.tag==0x58454853u || chunk.tag==0x52444853u) {
                std::vector<uint32_t> words(chunk.bytes.size()/4);std::memcpy(words.data(),chunk.bytes.data(),chunk.bytes.size());
                for(size_t at=2;at<words.size();) {
                    const auto length=edvr::dxbc_container::instructionLength(words,at);
                    if(mutation==2 && (words[at]&0x7ffu)==104 && length==2) {
                        words[at+1]=4090;altered=true;break;
                    }
                    if(mutation<2 && (words[at]&0x7ffu)==54 && length>=3 &&
                       ((words[at+1]>>12)&255u)==2 && words[at+2]<7) {
                        if(mutation==0)words[at+1]|=2u<<22; // unbounded relative output index
                        else words[at]|=0x80000000u; // malformed opcode extension consumes operand
                        altered=true;break;
                    }
                    at+=length;
                }
                std::memcpy(chunk.bytes.data(),words.data(),chunk.bytes.size());
            }
            auto malformed=edvr::dxbc_container::makeContainer(chunks);
            const char* labels[]={"replay rejects relative output operand","replay rejects malformed opcode extension","replay rejects SM5 temp overflow"};
            expect(altered && !edvr::flatOverlayCoverageFromPatchedPs(malformed.data(),malformed.size(),replay,why) && replay.empty(),labels[mutation]);
        }
    }
    context->ClearState();
    return failures;
}
