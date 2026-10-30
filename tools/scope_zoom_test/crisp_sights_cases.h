// C1: the crisp sights (docs\design-vr-scope-magnification-2026-10-28.md, section 12), included by scope_zoom_test.cpp after its helpers (check, has, before, replaced, readFile).
//
// C1a, the crisp reticle: the arming gate (crisp_reticle.h) driven as a truth table (the hip never, the sights' edges, the lens' own sights, the key off), the door side of the arming
// (scope_zoom.cpp: publish, freshness, the eye-frame counts, the press's reset), the press and announce lines, and the wiring: the family's arming asks the lens and then the crisp gate,
// the frame thread publishes the gate every tick and down on every other path, the composite counts the eye-frames, the key is read by its literal name and ships on.
// C1b, the withhold cycle (sight_withhold.h): the candidates, the cycle's order, the draw path's decision (a vertex shader first, the pixel shader only for a candidate's), the records, and the
// wiring that keeps it inside a manual dump: armed in one place, by the eye dump key, never by the airborne companion.
#pragma once

#include "../../src/d3d11/sight_withhold.h"

namespace c1cases {

namespace cr = edvr::crisp;
namespace sw = edvr::sightwithhold;

inline size_t countOf(const std::string& text, const std::string& what) {
    size_t n = 0, at = 0;
    while ((at = text.find(what, at)) != std::string::npos) {
        ++n;
        at += what.size();
    }
    return n;
}

inline void gateTests() {
    cr::GateIn g;
    g.keyOn = g.onFoot = g.adsUp = true;
    g.lensKeyOn = true;
    g.itemKnown = true;
    g.lensOwned = false;
    g.wFresh = true;
    g.w = 1.0;
    check(cr::eligible(g) && cr::armed(g), "C1 gate: the key on, on foot, the sights up, a weapon the lens does not name, the weight 1: eligible and armed");
    // the hip never: no press of the sights, whatever else is true
    cr::GateIn hip = g;
    hip.adsUp = false;
    check(!cr::eligible(hip) && !cr::armed(hip), "C1 gate: no press of the sights (the hip) is never eligible or armed");
    hip.w = 1.0;
    hip.wFresh = true;
    check(!cr::armed(hip), "C1 gate: and not even with the weight reading 1");
    // the sights' edges: the game's weight is 0 before the raise has moved it and after the lower has finished, and the quad is large there
    cr::GateIn edge = g;
    edge.w = 0.0;
    check(cr::eligible(edge) && !cr::armed(edge), "C1 gate: the first tick of a raise (the weight 0) is eligible and not armed: the quad is the hip's");
    edge.w = 0.04;
    check(cr::armed(edge), "C1 gate: the weight above 0 arms from the first frame the sights have moved");
    edge.w = 0.0;
    edge.adsUp = true;
    check(!cr::armed(edge), "C1 gate: the end of the lower (the weight back at 0, the press still open) disarms");
    edge.adsUp = false;
    edge.w = 0.5;
    check(!cr::armed(edge), "C1 gate: and the press closed disarms at once whatever the weight");
    cr::GateIn lower = g;
    lower.w = 0.3;
    check(cr::armed(lower), "C1 gate: the middle of the lower, the press still open and the weight above 0, is armed");
    cr::GateIn unread = g;
    unread.wFresh = false;
    unread.w = 0.0;
    check(cr::armed(unread), "C1 gate: a weight that was not read this tick says nothing: the press decides");
    cr::GateIn nan = g;
    nan.w = std::nan("");
    check(cr::armed(nan), "C1 gate: a weight that is no number says nothing either");
    // the lens' own sights
    cr::GateIn owned = g;
    owned.lensOwned = true;
    check(!cr::eligible(owned) && !cr::armed(owned), "C1 gate: with the scope zoom on, an optical sight of the lens table is the lens' to arm, as before C1: neither eligible nor armed");
    cr::GateIn lensOff = owned;
    lensOff.lensKeyOn = false;
    check(cr::eligible(lensOff) && cr::armed(lensOff), "C1 gate: with the scope zoom off there is no lens, and the optical sight is served like any other");
    cr::GateIn unknown = g;
    unknown.itemKnown = false;
    check(!cr::eligible(unknown) && !cr::armed(unknown), "C1 gate: with the scope zoom on, an item whose entry is not known yet waits (an optical sight must not move to this path for a tick)");
    unknown.lensKeyOn = false;
    check(cr::armed(unknown), "C1 gate: with the scope zoom off the entry does not matter");
    // the key off: nothing arms or is eligible whatever the other six facts are
    bool off = true, never = true, exactlyOn = true;
    for (unsigned bits = 0; bits < 128; ++bits) {
        cr::GateIn t;
        t.keyOn = false;
        t.onFoot = bits & 1u;
        t.adsUp = bits & 2u;
        t.lensKeyOn = bits & 4u;
        t.itemKnown = bits & 8u;
        t.lensOwned = bits & 16u;
        t.wFresh = bits & 32u;
        t.w = (bits & 64u) ? 1.0 : 0.0;
        off = off && !cr::eligible(t) && !cr::armed(t);
        t.keyOn = true;
        // the lens and this gate never both arm the quad: a sight the lens owns is never armed here
        never = never && !(t.lensKeyOn && t.lensOwned && cr::armed(t));
        // and the gate is exactly: on foot, the sights up, and the lens not the owner (or no lens)
        const bool lensServes = !t.lensKeyOn || (t.itemKnown && !t.lensOwned);
        const bool atHip = t.wFresh && !(t.w > 0.0);
        exactlyOn = exactlyOn && cr::eligible(t) == (t.onFoot && t.adsUp && lensServes) && cr::armed(t) == (t.onFoot && t.adsUp && lensServes && !atHip);
    }
    check(off, "C1 gate: with the key off the gate is false for every combination of the other facts: with it off nothing new runs");
    check(never, "C1 gate: no combination arms the quad here for a sight the scope lens owns");
    check(exactlyOn, "C1 gate: over all 128 combinations the gate is exactly on foot, the sights up, the lens not the owner, and not at the hip");
}

inline void lineTests() {
    char line[cr::kPressLineBytes];
    cr::PressLine p;
    p.press = 7;
    p.item = "$wpn_m_assaultrifle_kinetic_fauto_name;";
    p.scope = "no";
    p.eligibleTicks = 300;
    p.armedTicks = 280;
    p.door.armed[0] = 270;
    p.door.armed[1] = 271;
    p.door.noQuad[0] = 10;
    p.door.noQuad[1] = 11;
    p.taken[0] = 260;
    p.taken[1] = 261;
    p.pathSeeded = 521;
    p.pathReadOnly = 0;
    p.pathTestsOff = 0;
    p.pathUntested = 0;
    p.matched = 521;
    p.plain = 0;
    p.none = 0;
    p.standDowns = 0;
    const int n = cr::formatPress(line, sizeof(line), p);
    check(n > 0 && static_cast<size_t>(n) == std::strlen(line) &&
              std::strcmp(line, "crisp reticle (C1): press 7 item $wpn_m_assaultrifle_kinetic_fauto_name; scope no key on; eligible ticks 300, armed ticks 280; armed eye-frames L 270 R 271, "
                                "with no quad taken L 10 R 11; quad taken L 260 R 261 (depth path seeded 521 read-only 0 tests off 0 untested 0); composited matched 521 plain 0 none 0; stood down 0") == 0,
          "C1 line: the press line's exact text");
    // the longest it can be: a long item name and ten digit counts, in a buffer the log's 1167 bytes also holds
    cr::PressLine w = p;
    w.press = w.eligibleTicks = w.armedTicks = w.taken[0] = w.taken[1] = w.pathSeeded = w.pathReadOnly = w.pathTestsOff = w.pathUntested = w.matched = w.plain = w.none = w.standDowns = 4294967295u;
    w.door.armed[0] = w.door.armed[1] = w.door.noQuad[0] = w.door.noQuad[1] = 4294967295u;
    w.item = "$wpn_m_submachinegun_laser_fauto_name_with_a_very_long_suffix_name;";
    const int wn = cr::formatPress(line, sizeof(line), w);
    check(wn > 0 && static_cast<size_t>(wn) < sizeof(line) - 1 && wn <= 1167, "C1 line: the worst case (ten digit counts, a long item) fits its buffer and the log's 1167 bytes");
    char ann[512];
    const int an = cr::formatAnnounce(ann, sizeof(ann), true);
    check(an > 0 && static_cast<size_t>(an) < sizeof(ann) - 1 && std::strncmp(ann, "crisp reticle (C1): key on; ", 28) == 0, "C1 line: the announce says the key on, with room to spare in its buffer");
    cr::formatAnnounce(ann, sizeof(ann), false);
    check(std::strncmp(ann, "crisp reticle (C1): key off; ", 29) == 0, "C1 line: and the key off");
}

// The door side of the arming (scope_zoom.cpp), the production functions.
inline void doorTests() {
    check(!scopeZoomCrispArmed() && !scopeZoomMarkArmed(), "C1 door: nothing published, neither arming is up: the family's facts are as before C1");
    scopeZoomCrispPublish(true);
    check(scopeZoomCrispArmed(), "C1 door: published up, the crisp arming is up");
    check(!scopeZoomMarkArmed(), "C1 door: and the lens' own arming is not touched by it");
    scopeZoomCrispPublish(false);
    check(!scopeZoomCrispArmed(), "C1 door: published down, it is down");
    scopeZoomCrispPublish(true);
    Sleep(80);
    check(!scopeZoomCrispArmed(), "C1 door: an arming no tick has renewed for 80 ms is down (a stalled frame thread leaves the quad in the scene)");
    scopeZoomPressBegin(41);
    scopeZoomNoteCrispFrame(0, true);
    scopeZoomNoteCrispFrame(0, false);
    scopeZoomNoteCrispFrame(1, false);
    scopeZoomNoteCrispFrame(1, true);
    scopeZoomNoteCrispFrame(1, true);
    scopeZoomNoteCrispFrame(2, false);   // an eye that is none is not counted
    cr::DoorStats d;
    scopeZoomCrispTake(&d);
    check(d.armed[0] == 2 && d.noQuad[0] == 1 && d.armed[1] == 3 && d.noQuad[1] == 1, "C1 door: the eye-frames with the arming up are counted per eye, with those that held no quad");
    scopeZoomPressBegin(42);
    scopeZoomCrispTake(&d);
    check(d.armed[0] == 0 && d.armed[1] == 0 && d.noQuad[0] == 0 && d.noQuad[1] == 0, "C1 door: a new press starts the counts from nothing");
    scopeZoomCrispPublish(false);
}

inline void withholdTests() {
    const uint64_t vs = sw::kVsMaterial, ps5 = 0xC5A5C7E8216CB9AFull;
    check(sw::kCount == 11, "C1b: eleven candidates: five C5A5 shapes, the Intimidator's, four pixel shaders and the control");
    bool unique = true;
    for (uint32_t i = 0; i < sw::kCount; ++i)
        for (uint32_t j = i + 1; j < sw::kCount; ++j) unique = unique && std::strcmp(sw::kCandidates[i].name, sw::kCandidates[j].name) != 0;
    check(unique, "C1b: the candidates' names are distinct (the json names them)");
    // what each is
    check(sw::match(vs, ps5, 18, 1) == 0 && sw::match(vs, ps5, 6, 5) == 1 && sw::match(vs, ps5, 6, 2) == 2 && sw::match(vs, ps5, 9, 1) == 3 && sw::match(vs, ps5, 12, 2) == 4 && sw::match(vs, ps5, 6, 1) == 5,
          "C1b: the C5A5 draws are told apart by their shape");
    check(sw::match(vs, ps5, 24, 1) == -1 && sw::match(vs, ps5, 9, 2) == -1, "C1b: a C5A5 draw of another shape is no candidate");
    check(sw::match(vs, 0x0925AA1BD7A95BBAull, 360, 1) == 6 && sw::match(vs, 0x0925AA1BD7A95BBAull, 3, 9) == 6, "C1b: a pixel shader candidate is any shape");
    check(sw::match(0x98397963AAEC45D3ull, 0x66B08F89E4926C01ull, 24, 2) == 7 && sw::match(0xF512712C40D93C12ull, 0xAFED1D4B087E18A9ull, 12, 1) == 8 &&
              sw::match(0x94D5C556DFD6D705ull, 0x912477AEF6958379ull, 6, 38) == 9,
          "C1b: the unidentified pairs of the same pass are candidates");
    check(sw::match(vs, 0x46F92DC71BF8DFA5ull, 6, 1) == 10 && sw::match(vs, 0x46F92DC71BF8DFA5ull, 18, 1) == -1, "C1b: the gunsight quad is the control, by its shape");
    check(sw::match(vs ^ 1, ps5, 18, 1) == -1 && sw::match(vs, ps5 ^ 1, 18, 1) == -1, "C1b: the pair must be the pair");
    check(sw::vsIsCandidate(vs) && sw::vsIsCandidate(0x94D5C556DFD6D705ull) && !sw::vsIsCandidate(0x1234ull), "C1b: the vertex shader is asked first");
    // the cycle: nothing for the key press' frame and before, then the list in order, one a frame
    check(sw::cycleIndex(100, 100) == -1 && sw::cycleIndex(99, 100) == -1 && sw::cycleIndex(101, 100) == 0 && sw::cycleIndex(111, 100) == 10 && sw::cycleIndex(112, 100) == 0 && sw::cycleIndex(116, 100) == 4,
          "C1b: the cycle starts the frame after the key press, one candidate a frame, in the list's order, and wraps");
    check(std::strcmp(sw::name(0), "C5A5_n18_i1") == 0 && std::strcmp(sw::name(-1), "none") == 0 && std::strcmp(sw::name(11), "none") == 0, "C1b: the names, none for no candidate");
    // the draw path
    sw::disarm();
    sw::publishAdsUp(false);
    check(!sw::arm(100) && !sw::live(), "C1b: a dump begun with the sights down arms nothing");
    sw::publishAdsUp(true);
    check(sw::arm(100) && sw::live(), "C1b: a dump begun with the sights up arms the cycle");
    int psCalls = 0;
    const auto ps = [&] { ++psCalls; return ps5; };
    check(!sw::skips(100, 0, vs, 18, 1, ps), "C1b: the frame of the key press is untouched");
    psCalls = 0;
    check(sw::skips(101, 0, vs, 18, 1, ps) && psCalls == 1, "C1b: frame 101 leaves out candidate 0 (C5A5 n18 i1): the eye 0 draw is skipped");
    check(!sw::skips(101, 0, vs, 6, 5, ps) && !sw::skips(101, 0, 0x1234ull, 18, 1, ps), "C1b: the other C5A5 shapes and other vertex shaders are drawn");
    check(psCalls == 2, "C1b: the pixel shader is read only for a draw whose vertex shader is a candidate's (2 reads for 3 draws, one of them another vertex shader)");
    check(sw::skips(101, 1, vs, 18, 1, ps), "C1b: the same candidate is left out of eye 1");
    sw::noteCapture(1);
    sw::Record r1 = sw::record(1);
    check(r1.have && r1.frame == 101 && r1.candidate == 0 && r1.draws[0] == 1 && r1.draws[1] == 1, "C1b: the capture's record has the frame, the candidate and the draws left out per eye");
    check(!sw::skips(102, 0, vs, 18, 1, ps) && sw::skips(102, 0, vs, 6, 5, ps), "C1b: the next frame leaves out the next candidate");
    sw::noteCapture(2);
    check(sw::record(2).candidate == 1 && sw::record(2).draws[0] == 1 && sw::record(2).draws[1] == 0, "C1b: the next record names it");
    // a frame in which the candidate is not drawn still has its record (the phase starts at the first eye 0 draw)
    check(!sw::skips(103, 0, 0x1234ull, 5, 5, ps), "C1b: a draw that is no candidate is drawn");
    sw::noteCapture(3);
    check(sw::record(3).have && sw::record(3).candidate == 2 && sw::record(3).draws[0] == 0 && sw::record(3).frame == 103, "C1b: a frame whose candidate was not drawn is recorded with 0 draws");
    // the sights down: nothing is left out and the record says none
    sw::publishAdsUp(false);
    check(!sw::skips(104, 0, vs, 9, 1, ps), "C1b: with the sights down nothing is left out, even the frame's candidate");
    sw::noteCapture(4);
    check(sw::record(4).candidate == -1 && sw::record(4).draws[0] == 0, "C1b: and the record says none");
    sw::publishAdsUp(true);
    // a cycle that was not written ends itself
    check(sw::live() && !sw::skips(100 + sw::kMaxFrames + 1, 0, vs, 18, 1, ps) && !sw::live(), "C1b: a cycle ends itself after kMaxFrames frames whatever happened to the dump");
    check(!sw::skips(105, 0, vs, 18, 1, ps), "C1b: and nothing is left out after that");
    sw::noteCapture(99);
    check(!sw::record(99).have, "C1b: a capture number out of the run is ignored");
    sw::disarm();
    check(sw::arm(300) && sw::record(1).have == false, "C1b: arming starts the records from nothing");
    sw::disarm();
    sw::publishAdsUp(false);
    check(!sw::skips(301, 0, vs, 18, 1, ps), "C1b: disarmed, the draw path leaves nothing out");
}

inline void wiringTests() {
    const std::string vscreen = readFile("src/d3d11/vscreen.cpp"), layer = readFile("src/d3d11/ui_layer.cpp"), native = readFile("src/d3d11/native_on_foot.cpp"),
                      temporal = readFile("src/d3d11/temporal_pass.cpp"), census = readFile("src/d3d11/draw_census.cpp"), ini = readFile("edvr.ini"), device = readFile("src/d3d11/device_hook.cpp");
    check(!vscreen.empty() && !layer.empty() && !native.empty() && !temporal.empty() && !census.empty() && !ini.empty() && !device.empty(), "C1 wiring: the sources the scans read are readable");

    // C1a: the family's arming, the frame thread, the composite
    auto wiredArming = [&](const std::string& vs, const std::string& ly, const std::string& nat) {
        return has(vs, "f.vs, kind, count, instances, [] { return scopeZoomMarkArmed() || scopeZoomCrispArmed(); },") &&
               before(ly, "g_seedCensus.door(eye, sequence);", "if (scopeZoomCrispArmed()) scopeZoomNoteCrispFrame(eye, e.wMarks != 0 && e.wSeq == sequence);") &&
               has(nat, "std::atomic<bool> g_crispReticleKey{true};") &&
               has(nat, "cfg.getString(\"experimental.native_on_foot_ads_crisp_reticle\", \"on\") == \"on\"") &&
               has(nat, "scopeZoomCrispPublish(armedNow);") && has(nat, "cg.adsUp = true;   // past the check above") && has(nat, "cg.lensKeyOn = key;") &&
               has(nat, "cg.lensOwned = gi.optical && gi.m >= sz::kLensMinM;") && has(nat, "cg.itemKnown = s.item.coherent && s.item.scope != loadout::kScopeUnknown;") &&
               has(nat, "scopeZoomCrispPublish(false);   // C1a: no press of the sights, no arming") && has(nat, "        scopeZoomCrispPublish(false);\n        nativeOnFootPublishAdsUp(false);") &&
               has(nat, "if (!s.crispAnnounced || crispKey != s.crispAnnouncedKey) announceCrisp(s, crispKey);") && has(nat, "if (s.crispSeen) {") &&
               has(nat, "edvr::crisp::formatPress(crispLine, sizeof(crispLine), cl);") && has(nat, "        if (eligible) {\n            s.crispSeen = true;") && has(nat, "        if (armedNow) ++s.crispArmedTicks;") &&
               has(nat, "scopeZoomCrispTake(&cl.door);") && has(nat, "cl.taken[0] = pl.d.markRedirected[0];");
    };
    check(wiredArming(vscreen, layer, native), "C1a wiring: the family asks the lens and then the crisp gate; the tick publishes the gate each tick and down on every other path; the composite counts the eye-frames; the key ships on");
    check(!wiredArming(replaced(vscreen, "[] { return scopeZoomMarkArmed() || scopeZoomCrispArmed(); },", "[] { return scopeZoomMarkArmed(); },"), layer, native), "control: a family that never asks the crisp gate is not the wiring");
    check(!wiredArming(replaced(vscreen, "scopeZoomMarkArmed() || scopeZoomCrispArmed()", "scopeZoomCrispArmed() && scopeZoomMarkArmed()"), layer, native), "control: both armings required is not the wiring");
    check(!wiredArming(vscreen, replaced(layer, "if (scopeZoomCrispArmed()) scopeZoomNoteCrispFrame(", "if (false) scopeZoomNoteCrispFrame("), native), "control: a composite that counts no eye-frame is not the wiring");
    check(!wiredArming(vscreen, layer, replaced(native, "cfg.getString(\"experimental.native_on_foot_ads_crisp_reticle\", \"on\") == \"on\"", "cfg.getString(\"experimental.native_on_foot_ads_crisp_reticle\", \"off\") == \"on\"")),
          "control: the reader's fallback off is not the shipped default");
    check(!wiredArming(vscreen, layer, replaced(native, "std::atomic<bool> g_crispReticleKey{true};", "std::atomic<bool> g_crispReticleKey{false};")), "control: the atomic's start value off is not the shipped default");
    check(!wiredArming(vscreen, layer, replaced(native, "cg.lensOwned = gi.optical && gi.m >= sz::kLensMinM;", "cg.lensOwned = false;")), "control: a gate that never leaves the lens its own sights is not the wiring");
    check(!wiredArming(vscreen, layer, replaced(native, "scopeZoomCrispPublish(false);   // C1a: no press of the sights, no arming", "")), "control: an arming not dropped when the press ends is not the wiring");
    check(!wiredArming(vscreen, layer, replaced(native, "        scopeZoomCrispPublish(false);\n        nativeOnFootPublishAdsUp(false);", "        nativeOnFootPublishAdsUp(false);")),
          "control: an arming left up when native on foot is not armed or the flat profile runs is not the wiring");
    check(!wiredArming(vscreen, layer, replaced(native, "        if (armedNow) ++s.crispArmedTicks;", "        ++s.crispArmedTicks;")), "control: armed ticks counted when the gate was not armed is not the wiring");
    check(!wiredArming(vscreen, layer, replaced(native, "edvr::crisp::formatPress(crispLine, sizeof(crispLine), cl);", "")), "control: a press that writes no crisp line is not the wiring");
    // the shipped file
    {
        const size_t full = ini.find("native_on_foot_ads_scope_zoom_full_detail = off"), key = ini.find("native_on_foot_ads_crisp_reticle = on");
        check(full != std::string::npos && key != std::string::npos && full < key && before(ini, "[experimental]", "native_on_foot_ads_crisp_reticle = on") &&
                  has(ini, "# ui: ADS: crisp reticle (experimental) | choices on, off | recommended on | live") && has(ini, "# TEMPORARY (C1a): ADS: crisp reticle:"),
              "C1a wiring: the ini states the key on, in the experimental section after the full-detail key, marked TEMPORARY, with its row");
        check(!has(ini, "native_on_foot_ads_crisp_reticle = off"), "C1a wiring: the key is not off in the shipped file");
    }

    // C1b: the draw path, the census order, the dump
    auto wiredWithhold = [&](const std::string& vs, const std::string& nat, const std::string& tp, const std::string& dev) {
        return before(vs, "drawCensusEyeDraw(self, kind, count, instances, s->eyeDrawsThisFrame, args);", "if (nativeOnFootSightWithholdLive()) {") &&
               before(vs, "if (nativeOnFootSightWithholdLive()) {", "if (s->censusSkipCount) {") &&
               has(vs, "if (nativeOnFootSightWithholdSkips(self, kind, count, instances, withholdRt ? withholdRt->resource : nullptr)) return DrawVerdict::kSkip;") &&
               has(nat, "return sightwithhold::skips(frame, eye, bindingShaderHash(BindSlot::Vs), count, instances, [] { return bindingShaderHash(BindSlot::Ps); });") &&
               has(nat, "if (!sightwithhold::arm(frame)) return false;") &&
               has(tp, "g_eyeRunWithhold = !companion && nativeOnFootSightWithholdArm();") && has(tp, "sightwithhold::disarm();   // C1b: a cycle left over from a run that never wrote ends here") &&
               has(tp, "if (g_eyeRunWithhold) sightwithhold::noteCapture(static_cast<uint32_t>(g_eyeRunTaken));") && has(tp, "if(g_eyeRunWithhold)sightwithhold::noteCapture(static_cast<uint32_t>(k));") &&
               has(tp, "if (g_eyeRunWithhold) sightwithhold::disarm();") && has(tp, "if (g_eyeRunWithhold) writeWithholdHeaderJson(f);") && has(tp, "if (g_eyeRunWithhold) writeWithholdFrameJson(f, k);") &&
               has(tp, "writeWithholdRecord(dir);") && has(nat, "nativeOnFootPublishAdsUp(ads);") &&
               // armed in one place: one call in the native module, one in the dump's begin, none in the device hook's key handler or the draw path
               has(dev, "temporalPassArmEyeDump") && countOf(nat, "sightwithhold::arm(") == 1 && countOf(tp, "nativeOnFootSightWithholdArm()") == 1 && countOf(vs, "sightwithhold::arm(") == 0 &&
               countOf(dev, "sightwithhold::arm(") == 0 && countOf(dev, "nativeOnFootSightWithholdArm") == 0;
    };
    check(wiredWithhold(vscreen, native, temporal, device),
          "C1b wiring: the draw path asks after the census and before the suppression probe; the cycle is armed in one place, by a manual dump (never the companion's), ended before the files are written, and recorded in the json");
    check(!wiredWithhold(replaced(vscreen, "if (nativeOnFootSightWithholdLive()) {", "if (false) {"), native, temporal, device), "control: a draw path that never asks is not the wiring");
    check(!wiredWithhold(vscreen, native, replaced(temporal, "g_eyeRunWithhold = !companion && nativeOnFootSightWithholdArm();", "g_eyeRunWithhold = nativeOnFootSightWithholdArm();"), device),
          "control: a cycle the airborne companion's dump may arm is not the wiring");
    check(!wiredWithhold(vscreen, native, replaced(temporal, "if (g_eyeRunWithhold) sightwithhold::disarm();", ""), device), "control: a cycle that outlives the dump's capture is not the wiring");
    check(!wiredWithhold(vscreen, native + "\nsightwithhold::arm(7);", temporal, device), "control: a second place that arms the cycle is not the wiring");
    check(!wiredWithhold(vscreen, native, temporal, device + "\nnativeOnFootSightWithholdArm();"), "control: the key handler arming the cycle without a dump is not the wiring");
    check(!wiredWithhold(vscreen, native, replaced(temporal, "if (g_eyeRunWithhold) writeWithholdFrameJson(f, k);", ""), device), "control: a json that does not say which candidate a frame left out is not the wiring");

    // C1b: the census' own budgets
    auto wiredCensus = [&](const std::string& c) {
        return has(c, "bool otherLinesSpent() { return g_lines - g_eyeLines >= g_maxLines; }") && has(c, "if (tag[2] == '\\0') {   // \"DC\": an eye draw, under its own budget") &&
               has(c, "if (g_eyeLines >= g_maxLines) return;") && countOf(c, "if (g_lines >= g_maxLines) return;") == 0 && has(c, "unseen=%u eye_lines=%u lines=%u interned=%u overflow=%u truncated=%u") &&
               has(c, "g_draws > g_drawLines ? g_draws - g_drawLines : 0") && has(c, "        g_eyeLines = 0;\n        g_drawLines = 0;");
    };
    check(wiredCensus(census), "C1b wiring: the eye draws have a line budget of their own, every other line is capped on its own, the end line says both, and a census starts both from nothing");
    check(!wiredCensus(replaced(census, "if (g_eyeLines >= g_maxLines) return;", "if (g_lines >= g_maxLines) return;")), "control: eye draws under the shared cap are not the wiring");
    check(!wiredCensus(census + "\n    if (g_lines >= g_maxLines) return;"), "control: another line kind under the shared cap is not the wiring");
}

}  // namespace c1cases
