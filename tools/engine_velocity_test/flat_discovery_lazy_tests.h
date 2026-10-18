#pragma once
#include "flat_lazy_tests.h"
#include "../../src/d3d11/flat_temporal.h"
#include "../../src/d3d11/flat_camera_table.h"

// The full discovery collector is not linked into engine_velocity_test. These
// are its gate's real inline implementation with controlled collector atomics
// and compute-candidate input; the wrapper and D3D context below are real.
namespace discovery_lazy_tests { inline bool candidate=false; }
namespace edvr {
namespace detail {
inline std::atomic<bool> g_flatTemporalCapturing{false},g_flatTemporalPaused{false},g_flatTemporalManual{false};
inline std::atomic<DWORD> g_flatTemporalOwnerThread{0};
inline std::atomic<uint64_t> g_flatTemporalForeignCalls{0};
}
inline bool flatComputeCandidate() noexcept{return discovery_lazy_tests::candidate;}
}
namespace discovery_lazy_tests {
using namespace flat_lazy_tests;
inline void observing(bool manual,bool compute=false) {
    edvr::detail::g_flatTemporalCapturing=true;edvr::detail::g_flatTemporalPaused=false;
    edvr::detail::g_flatTemporalManual=manual;edvr::detail::g_flatTemporalOwnerThread=GetCurrentThreadId();
    edvr::detail::g_flatTemporalForeignCalls=0;candidate=compute;edvr::g_flatComputeInternal=false;
}
struct LogicalWitness {
    void* rtv=nullptr;void* dsv=nullptr;void* b1=nullptr;uint64_t vs=0,ps=0;
    unsigned char camera[edvr::kFlatCameraBytes]{};edvr::FlatCbExemplar exemplar{};
    uint64_t epoch=0,hash=0;uint32_t writeQ=0,drawQ=0,records=0,draws=0;bool complete=false;
};
inline LogicalWitness logicalWitness(Game& g) {
    LogicalWitness w;w.rtv=edvr::bindingGet(BindSlot::Rtv0);w.dsv=edvr::bindingGet(BindSlot::Dsv0);
    w.b1=edvr::bindingGet(BindSlot::VsCb1);w.vs=edvr::bindingShaderHash(BindSlot::Vs);w.ps=edvr::bindingShaderHash(BindSlot::Ps);
    w.epoch=19;w.writeQ=1;w.drawQ=2;
    // Exercise the production camera slice, CB exemplar and contract writer on
    // logical bindings while the real context alternates game/EDVR state. This
    // is a witness-slice fixture, not execution of flatTemporalDraw's assembly.
    const auto* bytes=reinterpret_cast<const unsigned char*>(g.rows[0].data());const auto width=uint32_t(g.rows[0].size()*sizeof(float));
    const bool camera=edvr::flatCaptureCameraRows(w.camera,bytes,width);w.hash=edvr::flatCameraHash(w.camera);
    const bool exemplar=edvr::flatFreezeCbExemplar(w.exemplar,w.b1,bytes,width,edvr::kFlatCbExemplarBytes,w.epoch,w.writeQ,w.epoch,w.drawQ,w.vs);
    edvr::FlatContractObservation key{};key.kind=edvr::kFlatContractPool;key.rtv=w.rtv;key.dsv=w.dsv;key.b1=w.b1;
    key.color=g.sourceColour[0].Get();key.depth=g.sourceDepth.Get();key.vs=w.vs;key.ps=w.ps;key.sequence=w.drawQ;
    key.count=4;key.instances=1;key.writeEpoch=w.epoch;key.writeSeq=w.writeQ;key.camera=w.camera;key.cameraHash=w.hash;
    edvr::FlatContractRecord records[1]{};uint32_t used=0,dropped=0;
    const auto* record=edvr::flatRecordContract(records,used,dropped,key);w.records=used;w.draws=record?record->draws:0;
    w.complete=camera&&exemplar&&record&&!dropped&&record->key.cameraHash==w.hash&&
        record->firstWriteEpoch==w.epoch&&record->lastWriteSeq==w.writeQ&&!std::memcmp(record->camera,w.camera,sizeof(w.camera));
    return w;
}
inline bool same(const LogicalWitness& a,const LogicalWitness& b) {
    return a.complete&&b.complete&&a.rtv==b.rtv&&a.dsv==b.dsv&&a.b1==b.b1&&a.vs==b.vs&&a.ps==b.ps&&
        a.epoch==b.epoch&&a.hash==b.hash&&a.writeQ==b.writeQ&&a.drawQ==b.drawQ&&a.records==b.records&&a.draws==b.draws&&
        !std::memcmp(a.camera,b.camera,sizeof(a.camera))&&a.exemplar.resource==b.exemplar.resource&&
        a.exemplar.width==b.exemplar.width&&a.exemplar.copied==b.exemplar.copied&&a.exemplar.shader==b.exemplar.shader&&
        a.exemplar.writeEpoch==b.exemplar.writeEpoch&&a.exemplar.writeSeq==b.exemplar.writeSeq&&
        a.exemplar.drawEpoch==b.exemplar.drawEpoch&&a.exemplar.drawSeq==b.exemplar.drawSeq&&
        !std::memcmp(a.exemplar.bytes,b.exemplar.bytes,sizeof(a.exemplar.bytes));
}
inline void run(const Harness& h) {
    edvr::g_runtimeProfile=edvr::RuntimeProfile::Flat;edvr::g_clockForTest=&lifecycle_fake::fakeClock;
    observing(false);
    h.check(edvr::flatTemporalCapturing()&&!edvr::flatTemporalNeedsOriginalState(),"discovery lazy: automatic observation stays active without demanding physical game state");
    observing(true);h.check(edvr::flatTemporalNeedsOriginalState(),"discovery lazy: manual General/Full demand physical state");
    observing(false,true);h.check(edvr::flatTemporalNeedsOriginalState(),"discovery lazy: a surviving candidate independently demands physical state during automatic observation");
    edvr::detail::g_flatTemporalCapturing=false;h.check(!edvr::flatTemporalNeedsOriginalState(),"discovery lazy: inactive observation cannot run candidate readers");
    observing(true,true);edvr::detail::g_flatTemporalPaused=true;h.check(!edvr::flatTemporalNeedsOriginalState(),"discovery lazy: paused observation cannot demand a restore");
    edvr::detail::g_flatTemporalPaused=false;h.check(edvr::flatTemporalNeedsOriginalState(),"discovery lazy: resumed manual observation regains demand");
    {edvr::FlatComputeInternalScope outer;h.check(!edvr::flatTemporalNeedsOriginalState(),"discovery lazy: internal producer calls cannot recursively demand restoration");
        {edvr::FlatComputeInternalScope inner;h.check(!edvr::flatTemporalNeedsOriginalState(),"discovery lazy: nested internal scopes stay suppressed");}
        h.check(!edvr::flatTemporalNeedsOriginalState(),"discovery lazy: inner exit preserves outer suppression");}
    h.check(edvr::flatTemporalNeedsOriginalState(),"discovery lazy: outer exit restores the prior observer demand");
    edvr::detail::g_flatTemporalOwnerThread=0;h.check(!edvr::flatTemporalNeedsOriginalState(),"discovery lazy: no owner is not permission to restore");
    edvr::detail::g_flatTemporalOwnerThread=GetCurrentThreadId()+1;
    h.check(!edvr::flatTemporalNeedsOriginalState()&&edvr::detail::g_flatTemporalForeignCalls.load()==0,"discovery lazy: foreign demand query neither restores nor increments observation counters");
    observing(true,true);
    observing(false,true);h.check(edvr::flatTemporalNeedsOriginalState(),"discovery lazy: passive rearm clears manual demand but a surviving candidate still requires physical state");
    observing(false,false);h.check(!edvr::flatTemporalNeedsOriginalState(),"discovery lazy: passive rearm with no candidate returns to lazy observation");
    observing(false);
    recInstall(h.context);
    {
        Game g(h);g.setup();for(auto& slot:lifecycle_fake::g_slots){slot.ptr=nullptr;slot.hash=0;++slot.gen;}
        edvr::engineVelocityConfigure(true);edvr::engineVelocityFlatLazy(true);g.makeSource(40,24);
        SceneCleanup cleanup(g.ctx);Emu<> e(h,g);e.warmUp();e.startFrame();
        const auto original=logicalWitness(g);h.check(original.complete,"discovery lazy: camera96/exemplar4096/contract record witness is complete");
        e.producer("automatic discovery producer");
        h.check(edvr::engineVelocityFlatPending(),"discovery lazy: automatic producer leaves the real bracket pending");
        e.gameGetPixelShader("automatic observer's real context differs from logical PS",true);
        h.check(same(original,logicalWitness(g)),"discovery lazy: logical binding IDs, shader hashes, camera bytes, CB bytes/epochs and record count survive physical substitution");
        for(unsigned request=0;request<3;++request) {
            observing(request!=2,request==2); // General, Full, then candidate without manual.
            if(edvr::flatTemporalNeedsOriginalState())e.substitution(FlatSubstEvent::kOtherDraw);
            e.expectGame("first physical draw observer after a lazy run");
            h.check(!edvr::engineVelocityFlatPending()&&same(original,logicalWitness(g)),"discovery lazy: manual/candidate preflight restores before first physical observer and preserves logical witnesses");
            edvr::engineVelocityFlatLazy(!edvr::flatTemporalNeedsOriginalState());e.producer("manual/candidate eager producer");e.expectGame("manual producer restores at its own end");
            observing(false);
            h.check(!edvr::flatTemporalNeedsOriginalState(),"discovery lazy: rearmed passive observation releases manual/candidate physical-state demand");
            edvr::engineVelocityFlatLazy(true);e.producer("automatic observation resumes a lazy run");
            h.check(edvr::engineVelocityFlatPending(),"discovery lazy: passive rearm uses the lazy bracket again after manual/candidate observation");
        }
        observing(false,true);if(edvr::flatTemporalNeedsOriginalState())e.substitution(FlatSubstEvent::kDispatch);
        e.expectGame("physical compute observer preflight");h.check(!edvr::engineVelocityFlatPending(),"discovery lazy: dispatch observer cannot inherit private producer bindings");
        h.check(e.ok,"discovery lazy: actual WARP state obeys observer transitions");e.present();
    }
    observing(false);const auto eager=countRuns(h,Form::EagerBracket,1,128,false);const auto automatic=countRuns(h,Form::Lazy,1,128,false);
    observing(true);const auto manual=countRuns(h,edvr::flatTemporalNeedsOriginalState()?Form::EagerBracket:Form::Lazy,1,128,false);
    std::printf("  discovery lazy: 128 producers, actual recorded context calls eager=%llu automatic=%llu manual=%llu; bounded startup amplification fixture, no FPS estimate\n",eager,automatic,manual);
    h.check(eager>0&&automatic*10<eager&&manual==eager,"discovery lazy: passive policy removes per-producer eager call amplification while manual retains it");
    recRemove();observing(false);edvr::detail::g_flatTemporalCapturing=false;edvr::g_runtimeProfile=edvr::RuntimeProfile::LegacyVr;
}
} // namespace discovery_lazy_tests
