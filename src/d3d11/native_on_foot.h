#pragma once
// Native on-foot VR, flight F0 (docs\design-native-onfoot-vr-2026-10-13.md; the decisions are native_on_foot_core.h).
//
// THE HOOK (VR profile only). A CodeHook on the game's render-mode setter (EliteDangerous64.exe+0x2874920, build 334925), installed
// by the first frame boundary. It is installed whether or not fix.native_on_foot is on, so every natural mode request is logged. An
// an incoming cinema mode (5 or 6) is forwarded as its HMD partner (3 or 4) when the key is on, the selector is the one frozen at the first valid call, and
// the journal says a game session is live. Everything else is forwarded unchanged.
//
// THE KEY. fix.native_on_foot, on/off, default off. TEMPORARY (F0): it goes away when the feature ships as behaviour. The frame
// thread re-reads it when Config::generation() moves; the hook reads the published atomic.
//
// THE LOG. One line per setter call (the rare ones), queued by the game's thread and written at the frame boundary; one line every
// 5 s while the native stereo flag is set, with the gate counters (cumulative since launch).
//
// THE FLAG. nativeOnFootActive() under method swap (F0 behaviour): true from a substitution until the next forwarded natural HMD mode (3 or 4).
// Under method gate (F2): true only while ALL of these hold, computed once a frame at the boundary: the gate byte is EDVR's (written, not
// refused), the most recent setter call forwarded an HMD mode, and the journal says on foot (journalOnFootKnown && journalOnFoot). The setter
// never substitutes under gate, so the flag is not set by it.
//
// THE METHOD (F2, TEMPORARY A/B). fix.native_on_foot_method, swap or gate, default gate, live. swap: the setter substitutes (F0). gate: no
// substitution; while the key is on and a game session is live EDVR holds the byte at EliteDangerous64.exe+0x5E85B8A at 0, so the game's own
// requester asks for HMD. Goes away when the arc picks one.
//
// THE BODY (F2). While the flag is set, nativeOnFootHooksBody(...) runs the avatar hooks' fade and head-part code (native_on_foot_hooks.h) for the local avatar, once a
// frame, under either method. Its head hooks are wanted from launch while fix.native_on_foot is on.
//
// THE GATES. Log-only counters, no behaviour: each engaged gate calls nativeOnFootCountGate() and counts only while the flag is set.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "native_on_foot_core.h"   // nofoot::hudIssuerIndex: the on-foot HUD issuers' vertex shaders
#include "native_on_foot_f100.h"   // F100: the marker tee's prefilter (pure parts)
#include "native_on_foot_f115.h"   // F115: the cockpit's own panels' vertex shaders (the boarding edge)

struct ID3D11DeviceContext;   // the F18 body cull's Begin/End (d3d11.h is not needed by the rigs that include this header)
struct ID3D11Resource;        // F44: the marker probe's Map/Unmap tees

namespace edvr {

// Read-only movement snapshot published from unique, pre-pin third-person AMC root samples.
// There is one writer: the unique-P1 branch of the serialized pfzAfterAnimate path.
// Consumers should validate sampleQpc freshness before using it to trigger a capture.
struct NativeOnFootAirborneSnapshot {
    uint64_t generation = 0, sampleQpc = 0;
    uint32_t sourceFrame = 0;
    double root[3] = {}, verticalVelocityMps = 0.0, rawVerticalVelocityMps = 0.0;
    bool valid = false, airborne = false, takeoffEdge = false;
};
namespace native_on_foot_airborne_detail {
inline std::atomic<uint32_t> seq{0};
inline std::atomic<uint64_t> generation{0}, sampleQpc{0};
inline std::atomic<uint32_t> sourceFrame{0};
inline std::atomic<double> root[3]{};
inline std::atomic<double> verticalVelocityMps{0.0}, rawVerticalVelocityMps{0.0};
inline std::atomic<bool> valid{false}, airborne{false}, takeoffEdge{false};
}
inline void nativeOnFootAirbornePublish(const NativeOnFootAirborneSnapshot& value) noexcept {
    using namespace native_on_foot_airborne_detail;
    const uint32_t version = seq.load(std::memory_order_relaxed);
    seq.store(version + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    generation.store(value.generation, std::memory_order_relaxed);
    sampleQpc.store(value.sampleQpc, std::memory_order_relaxed);
    sourceFrame.store(value.sourceFrame, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) root[i].store(value.root[i], std::memory_order_relaxed);
    verticalVelocityMps.store(value.verticalVelocityMps, std::memory_order_relaxed);
    rawVerticalVelocityMps.store(value.rawVerticalVelocityMps, std::memory_order_relaxed);
    valid.store(value.valid, std::memory_order_relaxed);
    airborne.store(value.airborne, std::memory_order_relaxed);
    takeoffEdge.store(value.takeoffEdge, std::memory_order_relaxed);
    seq.store(version + 2, std::memory_order_release);
}
inline bool nativeOnFootAirborneSnapshot(NativeOnFootAirborneSnapshot* out) noexcept {
    if (!out) return false;
    using namespace native_on_foot_airborne_detail;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t before = seq.load(std::memory_order_acquire);
        if (before & 1u) continue;
        NativeOnFootAirborneSnapshot value;
        value.generation = generation.load(std::memory_order_relaxed);
        value.sampleQpc = sampleQpc.load(std::memory_order_relaxed);
        value.sourceFrame = sourceFrame.load(std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) value.root[i] = root[i].load(std::memory_order_relaxed);
        value.verticalVelocityMps = verticalVelocityMps.load(std::memory_order_relaxed);
        value.rawVerticalVelocityMps = rawVerticalVelocityMps.load(std::memory_order_relaxed);
        value.valid = valid.load(std::memory_order_relaxed);
        value.airborne = airborne.load(std::memory_order_relaxed);
        value.takeoffEdge = takeoffEdge.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) == before) { *out = value; return before != 0; }
    }
    return false;
}

enum class NativeOnFootGate : uint8_t {
    Frames = 0,       // frame boundaries while the flag is set
    WorldRoute,       // the VR world route owned the frame (native_temporal begin) or took an eye's screen draw (layer-only treat)
    UiLayer,          // the UI layer's world-screen gate held (on foot and a world screen seen)
    FootSplit,        // footSplitNow: the on-foot path (no ship split) was chosen
    DeferredPacing,   // the deferred frame pacing (weapon stability) was on
    PanelShape,       // vScreen saw a 16:9 target taken as the on-foot panel
    TransitionFlash,  // the transition flash's camera-mode gate (game mode 2 with a reset mailbox)
    Count
};

// Inline (C++17), not defined in native_on_foot.cpp: the gate sites live in files several rigs compile without that file, and a rig
// must link whether or not it runs the hook.
inline std::atomic<bool> g_nativeOnFootActive{false};
inline std::atomic<uint64_t> g_nativeOnFootGates[size_t(NativeOnFootGate::Count)];

// The native stereo flag: true from a substitution until the next forwarded natural HMD mode (3 or 4).
inline bool nativeOnFootActive() { return g_nativeOnFootActive.load(std::memory_order_relaxed); }

// F93d: the game's own camera on foot. The camera suite's controller (VesselCameraMountControl, an activity that runs every update while the commander
// is on foot) keeps the suite's mode in its byte +0x3E0: 0 = closed, the game's first-person camera; 1, 2 = the suite on a preset ("Commander control",
// "Commander profile"), 3..6 = the free camera and its locks (avatar_hooks_core.h, modeText; analysis\decomp\explorer_cam\decomp_2DF14C0.txt). Opening
// the suite starts the game's VanityCamera activity and removes HumanoidCamera and PlacedCamera; closing it does the reverse (the F93d flight log,
// 10:31:17.355 and 10:31:43.539). The avatar hooks' controller hook (native_on_foot_hooks.cpp, controllerHooked) reads the byte after the game's own update, on
// the game's thread, while native stereo is on foot, and publishes it here; every EDVR change to the commander's body (the pin, the square-up, the bend,
// the arm IK, the head and first-person body hides, the unclip, the body shift and pose carry, the body cull) and the eye alignment's offsets are
// first-person behaviour and stand down while the byte is not 0, so the game's third-person camera shows the game's own body.
inline std::atomic<uint32_t> g_nativeOnFootCamMode{0};        // the byte as last read (bits 0..7), 0x100 once a read has succeeded
inline std::atomic<bool> g_nativeOnFootCamExternal{false};   // the byte was not 0 at the last read
inline std::atomic<uint64_t> g_nativeOnFootCamReads{0};      // controller updates read (the frame thread's staleness check)
inline std::atomic<uint64_t> g_nativeOnFootCamFlips{0};      // changes of the external flag the hook saw
inline std::atomic<uint64_t> g_nativeOnFootCamFlipQpc{0};    // QPC of the last change (the caller's clock: QueryPerformanceCounter)
// True while native stereo is on foot and the game's camera is not its first-person camera (the camera suite, its presets or its free camera).
inline bool nativeOnFootCameraExternal() {
    return g_nativeOnFootCamExternal.load(std::memory_order_relaxed) && nativeOnFootActive();
}
// The controller hook's post-call publication (native_on_foot_hooks.cpp), on the game's thread: mode is the byte after the update, qpc the read's time. Header-only so
// the rigs that compile native_on_foot_hooks.cpp without native_on_foot.cpp link. True when the flag changed.
inline bool nativeOnFootCameraPublish(uint8_t mode, uint64_t qpc) {
    const bool external = mode != 0;
    g_nativeOnFootCamMode.store(0x100u | mode, std::memory_order_relaxed);
    const bool flipped = g_nativeOnFootCamExternal.exchange(external, std::memory_order_acq_rel) != external;
    if (flipped) {
        g_nativeOnFootCamFlipQpc.store(qpc, std::memory_order_relaxed);
        g_nativeOnFootCamFlips.fetch_add(1, std::memory_order_relaxed);
    }
    g_nativeOnFootCamReads.fetch_add(1, std::memory_order_relaxed);
    return flipped;
}

// Count one engagement of a gate, only while the flag is set. A relaxed atomic add, safe from any thread.
inline void nativeOnFootCountGate(NativeOnFootGate gate) {
    if (nativeOnFootActive()) g_nativeOnFootGates[size_t(gate)].fetch_add(1, std::memory_order_relaxed);
}

// F49 (TEMPORARY): the pose freeze's hand-off. The census (native_on_foot_hooks.cpp, the game's job thread, every 100 ms) finds the local avatar's
// evaluator X from the local third-person skeleton interface and builds the plan (the frozen set and its rest locals) once per X; the Animate
// post-hook (native_on_foot.cpp, frAnim worker threads) reads it under the sequence counter and touches only that X. Inline, as above: the
// avatar hooks rig (tools\native_on_foot_hooks_test) compiles the census without native_on_foot.cpp.
namespace pfz {
struct Plan {
    uint64_t x = 0;                                  // the evaluator the plan belongs to
    uint32_t joints = 0;                             // the pose's joint count when the plan was built
    uint32_t anchor = 0xFFFFu;                       // the hips (the lowest common ancestor of the neck and the left foot)
    uint32_t count = 0;                              // members of F, parent first
    int32_t slot34 = -1, slot44 = -1;                // the member slots of the neck (34) and the head (44); -1 = not in F
    uint16_t member[nofoot::kPfzMaxF] = {};
    int16_t parentSlot[nofoot::kPfzMaxF] = {};      // -1 = the anchor
    float rest[nofoot::kPfzMaxF][8] = {};            // each member's rest record: pos vec4, quaternion
};
inline std::atomic<uint32_t> g_planSeq{0};           // odd while the census writes the plan
inline Plan g_plan;
inline std::atomic<uint64_t> g_localX{0};            // the local avatar's evaluator with a plan (0: none; the hook does nothing)
inline std::atomic<int> g_mode{1};                   // fix.native_on_foot_pose_freeze: 0 off, 1 probe (the default), 2 on
// The hook's newest span matrix of joint 34 for the local X (as the Animate handler left it, and as written when the freeze wrote it), sequence
// counted (the hook writes, the census reads it beside GetModelMatrix(34) for the cross-check).
inline std::atomic<uint32_t> g_spanSeq{0};
inline float g_span34[16] = {};
inline float g_span34Written[16] = {};
inline bool g_span34WasWritten = false;
inline uint64_t g_span34Ms = 0;
inline uint32_t g_span34Buffer = 0;                  // 0 [job+0x28], 1 [[X+0x1F0]+0xA0]
// F51: the evaluators the Animate hook sees, by call count. Two banks of open-addressed slots (no locks, no allocation; lossy by design): the hook
// counts into the active bank; every 5 s the frame thread clears the other bank, makes it the active one and reports the bank it left. So the
// inactive bank always holds the last full 5 s, and the census can ask whether an X was animated in the last 5 to 10 s.
constexpr uint32_t kXSlots = 64, kXProbe = 8;
struct XSlot {
    std::atomic<uint64_t> x{0};
    std::atomic<uint64_t> n{0};
};
inline XSlot g_xTable[2][kXSlots];
inline std::atomic<uint32_t> g_xBank{0};
inline std::atomic<uint64_t> g_xOverflow{0};
inline uint32_t xHash(uint64_t x) noexcept { return static_cast<uint32_t>(((x >> 4) * 0x9E3779B97F4A7C15ull) >> 58); }   // 6 bits: 64 slots
inline void xNote(uint64_t x) noexcept {
    if (!x) return;
    XSlot* bank = g_xTable[g_xBank.load(std::memory_order_relaxed) & 1u];
    const uint32_t h = xHash(x);
    for (uint32_t i = 0; i < kXProbe; ++i) {
        XSlot& s = bank[(h + i) % kXSlots];
        uint64_t cur = s.x.load(std::memory_order_relaxed);
        if (cur == 0 && s.x.compare_exchange_strong(cur, x, std::memory_order_relaxed)) cur = x;
        if (cur == x) {
            s.n.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_xOverflow.fetch_add(1, std::memory_order_relaxed);
}
// The calls counted for x in both banks (the last 5 to 10 s).
inline uint64_t xCount(uint64_t x) noexcept {
    if (!x) return 0;
    uint64_t total = 0;
    const uint32_t h = xHash(x);
    for (uint32_t b = 0; b < 2; ++b)
        for (uint32_t i = 0; i < kXProbe; ++i) {
            const XSlot& s = g_xTable[b][(h + i) % kXSlots];
            if (s.x.load(std::memory_order_relaxed) == x) total += s.n.load(std::memory_order_relaxed);
        }
    return total;
}
// F51: the census's candidates per latched skeleton ([0] the local 3P one, [1] the FP one), for the frame thread's 5 s line. Relaxed: a line may mix
// two census steps. how: 0 none, 1 the iface's X (seen in Animate), 2 matched by P = [X+8] == GetPoseData (unique among the X seen), 3 the iface's X
// unseen in Animate and no unique P match (the F49 behaviour), 4 ambiguous P (several X share it; none taken).
struct Candidates {
    std::atomic<uint64_t> iface{0}, kind{0}, x2B8{0}, x120{0}, f2C0{0}, fF8{0}, gp{0}, p2B8{0}, p120{0}, chosen{0}, pMatches{0}, how{0}, stamp{0};
};
inline Candidates g_cand[2];
// F55 (TEMPORARY): the FP X's plan. The census (FP skeleton, g_cand[1]'s chosen X: the kind's X seen in Animate, else the unique P match, else the
// kind's X) publishes the FP X, its joint count and the subtree of the clavicles 19 and 20 under g_fdSeq; the Animate post-hook snapshots joints 19,
// 20, 37 and 44 for that X (and the arm IK's FP joints). F66: the shoulder drop that lowered the subtree (fix.native_on_foot_fp_shoulder_drop_cm) is
// gone; the arm IK key keeps the hook and the FP candidates running when the pose freeze key is off.
struct HandRig {
    uint8_t why = 1;    // 0 valid; 1 bad plan/rest; 2 finger branch topology; 3 missing joint name; 4 degenerate geometry/transform
    uint8_t count = 0;
    int8_t palmarSign = 1;   // signed: the left physical palm faces opposite its proper stored basis
    uint16_t hand = 0xFFFFu, attach = 0xFFFFu;
    uint16_t joint[24] = {}, parentSlot[24] = {};
    uint16_t name[24] = {};
    float restLocal[24][16] = {}, boneToPalm[24][16] = {}, palm[16] = {};
    // F75: separate grip-contact geometry derived from K2. The anatomical palm and its finger retarget data remain unchanged.
    float supportContactLocal[16] = {};
    float supportWidth = 0.0f, supportLength = 0.0f, supportRadius = 0.0f;
    bool supportContactValid = false;
    float width = 0.0f, length = 0.0f, skinRadius = 0.0f;
};
inline int8_t handPalmarSign(int side) noexcept { return side == 0 ? int8_t(-1) : int8_t(1); }
struct FpDropPlan {
    uint64_t x = 0;
    uint32_t generation = 0;
    uint32_t joints = 0;
    uint32_t count = 0;
    uint16_t member[nofoot::kPfzMaxF] = {};
    uint8_t handCount = 0;
    HandRig hand[2];
};
inline std::atomic<uint32_t> g_fdSeq{0};
inline FpDropPlan g_fdPlan;
inline std::atomic<uint64_t> g_fdX{0};               // the plan's X (0: none), set after the plan is published; the hook's quick filter
// F56 (TEMPORARY): the 3P arm IK's plan. The census (the local 3P skeleton, the pose freeze's X) derives each arm's chain and groups once per X
// (nofoot::ikArmChain: [0] L, clavicle 35, hand 58, attach 67; [1] R, 36, 59, 66, from the joint probe) and publishes them under g_armSeq;
// g_armX (set after the plan is published) is the hook's quick filter. g_ikMode is fix.native_on_foot_arm_ik: 0 off, 1 probe,
// 2 nudge, 3 on (the default); g_ikWanted (the key not off) keeps the Animate hook and the FP census running when the pose freeze key is off.
// F57: plus the torso square-up's plan (nofoot::ikSquarePlan: the chest root, the parent of the clavicles 35 and 36, and the spine 16 subtree
// with each member's level); sqOk false refuses the square-up alone.
// F65b: plus the legs (stand armed): [0] L, [1] R by nofoot::ikLegChain from the joint probe's feet, legOk when nofoot::ikLegPairWhy passes
// (shared pelvis, the pelvis under the anchor, the RE's indices with the R foot confirmed by FindJoint), and the upper body UB (subtree of the
// anchor less both thigh subtrees) that the raise moves.
struct ArmPlan {
    uint64_t x = 0;
    uint32_t generation = 0;
    uint32_t joints = 0;
    bool ok[2] = {false, false};
    nofoot::IkArm side[2];
    bool sqOk = false;
    nofoot::IkSquarePlan sq;
    bool legOk = false;
    uint32_t legWhy = 1, legAnchor = 0xFFFFu;
    nofoot::IkArm leg[2];
    uint16_t ubN = 0;
    uint16_t ub[nofoot::kPfzMaxJoints] = {};
    uint8_t handCount = 0;
    HandRig hand[2];
};
inline std::atomic<uint32_t> g_armSeq{0};
inline ArmPlan g_armPlan;
inline std::atomic<uint64_t> g_armX{0};
inline std::atomic<int> g_ikMode{3};
inline std::atomic<bool> g_ikWanted{true};
}  // namespace pfz

// Render thread, once a frame at the Present boundary (device_hook.cpp's tick). Installs the hook the first time (VR only), re-reads the
// key when the configuration changes, writes the queued setter lines and the 5 s gate line.
void nativeOnFootFrameBoundary(uint32_t frameNo);

// Shutdown (device_hook.cpp, before nativeOnFootHooksShutdown): puts the gate byte back to 1 if EDVR holds it at 0. Frame-thread context.
void nativeOnFootShutdown();

// THE HUD ISSUER PROBE (F3, log only, always on, any key state). beginPanelOverride (vscreen.cpp) passes every VR-path draw's bound
// vertex shader here: the common case is one load and two compares. A match captures the call stack (the draw thread), at most once per
// vertex shader per 10 s, into a ring of eight; the frame boundary resolves the frames and logs them. Not called on the flat profile.
void nativeOnFootHudCapture(int index, uint64_t vs);   // native_on_foot.cpp
// F115: the cockpit's own panels (nofoot::f115::kCockpitPanelVs) seen by the same probe: the draw thread raises the flag, the frame thread's f115Tick takes it
// once a frame (the boarding's first cockpit frame ends the on-foot camera before Status.json does). The stock panel shader is only counted while on foot.
inline std::atomic<bool> g_f115CockpitPanelDrew{false};
inline std::atomic<uint32_t> g_f115StockPanelOnFoot{0};
// F115: the panels made the boarding edge and Status.json still says on foot: the seat memory's word (device_hook.cpp seatContextPublish, at the next frame
// tick) says seated from now, so the runtime puts the stored seat back at the same edge as the on-foot camera changes, not a second later. Frame thread writes.
inline std::atomic<bool> g_f115BoardedEarly{false};
inline bool nativeOnFootBoardedEarly() { return g_f115BoardedEarly.load(std::memory_order_relaxed); }
inline void nativeOnFootHudProbe(uint64_t vs) {
    const int index = nofoot::hudIssuerIndex(vs);
    if (index >= 0) nativeOnFootHudCapture(index, vs);
    if (vs == nofoot::f115::kCockpitPanelVs) g_f115CockpitPanelDrew.store(true, std::memory_order_relaxed);
    else if (vs == nofoot::f115::kCockpitPanelStockVs && nativeOnFootActive()) g_f115StockPanelOnFoot.fetch_add(1, std::memory_order_relaxed);
}

// F18 (TEMPORARY, fix.native_on_foot_body_cull: game | none | front). The frame thread publishes the mode here (readKey: 0 game, 1 none,
// 2 front); the DrawIndexedInstanced thunk (vscreen.cpp) asks nativeOnFootBodyCullWanted() for every VR-path draw -- two relaxed loads --
// and only then calls Begin, which matches the local player's third-person torso draws (shader hashes, index count, one instance,
// stencil ref 21) and binds a clone of the game's rasterizer state with only CullMode changed. End puts the game's own state back.
inline std::atomic<uint8_t> g_nativeOnFootBodyCull{0};
inline bool nativeOnFootBodyCullWanted() {
    // F93d: first-person body behaviour: the game's third-person camera draws the torso with the game's own state
    return g_nativeOnFootBodyCull.load(std::memory_order_relaxed) != 0 && nativeOnFootActive() && !nativeOnFootCameraExternal();
}
// Draw thread, owner context only. True when the culled clone is bound: the caller owes exactly one End after the draw.
bool nativeOnFootBodyCullBegin(ID3D11DeviceContext* ctx, uint32_t indexCount, uint32_t instances);   // native_on_foot.cpp
void nativeOnFootBodyCullEnd(ID3D11DeviceContext* ctx);
// vScreen's shutdown, beside remlokShutdown: releases the cached clones and the game states they were made from.
void nativeOnFootBodyCullShutdown();

// F26: the native on-foot mode-change counter (the frame thread bumps it on each enter or leave). The UI layer re-arms its eye check on a change.
inline std::atomic<uint32_t> g_nativeOnFootEdges{0};
inline uint32_t nativeOnFootEdges() { return g_nativeOnFootEdges.load(std::memory_order_relaxed); }
// F26: fix.native_on_foot_sharp_hud is on (live, VR only; set by readKey). The sharp on-foot HUD is wanted while this and native stereo on foot hold.
inline std::atomic<bool> g_nativeOnFootSharpHudKey{false};
inline bool nativeOnFootSharpHudWanted() { return g_nativeOnFootSharpHudKey.load(std::memory_order_relaxed) && nativeOnFootActive(); }
// F27: the native eye order. The game renders the RIGHT eye first in native on foot, so while the setter has it substituted the eye rule is flipped
// (the first target of a frame is the right eye). Set by the setter hook at the mode switch; the cockpit never sees it set.
// F27: the game's render-mode changes as the setter sees them (the UI layer's eye check re-arms on a change, not on 'native on foot active').
inline std::atomic<uint32_t> g_nativeModeEdges{0};
inline std::atomic<uint32_t> g_nativeLastEffective{0};

// F44 (TEMPORARY instrument, fix.native_on_foot_marker_probe, default off; draw_record_tag.h): measure only. The marker post tags each world
// marker's t' (low mantissa bits); the Map/Unmap tees find the tagged record in the HUD's t33 pool; the draw tee (F46: the on-foot HUD draws,
// the marker composite B10B032BDFD46700 and the world pool draws only) names the marker composite draw that reads it and each eye's world view,
// read back from GPU copies, and sets the marker's NDC against P's per eye. Nothing drawn changes. The frame thread publishes the key
// (readKey); every tee asks nativeOnFootMarkerProbeLive() first: two relaxed loads.
inline std::atomic<bool> g_nativeOnFootMarkerProbeKey{false};
inline bool nativeOnFootMarkerProbeLive() {
    return g_nativeOnFootMarkerProbeKey.load(std::memory_order_relaxed) && nativeOnFootActive();
}
// F100: the probe's measurement families (F53's census frames' notes, untagged records and key census, F57's cb1 census, F47 item 4's re-measure of
// the patched rows, the 8 burst lines a second, the `marker probe:` 1 s lines and F53's per-key lines) run only while fix.native_on_foot_marker_probe
// itself is on. g_nativeOnFootMarkerProbeKey above is the machinery (the probe key or the marker fix); this is the probe key alone (readKey).
inline std::atomic<bool> g_nativeOnFootMarkerMeasureKey{false};
inline bool nativeOnFootMarkerMeasure() { return g_nativeOnFootMarkerMeasureKey.load(std::memory_order_relaxed); }
// F100: the draw tee's prefilter (render thread; nofoot::f100::teeFast). The slow path leaves g_nativeOnFootTeeFastFrame at the frame a draw that is
// neither an on-foot HUD issuer's nor the marker composite's may skip it in; the frame boundary mirrors the probe's frame into g_nativeOnFootTeeFrameNow;
// Begin clears the fast frame. A skipped draw only advances the draw serial (F47's, which Begin and the label instrument compare against) and counts.
inline std::atomic<uint32_t> g_nativeOnFootTeeFastFrame{nofoot::f100::kNoFrame};
inline std::atomic<uint32_t> g_nativeOnFootTeeFrameNow{0};
inline uint64_t g_nativeOnFootDrawSerial = 0;     // render thread only
inline uint64_t g_nativeOnFootTeeFastCalls = 0;   // render thread only; the probe's cost line takes and clears it at its window's close (F125: 10 s, 1 s detail)
inline uint64_t g_nativeOnFootTeeClockTicks = 0, g_nativeOnFootTeeClockCalls = 0;   // F100: the tee's clock (vscreen.cpp), render thread only
inline bool nativeOnFootTeeFast(uint64_t vs) {
    if (!nofoot::f100::teeFast(g_nativeOnFootTeeFastFrame.load(std::memory_order_relaxed), g_nativeOnFootTeeFrameNow.load(std::memory_order_relaxed), vs))
        return false;
    ++g_nativeOnFootDrawSerial;
    ++g_nativeOnFootTeeFastCalls;
    return true;
}
// Owner context only (vscreen.cpp): a buffer mapped for writing (after the real Map), a buffer about to be unmapped (before the real Unmap), and
// a draw before its verdict (the game's bindings). F45: every draw call kind, named by `kind` ('N' DrawInstanced, 'X' DrawIndexedInstanced,
// 'D' Draw and 'I' DrawIndexed with one instance from 0, 'Y' and 'Z' the indirect ones, whose instance range is not readable here).
// F67d: and the draw's own range (the vertex or index count per instance, the first vertex or index, the base vertex), for the icon mesh; 0 count for
// the indirect ones.
void nativeOnFootMarkerProbeMap(ID3D11Resource* resource, void* data, int mapType);
void nativeOnFootMarkerProbeUnmap(const ID3D11Resource* resource);
void nativeOnFootMarkerProbeDraw(ID3D11DeviceContext* ctx, uint32_t instances, uint32_t startInstance, char kind, uint32_t count = 0, uint32_t start = 0,
                                 int32_t base = 0);
// F47: the game's UpdateSubresource of a buffer (owner context, before the real call): the marker composite's b0 CPU shadow takes the write.
// `boxed` with [left, right) the byte range the data covers; otherwise the whole buffer from data.
void nativeOnFootMarkerProbeUpdate(const ID3D11Resource* resource, const void* data, bool boxed, uint32_t left, uint32_t right);
// vScreen's shutdown: the staging buffers (and F47's private constant buffers).
void nativeOnFootMarkerProbeShutdown();

// F47 (TEMPORARY, fix.native_on_foot_marker_fix = off | on, default on since F98c, live; it turns the probe's machinery on with it): the marker
// composite's per-eye NDC correction. The draw tee decides (both eyes' marker draws matched in the recent read-backs, this draw the one that
// matched, the CPU shadows of its b0 and o_hud and the eye's world view present, |dx|, |dy| < 0.1 NDC and w > 0; the last good delta held one
// frame on a miss); Begin, inside the UI layer's bracket around the game's own issue, binds a private copy of b0 with rows 4 and 5 moved by
// (dx, dy) . row 7; End, after the layer's second issues, puts the game's b0 back. Begin returns false (and binds nothing) on every refusal.
// F63: the match is identified on the CPU at the draw (the instance stream shadowed at its Unmap) and held 90 frames (drawtag::HeldMatch). F100:
// the patch reads no read-back of its own patched rows (F63's bias EMA is gone), so F47 item 4's re-measure is a measurement (the probe key).
inline std::atomic<bool> g_nativeOnFootMarkerFixKey{false};
inline bool nativeOnFootMarkerFixLive() {
    return g_nativeOnFootMarkerFixKey.load(std::memory_order_relaxed) && nativeOnFootMarkerProbeLive();
}
// F52 (TEMPORARY, fix.native_on_foot_marker_ref = world_first | temporal, default temporal, live): the view the correction measures each eye
// against (drawtag::RefMode). temporal is used while the temporal pass has the eye's camera rows, world_first otherwise; a change of the
// reference in use resets the eye's baselines. Set by readKey; read by the render thread. F62c: the patch measures against the eye's scene
// rows whatever this key says (drawtag::patchDecide; no trust gate); the key now steers only the F52 measure lines.
inline std::atomic<int> g_nativeOnFootMarkerRefKey{1};
bool nativeOnFootMarkerFixBegin(ID3D11DeviceContext* ctx);
void nativeOnFootMarkerFixEnd(ID3D11DeviceContext* ctx);

// F53: the marker probe's newest projection per eye, for the eye dump's marker crop (temporal_pass.cpp): P through the eye's scene rows (ndc,
// set once per eye a frame at its first marker composite draw) and the marker as drawn (ndc, from the read-back, 2..3 frames late), each with
// its GetTickCount64 stamp (0: never). Relaxed: a reader may mix two frames' values, which only moves a crop's centre.
// F62c: the waypoint series' fields. s*: this eye-frame's P through the scene rows (ndc NaN when w <= 0; w; the find's key; the head's speed
// in deg/s, -1 unknown; the probe frame it was projected in), published at every eye-frame with scene rows and a find, in front or not.
// drawnFrame: the probe frame of the read-back that gave `drawn`. patch*: the last marker composite draw of the eye the fix bound (its probe
// frame, key, delta in NDC, fresh or (F64) through the eye's newest scene rows one frame older, and draws bound in that frame).
struct MarkerCropNdc {
    std::atomic<float> p[2];
    std::atomic<float> drawn[2];
    std::atomic<uint64_t> pMs, drawnMs;
    std::atomic<bool> sHave;
    std::atomic<float> sNdc[2], sW, sHead;
    std::atomic<uint32_t> sFrame, sKey, drawnFrame;
    std::atomic<uint32_t> patchFrame, patchKey, patchDraws;
    std::atomic<float> patchDx, patchDy;
    std::atomic<bool> patchNewest;
};
inline MarkerCropNdc g_markerCropNdc[2];   // zero-initialised (static storage)
// F62c: the marker probe's frame now (the frame the draws and the submits between two Presents share; the series' age-0 test).
uint32_t nativeOnFootMarkerFrameNow();
// The Present-boundary render/Animate frame identity for joining captures across frame domains.
uint32_t nativeOnFootRenderFrame();
// F86h: the marker composite draws of a render frame, per eye: those the fix decided to patch and those whose patch was bound at the draw's issue. False when the
// frame is not in the ring (256 frames) or the fix saw no draw in it.
bool nativeOnFootMarkerFrameCounts(uint32_t renderFrame, uint32_t decided[2], uint32_t bound[2]);
// F87p (read only): the label's draws of a render frame, as a JSON array of two arrays (eye 0, eye 1), for the eye dump's decisions.json; the characters written, 0 when the
// frame has none (the ring holds 256 frames).
size_t nativeOnFootLabelDrawsJson(uint32_t renderFrame, char* buf, size_t cap);
// F87p (read only; owner context, vscreen.cpp): right before the game's own issue of a draw, with whether the UI layer's bracket was begun for it. A draw the marker
// instrument named at fixDecide's untagged return is completed here (the viewport as mapped, the b0 bound); every other draw returns at once.
void nativeOnFootLabelIssue(ID3D11DeviceContext* ctx, bool layered);
struct NativeOnFootPoseStamp {
    bool valid = false, writePresent = false, matricesValid = false, sourceFresh = false;
    uint32_t renderFrame = 0, fpFrame = 0, fpSequence = 0, gGameFrame = 0, itemSerial = 0, rigGeneration = 0;
    uint64_t rig = 0, qpc = 0;
    uint16_t armJoints[2][3] = {}; // U/F/H indices for left and right.
};
struct WeaponUiGetterObservation {
    uintptr_t renderer = 0, part = 0, cameraPointer = 0;
    float attachment[16] = {};
};
struct NativeWeaponUiSubmitIdentity {
    uint32_t renderFrame = 0, itemSerial = 0, ordinal = ~0u, submitThread = 0;
    uint64_t submitQpc = 0, renderer = 0, part = 0, instance = 0;
    uint16_t nameId = 0;
    uint8_t facing = 0;
    int32_t space = 0;
    bool itemRead = false, fieldsValid = false, instanceValid = false;
};
bool nativeOnFootWeaponUiCurrentSubmit(uintptr_t instance, NativeWeaponUiSubmitIdentity* out) noexcept;
// Copy the newest completed after-write pose stamp for this exact native render frame.
bool nativeOnFootPoseStamp(uint32_t renderFrame, NativeOnFootPoseStamp* out);
// Capture-only companion data; never part of the eye dump's binary record layout. This copies completed existing publications, with no game calls.
// Froot/M3p are row-vector model-to-world matrices, G is the existing offset-adjusted FP-to-3P mapping, bodyOffset is world metres,
// and the pin targets are 3P model coordinates.
struct NativeOnFootCameraEvidence {
    enum : uint32_t { kMatricesValid = 1, kSourcesExact = 2, kTimesFresh = 4, kPinTargetsValid = 8, kPoseSourcesFresh = 16, kM3pCanonicalized = 32 };
    uint32_t status = 0; // 0 unavailable, 1 completed F81 row for the requested hook frame, 2 cached IkG (never an exact draw claim)
    uint32_t validFlags = 0, requestedFrame = 0, sourceFrame = 0, fpFrame = 0, rootReadRenderFrame = 0, gGameFrame = 0, itemSerial = 0;
    uint64_t sampleQpc = 0, qpc = 0, fpQpc = 0, rootQpc = 0, ms = 0, rootMs = 0, qpcFrequency = 0;
    // ms/rootMs exist only in cached IkG; F81 has QPC stamps instead. kTimesFresh means all available source stamps are non-future and <=100 ms old.
    float froot[16] = {}, m3p[16] = {}, G[16] = {}, bodyOffset[3] = {}, pinEyeTarget[3] = {}, pinTarget[3] = {};
};
bool nativeOnFootCameraEvidence(uint32_t nativeRenderFrame, NativeOnFootCameraEvidence* out);
// Eye-run-only observation of existing panel input calls; independent of the cursor drawing setting. No game getters or writes.
void nativeOnFootMenuInputArm(const wchar_t* stamp,uint32_t firstLedgerFrame,uint32_t lastLedgerFrame);
void nativeOnFootMenuInputBoundary(uint32_t ledgerFrame);
std::string nativeOnFootMenuInputEnd();
std::string nativeOnFootMenuInputDrawBridge(uint32_t ledgerFrame,uint32_t renderFrame,uint64_t qpc);
void nativeOnFootMenuInputDiscard();
// F63: the marker fix's per-frame residual for an eye, from the read-back of a fresh patched frame (2..3 frames after it): out[0..1] the drawn
// marker against P + e (the fixed e), out[2..3] the e the patch aimed at (e less the bias), px of the eye's target. False when that frame
// was not a fresh patched frame read back (or the ring has moved on).
bool nativeOnFootMarkerResidual(uint32_t frame, int eye, float out[4]);
// F76 synchronized rifle-optic diagnostic, called only by the FinalCrisp run lifecycle.
namespace nofoot::optic_capture { struct RowDescriptor; }
void nativeOnFootOpticCaptureBegin(const wchar_t* stamp);
void nativeOnFootOpticCaptureSample(uint32_t index, uint32_t sceneFrame, uint32_t renderFrame, uint32_t epoch, uint64_t sequence, const char* imageStatus,
                                    bool sceneRowsRead, bool sceneRowsOk, bool sceneRowsOwn, bool sceneRowsBound, int32_t sceneRowsFollow,
                                    uint32_t sceneRowsFrame, uint64_t sceneRowsQpc, const float sceneRows[12]);
void nativeOnFootOpticCaptureEnd(const char* reason, const nofoot::optic_capture::RowDescriptor* rows, uint32_t rowCount, uint32_t unmatchedCallbacks);

}  // namespace edvr
