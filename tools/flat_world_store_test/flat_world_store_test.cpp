#include "flat_world_store_tests.h"
#include "../../src/d3d11/explorer_cam_core.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#if !defined(near) || !defined(far)
#error Keep Windows SDK macros intact for header integration coverage.
#endif

namespace {
int checks=0,failures=0;
void check(bool ok,const char* reason) {
    ++checks;
    if(!ok){++failures;std::printf("FAIL: %s\n",reason);}
}
void sourceValidityRegression(){
    using namespace edvr;
    alignas(8) uint8_t record[0x300]{},node[0x20]{},bounds[0x40]{},builder[0x100]{},store[0x80]{},matrix[0x40]{};
    auto put=[](uint8_t* p,size_t at,const void* v,size_t n){std::memcpy(p+at,v,n);};
    uintptr_t np=reinterpret_cast<uintptr_t>(node),bp=reinterpret_cast<uintptr_t>(bounds),pp=reinterpret_cast<uintptr_t>(builder);
    uintptr_t sp=reinterpret_cast<uintptr_t>(store),mp=reinterpret_cast<uintptr_t>(matrix);
    put(record,0x18,&np,8);put(node,8,&bp,8);put(record,0x290,&pp,8);put(record,0x190,&sp,8);put(store,0x50,&mp,8);
    const float q[4]={0,0,0,1},p[4]={500,0,0,0},local[3]={600,0,0},good=6,bad=-1;
    put(builder,0x10,q,16);put(builder,0x20,p,16);put(bounds,0x20,local,12);put(bounds,0x2c,&good,4);put(record,0x280,&bad,4);
    float a[15]{},b[16]{};a[0]=a[5]=a[10]=1;b[0]=b[5]=b[10]=b[15]=1;put(store,0,a,sizeof(a));put(matrix,0,b,sizeof(b));
    FlatWorldWitnessInput before{},after{};const uintptr_t rec=reinterpret_cast<uintptr_t>(record);
    check(flatWorldStoreRead(0,rec,99,&before)==0&&before.source==4&&!(before.validMask&kRadius)&&
        (before.validMask&kPoseCenter)&&(before.sourceValidMask&kStoreCenter)&&!(before.sourceValidMask&kPoseCenter),
        "invalid primary pose radius preserves independently validated store alternative");
    check(before.bucket==0&&!before.ownerReadValid&&before.radius==0&&before.sourceRadius[0]==-1&&
        before.sourceRadius[1]==6&&before.sourceRadius[2]==-1,
        "reader retains already-read independent radii and unavailable bucket owner validity");
    const float moved[3]={660,0,0};put(bounds,0x20,moved,12);
    check(flatWorldStoreRead(0,rec,100,&after)==0,"second independent-source observation");
    FlatWorldWitnessCore core;core.observe(before);core.observe(after);
    const float previous[3]={80,0,0},now[3]={20,0,0};core.event(100,100,now,previous,16,true,true,false);
    FlatWorldWitnessReport report;check(core.complete(103,report)&&!report.edgeValid[2]&&
        report.alternativeCount[0]==0&&report.alternativeCount[1]==1&&report.alternativeMedian[1][0]==60,
        "reader to collector retains store displacement despite invalid primary radius");
    check(report.rowCount==1&&report.rows[0].alternativeRadius[0][0]==-1&&report.rows[0].alternativeRadius[1][0]==-1&&
        report.rows[0].alternativeRadius[0][1]==6&&report.rows[0].alternativeRadius[1][1]==6&&
        report.rows[0].radius[2]==0&&report.rows[0].radius[3]==0&&!report.rows[0].ownerReadValid[2],
        "reader to report retains invalid primary radius pair and valid store radius pair");
    alignas(8) uint8_t bucket[0x20]{};const uintptr_t bucketPtr=reinterpret_cast<uintptr_t>(bucket);
    check(flatWorldStoreRead(bucketPtr,rec,100,&after)==0&&after.bucket==bucketPtr&&after.ownerReadValid&&after.owner==0,
        "readable null owner differs explicitly from unavailable owner");
    char name[96]{};std::memset(name,'A',48);uintptr_t text=reinterpret_cast<uintptr_t>(name);put(node,0,&text,8);
    check(flatWorldStoreRead(0,rec,101,&after)==0&&(after.validMask&kName)&&std::strlen(after.name)==48,
        "valid 48-character metadata name is retained without truncation");
}
void entryFrameRegression(){
    using namespace edvr;
    namespace rt=flat_world_witness_runtime;
    std::atomic<bool> active{true};std::atomic<uint32_t> published{700};
    rt::active.store(true);rt::currentFrame.store(700);rt::attempts.store(0);
    for(auto& c:rt::claims)c.store(0);
    uintptr_t record=0x10000;while(flat_world_witness_detail::hash(record)&63u)++record;
    unsigned reads=0;uint32_t received=0;
    auto forward=[&](uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t) noexcept -> uintptr_t {
        rt::currentFrame.store(701);published.store(701);return 0x1234;
    };
    const uintptr_t result=flatWorldStoreStampedPostForward(0,0,0,0,record,false,active,published,forward,
        [](uintptr_t,uintptr_t) noexcept {},[&](uintptr_t,uintptr_t rec,uint32_t frame) noexcept {
            received=frame;if(flatWorldWitnessSelect(rec,frame))++reads;
        });
    check(result==0x1234&&received==700&&reads==0,"forward crossing Present retains entry stamp and selects no extra read");
    rt::active.store(false);
    std::atomic<bool> wanted{false};std::atomic<uint32_t> frame{0};unsigned installAttempts=0;
    for(uint32_t f=1;f<=1000;++f)if(flatWorldStorePublish(true,f,wanted,frame))++installAttempts;
    check(installAttempts==1&&frame.load()==1000,"flat Present publication requests only one install per activation");
    flatWorldStorePublish(false,1001,wanted,frame);
    if(flatWorldStorePublish(true,1002,wanted,frame))++installAttempts;
    check(installAttempts==2,"a later flat activation can retry a transient install failure");
}
}
int main(int argc,char** argv) {
    if(argc==2&&std::strcmp(argv[1],"--dry-run")==0)return 0;
    if(argc!=2||std::strcmp(argv[1],"--self-test")!=0)return 2;
    using namespace edvr;
    flatOriginCaptureInitialize(true); // this rig exercises the diagnostic store observer
    const uint8_t* pin=sealed::kStorePrologue;
    const uint32_t stamp=ecm::kExpectedTimestamp,size=ecm::kExpectedImageSize;
    check(sealed::storeInstallDecision(true,stamp,size,stamp,size,pin,pin,16)==sealed::StoreInstall::kInstall,
          "flat world store: only pinned build and prologue admit hook");
    check(sealed::storeInstallDecision(true,stamp+1,size,stamp,size,pin,pin,16)==sealed::StoreInstall::kWrongIdentity&&
          sealed::storeInstallDecision(false,stamp,size,stamp,size,pin,pin,16)!=sealed::StoreInstall::kInstall,
          "flat world store: wrong or unreadable PE refuses both consumers");
    uint8_t wrong[16]{};std::memcpy(wrong,pin,sizeof(wrong));wrong[4]^=1;
    check(sealed::storeInstallDecision(true,stamp,size,stamp,size,wrong,pin,16)!=sealed::StoreInstall::kInstall,
          "flat world store: wrong prologue refuses both consumers");
    {
        std::mutex installMutex;
        std::atomic<bool> ready{false},refused{false},start{false};
        std::atomic<unsigned> installs{0};
        std::vector<std::thread> callers;
        for(unsigned i=0;i<16;++i)callers.emplace_back([&] {
            while(!start.load(std::memory_order_acquire))std::this_thread::yield();
            flatWorldStoreInstallGuard(installMutex,ready,refused,[&]() noexcept {
                ++installs;std::this_thread::sleep_for(std::chrono::milliseconds(2));
                ready.store(true,std::memory_order_release);
            });
        });
        start.store(true,std::memory_order_release);
        for(auto& caller:callers)caller.join();
        check(installs==1&&ready,"flat world store: concurrent Present/config install requests patch once");
        ready.store(false);refused.store(false);installs.store(0);
        flatWorldStoreInstallGuard(installMutex,ready,refused,[&]() noexcept {++installs;refused.store(true);});
        flatWorldStoreInstallGuard(installMutex,ready,refused,[&]() noexcept {++installs;});
        check(installs==1&&refused,"flat world store: final refusal prevents installation retry");
    }
    flatWorldStoreTests(&check);
    sourceValidityRegression();entryFrameRegression();
    std::printf("flat_world_store_test: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
