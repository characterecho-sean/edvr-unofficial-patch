#pragma once
#include <fstream>
#include <string>
#include "../../src/d3d11/flat_deferred_camera.h"
#include "../../src/d3d11/flat_projection_math.h"
#include "../../src/d3d11/flat_projection_bindings.h"
#include "../../src/d3d11/flat_standdown.h"
#include "flat_laser_scene_tone_tests.h"
namespace deferred_camera_test {
using namespace edvr;
inline unsigned run(){
 unsigned failures=0;auto expect=[&](bool ok,const char* name){if(!ok){std::printf("FAIL: deferred camera %s\n",name);++failures;}};
 const uint32_t c[]={0xBF06B496,0xBFBAA737,0,0xB6C74B8F,0xBF695104,0x3F578757,0,0xB76E7C54,0x3786E4D9,0xB65DE5B1,0,0xBF800000,0,0,0x3CCCCCCD,0,0xB6C74B8F,0xB76E7C54,0xBF800000,0,0xC0D2CE6F,0x40FD9541,0xC1F57045,0};
 const uint32_t b[]={0xBF06B496,0xBF695104,0x3786E4D9,0x407068BF,0xBFBAA737,0x3F578757,0xB65DE5B1,0xC18239A7,0,0,0,0x3CCCCCCD,0xB6C74B8F,0xB76E7C54,0xBF800000,0xC1F5701E,0xBF7341C8,0x3F83AAA6,0x80000000,0xBD20A25A,0xBFD2AAA5,0xBF18091D,0,0x3F8F5723,0x37F398F1,0x361C872D,0,0xBF800084};
 float camera[6][4],rows[7][4];std::memcpy(camera,c,sizeof(c));std::memcpy(rows,b,sizeof(b));
 auto match=[](const void* a,const void* x){return flatMatchDeferredCamera(static_cast<const unsigned char*>(a),static_cast<const unsigned char*>(x))==FlatDeferredCameraMatch::Match;};
 expect(match(camera,rows),"actual frame 9201 original CB2 agrees with donor");
 for(unsigned index:{0u,3u,16u,21u}){float bad[7][4];std::memcpy(bad,rows,sizeof(bad));reinterpret_cast<float*>(bad)[index]+=.01f;expect(!match(camera,bad),"forward/translation/inverse/Y-flip alteration rejected");}
 float badCamera[6][4];std::memcpy(badCamera,camera,sizeof(camera));badCamera[0][0]=std::numeric_limits<float>::infinity();expect(!match(badCamera,rows),"nonfinite donor rejected");
 float jitterCamera[6][4],jitterRows[7][4],forward[4][4],inverse[3][4];std::memcpy(jitterCamera,camera,sizeof(camera));std::memcpy(jitterRows,rows,sizeof(rows));std::memcpy(forward,camera,sizeof(forward));
 FlatProjectionJitter jitter{};flatProjectionJitter(.375f,-.25f,1248,780,jitter);flatJitterForwardColumns(forward,jitter);std::memcpy(jitterCamera,forward,sizeof(forward));std::memcpy(forward,rows,sizeof(forward));flatJitterForwardDp4(forward,jitter);std::memcpy(jitterRows,forward,sizeof(forward));std::memcpy(inverse,rows+4,sizeof(inverse));flatJitterInverseUvRay(inverse,jitter);std::memcpy(jitterRows+4,inverse,sizeof(inverse));
 expect(match(jitterCamera,jitterRows),"jointly propagated nonzero jitter agrees");expect(!match(jitterCamera,rows)&&!match(camera,jitterRows),"donor-only and H-only jitter reject");
 float singularCamera[6][4]{},singularRows[7][4]{};singularCamera[0][0]=singularCamera[0][1]=singularCamera[1][0]=singularCamera[1][1]=1;singularCamera[2][3]=singularCamera[4][2]=1;singularCamera[3][2]=.025f;
 singularRows[0][0]=singularRows[0][1]=singularRows[1][0]=singularRows[1][1]=1;singularRows[3][2]=1;singularRows[2][3]=.025f;singularRows[4][0]=singularRows[4][1]=1e8f;singularRows[4][3]=-1e8f;singularRows[5][0]=singularRows[5][1]=-1e8f;singularRows[5][3]=1e8f;singularRows[6][3]=1;
 expect(!match(singularCamera,singularRows),"singular cancelling basis cannot certify an inverse");singularCamera[1][1]=singularRows[1][1]=std::nextafter(1.f,2.f);expect(!match(singularCamera,singularRows),"near singular cancelling basis cannot certify an inverse");
 expect(flatDeferredExecutionCoherent(true,FlatDeferredCameraMatch::Match,false),"coherent original executes unchanged");
 expect(flatDeferredExecutionCoherent(true,FlatDeferredCameraMatch::Inverse,true),"certified prepared clone executes coherently");
 expect(!flatDeferredExecutionCoherent(true,FlatDeferredCameraMatch::Inverse,false) && !flatDeferredExecutionCoherent(false,FlatDeferredCameraMatch::Match,true),"later unproved inverse or changed ownership fails closed");
 // Captured frame 6060: primary 1248x780 and secondary 256x256 share the
 // shader pair, but have separate HDR/depth identities. Ownership must precede
 // preparation and cannot be inferred from a successful camera certificate.
 const void* mainH=laser_scene_tone_test::token(0x6F988DD0);
 const void* mainDepth=laser_scene_tone_test::token(0x6F987B10);
 const void* secondaryH=laser_scene_tone_test::token(0xA1E842E0);
 const void* secondaryDepth=laser_scene_tone_test::token(0x6F6D8780);
 auto domain=[&](const void* h,const void* depth,unsigned w,unsigned height,const void* namedH){
     return flatDeferredPhaseDomain(h,depth,w,height,namedH,mainDepth,mainDepth,1248,780);};
 expect(domain(mainH,mainDepth,1248,780,mainH)==FlatDeferredPhaseDomain::KnownHdr,"captured primary belongs to selected HDR");
 expect(domain(mainH,nullptr,0,1,mainH)==FlatDeferredPhaseDomain::KnownHdr,"known HDR remains protected with corrupt depth and extent");
 expect(domain(secondaryH,secondaryDepth,256,256,mainH)==FlatDeferredPhaseDomain::Outside,"captured secondary domain is outside phase ownership");
 expect(domain(secondaryH,mainDepth,1248,780,mainH)==FlatDeferredPhaseDomain::Outside,"shared depth does not promote another target when HDR is known");
 expect(domain(mainH,mainDepth,1248,780,nullptr)==FlatDeferredPhaseDomain::SceneDepthExtent,"HDR-null candidate needs owned depth and extent");
 expect(domain(mainH,nullptr,1248,780,nullptr)==FlatDeferredPhaseDomain::Outside &&
        domain(mainH,mainDepth,256,256,nullptr)==FlatDeferredPhaseDomain::Outside &&
        flatDeferredPhaseDomain(mainH,mainDepth,0,0,nullptr,mainDepth,nullptr,0,0)==FlatDeferredPhaseDomain::Outside,"missing depth or phase extent cannot own a candidate");
 // Exercise the production gate followed by its execution-coherence decision.
 // No donor (missing, stale or ambiguous) is equivalent to qualified=false;
 // it must not affect ownership or exempt selected HDR from failure.
 unsigned preparations=0,failuresInPhase=0;
 auto execute=[&](FlatDeferredPhaseDomain owned,bool nonzero,bool qualified,FlatDeferredCameraMatch relation){
     if(!flatDeferredVerifyInPhase(nonzero,owned))return;
     if(qualified && relation==FlatDeferredCameraMatch::Inverse)++preparations;
     if(nonzero && !flatDeferredExecutionCoherent(qualified,relation,false))++failuresInPhase;
 };
 auto outside=domain(secondaryH,secondaryDepth,256,256,mainH);
 execute(outside,true,true,FlatDeferredCameraMatch::Match);
 execute(outside,true,false,FlatDeferredCameraMatch::Unavailable);
 execute(outside,true,true,FlatDeferredCameraMatch::Inverse);
 expect(preparations==0 && failuresInPhase==0,"coherent secondary then unproved secondary never prepares or poisons scene");
 for(unsigned missingStaleAmbiguous=0;missingStaleAmbiguous<3;++missingStaleAmbiguous)
     execute(domain(mainH,nullptr,0,0,mainH),true,false,FlatDeferredCameraMatch::Unavailable);
 execute(domain(mainH,mainDepth,1248,780,nullptr),true,false,FlatDeferredCameraMatch::Unavailable);
 execute(domain(mainH,mainDepth,1248,780,mainH),true,true,FlatDeferredCameraMatch::Inverse);
 expect(failuresInPhase==5,"selected and pending-main donor failures plus later owned inverse mismatch retain veto");
 expect(flatDeferredVerifyInPhase(false,outside) && !flatDeferredVerifyInPhase(true,outside),"zero phase permits strict new scene bootstrap without inventing nonzero execution");
 expect(flatDeferredExecutionCoherent(true,FlatDeferredCameraMatch::Match,false) &&
        !flatDeferredExecutionCoherent(false,FlatDeferredCameraMatch::Match,false),"zero-phase bootstrap still requires qualified original proof");
 // Pin the integration order as well as the pure policy: moving the gate after
 // preparation would mutate an outside-domain clone before the policy returns.
 std::ifstream runtimeSource("src/d3d11/flat_runtime.cpp");std::string sourceLine,verifier;
 bool foundVerifier=false;
 while(std::getline(runtimeSource,sourceLine)) {
     if(sourceLine.find("const FlatProjectionBindingPlan* verifyDeferredHdrCamera(")!=std::string::npos)foundVerifier=true;
     if(foundVerifier){verifier+=sourceLine+'\n';if(sourceLine=="}" || verifier.size()>16384)break;}
 }
 const auto gate=verifier.find("if(flatCameraInjectUpstreamOwns() && !flatDeferredVerifyInPhase(nonzero,phaseDomain))return nullptr;");
 const auto firstCheck=verifier.find("++s.deferredCameraChecks");
 const auto preflight=verifier.find("s.projection->preflight(");
 const auto failure=verifier.find("failPhase(s,");
 expect(foundVerifier && gate!=std::string::npos && firstCheck!=std::string::npos &&
        preflight!=std::string::npos && failure!=std::string::npos && gate<firstCheck && gate<preflight && gate<failure,
        "Upstream outside gate precedes all verifier work/preparation/failure while preserving Legacy");
 // Exact same-frame current original payload from the nonzero diagnostic flight.
 const uint32_t captureCamera[]={0xBF69510B,0xBF578742,0x00000000,0xB72B8D41,0xBF06B489,0x3FBAA73D,0x00000000,0xB7125F51,0x3A4892F6,0x3974AE5A,0x00000000,0xBF800000,0x00000000,0x00000000,0x3CCCCCCD,0x00000000,0xB72B8D41,0xB7125F51,0xBF800000,0x00000000,0x401E8A8A,0xC075C0CE,0xC2512BFA,0x00000000};
 const uint32_t captureRows[]={0xBF69510B,0xBF06B489,0x3A4892F6,0x3E8DECBA,0xBF578742,0x3FBAA73D,0x3974AE5A,0x40F64FA6,0x00000000,0x00000000,0x00000000,0x3CCCCCCD,0xB72B8D41,0xB7125F51,0xBF800000,0xC2512BFC,0xBFD2AAAE,0x3F180910,0x80000000,0x3F06A57A,0xBF7341B4,0xBF83AAAC,0x00000000,0x3F7D4AF4,0x37D2B746,0x3642BB1C,0x00000000,0xBF800076};
 FlatProjectionJitter captureJitter{};flatProjectionJitter(-.46875f,.092592597f,1248,780,captureJitter);
 unsigned char repaired[112]{};
 auto repair=[&](const void* cam,const void* original,const FlatProjectionJitter& phase){return flatRepairDeferredMissingPhase(static_cast<const unsigned char*>(cam),static_cast<const unsigned char*>(original),phase,repaired);};
 expect(repair(captureCamera,captureRows,captureJitter),"captured 9678 missing inverse phase certified and repaired");
 expect(match(captureCamera,repaired),"corrected captured rows strictly match current donor");
 expect(std::memcmp(repaired,captureRows,64)==0,"repair never changes already phased forward rows");
 expect(!repair(captureCamera,repaired,captureJitter),"already coherent inverse remains unchanged");
 unsigned char twice[112];std::memcpy(twice,repaired,112);float doubleRays[3][4];std::memcpy(doubleRays,twice+64,48);flatJitterInverseUvRay(doubleRays,captureJitter);std::memcpy(twice+64,doubleRays,48);
 expect(!repair(captureCamera,twice,captureJitter),"double phase is not missing current phase");
 expect(!repair(singularCamera,singularRows,captureJitter),"singular missing-phase candidate cannot certify");

 auto opposite=captureJitter;opposite.ndcX=-opposite.ndcX;opposite.ndcY=-opposite.ndcY;opposite.uvX=-opposite.uvX;opposite.uvY=-opposite.uvY;
 expect(!repair(captureCamera,captureRows,opposite),"opposite phase is not missing current phase");
 auto previous=captureJitter;previous.ndcX*=.5f;previous.ndcY*=.5f;previous.uvX*=.5f;previous.uvY*=.5f;
 expect(!repair(captureCamera,captureRows,previous),"previous phase is not missing current phase");
 unsigned char altered[112];std::memcpy(altered,captureRows,112);float wrong;std::memcpy(&wrong,altered+16*4+12,4);wrong+=.01f;std::memcpy(altered+16*4+12,&wrong,4);
 expect(!repair(captureCamera,altered,captureJitter),"unrelated inverse mismatch cannot be repaired");
 unsigned char entire[784]{},clone[784]{};std::memcpy(entire+160,captureRows,112);
 FlatProjectionPatchRequest inverseOnly{FlatProjectionPatchLayout::InverseUvRay,224,{}};
 expect(flatPrepareProjectionPatch(entire,784,&inverseOnly,1,captureJitter,clone,784),"existing shared preparation uploads inverse-only clone");
 expect(std::memcmp(clone+160,repaired,64)==0 && std::memcmp(entire,clone,224)==0 && std::memcmp(entire+272,clone+272,512)==0,"all forward and unrelated consumer bytes unchanged");
 expect(match(captureCamera,clone+160),"actual whole-buffer prepared slice passes strict certificate");
 using namespace laser_scene_tone_test;
 auto replay=[&](unsigned negative){auto p=std::make_unique<FlatRuntimePrefix>();p->frame=9201;auto source=draw(token(0x1000),23,token(0x2000),token(0x2001),0xBFE51414CC3024B4ull,0xDB79AE788E049DFDull);source.key.kind=kFlatContractPool;source.supported=true;laser_scene_tone_test::camera(source,9201,1);std::memcpy(source.camera,c,sizeof(c));source.key.cameraHash=flatCameraHash(source.camera);flatRuntimeObserve(*p,source);
 if(negative==7){auto other=source;other.key.color=token(0x1010);other.camera[80]^=1;other.key.cameraHash=flatCameraHash(other.camera);flatRuntimeObserve(*p,other);}
 auto gap=draw(token(0x9000),28,nullptr,nullptr,1,2);flatRuntimeObserve(*p,gap);
 auto h=draw(token(0x4000),26,token(0x2000),token(0x2001),0x7E38A6AA1269C901ull,0x7CECABDE34FFBE9Eull);h.deferredHdrVerified=true;h.key.srvResource[0]=source.key.color;h.key.srvView[0]=token(0x1002);std::memcpy(h.camera,c,sizeof(c));h.key.cameraHash=flatCameraHash(h.camera);auto& projection=h.key.projection[1];projection.resource=token(0x6000);projection.width=784;projection.copied=112;projection.hash=123;projection.status=FlatProjectionStatus::Available;projection.writeEpoch=9201;projection.writeSeq=2;
 if(negative==13){h.deferredInverseRepaired=true;h.deferredHdrVerified=false;}if(negative==14)h.deferredInverseRepaired=true;
 if(negative==1)projection.writeEpoch=9200;if(negative==2)projection.writeSeq=4;if(negative==3)projection.copied=111;if(negative==4)projection.hash=0;if(negative==5)h.key.srvResource[0]=token(0xBAD);if(negative==6)h.camera[80]^=1;if(negative==8)h.key.dsv=token(0xBAD);if(negative==9)h.deferredHdrVerified=false;if(negative==10)h.key.camera=h.camera;if(negative==11)p->uncertain=true;if(negative==12)projection.writeSeq=0;
 auto event=flatTraceEventFromDraw(h,false);auto decoded=flatTraceEventToDraw(event);flatRuntimeObserve(*p,decoded);auto* t=flatRuntimeTarget(*p,h.key.color);
 if(!negative || negative==14){expect(t->tone.deferredInverseRepaired==(negative==14),"repaired execution marker roundtrips without replacing original metadata");expect(!(event.flags&kFlatTraceHasCamera)&&(event.flags&kFlatTraceDeferredHdrVerified),"trace has typed donor and no fabricated H camera");expect(t->tone.key.b1==nullptr&&t->tone.sceneCameraB1==source.key.b1&&t->tone.first==3&&t->tone.firstWriteSeq==2,"actual H draw/write provenance distinct from donor");expect(flat_mono_detail::cameraCurrent(t->tone,9201),"typed current witness consumed");auto image=draw(token(0x5000),9,token(0x2000),token(0x2001),0xCFA91824129ECBBCull,0xFCFAD73924BF45B9ull);image.imageSourceCameraIndependentVerified=true;flatRuntimeObserve(*p,image);
 auto copy=draw(h.key.color,26,nullptr,nullptr,0xCFA91824129ECBBCull,0xDFCBA0EC70B03C9Bull);copy.key.depthWidth=copy.key.depthHeight=0;copy.key.srvResource[0]=image.key.color;copy.key.srvView[0]=token(0x5002);copy.hdrCopyVerified=true;flatRuntimeObserve(*p,copy);expect(p->imageCopiesAccepted==1,"verified image handoff consumes typed H witness");
 auto later=h;later.deferredHdrVerified=false;later.deferredInverseRepaired=false;laser_scene_tone_test::camera(later,9201,p->sequence+1,.0675f);flatRuntimeObserve(*p,later);expect(t->hdrBad,"later actual conflicting camera vetoes typed witness");return true;}
 if(t->hdrCamera)std::printf("deferred negative case %u admitted\n",negative);return !t->hdrCamera;};
 expect(replay(0),"typed actual H witness admitted");expect(replay(14),"repaired typed execution roundtrip retains original provenance");for(unsigned n=1;n<=13;++n)expect(replay(n),"stale/later/short/empty/changed/ambiguous/foreign malformed proof refused");
 // Loading -> stand-down -> cold Probe -> actual selected handoff. A strict
 // matrix proof alone is not the wake predicate; the ordinary selector is.
 auto coldProbe=[&](unsigned negative){
     FlatStandDown sleep;
     sleep.frameEnded(FlatFrameSeen::Structural,FlatMonoReason::NoScene,true,1000);
     expect(sleep.frameEnded(FlatFrameSeen::Structural,FlatMonoReason::NoScene,true,6001)==FlatStandDownEvent::Entered,"loading enters stand-down");
     expect(sleep.nextFrame(sleep.nextProbeMs)==FlatWork::Probe,"existing cadence opens cold probe");
     auto p=std::make_unique<FlatRuntimePrefix>();p->frame=9001;p->output=token(0x9000);p->width=1920;p->height=1200;p->format=28;
     auto pool=draw(token(0x1000),23,token(0x2000),token(0x2001),0xBFE51414CC3024B4ull,0xDB79AE788E049DFDull);
     pool.supported=true;pool.key.kind=kFlatContractPool;laser_scene_tone_test::camera(pool,p->frame,1);std::memcpy(pool.camera,c,sizeof(c));pool.key.cameraHash=flatCameraHash(pool.camera);
     for(unsigned i=0;i<8;++i)flatRuntimeObserve(*p,pool);
     if(negative==4){auto other=pool;other.key.color=token(0x1010);other.camera[80]^=1;other.key.cameraHash=flatCameraHash(other.camera);flatRuntimeObserve(*p,other);}
     auto h=draw(token(0x4000),26,pool.key.depth,pool.key.dsv,0x7E38A6AA1269C901ull,0x7CECABDE34FFBE9Eull);
     h.key.kind=kFlatContractScreen;h.key.srvResource[0]=pool.key.color;h.key.srvView[0]=token(0x1002);
     std::memcpy(h.camera,c,sizeof(c));h.key.cameraHash=pool.key.cameraHash;
     auto& meta=h.key.projection[1];meta.resource=token(0x6000);meta.width=784;meta.copied=112;meta.hash=123;meta.status=FlatProjectionStatus::Available;meta.writeEpoch=p->frame;meta.writeSeq=p->sequence;
     if(negative==1)meta.writeEpoch=p->frame-1; // stale or creation-only shadow
     if(negative==2)meta.copied=111;
     if(negative==3)meta.status=FlatProjectionStatus::InvalidWrite; // mapped/unavailable original
     if(negative==6)p->uncertain=true;
     if(negative==8)meta.writeSeq=0; // creation bytes have no observed write
     if(negative==9)meta.writeSeq=p->sequence+2;
     if(negative==10)meta.width=271;
     float inverseBad[7][4];std::memcpy(inverseBad,rows,sizeof(rows));inverseBad[4][0]+=.01f;
     const bool originalCurrent=meta.status==FlatProjectionStatus::Available && meta.copied==112 && meta.writeEpoch==p->frame && meta.writeSeq && meta.writeSeq<=p->sequence+1;
     h.deferredHdrVerified=originalCurrent && flatRuntimeDeferredDonor(*p,h.key,p->sequence+1) &&
         match(c,negative==7?static_cast<const void*>(inverseBad):static_cast<const void*>(rows));
     for(unsigned i=0;i<8;++i){flatRuntimeObserve(*p,h);h.deferredHdrVerified=false;}
     FlatMonoFrame selected{};
     if(negative!=5){
         auto tone=draw(token(0x5000),27,nullptr,nullptr,flat_mono_detail::kToneVs,flat_mono_detail::kTonePs);
         tone.key.kind=kFlatContractScreen;tone.key.depthWidth=tone.key.depthHeight=0;tone.key.srvResource[1]=h.key.color;tone.key.srvView[1]=token(0x4002);flatRuntimeObserve(*p,tone);
         auto copy=draw(p->output,28,nullptr,nullptr,flat_mono_detail::kCopyVs,flat_mono_detail::kCopyPs);
         copy.key.kind=kFlatContractOutput;copy.key.depthWidth=copy.key.depthHeight=0;copy.key.srvResource[0]=tone.key.color;copy.key.srvView[0]=token(0x5002);selected=flatRuntimeObserve(*p,copy);
     }
     const auto wake=sleep.frameEnded(flatFrameSeenFor(selected.selected(),selected.reason),selected.reason,true,sleep.nextProbeMs+16);
     if(!negative && !selected.selected())std::printf("cold probe handoff refused: %s\n",flatMonoReasonName(selected.reason));
     if(!negative)expect(selected.selected() && wake==FlatStandDownEvent::Resumed && sleep.nextFrame(8000)==FlatWork::Full,"cold original proof plus real handoff automatically wakes Full");
     else expect(!selected.selected() && wake!=FlatStandDownEvent::Resumed && sleep.standing,"stale partial mapped ambiguous foreign inverse creation later short or no-consumer probe cannot wake");
 };
 coldProbe(0);for(unsigned n=1;n<=10;++n)coldProbe(n);
 std::printf("flat deferred camera: %u failure(s)\n",failures);return failures;
}
}
