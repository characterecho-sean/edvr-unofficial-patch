#pragma once
// Native on-foot VR, flight F0 (docs\design-native-onfoot-vr-2026-10-13.md; the decisions are native_on_foot_core.h).
//
// THE HOOK (VR profile only). A CodeHook on the game's render-mode setter (EliteDangerous64.exe+0x28723C0, build 332841), installed
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
// substitution; while the key is on and a game session is live EDVR holds the byte at EliteDangerous64.exe+0x5E82B3E at 0, so the game's own
// requester asks for HMD. Goes away when the arc picks one.
//
// THE BODY (F2). While the flag is set, explorerCamNativeBody(...) runs Explorer Cam's fade and head-part code for the local avatar, once a
// frame, under either method. Its head hooks are wanted from launch while fix.native_on_foot is on.
//
// THE GATES. Log-only counters, no behaviour: each engaged gate calls nativeOnFootCountGate() and counts only while the flag is set.
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "native_on_foot_core.h"   // nofoot::hudIssuerIndex: the on-foot HUD issuers' vertex shaders

struct ID3D11DeviceContext;   // the F18 body cull's Begin/End (d3d11.h is not needed by the rigs that include this header)
struct ID3D11Resource;        // F44: the marker probe's Map/Unmap tees

namespace edvr {

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

// Count one engagement of a gate, only while the flag is set. A relaxed atomic add, safe from any thread.
inline void nativeOnFootCountGate(NativeOnFootGate gate) {
    if (nativeOnFootActive()) g_nativeOnFootGates[size_t(gate)].fetch_add(1, std::memory_order_relaxed);
}

// F49 (TEMPORARY): the pose freeze's hand-off. The census (explorer_cam.cpp, the game's job thread, every 100 ms) finds the local avatar's
// evaluator X from the local third-person skeleton interface and builds the plan (the frozen set and its rest locals) once per X; the Animate
// post-hook (native_on_foot.cpp, frAnim worker threads) reads it under the sequence counter and touches only that X. Inline, as above: the
// explorer cam rig compiles the census without native_on_foot.cpp.
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
}  // namespace pfz

// Render thread, once a frame at the Present boundary (device_hook.cpp's tick). Installs the hook the first time (VR only), re-reads the
// key when the configuration changes, writes the queued setter lines and the 5 s gate line.
void nativeOnFootFrameBoundary(uint32_t frameNo);

// Shutdown (device_hook.cpp, before explorerCamShutdown): puts the gate byte back to 1 if EDVR holds it at 0. Frame-thread context.
void nativeOnFootShutdown();

// THE HUD ISSUER PROBE (F3, log only, always on, any key state). beginPanelOverride (vscreen.cpp) passes every VR-path draw's bound
// vertex shader here: the common case is one load and two compares. A match captures the call stack (the draw thread), at most once per
// vertex shader per 10 s, into a ring of eight; the frame boundary resolves the frames and logs them. Not called on the flat profile.
void nativeOnFootHudCapture(int index, uint64_t vs);   // native_on_foot.cpp
inline void nativeOnFootHudProbe(uint64_t vs) {
    const int index = nofoot::hudIssuerIndex(vs);
    if (index >= 0) nativeOnFootHudCapture(index, vs);
}

// F18 (TEMPORARY, fix.native_on_foot_body_cull: game | none | front). The frame thread publishes the mode here (readKey: 0 game, 1 none,
// 2 front); the DrawIndexedInstanced thunk (vscreen.cpp) asks nativeOnFootBodyCullWanted() for every VR-path draw -- two relaxed loads --
// and only then calls Begin, which matches the local player's third-person torso draws (shader hashes, index count, one instance,
// stencil ref 21) and binds a clone of the game's rasterizer state with only CullMode changed. End puts the game's own state back.
inline std::atomic<uint8_t> g_nativeOnFootBodyCull{0};
inline bool nativeOnFootBodyCullWanted() {
    return g_nativeOnFootBodyCull.load(std::memory_order_relaxed) != 0 && nativeOnFootActive();
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
// Owner context only (vscreen.cpp): a buffer mapped for writing (after the real Map), a buffer about to be unmapped (before the real Unmap), and
// a draw before its verdict (the game's bindings). F45: every draw call kind, named by `kind` ('N' DrawInstanced, 'X' DrawIndexedInstanced,
// 'D' Draw and 'I' DrawIndexed with one instance from 0, 'Y' and 'Z' the indirect ones, whose instance range is not readable here).
void nativeOnFootMarkerProbeMap(ID3D11Resource* resource, void* data, int mapType);
void nativeOnFootMarkerProbeUnmap(const ID3D11Resource* resource);
void nativeOnFootMarkerProbeDraw(ID3D11DeviceContext* ctx, uint32_t instances, uint32_t startInstance, char kind);
// F47: the game's UpdateSubresource of a buffer (owner context, before the real call): the marker composite's b0 CPU shadow takes the write.
// `boxed` with [left, right) the byte range the data covers; otherwise the whole buffer from data.
void nativeOnFootMarkerProbeUpdate(const ID3D11Resource* resource, const void* data, bool boxed, uint32_t left, uint32_t right);
// vScreen's shutdown: the staging buffers (and F47's private constant buffers).
void nativeOnFootMarkerProbeShutdown();

// F47 (TEMPORARY, fix.native_on_foot_marker_fix = off | on, default off, live; it turns the probe's machinery on with it): the marker
// composite's per-eye NDC correction. The draw tee decides (both eyes' marker draws matched in the recent read-backs, this draw the one that
// matched, the CPU shadows of its b0 and o_hud and the eye's world view present, |dx|, |dy| < 0.1 NDC and w > 0; the last good delta held one
// frame on a miss); Begin, inside the UI layer's bracket around the game's own issue, binds a private copy of b0 with rows 4 and 5 moved by
// (dx, dy) . row 7; End, after the layer's second issues, puts the game's b0 back. Begin returns false (and binds nothing) on every refusal.
inline std::atomic<bool> g_nativeOnFootMarkerFixKey{false};
inline bool nativeOnFootMarkerFixLive() {
    return g_nativeOnFootMarkerFixKey.load(std::memory_order_relaxed) && nativeOnFootMarkerProbeLive();
}
// F52 (TEMPORARY, fix.native_on_foot_marker_ref = world_first | temporal, default temporal, live): the view the correction measures each eye
// against (drawtag::RefMode). temporal is used while the temporal pass has the eye's camera rows, world_first otherwise; a change of the
// reference in use resets the eye's baselines. Set by readKey; read by the render thread.
inline std::atomic<int> g_nativeOnFootMarkerRefKey{1};
bool nativeOnFootMarkerFixBegin(ID3D11DeviceContext* ctx);
void nativeOnFootMarkerFixEnd(ID3D11DeviceContext* ctx);

// F53: the marker probe's newest projection per eye, for the eye dump's marker crop (temporal_pass.cpp): P through the eye's scene rows (ndc,
// set once per eye a frame at its first marker composite draw) and the marker as drawn (ndc, from the read-back, 2..3 frames late), each with
// its GetTickCount64 stamp (0: never). Relaxed: a reader may mix two frames' values, which only moves a crop's centre.
struct MarkerCropNdc {
    std::atomic<float> p[2];
    std::atomic<float> drawn[2];
    std::atomic<uint64_t> pMs, drawnMs;
};
inline MarkerCropNdc g_markerCropNdc[2];   // zero-initialised (static storage)

}  // namespace edvr
