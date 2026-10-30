// The SRV turret view (issue 140): the camera and the HUD in the Scorpion's gunner view in VR. srv_turret_math.h says what is wrong, the two TEMPORARY keys and the
// maths; srv_turret_hud_shader.h and srv_turret_hud_binding.h are the private geometry shader and its binding (tools/srv_turret_hud_test runs them on WARP);
// srv_turret.cpp is this module.
//
// PART 1, THE CAMERA. srvTurretFrameTick (the frame thread, at the frame boundary just before alignTick) reads Status.json's turret-view bit through the journal
// watcher and the key; alignTick adds srvTurretBackSeatedZ() to the seated z of the eye alignment it already publishes (one publisher, so the serials stay coherent) and
// tells srvTurretNotePublished what it published. Applied at once, with no ramp: the game itself cuts the camera on a view switch.
//
// PART 2, THE HUD. While the turret view is on and the gap key is above 0 the draw gate is open (srvTurretHudWantsDraws), forwardWithVerdict opens a scope for every
// draw, and a draw of the targeting sprite's shader pair into an eye target is issued through the private geometry shader, for EVERY issue the draw path makes of
// it (the game's own, the UI layer's, the interface depth's): the scope spans the whole function and puts the game's geometry stage back on every way out.
//
// WHAT THE LOG SHOWS (a flight in the turret view must tell "never ran" from "ran"):
//   "srv turret settings: ..."        the two keys, at the first frame and at each change.
//   "srv turret view: on|off at frame N (Status.json Flags 0x........, seat kind 0x.)"      each edge of the turret view.
//   "srv turret camera: back X m published (serial N)" / "srv turret camera: back cleared (serial N)"      each change of what alignTick published.
//   "srv turret hud: precompiled geometry shader ... created|FAILED" (shader_swap.cpp), once, at the first draw that asks for it.
//   "srv turret hud: first draw ..." once; "srv turret hud: refused a draw (reason) ..." once per reason.
//   "srv turret hud: gap=... draws[L,R]=.. bound=.. refused[...]=.." every 10 s while the turret view was on at any point of the window.
//   THE GPU INSTRUMENT (whether the shader RAN, and what it wrote): once a second per eye, while the pass is bound, one matched draw is issued through a stream-output twin of the
//   shader (the same bytecode and constants, still rasterized) with a pipeline-statistics query and a stream-output-statistics query around the scope (Begin right after the bind,
//   End right before the restore). Read back without a stall (DONOTFLUSH, a staging copy mapped DO_NOT_WAIT) on later frames, given up after 8:
//   "srv turret hud stats: eye=E issues=N ia_prims=.. vs=.. gs=.. gs_prims=.. c_inv=.. c_prims=.. ps=.. (lost N) draw=K"   gs=0 with ia_prims>0: the shader never ran.
//   "srv turret hud capture: eye=E draw=K so_prims=.. vertices=.. in_window=.. w=[..] ndc_x=[..] ndc_y=[..] shift_clip=[..] s=.. gap=.."   the vertices the shader wrote.
#pragma once

#include <atomic>
#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

namespace detail {
// Asked per draw with no call: the build has no /GL to fold a cross-TU getter (target_sharp.h's reason).
extern std::atomic<bool> g_srvTurretHudWanted;
// The GPU instrument's issue count: true only inside the scope of the one draw a second that is sampled (srv_turret.cpp).
extern std::atomic<bool> g_srvTurretHudCounting;
extern std::atomic<uint32_t> g_srvTurretHudIssues;
}  // namespace detail

// vscreen.cpp, at each place that issues the matched draw (the game's own issue and pureDrawReissue's re-issues): counts it while the sampled scope is open. One relaxed load otherwise.
inline void srvTurretHudCountIssue() {
    if (detail::g_srvTurretHudCounting.load(std::memory_order_relaxed)) detail::g_srvTurretHudIssues.fetch_add(1, std::memory_order_relaxed);
}

// True while the turret view is on (VR) and fix.srv_turret_hud_gap is above 0. Also a term of the draw gate (vscreen.cpp drawGateSubscribed).
inline bool srvTurretHudWantsDraws() { return detail::g_srvTurretHudWanted.load(std::memory_order_relaxed); }

// Part 1 and the state both parts read. Frame thread, once a frame, nativeOnFootFrameBoundary just before alignTick.
void srvTurretFrameTick(uint32_t frameNo);
// The seated z alignTick adds to its eye alignment: -D metres while the camera is moved back, else exactly 0. Frame thread.
double srvTurretBackSeatedZ();
// alignTick, right after it published an alignment: the z it added and the serial it published under. Said in the log when it changed. Frame thread.
void srvTurretNotePublished(double seatedZ, uint32_t serial);

// Part 2, the draw path (vscreen.cpp, the render thread). Begin: the game's bindings are its own; eye is the draw's eye (0 left, 1 right, -1 unknown). True when the
// private shader is bound for the issues that follow; End puts the game's geometry stage and constant slot back and is called only after a true Begin.
bool srvTurretHudBegin(ID3D11DeviceContext* ctx, int eye);
void srvTurretHudEnd(ID3D11DeviceContext* ctx);
// Once a frame, render thread (vScreenFrameBoundary): settles a binding that did not restore.
void srvTurretHudFrameBoundary(ID3D11DeviceContext* ctx);
// DLL unload: the shader, the constants and the saved bindings released.
void srvTurretShutdown();

}  // namespace edvr
