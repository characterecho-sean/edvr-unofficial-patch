// (g) The supercruise gate (2026-10-08, src/d3d11/celestial_motion.h celestialMotionNoteStatus) and the bodies named in the log. Included by
// celestial_motion_test.cpp after tee_tests.h, whose device, buffers and writers it reuses.
//
// WHY. A Quest 3 log of v0.18.3, docked in a Coriolis hangar with a planet 12,000 km off: a celestial record bound every frame, the station
// background a grainy mess; v0.18.2 fine at the same frame rate. Outside supercruise the camera term already carries the ship's translation,
// so the path is only ever right in supercruise, and the module now runs only while Status.json says so (unknown counts as not).
//
// WHAT IS HELD HERE, through the module as vscreen.cpp and device_hook.cpp call it:
//   1. every (known, supercruise) pair gives the right gate state, and live follows it; a closed gate captures nothing, binds no records
//      (celestialMotionRecords returns no SRV: probe.w bit 8192 stays clear and the shader reads no t15), counts its frames, unwatches every
//      buffer (the Map/Unmap tees' first test goes false), and says it once, with its reason and the frame;
//   2. when it opens again nothing crosses the gap: the first frame has no previous frame (the fallback by that name), the next has one --
//      also when the gate shut and opened within one frame, where the stamps alone would pair the stale list;
//   3. the 5 s line carries the frames gated off, a window the gate held shut entirely is one short line, and every line is under the
//      1000-character budget at 20-digit counters;
//   4. device_hook.cpp's tickCelestialStatus pushes the journal's word and its boundary tick runs it, held to the text with mutations;
//   5. the decision table, with mutants (the gate dropped, unknown read as supercruise, unknown read as on, inverted) that must fail it;
//   6. each distinct body radius is named once per session, 16 at most.
#pragma once

namespace gate {   // (<fstream> and <sstream> are included at the top of celestial_motion_test.cpp: this file sits inside its anonymous namespace)
using namespace edvr;

size_t countLines(const char* needle) {
    size_t n = 0;
    for (const std::string& l : g_logLines) if (l.find(needle) != std::string::npos) ++n;
    return n;
}
const std::string* lastLine(const char* needle) {
    for (size_t i = g_logLines.size(); i-- > 0;) if (g_logLines[i].find(needle) != std::string::npos) return &g_logLines[i];
    return nullptr;
}

void drawFrame(tee::Dev& d, tee::Bufs& b, const Fixture& fx, uint32_t f) {
    for (uint32_t eye = 0; eye < 2; ++eye) {
        g_stubDsv = g_stubEyeDsv[eye];
        const EyeFrame* ef = fx.find(f, eye);
        tee::mapWrite(d, b.b0.Get(), tee::b0Block(ef->patches[0]));
        for (const cel::Patch& p : ef->patches) {
            tee::mapWrite(d, b.b2.Get(), tee::b2Block(p));
            celestialMotionNoteDraw(d.ctx.Get());
        }
    }
}
// The pass's consumer for one eye of frame f.
bool consume(tee::Dev& d, const Fixture& fx, uint32_t f, uint32_t eye, CelestialEyeRecords* rec) {
    const cel::EyeInput in = eyeOf(fx, f, eye);
    return celestialMotionRecords(d.ctx.Get(), static_cast<int>(eye), in.tan, in.w, in.h, rec);
}
void freshModule() {
    celestialMotionShutdown();
    celestialMotionConfigure(true);
    g_logLines.clear();
    g_stubEyeDsv[0] = reinterpret_cast<ID3D11DepthStencilView*>(0x1000);
    g_stubEyeDsv[1] = reinterpret_cast<ID3D11DepthStencilView*>(0x2000);
    g_stubDsv = g_stubEyeDsv[0];
}

// ---- the decision table and its mutants ---------------------------------------------------------------------------------------
enum class Gate3 { On, OffNormal, OffUnknown };
// The contract (M = 0) written once more with a seam per mistake; the production module is held to the same table below.
template <int M>
Gate3 decide(bool known, bool supercruise) {
    switch (M) {
        case 1: return Gate3::On;                                                       // the gate dropped
        case 2: return supercruise ? Gate3::On : (known ? Gate3::OffNormal : Gate3::OffUnknown);   // the flag byte read without asking whether it is known
        case 3: return known ? (supercruise ? Gate3::On : Gate3::OffNormal) : Gate3::On;           // unknown counts as supercruise
        case 4: return !known ? Gate3::OffUnknown : (supercruise ? Gate3::OffNormal : Gate3::On);  // inverted
        default: return !known ? Gate3::OffUnknown : (supercruise ? Gate3::On : Gate3::OffNormal);
    }
}
struct Row { bool known, supercruise; Gate3 want; };
const Row kRows[] = {{false, false, Gate3::OffUnknown}, {false, true, Gate3::OffUnknown}, {true, false, Gate3::OffNormal}, {true, true, Gate3::On}};
using DecideFn = Gate3 (*)(bool, bool);
bool holdsTable(DecideFn fn, const Row** failed) {
    for (const Row& r : kRows)
        if (fn(r.known, r.supercruise) != r.want) { if (failed) *failed = &r; return false; }
    return true;
}

// ---- the wiring pin -------------------------------------------------------------------------------------------------------
std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    check(f.good(), fmt("cannot read %s (run from the repository root)", path));
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
// device_hook.cpp's tickCelestialStatus pushes the journal's word to the module, known and flag both, and the frame boundary RUNS it (the
// call site is pinned too: the 2026-10-08 merge deleted the tick this push used to ride, which a body-only pin would not have noticed).
bool tickPushesStatus(const std::string& text) {
    if (text.find("tkCelestialStatus.run(tickCelestialStatus);") == std::string::npos) return false;
    const size_t at = text.find("void tickCelestialStatus() {");
    if (at == std::string::npos) return false;
    const size_t end = text.find("\n}", at);   // the function's closing brace at column 0 (either line ending)
    if (end == std::string::npos) return false;
    const std::string body = text.substr(at, end - at);
    return body.find("celestialMotionNoteStatus(journalSupercruiseKnown(), journalSupercruise());") != std::string::npos;
}

void all(const Fixture& fx) {
    tee::Dev d = tee::makeDevice();
    unsigned cases = 0;
    constexpr uint32_t kF = 23654;

    // ---- 1. every state, and what a closed gate does ----------------------------------------------------------------------
    {
        freshModule();
        tee::Bufs b = tee::makeBuffers(d, true);
        tee::bind(d, b);
        // before any status: not live, a draw is not even looked at, the boundary counts the frame gated off, no records
        check(!celestialMotionLive(), "gate: no status yet: not live");
        celestialMotionNoteDraw(d.ctx.Get());
        celestialMotionFrameBoundary();
        CelestialEyeRecords rec;
        check(g_win.draws == 0 && g_win.frames == 1 && g_win.gatedFrames == 1 && !consume(d, fx, kF, 0, &rec) && rec.srv == nullptr,
              "gate: no status yet: no capture, the frame counted gated off, no records");
        check(g_logLines.empty(), "gate: nothing said before the first status");
        // unknown, with a supercruise flag set: still off (known is what makes the flag mean anything)
        celestialMotionNoteStatus(false, true);
        check(!celestialMotionLive() && g_gate == kGateOffUnknown, "gate: status unknown (flag byte set): off");
        check(g_logLines.size() == 1 && g_logLines[0].find("gate OFF at frame 1") != std::string::npos && g_logLines[0].find("status unknown") != std::string::npos &&
                  g_logLines[0].find("was off (no status yet)") != std::string::npos,
              fmt("gate: one line, off, with the reason unknown and the frame: %s", g_logLines.empty() ? "(none)" : g_logLines[0].c_str()));
        celestialMotionNoteStatus(false, false);
        celestialMotionNoteStatus(false, true);
        check(g_logLines.size() == 1, "gate: the same state again says nothing more");
        // known, not supercruise: off for another reason, said
        celestialMotionNoteStatus(true, false);
        check(!celestialMotionLive() && g_gate == kGateOffNormal && g_logLines.size() == 2 && g_logLines[1].find("not supercruise") != std::string::npos &&
                  g_logLines[1].find("was off (status unknown)") != std::string::npos,
              "gate: known and not supercruise: off, a second line with that reason");
        // supercruise: on
        celestialMotionNoteStatus(true, true);
        check(celestialMotionLive() && g_gate == kGateOn && g_logLines.size() == 3 && g_logLines[2].find("gate ON at frame 1") != std::string::npos &&
                  g_logLines[2].find("says supercruise") != std::string::npos,
              "gate: supercruise: on, a third line");
        celestialMotionNoteStatus(true, true);
        check(g_logLines.size() == 3, "gate: still supercruise says nothing more");
        ++cases;
    }

    // ---- 2. a closed gate does nothing, an open one works, and nothing crosses the gap -------------------------------------
    {
        freshModule();
        tee::Bufs b = tee::makeBuffers(d, true);
        tee::bind(d, b);
        celestialMotionNoteStatus(true, true);
        celestialMotionNoteDraw(d.ctx.Get());   // registers the buffers (the first draw of a session can read nothing)
        drawFrame(d, b, fx, kF - 1);
        celestialMotionFrameBoundary();
        drawFrame(d, b, fx, kF);
        CelestialEyeRecords rec;
        for (uint32_t eye = 0; eye < 2; ++eye)
            check(consume(d, fx, kF, eye, &rec) && rec.srv != nullptr && rec.records == 1, fmt("gate open: eye %u gets its record", eye));
        check(celestialMotionAnyWatched() && g_watched == 2 && g_win.captured > 100, "gate open: patch draws captured, both buffers watched");
        // close it: unwatched at once, the tees' first test false, nothing kept
        const uint64_t drawsBefore = g_win.draws, capturedBefore = g_win.captured;
        celestialMotionNoteStatus(true, false);
        check(!celestialMotionLive() && !celestialMotionAnyWatched() && g_watched == 0, "gate closed: not live, the tees' first test false, no buffer watched");
        check(g_eye[0].n[0] == 0 && g_eye[0].n[1] == 0 && g_eye[1].n[0] == 0 && g_eye[1].n[1] == 0 && g_eye[0].stamp[0] == kNever, "gate closed: the captured frames are dropped");
        // three frames of the game drawing patches with the gate shut
        for (int i = 0; i < 3; ++i) {
            celestialMotionFrameBoundary();
            drawFrame(d, b, fx, kF + 1 + i);
            for (uint32_t eye = 0; eye < 2; ++eye) {
                CelestialEyeRecords r2;
                check(!consume(d, fx, kF + 1 + i, eye, &r2) && r2.srv == nullptr && r2.records == 0,
                      "gate closed: the consumer returns no SRV (the shader's t15 bit stays clear: zero path-12 pixels)");
            }
        }
        check(g_win.draws == drawsBefore && g_win.captured == capturedBefore && g_win.gatedFrames == 3 && g_watched == 0 && !celestialMotionAnyWatched(),
              "gate closed: three frames of patch draws captured and counted nothing, and watched nothing; the frames counted gated off");
        // the census says so: partly gated windows carry the count in the long line, a window the gate held shut is the short line
        g_logLines.clear();
        report();
        check(g_logLines.size() == 3 && g_logLines[0].find("gated-off=3") != std::string::npos && g_logLines[2].find("gate=off (not supercruise) (3 of 4 frames gated off)") != std::string::npos,
              "5 s line: a partly gated window carries gated-off=3 and the gate's state");
        g_win = Counters{};
        celestialMotionFrameBoundary();
        celestialMotionFrameBoundary();
        g_logLines.clear();
        report();
        check(g_logLines.size() == 1 && g_logLines[0].rfind("celestial motion 5s: frames=2 gated-off=2 ", 0) == 0 &&
                  g_logLines[0].find("supercruise gate: off (not supercruise)") != std::string::npos && g_logLines[0].size() < 400,
              fmt("5 s line: a window held shut throughout is one short line: %s", g_logLines.empty() ? "(none)" : g_logLines[0].c_str()));
        // open it again: the first frame has no previous frame, the one after has
        celestialMotionNoteStatus(true, true);
        celestialMotionNoteDraw(d.ctx.Get());   // registers again: the watch table was emptied when the gate shut
        const uint32_t g0 = 23660;
        drawFrame(d, b, fx, g0);
        g_win.fallbacks[cel::kFbNoPrevFrame] = 0;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            CelestialEyeRecords r3;
            check(!consume(d, fx, g0, eye, &r3) && r3.srv == nullptr, fmt("gate reopened: eye %u has no record on the first frame", eye));
        }
        check(g_win.fallbacks[cel::kFbNoPrevFrame] == 2, fmt("gate reopened: the first frame takes the no-previous-frame fallback on both eyes (%llu)",
                                                              static_cast<unsigned long long>(g_win.fallbacks[cel::kFbNoPrevFrame])));
        celestialMotionFrameBoundary();
        drawFrame(d, b, fx, g0 + 1);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            CelestialEyeRecords r4;
            check(consume(d, fx, g0 + 1, eye, &r4) && r4.srv != nullptr && r4.records == 1, fmt("gate reopened: the second frame binds eye %u's record", eye));
        }
        ++cases;
    }

    // ---- 2b. shut and reopened inside one frame: the stamps alone would pair the stale list --------------------------------
    {
        freshModule();
        tee::Bufs b = tee::makeBuffers(d, true);
        tee::bind(d, b);
        celestialMotionNoteStatus(true, true);
        celestialMotionNoteDraw(d.ctx.Get());
        drawFrame(d, b, fx, kF - 1);
        celestialMotionFrameBoundary();
        drawFrame(d, b, fx, kF);
        celestialMotionNoteStatus(true, false);   // shut and open again with no boundary between: the lists of frame kF are still the current stamp's
        celestialMotionNoteStatus(true, true);
        celestialMotionFrameBoundary();           // the next frame: its stamp is exactly one past the stale list's
        celestialMotionNoteDraw(d.ctx.Get());
        drawFrame(d, b, fx, kF + 1);
        g_win.fallbacks[cel::kFbNoPrevFrame] = 0;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            CelestialEyeRecords r;
            check(!consume(d, fx, kF + 1, eye, &r) && r.srv == nullptr, fmt("gate shut and reopened within a frame: eye %u does not pair with the frame before the gap", eye));
        }
        check(g_win.fallbacks[cel::kFbNoPrevFrame] == 2, "...it takes the no-previous-frame fallback");
        ++cases;
    }

    // ---- 2c. the module's own switch is a gap too ----------------------------------------------------------------------------
    {
        freshModule();
        tee::Bufs b = tee::makeBuffers(d, true);
        tee::bind(d, b);
        celestialMotionNoteStatus(true, true);
        celestialMotionNoteDraw(d.ctx.Get());
        drawFrame(d, b, fx, kF - 1);
        celestialMotionFrameBoundary();
        drawFrame(d, b, fx, kF);
        celestialMotionConfigure(false);   // the pass went away
        check(!celestialMotionLive(), "configured off: not live, whatever the gate said");
        celestialMotionNoteStatus(true, true);   // a status while off is not tracked
        check(!celestialMotionLive(), "configured off: a status does not open it");
        celestialMotionConfigure(true);
        check(!celestialMotionLive(), "configured on again: not live until the next status (the gate decides afresh)");
        celestialMotionNoteStatus(true, true);
        check(celestialMotionLive(), "configured on again: the next status opens it");
        check(g_eye[0].n[0] == 0 && g_eye[0].n[1] == 0 && g_eye[1].n[0] == 0 && g_eye[1].n[1] == 0 && g_watched == 0, "configured on again: nothing from before the switch is kept");
        ++cases;
    }

    // ---- 3. line budgets at 20-digit counters -------------------------------------------------------------------------------
    {
        freshModule();
        celestialMotionNoteStatus(true, true);
        Counters& c = g_win;
        const uint64_t big = 18446744073709551615ull;
        c.frames = big; c.draws = big; c.captured = big;
        for (uint64_t& x : c.declined) x = big;
        c.consumerCalls = big; c.eyeFrames = 1; c.patches = big; c.matched = big; c.unmatched = big; c.bodies = big; c.behind = big; c.offscreen = big;
        c.records = big; c.uploads = big; c.gatedFrames = big - 1; c.shellRecords = big;
        for (uint64_t& x : c.fallbacks) x = big;
        c.maxDisplacement = 1.0e19;
        c.pixels = big; c.pixelsKnown = true;
        c.teeMap = big; c.teeUpdate = big; c.teeInvalid = big; c.teeCopies = 1; c.teeTicks = big; c.captureTicks = big; c.buildTicks = big;
        g_watched = 4294967295u;
        g_logLines.clear();
        report();
        check(g_logLines.size() == 3, "line budget: the long census is three lines");
        size_t worst = 0;
        for (const std::string& l : g_logLines) worst = std::max(worst, l.size());
        check(worst < 700, fmt("line budget: each census line is well under 1000 characters at 20-digit counters (worst %zu)", worst));
        std::printf("    census line lengths at 20-digit counters: %zu, %zu and %zu (budget 1000)\n", g_logLines[0].size(), g_logLines[1].size(), g_logLines[2].size());
        // a window the gate held shut throughout, at the longest gate name
        c = Counters{};
        c.frames = big; c.gatedFrames = big;
        celestialMotionNoteStatus(false, false);
        g_logLines.clear();
        report();
        check(g_logLines.size() == 1 && g_logLines[0].size() < 400, fmt("line budget: the short gated line is short (%zu)", g_logLines.empty() ? size_t(0) : g_logLines[0].size()));
        // the transition lines, longest reason
        g_logLines.clear();
        celestialMotionNoteStatus(true, true);
        celestialMotionNoteStatus(false, true);
        worst = 0;
        for (const std::string& l : g_logLines) worst = std::max(worst, l.size());
        check(g_logLines.size() == 2 && worst < 600, fmt("line budget: the gate's transition lines are short (worst %zu)", worst));
        g_watched = 0;
        c = Counters{};
        ++cases;
    }

    // ---- the transition lines stop at 32 -------------------------------------------------------------------------------------
    {
        freshModule();
        for (int i = 0; i < 100; ++i) celestialMotionNoteStatus(true, (i & 1) == 0);
        check(countLines("celestial motion: gate ") == 32, fmt("gate lines: a flapping status is logged 32 times, not 100 (%zu)", countLines("celestial motion: gate ")));
        check(g_logLines.back().find("last gate line this session") != std::string::npos, "gate lines: the last one says it is the last");
        // the cap is the session's: the module switched off and on again (a settings reload) does not give it another 32
        celestialMotionConfigure(false);
        celestialMotionConfigure(true);
        for (int i = 0; i < 10; ++i) celestialMotionNoteStatus(true, (i & 1) == 0);
        check(countLines("celestial motion: gate ") == 32, "gate lines: a switch of the module off and on does not reset the cap");
        ++cases;
    }

    // ---- 4. the decision table, with mutants -----------------------------------------------------------------------------------
    {
        const Row* failed = nullptr;
        check(holdsTable(decide<0>, &failed), "table: the contract written once more holds the table");
        // the production module, driven through the same four rows, in both orders (each row from a fresh module and from the row before)
        for (int pass = 0; pass < 2; ++pass)
            for (const Row& r : kRows) {
                if (pass == 0) freshModule();
                celestialMotionNoteStatus(r.known, r.supercruise);
                const Gate3 got = g_gate == kGateOn ? Gate3::On : (g_gate == kGateOffNormal ? Gate3::OffNormal : Gate3::OffUnknown);
                check(got == r.want && celestialMotionLive() == (r.want == Gate3::On),
                      fmt("table: the module, known=%d supercruise=%d: state and live as the contract says", r.known, r.supercruise));
            }
        struct M { DecideFn fn; const char* what; };
        const M mutants[] = {{decide<1>, "the gate dropped (always on)"}, {decide<2>, "the flag byte used without asking whether it is known"},
                             {decide<3>, "unknown counted as supercruise"}, {decide<4>, "inverted"}};
        for (const M& m : mutants) {
            const Row* f = nullptr;
            check(!holdsTable(m.fn, &f), fmt("mutation control: %s fails the table", m.what));
            std::printf("    control '%s': caught -- known=%d supercruise=%d wants %d\n", m.what, f->known, f->supercruise, static_cast<int>(f->want));
        }
        // the wiring from the journal to the module, held to its text, and the mutants of the pin
        const std::string src = slurp("src/d3d11/device_hook.cpp");
        check(tickPushesStatus(src), "wiring: tickCelestialStatus pushes journalSupercruiseKnown() and journalSupercruise() to celestialMotionNoteStatus, and the boundary runs it");
        const std::string call = "celestialMotionNoteStatus(journalSupercruiseKnown(), journalSupercruise());";
        struct W { const char* what; std::string replacement; };
        const W wires[] = {{"the push removed", ""},
                           {"unknown dropped (known passed as true)", "celestialMotionNoteStatus(true, journalSupercruise());"},
                           {"the flag passed as always set", "celestialMotionNoteStatus(journalSupercruiseKnown(), true);"}};
        for (const W& w : wires) {
            std::string mutated = src;
            const size_t at = mutated.find(call);
            check(at != std::string::npos && mutated.find(call, at + 1) == std::string::npos, "wiring: the push is in device_hook.cpp exactly once");
            mutated.replace(at, call.size(), w.replacement);
            check(!tickPushesStatus(mutated), fmt("mutation control: %s fails the wiring pin", w.what));
            std::printf("    control 'wiring: %s': caught\n", w.what);
        }
        {   // the tick's run removed: the push is intact but nothing calls it
            std::string mutated = src;
            const std::string run = "tkCelestialStatus.run(tickCelestialStatus);";
            const size_t at = mutated.find(run);
            check(at != std::string::npos, "wiring: the boundary runs tickCelestialStatus");
            mutated.replace(at, run.size(), "");
            check(!tickPushesStatus(mutated), "mutation control: the tick's run removed fails the wiring pin");
            std::printf("    control 'wiring: the tick's run removed': caught\n");
        }
        ++cases;
    }

    // ---- 6. bodies named once per radius, sixteen at most ---------------------------------------------------------------------
    {
        freshModule();
        tee::Bufs b = tee::makeBuffers(d, true);
        tee::bind(d, b);
        celestialMotionNoteStatus(true, true);
        celestialMotionNoteDraw(d.ctx.Get());
        drawFrame(d, b, fx, kF - 1);
        celestialMotionFrameBoundary();
        drawFrame(d, b, fx, kF);
        CelestialEyeRecords rec;
        for (uint32_t eye = 0; eye < 2; ++eye) consume(d, fx, kF, eye, &rec);
        check(countLines("celestial motion: body ") == 6, fmt("bodies: the six bodies of the dump are each named once, across both eyes (%zu)", countLines("celestial motion: body ")));
        const std::string* moon = lastLine("radius 679 km");
        check(moon != nullptr && moon->find("record bound") != std::string::npos && moon->find("straddles the eye plane: no") != std::string::npos,
              "bodies: the moon is named by its radius to a km, with its record and no straddle");
        check(lastLine("wholly behind the eye, no record") != nullptr && lastLine("straddles the eye plane: n/a, wholly behind it") != nullptr, "bodies: the bodies behind the eye are named as such");
        std::printf("    a body line: %s\n", moon->c_str());
        // asked again, both eyes, a later frame: nothing new
        celestialMotionFrameBoundary();
        drawFrame(d, b, fx, kF + 1);
        for (uint32_t eye = 0; eye < 2; ++eye) consume(d, fx, kF + 1, eye, &rec);
        check(countLines("celestial motion: body ") == 6, "bodies: a later frame names nothing again");
        // twenty distinct radii: sixteen lines, the sixteenth saying it is the last
        g_logLines.clear();
        {
            std::vector<cel::Patch> cur, prev;
            const EyeFrame* ef = fx.find(kF, 0);
            const EyeFrame* pf = fx.find(kF - 1, 0);
            for (int i = 0; i < 20; ++i) {
                for (int pass = 0; pass < 2; ++pass) {
                    std::vector<cel::Patch> v;
                    for (const cel::Patch& q : (pass == 0 ? ef : pf)->patches) if (q.body[3] == kMoon) v.push_back(q);
                    for (cel::Patch& q : v) {
                        q.body[0] += 60000.0f * float(i); q.body[1] += 30000.0f * float(i % 4);
                        q.c[0] += 60000.0f * float(i); q.c[1] += 30000.0f * float(i % 4);
                        q.body[3] += 3000.0f * float(i);   // 3 km apart: twenty radii that differ to a km
                    }
                    (pass == 0 ? cur : prev).insert((pass == 0 ? cur : prev).end(), v.begin(), v.end());
                }
            }
            cel::BuildResult out;
            cel::build(cur.data(), static_cast<uint32_t>(cur.size()), prev.data(), static_cast<uint32_t>(prev.size()), eyeOf(fx, kF, 0), out, scratch());
            check(out.bodies == 20, fmt("bodies: twenty bodies in the synthetic frame (%u)", out.bodies));
            g_eye[0].result = out;
            g_bodiesNamed = 0;
            noteBodies(0, g_eye[0]);
            check(countLines("celestial motion: body ") == 16, fmt("bodies: twenty radii name sixteen bodies, no more (%zu)", countLines("celestial motion: body ")));
            check(g_logLines.back().find("The 16th: no more radii are named.") != std::string::npos, "bodies: the sixteenth line says it is the last");
            noteBodies(0, g_eye[0]);
            check(countLines("celestial motion: body ") == 16, "bodies: and asking again names nothing");
        }
        ++cases;
    }
    std::printf("(g) %u cases of the supercruise gate: every state and what a closed gate does, nothing across a gap (also within one frame, and across the module's own switch), "
                "line budgets at 20 digits, the decision table with 4 mutants and the journal wiring pin with 3, the bodies named\n", cases);
}

}  // namespace gate
