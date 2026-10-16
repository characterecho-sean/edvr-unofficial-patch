#pragma once

// Who owns a flat discovery window (src\d3d11\flat_owner_gate.h), driven from two
// real threads with logical thread ids (the OS reuses a finished thread's id, so
// the ids are passed in). The performance review F8 sequence: thread A makes the
// first DXGI_PRESENT_TEST Present and then never presents again; only thread B
// makes real Presents. With A as owner the deadline (owner-only) never ran and
// every observer call from B counted as foreign while collection stayed armed.
// The Session below is flatTemporalBeforePresent's control flow over the same
// helpers; the wiring scan in flatOwnerGateWiringTests holds the real function to
// those helpers.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include "../../src/d3d11/flat_capture_policy.h"
#include "../../src/d3d11/flat_temporal_model.h"
#include "../../src/d3d11/flat_owner_gate.h"

namespace flat_owner_gate_rig {

constexpr uint32_t kTest = edvr::kFlatPresentTestFlag;
constexpr uint32_t kA = 101, kB = 202;

struct Session {
    std::atomic<bool> waiting{false}, capturing{false}, paused{false};
    std::atomic<uint32_t> owner{0};
    std::atomic<uint64_t> foreign{0};
    uint64_t startedMs = 0, usefulFrames = 0;
    uint64_t budgetMs = 1000;
    uint32_t finished = 0;       // finishDiscovery calls

    // flatTemporalStart: a fresh window.
    void start() {
        capturing.store(false);
        owner.store(0);
        foreign.store(0);
        waiting.store(true);
        startedMs = 0;
    }
    // flatTemporalBeforePresent, down to the deadline.
    void beforePresent(uint32_t thread, uint32_t flags, uint64_t nowMs) {
        if (edvr::flatOwnerClaimOnPresent(waiting, owner, thread, flags)) {
            startedMs = nowMs;
            capturing.store(true);
        }
        if (edvr::flatOwnerMayExpire(capturing, owner, thread) &&
            edvr::flatCaptureExpired(startedMs, nowMs, static_cast<uint32_t>(usefulFrames), budgetMs, 1000000u)) {
            capturing.store(false);
            ++finished;
        }
    }
    // flatTemporalCapturing, for one observed draw or state call.
    bool observe(uint32_t thread) {
        if (!capturing.load()) return false;
        if (paused.load()) return false;
        return edvr::flatObserveAsOwner(owner, foreign, thread);
    }
};

// Run `body` on its own thread and wait: the hand-off the sequence needs, deterministic.
template<class F> void onThread(F&& body) { std::thread t(body); t.join(); }

}  // namespace flat_owner_gate_rig

inline int flatOwnerGateTests() {
    using namespace flat_owner_gate_rig;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: owner gate %s\n", name); ++failures; }
    };

    // ---- 1: the F8 sequence. A's only Present is a test; B presents and draws. ----
    {
        Session s; s.start();
        onThread([&] { s.beforePresent(kA, kTest, 10); });
        expect(s.owner.load() != kA, "1a a test Present does not make its thread the owner");
        expect(!s.capturing.load() && s.waiting.load(), "1b a test Present neither opens the window nor consumes the wait");
        onThread([&] {
            s.beforePresent(kB, 0, 20);
            expect(s.owner.load() == kB && s.capturing.load(), "1c the first real Present owns the window");
            for (int i = 0; i < 100; ++i) expect(s.observe(kB), "1d the owner observes");
            s.beforePresent(kB, 0, 20 + s.budgetMs - 1);
            expect(s.capturing.load(), "1e one millisecond short of the deadline the window is still open");
            s.beforePresent(kB, 0, 20 + s.budgetMs);
        });
        expect(!s.capturing.load() && s.finished == 1, "1f the deadline closes the window from the real thread, once");
        const uint64_t foreignAtExpiry = s.foreign.load();
        expect(foreignAtExpiry == 0, "1g B was never counted foreign");
        for (int i = 0; i < 100; ++i) { s.observe(kA); s.observe(kB); }
        s.beforePresent(kB, 0, 20 + 2 * s.budgetMs);
        s.beforePresent(kA, kTest, 20 + 2 * s.budgetMs);
        expect(s.foreign.load() == foreignAtExpiry && s.finished == 1,
               "1h after expiry the foreign counter is stable and nothing finishes twice");
    }

    // ---- 2: control: a real foreign thread during capture is still counted, and a test Present from it changes nothing ----
    {
        Session s; s.start();
        s.beforePresent(kB, 0, 5);
        s.beforePresent(kA, kTest, 6);
        expect(s.owner.load() == kB && s.capturing.load(), "2a a test Present from another thread leaves the owner alone");
        expect(!s.observe(kA) && s.foreign.load() == 1, "2b a foreign observer call is refused and counted once");
        s.beforePresent(kA, 0, 5 + s.budgetMs);
        expect(s.capturing.load() && s.finished == 0, "2c a foreign thread's Present cannot close the owner's window");
    }

    // ---- 3: several test Presents, from several threads, before the first real one ----
    {
        Session s; s.start();
        for (uint32_t t : {kA, kB, 303u, kA}) s.beforePresent(t, kTest, 1);
        expect(s.owner.load() == 0 && !s.capturing.load() && s.waiting.load(), "3a any number of test Presents leave the window unclaimed");
        s.beforePresent(303u, kTest | 0x4u, 2);   // test flag among others
        expect(s.owner.load() == 0, "3b the test flag is recognised among other Present flags");
        s.beforePresent(303u, 0x4u, 3);           // DXGI_PRESENT_DO_NOT_SEQUENCE alone is a real Present
        expect(s.owner.load() == 303u && s.capturing.load(), "3c a real Present with other flags owns the window");
    }

    // ---- 4: paused/no-draw: no draws are observed, the deadline still closes the window ----
    {
        Session s; s.start();
        s.beforePresent(kA, kTest, 1);
        s.beforePresent(kB, 0, 2);
        s.paused.store(true);
        for (int i = 0; i < 50; ++i) { s.observe(kA); s.observe(kB); }
        expect(s.foreign.load() == 0, "4a a paused window counts no foreign call: it observes nothing");
        s.beforePresent(kB, 0, 2 + s.budgetMs);
        expect(!s.capturing.load() && s.finished == 1, "4b the paused, drawless window still expires at the owner's Present");
        s.paused.store(false);
        for (int i = 0; i < 50; ++i) { s.observe(kA); s.observe(kB); }
        expect(s.foreign.load() == 0, "4c unpausing after expiry counts nothing");
    }

    // ---- 5: rearm: a fresh window repeats the whole sequence and resets the counter ----
    {
        Session s; s.start();
        s.beforePresent(kB, 0, 0);
        s.observe(kA);
        expect(s.foreign.load() == 1, "5a (setup) one foreign call in the first window");
        s.start();                                  // flatTemporalArm -> flatTemporalStart
        expect(s.foreign.load() == 0 && s.owner.load() == 0 && s.waiting.load(), "5b rearming clears owner and counter and waits again");
        onThread([&] { s.beforePresent(kA, kTest, 100); });
        expect(s.owner.load() == 0 && !s.capturing.load(), "5c the test Present of the new window claims nothing");
        s.beforePresent(kB, 0, 110);
        expect(s.owner.load() == kB && s.capturing.load(), "5d the real thread owns the new window");
        s.beforePresent(kB, 0, 110 + s.budgetMs);
        expect(!s.capturing.load() && s.finished == 1 && s.foreign.load() == 0, "5e the new window expires with the counter at zero");
    }

    // ---- 6: a rearm while the old window is open (the key pressed mid-capture) ----
    {
        Session s; s.start();
        s.beforePresent(kB, 0, 0);
        s.start();
        s.beforePresent(kA, kTest, 1);
        s.beforePresent(kB, 0, 2);
        s.beforePresent(kB, 0, 2 + s.budgetMs);
        expect(s.finished == 1 && !s.capturing.load(), "6 a mid-window rearm expires normally on the real thread");
    }
    return failures;
}

// The real flatTemporalBeforePresent, held to the helpers the Session above runs.
inline int flatOwnerGateWiringTests() {
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: owner gate wiring %s\n", name); ++failures; }
    };
    auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string cpp = slurp("src/d3d11/flat_temporal.cpp");
    const std::string hdr = slurp("src/d3d11/flat_temporal.h");
    expect(!cpp.empty() && !hdr.empty(), "sources are readable from the repo root");
    const size_t at = cpp.find("void flatTemporalBeforePresent(");
    const size_t end = cpp.find("void flatTemporalAfterPresent(", at);
    expect(at != std::string::npos && end > at, "the function can be delimited");
    const std::string body = cpp.substr(at, end - at);
    expect(body.find("flatOwnerClaimOnPresent(g_waitingForPresent, detail::g_flatTemporalOwnerThread, thread, flags)") != std::string::npos,
           "ownership is claimed through the helper, with the Present flags");
    expect(body.find("g_waitingForPresent.exchange") == std::string::npos &&
           body.find("g_flatTemporalOwnerThread.store") == std::string::npos,
           "no second, flag-blind way to claim ownership");
    expect(body.find("flatOwnerMayExpire(detail::g_flatTemporalCapturing, detail::g_flatTemporalOwnerThread, thread)") != std::string::npos,
           "the deadline runs through the helper");
    expect(hdr.find("flatObserveAsOwner(detail::g_flatTemporalOwnerThread") != std::string::npos,
           "the observers' foreign-thread count runs through the helper");
    return failures;
}
