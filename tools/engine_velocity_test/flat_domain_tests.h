#pragma once
#include "../../src/common/runtime_profile.h"

namespace flat_domain_tests {
inline void run(const lifecycle_tests::Harness& h) {
    using namespace edvr;
    using Microsoft::WRL::ComPtr;
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
        const bool began=engineVelocityFlatDomainBeginDraw(h.context,game.sourceDepth.Get(),code->GetBufferPointer(),code->GetBufferSize(),nullptr,0,domain,&reason);
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
            const auto pixels=lifecycle_tests::readTexture(h,resource.Get(),2,&width);
            unsigned marked=0;const float expected=domain==FlatEngineDomain::World?0:domain==FlatEngineDomain::ForeignPool?-13:11;
            for(size_t i=0;i<pixels.size()/2;++i)if(pixels[i*2]!=-1) {
                if(pixels[i*2]!=expected)std::printf("null marker domain=%u value=%.9g expected=%.9g\n",unsigned(domain),pixels[i*2],expected);
                h.check(pixels[i*2]==expected,"null-PS marker names exact camera domain and slot");
                h.check(pixels[i*2+1]==depth[i*2],"null-PS marker stores exact passing device depth");++marked;
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
    const bool combined=engineVelocityFlatDomainBeginDraw(h.context,game.sourceDepth.Get(),code->GetBufferPointer(),code->GetBufferSize(),psCode->GetBufferPointer(),psCode->GetBufferSize(),FlatEngineDomain::ForeignPool,&reason,true);
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
            const auto pixels=lifecycle_tests::readTexture(h,resource.Get(),2,&width);
            const auto cover=lifecycle_tests::readTexture(h,mask.Get(),1,&width);
            unsigned written=0;for(size_t i=0;i<cover.size();++i) {
                h.check(cover[i]==(pixels[i*2]==-1?0:1),"combined actual bracket one draw carries both marker and mask");
                if(cover[i]){h.check(pixels[i*2]==-13,"combined actual bracket preserves negative foreign owner");++written;}
            }
            h.check(written>0,"combined actual original fragment footprint is nonempty");
        }
    }
    engineVelocityConfigure(false);h.context->ClearState();g_runtimeProfile=previous;
}
} // namespace flat_domain_tests
