// Run the actual four production entry wrappers against owned synthetic
// CameraState memory. No game, resolver, installer or runtime is consulted.
#include "../../src/d3d11/native_preview_camera.cpp"
#include <array>
#include <thread>
#include <limits>
#include <stdexcept>
#include <cstdio>
#include <cmath>

namespace edvr {
unsigned unexpected=0;
unsigned layoutQueries=0;
Log& Log::get(){static Log l;return l;} Log::~Log()=default;
void Log::note(const char*,...){}
void CodeHook::uninstall(){}
bool CodeHook::install(void*,void*,void**,const char*,Prepare,void*){++unexpected;return false;}
uintptr_t gameRva(uint32_t) noexcept{++unexpected;return 0;}
bool gameRvasFound(const uint32_t*,size_t,uint32_t*) noexcept{++unexpected;return false;}
bool gamePinMatches(uint32_t,const uint8_t*,const uint8_t*,size_t) noexcept{++unexpected;return false;}
const char* gameBuildText() noexcept{++unexpected;return "test";}
bool gameBuildKnown() noexcept{++layoutQueries;return false;}
}
using namespace edvr::previewcam;
unsigned checks=0,failures=0;
void check(bool ok,const char* name){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",name);}}
float separation(const Pose& a,const Pose& b){
    const float dx=a.v[12]-b.v[12],dy=a.v[13]-b.v[13],dz=a.v[14]-b.v[14];
    return std::sqrt(dx*dx+dy*dy+dz*dz);
}
Pose identityPose(){Pose p;p.v[0]=p.v[5]=p.v[10]=1;return p;}
struct Fixture {
    std::array<uint8_t,65536> bytes{};
    Game game;Memory memory{read,write,this};Engine engine;
    unsigned hmdN=0,previewN=0,finalN=0,writes=0,managerN=0;
    bool hmdReady=true,overlapBeforeCommit=false,overlapAfterCommit=false;
    uintptr_t denied=0;
    Pose local,previewPose,world;
    uintptr_t at(size_t off) const{return reinterpret_cast<uintptr_t>(bytes.data()+off);}
    template<class T>void put(size_t off,const T& value){std::memcpy(bytes.data()+off,&value,sizeof(value));}
    template<class T>T load(size_t off)const{T v;std::memcpy(&v,bytes.data()+off,sizeof(v));return v;}
    static bool read(void* ctx,uintptr_t at,void* out,size_t n) noexcept {
        auto& f=*static_cast<Fixture*>(ctx);const uintptr_t start=f.at(0);
        if(at==f.denied||at<start||n>f.bytes.size()||at-start>f.bytes.size()-n)return false;
        std::memcpy(out,reinterpret_cast<const void*>(at),n);
        // Deterministic adversarial admission immediately before the core's
        // commit reservation: finalizer's last output read has completed.
        if(f.overlapBeforeCommit&&at==f.at(0x30B0)) {f.engine.enter();f.engine.leave();}
        return true;
    }
    static bool write(void* ctx,uintptr_t at,const void* in,size_t n) noexcept {
        auto& f=*static_cast<Fixture*>(ctx);
        if(f.overlapAfterCommit){f.engine.enter();f.engine.leave();}
        ++f.writes;
        return edvr::previewWrite(nullptr,at,in,n);
    }
    Fixture() {
        for(unsigned i=0;i<AddressCount;++i)game.at[i]=at(0x8000+i*0x80);
        put(0x3000,game.at[Child]);put(0x3010,game.at[Child10]);put(0x3030,at(0x4000));
        put<uint8_t>(0x3038,1);put(0x3040,at(0x5000));put<uint64_t>(0x3058,2);put(0x3060,at(0x6000));
        put<uint32_t>(0x3068,0);put(0x4000,game.at[Nested]);put(0x4020,at(0x7000));put(0x4028,at(0x3000));
        put<uint64_t>(0x7000,8);std::memcpy(bytes.data()+0x7014,"Location",8);
        put(0x6000,at(0x1020));put(0x6008,at(0x2060));
        put(0x1000,game.at[HmdObject]);put(0x1020,game.at[HmdIface]);put<uint8_t>(0x101C,1);put(0x1140,at(0x3000));
        put(0x2000,game.at[PreviewObject]);put(0x2060,game.at[PreviewIface]);put(0x2180,at(0x3000));
        put<uint32_t>(0x239C,0);
        put(0x8000+HmdIface*0x80+0x20,game.at[Weight]);put(0x8000+HmdIface*0x80+0x28,game.at[State]);
        put(0x8000+PreviewIface*0x80+0x20,game.at[ConstantWeight]);put(0x8000+PreviewIface*0x80+0x28,game.at[State]);
        const Pose base=identityPose();put(0x5040,base);
        local.v[1]=1;local.v[4]=-1;local.v[10]=1;local.v[12]=1;local.v[13]=2;local.v[14]=3;
        previewPose.v[2]=-1;previewPose.v[5]=1;previewPose.v[8]=1;
        previewPose.v[12]=100;previewPose.v[13]=200;previewPose.v[14]=300;
        world=identityPose();world.v[12]=-621;world.v[13]=-27;world.v[14]=-79;
        // Padding is deliberately not homogeneous and includes a NaN payload.
        uint32_t pad=0x7FC12345;std::memcpy(&world.v[15],&pad,4);
        local.v[3]=std::numeric_limits<float>::quiet_NaN();previewPose.v[7]=local.v[3];
        put(0x24D0,previewPose);put<float>(0x1030,0.875f);put<uint32_t>(0x1040,0x12345678);
        engine.enable(true);
    }
    uintptr_t hmdOriginal(){++hmdN;put(0x1070,hmdReady?world:load<Pose>(0x5040));put(0x10F0,local);put(0x9000,load<Pose>(0x1070));return 0x11111111;}
    uintptr_t previewOriginal(){++previewN;put(0x20B0,load<uint32_t>(0x239C)==2?load<Pose>(0x5040):load<Pose>(0x24D0));return 0x22222222;}
    uintptr_t finalOriginal(){++finalN;std::memcpy(bytes.data()+0x3070,bytes.data()+0x1030,0x110);return 0x33333333;}
    void h(){check(hmd(engine,memory,game,at(0x1000),[&]{return hmdOriginal();})==0x11111111,"HMD return preserved");}
    void p(){check(preview(engine,memory,game,at(0x21B0),[&]{return previewOriginal();})==0x22222222,"preview return preserved");}
    void f(){check(finalizer(engine,memory,game,at(0x3000),[&]{return finalOriginal();})==0x33333333,"finalizer return preserved");}
    bool eyeComposed(const Pose& eye) const {
        const Pose composed=load<Pose>(0x30B0);
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c)
            if(eye.v[r*4+c]!=composed.v[r*4+c])return false;
        return true;
    }
    static bool farFrom(const Pose& a,const Pose& b) {
        const float dx=a.v[12]-b.v[12],dy=a.v[13]-b.v[13],dz=a.v[14]-b.v[14];
        return dx*dx+dy*dy+dz*dz>50.f*50.f;
    }
    bool expected()const {
        const Pose p=load<Pose>(0x30B0),primary=load<Pose>(0x1070);
        const float want[12]={0,0,-1,0,1,0,1,0,0,100,200,300};
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c) {
            if(p.v[r*4+c]!=want[r*3+c])return false;
            if(primary.v[r*4+c]!=want[r*3+c])return false;
        }
        return true;
    }
    bool untouchedOutsideXYZ()const {
        for(unsigned i=0;i<0x110;++i) {
            const bool xyz=i>=0x40&&i<0x80&&((i-0x40)%16)<12;
            if(!xyz&&bytes[0x3070+i]!=bytes[0x1030+i])return false;
        }
        return true;
    }
};
Fixture* live=nullptr;
uintptr_t actualHmd(uintptr_t x){check(x==live->at(0x1000),"actual HMD object ABI");return live->hmdOriginal();}
uintptr_t actualPreview(uintptr_t c,uintptr_t s){check(c==0xABCD&&s==live->at(0x21B0),"actual callback context/subscriber ABI");return live->previewOriginal();}
uintptr_t actualFinal(uintptr_t c){check(c==live->at(0x3000),"actual child ABI");return live->finalOriginal();}
uintptr_t actualManager(uintptr_t object) {
    check(object==0xDCBA,"actual manager ABI");++live->managerN;
    edvr::previewSourceHook(0xABCD,live->at(0x21B0));edvr::previewHmdHook(live->at(0x1000));edvr::previewFinalizerHook(live->at(0x3000));
    return 0x44444444;
}
int main(int argc,char** argv) {
    if(argc!=2||std::strcmp(argv[1],"--self-test"))return 2;
    for(unsigned order=0;order<2;++order) {
        Fixture a;const auto result=manager(a.engine,[&]{if(order){a.p();a.h();}else{a.h();a.p();}a.f();return uintptr_t(99);});
        check(result==99&&a.hmdN==1&&a.previewN==1&&a.finalN==1,"both producer orders forward each original once");
        check(a.writes==2&&a.expected(),"preview pose without the local headset is the published eye");
        const Pose rawEye=a.load<Pose>(0x9000);
        check(applyEyeHead(a.engine,a.memory,a.at(0x9000)),"published head replaces the stale HMD copy the eyes multiply");
        const Pose eye=a.load<Pose>(0x9000);
        check(a.eyeComposed(eye)&&a.farFrom(rawEye,eye),"scene eye head is the preview pose, far from the raw HMD");
        if(order==0){
            std::printf("stable preview eye (%.6f, %.6f, %.6f) raw HMD (%.6f, %.6f, %.6f) distance %.3f m\n",
                eye.v[12],eye.v[13],eye.v[14],rawEye.v[12],rawEye.v[13],rawEye.v[14],separation(eye,rawEye));
            std::printf("original-once: hmd %u preview %u finalizer %u\n",a.hmdN,a.previewN,a.finalN);
            Pose menu{};menu.v[0]=menu.v[5]=menu.v[10]=1;menu.v[12]=0.012f;menu.v[13]=0.002f;menu.v[14]=0.005f;
            a.put(0x9200,menu);
            check(applyEyeHead(a.engine,a.memory,a.at(0x9200))&&equal(a.load<Pose>(0x9200),eye),
                "head-locked menu camera near the origin takes the preview pose");
            std::printf("menu camera: origin replaced with preview (%.6f, %.6f, %.6f)\n",
                a.load<Pose>(0x9200).v[12],a.load<Pose>(0x9200).v[13],a.load<Pose>(0x9200).v[14]);
        }
        check(a.untouchedOutsideXYZ(),"all padding FOV secondary and non-primary state bytes unchanged");
        check(!a.engine.current(),"manager generation closes after dispatch");
        Pose elsewhere=rawEye;elsewhere.v[12]+=80.f;a.put(0x9100,elsewhere);
        check(!applyEyeHead(a.engine,a.memory,a.at(0x9100))&&equal(a.load<Pose>(0x9100),elsewhere),
            "a head tens of metres from the HMD primary is not the eye copy");
    }
    {
        Fixture moved;moved.local.v[12]=9.f;
        manager(moved.engine,[&]{moved.h();moved.p();moved.f();return uintptr_t(0);});
        check(applyEyeHead(moved.engine,moved.memory,moved.at(0x9000)),"a different local headset pose still publishes");
        Fixture still;
        manager(still.engine,[&]{still.h();still.p();still.f();return uintptr_t(0);});
        check(applyEyeHead(still.engine,still.memory,still.at(0x9000)),"the original local headset pose publishes too");
        const Pose movedEye=moved.load<Pose>(0x9000),stillEye=still.load<Pose>(0x9000);
        check(equal(movedEye,stillEye)&&movedEye.v[12]==100.f&&movedEye.v[13]==200.f&&movedEye.v[14]==300.f,
            "local headset motion leaves the published eye on the preview pose");
        std::printf("local headset motion: published eye (%.6f, %.6f, %.6f) vs still (%.6f, %.6f, %.6f)\n",
            movedEye.v[12],movedEye.v[13],movedEye.v[14],stillEye.v[12],stillEye.v[13],stillEye.v[14]);
    }
    {
        // The 10:19 flight wrote the composed pose to the Location stage only.
        // That store is child+0xB0. It does not publish a head, so the eye
        // record copied from the HMD stays there and the eye replacement refuses.
        Fixture only;only.hmdOriginal();
        const Pose raw=only.load<Pose>(0x9000);
        const Pose changed=compose(only.local,only.previewPose,only.load<Pose>(0x30B0));
        check(only.memory.write(only.memory.context,only.at(0x30B0),&changed,sizeof(changed)),
            "Location stage output is the only pose store");
        const Pose stage=only.load<Pose>(0x30B0);
        const bool refused=!applyEyeHead(only.engine,only.memory,only.at(0x9000));
        const Pose left=only.load<Pose>(0x9000);
        check(only.writes==1&&only.farFrom(left,stage)&&refused&&equal(left,raw)&&stage.v[12]==changed.v[12],
            "changing only the Location stage output leaves the eye-origin record on the HMD");
        std::printf("Location-only miss: stage (%.6f, %.6f, %.6f) eye-origin left on HMD (%.6f, %.6f, %.6f) distance %.3f m refused %d\n",
            stage.v[12],stage.v[13],stage.v[14],left.v[12],left.v[13],left.v[14],separation(stage,left),refused?1:0);
    }
    {
        Fixture a;a.hmdOriginal();
        const Pose rawEye=a.load<Pose>(0x9000);
        const Pose composed=compose(a.local,a.previewPose,Pose{});
        uint8_t* body=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        check(body!=nullptr,"eye original body allocated");
        if(body){
            body[0]=0xB8;const uint32_t marker=0x51;std::memcpy(body+1,&marker,4);body[5]=0xC3;
            DWORD old=0;VirtualProtect(body,4096,PAGE_EXECUTE_READ,&old);
            FlushInstructionCache(GetCurrentProcess(),body,4096);
        }
        uint8_t* stub=body?edvr::eyeStub(reinterpret_cast<uintptr_t>(body)):nullptr;
        const bool sealed=stub&&edvr::eyePrepare(body,nullptr);
        check(sealed,"shipped eye stub sealed with its trampoline");
        uintptr_t ran=0;
        if(sealed){
            edvr::g_previewEngine.publishEye(1,composed,rawEye);
            using EyeFn=uintptr_t(__fastcall*)(uintptr_t,uintptr_t);
            ran=reinterpret_cast<EyeFn>(stub)(0,a.at(0x9000));
        }
        const Pose eye=a.load<Pose>(0x9000);
        check(ran==0x51&&equal(eye,composed)&&a.farFrom(rawEye,eye),
            "shipped eye stub stores the composed pose and the original body still runs");
        std::printf("eye stub: composed (%.6f, %.6f, %.6f) raw HMD (%.6f, %.6f, %.6f) distance %.3f m original %llu\n",
            eye.v[12],eye.v[13],eye.v[14],rawEye.v[12],rawEye.v[13],rawEye.v[14],separation(eye,rawEye),
            static_cast<unsigned long long>(ran));
        edvr::g_previewEngine.enter();edvr::g_previewEngine.leave();
        edvr::g_previewOriginal[4].store(nullptr,std::memory_order_release);
        if(stub)VirtualFree(stub,0,MEM_RELEASE);
        if(body)VirtualFree(body,0,MEM_RELEASE);
    }
    {
        Fixture a;manager(a.engine,[&]{std::thread h([&]{hmd(a.engine,a.memory,a.game,a.at(0x1000),[&]{return a.hmdOriginal();});});
            std::thread p([&]{preview(a.engine,a.memory,a.game,a.at(0x21B0),[&]{return a.previewOriginal();});});h.join();p.join();a.f();return uintptr_t(0);});
        check(a.expected()&&a.writes==2,"parallel producer publications join without task serialization");
    }
    unsigned refusalUnchanged=0;
    for(unsigned mode=0;mode<19;++mode) {
        Fixture a;
        manager(a.engine,[&]{
            if(mode==2)a.hmdReady=false;
            if(mode==3)a.put<uint32_t>(0x239C,2);
            if(mode==4)a.local.v[5]=std::numeric_limits<float>::quiet_NaN();
            if(mode==5){auto p=a.previewPose;p.v[12]=std::numeric_limits<float>::infinity();a.put(0x24D0,p);}
            if(mode==6)a.put(0x6008,a.at(0x1020));
            if(mode==7)a.put<uint8_t>(0x3038,0);
            if(mode==8)a.put(0x4030,a.at(0x3000));
            if(mode==9)a.denied=a.at(0x1140);
            if(mode==15)a.world.v[12]=std::numeric_limits<float>::quiet_NaN();
            if(mode!=0)a.h();if(mode!=1)a.p();
            if(mode==10){a.put<uint32_t>(0x3068,1);a.h();a.put<uint32_t>(0x3068,0);}
            if(mode==11){a.put<uint32_t>(0x3068,1);a.p();a.put<uint32_t>(0x3068,0);}
            if(mode==12){a.engine.enable(false);a.engine.enable(true);}
            if(mode==13)a.overlapBeforeCommit=true;
            if(mode==14){a.engine.enter();a.engine.leave();}
            if(mode==16){a.put(0x1140,a.at(0x3200));a.h();a.put(0x1140,a.at(0x3000));}
            if(mode==17){a.denied=a.at(0x2180);a.p();a.denied=0;}
            if(mode==18)a.engine.enable(false);
            a.f();return uintptr_t(0);
        });
        const Pose eyeBefore=a.load<Pose>(0x9000);
        check(a.writes==0&&equal(a.load<Pose>(0x30B0),a.load<Pose>(0x1070)),"skipped/failed/invalid/stale/overlapped evaluation leaves original pose");
        const bool refused=!applyEyeHead(a.engine,a.memory,a.at(0x9000));
        const Pose eyeAfter=a.load<Pose>(0x9000);
        check(refused&&equal(eyeAfter,eyeBefore),"refusal leaves the eye-origin record unchanged");
        if(refused&&equal(eyeAfter,eyeBefore)&&a.writes==0)++refusalUnchanged;
    }
    std::printf("refusal: %u modes left the eye-origin record unchanged\n",refusalUnchanged);
    {
        Fixture a;manager(a.engine,[&]{a.p();return uintptr_t(0);});
        manager(a.engine,[&]{a.h();a.f();return uintptr_t(0);});
        check(!a.writes,"old unconsumed preview cannot pair with next evaluation HMD");
        manager(a.engine,[&]{a.h();a.p();a.f();a.f();return uintptr_t(0);});
        check(a.writes==2,"duplicate finalizer consumes a completion pair only once");
        manager(a.engine,[&]{a.h();a.p();a.put<uint32_t>(0x3068,1);a.f();a.put<uint32_t>(0x3068,0);a.f();return uintptr_t(0);});
        check(a.writes==2,"unmatched finalizer consumes completion before roster re-entry");
    }
    {
        Fixture a;manager(a.engine,[&]{a.h();a.p();std::thread other([&]{manager(a.engine,[]{return uintptr_t(0);});});other.join();a.f();return uintptr_t(0);});
        check(!a.writes,"concurrent manager poisons active evaluation");
        manager(a.engine,[&]{a.h();a.p();a.f();return uintptr_t(0);});
        check(a.writes==2,"new exclusive evaluation recovers after overlap");
    }
    {
        Fixture a;try{manager(a.engine,[&]()->uintptr_t{a.h();throw std::runtime_error("fixture");});}catch(const std::runtime_error&){}
        check(!a.engine.current(),"manager RAII closes generation on exception");
        manager(a.engine,[&]{a.h();a.p();a.f();return uintptr_t(0);});check(a.writes==2,"exception cannot leak old completion into recovery");
    }
    {
        Fixture a;Engine::Completion late;
        manager(a.engine,[&]{late=a.engine.begin(a.at(0x3000),0,a.at(0x1000));a.f();return uintptr_t(0);});
        manager(a.engine,[&]{a.engine.finish(late,nullptr);a.h();a.p();a.f();return uintptr_t(0);});
        check(a.writes==2,"unfinished producer refuses; old completion cannot damage a new evaluation");
    }
    {
        Fixture a;a.overlapAfterCommit=true;
        manager(a.engine,[&]{a.h();a.p();a.f();check(!a.engine.current(),"manager admission after reservation poisons subsequent work");return uintptr_t(0);});
        check(a.writes==2&&a.expected(),"reservation commits owned finalizer write before later overlap; not an atomic-pose claim");
    }
    {
        Engine e;e.enable(true);Evaluation scope(e);
        for(uintptr_t i=1;i<=16;++i)check(e.begin(i,0).epoch!=0,"bounded child slot admitted");
        check(!e.begin(17,0).epoch&&!e.current(),"slot overflow refuses whole evaluation without eviction");
    }
    {
        Engine e;e.enable(true);Evaluation scope(e);Engine::Completion other;
        const auto first=e.beginWithClaim(1,0,101,[&]{
            e.invalidateProducer(202,0);other=e.begin(2,0,202);
        });
        check(first.epoch&&other.epoch&&e.current(),"different child producer between owner claim and phase publication does not poison");
        e.finish(first,nullptr);e.finish(other,nullptr);
        check(e.current()!=0,"parallel independent owner claims complete normally");
    }
    {
        Engine e;e.enable(true);Evaluation scope(e);
        e.beginWithClaim(1,0,101,[&]{e.invalidateProducer(101,0);});
        check(!e.current(),"same-owner duplicate is detected even before phase publication");
    }
    {
        Engine e;e.enable(true);Evaluation scope(e);Sample h,p;
        e.beginWithClaim(1,0,101,[&]{check(!e.take(e.current(),1,h,p),"finalizer refuses partially published owner claim");});
        check(!e.current(),"partial owner publication poisons an out-of-order finalizer");
    }
    {
        Fixture a;live=&a;edvr::g_previewGame=a.game;edvr::g_previewEngine.enable(true);
        edvr::g_previewOriginal[0].store(reinterpret_cast<void*>(&actualHmd));edvr::g_previewOriginal[1].store(reinterpret_cast<void*>(&actualPreview));
        edvr::g_previewOriginal[2].store(reinterpret_cast<void*>(&actualFinal));edvr::g_previewOriginal[3].store(reinterpret_cast<void*>(&actualManager));
        check(edvr::previewManagerHook(0xDCBA)==0x44444444,"actual production manager return preserved");
        const Pose rawEye=a.load<Pose>(0x9000);
        check(applyEyeHead(edvr::g_previewEngine,a.memory,a.at(0x9000))&&a.eyeComposed(a.load<Pose>(0x9000))&&a.farFrom(rawEye,a.load<Pose>(0x9000)),
            "production finalizer publishes the eye head the builder multiplies");
        check(a.managerN==1&&a.hmdN==1&&a.previewN==1&&a.finalN==1&&a.expected()&&a.untouchedOutsideXYZ(),
            "actual four hook wrappers forward exactly once and change only XYZ");
        check(edvr::unexpected==0,"actual wrappers make no resolver runtime or installation calls");
        edvr::g_previewEngine.enter();edvr::nativePreviewCameraStop();
        check(!edvr::g_previewEngine.current(),"runtime shutdown poisons the live preview evaluation");edvr::g_previewEngine.leave();
    }
    check(!edvr::previewInstall()&&edvr::layoutQueries==1&&edvr::unexpected==1,
        "unknown layout refuses before module lookup, resolver or hook installation");
    std::printf("native_preview_camera_test: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
