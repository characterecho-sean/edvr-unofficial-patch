#pragma once
#include "../../src/common/runtime_profile.h"
#include <fstream>

namespace flat_domain_tests {
inline void recordedMarkerTests(const lifecycle_tests::Harness& h) {
    using namespace edvr;
    using namespace dxbc_engine_velocity_detail;
    using Microsoft::WRL::ComPtr;
    auto read=[&](const char* name) {
        std::ifstream file(std::string("tools/flat_temporal_test/fixtures/")+name+".dxbc",std::ios::binary|std::ios::ate);
        const auto size=file.tellg();
        if(size<=0 || size>16*1024*1024)return std::vector<BYTE>{};
        std::vector<BYTE> bytes(static_cast<size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),size);return bytes;
    };
    auto program=[](const std::vector<BYTE>& bytes) {
        for(const auto& chunk:parseContainer(bytes.data(),bytes.size(),kPs50))
            if(isProgram(chunk.tag))return programWords(chunk.bytes);
        return std::vector<uint32_t>{};
    };
    auto preserved=[&](const std::vector<BYTE>& original,const std::vector<BYTE>& patched) {
        const auto before=program(original),after=program(patched);size_t cursor=2;unsigned discards=0;
        for(size_t at=2;at<before.size();) {
            const auto length=instructionLength(before,at);const auto opcode=before[at]&0x7ffu;
            if(opcode==kOpDclTemps || (opcode==kOpDclInputPsSiv && length>=4 && before[at+3]==1)) {at+=length;continue;}
            bool found=false;
            while(cursor<after.size()) {
                const auto n=instructionLength(after,cursor);
                if(n==length && std::equal(before.begin()+at,before.begin()+at+length,after.begin()+cursor)) {found=true;cursor+=n;break;}
                cursor+=n;
            }
            h.check(found,"flat marker retains each original PS instruction in order, including discard");
            discards+=opcode==13;at+=length;
        }
        return discards;
    };
    struct Recorded {const char* vs;const char* ps;bool pool;};
    for(const auto& record:std::array<Recorded,3>{{
        {"vs_F516BF0201303B87","ps_B40B0462256E31C2",true},
        {"vs_318693134643131A","ps_4EBA794D13603735",true},
        {"vs_72BDD292154158AD","ps_76849D64AC657DB9",false}}}) {
        const auto vs=read(record.vs),ps=read(record.ps);h.check(!vs.empty() && !ps.empty(),"recorded flat marker fixture present");
        if(vs.empty() || ps.empty())continue;
        EngineVelocityInputs inputs{};std::string why;
        if(record.pool)h.check(engineVelocityDeriveInputs(vs.data(),vs.size(),inputs,why),why.c_str());
        else {inputs.positionRegister=30;inputs.identityRegister=31;inputs.identityComponent=0;}
        std::vector<BYTE> patched;
        const auto kind=record.pool?FlatMarkerKind::WorldPool:FlatMarkerKind::World;
        h.check(engineVelocityPatchPs(ps.data(),ps.size(),inputs,patched,why,false,kind),why.c_str());
        if(patched.empty())continue;
        ComPtr<ID3D11PixelShader> shader;
        h.check(SUCCEEDED(h.device->CreatePixelShader(patched.data(),patched.size(),nullptr,&shader)),"recorded flat depth-only or nonpool world PS creates on WARP");
        const unsigned discards=preserved(ps,patched);
        if(record.pool) {
            h.check(discards>0,"recorded nonnull depth-only original discard preserved");
            unsigned tokenSlot=~0u;
            h.check(engineVelocityPatchPs(ps.data(),ps.size(),inputs,patched,why,false,FlatMarkerKind::ForeignPoolProvenance,&tokenSlot),why.c_str());
            shader.Reset();
            h.check(tokenSlot<14 && SUCCEEDED(h.device->CreatePixelShader(patched.data(),patched.size(),nullptr,&shader)),
                "recorded discarded depth-only provenance PS creates with a free token binding");
            preserved(ps,patched);
            h.check(!engineVelocityPatchPs(ps.data(),ps.size(),inputs,patched,why,false,FlatMarkerKind::ForeignPoolProvenance),
                "provenance patch cannot lose required token binding metadata");
            h.check(!engineVelocityPatchPs(ps.data(),ps.size(),inputs,patched,why) && patched.empty(),
                "VR None mode still refuses original PS without color output");
        } else {
            h.check(!engineVelocityPatchPs(ps.data(),ps.size(),inputs,patched,why,false,FlatMarkerKind::ForeignPool) && patched.empty(),
                "nonpool World sentinel does not authorize missing foreign pool identity");
        }
    }
}
inline void run(const lifecycle_tests::Harness& h) {
    using namespace edvr;
    using Microsoft::WRL::ComPtr;
    recordedMarkerTests(h);
    const auto previous=g_runtimeProfile;g_runtimeProfile=RuntimeProfile::Flat;
    lifecycle_tests::Game game(h);game.setup();game.makeSource(40,24);
    const auto code=game.compile(std::string(shader_tests::kVsCommon)+shader_tests::kVsB,"vs_5_0");
    ComPtr<ID3D11VertexShader> vs;
    h.check(code && SUCCEEDED(h.device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs)),"flat domain original VS fixture");
    if(!vs){g_runtimeProfile=previous;return;}
    auto seedColors=[&]() {
        for(unsigned target=0;target<4;++target) {
            std::vector<unsigned char> bytes(40*24*4);
            for(size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<unsigned char>(1+(i*17+target*53)%254);
            h.context->UpdateSubresource(game.sourceColour[target].Get(),0,nullptr,bytes.data(),40*4,0);
        }
    };
    engineVelocityConfigure(true);game.beginFrame();game.sourcePass(0);game.setVs(vs.Get(),lifecycle_tests::kVsHash);
    seedColors();
    game.setPs(nullptr,0);h.context->DrawInstanced(4,1,0,0);
    UINT width=0;
    const auto originalDepth=lifecycle_tests::readTexture(h,game.sourceDepth.Get(),2,&width);
    std::array<std::vector<float>,4> originalColors;
    for(unsigned target=0;target<4;++target)originalColors[target]=lifecycle_tests::readTexture(h,game.sourceColour[target].Get(),1,&width);
    for(auto domain:{FlatEngineDomain::World,FlatEngineDomain::ForeignPool,FlatEngineDomain::WorldPool}) {
        game.beginFrame();game.sourcePass(0);game.setVs(vs.Get(),lifecycle_tests::kVsHash);game.setPs(nullptr,0);
        seedColors();
        const char* reason=nullptr;
        const bool began=engineVelocityFlatDomainBeginDraw(h.context,game.sourceDepth.Get(),code->GetBufferPointer(),code->GetBufferSize(),nullptr,0,domain,&reason,false,1,2);
        if(!began)std::printf("null bracket failure domain=%u: %s\n",unsigned(domain),reason?reason:"unknown");
        h.check(began,reason?reason:"flat null-PS domain bracket");
        if(!began)continue;
        h.context->DrawInstanced(4,1,0,0);engineVelocityFlatDomainEndDraw(h.context);
        const auto depth=lifecycle_tests::readTexture(h,game.sourceDepth.Get(),2,&width);
        h.check(depth==originalDepth,"null-PS marker preserves exact original depth bytes");
        for(unsigned target=0;target<4;++target) {
            const auto color=lifecycle_tests::readTexture(h,game.sourceColour[target].Get(),1,&width);
            if(color.size()==originalColors[target].size())for(size_t i=0;i<color.size();++i)
                if(std::memcmp(&color[i],&originalColors[target][i],sizeof(float))) {
                    unsigned a=0,b=0;std::memcpy(&a,&color[i],4);std::memcpy(&b,&originalColors[target][i],4);
                    std::printf("null color domain=%u target=%u word=%zu actual=%08x original=%08x\n",unsigned(domain),target,i,a,b);break;
                }
            // readTexture stores raw R8G8B8A8 bytes in float-sized words; odd
            // color patterns can have NaN bit patterns, so compare bytes.
            h.check(color.size()==originalColors[target].size() &&
                std::memcmp(color.data(),originalColors[target].data(),color.size()*sizeof(float))==0,
                "null-PS marker preserves every original color pixel");
        }
        ComPtr<ID3D11PixelShader> restored;h.context->PSGetShader(&restored,nullptr,nullptr);
        h.check(!restored,"null-PS marker restores original null PS");
        ID3D11RenderTargetView* targets[8]{};h.context->OMGetRenderTargets(8,targets,nullptr);
        h.check(!targets[6],"null-PS marker restores absent original MRT6");for(auto* p:targets)if(p)p->Release();
        ComPtr<ID3D11ShaderResourceView> owners;h.check(engineVelocityFlatDomainSlots(game.sourceDepth.Get(),&owners),"null-PS final ownership accessible");
        if(owners) {
            ComPtr<ID3D11Resource> resource;owners->GetResource(&resource);
            const auto pixels=lifecycle_tests::readTexture(h,resource.Get(),4,&width);
            unsigned marked=0;const float expected=domain==FlatEngineDomain::World?0.f:domain==FlatEngineDomain::ForeignPool?-13.f:11.f;
            for(size_t i=0;i<pixels.size()/4;++i)if(pixels[i*4]!=-1) {
                h.check(pixels[i*4]==expected,"null-PS marker names exact camera domain and slot");
                h.check(pixels[i*4+1]==depth[i*2],"null-PS marker stores exact passing device depth");
                if(domain==FlatEngineDomain::ForeignPool) {
                    if(!(pixels[i*4+2]<=1 && pixels[i*4+3]==1))std::printf("null tuple %.9g %.9g %.9g %.9g\n",pixels[i*4],pixels[i*4+1],pixels[i*4+2],pixels[i*4+3]);
                    h.check(pixels[i*4+2]<=1 && pixels[i*4+3]==1,"null foreign marker stamps native primitive and actual draw token");
                }
                ++marked;
            }
            h.check(marked>0,"null-PS original passing fragments produce ownership");
        }
    }
    // Mimic untrusted.beginDraw followed by the domain bracket: the coverage
    // shader/RT7 are restored first, then the outer owner restores game state.
    const auto psCode=game.compile(shader_tests::kPsB,"ps_5_0");
    std::vector<BYTE> coverageCode;std::string why;
    h.check(flatOverlayPatchPs(psCode->GetBufferPointer(),psCode->GetBufferSize(),coverageCode,why),why.c_str());
    ComPtr<ID3D11PixelShader> coveragePs;
    h.check(SUCCEEDED(h.device->CreatePixelShader(coverageCode.data(),coverageCode.size(),nullptr,&coveragePs)),"combined outer coverage PS");
    D3D11_TEXTURE2D_DESC maskDesc{};maskDesc.Width=40;maskDesc.Height=24;maskDesc.MipLevels=maskDesc.ArraySize=maskDesc.SampleDesc.Count=1;
    maskDesc.Format=DXGI_FORMAT_R32_FLOAT;maskDesc.BindFlags=D3D11_BIND_RENDER_TARGET;maskDesc.Usage=D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Texture2D> mask;ComPtr<ID3D11RenderTargetView> maskRtv;
    h.check(SUCCEEDED(h.device->CreateTexture2D(&maskDesc,nullptr,&mask)) && SUCCEEDED(h.device->CreateRenderTargetView(mask.Get(),nullptr,&maskRtv)),"combined outer coverage target");
    game.beginFrame();game.sourcePass(0);game.setVs(vs.Get(),lifecycle_tests::kVsHash);game.setPs(coveragePs.Get(),0);
    ID3D11RenderTargetView* attached[8]={game.sourceRtv[0].Get(),game.sourceRtv[1].Get(),game.sourceRtv[2].Get(),game.sourceRtv[3].Get(),nullptr,nullptr,nullptr,maskRtv.Get()};
    h.context->OMSetRenderTargets(8,attached,game.sourceDsv.Get());h.context->OMSetBlendState(nullptr,nullptr,~0u);
    const float zero[4]{};h.context->ClearRenderTargetView(maskRtv.Get(),zero);
    const char* reason=nullptr;
    const bool combined=engineVelocityFlatDomainBeginDraw(h.context,game.sourceDepth.Get(),code->GetBufferPointer(),code->GetBufferSize(),psCode->GetBufferPointer(),psCode->GetBufferSize(),FlatEngineDomain::ForeignPool,&reason,true,1,2);
    h.check(combined,reason?reason:"combined original marker bracket");
    if(combined) {
        h.context->DrawInstanced(4,1,0,0);engineVelocityFlatDomainEndDraw(h.context);
        ComPtr<ID3D11PixelShader> restored;h.context->PSGetShader(&restored,nullptr,nullptr);
        h.check(restored==coveragePs,"combined inner restores qualified outer coverage PS");
        ID3D11RenderTargetView* actual[8]{};h.context->OMGetRenderTargets(8,actual,nullptr);
        for(unsigned i=0;i<8;++i){h.check(actual[i]==attached[i],"combined inner restores all outer render targets");if(actual[i])actual[i]->Release();}
        ComPtr<ID3D11ShaderResourceView> owners;h.check(engineVelocityFlatDomainSlots(game.sourceDepth.Get(),&owners),"combined final owner plane");
        if(owners) {
            ComPtr<ID3D11Resource> resource;owners->GetResource(&resource);
            const auto pixels=lifecycle_tests::readTexture(h,resource.Get(),4,&width);
            const auto cover=lifecycle_tests::readTexture(h,mask.Get(),1,&width);
            unsigned written=0;for(size_t i=0;i<cover.size();++i) {
                h.check(cover[i]==(pixels[i*4]==-1?0:1),"combined actual bracket one draw carries both marker and mask");
                if(cover[i]){h.check(pixels[i*4]==-13 && pixels[i*4+3]==1,"combined actual bracket preserves negative foreign owner and token");++written;}
            }
            h.check(written>0,"combined actual original fragment footprint is nonempty");
        }
    }
    // World material shading and its owner export must pass the very same
    // original conditional stencil test, including a rejected fragment.
    ComPtr<ID3D11PixelShader> originalPs;
    h.check(SUCCEEDED(h.device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&originalPs)),
        "world conditional-stencil original PS");
    D3D11_DEPTH_STENCIL_DESC conditionalDesc{};
    conditionalDesc.DepthEnable=TRUE;conditionalDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
    conditionalDesc.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
    conditionalDesc.StencilEnable=TRUE;conditionalDesc.StencilReadMask=255;conditionalDesc.StencilWriteMask=0;
    conditionalDesc.FrontFace.StencilFunc=D3D11_COMPARISON_EQUAL;
    conditionalDesc.FrontFace.StencilFailOp=conditionalDesc.FrontFace.StencilDepthFailOp=conditionalDesc.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
    conditionalDesc.BackFace=conditionalDesc.FrontFace;
    ComPtr<ID3D11DepthStencilState> conditional;
    h.check(SUCCEEDED(h.device->CreateDepthStencilState(&conditionalDesc,&conditional)),"world conditional-stencil state");
    for(unsigned stencil:{0u,1u}) {
        auto prepare=[&]() {
            game.beginFrame();game.sourcePass(0);game.setVs(vs.Get(),lifecycle_tests::kVsHash);game.setPs(originalPs.Get(),0);
            seedColors();h.context->ClearDepthStencilView(game.sourceDsv.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,0,BYTE(stencil));
            h.context->OMSetDepthStencilState(conditional.Get(),1);
        };
        prepare();
        const auto before=lifecycle_tests::readTexture(h,game.sourceColour[0].Get(),1,&width);
        h.context->DrawInstanced(4,1,0,0);
        std::array<std::vector<float>,4> nativeColors;
        for(unsigned target=0;target<4;++target)nativeColors[target]=lifecycle_tests::readTexture(h,game.sourceColour[target].Get(),1,&width);
        const auto nativeDepth=lifecycle_tests::readTexture(h,game.sourceDepth.Get(),2,&width);
        const bool nativeChanged=before.size()==nativeColors[0].size() &&
            std::memcmp(before.data(),nativeColors[0].data(),before.size()*sizeof(float))!=0;
        h.check(nativeChanged==(stencil==1),"original conditional stencil selects real color footprint");
        prepare();const char* conditionalReason=nullptr;
        const bool marked=engineVelocityFlatDomainBeginDraw(h.context,game.sourceDepth.Get(),code->GetBufferPointer(),code->GetBufferSize(),
            psCode->GetBufferPointer(),psCode->GetBufferSize(),FlatEngineDomain::WorldPool,&conditionalReason);
        h.check(marked,conditionalReason?conditionalReason:"world pool conditional-stencil bracket");
        if(!marked)continue;
        ComPtr<ID3D11DepthStencilState> bound;UINT reference=0;
        h.context->OMGetDepthStencilState(&bound,&reference);
        h.check(bound==conditional && reference==1,"world marker retains original conditional stencil during draw");
        h.context->DrawInstanced(4,1,0,0);engineVelocityFlatDomainEndDraw(h.context);
        bound.Reset();h.context->OMGetDepthStencilState(&bound,&reference);
        h.check(bound==conditional && reference==1,"world marker restores original conditional stencil state");
        for(unsigned target=0;target<4;++target) {
            const auto color=lifecycle_tests::readTexture(h,game.sourceColour[target].Get(),1,&width);
            h.check(color.size()==nativeColors[target].size() &&
                std::memcmp(color.data(),nativeColors[target].data(),color.size()*sizeof(float))==0,
                "world marker preserves original color bytes under conditional stencil");
        }
        const auto markedDepth=lifecycle_tests::readTexture(h,game.sourceDepth.Get(),2,&width);
        h.check(markedDepth.size()==nativeDepth.size() && std::memcmp(markedDepth.data(),nativeDepth.data(),nativeDepth.size()*sizeof(float))==0,
            "world marker preserves every original depth and stencil byte");
        ComPtr<ID3D11ShaderResourceView> owners;h.check(engineVelocityFlatDomainSlots(game.sourceDepth.Get(),&owners),"world conditional-stencil owner plane");
        if(owners) {
            ComPtr<ID3D11Resource> resource;owners->GetResource(&resource);
            const auto pixels=lifecycle_tests::readTexture(h,resource.Get(),4,&width);unsigned written=0;
            for(size_t i=0;i<pixels.size()/4;++i)if(pixels[i*4]!=-1) {
                ++written;h.check(pixels[i*4]==11 && pixels[i*4+2]==0 && pixels[i*4+3]==0,"conditional stencil passing owner remains exact positive pool slot with cleared provenance");
            }
            h.check((written>0)==(stencil==1),"same original conditional stencil selects both color and owner output");
        }
    }
    // Coincident triangles cannot share temporal provenance merely because
    // their slot and depth agree. Preserve original discard and test both an
    // intra-draw primitive choice and a later actual draw with the same ID.
    const char* discardSource=R"HLSL(
cbuffer Gate : register(b0) { float4 gate; };
struct Input { nointerpolation uint id:__USER_VERTEX_FACEINVARIANT;
    float3 n:__USER_VERTEX_M_LIGHTINGNORMAL;float3 t:__USER_VERTEX_M_LIGHTINGTANGENT;
    float2 uv:__USER_VERTEX_M_TEXCOORD;float4 p:SV_Position; };
float4 main(Input i,uint primitive:SV_PrimitiveID):SV_Target0 {
    if(primitive!=(uint)gate.x)discard;
    return float4(primitive==0?1:0,primitive==1?1:0,(i.id&1)?1:0,1);
}
)HLSL";
    const auto discardCode=game.compile(discardSource,"ps_5_0");
    ComPtr<ID3D11PixelShader> discardPs;
    h.check(SUCCEEDED(h.device->CreatePixelShader(discardCode->GetBufferPointer(),discardCode->GetBufferSize(),nullptr,&discardPs)),"native primitive-discard PS");
    EngineVelocityInputs provenanceInputs{};
    h.check(engineVelocityDeriveInputs(code->GetBufferPointer(),code->GetBufferSize(),provenanceInputs,why),why.c_str());
    unsigned tokenSlot=~0u;std::vector<BYTE> provenanceCode;
    const bool provenancePatched=engineVelocityPatchPs(discardCode->GetBufferPointer(),discardCode->GetBufferSize(),provenanceInputs,provenanceCode,why,false,
        dxbc_engine_velocity_detail::FlatMarkerKind::ForeignPoolProvenance,&tokenSlot);
    h.check(provenancePatched,why.c_str());
    h.check(tokenSlot==13,"existing native primitive input reuses a verified free b13 token slot");
    ComPtr<ID3D11PixelShader> provenancePs;
    h.check(SUCCEEDED(h.device->CreatePixelShader(provenanceCode.data(),provenanceCode.size(),nullptr,&provenancePs)),"native primitive-discard derivative creates");
    std::string occupiedSource;
    for(unsigned slot=0;slot<14;++slot)occupiedSource+="cbuffer C"+std::to_string(slot)+" : register(b"+std::to_string(slot)+") {float4 c"+std::to_string(slot)+";};\n";
    occupiedSource+="float4 main(nointerpolation uint id:__USER_VERTEX_FACEINVARIANT):SV_Target0 {return float4(id,0,0,1)";
    for(unsigned slot=0;slot<14;++slot)occupiedSource+="+c"+std::to_string(slot);
    occupiedSource+=";}";
    const auto occupiedCode=game.compile(occupiedSource,"ps_5_0");
    std::vector<BYTE> refused;unsigned refusedSlot=0;
    const bool occupiedPatched=engineVelocityPatchPs(occupiedCode->GetBufferPointer(),occupiedCode->GetBufferSize(),provenanceInputs,refused,why,false,
        dxbc_engine_velocity_detail::FlatMarkerKind::ForeignPoolProvenance,&refusedSlot);
    h.check(!occupiedPatched && refused.empty() && refusedSlot==~0u && why=="no free foreign token constant buffer",
        "foreign provenance refuses all fourteen occupied original CB slots without selecting or rebinding one");
    const float coincident[]={-.4f,-.4f,0,.4f,-.4f,0,0,.4f,0,-.4f,-.4f,0,.4f,-.4f,0,0,.4f,0};
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(coincident);bd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertexData{coincident,0,0};ComPtr<ID3D11Buffer> triangles,gateBuffer,tokenSentinel;
    h.check(SUCCEEDED(h.device->CreateBuffer(&bd,&vertexData,&triangles)),"coincident primitive geometry");
    bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    h.check(SUCCEEDED(h.device->CreateBuffer(&bd,nullptr,&gateBuffer)),"native discard gate CB");
    bd.ByteWidth=4096;h.check(SUCCEEDED(h.device->CreateBuffer(&bd,nullptr,&tokenSentinel)),"token binding original ranged CB");
    D3D11_DEPTH_STENCIL_DESC equalDesc{};equalDesc.DepthEnable=TRUE;equalDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;equalDesc.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
    ComPtr<ID3D11DepthStencilState> equalDepth;h.check(SUCCEEDED(h.device->CreateDepthStencilState(&equalDesc,&equalDepth)),"coincident equal-depth original state");
    ComPtr<ID3D11DeviceContext1> ranged;h.context->QueryInterface(IID_PPV_ARGS(&ranged));
    for(unsigned secondAccepted:{0u,2u}) {
        auto prepare=[&]() {
            game.beginFrame();game.sourcePass(0);game.setVs(vs.Get(),lifecycle_tests::kVsHash);game.setPs(discardPs.Get(),0);
            h.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11Buffer* vb=triangles.Get();UINT stride=12,offset=0;h.context->IASetVertexBuffers(0,1,&vb,&stride,&offset);
            h.context->OMSetDepthStencilState(equalDepth.Get(),0);
            ID3D11Buffer* cb=gateBuffer.Get();h.context->PSSetConstantBuffers(0,1,&cb);
            cb=tokenSentinel.Get();UINT first=16,count=16;
            if(ranged)ranged->PSSetConstantBuffers1(tokenSlot,1,&cb,&first,&count);else h.context->PSSetConstantBuffers(tokenSlot,1,&cb);
        };
        auto draw=[&](unsigned accepted,unsigned token,bool marked) {
            const float gate[4]={float(accepted),0,0,0};h.context->UpdateSubresource(gateBuffer.Get(),0,nullptr,gate,0,0);
            if(marked) {
                const char* failure=nullptr;
                const bool began=engineVelocityFlatDomainBeginDraw(h.context,game.sourceDepth.Get(),code->GetBufferPointer(),code->GetBufferSize(),
                    discardCode->GetBufferPointer(),discardCode->GetBufferSize(),FlatEngineDomain::ForeignPool,&failure,false,token,2);
                h.check(began,failure?failure:"discard provenance actual draw bracket");if(!began)return;
            }
            h.context->DrawInstanced(6,1,0,0);if(marked)engineVelocityFlatDomainEndDraw(h.context);
        };
        prepare();draw(1,11,false);draw(secondAccepted,12,false);
        const auto nativeColor=lifecycle_tests::readTexture(h,game.sourceColour[0].Get(),1,&width);
        const auto nativeDepth=lifecycle_tests::readTexture(h,game.sourceDepth.Get(),2,&width);
        prepare();draw(1,11,true);draw(secondAccepted,12,true);
        const auto actualColor=lifecycle_tests::readTexture(h,game.sourceColour[0].Get(),1,&width);
        const auto actualDepth=lifecycle_tests::readTexture(h,game.sourceDepth.Get(),2,&width);
        h.check(actualColor.size()==nativeColor.size() && !std::memcmp(actualColor.data(),nativeColor.data(),nativeColor.size()*4),"discard provenance preserves original coincident color bytes");
        h.check(actualDepth.size()==nativeDepth.size() && !std::memcmp(actualDepth.data(),nativeDepth.data(),nativeDepth.size()*4),"discard provenance preserves original coincident depth/stencil bytes");
        ComPtr<ID3D11Buffer> restored;UINT first=0,count=0;
        if(ranged)ranged->PSGetConstantBuffers1(tokenSlot,1,&restored,&first,&count);else h.context->PSGetConstantBuffers(tokenSlot,1,&restored);
        h.check(restored==tokenSentinel && (!ranged || (first==16 && count==16)),"foreign token restores exact original CB and ranged binding");
        ComPtr<ID3D11ShaderResourceView> owners;h.check(engineVelocityFlatDomainSlots(game.sourceDepth.Get(),&owners),"coincident provenance plane accessible");
        if(owners) {
            ComPtr<ID3D11Resource> resource;owners->GetResource(&resource);
            const auto pixels=lifecycle_tests::readTexture(h,resource.Get(),4,&width);unsigned selected=0;
            for(size_t i=0;i<pixels.size()/4;++i)if(pixels[i*4]!=-1) {
                ++selected;h.check(pixels[i*4]==-13 && pixels[i*4+1]==nativeDepth[i*2],"discard provenance retains actual pool/depth");
                h.check(pixels[i*4+2]==(secondAccepted==0?0.f:1.f) && pixels[i*4+3]==(secondAccepted==0?12.f:11.f),
                    "final stamp distinguishes coincident primitives and different actual draw tokens, including discarded later draw");
            }
            h.check(selected>0,"coincident original discard passing footprint nonempty");
        }
    }
    // A pre-world marker on one DSV must survive a second DSV in the same
    // present frame. The later named source producer must share the second
    // plane without clearing the foreign tuple already written there.
    {
        const auto depthA=game.sourceDepth;
        D3D11_TEXTURE2D_DESC desc{};depthA->GetDesc(&desc);
        ComPtr<ID3D11Texture2D> depthB;ComPtr<ID3D11DepthStencilView> dsvB;
        h.check(SUCCEEDED(h.device->CreateTexture2D(&desc,nullptr,&depthB)) &&
                SUCCEEDED(h.device->CreateDepthStencilView(depthB.Get(),nullptr,&dsvB)),
                "two-DSV marker fixture creates second depth");
        if(depthB && dsvB) {
            game.beginFrame();game.sourcePass(0);game.setVs(vs.Get(),lifecycle_tests::kVsHash);game.setPs(nullptr,0);
            const char* whyA=nullptr;
            const bool markedA=engineVelocityFlatDomainBeginDraw(h.context,depthA.Get(),code->GetBufferPointer(),code->GetBufferSize(),
                nullptr,0,FlatEngineDomain::World,&whyA,false,1,2);
            h.check(markedA,whyA?whyA:"first DSV marker admitted");
            if(markedA){h.context->DrawInstanced(4,1,0,0);engineVelocityFlatDomainEndDraw(h.context);}
            ComPtr<ID3D11ShaderResourceView> ownerA;
            h.check(engineVelocityFlatDomainSlots(depthA.Get(),&ownerA),"first DSV marker retrievable before second");
            game.sourceDepth=depthB;game.sourceDsv=dsvB;game.sourcePass(0);game.setVs(vs.Get(),lifecycle_tests::kVsHash);game.setPs(nullptr,0);
            const char* whyB=nullptr;
            const bool markedB=engineVelocityFlatDomainBeginDraw(h.context,depthB.Get(),code->GetBufferPointer(),code->GetBufferSize(),
                nullptr,0,FlatEngineDomain::ForeignPool,&whyB,false,1,2);
            h.check(markedB,whyB?whyB:"second DSV foreign marker admitted in same frame");
            if(markedB){h.context->DrawInstanced(4,1,0,0);engineVelocityFlatDomainEndDraw(h.context);}
            ComPtr<ID3D11ShaderResourceView> ownerB;
            h.check(engineVelocityFlatDomainSlots(depthB.Get(),&ownerB),"second DSV marker retrievable before world producer");
            ComPtr<ID3D11ShaderResourceView> ownerAAgain;
            h.check(engineVelocityFlatDomainSlots(depthA.Get(),&ownerAAgain) && ownerAAgain==ownerA,
                "first DSV marker remains retrievable after second");
            auto pixels=[&](ID3D11ShaderResourceView* owner){
                ComPtr<ID3D11Resource> resource;if(owner)owner->GetResource(&resource);
                UINT width=0;return resource?lifecycle_tests::readTexture(h,resource.Get(),4,&width):std::vector<float>{};
            };
            const auto beforeA=pixels(ownerA.Get()),beforeB=pixels(ownerB.Get());
            unsigned worldA=0,foreignB=0;
            for(size_t i=0;i<beforeA.size()/4;++i)worldA+=beforeA[i*4]==0.f;
            for(size_t i=0;i<beforeB.size()/4;++i)foreignB+=beforeB[i*4]==-13.f;
            h.check(worldA>0 && foreignB>0,"different DSVs contain their own original marker tuples");
            game.writeScene(game.sceneA.Get(),game.rows[0]);
            engineVelocityNoteSource(depthB.Get(),game.sceneA.Get());
            game.setVs();game.setPs(game.ps.Get(),lifecycle_tests::kPsHash);
            game.sourceDraw(9);
            ComPtr<ID3D11ShaderResourceView> afterA,afterB;
            h.check(engineVelocityFlatDomainSlots(depthA.Get(),&afterA) &&
                    engineVelocityFlatDomainSlots(depthB.Get(),&afterB) && afterA==ownerA && afterB==ownerB,
                    "world producer keeps both DSV planes available by exact depth");
            const auto finalA=pixels(afterA.Get()),finalB=pixels(afterB.Get());
            h.check(finalA==beforeA,"world producer on second DSV preserves first DSV marker bytes");
            unsigned preservedForeign=0;
            for(size_t i=0;i<beforeB.size()/4 && i<finalB.size()/4;++i)
                if(beforeB[i*4]==-13.f && std::memcmp(&beforeB[i*4],&finalB[i*4],4*sizeof(float))==0)++preservedForeign;
            h.check(preservedForeign==foreignB,"world producer does not clear prior foreign tuples on its DSV");
        }
    }
    engineVelocityConfigure(false);h.context->ClearState();g_runtimeProfile=previous;
}
} // namespace flat_domain_tests
