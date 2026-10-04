#pragma once
#include "../../src/d3d11/flat_foreground_ownership.h"
#include "../../src/d3d11/flat_overlay_layer.h"

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
    if(!vsCode || !psCode || !csCode)return failures;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ComputeShader> cs;
    check(SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs)) &&
          SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps)) &&
          SUCCEEDED(device->CreateComputeShader(csCode->GetBufferPointer(),csCode->GetBufferSize(),nullptr,&cs)),
          "raster and actual ownership compute shaders create");
    if(!vs || !ps || !cs)return failures;
    edvr::FlatOverlayLayer::rememberPixelShader(ps.Get(),psCode->GetBufferPointer(),psCode->GetBufferSize(),false);

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
    rasterDesc.DepthClipEnable=TRUE;
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
    auto bind=[&](ID3D11RenderTargetView* rtv,ID3D11DepthStencilView* dsv) {
        context->OMSetRenderTargets(1,&rtv,dsv);
        context->OMSetBlendState(blend.Get(),nullptr,~0u);
        context->OMSetDepthStencilState(depthState.Get(),0);
        context->RSSetState(raster.Get());
        context->RSSetViewports(1,&viewport);
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
    const bool began=probe.beginDraw(context,1,liveColor.Get(),liveDsv.Get(),&reason,true);
    check(began,"diagnostic admits format-23 pool with its format-24 RTV");
    if(began) { context->Draw(3,0); probe.endDraw(context); }
    check(probe.ready(1,liveColor.Get()) && probe.markedDraws()==1,
          "actual original draw exports private coverage");
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
        if(!good || !extentCb || !probe.coverageView())return result;
        ID3D11ShaderResourceView* inputs[4]={probe.coverageView(),cohortView.Get(),
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
    context->ClearState();
    return failures;
}
