// F157 checks (native on-foot VR), run157 below. Header only, run by native_frame_test.
//
// F157 (native_on_foot_f157.h): the coat outfit's pack draw is left out of both eyes. The draw is told apart by the vertex shader, the pixel shader, the index count and the instance
// count (the flight edvr_gfx_20261028_105512's entry 79: VS 7B0DC42D383F694C, PS 0DF03E64DF9DBEF1, 30312 indices, 1 instance); it is left out only into an eye-sized target with stencil
// ref 21, and only while native on foot is active and F144's Gear_Back decision for the local avatar is the outer garment. Each requirement refuses on its own. The draws left out are
// counted by eye and by frame.
#pragma once

#include "../../src/d3d11/native_on_foot_f144.h"
#include "../../src/d3d11/native_on_foot_f157.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace f157test {

namespace f157 = edvr::nofoot::f157;
namespace f144 = edvr::nofoot::f144;

template <class Check>
inline void run157(Check&& check) {
    constexpr uint64_t vs = 0x7B0DC42D383F694Cull, ps = 0x0DF03E64DF9DBEF1ull;
    // ---- 1. the identity ----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f157::kCoatPackVs == vs && f157::kCoatPackPs == ps && f157::kCoatPackIndexCount == 30312u && f157::kCoatPackInstances == 1u && f157::kStencilRef == 21u,
              "F157: the pack's identity is the flight's entry 79: VS 7B0DC42D383F694C, PS 0DF03E64DF9DBEF1, 30312 indices, 1 instance, stencil ref 21");
        check(f157::packIdentity(vs, ps, 30312, 1), "F157: the flight's draw matches");
        check(!f157::packIdentity(vs ^ 1ull, ps, 30312, 1), "F157: another vertex shader does not");
        check(!f157::packIdentity(vs, ps ^ 1ull, 30312, 1), "F157: another pixel shader does not");
        check(!f157::packIdentity(vs, ps, 3474, 1), "F157: entry 78 (the same shaders, 3474 indices) does not: it stays drawn");
        check(!f157::packIdentity(vs, ps, 30311, 1) && !f157::packIdentity(vs, ps, 30313, 1), "F157: an index count one off does not");
        check(!f157::packIdentity(vs, ps, 30312, 2) && !f157::packIdentity(vs, ps, 30312, 0), "F157: another instance count does not");
        check(!f157::packIdentity(0, 0, 0, 0), "F157: nothing bound does not");
        check(f157::countsMatch(30312, 1) && !f157::countsMatch(3474, 1) && !f157::countsMatch(30312, 2) && !f157::countsMatch(0, 0),
              "F157: the cheap match, from the call's own arguments, is the index count and the instance count");
    }
    // ---- 2. every requirement refuses on its own ------------------------------------------------------------------------------------------------------------------------------------
    {
        check(f157::hides(true, true, 21, vs, ps, 30312, 1), "F157: armed, an eye target, stencil ref 21 and the identity: left out");
        check(!f157::hides(false, true, 21, vs, ps, 30312, 1), "F157: ... the coat outfit not armed: drawn");
        check(!f157::hides(true, false, 21, vs, ps, 30312, 1), "F157: ... not an eye-sized target (a shadow map or an offscreen target): drawn");
        check(!f157::hides(true, true, 20, vs, ps, 30312, 1) && !f157::hides(true, true, 22, vs, ps, 30312, 1) && !f157::hides(true, true, 0, vs, ps, 30312, 1),
              "F157: ... another stencil ref (another person's body, or none): drawn");
        check(!f157::hides(true, true, 21, vs ^ 1ull, ps, 30312, 1) && !f157::hides(true, true, 21, vs, ps ^ 1ull, 30312, 1), "F157: ... another shader: drawn");
        check(!f157::hides(true, true, 21, vs, ps, 3474, 1) && !f157::hides(true, true, 21, vs, ps, 30312, 2), "F157: ... another index count or instance count: drawn");
        check(!f157::hides(false, false, 0, 0, 0, 0, 0), "F157: ... and a draw that is none of these: drawn");
    }
    // ---- 3. the outfit gate: native on foot active and Gear_Back decided as the outer garment -----------------------------------------------------------------------------------------
    {
        const auto armedFor = [](bool active, int decision) { return f157::coatArmed(active, f144::gearBackPromoted(decision)); };
        check(armedFor(true, f144::kGearOuter), "F157: native on foot active and the outer garment (the coat): armed");
        check(!armedFor(true, f144::kGearSuitArms), "F157: ... the other suit (suit arms present): not armed, its backpack is drawn");
        check(!armedFor(true, f144::kGearNoArms), "F157: ... an outfit with no outer-garment arms: not armed");
        check(!armedFor(true, f144::kGearUnknown), "F157: ... Gear_Back not decided yet: not armed");
        check(!armedFor(false, f144::kGearOuter), "F157: the coat decided but native on foot not active: not armed");
        check(!armedFor(false, f144::kGearUnknown), "F157: ... neither: not armed");
        // the gate carries through to the hide: only the coat's armed state leaves the draw out
        const int decisions[4] = {f144::kGearUnknown, f144::kGearOuter, f144::kGearSuitArms, f144::kGearNoArms};
        int hidden = 0;
        for (int d : decisions)
            for (int active = 0; active < 2; ++active)
                if (f157::hides(armedFor(active != 0, d), true, 21, vs, ps, 30312, 1)) ++hidden;
        check(hidden == 1, "F157: of the eight combinations of active and decision, exactly one leaves the pack out: active and the outer garment");
    }
    // ---- 4. the counts: by eye, and the frames once ----------------------------------------------------------------------------------------------------------------------------------
    {
        f157::SkipCounts c;
        f157::SkipCounts::Taken t = c.take();
        check(t.total() == 0 && t.frames == 0, "F157: nothing left out: the counts are 0");
        c.count(0, 100);
        c.count(1, 100);
        c.count(0, 101);
        c.count(1, 101);
        c.count(-1, 101);
        c.count(7, 102);
        t = c.take();
        check(t.eye[0] == 2 && t.eye[1] == 2 && t.eye[2] == 2 && t.total() == 6, "F157: the draws left out are counted in eye 0, eye 1 and the eye the target's table could not name");
        check(t.frames == 3, "F157: ... and the frames with at least one, once each however many draws it held (frames 100, 101, 102)");
        t = c.take();
        check(t.total() == 0 && t.frames == 0, "F157: taking the counts starts them again");
        c.count(0, 102);
        c.count(0, 103);
        t = c.take();
        check(t.eye[0] == 2 && t.frames == 1, "F157: ... a draw left out in the frame the last take counted is not that frame's second count (frame 102), and frame 103 is counted");
        for (uint32_t f = 200; f < 260; ++f) {
            c.count(0, f);
            c.count(1, f);
        }
        t = c.take();
        check(t.eye[0] == 60 && t.eye[1] == 60 && t.frames == 60, "F157: a second of 60 frames with the pack in both eyes: 60, 60 and 60 frames (the flight's 55 to 59)");
    }
    // ---- 5. one writer counting while the reader takes: no count is lost or counted twice ------------------------------------------------------------------------------------------------
    {
        f157::SkipCounts c;
        constexpr uint32_t kDraws = 20000;
        std::atomic<bool> done{false};
        uint64_t seen0 = 0, seen1 = 0;
        std::thread reader([&] {
            while (!done.load(std::memory_order_acquire)) {
                const f157::SkipCounts::Taken t = c.take();
                seen0 += t.eye[0];
                seen1 += t.eye[1];
            }
        });
        for (uint32_t i = 0; i < kDraws; ++i) {
            c.count(0, i / 2);
            c.count(1, i / 2);
        }
        done.store(true, std::memory_order_release);
        reader.join();
        const f157::SkipCounts::Taken rest = c.take();
        check(seen0 + rest.eye[0] == kDraws && seen1 + rest.eye[1] == kDraws, "F157: a writer counting 20000 draws in each eye while the frame thread takes: every one is taken once");
    }
}

}  // namespace f157test
