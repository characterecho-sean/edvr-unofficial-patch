#include "../../src/d3d11/eye_draw_snapshot.h"
#include "../../src/d3d11/gui_draw_snapshot.h"
#include "../../src/d3d11/eye_depth_capture.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;
void check(bool ok, const char* why) {
    if (!ok) { std::printf("FAIL: %s\n", why); std::exit(1); }
}
void hr(HRESULT v) { check(SUCCEEDED(v), "D3D operation"); }
void wait_gpu(ID3D11DeviceContext* ctx, ID3D11Device* dev) {
    check(ctx && dev, "GPU wait inputs");
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query; hr(dev->CreateQuery(&qd, &query));
    ctx->End(query.Get()); ctx->Flush();
    const ULONGLONG deadline = GetTickCount64() + 10000;
    HRESULT ready = S_FALSE;
    while (ready == S_FALSE && GetTickCount64() < deadline)
        ready = ctx->GetData(query.Get(), nullptr, 0, 0);
    hr(ready); check(ready == S_OK, "GPU timeout");
}
int wmain(int argc, wchar_t** argv) {
    check(argc == 2, "output path required");
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 192; bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb; hr(dev->CreateBuffer(&bd, nullptr, &cb));
    ID3D11Buffer* b = cb.Get(); ctx->VSSetConstantBuffers(0, 1, &b); ctx->PSSetConstantBuffers(2, 1, &b);
    D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 8;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> rt; hr(dev->CreateTexture2D(&td, nullptr, &rt));
    ComPtr<ID3D11RenderTargetView> rtv; hr(dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv));
    ID3D11RenderTargetView* r = rtv.Get(); ctx->OMSetRenderTargets(1, &r, nullptr);
    td.Width = 3; td.Height = 2; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    uint8_t rgba[24]; for (int i = 0; i < 24; ++i) rgba[i] = static_cast<uint8_t>(i);
    D3D11_SUBRESOURCE_DATA sd{rgba, 12, 0};
    ComPtr<ID3D11Texture2D> texture; hr(dev->CreateTexture2D(&td, &sd, &texture));
    ComPtr<ID3D11ShaderResourceView> srv; hr(dev->CreateShaderResourceView(texture.Get(), nullptr, &srv));
    ID3D11ShaderResourceView* s = srv.Get(); ctx->PSSetShaderResources(2, 1, &s);
    edvr::EyeDrawSnapshot snap;
    snap.capture(ctx.Get(), 0, 0, 123, 0, 'X', 6, 1, 0);
    check(snap.draws.empty(), "unwatched family captured");
    for (uint32_t i = 0; i < 3; ++i) {
        float values[48]; for (float& v : values) v = static_cast<float>(12 + i*3);
        ctx->UpdateSubresource(cb.Get(), 0, nullptr, values, 0, 0);
        snap.capture(ctx.Get(), 100+i, i*2, edvr::EyeDrawSnapshot::kHolo, 0xA2965EC2931A39C8ull, 'X', 6, 1, 0);
        // Mutating the same resources after a draw must not change its copy.
        for (float& v : values) v = 99;
        ctx->UpdateSubresource(cb.Get(), 0, nullptr, values, 0, 0);
        ctx->UpdateSubresource(texture.Get(), 0, nullptr, values, 12, 0);
    }
    ComPtr<ID3D11Buffer> bound; ctx->VSGetConstantBuffers(0, 1, &bound);
    check(bound.Get() == cb.Get(), "capture changed constant bindings");
    ComPtr<ID3D11ShaderResourceView> boundSrv; ctx->PSGetShaderResources(2, 1, &boundSrv);
    check(boundSrv.Get() == srv.Get(), "capture changed texture binding");
    ComPtr<ID3D11RenderTargetView> boundRt; ctx->OMGetRenderTargets(1, &boundRt, nullptr);
    check(boundRt.Get() == rtv.Get(), "capture changed render target");
    check(snap.surfaces.size() == 1 && snap.draws.size() == 3, "duplicate source or lost draw");
    edvr::EyeDrawSnapshot unlitSnap;
    ID3D11ShaderResourceView* none=nullptr;ctx->PSSetShaderResources(2,1,&none);ctx->PSSetShaderResources(1,1,&s);
    unlitSnap.capture(ctx.Get(),102,0,edvr::EyeDrawSnapshot::kHolo,edvr::kHoloUnlitPs,'X',6,1,0);
    check(unlitSnap.draws.size()==1 && unlitSnap.surfaces.size()==1 && unlitSnap.draws[0].texture==0,"unlit hologram captures t1 with no t2 surface");
    ctx->PSSetShaderResources(2,1,&s);
    // Exit-profile/menu and direct-screen composites use t1 and t0,
    // respectively. They were previously absent from an otherwise valid
    // eye dump, leaving the profile's actual source resolution unknown.
    edvr::EyeDrawSnapshot menuSnap;
    ctx->PSSetShaderResources(1,1,&s);
    menuSnap.capture(ctx.Get(),102,0,edvr::EyeDrawSnapshot::kPanel,0x9107E72CB016CC02ull,'X',6,1,0);
    ctx->PSSetShaderResources(0,1,&s);
    menuSnap.capture(ctx.Get(),102,1,edvr::EyeDrawSnapshot::kScreen,0x85565E9261812E2Full,'X',6,1,0);
    check(menuSnap.draws.size()==2 && menuSnap.surfaces.size()==1,"menu and screen sources captured with deduplication");
    check(menuSnap.draws[0].texture==0 && menuSnap.draws[1].texture==0,"menu and screen use their actual source slots");

    // The targeting sprite's measured 2613x2286 RGBA8 source is 22.79 MiB:
    // the exact E508/63ABD pair gets the bounded 32 MiB diagnostic exception,
    // while another PS on the same VS still uses the normal 16 MiB cap.
    D3D11_TEXTURE2D_DESC spriteDesc{}; spriteDesc.Width=2613; spriteDesc.Height=2286;
    spriteDesc.MipLevels=spriteDesc.ArraySize=spriteDesc.SampleDesc.Count=1;
    spriteDesc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS; spriteDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> spriteTexture; hr(dev->CreateTexture2D(&spriteDesc,nullptr,&spriteTexture));
    D3D11_SHADER_RESOURCE_VIEW_DESC spriteView{}; spriteView.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    spriteView.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; spriteView.Texture2D.MipLevels=1;
    ComPtr<ID3D11ShaderResourceView> spriteSrv; hr(dev->CreateShaderResourceView(spriteTexture.Get(),&spriteView,&spriteSrv));
    ID3D11ShaderResourceView* spriteBinding=spriteSrv.Get(); ctx->PSSetShaderResources(0,1,&spriteBinding);
    edvr::EyeDrawSnapshot spriteSnap;
    spriteSnap.capture(ctx.Get(),103,0,edvr::EyeDrawSnapshot::kSprite,
                       edvr::EyeDrawSnapshot::kSpritePs,'X',6,1,0);
    check(spriteSnap.surfaces.size()==1 && spriteSnap.failures==0,
          "exact E508 sprite source fits diagnostic texture exception");
    const auto capturedTextureBytes=spriteSnap.textureBytes;
    spriteSnap.capture(ctx.Get(),103,1,edvr::EyeDrawSnapshot::kSprite,
                       edvr::EyeDrawSnapshot::kSpritePs,'X',6,1,0);
    check(spriteSnap.surfaces.size()==1 && spriteSnap.textureBytes==capturedTextureBytes,
          "exact E508 sprite source deduplicates repeated binding");
    ComPtr<ID3D11ShaderResourceView> retainedSrv; ctx->PSGetShaderResources(0,1,&retainedSrv);
    check(retainedSrv.Get()==spriteSrv.Get(), "exact E508 capture preserves SRV binding");
    D3D11_TEXTURE2D_DESC spriteTargetDesc{};spriteTargetDesc.Width=spriteTargetDesc.Height=1504;spriteTargetDesc.MipLevels=spriteTargetDesc.ArraySize=spriteTargetDesc.SampleDesc.Count=1;spriteTargetDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;spriteTargetDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> spriteTarget;ComPtr<ID3D11RenderTargetView> spriteTargetRtv;hr(dev->CreateTexture2D(&spriteTargetDesc,nullptr,&spriteTarget));hr(dev->CreateRenderTargetView(spriteTarget.Get(),nullptr,&spriteTargetRtv));
    D3D11_TEXTURE2D_DESC spriteDepthDesc{}; spriteDepthDesc.Width=spriteDepthDesc.Height=1504;
    spriteDepthDesc.MipLevels=spriteDepthDesc.ArraySize=spriteDepthDesc.SampleDesc.Count=1;
    spriteDepthDesc.Format=DXGI_FORMAT_R24G8_TYPELESS; spriteDepthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> spriteDepth;ComPtr<ID3D11DepthStencilView> spriteDsv;
    hr(dev->CreateTexture2D(&spriteDepthDesc,nullptr,&spriteDepth));
    D3D11_DEPTH_STENCIL_VIEW_DESC spriteDsvDesc{};spriteDsvDesc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;spriteDsvDesc.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    hr(dev->CreateDepthStencilView(spriteDepth.Get(),&spriteDsvDesc,&spriteDsv));
    const char* spriteVsCode="float4 main(float4 p:POSITION):SV_Position{return p;}";
    ComPtr<ID3DBlob> spriteVsBlob;hr(D3DCompile(spriteVsCode,strlen(spriteVsCode),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&spriteVsBlob,nullptr));
    D3D11_INPUT_ELEMENT_DESC spriteElement{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    ComPtr<ID3D11InputLayout> spriteLayout;hr(dev->CreateInputLayout(&spriteElement,1,spriteVsBlob->GetBufferPointer(),spriteVsBlob->GetBufferSize(),&spriteLayout));
    edvr::EyeDrawSnapshot::rememberLayout(spriteLayout.Get(),&spriteElement,1,edvr::EyeDrawSnapshot::kSprite);ctx->IASetInputLayout(spriteLayout.Get());
    D3D11_BUFFER_DESC structured{};structured.Usage=D3D11_USAGE_DEFAULT;structured.BindFlags=D3D11_BIND_SHADER_RESOURCE;structured.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    structured.ByteWidth=672;structured.StructureByteStride=336;ComPtr<ID3D11Buffer> spriteT33;ComPtr<ID3D11ShaderResourceView> spriteT33Srv;hr(dev->CreateBuffer(&structured,nullptr,&spriteT33));hr(dev->CreateShaderResourceView(spriteT33.Get(),nullptr,&spriteT33Srv));
    structured.ByteWidth=96;structured.StructureByteStride=48;ComPtr<ID3D11Buffer> spriteT38;ComPtr<ID3D11ShaderResourceView> spriteT38Srv;hr(dev->CreateBuffer(&structured,nullptr,&spriteT38));hr(dev->CreateShaderResourceView(spriteT38.Get(),nullptr,&spriteT38Srv));
    uint8_t spriteT33Bytes[672];uint8_t spriteT38Bytes[96];std::memset(spriteT33Bytes,0x31,sizeof(spriteT33Bytes));std::memset(spriteT38Bytes,0x42,sizeof(spriteT38Bytes));
    ctx->UpdateSubresource(spriteT33.Get(),0,nullptr,spriteT33Bytes,0,0);ctx->UpdateSubresource(spriteT38.Get(),0,nullptr,spriteT38Bytes,0,0);
    ID3D11ShaderResourceView* spriteT33Binding=spriteT33Srv.Get();ID3D11ShaderResourceView* spriteT38Binding=spriteT38Srv.Get();ctx->VSSetShaderResources(33,1,&spriteT33Binding);ctx->VSSetShaderResources(38,1,&spriteT38Binding);
    ID3D11RenderTargetView* spriteTargetBinding=spriteTargetRtv.Get();ctx->OMSetRenderTargets(1,&spriteTargetBinding,spriteDsv.Get());ctx->PSSetConstantBuffers(1,1,&b);
    float spriteCb[48];for(float& v:spriteCb)v=31.f;ctx->UpdateSubresource(cb.Get(),0,nullptr,spriteCb,0,0);
    const FLOAT spriteBefore[4]={.1f,.2f,.3f,1.f};ctx->ClearRenderTargetView(spriteTargetRtv.Get(),spriteBefore);ctx->ClearDepthStencilView(spriteDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.25f,1);
    spriteSnap.capture(ctx.Get(),103,2,edvr::EyeDrawSnapshot::kSprite,edvr::EyeDrawSnapshot::kSpritePs,'X',6,1,0);
    const FLOAT spriteAfter[4]={.7f,.2f,.1f,1.f};ctx->ClearRenderTargetView(spriteTargetRtv.Get(),spriteAfter);
    spriteSnap.captureEffectEnd(ctx.Get());
    std::memset(spriteT33Bytes,0x73,sizeof(spriteT33Bytes));std::memset(spriteT38Bytes,0x84,sizeof(spriteT38Bytes));ctx->UpdateSubresource(spriteT33.Get(),0,nullptr,spriteT33Bytes,0,0);ctx->UpdateSubresource(spriteT38.Get(),0,nullptr,spriteT38Bytes,0,0);
    float spriteCbAfter[48];for(float& v:spriteCbAfter)v=97.f;ctx->UpdateSubresource(cb.Get(),0,nullptr,spriteCbAfter,0,0);const FLOAT spriteLater[4]={.9f,.8f,.7f,1.f};ctx->ClearRenderTargetView(spriteTargetRtv.Get(),spriteLater);ctx->ClearDepthStencilView(spriteDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.75f,7);
    check(spriteSnap.spriteDiagnostics.size()==1 && spriteSnap.spriteDiagnostics[0].draw==2 && spriteSnap.spriteDiagnostics[0].layout.size()==1,
          "exact sprite boundary reserves one paired first-frame record");
    check(spriteSnap.spriteDiagnostics[0].width==1400 && spriteSnap.spriteDiagnostics[0].height==1400 && spriteSnap.spriteDiagnostics[0].beforeBytes==1400*1400*4 && spriteSnap.spriteDiagnostics[0].depthBytes==1400*1400*4 &&
          (spriteSnap.spriteDiagnostics[0].mesh[0]!=UINT32_MAX && spriteSnap.spriteDiagnostics[0].mesh[1]!=UINT32_MAX) &&
          (spriteSnap.spriteDiagnostics[0].stateMask&(1u<<3)),"sprite boundary retains RT, DSV, layout and structured-buffer metadata");
    edvr::EyeDrawSnapshot spriteCap;
    for(UINT i=0;i<11;++i){spriteCap.capture(ctx.Get(),200,i,edvr::EyeDrawSnapshot::kSprite,edvr::EyeDrawSnapshot::kSpritePs,'X',6,1,0);spriteCap.captureEffectEnd(ctx.Get());}
    check(spriteCap.spriteDiagnostics.size()==10 && spriteCap.spriteImageDeclined>=1,"sprite pair budget declines beyond first-frame cap");
    edvr::EyeDrawSnapshot wrongSpritePs;
    wrongSpritePs.capture(ctx.Get(),103,0,edvr::EyeDrawSnapshot::kSprite,0x1234ull,'X',6,1,0);
    check(wrongSpritePs.surfaces.empty() && wrongSpritePs.failures==1,
          "non-E508 sprite retains normal texture cap");
    edvr::EyeDrawSnapshot totalBudget;
    ComPtr<ID3D11Texture2D> secondTexture,thirdTexture;
    ComPtr<ID3D11ShaderResourceView> secondSrv,thirdSrv;
    hr(dev->CreateTexture2D(&spriteDesc,nullptr,&secondTexture));
    hr(dev->CreateTexture2D(&spriteDesc,nullptr,&thirdTexture));
    hr(dev->CreateShaderResourceView(secondTexture.Get(),&spriteView,&secondSrv));
    hr(dev->CreateShaderResourceView(thirdTexture.Get(),&spriteView,&thirdSrv));
    ID3D11ShaderResourceView* firstBudgetBinding=spriteSrv.Get(); ctx->PSSetShaderResources(0,1,&firstBudgetBinding);
    totalBudget.capture(ctx.Get(),103,0,edvr::EyeDrawSnapshot::kSprite,
                        edvr::EyeDrawSnapshot::kSpritePs,'X',6,1,0);
    ID3D11ShaderResourceView* secondBinding=secondSrv.Get(); ctx->PSSetShaderResources(0,1,&secondBinding);
    totalBudget.capture(ctx.Get(),103,1,edvr::EyeDrawSnapshot::kSprite,
                        edvr::EyeDrawSnapshot::kSpritePs,'X',6,1,0);
    ID3D11ShaderResourceView* thirdBinding=thirdSrv.Get(); ctx->PSSetShaderResources(0,1,&thirdBinding);
    totalBudget.capture(ctx.Get(),103,2,edvr::EyeDrawSnapshot::kSprite,
                        edvr::EyeDrawSnapshot::kSpritePs,'X',6,1,0);
    check(totalBudget.surfaces.size()==2 && totalBudget.textureBytes==2*capturedTextureBytes,
          "distinct exact E508 sources fit below total budget");
    check(totalBudget.draws[2].texture==UINT32_MAX,
          "third exact E508 source is refused by unchanged total budget");
    D3D11_TEXTURE2D_DESC oversizeDesc=spriteDesc; oversizeDesc.Width=2900; oversizeDesc.Height=2900;
    ComPtr<ID3D11Texture2D> oversizeTexture; hr(dev->CreateTexture2D(&oversizeDesc,nullptr,&oversizeTexture));
    ComPtr<ID3D11ShaderResourceView> oversizeSrv; hr(dev->CreateShaderResourceView(oversizeTexture.Get(),&spriteView,&oversizeSrv));
    ID3D11ShaderResourceView* oversizeBinding=oversizeSrv.Get(); ctx->PSSetShaderResources(0,1,&oversizeBinding);
    edvr::EyeDrawSnapshot oversizeSprite;
    oversizeSprite.capture(ctx.Get(),103,0,edvr::EyeDrawSnapshot::kSprite,
                            edvr::EyeDrawSnapshot::kSpritePs,'X',6,1,0);
    check(oversizeSprite.surfaces.empty() && oversizeSprite.failures==1,
          "exact E508 sprite still rejects sources above 32 MiB");
    bd.ByteWidth=64;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;ComPtr<ID3D11Buffer> vertices;hr(dev->CreateBuffer(&bd,nullptr,&vertices));
    UINT stride=16,offset=8;ctx->IASetVertexBuffers(0,1,vertices.GetAddressOf(),&stride,&offset);
    for(UINT i=0;i<2;++i){
        uint8_t data[64];for(UINT k=0;k<64;++k)data[k]=static_cast<uint8_t>(k+i*64);
        ctx->UpdateSubresource(vertices.Get(),0,nullptr,data,0,0);
        snap.capture(ctx.Get(),101,7+i,edvr::EyeDrawSnapshot::kHud,0,'X',3,1,0,2,-3);
    }
    uint8_t overwritten[64]{};ctx->UpdateSubresource(vertices.Get(),0,nullptr,overwritten,0,0);
    check(snap.vertexDraws==2&&snap.vertexBytes==112,"per-draw vertex snapshots counted");
    // The flight sprites use large base/start offsets into shared buffers.
    // Verify the capture preserves the actual draw window across reuse.
    bd.ByteWidth=320200;ComPtr<ID3D11Buffer> packed;hr(dev->CreateBuffer(&bd,nullptr,&packed));
    UINT packedStride=40,packedOffset=16;ctx->IASetVertexBuffers(1,1,packed.GetAddressOf(),&packedStride,&packedOffset);
    bd.ByteWidth=300100;bd.BindFlags=D3D11_BIND_INDEX_BUFFER;ComPtr<ID3D11Buffer> indices;hr(dev->CreateBuffer(&bd,nullptr,&indices));
    ctx->IASetIndexBuffer(indices.Get(),DXGI_FORMAT_R16_UINT,4);
    for(UINT i=0;i<2;++i){
        std::vector<uint8_t> data(320200);for(UINT k=0;k<data.size();++k)data[k]=static_cast<uint8_t>(k+i*37);
        ctx->UpdateSubresource(packed.Get(),0,nullptr,data.data(),0,0);
        ctx->UpdateSubresource(indices.Get(),0,nullptr,data.data(),0,0);
        snap.capture(ctx.Get(),102,12+i,edvr::EyeDrawSnapshot::kHud,0,'X',6,1,0,150000,8000);
        const auto& captured=snap.draws.back();
        check(captured.streams[1].captureOffset==320016&&captured.streams[1].copied==184,"vertex window starts at draw base");
        check(captured.streams[2].captureOffset==300004&&captured.streams[2].copied==12,"index window starts at draw start");
        check(captured.streams[1].offset==16&&captured.streams[2].offset==4,"binding offsets remain distinct from capture offsets");
    }
    const uint32_t capturedBytes=snap.vertexBytes;
    edvr::EyeDrawSnapshot effects;
    const char* effectCode="float4 main(float2 p:POSITION,float4 b:TEXCOORD):SV_Position{return float4(p,0,1)+b;}";
    ComPtr<ID3DBlob> effectBlob;hr(D3DCompile(effectCode,strlen(effectCode),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&effectBlob,nullptr));
    D3D11_INPUT_ELEMENT_DESC effectElements[2]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,16,D3D11_INPUT_PER_INSTANCE_DATA,1}};
    ComPtr<ID3D11InputLayout> effectLayout;hr(dev->CreateInputLayout(effectElements,2,effectBlob->GetBufferPointer(),effectBlob->GetBufferSize(),&effectLayout));
    edvr::EyeDrawSnapshot::rememberLayout(effectLayout.Get(),effectElements,2,0x9AEC596A2B036EA6ull);ctx->IASetInputLayout(effectLayout.Get());
    for(uint32_t frame:{100u,118u}) {
        effects.captureSourceMesh(ctx.Get(),frame,0x9AEC596A2B036EA6ull,0,'X',6,1,0,150000,8000);
        effects.captureSourceMesh(ctx.Get(),frame,0x3D05E7CF11AC9BEEull,0,'N',6,209,0,0,0);
    }
    check(effects.draws.size()==4 && effects.vertexDraws==4,"source effects capture vertices throughout eye run");
    check(effects.draws[0].streams[2].copied && !effects.draws[1].streams[2].copied,"nonindexed effects do not copy a stale bound index buffer");
    check(effects.meshBuffers.empty(),"source effects do not pretend to use weapon instance/bone pools");
    for(const auto& d:effects.draws) {
        check(d.ordinal==UINT32_MAX-2,"source effects have distinct ordinal");
        check(d.layout.size()==2 && d.layout[1].offset==16 && d.layout[1].classification==1,"source effects retain actual instance layout");
        check(d.streams[0].captureOffset==d.streams[0].offset && d.streams[1].captureOffset==d.streams[1].offset,"effect binding windows do not assume packed mesh layout");
    }
    effects.capture(ctx.Get(),119,0,0x9AEC596A2B036EA6ull,0,'X',6,1,0);
    check(effects.draws.size()==4,"effect diagnostic restricted to source image");
    effects.vertexBytes=32*1024*1024;
    effects.captureSourceMesh(ctx.Get(),119,0x9AEC596A2B036EA6ull,0,'X',6,1,0,0,0);
    check(effects.vertexDeclined>0 && effects.draws.size()==5,"effect vertex budget declines explicitly but retains constants");
    // A real HDR target changed between the two capture boundaries. The
    // Python fixture checks exact packed pixels after subsequent overwrites.
    edvr::EyeDrawSnapshot cropSnap;
    D3D11_TEXTURE2D_DESC cropDesc{};cropDesc.Width=1026;cropDesc.Height=1027;
    cropDesc.MipLevels=cropDesc.ArraySize=cropDesc.SampleDesc.Count=1;
    cropDesc.Format=DXGI_FORMAT_R11G11B10_FLOAT;cropDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> cropTarget;hr(dev->CreateTexture2D(&cropDesc,nullptr,&cropTarget));
    ComPtr<ID3D11RenderTargetView> cropRt;hr(dev->CreateRenderTargetView(cropTarget.Get(),nullptr,&cropRt));
    ctx->OMSetRenderTargets(1,cropRt.GetAddressOf(),nullptr);
    const float cropBefore[4]={1,.5f,.25f,1},cropAfter[4]={0,0,1,1},black[4]={};
    ctx->ClearRenderTargetView(cropRt.Get(),cropBefore);
    cropSnap.captureSourceMesh(ctx.Get(),400,0x0357BBB2DEE43C1Full,0,'X',14,1,0,0,0);
    ctx->ClearRenderTargetView(cropRt.Get(),cropAfter);cropSnap.captureEffectEnd(ctx.Get());
    ctx->ClearRenderTargetView(cropRt.Get(),black);cropSnap.captureEffectEnd(ctx.Get());
    check(cropSnap.effectImages.size()==2,"one before/after pair per source draw; no stale end capture");
    ctx->OMGetRenderTargets(1,&boundRt,nullptr);check(boundRt.Get()==cropRt.Get(),"effect crop preserves render target binding");
    cropSnap.captureSourceMesh(ctx.Get(),401,0x0357BBB2DEE43C1Full,0,'X',14,1,0,0,0);cropSnap.captureEffectEnd(ctx.Get());
    check(cropSnap.effectImages.size()==2,"effect images restricted to first watched source frame");
    cropSnap.effectImageBytes=edvr::EyeDrawSnapshot::kEffectImageBudget;
    cropSnap.captureSourceMesh(ctx.Get(),400,0x359BF8FF5CFAA4C3ull,0,'X',6,1,0,0,0);cropSnap.captureEffectEnd(ctx.Get());
    check(cropSnap.effectImageDeclined==1,"effect image budget decline visible without unmatched end capture");
    edvr::EyeDrawSnapshot night;
    ctx->PSSetConstantBuffers(1,1,&b);
    const char* nightVs="float4 main(float3 p:POSITION):SV_Position{return float4(p,1);}";
    ComPtr<ID3DBlob> nightBlob;hr(D3DCompile(nightVs,strlen(nightVs),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&nightBlob,nullptr));
    D3D11_INPUT_ELEMENT_DESC nightElement={"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    ComPtr<ID3D11InputLayout> nightLayout;hr(dev->CreateInputLayout(&nightElement,1,nightBlob->GetBufferPointer(),nightBlob->GetBufferSize(),&nightLayout));
    edvr::EyeDrawSnapshot::rememberLayout(nightLayout.Get(),&nightElement,1,edvr::EyeDrawSnapshot::kNight);ctx->IASetInputLayout(nightLayout.Get());
    bd.ByteWidth=96;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;ComPtr<ID3D11Buffer> nightVertices;hr(dev->CreateBuffer(&bd,nullptr,&nightVertices));
    UINT nightStride=12,nightOffset=12;ctx->IASetVertexBuffers(0,1,nightVertices.GetAddressOf(),&nightStride,&nightOffset);
    bd.ByteWidth=32;bd.BindFlags=D3D11_BIND_INDEX_BUFFER;ComPtr<ID3D11Buffer> nightIndices;hr(dev->CreateBuffer(&bd,nullptr,&nightIndices));
    ctx->IASetIndexBuffer(nightIndices.Get(),DXGI_FORMAT_R16_UINT,2);
    {
        edvr::EyeDrawSnapshot planets;
        ComPtr<ID3D11InputLayout> planetLayout;
        hr(dev->CreateInputLayout(&nightElement,1,nightBlob->GetBufferPointer(),nightBlob->GetBufferSize(),&planetLayout));
        for(uint64_t vs:{0x71DD9863DCFC0986ull,0x3530A6FD15EDE145ull}) {
            edvr::EyeDrawSnapshot::rememberLayout(planetLayout.Get(),&nightElement,1,vs);
            ctx->IASetInputLayout(planetLayout.Get());
            planets.capture(ctx.Get(),900,1,vs,0,'X',6,1,0,1,2);
        }
        planets.capture(ctx.Get(),901,1,0x71DD9863DCFC0986ull,0,'X',6,1,0,1,2);
        check(planets.draws.size()==3 && planets.vertexDraws==2,"planet transforms captured each frame, geometry only first frame");
        for(unsigned i=0;i<2;++i) {
            const auto& d=planets.draws[i];
            check(d.stage[0] && d.copied[0]==192 && d.layout.size()==1,"actual planet constants and input layout retained");
            check(d.streams[0].captureOffset==12 && d.streams[0].copied==84 && !d.streams[1].copied,
                  "planet POSITION uses original VB0 binding, no packed-mesh base assumption");
            check(d.streams[2].captureOffset==4 && d.streams[2].copied==12,"planet index window follows draw start");
        }
        check(planets.draws[2].stage[0] && !planets.draws[2].streams[0].copied,"later planet constants survive without repeated vertex copies");
        ctx->IASetInputLayout(nightLayout.Get());
    }
    {
        // The sun families are outside the planet/pool paths. Retain both
        // shader stages, actual unpacked layout, and changing draw constants.
        const uint64_t vs[]={0x0EE43D81E394E70Cull,0x4D516EF05C68FFA5ull,0xD95905C18B7FAD93ull,
            0x8BD7C37ABCEE7E45ull,0xD1281DF454A153ADull,0x5E417E9DF2E7F9E6ull,0x1F3AD1584D7FA3C8ull};
        const uint64_t ps[]={0,0x147E748F4CD3AE9Aull,0x5BCB6B95BE7C0700ull,
            0x94676B1FD0DF150Full,0x97DBC87FCAA429C4ull,0xBD801F2FB02522EBull,0xBA65C50BBA1ECCBBull};
        edvr::EyeDrawSnapshot solar;
        const char bytes[]="solar-bytecode";
        for(UINT family=0;family<7;++family) {
            edvr::EyeDrawSnapshot::rememberShader(vs[family],bytes,sizeof(bytes));
            if(ps[family])edvr::EyeDrawSnapshot::rememberShader(ps[family],bytes,sizeof(bytes));
            edvr::EyeDrawSnapshot::rememberLayout(nightLayout.Get(),&nightElement,1,vs[family]);
            for(UINT frame=0;frame<4;++frame) {
                float values[48];for(float& v:values)v=float(300+frame);
                ctx->UpdateSubresource(cb.Get(),0,nullptr,values,0,0);
                solar.capture(ctx.Get(),1000+frame,family,vs[family],ps[family],'X',6,1,0,1,2);
                const auto& d=solar.draws.back();
                check(d.layout.size()==1 && d.copied[0]==192,"solar draw constants/layout retained");
                check((d.streams[0].copied!=0)==(frame<3),"solar geometry limited to three frames");
                if(frame<3)check(d.streams[0].captureOffset==12 && d.streams[2].captureOffset==4,"solar original vertex/index binding offsets retained");
            }
        }
        float solarOverwritten[48]{};ctx->UpdateSubresource(cb.Get(),0,nullptr,solarOverwritten,0,0);
        check(solar.draws.size()==28 && solar.vertexDraws==21,"all solar families captured");
        wait_gpu(ctx.Get(),dev.Get());
        check(solar.write(ctx.Get(),(std::wstring(argv[1])+L".solar").c_str()) && !solar.failures,"solar draw-time GPU payload write");
        std::wstring dir=argv[1];dir.resize(dir.find_last_of(L"\\/"));
        check(solar.writeShaders(dir.c_str())==0,"solar vertex/pixel shader files including null-PS prepass");
        solar.draws.back().ps=123;
        check(solar.writeShaders(dir.c_str())==1,"unexpected solar PS reported missing, never silently substituted");
    }
    {
        // Census unknown-A/B settlement prop batches, 2026-09-21. The VS
        // alone enters sourceMesh(), but the paired PS dxbc needs BOTH the
        // creation-time retention gate (device_hook hookedCreatePS) and the
        // writeShaders pixel branch -- prove the write half per family here.
        edvr::EyeDrawSnapshot unknowns;
        const char bytes[]="unknown-bytecode";
        const uint64_t vs[]={edvr::EyeDrawSnapshot::kUnknownA,edvr::EyeDrawSnapshot::kUnknownB};
        const uint64_t ps[]={edvr::EyeDrawSnapshot::kUnknownAPs,edvr::EyeDrawSnapshot::kUnknownBPs};
        for(unsigned family=0;family<2;++family) {
            edvr::EyeDrawSnapshot::rememberShader(vs[family],bytes,sizeof(bytes));
            edvr::EyeDrawSnapshot::rememberShader(ps[family],bytes,sizeof(bytes));
            edvr::EyeDrawSnapshot::rememberLayout(nightLayout.Get(),&nightElement,1,vs[family]);
            unknowns.capture(ctx.Get(),1100,family,vs[family],ps[family],'X',6,1,0,0,0,true);
        }
        check(unknowns.draws.size()==2,"unknown-A/B draws retained via sourceMesh");
        std::wstring udir=argv[1];udir.resize(udir.find_last_of(L"\\/"));
        check(unknowns.writeShaders(udir.c_str())==0,"unknown-A/B vertex and pixel shader writes");
        for(const wchar_t* name:{L"vs_8056C9D5F22007F9.dxbc",L"ps_669CC896CA4AA988.dxbc",
                                L"vs_2684F02B9B0BB0DE.dxbc",L"ps_2376A8D9AA874372.dxbc"}) {
            FILE* f=nullptr;
            check(_wfopen_s(&f,(udir+L"\\"+name).c_str(),L"rb")==0 && f,"unknown shader file written");
            if(f)fclose(f);
        }
    }
    ComPtr<ID3D11Texture2D> nightTexture[5];ComPtr<ID3D11ShaderResourceView> nightView[5];
    UINT nightRow[5]{},nightRows[5]{};
    for(UINT slot=0;slot<5;++slot) {
        D3D11_TEXTURE2D_DESC nt{};nt.Width=slot==0?6:slot==3?1:slot==4?16:1026;
        nt.Height=slot==0 || slot==3?1:slot==4?16:1027;nt.MipLevels=nt.ArraySize=nt.SampleDesc.Count=1;
        nt.Format=slot<=1?DXGI_FORMAT_R32_TYPELESS:slot==2?DXGI_FORMAT_R10G10B10A2_TYPELESS:slot==3?DXGI_FORMAT_R8G8B8A8_TYPELESS:DXGI_FORMAT_BC4_UNORM;
        nt.BindFlags=D3D11_BIND_SHADER_RESOURCE;hr(dev->CreateTexture2D(&nt,nullptr,&nightTexture[slot]));
        D3D11_SHADER_RESOURCE_VIEW_DESC ns{};ns.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;ns.Texture2D.MipLevels=1;
        ns.Format=slot<=1?DXGI_FORMAT_R32_FLOAT:slot==2?DXGI_FORMAT_R10G10B10A2_UNORM:slot==3?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_BC4_UNORM;
        hr(dev->CreateShaderResourceView(nightTexture[slot].Get(),&ns,&nightView[slot]));ctx->PSSetShaderResources(slot,1,nightView[slot].GetAddressOf());
        nightRow[slot]=slot==4?32:nt.Width*4;nightRows[slot]=slot==4?4:nt.Height;
    }
    ComPtr<ID3D11SamplerState> nightSampler[2];
    for(UINT slot=0;slot<2;++slot) {
        D3D11_SAMPLER_DESC ns{};ns.Filter=slot?D3D11_FILTER_MIN_MAG_MIP_POINT:D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        ns.AddressU=ns.AddressV=ns.AddressW=slot?D3D11_TEXTURE_ADDRESS_CLAMP:D3D11_TEXTURE_ADDRESS_WRAP;
        ns.MipLODBias=slot?0:.25f;ns.MinLOD=slot?0.f:-2.f;ns.MaxLOD=slot?0.f:3.f;ns.MaxAnisotropy=1;ns.ComparisonFunc=D3D11_COMPARISON_ALWAYS;
        hr(dev->CreateSamplerState(&ns,&nightSampler[slot]));ctx->PSSetSamplers(slot,1,nightSampler[slot].GetAddressOf());
    }
    // Both eyes reuse the same inputs. The saved copies must differ, and
    // later overwrites must affect neither. Also exercise native BC4 rows.
    for(UINT eye=0;eye<2;++eye) {
        for(UINT slot=0;slot<5;++slot) {
            std::vector<uint8_t> data(nightRow[slot]*nightRows[slot],uint8_t(11+20*slot+eye*3));
            ctx->UpdateSubresource(nightTexture[slot].Get(),0,nullptr,data.data(),nightRow[slot],0);
        }
        uint8_t data[96];std::memset(data,31+eye,96);
        ctx->UpdateSubresource(nightVertices.Get(),0,nullptr,data,0,0);ctx->UpdateSubresource(nightIndices.Get(),0,nullptr,data,0,0);
        D3D11_VIEWPORT vp{float(eye),float(eye*2),float(1026-eye*2),float(1027-eye*2),0,1};ctx->RSSetViewports(1,&vp);
        ctx->ClearRenderTargetView(cropRt.Get(),cropBefore);
        night.capture(ctx.Get(),500,17+eye,edvr::EyeDrawSnapshot::kNight,edvr::EyeDrawSnapshot::kNightPs,'X',6,1,0,2,1);
        ctx->ClearRenderTargetView(cropRt.Get(),cropAfter);night.captureEffectEnd(ctx.Get());
        check(night.nightSampling.back().mask==3 && night.nightSampling.back().viewportCount==1,"night samplers and viewport present");
    }
    check(night.draws.size()==2 && night.effectImages.size()==14,"stereo night-vision pass captures all draw-time inputs and contribution");
    check(night.draws[0].source[1]==reinterpret_cast<uint64_t>(b) && night.draws[0].copied[1]==192,"night-vision captures actual PS b1, not unused VS b1");
    for(UINT slot=0;slot<5;++slot) {
        std::vector<uint8_t> data(nightRow[slot]*nightRows[slot],199);ctx->UpdateSubresource(nightTexture[slot].Get(),0,nullptr,data.data(),nightRow[slot],0);
        ComPtr<ID3D11ShaderResourceView> nv;ctx->PSGetShaderResources(slot,1,&nv);check(nv.Get()==nightView[slot].Get(),"night capture preserves PS SRV bindings");
    }
    for(UINT slot=0;slot<2;++slot) {ComPtr<ID3D11SamplerState> ns;ctx->PSGetSamplers(slot,1,&ns);check(ns.Get()==nightSampler[slot].Get(),"night capture preserves samplers");}
    uint8_t nightOverwrite[96]{};ctx->UpdateSubresource(nightVertices.Get(),0,nullptr,nightOverwrite,0,0);ctx->UpdateSubresource(nightIndices.Get(),0,nullptr,nightOverwrite,0,0);
    night.capture(ctx.Get(),501,17,edvr::EyeDrawSnapshot::kNight,edvr::EyeDrawSnapshot::kNightPs,'X',240,1,0);night.captureEffectEnd(ctx.Get());
    check(night.draws.size()==3 && night.effectImages.size()==14 && night.nightSampling.size()==2,"later night-vision frames retain constants without more input copies");
    edvr::EyeDrawSnapshot nightDeclined;nightDeclined.effectImageBytes=edvr::EyeDrawSnapshot::kEffectImageBudget;
    ID3D11ShaderResourceView* noNightViews[5]{};ctx->PSSetShaderResources(0,5,noNightViews);
    ID3D11SamplerState* noNightSamplers[2]{};ctx->PSSetSamplers(0,2,noNightSamplers);
    nightDeclined.capture(ctx.Get(),500,17,edvr::EyeDrawSnapshot::kNight,edvr::EyeDrawSnapshot::kNightPs,'X',6,1,0,2,1);nightDeclined.captureEffectEnd(ctx.Get());
    check(nightDeclined.effectImageDeclined==6 && nightDeclined.effectImages.empty() && nightDeclined.nightSampling[0].mask==0,"night missing inputs, absent samplers and image budget are explicit");
    ctx->OMSetRenderTargets(1,&r,nullptr);
    ID3D11Buffer* noBuffer=nullptr;UINT zero=0;ctx->IASetVertexBuffers(1,1,&noBuffer,&zero,&zero);ctx->IASetIndexBuffer(nullptr,DXGI_FORMAT_R16_UINT,0);
    edvr::EyeDrawSnapshot vscreenSnap;
    td.Width=td.Height=8;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source;ComPtr<ID3D11ShaderResourceView> sourceView;
    hr(dev->CreateTexture2D(&td,nullptr,&source));hr(dev->CreateShaderResourceView(source.Get(),nullptr,&sourceView));
    td.Format=DXGI_FORMAT_R32_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> sourceDepth;ComPtr<ID3D11DepthStencilView> sourceDsv;
    hr(dev->CreateTexture2D(&td,nullptr,&sourceDepth));
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    hr(dev->CreateDepthStencilView(sourceDepth.Get(),&dd,&sourceDsv));
    ctx->OMSetRenderTargets(1,&r,sourceDsv.Get());ctx->ClearDepthStencilView(sourceDsv.Get(),D3D11_CLEAR_DEPTH,.25f,0);
    vscreenSnap.captureSource(ctx.Get(),100,edvr::EyeDrawSnapshot::kScene,0,'X',6,1,0,0,0);
    vscreenSnap.captureSource(ctx.Get(),100,edvr::EyeDrawSnapshot::kScene,0,'X',6,1,0,0,0);
    check(vscreenSnap.draws.size()==1,"on-foot source camera captured once per frame");
    ctx->ClearDepthStencilView(sourceDsv.Get(),D3D11_CLEAR_DEPTH,.75f,0);ctx->OMSetRenderTargets(1,&r,nullptr);
    ctx->PSSetShaderResources(0,1,sourceView.GetAddressOf());
    vscreenSnap.capture(ctx.Get(),100,0,edvr::EyeDrawSnapshot::kVscreen,edvr::EyeDrawSnapshot::kVscreenPs,'X',6,1,0);
    vscreenSnap.capture(ctx.Get(),100,1,edvr::EyeDrawSnapshot::kVscreen,edvr::EyeDrawSnapshot::kVscreenPs,'X',6,1,0);
    ctx->ClearDepthStencilView(sourceDsv.Get(),D3D11_CLEAR_DEPTH,0,0);
    check(vscreenSnap.surfaces.size()==2,"on-foot source colour and completed depth copied once for both eyes");
    edvr::GuiDrawSnapshot gui;
    ctx->OMSetRenderTargets(1,&r,sourceDsv.Get());
    const FLOAT bg[4]={.2f,.4f,.6f,1};ctx->ClearRenderTargetView(rtv.Get(),bg);ctx->ClearDepthStencilView(sourceDsv.Get(),D3D11_CLEAR_DEPTH,.75f,0);
    ctx->VSSetConstantBuffers(2,1,cb.GetAddressOf());
    td.Width=td.Height=8;td.Format=DXGI_FORMAT_BC7_UNORM;td.MipLevels=3;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    uint8_t compressed[3][64]{};D3D11_SUBRESOURCE_DATA atlasData[3]{};
    for(int i=0;i<3;++i){std::memset(compressed[i],17+i,64);atlasData[i]={compressed[i],i?16u:32u,0};}
    ComPtr<ID3D11Texture2D> atlas;ComPtr<ID3D11ShaderResourceView> atlasSrv;hr(dev->CreateTexture2D(&td,atlasData,&atlas));hr(dev->CreateShaderResourceView(atlas.Get(),nullptr,&atlasSrv));ctx->PSSetShaderResources(1,1,atlasSrv.GetAddressOf());
    const char* guiVs="float4 main(float2 p:POSITION):SV_Position{return float4(p,0,1);}";
    ComPtr<ID3DBlob> code;hr(D3DCompile(guiVs,strlen(guiVs),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&code,nullptr));
    D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};ComPtr<ID3D11InputLayout> layout;hr(dev->CreateInputLayout(&element,1,code->GetBufferPointer(),code->GetBufferSize(),&layout));
    constexpr uint64_t guiHash=0x666EF0C4C616F67Eull,guiPs=0xC0C4E6413DF14E9Aull;
    edvr::GuiDrawSnapshot::rememberLayout(layout.Get(),&element,1,guiHash);ctx->IASetInputLayout(layout.Get());
    edvr::GuiDrawSnapshot::rememberShader(guiHash,code->GetBufferPointer(),code->GetBufferSize());edvr::GuiDrawSnapshot::rememberShader(guiPs,code->GetBufferPointer(),code->GetBufferSize());
    ctx->IASetIndexBuffer(indices.Get(),DXGI_FORMAT_R16_UINT,4);
    for(int i=0;i<2;++i){float values[48];for(float& v:values)v=float(7+i);ctx->UpdateSubresource(cb.Get(),0,nullptr,values,0,0);gui.capture(ctx.Get(),200,guiHash,guiPs,'X',6,1,150000,0,0);}
    gui.capture(ctx.Get(),201,guiHash,guiPs,'X',6,1,0,0,0);check(gui.count()==2,"GUI captures one source frame only");
    ctx->ClearRenderTargetView(rtv.Get(),bg);ctx->ClearDepthStencilView(sourceDsv.Get(),D3D11_CLEAR_DEPTH,0,0);
    edvr::EyeDrawSnapshot meshSnap;
    ComPtr<ID3D11Buffer> meshPool,meshBones,meshIds;
    ComPtr<ID3D11ShaderResourceView> meshPoolView,meshBoneView;
    bd={};bd.ByteWidth=672;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=336;
    hr(dev->CreateBuffer(&bd,nullptr,&meshPool));hr(dev->CreateShaderResourceView(meshPool.Get(),nullptr,&meshPoolView));
    bd.ByteWidth=48*24000;bd.StructureByteStride=48; // palette extends past the old 1 MiB cap
    hr(dev->CreateBuffer(&bd,nullptr,&meshBones));hr(dev->CreateShaderResourceView(meshBones.Get(),nullptr,&meshBoneView));
    bd.ByteWidth=32;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;bd.MiscFlags=bd.StructureByteStride=0;
    hr(dev->CreateBuffer(&bd,nullptr,&meshIds));
    ctx->VSSetShaderResources(33,1,meshPoolView.GetAddressOf());ctx->VSSetShaderResources(38,1,meshBoneView.GetAddressOf());
    UINT meshStride=8;ctx->IASetVertexBuffers(0,1,meshIds.GetAddressOf(),&meshStride,&zero);
    constexpr uint64_t meshVs=0x7B0DC42D383F694Cull;
    for(unsigned i=0;i<2;++i) {
        std::vector<uint8_t> pool(672,uint8_t(21+i)),bones(48*24000,uint8_t(31+i)),ids(32,uint8_t(41+i));
        ctx->UpdateSubresource(meshPool.Get(),0,nullptr,pool.data(),0,0);
        ctx->UpdateSubresource(meshBones.Get(),0,nullptr,bones.data(),0,0);
        ctx->UpdateSubresource(meshIds.Get(),0,nullptr,ids.data(),0,0);
        meshSnap.captureSourceMesh(ctx.Get(),300+i,meshVs,0,'X',6,1,2,0,0);
        meshSnap.captureSourceMesh(ctx.Get(),300+i,meshVs,0,'X',6,1,3,0,0);
    }
    check(meshSnap.draws.size()==4 && meshSnap.meshBuffers.size()==6 && !meshSnap.meshDeclined,"source buffer deduplication is per frame");
    ComPtr<ID3D11ShaderResourceView> checkPool;ctx->VSGetShaderResources(33,1,&checkPool);
    check(checkPool.Get()==meshPoolView.Get(),"source capture changed SRV binding");
    edvr::EyeDrawSnapshot eyeMesh;
    edvr::EyeDrawSnapshot::rememberLayout(layout.Get(),&element,1,meshVs);
    ctx->IASetInputLayout(layout.Get());
    D3D11_TEXTURE2D_DESC eyeDesc{};eyeDesc.Width=eyeDesc.Height=8;
    eyeDesc.MipLevels=eyeDesc.ArraySize=eyeDesc.SampleDesc.Count=1;
    eyeDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;eyeDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> otherEye;ComPtr<ID3D11RenderTargetView> otherEyeRtv;
    hr(dev->CreateTexture2D(&eyeDesc,nullptr,&otherEye));hr(dev->CreateRenderTargetView(otherEye.Get(),nullptr,&otherEyeRtv));
    eyeMesh.captureEyeMesh(ctx.Get(),699,0,edvr::EyeDrawSnapshot::kHud,0,'X',6,1,0,0,0);
    check(eyeMesh.draws.empty() && !eyeMesh.firstFrame,"non-mesh does not start eye mesh window");
    for(unsigned frame=700;frame<703;++frame)for(unsigned eye=0;eye<2;++eye) {
        ID3D11RenderTargetView* target=eye?otherEyeRtv.Get():rtv.Get();ctx->OMSetRenderTargets(1,&target,nullptr);
        const uint8_t value=uint8_t(51+(frame-700)*2+eye);
        std::vector<uint8_t> pool(672,value),bones(48*24000,uint8_t(value+10)),ids(32,uint8_t(value+20));
        ctx->UpdateSubresource(meshPool.Get(),0,nullptr,pool.data(),0,0);
        ctx->UpdateSubresource(meshBones.Get(),0,nullptr,bones.data(),0,0);
        ctx->UpdateSubresource(meshIds.Get(),0,nullptr,ids.data(),0,0);
        for(unsigned draw=0;draw<2;++draw) {
            float constants[48];for(float& v:constants)v=float(frame*10+eye*2+draw);
            ctx->UpdateSubresource(cb.Get(),0,nullptr,constants,0,0);
            eyeMesh.captureEyeMesh(ctx.Get(),frame,eye*100+draw,meshVs,0,'X',6,1,2+draw,150000,8000);
        }
        ctx->OMGetRenderTargets(1,&boundRt,nullptr);check(boundRt.Get()==target,"eye capture preserves target binding");
    }
    check(eyeMesh.draws.size()==12 && eyeMesh.meshBuffers.size()==18 && !eyeMesh.meshDeclined,
          "eye pools deduplicate within a frame and target, never across eyes");
    for(const auto& d:eyeMesh.draws)check(d.layout.size()==1,"eye mesh input layout retained");
    const auto eyeBytes=eyeMesh.meshBytes;
    for(unsigned frame:{699u,703u,720u})eyeMesh.captureEyeMesh(ctx.Get(),frame,1,meshVs,0,'X',6,1,0,0,0);
    check(eyeMesh.draws.size()==12 && eyeMesh.meshBytes==eyeBytes,"eye mesh capture stops after three consecutive frames");
    // Only the test waits, to make WARP deterministic. Production writes
    // after the eye ledger grace period and reports unavailable copies.
    // The eye-run depth capture (advanced.eye_depth_capture): each pass's
    // completed depth is staged the moment the NEXT pass's first draw is
    // noted; values written after the pass must not leak in, and later
    // overwrites of the source textures must not change the staged copies.
    {
        edvr::EyeDepthCapture depth;
        depth.configure(true);
        check(depth.enabled() && depth.count()==0,"depth capture configures on");
        D3D11_TEXTURE2D_DESC eyeDepthDesc{};eyeDepthDesc.Width=eyeDepthDesc.Height=8;
        eyeDepthDesc.MipLevels=eyeDepthDesc.ArraySize=eyeDepthDesc.SampleDesc.Count=1;
        eyeDepthDesc.Format=DXGI_FORMAT_R32_TYPELESS;eyeDepthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        D3D11_TEXTURE2D_DESC eyeColDesc{};eyeColDesc.Width=eyeColDesc.Height=8;
        eyeColDesc.MipLevels=eyeColDesc.ArraySize=eyeColDesc.SampleDesc.Count=1;
        eyeColDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;eyeColDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
        D3D11_DEPTH_STENCIL_VIEW_DESC eyeDsvDesc{};eyeDsvDesc.Format=DXGI_FORMAT_D32_FLOAT;
        eyeDsvDesc.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11Texture2D> depthTex[2];ComPtr<ID3D11DepthStencilView> depthDsv[2];
        ComPtr<ID3D11RenderTargetView> eyeRtv[2];
        for(int e=0;e<2;++e) {
            ComPtr<ID3D11Texture2D> t;hr(dev->CreateTexture2D(&eyeDepthDesc,nullptr,&t));depthTex[e]=t;
            hr(dev->CreateDepthStencilView(t.Get(),&eyeDsvDesc,&depthDsv[e]));
            ComPtr<ID3D11Texture2D> c;hr(dev->CreateTexture2D(&eyeColDesc,nullptr,&c));
            hr(dev->CreateRenderTargetView(c.Get(),nullptr,&eyeRtv[e]));
        }
        D3D11_BUFFER_DESC cb1Desc{};cb1Desc.ByteWidth=4096;cb1Desc.Usage=D3D11_USAGE_DEFAULT;
        cb1Desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> cb1;hr(dev->CreateBuffer(&cb1Desc,nullptr,&cb1));
        ctx->VSSetConstantBuffers(1,1,cb1.GetAddressOf());
        // THE 2026-09-21 FLIGHT REGRESSION: the engine runs offscreen stages
        // BEFORE the eye passes, and those draws reach the armed ledger with
        // their own depth targets. The caller's eye verdict (depthProbeScene
        // EyeOf: 0/1 for the scene pair, -1 otherwise) must send them to
        // nonEyeSkips without touching the two eye slots -- the first build
        // interned them instead and wrote zero files. Two offscreen targets
        // of different sizes, drawn before AND between the eye passes.
        D3D11_TEXTURE2D_DESC offDepthDesc{};offDepthDesc.Width=4;offDepthDesc.Height=4;
        offDepthDesc.MipLevels=offDepthDesc.ArraySize=offDepthDesc.SampleDesc.Count=1;
        offDepthDesc.Format=DXGI_FORMAT_R32_TYPELESS;offDepthDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> offTex[2];ComPtr<ID3D11DepthStencilView> offDsv[2];
        for(int o=0;o<2;++o) {
            if(o)offDepthDesc.Width=offDepthDesc.Height=16;
            ComPtr<ID3D11Texture2D> t;hr(dev->CreateTexture2D(&offDepthDesc,nullptr,&t));offTex[o]=t;
            hr(dev->CreateDepthStencilView(t.Get(),&eyeDsvDesc,&offDsv[o]));
        }
        const unsigned firstFrame=9000;
        for(unsigned frame=firstFrame;frame<=firstFrame+2;++frame) {
            depth.noteEyeDraw(ctx.Get(),frame,offDsv[0].Get(),false,-1);
            depth.noteEyeDraw(ctx.Get(),frame,offDsv[1].Get(),false,-1);
            for(unsigned eye=0;eye<2;++eye) {
                ID3D11RenderTargetView* eyeRt=eyeRtv[eye].Get();
                ctx->OMSetRenderTargets(1,&eyeRt,depthDsv[eye].Get());
                // A known value pattern per pass; the staged copy must keep it.
                float pattern[64];
                for(int y=0;y<8;++y)for(int x=0;x<8;++x)
                    pattern[y*8+x]=0.001f*float(x+y*8)+0.5f*float(eye)+float(frame-firstFrame)*0.01f;
                ctx->UpdateSubresource(depthTex[eye].Get(),0,nullptr,pattern,8*4,0);
                std::vector<float> cb1Data(1024);
                for(size_t k=0;k<cb1Data.size();++k)
                    cb1Data[k]=float(frame)+0.5f*float(eye)+0.0001f*float(k);
                ctx->UpdateSubresource(cb1.Get(),0,nullptr,cb1Data.data(),0,0);
                depth.noteEyeDraw(ctx.Get(),frame,depthDsv[eye].Get(),true,int(eye));
                // A second pool draw of the same pass must not replace the
                // first draw's constants block. A non-eye draw between the
                // passes must not end the open one either.
                depth.noteEyeDraw(ctx.Get(),frame,depthDsv[eye].Get(),true,int(eye));
                if(!eye)depth.noteEyeDraw(ctx.Get(),frame,offDsv[0].Get(),false,-1);
            }
        }
        // Frames firstFrame..firstFrame+1 kept (kMaxFrames=2); the third
        // frame's eye A declines as frame-cap, and the last open pass is
        // never followed by a switch, so it is not captured.
        check(depth.count()==4,"two frames of two completed eye passes staged");
        check(depth.declinedFrameCap>=1 && depth.declinedFormat==0 && depth.declinedBytes==0,
              "the frame-cap decline is reason-coded apart from format and bytes");
        check(depth.nonEyeSkips==9,"the frame's three non-eye phases skipped per frame, never staged");
        // Late overwrites of both source textures must not change the copies.
        float pollution[64];for(float& v:pollution)v=0.99f;
        ctx->UpdateSubresource(depthTex[0].Get(),0,nullptr,pollution,8*4,0);
        ctx->UpdateSubresource(depthTex[1].Get(),0,nullptr,pollution,8*4,0);
        std::vector<float> dead(1024,7.7f);ctx->UpdateSubresource(cb1.Get(),0,nullptr,dead.data(),0,0);
        wait_gpu(ctx.Get(),dev.Get());
        std::wstring depthDir=argv[1];depthDir.resize(depthDir.find_last_of(L"\\/"));
        check(depth.write(ctx.Get(),depthDir.c_str(),L"TEST")==4,"four depth files written");
        check(depth.failures==0 && depth.faults==0,"depth readbacks complete without faults");
        struct DepthHeader{char magic[8];uint32_t version,frame,eye,width,height,format,constFloats;};
        static_assert(sizeof(DepthHeader)==36,"tools/eye_depth_dump.py reads a 36-byte header");
        auto readDepthFile=[&](const wchar_t* name,DepthHeader& hd,std::vector<float>& constants,std::vector<float>& grid)->bool{
            FILE* f=nullptr;if(_wfopen_s(&f,(depthDir+L"\\"+name).c_str(),L"rb")||!f)return false;
            bool ok=fread(&hd,1,sizeof(hd),f)==sizeof(hd) && std::memcmp(hd.magic,"EDVRDEPT",8)==0;
            uint32_t depthBytes=0;uint32_t fileFaults=0;
            if(ok) {
                constants.resize(hd.constFloats);
                ok=hd.constFloats<=336 && (constants.empty() ||
                   fread(constants.data(),4,constants.size(),f)==constants.size());
            }
            if(ok) ok=fread(&depthBytes,4,1,f)==1 && depthBytes==hd.width*hd.height*4;
            if(ok) {grid.resize(depthBytes/4);ok=fread(grid.data(),4,grid.size(),f)==grid.size();}
            if(ok) ok=fread(&fileFaults,4,1,f)==1 && fileFaults==0;
            if(fclose(f)!=0)ok=false;
            return ok;
        };
        for(unsigned eye=0;eye<2;++eye) {
            wchar_t name[64];
            _snwprintf_s(name,64,_TRUNCATE,L"depth_TEST_f%u_%c.bin",firstFrame,eye?'B':'A');
            DepthHeader hd{};std::vector<float> constants,grid;
            check(readDepthFile(name,hd,constants,grid),"depth fixture file parses");
            check(hd.version==1 && hd.frame==firstFrame && hd.eye==eye && hd.width==8 && hd.height==8 &&
                  hd.format==int(DXGI_FORMAT_R32_FLOAT),
                  "depth fixture header round-trips with an R32_FLOAT payload format");
            check(constants.size()==336,"336 VS b1 floats from the first pool draw");
            // ~9000 in f32 quantizes to ~5e-4: expect the same float formula,
            // with tolerance wider than one ulp at this magnitude.
            const float wantConst=float(firstFrame)+0.5f*float(eye)+0.0001f*256.0f;
            check(std::fabs(constants[0]-wantConst)<1e-3f,"constants start at cb1[256]");
            const float wantRow=float(firstFrame)+0.5f*float(eye)+0.0001f*270.0f;
            check(std::fabs(constants[14]-wantRow)<1e-3f,"view-proj row slot present");
            check(grid.size()==64,"64 depth texels");
            for(int y=0;y<8;++y)for(int x=0;x<8;++x) {
                const float want=0.001f*float(x+y*8)+0.5f*float(eye);
                check(std::fabs(grid[y*8+x]-want)<1e-5f,"staged depth holds the pass-end pattern");
            }
        }
        // The flight rig's actual eye family: R32G8X24_TYPELESS with a
        // D32_FLOAT_S8X24_UINT view (the census line names the VIEW "D32").
        // It stages in its own typeless family and saves plain R32_FLOAT
        // texels, constants intact.
        edvr::EyeDepthCapture deep;
        deep.configure(true);
        D3D11_TEXTURE2D_DESC s8Desc{};s8Desc.Width=s8Desc.Height=8;
        s8Desc.MipLevels=s8Desc.ArraySize=s8Desc.SampleDesc.Count=1;
        s8Desc.Format=DXGI_FORMAT_R32G8X24_TYPELESS;s8Desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        D3D11_DEPTH_STENCIL_VIEW_DESC s8View{};s8View.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        s8View.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11Texture2D> s8Tex[2];ComPtr<ID3D11DepthStencilView> s8Dsv[2];
        for(int e=0;e<2;++e) {
            ComPtr<ID3D11Texture2D> t;hr(dev->CreateTexture2D(&s8Desc,nullptr,&t));s8Tex[e]=t;
            hr(dev->CreateDepthStencilView(t.Get(),&s8View,&s8Dsv[e]));
        }
        ctx->VSSetConstantBuffers(1,1,cb1.GetAddressOf());
        const unsigned s8Frame=9200;
        for(unsigned frame=s8Frame;frame<=s8Frame+1;++frame) {
            for(unsigned eye=0;eye<2;++eye) {
                // 8-byte texels: depth float, stencil byte, three pad bytes.
                uint8_t texels[64][8]{};
                for(int i=0;i<64;++i) {
                    const float d=0.25f+0.001f*float(i)+0.5f*float(eye)+float(frame-s8Frame)*0.01f;
                    std::memcpy(texels[i],&d,4);
                    texels[i][4]=static_cast<uint8_t>(i+eye);
                }
                ctx->UpdateSubresource(s8Tex[eye].Get(),0,nullptr,texels,8*8,0);
                std::vector<float> cb1Data(1024);
                for(size_t k=0;k<cb1Data.size();++k)
                    cb1Data[k]=float(frame)+0.5f*float(eye)+0.0001f*float(k);
                ctx->UpdateSubresource(cb1.Get(),0,nullptr,cb1Data.data(),0,0);
                deep.noteEyeDraw(ctx.Get(),frame,s8Dsv[eye].Get(),true,int(eye));
            }
        }
        // The last open pass (eye B of the second frame) has no following
        // switch, so three records: both eyes of the first frame, eye A of
        // the second.
        check(deep.count()==3 && deep.declined()==0 && deep.failures==0 && deep.faults==0,
              "R32G8X24 eye passes stage and decline nothing");
        wait_gpu(ctx.Get(),dev.Get());
        check(deep.write(ctx.Get(),depthDir.c_str(),L"S8")==3,"converted depth files written");
        for(unsigned eye=0;eye<2;++eye) {
            wchar_t name[64];
            _snwprintf_s(name,64,_TRUNCATE,L"depth_S8_f%u_%c.bin",s8Frame,eye?'B':'A');
            DepthHeader hd{};std::vector<float> constants,grid;
            check(readDepthFile(name,hd,constants,grid),"converted depth fixture parses");
            check(hd.format==int(DXGI_FORMAT_R32_FLOAT),"converted payload saved as plain R32_FLOAT");
            check(hd.width==8 && hd.height==8 && grid.size()==64,"converted grid shape");
            check(constants.size()==336,"constants intact alongside the conversion");
            for(int i=0;i<64;++i) {
                const float want=0.25f+0.001f*float(i)+0.5f*float(eye);
                check(std::fabs(grid[i]-want)<1e-5f,"converted texel keeps the depth float");
            }
        }
        // Off is a no-op: one bool, no copies, no files.
        edvr::EyeDepthCapture off;
        off.noteEyeDraw(ctx.Get(),firstFrame,depthDsv[0].Get(),true,0);
        check(off.count()==0 && !off.enabled(),"depth capture stays silent while off");
        // A non-eye draw between the two eye passes is skipped (not
        // declined), must not end the open pass, and a constants buffer
        // shorter than cb1[256..592) keeps an explicit zero-float block on
        // disk.
        edvr::EyeDepthCapture odd;
        odd.configure(true);
        ComPtr<ID3D11Texture2D> thirdTex;ComPtr<ID3D11DepthStencilView> thirdDsv;
        hr(dev->CreateTexture2D(&eyeDepthDesc,nullptr,&thirdTex));
        hr(dev->CreateDepthStencilView(thirdTex.Get(),&eyeDsvDesc,&thirdDsv));
        D3D11_BUFFER_DESC smallDesc{};smallDesc.ByteWidth=1024;smallDesc.Usage=D3D11_USAGE_DEFAULT;
        smallDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> smallCb1;hr(dev->CreateBuffer(&smallDesc,nullptr,&smallCb1));
        ctx->VSSetConstantBuffers(1,1,smallCb1.GetAddressOf());
        const unsigned oddFrame=9100;
        odd.noteEyeDraw(ctx.Get(),oddFrame,depthDsv[0].Get(),true,0);
        odd.noteEyeDraw(ctx.Get(),oddFrame,thirdDsv.Get(),true,-1);
        check(odd.nonEyeSkips==1 && odd.declined()==0 && odd.count()==0,
              "a non-eye draw mid-pass is skipped and does not end the open pass");
        odd.noteEyeDraw(ctx.Get(),oddFrame,depthDsv[1].Get(),true,1);
        check(odd.count()==1,"the eye pass ends at the next eye pass's first draw");
        odd.noteEyeDraw(ctx.Get(),oddFrame+1,depthDsv[0].Get(),true,0);
        check(odd.count()==2,"an already-staged pass is never enqueued twice");
        wait_gpu(ctx.Get(),dev.Get());
        check(odd.write(ctx.Get(),depthDir.c_str(),L"ODD")==2,"odd-run depth files written");
        wchar_t oddName[64];
        _snwprintf_s(oddName,64,_TRUNCATE,L"depth_ODD_f%u_A.bin",oddFrame);
        DepthHeader oh{};std::vector<float> oc,og;
        check(readDepthFile(oddName,oh,oc,og),"short-constants depth file parses");
        check(oh.constFloats==0 && oc.empty(),"constants block explicit when b1 is too short");
        // A multisampled depth target is declined, never half-copied.
        edvr::EyeDepthCapture msaa;
        msaa.configure(true);
        D3D11_TEXTURE2D_DESC msaaDesc=eyeDepthDesc;msaaDesc.SampleDesc.Count=4;
        ComPtr<ID3D11Texture2D> msaaTex;ComPtr<ID3D11DepthStencilView> msaaDsv;
        hr(dev->CreateTexture2D(&msaaDesc,nullptr,&msaaTex));
        D3D11_DEPTH_STENCIL_VIEW_DESC msaaView=eyeDsvDesc;
        msaaView.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMS;
        hr(dev->CreateDepthStencilView(msaaTex.Get(),&msaaView,&msaaDsv));
        msaa.noteEyeDraw(ctx.Get(),9200,msaaDsv.Get(),true,0);
        msaa.noteEyeDraw(ctx.Get(),9200,depthDsv[0].Get(),true,0);
        check(msaa.declinedFormat==1 && msaa.declinedBytes==0 && msaa.declinedFrameCap==0 &&
                  msaa.count()==0,
              "multisampled depth declined as format, reason-coded apart from the frame cap");
        odd.reset();
        check(odd.count()==0 && !odd.declined() && !odd.failures && !odd.nonEyeSkips,
              "depth capture reset clears the run");
        ctx->VSSetConstantBuffers(1,1,&b);
    }
    wait_gpu(ctx.Get(),dev.Get());
    check(snap.write(ctx.Get(), argv[1]), "snapshot write");
    check(meshSnap.write(ctx.Get(),(std::wstring(argv[1])+L".mesh").c_str()),"source mesh snapshot write");
    check(eyeMesh.write(ctx.Get(),(std::wstring(argv[1])+L".eyemesh").c_str()) && !eyeMesh.failures,"eye mesh snapshot payloads complete");
    eyeMesh.reset();check(eyeMesh.meshBuffers.empty() && eyeMesh.draws.empty() && !eyeMesh.firstFrame,"eye mesh reset releases its independent budget");
    check(!meshSnap.failures,"source mesh copies complete");
    meshSnap.meshBytes=edvr::EyeDrawSnapshot::kMeshBudget;
    meshSnap.captureSourceMesh(ctx.Get(),302,meshVs,0,'X',6,1,0,0,0);
    check(meshSnap.meshDeclined==3,"source buffer budget declines all new copies explicitly");
    meshSnap.reset();check(meshSnap.meshBuffers.empty() && !meshSnap.meshBytes,"source reset releases retained buffers");
    check(vscreenSnap.write(ctx.Get(),(std::wstring(argv[1])+L".vscreen").c_str()),"on-foot snapshot write");
    check(vscreenSnap.failures==0,"on-foot capture completed without missing copies");
    check(effects.write(ctx.Get(),(std::wstring(argv[1])+L".effects").c_str()) && effects.failures==0,"effect snapshot writes complete GPU copies");
    check(spriteSnap.write(ctx.Get(),(std::wstring(argv[1])+L".sprite").c_str()),"sprite boundary diagnostic writes complete");
    check(cropSnap.write(ctx.Get(),(std::wstring(argv[1])+L".crops").c_str()) && cropSnap.failures==0,"effect colour crop write");
    check(night.write(ctx.Get(),(std::wstring(argv[1])+L".night").c_str()) && night.failures==0,"night-vision draw-time data writes without missing copies");
    check(snap.failures == 0, "missing copies");
    const char shaderBytes[] = "captured-bytecode";
    edvr::EyeDrawSnapshot::rememberShader(edvr::EyeDrawSnapshot::kHolo, shaderBytes, sizeof(shaderBytes));
    edvr::EyeDrawSnapshot::rememberShader(edvr::EyeDrawSnapshot::kHud, shaderBytes, sizeof(shaderBytes));
    for(uint64_t hash:{edvr::EyeDrawSnapshot::kVscreen,edvr::EyeDrawSnapshot::kVscreenPs,edvr::EyeDrawSnapshot::kScene,edvr::EyeDrawSnapshot::kNight,edvr::EyeDrawSnapshot::kNightPs})
        edvr::EyeDrawSnapshot::rememberShader(hash,shaderBytes,sizeof(shaderBytes));
    std::wstring directory = argv[1]; directory.resize(directory.find_last_of(L"\\/"));
    check(gui.write(ctx.Get(),(std::wstring(argv[1])+L".gui").c_str(),directory.c_str()),"GUI source snapshot write");
    check(gui.failures==0 && gui.missingLayouts==0 && gui.declined==0,"GUI source payloads and layout complete");
    check(snap.writeShaders(directory.c_str()) == 0, "retained shader write");
    check(vscreenSnap.writeShaders(directory.c_str())==0,"on-foot vertex and pixel shader writes");
    check(night.writeShaders(directory.c_str())==0,"night-vision vertex and pixel shader writes");
    const std::wstring shaderPath = directory + L"\\vs_81216C77F90DEDD6.dxbc";
    FILE* shader = nullptr; check(_wfopen_s(&shader, shaderPath.c_str(), L"rb") == 0, "shader file");
    char saved[sizeof(shaderBytes)] = {};
    check(fread(saved, 1, sizeof(saved), shader) == sizeof(saved), "shader payload length");
    check(std::memcmp(saved, shaderBytes, sizeof(saved)) == 0, "shader payload content");
    fclose(shader);
    snap.capture(ctx.Get(),103,10,edvr::EyeDrawSnapshot::kHud,0,'X',3,1,0);
    check(snap.vertexBytes==capturedBytes,"vertex capture stops after three frames");
    ctx->IASetIndexBuffer(nullptr,DXGI_FORMAT_R16_UINT,0);
    snap.vertexBytes=32*1024*1024;snap.capture(ctx.Get(),102,11,edvr::EyeDrawSnapshot::kHud,0,'X',3,1,0);
    check(snap.vertexDeclined==1,"vertex byte budget declines explicitly");
    snap.draws.resize(edvr::EyeDrawSnapshot::kMaxDraws);
    snap.capture(ctx.Get(), 103, 6, edvr::EyeDrawSnapshot::kHolo, 0, 'X', 6, 1, 0);
    check(snap.dropped == 1 && snap.draws.size() == edvr::EyeDrawSnapshot::kMaxDraws, "capture cap");
    snap.reset(); check(snap.draws.empty() && snap.surfaces.empty() && !snap.dropped, "reset");
    cropSnap.reset();check(cropSnap.effectImages.empty() && !cropSnap.effectImageBytes &&
        !cropSnap.effectImageDeclined && cropSnap.pendingEffectImage==UINT32_MAX,"effect capture reset releases images and pending work");
    night.reset();check(night.nightSampling.empty() && night.effectImages.empty() && !night.vertexBytes,"night reset releases sampling and geometry");
    std::puts("GPU draw snapshot capture passed");
}
