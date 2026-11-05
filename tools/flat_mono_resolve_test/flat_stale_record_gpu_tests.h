// The stale slot's own record, on WARP (FlatMonoResolveFrame::staleRecordTerm, debug.w bit 4; flat profile only).
//
// A pixel whose engine slot a later draw overdrew is stale. With the steady-detail rule it takes the camera term where last frame's depth confirms
// it. The commander's body and the flight controls are such pixels over the keyed cockpit shell, and they ride the camera: with the ship moving
// metres a frame the camera term holds them still in the world, last frame's depth is never there, and they were refused (stale=11.66%, stale-kept
// 0 in a flight). With staleRecordTerm the record under the pixel is asked first (a joined rig record with this frame's marker) and last frame's
// depth validates ITS motion; anything else leaves the camera term as it was.
//
// The fixture is steadygpu::Rig (flat_steady_depth_gpu_tests.h): 16 x 16, a stale block at texels (4..7, 4..7) over a slot whose record is a rig
// record that did not move in the engine's rows (it rides the camera), the f.camera rows translated 5 texels between the frames (so the camera term
// looks 5 texels away) and last frame's depth set per case. Cases:
//   1  the camera moved, last frame's depth at the block equals its current depth: the bit on keeps the block (no motion, class stale, counted
//      stale-kept); the bit off refuses it as before; the rest of the image is the same either way
//   2  a record that is masked, no rig record, or carries an older marker: the record term is not taken; the output equals the bit off, bit for bit
//   3  the bit on, but last frame's depth differs (the surface moved): refused
//   4  a still camera: kept with the bit on or off, with the same result
//   5  staleRecordTerm false is the old path (refused), and without the steady-detail check the field has no effect
// And, as mutation checks on the shader source (the mutated prep is compiled here and the scenario must FAIL): the helper never takes the record; the
// bit is ignored; the record is accepted without its certification; last frame's depth does not validate it.
#pragma once
#include <string>
#include <vector>
#include "flat_steady_depth_gpu_tests.h"

namespace stalerecordgpu {
using steadygpu::Rig;
using steadygpu::Seen;
constexpr UINT W = steadygpu::W, H = steadygpu::H;
constexpr UINT b0 = 4, bw = 4;
constexpr float kCameraShift = 1.5625f;   // five texels at the fixture's depth (.9375 is three)

// The slot's record: a rig record at the fixture's depth whose previous pose is its current one (it did not move relative to the engine rows).
// markerKind: 0 none (no rig record), 1 fresh joined, 2 fresh masked, 3 a joined marker stamped a frame ago.
inline void ridingRecord(Rig& r, uint32_t markerKind) {
    std::memset(r.record, 0, sizeof(r.record));
    r.record[1] = r.record[77] = bits(1); r.record[2] = r.record[78] = 0x7fff7fff; r.record[3] = r.record[79] = 0xfffe7fff;
    r.record[4] = r.record[73] = bits(0); r.record[5] = r.record[74] = bits(0); r.record[6] = r.record[75] = bits(2.5f);
    edvr::engine_velocity_emit::Pose np{{r.record[4], r.record[5], r.record[6], r.record[2], r.record[3]}};
    edvr::engine_velocity_emit::Pose pp{{r.record[73], r.record[74], r.record[75], r.record[78], r.record[79]}};
    const uint32_t stamp = markerKind == 3 ? 76u : 77u;   // the frame token is 77
    r.record[72] = markerKind == 0 ? 0u : (markerKind == 2 ? 0x7FC0ED02u : 0x7FC0ED01u) ^ edvr::engine_velocity_emit::markerHash(np, pp, stamp);
}

// Two frames: last frame (its depth: `blockThen` in the block, `otherThen` elsewhere; the camera still), then this one (depth .01 everywhere, the camera
// moved `shift`, the treatment `bit`). Returns this frame's output.
inline Seen frames(Rig& r, float shift, float blockThen, float otherThen, bool bit, bool census = false) {
    r.f.staleRecordTerm = false; r.f.refusalCensus = false;
    r.f.camera[5][0] = 0;
    r.fillZ(otherThen); r.setZ(b0, b0, bw, bw, blockThen);
    r.run("stale record: last frame");
    r.fillZ(.01f);
    r.f.camera[5][0] = shift;
    r.f.staleRecordTerm = bit; r.f.refusalCensus = census;
    const Seen s = r.run("stale record: this frame");
    r.f.camera[5][0] = 0; r.f.staleRecordTerm = false; r.f.refusalCensus = false;
    return s;
}
inline unsigned inBlock(const Seen& s) { return s.refused(b0, b0, bw, bw); }
inline bool sameOutput(const Seen& a, const Seen& b) { return a.ok && b.ok && a.mask == b.mask && a.motion == b.motion; }
inline float blockMotion(const Seen& s) {
    float m = 0;
    for (UINT y = b0; y < b0 + bw; ++y)
        for (UINT x = b0; x < b0 + bw; ++x) {
            const size_t t = size_t(y) * W + x;
            if (2 * t + 1 >= s.motion.size()) return 1e9f;
            m = (std::max)(m, (std::max)(std::fabs(s.motion[2 * t]), std::fabs(s.motion[2 * t + 1])));
        }
    return m;
}

inline void staleScenario(Rig& r, edvr::FlatMonoResolveMode mode) {
    using namespace edvr;
    flatMonoResolveReset();
    r.start(mode);
    r.staleBlock(b0, b0, bw, bw);
    ridingRecord(r, 1);
    r.run("stale record: the reset frame that starts the run");
    r.f.reset = false;
    r.f.steadyDetail = true;
    flatMonoResolveTakeRefusalCensus();

    // 1. The camera moved; the block's depth last frame is its depth now, elsewhere it was farther.
    const Seen off = frames(r, kCameraShift, .01f, .02f, false);
    const Seen on = frames(r, kCameraShift, .01f, .02f, true);
    check(inBlock(off) == 16, "stale record: with the bit off, a stale pixel over a record that rides the camera is refused (the camera term looks five texels away)");
    check(inBlock(on) == 0 && blockMotion(on) < .01f,
          "stale record: with the bit on, the record's own motion (none: it rides the camera) is confirmed by last frame's depth: the stale block is kept, with no motion");
    {
        bool restSame = on.ok && off.ok && on.mask.size() == off.mask.size();
        for (UINT y = 0; restSame && y < H; ++y)
            for (UINT x = 0; x < W; ++x)
                if ((x < b0 || x >= b0 + bw || y < b0 || y >= b0 + bw) && on.mask[size_t(y) * W + x] != off.mask[size_t(y) * W + x]) restSame = false;
        check(restSame, "stale record: the bit changes the stale block and nothing else");
    }
    {   // The census: the class stays stale, and the kept pixels are stale-kept.
        flatMonoResolveTakeRefusalCensus();
        for (unsigned i = 0; i < 4; ++i) frames(r, kCameraShift, .01f, .02f, true, true);
        const auto c = steadygpu::takeAll(r.context, 1);
        check(c.counts[kFlatMonoRefusalStaleKept] == 16 && c.counts[kFlatMonoClassStale] == 0,
              "stale record: the census counts the 16 kept pixels as stale-kept (their class is stale), none as stale-refused");
    }

    // 2. A record that is not certified: the record term is not taken, the output is the bit off's.
    {
        const char* const names[] = {"no rig record", "a masked record", "an older marker"};
        const uint32_t kinds[] = {0, 2, 3};
        for (int i = 0; i < 3; ++i) {
            ridingRecord(r, kinds[i]);
            const Seen without = frames(r, kCameraShift, .01f, .02f, false);
            const Seen with = frames(r, kCameraShift, .01f, .02f, true);
            const std::string what = std::string("stale record: ") + names[i] + " under a stale pixel: the record term is not taken (refused, equal to the bit off bit for bit)";
            check(inBlock(with) == 16 && sameOutput(with, without), what.c_str());
        }
        ridingRecord(r, 1);
    }

    // 3. Last frame's depth differs: the surface is not where the record says it was.
    {
        const Seen moved = frames(r, kCameraShift, .02f, .02f, true);
        check(inBlock(moved) == 16, "stale record: with the bit on, a block whose depth last frame was another surface's is refused (more than the 1% tolerance)");
    }

    // 4. A still camera: kept either way, with the same result.
    {
        const Seen stillOff = frames(r, 0, .01f, .01f, false);
        const Seen stillOn = frames(r, 0, .01f, .01f, true);
        bool closeMotion = stillOn.motion.size() == stillOff.motion.size();
        for (size_t i = 0; closeMotion && i < stillOn.motion.size(); ++i) closeMotion = std::fabs(stillOn.motion[i] - stillOff.motion[i]) < .001f;
        check(inBlock(stillOff) == 0 && inBlock(stillOn) == 0 && stillOn.mask == stillOff.mask && closeMotion,
              "stale record: with a still camera the block is kept with the bit on and off, and the two outputs agree");
    }

    // 5. The field false is the old path; the field without the steady-detail check does nothing.
    {
        const Seen field = frames(r, kCameraShift, .01f, .02f, false);
        check(inBlock(field) == 16 && field.noMotion(b0, b0, bw, bw), "stale record: staleRecordTerm false leaves the old path: refused, no motion");
        r.f.steadyDetail = false;
        const Seen noCheck = frames(r, kCameraShift, .01f, .02f, true);
        r.f.steadyDetail = true;
        check(inBlock(noCheck) == 16 && noCheck.noMotion(b0, b0, bw, bw),
              "stale record: with the steady-detail check off the field has no effect: a stale pixel is refused as by default");
    }
    r.f.steadyDetail = false;
    r.f.staleRecordTerm = false;
}

inline void mutationChecks(Rig& r) {
    using fpgpu::Mutant;
    static const Mutant mutants[] = {
        {"the helper never takes the record", "if((debug.w&16)==0)return false;", "return false;", nullptr},
        {"the helper ignores the bit", "if((debug.w&16)==0)return false;", "", nullptr},
        {"the record is accepted without its certification", "if(kind!=1)return false;\n    if(r.data[18].x!=(0x7FC0ED01u^engineMarkerHash(r,asuint(EN[276].x))))return false;\n", "", nullptr},
        {"last frame's depth does not validate the record's motion", "if(inRange)recordKept=historyDepthMatches(recordPrev,recordExpected);", "if(inRange)recordKept=true;", nullptr},
    };
    const std::string shipped = edvr::kFlatMonoShaderSource;
    int caught = 0;
    auto runMutated = [&](const std::string& hlsl, int* failed, std::string* first) {
        std::vector<unsigned char> bytes;
        if (!fpgpu::compilePrep(hlsl, bytes)) return false;
        ComPtr<ID3D11ComputeShader> probe;
        if (FAILED(r.device->CreateComputeShader(bytes.data(), bytes.size(), nullptr, probe.GetAddressOf()))) return false;
        edvr::flatMonoResolveTestPrepBytecode(bytes.data(), bytes.size());
        edvr::flatMonoResolveReset();
        *failed = 0; mutationFailures = failed; mutationFirst.clear();
        staleScenario(r, edvr::FlatMonoResolveMode::Dlss);
        mutationFailures = nullptr; if (first) *first = mutationFirst;
        return true;
    };
    {   // The control: the shipped source, compiled here, passes.
        int failed = 0; std::string first;
        const bool ran = runMutated(shipped, &failed, &first);
        check(ran && failed == 0, "stale record mutations: control: the unmutated prep, compiled here and run through the test seam, passes the whole scenario");
    }
    for (const Mutant& m : mutants) {
        bool once = false;
        const std::string hlsl = fpgpu::replaceOnce(shipped, m.from, m.to, &once);
        char what[256];
        std::snprintf(what, sizeof(what), "stale record mutations: \"%s\": its anchor is in the shader source exactly once (a moved anchor tests nothing)", m.name);
        check(once, what);
        int failed = 0; std::string first;
        const bool ran = once && runMutated(hlsl, &failed, &first);
        std::snprintf(what, sizeof(what), "stale record mutations: \"%s\" compiles and makes a compute shader", m.name);
        check(ran, what);
        if (!ran) continue;
        std::snprintf(what, sizeof(what), "stale record mutations: \"%s\" is caught by the scenario", m.name);
        check(failed > 0, what);
        if (failed > 0) ++caught;
        std::printf("flat mono resolve: stale-record mutation \"%s\": %d checks fail; first: %s\n", m.name, failed, first.c_str());
    }
    edvr::flatMonoResolveTestPrepBytecode(nullptr, 0);
    edvr::flatMonoResolveReset();
    std::printf("flat mono resolve: stale-record mutations: %d of %zu caught by the scenario\n", caught, sizeof(mutants) / sizeof(mutants[0]));
    check(size_t(caught) == sizeof(mutants) / sizeof(mutants[0]), "stale record mutations: every mutation was caught");
}
}  // namespace stalerecordgpu

inline void staleRecordGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    const std::string source = edvr::kFlatMonoShaderSource;
    check(source.find("staleRecordBefore") != std::string::npos && source.find("(debug.w&16)==0") != std::string::npos,
          "stale record: the shader carries the helper and its bit (debug.w value 16)");
    steadygpu::Rig rig(device, context);
    stalerecordgpu::staleScenario(rig, edvr::FlatMonoResolveMode::Dlss);
    stalerecordgpu::staleScenario(rig, edvr::FlatMonoResolveMode::Fsr);
    edvr::flatMonoResolveReset();
    stalerecordgpu::mutationChecks(rig);
    edvr::flatMonoResolveReset();
}
