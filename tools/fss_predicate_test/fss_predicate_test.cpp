#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Use the installed SDK's complete COM layout. Production translation units
// keep their normal C++ interfaces; these C structs have the same ABI.
#define CINTERFACE
// SDK descriptor convenience classes call C++ COM methods even when the
// interfaces use C layout. This TU needs only the typed ABI declarations.
#define D3D11_NO_HELPERS
#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/d3d11/fss_observation.h"
#include "../../src/d3d11/fss_panel.h"
#include "../../src/d3d11/fss_reveal.h"
#include "../../src/d3d11/shader_swap.h"

namespace {

constexpr uint64_t kPanelColor = 0xA888D51024D9798Eull;
constexpr uint64_t kPanelPrepass = 0xB018D143700AB803ull;
constexpr uint64_t kRevealComposite = 0x953C8123AD8DC13Bull;
constexpr DWORD kTestFault = 0xE055F551u;

unsigned g_checks = 0;
unsigned g_failures = 0;

void check(bool ok, const char* message) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", message);
    }
}

template <class T>
bool fact(const edvr::FssRead<T>& value, bool reached, bool known, T expected) {
    return value.reached == reached && value.known == known &&
           (!known || value.value == expected);
}

struct CallState {
    std::string order;
    uint64_t hash = 0;
    bool returnShader = true;
    bool faultGet = false;
    bool faultLookup = false;
    bool faultRelease = false;
    unsigned getCalls = 0;
    unsigned lookupCalls = 0;
    unsigned releaseCalls = 0;
    unsigned shaderRefs = 1;
};

CallState* g_active = nullptr;

using FakeContext = ID3D11DeviceContext;
using FakeShader = ID3D11VertexShader;

extern FakeShader g_fakeShader;

void STDMETHODCALLTYPE fakeVsGetShader(ID3D11DeviceContext*,
                                        ID3D11VertexShader** shader,
                                        ID3D11ClassInstance**, UINT*) {
    CallState& s = *g_active;
    s.order.push_back('G');
    ++s.getCalls;
    if (s.faultGet) RaiseException(kTestFault, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    *shader = nullptr;
    // The shader object is supplied through the stable test global below.
    if (s.returnShader) {
        *shader = reinterpret_cast<ID3D11VertexShader*>(&g_fakeShader);
        ++s.shaderRefs; // VSGetShader returns an AddRef'd interface.
    }
}

ULONG STDMETHODCALLTYPE fakeShaderRelease(ID3D11VertexShader*) {
    CallState& s = *g_active;
    s.order.push_back('R');
    ++s.releaseCalls;
    if (s.faultRelease) RaiseException(kTestFault, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    if (s.shaderRefs) --s.shaderRefs;
    return s.shaderRefs;
}

ID3D11DeviceContextVtbl g_contextVtable{};
ID3D11VertexShaderVtbl g_shaderVtable{};
FakeShader g_fakeShader{&g_shaderVtable};
FakeContext g_fakeContext{&g_contextVtable};

void initializeFakeCom() {
    g_contextVtable.VSGetShader = fakeVsGetShader;
    g_shaderVtable.Release = fakeShaderRelease;
}

uint64_t expectedHash(const CallState& s, bool budgetAvailable = true) {
    if (!budgetAvailable || !s.returnShader || s.faultGet || s.faultLookup) return 0;
    return s.hash;
}

ID3D11DeviceContext* fakeContext() {
    return reinterpret_cast<ID3D11DeviceContext*>(&g_fakeContext);
}

CallState fresh(uint64_t hash) {
    CallState s;
    s.hash = hash;
    return s;
}

bool legacyPanelCaller(bool enabled, uint32_t frameNo, uint32_t bodyFrame,
                       char kind, uint32_t count, uint32_t instances,
                       const CallState& calls, bool budgetAvailable = true) {
    if (!enabled || bodyFrame == 0 || uint32_t(frameNo - bodyFrame) > 2) return false;
    if (kind != 'X' || count != 6 || instances != 1) return false;
    const uint64_t hash = expectedHash(calls, budgetAvailable);
    return hash == kPanelColor || hash == kPanelPrepass;
}

bool observedPanelCall(bool enabled, uint32_t frameNo, uint32_t bodyFrame,
                       char kind, uint32_t count, uint32_t instances,
                       CallState& calls, edvr::FssPanelObservation& o,
                       bool budgetAvailable = true) {
    o = {};
    edvr::fss_predicate_test::setPanelEnabled(enabled);
    o.outerEnabled = {true, true, enabled};
    if (!enabled) return false;
    o.bodyFrame = {true, true, bodyFrame};
    if (bodyFrame == 0) return false;
    o.frameNo = {true, true, frameNo};
    if (uint32_t(frameNo - bodyFrame) > 2) return false;
    g_active = &calls;
    edvr::fss_predicate_test::resetPanelBudget(budgetAvailable ? 8 : 0);
    return edvr::fssPanelOnEyeDrawObserved(
        fakeContext(), kind, count, instances, o);
}

bool runPanelLegacy(bool enabled, uint32_t frameNo, uint32_t bodyFrame,
                    char kind, uint32_t count, uint32_t instances,
                    CallState& calls, bool budgetAvailable = true) {
    edvr::fss_predicate_test::setPanelEnabled(enabled);
    if (!enabled || bodyFrame == 0 || uint32_t(frameNo - bodyFrame) > 2)
        return false;
    g_active = &calls;
    edvr::fss_predicate_test::resetPanelBudget(budgetAvailable ? 8 : 0);
    return edvr::fssPanelOnEyeDraw(fakeContext(), kind, count, instances);
}

void testPanelInputsAndHashMutation() {
    for (uint64_t hash : {kPanelColor, kPanelPrepass}) {
        CallState s = fresh(hash);
        edvr::fss_predicate_test::setPanelMatchedHash(0x12345678ull);
        edvr::FssPanelObservation o{};
        const bool observed = observedPanelCall(true, 102, 100, 'X', 6, 1, s, o);
        check(observed, "panel observes both literal legacy shader hashes");
        check(o.helper.assignedHash.known && o.helper.assignedHash.value == hash,
              "panel records the raw assigned hash");
        check(o.matchedHashBefore.known && o.matchedHashBefore.value == 0x12345678ull &&
              o.matchedHashAfter.known && o.matchedHashAfter.value == hash,
              "panel matched-hash state changes only after a positive predicate");
        check(s.order == "GHR" && s.getCalls == 1 && s.lookupCalls == 1 &&
              s.releaseCalls == 1 && s.shaderRefs == 1,
              "panel performs VSGetShader, lookup, and Release in order");

        CallState ordinary = fresh(hash);
        edvr::fss_predicate_test::setPanelMatchedHash(0x12345678ull);
        const bool legacy = runPanelLegacy(true, 102, 100, 'X', 6, 1, ordinary);
        check(legacy == observed && ordinary.order == s.order,
              "panel ordinary and observed helpers preserve return and call order");
    }

    CallState miss = fresh(kPanelColor ^ 1ull);
    edvr::fss_predicate_test::setPanelMatchedHash(0x7777ull);
    edvr::FssPanelObservation o{};
    check(!observedPanelCall(true, 10, 10, 'X', 6, 1, miss, o),
          "panel rejects one-bit shader hash mutation");
    check(o.matchedHashBefore.value == 0x7777ull && o.matchedHashAfter.value == 0x7777ull,
          "panel mismatch preserves prior matched hash");

    CallState zero = fresh(0);
    check(!observedPanelCall(true, 10, 10, 'X', 6, 1, zero, o),
          "panel treats a zero lookup as a known miss");
    check(o.helper.assignedHash.reached && o.helper.assignedHash.known &&
          o.helper.assignedHash.value == 0,
          "panel zero hash remains a concrete assigned selector input");

    CallState nullShader = fresh(kPanelColor);
    nullShader.returnShader = false;
    check(!observedPanelCall(true, 10, 10, 'X', 6, 1, nullShader, o),
          "panel null bound shader is a known zero lookup");
    check(nullShader.order == "GH" &&
          fact(o.helper.shaderNonNull, true, true, false) &&
          fact(o.helper.assignedHash, true, true, uint64_t(0)) &&
          !o.helper.releaseReached.value,
          "panel null shader skips Release after assigned zero hash");
}

void testPanelCallerAndShapeGates() {
    struct Case { uint32_t now, body; char kind; uint32_t count, instances; bool enabled, pass; };
    const Case cases[] = {
        {10, 10, 'X', 6, 1, true, true},
        {12, 10, 'X', 6, 1, true, true},
        {13, 10, 'X', 6, 1, true, false},
        {10, 0, 'X', 6, 1, true, false},
        {0, UINT32_MAX, 'X', 6, 1, true, true},
        {1, UINT32_MAX, 'X', 6, 1, true, true},
        {2, UINT32_MAX, 'X', 6, 1, true, false},
        {10, 10, 'N', 6, 1, true, false},
        {10, 10, 'X', 5, 1, true, false},
        {10, 10, 'X', 6, 0, true, false},
        {10, 10, 'X', 6, 1, false, false},
    };
    for (const Case& c : cases) {
        CallState s = fresh(kPanelColor);
        edvr::fss_predicate_test::setPanelMatchedHash(0);
        const bool expected = legacyPanelCaller(c.enabled, c.now, c.body,
                                                  c.kind, c.count, c.instances, s);
        check(expected == c.pass, "frozen panel caller boundary fixture is correct");
        edvr::FssPanelObservation o{};
        const bool got = observedPanelCall(c.enabled, c.now, c.body,
                                            c.kind, c.count, c.instances, s, o);
        check(got == expected, "observed panel caller matches frozen body/shape gate");
        if (!c.pass) {
            check(s.getCalls == 0 && s.lookupCalls == 0 && s.releaseCalls == 0,
                  "panel caller/shape miss does no shader query work");
            if (!c.enabled) {
                check(fact(o.outerEnabled, true, true, false) &&
                      !o.bodyFrame.reached && !o.helper.enabled.reached,
                      "disabled panel stops before body and helper reads");
            } else if (c.body == 0) {
                check(fact(o.bodyFrame, true, true, uint32_t(0)) &&
                      !o.frameNo.reached && !o.helper.enabled.reached,
                      "zero panel body stamp stops before frame age/helper");
            } else if (uint32_t(c.now - c.body) > 2) {
                check(o.frameNo.reached && !o.helper.enabled.reached,
                      "old panel body frame stops before helper observation");
            }
        }
    }
}

void testPanelFaultOrderingAndBudget() {
    edvr::FssPanelObservation o{};
    CallState getFault = fresh(kPanelColor);
    getFault.faultGet = true;
    edvr::fss_predicate_test::setPanelMatchedHash(0xA11CEull);
    check(!observedPanelCall(true, 1, 1, 'X', 6, 1, getFault, o),
          "panel VSGetShader fault declines with initialized zero hash");
    check(getFault.order == "G" && o.helper.callbackEntered.value &&
          !o.helper.vsGetShaderCompleted.value && !o.helper.lookupReached.value &&
          !o.helper.releaseReached.value && o.helper.hashAfterGuard.value == 0 &&
          !o.helper.guardReturned.value && !o.helper.assignedHash.reached &&
          !o.helper.shaderNonNull.reached &&
          fact(o.helper.lookupCompleted, true, true, false) &&
          fact(o.helper.releaseCompleted, true, true, false) &&
          fact(o.helper.callbackCompleted, true, true, false) &&
          fact(o.matchedHashBefore, true, true, uint64_t(0xA11CE)) &&
          fact(o.matchedHashAfter, true, true, uint64_t(0xA11CE)),
          "panel getter fault observation preserves exact reached stages");

    CallState lookupFault = fresh(kPanelColor);
    lookupFault.faultLookup = true;
    edvr::fss_predicate_test::setPanelMatchedHash(0xB22CEull);
    check(!observedPanelCall(true, 1, 1, 'X', 6, 1, lookupFault, o),
          "panel lookup fault declines rather than using an unassigned hash");
    check(lookupFault.order == "GH" && o.helper.vsGetShaderCompleted.value &&
          o.helper.lookupReached.value && !o.helper.lookupCompleted.value &&
          !o.helper.assignedHash.reached && !o.helper.shaderNonNull.reached &&
          fact(o.helper.releaseReached, true, true, false) &&
          fact(o.helper.releaseCompleted, true, true, false) &&
          fact(o.helper.callbackCompleted, true, true, false) &&
          fact(o.helper.hashAfterGuard, true, true, uint64_t(0)) &&
          fact(o.matchedHashBefore, true, true, uint64_t(0xB22CE)) &&
          fact(o.matchedHashAfter, true, true, uint64_t(0xB22CE)),
          "panel lookup fault keeps raw hash unavailable and skips Release");

    CallState releaseFault = fresh(kPanelPrepass);
    releaseFault.faultRelease = true;
    edvr::fss_predicate_test::setPanelMatchedHash(0xC33CEull);
    check(observedPanelCall(true, 1, 1, 'X', 6, 1, releaseFault, o),
          "panel retains match assigned before a Release fault");
    check(releaseFault.order == "GHR" && o.helper.assignedHash.value == kPanelPrepass &&
          fact(o.helper.shaderNonNull, true, true, true) &&
          fact(o.helper.releaseReached, true, true, true) &&
          fact(o.helper.releaseCompleted, true, true, false) &&
          fact(o.helper.callbackCompleted, true, true, false) &&
          !o.helper.guardReturned.value && o.helper.hashAfterGuard.value == kPanelPrepass &&
          fact(o.matchedHashBefore, true, true, uint64_t(0xC33CE)) &&
          fact(o.matchedHashAfter, true, true, kPanelPrepass),
          "panel Release fault records post-assignment hash and exact callback status");
    CallState legacyReleaseFault = fresh(kPanelPrepass);
    legacyReleaseFault.faultRelease = true;
    check(runPanelLegacy(true, 1, 1, 'X', 6, 1, legacyReleaseFault) &&
          legacyReleaseFault.order == releaseFault.order,
          "panel ordinary path keeps the same Release-fault claim and call order");

    CallState dead = fresh(kPanelColor);
    edvr::fss_predicate_test::setPanelMatchedHash(0xD44CEull);
    check(!observedPanelCall(true, 1, 1, 'X', 6, 1, dead, o, false),
          "panel exhausted FaultBudget declines");
    check(dead.order.empty() && o.helper.guardCallReached.value &&
          fact(o.helper.callbackEntered, true, true, false) &&
          fact(o.helper.vsGetShaderCompleted, true, true, false) &&
          fact(o.helper.lookupReached, true, true, false) &&
          fact(o.helper.lookupCompleted, true, true, false) &&
          o.helper.hashAfterGuard.value == 0 &&
          fact(o.matchedHashBefore, true, true, uint64_t(0xD44CE)) &&
          fact(o.matchedHashAfter, true, true, uint64_t(0xD44CE)),
          "panel dead budget enters no callback and performs no COM/hash calls");

    CallState nullContext = fresh(kPanelColor);
    o = {}; // Production callers provide a fresh observation for every draw.
    edvr::fss_predicate_test::setPanelEnabled(true);
    edvr::fss_predicate_test::setPanelMatchedHash(0xE55CEull);
    edvr::fss_predicate_test::resetPanelBudget();
    g_active = &nullContext;
    check(!edvr::fssPanelOnEyeDrawObserved(nullptr, 'X', 6, 1, o),
          "panel null context declines before guarded callback");
    check(nullContext.order.empty() && o.helper.contextNonNull.reached &&
          !o.helper.contextNonNull.value && !o.helper.guardCallReached.reached &&
          fact(o.matchedHashBefore, true, true, uint64_t(0xE55CE)) &&
          fact(o.matchedHashAfter, true, true, uint64_t(0xE55CE)),
          "panel context-null fact is known without shader work");
}

struct RevealCallerInput {
    bool steady = false;
    bool lockstep = false;
    uint32_t now = 0;
    bool splitFrameNo = false;
    uint32_t jumpNow = 0;
    uint32_t body = 0;
    uint32_t jump = 0;
    bool latch = false;
    bool arrivalOpen = false;
    uint32_t arrivalRecognitions = 0;
    char kind = 'N';
    uint32_t count = 6;
    uint32_t instances = 1;
};

struct RevealCallerOutput {
    bool helperReached = false;
    bool claimed = false;
    unsigned latchReads = 0;
    RevealCallerInput after;
    edvr::FssRevealObservation observation{};
};

bool frozenRevealCaller(const RevealCallerInput& in, const CallState& calls,
                        bool budgetAvailable = true) {
    if (!(in.steady || in.lockstep)) return false;
    const bool body = in.body != 0 && uint32_t(in.now - in.body) <= 2;
    bool jump = false;
    if (!body && in.jump != 0 && uint32_t((in.splitFrameNo ? in.jumpNow : in.now) - in.jump) <= 600) jump = in.latch;
    if (!body && !jump) return false;
    if (in.kind != 'N' || in.count != 6 || in.instances != 1) return false;
    const uint64_t hash = expectedHash(calls, budgetAvailable);
    return hash == kRevealComposite;
}

RevealCallerOutput runReveal(const RevealCallerInput& in, CallState& calls,
                             bool observed, bool budgetAvailable = true) {
    RevealCallerOutput out{};
    out.after = in;
    const bool wants = in.steady || in.lockstep;
    const bool bodyStampReached = wants;
    const bool bodyAgeReached = wants && in.body != 0;
    const bool body = bodyAgeReached && uint32_t(in.now - in.body) <= 2;
    bool jump = false;
    const bool jumpStampReached = wants && !body;
    const bool jumpFrameReached = jumpStampReached && in.jump != 0;
    const uint32_t jumpNow = in.splitFrameNo ? in.jumpNow : in.now;
    const bool jumpAgeReached = jumpFrameReached && uint32_t(jumpNow - in.jump) <= 600;
    if (!body && jumpAgeReached) {
        ++out.latchReads;
        jump = in.latch;
    }
    out.helperReached = wants && (body || jump);
    g_active = &calls;
    edvr::fss_predicate_test::setRevealModes(in.steady, in.lockstep);
    edvr::fss_predicate_test::resetRevealBudget(budgetAvailable ? 8 : 0);
    if (out.helperReached) {
        if (observed) {
            out.claimed = edvr::fssRevealOnEyeDrawObserved(
                fakeContext(), in.kind, in.count, in.instances, out.observation);
        } else {
            out.claimed = edvr::fssRevealOnEyeDraw(
                fakeContext(), in.kind, in.count, in.instances);
        }
    }
    // Exercise the original caller's real branch; compare its result and
    // mutation against the independent frozen oracle in the assertions.
    if (out.claimed && in.arrivalOpen)
        ++out.after.arrivalRecognitions;

    auto& f = out.observation;
    f.outerSteady = {true, true, in.steady};
    if (!in.steady) f.outerLockstep = {true, true, in.lockstep};
    if (bodyStampReached) f.bodyFrame = {true, true, in.body};
    if (bodyAgeReached) f.bodyFrameNo = {true, true, in.now};
    if (jumpStampReached) f.jumpFrame = {true, true, in.jump};
    if (jumpFrameReached) {
        f.jumpFrameNo = {true, true, jumpNow};
    }
    if (jumpAgeReached) f.modeLatch = {true, true, in.latch};
    if (out.claimed) {
        f.arrivalOpen = {true, true, in.arrivalOpen};
        if (in.arrivalOpen) {
            f.arrivalBefore = {true, true, in.arrivalRecognitions};
            f.arrivalAfter = {true, true, out.after.arrivalRecognitions};
        }
    }
    return out;
}

void checkHelperMatched(const edvr::FssHelperObservation& h, uint64_t hash,
                        const char* message) {
    const bool ok = fact(h.contextNonNull, true, true, true) &&
        fact(h.guardCallReached, true, true, true) &&
        fact(h.callbackEntered, true, true, true) &&
        fact(h.vsGetShaderCompleted, true, true, true) &&
        fact(h.shaderNonNull, true, true, true) &&
        fact(h.lookupReached, true, true, true) &&
        fact(h.lookupCompleted, true, true, true) &&
        fact(h.assignedHash, true, true, hash) &&
        fact(h.releaseReached, true, true, true) &&
        fact(h.releaseCompleted, true, true, true) &&
        fact(h.callbackCompleted, true, true, true) &&
        fact(h.guardReturned, true, true, true) &&
        fact(h.hashAfterGuard, true, true, hash);
    check(ok, message);
}

void testHelperFactShapes() {
    edvr::FssPanelObservation p{};
    CallState panel = fresh(kPanelColor);
    const bool panelResult = observedPanelCall(true, 3, 3, 'X', 6, 1, panel, p);
    check(panelResult, "panel fact-shape positive fixture claims");
    checkHelperMatched(p.helper, kPanelColor,
                       "panel complete observation marks every successful getter stage");

    edvr::FssRevealObservation r{};
    RevealCallerInput in{};
    in.steady = true;
    in.now = in.body = 5;
    CallState reveal = fresh(kRevealComposite);
    RevealCallerOutput revealOut = runReveal(in, reveal, true);
    r = revealOut.observation;
    check(revealOut.claimed, "reveal fact-shape positive fixture claims");
    checkHelperMatched(r.helper, kRevealComposite,
                       "reveal complete observation marks every successful getter stage");

    CallState ordinary = fresh(kRevealComposite);
    g_active = &ordinary;
    edvr::fss_predicate_test::setRevealModes(true, false);
    edvr::fss_predicate_test::resetRevealBudget();
    const bool legacy = edvr::fssRevealOnEyeDraw(fakeContext(), 'N', 6, 1);
    check(legacy == revealOut.claimed && ordinary.order == reveal.order,
          "reveal ordinary and observed helpers preserve return and call order");
}

/*
 * The source facts below are independent caller inputs and pure frozen-14a
 * selector expressions. In particular, observed helper results never decide
 * whether a helper stage was eligible or whether the arrival count changes.
 */

void testRevealCallerAndArrivalFacts() {
    RevealCallerInput bodyClaim{};
    bodyClaim.steady = true;
    bodyClaim.now = 20;
    bodyClaim.body = 18;
    bodyClaim.jump = 1;
    bodyClaim.latch = false;
    bodyClaim.arrivalOpen = true;
    bodyClaim.arrivalRecognitions = 9;
    CallState bodyCalls = fresh(kRevealComposite);
    RevealCallerOutput bodyObserved = runReveal(bodyClaim, bodyCalls, true);
    check(frozenRevealCaller(bodyClaim, bodyCalls) && bodyObserved.claimed,
          "reveal body claim is independent of jump latch result");
    check(bodyObserved.claimed == frozenRevealCaller(bodyClaim, bodyCalls),
          "reveal helper result equals independent caller/helper oracle");
    check(bodyObserved.latchReads == 0 && bodyObserved.after.arrivalRecognitions == 10 &&
          fact(bodyObserved.observation.arrivalBefore, true, true, uint32_t(9)) &&
          fact(bodyObserved.observation.arrivalAfter, true, true, uint32_t(10)),
          "arrival-open accounting follows a body claim and records before/after");

    RevealCallerInput bodyExpired{};
    bodyExpired.steady = true;
    bodyExpired.now = 21;
    bodyExpired.body = 18; // age 3: the body route is closed.
    bodyExpired.jump = 20;
    bodyExpired.latch = true;
    CallState expiredCalls = fresh(kRevealComposite);
    RevealCallerOutput expired = runReveal(bodyExpired, expiredCalls, true);
    check(expired.claimed && expired.latchReads == 1 &&
          expired.claimed == frozenRevealCaller(bodyExpired, expiredCalls) &&
          expired.observation.modeLatch.value,
          "reveal age-three body falls through to the independently gated jump route");

    RevealCallerInput jumpClaim{};
    jumpClaim.lockstep = true;
    jumpClaim.now = 700;
    jumpClaim.jump = 100;
    jumpClaim.latch = true;
    jumpClaim.arrivalOpen = true;
    jumpClaim.arrivalRecognitions = UINT32_MAX;
    CallState jumpCalls = fresh(kRevealComposite);
    RevealCallerOutput jumpObserved = runReveal(jumpClaim, jumpCalls, true);
    check(frozenRevealCaller(jumpClaim, jumpCalls) && jumpObserved.claimed &&
          jumpObserved.latchReads == 1,
          "reveal jump fallback accepts inclusive age 600 when mode latch is on");
    check(jumpObserved.claimed == frozenRevealCaller(jumpClaim, jumpCalls),
          "reveal jump path agrees with independent frozen hash oracle");
    check(jumpObserved.after.arrivalRecognitions == 0 &&
          jumpObserved.observation.arrivalAfter.value == 0,
          "reveal arrival recognition counter wraps as uint32");

    struct AgeCase { uint32_t now, stamp; bool latch, pass; };
    const AgeCase ages[] = {
        {600, 0, true, false}, {700, 100, true, true},
        {701, 100, true, false}, {0, UINT32_MAX, true, true},
        {1, UINT32_MAX, true, true},
        {599, UINT32_MAX, true, true}, {600, UINT32_MAX, true, false},
    };
    for (const AgeCase& age : ages) {
        RevealCallerInput in{};
        in.steady = true;
        in.now = age.now;
        in.jump = age.stamp;
        in.latch = age.latch;
        CallState calls = fresh(kRevealComposite);
        RevealCallerOutput out = runReveal(in, calls, true);
        check(out.claimed == age.pass &&
              out.claimed == frozenRevealCaller(in, calls),
              "frozen reveal jump boundary/wrap fixture");
        if (age.stamp == 0 || uint32_t(age.now - age.stamp) > 600)
            check(out.latchReads == 0, "reveal skips mode-latch read when jump gate fails");
    }

    RevealCallerInput bodyWrap{};
    bodyWrap.lockstep = true;
    bodyWrap.now = 0;
    bodyWrap.body = UINT32_MAX;
    bodyWrap.jump = 1;
    bodyWrap.latch = false;
    CallState wrapCalls = fresh(kRevealComposite);
    RevealCallerOutput wrap = runReveal(bodyWrap, wrapCalls, true);
    check(wrap.claimed && wrap.claimed == frozenRevealCaller(bodyWrap, wrapCalls) &&
          wrap.latchReads == 0,
          "reveal body age uses unsigned frame subtraction across wrap");

    RevealCallerInput disabled{};
    disabled.now = 1;
    disabled.body = 1;
    CallState disabledCalls = fresh(kRevealComposite);
    RevealCallerOutput off = runReveal(disabled, disabledCalls, true);
    check(!off.helperReached && disabledCalls.order.empty() &&
          !off.observation.helper.contextNonNull.reached,
          "reveal off means no helper call and no helper facts");

    RevealCallerInput notLatched = jumpClaim;
    notLatched.latch = false;
    notLatched.arrivalOpen = true;
    CallState noLatchCalls = fresh(kRevealComposite);
    RevealCallerOutput noLatch = runReveal(notLatched, noLatchCalls, true);
    check(!noLatch.claimed && !frozenRevealCaller(notLatched, noLatchCalls) &&
          noLatch.after.arrivalRecognitions == notLatched.arrivalRecognitions &&
          !noLatch.observation.arrivalOpen.reached,
          "arrival-open is not read when reveal helper declines");
}

void testRevealModeShortCircuitAndHelper() {
    RevealCallerInput both{};
    both.steady = true;
    both.lockstep = true;
    both.now = 4;
    both.body = 4;
    CallState calls = fresh(kRevealComposite);
    RevealCallerOutput out = runReveal(both, calls, true);
    check(out.claimed && fact(out.observation.outerSteady, true, true, true) &&
          !out.observation.outerLockstep.reached,
          "steady short-circuits the lockstep mode read");
    check(fact(out.observation.helper.steady, true, true, true) &&
          !out.observation.helper.lockstep.reached,
          "observed helper preserves the same lazy mode OR");

    RevealCallerInput lock{};
    lock.lockstep = true;
    lock.now = 4;
    lock.body = 4;
    CallState lockCalls = fresh(kRevealComposite);
    RevealCallerOutput lockOut = runReveal(lock, lockCalls, true);
    check(lockOut.claimed && fact(lockOut.observation.helper.steady, true, true, false) &&
          fact(lockOut.observation.helper.lockstep, true, true, true),
          "lockstep-only helper reads both OR operands");

    RevealCallerInput wrongHash = lock;
    CallState mismatch = fresh(kRevealComposite ^ 1ull);
    RevealCallerOutput miss = runReveal(wrongHash, mismatch, true);
    check(!miss.claimed && miss.observation.helper.assignedHash.value == (kRevealComposite ^ 1ull),
          "reveal exact composite hash rejects a one-bit mutation");
    checkHelperMatched(miss.observation.helper, kRevealComposite ^ 1ull,
                       "reveal hash miss records the complete successful query sequence");

    RevealCallerInput wrongShape = lock;
    wrongShape.kind = 'X';
    CallState noQuery = fresh(kRevealComposite);
    RevealCallerOutput shape = runReveal(wrongShape, noQuery, true);
    check(!shape.claimed && noQuery.order.empty(),
          "reveal wrong draw shape declines before VSGetShader");

    edvr::FssRevealObservation offObservation{};
    CallState offCalls = fresh(kRevealComposite);
    g_active = &offCalls;
    edvr::fss_predicate_test::setRevealModes(false, false);
    edvr::fss_predicate_test::resetRevealBudget();
    check(!edvr::fssRevealOnEyeDrawObserved(fakeContext(), 'N', 6, 1, offObservation),
          "direct reveal helper is disabled when both mode bits are false");
    check(fact(offObservation.helper.steady, true, true, false) &&
          fact(offObservation.helper.lockstep, true, true, false) &&
          !offObservation.helper.contextNonNull.reached && offCalls.order.empty(),
          "disabled reveal helper lazily reads modes and stops before context/query");

    edvr::FssPanelObservation panelOff{};
    CallState panelOffCalls = fresh(kPanelColor);
    g_active = &panelOffCalls;
    edvr::fss_predicate_test::setPanelEnabled(false);
    edvr::fss_predicate_test::setPanelMatchedHash(0x8888ull);
    edvr::fss_predicate_test::resetPanelBudget();
    check(!edvr::fssPanelOnEyeDrawObserved(fakeContext(), 'X', 6, 1, panelOff),
          "direct panel helper is disabled when outer feature is off");
    check(fact(panelOff.helper.enabled, true, true, false) &&
          panelOff.matchedHashBefore.value == 0x8888ull &&
          panelOff.matchedHashAfter.value == 0x8888ull &&
          !panelOff.helper.contextNonNull.reached && panelOffCalls.order.empty(),
          "disabled panel records state without entering shader helper");
}

void testRevealGuardFaults() {
    RevealCallerInput in{};
    in.steady = true;
    in.body = in.now = 1;

    CallState get = fresh(kRevealComposite);
    get.faultGet = true;
    RevealCallerOutput getOut = runReveal(in, get, true);
    check(!getOut.claimed && get.order == "G" &&
          getOut.observation.helper.callbackEntered.value &&
          !getOut.observation.helper.vsGetShaderCompleted.value &&
          !getOut.observation.helper.lookupReached.value &&
          !getOut.observation.helper.assignedHash.reached &&
          fact(getOut.observation.helper.releaseReached, true, true, false) &&
          fact(getOut.observation.helper.hashAfterGuard, true, true, uint64_t(0)),
          "reveal VSGetShader fault records only the reached getter stage");

    CallState lookup = fresh(kRevealComposite);
    lookup.faultLookup = true;
    RevealCallerOutput lookupOut = runReveal(in, lookup, true);
    check(!lookupOut.claimed && lookup.order == "GH" &&
          lookupOut.observation.helper.hashAfterGuard.value == 0 &&
          !lookupOut.observation.helper.assignedHash.reached,
          "reveal lookup fault has known initialized zero result and no Release");

    CallState release = fresh(kRevealComposite);
    release.faultRelease = true;
    RevealCallerOutput releaseOut = runReveal(in, release, true);
    check(releaseOut.claimed && releaseOut.claimed == frozenRevealCaller(in, release) &&
          release.order == "GHR" &&
          releaseOut.observation.helper.hashAfterGuard.value == kRevealComposite &&
          !releaseOut.observation.helper.guardReturned.value,
          "reveal keeps matching raw hash assigned before Release fault");
    CallState ordinaryRelease = fresh(kRevealComposite);
    ordinaryRelease.faultRelease = true;
    RevealCallerOutput ordinaryReleaseOut = runReveal(in, ordinaryRelease, false);
    check(ordinaryReleaseOut.claimed == releaseOut.claimed &&
          ordinaryRelease.order == release.order,
          "reveal ordinary path preserves Release-fault result and call order");

    CallState dead = fresh(kRevealComposite);
    RevealCallerOutput deadOut = runReveal(in, dead, true, false);
    check(!deadOut.claimed && dead.order.empty() &&
          !deadOut.observation.helper.callbackEntered.value &&
          deadOut.observation.helper.hashAfterGuard.value == 0,
          "reveal exhausted FaultBudget makes no callback or shader query");

    CallState nullShader = fresh(kRevealComposite);
    nullShader.returnShader = false;
    RevealCallerOutput nullOut = runReveal(in, nullShader, true);
    check(!nullOut.claimed && nullShader.order == "GH" &&
          fact(nullOut.observation.helper.shaderNonNull, true, true, false) &&
          fact(nullOut.observation.helper.assignedHash, true, true, uint64_t(0)) &&
          !nullOut.observation.helper.releaseReached.value,
          "reveal null shader is a concrete zero hash and skips Release");

    edvr::FssRevealObservation nullObservation{};
    edvr::fss_predicate_test::setRevealModes(true, false);
    edvr::fss_predicate_test::resetRevealBudget();
    CallState nullContext = fresh(kRevealComposite);
    g_active = &nullContext;
    check(!edvr::fssRevealOnEyeDrawObserved(nullptr, 'N', 6, 1, nullObservation),
          "reveal null context declines after mode and shape gates");
    check(nullContext.order.empty() && nullObservation.helper.contextNonNull.reached &&
          !nullObservation.helper.contextNonNull.value &&
          !nullObservation.helper.guardCallReached.reached,
          "reveal context-null fact is known without entering the guard");
}

void testIndependentParityMatrix() {
    for (bool budget : {false, true}) for (bool shader : {false, true})
    for (unsigned fault = 0; fault < 4; ++fault)
    for (uint64_t hash : {uint64_t(0), kPanelColor, kPanelPrepass,
                          kRevealComposite, UINT64_MAX}) {
        CallState input = fresh(hash);
        input.returnShader = shader;
        input.faultGet = fault == 1;
        input.faultLookup = fault == 2;
        input.faultRelease = fault == 3;
        const std::string expectedOrder = !budget ? "" : input.faultGet ? "G" :
            input.faultLookup || !shader ? "GH" : "GHR";
        const uint64_t frozenHash = expectedHash(input, budget);
        const bool panelExpected = frozenHash == kPanelColor || frozenHash == kPanelPrepass;
        CallState panelObserved = input, panelOrdinary = input;
        edvr::FssPanelObservation panelFacts{};
        constexpr uint64_t previous = UINT64_MAX;
        edvr::fss_predicate_test::setPanelMatchedHash(previous);
        const bool panelGot = observedPanelCall(true, 1, 1, 'X', 6, 1,
                                                panelObserved, panelFacts, budget);
        check(panelGot == panelExpected && panelObserved.order == expectedOrder,
              "panel independent fault/budget/hash matrix result and order");
        check(fact(panelFacts.matchedHashBefore, true, true, previous) &&
              fact(panelFacts.matchedHashAfter, true, true, panelExpected ? frozenHash : previous),
              "panel matrix fresh actual matched-hash mutation");
        edvr::fss_predicate_test::setPanelMatchedHash(previous);
        check(runPanelLegacy(true, 1, 1, 'X', 6, 1, panelOrdinary, budget) == panelExpected &&
              panelOrdinary.order == expectedOrder,
              "panel ordinary matrix parity");

        RevealCallerInput reveal{};
        reveal.steady = true; reveal.body = reveal.now = 1;
        reveal.arrivalOpen = true; reveal.arrivalRecognitions = UINT32_MAX;
        const bool revealExpected = frozenRevealCaller(reveal, input, budget);
        CallState revealObserved = input, revealOrdinary = input;
        const auto observed = runReveal(reveal, revealObserved, true, budget);
        const auto ordinary = runReveal(reveal, revealOrdinary, false, budget);
        check(observed.claimed == revealExpected && ordinary.claimed == revealExpected &&
              revealObserved.order == expectedOrder && revealOrdinary.order == expectedOrder,
              "reveal independent fault/budget/hash matrix result and order");
        check(observed.after.arrivalRecognitions == (revealExpected ? 0u : UINT32_MAX) &&
              ordinary.after.arrivalRecognitions == observed.after.arrivalRecognitions,
              "reveal actual arrival branch matches independent counter-wrap oracle");
    }
    RevealCallerInput split{};
    split.steady = true; split.body = 10; split.now = 13;
    split.jump = 100; split.splitFrameNo = true; split.jumpNow = 700; split.latch = true;
    CallState splitCalls = fresh(kRevealComposite);
    const auto splitResult = runReveal(split, splitCalls, true);
    check(splitResult.claimed && frozenRevealCaller(split, splitCalls) &&
          fact(splitResult.observation.bodyFrameNo, true, true, uint32_t(13)) &&
          fact(splitResult.observation.jumpFrameNo, true, true, uint32_t(700)),
          "reveal body/jump frame-number loads remain separate raw inputs");
    RevealCallerInput off{}; CallState offCalls = fresh(kRevealComposite);
    const auto offResult = runReveal(off, offCalls, true);
    check(!offResult.claimed && fact(offResult.observation.outerSteady, true, true, false) &&
          fact(offResult.observation.outerLockstep, true, true, false) &&
          !offResult.observation.bodyFrame.reached && offCalls.order.empty(),
          "outer reveal false OR still records both consumed flag reads");
}

void run() {
    initializeFakeCom();
    testPanelInputsAndHashMutation();
    testPanelCallerAndShapeGates();
    testPanelFaultOrderingAndBudget();
    testHelperFactShapes();
    testRevealCallerAndArrivalFacts();
    testRevealModeShortCircuitAndHelper();
    testRevealGuardFaults();
    testIndependentParityMatrix();
}

} // namespace

namespace edvr {

// The rig links the actual helpers, while the selector's hash database is a
// deterministic leaf stub. Each call is still made from the production helper
// inside its real guarded/fault-budget region.
uint64_t lookupShaderHash(void* shader) {
    CallState& s = *g_active;
    s.order.push_back('H');
    ++s.lookupCalls;
    if (s.faultLookup) RaiseException(kTestFault, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return shader ? s.hash : 0;
}

ID3D11VertexShader* shaderSwapCompileVs(ID3D11DeviceContext*, const char*,
                                        size_t, const char*, const char*,
                                        const SwapMacro*, const char*) {
    return nullptr;
}

} // namespace edvr

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("fss_predicate_test: dry-run (no device or files)");
        return 0;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) return 2;
    run();
    std::printf("fss_predicate_test: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
