#include "../../src/d3d11/eye_draw_snapshot.h"
#include "../../src/d3d11/eye_late_menu_capture.h"
#include "../../src/d3d11/gui_draw_snapshot.h"
#include "../../src/d3d11/eye_depth_capture.h"
#include "../../src/d3d11/eye_jump_lattice.h"
#include "../../src/d3d11/eye_jump_lattice_shader.h"
#include "../../src/d3d11/eye_airborne_capture.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;
namespace edvr { thread_local bool g_vrWorldInternal=false; thread_local bool g_flatComputeInternal=false; }
void check(bool ok, const char* why) {
    if (!ok) { std::printf("FAIL: %s\n", why); std::exit(1); }
}
void hr(HRESULT v) { check(SUCCEEDED(v), "D3D operation"); }
#include "menu_input_checks.h"
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
    const std::string menuInputFixture=menuInputChecks();
    {
        using S=edvr::EyeDrawSnapshot;edvr::LateMenuWindow menu;menu.arm(100);
        check(!menu.take(100,0,1,S::kLateMenuVs,360)&&menu.take(101,0,2,S::kScene,900)&&
              menu.take(101,0,3,S::kLateMenuVs,6)&&menu.take(101,0,4,S::kLateMenuBaseVs,360)&&
              menu.take(101,0,5,S::kLateMenuVs,360),
              "menu frame fixed after partial arm frame; earlier scene and siblings before either anchor retained");
        check(!menu.take(101,0,6,S::kScene,900)&&!menu.take(102,1,7,S::kLateMenuVs,360)&&
              menu.take(101,1,8,S::kScene,900),"scene reference precedes menu per eye and capture never moves to a later frame");
        for(uint32_t i=0;i<20;++i)menu.take(101,-1,10+i,S::kLateMenuVs,6);
        check(menu.take(101,0,40,S::kLateMenuVs,360)&&menu.take(101,1,41,S::kLateMenuVs,6)&&
              menu.take(101,1,42,S::kLateMenuVs,360)&&menu.declined[2][0][1]==16,
              "unknown-eye sibling saturation cannot consume either eye or either reserved 360-index anchor");
        edvr::LateMenuWindow astra;astra.arm(10);astra.take(11,0,0,S::kLateMenuVs,6);astra.take(11,1,1,S::kLateMenuVs,6);
        check(astra.seen[0][0][0]==0&&astra.seen[1][0][0]==0,"six-index-only stock station menu never claims a 360-index anchor");
    }
    check(edvr::EyeDrawSnapshot::f77Candidate(edvr::EyeDrawSnapshot::kF77HeldCandidateVs,0),
          "bounded held family admits only the allowlisted VS");
    check(edvr::EyeDrawSnapshot::f77Candidate(edvr::EyeDrawSnapshot::kF77OpticVs,edvr::EyeDrawSnapshot::kF77OpticPs) &&
          edvr::EyeDrawSnapshot::f77Candidate(edvr::EyeDrawSnapshot::kWeaponUiVs,edvr::EyeDrawSnapshot::kWeaponUiPs) &&
          edvr::EyeDrawSnapshot::weaponUiCandidate(edvr::EyeDrawSnapshot::kF77OpticVs,edvr::EyeDrawSnapshot::kF77OpticPs) &&
          !edvr::EyeDrawSnapshot::f77Candidate(edvr::EyeDrawSnapshot::kF77OpticVs,0x1234ull) &&
          !edvr::EyeDrawSnapshot::f77Candidate(0x1234ull,edvr::EyeDrawSnapshot::kF77OpticPs),
          "optic family requires exact VS and PS pair");
    check(edvr::EyeDrawSnapshot::f77RenderFrameEligible(100,100) &&
          edvr::EyeDrawSnapshot::f77RenderFrameEligible(100,102) &&
          !edvr::EyeDrawSnapshot::f77RenderFrameEligible(100,103) &&
          !edvr::EyeDrawSnapshot::f77RenderFrameEligible(0,100),
          "candidate capture is limited to first three nonzero render frames");
    check(edvr::eyeMeshVb0BindingStatus(false,0,0)==0 &&
          edvr::eyeMeshVb0BindingStatus(true,16,0)==4 &&
          edvr::eyeMeshVb0BindingStatus(true,8,16)==3 &&
          edvr::eyeMeshVb0BindingStatus(true,8,0)==1,
          "VB0 capture distinguishes absent binding, unsupported stride and offset, and a supported binding");
    check(edvr::EyeDrawSnapshot::kCandidateMaxDraws==1152 && edvr::EyeDrawSnapshot::kCandidateUiPerFrameEye==16 &&
          edvr::EyeDrawSnapshot::kCandidateUiReserve==16*3*16 && edvr::EyeDrawSnapshot::kCandidateNonUiMax==384 &&
          edvr::EyeDrawSnapshot::kCandidateMeshBudget==64*1024*1024 &&
          edvr::EyeDrawSnapshot::kCandidateInputBudget+edvr::EyeDrawSnapshot::kCandidateRoiBudget==136*1024*1024 &&
          edvr::EyeDrawSnapshot::kCandidateMaxOpticDraws==2,
          "candidate evidence reserves stay within approved independent bounds");
    {
        using Capture=edvr::EyeDrawSnapshot;
        uint32_t total=0,nonUi=0,ui[16][3]{};
        for(uint32_t i=0;i<Capture::kCandidateNonUiMax;++i){
            check(Capture::candidateCapacityAvailable(false,0,nonUi,total),"non-UI rows fit their isolated reservation");
            ++total;++nonUi;
        }
        for(uint32_t frame=0;frame<16;++frame)for(uint32_t eye=0;eye<3;++eye)for(uint32_t n=0;n<Capture::kCandidateUiPerFrameEye;++n){
            check(Capture::candidateCapacityAvailable(true,ui[frame][eye],nonUi,total),"each UI frame/eye bucket retains full capacity after non-UI saturation");
            ++ui[frame][eye];++total;
        }
        check(total==Capture::kCandidateMaxDraws&&
              !Capture::candidateCapacityAvailable(true,ui[0][0],nonUi,total)&&
              !Capture::candidateCapacityAvailable(false,0,nonUi,total),
              "bounded candidate window fills exactly all 384 non-UI plus 768 UI rows");
        check(!Capture::candidateCapacityAvailable(true,Capture::kCandidateUiPerFrameEye,nonUi,0)&&
              Capture::candidateCapacityAvailable(true,0,nonUi,0),
              "an overfull early UI bucket cannot consume another frame/eye reservation");
    }
    {
        using namespace edvr::eye_airborne_capture;
        Request request;
        check(request.poll(1000,1000,true,true,true,90,true,false)==Decision::None,
              "manual already-airborne run does not arm an automatic duplicate");
        request.arm(1000,90);
        check(request.poll(1100,1000,true,false,false,0,false,false)==Decision::Stale&&
              request.poll(1200,1000,true,false,false,0,false,false)==Decision::None,
              "first invalid or stale snapshot is reported once and never triggers");
        check(request.poll(1300,1000,true,true,true,90,false,false)==Decision::None&&
              request.poll(1400,1000,true,true,true,90,true,false)==Decision::None,
              "grounded and duplicate-generation samples cannot trigger");
        check(request.poll(1500,1000,true,true,true,91,true,true)==Decision::DeferredBusy&&
              request.state==State::Deferred,
              "airborne edge during a run is explicitly deferred");
        check(request.poll(1600,1000,true,true,true,92,false,false)==Decision::None&&
              request.poll(1700,1000,true,true,true,93,true,false)==Decision::Trigger&&
              request.poll(1800,1000,true,true,true,94,true,false)==Decision::None,
              "busy jump that lands waits for the next fresh jump and triggers exactly once");
        request.arm(1900,20);
        check(request.poll(2000,1000,true,true,true,21,true,false)==Decision::Trigger,
              "fresh airborne sample requests one companion start");
        request.retryAfterStartRefusal(21);
        check(request.state==State::Deferred&&
              request.poll(2100,1000,true,true,true,21,true,false)==Decision::None&&
              request.poll(2200,1000,true,true,true,22,true,false)==Decision::Trigger,
              "begin refusal retains the request and requires a newer airborne sample");
        request.arm(2000,10);
        check(request.poll(32000,1000,true,true,true,11,true,false)==Decision::Expired&&request.state==State::Expired,
              "airborne companion expires at its fixed 30-second bound");
        request.arm(40000,10);
        check(request.poll(40100,1000,false,true,true,11,true,false)==Decision::Cancelled&&request.state==State::Cancelled,
              "native on-foot exit cancels the one-shot request");
    }
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    {
        constexpr UINT sw=1024,sh=768;
        std::vector<uint8_t> grid(size_t(sw)*sh*4);
        for(UINT y=0;y<sh;++y)for(UINT x=0;x<sw;++x){const size_t at=(size_t(y)*sw+x)*4;grid[at]=uint8_t(x);grid[at+1]=uint8_t(y);grid[at+2]=0xa5;grid[at+3]=0xff;}
        D3D11_TEXTURE2D_DESC td{};td.Width=sw;td.Height=sh;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{grid.data(),sw*4,0};ComPtr<ID3D11Texture2D> source;hr(dev->CreateTexture2D(&td,&initial,&source));
        ComPtr<ID3D11ShaderResourceView> sourceView;hr(dev->CreateShaderResourceView(source.Get(),nullptr,&sourceView));
        D3D11_TEXTURE2D_DESC sentinelDesc=td;sentinelDesc.Width=sentinelDesc.Height=1;const uint8_t sentinelPixel[4]={9,8,7,6};D3D11_SUBRESOURCE_DATA sentinelInit{sentinelPixel,4,0};
        ComPtr<ID3D11Texture2D> sentinelTexture;hr(dev->CreateTexture2D(&sentinelDesc,&sentinelInit,&sentinelTexture));ComPtr<ID3D11ShaderResourceView> sentinelSrv;hr(dev->CreateShaderResourceView(sentinelTexture.Get(),nullptr,&sentinelSrv));
        const char* sentinelSource="[numthreads(1,1,1)]void main(){}";ComPtr<ID3DBlob> sentinelCode;hr(D3DCompile(sentinelSource,strlen(sentinelSource),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&sentinelCode,nullptr));
        ComPtr<ID3D11ComputeShader> sentinelShader;hr(dev->CreateComputeShader(sentinelCode->GetBufferPointer(),sentinelCode->GetBufferSize(),nullptr,&sentinelShader));
        D3D11_BUFFER_DESC counterDesc{};counterDesc.ByteWidth=16*4;counterDesc.Usage=D3D11_USAGE_DEFAULT;counterDesc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;counterDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;counterDesc.StructureByteStride=4;
        ComPtr<ID3D11Buffer> counterBuffer;hr(dev->CreateBuffer(&counterDesc,nullptr,&counterBuffer));D3D11_UNORDERED_ACCESS_VIEW_DESC counterViewDesc{};counterViewDesc.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;counterViewDesc.Format=DXGI_FORMAT_UNKNOWN;counterViewDesc.Buffer.NumElements=16;counterViewDesc.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_APPEND;
        ComPtr<ID3D11UnorderedAccessView> counterUav;hr(dev->CreateUnorderedAccessView(counterBuffer.Get(),&counterViewDesc,&counterUav));
        ID3D11ShaderResourceView* oldSrv=sentinelSrv.Get();ID3D11UnorderedAccessView* oldUav=counterUav.Get();UINT initialCounter=7;
        ctx->CSSetShader(sentinelShader.Get(),nullptr,0);ctx->CSSetShaderResources(0,1,&oldSrv);ctx->CSSetUnorderedAccessViews(0,1,&oldUav,&initialCounter);
        ComPtr<ID3D11Texture2D> sampled;check(edvr::EyeDrawSnapshot::pointSampleUiTextureForTest(ctx.Get(),dev.Get(),sourceView.Get(),sw,sh,&sampled),"bounded full-image point sampler runs on WARP");wait_gpu(ctx.Get(),dev.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};hr(ctx->Map(sampled.Get(),0,D3D11_MAP_READ,0,&mapped));
        auto pixel=[&](UINT x,UINT y){return static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch+size_t(x)*4;};
        bool gridMatches=true;for(const auto xy:{std::pair<UINT,UINT>{0,0},{127,91},{383,383}}){const UINT sx=((xy.first*2+1)*sw)/768,sy=((xy.second*2+1)*sh)/768;const uint8_t* got=pixel(xy.first,xy.second);gridMatches=gridMatches&&got[0]==uint8_t(sx)&&got[1]==uint8_t(sy)&&got[2]==0xa5&&got[3]==0xff;}
        ctx->Unmap(sampled.Get(),0);check(gridMatches,"full-source center-point mapping preserves synthetic UV-grid coordinates");
        ComPtr<ID3D11ComputeShader> restoredShader;ComPtr<ID3D11ShaderResourceView> restoredSrv;ComPtr<ID3D11UnorderedAccessView> restoredUav;
        ctx->CSGetShader(&restoredShader,nullptr,nullptr);ctx->CSGetShaderResources(0,1,&restoredSrv);ctx->CSGetUnorderedAccessViews(0,1,&restoredUav);
        check(restoredShader.Get()==sentinelShader.Get()&&restoredSrv.Get()==sentinelSrv.Get()&&restoredUav.Get()==counterUav.Get(),"point sampler restores compute shader/SRV/UAV bindings");
        D3D11_BUFFER_DESC countDesc{};countDesc.ByteWidth=4;countDesc.Usage=D3D11_USAGE_STAGING;countDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Buffer> countStage;hr(dev->CreateBuffer(&countDesc,nullptr,&countStage));
        ctx->CopyStructureCount(countStage.Get(),0,counterUav.Get());wait_gpu(ctx.Get(),dev.Get());D3D11_MAPPED_SUBRESOURCE countMap{};hr(ctx->Map(countStage.Get(),0,D3D11_MAP_READ,0,&countMap));const uint32_t preservedCounter=*static_cast<const uint32_t*>(countMap.pData);ctx->Unmap(countStage.Get(),0);
        check(preservedCounter==7,"point sampler preserves the prebound UAV append counter");
        check(!edvr::g_vrWorldInternal&&!edvr::g_flatComputeInternal,"point sampler leaves internal render scopes balanced");
    }
    check(edvr::eye_jump_lattice::runFits(5,3) && !edvr::eye_jump_lattice::runFits(4032,3896),
          "jump lattice admits irregular input and refuses over-budget production dimensions");
    {
        using namespace edvr::eye_jump_lattice;
        Run refusal;refusal.arm();refusal.refuse(0,11,22,33,BudgetRefused);
        check(refusal.rows[0].status==BudgetRefused&&refusal.bytes==0&&!refusal.rows[0].skin,
              "budget refusal records status and allocates/writes no payload");
        Row ready{};ready.status=Staged;ready.sceneFrame=11;ready.renderFrame=22;ready.epoch=33;
        uint64_t sequence=0;
        check(finalSequenceMatch(ready,true,11,22,33,71,71,sequence)&&sequence==71,
              "lattice finalizes only after a matching two-eye sequence and schedule identity");
        check(!finalSequenceMatch(ready,true,11,22,33,71,72,sequence)&&sequence==0&&
              !finalSequenceMatch(ready,false,11,22,33,71,71,sequence)&&
              !finalSequenceMatch(ready,true,11,22,34,71,71,sequence),
              "sequence mismatch, absent schedule, and epoch mismatch remain unfinalized");
    }
    {
        ComPtr<ID3DBlob> code,errors;hr(D3DCompile(edvr::kEyeJumpLatticeHlsl,sizeof(edvr::kEyeJumpLatticeHlsl)-1,
            "eye_jump_lattice_test",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors));
        ComPtr<ID3D11ComputeShader> shader;hr(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
        auto source=[&](DXGI_FORMAT format,const void* data,UINT pitch,ComPtr<ID3D11ShaderResourceView>& view){
            D3D11_TEXTURE2D_DESC desc{};desc.Width=5;desc.Height=3;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=format;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initial{data,pitch,0};ComPtr<ID3D11Texture2D> texture;hr(dev->CreateTexture2D(&desc,&initial,&texture));hr(dev->CreateShaderResourceView(texture.Get(),nullptr,&view));};
        uint16_t skin[5*3*4],motion[5*3*2];float slots[5*3*2];
        for(unsigned i=0;i<15;++i){skin[i*4]=0x3c00;skin[i*4+1]=0x4000;skin[i*4+2]=0x4200;skin[i*4+3]=0x3c00;motion[i*2]=0x3800;motion[i*2+1]=0xb800;slots[i*2]=float(i);slots[i*2+1]=float(i)+0.25f;}
        skin[14*4]=0x4b40;skin[14*4+1]=0x4b80;motion[14*2]=0x4300;motion[14*2+1]=0xc700;
        uint32_t slotBits[2]={0x7fc12345u,0x80000000u};std::memcpy(slots+((2*5+4)*2),slotBits,sizeof(slotBits));
        ComPtr<ID3D11ShaderResourceView> skinView,motionView,slotView;source(DXGI_FORMAT_R16G16B16A16_FLOAT,skin,sizeof(uint16_t)*5*4,skinView);source(DXGI_FORMAT_R16G16_FLOAT,motion,sizeof(uint16_t)*5*2,motionView);source(DXGI_FORMAT_R32G32_FLOAT,slots,sizeof(float)*5*2,slotView);
        edvr::eye_jump_lattice::Run run;run.arm();
        ID3D11ShaderResourceView* originalSrv=slotView.Get();ctx->CSSetShaderResources(0,1,&originalSrv);ctx->CSSetShader(nullptr,nullptr,0);
        bool allRows=true;for(uint32_t i=0;i<edvr::eye_jump_lattice::Count;++i)
            allRows=run.stage(ctx.Get(),shader.Get(),i,11+i,22+i,33,true,skinView.Get(),motionView.Get(),slotView.Get())==edvr::eye_jump_lattice::Staged&&allRows;
        check(allRows&&run.count==edvr::eye_jump_lattice::Count&&run.bytes==uint64_t(edvr::eye_jump_lattice::Count)*120,
              "jump lattice stages sixteen consecutive rows while reusing scratch dimensions");wait_gpu(ctx.Get(),dev.Get());
        ComPtr<ID3D11ShaderResourceView> restoredSrv;ctx->CSGetShaderResources(0,1,&restoredSrv);ComPtr<ID3D11ComputeShader> restoredShader;ctx->CSGetShader(&restoredShader,nullptr,nullptr);
        check(restoredSrv.Get()==slotView.Get()&&!restoredShader,"jump lattice restores compute SRV and shader bindings");
        const auto& row=run.rows[0];check(row.width==5&&row.height==3&&row.sampleWidth==3&&row.sampleHeight==2&&row.bytes==120,
              "irregular odd dimensions use ceil-half sample shape");
        D3D11_MAPPED_SUBRESOURCE a{},b{},c{};hr(ctx->Map(row.skin.Get(),0,D3D11_MAP_READ,0,&a));hr(ctx->Map(row.motion.Get(),0,D3D11_MAP_READ,0,&b));hr(ctx->Map(row.slots.Get(),0,D3D11_MAP_READ,0,&c));
        const auto* es=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(a.pData)+a.RowPitch);const auto* ms=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(b.pData)+b.RowPitch);const auto* ss=reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(c.pData)+c.RowPitch);
        check(es[4]==0x4b804b40u && ms[2]==0xc7004300u && ss[4]==slotBits[0]&&ss[5]==slotBits[1],
              "lattice includes last valid pixel and preserves slot NaN/sign bits");
        ctx->Unmap(row.skin.Get(),0);ctx->Unmap(row.motion.Get(),0);ctx->Unmap(row.slots.Get(),0);
        run.reset();
    }
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
    // The same panels with Elite's Disable GUI effects on (vs 1989E6D3B405FDE0 / ps EAB8A1C95A13FFBE, holo_material.h) are watched and their
    // interface surface captured from t2 (holoSurfaceSlot's lit slot), exactly as the stock pair's is: an eye run on such a rig otherwise
    // misses the panels and the vertex shader's bytes. t1 is cleared so only t2 can be the source.
    check(edvr::EyeDrawSnapshot::watches(edvr::kHoloGuiFxOffVs) && edvr::EyeDrawSnapshot::watches(edvr::EyeDrawSnapshot::kHolo) &&
              !edvr::EyeDrawSnapshot::watches(edvr::kHoloGuiFxOffPs) && !edvr::EyeDrawSnapshot::watches(edvr::kHoloGuiFxOffVs ^ 1ull),
          "the eye run watches the Disable-GUI-effects holo vertex shader beside the stock one, and no neighbouring hash");
    ctx->PSSetShaderResources(1,1,&none);
    edvr::EyeDrawSnapshot fxSnap;
    fxSnap.capture(ctx.Get(),104,0,edvr::kHoloGuiFxOffVs,edvr::kHoloGuiFxOffPs,'X',6,1,0);
    check(fxSnap.draws.size()==1 && fxSnap.surfaces.size()==1 && fxSnap.draws[0].texture==0 && fxSnap.draws[0].vs==edvr::kHoloGuiFxOffVs &&
              fxSnap.draws[0].ps==edvr::kHoloGuiFxOffPs,
          "the Disable-GUI-effects holo draw is captured with its interface surface from t2");
    edvr::EyeDrawSnapshot fxMiss;
    fxMiss.capture(ctx.Get(),105,0,edvr::kHoloGuiFxOffVs ^ 1ull,edvr::kHoloGuiFxOffPs,'X',6,1,0);
    check(fxMiss.draws.empty() && fxMiss.surfaces.empty(), "a vertex shader one bit from it is not watched");
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
    // Exercise the production candidate capture branch with the observed
    // multi-megabyte base vertex, nonzero IA offsets and a nonzero start index.
    ComPtr<ID3D11InputLayout> savedOpticLayout;ctx->IAGetInputLayout(&savedOpticLayout);
    ID3D11Buffer* savedVbRaw=nullptr;UINT savedStride=0,savedOffset=0;ctx->IAGetVertexBuffers(1,1,&savedVbRaw,&savedStride,&savedOffset);
    ComPtr<ID3D11Buffer> savedVb;savedVb.Attach(savedVbRaw);
    ID3D11Buffer* savedIbRaw=nullptr;DXGI_FORMAT savedIndexFormat{};UINT savedIndexOffset=0;ctx->IAGetIndexBuffer(&savedIbRaw,&savedIndexFormat,&savedIndexOffset);
    ComPtr<ID3D11Buffer> savedIb;savedIb.Attach(savedIbRaw);
    ID3D11ShaderResourceView* savedSrvs[3]{};ctx->PSGetShaderResources(1,3,savedSrvs);
    ComPtr<ID3D11ShaderResourceView> savedSrv1,savedSrv2,savedSrv3;savedSrv1.Attach(savedSrvs[0]);savedSrv2.Attach(savedSrvs[1]);savedSrv3.Attach(savedSrvs[2]);
    const char* opticCode="float4 main(uint4 a:PACKEDVERTEXDATAA,uint2 c:PACKEDVERTEXDATAC):SV_Position{return float4(asfloat(a.xy),0,1)+float4(asfloat(c),0,0);}";
    ComPtr<ID3DBlob> opticBlob;hr(D3DCompile(opticCode,strlen(opticCode),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&opticBlob,nullptr));
    D3D11_INPUT_ELEMENT_DESC opticElements[2]={{"PACKEDVERTEXDATAA",0,DXGI_FORMAT_R32G32B32A32_UINT,1,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"PACKEDVERTEXDATAC",0,DXGI_FORMAT_R32G32_UINT,1,16,D3D11_INPUT_PER_VERTEX_DATA,0}};
    ComPtr<ID3D11InputLayout> opticLayout;hr(dev->CreateInputLayout(opticElements,2,opticBlob->GetBufferPointer(),opticBlob->GetBufferSize(),&opticLayout));
    edvr::EyeDrawSnapshot::rememberLayout(opticLayout.Get(),opticElements,2,edvr::EyeDrawSnapshot::kF77OpticVs);
    ctx->IASetInputLayout(opticLayout.Get());
    const uint32_t opticBase=2'955'444,opticStride=40,opticBindingOffset=24;UINT opticStrideBinding=opticStride;
    const uint64_t opticVertexBegin=uint64_t(opticBindingOffset)+uint64_t(opticBase+2)*opticStride;
    const UINT opticVertexSize=static_cast<UINT>(opticVertexBegin+4*opticStride);
    D3D11_BUFFER_DESC opticVbDesc{};opticVbDesc.ByteWidth=opticVertexSize;opticVbDesc.Usage=D3D11_USAGE_DEFAULT;opticVbDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    ComPtr<ID3D11Buffer> opticVb;hr(dev->CreateBuffer(&opticVbDesc,nullptr,&opticVb));
    uint8_t opticVertexBytes[4*opticStride];std::memset(opticVertexBytes,0xA7,sizeof(opticVertexBytes));
    D3D11_BOX opticVbBox{static_cast<UINT>(opticVertexBegin),0,0,static_cast<UINT>(opticVertexBegin+sizeof(opticVertexBytes)),1,1};
    ctx->UpdateSubresource(opticVb.Get(),0,&opticVbBox,opticVertexBytes,0,0);
    const uint16_t opticIndexValues[18]={9,9,9,9,5,2,5,4,2,5,9,9,9,9,9,9,9,9};
    D3D11_BUFFER_DESC opticIbDesc{};opticIbDesc.ByteWidth=sizeof(opticIndexValues);opticIbDesc.Usage=D3D11_USAGE_DEFAULT;opticIbDesc.BindFlags=D3D11_BIND_INDEX_BUFFER;
    ComPtr<ID3D11Buffer> opticIb;hr(dev->CreateBuffer(&opticIbDesc,nullptr,&opticIb));
    ctx->UpdateSubresource(opticIb.Get(),0,nullptr,opticIndexValues,0,0);
    UINT opticOffset=opticBindingOffset;ctx->IASetVertexBuffers(1,1,opticVb.GetAddressOf(),&opticStrideBinding,&opticOffset);
    ctx->IASetIndexBuffer(opticIb.Get(),DXGI_FORMAT_R16_UINT,4);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    edvr::EyeDrawSnapshot opticCapture;
    opticCapture.capture(ctx.Get(),91,7,edvr::EyeDrawSnapshot::kF77OpticVs,edvr::EyeDrawSnapshot::kF77OpticPs,'I',6,1,0,2,static_cast<int32_t>(opticBase),false,false,true,700);
    check(opticCapture.draws.size()==1 && opticCapture.draws[0].streams[1].captureOffset==opticVertexBegin &&
          opticCapture.draws[0].streams[1].copied==4*opticStride && opticCapture.draws[0].streams[2].captureOffset==8 &&
          opticCapture.draws[0].streams[2].copied==12,"production optic capture resolves indexed min/max vertex window from actual WARP buffers");
    wait_gpu(ctx.Get(),dev.Get());D3D11_MAPPED_SUBRESOURCE opticMapped{};
    hr(ctx->Map(opticCapture.draws[0].streams[1].stage.Get(),0,D3D11_MAP_READ,0,&opticMapped));
    check(opticMapped.pData && static_cast<const uint8_t*>(opticMapped.pData)[0]==0xA7 &&
          static_cast<const uint8_t*>(opticMapped.pData)[sizeof(opticVertexBytes)-1]==0xA7,"WARP indexed optic vertex bytes match the selected large-base window");
    ctx->Unmap(opticCapture.draws[0].streams[1].stage.Get(),0);
    const uint16_t negativeBaseIndices[18]={9,9,9,9,1,2,3,2,1,4,9,9,9,9,9,9,9,9};
    ComPtr<ID3D11Buffer> negativeBaseIb;hr(dev->CreateBuffer(&opticIbDesc,nullptr,&negativeBaseIb));
    ctx->UpdateSubresource(negativeBaseIb.Get(),0,nullptr,negativeBaseIndices,0,0);ctx->IASetIndexBuffer(negativeBaseIb.Get(),DXGI_FORMAT_R16_UINT,4);
    edvr::EyeDrawSnapshot negativeBaseCapture;
    negativeBaseCapture.capture(ctx.Get(),91,8,edvr::EyeDrawSnapshot::kF77OpticVs,edvr::EyeDrawSnapshot::kF77OpticPs,'I',6,1,0,2,-1,false,false,true,700);
    check(negativeBaseCapture.draws.size()==1 && negativeBaseCapture.draws[0].streams[1].captureOffset==opticBindingOffset &&
          negativeBaseCapture.draws[0].streams[1].copied==4*opticStride,"negative base with positive indices resolves a valid WARP vertex window");
    D3D11_TEXTURE2D_DESC opticMaterialDesc{};opticMaterialDesc.Width=opticMaterialDesc.Height=1024;opticMaterialDesc.MipLevels=0;
    opticMaterialDesc.ArraySize=8;opticMaterialDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;opticMaterialDesc.SampleDesc.Count=1;
    opticMaterialDesc.Usage=D3D11_USAGE_DEFAULT;opticMaterialDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> opticMaterial;hr(dev->CreateTexture2D(&opticMaterialDesc,nullptr,&opticMaterial));
    ComPtr<ID3D11ShaderResourceView> opticMaterialSrv;hr(dev->CreateShaderResourceView(opticMaterial.Get(),nullptr,&opticMaterialSrv));
    ID3D11ShaderResourceView* opticMaterialSrvRaw=opticMaterialSrv.Get();ctx->PSSetShaderResources(2,1,&opticMaterialSrvRaw);
    ID3D11ShaderResourceView* noSrv=nullptr;ctx->PSSetShaderResources(1,1,&noSrv);ctx->PSSetShaderResources(3,1,&noSrv);
    edvr::EyeDrawSnapshot opticMaterialCapture;
    opticMaterialCapture.capture(ctx.Get(),91,7,edvr::EyeDrawSnapshot::kF77OpticVs,edvr::EyeDrawSnapshot::kF77OpticPs,'I',6,1,0,2,static_cast<int32_t>(opticBase),false,false,true,700);
    opticMaterialCapture.capture(ctx.Get(),92,9,edvr::EyeDrawSnapshot::kF77OpticVs,edvr::EyeDrawSnapshot::kF77OpticPs,'I',6,1,0,2,static_cast<int32_t>(opticBase),false,false,true,701);
    check(opticMaterialCapture.candidateInputs.size()==2 && opticMaterialCapture.candidateInputs[0].slot==2 && opticMaterialCapture.candidateInputs[1].slot==2 &&
          opticMaterialCapture.candidateInputs[0].requestedMip==0 && opticMaterialCapture.candidateInputs[0].mip==1 &&
          opticMaterialCapture.candidateInputs[0].sourceArraySize==8 && opticMaterialCapture.candidateInputs[0].slices==8 &&
          opticMaterialCapture.candidateInputs[1].requestedMip==0 && opticMaterialCapture.candidateInputs[1].mip==1 &&
          opticMaterialCapture.candidateInputs[1].sourceArraySize==8 && opticMaterialCapture.candidateInputs[1].slices==8 &&
          opticMaterialCapture.candidateInputs[0].bytes<=edvr::EyeDrawSnapshot::kCandidateInputBudget/2 &&
          opticMaterialCapture.candidateInputs[0].bytes==opticMaterialCapture.candidateInputs[1].bytes &&
          opticMaterialCapture.candidateInputBytes<=edvr::EyeDrawSnapshot::kCandidateInputBudget,
          "WARP t2 capture retains full view slices from the highest fitting mip chain within equal per optic budgets");
    ctx->IASetInputLayout(savedOpticLayout.Get());ID3D11Buffer* restoreVb=savedVb.Get();
    ctx->IASetVertexBuffers(1,1,&restoreVb,&savedStride,&savedOffset);ctx->IASetIndexBuffer(savedIb.Get(),savedIndexFormat,savedIndexOffset);
    ID3D11ShaderResourceView* restoreSrvs[3]={savedSrv1.Get(),savedSrv2.Get(),savedSrv3.Get()};ctx->PSSetShaderResources(1,3,restoreSrvs);
    wait_gpu(ctx.Get(),dev.Get());
    check(opticMaterialCapture.write(ctx.Get(),(std::wstring(argv[1])+L".t2").c_str()),"bounded T2 mip fallback snapshot write");
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

    // The exact rifle bolt pair reads two endpoint positions plus a stepped
    // per-instance record. Preserve its original binding origins and draw
    // window in the existing bounded geometry capture.
    const char* boltCode="struct I{float3 a:POSITION0;float3 b:POSITION1;float4 p:TEXCOORD0;};float4 main(I i):SV_Position{return float4(i.a+i.b+i.p.xyz*0.001,1);}";
    ComPtr<ID3DBlob> boltBlob;hr(D3DCompile(boltCode,strlen(boltCode),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&boltBlob,nullptr));
    D3D11_INPUT_ELEMENT_DESC boltElements[3]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"POSITION",1,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,4,D3D11_INPUT_PER_INSTANCE_DATA,1}};
    ComPtr<ID3D11InputLayout> boltLayout;hr(dev->CreateInputLayout(boltElements,3,boltBlob->GetBufferPointer(),boltBlob->GetBufferSize(),&boltLayout));
    edvr::EyeDrawSnapshot::rememberLayout(boltLayout.Get(),boltElements,3,edvr::EyeDrawSnapshot::kBoltVs);
    bd.ByteWidth=128;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;bd.CPUAccessFlags=bd.MiscFlags=bd.StructureByteStride=0;
    ComPtr<ID3D11Buffer> boltVertices,boltInstances;hr(dev->CreateBuffer(&bd,nullptr,&boltVertices));hr(dev->CreateBuffer(&bd,nullptr,&boltInstances));
    uint8_t boltVertexBytes[128],boltInstanceBytes[128];for(UINT i=0;i<128;++i){boltVertexBytes[i]=static_cast<uint8_t>(0x20+i);boltInstanceBytes[i]=static_cast<uint8_t>(0x80+i);}
    ctx->UpdateSubresource(boltVertices.Get(),0,nullptr,boltVertexBytes,0,0);ctx->UpdateSubresource(boltInstances.Get(),0,nullptr,boltInstanceBytes,0,0);
    D3D11_BUFFER_DESC boltIndexDesc{};boltIndexDesc.ByteWidth=64;boltIndexDesc.Usage=D3D11_USAGE_DEFAULT;boltIndexDesc.BindFlags=D3D11_BIND_INDEX_BUFFER;
    ComPtr<ID3D11Buffer> boltIndices;hr(dev->CreateBuffer(&boltIndexDesc,nullptr,&boltIndices));
    UINT boltStrides[2]={24,16},boltOffsets[2]={12,8};ID3D11Buffer* boltBindings[2]={boltVertices.Get(),boltInstances.Get()};
    ctx->IASetVertexBuffers(0,2,boltBindings,boltStrides,boltOffsets);ctx->IASetIndexBuffer(boltIndices.Get(),DXGI_FORMAT_R16_UINT,4);
    ctx->IASetInputLayout(boltLayout.Get());ID3D11Buffer* boltCb=cb.Get();ctx->VSSetConstantBuffers(1,1,&boltCb);
    float boltCbData[48];for(UINT i=0;i<48;++i)boltCbData[i]=static_cast<float>(i)+0.25f;ctx->UpdateSubresource(cb.Get(),0,nullptr,boltCbData,0,0);
    edvr::EyeDrawSnapshot boltSnap;
    boltSnap.capture(ctx.Get(),500,0,edvr::EyeDrawSnapshot::kBoltVs,edvr::EyeDrawSnapshot::kBoltPs,'X',4,3,2,3,0);
    const auto& boltDraw=boltSnap.draws.back();
    check(edvr::EyeDrawSnapshot::watches(edvr::EyeDrawSnapshot::kBoltVs) && boltDraw.layout.size()==3 &&
              std::strcmp(boltDraw.layout[0].semantic,"POSITION")==0 && boltDraw.layout[0].index==0 &&
              std::strcmp(boltDraw.layout[1].semantic,"POSITION")==0 && boltDraw.layout[1].index==1 &&
              boltDraw.streams[0].offset==12 && boltDraw.streams[0].captureOffset==12 && boltDraw.streams[0].copied==116 &&
              boltDraw.streams[1].offset==8 && boltDraw.streams[1].captureOffset==8 && boltDraw.streams[1].copied==80 &&
              boltDraw.streams[2].captureOffset==10 && boltDraw.streams[2].copied==8,
          "exact bolt pair captures the indexed VB0 tail and stepped instance window from nonzero bindings");
    // Indexed VB0 is retained from its binding offset to the stream cap: the
    // current reader does not decode indices to infer the maximum vertex.
    uint8_t boltVertexExpected[116];std::memcpy(boltVertexExpected,boltVertexBytes+12,sizeof(boltVertexExpected));
    uint8_t boltInstanceExpected[80];std::memcpy(boltInstanceExpected,boltInstanceBytes+8,sizeof(boltInstanceExpected));
    float boltCbLater[48];for(float& v:boltCbLater)v=-17.f;ctx->UpdateSubresource(cb.Get(),0,nullptr,boltCbLater,0,0);
    std::memset(boltVertexBytes,0,sizeof(boltVertexBytes));std::memset(boltInstanceBytes,0,sizeof(boltInstanceBytes));
    ctx->UpdateSubresource(boltVertices.Get(),0,nullptr,boltVertexBytes,0,0);ctx->UpdateSubresource(boltInstances.Get(),0,nullptr,boltInstanceBytes,0,0);wait_gpu(ctx.Get(),dev.Get());
    D3D11_MAPPED_SUBRESOURCE boltMapped{};hr(ctx->Map(boltDraw.streams[0].stage.Get(),0,D3D11_MAP_READ,0,&boltMapped));
    const bool boltVertexSnapshot=std::memcmp(boltMapped.pData,boltVertexExpected,sizeof(boltVertexExpected))==0;ctx->Unmap(boltDraw.streams[0].stage.Get(),0);
    hr(ctx->Map(boltDraw.streams[1].stage.Get(),0,D3D11_MAP_READ,0,&boltMapped));
    const bool boltInstanceSnapshot=std::memcmp(boltMapped.pData,boltInstanceExpected,sizeof(boltInstanceExpected))==0;ctx->Unmap(boltDraw.streams[1].stage.Get(),0);
    hr(ctx->Map(boltDraw.stage[1].Get(),0,D3D11_MAP_READ,0,&boltMapped));
    const bool boltConstantSnapshot=boltDraw.copied[1]==sizeof(boltCbData) && std::memcmp(boltMapped.pData,boltCbData,sizeof(boltCbData))==0;ctx->Unmap(boltDraw.stage[1].Get(),0);
    ComPtr<ID3D11InputLayout> boltLayoutBound;ctx->IAGetInputLayout(&boltLayoutBound);
    ID3D11Buffer* boltVbAfter[2]{};UINT boltStrideAfter[2]{},boltOffsetAfter[2]{};ctx->IAGetVertexBuffers(0,2,boltVbAfter,boltStrideAfter,boltOffsetAfter);
    ID3D11Buffer* boltIbAfter=nullptr;DXGI_FORMAT boltFmtAfter{};UINT boltIbOffsetAfter=0;ctx->IAGetIndexBuffer(&boltIbAfter,&boltFmtAfter,&boltIbOffsetAfter);
    ID3D11Buffer* boltCbAfter=nullptr;ctx->VSGetConstantBuffers(1,1,&boltCbAfter);
    check(boltVertexSnapshot && boltInstanceSnapshot && boltConstantSnapshot && boltLayoutBound.Get()==boltLayout.Get() && boltVbAfter[0]==boltVertices.Get() && boltVbAfter[1]==boltInstances.Get() &&
              boltStrideAfter[0]==24 && boltStrideAfter[1]==16 && boltOffsetAfter[0]==12 && boltOffsetAfter[1]==8 && boltIbAfter==boltIndices.Get() &&
              boltFmtAfter==DXGI_FORMAT_R16_UINT && boltIbOffsetAfter==4 && boltCbAfter==cb.Get(),
          "bolt stream and constant copies retain draw-time data without changing pipeline bindings");
    if(boltVbAfter[0])boltVbAfter[0]->Release();if(boltVbAfter[1])boltVbAfter[1]->Release();if(boltIbAfter)boltIbAfter->Release();if(boltCbAfter)boltCbAfter->Release();
    boltSnap.capture(ctx.Get(),501,1,edvr::EyeDrawSnapshot::kBoltVs,edvr::EyeDrawSnapshot::kBoltPs,'X',4,3,2,3,0);
    check(boltSnap.fireGeometrySeen==2 && boltSnap.fireGeometryCaptured==2,
          "bolt geometry remains captured after the eye dump's first frame");
    boltSnap.capture(ctx.Get(),501,2,edvr::EyeDrawSnapshot::kBoltVs,edvr::EyeDrawSnapshot::kBoltPs^1ull,'X',4,3,2,3,0);
    check(boltSnap.fireGeometrySeen==2 && boltSnap.draws.back().layout.empty() && !boltSnap.draws.back().streams[0].copied,
          "a neighboring pixel shader does not activate bolt geometry capture");
    ctx->IASetInputLayout(nullptr);boltSnap.capture(ctx.Get(),502,3,edvr::EyeDrawSnapshot::kBoltVs,edvr::EyeDrawSnapshot::kBoltPs,'X',4,3,2,3,0);
    ctx->IASetInputLayout(boltLayout.Get());
    boltSnap.vertexBytes=32*1024*1024;boltSnap.capture(ctx.Get(),503,4,edvr::EyeDrawSnapshot::kBoltVs,edvr::EyeDrawSnapshot::kBoltPs,'X',4,3,2,3,0);
    check(boltSnap.fireGeometrySeen==4 && boltSnap.fireGeometryCaptured==2 && boltSnap.fireGeometryMissingLayout==1 && boltSnap.fireGeometryRefusedBytes>0,
          "bounded bolt summary distinguishes observed draws, captured geometry, missing layouts, and refused bytes");

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
    for(int i=0;i<2;++i){float values[48];for(float& v:values)v=float(7+i);ctx->UpdateSubresource(cb.Get(),0,nullptr,values,0,0);gui.capture(ctx.Get(),200,200,guiHash,guiPs,'X',6,1,150000,0,0);}
    gui.capture(ctx.Get(),201,201,guiHash,guiPs,'X',6,1,0,0,0);check(gui.count()==2,"GUI captures one source frame only");
    ctx->ClearRenderTargetView(rtv.Get(),bg);ctx->ClearDepthStencilView(sourceDsv.Get(),D3D11_CLEAR_DEPTH,0,0);
    edvr::EyeDrawSnapshot meshSnap;
    ComPtr<ID3D11Buffer> meshPool,meshBones,meshOtherPalette,meshIds;
    ComPtr<ID3D11ShaderResourceView> meshPoolView,meshBoneView;
    bd={};bd.ByteWidth=672;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=336;
    hr(dev->CreateBuffer(&bd,nullptr,&meshPool));hr(dev->CreateShaderResourceView(meshPool.Get(),nullptr,&meshPoolView));
    bd.ByteWidth=48*24000;bd.StructureByteStride=48; // palette extends past the old 1 MiB cap
    hr(dev->CreateBuffer(&bd,nullptr,&meshBones));hr(dev->CreateShaderResourceView(meshBones.Get(),nullptr,&meshBoneView));
    hr(dev->CreateBuffer(&bd,nullptr,&meshOtherPalette));
    bd.ByteWidth=32;bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;bd.MiscFlags=bd.StructureByteStride=0;
    hr(dev->CreateBuffer(&bd,nullptr,&meshIds));
    ctx->VSSetShaderResources(33,1,meshPoolView.GetAddressOf());ctx->VSSetShaderResources(38,1,meshBoneView.GetAddressOf());
    UINT meshStride=8;ctx->IASetVertexBuffers(0,1,meshIds.GetAddressOf(),&meshStride,&zero);
    constexpr uint64_t meshVs=0x7B0DC42D383F694Cull;
    constexpr uint64_t meshPs=0x0DF03E64DF9DBEF1ull;
    wchar_t longestPath[MAX_PATH];for(auto& ch:longestPath)ch=L'x';longestPath[MAX_PATH-1]=0;char bodyReport[1200]{};
    const int bodyLength=std::snprintf(bodyReport,sizeof(bodyReport),edvr::EyeDrawSnapshot::kBodyCaptureReport,longestPath,
        UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,"WRITE FAILED",UINT32_MAX,UINT32_MAX,UINT32_MAX);
    const int candidateLength=std::snprintf(bodyReport,sizeof(bodyReport),edvr::EyeDrawSnapshot::kBodyCandidateReport,L"123456789012345",
        UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX);
    check(bodyLength>0&&bodyLength<1150&&candidateLength>0&&candidateLength<1150,"worst-case body/candidate reports fit1200-byte logger with prefix headroom");
    check(edvr::EyeDrawSnapshot::bodyMeshSelector(meshVs,meshPs,21,37080) &&
          edvr::EyeDrawSnapshot::bodyMeshSelector(meshVs,meshPs,21,10050) &&
          edvr::EyeDrawSnapshot::bodyMeshSelector(meshVs,meshPs,21,3216) &&
          !edvr::EyeDrawSnapshot::bodyMeshSelector(meshVs,meshPs,20,10050) &&
          !edvr::EyeDrawSnapshot::bodyMeshSelector(0x9D800F45B12A6AACull,0x194DD6A677084A74ull,21,3216) &&
          edvr::EyeDrawSnapshot::bodyMeshSelector(0x9D800F45B12A6AACull,0x194DD6A677084A74ull,21,13260) &&
          !edvr::EyeDrawSnapshot::bodyMeshSelector(meshVs,meshPs,20,37080) &&
          !edvr::EyeDrawSnapshot::bodyMeshSelector(meshVs,meshPs,21,6) &&
          !edvr::EyeDrawSnapshot::bodyMeshSelector(meshVs,0x1234,21,37080),
          "body capture selector requires the measured VS/PS, draw count, and stencil ref");
    ID3D11Buffer* paletteSet[4]={meshBones.Get(),meshOtherPalette.Get(),meshPool.Get(),meshIds.Get()};
    ID3D11Buffer* duplicatePalette[2]={meshBones.Get(),meshBones.Get()};
    check(edvr::EyeDrawSnapshot::otherTrackedPaletteIndex(0,paletteSet,meshBones.Get())==-1 &&
          edvr::EyeDrawSnapshot::otherTrackedPaletteIndex(1,paletteSet,meshBones.Get())==-1 &&
          edvr::EyeDrawSnapshot::otherTrackedPaletteIndex(2,paletteSet,meshBones.Get())==1 &&
          edvr::EyeDrawSnapshot::otherTrackedPaletteIndex(2,paletteSet,meshOtherPalette.Get())==0 &&
          edvr::EyeDrawSnapshot::otherTrackedPaletteIndex(2,paletteSet,meshPool.Get())==-1 &&
          edvr::EyeDrawSnapshot::otherTrackedPaletteIndex(3,paletteSet,meshBones.Get())==-1 &&
          edvr::EyeDrawSnapshot::otherTrackedPaletteIndex(4,paletteSet,meshBones.Get())==-1 &&
          edvr::EyeDrawSnapshot::otherTrackedPaletteIndex(2,duplicatePalette,meshBones.Get())==-1,
          "other-palette identity is available only for exactly two tracked palettes and an exact bound match");
    for(unsigned i=0;i<2;++i) {
        std::vector<uint8_t> pool(672,uint8_t(21+i)),bones(48*24000,uint8_t(31+i)),ids(32,uint8_t(41+i));
        ctx->UpdateSubresource(meshPool.Get(),0,nullptr,pool.data(),0,0);
        ctx->UpdateSubresource(meshBones.Get(),0,nullptr,bones.data(),0,0);
        ctx->UpdateSubresource(meshIds.Get(),0,nullptr,ids.data(),0,0);
        meshSnap.captureSourceMesh(ctx.Get(),300+i,meshVs,0,'X',6,1,2,0,0);
        meshSnap.captureSourceMesh(ctx.Get(),300+i,meshVs,0,'X',6,1,3,0,0);
    }
    check(meshSnap.draws.size()==4 && meshSnap.meshBuffers.size()==6 && !meshSnap.meshDeclined,"ordinary source snapshots retain their existing per-frame deduplication");
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
    eyeMesh.captureEyeMesh(ctx.Get(),699,899,-1,{},0,edvr::EyeDrawSnapshot::kHud,0,0,nullptr,'X',6,1,0,0,0);
    check(eyeMesh.draws.empty() && !eyeMesh.firstFrame,"non-mesh does not start eye mesh window");
    eyeMesh.captureEyeMesh(ctx.Get(),699,899,-1,{},0,meshVs,meshPs,21,nullptr,'X',6,1,0,0,0);
    check(eyeMesh.draws.empty() && eyeMesh.meshNonBodyDraws==1,"body-family draw with nonmatching count is aggregated, not retained");
    for(unsigned frame=700;frame<716;++frame)for(unsigned eye=0;eye<2;++eye) {
        ID3D11RenderTargetView* target=eye?otherEyeRtv.Get():rtv.Get();ctx->OMSetRenderTargets(1,&target,nullptr);
        const uint8_t value=uint8_t(51+(frame-700)*2+eye);
        std::vector<uint8_t> pool(672,value),bones(48*24000,uint8_t(value+10)),otherBones(48*24000,uint8_t(value+30)),ids(32,uint8_t(value+20));
        ctx->UpdateSubresource(meshPool.Get(),0,nullptr,pool.data(),0,0);
        ctx->UpdateSubresource(meshBones.Get(),0,nullptr,bones.data(),0,0);
        ctx->UpdateSubresource(meshOtherPalette.Get(),0,nullptr,otherBones.data(),0,0);
        ctx->UpdateSubresource(meshIds.Get(),0,nullptr,ids.data(),0,0);
        for(unsigned draw=0;draw<2;++draw) {
            float constants[48];for(float& v:constants)v=float(frame*10+eye*2+draw);
            ctx->UpdateSubresource(cb.Get(),0,nullptr,constants,0,0);
            std::vector<uint8_t> drawPool(672,uint8_t(value+draw)),drawBones(48*24000,uint8_t(value+10+draw)),drawOtherBones(48*24000,uint8_t(value+30+draw)),drawIds(32,uint8_t(value+20+draw));
            ctx->UpdateSubresource(meshPool.Get(),0,nullptr,drawPool.data(),0,0);
            ctx->UpdateSubresource(meshBones.Get(),0,nullptr,drawBones.data(),0,0);
            ctx->UpdateSubresource(meshOtherPalette.Get(),0,nullptr,drawOtherBones.data(),0,0);
            ctx->UpdateSubresource(meshIds.Get(),0,nullptr,drawIds.data(),0,0);
            edvr::EyeMeshJoinStamp join{};join.sceneFrame=1200+(frame-700);join.epoch=44;
            join.scheduledIndex=draw;join.scheduledRenderFrame=900+(frame-700);
            join.fpFrame=frame;join.fpSequence=7000+frame;join.gGameFrame=8000+frame;join.itemSerial=eye+1;
            join.rig=0x12340000ull+eye;join.rigGeneration=9;join.poseQpc=100000+frame;join.drawQpc=200000+frame;
            join.armJoints[0][0]=24;join.armJoints[0][1]=34;join.armJoints[0][2]=35;
            join.armJoints[1][0]=36;join.armJoints[1][1]=37;join.armJoints[1][2]=38;
            join.validFlags=(1u<<0)|(1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<7);
            eyeMesh.captureEyeMesh(ctx.Get(),frame,900+(frame-700),int32_t(eye),join,eye*100+draw,meshVs,meshPs,21,meshOtherPalette.Get(),'X',37080,1,2+draw,150000,8000);
        }
        ctx->OMGetRenderTargets(1,&boundRt,nullptr);check(boundRt.Get()==target,"eye capture preserves target binding");
    }
    check(eyeMesh.draws.size()==64 && eyeMesh.meshBuffers.size()==128 && eyeMesh.meshCaptureSnapshots==32 && !eyeMesh.meshDeclined,
          "sixteen frames retain every eye draw and one body plus other-palette snapshot per eye and frame");
    check(eyeMesh.draws.front().frame==700 && eyeMesh.draws.front().renderFrame==900 && eyeMesh.draws.front().eye==0 &&
          eyeMesh.draws.back().frame==715 && eyeMesh.draws.back().renderFrame==915 && eyeMesh.draws.back().eye==1,
          "ledger and native render frames remain separate and each row carries its eye");
    for(size_t i=0;i<eyeMesh.draws.size();++i) {
        check(eyeMesh.draws[i].layout.size()==1,"eye mesh input layout retained");
        check(eyeMesh.draws[i].meshSelected==1,"body selector retained on every actual draw row");
        if(i%2==0)check(eyeMesh.draws[i].mesh[3]!=UINT32_MAX,"first body draw carries other palette candidate");
        else check(eyeMesh.draws[i].mesh[3]==UINT32_MAX && eyeMesh.draws[i].meshStatus[3]==7,"later body draw records explicit quota status");
    }
    const auto eyeBytes=eyeMesh.meshBytes;
    eyeMesh.captureEyeMesh(ctx.Get(),699,899,-1,{},0,meshVs,meshPs,21,nullptr,'X',37080,1,0,0,0);
    eyeMesh.captureEyeMesh(ctx.Get(),716,916,-1,{},0,meshVs,meshPs,21,nullptr,'X',37080,1,0,0,0);
    check(eyeMesh.draws.size()==64 && eyeMesh.meshBytes==eyeBytes && eyeMesh.meshCaptureDeclined==2,
          "pre-window and post-sixteen-frame attempts are explicit declines");
    edvr::EyeDrawSnapshot unknownEyeMesh;
    unknownEyeMesh.captureEyeMesh(ctx.Get(),800,0,-1,{},0,meshVs,meshPs,21,nullptr,'X',37080,1,0,0,0);
    check(unknownEyeMesh.draws.size()==1 && unknownEyeMesh.meshCaptureSnapshots==0 &&
          unknownEyeMesh.draws[0].meshStatus[0]==10 && unknownEyeMesh.draws[0].eye==-1,
          "unknown-eye body metadata is retained without consuming a snapshot quota");
    unknownEyeMesh.captureEyeMesh(ctx.Get(),800,0,0,{},1,meshVs,meshPs,21,meshOtherPalette.Get(),'X',37080,1,0,0,0);
    check(unknownEyeMesh.meshCaptureSnapshots==1 && unknownEyeMesh.draws[1].mesh[0]!=UINT32_MAX,
          "known eye can still receive a full snapshot after unknown-eye metadata");
    edvr::EyeDrawSnapshot candidateBody;
    candidateBody.armBodyCandidates(901);
    candidateBody.captureEyeMesh(ctx.Get(),900,1000,0,{},0,meshVs,meshPs,21,meshOtherPalette.Get(),'X',37080,1,0,0,0);
    // Simulate all regular-body reserved capacity consumed before the candidate draws, without allocating hundreds of MiB in the rig.
    candidateBody.meshBytes=edvr::EyeDrawSnapshot::kEyeMeshBudget-edvr::EyeDrawSnapshot::kBodyCandidateMeshReserve;
    candidateBody.vertexBytes=32*1024*1024-edvr::EyeDrawSnapshot::kBodyCandidateVertexReserve;
    for(int eye=0;eye<2;++eye)for(uint32_t count:{10050u,3216u}) {
        candidateBody.captureEyeMesh(ctx.Get(),901,1001,eye,{},count,meshVs,meshPs,21,meshOtherPalette.Get(),'X',count,1,0,0,0);
        check(candidateBody.draws.back().meshSelected==2&&candidateBody.draws.back().meshStatus[0]==1&&candidateBody.draws.back().meshStatus[1]==1&&candidateBody.draws.back().meshStatus[3]==1,
              "earlier regular-body saturation cannot consume either observed candidate's reserved per-eye draw-time palettes");
    }
    check(candidateBody.meshBytes<=edvr::EyeDrawSnapshot::kEyeMeshBudget&&candidateBody.vertexBytes<=32*1024*1024,
          "candidate reservations preserve whole-run structured and geometry byte caps");
    candidateBody.captureEyeMesh(ctx.Get(),901,1001,0,{},99,meshVs,meshPs,21,meshOtherPalette.Get(),'X',10050,1,0,0,0);
    check(candidateBody.draws.back().meshStatus[0]==7,"repeated candidate refuses another palette copy explicitly");
    edvr::EyeDrawSnapshot bodyCandidateFixture;
    bodyCandidateFixture.armBodyCandidates(951);
    bodyCandidateFixture.captureEyeMesh(ctx.Get(),950,0,-1,{},0,meshVs,meshPs,21,nullptr,'X',10050,1,0,0,0);
    check(bodyCandidateFixture.draws.back().meshSelected==2&&bodyCandidateFixture.draws.back().meshStatus[0]==10,"candidate unknown-eye metadata remains explicit and consumes no reserved slot");
    edvr::EyeDrawSnapshot firstCandidate;firstCandidate.armBodyCandidates(1001);
    firstCandidate.captureEyeMesh(ctx.Get(),1001,0,0,{},0,meshVs,meshPs,21,meshOtherPalette.Get(),'X',10050,1,0,0,0);
    check(firstCandidate.draws.back().meshStatus[0]==1&&firstCandidate.bodyCandidateSlots==1,
          "first body arrival on the selected complete ledger frame gets candidate palettes without needing a partial-frame predecessor");
    ctx->PSSetConstantBuffers(2,1,cb.GetAddressOf());
    ctx->IASetVertexBuffers(1,1,meshIds.GetAddressOf(),&meshStride,&zero);
    for(int eye=0;eye<2;++eye)for(uint32_t count:{10050u,3216u})
        bodyCandidateFixture.captureEyeMesh(ctx.Get(),951,0,eye,{},count,meshVs,meshPs,21,meshOtherPalette.Get(),'X',count,1,0,0,0);
    check(bodyCandidateFixture.write(ctx.Get(),(std::wstring(argv[1])+L".bodycandidates").c_str())&&!bodyCandidateFixture.failures,"candidate geometry, PSb2 and palettes serialize");
    ID3D11Buffer* noPsBuffer=nullptr;ctx->PSSetConstantBuffers(2,1,&noPsBuffer);
    ctx->IASetVertexBuffers(1,1,&noPsBuffer,&meshStride,&zero);
    std::vector<uint8_t> geometryVertices(32,76);
    ctx->UpdateSubresource(meshIds.Get(),0,nullptr,geometryVertices.data(),0,0);
    // Version 9: the cull gate probe's armed-frame geometry. Every pool draw
    // of the armed frame gets a map row; a mesh drawn twice is copied once;
    // a second blend state is a second state row; other frames are ignored.
    // The layout names slot 0 per-vertex (meshIds, stride 8, 32 bytes: the
    // window clamps to the buffer); the index buffer is `indices` at +4.
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    eyeMesh.armGeometry(710);
    eyeMesh.captureGeometry(ctx.Get(),709,5,meshVs,0,'X',6,1,0,1,0);
    check(eyeMesh.geoDraws.empty(),"geometry ignores frames other than the armed one");
    eyeMesh.captureGeometry(ctx.Get(),710,7,meshVs,0x77,'X',6,2,16,1,2);
    eyeMesh.captureGeometry(ctx.Get(),710,8,meshVs,0x78,'X',6,3,24,1,2);
    {
        D3D11_BLEND_DESC blendDesc{};blendDesc.RenderTarget[0].BlendEnable=TRUE;
        blendDesc.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;blendDesc.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;blendDesc.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;blendDesc.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D11BlendState> blend;hr(dev->CreateBlendState(&blendDesc,&blend));
        ctx->OMSetBlendState(blend.Get(),nullptr,0xffffffff);
        eyeMesh.captureGeometry(ctx.Get(),710,9,meshVs,0x79,'X',3,1,32,0,0);
        ctx->OMSetBlendState(nullptr,nullptr,0xffffffff);
    }
    check(eyeMesh.geoDraws.size()==3 && eyeMesh.geoMeshes.size()==2 && eyeMesh.geoStates.size()==2 &&
          !eyeMesh.geoDeclined && !eyeMesh.geoDrawsDropped,
          "geometry keeps one copy per distinct mesh and state within the armed frame");
    check(eyeMesh.geoDraws[0].mesh==0 && eyeMesh.geoDraws[1].mesh==0 && eyeMesh.geoDraws[2].mesh==1 &&
          eyeMesh.geoDraws[2].state==1 && eyeMesh.geoMeshes[0].vBytes==16 && eyeMesh.geoMeshes[0].iBytes==12,
          "geometry map rows, the clamped vertex window and the index window");
    // The geometry arm across a reset (design-occlusion-culling §10, run
    // 152632: the pool's first sighting 60 ms after the arm wiped it). A
    // release before any draw of the armed frame is mapped keeps the arm; one
    // after draws were mapped, or past the frame, loses it and records it.
    // (object_probe.cpp's call site -- ledgerRelease under an armed gate run,
    // and the log lines -- is beyond this rig: only a flight reaches it.)
    {
        edvr::EyeDrawSnapshot geoArm;
        check(geoArm.resetKeepingGeometry(5)==edvr::EyeDrawSnapshot::kGeoNotArmed && !geoArm.geoFrame,
              "a release with no geometry arm leaves none");
        geoArm.armGeometry(12);
        check(geoArm.resetKeepingGeometry(11)==edvr::EyeDrawSnapshot::kGeoRearmed && geoArm.geoFrame==12 &&
              geoArm.geoRearms==1 && !geoArm.geoLostFrame,"a release before the armed frame re-arms it");
        check(geoArm.resetKeepingGeometry(12)==edvr::EyeDrawSnapshot::kGeoRearmed && geoArm.geoFrame==12 &&
              geoArm.geoRearms==2,"a release in the armed frame before any draw is mapped re-arms it");
        geoArm.captureGeometry(ctx.Get(),12,3,meshVs,0x77,'X',6,2,16,1,2);
        check(geoArm.geoDraws.size()==1,"the re-armed frame maps its draws");
        check(geoArm.resetKeepingGeometry(12)==edvr::EyeDrawSnapshot::kGeoLost && !geoArm.geoFrame &&
              geoArm.geoLostFrame==12 && geoArm.geoLostAt==12 && geoArm.geoLostDraws==1 && geoArm.geoDraws.empty(),
              "a release after draws were mapped disarms and records the loss");
        geoArm.captureGeometry(ctx.Get(),12,4,meshVs,0x77,'X',6,2,16,1,2);
        check(geoArm.geoDraws.empty(),"a disarmed frame maps nothing");
        geoArm.clearGeometryFate();geoArm.armGeometry(20);
        check(geoArm.resetKeepingGeometry(21)==edvr::EyeDrawSnapshot::kGeoLost && geoArm.geoLostFrame==20 &&
              geoArm.geoLostAt==21 && !geoArm.geoLostDraws && !geoArm.geoRearms,
              "a release past the armed frame loses it; a new run's fate starts clear");
        geoArm.armGeometry(30);geoArm.reset();
        check(!geoArm.geoFrame && geoArm.geoLostFrame==20,"plain reset() still disarms; the fate outlives it");
    }
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
        // The game's pool-VS b1 is 336 float4 registers (5376 bytes, the
        // eyemesh snapshot's copy); the camera sits at registers 270..275.
        // Every fixture float holds its own cb1 float index (+ a per-eye and
        // per-frame offset), exact in f32, so a window counted in the wrong
        // units shows up as the wrong index -- version 1 counted floats.
        constexpr unsigned kCb1Floats=336*4;
        auto cb1Value=[](unsigned k,unsigned eye,unsigned frameIndex){
            return float(k)+0.5f*float(eye)+2000.0f*float(frameIndex);
        };
        D3D11_BUFFER_DESC cb1Desc{};cb1Desc.ByteWidth=kCb1Floats*4;cb1Desc.Usage=D3D11_USAGE_DEFAULT;
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
                std::vector<float> cb1Data(kCb1Floats);
                for(unsigned k=0;k<kCb1Floats;++k)cb1Data[k]=cb1Value(k,eye,frame-firstFrame);
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
        std::vector<float> dead(kCb1Floats,7.7f);ctx->UpdateSubresource(cb1.Get(),0,nullptr,dead.data(),0,0);
        wait_gpu(ctx.Get(),dev.Get());
        std::wstring depthDir=argv[1];depthDir.resize(depthDir.find_last_of(L"\\/"));
        check(depth.write(ctx.Get(),depthDir.c_str(),L"TEST")==4,"four depth files written");
        check(depth.failures==0 && depth.faults==0,"depth readbacks complete without faults");
        struct DepthHeader{char magic[8];uint32_t version,frame,eye,width,height,format,constFirstFloat,constFloats;};
        static_assert(sizeof(DepthHeader)==40,"tools/eye_depth_dump.py reads a 40-byte version-2 header");
        auto readDepthFile=[&](const wchar_t* name,DepthHeader& hd,std::vector<float>& constants,std::vector<float>& grid)->bool{
            FILE* f=nullptr;if(_wfopen_s(&f,(depthDir+L"\\"+name).c_str(),L"rb")||!f)return false;
            bool ok=fread(&hd,1,sizeof(hd),f)==sizeof(hd) && std::memcmp(hd.magic,"EDVRDEPT",8)==0;
            uint32_t depthBytes=0;uint32_t fileFaults=0;
            if(ok) {
                constants.resize(hd.constFloats);
                ok=hd.constFloats<=(336-256)*4 && (constants.empty() ||
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
            check(hd.version==2 && hd.frame==firstFrame && hd.eye==eye && hd.width==8 && hd.height==8 &&
                  hd.format==int(DXGI_FORMAT_R32_FLOAT),
                  "depth fixture header round-trips (version 2) with an R32_FLOAT payload format");
            check(hd.constFirstFloat==1024,"the header names the block's first cb1 float: register 256");
            check(constants.size()==320,"VS b1 registers 256..335 from the first pool draw");
            bool exact=constants.size()==320;
            for(unsigned j=0;exact && j<constants.size();++j)exact=constants[j]==cb1Value(1024+j,eye,0);
            check(exact,"every block float is its own cb1 float index: registers 256..335, not floats 256..591");
            // The camera the join needs: the clip matrix's columns at
            // registers 270..273 and the eye origin at 275.
            check(constants[(270-256)*4]==cb1Value(1080,eye,0) && constants[(273-256)*4+3]==cb1Value(1095,eye,0),
                  "view-projection columns cb1[270..273] inside the block");
            check(constants[(275-256)*4]==cb1Value(1100,eye,0) && constants[(275-256)*4+2]==cb1Value(1102,eye,0),
                  "eye origin cb1[275] inside the block");
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
                std::vector<float> cb1Data(kCb1Floats);
                for(unsigned k=0;k<kCb1Floats;++k)cb1Data[k]=cb1Value(k,eye,frame-s8Frame);
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
            check(constants.size()==320 && constants[(270-256)*4]==cb1Value(1080,eye,0),
                  "constants intact alongside the conversion");
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
        // that stops short of register 275 keeps an explicit zero-float
        // block on disk.
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
        check(oh.constFloats==0 && oc.empty() && oh.constFirstFloat==1024,
              "constants block explicit when b1 is too short");
        // A buffer that ends exactly after the camera (CB1[276], 4416 bytes):
        // the window clamps to registers 256..275 and keeps all of it.
        check(edvr::EyeDepthCapture::constWindowEnd(4416)==4416 &&
              edvr::EyeDepthCapture::constWindowEnd(4400)==0 &&
              edvr::EyeDepthCapture::constWindowEnd(8192)==336*16,
              "the constants window clamps to the buffer and needs register 275");
        {
            edvr::EyeDepthCapture tight;
            tight.configure(true);
            D3D11_BUFFER_DESC tightDesc{};tightDesc.ByteWidth=276*16;tightDesc.Usage=D3D11_USAGE_DEFAULT;
            tightDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            ComPtr<ID3D11Buffer> tightCb1;hr(dev->CreateBuffer(&tightDesc,nullptr,&tightCb1));
            std::vector<float> tightData(276*4);
            for(unsigned k=0;k<276*4;++k)tightData[k]=cb1Value(k,0,0);
            ctx->UpdateSubresource(tightCb1.Get(),0,nullptr,tightData.data(),0,0);
            ctx->VSSetConstantBuffers(1,1,tightCb1.GetAddressOf());
            const unsigned tightFrame=9300;
            tight.noteEyeDraw(ctx.Get(),tightFrame,depthDsv[0].Get(),true,0);
            tight.noteEyeDraw(ctx.Get(),tightFrame,depthDsv[1].Get(),true,1);
            wait_gpu(ctx.Get(),dev.Get());
            check(tight.write(ctx.Get(),depthDir.c_str(),L"TIGHT")==1,"tight-constants depth file written");
            wchar_t tightName[64];
            _snwprintf_s(tightName,64,_TRUNCATE,L"depth_TIGHT_f%u_A.bin",tightFrame);
            DepthHeader th{};std::vector<float> tc,tg;
            check(readDepthFile(tightName,th,tc,tg),"tight-constants depth file parses");
            check(th.constFirstFloat==1024 && tc.size()==80 && tc[(275-256)*4+2]==cb1Value(1102,0,0),
                  "a CB1[276] buffer yields registers 256..275, the eye origin included");
        }
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
    // A nonempty v10 candidate record pins the C++ writer to the Python reader:
    // exact optic identity, shifted VB/IB windows, material bytes, s0, and a
    // draw-issued before/after ROI pair all share the same draw id.
    edvr::EyeDrawSnapshot candidateFixture;
    auto stageBufferWindow=[&](const void* bytes,UINT whole,UINT begin,UINT length,ComPtr<ID3D11Buffer>& stage) {
        D3D11_BUFFER_DESC sourceDesc{};sourceDesc.ByteWidth=whole;sourceDesc.Usage=D3D11_USAGE_DEFAULT;
        ComPtr<ID3D11Buffer> source;hr(dev->CreateBuffer(&sourceDesc,nullptr,&source));
        ctx->UpdateSubresource(source.Get(),0,nullptr,bytes,0,0);
        D3D11_BUFFER_DESC stageDesc{};stageDesc.ByteWidth=length;stageDesc.Usage=D3D11_USAGE_STAGING;stageDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        hr(dev->CreateBuffer(&stageDesc,nullptr,&stage));D3D11_BOX box{begin,0,0,begin+length,1,1};
        ctx->CopySubresourceRegion(stage.Get(),0,0,0,0,source.Get(),0,&box);
    };
    edvr::EyeDrawSnapshot::Draw candidateDraw{};
    candidateDraw.vs=edvr::EyeDrawSnapshot::kF77OpticVs;candidateDraw.ps=edvr::EyeDrawSnapshot::kF77OpticPs;
    candidateDraw.frame=101;candidateDraw.ordinal=17;candidateDraw.kind='I';candidateDraw.count=2;candidateDraw.instances=1;
    candidateDraw.start=1;candidateDraw.base=2;
    uint8_t candidateVertices[192];for(UINT i=0;i<sizeof(candidateVertices);++i)candidateVertices[i]=static_cast<uint8_t>(i);
    ComPtr<ID3D11Buffer> candidateVertexStage;
    stageBufferWindow(candidateVertices,sizeof(candidateVertices),96,96,candidateVertexStage);
    candidateDraw.streams[1].offset=0;candidateDraw.streams[1].stride=48;candidateDraw.streams[1].whole=sizeof(candidateVertices);
    candidateDraw.streams[1].captureOffset=96;candidateDraw.streams[1].copied=96;candidateDraw.streams[1].stage=candidateVertexStage;
    const uint16_t candidateIndices[8]={7,7,0,1,7,7,7,7};ComPtr<ID3D11Buffer> candidateIndexStage;
    stageBufferWindow(candidateIndices,sizeof(candidateIndices),4,4,candidateIndexStage);
    candidateDraw.streams[2].offset=2;candidateDraw.streams[2].stride=2;candidateDraw.streams[2].whole=sizeof(candidateIndices);
    candidateDraw.streams[2].captureOffset=4;candidateDraw.streams[2].copied=4;candidateDraw.streams[2].stage=candidateIndexStage;
    edvr::EyeDrawSnapshot::Layout candidateLayout0{},candidateLayout1{};
    std::strcpy(candidateLayout0.semantic,"PACKEDVERTEXDATAA");candidateLayout0.format=DXGI_FORMAT_R32G32B32A32_UINT;candidateLayout0.slot=1;candidateLayout0.offset=0;
    std::strcpy(candidateLayout1.semantic,"PACKEDVERTEXDATAC");candidateLayout1.format=DXGI_FORMAT_R32G32_UINT;candidateLayout1.slot=1;candidateLayout1.offset=16;
    candidateDraw.layout={candidateLayout0,candidateLayout1};
    candidateFixture.draws.push_back(std::move(candidateDraw));
    edvr::EyeDrawSnapshot::CandidateFrame candidateFrame{};candidateFrame.draw=0;candidateFrame.ledgerFrame=101;candidateFrame.renderFrame=501;
    candidateFrame.eye=1;candidateFrame.issued=1;candidateFrame.roiCaptured=1;candidateFrame.roiComplete=1;candidateFrame.cameraCbSlot=1;
    candidateFixture.candidateFrames.push_back(candidateFrame);candidateFixture.candidateOpticDraws=1;candidateFixture.candidateRoiDraw=0;
    edvr::EyeDrawSnapshot::CandidateInput candidateMaterial{};candidateMaterial.draw=0;candidateMaterial.slot=1;candidateMaterial.kind=0;
    candidateMaterial.viewDimension=D3D11_SRV_DIMENSION_BUFFER;candidateMaterial.bytes=96;candidateMaterial.width=96;
    candidateMaterial.sourceResource=0x12340000;candidateMaterial.sourceView=0x56780000;
    candidateMaterial.sourceWidth=96;candidateMaterial.elements=24;candidateMaterial.stride=4;
    uint8_t materialBytes[96];std::memset(materialBytes,0x5a,sizeof(materialBytes));
    stageBufferWindow(materialBytes,sizeof(materialBytes),0,sizeof(materialBytes),candidateMaterial.bufferStage);
    candidateFixture.candidateInputs.push_back(std::move(candidateMaterial));candidateFixture.candidateInputBytes=sizeof(materialBytes);
    edvr::EyeDrawSnapshot::CandidateSampler candidateSampler{};candidateSampler.draw=0;candidateSampler.present=1;
    candidateSampler.desc.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;candidateSampler.desc.AddressU=D3D11_TEXTURE_ADDRESS_WRAP;
    candidateSampler.desc.AddressV=D3D11_TEXTURE_ADDRESS_WRAP;candidateSampler.desc.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
    candidateSampler.desc.MaxLOD=D3D11_FLOAT32_MAX;candidateFixture.candidateSamplers.push_back(candidateSampler);
    D3D11_TEXTURE2D_DESC roiDesc{};roiDesc.Width=roiDesc.Height=512;roiDesc.MipLevels=roiDesc.ArraySize=roiDesc.SampleDesc.Count=1;
    roiDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;roiDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> roiSource;ComPtr<ID3D11RenderTargetView> roiRtv;hr(dev->CreateTexture2D(&roiDesc,nullptr,&roiSource));
    hr(dev->CreateRenderTargetView(roiSource.Get(),nullptr,&roiRtv));D3D11_TEXTURE2D_DESC roiStageDesc=roiDesc;
    roiStageDesc.Usage=D3D11_USAGE_STAGING;roiStageDesc.BindFlags=0;roiStageDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    for(uint32_t phase=0;phase<2;++phase) {
        const FLOAT colour[4]={phase?0.7f:0.1f,0.2f,phase?0.1f:0.3f,1.f};ctx->ClearRenderTargetView(roiRtv.Get(),colour);
        edvr::EyeDrawSnapshot::EffectImage image{};image.source=roiSource;image.draw=0;image.after=phase;
        image.width=image.height=image.sourceWidth=image.sourceHeight=512;image.format=DXGI_FORMAT_R8G8B8A8_UNORM;image.bytes=512*512*4;
        hr(dev->CreateTexture2D(&roiStageDesc,nullptr,&image.stage));ctx->CopyResource(image.stage.Get(),roiSource.Get());
        candidateFixture.effectImages.push_back(std::move(image));candidateFixture.effectImageBytes+=512*512*4;
    }
    wait_gpu(ctx.Get(),dev.Get());
    check(candidateFixture.write(ctx.Get(),(std::wstring(argv[1])+L".candidate").c_str()),"nonempty candidate snapshot write");
    edvr::EyeDrawSnapshot capacityFixture;capacityFixture.candidateUiFirstLedgerFrame=100;
    uint32_t capacityTotal=0,capacityNonUi=0,capacityUi[16][3]{};
    auto appendCapacityDraw=[&](bool ui,uint32_t frame,uint32_t eye) {
        check(edvr::EyeDrawSnapshot::candidateCapacityAvailable(ui,ui?capacityUi[frame-100][eye]:0,capacityNonUi,capacityTotal),
              "candidate capture admission accepts reserved full-window row");
        const uint32_t drawIndex=uint32_t(capacityFixture.draws.size());edvr::EyeDrawSnapshot::Draw d{};
        d.vs=ui?edvr::EyeDrawSnapshot::kWeaponUiVs:edvr::EyeDrawSnapshot::kF77HeldCandidateVs;
        d.ps=ui?edvr::EyeDrawSnapshot::kWeaponUiPs:0;d.frame=frame;d.ordinal=drawIndex;d.kind='I';d.count=3;d.instances=1;
        capacityFixture.draws.push_back(std::move(d));edvr::EyeDrawSnapshot::CandidateFrame c{};
        c.draw=drawIndex;c.ledgerFrame=frame;c.renderFrame=frame+1000;c.eye=ui?(eye==0?0:eye==1?1:-1):-1;
        c.issued=2;c.cameraCbSlot=0;capacityFixture.candidateFrames.push_back(c);
        if(ui){++capacityUi[frame-100][eye];++capacityFixture.candidateUiCounts[frame-100][eye];}
        else {++capacityNonUi;++capacityFixture.candidateNonUiDraws;}
        ++capacityTotal;
    };
    for(uint32_t n=0;n<edvr::EyeDrawSnapshot::kCandidateNonUiMax;++n)appendCapacityDraw(false,1,0);
    for(uint32_t frame=100;frame<116;++frame)for(uint32_t eye=0;eye<3;++eye)
        for(uint32_t n=0;n<edvr::EyeDrawSnapshot::kCandidateUiPerFrameEye;++n)appendCapacityDraw(true,frame,eye);
    check(capacityTotal==1152&&capacityNonUi==384&&capacityFixture.candidateFrames.size()==1152,
          "candidate fixture fills every independently reserved row");
    check(capacityFixture.write(ctx.Get(),(std::wstring(argv[1])+L".capacity").c_str()),"full-window candidate snapshot write");
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
    {
        using S=edvr::EyeDrawSnapshot;edvr::EyeLateMenuCapture menu;menu.arm(800);
        D3D11_INPUT_ELEMENT_DESC menuElements[3]={opticElements[0],opticElements[1],
            {"INSTANCEANDMODELDATAINDEX",0,DXGI_FORMAT_R32G32_UINT,0,0,D3D11_INPUT_PER_INSTANCE_DATA,1}};
        ComPtr<ID3D11InputLayout> menuLayout;hr(dev->CreateInputLayout(menuElements,3,opticBlob->GetBufferPointer(),opticBlob->GetBufferSize(),&menuLayout));
        edvr::EyeDrawSnapshot::rememberLayout(menuLayout.Get(),menuElements,3,S::kLateMenuVs);
        ctx->IASetInputLayout(menuLayout.Get());UINT menuStride=opticStride,menuOffset=opticBindingOffset;
        ctx->IASetVertexBuffers(1,1,opticVb.GetAddressOf(),&menuStride,&menuOffset);
        std::vector<uint16_t> menuIndices(364,2);for(size_t i=4;i<menuIndices.size();++i)menuIndices[i]=static_cast<uint16_t>(2+(i%4));
        D3D11_BUFFER_DESC mid{};mid.ByteWidth=static_cast<UINT>(menuIndices.size()*2);mid.Usage=D3D11_USAGE_DEFAULT;mid.BindFlags=D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA mi{menuIndices.data(),0,0};ComPtr<ID3D11Buffer> menuIb;hr(dev->CreateBuffer(&mid,&mi,&menuIb));
        ctx->IASetIndexBuffer(menuIb.Get(),DXGI_FORMAT_R16_UINT,4);
        ComPtr<ID3D11Buffer> tables[2];ComPtr<ID3D11ShaderResourceView> views[2];
        std::vector<uint8_t> tableBytes[2]={std::vector<uint8_t>(672,0x47),std::vector<uint8_t>(192,0x58)};
        for(unsigned i=0;i<2;++i){D3D11_BUFFER_DESC menuTableDesc{};menuTableDesc.ByteWidth=static_cast<UINT>(tableBytes[i].size());menuTableDesc.Usage=D3D11_USAGE_DEFAULT;
            menuTableDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;menuTableDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;menuTableDesc.StructureByteStride=i?48:336;
            D3D11_SUBRESOURCE_DATA data{tableBytes[i].data(),0,0};hr(dev->CreateBuffer(&menuTableDesc,&data,&tables[i]));
            D3D11_SHADER_RESOURCE_VIEW_DESC menuViewDesc{};menuViewDesc.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;menuViewDesc.Buffer.NumElements=menuTableDesc.ByteWidth/menuTableDesc.StructureByteStride;
            hr(dev->CreateShaderResourceView(tables[i].Get(),&menuViewDesc,&views[i]));ctx->VSSetShaderResources(i?38:33,1,views[i].GetAddressOf());}
        const uint32_t menuIds[4]={0,0,1,0};D3D11_BUFFER_DESC idDesc{};idDesc.ByteWidth=sizeof(menuIds);idDesc.Usage=D3D11_USAGE_DEFAULT;idDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA idData{menuIds,0,0};ComPtr<ID3D11Buffer> ids;hr(dev->CreateBuffer(&idDesc,&idData,&ids));
        UINT idStride=8,idOffset=0;ctx->IASetVertexBuffers(0,1,ids.GetAddressOf(),&idStride,&idOffset);
        ComPtr<ID3D11DeviceContext1> ranged;hr(ctx.As(&ranged));
        D3D11_BUFFER_DESC cd{};cd.ByteWidth=5120;cd.Usage=D3D11_USAGE_DEFAULT;cd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> camera;hr(dev->CreateBuffer(&cd,nullptr,&camera));
        std::vector<uint8_t> cameraBytes(cd.ByteWidth,0x35);ctx->UpdateSubresource(camera.Get(),0,nullptr,cameraBytes.data(),0,0);
        UINT first=16,num=288;ranged->VSSetConstantBuffers1(1,1,camera.GetAddressOf(),&first,&num);
        edvr::EyeMeshJoinStamp join;join.sceneFrame=799;join.validFlags=1;
        for(int eye=0;eye<2;++eye) {
            menu.capture(ctx.Get(),801,0,eye,join,eye*10,0xDE545DC8EE4FBB87ull,0,'I',900,1,0,2,static_cast<int32_t>(opticBase),1);
            menu.evidence.candidateDrawIssued(ctx.Get(),true,0xDE545DC8EE4FBB87ull,0);
            for(unsigned family=0;family<3;++family) {
                const uint64_t vs=family==1?S::kLateMenuBaseVs:S::kLateMenuVs,ps=family==1?S::kLateMenuBasePs:S::kLateMenuPs;
                menu.capture(ctx.Get(),801,0,eye,join,eye*10+family+1,vs,ps,'I',family==0?6u:360u,1,1,2,static_cast<int32_t>(opticBase),2);
                menu.evidence.candidateDrawIssued(ctx.Get(),family!=0,vs,ps);
                std::fill(tableBytes[0].begin(),tableBytes[0].end(),static_cast<uint8_t>(0x48+eye*3+family));
                ctx->UpdateSubresource(tables[0].Get(),0,nullptr,tableBytes[0].data(),0,0);
            }
        }
        check(menu.rows.size()==8&&menu.evidence.draws.size()==8&&menu.window.scene[0]&&menu.window.scene[1],
              "independent menu snapshot retains both eyes, earlier scene references, skipped siblings and both menu families with zero native frame");
        check(menu.rows[1].constants[1].first==16&&menu.evidence.draws[1].copied[1]==288*16&&
              menu.evidence.draws[1].streams[1].captureOffset==opticVertexBegin,
              "menu capture preserves ranged camera CB origin and large indexed geometry base");
        check(menu.evidence.meshBuffers.size()==12&&menu.evidence.draws[1].mesh[0]!=menu.evidence.draws[2].mesh[0]&&
              menu.evidence.draws[1].streams[0].captureOffset==8&&menu.evidence.draws[1].streams[0].copied==8,
              "menu structured inputs are copied per draw; instance IDs retain only the exact start-instance window without whole-VB duplication");
        wait_gpu(ctx.Get(),dev.Get());
        auto& cachedAvatar=menu.rows[1].avatar;cachedAvatar.status=2;cachedAvatar.validFlags=1;cachedAvatar.sourceFrame=123;
        cachedAvatar.sampleQpc=1000;cachedAvatar.qpc=900;cachedAvatar.rootQpc=800;cachedAvatar.qpcFrequency=1000;
        for(unsigned i=0;i<4;++i)cachedAvatar.froot[i*5]=cachedAvatar.m3p[i*5]=cachedAvatar.G[i*5]=1.0f;
        check(menu.write(ctx.Get(),(std::wstring(argv[1])+L".menu.bin").c_str(),(std::wstring(argv[1])+L".menu.json").c_str(),menuInputFixture),
              "menu binary and bounded binding/selection companion write");
        menu.release();check(menu.evidence.draws.empty()&&menu.evidence.meshBuffers.empty()&&menu.rows.empty()&&menu.window.frame==0,
              "closing or aborting a menu run releases staging and source references before another manual capture");
    }
    std::puts("GPU draw snapshot capture passed");
}
