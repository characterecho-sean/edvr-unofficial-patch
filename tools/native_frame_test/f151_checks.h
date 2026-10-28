// F151 checks (native on-foot VR), run151 below. Header only, run by native_frame_test.
//
// F151 (native_on_foot_f151.h, journal_loadout.h): the weapon readout's draw whose stream entry lay outside every shadow window is bound to the part's record by its place, and the
// held copy's scope state is read from the game's selected slot when the name finds two copies. The checks: the ordinals (the first draw of a mesh in an eye-frame is 0, a mesh
// shared by two draws is told by the ordinal, the eyes count apart, a new frame starts over); every guard of the bind refusing on its own (the eye, each field of the mesh, the
// ordinal, the instance count, the previous frame, the part drawn once, the key not named or taken in the eye-frame, exactly one tagged record in this frame's Unmap); one bind per key
// per eye-frame; a place never coming from a bind; the lost-frame count; and the scope by slot (an ambiguous loadout resolved by the selected slot, a slot whose item differs from the
// held name unknown, no selection unknown, an unambiguous name left alone).
#pragma once

#include "../../src/d3d11/journal_loadout.h"
#include "../../src/d3d11/native_on_foot_f144.h"
#include "../../src/d3d11/native_on_foot_f151.h"
#include "f140_checks.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace f151test {

namespace pf = edvr::nofoot::f151;
namespace ls = edvr::loadout;
namespace f144 = edvr::nofoot::f144;

inline pf::Mesh meshA() {
    pf::Mesh m;
    m.vb = reinterpret_cast<const void*>(0x1000);
    m.ib = reinterpret_cast<const void*>(0x2000);
    m.stride = 32;
    m.vbOffset = 64;
    m.ibOffset = 128;
    m.ibFmt = 57;
    m.count = 36;
    m.start = 12;
    m.base = -4;
    m.kind = 'X';
    m.valid = true;
    return m;
}
inline pf::Mesh meshB() {
    pf::Mesh m = meshA();
    m.vb = reinterpret_cast<const void*>(0x3000);
    m.count = 6;
    return m;
}

// One eye-frame's draws as the tee sees them: the ordinal of each, in order.
struct Frame {
    pf::Places& p;
    int eye;
    uint32_t frame;
    uint16_t next(const pf::Mesh& m) { return p.ordinal(eye, frame, m); }
};

// The find count a pool gives: `count` records for `key`, and the keys asked, to check the rule asks once and only for the place's key.
struct Finds {
    uint32_t key = 0, count = 1, asked = 0, askedKey = 0;
    uint32_t operator()(uint32_t k) {
        ++asked;
        askedKey = k;
        return k == key ? count : 0u;
    }
};

constexpr uint32_t kKey1 = 0xF7u;   // the part's second post of a frame (bit 7) in slot 7
constexpr uint32_t kKey0 = 0x77u;

// The baseline: eye 1, frame 10 draws A, B, A and the third (A, ordinal 1) is the weapon readout named by its stream entry, one instance, key kKey1 in slot 7.
inline void frame10(pf::Places& p, int eye = 1, uint32_t key = kKey1) {
    Frame f{p, eye, 10};
    f.next(meshA());
    f.next(meshB());
    const uint16_t ord = f.next(meshA());
    p.note(eye, 7, 10, meshA(), ord, 1, key);
}
// The same draws in frame 11 up to the readout's draw (its ordinal is returned): it is the lost one.
inline uint16_t frame11ToReadout(pf::Places& p, int eye = 1, uint32_t frame = 11) {
    Frame f{p, eye, frame};
    f.next(meshA());
    f.next(meshB());
    return f.next(meshA());
}

template <class Check>
inline void run151(Check&& check) {
    // ---- 1. the ordinals ------------------------------------------------------------------------------------------------------------------------------------------------
    {
        pf::Places p;
        check(p.ordinal(1, 10, meshA()) == 0 && p.ordinal(1, 10, meshA()) == 1 && p.ordinal(1, 10, meshA()) == 2, "F151: the first draw of a mesh in an eye-frame is ordinal 0, then 1, 2");
        check(p.ordinal(1, 10, meshB()) == 0 && p.ordinal(1, 10, meshA()) == 3 && p.ordinal(1, 10, meshB()) == 1, "F151: another mesh counts on its own, the first one's count goes on");
        check(p.ordinal(0, 10, meshA()) == 0 && p.ordinal(0, 10, meshA()) == 1, "F151: the other eye counts apart");
        check(p.ordinal(1, 11, meshA()) == 0 && p.ordinal(1, 11, meshA()) == 1, "F151: a new frame starts the count over");
        pf::Mesh none;
        check(p.ordinal(1, 11, none) == pf::kNoOrdinal && p.ordinal(-1, 11, meshA()) == pf::kNoOrdinal && p.ordinal(2, 11, meshA()) == pf::kNoOrdinal,
              "F151: a draw with no mesh (no icon table slot), or in no eye, has no ordinal");
        pf::Places q;
        bool all = true;
        for (uint32_t i = 0; i < pf::kMeshesPerFrame; ++i) {
            pf::Mesh m = meshA();
            m.start = 100 + i;
            all = all && q.ordinal(0, 5, m) == 0;
        }
        pf::Mesh extra = meshA();
        extra.start = 999;
        pf::Mesh first = meshA();
        first.start = 100;
        check(all && q.ordinal(0, 5, extra) == pf::kNoOrdinal && q.ordinal(0, 5, first) == 1,
              "F151: a mesh past the table's size has no ordinal, and the meshes counted keep theirs");
    }
    // ---- 2. the bind --------------------------------------------------------------------------------------------------------------------------------------------------
    {   // the baseline binds, asks the pool for the place's key once, and says which part and key
        pf::Places p;
        frame10(p);
        const uint16_t ord = frame11ToReadout(p);
        Finds finds;
        finds.key = kKey1;
        uint32_t slot = 99, key = 0;
        const pf::Verdict v = p.bind(1, 11, meshA(), ord, 1, finds, &slot, &key);
        check(ord == 1 && v == pf::kBound && slot == 7 && key == kKey1 && finds.asked == 1 && finds.askedKey == kKey1,
              "F151: the readout lost in eye 1 (named in the last frame at ordinal 1 of the mesh) is bound to slot 7 and its second-post key, and the pool is asked for that key once");
    }
    {   // the eye
        pf::Places p;
        frame10(p, 1);
        Finds finds;
        finds.key = kKey1;
        const uint16_t ord = frame11ToReadout(p, 0);
        check(p.bind(0, 11, meshA(), ord, 1, finds) == pf::kNoPlace && finds.asked == 0, "F151: a place kept in eye 1 does not bind a draw of eye 0");
        frame10(p, 0, kKey0);
        Finds finds0;
        finds0.key = kKey0;
        uint32_t slot = 99, key = 0;
        check(p.bind(0, 11, meshA(), ord, 1, finds0, &slot, &key) == pf::kBound && key == kKey0 && p.bind(1, 11, meshA(), frame11ToReadout(p, 1), 1, finds) == pf::kBound,
              "F151: each eye binds to the place it kept, with its own key");
    }
    {   // the mesh: each field of the draw's geometry, on its own
        pf::Places p;
        frame10(p);
        const uint16_t ord = frame11ToReadout(p);
        Finds finds;
        finds.key = kKey1;
        struct Case {
            const char* what;
            pf::Mesh m;
        };
        Case cases[10];
        int n = 0;
        auto add = [&](const char* what, pf::Mesh m) { cases[n++] = Case{what, m}; };
        { pf::Mesh m = meshA(); m.vb = reinterpret_cast<const void*>(0x1001); add("vertex buffer", m); }
        { pf::Mesh m = meshA(); m.ib = reinterpret_cast<const void*>(0x2001); add("index buffer", m); }
        { pf::Mesh m = meshA(); m.stride = 36; add("stride", m); }
        { pf::Mesh m = meshA(); m.vbOffset = 68; add("vertex buffer offset", m); }
        { pf::Mesh m = meshA(); m.ibOffset = 132; add("index buffer offset", m); }
        { pf::Mesh m = meshA(); m.ibFmt = 42; add("index format", m); }
        { pf::Mesh m = meshA(); m.count = 37; add("index count", m); }
        { pf::Mesh m = meshA(); m.start = 13; add("start", m); }
        { pf::Mesh m = meshA(); m.base = -3; add("base vertex", m); }
        { pf::Mesh m = meshA(); m.kind = 'I'; add("draw kind", m); }
        bool all = true;
        for (int i = 0; i < n; ++i) all = all && p.bind(1, 11, cases[i].m, ord, 1, finds) == pf::kOtherMesh;
        check(all && finds.asked == 0, "F151: a draw whose vertex buffer, index buffer, stride, either offset, index format, count, start, base vertex or kind differs is another mesh, each on its own");
        pf::Mesh bad;
        check(p.bind(1, 11, bad, ord, 1, finds) == pf::kNoMesh && p.bind(1, 11, meshA(), pf::kNoOrdinal, 1, finds) == pf::kNoMesh && finds.asked == 0,
              "F151: a draw with no mesh, or none in its ordinal count, is not bound");
        check(p.bind(1, 11, meshA(), ord, 1, finds) == pf::kBound, "F151: the unchanged mesh still binds (the cases above left no trace)");
    }
    {   // the ordinal: a mesh shared by two draws of an eye is bound by the ordinal that matches only
        pf::Places p;
        frame10(p);   // the readout is ordinal 1 of mesh A, draw 0 of A is something else
        Frame f{p, 1, 11};
        const uint16_t first = f.next(meshA());
        f.next(meshB());
        const uint16_t second = f.next(meshA());
        const uint16_t third = f.next(meshA());
        Finds finds;
        finds.key = kKey1;
        check(first == 0 && second == 1 && third == 2, "F151: (the frame's draws of the shared mesh are ordinals 0, 1, 2)");
        check(p.bind(1, 11, meshA(), first, 1, finds) == pf::kOtherOrdinal && p.bind(1, 11, meshA(), third, 1, finds) == pf::kOtherOrdinal && finds.asked == 0,
              "F151: the shared mesh's draws at ordinal 0 and 2 are not the readout (it sat at ordinal 1): refused as another ordinal");
        check(p.bind(1, 11, meshA(), second, 1, finds) == pf::kBound, "F151: the draw at ordinal 1 is");
        // an ordinal moved by a new draw of the mesh before it is the readout's no more
        pf::Places q;
        frame10(q);
        Frame g{q, 1, 11};
        g.next(meshA());
        g.next(meshA());
        const uint16_t moved = g.next(meshA());
        Finds f2;
        f2.key = kKey1;
        check(moved == 2 && q.bind(1, 11, meshA(), moved, 1, f2) == pf::kOtherOrdinal, "F151: a new draw of the mesh ahead of the readout moves it to ordinal 2: not the place");
    }
    {   // the instance count
        pf::Places p;
        frame10(p);
        const uint16_t ord = frame11ToReadout(p);
        Finds finds;
        finds.key = kKey1;
        check(p.bind(1, 11, meshA(), ord, 2, finds) == pf::kOtherInstances && p.bind(1, 11, meshA(), ord, 0, finds) == pf::kOtherInstances && finds.asked == 0,
              "F151: the same mesh and ordinal with another instance count (the readout is one instance) is refused");
    }
    {   // the previous frame
        pf::Places p;
        frame10(p);
        Finds finds;
        finds.key = kKey1;
        const uint16_t ord12 = frame11ToReadout(p, 1, 12);
        check(p.bind(1, 12, meshA(), ord12, 1, finds) == pf::kStale && finds.asked == 0, "F151: a place two frames old is not the last frame's: refused");
        const uint16_t ord10 = 1;
        check(p.bind(1, 10, meshA(), ord10, 1, finds) == pf::kStale, "F151: a place named in this very frame is not the last frame's: refused");
        const uint16_t ord9 = 1;
        check(p.bind(1, 9, meshA(), ord9, 1, finds) == pf::kStale, "F151: a frame number going back is refused");
        const uint16_t ord11 = frame11ToReadout(p, 1, 11);
        check(p.bind(1, 11, meshA(), ord11, 1, finds) == pf::kBound, "F151: the next frame binds");
        pf::Places none;
        check(none.bind(1, 11, meshA(), 1, 1, finds) == pf::kNoPlace, "F151: with no place kept at all the draw is not bound");
    }
    {   // a place never comes from a bind: the frame after a bind has no place of its own
        pf::Places p;
        frame10(p);
        Finds finds;
        finds.key = kKey1;
        const uint16_t o11 = frame11ToReadout(p, 1, 11);
        check(p.bind(1, 11, meshA(), o11, 1, finds) == pf::kBound, "F151: frame 11's lost draw is bound");
        const uint16_t o12 = frame11ToReadout(p, 1, 12);
        check(p.bind(1, 12, meshA(), o12, 1, finds) == pf::kStale, "F151: frame 12's lost draw is not: the bind in frame 11 left no place, only a named draw does");
        // a draw named in frame 12 puts the place back
        p.note(1, 7, 12, meshA(), o12, 1, kKey1);
        const uint16_t o13 = frame11ToReadout(p, 1, 13);
        check(p.bind(1, 13, meshA(), o13, 1, finds) == pf::kBound, "F151: a draw named in frame 12 makes frame 13's lost draw bindable again");
    }
    {   // the part drawn once: two draws of it named in one eye-frame are not one place
        pf::Places p;
        Frame f{p, 1, 10};
        f.next(meshA());
        const uint16_t second = f.next(meshA());
        p.note(1, 7, 10, meshA(), 0, 1, kKey1);
        p.note(1, 7, 10, meshA(), second, 1, kKey1);
        Finds finds;
        finds.key = kKey1;
        check(p.bind(1, 11, meshA(), 0, 1, finds) == pf::kTwoDraws && finds.asked == 0, "F151: a part named twice in its eye-frame has no one place: refused");
    }
    {   // the key: named in this eye-frame, taken already, and the pool's records
        pf::Places p;
        frame10(p);
        Finds finds;
        finds.key = kKey1;
        finds.count = 0;
        uint16_t ord = frame11ToReadout(p);
        check(p.bind(1, 11, meshA(), ord, 1, finds) == pf::kNoFind && finds.asked == 1, "F151: the key has no tagged record in this frame's Unmap: refused");
        finds.count = 2;
        check(p.bind(1, 11, meshA(), ord, 1, finds) == pf::kManyFinds, "F151: the key has two tagged records there: refused (which one the draw reads is not known)");
        finds.count = 1;
        check(p.bind(1, 11, meshA(), ord, 1, finds) == pf::kBound, "F151: one tagged record binds (the refusals before left nothing taken)");
        Finds again;
        again.key = kKey1;
        check(p.bind(1, 11, meshA(), ord, 1, again) == pf::kKeyBoundHere && again.asked == 0, "F151: a second bind to the key in the same eye-frame is refused");
        // the key named by a draw of this eye in this frame
        pf::Places q;
        frame10(q);
        const uint16_t o = frame11ToReadout(q);
        q.note(1, 3, 11, meshB(), 0, 1, kKey1);
        Finds f2;
        f2.key = kKey1;
        check(q.bind(1, 11, meshA(), o, 1, f2) == pf::kKeyNamedHere && f2.asked == 0, "F151: the key was named by a draw of this eye in this frame: refused");
        // the other eye's naming of its own key does not count against this eye
        pf::Places r;
        frame10(r);
        const uint16_t o2 = frame11ToReadout(r);
        r.note(0, 7, 11, meshA(), 1, 1, kKey1);
        Finds f3;
        f3.key = kKey1;
        check(r.bind(1, 11, meshA(), o2, 1, f3) == pf::kBound, "F151: the key named in the other eye in this frame does not stop this eye's bind (the eyes keep their own)");
        // a new eye-frame forgets the keys taken and named
        Finds f4;
        f4.key = kKey1;
        p.note(1, 7, 12, meshA(), 1, 1, kKey1);
        const uint16_t o13 = frame11ToReadout(p, 1, 13);
        check(p.bind(1, 13, meshA(), o13, 1, f4) == pf::kBound, "F151: the next eye-frame starts with no key taken");
    }
    {   // the find's frame
        check(pf::findIsThisUnmap(11, 11, 11), "F151: a find from this frame's Unmap, in this frame, counts");
        check(!pf::findIsThisUnmap(10, 10, 11), "F151: the pool's newest Unmap an older frame's: no find of it counts");
        check(!pf::findIsThisUnmap(10, 11, 11), "F151: an older find in this frame's Unmap does not count");
        check(!pf::findIsThisUnmap(12, 12, 11) && !pf::findIsThisUnmap(11, 12, 11), "F151: a later Unmap's finds do not count for an earlier frame's draw");
        check(!pf::findIsThisUnmap(11, ~0u, 11) && !pf::findIsThisUnmap(~0u, ~0u, ~0u), "F151: a pool never Unmapped has no finds");
    }
    {   // the reasons are named, each once
        bool named = true;
        for (int a = 0; a < pf::kVerdictCount; ++a) {
            named = named && std::strcmp(pf::verdictName(a), "?") != 0 && std::strchr(pf::verdictName(a), '(') == nullptr && std::strchr(pf::verdictName(a), ',') == nullptr;
            for (int b = a + 1; b < pf::kVerdictCount; ++b) named = named && std::strcmp(pf::verdictName(a), pf::verdictName(b)) != 0;
        }
        check(named && std::strcmp(pf::verdictName(pf::kBound), "bound") == 0 && std::strcmp(pf::verdictName(99), "?") == 0,
              "F151: every verdict has its own name, with no comma or bracket (the 1 s line lists them and --health reads the list back)");
    }
    // ---- 3. the lost frames -------------------------------------------------------------------------------------------------------------------------------------------
    {
        pf::Seen s;
        check(s.see(0, 100) == -1 && s.see(1, 100) == -1 && s.see(0, 101) == -1 && s.see(1, 101) == -1, "F151: frames in which both eyes drew the part lose none");
        check(s.see(0, 102) == -1 && s.see(0, 103) == 1 && s.see(1, 103) == -1, "F151: a frame in which only eye 0 drew it is counted lost for eye 1 when the next frame closes it");
        check(s.see(1, 104) == -1 && s.see(1, 105) == 0, "F151: and one in which only eye 1 drew it for eye 0 (frame 103 had both eyes and lost none)");
        check(s.see(0, 105) == -1 && s.see(0, 106) == -1 && s.see(1, 106) == -1 && s.see(2, 107) == -1 && s.see(-1, 107) == -1, "F151: a frame with both eyes in loses none; no eye is no draw");
        pf::Seen t;
        check(t.see(1, 7) == -1 && t.see(0, 9) == 0, "F151: the first frame seen is closed by the next draw, whatever the gap");
    }
    // ---- 4. the scope by the selected slot ------------------------------------------------------------------------------------------------------------------------
    {
        namespace f140t = f140test;
        ls::Loadout two;   // the pilot's two plasma snipers: PrimaryWeapon1 scoped, PrimaryWeapon2 not; the plasma pistol in the secondary slot
        f140t::feedLine(two, f140t::equipLine("PrimaryWeapon1", "wpn_m_sniper_plasma_charged", "\"weapon_range\", \"weapon_scope\""));
        f140t::feedLine(two, f140t::equipLine("PrimaryWeapon2", "wpn_m_sniper_plasma_charged", "\"weapon_range\""));
        f140t::feedLine(two, f140t::equipLine("SecondaryWeapon", "wpn_s_pistol_plasma_charged", "\"weapon_accuracy\""));
        const char* sniper = f140t::kSniper2;
        const ls::HeldScope byName = ls::scopeOfHeld(two, sniper);
        check(byName.state == ls::kScopeUnknown && byName.why == ls::kWhyAmbiguous && byName.slots == 2 && byName.slot == -1, "F151: (the name alone finds two snipers with different mods: unknown)");
        check(ls::slotOfSelectedId(2) == 0 && ls::slotOfSelectedId(3) == 1 && ls::slotOfSelectedId(4) == 2 && ls::slotOfSelectedId(1) == -1 && ls::slotOfSelectedId(5) == -1 &&
                  ls::slotOfSelectedId(0) == -1 && ls::slotOfSelectedId(0xFFFFFFFFu) == -1 && ls::slotOfSelectedId(0xFFFFFFFEu) == -1,
              "F151: the game's ids 2, 3, 4 are PrimaryWeapon1, PrimaryWeapon2, SecondaryWeapon; the tool (5), the holster and the rest name no slot");
        const ls::HeldScope s2 = ls::scopeOfHeldBySlot(two, sniper, true, 2), s3 = ls::scopeOfHeldBySlot(two, sniper, true, 3);
        check(s2.state == ls::kScopeYes && s2.slot == 0 && s2.why == ls::kWhyOk && s2.slots == 2 && s3.state == ls::kScopeNo && s3.slot == 1 && s3.why == ls::kWhyOk,
              "F151: the selected slot 2 is the scoped copy (yes, slot 0) and 3 the unscoped one (no, slot 1), as in the flight where the sniper's ids 2 and 3 went with E +5.00 and +16.00");
        check(ls::scopeOfHeldBySlot(two, "$WPN_M_Sniper_Plasma_Charged_NAME;", true, 2).state == ls::kScopeYes, "F151: the name is compared as everywhere (no case)");
        // a slot whose item is not the held name stays unknown
        const ls::HeldScope pistolSlot = ls::scopeOfHeldBySlot(two, sniper, true, 4);
        check(pistolSlot.state == ls::kScopeUnknown && pistolSlot.why == ls::kWhyAmbiguous && pistolSlot.slot == -1,
              "F151: the selection names the secondary slot, which holds the pistol, not the held sniper: unknown");
        // no selection, or none that names a slot, is unknown
        check(ls::scopeOfHeldBySlot(two, sniper, false, 2).state == ls::kScopeUnknown && ls::scopeOfHeldBySlot(two, sniper, false, 2).slot == -1,
              "F151: no readable selection (stale, suspended, or an item not held): unknown even with an id");
        check(ls::scopeOfHeldBySlot(two, sniper, true, 5).state == ls::kScopeUnknown && ls::scopeOfHeldBySlot(two, sniper, true, 0xFFFFFFFFu).state == ls::kScopeUnknown &&
                  ls::scopeOfHeldBySlot(two, sniper, true, 1).state == ls::kScopeUnknown && ls::scopeOfHeldBySlot(two, sniper, true, 6).state == ls::kScopeUnknown,
              "F151: the tool, the holster and ids outside the weapon slots: unknown");
        // an empty slot, a name in no slot, no name, no loadout
        ls::Loadout oneEmpty;
        f140t::feedLine(oneEmpty, f140t::equipLine("PrimaryWeapon1", "wpn_m_sniper_plasma_charged", "\"weapon_scope\""));
        f140t::feedLine(oneEmpty, f140t::equipLine("SecondaryWeapon", "wpn_m_sniper_plasma_charged", ""));
        check(ls::scopeOfHeld(oneEmpty, sniper).why == ls::kWhyAmbiguous && ls::scopeOfHeldBySlot(oneEmpty, sniper, true, 3).state == ls::kScopeUnknown,
              "F151: the selected slot is empty: unknown");
        check(ls::scopeOfHeldBySlot(two, f140t::kRocket, true, 2).why == ls::kWhyNotInSlots && ls::scopeOfHeldBySlot(two, "", true, 2).why == ls::kWhyNoName &&
                  ls::scopeOfHeldBySlot(two, nullptr, true, 2).state == ls::kScopeUnknown && ls::scopeOfHeldBySlot(ls::Loadout{}, sniper, true, 2).why == ls::kWhyNoLoadout,
              "F151: a name in no slot, no name and no loadout read: as before (unknown, with their reasons)");
        // a name that is not ambiguous is left to the name, whatever the selection says
        ls::Loadout one;
        f140t::feedLine(one, f140t::equipLine("PrimaryWeapon1", "wpn_m_sniper_plasma_charged", "\"weapon_scope\""));
        const ls::HeldScope solo = ls::scopeOfHeldBySlot(one, sniper, true, 3);
        check(solo.state == ls::kScopeYes && solo.slot == -1 && solo.why == ls::kWhyOk, "F151: one slot holds the sniper: the name settles it (yes), the selection is not read (slot -1)");
        ls::Loadout both;
        f140t::feedLine(both, f140t::equipLine("PrimaryWeapon1", "wpn_m_sniper_plasma_charged", "\"weapon_scope\""));
        f140t::feedLine(both, f140t::equipLine("PrimaryWeapon2", "wpn_m_sniper_plasma_charged", "\"weapon_scope\", \"weapon_range\""));
        const ls::HeldScope agree = ls::scopeOfHeldBySlot(both, sniper, true, 3);
        check(agree.state == ls::kScopeYes && agree.slot == -1, "F151: two scoped snipers agree by name: yes, the selection is not read");
        // what the consumers see: the scope state is known, so the F144 cache needs no copy key and the two copies are two entries by their scope
        check(f144::copyKey(s2.state, true, 2) == 0 && f144::copyKey(s3.state, true, 3) == 0 && s2.state != s3.state,
              "F151: with the scope resolved the copy key is 0 for both copies (the scope tells them apart, F150's slot key is not needed)");
        f144::Cache cache;
        f144::Entry* scoped = cache.get(sniper, s2.state, 100, f144::copyKey(s2.state, true, 2));
        f144::Entry* plain = cache.get(sniper, s3.state, 200, f144::copyKey(s3.state, true, 3));
        check(scoped && plain && scoped != plain && scoped->scope == ls::kScopeYes && plain->scope == ls::kScopeNo, "F151: the F144 cache keeps one entry for each copy, by its resolved scope");
        // the publication path: the same, through the published loadout
        ls::Loadout previous;
        const bool hadPrevious = ls::g_published.read(&previous);
        ls::g_published.publish(two);
        uint32_t gen = 0;
        const ls::HeldScope viaPub = ls::heldScopeBySlot(sniper, true, 3, &gen);
        const ls::HeldScope viaPubNone = ls::heldScopeBySlot(sniper, false, 3);
        check(viaPub.state == ls::kScopeNo && viaPub.slot == 1 && gen == two.generation && viaPubNone.state == ls::kScopeUnknown, "F151: through the published loadout: the selected slot settles the copy, no selection does not");
        if (hadPrevious) ls::g_published.publish(previous);
    }
}

}  // namespace f151test
