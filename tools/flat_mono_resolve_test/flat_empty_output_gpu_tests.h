#pragma once
#include "flat_empty_output_fixture.h"
#include "../../src/d3d11/flat_untrusted_coverage.h"

inline int flatEmptyOutputGpuTests(ID3D11Device* device,ID3D11DeviceContext* context) {
    using Microsoft::WRL::ComPtr;
    int failures=0;
    auto check=[&](bool ok,const char* why){if(!ok){std::printf("FAIL: empty-output GPU %s\n",why);++failures;}};
    uint64_t hash=1469598103934665603ull;
    for(unsigned char b:kFlatEmptyB40B){hash^=b;hash*=1099511628211ull;}
    check(hash==0xB40B0462256E31C2ull,"exact captured B40B creation bytes retain their EDVR hash");
    auto compile=[&](const char* text,const char* profile){
        ComPtr<ID3DBlob> code,error;
        const HRESULT hr=D3DCompile(text,std::strlen(text),nullptr,nullptr,nullptr,"main",profile,0,0,&code,&error);
        if(FAILED(hr)&&error)std::printf("empty-output shader: %s\n",static_cast<const char*>(error->GetBufferPointer()));
        check(SUCCEEDED(hr),"fixture compiles");return code;
    };
    auto vertex=compile("struct O{float3 uv:__USER_VERTEX_M_TEXCOORD;float4 p:SV_Position;};"
        "O main(uint id:SV_VertexID){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};"
        "O o;o.p=float4(p[id],.5,1);o.uv=float3((p[id].x+1)*.5,(1-p[id].y)*.5,.5);return o;}","vs_5_0");
    auto color=compile("float4 main():SV_Target{return float4(.8,.2,.1,1);}","ps_5_0");
    auto forced=compile("[earlydepthstencil] void main(float4 p:SV_Position){if(p.x<4)discard;}","ps_5_0");
    auto depth=compile("float main():SV_Depth{return .5;}","ps_5_0");
    auto coverage=compile("uint main():SV_Coverage{return 1;}","ps_5_0");
    auto uav=compile("RWByteAddressBuffer data:register(u1);void main(){data.Store(0,1);}","ps_5_0");
    if(!vertex||!color||!forced||!depth||!coverage||!uav)return failures;
    std::vector<BYTE> patched;std::string why;
    check(edvr::flatOverlayPatchPs(kFlatEmptyB40B,sizeof(kFlatEmptyB40B),patched,why),"actual conditional-alpha empty signature patches");
    ComPtr<ID3D11PixelShader> original,patchedPs,colorPs;
    ComPtr<ID3D11VertexShader> vs;
    check(SUCCEEDED(device->CreatePixelShader(kFlatEmptyB40B,sizeof(kFlatEmptyB40B),nullptr,&original)) &&
          !patched.empty() && SUCCEEDED(device->CreatePixelShader(patched.data(),patched.size(),nullptr,&patchedPs)) &&
          SUCCEEDED(device->CreatePixelShader(color->GetBufferPointer(),color->GetBufferSize(),nullptr,&colorPs)) &&
          SUCCEEDED(device->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&vs)),
          "original and patched actual B40B create on WARP");
    if(!original||!patchedPs||!colorPs||!vs)return failures;
    edvr::FlatOverlayLayer::rememberPixelShader(original.Get(),kFlatEmptyB40B,sizeof(kFlatEmptyB40B),false);
    edvr::FlatOverlayLayer::rememberPixelShader(colorPs.Get(),color->GetBufferPointer(),color->GetBufferSize(),false);
    for(auto* bad:{forced.Get(),depth.Get(),coverage.Get(),uav.Get()})
        check(!edvr::flatOverlayPatchPs(bad->GetBufferPointer(),bad->GetBufferSize(),patched,why) && patched.empty(),
              "forced early depth, explicit depth/coverage and UAV empty shaders remain refused");
    auto reverseChunks=[&](ID3DBlob* blob){
        auto chunks=edvr::dxbc_container::parseContainer(blob->GetBufferPointer(),blob->GetBufferSize(),0x50u);
        std::reverse(chunks.begin(),chunks.end());return edvr::dxbc_container::makeContainer(chunks);
    };
    const auto reversed=reverseChunks(forced.Get());
    check(!edvr::flatOverlayPatchPs(reversed.data(),reversed.size(),patched,why),"forced early-depth empty refusal is independent of chunk order");
    auto emptyChunks=edvr::dxbc_container::parseContainer(kFlatEmptyB40B,sizeof(kFlatEmptyB40B),0x50u);
    std::reverse(emptyChunks.begin(),emptyChunks.end());
    const auto reversedEmpty=edvr::dxbc_container::makeContainer(emptyChunks);
    check(edvr::flatOverlayPatchPs(reversedEmpty.data(),reversedEmpty.size(),patched,why),"empty signature admission is independent of chunk order");
    bool changed=false;
    for(auto& chunk:emptyChunks)if(chunk.tag==0x58454853u||chunk.tag==0x52444853u){
        std::vector<uint32_t> words(chunk.bytes.size()/4);std::memcpy(words.data(),chunk.bytes.data(),chunk.bytes.size());
        for(size_t at=2;at<words.size();at+=edvr::dxbc_container::instructionLength(words,at))
            if((words[at]&0x7ffu)==62){words[at]=(words[at]&~0x7ffu)|63u;changed=true;break;}
        std::memcpy(chunk.bytes.data(),words.data(),chunk.bytes.size());
    }
    const auto conditionalReturn=edvr::dxbc_container::makeContainer(emptyChunks);
    check(changed&&!edvr::flatOverlayPatchPs(conditionalReturn.data(),conditionalReturn.size(),patched,why),"empty shader conditional return remains refused");

    constexpr UINT w=16,h=16;
    ComPtr<ID3D11Texture2D> colors[2],depths[2],alpha;
    ComPtr<ID3D11RenderTargetView> rtvs[2];ComPtr<ID3D11DepthStencilView> dsvs[2];
    D3D11_TEXTURE2D_DESC cd{};cd.Width=w;cd.Height=h;cd.MipLevels=cd.ArraySize=cd.SampleDesc.Count=1;
    cd.Format=DXGI_FORMAT_R10G10B10A2_TYPELESS;cd.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rv{};rv.Format=DXGI_FORMAT_R10G10B10A2_UNORM;rv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_TEXTURE2D_DESC dd=cd;dd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    for(UINT i=0;i<2;++i)check(SUCCEEDED(device->CreateTexture2D(&cd,nullptr,&colors[i])) &&
        SUCCEEDED(device->CreateRenderTargetView(colors[i].Get(),&rv,&rtvs[i])) &&
        SUCCEEDED(device->CreateTexture2D(&dd,nullptr,&depths[i])) &&
        SUCCEEDED(device->CreateDepthStencilView(depths[i].Get(),nullptr,&dsvs[i])),"color/depth targets create");
    D3D11_TEXTURE2D_DESC ad=cd;ad.Format=DXGI_FORMAT_R32_FLOAT;ad.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11ShaderResourceView> alphaView;
    check(SUCCEEDED(device->CreateTexture2D(&ad,nullptr,&alpha))&&
          SUCCEEDED(device->CreateShaderResourceView(alpha.Get(),nullptr,&alphaView)),"alpha texture creates");
    D3D11_BUFFER_DESC cbd{};cbd.ByteWidth=32;cbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb;check(SUCCEEDED(device->CreateBuffer(&cbd,nullptr,&cb)),"actual PS cb2 creates");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;check(SUCCEEDED(device->CreateSamplerState(&sd,&sampler)),"alpha sampler creates");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.ScissorEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;check(SUCCEEDED(device->CreateRasterizerState(&rd,&raster)),"raster state creates");
    D3D11_DEPTH_STENCIL_DESC zd{};zd.DepthEnable=TRUE;zd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;zd.DepthFunc=D3D11_COMPARISON_LESS;
    zd.StencilEnable=TRUE;zd.StencilReadMask=zd.StencilWriteMask=0xff;
    zd.FrontFace.StencilFunc=zd.BackFace.StencilFunc=D3D11_COMPARISON_EQUAL;
    zd.FrontFace.StencilFailOp=zd.BackFace.StencilFailOp=D3D11_STENCIL_OP_KEEP;
    zd.FrontFace.StencilDepthFailOp=zd.BackFace.StencilDepthFailOp=D3D11_STENCIL_OP_KEEP;
    zd.FrontFace.StencilPassOp=zd.BackFace.StencilPassOp=D3D11_STENCIL_OP_INCR_SAT;
    ComPtr<ID3D11DepthStencilState> tested,prefillDepth,prefillStencil,unionState;
    check(SUCCEEDED(device->CreateDepthStencilState(&zd,&tested)),"depth and changing stencil test creates");
    zd.StencilEnable=FALSE;check(SUCCEEDED(device->CreateDepthStencilState(&zd,&prefillDepth)),"prefill depth creates");
    zd.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    check(SUCCEEDED(device->CreateDepthStencilState(&zd,&unionState)),"equal-depth union state creates");
    zd.DepthEnable=FALSE;zd.StencilEnable=TRUE;
    zd.FrontFace.StencilFunc=zd.BackFace.StencilFunc=D3D11_COMPARISON_ALWAYS;
    zd.FrontFace.StencilPassOp=zd.BackFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;
    check(SUCCEEDED(device->CreateDepthStencilState(&zd,&prefillStencil)),"prefill stencil creates");
    D3D11_BLEND_DESC bd{};bd.RenderTarget[0].RenderTargetWriteMask=0xf;
    ComPtr<ID3D11BlendState> blend;check(SUCCEEDED(device->CreateBlendState(&bd,&blend)),"blend creates");
    if(!rtvs[0]||!rtvs[1]||!dsvs[0]||!dsvs[1]||!alphaView||!cb||!sampler||!raster||!tested||!prefillDepth||!prefillStencil||!unionState||!blend)return failures;
    auto read=[&](ID3D11Texture2D* src,UINT stride){
        D3D11_TEXTURE2D_DESC d{};src->GetDesc(&d);d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stage;std::vector<BYTE> out;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&stage)))return out;
        context->CopyResource(stage.Get(),src);D3D11_MAPPED_SUBRESOURCE map{};
        if(FAILED(context->Map(stage.Get(),0,D3D11_MAP_READ,0,&map)))return out;
        out.resize(w*h*stride);for(UINT y=0;y<h;++y)std::memcpy(out.data()+y*w*stride,static_cast<BYTE*>(map.pData)+y*map.RowPitch,w*stride);
        context->Unmap(stage.Get(),0);return out;
    };
    const float clear[]={.1f,.2f,.3f,1};D3D11_RECT full{0,0,w,h},bottom{0,8,w,h},upperRight{8,0,w,8},left{0,0,8,h};
    auto bind=[&](UINT i,ID3D11PixelShader* ps,ID3D11DepthStencilState* ds,UINT stencil,const D3D11_RECT& rect){
        context->OMSetRenderTargets(1,rtvs[i].GetAddressOf(),dsvs[i].Get());context->OMSetBlendState(blend.Get(),nullptr,~0u);
        context->OMSetDepthStencilState(ds,stencil);context->RSSetState(raster.Get());
        D3D11_VIEWPORT vp{0,0,float(w),float(h),0,1};context->RSSetViewports(1,&vp);context->RSSetScissorRects(1,&rect);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps,nullptr,0);
        context->PSSetShaderResources(0,1,alphaView.GetAddressOf());context->PSSetSamplers(0,1,sampler.GetAddressOf());context->PSSetConstantBuffers(2,1,cb.GetAddressOf());
    };
    auto same=[&](){const auto c=read(colors[0].Get(),4),d=read(depths[0].Get(),4);
        return c.size()==w*h*4&&d.size()==w*h*4&&c==read(colors[1].Get(),4)&&d==read(depths[1].Get(),4);};
    auto configure=[&](UINT mode){float values[8]={0,0,0,mode?1.f:0.f,1,1,.5f,0};
        context->UpdateSubresource(cb.Get(),0,nullptr,values,0,0);std::vector<float> pixels(w*h);
        for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)pixels[y*w+x]=mode==2?.75f:mode==3?.25f:x<4?.25f:.75f;
        context->UpdateSubresource(alpha.Get(),0,nullptr,pixels.data(),w*4,0);};
    auto maskBytes=[&](edvr::FlatUntrustedCoverage& capture){ComPtr<ID3D11Resource> r;ComPtr<ID3D11Texture2D> t;
        if(capture.view())capture.view()->GetResource(&r);if(r)r.As(&t);return t?read(t.Get(),1):std::vector<BYTE>{};};
    float world[6][4]{};world[0][0]=world[1][1]=world[2][3]=world[4][2]=1;world[3][2]=.025f;
    float alternate[6][4];std::memcpy(alternate,world,sizeof(world));alternate[3][2]=.0675f;
    const auto* wb=reinterpret_cast<unsigned char*>(world);const auto* ab=reinterpret_cast<unsigned char*>(alternate);
    for(UINT mode=0;mode<4;++mode){
        context->ClearState();configure(mode);
        for(UINT i=0;i<2;++i){
            context->ClearRenderTargetView(rtvs[i].Get(),clear);context->ClearDepthStencilView(dsvs[i].Get(),3,.8f,3);
            bind(i,colorPs.Get(),prefillDepth.Get(),0,bottom);context->Draw(3,0);
            bind(i,colorPs.Get(),prefillStencil.Get(),1,upperRight);context->Draw(3,0);
        }
        const auto before=read(colors[0].Get(),4);
        edvr::FlatUntrustedCoverage capture;capture.beginFrame(100+mode);
        bind(0,original.Get(),tested.Get(),3,full);context->Draw(3,0);
        bind(1,original.Get(),tested.Get(),3,full);
        const bool begun=capture.plan(100+mode,1,colors[1].Get(),depths[1].Get(),dsvs[1].Get(),0xF516BF0201303B87ull,hash,ab)&&capture.beginDraw(context,100+mode);
        context->Draw(3,0);if(begun)capture.endDraw(context);
        const bool selected=capture.select(depths[1].Get(),dsvs[1].Get(),wb,0,0);
        const auto mask=maskBytes(capture);UINT marked=0;for(BYTE b:mask)marked+=b==255;
        const UINT expected=mode==1?32:mode==3?0:64;
        check(begun&&selected&&mask.size()==w*h&&marked==expected&&same()&&before==read(colors[1].Get(),4),
              "actual empty PS alpha off/mixed/pass/discard keeps original color and all depth/stencil bytes");
        bool footprint=mask.size()==w*h;
        if(footprint)for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)
            footprint&=(mask[y*w+x]!=0)==(x<8&&y<8&&(mode!=1||x>=4)&&mode!=3);
        check(footprint,"empty PS mask excludes alpha, depth and stencil rejected fragments");
        ComPtr<ID3D11PixelShader> restored;context->PSGetShader(&restored,nullptr,nullptr);
        ComPtr<ID3D11DepthStencilState> restoredDs;UINT ref=0;context->OMGetDepthStencilState(&restoredDs,&ref);
        ComPtr<ID3D11BlendState> restoredBlend;FLOAT factors[4];UINT samples;context->OMGetBlendState(&restoredBlend,factors,&samples);
        ID3D11RenderTargetView* views[8]{};ID3D11DepthStencilView* restoredDsv=nullptr;context->OMGetRenderTargets(8,views,&restoredDsv);
        check(restored.Get()==original.Get()&&restoredDs.Get()==tested.Get()&&ref==3&&restoredBlend.Get()==blend.Get()&&samples==~0u&&
              views[0]==rtvs[1].Get()&&!views[7]&&restoredDsv==dsvs[1].Get(),"empty PS bracket restores original PS, blend, DSV, stencil ref and MRTs");
        for(auto* v:views)if(v)v->Release();if(restoredDsv)restoredDsv->Release();
    }
    context->ClearState();configure(0);
    for(UINT i=0;i<2;++i){context->ClearRenderTargetView(rtvs[i].Get(),clear);context->ClearDepthStencilView(dsvs[i].Get(),3,.8f,3);}
    edvr::FlatUntrustedCoverage capture;capture.beginFrame(200);edvr::FlatUntrustedObservedCamera observed[2]{};uint32_t used=0;
    bool captured=true;
    for(UINT seq=1;seq<=3;++seq){
        const uint64_t vh=seq==1?0xF516BF0201303B87ull:seq==2?0x7B0DC42D383F694Cull:0xAACFDCF2FB9AD809ull;
        const uint64_t ph=seq==1?hash:seq==2?0x0DF03E64DF9DBEF1ull:0xCF534B32F491561Aull;
        auto* ps=seq==1?original.Get():colorPs.Get();
        bind(0,ps,unionState.Get(),0,left);context->Draw(3,0);bind(1,ps,unionState.Get(),0,left);
        edvr::FlatRuntimeDraw draw{};auto& k=draw.key;k.color=colors[1].Get();k.depth=depths[1].Get();k.dsv=dsvs[1].Get();
        k.format=23;k.width=w;k.height=h;k.vs=vh;k.ps=ph;k.b1=cb.Get();k.writeEpoch=200;k.writeSeq=1;k.viewportCount=1;k.viewport[2]=w;k.viewport[3]=h;k.viewport[5]=1;
        std::memcpy(draw.camera,ab,sizeof(draw.camera));k.camera=draw.camera;k.cameraHash=edvr::flatCameraHash(ab);draw.supported=seq==3;
        const auto nominee=edvr::flatUntrustedNomination(draw,nullptr,nullptr,true,true,seq,200);
        captured&=edvr::flatUntrustedObserveCamera(observed,2,used,k.depth,ab)!=nullptr;
        const bool begun=nominee.admissible()&&capture.plan(200,seq,colors[1].Get(),depths[1].Get(),dsvs[1].Get(),vh,ph,ab)&&capture.beginDraw(context,200);
        captured&=begun;context->Draw(3,0);if(begun)capture.endDraw(context);
    }
    edvr::FlatContractRecord record{};record.key.color=colors[1].Get();record.key.depth=depths[1].Get();record.key.dsv=dsvs[1].Get();record.key.width=w;record.key.height=h;
    record.key.vs=0xAACFDCF2FB9AD809ull;record.key.ps=0xCF534B32F491561Aull;std::memcpy(record.camera,ab,sizeof(record.camera));record.key.camera=record.camera;
    record.first=record.last=3;record.draws=1;
    check(captured&&used==1&&observed[0].draws==3&&edvr::flatUntrustedObservationAccounted(observed[0],capture)&&
          capture.qualifies(record,wb,0,0)&&capture.select(depths[1].Get(),dsvs[1].Get(),wb,0,0),
          "first empty alternate followed by unsupported material and AACF has complete receipts and qualifies at H");
    const auto beforeWorld=maskBytes(capture);
    for(UINT i=0;i<2;++i){bind(i,colorPs.Get(),unionState.Get(),0,full);context->Draw(3,0);}
    const auto afterWorld=maskBytes(capture);UINT marked=0;for(BYTE b:afterWorld)marked+=b==255;
    check(beforeWorld.size()==w*h&&afterWorld==beforeWorld&&marked==128&&same(),
          "equal-depth world overdraw preserves alternate depth-only coverage and original color/depth/stencil");
    context->ClearState();return failures;
}
