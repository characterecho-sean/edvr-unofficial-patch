#pragma once
#include "../../src/d3d11/flat_domain_admission.h"
#include "flat_shader_classifier_tests.h"
#include <d3dcompiler.h>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

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
    const auto inert=proof("vs_FC1193AFFC596F74","ps_258B95AC99520C1F",0xFC1193AFFC596F74ull,0x258B95AC99520C1Full);
    check(inert.present && !inert.projection && inert.inertNoSideEffects &&
          !flatDomainPlan(inert,true,23,true,true,false).admitted(),
          "captured passthrough/constant pair is pure but does not fabricate a projection");
    std::vector<uint8_t> inertVs,inertPs;
    flat_shader_classifier_tests::loadFixture("vs_FC1193AFFC596F74",inertVs);
    flat_shader_classifier_tests::loadFixture("ps_258B95AC99520C1F",inertPs);
    const auto mutatedProgram=[&](const std::vector<uint8_t>& original,uint32_t stage,
                                  uint32_t from,uint32_t to) {
        auto chunks=dxbc_engine_velocity_detail::parseContainer(original.data(),original.size(),stage);
        bool changed=false;
        for(auto& chunk:chunks)if(dxbc_engine_velocity_detail::isProgram(chunk.tag)) {
            auto words=dxbc_engine_velocity_detail::programWords(chunk.bytes);
            for(size_t at=2;at<words.size();at+=dxbc_engine_velocity_detail::instructionLength(words,at))
                if((words[at]&0x7ffu)==from) {words[at]=(words[at]&~0x7ffu)|to;changed=true;break;}
            chunk.bytes=dxbc_engine_velocity_detail::programBytes(std::move(words));
        }
        check(changed,"mutation found target instruction");
        return dxbc_engine_velocity_detail::makeContainer(chunks);
    };
    const auto store=mutatedProgram(inertPs,dxbc_engine_velocity_detail::kPs50,
        flat_shader_classifier_detail::kOpMov,164);
    const auto rawStore=mutatedProgram(inertPs,dxbc_engine_velocity_detail::kPs50,
        flat_shader_classifier_detail::kOpMov,166);
    const auto atomic=mutatedProgram(inertPs,dxbc_engine_velocity_detail::kPs50,
        flat_shader_classifier_detail::kOpMov,170);
    const auto unknownInstruction=mutatedProgram(inertPs,dxbc_engine_velocity_detail::kPs50,
        flat_shader_classifier_detail::kOpMov,511);
    const auto uavDeclaration=mutatedProgram(inertPs,dxbc_engine_velocity_detail::kPs50,
        dxbc_engine_velocity_detail::kOpDclOutput,158);
    check(!flatDomainNoSideEffectProgram(store.data(),store.size(),dxbc_engine_velocity_detail::kPs50) &&
          !flatDomainNoSideEffectProgram(rawStore.data(),rawStore.size(),dxbc_engine_velocity_detail::kPs50) &&
          !flatDomainNoSideEffectProgram(atomic.data(),atomic.size(),dxbc_engine_velocity_detail::kPs50) &&
          !flatDomainNoSideEffectProgram(unknownInstruction.data(),unknownInstruction.size(),dxbc_engine_velocity_detail::kPs50) &&
          !flatDomainNoSideEffectProgram(uavDeclaration.data(),uavDeclaration.size(),dxbc_engine_velocity_detail::kPs50) &&
          !flatDomainNoSideEffectProgram(inertPs.data(),inertPs.size()-1,dxbc_engine_velocity_detail::kPs50),
          "typed/raw UAV stores, atomic, unknown opcode/declaration and malformed bytecode cannot earn inert proof");
    // Compile a new no-CB arithmetic VS during the test: admission is based
    // on effects and original state, not on either captured shader hash.
    FlatDomainShaderProof arithmetic{};
    HMODULE compiler=LoadLibraryW(L"d3dcompiler_47.dll");
    check(compiler!=nullptr,"test compiler available for generated arithmetic VS");
    if(compiler) {
        auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile"));
        check(compile!=nullptr,"test compiler exposes D3DCompile");
        if(compile) {
            constexpr char source[]=
                "float4 main(float4 p:POSITION):SV_Position{return p*float4(.5,.5,1,1)+float4(.25,0,0,0);}";
            ID3DBlob *blob=nullptr,*errors=nullptr;
            const HRESULT hr=compile(source,sizeof(source)-1,"generated-inert-arithmetic",nullptr,nullptr,
                "main","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
            check(SUCCEEDED(hr) && blob,"generated arithmetic VS compiles");
            if(SUCCEEDED(hr) && blob) {
                const auto* bytes=blob->GetBufferPointer();const size_t size=blob->GetBufferSize();
                arithmetic=flatDomainShaderProof(flatDomainBytecodeHash(bytes,size),0x258B95AC99520C1Full,
                    bytes,size,inertPs.data(),inertPs.size());
                check(arithmetic.inertNoSideEffects && !arithmetic.projection,
                      "generated arithmetic position transform earns structural no-side-effect proof");
            }
            if(errors)errors->Release();if(blob)blob->Release();
            constexpr char readOnlySource[]=
                "Texture2D<float4> t:register(t0);float4 main():SV_Target{return t.Load(int3(0,0,0));}";
            blob=nullptr;errors=nullptr;
            const HRESULT readOnlyHr=compile(readOnlySource,sizeof(readOnlySource)-1,"generated-read-only-PS",
                nullptr,nullptr,"main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
            check(SUCCEEDED(readOnlyHr) && blob,"generated read-only texture PS compiles");
            if(SUCCEEDED(readOnlyHr) && blob) {
                const auto* bytes=blob->GetBufferPointer();const size_t size=blob->GetBufferSize();
                const auto readOnly=flatDomainShaderProof(0xFC1193AFFC596F74ull,
                    flatDomainBytecodeHash(bytes,size),inertVs.data(),inertVs.size(),bytes,size);
                check(readOnly.inertNoSideEffects,"generated texture load remains a read-only inert PS");
            }
            if(errors)errors->Release();if(blob)blob->Release();
        }
        FreeLibrary(compiler);
    }
    FlatDomainInertBindings inertBindings{};
    D3D11_BLEND_DESC noColor{};
    D3D11_DEPTH_STENCIL_DESC stencilOnly{};
    stencilOnly.StencilEnable=TRUE;stencilOnly.StencilWriteMask=0xff;
    inertBindings.blend=&noColor;inertBindings.depth=&stencilOnly;inertBindings.boundTargets=1;
    inertBindings.actualShaderPair=inertBindings.originalDsv=inertBindings.noOtherStages=true;
    inertBindings.noUavs=inertBindings.noStreamOutput=inertBindings.noPredicate=true;
    check(flatDomainInertNoWrite(inert,inertBindings),
          "zero color/depth writes pass while the original stencil write stays in the draw");
    check(flatDomainInertNoWrite(arithmetic,inertBindings),
          "generated arithmetic shader uses the same effect-based admission");
    auto changedBindings=inertBindings;
    changedBindings.blend=nullptr;
    check(!flatDomainInertNoWrite(inert,changedBindings),"null blend defaults to full color writes");
    changedBindings=inertBindings;changedBindings.depth=nullptr;
    check(!flatDomainInertNoWrite(inert,changedBindings),"null depth state defaults to depth writes");
    changedBindings.readOnlyDepth=true;
    check(flatDomainInertNoWrite(inert,changedBindings),"read-only DSV makes default depth state harmless");
    changedBindings=inertBindings;D3D11_DEPTH_STENCIL_DESC depthWriter=stencilOnly;
    depthWriter.DepthEnable=TRUE;depthWriter.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    changedBindings.depth=&depthWriter;
    check(!flatDomainInertNoWrite(inert,changedBindings) &&
          std::strcmp(flatDomainInertRefusal(inert,changedBindings),"foreground-inert-depth-write")==0,
          "effective depth writer is never inert and names its first failure");
    changedBindings=inertBindings;changedBindings.boundTargets=1u<<7;
    noColor.IndependentBlendEnable=TRUE;noColor.RenderTarget[7].RenderTargetWriteMask=15;
    check(!flatDomainInertNoWrite(inert,changedBindings),"independent MRT7 color writer is checked");
    noColor.IndependentBlendEnable=FALSE;
    check(flatDomainInertNoWrite(inert,changedBindings),"shared RTV0 mask governs all MRTs when independent blending is off");
    noColor.RenderTarget[0].RenderTargetWriteMask=15;
    check(!flatDomainInertNoWrite(inert,inertBindings) &&
          std::strcmp(flatDomainInertRefusal(inert,inertBindings),"foreground-inert-color-write")==0,
          "ordinary color writer is never inert and names its first failure");
    check(!flatDomainInertNoWrite(arithmetic,inertBindings),
          "generated arithmetic shader with live color writes is refused");
    noColor.RenderTarget[0].RenderTargetWriteMask=0;
    changedBindings=inertBindings;changedBindings.actualShaderPair=false;
    check(!flatDomainInertNoWrite(inert,changedBindings),"stale actual shader pair refuses");
    changedBindings=inertBindings;changedBindings.noUavs=false;
    check(!flatDomainInertNoWrite(inert,changedBindings),"any bound OM UAV refuses");
    changedBindings=inertBindings;changedBindings.noOtherStages=false;
    check(!flatDomainInertNoWrite(inert,changedBindings),"another shader stage refuses");
    changedBindings=inertBindings;changedBindings.noStreamOutput=false;
    check(!flatDomainInertNoWrite(inert,changedBindings),"stream output refuses");
    changedBindings=inertBindings;changedBindings.originalDsv=false;
    check(!flatDomainInertNoWrite(inert,changedBindings),"stale DSV refuses");
    auto unprovenInert=inert;unprovenInert.inertNoSideEffects=false;
    check(!flatDomainInertNoWrite(unprovenInert,inertBindings),"unproven side effects refuse");
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
    // Section 104: a first-person forward mesh writes the HDR light target; it is captured with its own motion.
    check(flatDomainPlan(hdrDiscard,true,26,true,true,false).kind==FlatDomainPlanKind::ForeignPool &&
          flatDomainPlan(hdrDiscard,true,26,true,false,false).kind==FlatDomainPlanKind::ForeignPool,"first-person HDR mesh is captured as foreign");
    check(!flatDomainPlan(hdrDiscard,true,60,true,false,false).admitted(),"other colour formats still refuse");
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
    {
        // The mismatch line (section 104: aiming down sights refused H at the pending-null check). It names the witness's kind and
        // draw, what differs and the first differing float, says nothing when every witness matches, and explains a refusal
        // matches() makes for witnesses kept from an earlier frame.
        FlatDomainPendingNull said;char text[768];
        check(said.add(1,&depth,3840,2160,world,0,0,0xAAAAull,0xBBBBull,1u) &&
              !said.describeMismatch(1,&depth,3840,2160,world,0,0,text,sizeof(text)) && text[0]==0,
              "pending-null mismatch line: a matching witness says nothing");
        float zoomed[6][4];std::memcpy(zoomed,world,sizeof(world));zoomed[0][0]*=2;zoomed[1][1]*=2;
        check(said.describeMismatch(1,&depth,3840,2160,zoomed,0,0,text,sizeof(text)) && std::strstr(text,"kind=predicted-world-near") &&
              std::strstr(text,"VS=000000000000AAAA") && std::strstr(text,"differs: camera") && std::strstr(text,"row=270 col=0"),
              "pending-null mismatch line: names the kind, the draw and the first differing float (row 270, the x scale)");
        FlatDomainPendingNull prepass;
        check(prepass.add(2,&depth,3840,2160,foreignCamera,0,0,1,2,2u) &&
              prepass.describeMismatch(2,&depth,3840,2160,world,0,0,text,sizeof(text)) && std::strstr(text,"kind=null-prepass") &&
              std::strstr(text,"row=273 col=2"),"pending-null mismatch line: a null prepass at another near names row 273 (the near)");
        check(prepass.describeMismatch(3,&depth,3840,2160,world,0,0,text,sizeof(text)) && std::strstr(text,"left from frame 2"),
              "pending-null mismatch line: witnesses kept from an earlier frame are said, as matches() refuses them");
        check(said.describeMismatch(1,&otherDepth,3840,2160,world,.25f,0,text,sizeof(text)) && std::strstr(text,"depth") &&
              std::strstr(text,"phase") && !std::strstr(text,"camera;"),"pending-null mismatch line: depth and phase are named");
        // The projection scale ratio (witness over selected, per axis): 1 for the world's own draw, the weapon camera's factor for a draw
        // that shares the world's near plane. The synthetic rows here are a unit camera (scale 1 on both axes) and the same doubled on both.
        check(said.describeMismatch(1,&depth,3840,2160,zoomed,0,0,text,sizeof(text)) &&
              std::strstr(text,"projection scale ratio witness/selected=(0.5,0.5)") &&
              std::strstr(text,"first differing float"),
              "pending-null mismatch line: names the projection scale ratio of the witness over the selected camera, ahead of the first differing float");
        float noScale[6][4]{};noScale[3][2]=.025f;
        FlatDomainPendingNull degenerate;
        check(degenerate.add(1,&depth,3840,2160,noScale,0,0) &&
              degenerate.describeMismatch(1,&depth,3840,2160,world,0,0,text,sizeof(text)) &&
              std::strstr(text,"projection scale ratio witness/selected=n/a"),
              "pending-null mismatch line: rows with no projection scale say n/a rather than a number");
    }
    pending.beginFrame(46475);check(pending.count()==0 && pending.matches(46475,&depth,3840,2160,world,0,0),"next frame holstered empty pending set starts clean");
    for(unsigned i=0;i<4;++i){world[4][0]=float(i);check(pending.add(46475,&depth,3840,2160,world,0,0),"bounded distinct pending witness");}
    world[4][0]=9;check(!pending.add(46475,&depth,3840,2160,world,0,0),"pending camera overflow refuses rather than dropping proof");
    FlatDomainShaderProof unknown;check(!flatDomainPlan(unknown,true,23,true,false,false).admitted(),"missing/unsupported original bytecode refuses");
    auto noPool=foreign;noPool.pool=false;check(!flatDomainPlan(noPool,true,23,true,false,false).admitted(),"unknown foreign nonpool cannot receive world marker");
    auto discard=foreign;discard.foreignPs=false;discard.refusal="foreground-foreign-discard-provenance";
    check(!flatDomainPlan(discard,true,23,true,false,false).admitted(),"discard foreign cannot fabricate history ownership");
    check(!flatDomainPlan(foreign,true,23,false,false,false).admitted() &&
          flatDomainPlan(foreign,true,26,true,false,false).kind==FlatDomainPlanKind::ForeignPool,
          "missing camera refuses; a first-person HDR writer is captured");
    std::vector<uint8_t> vs,ps;flat_shader_classifier_tests::loadFixture("vs_AACFDCF2FB9AD809",vs);flat_shader_classifier_tests::loadFixture("ps_CAD1F585EDDC5641",ps);
    check(!flatDomainShaderProof(1,0xCAD1F585EDDC5641ull,vs.data(),vs.size(),ps.data(),ps.size()).present,"forged exact recipe hash refuses");
    {
        // The settlement rules (2026-10-06). A non-HDR draw that writes depth defines the surface its fragments
        // show, so its owner mark is exact however its color blends or masks; one that cannot write depth never
        // changes the surface and is forwarded unchanged; HDR color is final and stays strict.
        const auto reason=[](const char* got,const char* want){return got && std::strcmp(got,want)==0;};
        D3D11_BLEND_DESC minBlend{};minBlend.IndependentBlendEnable=TRUE;
        for(auto& rt:minBlend.RenderTarget)rt.RenderTargetWriteMask=15;
        {
            auto& rt=minBlend.RenderTarget[0];rt.BlendEnable=TRUE;
            rt.SrcBlend=rt.DestBlend=rt.SrcBlendAlpha=rt.DestBlendAlpha=D3D11_BLEND_ONE;
            rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_MIN;
        }
        D3D11_DEPTH_STENCIL_DESC writesDepth{};writesDepth.DepthEnable=TRUE;
        writesDepth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;writesDepth.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
        auto keepsDepth=writesDepth;keepsDepth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        auto noDepthTest=writesDepth;noDepthTest.DepthEnable=FALSE;
        check(cfca.colorComponents[0]==15 && !cfca.colorComponents[1] && !cfca.colorComponents[2] && !cfca.colorComponents[3],
              "actual CFCA pixel shader writes exactly RT0 xyzw, so the MIN blend below is a real mixed writer");
        check(cfca.worldPs && cfca.foreignPs &&
              flatDomainPlan(cfca,true,23,true,false,false).kind==FlatDomainPlanKind::ForeignPool &&
              flatDomainPlan(cfca,true,23,true,true,true).kind==FlatDomainPlanKind::WorldPool,
              "CFCA is a foreign pool writer in the first-person camera and a world pool writer as the predicted world camera");
        bool cfcaAdmitted=true;
        for(const bool foreignDraw:{false,true})for(const bool colorAlready:{false,true})
            cfcaAdmitted=cfcaAdmitted && !flatDomainRasterRefusal(cfca,minBlend,writesDepth,0x0F,foreignDraw,false,colorAlready);
        check(cfcaAdmitted,"CFCA RT0 MIN blend with depth write ALL is admitted foreign or world, before or after color");
        check(reason(flatDomainRasterRefusal(cfca,minBlend,keepsDepth,0x0F,false,false,false),"foreground-mixed-component-writer") &&
              reason(flatDomainRasterRefusal(cfca,minBlend,keepsDepth,0x0F,false,false,true),"foreground-mixed-component-writer"),
              "the same MIN blend without a depth write is still a mixed-component writer");
        auto keepsDepthEqual=keepsDepth;keepsDepthEqual.DepthFunc=D3D11_COMPARISON_EQUAL;
        check(reason(flatDomainRasterRefusal(cfca,minBlend,keepsDepthEqual,0x0F,true,false,false),"foreground-mixed-component-writer"),
              "a foreign EQUAL depth-test MIN blend without a depth write still names the mixed-component writer");
        check(reason(flatDomainRasterRefusal(cfca,minBlend,keepsDepth,0x0F,true,false,false),"foreground-unproven-readonly-depth"),
              "a foreign non-EQUAL read-only depth draw keeps its own refusal ahead of the blend check");
        check(flatDomainPreservesSurface(keepsDepth) && flatDomainPreservesSurface(keepsDepthEqual),
              "the blended draw that cannot write depth preserves the surface and is forwarded");
        check(flatDomainPreservesSurface(noDepthTest) && !flatDomainPreservesSurface(writesDepth),
              "no depth test preserves the surface; a depth write never does");
        D3D11_DEPTH_STENCIL_DESC disabledWriter{};disabledWriter.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        D3D11_DEPTH_STENCIL_DESC zeroedWriter{};zeroedWriter.DepthEnable=TRUE;zeroedWriter.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        D3D11_DEPTH_STENCIL_DESC enabledWriter{};enabledWriter.DepthEnable=TRUE;enabledWriter.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        check(flatDomainPreservesSurface(disabledWriter) && flatDomainPreservesSurface(zeroedWriter) &&
              !flatDomainPreservesSurface(enabledWriter),
              "preserve truth table: DepthEnable FALSE and enable+ZERO preserve; enable+ALL does not");
        // HDR color is the final picture: a blended or partly masked HDR writer cannot stand in for the owner,
        // whatever it does to depth.
        D3D11_BLEND_DESC hdrBlend{};for(auto& rt:hdrBlend.RenderTarget)rt.RenderTargetWriteMask=15;
        auto hdrAlways=writesDepth;hdrAlways.DepthFunc=D3D11_COMPARISON_ALWAYS;
        check(!flatDomainRasterRefusal(hdrDiscard,hdrBlend,hdrAlways,1,false,true,true),
              "positive control: an opaque full-mask HDR writer that writes depth is still admitted");
        // Section 104: a depth writer owns its surface whatever its colour does, HDR included.
        hdrBlend.RenderTarget[0].BlendEnable=TRUE;
        check(!flatDomainRasterRefusal(hdrDiscard,hdrBlend,hdrAlways,1,false,true,true) &&
              !flatDomainRasterRefusal(hdrDiscard,hdrBlend,hdrAlways,1,false,true,false),
              "a blended HDR writer with a depth write owns its surface");
        hdrBlend.RenderTarget[0].BlendEnable=FALSE;hdrBlend.RenderTarget[0].RenderTargetWriteMask=3;
        check(!flatDomainRasterRefusal(hdrDiscard,hdrBlend,hdrAlways,1,false,true,true),
              "a partly masked HDR writer with a depth write owns its surface");
        // A depth-only draw after color: legal and exact when it writes depth (non-HDR), refused when it cannot.
        D3D11_BLEND_DESC allColor{};for(auto& rt:allColor.RenderTarget)rt.RenderTargetWriteMask=15;
        auto nullWritesDepth=writesDepth;nullWritesDepth.DepthFunc=D3D11_COMPARISON_LESS;
        auto nullKeepsDepth=nullWritesDepth;nullKeepsDepth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        check(!flatDomainRasterRefusal(nullA,allColor,nullWritesDepth,15,false,false,true) &&
              !flatDomainRasterRefusal(nullA,allColor,nullWritesDepth,15,false,false,false),
              "a null-PS depth writer after color is admitted when it writes depth");
        check(reason(flatDomainRasterRefusal(nullA,allColor,nullKeepsDepth,15,false,false,true),"foreground-depth-only-after-color") &&
              !flatDomainRasterRefusal(nullA,allColor,nullKeepsDepth,15,false,false,false),
              "the same null-PS draw without a depth write still refuses after color and not before it");
        check(reason(flatDomainRasterRefusal(nullA,allColor,[&]{auto d=nullWritesDepth;d.DepthEnable=FALSE;return d;}(),15,false,false,true),
                     "foreground-depth-only-after-color"),
              "a null-PS draw with no depth test is not a surface writer and still refuses after color");
        // The settlement pair itself: the world camera's depth-only pool prepass (color masked off, PS without outputs).
        const auto prepass=proof("vs_F516BF0201303B87","ps_B40B0462256E31C2",0xF516BF0201303B87ull,0xB40B0462256E31C2ull);
        check(prepass.present && prepass.pool && prepass.projection && prepass.worldPs && prepass.foreignPs &&
              prepass.projectionSlot==1 && prepass.projectionRow==270 &&
              prepass.projectionLayout==FlatProjectionPatchLayout::ForwardColumns &&
              !prepass.colorComponents[0],
              "actual F516BF02/B40B0462 prepass pair has the canonical B1 recipe, both PS proofs and no color output");
        check(flatDomainPlan(prepass,true,23,true,false,false).kind==FlatDomainPlanKind::ForeignPool,
              "before naming and without the predicted-world witness the prepass pair is a foreign pool writer");
        check(flatDomainPlan(prepass,true,23,true,true,true).kind==FlatDomainPlanKind::WorldPool,
              "planned as the predicted world camera it is a world pool writer and is never captured");
        D3D11_BLEND_DESC maskedOff{};
        check(!flatDomainWritesColor(prepass,maskedOff,1),"the masked-off prepass writes no color");
        check(!flatDomainRasterRefusal(prepass,maskedOff,writesDepth,1,false,false,true) &&
              !flatDomainRasterRefusal(prepass,maskedOff,writesDepth,1,false,false,false),
              "the masked-off depth-writing prepass is admitted after color and before it");
        check(reason(flatDomainRasterRefusal(prepass,maskedOff,keepsDepth,1,false,false,true),"foreground-depth-only-after-color"),
              "a masked-off prepass that cannot write depth keeps the depth-only-after-color refusal");
    }
    if(!failures)std::puts("flat domain admission: recorded foreign/null/holstered and mismatch cases PASS");return failures;
}

// ---- the world camera predicted before it is named: near plane AND projection scale (design section 104, aiming down sights) --------------
// Flight 13:18: aiming down sights the weapon camera takes the world's near plane (0.025, the world's pose and phase) and keeps its own
// projection, 1.23 to 1.66 times the world's. Its depth prepass was "predicted world" by the near alone, left a witness no world camera could
// match, and every frame refused H (foreground-pending-null-not-selected-world). flatCameraProjectionScale reads the scale out of the rows
// free of the rotation and the phase; flatDomainPredictsWorld is the one predicate, and the runtime asks it at both sites that used to
// repeat the near-only test.
namespace domain_prediction_test {
// Rows as the game composes them: rows 0..2 hold the clip x column (component 0), the clip y column (1), nothing in 2 and the view direction
// (3); row 3 the depth terms (near in component 2); row 4 the view direction and row 5 the position.
// x column = s0*right + nx*f, y column = s1*up + ny*f (flatCameraMeasureRowShift's decomposition).
inline void composeRows(float (&out)[6][4], double yaw, double pitch, double roll, double s0, double s1, double nx, double ny, float nearPlane,
                        double fScale = 1.0) {
    const auto mul = [](const double (&a)[3][3], const double (&b)[3][3], double (&c)[3][3]) {
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { c[i][j] = 0; for (int k = 0; k < 3; ++k) c[i][j] += a[i][k] * b[k][j]; }
    };
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll), sr = std::sin(roll);
    const double ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}}, rx[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}},
                 rz[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
    double t1[3][3], r[3][3];
    mul(rz, rx, t1); mul(t1, ry, r);   // r's columns: right, up, forward
    for (int i = 0; i < 3; ++i) {
        const double right = r[i][0], up = r[i][1], fwd = r[i][2], f = fScale * fwd;
        out[i][0] = static_cast<float>(s0 * right + nx * f);
        out[i][1] = static_cast<float>(s1 * up + ny * f);
        out[i][2] = 0.0f;
        out[i][3] = static_cast<float>(f);
        out[4][i] = static_cast<float>(fwd);
        out[5][i] = -21.0f + float(i);
    }
    out[3][0] = out[3][1] = out[3][3] = 0.0f; out[3][2] = nearPlane; out[4][3] = out[5][3] = 0.0f;
}
// The rows of two cameras of one object, from a real flight (tools\edvr_log.py's self-test, flight 1): the scene camera and the first-person
// weapon camera. Rows 270..273 as the log prints them; rows 274 and 275 are zero here (the scale reads rows 0..2 only).
inline void loggedRows(float (&out)[6][4], bool weapon) {
    static const float scene[16] = {-1.04384f, -0.007344862f, 0, 0.113728f, -2.103792e-08f, 1.866733f, 0, 0.03455516f,
                                    -0.1195614f, 0.06412452f, 0, -0.9929108f, 0, 0, 0.025f, 0};
    static const float fp[16] = {-1.285258f, -0.009043574f, 0, 0.113728f, -2.590355e-08f, 2.298469f, 0, 0.03455516f,
                                 -0.1472135f, 0.07895518f, 0, -0.9929108f, 0, 0, 0.0675f, 0};
    std::memset(out, 0, sizeof(out));
    std::memcpy(out, weapon ? fp : scene, sizeof(scene));
}
inline void scaled(float (&rows)[6][4], double x, double y) {   // the same camera at another projection scale (columns 0 and 1 of rows 0..2)
    for (int i = 0; i < 3; ++i) { rows[i][0] = static_cast<float>(rows[i][0] * x); rows[i][1] = static_cast<float>(rows[i][1] * y); }
}
}  // namespace domain_prediction_test

inline int flatDomainWorldPredictionTests() {
    using namespace edvr;
    using namespace domain_prediction_test;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) { if (!ok) { std::printf("FAIL: flat domain world prediction %s\n", name); ++failures; } };
    const auto near1 = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };

    // ---- the scale, free of the rotation and the phase -------------------------------------------------------------------------
    {
        bool all = true;
        const double angles[][3] = {{0, 0, 0}, {0.4, 0, 0}, {-1.1, 0.3, 0}, {2.7, -0.5, 0.2}, {0.1, 1.2, -0.7}, {3.0, 0.7, 1.4}};
        const double phases[][2] = {{0, 0}, {0.0003, -0.0002}, {0.0125, 0.0100}, {-0.4, 0.3}};
        for (const auto& a : angles)
            for (const auto& ph : phases)
                for (const double fScale : {1.0, 2.5}) {
                    float rows[6][4];
                    composeRows(rows, a[0], a[1], a[2], 1.0507, 1.8678, ph[0], ph[1], 0.025f, fScale);
                    double p0 = 0, p1 = 0;
                    if (!flatCameraProjectionScale(rows, p0, p1) || !near1(p0, 1.0507, 2e-5) || !near1(p1, 1.8678, 4e-5)) {
                        std::printf("  rotation (%g,%g,%g) phase (%g,%g) f*%g gave (%.7g,%.7g)\n", a[0], a[1], a[2], ph[0], ph[1], fScale, p0, p1);
                        all = false;
                    }
                }
        check(all, "the projection scale of synthetic rows is the scale they were composed with, for any rotation, any phase and a view direction of any length");
        // The phase alone moves the x and y columns along f and leaves the scale: the measured shift is the phase, the scale does not move.
        float a[6][4], b[6][4];
        composeRows(a, 0.4, 0.2, 0.1, 1.0507, 1.8678, 0, 0, 0.025f);
        composeRows(b, 0.4, 0.2, 0.1, 1.0507, 1.8678, 0.0125, -0.0100, 0.025f);
        double a0, a1, b0, b1, sx = 0, sy = 0;
        check(flatCameraProjectionScale(a, a0, a1) && flatCameraProjectionScale(b, b0, b1) && near1(a0, b0, 2e-6) && near1(a1, b1, 2e-6) &&
                  flatCameraMeasureRowShift(b, sx, sy) && near1(sx, 0.0125, 2e-6) && near1(sy, -0.0100, 2e-6),
              "a jitter phase moves the measured shift and leaves the scale where it was");
    }
    {
        // The logged audit rows (flight 1): the weapon camera against the world's, 1.2313 on both axes.
        float scene[6][4], weapon[6][4];
        loggedRows(scene, false); loggedRows(weapon, true);
        double s0 = 0, s1 = 0, w0 = 0, w1 = 0;
        check(flatCameraProjectionScale(scene, s0, s1) && near1(s0, 1.0507, 2e-4) && near1(s1, 1.8678, 2e-4),
              "the logged scene rows read back as 1.0507 x 1.8678");
        check(flatCameraProjectionScale(weapon, w0, w1) && near1(w0, 1.2937, 2e-4) && near1(w1, 2.2998, 2e-4) &&
                  near1(w0 / s0, 1.2313, 5e-4) && near1(w1 / s1, 1.2313, 5e-4),
              "the logged first-person rows read back as 1.2937 x 2.2998: 1.2313 times the world's on both axes");
        float zero[6][4]{}, notFinite[6][4], noScale[6][4];
        loggedRows(notFinite, false); notFinite[0][0] = std::numeric_limits<float>::quiet_NaN();
        loggedRows(noScale, false); noScale[0][0] = noScale[1][0] = noScale[2][0] = 0;
        double p0 = 7, p1 = 7;
        check(!flatCameraProjectionScale(zero, p0, p1) && !flatCameraProjectionScale(notFinite, p0, p1) && !flatCameraProjectionScale(noScale, p0, p1),
              "rows with no view direction, a number that is not finite, or a column with no scale have no projection scale");
    }

    // ---- the predicate: near AND scale ---------------------------------------------------------------------------------------
    {
        float world[6][4];
        loggedRows(world, false);
        const FlatDomainWorldReference ref = flatDomainWorldReference(world);
        check(ref.valid() && ref.nearPlane == 0.025f && near1(ref.p0, 1.0507, 2e-4) && near1(ref.p1, 1.8678, 2e-4),
              "the reference a named world camera leaves is its near plane and its two projection scales");
        FlatDomainWorldPrediction detail;
        float otherNear[6][4]; std::memcpy(otherNear, world, sizeof(world)); otherNear[3][2] = 0.04f;
        const FlatDomainWorldReference otherRef = flatDomainWorldReference(otherNear);
        check(otherRef.valid() && otherRef.nearPlane == 0.04f && flatDomainPredictsWorld(otherNear, otherRef) && !flatDomainPredictsWorld(world, otherRef),
              "the reference carries the near plane of the camera it was made from, whatever that is");
        // The world against its own previous frame while zooming in: up to 3.8% a frame at 50 fps (the log's 1.000 to 1.038).
        float nextWorld[6][4]; std::memcpy(nextWorld, world, sizeof(world)); scaled(nextWorld, 1.038, 1.038);
        check(flatDomainPredictsWorld(world, ref, &detail) && detail.nearEqual && near1(detail.ratio0, 1.0, 1e-9) && near1(detail.ratio1, 1.0, 1e-9),
              "the world's own draw is predicted, ratio 1");
        check(flatDomainPredictsWorld(nextWorld, ref, &detail) && detail.nearEqual && near1(detail.ratio0, 1.038, 1e-5),
              "the world a frame later, zooming 3.8%, is still predicted");
        // The weapon: its hipfire near plane (0.0675) differs, so the cheap test says no and the scales are never taken ...
        float weapon[6][4]; loggedRows(weapon, true);
        check(!flatDomainPredictsWorld(weapon, ref, &detail) && !detail.nearEqual && detail.ratio0 == 0 && detail.ratio1 == 0,
              "the weapon camera at its own near plane is not the world's, and the scales are not even taken");
        // ... and aiming down sights it takes the world's near plane: only the scale tells it from the world.
        float ads[6][4]; std::memcpy(ads, weapon, sizeof(weapon)); ads[3][2] = 0.025f;
        check(!flatDomainPredictsWorld(ads, ref, &detail) && detail.nearEqual && near1(detail.ratio0, 1.2313, 5e-4) && near1(detail.ratio1, 1.2313, 5e-4),
              "the weapon camera aiming down sights, at the world's near plane, is not predicted: near equal, scale 1.2313 times the world's");
        for (const double factor : {1.32, 1.45, 1.66, 1.18}) {
            float entering[6][4]; std::memcpy(entering, world, sizeof(world)); scaled(entering, factor, factor);
            check(!flatDomainPredictsWorld(entering, ref, &detail) && detail.nearEqual && near1(detail.ratio0, factor, 1e-5),
                  "the weapon entering the sights (1.32 to 1.66 times the world's) and leaving them (at least 1.18) is not predicted");
        }
        // The tolerance: 10% of the reference on each axis.
        float inside[6][4], outside[6][4];
        std::memcpy(inside, world, sizeof(world)); scaled(inside, 1.099, 1.099);
        std::memcpy(outside, world, sizeof(world)); scaled(outside, 1.101, 1.101);
        check(flatDomainPredictsWorld(inside, ref) && !flatDomainPredictsWorld(outside, ref), "9.9% off is predicted, 10.1% is not");
        std::memcpy(inside, world, sizeof(world)); scaled(inside, 0.901, 0.901);
        std::memcpy(outside, world, sizeof(world)); scaled(outside, 0.899, 0.899);
        check(flatDomainPredictsWorld(inside, ref) && !flatDomainPredictsWorld(outside, ref), "the tolerance holds below the reference too: 9.9% under is predicted, 10.1% under is not");
        float xOnly[6][4], yOnly[6][4], oneAxisInside[6][4];
        std::memcpy(xOnly, world, sizeof(world)); scaled(xOnly, 1.25, 1.0);
        std::memcpy(yOnly, world, sizeof(world)); scaled(yOnly, 1.0, 1.25);
        std::memcpy(oneAxisInside, world, sizeof(world)); scaled(oneAxisInside, 1.05, 1.099);
        check(!flatDomainPredictsWorld(xOnly, ref) && !flatDomainPredictsWorld(yOnly, ref) && flatDomainPredictsWorld(oneAxisInside, ref),
              "both axes must be within the tolerance: one axis 25% off refuses, both inside predicts");
        // The near plane stays exact: one float away is another camera.
        float nearAway[6][4]; std::memcpy(nearAway, world, sizeof(world)); nearAway[3][2] = std::nextafter(0.025f, 1.0f);
        check(!flatDomainPredictsWorld(nearAway, ref, &detail) && !detail.nearEqual, "a near plane one float away is not the reference's");
        // The rotation and the phase do not matter: the world turned and jittered is still the world.
        float turned[6][4];
        composeRows(turned, 0.9, -0.3, 0.2, ref.p0, ref.p1, 0.0125, -0.0100, 0.025f);
        check(flatDomainPredictsWorld(turned, ref), "the world turned and carrying a jitter phase is still predicted");
        // No reference, no prediction: the draw is classified as any other (a first-person pool draw is captured).
        FlatDomainWorldReference none;
        float noScale[6][4]{}; noScale[3][2] = 0.025f;
        FlatDomainWorldReference degenerate = flatDomainWorldReference(noScale);
        FlatDomainWorldReference noNear = ref; noNear.nearPlane = 0;
        FlatDomainWorldReference noP0 = ref; noP0.p0 = 0;
        check(!none.valid() && !degenerate.valid() && !noNear.valid() && !noP0.valid() &&
                  !flatDomainPredictsWorld(world, none, &detail) && !detail.nearEqual &&
                  !flatDomainPredictsWorld(world, degenerate) && !flatDomainPredictsWorld(world, noNear) && !flatDomainPredictsWorld(world, noP0),
              "a reference that is missing or degenerate predicts nothing, whatever the draw");
        // A draw whose own rows have no scale at the reference's near plane: near equal, never predicted.
        check(!flatDomainPredictsWorld(noScale, ref, &detail) && detail.nearEqual && detail.ratio0 == 0, "a draw at the reference's near plane with no projection scale is not predicted");
    }

    // ---- an aiming-down-sights frame through the pending-null witnesses -----------------------------------------------------------
    // The frame's pre-naming draws, as the 13:18 flight saw them: the world's depth prepass (F516BF02/B40B0462, the world's own rows) and the
    // weapon's, at the world's near plane with its own projection scale. The near-only classification takes both for the world, so the
    // witness set holds a camera the selected world camera is not and H refuses; the scale keeps the weapon's out of the set.
    {
        float previousWorld[6][4], world[6][4], weaponPrepass[6][4];
        loggedRows(previousWorld, false);
        std::memcpy(world, previousWorld, sizeof(world)); scaled(world, 1.038, 1.038);   // this frame's world: the sights zooming in
        std::memcpy(weaponPrepass, world, sizeof(world)); scaled(weaponPrepass, 1.66, 1.66);   // the weapon's camera, near switched to the world's
        const FlatDomainWorldReference ref = flatDomainWorldReference(previousWorld);
        int depth = 0;
        struct Draw { const float (*rows)[4]; bool weapon; };
        const Draw draws[] = {{world, false}, {world, false}, {weaponPrepass, true}, {world, false}, {weaponPrepass, true}};
        const auto nearOnly = [&](const float (&rows)[6][4]) { return ref.valid() && rows[3][2] == ref.nearPlane; };   // the rule before the scale
        const auto withScale = [&](const float (&rows)[6][4]) { return flatDomainPredictsWorld(rows, ref); };
        for (const bool scaleRule : {false, true}) {
            FlatDomainPendingNull pending;
            unsigned predicted = 0, scaleRejected = 0;
            for (const Draw& d : draws) {
                float rows[6][4]; std::memcpy(rows, d.rows, sizeof(rows));
                FlatDomainWorldPrediction detail;
                const bool taken = scaleRule ? (flatDomainPredictsWorld(rows, ref, &detail), detail.predicted) : nearOnly(rows);
                if (scaleRule && detail.nearEqual && !detail.predicted) ++scaleRejected;
                if (taken) { ++predicted; pending.add(51700, &depth, 3840, 2160, rows, 0, 0, d.weapon ? 0xF516BF0201303B87ull : 0xF516BF0201303B87ull, 0xB40B0462256E31C2ull, 1u); }
            }
            const bool matched = pending.matches(51700, &depth, 3840, 2160, world, 0, 0);
            if (!scaleRule) {
                char text[768];
                check(predicted == 5 && !matched && pending.describeMismatch(51700, &depth, 3840, 2160, world, 0, 0, text, sizeof(text)) &&
                          std::strstr(text, "kind=predicted-world-near") && std::strstr(text, "projection scale ratio witness/selected=(1.66,1.66)"),
                      "near alone: both weapon prepass draws are taken for the world, their witness cannot match the selected camera and H refuses (the flight's refusal), the line naming the 1.66 ratio");
            } else {
                char text[768];
                check(predicted == 3 && scaleRejected == 2 && matched && !pending.describeMismatch(51700, &depth, 3840, 2160, world, 0, 0, text, sizeof(text)),
                      "near and scale: the three world prepass draws are predicted, the two weapon draws are counted as rejected by scale, and the witnesses match the selected world camera");
            }
        }
        // The same sequence with the 10% margin met from the other side: a world draw 9% off its reference (a fast zoom) is still the world.
        float fast[6][4]; std::memcpy(fast, previousWorld, sizeof(fast)); scaled(fast, 1.09, 1.09);
        check(withScale(fast), "a world draw 9% off its reference (a fast zoom) is still predicted");
    }
    return failures;
}

// The runtime asks the one predicate at both sites (source pins with mutation controls).
inline int flatDomainWorldPredictionWiringTests() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) { if (!ok) { std::printf("FAIL: flat domain world prediction wiring %s\n", name); ++failures; } };
    const auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const auto compact = [](const std::string& in) {
        std::string out; out.reserve(in.size());
        for (char c : in) if (c != ' ' && c != '\r' && c != '\n' && c != '\t') out += c;
        return out;
    };
    const auto body = [](const std::string& text, const char* signature) {
        const size_t at = text.find(signature);
        if (at == std::string::npos) return std::string();
        const size_t end = text.find("\n}\n", at);
        return text.substr(at, end == std::string::npos ? std::string::npos : end + 3 - at);
    };
    const auto count = [](const std::string& text, const std::string& needle) {
        size_t n = 0, at = 0;
        while ((at = text.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
        return n;
    };
    const auto ordered = [&](const std::string& compacted, std::initializer_list<const char*> needles) {
        size_t pos = 0;
        bool ok = !compacted.empty();
        for (const char* needle : needles) {
            const std::string n = compact(needle);
            const size_t at = compacted.find(n, pos);
            if (at == std::string::npos) { ok = false; break; }
            pos = at + n.size();
        }
        return ok;
    };
    const auto without = [&](std::string text, const char* needle) {
        const std::string n = compact(needle);
        const size_t at = text.find(n);
        if (at != std::string::npos) text.erase(at, n.size());
        return text;
    };
    const std::string runtime = slurp("src/d3d11/flat_runtime.cpp");
    check(!runtime.empty(), "the runtime source is readable from the repo root");
    const std::string all = compact(runtime);
    const std::string scope = compact(body(runtime, "FlatRuntimeDrawScope::FlatRuntimeDrawScope("));

    // One predicate, one place that calls it: a lazy lambda, read by the cohort flag and by the domain's classification.
    const auto oneValid = [&](const std::string& text) {
        return count(text, "flatDomainPredictsWorld(") == 1 &&
               ordered(text, {"const auto worldPredicted=[&]()->const FlatDomainWorldPrediction& {",
                              "flatDomainPredictsWorld(rows,s.worldReference,&worldPrediction);",
                              "const bool predictedWorld=!s.namedDepth && worldPredicted().predicted;",
                              "d.firstPersonCohort=!predictedWorld && !worldCamera;",
                              "const bool predictedWorld=!s.namedDepth && k.camera && worldPredicted().predicted;",
                              "if(!s.namedDepth && k.camera && worldPredicted().nearEqual && !worldPredicted().predicted)++s.predictedScaleRejectedWindow;"}) &&
               text.find("drawNear") == std::string::npos && text.find("predictedWorldNear") == std::string::npos;
    };
    check(oneValid(scope) && oneValid(all),
          "the cohort flag and the domain's classification read the one prediction (flatDomainPredictsWorld: near and projection scale), and the near-only test is gone");
    check(!oneValid(without(scope, "const bool predictedWorld=!s.namedDepth && worldPredicted().predicted;")),
          "mutation control: a cohort flag with a test of its own fails the wiring");
    check(!oneValid(without(scope, "const bool predictedWorld=!s.namedDepth && k.camera && worldPredicted().predicted;")),
          "mutation control: a domain classification with a test of its own fails the wiring");
    check(!oneValid(scope + compact("float drawNear=0;std::memcpy(&drawNear,d.camera+(3*4+2)*sizeof(float),sizeof(float));")),
          "mutation control: a near-only test back anywhere in the draw scope fails the wiring");
    check(!oneValid(scope + compact("const bool again=flatDomainPredictsWorld(rows,s.worldReference);")), "mutation control: a second call of the predicate fails the wiring");
    check(!oneValid(without(scope, "++s.predictedScaleRejectedWindow;")), "mutation control: a rejection that is never counted fails the wiring");
    // The reference is stored where the world is named, beside the camera it describes.
    const auto namingValid = [&](const std::string& text) {
        return ordered(text, {"s.namedDepth=k.depth;s.namedConstants=k.b1;s.namedWorldQ=s.prefix.sequence;", "std::memcpy(s.namedCamera,d.camera,sizeof(d.camera));",
                              "s.worldReference=flatDomainWorldReference(namedRows);", "engineVelocityNoteSource("});
    };
    check(namingValid(scope), "naming the world stores its near plane and projection scales as the reference");
    check(!namingValid(without(scope, "s.worldReference=flatDomainWorldReference(namedRows);")), "mutation control: a naming that leaves no reference fails the wiring");
    // The 5 s line carries the window's counter and resets it with the other per-line windows.
    const std::string report = compact(body(runtime, "static void reportForegroundDomain(State& s) {"));
    const auto reportValid = [&](const std::string& text) {
        return ordered(text, {"predicted-near=%.9g scale-rejected-5s=%llu", "s.worldReference.nearPlane,(unsigned long long)s.predictedScaleRejectedWindow,",
                              "s.foregroundHRefusalWindow=nullptr;", "s.predictedScaleRejectedWindow=0;"});
    };
    check(reportValid(report), "the SDK domain line prints the near-equal draws the scale rejected since the last line, and zeroes the count");
    check(!reportValid(without(report, "s.predictedScaleRejectedWindow=0;")), "mutation control: a window that never resets fails the wiring");
    return failures;
}
