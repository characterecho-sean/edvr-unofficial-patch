#pragma once
#include "../../src/d3d11/flat_domain_admission.h"
#include "flat_shader_classifier_tests.h"

inline int flatDomainAdmissionTests() {
    using namespace edvr;int failures=0;
    auto check=[&](bool ok,const char* name){if(!ok){std::printf("FAIL: flat domain admission %s\n",name);++failures;}};
    auto proof=[&](const char* v,const char* p,uint64_t vh,uint64_t ph) {
        std::vector<uint8_t> vs,ps;
        check(flat_shader_classifier_tests::loadFixture(v,vs),"required original VS fixture exists");
        if(p)check(flat_shader_classifier_tests::loadFixture(p,ps),"required original PS fixture exists");
        return flatDomainShaderProof(vh,ph,vs.data(),vs.size(),ps.data(),ps.size());
    };
    const auto foreign=proof("vs_AACFDCF2FB9AD809","ps_CAD1F585EDDC5641",0xAACFDCF2FB9AD809ull,0xCAD1F585EDDC5641ull);
    check(!engine_velocity_family::supportedPair(0xAACFDCF2FB9AD809ull,0xCAD1F585EDDC5641ull),"recorded legacy world-pair gate still refuses CAD1");
    check(foreign.pool && foreign.foreignPs && foreign.projection,"actual CAD1 original bytecode proves pool/PS/projection");
    check(flatDomainPlan(foreign,true,23,true,false,false).kind==FlatDomainPlanKind::ForeignPool,"recorded pre-world foreign admitted structurally");
    check(flatDomainPlan(foreign,true,23,true,true,true).kind==FlatDomainPlanKind::WorldPool,"same-camera world draw preserves world ownership");
    const auto nullA=proof("vs_84F6596FAF22CCFA",nullptr,0x84F6596FAF22CCFAull,0);
    const auto nullB=proof("vs_ACE405F428C17EF6",nullptr,0xACE405F428C17EF6ull,0);
    check(!nullA.pool && !nullB.pool && nullA.projection && nullB.projection,"actual nonpool null prepasses have exact clip recipes");
    FlatDomainPhasePublication receipt;
    check(!receipt.matches(nullA,1,3),"unchecked phase publication cannot serve a draw");
    receipt.checked(nullA,1,3);
    check(receipt.matches(nullB,1,3),"different proven shaders share the same actual canonical B1 publication");
    check(!receipt.matches(nullB,2,3) && !receipt.matches(nullB,1,4),"B1 rebinding and mapped publication invalidate phase receipt");
    auto otherRecipe=nullB;otherRecipe.projectionSlot=0;otherRecipe.projectionLayout=FlatProjectionPatchLayout::ForwardDp4;
    check(!receipt.matches(otherRecipe,1,3),"different actual projection binding/layout cannot inherit B1 phase receipt");
    check(flatDomainPlan(nullA,true,23,true,false,false).kind==FlatDomainPlanKind::PendingWorldNull &&
          flatDomainPlan(nullB,true,23,true,false,false).kind==FlatDomainPlanKind::PendingWorldNull,"recorded null prepasses provisional before world naming");
    const auto cfca=proof("vs_CFCA8FFC6B058630","ps_8A08FF781272C5F6",0xCFCA8FFC6B058630ull,0x8A08FF781272C5F6ull);
    check(cfca.pool && cfca.projection && cfca.projectionSlot==0 && cfca.projectionRow==4 &&
          cfca.projectionLayout==FlatProjectionPatchLayout::ForwardDp4,"actual CFCA requires its original CB0 clip recipe");
    const auto hdrDiscard=proof("vs_EC8132B90115EB9F","ps_B7650788F5DB6714",0xEC8132B90115EB9Full,0xB7650788F5DB6714ull);
    const auto vertexRecipe=proof("vs_72BDD292154158AD","ps_F1670378EE92F1E1",0x72BDD292154158ADull,0xF1670378EE92F1E1ull);
    check(vertexRecipe.projection && vertexRecipe.projectionSlot==1 && vertexRecipe.projectionRow==270 &&
          vertexRecipe.projectionLayout==FlatProjectionPatchLayout::ForwardColumns,
          "audited actual vertex clip recipe is independent of its original consumer PS");
    const auto legacyRecipe=flatProjectionDrawRecipes(0x72BDD292154158ADull,0xF1670378EE92F1E1ull);
    check(legacyRecipe.count==1 && legacyRecipe.requests[0].stage==FlatProjectionStage::Vertex &&
          legacyRecipe.requests[0].slot==1 && legacyRecipe.requests[0].patches[0].byteOffset==270*16 &&
          legacyRecipe.requests[0].patches[0].layout==FlatProjectionPatchLayout::ForwardColumns,
          "legacy and full upstream qualification receive the exact VS-only recipe with no invented PS inverse patch");
    const auto hdrOpaque=proof("vs_98397963AAEC45D3","ps_3507358D373C79C3",0x98397963AAEC45D3ull,0x3507358D373C79C3ull);
    check(hdrDiscard.pool && hdrDiscard.worldPs && hdrDiscard.foreignPs && hdrOpaque.worldPs,"actual HDR material discard retains original primitive and writer provenance");
    check(flatDomainPlan(hdrDiscard,true,26,true,true,true).kind==FlatDomainPlanKind::WorldPool &&
          flatDomainPlan(hdrOpaque,true,26,true,true,true).kind==FlatDomainPlanKind::WorldPool,"same-world original HDR mesh can update final owner");
    check(!flatDomainPlan(hdrOpaque,true,26,true,true,false).admitted() &&
          !flatDomainPlan(hdrOpaque,true,26,true,false,false).admitted(),"foreign/unassociated HDR cannot replace world owner");
    D3D11_BLEND_DESC blend{};for(auto& rt:blend.RenderTarget)rt.RenderTargetWriteMask=15;
    D3D11_DEPTH_STENCIL_DESC depthState{};depthState.DepthEnable=TRUE;depthState.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;depthState.DepthFunc=D3D11_COMPARISON_EQUAL;
    check(!flatDomainRasterRefusal(hdrDiscard,blend,depthState,1,false,true,true),"opaque EQUAL HDR mesh updates owner without changing raw depth");
    blend.RenderTarget[0].BlendEnable=TRUE;
    check(flatDomainRasterRefusal(hdrDiscard,blend,depthState,1,false,true,true)!=nullptr,"additive HDR cannot erase underlying camera owner");
    blend.RenderTarget[0].BlendEnable=FALSE;blend.RenderTarget[0].RenderTargetWriteMask=3;
    check(flatDomainRasterRefusal(hdrDiscard,blend,depthState,1,false,true,true)!=nullptr,"partial RGB HDR cannot erase owner");
    blend.RenderTarget[0].RenderTargetWriteMask=15;depthState.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
    check(flatDomainRasterRefusal(hdrDiscard,blend,depthState,1,false,true,true)!=nullptr,"unproven readonly HDR depth refuses");
    depthState.DepthFunc=D3D11_COMPARISON_EQUAL;depthState.StencilEnable=TRUE;depthState.FrontFace.StencilFunc=D3D11_COMPARISON_EQUAL;depthState.BackFace.StencilFunc=D3D11_COMPARISON_ALWAYS;
    check(!flatDomainRasterRefusal(hdrDiscard,blend,depthState,1,false,true,true),"world HDR conditional stencil selects original color and owner together");
    check(!flatDomainRasterRefusal(foreign,blend,depthState,1,true,false,false),"provenance-stamped foreign motion inherits actual conditional stencil coverage");
    auto unstamped=foreign;unstamped.foreignPs=false;
    check(flatDomainRasterRefusal(unstamped,blend,depthState,1,true,false,false)!=nullptr,"unstamped foreign replay cannot infer conditional stencil coverage");
    depthState.StencilEnable=FALSE;
    check(!flatDomainRasterRefusal(nullA,blend,depthState,15,false,false,false) &&
          flatDomainRasterRefusal(nullA,blend,depthState,15,false,false,true)!=nullptr,"null prepass legal before material but never changes later color ownership");
    float world[6][4]{};world[0][0]=world[1][1]=1;world[2][3]=-1;world[3][2]=.025f;
    int depth=0,otherDepth=0;FlatDomainPendingNull pending;
    check(pending.add(46474,&depth,3840,2160,world,0,0) && pending.add(46474,&depth,3840,2160,world,0,0) && pending.count()==1,"same-camera null witnesses deduplicate");
    check(pending.matches(46474,&depth,3840,2160,world,0,0),"holstered sequence null→world→H validates without foreign draws");
    check(flatDomainPlan(nullA,true,23,true,true,true).kind==FlatDomainPlanKind::World,"later same-camera null remains world");
    check(!pending.matches(46474,&otherDepth,3840,2160,world,0,0) && !pending.matches(46474,&depth,2880,1620,world,0,0) &&
          !pending.matches(46474,&depth,3840,2160,world,.25f,0),"pending null depth/extent/phase mismatch refuses H");
    float foreignCamera[6][4];std::memcpy(foreignCamera,world,sizeof(world));foreignCamera[3][2]=.0675f;
    check(!pending.matches(46474,&depth,3840,2160,foreignCamera,0,0),"foreign near cannot be mislabeled world by null prepass");
    check(!pending.matches(46475,&depth,3840,2160,world,0,0),"pending witness cannot cross frames");
    pending.beginFrame(46475);check(pending.count()==0 && pending.matches(46475,&depth,3840,2160,world,0,0),"next frame holstered empty pending set starts clean");
    for(unsigned i=0;i<4;++i){world[4][0]=float(i);check(pending.add(46475,&depth,3840,2160,world,0,0),"bounded distinct pending witness");}
    world[4][0]=9;check(!pending.add(46475,&depth,3840,2160,world,0,0),"pending camera overflow refuses rather than dropping proof");
    FlatDomainShaderProof unknown;check(!flatDomainPlan(unknown,true,23,true,false,false).admitted(),"missing/unsupported original bytecode refuses");
    auto noPool=foreign;noPool.pool=false;check(!flatDomainPlan(noPool,true,23,true,false,false).admitted(),"unknown foreign nonpool cannot receive world marker");
    auto discard=foreign;discard.foreignPs=false;discard.refusal="foreground-foreign-discard-provenance";
    check(!flatDomainPlan(discard,true,23,true,false,false).admitted(),"discard foreign cannot fabricate history ownership");
    check(!flatDomainPlan(foreign,true,23,false,false,false).admitted() &&
          !flatDomainPlan(foreign,true,26,true,false,false).admitted(),"missing camera or unknown HDR writer refuses");
    std::vector<uint8_t> vs,ps;flat_shader_classifier_tests::loadFixture("vs_AACFDCF2FB9AD809",vs);flat_shader_classifier_tests::loadFixture("ps_CAD1F585EDDC5641",ps);
    check(!flatDomainShaderProof(1,0xCAD1F585EDDC5641ull,vs.data(),vs.size(),ps.data(),ps.size()).present,"forged exact recipe hash refuses");
    if(!failures)std::puts("flat domain admission: recorded foreign/null/holstered and mismatch cases PASS");return failures;
}
