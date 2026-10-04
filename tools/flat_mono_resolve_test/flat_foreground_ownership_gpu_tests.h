#pragma once
#include "../../src/d3d11/flat_foreground_ownership.h"
#include "../../src/d3d11/flat_overlay_layer.h"
#include "../../src/d3d11/flat_untrusted_coverage.h"
#include "flat_empty_output_gpu_tests.h"

inline int flatForegroundOwnershipGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using Microsoft::WRL::ComPtr;
    int failures=0;
    auto check=[&](bool good,const char* label) {
        if(!good) { std::printf("FAIL: foreground ownership GPU %s\n",label); ++failures; }
    };
    constexpr UINT w=13,h=9;
    auto compile=[&](const char* source,const char* entry,const char* profile) {
        ComPtr<ID3DBlob> code,errors;
        const HRESULT hr=D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,
                                    entry,profile,0,0,&code,&errors);
        if(FAILED(hr) && errors) std::printf("foreground shader: %s\n",
            static_cast<const char*>(errors->GetBufferPointer()));
        check(SUCCEEDED(hr),"fixture and production shaders compile");
        return code;
    };
    const auto vsCode=compile("float4 main(uint id:SV_VertexID):SV_Position {"
        "float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};"
        "return float4(p[id],.5,1);}","main","vs_5_0");
    const auto psCode=compile("float4 main(float4 p:SV_Position):SV_Target {"
        "if(p.x<3) discard; return float4(1,0,0,0);}","main","ps_5_0");
    const auto csCode=compile(edvr::kFlatForegroundOwnershipCs,"main","cs_5_0");
    const auto mergeCode=compile(edvr::kFlatForegroundMergeCs,"main","cs_5_0");
    const auto secondVsCode=compile("float4 main(uint id:SV_VertexID):SV_Position {"
        "float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};"
        "return float4(p[id],.4,1);}","main","vs_5_0");
    const auto secondPsCode=compile("float4 main():SV_Target {return float4(1,0,0,0);}","main","ps_5_0");
    const auto explicitDepthPsCode=compile("float main():SV_Depth{return .5;}","main","ps_5_0");
    if(!vsCode || !psCode || !csCode || !mergeCode || !secondVsCode ||
       !secondPsCode || !explicitDepthPsCode)return failures;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11VertexShader> secondVs;
    ComPtr<ID3D11PixelShader> ps,secondPs,explicitDepthPs;
    ComPtr<ID3D11ComputeShader> cs,mergeCs;
    check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)) &&
          SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps)) &&
          SUCCEEDED(device->CreateComputeShader(csCode->GetBufferPointer(),csCode->GetBufferSize(),nullptr,&cs)) &&
          SUCCEEDED(device->CreateComputeShader(mergeCode->GetBufferPointer(),mergeCode->GetBufferSize(),nullptr,&mergeCs)) &&
          SUCCEEDED(device->CreateVertexShader(secondVsCode->GetBufferPointer(),secondVsCode->GetBufferSize(),nullptr,&secondVs)) &&
          SUCCEEDED(device->CreatePixelShader(secondPsCode->GetBufferPointer(),secondPsCode->GetBufferSize(),nullptr,&secondPs)) &&
          SUCCEEDED(device->CreatePixelShader(explicitDepthPsCode->GetBufferPointer(),explicitDepthPsCode->GetBufferSize(),nullptr,&explicitDepthPs)),
          "raster and actual ownership compute shaders create");
    if(!vs || !ps || !cs || !mergeCs || !secondVs || !secondPs || !explicitDepthPs)return failures;
    edvr::FlatOverlayLayer::rememberPixelShader(ps.Get(),psCode->GetBufferPointer(),psCode->GetBufferSize(),false);
    edvr::FlatOverlayLayer::rememberPixelShader(secondPs.Get(),secondPsCode->GetBufferPointer(),secondPsCode->GetBufferSize(),false);
    edvr::FlatOverlayLayer::rememberPixelShader(explicitDepthPs.Get(),explicitDepthPsCode->GetBufferPointer(),explicitDepthPsCode->GetBufferSize(),false);

    D3D11_TEXTURE2D_DESC colorDesc{};
    colorDesc.Width=w;colorDesc.Height=h;colorDesc.MipLevels=colorDesc.ArraySize=colorDesc.SampleDesc.Count=1;
    colorDesc.Format=DXGI_FORMAT_R10G10B10A2_TYPELESS;
    colorDesc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> baselineColor,liveColor;
    check(SUCCEEDED(device->CreateTexture2D(&colorDesc,nullptr,&baselineColor)) &&
          SUCCEEDED(device->CreateTexture2D(&colorDesc,nullptr,&liveColor)),
          "format-23 typeless pool resources create");
    D3D11_RENDER_TARGET_VIEW_DESC colorViewDesc{};
    colorViewDesc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
    colorViewDesc.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> baselineRtv,liveRtv;
    if(baselineColor && liveColor)
        check(SUCCEEDED(device->CreateRenderTargetView(baselineColor.Get(),&colorViewDesc,&baselineRtv)) &&
              SUCCEEDED(device->CreateRenderTargetView(liveColor.Get(),&colorViewDesc,&liveRtv)),
              "format-24 typed pool RTVs create");

    D3D11_TEXTURE2D_DESC depthDesc=colorDesc;
    depthDesc.Format=DXGI_FORMAT_R32G8X24_TYPELESS;
    depthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> baselineDepth,liveDepth,cohortDepth;
    check(SUCCEEDED(device->CreateTexture2D(&depthDesc,nullptr,&baselineDepth)) &&
          SUCCEEDED(device->CreateTexture2D(&depthDesc,nullptr,&liveDepth)) &&
          SUCCEEDED(device->CreateTexture2D(&depthDesc,nullptr,&cohortDepth)),
          "shared-depth resources and post-cohort snapshot create");
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dsvDesc.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> baselineDsv,liveDsv;
    if(baselineDepth && liveDepth)
        check(SUCCEEDED(device->CreateDepthStencilView(baselineDepth.Get(),&dsvDesc,&baselineDsv)) &&
              SUCCEEDED(device->CreateDepthStencilView(liveDepth.Get(),&dsvDesc,&liveDsv)),
              "typed depth-stencil views create");
    if(!baselineRtv || !liveRtv || !baselineDsv || !liveDsv || !cohortDepth)return failures;

    D3D11_SHADER_RESOURCE_VIEW_DESC depthViewDesc{};
    depthViewDesc.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    depthViewDesc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
    depthViewDesc.Texture2D.MipLevels=1;
    ComPtr<ID3D11ShaderResourceView> cohortView,liveDepthView,liveStencilView;
    check(SUCCEEDED(device->CreateShaderResourceView(cohortDepth.Get(),&depthViewDesc,&cohortView)) &&
          SUCCEEDED(device->CreateShaderResourceView(liveDepth.Get(),&depthViewDesc,&liveDepthView)),
          "snapshot and consumer depth planes create");
    depthViewDesc.Format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    check(SUCCEEDED(device->CreateShaderResourceView(liveDepth.Get(),&depthViewDesc,&liveStencilView)),
          "consumer stencil plane creates");
    if(!cohortView || !liveDepthView || !liveStencilView)return failures;

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable=TRUE;
    blendDesc.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;
    check(SUCCEEDED(device->CreateBlendState(&blendDesc,&blend)),"zero-alpha game blend state");
    D3D11_DEPTH_STENCIL_DESC depthStateDesc{};
    depthStateDesc.DepthEnable=TRUE;
    depthStateDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    depthStateDesc.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    ComPtr<ID3D11DepthStencilState> depthState;
    check(SUCCEEDED(device->CreateDepthStencilState(&depthStateDesc,&depthState)),
          "game depth state");
    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode=D3D11_FILL_SOLID;rasterDesc.CullMode=D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable=TRUE;rasterDesc.ScissorEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    check(SUCCEEDED(device->CreateRasterizerState(&rasterDesc,&raster)),
          "game rasterizer for both original draws");
    if(!blend || !depthState || !raster)return failures;
    const FLOAT clear[4]={.25f,.5f,.75f,1};
    context->ClearRenderTargetView(baselineRtv.Get(),clear);
    context->ClearRenderTargetView(liveRtv.Get(),clear);
    context->ClearDepthStencilView(baselineDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8f,0);
    context->ClearDepthStencilView(liveDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8f,0);
    D3D11_VIEWPORT viewport{};viewport.Width=float(w);viewport.Height=float(h);viewport.MaxDepth=1;
    D3D11_RECT full{0,0,LONG(w),LONG(h)};
    auto bind=[&](ID3D11RenderTargetView* rtv,ID3D11DepthStencilView* dsv) {
        context->OMSetRenderTargets(1,&rtv,dsv);
        context->OMSetBlendState(blend.Get(),nullptr,~0u);
        context->OMSetDepthStencilState(depthState.Get(),0);
        context->RSSetState(raster.Get());
        context->RSSetViewports(1,&viewport);
        context->RSSetScissorRects(1,&full);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(),nullptr,0);
        context->PSSetShader(ps.Get(),nullptr,0);
    };
    auto readTex=[&](ID3D11Texture2D* source,UINT pixelBytes) {
        D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
        d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
        ComPtr<ID3D11Texture2D> staging;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&staging)))return std::vector<BYTE>{};
        context->CopyResource(staging.Get(),source);
        D3D11_MAPPED_SUBRESOURCE map{};
        if(FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)))return std::vector<BYTE>{};
        std::vector<BYTE> out(size_t(w)*h*pixelBytes);
        for(UINT y=0;y<h;++y)
            std::memcpy(out.data()+size_t(y)*w*pixelBytes,
                        static_cast<const BYTE*>(map.pData)+size_t(y)*map.RowPitch,w*pixelBytes);
        context->Unmap(staging.Get(),0);
        return out;
    };
    bind(baselineRtv.Get(),baselineDsv.Get());
    context->Draw(3,0);
    bind(liveRtv.Get(),liveDsv.Get());
    edvr::FlatOverlayLayer defaultPolicy;
    defaultPolicy.beginFrame(1);
    const char* reason=nullptr;
    check(!defaultPolicy.beginDraw(context,1,liveColor.Get(),liveDsv.Get(),&reason) &&
          reason && std::strstr(reason,"unsupported-HDR-format"),
          "typed-HDR policy refuses format-23 source unless diagnostic opt-in is explicit");
    edvr::FlatOverlayLayer occupied;
    occupied.beginFrame(1);
    ID3D11RenderTargetView* occupiedTargets[8]{};
    occupiedTargets[0]=liveRtv.Get();occupiedTargets[7]=baselineRtv.Get();
    context->OMSetRenderTargets(8,occupiedTargets,liveDsv.Get());
    check(!occupied.beginDraw(context,1,liveColor.Get(),liveDsv.Get(),&reason,true) &&
          reason && std::strstr(reason,"MRT7"),
          "diagnostic refuses occupied MRT7 before the original draw");
    bind(liveRtv.Get(),liveDsv.Get());
    edvr::FlatOverlayLayer probe;
    probe.beginFrame(1);
    const bool began=probe.beginDraw(context,1,liveColor.Get(),liveDsv.Get(),&reason,true,true);
    check(began,"diagnostic admits format-23 pool with its format-24 RTV");
    if(began) { context->Draw(3,0); probe.endDraw(context); }
    check(probe.coverageReady(1,liveColor.Get()) && !probe.ready(1,liveColor.Get()) &&
          !probe.cleanHdr() && !probe.cleanHdrView() && probe.markedDraws()==1,
          "coverage-only diagnostic exports the original draw without allocating a clean pool copy");
    ComPtr<ID3D11PixelShader> afterPs;context->PSGetShader(&afterPs,nullptr,nullptr);
    ComPtr<ID3D11BlendState> afterBlend;FLOAT factors[4]{};UINT sampleMask=0;
    context->OMGetBlendState(&afterBlend,factors,&sampleMask);
    ID3D11RenderTargetView* afterRtv=nullptr;ID3D11DepthStencilView* afterDsv=nullptr;
    context->OMGetRenderTargets(1,&afterRtv,&afterDsv);
    check(afterPs.Get()==ps.Get() && afterBlend.Get()==blend.Get() &&
          afterRtv==liveRtv.Get() && afterDsv==liveDsv.Get() && sampleMask==~0u,
          "original PS and OM state restored after diagnostic draw");
    if(afterRtv)afterRtv->Release();if(afterDsv)afterDsv->Release();
    const auto colorA=readTex(baselineColor.Get(),4),colorB=readTex(liveColor.Get(),4);
    const auto depthA=readTex(baselineDepth.Get(),8),depthB=readTex(liveDepth.Get(),8);
    check(!colorA.empty() && colorA==colorB && !depthA.empty() && depthA==depthB,
          "diagnostic retains original zero-alpha color, depth, and stencil bytes");
    context->OMSetRenderTargets(0,nullptr,nullptr);
    context->CopyResource(cohortDepth.Get(),liveDepth.Get());

    struct Extent {UINT width,height,pad0,pad1;};
    const Extent extent{w,h,0,0};
    D3D11_BUFFER_DESC cbDesc{};cbDesc.ByteWidth=sizeof(extent);cbDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cbInit{};cbInit.pSysMem=&extent;
    ComPtr<ID3D11Buffer> extentCb;
    check(SUCCEEDED(device->CreateBuffer(&cbDesc,&cbInit,&extentCb)),"odd extent constant buffer");
    D3D11_TEXTURE2D_DESC ownerDesc{};
    ownerDesc.Width=w;ownerDesc.Height=h;
    ownerDesc.MipLevels=ownerDesc.ArraySize=ownerDesc.SampleDesc.Count=1;
    ownerDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    std::vector<BYTE> emptyMask(size_t(w)*h,0);
    std::vector<float> emptyDepth(size_t(w)*h,1.0f);
    D3D11_SUBRESOURCE_DATA maskInit{};maskInit.pSysMem=emptyMask.data();maskInit.SysMemPitch=w;
    D3D11_SUBRESOURCE_DATA ownerInit{};ownerInit.pSysMem=emptyDepth.data();ownerInit.SysMemPitch=w*sizeof(float);
    ComPtr<ID3D11Texture2D> unionMask,ownerDepth;
    ownerDesc.Format=DXGI_FORMAT_R8_UNORM;
    const bool maskMade=SUCCEEDED(device->CreateTexture2D(&ownerDesc,&maskInit,&unionMask));
    ownerDesc.Format=DXGI_FORMAT_R32_FLOAT;
    const bool depthMade=SUCCEEDED(device->CreateTexture2D(&ownerDesc,&ownerInit,&ownerDepth));
    check(maskMade && depthMade,"persistent union and last-foreground owner depth create");
    ComPtr<ID3D11ShaderResourceView> unionView,ownerView;
    ComPtr<ID3D11UnorderedAccessView> unionUav,ownerUav;
    if(unionMask && ownerDepth)
        check(SUCCEEDED(device->CreateShaderResourceView(unionMask.Get(),nullptr,&unionView)) &&
              SUCCEEDED(device->CreateShaderResourceView(ownerDepth.Get(),nullptr,&ownerView)) &&
              SUCCEEDED(device->CreateUnorderedAccessView(unionMask.Get(),nullptr,&unionUav)) &&
              SUCCEEDED(device->CreateUnorderedAccessView(ownerDepth.Get(),nullptr,&ownerUav)),
              "persistent typed SRVs and UAVs create");
    if(!extentCb || !unionView || !ownerView || !unionUav || !ownerUav)return failures;
    auto merge=[&](ID3D11ShaderResourceView* currentMask) {
        ID3D11ShaderResourceView* inputs[2]={currentMask,cohortView.Get()};
        ID3D11UnorderedAccessView* outputs[2]={unionUav.Get(),ownerUav.Get()};
        ID3D11Buffer* params=extentCb.Get();
        context->OMSetRenderTargets(0,nullptr,nullptr);
        context->CSSetShader(mergeCs.Get(),nullptr,0);
        context->CSSetShaderResources(0,2,inputs);
        context->CSSetUnorderedAccessViews(0,2,outputs,nullptr);
        context->CSSetConstantBuffers(0,1,&params);
        context->Dispatch((w+7)/8,(h+7)/8,1);
        ID3D11ShaderResourceView* noInputs[2]{};
        ID3D11UnorderedAccessView* noOutputs[2]{};
        ID3D11Buffer* noParams=nullptr;
        context->CSSetShaderResources(0,2,noInputs);
        context->CSSetUnorderedAccessViews(0,2,noOutputs,nullptr);
        context->CSSetConstantBuffers(0,1,&noParams);
        context->CSSetShader(nullptr,nullptr,0);
    };
    merge(probe.coverageView());
    auto count=[&]() {
        edvr::FlatForegroundOwnershipCounts result{};
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth=sizeof(result);bd.StructureByteStride=sizeof(uint32_t);
        bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        D3D11_SUBRESOURCE_DATA init{};init.pSysMem=&result;
        ComPtr<ID3D11Buffer> counters;
        ComPtr<ID3D11UnorderedAccessView> uav;
        ComPtr<ID3D11Buffer> staging;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=DXGI_FORMAT_UNKNOWN;
        uv.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;uv.Buffer.NumElements=edvr::kFlatForegroundOwnershipCounterCount;
        bool good=SUCCEEDED(device->CreateBuffer(&bd,&init,&counters)) &&
                  SUCCEEDED(device->CreateUnorderedAccessView(counters.Get(),&uv,&uav));
        bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        bd.MiscFlags=0;bd.StructureByteStride=0;
        good=good && SUCCEEDED(device->CreateBuffer(&bd,nullptr,&staging));
        check(good,"eight-counter UAV and readback create");
        if(!good)return result;
        ID3D11ShaderResourceView* inputs[4]={unionView.Get(),ownerView.Get(),
                                             liveDepthView.Get(),liveStencilView.Get()};
        ID3D11UnorderedAccessView* output=uav.Get();
        ID3D11Buffer* params=extentCb.Get();
        context->OMSetRenderTargets(0,nullptr,nullptr);
        context->CSSetShader(cs.Get(),nullptr,0);
        context->CSSetShaderResources(0,4,inputs);
        context->CSSetUnorderedAccessViews(0,1,&output,nullptr);
        context->CSSetConstantBuffers(0,1,&params);
        context->Dispatch((w+7)/8,(h+7)/8,1);
        ID3D11ShaderResourceView* noInputs[4]{};
        ID3D11UnorderedAccessView* noOutput=nullptr;
        ID3D11Buffer* noParams=nullptr;
        context->CSSetShaderResources(0,4,noInputs);
        context->CSSetUnorderedAccessViews(0,1,&noOutput,nullptr);
        context->CSSetConstantBuffers(0,1,&noParams);
        context->CSSetShader(nullptr,nullptr,0);
        context->CopyResource(staging.Get(),counters.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        if(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map))) {
            std::memcpy(&result,map.pData,sizeof(result));context->Unmap(staging.Get(),0);
        } else check(false,"ownership counts read back");
        return result;
    };
    auto counts=count();
    check(counts.total==w*h && counts.covered==90 && counts.survivingExactDepth==90 &&
          counts.survivingMarked16==0 && counts.survivingUnmarked==90 &&
          counts.overwritten==0 && counts.markedWithoutSurvivingCoverage==0 && counts.totalStencil16==0,
          "discard and zero-alpha leave exact-depth coverage but no first-person stencil");
    context->ClearDepthStencilView(liveDsv.Get(),D3D11_CLEAR_STENCIL,.8f,16);
    counts=count();
    check(counts.total==w*h && counts.covered==90 && counts.survivingExactDepth==90 &&
          counts.survivingMarked16==90 && counts.survivingUnmarked==0 && counts.overwritten==0 &&
          counts.markedWithoutSurvivingCoverage==27 && counts.totalStencil16==w*h,
          "unowned stencil marks outside coverage remain visible in the counter ABI");
    context->ClearDepthStencilView(liveDsv.Get(),D3D11_CLEAR_DEPTH,.2f,16);
    counts=count();
    check(counts.total==w*h && counts.covered==90 && counts.survivingExactDepth==0 &&
          counts.survivingMarked16==0 && counts.survivingUnmarked==0 && counts.overwritten==90 &&
          counts.markedWithoutSurvivingCoverage==w*h && counts.totalStencil16==w*h,
          "later world depth overwrite does not turn stale stencil into surviving ownership");

    // A world pass overwrites the shared depth between two foreground draws.
    // The second original draw covers a 5x5 square: 15 new pixels, 10 that
    // overlap the first draw. Its immediate depth copy must replace only those
    // 25 owner depths; the first draw's other 80 owner depths must survive.
    context->ClearDepthStencilView(baselineDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.6f,16);
    context->ClearDepthStencilView(liveDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.6f,16);
    const D3D11_RECT secondRect{0,0,5,5};
    bind(baselineRtv.Get(),baselineDsv.Get());
    context->VSSetShader(secondVs.Get(),nullptr,0);
    context->PSSetShader(secondPs.Get(),nullptr,0);
    context->RSSetScissorRects(1,&secondRect);
    context->Draw(3,0);
    bind(liveRtv.Get(),liveDsv.Get());
    context->VSSetShader(secondVs.Get(),nullptr,0);
    context->PSSetShader(secondPs.Get(),nullptr,0);
    context->RSSetScissorRects(1,&secondRect);
    const bool secondBegan=probe.beginDraw(context,1,liveColor.Get(),liveDsv.Get(),&reason,true,true);
    check(secondBegan,"same-frame second original foreground draw reuses private MRT7");
    if(secondBegan){context->Draw(3,0);probe.endDraw(context);}
    check(probe.coverageReady(1,liveColor.Get()) && !probe.ready(1,liveColor.Get()) &&
          !probe.cleanHdr() && !probe.cleanHdrView() && probe.markedDraws()==2,
          "same coverage-only object clears its mask for the second draw without a clean pool copy");
    check(readTex(baselineColor.Get(),4)==readTex(liveColor.Get(),4) &&
          readTex(baselineDepth.Get(),8)==readTex(liveDepth.Get(),8),
          "second diagnostic draw preserves the original color, depth, and stencil bytes");
    context->OMSetRenderTargets(0,nullptr,nullptr);
    context->CopyResource(cohortDepth.Get(),liveDepth.Get());
    merge(probe.coverageView());
    ComPtr<ID3D11Resource> secondMaskResource;
    if(probe.coverageView())probe.coverageView()->GetResource(&secondMaskResource);
    ComPtr<ID3D11Texture2D> secondMask;
    if(secondMaskResource)secondMaskResource.As(&secondMask);
    const auto secondMaskBytes=secondMask?readTex(secondMask.Get(),1):std::vector<BYTE>{};
    UINT secondCovered=0;for(BYTE v:secondMaskBytes)secondCovered+=v==255;
    check(secondMaskBytes.size()==w*h && secondCovered==25,
          "the second mask is actual raster coverage, including both overlap and new pixels");
    const auto unionBytes=readTex(unionMask.Get(),1), ownerBytes=readTex(ownerDepth.Get(),4);
    UINT unionCovered=0;for(BYTE v:unionBytes)unionCovered+=v==255;
    auto ownerAt=[&](UINT x,UINT y) {
        float value=0;
        if(ownerBytes.size()==size_t(w)*h*4)
            std::memcpy(&value,ownerBytes.data()+(size_t(y)*w+x)*4,4);
        return value;
    };
    check(unionBytes.size()==w*h && unionCovered==105 &&
          unionBytes[w+1]==255 && unionBytes[w+4]==255 && unionBytes[8*w+8]==255 && unionBytes[8*w+1]==0 &&
          ownerAt(1,1)==.4f && ownerAt(4,1)==.4f && ownerAt(8,8)==.5f,
          "per-pixel merge retains old owner depth and replaces overlap with the last foreground draw");
    counts=count();
    check(counts.total==w*h && counts.covered==105 && counts.survivingExactDepth==25 &&
          counts.survivingMarked16==25 && counts.survivingUnmarked==0 && counts.overwritten==80 &&
          counts.markedWithoutSurvivingCoverage==92 && counts.totalStencil16==w*h,
          "counter shader sees second owner survive and first owner overwritten after interleaved world writes");
    context->ClearDepthStencilView(liveDsv.Get(),D3D11_CLEAR_STENCIL,.6f,0);
    counts=count();
    check(counts.covered==105 && counts.survivingExactDepth==25 &&
          counts.survivingMarked16==0 && counts.survivingUnmarked==25 &&
          counts.overwritten==80 && counts.totalStencil16==0,
          "missing current stencil is distinct from missing foreground coverage or depth overwrite");

    // The production conservative union brackets the original game draws.
    // A world draw later passes at exactly the first foreground's encoded Z;
    // that cannot erase the camera ambiguity, even though depth is equal.
    {
        float worldRows[6][4]{};camera(worldRows);
        float alternateRows[6][4]{};std::memcpy(alternateRows,worldRows,sizeof(worldRows));
        alternateRows[3][2]=.0675f;
        const auto* alternateBytes=reinterpret_cast<const unsigned char*>(alternateRows);
        const auto* worldBytes=reinterpret_cast<const unsigned char*>(worldRows);
        edvr::FlatUntrustedCoverage unionCapture;
        unionCapture.beginFrame(42);
        auto nomination=[&](uint32_t seq,uint64_t vsHash,uint64_t psHash,
                            const unsigned char* cameraBytes,bool supported,
                            const void* namedDepth=nullptr,bool namesWorldSource=false) {
            edvr::FlatRuntimeDraw draw{};
            auto& k=draw.key;
            k.color=liveColor.Get();k.depth=liveDepth.Get();k.dsv=liveDsv.Get();
            k.format=23;k.width=w;k.height=h;k.vs=vsHash;k.ps=psHash;
            k.b1=liveColor.Get();k.viewportCount=1;
            k.viewport[2]=float(w);k.viewport[3]=float(h);k.viewport[5]=1;
            k.writeEpoch=42;k.writeSeq=1;
            std::memcpy(draw.camera,cameraBytes,sizeof(draw.camera));
            k.camera=draw.camera;k.cameraHash=edvr::flatCameraHash(draw.camera);
            draw.supported=supported;
            return edvr::flatUntrustedNomination(draw,namedDepth,worldBytes,true,true,seq,42,
                                                  namesWorldSource);
        };
        const auto cfca=nomination(10,0xCFCA8FFC6B058630ull,0x8A08FF781272C5F6ull,
                                   alternateBytes,false);
        const auto sevenB=nomination(12,0x7B0DC42D383F694Cull,0x0DF03E64DF9DBEF1ull,
                                     alternateBytes,false);
        const auto eightB=nomination(13,0x8B589D25B2A0ADDCull,0x7268762D11A610F2ull,
                                     alternateBytes,false);
        const auto aacf=nomination(14,0xAACFDCF2FB9AD809ull,0xCF534B32F491561Aull,
                                   alternateBytes,true);
        const auto worldNomination=nomination(11,0xEB5234DB6ADB491Dull,0x22,
                                              worldBytes,true);
        check(cfca.admissible() && sevenB.admissible() && eightB.admissible() &&
              aacf.admissible() && worldNomination.admissible() &&
              cfca.preWorld && sevenB.preWorld && eightB.preWorld &&
              !nomination(15,0xCFCA8FFC6B058630ull,0x8A08FF781272C5F6ull,
                          worldBytes,false,liveDepth.Get()).candidate,
              "the constructor's shared nomination admits unsupported pre-world geometry and a world-camera bucket without admitting a post-name world draw");
        check(!nomination(11,0xEB5234DB6ADB491Dull,0x22,worldBytes,true,
                          nullptr,true).candidate,
              "the first supported world source is named and observed without competing with its MRT6 substitution");
        edvr::FlatShaderPairClassification inertPair{};
        inertPair.vs=edvr::FlatVsProjectionClass::InertNoCB;
        inertPair.ps=edvr::FlatPsProjectionSafety::Clean;
        check(edvr::flatUntrustedProvenCameraIndependent(inertPair),
              "proven camera-independent scene shaders need no alternate-camera capture");
        inertPair.vs=edvr::FlatVsProjectionClass::Unclassified;
        check(!edvr::flatUntrustedProvenCameraIndependent(inertPair),
              "unknown projection shaders remain capture candidates");
        context->ClearRenderTargetView(baselineRtv.Get(),clear);
        context->ClearRenderTargetView(liveRtv.Get(),clear);
        context->ClearDepthStencilView(baselineDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8f,0);
        context->ClearDepthStencilView(liveDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8f,0);
        bind(baselineRtv.Get(),baselineDsv.Get());context->Draw(3,0);
        bind(liveRtv.Get(),liveDsv.Get());
        const bool firstPlanned=cfca.admissible() &&
            unionCapture.plan(42,10,liveColor.Get(),liveDepth.Get(),liveDsv.Get(),
                              0xCFCA8FFC6B058630ull,0x8A08FF781272C5F6ull,
                              alternateBytes,cfca.admissible());
        const bool firstBegan=firstPlanned && unionCapture.beginDraw(context,42);
        if(firstBegan){context->Draw(3,0);unionCapture.endDraw(context);}
        const bool firstSelected=unionCapture.select(liveDepth.Get(),liveDsv.Get(),worldBytes,0,0);
        auto capturedMask=[&]() {
            ComPtr<ID3D11Resource> resource;
            if(unionCapture.view())unionCapture.view()->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;if(resource)resource.As(&texture);
            return texture?readTex(texture.Get(),1):std::vector<BYTE>{};
        };
        const auto firstMask=capturedMask(),firstDepth=readTex(liveDepth.Get(),8);
        UINT firstMarked=0;for(BYTE v:firstMask)firstMarked+=v==255;
        check(firstBegan && firstSelected && firstMarked==90 && firstMask.size()==w*h,
              "production union marks only original-PS fragments passing discard and depth");

        // The world draw uses the same z=.5 vertex shader and LESS_EQUAL
        // depth state. No blend proves it really rasterized into both targets.
        bind(baselineRtv.Get(),baselineDsv.Get());
        context->PSSetShader(secondPs.Get(),nullptr,0);
        context->OMSetBlendState(nullptr,nullptr,~0u);context->Draw(3,0);
        bind(liveRtv.Get(),liveDsv.Get());
        context->PSSetShader(secondPs.Get(),nullptr,0);
        context->OMSetBlendState(nullptr,nullptr,~0u);
        const bool worldPlanned=worldNomination.admissible() &&
            unionCapture.plan(42,11,liveColor.Get(),liveDepth.Get(),liveDsv.Get(),
                              0xEB5234DB6ADB491Dull,0x22,worldBytes,
                              worldNomination.admissible());
        const bool worldBegan=worldPlanned && unionCapture.beginDraw(context,42);
        if(worldBegan){context->Draw(3,0);unionCapture.endDraw(context);}
        const auto afterWorldMask=capturedMask(),afterWorldDepth=readTex(liveDepth.Get(),8);
        const auto afterWorldColor=readTex(liveColor.Get(),4);
        UINT retained=0;for(BYTE v:afterWorldMask)retained+=v==255;
        const size_t ownedPixel=(size_t(4)*w+4)*8;
        check(worldBegan && retained==90 && afterWorldMask==firstMask &&
              firstDepth.size()==size_t(w)*h*8 && afterWorldDepth.size()==firstDepth.size() &&
              std::memcmp(firstDepth.data()+ownedPixel,afterWorldDepth.data()+ownedPixel,8)==0 &&
              afterWorldColor.size()==size_t(w)*h*4 && afterWorldColor[(size_t(4)*w+4)*4]!=0,
              "equal-depth world overdraw rasterizes yet conservatively retains the first camera mark");

        bind(baselineRtv.Get(),baselineDsv.Get());
        context->VSSetShader(secondVs.Get(),nullptr,0);
        context->PSSetShader(secondPs.Get(),nullptr,0);
        context->RSSetScissorRects(1,&secondRect);context->Draw(3,0);
        bind(liveRtv.Get(),liveDsv.Get());
        context->VSSetShader(secondVs.Get(),nullptr,0);
        context->PSSetShader(secondPs.Get(),nullptr,0);
        context->RSSetScissorRects(1,&secondRect);
        const bool nextPlanned=sevenB.admissible() &&
            unionCapture.plan(42,12,liveColor.Get(),liveDepth.Get(),liveDsv.Get(),
                              0x7B0DC42D383F694Cull,0x0DF03E64DF9DBEF1ull,
                              alternateBytes,sevenB.admissible());
        const bool nextBegan=nextPlanned && unionCapture.beginDraw(context,42);
        if(nextBegan){context->Draw(3,0);unionCapture.endDraw(context);}
        bind(baselineRtv.Get(),baselineDsv.Get());
        context->VSSetShader(secondVs.Get(),nullptr,0);
        context->PSSetShader(secondPs.Get(),nullptr,0);
        context->RSSetScissorRects(1,&secondRect);context->Draw(3,0);
        bind(liveRtv.Get(),liveDsv.Get());
        context->VSSetShader(secondVs.Get(),nullptr,0);
        context->PSSetShader(secondPs.Get(),nullptr,0);
        context->RSSetScissorRects(1,&secondRect);
        const bool eighthPlanned=eightB.admissible() &&
            unionCapture.plan(42,13,liveColor.Get(),liveDepth.Get(),liveDsv.Get(),
                              0x8B589D25B2A0ADDCull,0x7268762D11A610F2ull,
                              alternateBytes,eightB.admissible());
        const bool eighthBegan=eighthPlanned && unionCapture.beginDraw(context,42);
        if(eighthBegan){context->Draw(3,0);unionCapture.endDraw(context);}
        bind(baselineRtv.Get(),baselineDsv.Get());
        context->VSSetShader(secondVs.Get(),nullptr,0);
        context->PSSetShader(secondPs.Get(),nullptr,0);
        context->RSSetScissorRects(1,&secondRect);context->Draw(3,0);
        bind(liveRtv.Get(),liveDsv.Get());
        context->VSSetShader(secondVs.Get(),nullptr,0);
        context->PSSetShader(secondPs.Get(),nullptr,0);
        context->RSSetScissorRects(1,&secondRect);
        const bool aacfPlanned=aacf.admissible() &&
            unionCapture.plan(42,14,liveColor.Get(),liveDepth.Get(),liveDsv.Get(),
                              0xAACFDCF2FB9AD809ull,0xCF534B32F491561Aull,
                              alternateBytes,aacf.admissible());
        const bool aacfBegan=aacfPlanned && unionCapture.beginDraw(context,42);
        if(aacfBegan){context->Draw(3,0);unionCapture.endDraw(context);}
        const bool selected=unionCapture.select(liveDepth.Get(),liveDsv.Get(),worldBytes,0,0);
        const auto allMask=capturedMask();UINT allMarked=0;for(BYTE v:allMask)allMarked+=v==255;
        check(nextBegan && eighthBegan && aacfBegan && selected && unionCapture.mixed() &&
              unionCapture.completedDraws(liveDepth.Get(),alternateBytes)==4 &&
              unionCapture.completedDraws(liveDepth.Get(),worldBytes)==1 &&
              allMask.size()==w*h && allMarked==105 &&
              allMask[8*w+8]==255 && allMask[w+1]==255,
              "production union selects all four alternate-camera fragments and excludes the pre-world world-camera bucket");
        check(readTex(baselineColor.Get(),4)==readTex(liveColor.Get(),4) &&
              readTex(baselineDepth.Get(),8)==readTex(liveDepth.Get(),8),
              "production union preserves original color, depth, and stencil bytes across both draws");

        edvr::FlatContractRecord represented{};
        represented.key.color=liveColor.Get();represented.key.depth=liveDepth.Get();
        represented.key.dsv=liveDsv.Get();represented.key.width=w;represented.key.height=h;
        represented.key.vs=0xCFCA8FFC6B058630ull;
        represented.key.ps=0x8A08FF781272C5F6ull;
        std::memcpy(represented.camera,alternateBytes,sizeof(represented.camera));
        represented.key.camera=represented.camera;
        represented.first=represented.last=10;represented.draws=1;
        check(unionCapture.qualifies(represented,worldBytes,0,0),
              "production selector can certify the exact completed alternate-camera draw");
        unsigned char recycledProducerCamera[edvr::kFlatCameraBytes]{};
        std::memcpy(recycledProducerCamera,alternateBytes,sizeof(recycledProducerCamera));
        represented.key.camera=recycledProducerCamera;
        std::memcpy(recycledProducerCamera,worldBytes,sizeof(recycledProducerCamera));
        check(unionCapture.qualifies(represented,worldBytes,0,0),
              "qualification uses the record's frozen camera bytes after the producer buffer changes");
        represented.draws=2;
        check(!unionCapture.qualifies(represented,worldBytes,0,0),
              "selector refuses a coalesced record with an uncaptured draw");
        represented.draws=1;
        check(unionCapture.completedDraws(liveDepth.Get(),alternateBytes)!=5,
              "an observed but unbracketed unsupported variant cannot match completed receipts");
        unionCapture.noteMutation(nullptr);
        check(!unionCapture.view() && !unionCapture.qualifies(represented,worldBytes,0,0),
              "unknown writer invalidates the union and closes qualification");

        edvr::FlatUntrustedCoverage capped;capped.beginFrame(43);
        bool within=true;
        for(uint32_t i=0;i<128;++i)
            within &= capped.plan(43,i+1,liveColor.Get(),liveDepth.Get(),liveDsv.Get(),
                                  0x11,0x22,alternateBytes,true);
        const bool over=capped.plan(43,129,liveColor.Get(),liveDepth.Get(),liveDsv.Get(),
                                    0x11,0x22,alternateBytes,true);
        check(within && !over && capped.failure() && !capped.view(),
              "production union record cap fails closed instead of dropping a draw");

        // A frame with no supported AACF record can still be mixed: an
        // unsupported pre-world draw was captured, and H later names the
        // other camera. A failed H-camera bucket cannot contaminate its mask.
        edvr::FlatUntrustedCoverage unsupportedOnly;
        unsupportedOnly.beginFrame(44);
        bind(liveRtv.Get(),liveDsv.Get());
        const bool unknownPlanned=unsupportedOnly.plan(
            44,20,liveColor.Get(),liveDepth.Get(),liveDsv.Get(),
            0x7B0DC42D383F694Cull,0x0DF03E64DF9DBEF1ull,
            alternateBytes,sevenB.admissible());
        const bool unknownBegan=unknownPlanned && unsupportedOnly.beginDraw(context,44);
        if(unknownBegan){context->Draw(3,0);unsupportedOnly.endDraw(context);}
        const bool failedWorldPlan=unsupportedOnly.plan(
            44,21,liveColor.Get(),liveDepth.Get(),liveDsv.Get(),
            0xEB5234DB6ADB491Dull,0x22,worldBytes,false);
        const bool unknownSelected=unsupportedOnly.select(liveDepth.Get(),liveDsv.Get(),
                                                            worldBytes,0,0);
        check(unknownBegan && !failedWorldPlan && unknownSelected &&
              unsupportedOnly.mixed() && unsupportedOnly.view() &&
              unsupportedOnly.completedDraws(liveDepth.Get(),alternateBytes)==1,
              "unsupported-only alternate selects its passing-fragment mask despite a failed world-camera bucket");
        float thirdRows[6][4]{};
        std::memcpy(thirdRows,alternateRows,sizeof(thirdRows));
        thirdRows[3][2]=.09f;
        const auto* thirdBytes=reinterpret_cast<const unsigned char*>(thirdRows);
        const bool thirdPlanned=unsupportedOnly.plan(
            44,22,liveColor.Get(),liveDepth.Get(),liveDsv.Get(),
            0x8B589D25B2A0ADDCull,0x7268762D11A610F2ull,
            thirdBytes,true);
        check(!thirdPlanned && unsupportedOnly.failure() && !unsupportedOnly.view(),
              "a third camera bucket exceeds the bounded mask budget and refuses treatment");

        // The v40 ordering starts with an H-camera draw whose original PS is
        // null. It must fail only its own bucket; later unsupported material
        // draws and AACF must still form a complete alternate mask. The second
        // case starts with a real shader that has non-colour PS output, so MRT7
        // patching itself fails before another null-PS world draw.
        struct Submitted { bool observed=false, nominee=false, planned=false, began=false; };
        auto submit=[&](edvr::FlatUntrustedCoverage& capture,
                        edvr::FlatUntrustedObservedCamera* observed,uint32_t& used,
                        uint64_t frame,uint32_t seq,const unsigned char* cameraBytes,
                        uint64_t vsHash,uint64_t psHash,ID3D11VertexShader* gameVs,
                        ID3D11PixelShader* gamePs,const D3D11_RECT& scissor,
                        bool supported,bool bracket=true) {
            bind(baselineRtv.Get(),baselineDsv.Get());
            context->VSSetShader(gameVs,nullptr,0);
            context->PSSetShader(gamePs,nullptr,0);
            context->RSSetScissorRects(1,&scissor);
            context->Draw(3,0);
            bind(liveRtv.Get(),liveDsv.Get());
            context->VSSetShader(gameVs,nullptr,0);
            context->PSSetShader(gamePs,nullptr,0);
            context->RSSetScissorRects(1,&scissor);
            const bool seen=edvr::flatUntrustedObserveCamera(
                observed,4,used,liveDepth.Get(),cameraBytes)!=nullptr;
            edvr::FlatRuntimeDraw draw{};
            auto& key=draw.key;
            key.color=liveColor.Get();key.depth=liveDepth.Get();key.dsv=liveDsv.Get();
            key.format=23;key.width=w;key.height=h;key.vs=vsHash;key.ps=psHash;
            key.b1=liveColor.Get();key.viewportCount=1;
            key.viewport[2]=float(w);key.viewport[3]=float(h);key.viewport[5]=1;
            key.writeEpoch=frame;key.writeSeq=1;
            std::memcpy(draw.camera,cameraBytes,sizeof(draw.camera));
            key.camera=draw.camera;key.cameraHash=edvr::flatCameraHash(draw.camera);
            draw.supported=supported;
            const auto nominee=edvr::flatUntrustedNomination(
                draw,nullptr,nullptr,true,true,seq,frame);
            edvr::FlatUntrustedDrawDiagnostic provenance{};
            provenance.cameraHash=key.cameraHash;
            provenance.writeEpoch=key.writeEpoch;
            provenance.writeSeq=key.writeSeq;
            provenance.current=nominee.current;
            provenance.viewport=nominee.fullViewport;
            const bool planned=bracket && nominee.admissible() &&
                capture.plan(frame,seq,liveColor.Get(),liveDepth.Get(),liveDsv.Get(),
                             vsHash,psHash,cameraBytes,nominee.admissible(),&provenance);
            const bool began=planned && capture.beginDraw(context,frame,
                [](void* shader)->uint64_t {
                    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(shader));
                });
            if(planned && !began)capture.abandon();
            context->Draw(3,0);
            if(began)capture.endDraw(context);
            return Submitted{seen,nominee.admissible(),planned,began};
        };
        auto clearPair=[&]() {
            context->ClearRenderTargetView(baselineRtv.Get(),clear);
            context->ClearRenderTargetView(liveRtv.Get(),clear);
            context->ClearDepthStencilView(baselineDsv.Get(),
                D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8f,0);
            context->ClearDepthStencilView(liveDsv.Get(),
                D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.8f,0);
        };
        auto sameOriginal=[&]() {
            const auto baseColor=readTex(baselineColor.Get(),4);
            const auto seenColor=readTex(liveColor.Get(),4);
            const auto baseDepth=readTex(baselineDepth.Get(),8);
            const auto seenDepth=readTex(liveDepth.Get(),8);
            return baseColor.size()==size_t(w)*h*4 &&
                seenColor.size()==baseColor.size() &&
                baseDepth.size()==size_t(w)*h*8 &&
                seenDepth.size()==baseDepth.size() &&
                baseColor==seenColor && baseDepth==seenDepth;
        };
        for(uint32_t scenario=0;scenario<2;++scenario) {
            const uint64_t frame=50+scenario;
            edvr::FlatUntrustedCoverage capture;capture.beginFrame(frame);
            edvr::FlatUntrustedObservedCamera observed[4]{};
            uint32_t used=0;
            clearPair();
            const auto firstWorld=submit(capture,observed,used,frame,1,worldBytes,
                0xEB5234DB6ADB491Dull,scenario?0xABCDEF01ull:0ull,
                vs.Get(),scenario?explicitDepthPs.Get():nullptr,full,false);
            const char* firstFailure=capture.failure();
            const bool worldFailedAsExpected=firstFailure &&
                std::strstr(firstFailure,scenario?"non-colour PS output":"shader-identity");
            Submitted secondWorld{};
            if(scenario)secondWorld=submit(capture,observed,used,frame,2,worldBytes,
                0xEB5234DB6ADB491Dull,0,vs.Get(),nullptr,full,false);
            const auto cfcaDraw=submit(capture,observed,used,frame,3,alternateBytes,
                0xCFCA8FFC6B058630ull,0x8A08FF781272C5F6ull,
                vs.Get(),ps.Get(),full,false);
            const auto sevenDraw=submit(capture,observed,used,frame,4,alternateBytes,
                0x7B0DC42D383F694Cull,0x0DF03E64DF9DBEF1ull,
                secondVs.Get(),secondPs.Get(),secondRect,false);
            const auto eightDraw=submit(capture,observed,used,frame,5,alternateBytes,
                0x8B589D25B2A0ADDCull,0x7268762D11A610F2ull,
                secondVs.Get(),secondPs.Get(),secondRect,false);
            const auto aacfDraw=submit(capture,observed,used,frame,6,alternateBytes,
                0xAACFDCF2FB9AD809ull,0xCF534B32F491561Aull,
                secondVs.Get(),secondPs.Get(),secondRect,true);
            const bool accepted=capture.select(liveDepth.Get(),liveDsv.Get(),worldBytes,0,0);
            ComPtr<ID3D11Resource> maskResource;
            if(capture.view())capture.view()->GetResource(&maskResource);
            ComPtr<ID3D11Texture2D> maskTexture;
            if(maskResource)maskResource.As(&maskTexture);
            const auto mask=maskTexture?readTex(maskTexture.Get(),1):std::vector<BYTE>{};
            UINT marked=0;for(BYTE pixel:mask)marked+=pixel==255;
            uint32_t completed=~0u;
            const bool alternateAccounted=used==2 &&
                edvr::flatUntrustedObservationAccounted(observed[1],capture,&completed);
            check(firstWorld.observed && firstWorld.nominee && !firstWorld.began &&
                  worldFailedAsExpected && (!scenario || (secondWorld.observed &&
                  secondWorld.nominee && !secondWorld.began)) &&
                  cfcaDraw.began && sevenDraw.began && eightDraw.began && aacfDraw.began &&
                  accepted && capture.mixed() && alternateAccounted && completed==4 &&
                  !edvr::flatUntrustedObservationAccounted(observed[0],capture) &&
                  mask.size()==w*h && marked==105 && mask[4*w+4]==255 &&
                  sameOriginal(),
                  scenario?"explicit-depth then null H-camera shader failures leave complete alternate coverage":
                           "world-first null PS is bucket-local and all alternate originals remain accounted");
            const auto omitted=submit(capture,observed,used,frame,7,alternateBytes,
                0x7B0DC42D383F694Cull,0x0DF03E64DF9DBEF1ull,
                secondVs.Get(),secondPs.Get(),secondRect,false,false);
            check(omitted.observed && !omitted.planned &&
                  !edvr::flatUntrustedObservationAccounted(observed[1],capture,&completed) &&
                  observed[1].draws==5 && completed==4 && sameOriginal(),
                  "an actual unbracketed alternate draw fails the shared observation-to-receipt comparison");
        }
        for(uint32_t failing=0;failing<2;++failing) {
            const uint64_t frame=60+failing;
            edvr::FlatUntrustedCoverage capture;capture.beginFrame(frame);
            edvr::FlatUntrustedObservedCamera observed[4]{};
            uint32_t used=0;clearPair();
            const auto bad=submit(capture,observed,used,frame,1,alternateBytes,
                0x7B0DC42D383F694Cull,failing?0xABCDEF01ull:0ull,
                vs.Get(),failing?explicitDepthPs.Get():nullptr,full,false);
            const auto good=submit(capture,observed,used,frame,2,alternateBytes,
                0xAACFDCF2FB9AD809ull,0xCF534B32F491561Aull,
                secondVs.Get(),secondPs.Get(),secondRect,true);
            check(bad.nominee && !bad.began && good.nominee && !good.began &&
                  !capture.select(liveDepth.Get(),liveDsv.Get(),worldBytes,0,0) &&
                  !edvr::flatUntrustedObservationAccounted(observed[0],capture) &&
                  sameOriginal(),
                  failing?"explicit-depth alternate shader failure refuses treatment":
                          "null-PS alternate shader failure refuses treatment");
        }
        {
            float worldLikeRows[6][4]{};
            std::memcpy(worldLikeRows,worldRows,sizeof(worldLikeRows));
            worldLikeRows[3][2]=.0252f;
            const auto* worldLikeBytes=reinterpret_cast<const unsigned char*>(worldLikeRows);
            edvr::FlatUntrustedCoverage capture;capture.beginFrame(70);
            edvr::FlatUntrustedObservedCamera observed[4]{};
            uint32_t used=0;clearPair();
            const auto bad=submit(capture,observed,used,70,1,worldLikeBytes,
                0xEB5234DB6ADB491Dull,0,vs.Get(),nullptr,full,false);
            const auto alternate=submit(capture,observed,used,70,2,alternateBytes,
                0x7B0DC42D383F694Cull,0x0DF03E64DF9DBEF1ull,
                vs.Get(),ps.Get(),full,false);
            check(bad.nominee && !bad.began && alternate.began && used==2 &&
                  !capture.select(liveDepth.Get(),liveDsv.Get(),worldBytes,0,0) &&
                  sameOriginal(),
                  "a failed near-world bucket with camera bytes different from H remains an alternate and refuses");
        }
        {
            edvr::FlatUntrustedCoverage capture;capture.beginFrame(80);
            edvr::FlatUntrustedObservedCamera observed[4]{};
            uint32_t used=0;clearPair();
            const auto worldFailure=submit(capture,observed,used,80,1,worldBytes,
                0xEB5234DB6ADB491Dull,0,vs.Get(),nullptr,full,false);
            capture.diagnosePlanShader(context,1,[](void*) {return uint64_t(0);});
            const auto alternateFailure=submit(capture,observed,used,80,2,alternateBytes,
                0x7B0DC42D383F694Cull,0xABCDEF01ull,
                secondVs.Get(),explicitDepthPs.Get(),secondRect,false);
            const auto supported=submit(capture,observed,used,80,3,alternateBytes,
                0xAACFDCF2FB9AD809ull,0xCF534B32F491561Aull,
                secondVs.Get(),secondPs.Get(),secondRect,true);
            edvr::FlatContractRecord supportedRecord{};
            supportedRecord.key.color=liveColor.Get();
            supportedRecord.key.depth=liveDepth.Get();
            supportedRecord.key.dsv=liveDsv.Get();
            supportedRecord.key.width=w;supportedRecord.key.height=h;
            supportedRecord.key.vs=0xAACFDCF2FB9AD809ull;
            supportedRecord.key.ps=0xCF534B32F491561Aull;
            std::memcpy(supportedRecord.camera,alternateBytes,sizeof(supportedRecord.camera));
            supportedRecord.key.camera=supportedRecord.camera;
            supportedRecord.first=supportedRecord.last=3;supportedRecord.draws=1;
            edvr::FlatUntrustedQualification qualification{};
            const bool qualified=capture.qualifies(
                supportedRecord,worldBytes,0,0,&qualification);
            const bool selected=capture.select(liveDepth.Get(),liveDsv.Get(),worldBytes,0,0);
            capture.consumer();
            const auto worldReport=capture.diagnoseBucket(
                0,liveDepth.Get(),liveDsv.Get(),worldBytes,0,0);
            const auto alternateReport=capture.diagnoseBucket(
                1,liveDepth.Get(),liveDsv.Get(),worldBytes,0,0);
            check(worldFailure.observed && worldFailure.nominee && !worldFailure.began &&
                  alternateFailure.observed && alternateFailure.nominee &&
                  alternateFailure.planned && !alternateFailure.began &&
                  supported.observed && supported.nominee && !supported.began &&
                  used==2 && observed[0].draws==1 && observed[1].draws==2 &&
                  !qualified && !selected && !capture.view() && sameOriginal(),
                  "world-first null PS and alternate patch failure both execute originals but refuse the supported record");
            check(worldReport.used && worldReport.world && !worldReport.alternate &&
                  worldReport.firstFailure.sequence==1 &&
                  std::strcmp(worldReport.firstFailure.stage,"plan")==0 &&
                  worldReport.firstFailure.reason=="untrusted-source-shader-identity" &&
                  worldReport.firstFailure.ps==0 &&
                  worldReport.firstFailure.actualPsRead &&
                  !worldReport.firstFailure.actualPsObject && !worldReport.firstFailure.actualPs &&
                  worldReport.firstFailure.pending==0 &&
                  worldReport.firstFailure.completed==0 &&
                  alternateReport.used && alternateReport.alternate && !alternateReport.world &&
                  alternateReport.dsvMatch && alternateReport.cameraShape &&
                  alternateReport.phasePair &&
                  alternateReport.firstFailure.sequence==2 &&
                  std::strcmp(alternateReport.firstFailure.stage,"begin")==0 &&
                  alternateReport.firstFailure.reason.find("non-colour PS output")!=std::string::npos &&
                  alternateReport.firstFailure.ps==0xABCDEF01ull &&
                  alternateReport.firstFailure.pending==1 &&
                  alternateReport.firstFailure.completed==0 &&
                  alternateReport.firstFailure.actualPsRead &&
                  alternateReport.firstFailure.actualPsObject==explicitDepthPs.Get() &&
                  alternateReport.firstFailure.actualPs==
                      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(explicitDepthPs.Get())) &&
                  alternateReport.firstFailure.current && alternateReport.firstFailure.viewport &&
                  alternateReport.firstFailure.writeEpoch==80 &&
                  qualification.hIdentity && qualification.uniqueAlternate &&
                  qualification.dsvMatch && qualification.countPresent &&
                  !qualification.ready && !qualification.bucketValid &&
                  qualification.expected==1 && qualification.matching==0 &&
                  qualification.first==supportedRecord.first && qualification.last==supportedRecord.last &&
                  std::memcmp(worldReport.camera,worldBytes,edvr::kFlatCameraBytes)==0 &&
                  std::memcmp(alternateReport.camera,alternateBytes,edvr::kFlatCameraBytes)==0 &&
                  std::memcmp(qualification.recordCamera,supportedRecord.camera,edvr::kFlatCameraBytes)==0 &&
                  std::strcmp(qualification.reason,
                              alternateReport.firstFailure.reason.c_str())==0 &&
                  !capture.globalFailure(),
                  "consumer diagnosis retains the alternate patcher's first reason and origin despite an earlier world-bucket failure");
        }
        {
            edvr::FlatUntrustedCoverage capture;capture.beginFrame(81);
            context->PSSetShader(explicitDepthPs.Get(),nullptr,0);
            check(!capture.plan(81,1,liveColor.Get(),liveDepth.Get(),liveDsv.Get(),
                  0xEB5234DB6ADB491Dull,0,worldBytes),
                  "an untracked nonnull shader still fails nominal shader identity");
            capture.diagnosePlanShader(context,1,[](void*) {return uint64_t(0);});
            context->PSSetShader(nullptr,nullptr,0);
            capture.diagnosePlanShader(context,1,[](void*) {return uint64_t(0);});
            const auto report=capture.diagnoseBucket(0,liveDepth.Get(),liveDsv.Get(),worldBytes,0,0);
            check(report.firstFailure.actualPsRead &&
                  report.firstFailure.actualPsObject==explicitDepthPs.Get() &&
                  report.firstFailure.actualPs==0,
                  "first plan failure distinguishes an untracked nonnull shader from null and preserves its first snapshot");
        }
        {
            edvr::FlatUntrustedDiagnosticBudget budget;
            check(!budget.take(100,false,nullptr,nullptr) &&
                  !budget.firstSupported() && budget.emitted()==0 && budget.dropped()==0,
                  "a holstered successful frame does not spend a supported-alternate refusal report");
            const bool ordinary=budget.take(101,false,"plan","world-null");
            const bool first=budget.take(102,true,"begin","alternate-no-color-output");
            const bool early=budget.take(161,true,"begin","alternate-no-color-output");
            const bool second=budget.take(162,true,"begin","alternate-no-color-output");
            const bool late=budget.take(222,true,"other","new-alternate-reason");
            const bool duplicate=budget.take(223,false,"plan","world-null");
            const bool newOrdinary=budget.take(224,false,"begin","world-null");
            check(ordinary && first && !early && second && !late &&
                  !duplicate && newOrdinary && budget.firstSupported() &&
                  budget.secondSupported() && budget.emitted()==4 &&
                  budget.dropped()==3,
                  "two supported-refusal reports require 60 frames and do not fall through to the distinct ordinary budget");
        }
    }
    context->ClearState();
    return failures+flatEmptyOutputGpuTests(device,context);
}
