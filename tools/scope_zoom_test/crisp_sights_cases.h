// C1: the crisp sights (docs\design-vr-scope-magnification-2026-10-28.md, section 12), included by scope_zoom_test.cpp after its helpers (check, has, before, replaced, readFile).
//
// C1a, the crisp reticle: the arming gate (crisp_reticle.h) driven as a truth table (the hip never, the sights' edges, the lens' own sights), the door side of the arming
// (scope_zoom.cpp: publish, freshness, the eye-frame counts, the press's reset), the press and announce lines, and the wiring: the family's arming asks the lens and then the crisp gate,
// the frame thread publishes the gate every tick and down on every other path, the composite counts the eye-frames, and the retired key (experimental.native_on_foot_ads_crisp_reticle) is read nowhere.
// The draw census' own line budgets (the eye draws' and the other lines') are scanned in the wiring too. The sight census' withhold cycle (a manual eye dump with the sights up leaving one candidate
// sight-effect draw out of each frame) is retired: its header is gone, and so are its draw-path hook, its arming at the dump key and its records in the dump's json.
#pragma once

namespace c1cases {

namespace cr = edvr::crisp;

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
    g.onFoot = g.adsUp = true;
    g.lensKeyOn = true;
    g.itemKnown = true;
    g.lensOwned = false;
    g.wFresh = true;
    g.w = 1.0;
    check(cr::eligible(g) && cr::armed(g), "C1 gate: on foot, the sights up, a weapon the lens does not name, the weight 1: eligible and armed");
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
    // every combination of the seven facts
    bool never = true, exactlyOn = true;
    for (unsigned bits = 0; bits < 128; ++bits) {
        cr::GateIn t;
        t.onFoot = bits & 1u;
        t.adsUp = bits & 2u;
        t.lensKeyOn = bits & 4u;
        t.itemKnown = bits & 8u;
        t.lensOwned = bits & 16u;
        t.wFresh = bits & 32u;
        t.w = (bits & 64u) ? 1.0 : 0.0;
        // the lens and this gate never both arm the quad: a sight the lens owns is never armed here
        never = never && !(t.lensKeyOn && t.lensOwned && cr::armed(t));
        // and the gate is exactly: on foot, the sights up, and the lens not the owner (or no lens)
        const bool lensServes = !t.lensKeyOn || (t.itemKnown && !t.lensOwned);
        const bool atHip = t.wFresh && !(t.w > 0.0);
        exactlyOn = exactlyOn && cr::eligible(t) == (t.onFoot && t.adsUp && lensServes) && cr::armed(t) == (t.onFoot && t.adsUp && lensServes && !atHip);
    }
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
              std::strcmp(line, "crisp reticle (C1): press 7 item $wpn_m_assaultrifle_kinetic_fauto_name; scope no; eligible ticks 300, armed ticks 280; armed eye-frames L 270 R 271, "
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
    const int an = cr::formatAnnounce(ann, sizeof(ann));
    check(an > 0 && static_cast<size_t>(an) < sizeof(ann) - 1 && static_cast<size_t>(an) == std::strlen(ann) &&
              std::strcmp(ann, "crisp reticle (C1): the gunsight quad of every weapon the scope lens does not name is drawn into the sharp weapons layer for the whole press of the sights "
                               "(from the raise to the end of the lower, never at the hip), composited at the game's tonemap and out of the bloom input; one line a press of those follows") == 0,
          "C1 line: the announce's exact text, with room to spare in its buffer");
    check(std::strstr(ann, "key") == nullptr && std::strstr(line, " key ") == nullptr, "C1 line: neither the announce nor the press line names a key (the key is retired)");
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

inline void wiringTests() {
    const std::string vscreen = readFile("src/d3d11/vscreen.cpp"), layer = readFile("src/d3d11/ui_layer.cpp"), native = readFile("src/d3d11/native_on_foot.cpp"),
                      temporal = readFile("src/d3d11/temporal_pass.cpp"), census = readFile("src/d3d11/draw_census.cpp"), ini = readFile("edvr.ini");
    check(!vscreen.empty() && !layer.empty() && !native.empty() && !temporal.empty() && !census.empty() && !ini.empty(), "C1 wiring: the sources the scans read are readable");

    // C1a: the family's arming, the frame thread, the composite
    auto wiredArming = [&](const std::string& vs, const std::string& ly, const std::string& nat) {
        return has(vs, "f.vs, kind, count, instances, [] { return scopeZoomMarkArmed() || scopeZoomCrispArmed(); },") &&
               before(ly, "g_seedCensus.door(eye, sequence);", "if (scopeZoomCrispArmed()) scopeZoomNoteCrispFrame(eye, e.wMarks != 0 && e.wSeq == sequence);") &&
               has(nat, "scopeZoomCrispPublish(armedNow);") && has(nat, "cg.adsUp = true;   // past the check above") && has(nat, "cg.lensKeyOn = key;") &&
               has(nat, "cg.lensOwned = gi.optical && gi.m >= sz::kLensMinM;") && has(nat, "cg.itemKnown = s.item.coherent && s.item.scope != loadout::kScopeUnknown;") &&
               has(nat, "scopeZoomCrispPublish(false);   // C1a: no press of the sights, no arming") && has(nat, "        scopeZoomCrispPublish(false);\n        scopeZoomPublish(out);") &&
               has(nat, "if (!s.crispAnnounced) announceCrisp(s);") && has(nat, "if (s.crispSeen) {") &&
               has(nat, "edvr::crisp::formatPress(crispLine, sizeof(crispLine), cl);") && has(nat, "        if (eligible) {\n            s.crispSeen = true;") && has(nat, "        if (armedNow) ++s.crispArmedTicks;") &&
               has(nat, "scopeZoomCrispTake(&cl.door);") && has(nat, "cl.taken[0] = pl.d.markRedirected[0];");
    };
    check(wiredArming(vscreen, layer, native), "C1a wiring: the family asks the lens and then the crisp gate; the tick publishes the gate each tick and down on every other path; the composite counts the eye-frames");
    check(!wiredArming(replaced(vscreen, "[] { return scopeZoomMarkArmed() || scopeZoomCrispArmed(); },", "[] { return scopeZoomMarkArmed(); },"), layer, native), "control: a family that never asks the crisp gate is not the wiring");
    check(!wiredArming(replaced(vscreen, "scopeZoomMarkArmed() || scopeZoomCrispArmed()", "scopeZoomCrispArmed() && scopeZoomMarkArmed()"), layer, native), "control: both armings required is not the wiring");
    check(!wiredArming(vscreen, replaced(layer, "if (scopeZoomCrispArmed()) scopeZoomNoteCrispFrame(", "if (false) scopeZoomNoteCrispFrame("), native), "control: a composite that counts no eye-frame is not the wiring");
    check(!wiredArming(vscreen, layer, replaced(native, "if (!s.crispAnnounced) announceCrisp(s);", "")), "control: a frame thread that never announces the crisp reticle is not the wiring");
    check(!wiredArming(vscreen, layer, replaced(native, "cg.lensOwned = gi.optical && gi.m >= sz::kLensMinM;", "cg.lensOwned = false;")), "control: a gate that never leaves the lens its own sights is not the wiring");
    check(!wiredArming(vscreen, layer, replaced(native, "scopeZoomCrispPublish(false);   // C1a: no press of the sights, no arming", "")), "control: an arming not dropped when the press ends is not the wiring");
    check(!wiredArming(vscreen, layer, replaced(native, "        scopeZoomCrispPublish(false);\n        scopeZoomPublish(out);", "        scopeZoomPublish(out);")),
          "control: an arming left up when native on foot is not armed or the flat profile runs is not the wiring");
    check(!wiredArming(vscreen, layer, replaced(native, "        if (armedNow) ++s.crispArmedTicks;", "        ++s.crispArmedTicks;")), "control: armed ticks counted when the gate was not armed is not the wiring");
    check(!wiredArming(vscreen, layer, replaced(native, "edvr::crisp::formatPress(crispLine, sizeof(crispLine), cl);", "")), "control: a press that writes no crisp line is not the wiring");
    {   // C1a: on is the behaviour. No key (experimental.native_on_foot_ads_crisp_reticle: its reader, atomic, gate field and ini entry are gone, and so is the startup note that named it)
        const std::string gateHeader = readFile("src/d3d11/crisp_reticle.h");
        auto noKey = [&](const std::string& nat, const std::string& ini2, const std::string& hdr) {
            return !has(nat, "native_on_foot_ads_crisp_reticle") && !has(ini2, "native_on_foot_ads_crisp_reticle") && !has(hdr, "native_on_foot_ads_crisp_reticle") && !has(nat, "g_crispReticleKey") &&
                   !has(nat, "crispKey") && !has(nat, "crispAnnouncedKey") && !has(nat, "cg.keyOn") && !has(hdr, "keyOn") && !has(nat, "ADS crisp reticle (C1, TEMPORARY)") && has(hdr, "inline bool eligible(");
        };
        check(noKey(native, ini, gateHeader), "C1a: the crisp reticle key is gone: no reader, atomic, gate field, ini entry or startup note, and the gate is the one function");
        check(!noKey(native + "\nconst bool crispOn = cfg.getString(\"experimental.native_on_foot_ads_crisp_reticle\", \"on\") == \"on\";", ini, gateHeader), "control: a source that reads the crisp reticle key again is not the wiring");
        check(!noKey(native, ini + "\nnative_on_foot_ads_crisp_reticle = on\n", gateHeader), "control: an ini that carries the crisp reticle key again is not the wiring");
        check(!noKey(native, ini, gateHeader + "\n    bool keyOn = false;"), "control: a gate that carries a key field again is not the wiring");
    }

    // the draw census' own budgets
    auto wiredCensus = [&](const std::string& c) {
        return has(c, "bool otherLinesSpent() { return g_lines - g_eyeLines >= g_maxLines; }") && has(c, "if (tag[2] == '\\0') {   // \"DC\": an eye draw, under its own budget") &&
               has(c, "if (g_eyeLines >= g_maxLines) return;") && countOf(c, "if (g_lines >= g_maxLines) return;") == 0 && has(c, "unseen=%u eye_lines=%u lines=%u interned=%u overflow=%u truncated=%u") &&
               has(c, "g_draws > g_drawLines ? g_draws - g_drawLines : 0") && has(c, "        g_eyeLines = 0;\n        g_drawLines = 0;");
    };
    check(wiredCensus(census), "census wiring: the eye draws have a line budget of their own, every other line is capped on its own, the end line says both, and a census starts both from nothing");
    check(!wiredCensus(replaced(census, "if (g_eyeLines >= g_maxLines) return;", "if (g_lines >= g_maxLines) return;")), "control: eye draws under the shared cap are not the wiring");
    check(!wiredCensus(census + "\n    if (g_lines >= g_maxLines) return;"), "control: another line kind under the shared cap is not the wiring");

    {   // the sight census' withhold cycle is retired: its header does not exist, and the draw path, the native module, the dump's begin and its json carry nothing of it, so a manual eye dump with the sights up is a plain dump
        const std::string nativeHeader = readFile("src/d3d11/native_on_foot.h");
        auto noWithhold = [&](bool headerExists, const std::string& nat, const std::string& natHdr, const std::string& vs, const std::string& tp) {
            if (headerExists) return false;
            for (const std::string* t : {&nat, &natHdr, &vs, &tp})
                if (has(*t, "nativeOnFootSightWithhold") || has(*t, "sight census (C1b)") || has(*t, "sight_withhold") || has(*t, "sightwithhold::") || has(*t, "g_eyeRunWithhold")) return false;
            return true;
        };
        const bool headerExists = std::ifstream("src/d3d11/sight_withhold.h", std::ios::binary).is_open();
        check(!nativeHeader.empty() && noWithhold(headerExists, native, nativeHeader, vscreen, temporal),
              "C1b retired: sight_withhold.h does not exist, and the native module, its header, the draw path and the eye dump carry no withhold hook, arming, log line, record or json key");
        check(!noWithhold(true, native, nativeHeader, vscreen, temporal), "control: a sight_withhold.h that exists again is not the wiring");
        check(!noWithhold(false, native + "\nbool nativeOnFootSightWithholdArm();", nativeHeader, vscreen, temporal), "control: a native module that arms the cycle again is not the wiring");
        check(!noWithhold(false, native, nativeHeader, vscreen, temporal + "\nfprintf(f, \"  \\\"sight_withhold\\\": {\");"), "control: a dump whose json carries the withhold record again is not the wiring");
    }
}

}  // namespace c1cases
