#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../src/common/timing.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/sunglare_fix.h"

namespace {
using namespace edvr;
struct View { bool resolve = true; ResourceInfo info{}; } views[2];
unsigned dampingCalls = 0, clockCalls = 0;
unsigned dampingPattern = 0;
uint64_t sample = 0;
std::vector<unsigned> queries;
std::vector<unsigned> getters;
bool damping() { return (dampingPattern & (1u << dampingCalls++)) != 0; }
uint64_t clockSample() { ++clockCalls; return sample; }
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> void record(SunglareRead<T>& r, T value) { r = {true, true, value}; }
template<class T> void equal(const SunglareRead<T>& a, const SunglareRead<T>& b) {
    check(a.reached == b.reached && a.known == b.known && a.value == b.value,
          "production read envelope differs from frozen lazy stages");
}
void equalTexture(const SunglareTextureRead& a, const SunglareTextureRead& b) {
    equal(a.resolveOk,b.resolveOk); equal(a.isTexture2D,b.isTexture2D);
    equal(a.width,b.width); equal(a.height,b.height); equal(a.format,b.format);
}
void equalObservation(const SunglareSelectorObservation& a, const SunglareSelectorObservation& b) {
    equal(a.outerWantsMode,b.outerWantsMode); equal(a.outerExposureDamping,b.outerExposureDamping);
    equal(a.outerProbe,b.outerProbe); equal(a.outerTrainShape,b.outerTrainShape); equal(a.outerWantsResult,b.outerWantsResult);
    equal(a.helperWantsMode,b.helperWantsMode); equal(a.helperExposureDamping,b.helperExposureDamping);
    equal(a.helperProbe,b.helperProbe); equal(a.helperTrainShape,b.helperTrainShape); equal(a.helperWantsResult,b.helperWantsResult);
    equalTexture(a.ps0,b.ps0); equalTexture(a.ps1,b.ps1);
    equal(a.lastSeenBeforeMs,b.lastSeenBeforeMs); equal(a.nowMs,b.nowMs);
    equal(a.lastSeenAfterMs,b.lastSeenAfterMs); equal(a.actionMode,b.actionMode); equal(a.action,b.action);
}
// Literal frozen 14a gates: no production selector, shape, wants or observed
// output is an oracle input. Recording below only describes consumed reads.
bool frozenWants(unsigned mode, bool probe, SunglareRead<SunglareTraceMode>& mr,
                 SunglareRead<bool>& dr, SunglareRead<bool>& pr, SunglareRead<bool>& rr) {
    record(mr, static_cast<SunglareTraceMode>(mode));
    bool result = mode != 0;
    if (!result) { const bool d = damping(); record(dr,d); result = d;
        if (!d) { record(pr,probe); result = probe; } }
    record(rr,result); return result;
}
bool frozenTexture(unsigned slot, SunglareTextureRead& r) {
    getters.push_back(slot); queries.push_back(slot); const View& v = views[slot]; record(r.resolveOk,v.resolve);
    if (!v.resolve) return false;
    record(r.isTexture2D,v.info.isTexture2D); if (!v.info.isTexture2D) return false;
    record(r.width,v.info.a); if (v.info.a != 2048) return false;
    record(r.height,v.info.b); if (v.info.b != 1024) return false;
    record(r.format,v.info.fmt); return v.info.fmt == 98;
}
SunglareAction frozen(unsigned mode, bool probe, char kind, uint32_t count,
                      uint32_t instances, uint64_t& stamp, SunglareSelectorObservation& o) {
    const bool shape = kind == 'N' && count == 6 && instances >= 2;
    record(o.outerTrainShape,shape);
    if (!shape || !frozenWants(mode,probe,o.outerWantsMode,o.outerExposureDamping,o.outerProbe,o.outerWantsResult)) {
        record(o.action,SunglareTraceAction::kStock); return SunglareAction::kStock; }
    if (!frozenWants(mode,probe,o.helperWantsMode,o.helperExposureDamping,o.helperProbe,o.helperWantsResult)) {
        record(o.action,SunglareTraceAction::kStock); return SunglareAction::kStock; }
    const bool helperShape = kind == 'N' && count == 6 && instances >= 2;
    record(o.helperTrainShape,helperShape);
    if (!helperShape || !frozenTexture(0,o.ps0) || !frozenTexture(1,o.ps1)) {
        record(o.action,SunglareTraceAction::kStock); return SunglareAction::kStock; }
    record(o.lastSeenBeforeMs,stamp); const uint64_t now = clockSample();
    record(o.nowMs,now); stamp = now; record(o.lastSeenAfterMs,stamp);
    record(o.actionMode,static_cast<SunglareTraceMode>(mode));
    const SunglareAction result = mode == 1 ? SunglareAction::kSkip : SunglareAction::kMatch;
    record(o.action,mode == 1 ? SunglareTraceAction::kSkip : SunglareTraceAction::kMatch); return result;
}
void resetCounters() { dampingCalls = clockCalls = 0; queries.clear(); getters.clear(); }
void runCase(unsigned mode, bool probe, char kind, uint32_t count, uint32_t instances,
             uint64_t initial) {
    resetCounters(); uint64_t expectedStamp = initial; SunglareSelectorObservation expected{};
    const auto expectedAction = frozen(mode,probe,kind,count,instances,expectedStamp,expected);
    const auto expectedQueries = queries; const auto expectedGetters = getters;
    const auto expectedDamping = dampingCalls, expectedClock = clockCalls;
    resetCounters(); sunglarePredicateTestState(static_cast<SunglareTraceMode>(mode),probe,0,initial);
    SunglareSelectorObservation actual{};
    const auto action = sunglareOnEyeDrawObserved(kind,count,instances,actual);
    check(action == expectedAction && sunglareLastSeenMs() == expectedStamp,"production action/stamp parity");
    check(queries == expectedQueries && getters == expectedGetters && dampingCalls == expectedDamping && clockCalls == expectedClock,
          "production query, damping or clock count/order parity"); equalObservation(actual,expected);
    resetCounters(); sunglarePredicateTestState(static_cast<SunglareTraceMode>(mode),probe,0,initial);
    SunglareAction ordinary = SunglareAction::kStock;
    // Frozen original caller gate surrounding the actual ordinary helper.
    if (kind == 'N' && count == 6 && instances >= 2 && sunglareWantsDraws())
        ordinary = sunglareOnEyeDraw(kind,count,instances);
    check(ordinary == expectedAction && sunglareLastSeenMs() == expectedStamp,"ordinary helper parity");
    check(queries == expectedQueries && getters == expectedGetters && dampingCalls == expectedDamping && clockCalls == expectedClock,
          "ordinary helper lazy parity");
}
void selfTest() {
    g_clockForTest = clockSample;
    const char kinds[] = {'N','X','D'};
    const uint32_t counts[] = {0,5,6,7,UINT32_MAX};
    const uint32_t instances[] = {0,1,2,15,UINT32_MAX};
    const uint64_t stamps[] = {0,1,UINT64_MAX};
    unsigned cases = 0;
    for (unsigned mode=0;mode<4;++mode) for (unsigned pattern=0;pattern<4;++pattern)
    for (unsigned probe=0;probe<2;++probe) for (unsigned resource=0;resource<17;++resource) {
        dampingPattern = pattern;
        for (auto& v: views) { v = View{}; v.info.isTexture2D=true; v.info.a=2048; v.info.b=1024; v.info.fmt=98; }
        if (resource) { View& v=views[(resource-1)/8]; switch ((resource-1)%8) {
            case 0:v.resolve=false;break; case 1:v.info.isTexture2D=false;break;
            case 2:v.info.a=2047;break; case 3:v.info.a=2049;break;
            case 4:v.info.b=1023;break; case 5:v.info.b=1025;break;
            case 6:v.info.fmt=97;break; case 7:v.info.fmt=99;break; } }
        for (char kind:kinds) for (uint32_t count:counts) for (uint32_t n:instances)
        for (uint64_t stamp:stamps) for (uint64_t prior:stamps) {
            sample = stamp; runCase(mode,probe!=0,kind,count,n,prior); ++cases; }
    }
    g_clockForTest=nullptr;
    std::printf("sunglare predicate self-test: %u frozen/observed/ordinary cases passed\n",cases);
}
} // namespace
namespace edvr {
void* bindingGet(BindSlot slot) {
    check(slot == BindSlot::PsSrv0 || slot == BindSlot::PsSrv1,"unexpected binding getter");
    const unsigned index = slot == BindSlot::PsSrv0 ? 0 : 1;
    getters.push_back(index); return &views[index];
}
bool bindingResolve(void* pointer, ResourceInfo* out) {
    check(pointer == &views[0] || pointer == &views[1],"unexpected resource query");
    const unsigned slot=pointer == &views[0] ? 0 : 1; queries.push_back(slot);
    if (!views[slot].resolve) return false; *out=views[slot].info; return true;
}
bool exposureDampingActive() { return damping(); }
// The linked translation unit contains GPU entry points outside this test.
// Any accidental call into those paths must fail rather than silently pass.
void billboardGlareWatch(bool) { throw std::runtime_error("unexpected billboard watch"); }
const float* billboardShadowFloats(uint32_t*) { throw std::runtime_error("unexpected billboard shadow"); }
uint64_t billboardShadowAgeMs() { throw std::runtime_error("unexpected billboard age"); }
void* billboardTarget() { throw std::runtime_error("unexpected billboard target"); }
}
int main(int argc,char** argv) {
    if (argc != 2) { std::fprintf(stderr,"use --self-test or --dry-run\n"); return 2; }
    const std::string arg=argv[1];
    if (arg == "--dry-run") { std::puts("sunglare predicate dry-run: no file writes"); return 0; }
    if (arg != "--self-test") return 2;
    try { selfTest(); return 0; } catch (const std::exception& e) { std::fprintf(stderr,"sunglare predicate: %s\n",e.what()); return 1; }
}
