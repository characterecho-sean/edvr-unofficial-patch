#pragma once
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace edvr { namespace previewcam {
using Read = bool (*)(void*, uintptr_t, void*, size_t) noexcept;
using Write = bool (*)(void*, uintptr_t, const void*, size_t) noexcept;
struct Memory { Read read; Write write; void* context; };
template<class T> bool get(const Memory& m, uintptr_t p, T& v) noexcept {
    return p && p <= UINTPTR_MAX-sizeof(v) && m.read(m.context,p,&v,sizeof(v));
}
struct Pose { float v[16]{}; };
inline bool finite(const Pose& p) noexcept {
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<3;++c)
        if(!std::isfinite(p.v[r*4+c])) return false;
    return true;
}
inline bool equal(const Pose& a,const Pose& b) noexcept {
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<3;++c)
        if(std::memcmp(&a.v[r*4+c],&b.v[r*4+c],sizeof(float))) return false;
    return true;
}
// CameraState has four Vec3 rows, with uninterpreted fourth lanes. The game
// multiplies the three basis rows, and adds base translation for row three.
inline Pose compose(const Pose& local,const Pose& base,const Pose& original) noexcept {
    Pose out=original;
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<3;++c) {
        out.v[r*4+c]=(local.v[r*4+1]*base.v[4+c]+local.v[r*4]*base.v[c])+
            local.v[r*4+2]*base.v[8+c];
        if(r==3) out.v[r*4+c]+=base.v[12+c];
    }
    return out;
}
enum Address { Child,Child10,Stack,Nested,HmdIface,HmdObject,PreviewIface,PreviewObject,
    Weight,ConstantWeight,State,AddressCount };
constexpr uint32_t kRvas[AddressCount]={0x5189070,0x5189078,0x5189370,0x5189A18,
    0x518C920,0x518C8B0,0x51AA500,0x51AA388,0x8BEA20,0xF81910,0x5294E0};
struct Game { uintptr_t at[AddressCount]{}; };
struct Identity {
    uintptr_t child=0,stack=0,predecessor=0,array=0,hmd=0,preview=0;
    bool operator==(const Identity& b) const noexcept {
        return child==b.child&&stack==b.stack&&predecessor==b.predecessor&&
            array==b.array&&hmd==b.hmd&&preview==b.preview;
    }
};
inline bool identity(const Memory& m,const Game& g,uintptr_t child,Identity& out) noexcept {
    Identity q; q.child=child;
    uintptr_t v=0,v10=0; uint64_t count=0; uint32_t mode=0;
    if(!get(m,child,v)||v!=g.at[Child]||!get(m,child+0x10,v10)||v10!=g.at[Child10]||
       !get(m,child+0x30,q.stack)||!get(m,q.stack,v)||(v!=g.at[Stack]&&v!=g.at[Nested])||
       !get(m,child+0x40,q.predecessor)||!q.predecessor||!get(m,child+0x58,count)||count!=2||
       !get(m,child+0x60,q.array)||!get(m,child+0x68,mode)||mode!=0) return false;
    unsigned members=0;uint8_t registered=0;
    for(unsigned i=0;i<11;++i) {uintptr_t c=0;if(!get(m,q.stack+0x28+i*8,c))return false;members+=c==child?1u:0u;}
    uintptr_t name=0;uint64_t length=0;char text[8]{};
    if(members!=1||!get(m,child+0x38,registered)||registered!=1||!get(m,q.stack+0x20,name)||!get(m,name,length)||length!=8||
       !m.read(m.context,name+0x14,text,8)||std::memcmp(text,"Location",8)) return false;
    if(!get(m,q.array,q.hmd)||!get(m,q.array+8,q.preview)||q.hmd<0x20||q.preview<0x60) return false;
    uintptr_t hc=0,pc=0;uint8_t enabled=0;
    if(!get(m,q.hmd,v)||v!=g.at[HmdIface]||!get(m,q.preview,v)||v!=g.at[PreviewIface]||
       !get(m,q.hmd-0x20,v)||v!=g.at[HmdObject]||!get(m,q.preview-0x60,v)||v!=g.at[PreviewObject]||
       !get(m,q.hmd-4,enabled)||!enabled||!get(m,q.hmd+0x120,hc)||hc!=child||
       !get(m,q.preview+0x120,pc)||pc!=child) return false;
    uintptr_t f=0;
    if(!get(m,g.at[HmdIface]+0x20,f)||f!=g.at[Weight]||
       !get(m,g.at[PreviewIface]+0x20,f)||f!=g.at[ConstantWeight]||
       !get(m,g.at[HmdIface]+0x28,f)||f!=g.at[State]||
       !get(m,g.at[PreviewIface]+0x28,f)||f!=g.at[State]) return false;
    out=q;return true;
}
struct Sample { Identity id; Pose primary,local,base; };
struct EyeSnapshot { Pose composed{}; float raw[3]{}; uint32_t epoch=0; uint64_t generation=0; };
inline bool hmdSample(const Memory& m,const Identity& id,Sample& s) noexcept {
    s.id=id;
    return get(m,id.hmd+0x50,s.primary)&&get(m,id.hmd+0xD0,s.local)&&
        get(m,id.predecessor+0x40,s.base)&&finite(s.primary)&&finite(s.local)&&finite(s.base);
}
inline bool previewSample(const Memory& m,const Identity& id,Sample& s) noexcept {
    s.id=id;uint32_t mode=2;
    return get(m,id.preview+0x33C,mode)&&mode!=2&&get(m,id.preview+0x50,s.primary)&&
        get(m,id.preview+0x470,s.base)&&finite(s.primary)&&finite(s.base)&&equal(s.primary,s.base);
}

// One synchronous camera-manager evaluation encloses all parallel producers
// and the later-priority finalizers. Overlap poisons that entire evaluation.
// Independent producer slots publish immutable samples with release/acquire;
// there is no mutex, waiting, allocation or game task serialization.
class Engine {
    static constexpr uint64_t Commit=uint64_t(1)<<29,Initial=uint64_t(1)<<30,Poison=uint64_t(1)<<31,CountMask=Commit-1;
    static constexpr unsigned Slots=16;
    struct Row {
        std::atomic<uintptr_t> child{0};std::atomic<uint32_t> cycle{0};
        std::atomic<bool> closed{false};std::atomic<unsigned> phase[2]{};
        std::atomic<uintptr_t> owner[2]{};
        Sample sample[2];
        void reset() noexcept {child.store(0);cycle.store(0);closed.store(false);for(auto& p:phase)p.store(0);for(auto& p:owner)p.store(0);}
    } rows_[Slots];
    std::atomic<uint64_t> state_{0};std::atomic<bool> enabled_{false};
    // The kind-5 eye cameras are built after this evaluation, from a copy of
    // the HMD primary made before the finalizer. The copy is what becomes
    // camera+0x50 and then cb1 row 275. Location child+0xB0 is a different store.
    struct EyeHead {
        // A failed claim leaves that eye on the game's pose. Neither side waits
        // for another camera job, and the payload is never read while written.
        mutable std::atomic_flag busy=ATOMIC_FLAG_INIT;
        std::atomic<uint32_t> serial{0};
        std::atomic<uint64_t> generation{1};
        uint64_t publishedGeneration=0;
        Pose composed{};
        float raw[3]{};
    } eye_;
    Row* row(uintptr_t child,bool create) noexcept {
        if(!child)return nullptr;
        for(unsigned i=0;i<Slots;++i) {
            auto& r=rows_[i];uintptr_t key=r.child.load(std::memory_order_acquire);
            if(!key&&create) {uintptr_t zero=0;r.child.compare_exchange_strong(zero,child);key=r.child.load();}
            if(key==child)return &r;
        }
        return nullptr;
    }
public:
    std::atomic<uint64_t> applied{0},refused{0},evaluations{0},hmdCalls{0},previewCalls{0},finalizerCalls{0},hmdQualified{0},previewQualified{0};
    void poison() noexcept {state_.fetch_or(Poison,std::memory_order_acq_rel);}
    void enable(bool value) noexcept {
        enabled_.store(value,std::memory_order_release);
        if(!value) {
            eye_.generation.fetch_add(1,std::memory_order_acq_rel);
            eye_.serial.store(0,std::memory_order_release);
            poison();
        }
    }
    uint32_t current() const noexcept {
        const uint64_t s=state_.load(std::memory_order_acquire);
        return enabled_.load()&&(s&CountMask)==1&&!(s&(Commit|Initial|Poison))?uint32_t(s>>32):0;
    }
    bool valid(uint32_t epoch) const noexcept {return epoch&&current()==epoch;}
    bool eyeEpoch(uint32_t epoch) const noexcept {
        // The eye builder follows manager completion, when the count is zero.
        const uint64_t s=state_.load(std::memory_order_acquire);
        return epoch&&enabled_.load(std::memory_order_acquire)&&uint32_t(s>>32)==epoch&&
            !(s&(Commit|Initial|Poison));
    }
    void enter() noexcept {
        uint64_t old=state_.load(),next=0;
        for(;;) {
            const uint64_t n=old&CountMask;
            next=n?((old+1)|Poison):((((old>>32)+1)<<32)|Initial|1);
            if(state_.compare_exchange_weak(old,next))break;
        }
        if(!(old&CountMask)) {
            for(auto& r:rows_)r.reset();
            eye_.serial.store(0,std::memory_order_release);
            evaluations.fetch_add(1,std::memory_order_relaxed);
            if(enabled_.load()&&uint32_t(next>>32))state_.compare_exchange_strong(next,next&~Initial);
        }
    }
    void leave() noexcept {state_.fetch_sub(1,std::memory_order_release);}
    // The commit decision is atomic with manager admission. A later manager
    // poisons subsequent work, not a write already committed. Actual pose
    // stores remain inside the game's exclusive child-finalizer callback;
    // this is not an atomic multiword pose or a repair for invalid concurrent
    // dispatch of the same game subscriber collection.
    // Publish the head the later eye-camera builder multiplies. Cleared when
    // the next evaluation starts, so a missed builder cannot reuse a stale head.
    void publishEye(uint32_t epoch,const Pose& composed,const Pose& raw) noexcept {
        const uint64_t generation=eye_.generation.load(std::memory_order_acquire);
        if(!valid(epoch))return;
        if(eye_.busy.test_and_set(std::memory_order_acquire)) {
            // Another head cannot stand in for this one. Invalidate even a
            // writer holding the claim, rather than leave its pose available.
            eye_.generation.fetch_add(1,std::memory_order_acq_rel);
            eye_.serial.store(0,std::memory_order_release);
            return;
        }
        eye_.serial.store(0,std::memory_order_release);
        eye_.composed=composed;
        eye_.raw[0]=raw.v[12];eye_.raw[1]=raw.v[13];eye_.raw[2]=raw.v[14];
        eye_.publishedGeneration=generation;
        if(valid(epoch)&&eye_.generation.load(std::memory_order_acquire)==generation)
            eye_.serial.store(epoch,std::memory_order_release);
        eye_.busy.clear(std::memory_order_release);
    }
    bool eyeSnapshotValid(const EyeSnapshot& snapshot) const noexcept {
        return snapshot.epoch&&eye_.serial.load(std::memory_order_acquire)==snapshot.epoch&&
            eye_.generation.load(std::memory_order_acquire)==snapshot.generation&&eyeEpoch(snapshot.epoch);
    }
    bool loadEye(EyeSnapshot& snapshot) const noexcept {
        if(!enabled_.load(std::memory_order_acquire)||eye_.busy.test_and_set(std::memory_order_acquire))return false;
        const uint32_t serial=eye_.serial.load(std::memory_order_acquire);
        const uint64_t generation=eye_.generation.load(std::memory_order_acquire);
        const bool published=eye_.publishedGeneration==generation&&eyeEpoch(serial);
        if(published) {
            snapshot.composed=eye_.composed;
            snapshot.raw[0]=eye_.raw[0];snapshot.raw[1]=eye_.raw[1];snapshot.raw[2]=eye_.raw[2];
            snapshot.epoch=serial;snapshot.generation=generation;
        }
        const bool valid=published&&eye_.serial.load(std::memory_order_acquire)==serial&&
            eye_.generation.load(std::memory_order_acquire)==generation&&eyeEpoch(serial);
        eye_.busy.clear(std::memory_order_release);
        return valid;
    }
    bool loadEye(Pose& composed,float raw[3]) const noexcept {
        EyeSnapshot snapshot;
        if(!loadEye(snapshot))return false;
        composed=snapshot.composed;
        raw[0]=snapshot.raw[0];raw[1]=snapshot.raw[1];raw[2]=snapshot.raw[2];
        return true;
    }
    template<class Store> bool commit(uint32_t epoch,Store store) noexcept {
        if(!epoch||!enabled_.load())return false;
        uint64_t expected=(uint64_t(epoch)<<32)|1;
        if(!state_.compare_exchange_strong(expected,expected|Commit))return false;
        const bool result=store();state_.fetch_and(~Commit,std::memory_order_release);return result;
    }
    void invalidateRole(uintptr_t child,unsigned role) noexcept {
        if(!current())return;
        Row* r=row(child,false);if(!r)return;
        // A second invocation before finalization cannot reuse an older
        // successful result, even if this invocation has an unsupported roster.
        if(r->closed.load()||r->phase[role].load()!=0)poison();
    }
    void invalidateProducer(uintptr_t owner,unsigned role) noexcept {
        if(!current())return;
        for(auto& r:rows_) {
            const uintptr_t prior=r.owner[role].load(std::memory_order_acquire);
            if(prior&&prior==owner){poison();return;}
        }
    }
    struct Completion {uint32_t epoch=0,cycle=0;uintptr_t child=0;unsigned role=0;};
    // The no-op production specialization has no callback overhead. The seam
    // lets the rig enter another producer at the exact claim/publication gap.
    template<class AfterClaim> Completion beginWithClaim(uintptr_t child,unsigned role,uintptr_t owner,AfterClaim afterClaim) noexcept {
        Completion c;c.epoch=current();c.child=child;c.role=role;
        if(!c.epoch)return {};
        Row* r=row(child,true);if(!r||r->closed.load()){poison();return {};}
        c.cycle=r->cycle.load();uintptr_t unowned=0;
        if(!r->owner[role].compare_exchange_strong(unowned,owner?owner:child)){poison();return {};}
        afterClaim();
        unsigned empty=0;
        if(!r->phase[role].compare_exchange_strong(empty,1)){poison();return {};}
        return c;
    }
    Completion begin(uintptr_t child,unsigned role,uintptr_t owner=0) noexcept {
        return beginWithClaim(child,role,owner,[]{});
    }
    void finish(const Completion& c,const Sample* s) noexcept {
        if(!valid(c.epoch))return;
        Row* r=row(c.child,false);
        if(!r||r->closed.load()||r->cycle.load()!=c.cycle){poison();return;}
        if(s)r->sample[c.role]=*s;
        r->phase[c.role].store(s?2u:3u,std::memory_order_release);
    }
    bool take(uint32_t epoch,uintptr_t child,Sample& h,Sample& p) noexcept {
        if(!valid(epoch))return false;
        Row* r=row(child,false);if(!r)return false;
        if(r->closed.exchange(true)){poison();return false;}
        const unsigned a=r->phase[0].load(std::memory_order_acquire),b=r->phase[1].load(std::memory_order_acquire);
        if(a==1||b==1||(!a&&r->owner[0].load())||(!b&&r->owner[1].load())){poison();return false;}
        const bool ok=a==2&&b==2;
        if(ok){h=r->sample[0];p=r->sample[1];}
        r->phase[0].store(0);r->phase[1].store(0);r->owner[0].store(0);r->owner[1].store(0);
        r->cycle.fetch_add(1);r->closed.store(false);
        return ok&&valid(epoch);
    }
};
struct Evaluation {Engine& e;explicit Evaluation(Engine& a):e(a){e.enter();}~Evaluation(){e.leave();}};
template<class Original> uintptr_t manager(Engine& e,Original original) {Evaluation scope(e);return original();}
template<class Original> uintptr_t hmd(Engine& e,const Memory& m,const Game& g,uintptr_t object,Original original) {
    e.hmdCalls.fetch_add(1,std::memory_order_relaxed);
    e.invalidateProducer(object,0);
    uintptr_t child=0;Identity before;Pose baseBefore;
    const bool linked=e.current()&&get(m,object+0x140,child);
    if(linked)e.invalidateRole(child,0);
    const bool eligible=linked&&
        identity(m,g,child,before)&&before.hmd==object+0x20&&
        get(m,before.predecessor+0x40,baseBefore)&&finite(baseBefore);
    const auto c=eligible?e.begin(child,0,object):Engine::Completion{};const uintptr_t result=original();
    Identity after;Sample s;
    const bool good=eligible&&e.valid(c.epoch)&&identity(m,g,child,after)&&before==after&&
        hmdSample(m,after,s)&&equal(s.base,baseBefore)&&!equal(s.primary,s.base);
    if(good)e.hmdQualified.fetch_add(1,std::memory_order_relaxed);
    e.finish(c,good?&s:nullptr);return result;
}
template<class Original> uintptr_t preview(Engine& e,const Memory& m,const Game& g,uintptr_t subscriber,Original original) {
    e.previewCalls.fetch_add(1,std::memory_order_relaxed);
    e.invalidateProducer(subscriber,1);
    uintptr_t child=0;Identity before;const bool linked=subscriber>=0x1B0&&e.current()&&get(m,subscriber-0x30,child);
    if(linked)e.invalidateRole(child,1);
    const bool eligible=linked&&identity(m,g,child,before)&&before.preview==subscriber-0x150;
    const auto c=eligible?e.begin(child,1,subscriber):Engine::Completion{};const uintptr_t result=original();
    Identity after;Sample s;
    const bool good=eligible&&e.valid(c.epoch)&&identity(m,g,child,after)&&before==after&&previewSample(m,after,s);
    if(good)e.previewQualified.fetch_add(1,std::memory_order_relaxed);
    e.finish(c,good?&s:nullptr);return result;
}
template<class Original> uintptr_t finalizer(Engine& e,const Memory& m,const Game& g,uintptr_t child,Original original) {
    e.finalizerCalls.fetch_add(1,std::memory_order_relaxed);
    const uint32_t epoch=e.current();Identity before;const bool eligible=epoch&&identity(m,g,child,before);
    const uintptr_t result=original();Sample h,p;
    // Consume on every finalizer, including inactive/changed/unsupported rosters.
    const bool complete=e.take(epoch,child,h,p);
    Identity after;Sample hc,pc;Pose output;
    const bool good=eligible&&complete&&identity(m,g,child,after)&&before==after&&after==h.id&&after==p.id&&
        hmdSample(m,after,hc)&&previewSample(m,after,pc)&&equal(h.primary,hc.primary)&&equal(h.local,hc.local)&&
        equal(h.base,hc.base)&&equal(p.primary,pc.primary)&&get(m,child+0xB0,output)&&finite(output)&&equal(output,h.primary);
    if(good) {
        // The preview pose is the stable exterior camera. Composing the local
        // headset onto it makes the ship follow the head. The 13:48 flight's
        // scene eyes were this pose, and a second kind-5 pair stayed at the
        // origin with the headset's orientation: the head-locked menu.
        const Pose changed=p.primary;
        const bool finiteChanged=finite(changed);
        const bool wrote=finiteChanged&&e.commit(epoch,[&]{
            return m.write(m.context,after.hmd+0x50,&changed,sizeof(changed))&&
                m.write(m.context,child+0xB0,&changed,sizeof(changed));
        });
        if(wrote) {
            e.publishEye(epoch,changed,h.primary);
            e.applied.fetch_add(1,std::memory_order_relaxed);return result;
        }
    }
    if(eligible)e.refused.fetch_add(1,std::memory_order_relaxed);
    return result;
}
// The eye-camera producer multiplies this head by the per-eye offset and
// stores the product at the kind-5 camera's +0x50, which the view-constant
// refresh loads into cb1 row 275. Replace a head that is still the raw HMD
// primary, and a head sitting on the tracking origin: the ship-menu cameras
// on the 13:48 frame were a stereo pair there, 36 m from the preview. A pose
// tens of metres from both is another camera and is left alone.
inline bool applyEyeHead(Engine& e,const Memory& m,uintptr_t pose) noexcept {
    EyeSnapshot snapshot;
    if(!e.loadEye(snapshot))return false;
    Pose current;
    if(!get(m,pose,current)||!finite(current))return false;
    const float dx=current.v[12]-snapshot.raw[0],dy=current.v[13]-snapshot.raw[1],dz=current.v[14]-snapshot.raw[2];
    const float ox=current.v[12],oy=current.v[13],oz=current.v[14];
    if(dx*dx+dy*dy+dz*dz>0.25f&&ox*ox+oy*oy+oz*oz>1.f)return false;
    return e.eyeSnapshotValid(snapshot)&&m.write(m.context,pose,&snapshot.composed,sizeof(snapshot.composed));
}
}} // namespace edvr::previewcam
