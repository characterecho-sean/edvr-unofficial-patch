#pragma once
#include "../../src/d3d11/flat_forward_light.h"
#include <cstdio>
#include <cstring>
#include <limits>

namespace forward_watch_test {
using namespace edvr::forward_light;
static_assert(Issue::Count==4,"New issue kinds require review of the output-scan no-resurrection proof");
inline Surface raw(uint64_t resource) {
    Surface s{};s.resource=resource;s.width=3440;s.height=1440;s.slices=1;s.samples=1;s.known=true;return s;
}
inline Source source(uint64_t frame) {
    Source s{};s.buffer=90;s.generation=1;s.bank=2;s.writeGeneration=3;s.frame=frame;s.writeQ=1;s.count=4096;return s;
}
struct Rig {
    Ledger ledger{};Authority authority[kPublications]{};unsigned revoked=0;bool hdrBad=false;
    unsigned char camera[96]{};uint64_t frame=19;uint32_t q=2;
    Rig(){ledger.reset(frame);camera[0]=1;}
    bool anyPublication()const {for(const auto& p:ledger.publications)if(p.live)return true;return false;}
    OutputWatchDecision watch(bool known=true,bool pending=false)const {
        return outputWatch(ledger,authority,frame,anyPublication(),known,pending);
    }
    void observe(uint64_t resource,bool conflict=false) {
        unsigned char bytes[96];std::memcpy(bytes,camera,96);if(conflict)bytes[0]^=2;
        ledger.observe(raw(resource),Role::Origin,true,true,source(frame),bytes,q++);
    }
    void unknownWriter(uint64_t resource) {ledger.observe(raw(resource),Role::Unknown,true,false,{},nullptr,q++);}
    void mutation(uint64_t resource,bool forget) {
        ledger.mutation(resource,forget);
        for(auto& a:authority)if(a.affected(frame,resource)){a.live=false;++revoked;hdrBad=true;}
    }
    void output(uint64_t resource,bool uav,bool prospective) {
        if(!prospective||watch().needed())mutation(resource,uav);
        ++q;
    }
    void publish(uint64_t depth,uint64_t linear,bool returned=true) {
        const auto token=ledger.publish(raw(depth),raw(linear),q++);
        if(token){ledger.issued(token);if(returned)ledger.returned(token);}
    }
    bool admit(uint64_t depth,uint64_t linear)const {
        Reason why;const auto* p=ledger.find(raw(depth),raw(linear),q,why);if(!p)return false;
        for(unsigned i=0;i<ledger.used;++i)if(ledger.domains[i].raw.resource==depth)
            return ledger.domains[i].failure==Reason::None&&ledger.domains[i].origins!=0;
        return false;
    }
    void attest(uint64_t depth,uint64_t linear) {
        if(!admit(depth,linear))return;
        Reason why;const auto* p=ledger.find(raw(depth),raw(linear),q,why);
        authority[0]={frame,100,depth,linear,p->token,true};hdrBad=false;
    }
    void nextFrame() {++frame;q=2;ledger.reset(frame);for(auto& a:authority)a={};hdrBad=false;}
};
inline bool equivalent(const Rig& a,const Rig& b) {
    if(a.frame!=b.frame||a.q!=b.q||a.revoked!=b.revoked||a.hdrBad!=b.hdrBad||
       a.ledger.used!=b.ledger.used||a.ledger.overflow!=b.ledger.overflow)return false;
    for(unsigned i=0;i<a.ledger.used;++i)
        if((a.ledger.domains[i].failure==Reason::None)!=(b.ledger.domains[i].failure==Reason::None))return false;
    for(unsigned i=0;i<kPublications;++i) {
        const auto& x=a.ledger.publications[i];const auto& y=b.ledger.publications[i];
        if(x.token!=y.token||x.live!=y.live||x.issued!=y.issued||x.returned!=y.returned)return false;
        const auto& u=a.authority[i];const auto& v=b.authority[i];
        if(u.live!=v.live||u.frame!=v.frame||u.raw!=v.raw||u.linear!=v.linear||u.token!=v.token)return false;
    }
    for(uint64_t depth:{1ull,2ull,3ull})for(uint64_t linear:{1ull,2ull,3ull,4ull})
        if(a.admit(depth,linear)!=b.admit(depth,linear))return false;
    return true;
}
inline void event(Rig& r,unsigned kind,bool prospective) {
    switch(kind) {
    case 0:r.observe(1);break;
    case 1:r.observe(1,true);break;
    case 2:r.unknownWriter(1);break;
    case 3:r.observe(2);break;
    case 4:r.output(1,false,prospective);break;
    case 5:r.output(1,true,prospective);break;
    case 6:r.output(2,true,prospective);break;
    case 7:r.publish(1,3);break;
    case 8:r.publish(2,3);break;
    case 9:r.publish(2,1,false);break; // absent raw / aliased linear / pending return
    case 10:r.attest(1,3);break;
    case 11:r.mutation(0,true);break; // explicit/foreign unknown write remains unconditional
    case 12:r.nextFrame();break;
    }
}
inline int run() {
    unsigned checks=0;int failures=0;
    const auto expect=[&](bool ok,const char* message){++checks;if(!ok){++failures;std::printf("FAIL: output watch %s\n",message);}};
    using W=OutputWatchReason;
    Rig r;
    expect(r.watch().reason==W::NoUsableState&&!r.watch().needed(),"empty established epoch has no prospective demand");
    expect(r.watch(false).reason==W::UnknownState&&r.watch(false).needed(),"unknown context/foreign ownership falls back");
    expect(r.watch(true,true).reason==W::UnknownState,"unrepresented held issue falls back");
    expect(outputWatch(r.ledger,r.authority,r.frame+1,false,true,false).reason==W::UnknownState,"frame mismatch falls back before reset");
    r.observe(1);expect(r.watch().reason==W::HealthyRaw,"new healthy raw domain re-enables watching");
    r.observe(1,true);expect(r.watch().reason==W::NoUsableState,"conflicting camera irreversibly fails this domain");
    r.observe(1);expect(r.watch().reason==W::NoUsableState,"matching later camera cannot heal failed domain");
    r.ledger.mutation(1);expect(r.watch().reason==W::NoUsableState,"nonzero failure overwrite remains failed");
    r.publish(1,3,false);expect(r.watch().reason==W::LivePublication,"failed-domain pending publication still watches");
    r.ledger.overflow=true;expect(r.watch().reason==W::LivePublication,"overflow does not hide a live publication");
    r.ledger.abandon(r.ledger.publications[0].token);expect(r.watch().reason==W::NoUsableState,"abandoned publication cannot later return live");
    r.ledger.returned(r.ledger.publications[0].token);expect(r.watch().reason==W::NoUsableState,"late return cannot resurrect abandoned token");
    r.nextFrame();r.publish(2,1,false);expect(r.watch().reason==W::LivePublication,"publication with an absent raw domain must watch");
    r.observe(2);expect(!r.admit(2,1),"pending publication is not returned authority");
    r.ledger.returned(r.ledger.publications[0].token);expect(r.admit(2,1),"later healthy raw plus returned publication may admit");
    r.attest(2,1);expect(r.watch().reason==W::LiveAuthority,"current authority takes priority");
    r.ledger.overflow=true;expect(r.watch().reason==W::LiveAuthority,"capacity loss cannot hide prior live authority");
    r.mutation(1,true);expect(r.revoked==1&&r.hdrBad&&!r.authority[0].live,"linear alias mutation revokes and invalidates typed HDR");
    expect(r.watch().reason==W::HealthyRaw,"revoked authority does not hide another healthy raw");
    r.unknownWriter(2);expect(r.watch().reason==W::NoUsableState&&r.hdrBad,"retained typed HDR after revoke does not restore authority");
    r.observe(3);expect(r.watch().reason==W::HealthyRaw,"later distinct healthy raw re-enables watching despite overflow");
    r.nextFrame();expect(r.watch().reason==W::NoUsableState&&!r.ledger.overflow,"frame reset expires failures, overflow and authority together");
    r.observe(1);r.publish(1,1);expect(r.watch().reason==W::LivePublication,"raw equals linear publication conservatively watches");
    Rig invalid;invalid.ledger.used=kDomains+1;expect(invalid.watch().reason==W::UnknownState,"invalid domain bound falls back");
    invalid=Rig{};invalid.ledger.used=1;expect(invalid.watch().reason==W::UnknownState,"unknown DSV/resource shape falls back");
    invalid=Rig{};invalid.observe(1);invalid.ledger.domains[0].failure=static_cast<Reason>(999);
    expect(invalid.watch().reason==W::UnknownState,"unknown failure code cannot be treated as a proved failed domain");
    invalid=Rig{};invalid.unknownWriter(1);invalid.ledger.domains[0].raw.samples=0;
    expect(invalid.watch().reason==W::UnknownState,"corrupt known surface cannot certify a failed domain");
    invalid=Rig{};invalid.authority[0]={invalid.frame+1,100,1,3,1,true};
    expect(invalid.watch().reason==W::UnknownState,"foreign authority epoch falls back");
    expect(outputWatch(r.ledger,r.authority,0,r.anyPublication(),true,false).reason==W::UnknownState,"unestablished frame zero falls back");
    Rig capacity;
    for(unsigned i=0;i<kDomains+1;++i)capacity.unknownWriter(1000+i);
    expect(capacity.ledger.overflow&&capacity.watch().reason==W::NoUsableState,"actual domain-cap overflow with only failed domains has no prospective demand");
    capacity.publish(1000,2000,false);
    expect(capacity.watch().reason==W::LivePublication,"new pending publication re-enables demand after actual domain overflow");
    capacity=Rig{};
    for(unsigned i=0;i<kPublications+1;++i)capacity.publish(1000,2000+i);
    expect(capacity.ledger.overflow&&capacity.watch().reason==W::LivePublication,"actual publication-cap overflow retains all existing live producers");
    capacity=Rig{};capacity.observe(1);capacity.ledger.domains[0].versions=UINT32_MAX-1;
    capacity.observe(1);
    expect(capacity.ledger.overflow&&capacity.watch().reason==W::NoUsableState,"actual domain-version exhaustion remains a failed-domain state");
    capacity.authority[0]={capacity.frame,100,1,3,1,true};
    expect(capacity.watch().reason==W::LiveAuthority,"live authority still watches after domain-version exhaustion");
    capacity=Rig{};
    capacity.ledger.observe(raw(1),Role::Unknown,false,false,{},nullptr,capacity.q++);
    expect(capacity.watch().reason==W::HealthyRaw,"unfailed domain without a donor stays conservative for future geometry");
    // A Converter can pass actual binding validation after its publication died.
    // These are its real Ledger completion operations, including an issue that
    // had not begun yet when a nested draw invalidated the publication.
    Rig held;held.unknownWriter(1);const auto dead=held.ledger.publish(raw(1),raw(3),held.q++);
    held.mutation(3,false);held.ledger.issued(dead);held.ledger.returned(dead);
    expect(!held.ledger.publications[0].live&&!held.ledger.publications[0].issued&&
        !held.ledger.publications[0].returned&&held.watch().reason==W::NoUsableState,
        "late Converter begin/return cannot revive a publication invalidated before issue");
    const auto replacement=held.ledger.publish(raw(1),raw(3),held.q++);
    held.ledger.returned(dead);held.ledger.abandon(dead);
    expect(replacement!=dead&&held.ledger.publications[0].token==replacement&&
        held.ledger.publications[0].live&&!held.ledger.publications[0].returned&&held.watch().needed(),
        "replacement publication is watched and isolated from old return/abandon");
    held.ledger.issued(replacement);held.mutation(3,false);held.ledger.returned(replacement);
    expect(!held.ledger.publications[0].returned&&held.watch().reason==W::NoUsableState,
        "already-issued Converter return cannot revive a subsequently invalidated token");
    held.nextFrame();const auto next=held.ledger.publish(raw(2),raw(3),held.q++);
    held.ledger.issued(replacement);held.ledger.returned(replacement);held.ledger.abandon(replacement);
    expect(next>replacement&&held.ledger.publications[0].live&&!held.ledger.publications[0].issued&&held.watch().needed(),
        "frame reset preserves token serial against delayed prior-frame completion");
    held.observe(2);held.ledger.issued(next);held.ledger.returned(next);held.attest(2,3);
    expect(held.watch().reason==W::LiveAuthority&&held.admit(2,3),"nested reset/new producer may establish fresh watched authority");
    held.mutation(3,false);Reason missing;const auto* light=held.ledger.find(raw(2),raw(3),held.q,missing);
    expect(!light&&missing==Reason::MutatedProducer&&!held.authority[0].live&&held.hdrBad,
        "Light token becomes invalid and retained HDR cannot hide revocation");
    held.unknownWriter(2);const auto failed=held.ledger.domains[0].failure;
    // Manual callback audit establishes that a valid Geometry return has no
    // Ledger operation. Test the real subsequent observation, without a fake
    // callback implementation that merely mirrors that audited behavior.
    held.observe(2);
    expect(held.ledger.domains[0].failure==failed&&held.watch().reason==W::NoUsableState,
        "failed Geometry's later matching donor cannot heal domain");
    held.ledger.mutation(2);expect(held.ledger.domains[0].failure!=Reason::None,
        "invalid or abandoned Geometry mutation only overwrites failure with failure");
    held.observe(4);held.publish(4,2,false);
    expect(held.watch().reason==W::LivePublication,"new healthy raw and aliased pending output re-enable watching immediately");
    // Check the probe itself, including padding bytes: no serial, failure,
    // publication, authority or camera byte may change while taking a sample.
    unsigned char before[sizeof(r)];std::memcpy(before,&r,sizeof(r));for(unsigned i=0;i<32;++i)(void)r.watch();
    expect(std::memcmp(before,&r,sizeof(r))==0,"prospective sampling changes no state byte");
    // All 13^4 short event sequences exercise the actual Ledger, not a second
    // implementation of its rules. Only the hypothetical output scan is gated;
    // explicit writes and all domain/publication/authority transitions still run.
    unsigned sequences=0;
    for(unsigned a=0;a<13;++a)for(unsigned b=0;b<13;++b)for(unsigned c=0;c<13;++c)for(unsigned d=0;d<13;++d) {
        Rig baseline,prospective;bool ok=true;
        for(unsigned e:{a,b,c,d}) {event(baseline,e,false);event(prospective,e,true);ok&=equivalent(baseline,prospective);}
        ++sequences;if(!ok){++failures;std::printf("FAIL: output watch admission/revocation sequence %u/%u/%u/%u\n",a,b,c,d);if(failures>12)return failures;}
    }
    ++checks;
    // Longer trace: dead A, output alias, previously absent B, pending producer,
    // return, attestation, secondary output mutation, then clean next-frame A.
    Rig baseline,prospective;
    for(unsigned e:{0u,1u,5u,9u,3u,6u,8u,10u,11u,12u,0u,7u,10u,4u}) {
        event(baseline,e,false);event(prospective,e,true);
        expect(equivalent(baseline,prospective),"long transition trace preserves future admission and revocation");
    }
    std::printf("forward output watch: %u checks, %u state sequences, %d failed\n",checks,sequences,failures);return failures;
}
} // namespace forward_watch_test
