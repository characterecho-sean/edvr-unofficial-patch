// F113 checks (native on-foot VR): each eye's scene rows whenever the world is drawn. Header only, run by native_frame_test (run113).
// The sources from the F57 row census (the F98 flight and a tester's): the depth prepass (VS F516BF0201303B87, PS B40B0462256E31C2) and the terrain patch
// (VS ACE405F428C17EF6, no PS) carried the eye's main scene content in every frame both drew; VS 53211E8C072CD02E carried another camera.
#pragma once

#include "../../src/d3d11/native_on_foot_f100.h"
#include "../../src/d3d11/native_on_foot_f113.h"

#include <cstdint>

namespace f113test {

namespace f113 = edvr::nofoot::f113;
namespace f87 = edvr::nofoot::f87;
namespace f100 = edvr::nofoot::f100;

template <class Check>
inline void run113(Check&& check) {
    {   // the sources: the prepass pair and the terrain patch without a pixel shader; not their shaders with another partner, not the other camera, not a mesh
        const bool prepass = f113::rowSourceOf(0xF516BF0201303B87ull, 0xB40B0462256E31C2ull) == f113::kFromPrepass;
        const bool terrain = f113::rowSourceOf(0xACE405F428C17EF6ull, 0) == f113::kFromTerrain;
        const bool notOther = f113::rowSourceOf(0xF516BF0201303B87ull, 0x1234ull) == f113::kRowSources &&
                              f113::rowSourceOf(0xACE405F428C17EF6ull, 0x43E5E6EB67AC751Bull) == f113::kRowSources &&
                              f113::rowSourceOf(0x53211E8C072CD02Eull, 0x7EAC71963E66C5FEull) == f113::kRowSources &&
                              f113::rowSourceOf(0xEB5234DB6ADB491Dull, 0xCB9F297EFF264251ull) == f113::kRowSources;
        const bool vsTest = f113::rowSourceVs(0xF516BF0201303B87ull) && f113::rowSourceVs(0xACE405F428C17EF6ull) && !f113::rowSourceVs(0x53211E8C072CD02Eull);
        check(prepass && terrain && notOther && vsTest,
              "F113: the row sources are the depth prepass pair and the terrain patch with no pixel shader; another partner, the other camera's shader and a mesh are not");
    }
    {   // a second's accounting: each frame's rows by source, the frames with none
        f113::EyeSecond s;
        for (int i = 0; i < 10; ++i) s.frameEnd(true, f113::kFromMeshes);
        for (int i = 0; i < 70; ++i) s.frameEnd(true, f113::kFromPrepass);
        for (int i = 0; i < 8; ++i) s.frameEnd(true, f113::kFromTerrain);
        for (int i = 0; i < 2; ++i) s.frameEnd(false, f113::kFromMeshes);
        check(s.frames == 90 && s.from[f113::kFromMeshes] == 10 && s.from[f113::kFromPrepass] == 70 && s.from[f113::kFromTerrain] == 8 && s.none == 2,
              "F113: a second's frames by the source of their rows (meshes 10, prepass 70, terrain 8) and the frames with none (2)");
    }
    {   // the check: due once a second an eye; while one is pending the tee keeps its slow path (no fast frame); the guard latches on a difference
        f113::CheckClock c;
        const bool first = c.due(0, 5000);
        c.done(0, 5000);
        const bool soon = c.due(0, 5900), later = c.due(0, 6000), other = c.due(1, 5900);
        f100::TeeState st;
        st.polledFrame = 7;
        st.worldOk[0] = st.worldOk[1] = 7;
        const uint32_t fast = f100::teeFastFrame(st, 7);
        st.check = true;
        const uint32_t held = f100::teeFastFrame(st, 7);
        f87::FbGuard g;
        const bool same = g.compare(0.0);
        const bool off = !g.compare(2e-5) && g.latchedOff;
        check(first && !soon && later && other && fast == 7 && held == f100::kNoFrame && same && off,
              "F113: the check is due once a second an eye; a pending check keeps the tee's slow path for the frame; a difference over 1e-5 latches the sources off");
    }
}

}  // namespace f113test
