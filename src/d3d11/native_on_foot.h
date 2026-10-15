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
// marker's t' (low mantissa bits); the Map/Unmap tees find the tagged record in the HUD's t33 pool; the draw tee names the on-foot HUD draw that
// reads it and the eye's world view, read back from GPU copies. Nothing drawn changes. The frame thread publishes the key (readKey); every tee
// asks nativeOnFootMarkerProbeLive() first: two relaxed loads.
inline std::atomic<bool> g_nativeOnFootMarkerProbeKey{false};
inline bool nativeOnFootMarkerProbeLive() {
    return g_nativeOnFootMarkerProbeKey.load(std::memory_order_relaxed) && nativeOnFootActive();
}
// Owner context only (vscreen.cpp): a buffer mapped for writing (after the real Map), a buffer about to be unmapped (before the real Unmap), and
// an instanced draw before its verdict (the game's bindings).
void nativeOnFootMarkerProbeMap(ID3D11Resource* resource, void* data, int mapType);
void nativeOnFootMarkerProbeUnmap(const ID3D11Resource* resource);
void nativeOnFootMarkerProbeDraw(ID3D11DeviceContext* ctx, uint32_t instances, uint32_t startInstance);
// vScreen's shutdown: the staging buffers.
void nativeOnFootMarkerProbeShutdown();

}  // namespace edvr
