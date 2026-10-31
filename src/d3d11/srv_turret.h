// The SRV turret view (issue 140): the camera and the HUD in the Scorpion's gunner view in VR. srv_turret_math.h says what is wrong, the four TEMPORARY keys, the families and the
// maths; srv_turret_hud_shader.h and srv_turret_hud_binding.h are the private geometry shaders and their binding (tools/srv_turret_hud_test runs them on WARP);
// journal_vehicle_latch.h is the Scorpion latch; srv_turret.cpp is this module.
//
// THE SCORPION ONLY. Status.json's turret-view bit is set in the Scarab too, whose turret view was fine, so both parts act only while the turret view is on AND the journal says the
// vehicle is the Scorpion (journalSrvScorpion: LoadGame's Ship or LaunchSRV's SRVType is combat_multicrew_srv_01; unknown, a training scenario included, is "not the Scorpion"; the
// main menu's music, a shutdown, DockSRV, SRVDestroyed and a load or launch of anything else clear it).
//
// PART 1, THE CAMERA. srvTurretFrameTick (the frame thread, at the frame boundary just before alignTick) reads Status.json's turret-view bit through the journal
// watcher and the key; alignTick adds srvTurretBackSeatedZ() to the seated z of the eye alignment it already publishes (one publisher, so the serials stay coherent) and
// tells srvTurretNotePublished what it published. Applied at once, with no ramp: the game itself cuts the camera on a view switch.
//
// PART 2, THE HUD. While the Scorpion's turret view is on and any of fix.srv_turret_panels_back, fix.srv_turret_radar_back and fix.srv_turret_hud_gap is above 0 the draw gate is
// open (srvTurretHudWantsDraws), forwardWithVerdict opens a scope for every draw, and a draw of one of the HUD families' shader pairs (srv_turret_math.h kFamilies: the two
// hologram families that draw the weapon panels from records 0.27 to 0.70 m out and the larger panels from 1.3 m and deeper, and the radar-named billboards and the target
// hologram sphere that draw the radar from 0.11 to 0.44 m) into an eye target, as a triangle list or strip, whose group (panels or radar) has a push above 0, or else a gap above 0,
// is issued through that signature's private geometry shader, for EVERY issue the draw path makes of it (the game's own, the UI layer's, the interface depth's): the scope spans
// the whole function and puts the game's geometry stage back on every way out. A group's push moves a vertex nearer than 1 m deeper by it; the gap (the first build's pull) is used
// for a group with no push. A draw of a group with neither is the game's own and is not counted. (The reticle's sprite, vs E508648660A352B2, is not a panel: an earlier build
// targeted it, at about 500 m, and moved nothing. The flight HUD's vectors, vs B7790CBFC6554097, are not in the pass.)
//
// WHAT THE LOG SHOWS (a flight in the turret view must tell "never ran" from "ran"):
//   "srv turret: the vehicle is the Scorpion|not the Scorpion (event E)"   each change of the latch (journal_watch.cpp, the worker thread): E is LoadGame, LaunchSRV, Music, DockSRV,
//     SRVDestroyed, Shutdown or NewJournal (another journal file).
//   "srv turret settings: ..."        the four keys, at the first frame and at each change.
//   "srv turret view: on|off at frame N (Status.json Flags 0x........, seat kind 0x.)"      each edge of the Scorpion's turret view (the view bit AND the latch).
//   "srv turret view: not the Scorpion at frame N (Status.json Flags ..., seat kind ...): ..."      the view bit came on in an SRV the latch does not call the Scorpion: nothing is changed.
//   "srv turret camera: back X m published (serial N)" / "srv turret camera: back cleared (serial N)"      each change of what alignTick published.
//   "srv turret hud: precompiled geometry shader ... created|FAILED" (shader_swap.cpp), once for each signature, at the first draw that asks for them.
//   "srv turret hud: first draw of family F ... the G group: a vertex at view depth A to B m moves D m deeper|nearer ..." once for each family (deeper: the push-back, nearer: the pull);
//     "srv turret hud: refused a draw (reason) ..." once per reason.
//   "srv turret hud: panels: gap=-1.10 m wMin=0.02 wMax=1.00; radar: ...; s[L,R]=.. draws[L,R]=.. bound=.. refused[...]=.. families[1989E6D3,...]=n,..." every 10 s while the
//     Scorpion's turret view was on at any point of the window: each group's constants (or "off"), the matched draws of each family (names, then counts, in kFamilies' order).
//   THE GPU INSTRUMENT (whether the shader RAN, and what it wrote): once a second per eye and family, while the pass is bound, one matched draw (the frame's draws of the family
//   rotate) is issued through a stream-output twin of its signature's shader (the same bytecode and constants, still rasterized) with a pipeline-statistics query and a
//   stream-output-statistics query around the scope (Begin right after the bind, End right before the restore). Read back without a stall (DONOTFLUSH, a staging copy mapped
//   DO_NOT_WAIT) on later frames, given up after 8:
//   "srv turret hud stats: eye=E issues=N ia_prims=.. vs=.. gs=.. gs_prims=.. c_inv=.. c_prims=.. ps=.. (lost N) draw=K family=F"   gs=0 with ia_prims>0: the shader never ran.
//   "srv turret hud capture: family=F eye=E draw=K so_prims=.. vertices=.. in_window=.. w=[..] ndc_x=[..] ndc_y=[..] shift_clip=[..] s=.. gap=.."   the vertices the shader wrote:
//   with a push the weapon panels' and the radar's draws read in_window above 0 at w 0.1 to 0.7 m and the larger panels' in_window 0 at 1.3 m and more (the pull: the other way round);
//   gap= is the signed gap the family's group ran with. The capture holds the first kCaptureBufferBytes of a draw's output, so a long draw is a sample.
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

// True while the Scorpion's turret view is on (VR) and any of fix.srv_turret_panels_back, fix.srv_turret_radar_back and fix.srv_turret_hud_gap is above 0. Also a term of the draw gate
// (vscreen.cpp drawGateSubscribed).
inline bool srvTurretHudWantsDraws() { return detail::g_srvTurretHudWanted.load(std::memory_order_relaxed); }

// Part 1 and the state both parts read. Frame thread, once a frame, nativeOnFootFrameBoundary just before alignTick.
void srvTurretFrameTick(uint32_t frameNo);
// The seated z alignTick adds to its eye alignment: -D metres while the camera is moved back, else exactly 0. Frame thread.
double srvTurretBackSeatedZ();
// alignTick, right after it published an alignment: the z it added and the serial it published under. Said in the log when it changed. Frame thread.
void srvTurretNotePublished(double seatedZ, uint32_t serial);

// Part 2, the draw path (vscreen.cpp, the render thread). Begin: the game's bindings are its own; family is the draw's shader pair's family (srv_turret_math.h familyOf), eye
// the draw's eye (0 left, 1 right, -1 unknown). True when the family's private shader is bound for the issues that follow; End puts the game's geometry stage and constant
// slot back and is called only after a true Begin.
bool srvTurretHudBegin(ID3D11DeviceContext* ctx, int family, int eye);
void srvTurretHudEnd(ID3D11DeviceContext* ctx);
// Once a frame, render thread (vScreenFrameBoundary): settles a binding that did not restore.
void srvTurretHudFrameBoundary(ID3D11DeviceContext* ctx);
// DLL unload: the shader, the constants and the saved bindings released.
void srvTurretShutdown();

}  // namespace edvr
