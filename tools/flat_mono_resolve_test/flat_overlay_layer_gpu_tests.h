#pragma once
#include "../../src/d3d11/flat_overlay_layer.h"

inline int flatOverlayLayerGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using Microsoft::WRL::ComPtr;
    int failures = 0;
    auto expect = [&](bool ok, const char* why) {
        if (!ok) { std::printf("FAIL: flat overlay GPU %s\n", why); ++failures; }
    };
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
    if (!vsBytes || !psBytes || !variantBytes || !neutralBytes || !linkedBytes || !occupiedBytes || !depthBytes) return failures;
    std::vector<BYTE> patched;
    std::string patchWhy;
    expect(edvr::flatOverlayPatchPs(psBytes->GetBufferPointer(), psBytes->GetBufferSize(), patched, patchWhy) &&
           !patched.empty(), "pure discard shader gains a private output");
    expect(!edvr::flatOverlayPatchPs(occupiedBytes->GetBufferPointer(), occupiedBytes->GetBufferSize(),
                                     patched, patchWhy) && patched.empty(), "existing MRT7 is refused");
    expect(!edvr::flatOverlayPatchPs(depthBytes->GetBufferPointer(), depthBytes->GetBufferSize(),
                                     patched, patchWhy) && patched.empty(), "depth-writing PS is refused");
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
    layer.beginFrame(10);
    bind(baselineRtv.Get(),baselineDsv.Get(),ps.Get());context->Draw(3,0);
    bind(instrumentedRtv.Get(),instrumentedDsv.Get(),ps.Get());
    const char* reason=nullptr;
    expect(layer.beginDraw(context,10,instrumented.Get(),instrumentedDsv.Get(),&reason),
           "supported draw binds private MRT7");
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
    context->ClearState();
    return failures;
}
