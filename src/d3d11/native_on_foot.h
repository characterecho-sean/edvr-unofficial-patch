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
// THE FLAG. nativeOnFootActive() is true from a substitution until the next forwarded natural HMD mode (3 or 4).
//
// THE GATES. Log-only counters, no behaviour: each engaged gate calls nativeOnFootCountGate() and counts only while the flag is set.
#include <atomic>
#include <cstddef>
#include <cstdint>

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

}  // namespace edvr
