// F130 checks (native on-foot VR): one disagreeing scene mesh no longer switches the eye rows' sources off. Header only, run by native_frame_test (run130).
// The flight (edvr_gfx_20261027_064839, the SRV training scenario): every check agreed (max diff 0) until a mesh that drew into eye 0 for 8 frames differed from the
// terrain's rows by 2.393e-01; F113 latched both sources off for the session at that check and the weapon UI pin lost its rows.
#pragma once

#include "../../src/d3d11/native_on_foot_f113.h"
#include "../../src/d3d11/native_on_foot_f130.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace f130test {

namespace f113 = edvr::nofoot::f113;
namespace f130 = edvr::nofoot::f130;
namespace f87 = edvr::nofoot::f87;

// A mini session as the render thread runs it for one eye: which rows the eye has in a frame and from whom, the marks, the sources' state. Mirrors rowsCheck,
// rowSourceDraw and worldDraw's gate with the same pure helpers.
struct Eye {
    f130::MarkTable marks;
    bool have = false;
    bool fromMeshes = false;
    bool meshCpu = false;
    bool pending = false;
    float rows[20] = {};
    f130::MeshId mesh;
    uint32_t replaced = 0, refused = 0, checked = 0;
    void newFrame() {
        have = fromMeshes = meshCpu = pending = false;
    }
    // A scene mesh draw: gated by its mark, the eye's rows if it has none, else the check against a source's when due.
    void meshDraw(const f130::MeshId& id, const float r[20]) {
        if (marks.isMarked(id)) {
            ++refused;
            return;
        }
        if (have) {
            if (!fromMeshes && pending) {
                pending = false;
                ++checked;
                const f130::RowsDiff d = f130::rowsDiff(rows, r);
                if (f130::disagree(d.max())) marks.mark(id);
            }
            return;
        }
        have = fromMeshes = meshCpu = true;
        for (int i = 0; i < 20; ++i) rows[i] = r[i];
        mesh = id;
        pending = true;
    }
    // A source draw: its rows if the eye has none, else (when due) the comparison with the mesh's.
    void sourceDraw(const float r[20], bool due) {
        const f130::SourceAction a = f130::sourceAction(have, fromMeshes, meshCpu, pending);
        if (a == f130::kSrcPublish) {
            have = true;
            fromMeshes = false;
            for (int i = 0; i < 20; ++i) rows[i] = r[i];
            pending = due;
        } else if (a == f130::kSrcCompare) {
            pending = false;
            ++checked;
            const f130::RowsDiff d = f130::rowsDiff(rows, r);
            if (f130::disagree(d.max())) {
                marks.mark(mesh);
                ++replaced;
                for (int i = 0; i < 20; ++i) rows[i] = r[i];
                fromMeshes = false;
            }
        }
    }
};

template <class Check>
inline void run130(Check&& check) {
    {   // the table: marked once, remembered by the whole identity, 32 kept, the rest counted
        f130::MarkTable t;
        f130::MeshId a{0x1111, 0x2222, 0xB0F0, 0};
        f130::MeshId sameShadersOtherBuffer{0x1111, 0x2222, 0xB1F0, 0};
        f130::MeshId sameBufferOtherOffset{0x1111, 0x2222, 0xB0F0, 64};
        const f130::MarkResult first = t.mark(a), again = t.mark(a);
        check(first == f130::kMarkedNew && again == f130::kMarkedAlready && t.n == 1 && t.isMarked(a) && !t.isMarked(sameShadersOtherBuffer) && !t.isMarked(sameBufferOtherOffset),
              "F130 marks: an identity is the shaders, cb1's buffer and its first constant; marking it twice is one mark, and the same shaders on another buffer or offset are not it");
        for (uint64_t i = 1; i < f130::kMarkMax; ++i) t.mark(f130::MeshId{i, i, i, 0});
        const f130::MarkResult full = t.mark(f130::MeshId{999, 999, 999, 0});
        check(t.n == f130::kMarkMax && full == f130::kMarkTableFull && t.overflow == 1 && !t.isMarked(f130::MeshId{999, 999, 999, 0}) && t.mark(a) == f130::kMarkedAlready,
              "F130 marks: 32 identities are kept; the 33rd is counted (overflow 1) and not marked, and a marked one stays marked");
    }
    {   // the difference, split into the rows and the origin; the tolerance; a NaN
        float a[20] = {}, b[20] = {};
        for (int i = 0; i < 20; ++i) a[i] = b[i] = 0.5f * static_cast<float>(i);
        const f130::RowsDiff same = f130::rowsDiff(a, b);
        b[3] += 0.2393f;
        const f130::RowsDiff rows = f130::rowsDiff(a, b);
        b[3] = a[3];
        b[18] += 0.05f;
        const f130::RowsDiff origin = f130::rowsDiff(a, b);
        b[18] = a[18];
        b[7] = std::nanf("");
        const f130::RowsDiff nan = f130::rowsDiff(a, b);
        check(same.max() == 0.0 && std::fabs(rows.rows - 0.2393) < 1e-6 && rows.origin == 0.0 && origin.rows == 0.0 && std::fabs(origin.origin - 0.05) < 1e-6 &&
                  std::fabs(origin.max() - 0.05) < 1e-6 && nan.rows >= 1e29,
              "F130 diff: the largest difference is split into the 16 row floats and the 4 origin floats; a NaN counts as infinite");
        check(!f130::disagree(0.0) && !f130::disagree(f87::kFbTolerance) && f130::disagree(f87::kFbTolerance * 1.01) && f130::disagree(0.2393) && f130::disagree(1e30),
              "F130 diff: the same tolerance as F113 (1e-5): one over it is a disagreement");
    }
    {   // what a source's draw does
        using namespace f130;
        check(sourceAction(false, false, false, false) == kSrcPublish && sourceAction(false, true, true, true) == kSrcPublish &&
                  sourceAction(true, true, true, true) == kSrcCompare && sourceAction(true, true, true, false) == kSrcSkip && sourceAction(true, true, false, true) == kSrcSkip &&
                  sourceAction(true, false, true, true) == kSrcSkip,
              "F130 source draw: no rows yet publishes; rows from a scene mesh known on the CPU compare when the once-a-second check is due; a staged mesh's, a source's own and an "
              "undue frame are left alone");
        check(std::string(sourceNoun(f113::kFromPrepass)) == "depth prepass" && std::string(sourceNoun(f113::kFromTerrain)) == "terrain",
              "F130 lines: the event line names the source it disagreed with");
    }
    {   // the flight: the terrain gave the rows, a mesh disagreed at one check: the mesh is marked, the sources stay, the pin keeps its rows
        Eye eye;
        float terrain[20] = {}, srv[20] = {}, main[20] = {};
        for (int i = 0; i < 20; ++i) terrain[i] = main[i] = 0.01f * static_cast<float>(i);
        for (int i = 0; i < 20; ++i) srv[i] = terrain[i] + (i == 4 ? 0.2393f : 0.0f);
        const f130::MeshId mainMesh{0xA1, 0xB1, 0xC0DE, 0}, srvMesh{0xA1, 0xB1, 0xC0DE, 96};
        // the earlier checks of the session agreed
        for (int k = 0; k < 5; ++k) {
            eye.newFrame();
            eye.sourceDraw(terrain, true);
            eye.meshDraw(mainMesh, main);
        }
        check(eye.checked == 5 && eye.marks.n == 0 && eye.have && !eye.fromMeshes, "F130 flight: the five agreeing checks mark nothing");
        // the SRV mesh draws into the eye after the terrain: disagrees
        eye.newFrame();
        eye.sourceDraw(terrain, true);
        eye.meshDraw(srvMesh, srv);
        check(eye.marks.n == 1 && eye.marks.isMarked(srvMesh) && !eye.marks.isMarked(mainMesh) && eye.have && !eye.fromMeshes && eye.rows[4] == terrain[4],
              "F130 flight: the SRV mesh's check disagrees: it is marked, the main mesh is not, and the eye keeps the terrain's rows (nothing is latched off)");
        // the following frames: the terrain still gives rows where no mesh draws; the SRV mesh is refused; the main mesh still agrees
        eye.newFrame();
        eye.sourceDraw(terrain, false);
        check(eye.have && !eye.fromMeshes, "F130 flight: a frame with only the terrain still has rows (the pin's refusals 'no scene rows for the eye' do not start)");
        eye.newFrame();
        eye.meshDraw(srvMesh, srv);
        check(!eye.have && eye.refused == 1, "F130 flight: the marked SRV mesh gives no rows again");
        eye.sourceDraw(terrain, false);
        eye.meshDraw(mainMesh, main);
        check(eye.have && eye.rows[4] == terrain[4] && eye.refused == 1, "F130 flight: the source then gives the frame's rows, and the main mesh draws on");
    }
    {   // a mesh first: the source that draws after it compares (once the check is due) and replaces the mesh's rows; the mesh is marked
        Eye eye;
        float terrain[20] = {}, srv[20] = {};
        for (int i = 0; i < 20; ++i) srv[i] = terrain[i] = 0.02f * static_cast<float>(i);
        srv[0] += 0.3f;
        const f130::MeshId srvMesh{0xA1, 0xB1, 0xC0DE, 96};
        eye.newFrame();
        eye.meshDraw(srvMesh, srv);
        check(eye.have && eye.fromMeshes && eye.pending && eye.rows[0] == srv[0], "F130 mesh first: the scene mesh gives the rows and the once-a-second check of the frame is open");
        eye.sourceDraw(terrain, false);
        check(eye.replaced == 1 && eye.marks.isMarked(srvMesh) && !eye.fromMeshes && eye.rows[0] == terrain[0] && !eye.pending,
              "F130 mesh first: the source's rows replace the mesh's for the eye and the frame, and the mesh is marked");
        eye.newFrame();
        eye.meshDraw(srvMesh, srv);
        eye.sourceDraw(terrain, false);
        check(eye.refused == 1 && eye.rows[0] == terrain[0] && eye.replaced == 1, "F130 mesh first: next frame the marked mesh is refused and the source gives the rows");
        Eye agree;
        float r[20] = {};
        for (int i = 0; i < 20; ++i) r[i] = 0.1f * static_cast<float>(i);
        agree.newFrame();
        agree.meshDraw(srvMesh, r);
        agree.sourceDraw(r, false);
        check(agree.replaced == 0 && agree.marks.n == 0 && agree.fromMeshes && agree.checked == 1, "F130 mesh first: rows that agree are compared and nothing changes");
        Eye undue;
        undue.newFrame();
        undue.meshDraw(srvMesh, srv);
        undue.pending = false;   // the clock said the check is not due this second
        undue.sourceDraw(terrain, false);
        check(undue.checked == 0 && undue.fromMeshes && undue.rows[0] == srv[0], "F130 mesh first: with the check not due the source leaves the mesh's rows (the tee's fast path stays shut)");
    }
}

}  // namespace f130test
