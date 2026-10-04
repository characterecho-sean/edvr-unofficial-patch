// The flat HDR route's crash-safe breadcrumbs on the rig (src\d3d11\flat_hdr_crumbs.h): what the gate, the frame budget and
// the crumb budget do; what one frame's trail looks like; and, as source pins, where the crumbs sit in the runtime, the
// resolver and the backends, in the order the steps run. The resolver itself is exercised on WARP by flat_mono_resolve_test
// (flat_hdr_route_gpu_tests.h, section 9); what no rig can run is the game's Present, the runtime's draw scope and the two
// SDKs, so those are read as source.
//
// The rig's breadcrumb() collects the lines the route would have put in edvr_breadcrumbs.txt. The real one's own
// behaviour (open, one WriteFile, close, per line) is src\common\proxy.cpp's and is not stood in for here.
#pragma once
#include "../../src/d3d11/flat_hdr_crumbs.h"
#include "../hdr_crumb_trail.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace hdr_crumb_rig {
inline std::vector<std::string>& lines() { static std::vector<std::string> v; return v; }
}  // namespace hdr_crumb_rig
namespace edvr {
// Test stand-in for src\common\proxy.cpp's breadcrumb(): keeps each line, in order.
void breadcrumb(const char* stage) { hdr_crumb_rig::lines().emplace_back(stage ? stage : ""); }
}  // namespace edvr

inline int flatHdrCrumbTests() {
    using namespace edvr;
    namespace t = hdr_crumb_trail;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: hdr crumbs %s\n", name); ++failures; }
    };
    auto& out = hdr_crumb_rig::lines();
    // A new session on a DXMT device: the device gate (flat_hdr_crumbs.h, THE GATE) open, which only the runtime does, from the markers.
    // Every section below but the gate's own is the DXMT case.
    const auto fresh = [&] { hdrCrumbReset(); hdrCrumbEnable(true); out.clear(); };
    const auto written = [&] { return g_hdrCrumbs.written.load(); };
    // One frame as the runtime drives it: the route admits it, it reaches the resolver (or is declined), a step with another
    // inside it, then the Present's end.
    const auto frame = [&](uint64_t n, bool reach) {
        hdrCrumbAdmit(n, "dlss");
        if (reach) {
            hdrCrumbReach(n, "dlss", "resolve");
            { HdrCrumbSpan copy(true, "copy-h", "size=%ux%u", 16u, 16u); copy.close(); }
            {
                HdrCrumbSpan backend(true, "backend", "mode=dlss");
                { HdrCrumbSpan evaluate(true, "backend-evaluate"); evaluate.result("ngx=0x%08X", 1u); }
                backend.result("ok=%u", 1u);
            }
        } else {
            hdrCrumbDeclined("engine-source-not-ready");
        }
        { HdrCrumbFrameEnd end(0); }
    };

    // ---- one normal frame --------------------------------------------------------------------------------------
    fresh();
    frame(100, true);
    {
        const auto trail = t::trail(out);
        std::string why;
        expect(trail.size() == 10 && trail.size() == out.size(), "a normal frame writes ten crumbs, every one of them the route's");
        expect(t::balanced(trail, &why), "every begin has its end, nested like the steps");
        const int admitted = t::find(trail, "admitted", ""), reached = t::find(trail, "reached", ""),
                  copy = t::find(trail, "copy-h", "begin"), copyEnd = t::find(trail, "copy-h", "end"),
                  backend = t::find(trail, "backend", "begin"), evaluate = t::find(trail, "backend-evaluate", "begin"),
                  evaluateEnd = t::find(trail, "backend-evaluate", "end"), backendEnd = t::find(trail, "backend", "end"),
                  endBegin = t::find(trail, "frame-end", "begin"), endEnd = t::find(trail, "frame-end", "end");
        expect(admitted == 0 && reached == 1 && copy == 2 && copyEnd == 3 && backend == 4 && evaluate == 5 && evaluateEnd == 6 &&
                   backendEnd == 7 && endBegin == 8 && endEnd == 9,
               "the crumbs come in the order the steps run, from the admission to the frame's end");
        bool oneNumber = true;
        for (const auto& c : trail) oneNumber = oneNumber && c.slot == 1;
        expect(oneNumber, "every crumb of the first frame to reach the resolver reads 1/3");
        expect(trail[0].detail == "frame=100 backend=dlss" && trail[1].detail == "frame=100 backend=dlss step=resolve",
               "the admission and the reach name the frame and the backend");
        expect(trail[6].detail == "ngx=0x00000001" && trail[2].detail == "size=16x16" && trail[8].detail == "hr=0x00000000",
               "an end carries its result, a begin its parameters, the frame's end the Present's result");
        expect(!hdrCrumbLive() && !hdrCrumbPresentSide(), "the frame's end closes the gate");
        expect(out[0].compare(0, 15, "gfx: hdr-treat ") == 0 && out[0] == "gfx: hdr-treat 1/3 admitted frame=100 backend=dlss",
               "the line is 'gfx: hdr-treat K/3 <step> ...' exactly");
    }

    // ---- the gate, inside a frame --------------------------------------------------------------------------------
    fresh();
    {
        expect(!hdrCrumbLive(), "a new session is silent");
        { HdrCrumbSpan idle(true, "prep"); expect(!idle.armed(), "a step before any frame is admitted writes nothing"); }
        { HdrCrumbFrameEnd end(0); }
        expect(out.empty() && written() == 0, "a Present with no frame in progress writes nothing and counts nothing");
        hdrCrumbAdmit(7, "fsr");
        expect(hdrCrumbLive() && !hdrCrumbPresentSide(), "an admitted frame is live; the steps after the treatment wait for it to reach the resolver");
        {
            HdrCrumbSpan copyRoute(false, "copy-h");
            expect(!copyRoute.armed(), "a call that is not the route's writes nothing even while the route's frame is live");
        }
        hdrCrumbReach(7, "fsr", "spatial-recovery");
        hdrCrumbReach(7, "fsr", "resolve");
        expect(hdrCrumbPresentSide() && t::count(t::trail(out), "reached", "") == 1, "a frame reaches the resolver once, whatever asks twice");
        { HdrCrumbFrameEnd end(0); }
    }

    // ---- the armed line: the one line that tells a trail from no trail ---------------------------------------------------
    fresh();
    expect(hdrCrumbArmed("auto"), "the first armed line is written");
    {
        const std::string want = std::string("gfx: hdr-treat armed key=auto frames=") + std::to_string(kHdrCrumbFrames) +
                                 " declined=" + std::to_string(kHdrCrumbDeclined) + " cap=" + std::to_string(kHdrCrumbCap);
        const auto trail = t::trail(out);
        expect(out.size() == 1 && out[0] == want && trail.size() == 1 && trail[0].step == "armed" && written() == 0 && !hdrCrumbLive(),
               "the armed line says the route is on and what the trail will hold, costs none of the budget and opens no frame");
        const bool second = hdrCrumbArmed("auto"), third = hdrCrumbArmed("auto"), fourth = hdrCrumbArmed("auto");
        expect(out.size() == 3 && second && third && !fourth,
               "it is written three times a session at most (and says whether it wrote): a key flipped back and forth does not fill the trail");
        frame(1, true);
        expect(t::balanced(t::trail(out)) && out.size() == 13, "and a frame after it writes what it always wrote");
    }

    // ---- three frames, then nothing --------------------------------------------------------------------------------
    fresh();
    for (uint64_t n = 1; n <= 3; ++n) frame(n, true);
    const size_t afterThree = out.size();
    const uint32_t countedThree = written();
    {
        const auto trail = t::trail(out);
        std::string why;
        unsigned slots[3] = {0, 0, 0};
        for (const auto& c : trail) if (c.step == "reached") slots[c.slot - 1 < 3 ? c.slot - 1 : 0] += 1;
        expect(t::balanced(trail, &why) && afterThree == 30, "three normal frames write thirty crumbs and every begin has its end");
        expect(slots[0] == 1 && slots[1] == 1 && slots[2] == 1 && t::find(trail, "reached", "") >= 0,
               "the three frames are numbered 1/3, 2/3 and 3/3");
        bool noFourth = true;
        for (const auto& c : trail) noFourth = noFourth && c.slot >= 1 && c.slot <= 3;
        expect(noFourth, "no crumb is numbered past 3/3");
    }
    for (uint64_t n = 4; n <= 40; ++n) frame(n, true);
    {
        HdrCrumbSpan late(true, "prep", "groups=%ux%u", 1u, 1u);
        expect(!late.armed(), "a step after the third frame is not armed");
    }
    expect(out.size() == afterThree && written() == countedThree && !hdrCrumbLive(),
           "after the third frame nothing is written and nothing is counted: a frame costs one branch");

    // ---- a declined frame does not use one of the three --------------------------------------------------------------
    fresh();
    frame(1, false);
    frame(2, false);
    for (uint64_t n = 3; n <= 5; ++n) frame(n, true);
    frame(6, true);
    {
        const auto trail = t::trail(out);
        std::string why;
        expect(t::balanced(trail, &why), "declined frames leave no open step");
        expect(t::count(trail, "declined", "") == 2 && t::count(trail, "reached", "") == 3 && t::count(trail, "admitted", "") == 5 &&
                   t::count(trail, "frame-end", "begin") == 5,
               "two declined frames, then the three that reach the resolver, then silence: five frame ends, none for the sixth");
        unsigned reachedSlot[3] = {0, 0, 0}, n = 0;
        for (const auto& c : trail) if (c.step == "reached" && n < 3) reachedSlot[n++] = c.slot;
        expect(reachedSlot[0] == 1 && reachedSlot[1] == 2 && reachedSlot[2] == 3,
               "the frames that reach the resolver are 1/3, 2/3 and 3/3 however many declined before them");
        const int first = t::find(trail, "admitted", "");
        expect(first == 0 && trail[0].slot == 1 && trail[1].step == "declined" && trail[1].slot == 1 &&
                   trail[1].detail == "why=engine-source-not-ready",
               "a declined frame is numbered with the number it would have had, and names why");
    }

    // ---- a long run of declines cannot spend the trail -------------------------------------------------------------
    fresh();
    for (uint64_t n = 1; n <= 12; ++n) frame(n, false);
    {
        const auto trail = t::trail(out);
        expect(t::count(trail, "declined", "") == kHdrCrumbDeclined && t::count(trail, "frame-end", "begin") == kHdrCrumbDeclined,
               "only the first few declined frames of a session write, so a run of them leaves the budget for the treatment");
        const size_t before = out.size();
        frame(13, true);
        const auto later = t::trail(out);
        expect(out.size() > before && t::find(later, "reached", "", 0) >= 0 && t::count(later, "admitted", "") == kHdrCrumbDeclined &&
                   t::count(later, "frame-end", "begin") == kHdrCrumbDeclined + 1,
               "a frame that reaches the resolver after the declines wrote their allowance writes from its reach, its frame end included");
        expect(t::balanced(later), "and it balances");
    }

    // ---- the crumb budget --------------------------------------------------------------------------------------------
    fresh();
    hdrCrumbAdmit(1, "dlss");
    hdrCrumbReach(1, "dlss", "resolve");
    for (int i = 0; i < 1000; ++i) { HdrCrumbSpan spin(true, "spin"); }
    {
        const auto trail = t::trail(out);
        expect(out.size() == kHdrCrumbCap && trail.back().budgetLine && !hdrCrumbLive() && g_hdrCrumbs.spent,
               "a session writes at most kHdrCrumbCap crumbs, the last of them the line that says the budget is spent");
        const size_t count = out.size();
        { HdrCrumbSpan more(true, "prep"); expect(!more.armed(), "nothing arms after the budget"); }
        hdrCrumbAdmit(2, "dlss");
        hdrCrumbReach(2, "dlss", "resolve");
        { HdrCrumbFrameEnd end(0); }
        expect(out.size() == count && !hdrCrumbLive(), "an admission after the budget writes nothing and opens nothing");
    }

    // ---- the longest line the route writes fits the breadcrumb writer --------------------------------------------------
    // proxy.cpp's line is 256 bytes with the stamp (up to 20 digits), a space and the CRLF in front and behind; ours is 231.
    fresh();
    hdrCrumbAdmit(1, "dlss");
    hdrCrumbReach(1, "dlss", "resolve");
    {
        HdrCrumbSpan wide(true, "create-texture", "role=%s fmt=%s(%u) size=%ux%u uav=%u", "fsr-previous-nearest-depth",
                          "R32_FLOAT_X8X24_TYPELESS", 21u, 16384u, 16384u, 1u);
        wide.result("hr=0x%08X srv=0x%08X uav=0x%08X srgb=0x%08X", 0x80070057u, 0x80070057u, 0x80070057u, 0x80070057u);
    }
    {
        HdrCrumbSpan overlong(true, "create-texture", "%s%s%s", std::string(150, 'x').c_str(), std::string(150, 'y').c_str(), "z");
        overlong.result("%s", std::string(300, 'r').c_str());
    }
    {
        size_t longest = 0;
        for (const auto& line : out) longest = line.size() > longest ? line.size() : longest;
        expect(longest <= 231, "no crumb is longer than 231 characters, whatever a detail asks for");
        expect(t::balanced(t::trail(out)), "a cut detail still leaves a balanced trail");
    }

    // ---- the crumb numbers the frames of a session from the first ------------------------------------------------------
    fresh();
    expect(hdrCrumbSlot() == 1, "a session's first frame reads 1/3 before it has reached the resolver");
    hdrCrumbAdmit(1, "dlss");
    hdrCrumbReach(1, "dlss", "resolve");
    expect(hdrCrumbSlot() == 1, "and once it has");
    { HdrCrumbFrameEnd end(0); }
    hdrCrumbAdmit(2, "dlss");
    expect(hdrCrumbSlot() == 2, "the next reads 2/3 from its admission");
    hdrCrumbReset();

    // ---- the explicit capture's per-group crumbs: once a session each way, for the first call made while a frame writes ----
    fresh();
    expect(!hdrCrumbFirstCapture(true) && !hdrCrumbFirstRestore(true) && !g_hdrCrumbs.captureCrumbed && !g_hdrCrumbs.restoreCrumbed,
           "with no frame writing, neither first-use gate opens, and neither is spent");
    hdrCrumbAdmit(1, "dlss");
    expect(!hdrCrumbFirstCapture(false) && !g_hdrCrumbs.captureCrumbed,
           "a call that is not the route's (the resolver's own flag) never opens the gate, and does not spend it");
    expect(hdrCrumbFirstCapture(true) && !hdrCrumbFirstCapture(true) && g_hdrCrumbs.captureCrumbed && !g_hdrCrumbs.restoreCrumbed,
           "the capture's gate opens once while a frame writes, and the restore's is its own");
    expect(hdrCrumbFirstRestore(true) && !hdrCrumbFirstRestore(true), "the restore's gate opens once as well");
    { HdrCrumbFrameEnd end(0); }
    hdrCrumbAdmit(2, "dlss");
    expect(!hdrCrumbFirstCapture(true) && !hdrCrumbFirstRestore(true), "and stays shut for the rest of the session, a later frame included");
    hdrCrumbReset();
    hdrCrumbAdmit(3, "dlss");
    expect(hdrCrumbFirstCapture(true) && hdrCrumbFirstRestore(true), "a new session opens both again");
    hdrCrumbReset();
    // The budget holds the worst session (DXMT's): three reaching frames, two declined ones, the preflight (about 115) and the
    // explicit capture's 44 group crumbs, with room over.
    expect(kHdrCrumbCap >= 115 + 44 + 16, "the crumb budget holds a session that also writes the explicit capture's eleven groups each way");

    // ---- THE DEVICE GATE: DXMT only. Shut, which is every process the runtime has not told otherwise (every Windows one), the route ----
    // ---- writes not one crumb, counts nothing and opens nothing; open, it is the route that was written above ------------------
    hdrCrumbReset();
    hdrCrumbEnable(false);
    out.clear();
    expect(!hdrCrumbEnabled(), "the gate is shut when it is shut");
    expect(!hdrCrumbArmed("auto") && out.empty() && g_hdrCrumbs.armedSaid == 0,
           "shut: the armed line, the one line of the trail that is not a frame's, is not written and not counted");
    for (uint64_t n = 1; n <= 8; ++n) frame(n, n % 2 == 0);   // admitted and declined, admitted and reached, steps, frame ends
    {
        HdrCrumbSpan idle(true, "prep", "groups=%ux%u", 1u, 1u);
        expect(!idle.armed(), "shut: a step, the route's own flag on, is not armed");
        // The Present hook's pair (device_hook.cpp) and the explicit capture's first-use gates, called as they are called.
        hdrCrumbWrite("present", "begin");
        hdrCrumbWrite("present", "end", "hr=0x%08X removed=0x%08X", 0u, 0u);
        hdrCrumbWrite("capture-ia", "begin");
        hdrCrumbDeclined("engine-source-not-ready");
        expect(!hdrCrumbFirstCapture(true) && !hdrCrumbFirstRestore(true) && !g_hdrCrumbs.captureCrumbed && !g_hdrCrumbs.restoreCrumbed,
               "shut: the explicit capture's per-group gates stay shut and unspent");
    }
    expect(out.empty() && written() == 0 && !hdrCrumbLive() && !hdrCrumbPresentSide() && g_hdrCrumbs.reached == 0 &&
               g_hdrCrumbs.declinedFrames == 0 && !g_hdrCrumbs.frameReached && !g_hdrCrumbs.spent,
           "shut: eight frames, a Present pair and a declined step write no crumb, count none against the budget, and open no frame");
    {
        // Even the budget's own stress writes nothing: a thousand spans inside a frame that cannot open.
        hdrCrumbAdmit(1, "dlss");
        hdrCrumbReach(1, "dlss", "resolve");
        for (int i = 0; i < 1000; ++i) { HdrCrumbSpan spin(true, "spin"); }
        { HdrCrumbFrameEnd end(0); }
        expect(out.empty() && written() == 0 && !g_hdrCrumbs.spent, "shut: a thousand spans in an admitted frame write nothing and never start the budget");
    }
    // Open, it is the route that was always written, the armed line and the budget included.
    hdrCrumbEnable(true);
    expect(hdrCrumbEnabled() && hdrCrumbArmed("auto") && out.size() == 1 && out[0].find("gfx: hdr-treat armed key=auto") == 0,
           "open: the armed line is written");
    frame(100, true);
    expect(t::balanced(t::trail(out)) && out.size() == 11 && t::count(t::trail(out), "reached", "") == 1,
           "open: a frame writes what it always wrote (ten crumbs and the armed line)");
    // Shutting it inside a frame ends the frame: no step after it writes, and the frame's end closes nothing and writes nothing.
    hdrCrumbReset();
    out.clear();
    hdrCrumbAdmit(1, "dlss");
    hdrCrumbReach(1, "dlss", "resolve");
    expect(hdrCrumbLive() && hdrCrumbPresentSide() && !out.empty(), "open: the frame is live and has written its admission and reach");
    const size_t beforeShut = out.size();
    hdrCrumbEnable(false);
    {
        HdrCrumbSpan after(true, "prep");
        hdrCrumbWrite("present", "begin");
        expect(!hdrCrumbLive() && !hdrCrumbPresentSide() && !after.armed(), "shutting the gate inside a frame ends it: nothing after it is armed");
    }
    { HdrCrumbFrameEnd end(0); }
    expect(out.size() == beforeShut, "and nothing more is written, the frame's end included");
    // A session reset is not the gate: it says which device this is.
    hdrCrumbEnable(true);
    hdrCrumbReset();
    expect(hdrCrumbEnabled(), "a session reset leaves an open gate open");
    hdrCrumbEnable(false);
    hdrCrumbReset();
    expect(!hdrCrumbEnabled(), "and a shut one shut");
    return failures;
}

// The wiring no rig can run, read as source: where the crumbs sit in the runtime, the resolver, the backends and the Present
// hook, in the order the steps run. A step a crumb is placed after is a step the crumb does not name; a crumb after the call
// it brackets is a crumb that never gets written when that call ends the process, which is all it is for.
inline int flatHdrCrumbWiringTests() {
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: hdr crumb wiring %s\n", name); ++failures; }
    };
    const auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    // The text of one function: from its signature to the closing brace at the start of a line.
    const auto body = [](const std::string& text, const char* signature) {
        const size_t at = text.find(signature);
        if (at == std::string::npos) return std::string();
        const size_t end = text.find("\n}\n", at);
        return text.substr(at, end == std::string::npos ? std::string::npos : end + 3 - at);
    };
    // The needles occur in this order (each after the one before), each once at least.
    const auto ordered = [&](const std::string& text, std::initializer_list<const char*> needles, const char* name) {
        size_t pos = 0;
        bool ok = !text.empty();
        for (const char* needle : needles) {
            const size_t at = text.find(needle, pos);
            if (at == std::string::npos) {
                std::printf("  (hdr crumb wiring: '%s' not found after offset %zu for: %s)\n", needle, pos, name);
                ok = false;
                break;
            }
            pos = at + std::strlen(needle);
        }
        expect(ok, name);
    };
    const auto count = [](const std::string& text, const char* needle) {
        size_t n = 0, at = 0;
        const std::string s(needle);
        while ((at = text.find(s, at)) != std::string::npos) { ++n; at += s.size(); }
        return n;
    };

    const std::string runtime = slurp("src/d3d11/flat_runtime.cpp");
    const std::string resolve = slurp("src/d3d11/flat_mono_resolve.cpp");
    const std::string dlaa = slurp("src/d3d11/dlaa.cpp");
    const std::string fsr = slurp("src/d3d11/fsr3_engine.cpp");
    const std::string hook = slurp("src/d3d11/device_hook.cpp");
    const std::string vscreen = slurp("src/d3d11/vscreen.cpp");
    expect(!runtime.empty() && !resolve.empty() && !dlaa.empty() && !fsr.empty() && !hook.empty() && !vscreen.empty(),
           "the runtime, resolver, backend and hook sources are readable from the repo root");
    const std::string omUav = body(vscreen, "void STDMETHODCALLTYPE hookedOMSetRtvAndUav(");
    ordered(omUav, {"flatRuntimeOverlayUavBind(self,uavCount,uavs);", "if (foreignContext(self))",
                    "g_state->realOMSetRtvAndUav(self, n, rtvs, dsv, uavStart, uavCount, uavs,"},
            "a PS UAV bind invalidates an open overlay suffix before the game's bind, including foreign and KEEP-target paths");
    expect(vscreen.find("s.hook.replace(kSlotOMSetRtvAndUav, &hookedOMSetRtvAndUav,") != std::string::npos,
           "the OM UAV hook with overlay suffix guard is installed");

    // -- the runtime's draw scope: admission first, the reach after the last decline that precedes the resolver --
    const std::string treat = body(runtime, "void FlatRuntimeDrawScope::treatHdr(");
    ordered(treat, {"auto& s = state();", "const FlatMonoResolveMode effectiveMode=selected.mixedCamera?FlatMonoResolveMode::Taa:s.engine;",
                    "hdrCrumbAdmit(s.prefix.frame, flatMonoResolveModeName(effectiveMode));", "++s.hdrWindow.steps.admitted;",
                    "const auto reach = [&](const char* step) {", "hdrCrumbReach(s.prefix.frame, flatMonoResolveModeName(effectiveMode), step);",
                    "const auto decline = [&](const char* why) {", "hdrCrumbDeclined(why);", "if (s.hdrLatch.tripped) { decline(\"latched-off\"); return; }"},
            "treatHdr admits the frame before any decline, counts it, and every decline writes its reason");
    ordered(treat, {"const auto recoverHdr = [&]", "reach(\"spatial-recovery\");", "failPhase(s, temporalReason);", "flatMonoResolveSpatialFallback("},
            "the spatial recovery's frame is numbered before the recovery touches the context");
    ordered(treat, {"engineVelocitySourceViews(", "decline(\"engine-source-not-ready\")", "reach(\"resolve\");",
                    "if (!flatMonoResolve(s.device.Get(), ctx, f, &outputView, &s.reason)) {"},
            "a frame declined for engine-source-not-ready has not reached the resolver; one that goes on is numbered just before it");
    expect(count(treat, "hdrCrumbAdmit(") == 1 && count(treat, "hdrCrumbReach(") == 1 && count(treat, "reach(\"") == 2 &&
               count(treat, "++s.hdrWindow.steps.reached;") == 1 && count(treat, "hdrCrumbDeclined(") == 1,
           "treatHdr admits once, declines through one place and reaches the resolver at its two calls, counting the frame once");
    // -- the route's key and its 5 s census --
    const std::string readKey = body(runtime, "static void hdrReadKey(State& s, uint64_t frame) {");
    ordered(readKey, {"s.hdrKey = key; s.hdrKeyRead = true;", "if (key == FlatHdrKey::Auto && hdrCrumbArmed(flatHdrKeyName(key)))",
                      "Log::get().note(\"flat hdr route: crash-safe trail on:", "Log::get().note(\"flat hdr route: experimental.temporal_aa_before_post="},
            "the trail's armed line, and the log's line that says where to look, are written when the key is read as auto, at startup or after a change");
    ordered(body(runtime, "void flatRuntimePresent(IDXGISwapChain* swap, uint64_t frame, HRESULT hr, UINT flags) {"),
            {"const FlatMonoResolveStats rs = flatMonoResolveStats();", "gained.captured = rs.hdrCaptured - seen.captured;",
             "gained.copied = rs.hdrCopied - seen.copied;", "gained.prepped = rs.hdrPrepped - seen.prepped;", "gained.backend = rs.hdrBackend - seen.backend;",
             "gained.finished = rs.hdrFinished - seen.finished;", "gained.restored = rs.hdrRestored - seen.restored;",
             "flatHdrFormatWindow(hdrText, sizeof(hdrText), s.hdrKey,", "s.hdrWindow.reset();"},
            "the 5 s line prints the resolver's step counts as what they gained since the last window, then the window resets");
    // -- the frame's end: the guard wraps everything the Present does, the pre-Present span the work before the real call --
    const std::string present = body(runtime, "void flatRuntimePresent(IDXGISwapChain* swap, uint64_t frame, HRESULT hr, UINT flags) {");
    ordered(present, {"if (s.thread && !owner()) return;", "HdrCrumbFrameEnd routeFrameEnd(hr);", "s.thread = GetCurrentThreadId();",
                      "hdrFrameEnd(s, frame);", "flatMonoResolvePreflight("},
            "flatRuntimePresent's frame end is open before anything it does, the preflight included");
    const std::string before = body(runtime, "void flatRuntimeBeforePresent() {");
    ordered(before, {"HdrCrumbSpan routeBeforePresent(hdrCrumbPresentSide(), \"before-present\");",
                     "flatRuntimeSubstitution(state().context.Get(), FlatSubstEvent::kPresent);", "engineVelocityFlatFrameEnd();",
                     "gpuFrameClose(state());"},
            "the pre-Present span opens before engine motion's state goes back and the census span closes");
    const std::string depth = body(runtime, "bool depthView(ID3D11Texture2D* depth) {");
    ordered(depth, {"\"create-depth-srv\"", "s.device->CreateShaderResourceView(depth, &v, &s.depthView);", "FAILED(hr)"},
            "the scene depth's shader view is crumbed around its creation");
    // -- the real Present ---------------------------------------------------------------------------------------------------
    const std::string hooked = body(hook, "HRESULT STDMETHODCALLTYPE hookedPresent(");
    ordered(hooked, {"const bool routeCrumbs = hdrCrumbPresentSide();", "hdrCrumbWrite(\"present\", \"begin\");", "const int64_t presentT0 = qpcNow();",
                     "g_state->realPresent(self, syncInterval, flags);", "const int64_t presentT1 = qpcNow();",
                     "hdrCrumbWrite(\"present\", \"end\", \"hr=0x%08X removed=0x%08X\"", "flatRuntimePresent(self, g_state->frameCounter, hr, flags);"},
            "the real Present is bracketed by its crumbs, outside the clock reads that time it, and before the runtime's frame end");

    // -- the resolver ------------------------------------------------------------------------------------------------------------
    expect(count(resolve, "CrumbScope crumbs(f.hdr);") == 2 && count(resolve, "CrumbScope crumbs(planned.hdr);") == 1,
           "the resolver's three entry points (resolve, spatial recovery, preflight) each open the route's scope from their own hdr bit");
    const std::string solve = body(resolve, "bool flatMonoResolve(ID3D11Device* device,ID3D11DeviceContext* context,const FlatMonoResolveFrame& f,");
    ordered(solve, {"CrumbScope crumbs(f.hdr);", "initialize(device,context,reason)", "initializeHdr(device,reason)", "resources(f,reason)",
                    "hdrTargetView(color.Get(),reason)", "Isolate isolated(", "backendAvailable(f.mode,device,reason)", "HdrCrumbSpan copyStep(",
                    "SpanGuard span(context);", "context->CopyResource(g.color.texture.Get(),overlay?cleanColor.Get():color.Get());",
                    "if(overlay) context->CopyResource(g.rawOverlay.texture.Get(),color.Get());", "copyStep.close();",
                    "HdrCrumbSpan prepStep(", "context->Dispatch((f.renderWidth+7)/8,(f.renderHeight+7)/8,1);", "prepStep.close();",
                    "HdrCrumbSpan backendStep(", "ok=fsr3Evaluate(", "ok=dlaaEvaluate(", "backendStep.close();",
                    "drawHdrTarget(context,g.finishHdr.Get(),f.renderWidth,f.renderHeight,views,overlay?13:8);"},
            "the resolve writes capture, copy, prep, backend and finish in the order it runs them, each crumb before its call");
    const std::string spatial = body(resolve, "bool flatMonoResolveSpatialFallback(ID3D11Device* device,ID3D11DeviceContext* context,const FlatMonoResolveFrame& f,");
    ordered(spatial, {"CrumbScope crumbs(f.hdr);", "Isolate isolated(", "HdrCrumbSpan copyStep(", "context->CopyResource(g.color.texture.Get(),color.Get());",
                      "drawHdrTarget(context,g.spatialHdr.Get(),f.renderWidth,f.renderHeight,views,1);"},
            "the spatial recovery writes the same capture, copy and finish crumbs around the same calls");
    const std::string isolate = body(resolve, "struct Isolate {");
    ordered(isolate, {"HdrCrumbSpan capture(g_crumbOn,\"capture-state\",\"by=%s\"", "if(byCapture) {", "g_contextBlock.capture(context,g.ranges,hdrCrumbFirstCapture(g_crumbOn));",
                      "} else {", "context->SwapDeviceContextState(state, previous.GetAddressOf());", "context->ClearState();", "if(g_hdrCall)++stats.hdrCaptured;",
                      "~Isolate() {", "HdrCrumbSpan restore(g_crumbOn,\"restore-state\",\"by=%s\"", "context->ClearState();",
                      "if(byCapture)g_contextBlock.restore(context,hdrCrumbFirstRestore(g_crumbOn));", "else context->SwapDeviceContextState(previous.Get(), nullptr);",
                      "if(g_hdrCall)++stats.hdrRestored;"},
            "the game's state is crumbed going out and coming back, by the swap or the explicit capture, the crumb before the calls, ClearState between the two halves "
            "either way, and counted for the 5 s line after them");
    // The swap is what it was: one call out and one back in the whole resolver, in that order around ClearState, and one state object made for it.
    expect(count(resolve, "SwapDeviceContextState(") == 2 && count(resolve, "CreateDeviceContextState(") == 1 && count(resolve, "Isolate isolated(g.context.Get(),g.isolated.Get(),g.capture);") == 2,
           "the resolver's swap is two calls in Isolate and its state object one creation, and both entry points isolate through Isolate");
    const std::string init = body(resolve, "bool initialize(ID3D11Device* device,ID3D11DeviceContext* context,const char** reason) {");
    ordered(init, {"(g.capture || g.isolated))return true;", "context->QueryInterface(IID_PPV_ARGS(g.context.GetAddressOf()))", "flatDetectDxmt(device,context)",
                   "flatChooseContextIsolation(g_isolationRequest,dxmt)", "g.capture=choice.mode==FlatContextIsolation::Capture;",
                   "flatContextRanges(device->GetFeatureLevel(),dxmt.dxmt())", "flatFormatContextIsolationLine(choice,dxmt,line,sizeof(line));",
                   "Log::get().note(\"%s\",line);", "if(!g.capture && FAILED(device->QueryInterface(IID_PPV_ARGS(d1.GetAddressOf()))))", "if(g.capture) {",
                   "} else {", "\"create-context-state\"", "d1->CreateDeviceContextState(", "flat-resolve-context-state-create-failed"},
            "the renderer decides its isolation, says so in one log line, and makes the swap's state object only when it is the swap");
    // The key is handed to the resolver once, before anything of the resolver's runs.
    ordered(runtime, {"flatMonoResolveSetSpanHooks(&resolveSpanBegin, &resolveSpanEnd);", "if (!s.isolationRead) {",
                      "flatMonoResolveSetIsolation(flatContextIsolationFromText(Config::get().getString(\"advanced.flat_context_isolation\", \"auto\").c_str()));",
                      "flatMonoResolvePreflight(s.device.Get(),s.context.Get(),s.plannedResolve)"},
            "advanced.flat_context_isolation is read once, at the first frame-end with a mode on, before the first preflight or resolve");
    // The explicit block: no swap in it, every stage's calls, in the order the groups are written.
    const std::string blockSrc = slurp("src/d3d11/flat_context_state.h");
    expect(!blockSrc.empty() && count(blockSrc, "->SwapDeviceContextState(") == 0 && count(blockSrc, "->CreateDeviceContextState(") == 0,
           "the explicit block never calls SwapDeviceContextState or makes a context state (DXMT aborts in the first)");
    {
        static const char* const stages[] = {"VS", "HS", "DS", "GS", "PS", "CS"};
        static const char* const tails[] = {"GetShader(", "GetShaderResources(", "GetConstantBuffers1(", "GetSamplers(", "SetShader(", "SetShaderResources(",
                                            "SetConstantBuffers1(", "SetSamplers("};
        bool all = true;
        for (const char* s : stages)
            for (const char* tail : tails) {
                const std::string call = std::string("->") + s + tail;
                if (count(blockSrc, call.c_str()) != 1) { std::printf("  (explicit block: %s appears %zu times, not once)\n", call.c_str(), count(blockSrc, call.c_str())); all = false; }
            }
        static const char* const others[] = {"->IAGetInputLayout(", "->IAGetPrimitiveTopology(", "->IAGetVertexBuffers(", "->IAGetIndexBuffer(", "->IASetInputLayout(",
                                             "->IASetPrimitiveTopology(", "->IASetVertexBuffers(", "->IASetIndexBuffer(", "->CSGetUnorderedAccessViews(",
                                             "->CSSetUnorderedAccessViews(", "->SOGetTargets(", "->SOSetTargets(", "->OMGetRenderTargetsAndUnorderedAccessViews(",
                                             "->OMSetRenderTargetsAndUnorderedAccessViews(", "->OMSetRenderTargets(", "->OMGetBlendState(", "->OMSetBlendState(",
                                             "->OMGetDepthStencilState(", "->OMSetDepthStencilState(", "->RSGetState(", "->RSSetState(", "->RSGetViewports(",
                                             "->RSSetViewports(", "->RSGetScissorRects(", "->RSSetScissorRects(", "->GetPredication(", "->SetPredication("};
        for (const char* call : others)
            if (count(blockSrc, call) != 1) { std::printf("  (explicit block: %s appears %zu times, not once)\n", call, count(blockSrc, call)); all = false; }
        expect(all, "the explicit block reads and sets every stage it documents, each call once (six stages at four Get and four Set calls, and the IA, UAV, SO, OM, RS and predication calls)");
    }
    {
        const size_t cap = blockSrc.find("void capture(");
        expect(cap != std::string::npos, "the explicit block has its capture");
        ordered(cap == std::string::npos ? std::string() : blockSrc.substr(cap),
                {"\"capture-ia\"", "->IAGetInputLayout(", "->IAGetIndexBuffer(", "kStep[kStageCount] = {\"capture-vs\", \"capture-hs\", \"capture-ds\", \"capture-gs\", \"capture-ps\", \"capture-cs\"}",
                 "captureStage(c, s);", "->CSGetUnorderedAccessViews(", "\"capture-so\"", "->SOGetTargets(", "\"capture-om\"", "->OMGetRenderTargetsAndUnorderedAccessViews(",
                 "->OMGetBlendState(", "->OMGetDepthStencilState(", "\"capture-rs\"", "->RSGetState(", "->RSGetViewports(", "->RSGetScissorRects(", "\"capture-predication\"",
                 "->GetPredication(", "held_ = true;", "void restore(", "\"restore-ia\"", "->IASetInputLayout(", "->IASetIndexBuffer(",
                 "kStep[kStageCount] = {\"restore-vs\", \"restore-hs\", \"restore-ds\", \"restore-gs\", \"restore-ps\", \"restore-cs\"}", "restoreStage(c, s);",
                 "->CSSetUnorderedAccessViews(", "\"restore-so\"", "->SOSetTargets(", "\"restore-om\"", "->OMSetRenderTargetsAndUnorderedAccessViews(", "->OMSetRenderTargets(",
                 "->OMSetBlendState(", "->OMSetDepthStencilState(", "\"restore-rs\"", "->RSSetState(", "->RSSetViewports(", "->RSSetScissorRects(", "\"restore-predication\"",
                 "->SetPredication(", "release();"},
                "the explicit block captures and restores its eleven groups in the order its crumbs name them, each crumb before its calls, and lets go of everything at the end");
    }
    const std::string draw = body(resolve, "void drawHdrTarget(");
    ordered(draw, {"\"finish-bind\"", "context->ClearState();", "context->OMSetRenderTargets(1,&rtv,nullptr);", "\"finish-draw\"", "context->Draw(3,0);",
                   "++stats.hdrFinished;"},
            "H's binding as the render target and the draw into it are two crumbed steps, in that order, and the draw is counted");
    ordered(solve, {"context->CopyResource(g.color.texture.Get(),overlay?cleanColor.Get():color.Get());",
                    "if(overlay) context->CopyResource(g.rawOverlay.texture.Get(),color.Get());",
                    "if(hdr)++stats.hdrCopied;", "copyStep.close();",
                    "context->CSSetShaderResources(0,14,nullViews);", "if(hdr)++stats.hdrPrepped;", "prepStep.close();",
                    "backendStep.close();", "if(hdr && ok)++stats.hdrBackend;", "if(!ok) {"},
            "the resolve counts its copy, prep and backend for the 5 s line as each completes");
    ordered(spatial, {"context->CopyResource(g.color.texture.Get(),color.Get());", "if(hdr)++stats.hdrCopied;"},
            "the spatial recovery counts its copy too");
    ordered(resolve, {"\"create-context-state\"", "d1->CreateDeviceContextState(", "\"create-compute-shaders\"", "device->CreateComputeShader(kFlatMonoPrepBytecode",
                      "\"create-constants-sampler\"", "device->CreateBuffer(&cb", "device->CreateSamplerState(&sm",
                      "\"create-hdr-vs\"", "\"create-hdr-ps-finish\"", "\"create-hdr-ps-spatial\"", "\"create-rtv\"", "CreateRenderTargetView(texture,&rv"},
            "each creation is crumbed around its call: the context state, the shaders, the constants and sampler, the route's three shaders, the target view");
    const std::string image = body(resolve, "bool image(ID3D11Device* device,");
    ordered(image, {"HdrCrumbSpan span(g_crumbOn,\"create-texture\"", "hrTexture=device->CreateTexture2D(", "hrSrv=device->CreateShaderResourceView(",
                    "hrUav=device->CreateUnorderedAccessView("},
            "each private texture is crumbed with its format and size before it is made and its three results after");
    const std::string pre = body(resolve, "FlatMonoResolvePreflightResult flatMonoResolvePreflight(ID3D11Device* device,");
    ordered(pre, {"CrumbScope crumbs(planned.hdr);", "HdrCrumbSpan span(g_crumbOn,\"preflight\"", "preflightBody(device,context,planned)", "span.result("},
            "the preflight's own pair brackets the whole of it, the creations it makes among it");

    // -- the backends: the crumbs are the route's bit alone, and bracket the SDK's call ---------------------------------------
    expect(count(dlaa, "HdrCrumbSpan ") == 3 && count(dlaa, "HdrCrumbSpan query(hdr, ") == 1 && count(dlaa, "HdrCrumbSpan create(hdr, ") == 1 &&
               count(dlaa, "HdrCrumbSpan evaluate(hdr, ") == 1,
           "dlaa.cpp writes its three steps only for the route's hdr bit (the VR path and the warm-up write none)");
    ordered(dlaa, {"HdrCrumbSpan query(hdr, \"backend-query\"", "NGX_DLSS_GET_OPTIMAL_SETTINGS(g_caps, w, h, quality", "query.close();",
                   "HdrCrumbSpan create(hdr, \"backend-create\"", "NGX_D3D11_CREATE_DLSS_EXT(ctx, &f.handle, g_params, &cp);", "create.close();",
                   "HdrCrumbSpan evaluate(hdr, \"backend-evaluate\"", "NGX_D3D11_EVALUATE_DLSS_EXT(ctx, f.handle, g_params, &ep);", "evaluate.close();"},
            "the NGX queries, the feature's creation and its evaluation are each crumbed around the SDK call");
    expect(count(fsr, "HdrCrumbSpan ") == 3 && count(fsr, "HdrCrumbSpan create(hdr, ") == 1 && count(fsr, "HdrCrumbSpan evaluate(hdr, ") == 1 &&
               count(fsr, "HdrCrumbSpan span(crumbs, ") == 1 && count(fsr, ", hdr, \"fsr-") == 3,
           "fsr3_engine.cpp writes its steps only for the route's hdr bit, and names its three shared surfaces");
    ordered(fsr, {"HdrCrumbSpan create(hdr, \"backend-create\"", "ffxFsr3UpscalerContextCreate(&e.ctx, &desc);", "create.close();",
                  "makeSharedSurface(shared.dilatedDepth", "HdrCrumbSpan evaluate(hdr, \"backend-evaluate\"", "ffxFsr3UpscalerContextDispatch(&e.ctx, &dd);",
                  "evaluate.close();"},
            "AMD's context creation and dispatch are each crumbed around the port's call");

    // -- THE DEVICE GATE (flat_hdr_crumbs.h): the crumbs are DXMT's alone --
    const std::string crumbs = slurp("src/d3d11/flat_hdr_crumbs.h");
    const std::string isolation = slurp("src/d3d11/flat_context_isolation.h");
    expect(!crumbs.empty() && !isolation.empty(), "the crumbs and the isolation headers are readable from the repo root");
    // The one writer and the three entry points test the gate first, before any state moves; the gate is the first thing each does.
    ordered(body(crumbs, "inline void hdrCrumbEmit("), {"if (!hdrCrumbEnabled()) return;", "c.written.fetch_add("},
            "the one writer tests the device gate before it counts or writes");
    ordered(body(crumbs, "inline bool hdrCrumbArmed("), {"if (!hdrCrumbEnabled() || c.armedSaid >= 3) return false;", "++c.armedSaid;", "breadcrumb(line);"},
            "the armed line tests the device gate before it counts or writes");
    ordered(body(crumbs, "inline void hdrCrumbAdmit("), {"if (!hdrCrumbEnabled() || c.reached >= kHdrCrumbFrames || c.spent) return;", "c.live.store(true"},
            "a frame is admitted only through the device gate");
    ordered(body(crumbs, "inline void hdrCrumbReach("), {"if (!hdrCrumbEnabled() || c.frameReached || c.reached >= kHdrCrumbFrames || c.spent) return;", "c.live.store(true"},
            "a frame reaches the resolver, for the crumbs, only through the device gate");
    expect(count(crumbs, "live.store(true") == 2 && count(crumbs, "breadcrumb(line);") == 2,
           "the frame goes live in exactly two places (admit and reach) and the header writes through breadcrumb() in exactly two (the writer and the armed line), all gated");
    ordered(body(crumbs, "inline void hdrCrumbEnable("), {"c.enabled.store(on", "if (!on) c.live.store(false"},
            "shutting the gate lowers a frame in progress");
    // Every crumb line is written by that header and by nothing else: no other file starts a string literal with the route's prefix.
    {
        size_t others = 0;
        std::string where;
        for (const auto& entry : std::filesystem::recursive_directory_iterator("src")) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;
            const std::string name = entry.path().generic_string();
            if (name == "src/d3d11/flat_hdr_crumbs.h") continue;
            if (slurp(name.c_str()).find("\"gfx: hdr-treat") != std::string::npos) { ++others; where += " " + name; }
        }
        expect(others == 0, "no file but the crumbs header writes a line that starts \"gfx: hdr-treat\", so nothing bypasses the gate");
        if (others) std::printf("  (also written in:%s)\n", where.c_str());
    }
    // The runtime opens the gate from the markers, once, before the route's accounting and before the key's first read arms the trail,
    // and the isolation key has no part in it.
    ordered(runtime, {"if (!s.crumbGateRead) {", "s.crumbGateRead = true;", "hdrCrumbEnable(flatCrumbsWantedFor(flatDetectDxmt(s.device.Get(), s.context.Get())));",
                      "hdrFrameEnd(s, frame);", "hdrReadKey(s, frame);"},
            "the runtime sets the crumbs' gate from the DXMT markers, once, before the route's frame accounting and before the key's first read arms the trail");
    {
        const size_t g = runtime.find("if (!s.crumbGateRead) {");
        const size_t gEnd = g == std::string::npos ? g : runtime.find("\n    }\n", g);
        const std::string gate = (g == std::string::npos || gEnd == std::string::npos) ? std::string() : runtime.substr(g, gEnd - g);
        expect(!gate.empty() && gate.find("isolation") == std::string::npos && gate.find("Isolation") == std::string::npos &&
                   gate.find("getString") == std::string::npos && gate.find("Config") == std::string::npos,
               "the gate is the detection alone: forcing the capture on Windows (advanced.flat_context_isolation) cannot open it");
    }
    expect(count(runtime, "hdrCrumbEnable(") == 1 && count(slurp("src/d3d11/device_hook.cpp"), "hdrCrumbEnable(") == 0,
           "the runtime opens the gate in one place, and nothing else does");
    expect(isolation.find("inline bool flatCrumbsWantedFor(const FlatDxmtDetection& d) { return d.dxmt(); }") != std::string::npos,
           "the gate's question is the markers' answer and no argument but the detection");
    return failures;
}
