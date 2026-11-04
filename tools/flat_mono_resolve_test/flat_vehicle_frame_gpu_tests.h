// The vehicle frame on WARP (FlatMonoResolveFrame::vehicleFrame, debug.w value 32; the flat profile in a vehicle, never on foot), and its pure half (vehicle_frame.h).
//
// The cockpit's body, seat and controls are near surfaces that ride the vehicle. Some carry no certified record of their own (no slot, an unmarked record, a stale slot
// over one), so they took the camera term, which holds them still in the world: at metres a frame a vector of hundreds of pixels that reached the backend unchecked.
// The game's own rig names what they ride: the root record of the player vehicle's cockpit rig. With the frame (its pose this frame and last, from the emit, and its
// sphere) a near pixel (within 10 m) inside the sphere rides that root, last frame's depth validates it, and a kind-0 pixel inside the volume left on the camera term is
// checked against last frame's depth as well.
//
// The fixture is a 96 x 96 render, a root at (0,0,2.5) with a sphere of radius 2.0 in its own frame, and five 4 x 4 blocks along the middle row: near with no slot, near over
// an unmarked slot, a stale slot over an unmarked record, far (depth .001) with no slot, and near with no slot but outside the sphere (at the left edge: the camera term looks to the right). The camera translates about ten
// texels between the frames (f.camera; the engine rows are still), so the camera term looks ten texels away. Cases:
//   1  last frame's depth at the blocks is their depth now: the three near blocks inside the sphere are kept with no motion, class skinned (the census's accepted counter);
//      the block outside the sphere and the far block are exactly what they are with the frame absent
//   2  last frame's depth changed: the three are refused with no motion (class camera for the two kind-0 blocks, stale for the stale one); the outside block is not
//   3  a frame that moved (the root's pose last frame differs): the blocks take the root's own motion, not zero
//   4  a sphere so large that the far block is inside it: still not taken, the far block is 25 m away
//   5  no frame this resolve (the field false, with the pose still in the frame struct): the old path exactly
// Beside them: the pose ring (adjacent stamps, a missing stamp, a stamp that is not adjacent, a torn read, a vehicle change) and the gate (unknown, on foot, in a vehicle).
// And, as mutation checks on the shader source (the mutated prep is compiled here and the scenario must FAIL): the prep never takes the frame; the near camera check is gone,
// or applies without the frame; the frame's motion is not validated; far pixels ride it; the sphere is ignored; last frame's pose is the current one.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>
#include "flat_stale_record_gpu_tests.h"
#include "../../src/d3d11/vehicle_frame.h"

namespace vehiclegpu {
constexpr UINT N = 96, kBlock = 4, kRow = 46;
struct Spot { UINT x, y; };
constexpr Spot kNone{10, kRow}, kUnmarked{26, kRow}, kStale{42, kRow}, kFar{58, kRow}, kOutside{2, kRow};   // each 4 x 4; the sphere (radius 2.0) holds the first four

struct Out {
    bool ok = false;
    std::vector<unsigned char> mask;
    std::vector<float> motion;
    unsigned refused(Spot s) const {
        unsigned n = 0;
        for (UINT y = s.y; y < s.y + kBlock; ++y)
            for (UINT x = s.x; x < s.x + kBlock; ++x)
                if (size_t(y) * N + x < mask.size() && mask[size_t(y) * N + x]) ++n;
        return n;
    }
    float maxMotion(Spot s) const {
        float m = 0;
        for (UINT y = s.y; y < s.y + kBlock; ++y)
            for (UINT x = s.x; x < s.x + kBlock; ++x) {
                const size_t t = size_t(y) * N + x;
                if (2 * t + 1 >= motion.size()) return 1e9f;
                m = (std::max)(m, (std::max)(std::fabs(motion[2 * t]), std::fabs(motion[2 * t + 1])));
            }
        return m;
    }
    float meanX(Spot s) const {
        float sum = 0;
        for (UINT y = s.y; y < s.y + kBlock; ++y)
            for (UINT x = s.x; x < s.x + kBlock; ++x) sum += motion[2 * (size_t(y) * N + x)];
        return sum / float(kBlock * kBlock);
    }
};
// The block's mask and motion, bit for bit, in two outputs.
inline bool sameIn(const Out& a, const Out& b, Spot s) {
    if (!a.ok || !b.ok) return false;
    for (UINT y = s.y; y < s.y + kBlock; ++y)
        for (UINT x = s.x; x < s.x + kBlock; ++x) {
            const size_t t = size_t(y) * N + x;
            if (2 * t + 1 >= a.motion.size() || 2 * t + 1 >= b.motion.size() || t >= a.mask.size() || t >= b.mask.size()) return false;
            if (a.mask[t] != b.mask[t] || a.motion[2 * t] != b.motion[2 * t] || a.motion[2 * t + 1] != b.motion[2 * t + 1]) return false;
        }
    return true;
}
inline bool same(const Out& a, const Out& b) { return a.ok && b.ok && a.mask == b.mask && a.motion == b.motion; }

struct Rig {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ResolveFixture fx;
    ComPtr<ID3D11Texture2D> depth, color;
    ComPtr<ID3D11ShaderResourceView> depthView, colorView;
    std::vector<float> z, slots;
    std::vector<uint32_t> rgba;
    uint32_t records[4][84]{};
    edvr::FlatMonoResolveFrame f{};

    Rig(ID3D11Device* d, ID3D11DeviceContext* c) : device(d), context(c), fx(d, c), z(N * N, .01f), slots(N * N * 2), rgba(N * N, 0xff808080u) {
        depth = texture(d, N, N, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, z.data(), N * 4);
        depthView = view(d, depth.Get());
        color = texture(d, N, N, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, rgba.data(), N * 4);
        colorView = view(d, color.Get());
        // The fixture's slot texture is 16 x 16 and its pool holds one record: this rig's are the render's size and four records (all unmarked).
        fx.slotTexture = texture(d, N, N, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE, slots.data(), N * 8);
        fx.slotView = view(d, fx.slotTexture.Get());
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(records); bd.StructureByteStride = 336;
        bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = records;
        fx.pool.Reset(); fx.poolView.Reset();
        check(SUCCEEDED(d->CreateBuffer(&bd, &initial, fx.pool.GetAddressOf())), "vehicle frame: the four-record pool");
        fx.poolView = view(d, fx.pool.Get());
        start();
    }
    void start(edvr::FlatMonoResolveMode mode = edvr::FlatMonoResolveMode::Dlss) {
        f = edvr::FlatMonoResolveFrame{};
        f.color = colorView.Get(); f.depth = depthView.Get();
        f.renderWidth = f.renderHeight = f.outputWidth = f.outputHeight = N;
        f.deltaMs = 16; camera(f.camera); camera(f.previousCamera); fx.engine(f);
        f.mode = mode; f.frame = 60000; f.reset = true;
        std::fill(rgba.begin(), rgba.end(), 0xff808080u);
        setFrame(0, 2.0f);
    }
    // The frame's pose: the root at (0,0,2.5) with the identity rotation this frame, last frame at x = -prevX; the sphere's radius in the root's own frame.
    void setFrame(float prevX, float radius) {
        const float now[3] = {0, 0, 2.5f}, prev[3] = {-prevX, 0, 2.5f};
        for (int i = 0; i < 3; ++i) { f.vehiclePosition[0][i] = now[i]; f.vehiclePosition[1][i] = prev[i]; }
        f.vehicleQuat[0][0] = f.vehicleQuat[1][0] = 32767u | (32767u << 16);
        f.vehicleQuat[0][1] = f.vehicleQuat[1][1] = (65534u << 16) | 32767u;
        f.vehicleSphere[0] = f.vehicleSphere[1] = f.vehicleSphere[2] = 0; f.vehicleSphere[3] = radius;
    }
    void put(UINT x, UINT y, float code, float slotDepth) { slots[(size_t(y) * N + x) * 2] = code; slots[(size_t(y) * N + x) * 2 + 1] = slotDepth; }
    void block(Spot s, float code, float slotDepth) { for (UINT y = s.y; y < s.y + kBlock; ++y) for (UINT x = s.x; x < s.x + kBlock; ++x) put(x, y, code, slotDepth); }
    void fillBlock(Spot s, float v, UINT widen = 0) {
        for (UINT y = s.y; y < s.y + kBlock; ++y) for (UINT x = s.x > widen ? s.x - widen : 0; x < (std::min)(N, s.x + kBlock + widen); ++x) z[size_t(y) * N + x] = v;
    }
    // Every pixel has no slot, but the blocks: an unmarked record (slot 1, all records are unmarked), and a stale slot over it.
    void layout() {
        for (UINT y = 0; y < N; ++y) for (UINT x = 0; x < N; ++x) put(x, y, -1.0f, .01f);
        block(kUnmarked, 3.0f, .01f); block(kStale, 3.0f, .5f);
    }
    Out run(const char* what) {
        context->UpdateSubresource(depth.Get(), 0, nullptr, z.data(), N * 4, 0);
        context->UpdateSubresource(color.Get(), 0, nullptr, rgba.data(), N * 4, 0);
        context->UpdateSubresource(fx.slotTexture.Get(), 0, nullptr, slots.data(), N * 8, 0);
        context->UpdateSubresource(fx.pool.Get(), 0, nullptr, records, 0, 0);
        expectedJx = f.jitterX; expectedJy = f.jitterY;
        fx.bindOriginal();
        ComPtr<ID3D11ShaderResourceView> resolved; const char* why = nullptr;
        const int before = backendCalls;
        const bool ok = edvr::flatMonoResolve(device, context, f, resolved.GetAddressOf(), &why);
        if (!ok) std::printf("info: vehicle frame scenario \"%s\": resolver reason %s\n", what, why ? why : "none");
        check(ok, what);
        check(fx.restored(), "vehicle frame: the game's whole pipeline comes back untouched");
        Out o; o.ok = ok;
        if (backendCalls > before) { o.mask = observedMaskAll; o.motion = observedMotionAll; }
        ++f.frame;
        return o;
    }
    // Two frames: last frame (`lastNear` in the near blocks and the outside block, widened by `widen` texels each side, .001 elsewhere, the camera still), then this one
    // (.01 in the near blocks, .001 elsewhere, the camera ten texels along). `frame` is the field; `census` samples this frame.
    Out frames(bool frame, float lastNear, bool census = false, UINT widen = 0) {
        const bool wasFrame = f.vehicleFrame;
        f.vehicleFrame = false; f.staleRecordTerm = false; f.refusalCensus = false;
        f.camera[5][0] = 0;
        // Everything but the four near blocks is far (.001, past 10 m): no pixel of it is taken or checked, so a census over the whole image counts the blocks alone.
        std::fill(z.begin(), z.end(), .001f);
        for (Spot s : {kNone, kUnmarked, kStale, kOutside}) fillBlock(s, lastNear, widen);
        run("vehicle frame: last frame");
        std::fill(z.begin(), z.end(), .001f);
        for (Spot s : {kNone, kUnmarked, kStale, kOutside}) fillBlock(s, .01f);
        f.camera[5][0] = widen ? 0.0f : 0.5f;
        f.vehicleFrame = frame; f.staleRecordTerm = true; f.refusalCensus = census;
        const Out o = run("vehicle frame: this frame");
        f.camera[5][0] = 0; f.vehicleFrame = wasFrame; f.staleRecordTerm = false; f.refusalCensus = false;
        return o;
    }
};

inline void ringAndGateTests() {
    using namespace edvr::vframe;
    auto pose = [](uint32_t v) { Pose p; for (int i = 0; i < 3; ++i) p.pos[i] = v + i; p.quat[0] = v + 10; p.quat[1] = v + 11; return p; };
    Frame frame;
    {
        Ring ring;
        ring.record(100, 7, pose(100)); ring.record(101, 7, pose(101));
        check(select(ring, 101, 7, &frame) == Why::kOk && frame.now.pos[0] == 101 && frame.prev.pos[0] == 100 && frame.now.quat[1] == 112 && frame.prev.quat[0] == 110,
              "vehicle ring: adjacent stamps give this frame's pose and last frame's");
        check(select(ring, 102, 7, &frame) == Why::kMissing, "vehicle ring: no entry for this frame's stamp is missing");
        Ring lone;
        lone.record(100, 7, pose(100));
        check(select(lone, 100, 7, &frame) == Why::kMissing, "vehicle ring: this frame's entry alone, nothing older, is missing");
        Ring gap;
        gap.record(98, 7, pose(98)); gap.record(100, 7, pose(100));
        check(select(gap, 100, 7, &frame) == Why::kNotAdjacent, "vehicle ring: an older entry that is not the previous stamp is not adjacent");
        Ring changed;
        changed.record(100, 7, pose(100)); changed.record(101, 8, pose(101));
        check(select(changed, 101, 8, &frame) == Why::kChanged, "vehicle ring: the two frames from different rigs are a vehicle change");
        check(select(ring, 101, 9, &frame) == Why::kChanged, "vehicle ring: entries of a rig that is no longer the tracked one are a vehicle change");
        Ring old;
        old.record(93, 7, pose(93)); old.record(101, 7, pose(101));
        Entry gone;
        check(!old.read(93, &gone) && select(old, 101, 7, &frame) == Why::kMissing, "vehicle ring: a slot reused by a stamp eight frames on does not answer for the old one");
    }
    {
        // The recent rigs: the target names a rig ahead of it being drawn, so the newest with both stamps answers.
        Ring named, drawn;
        const Ring* rings[2] = {&named, &drawn};
        const uint32_t gens[2] = {8, 7};
        uint32_t chosen = 99;
        Frame recent;
        check(selectRecent(rings, gens, 0, 101, &recent, &chosen) == Why::kMissing && selectRecent(rings, gens, 2, 101, &recent, &chosen) == Why::kMissing, "recent rigs: no rig, or none that emitted, is missing");
        drawn.record(100, 7, pose(100)); drawn.record(101, 7, pose(101));
        check(selectRecent(rings, gens, 2, 101, &recent, &chosen) == Why::kOk && chosen == 1 && recent.now.pos[0] == 101 && recent.prev.pos[0] == 100,
              "recent rigs: the named rig emits nothing and the earlier one is drawn: the earlier one answers");
        named.record(101, 8, pose(201));
        check(selectRecent(rings, gens, 2, 101, &recent, &chosen) == Why::kOk && chosen == 1 && recent.now.pos[0] == 101, "recent rigs: the named rig's first frame has no last frame, so the earlier one still answers");
        named.record(102, 8, pose(202));
        check(selectRecent(rings, gens, 2, 102, &recent, &chosen) == Why::kOk && chosen == 0 && recent.now.pos[0] == 202 && recent.prev.pos[0] == 201, "recent rigs: from its second frame the named rig answers");
        drawn.record(103, 7, pose(103)); drawn.record(102, 7, pose(102)); named.record(103, 8, pose(203));
        check(selectRecent(rings, gens, 2, 103, &recent, &chosen) == Why::kOk && chosen == 0 && recent.now.pos[0] == 203, "recent rigs: both emitting, a parked ship on screen, the newest answers");
        const uint32_t retired[2] = {9, 7};
        check(selectRecent(rings, retired, 2, 103, &recent, &chosen) == Why::kOk && chosen == 1 && recent.now.pos[0] == 103, "recent rigs: entries from before a slot's rig changed do not answer, the next rig does");
        check(selectRecent(rings, retired, 1, 103, &recent, &chosen) == Why::kChanged, "recent rigs: with nothing else to answer the reason survives");
    }
    {
        // A torn read never returns mixed words: a writer rewrites one slot with all five words equal while a reader asks for it.
        Ring ring;
        std::atomic<bool> stop{false};
        std::thread writer([&] { for (uint32_t i = 1; !stop.load(); ++i) { Pose p; for (int k = 0; k < 3; ++k) p.pos[k] = i; p.quat[0] = p.quat[1] = i; ring.record(100, 1, p); } });
        // The count starts at the writer's first entry and the loop is bounded by time, not reads: on a loaded build machine the writer can start after a fixed number of reads has gone by.
        bool mixed = false; uint64_t got = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (got < 20000 && std::chrono::steady_clock::now() < deadline) {
            for (int i = 0; i < 1024; ++i) {
                Entry e;
                if (ring.read(100, &e)) { ++got; mixed = mixed || e.pose.pos[0] != e.pose.pos[1] || e.pose.pos[1] != e.pose.pos[2] || e.pose.pos[2] != e.pose.quat[0] || e.pose.quat[0] != e.pose.quat[1]; }
            }
        }
        stop = true; writer.join();
        check(!mixed, "vehicle ring: a read that overlaps a write is refused, never mixed");
        check(got > 0, "vehicle ring: the stress read at least one whole entry, so it tested something");
    }
    check(gate(false, false) == Gate::kNoStatus && gate(false, true) == Gate::kNoStatus, "vehicle gate: an unknown status is off(no-status), whatever the flag says");
    check(gate(true, true) == Gate::kOnFoot && gate(true, false) == Gate::kOn, "vehicle gate: on foot is off(on-foot), in a vehicle on");
    check(!useFrame(false, false, true) && !useFrame(true, true, true) && !useFrame(true, false, false) && useFrame(true, false, true),
          "vehicle gate: the frame is used only in a vehicle the journal says so of, with a frame in hand");
}

inline void frameScenario(Rig& r, edvr::FlatMonoResolveMode mode) {
    using namespace edvr;
    flatMonoResolveReset();
    r.start(mode);
    r.layout();
    r.run("vehicle frame: the reset frame that starts the run");
    r.f.reset = false;
    r.f.steadyDetail = true;
    flatMonoResolveTakeRefusalCensus();

    // 1. Last frame's depth at the blocks is their depth now.
    r.f.vehicleFrame = false;
    const Out off = r.frames(false, .01f);
    const Out on = r.frames(true, .01f);
    check(off.refused(kNone) == 0 && off.refused(kUnmarked) == 0 && off.refused(kStale) == 16 && off.maxMotion(kNone) > 3 && off.maxMotion(kUnmarked) > 3,
          "vehicle frame: with the frame absent the kind-0 near blocks keep their unchecked camera vector (several texels long) and the stale block is refused");
    check(on.refused(kNone) == 0 && on.refused(kUnmarked) == 0 && on.refused(kStale) == 0 &&
              on.maxMotion(kNone) < .05f && on.maxMotion(kUnmarked) < .05f && on.maxMotion(kStale) < .05f,
          "vehicle frame: with the frame the three near blocks inside the sphere ride the root (no motion) and are kept, the stale one included");
    check(sameIn(on, off, kOutside) && on.refused(kOutside) == 0 && off.maxMotion(kOutside) > 3,
          "vehicle frame: a near block outside the sphere is exactly the camera term it was, unchecked");
    check(sameIn(on, off, kFar) && on.refused(kFar) == 0 && off.maxMotion(kFar) > 0.1f, "vehicle frame: a far block is exactly the camera term it was, unchecked and not refused");
    {
        flatMonoResolveTakeRefusalCensus();
        for (unsigned i = 0; i < 4; ++i) r.frames(true, .01f, true);
        const auto c = steadygpu::takeAll(r.context, 1);
        check(c.skinned == 48 && c.counts[kFlatMonoClassCamera] == 0 && c.counts[kFlatMonoClassStale] == 0,
              "vehicle frame: the census counts the 48 pixels that rode the vehicle (class skinned, the accepted counter) and refuses none of the blocks");
    }

    // 2. Last frame's depth changed: the surface is not where the root says.
    {
        const Out moved = r.frames(true, .02f);
        check(moved.refused(kNone) == 16 && moved.refused(kUnmarked) == 16 && moved.refused(kStale) == 16 &&
                  moved.maxMotion(kNone) == 0 && moved.maxMotion(kUnmarked) == 0 && moved.maxMotion(kStale) == 0,
              "vehicle frame: with last frame's depth changed the three blocks are refused with no motion (the root's and the camera term's depth checks both fail)");
        check(moved.refused(kOutside) == 0 && moved.maxMotion(kOutside) > 3, "vehicle frame: ... and the block outside the sphere is not checked at all");
        flatMonoResolveTakeRefusalCensus();
        for (unsigned i = 0; i < 4; ++i) r.frames(true, .02f, true);
        const auto c = steadygpu::takeAll(r.context, 1);
        check(c.counts[kFlatMonoClassCamera] == 32 && c.counts[kFlatMonoClassStale] == 16 && c.skinned == 0,
              "vehicle frame: the two kind-0 blocks are refused as class camera, the stale block keeps its stale class, and none counts as vehicle-frame");
    }

    // 3. The root moved: the blocks take its own motion. It was 0.3 to the left last frame, so a pixel at the root's depth moved -0.3/2.5 of half the width: about -5.8 texels.
    {
        r.setFrame(0.3f, 2.0f);
        const Out walking = r.frames(true, .01f, false, 8);
        check(walking.refused(kNone) == 0 && walking.refused(kUnmarked) == 0 && walking.refused(kStale) == 0 &&
                  std::fabs(walking.meanX(kNone) + 5.76f) < .3f && std::fabs(walking.meanX(kUnmarked) + 5.76f) < .3f && std::fabs(walking.meanX(kStale) + 5.76f) < .3f,
              "vehicle frame: a root that moved between the frames moves the blocks by its own motion (about -5.8 texels), not by zero");
        r.setFrame(0, 2.0f);
    }

    // 4. A sphere that holds the far block: it is 25 m away and past the 10 m reach.
    {
        r.setFrame(0, 50.0f);
        const Out wide = r.frames(true, .01f);
        r.f.vehicleFrame = false;
        const Out none = r.frames(false, .01f);
        check(sameIn(wide, none, kFar) && wide.refused(kFar) == 0 && wide.refused(kNone) == 0 && wide.maxMotion(kNone) < .05f,
              "vehicle frame: a pixel inside the sphere but past 10 m is not taken (the far block is the camera term), while the near ones still ride the root");
        r.setFrame(0, 2.0f);
    }

    // 5. No frame this resolve: the field false with the pose still in the struct is the old path exactly.
    {
        r.f.vehicleFrame = false;
        const Out old = r.frames(false, .01f);
        check(same(old, off), "vehicle frame: with no frame this resolve (the pose still in the struct) the output is the old path's, every time");
    }
    r.f.steadyDetail = false;
}

// ---- the shader source, one rule flipped at a time -------------------------------------------------------------------------------------
inline bool compileEntry(const std::string& hlsl, const char* entry, std::vector<unsigned char>& bytes) {
    const std::string full = std::string(edvr::kEngineMotionCoreHlsl) + hlsl;
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(full.c_str(), full.size(), "flat_mono_prep_cs", nullptr, nullptr, entry, "cs_5_0", 0, 0, code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr) || !code) {
        std::printf("info: %s compile failed: %s\n", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        return false;
    }
    const auto* p = static_cast<const unsigned char*>(code->GetBufferPointer());
    bytes.assign(p, p + code->GetBufferSize());
    return true;
}

inline void mutationChecks(Rig& r) {
    struct M { const char* name; const char* from; const char* to; };
    static const M mutants[] = {
        {"the prep never takes the vehicle frame", "if((debug.w&32)==0 || !vehicleNear(depth))return 0;", "return 0;"},
        {"the near camera check is gone", "if(valid && kind==0 && vehicleState!=0) {", "if(false) {"},
        {"the near camera check applies without the frame", "if(valid && kind==0 && vehicleState!=0) {", "if(valid && kind==0 && vehicleNear(depth)) {"},
        {"the frame's motion is not validated by last frame's depth", "if(inRange)kept=historyDepthMatches(p,expected);", "if(inRange)kept=true;"},
        {"a far pixel rides the vehicle frame", "if((debug.w&32)==0 || !vehicleNear(depth))return 0;", "if((debug.w&32)==0)return 0;"},
        {"the sphere is ignored", "if(!all(isfinite(d)) || !(dot(d,d)<=vehicleSphere.w*vehicleSphere.w))return 0;", "if(!all(isfinite(d)))return 0;"},
        {"last frame's pose is the current one", "r.data[18].yzw=asuint(vehiclePosPrev.xyz);", "r.data[18].yzw=asuint(vehiclePosNow.xyz);"},
    };
    const std::string shipped = edvr::kFlatMonoShaderSource;
    int caught = 0;
    auto runMutated = [&](const std::string& hlsl, int* failed, std::string* first) {
        std::vector<unsigned char> bytes;
        if (!compileEntry(hlsl, "prep", bytes)) return false;
        ComPtr<ID3D11ComputeShader> probe;
        if (FAILED(r.device->CreateComputeShader(bytes.data(), bytes.size(), nullptr, probe.GetAddressOf()))) return false;
        edvr::flatMonoResolveTestPrepBytecode(bytes.data(), bytes.size());
        edvr::flatMonoResolveReset();
        *failed = 0; mutationFailures = failed; mutationFirst.clear();
        frameScenario(r, edvr::FlatMonoResolveMode::Dlss);
        mutationFailures = nullptr; if (first) *first = mutationFirst;
        edvr::flatMonoResolveTestPrepBytecode(nullptr, 0);
        return true;
    };
    {
        int failed = 0; std::string first;
        const bool ran = runMutated(shipped, &failed, &first);
        check(ran && failed == 0, "vehicle frame mutations: control: the unmutated prep, compiled here and run through the test seam, passes the whole scenario");
    }
    for (const M& m : mutants) {
        bool once = false;
        const std::string hlsl = fpgpu::replaceOnce(shipped, m.from, m.to, &once);
        char what[256];
        std::snprintf(what, sizeof(what), "vehicle frame mutations: \"%s\": its anchor is in the shader source exactly once (a moved anchor tests nothing)", m.name);
        check(once, what);
        int failed = 0; std::string first;
        const bool ran = once && runMutated(hlsl, &failed, &first);
        std::snprintf(what, sizeof(what), "vehicle frame mutations: \"%s\" compiles and makes a compute shader", m.name);
        check(ran, what);
        if (!ran) continue;
        std::snprintf(what, sizeof(what), "vehicle frame mutations: \"%s\" is caught by the scenario", m.name);
        check(failed > 0, what);
        if (failed > 0) ++caught;
        std::printf("flat mono resolve: vehicle-frame mutation \"%s\": %d checks fail; first: %s\n", m.name, failed, first.c_str());
    }
    edvr::flatMonoResolveReset();
    std::printf("flat mono resolve: vehicle-frame mutations: %d of %zu caught by the scenario\n", caught, sizeof(mutants) / sizeof(mutants[0]));
    check(size_t(caught) == sizeof(mutants) / sizeof(mutants[0]), "vehicle frame mutations: every mutation was caught");
}
}  // namespace vehiclegpu

inline void vehicleFrameGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    const std::string source = edvr::kFlatMonoShaderSource;
    check(source.find("vehicleBefore") != std::string::npos && source.find("(debug.w&32)") != std::string::npos && source.find("shipCount") == std::string::npos &&
              source.find("ShipPick") == std::string::npos,
          "vehicle frame: the shader carries the helper and the bit (debug.w value 32), and no vote kernel");
    vehiclegpu::ringAndGateTests();
    vehiclegpu::Rig rig(device, context);
    vehiclegpu::frameScenario(rig, edvr::FlatMonoResolveMode::Dlss);
    vehiclegpu::frameScenario(rig, edvr::FlatMonoResolveMode::Fsr);
    edvr::flatMonoResolveReset();
    vehiclegpu::mutationChecks(rig);
    edvr::flatMonoResolveReset();
}
