#pragma once
#include "../../src/d3d11/flat_domain_admission.h"
#include "flat_shader_classifier_tests.h"
#include <d3dcompiler.h>

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
