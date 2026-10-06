#pragma once
#include "dxbc_engine_velocity.h"
#include "flat_projection_recipes.h"
#include "flat_shader_classifier.h"
#include <array>
#include <cstring>

namespace edvr {
inline uint64_t flatDomainBytecodeHash(const void* data,size_t size) {
    uint64_t h=1469598103934665603ull;
    const auto* bytes=static_cast<const unsigned char*>(data);
    for(size_t i=0;i<size;++i){h^=bytes[i];h*=1099511628211ull;}return h;
}
struct FlatDomainShaderProof {
    bool present=false,pool=false,projection=false,nullPs=false,worldPs=false,foreignPs=false;
    bool inertNoSideEffects=false;
    unsigned projectionSlot=0,projectionRow=0;
    FlatProjectionPatchLayout projectionLayout=FlatProjectionPatchLayout::ForwardColumns;
    std::array<unsigned char,6> colorComponents{};
    const char* refusal="foreground-original-shader-unavailable";
};
// The camera classifier proves jitter independence, not freedom from UAV
// effects. Accept only its understood read/arithmetic/control operations and
// known read-only declarations. The store opcodes 164/166, other stores,
// atomics, UAV/thread declarations and unknown token forms all fail closed.
inline bool flatDomainNoSideEffectProgram(const void* bytes,size_t size,uint32_t stage) {
    try {
        const auto chunks=dxbc_engine_velocity_detail::parseContainer(bytes,size,stage);
        bool found=false,returned=false;
        for(const auto& chunk:chunks)if(dxbc_engine_velocity_detail::isProgram(chunk.tag)) {
            if(found)return false;
            found=true;
            const auto words=dxbc_engine_velocity_detail::programWords(chunk.bytes);
            std::vector<flat_shader_classifier_detail::Instr> instructions;
            flat_shader_classifier_detail::ProgramFacts facts;
            if(!flat_shader_classifier_detail::walkProgram(words,instructions,facts) ||
               facts.sawUnknownOpcode || facts.operandOutOfRange)return false;
            for(const auto& in:instructions) {
                const uint32_t op=in.opcode;
                if(in.parseError || in.unmodelable)return false;
                if(op==88 || op==dxbc_engine_velocity_detail::kOpDclConstantBuffer || op==90 ||
                   (op>=dxbc_engine_velocity_detail::kOpDclInput && op<=dxbc_engine_velocity_detail::kOpDclTemps) ||
                   op==dxbc_engine_velocity_detail::kOpDclGlobalFlags || op==161 || op==162)continue;
                if(flat_shader_classifier_detail::isDeclaration(op) ||
                   op==164 || op==166 || op>=168 ||
                   flat_shader_classifier_detail::operandCount(op)<0)return false;
                if(op==dxbc_engine_velocity_detail::kOpRet) returned=true;
            }
        }
        return found && returned;
    } catch(...) {return false;}
}
// Creation bytes, rather than membership of the legacy world producer table,
// establish the additional flat ownership producer. Exact projection recipes
// remain valid only for the exact bytecode identities they describe.
inline FlatDomainShaderProof flatDomainShaderProof(uint64_t vsHash,uint64_t psHash,
    const void* vs,size_t vsSize,const void* ps,size_t psSize) {
    FlatDomainShaderProof p;p.nullPs=psHash==0;
    if(!vs || !vsSize || (!p.nullPs && (!ps || !psSize)))return p;
    if(flatDomainBytecodeHash(vs,vsSize)!=vsHash ||
       (!p.nullPs && flatDomainBytecodeHash(ps,psSize)!=psHash)) {
        p.refusal="foreground-original-bytecode-identity";return p;
    }
    p.present=true;EngineVelocityInputs inputs{};std::string why;
    p.pool=engineVelocityDeriveInputs(vs,vsSize,inputs,why);
    auto recipes=flatProjectionDrawRecipes(vsHash,psHash);
    // The audited 72BDD vertex suffix is an unconditional B1[270..273]
    // forward-column clip calculation, independent of its consumer PS. Reuse
    // that existing exact-bytecode recipe; PS ownership proof remains below.
    if(!recipes.count && vsHash==0x72BDD292154158ADull)
        recipes=flatProjectionDrawRecipes(vsHash,0x76849D64AC657DB9ull);
    for(unsigned i=0;i<recipes.count;++i) {
        const auto& r=recipes.requests[i];
        if(r.stage!=FlatProjectionStage::Vertex || r.patchCount!=1)continue;
        const auto& patch=r.patches[0];
        if(patch.layout!=FlatProjectionPatchLayout::ForwardColumns &&
           patch.layout!=FlatProjectionPatchLayout::ForwardDp4)continue;
        p.projection=true;p.projectionSlot=r.slot;p.projectionRow=patch.byteOffset/16;p.projectionLayout=patch.layout;break;
    }
    if(!p.projection) {
        const auto c=classifyFlatShaderPair(vs,vsSize,ps,psSize);
        p.inertNoSideEffects=!p.nullPs && c.vs==FlatVsProjectionClass::InertNoCB &&
            c.ps==FlatPsProjectionSafety::Clean &&
            flatDomainNoSideEffectProgram(vs,vsSize,dxbc_engine_velocity_detail::kVs50) &&
            flatDomainNoSideEffectProgram(ps,psSize,dxbc_engine_velocity_detail::kPs50);
        if(c.vs==FlatVsProjectionClass::ForwardColumns || c.vs==FlatVsProjectionClass::ForwardDp4) {
            p.projection=true;p.projectionSlot=c.vsSlot;p.projectionRow=c.vsRow;
            p.projectionLayout=c.vs==FlatVsProjectionClass::ForwardColumns?
                FlatProjectionPatchLayout::ForwardColumns:FlatProjectionPatchLayout::ForwardDp4;
        }
    }
    if(!p.projection){p.refusal="foreground-original-projection-unproven";return p;}
    if(p.nullPs){p.worldPs=true;p.foreignPs=p.pool;p.refusal=nullptr;return p;}
    bool discard=false;
    try {
        const auto chunks=dxbc_engine_velocity_detail::parseContainer(ps,psSize,dxbc_engine_velocity_detail::kPs50);
        for(const auto& chunk:chunks) {
            if(chunk.tag==dxbc_engine_velocity_detail::kTagOsgn)
                for(const auto& output:dxbc_engine_velocity_detail::parseSignature(chunk.bytes)) {
                    if(output.systemValue && output.systemValue!=64){p.refusal="foreground-depth-or-coverage-output";return p;}
                    if((output.systemValue==64 || dxbc_engine_velocity_detail::equalName(output.name,"SV_Target")) && output.semanticIndex<6)
                        p.colorComponents[output.semanticIndex]=static_cast<unsigned char>(output.masks&15);
                }
            if(dxbc_engine_velocity_detail::isProgram(chunk.tag)) {
                const auto words=dxbc_engine_velocity_detail::programWords(chunk.bytes);
                for(size_t at=2;at<words.size();at+=dxbc_engine_velocity_detail::instructionLength(words,at))
                    discard=discard || (words[at]&0x7ffu)==13;
            }
        }
    } catch(...) {p.refusal="foreground-original-PS-proof";return p;}
    std::vector<BYTE> derivative;EngineVelocityInputs world=inputs;
    if(!p.pool){world.positionRegister=30;world.identityRegister=31;world.identityComponent=0;}
    p.worldPs=engineVelocityPatchPs(ps,psSize,world,derivative,why,false,p.pool?
        dxbc_engine_velocity_detail::FlatMarkerKind::WorldPool:dxbc_engine_velocity_detail::FlatMarkerKind::World);
    unsigned tokenSlot=~0u;
    p.foreignPs=p.pool && engineVelocityPatchPs(ps,psSize,inputs,derivative,why,false,
        dxbc_engine_velocity_detail::FlatMarkerKind::ForeignPoolProvenance,&tokenSlot);
    p.refusal=!p.worldPs?"foreground-original-PS-proof":discard && !p.foreignPs?"foreground-foreign-discard-provenance":nullptr;
    return p;
}
struct FlatDomainInertBindings {
    const D3D11_BLEND_DESC* blend=nullptr;
    const D3D11_DEPTH_STENCIL_DESC* depth=nullptr;
    unsigned boundTargets=0;
    bool actualShaderPair=false,originalDsv=false,readOnlyDepth=false;
    bool noOtherStages=false,noUavs=false,noStreamOutput=false,noPredicate=false;
};
inline const char* flatDomainInertRefusal(const FlatDomainShaderProof& proof,const FlatDomainInertBindings& b) {
    if(!proof.present || !proof.inertNoSideEffects)return "foreground-inert-shader-unproven";
    if(!b.actualShaderPair)return "foreground-inert-actual-shader-mismatch";
    if(!b.originalDsv)return "foreground-inert-original-DSV-mismatch";
    if(!b.noOtherStages)return "foreground-inert-other-shader-stage";
    if(!b.noUavs)return "foreground-inert-OM-UAV-bound";
    if(!b.noStreamOutput)return "foreground-inert-stream-output-bound";
    if(!b.noPredicate)return "foreground-inert-predicate-bound";
    if(!b.readOnlyDepth && (!b.depth || (b.depth->DepthEnable &&
        b.depth->DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO)))return "foreground-inert-depth-write";
    for(unsigned i=0;i<8;++i)if(b.boundTargets&(1u<<i)) {
        const unsigned mask=b.blend?
            b.blend->RenderTarget[b.blend->IndependentBlendEnable?i:0].RenderTargetWriteMask:
            D3D11_COLOR_WRITE_ENABLE_ALL;
        if(mask)return "foreground-inert-color-write";
    }
    return nullptr;
}
inline bool flatDomainInertNoWrite(const FlatDomainShaderProof& proof,const FlatDomainInertBindings& b) {
    return flatDomainInertRefusal(proof,b)==nullptr;
}
// A checked actual B1 publication can serve multiple independently proven
// shaders using the same canonical projection. Other recipes must be checked
// against their own actual bound constants rather than inheriting this receipt.
struct FlatDomainPhasePublication {
    uint64_t epoch=0;uint32_t generation=0;bool valid=false;
    static bool canonical(const FlatDomainShaderProof& p) {
        return p.projection && p.projectionSlot==1 && p.projectionRow==270 &&
            p.projectionLayout==FlatProjectionPatchLayout::ForwardColumns;
    }
    void checked(const FlatDomainShaderProof& p,uint32_t binding,uint64_t publication) {
        valid=canonical(p);generation=binding;epoch=publication;
    }
    bool matches(const FlatDomainShaderProof& p,uint32_t binding,uint64_t publication)const {
        return valid && canonical(p) && generation==binding && epoch==publication;
    }
};
enum class FlatDomainPlanKind { Refuse,World,WorldPool,ForeignPool,PendingWorldNull };
struct FlatDomainPlan {
    FlatDomainPlanKind kind=FlatDomainPlanKind::Refuse;
    const char* refusal=nullptr;
    bool admitted()const{return kind!=FlatDomainPlanKind::Refuse;}
};
inline FlatDomainPlan flatDomainPlan(const FlatDomainShaderProof& p,bool color,uint32_t format,
                                     bool camera,bool namedWorld,bool sameWorld) {
    if(!camera)return {FlatDomainPlanKind::Refuse,"foreground-original-camera-unavailable"};
    if(color && format!=23 && !(format==26 && namedWorld && sameWorld && p.pool && !p.nullPs))
        return {FlatDomainPlanKind::Refuse,"foreground-non-Gbuffer-writer"};
    if(!p.present || !p.projection || !p.worldPs)return {FlatDomainPlanKind::Refuse,p.refusal};
    if(p.nullPs && !p.pool) {
        if(!namedWorld)return {FlatDomainPlanKind::PendingWorldNull,nullptr};
        return sameWorld?FlatDomainPlan{FlatDomainPlanKind::World,nullptr}:
            FlatDomainPlan{FlatDomainPlanKind::Refuse,"foreground-null-camera-not-world"};
    }
    if(!namedWorld || !sameWorld) {
        if(!p.pool || !p.foreignPs)return {FlatDomainPlanKind::Refuse,p.refusal?p.refusal:"foreground-foreign-pool-unproven"};
        return {FlatDomainPlanKind::ForeignPool,nullptr};
    }
    return {p.pool?FlatDomainPlanKind::WorldPool:FlatDomainPlanKind::World,nullptr};
}
inline bool flatDomainWritesColor(const FlatDomainShaderProof& p,const D3D11_BLEND_DESC& blend,unsigned boundTargets) {
    for(unsigned i=0;i<6;++i)if((boundTargets&(1u<<i)) &&
        (blend.RenderTarget[blend.IndependentBlendEnable?i:0].RenderTargetWriteMask&p.colorComponents[i]))return true;
    return false;
}
inline const char* flatDomainRasterRefusal(const FlatDomainShaderProof& p,const D3D11_BLEND_DESC& blend,
    const D3D11_DEPTH_STENCIL_DESC& depth,unsigned boundTargets,bool foreign,bool hdr,bool colorAlready) {
    if(blend.AlphaToCoverageEnable)return "foreground-alpha-to-coverage";
    if((foreign || hdr) && (!depth.DepthEnable ||
       (depth.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL && depth.DepthFunc!=D3D11_COMPARISON_EQUAL)))
        return "foreground-unproven-readonly-depth";
    // H's primitive/writer tuple limits the raster to fragments that passed
    // the original stencil and discard. Unstamped replay cannot inherit that
    // proof. World HDR likewise writes color and owner in the same fragment.
    if(foreign && !p.foreignPs && depth.StencilEnable &&
       (depth.FrontFace.StencilFunc!=D3D11_COMPARISON_ALWAYS || depth.BackFace.StencilFunc!=D3D11_COMPARISON_ALWAYS))
        return "foreground-conditional-stencil";
    for(unsigned i=0;i<6;++i)if((boundTargets&(1u<<i)) && p.colorComponents[i]) {
        const auto& rt=blend.RenderTarget[blend.IndependentBlendEnable?i:0];
        if(!rt.RenderTargetWriteMask)continue;
        if(rt.BlendEnable || (rt.RenderTargetWriteMask&p.colorComponents[i])!=p.colorComponents[i])
            return "foreground-mixed-component-writer";
    }
    const bool color=flatDomainWritesColor(p,blend,boundTargets);
    if(hdr && (!color || (p.colorComponents[0]&7)!=7))return "foreground-incomplete-HDR-color";
    if((p.nullPs || !color) && colorAlready)return "foreground-depth-only-after-color";
    return nullptr;
}
// A pre-world depth-only draw is provisional: its camera must become the
// actual selected world camera in this frame. No later-material coverage is
// guessed, and a foreign or missing world witness leaves the frame refused.
class FlatDomainPendingNull {
    struct Witness { const void* depth=nullptr;unsigned width=0,height=0;float camera[6][4]{};float x=0,y=0; };
    std::array<Witness,4> witnesses_{};unsigned count_=0;uint64_t frame_=~0ull;
public:
    void beginFrame(uint64_t frame){if(frame_!=frame){frame_=frame;count_=0;}}
    bool add(uint64_t frame,const void* depth,unsigned width,unsigned height,const float camera[6][4],float x,float y) {
        beginFrame(frame);Witness w{depth,width,height,{},x,y};std::memcpy(w.camera,camera,sizeof(w.camera));
        for(unsigned i=0;i<count_;++i)if(witnesses_[i].depth==depth && witnesses_[i].width==width && witnesses_[i].height==height &&
            witnesses_[i].x==x && witnesses_[i].y==y && std::memcmp(witnesses_[i].camera,camera,sizeof(w.camera))==0)return true;
        if(count_==witnesses_.size())return false;witnesses_[count_++]=w;return true;
    }
    bool matches(uint64_t frame,const void* depth,unsigned width,unsigned height,const float camera[6][4],float x,float y)const {
        if(frame!=frame_)return count_==0;
        for(unsigned i=0;i<count_;++i){const auto& w=witnesses_[i];if(w.depth!=depth || w.width!=width || w.height!=height ||
            w.x!=x || w.y!=y || std::memcmp(w.camera,camera,sizeof(w.camera)))return false;}return true;
    }
    unsigned count()const{return count_;}
};
}
