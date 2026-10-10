#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/d3d11/binding_shadow.h"
#include "../../src/d3d11/shader_registry.h"
#include "../../src/plugins/exposure/exposure_dispatch.h"
#include "../../src/plugins/exposure/exposure_shape.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using Observer = edvr::plugins::exposure::ExposureDispatchObserverState;
using View = ID3D11UnorderedAccessView;
using Context = ID3D11DeviceContext;

struct PairAction {
    void* state = nullptr;
    Context* context = nullptr;
    std::array<View*, 4> first{};
    std::array<View*, 4> second{};
};

std::unordered_map<void*, uint64_t> g_shaderHashes;
std::vector<PairAction> g_pairActions;
std::vector<std::string> g_logLines;
bool g_shapeMatches = false;
uint32_t g_shapeCalls = 0;
uint32_t g_checks = 0;

template <typename T>
T* fakePointer(uintptr_t value) {
    return reinterpret_cast<T*>(value);
}

void check(bool ok, const char* message) {
    ++g_checks;
    if (!ok) {
        std::printf("FAIL: %s\n", message);
        std::exit(1);
    }
}

void resetServices() {
    g_shaderHashes.clear();
    g_pairActions.clear();
    g_logLines.clear();
    g_shapeMatches = false;
    g_shapeCalls = 0;
    for (size_t i = 0; i < static_cast<size_t>(edvr::BindSlot::Count); ++i) {
        edvr::detail::g_bindingSlots[i].ptr = nullptr;
        edvr::detail::g_bindingSlots[i].gen = 1;
        edvr::detail::g_bindingSlots[i].hash = 0;
    }
}

void bindShader(void* shader, uint64_t hash) {
    edvr::detail::g_bindingSlots[static_cast<size_t>(edvr::BindSlot::Cs)].ptr = shader;
    g_shaderHashes[shader] = hash;
}

void bindUavs(const std::array<View*, 4>& views) {
    for (uint32_t i = 0; i < 4; ++i) {
        const auto slot = static_cast<edvr::BindSlot>(
            static_cast<uint32_t>(edvr::BindSlot::CsUav0) + i);
        edvr::detail::g_bindingSlots[static_cast<size_t>(slot)].ptr = views[i];
    }
}

std::array<View*, 4> fakeViews(uintptr_t base) {
    return {fakePointer<View>(base + 1), fakePointer<View>(base + 2),
            fakePointer<View>(base + 3), fakePointer<View>(base + 4)};
}

bool sameViews(View* const* actual, const std::array<View*, 4>& expected) {
    for (uint32_t i = 0; i < 4; ++i) {
        if (actual[i] != expected[i]) return false;
    }
    return true;
}

void checkPair(size_t index, void* state, Context* context,
               const std::array<View*, 4>& first,
               const std::array<View*, 4>& second, const char* message) {
    check(g_pairActions.size() > index, message);
    const PairAction& action = g_pairActions[index];
    check(action.state == state && action.context == context && action.first == first &&
              action.second == second,
          message);
}

void testDefaultsAndNullState() {
    resetServices();
    Observer state;
    check(!state.enabled && state.targetHash == 0 && !state.pinned &&
              !state.rejected && state.shapeVerdict.empty() &&
              state.everExamined.empty() && state.detectStreak == 0 &&
              !state.announced && !state.gaveUpNotice &&
              state.seenThisFrame == 0,
          "observer starts disabled, unpinned, and empty");
    for (View* view : state.firstEye)
        check(view == nullptr, "observer starts without held UAV pointers");
    check(edvr::plugins::exposure::kExposureConfirmFrames == 5,
          "detector confirmation frame count stays pinned at five");

    const auto nullTicket = edvr::exposurePluginBeginDispatch(nullptr);
    edvr::exposurePluginCompleteDispatch(nullptr, {1}, fakePointer<Context>(0x91));
    edvr::exposurePluginResetDispatchFrame(nullptr);
    edvr::exposurePluginExpireDispatchVerdicts(nullptr);
    check(nullTicket.target == 0 && g_pairActions.empty(),
          "null observer is an inert no-op for all operations");
}

void testClassificationGatesAndPinnedHash() {
    resetServices();
    auto* shader = fakePointer<void>(0x101);
    bindShader(shader, 0xA001);
    g_shapeMatches = true;

    Observer state;
    state.enabled = false;
    check(edvr::exposurePluginBeginDispatch(&state).target == 0 &&
              g_shapeCalls == 0,
          "disabled observer does not classify");
    state.enabled = true;
    state.rejected = true;
    check(edvr::exposurePluginBeginDispatch(&state).target == 0 &&
              g_shapeCalls == 0,
          "rejected observer does not classify");
    state.rejected = false;
    edvr::detail::g_bindingSlots[static_cast<size_t>(edvr::BindSlot::Cs)].ptr = nullptr;
    check(edvr::exposurePluginBeginDispatch(&state).target == 0 &&
              g_shapeCalls == 0,
          "zero shader hash is declined before shape detection");

    state.pinned = true;
    state.targetHash = 0xA001;
    bindShader(shader, 0xA001);
    const auto match = edvr::exposurePluginBeginDispatch(&state);
    check(match.target == 1 && g_shapeCalls == 0,
          "pinned hash match is accepted without shape detection");
    bindShader(fakePointer<void>(0x102), 0xA002);
    check(edvr::exposurePluginBeginDispatch(&state).target == 0 &&
              g_shapeCalls == 0,
          "pinned hash mismatch is declined without shape detection");

    // A pinned ticket is trusted even if ordinary gates change after Begin.
    const auto preForwardUavs = fakeViews(0x200);
    const auto postForwardFirst = fakeViews(0x300);
    const auto postForwardSecond = fakeViews(0x400);
    bindShader(shader, 0xA001);
    bindUavs(preForwardUavs);
    Context* context = fakePointer<Context>(0x777);
    const auto firstTicket = edvr::exposurePluginBeginDispatch(&state);
    check(firstTicket.target == 1 && state.seenThisFrame == 0 &&
              g_pairActions.empty(),
          "Begin classifies only and does not capture or act");
    bindUavs(postForwardFirst);  // simulated real Dispatch changed the shadow
    edvr::exposurePluginCompleteDispatch(&state, firstTicket, context);
    check(state.seenThisFrame == 1 && sameViews(state.firstEye, postForwardFirst) &&
              g_pairActions.empty(),
          "first Complete captures post-forward UAV bindings only");

    const auto secondTicket = edvr::exposurePluginBeginDispatch(&state);
    check(secondTicket.target == 1, "second pinned pass receives its ticket");
    state.enabled = false;
    state.rejected = true;
    state.targetHash = 0xFFFF;  // the pre-forward verdict remains authoritative
    bindShader(fakePointer<void>(0x103), 0xA003);
    bindUavs(postForwardSecond);
    edvr::exposurePluginCompleteDispatch(&state, secondTicket, context);
    check(state.seenThisFrame == 2 && g_pairActions.size() == 1,
          "Complete consumes ticket without rechecking gates or shader freshness");
    checkPair(0, &state, context, postForwardFirst, postForwardSecond,
              "pair action uses post-forward UAVs and the supplied context");

    state.enabled = true;
    state.rejected = false;
    state.targetHash = 0xA001;
    bindShader(shader, 0xA001);
    const auto thirdTicket = edvr::exposurePluginBeginDispatch(&state);
    bindUavs(fakeViews(0x500));
    edvr::exposurePluginCompleteDispatch(&state, thirdTicket, context);
    check(state.seenThisFrame == 3 && g_pairActions.size() == 1,
          "third target pass preserves prior second-pass action without another pair");
    state.detectStreak = 4;
    edvr::exposurePluginResetDispatchFrame(&state);
    check(state.detectStreak == 0 && state.seenThisFrame == 0,
          "three target passes reset detection streak");
    for (View* view : state.firstEye)
        check(view == nullptr, "frame reset clears every captured UAV pointer");
}

void testCachedShapeVerdictsAndExpiration() {
    resetServices();
    Observer state;
    state.enabled = true;
    auto* positiveShader = fakePointer<void>(0x601);
    auto* negativeShader = fakePointer<void>(0x602);
    bindShader(positiveShader, 0xB001);
    g_shapeMatches = true;
    check(edvr::exposurePluginBeginDispatch(&state).target == 1 &&
              g_shapeCalls == 1 && state.shapeVerdict[0xB001] &&
              state.everExamined.count(0xB001) == 1,
          "first positive shader evaluates shape and records the verdict");
    check(edvr::exposurePluginBeginDispatch(&state).target == 1 &&
              g_shapeCalls == 1,
          "positive shader verdict is cached");

    bindShader(negativeShader, 0xB002);
    g_shapeMatches = false;
    check(edvr::exposurePluginBeginDispatch(&state).target == 0 &&
              g_shapeCalls == 2 && !state.shapeVerdict[0xB002] &&
              state.everExamined.count(0xB002) == 1,
          "first negative shader evaluates and records its verdict");
    check(edvr::exposurePluginBeginDispatch(&state).target == 0 &&
              g_shapeCalls == 2,
          "negative shader verdict is cached until expiration");

    edvr::exposurePluginExpireDispatchVerdicts(&state);
    check(state.shapeVerdict.count(0xB001) == 1 &&
              state.shapeVerdict.count(0xB002) == 0 &&
              state.everExamined.count(0xB002) == 1,
          "eligible expiration removes only negative verdicts, not examined history or positives");
    check(edvr::exposurePluginBeginDispatch(&state).target == 0 &&
              g_shapeCalls == 3,
          "expired negative is re-evaluated on its next observation");

    const auto negative = std::make_pair(uint64_t{0xB010}, false);
    const auto positive = std::make_pair(uint64_t{0xB011}, true);
    for (uint32_t blocker = 0; blocker < 3; ++blocker) {
        Observer blocked;
        blocked.shapeVerdict.insert(negative);
        blocked.shapeVerdict.insert(positive);
        blocked.announced = blocker == 0;
        blocked.gaveUpNotice = blocker == 1;
        blocked.targetHash = blocker == 2 ? 0xB010 : 0;
        edvr::exposurePluginExpireDispatchVerdicts(&blocked);
        check(blocked.shapeVerdict.count(0xB010) == 1 &&
                  blocked.shapeVerdict.count(0xB011) == 1,
              "announced, gave-up, and pinned states each prevent verdict expiration");
    }

    Observer noticesDoNotGate;
    noticesDoNotGate.enabled = true;
    noticesDoNotGate.announced = true;
    noticesDoNotGate.gaveUpNotice = true;
    noticesDoNotGate.pinned = true;
    noticesDoNotGate.targetHash = 0xB020;
    bindShader(fakePointer<void>(0x603), 0xB020);
    const auto noticeTicket = edvr::exposurePluginBeginDispatch(&noticesDoNotGate);
    check(noticeTicket.target == 1,
          "announcement and gave-up log flags do not gate classification");
}

void testNoCompleteAndTicketOnlyCompletion() {
    resetServices();
    Observer state;
    state.enabled = true;
    state.pinned = true;
    state.targetHash = 0xC001;
    bindShader(fakePointer<void>(0x701), 0xC001);
    Context* context = fakePointer<Context>(0x788);
    const auto ticket = edvr::exposurePluginBeginDispatch(&state);
    check(ticket.target == 1 && state.seenThisFrame == 0 &&
              g_pairActions.empty(),
          "Begin without Complete does not count, capture, log confirmation, or act");
    edvr::exposurePluginResetDispatchFrame(&state);
    check(state.detectStreak == 0 && state.seenThisFrame == 0 &&
              g_pairActions.empty(),
          "an incomplete dispatch cannot affect frame confirmation");

    const auto first = fakeViews(0x710);
    const auto second = fakeViews(0x720);
    bindUavs(first);
    const auto firstTicket = edvr::exposurePluginBeginDispatch(&state);
    bindUavs(first);
    edvr::exposurePluginCompleteDispatch(&state, firstTicket, context);
    bindUavs(second);
    const auto secondTicket = edvr::exposurePluginBeginDispatch(&state);
    state.announced = true;
    state.gaveUpNotice = true;
    bindUavs(second);
    edvr::exposurePluginCompleteDispatch(&state, secondTicket, context);
    check(g_pairActions.size() == 1,
          "pinned observer acts on confirmation even when log-state flags are already set");
}

void testResetContract() {
    resetServices();
    const auto heldViews = fakeViews(0x800);
    Observer state;
    state.detectStreak = edvr::plugins::exposure::kExposureConfirmFrames - 1;
    state.seenThisFrame = 2;
    std::copy(heldViews.begin(), heldViews.end(), state.firstEye);
    edvr::exposurePluginResetDispatchFrame(&state);
    check(state.detectStreak == edvr::plugins::exposure::kExposureConfirmFrames &&
              state.seenThisFrame == 0,
          "two completed target dispatches increment streak up to confirmation");
    for (View* view : state.firstEye)
        check(view == nullptr, "two-pass reset clears retained pointers");

    state.detectStreak = edvr::plugins::exposure::kExposureConfirmFrames;
    state.seenThisFrame = 2;
    edvr::exposurePluginResetDispatchFrame(&state);
    check(state.detectStreak == edvr::plugins::exposure::kExposureConfirmFrames,
          "two-pass streak remains capped at confirmation");

    for (uint32_t count : {1u, 3u}) {
        state.detectStreak = 4;
        state.seenThisFrame = count;
        std::copy(heldViews.begin(), heldViews.end(), state.firstEye);
        edvr::exposurePluginResetDispatchFrame(&state);
        check(state.detectStreak == 0 && state.seenThisFrame == 0,
              "one or three target dispatches reset the detection streak");
        for (View* view : state.firstEye)
            check(view == nullptr, "non-pair reset clears every retained pointer");
    }

    state.detectStreak = 3;
    state.seenThisFrame = 0;
    std::copy(heldViews.begin(), heldViews.end(), state.firstEye);
    edvr::exposurePluginResetDispatchFrame(&state);
    check(state.detectStreak == 3 && state.seenThisFrame == 0,
          "zero target dispatches retain the existing streak");
    for (View* view : state.firstEye)
        check(view == nullptr, "zero-pass reset still clears every pointer");
}

void testDetectedConfirmationTiming() {
    resetServices();
    Observer state;
    state.enabled = true;
    auto* shader = fakePointer<void>(0x901);
    bindShader(shader, 0xD001);
    g_shapeMatches = true;
    Context* context = fakePointer<Context>(0x799);

    for (uint32_t frame = 0; frame < edvr::plugins::exposure::kExposureConfirmFrames;
         ++frame) {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const auto ticket = edvr::exposurePluginBeginDispatch(&state);
            check(ticket.target == 1, "shape-positive candidate remains a target");
            bindUavs(fakeViews(0xA000 + frame * 0x100 + eye * 0x10));
            edvr::exposurePluginCompleteDispatch(&state, ticket, context);
        }
        check(g_pairActions.empty(),
              "detected candidate does not act before confirmation streak reaches five");
        edvr::exposurePluginResetDispatchFrame(&state);
        check(state.detectStreak == frame + 1,
              "two passes per frame advance the confirmation streak by one");
    }

    const auto firstTicket = edvr::exposurePluginBeginDispatch(&state);
    const auto firstViews = fakeViews(0xB000);
    bindUavs(firstViews);
    edvr::exposurePluginCompleteDispatch(&state, firstTicket, context);
    const auto secondTicket = edvr::exposurePluginBeginDispatch(&state);
    const auto secondViews = fakeViews(0xB100);
    bindUavs(secondViews);
    edvr::exposurePluginCompleteDispatch(&state, secondTicket, context);
    check(g_pairActions.size() == 1 && state.announced,
          "detected candidate acts at the old five-frame confirmation threshold");
    checkPair(0, &state, context, firstViews, secondViews,
              "confirmed pair bridge receives both post-forward UAV sets");
}
}  // namespace

namespace edvr {

namespace detail {
BindingSlot g_bindingSlots[static_cast<size_t>(BindSlot::Count)] = {};
}  // namespace detail

Config& Config::get() {
    static Config config;
    return config;
}

Log& Log::get() {
    static Log* log = new Log();
    return *log;
}

void Log::note(const char* fmt, ...) {
    char buffer[512] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_logLines.emplace_back(buffer);
}

uint64_t lookupShaderHash(void* shader) {
    const auto it = g_shaderHashes.find(shader);
    return it == g_shaderHashes.end() ? 0 : it->second;
}

void exposureDispatchApplyPair(void* state, ID3D11DeviceContext* context,
                               ID3D11UnorderedAccessView** first,
                               ID3D11UnorderedAccessView** second) {
    PairAction action;
    action.state = state;
    action.context = context;
    for (uint32_t i = 0; i < 4; ++i) {
        action.first[i] = first[i];
        action.second[i] = second[i];
    }
    g_pairActions.push_back(action);
}

}  // namespace edvr

namespace edvr::plugins::exposure {

bool shapeLooksLikeExposure() {
    ++g_shapeCalls;
    return g_shapeMatches;
}

}  // namespace edvr::plugins::exposure

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--dry-run" &&
                                  std::string(argv[1]) != "--self-test")) {
        std::puts("Usage: exposure_dispatch_test [--dry-run|--self-test]");
        return 2;
    }
    if (argc == 2 && std::string(argv[1]) == "--dry-run") {
        std::puts("DRY RUN: exposure observer validates the production dispatch module without D3D actions");
        return 0;
    }

    testDefaultsAndNullState();
    testClassificationGatesAndPinnedHash();
    testCachedShapeVerdictsAndExpiration();
    testNoCompleteAndTicketOnlyCompletion();
    testResetContract();
    testDetectedConfirmationTiming();
    std::printf("PASS: exposure_dispatch_test (%u checks)\n", g_checks);
    return 0;
}
