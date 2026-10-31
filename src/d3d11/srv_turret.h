// The SRV turret view (issue 140): the camera and the HUD in the Scorpion's gunner view in VR. srv_turret_math.h says what is wrong, the eight TEMPORARY keys, the families and the
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
// PART 2, THE HUD. While the Scorpion's turret view is on and any of fix.srv_turret_hud_forward, fix.srv_turret_radar_forward, fix.srv_turret_panels_back, fix.srv_turret_radar_back and
// fix.srv_turret_hud_gap is above 0 the draw gate is open (srvTurretHudWantsDraws), forwardWithVerdict opens a scope for every draw, and a draw of one of the HUD families' shader pairs (srv_turret_math.h
// kFamilies: the two hologram families that draw the weapon panels from records 0.27 to 0.70 m out and the larger panels from 1.3 m and deeper, and the radar-named billboards and the
// target hologram sphere that draw the radar from 0.11 to 0.44 m) into an eye target, as a triangle list or strip, whose group (panels or radar) has a push above 0, a gap above 0 or
// its forward key above 0 (HUD forward for the panels, radar forward for the radar), is issued through that signature's private geometry shader, for EVERY issue the draw path makes of it (the game's own, the UI layer's, the interface
// depth's): the scope spans the whole function and puts the game's geometry stage back on every way out. The shader moves each vertex in 3D (srv_turret_math.h THE MOVE): every vertex
// forward by the group's forward key (the radar's own, so it can stay near field and be seen from above, not carried level with the head into a sliver), then, within 1 m of a fixed ANCHOR (the head's seated position when the view came on), along its ray from the anchor by the group's push, deeper (or
// the gap, nearer, for a group with no push). Each frame the frame tick (srvTurretFrameTick -> cacheGeometry) reads the host's head and eye matrices and tangents, steps the anchor and publishes each
// eye's frame (tangents with the jitter, the anchor and the two translations in the eye's space) for the log and as a fallback. That tick runs at the Present boundary and the host's
// beginFrame (the game's WaitGetPoses) publishes the pose of the frame about to be drawn: when that comes after the tick the tick's frames are a frame old, so the DRAW (drawEyeFrame) compares the host's frame sequence
// (nativeTemporalFrameSequence) with the one its frames were built from and, for a new one, rebuilds both eyes' frames once from that frame's pose and the tick's anchor (the judder was
// the HUD following the head a frame late). A draw of a group with none of its keys is the game's own and is not counted. How the moved vertices' depth is written is the seventh key's
// (fix.srv_turret_hud_depth: front, game or real, srv_turret_math.h DepthMode). The game sets a scissor rectangle for the radar's draws from where the radar was, and the moved geometry was cropped
// to it: with the eighth key at open (fix.srv_turret_hud_scissor, the default) a bound draw's scissor rectangles are the whole render target while it is issued and the game's are put back exactly
// after (srv_turret_hud_binding.h Scissor), refused draws and a boundary settle covering a restore that failed. The Scorpion's own hologram (vs 5559BD94, signature H) also needs the draw's vertex constants: its pixel shader
// divides an interpolant the vertex shader makes from the WORLD position by the fragment's view depth, so the geometry shader moves that interpolant too, by the camera's right, up and forward
// vectors (rows 277 to 279 of the draw's vertex constants, which the module binds at GS slot 1 for that signature and gives back). (The
// reticle's sprite, vs E508648660A352B2, is not a panel: an earlier build targeted it, at about 500 m, and moved nothing. The flight HUD's vectors, vs B7790CBFC6554097, are not in
// the pass.)
//
// WHAT THE LOG SHOWS (a flight in the turret view must tell "never ran" from "ran"):
//   "srv turret: the vehicle is the Scorpion|not the Scorpion (event E)"   each change of the latch (journal_watch.cpp, the worker thread): E is LoadGame, LaunchSRV, Music, DockSRV,
//     SRVDestroyed, Shutdown or NewJournal (another journal file).
//   "srv turret settings: ..."        the eight keys (the last two, the depth mode as front, game or real and the scissor as open or game, as text), at the first frame and at each change.
//   "srv turret anchor: latched at x,y,z m seated (view on|camera back changed|HUD forward changed|radar forward changed|recentre|head away)"      each time the anchor is taken (the head's seated position).
//     A frozen head pose (the same bits frame after frame) never latches it; "head away" is the safety net: the head stayed more than 35 cm from the anchor for 90 frames in a row.
//   "srv turret view: on|off at frame N (Status.json Flags 0x........, seat kind 0x.)"      each edge of the Scorpion's turret view (the view bit AND the latch).
//   "srv turret view: not the Scorpion at frame N (Status.json Flags ..., seat kind ...): ..."      the view bit came on in an SRV the latch does not call the Scorpion: nothing is changed.
//   "srv turret camera: back X m published (serial N)" / "srv turret camera: back cleared (serial N)"      each change of what alignTick published.
//   "srv turret hud: precompiled geometry shader ... created|FAILED" (shader_swap.cpp), once for each signature, at the first draw that asks for them.
//   "srv turret hud: first draw of family F ... the G group: a vertex A to B m from the anchor moves D m deeper|nearer|does not move radially, in 3D about the anchor (x,y,z m seated),
//     after the group's HUD moves H m forward ..." once for each family (deeper: the push-back, nearer: the pull); "srv turret hud: refused a draw (reason) ..." once per reason.
//   "srv turret hud: panels: gap=-1.10 m dMin=0.02 dMax=1.00; radar: ...; anchor=(x,y,z) head_offset=N cm T=(x,y,z) Tradar=(x,y,z) ipd_elite=.. P00[L,R]=.. draws[L,R]=.. bound=.. pose refreshed at draw: N of M frames
//     refused[...]=.. families[1989E6D3,...]=n,..." every 10 s while the Scorpion's turret view was on at any point of the window: each group's constants (gap and the window of anchor distances, or
//     "off"; a gap of 0 is the translation alone), the anchor, how far the head is from it, eye 0's translation for the panels (T) and for the radar (Tradar), the frames in the window (M) whose pose the draw
//     found newer than the one the frame tick built from (N; near M when the host's beginFrame runs after the boundary tick, near 0 when it runs before it), the matched draws of each family
//     (names, then counts, in kFamilies' order).
//   THE GPU INSTRUMENT (whether the shader RAN, and what it wrote): once a second per eye and family, while the pass is bound, one matched draw (the frame's draws of the family
//   rotate) is issued through a stream-output twin of its signature's shader (the same bytecode and constants, still rasterized) with a pipeline-statistics query and a
//   stream-output-statistics query around the scope (Begin right after the bind, End right before the restore). Read back without a stall (DONOTFLUSH, a staging copy mapped
//   DO_NOT_WAIT) on later frames, given up after 8:
//   "srv turret hud stats: eye=E issues=N ia_prims=.. vs=.. gs=.. gs_prims=.. c_inv=.. c_prims=.. ps=.. (lost N) draw=K family=F"   gs=0 with ia_prims>0: the shader never ran.
//   "srv turret hud capture: family=F eye=E draw=K so_prims=.. vertices=.. in_window=.. w=[..] ndc_x=[..] ndc_y=[..] dist=[a,b]->[c,d] a=(x,y,z) zfix=N gap=.."   the vertices the shader wrote
//   (in_window: how many it moved, read from the shader's own `pre` output, the clip position as the game made it): with a push the weapon panels' and the radar's draws read in_window
//   above 0 (anchor distance before 0.1 to 0.7 m, after that plus the push) and the larger panels' in_window 0 at 1.3 m and more (the pull: the other way round); with only the HUD-forward
//   key every vertex reads as moved. zfix is how many of the moved vertices took the depth fallback (the game's NDC z was not a valid reversed-Z depth: the vertex was behind the eye or
//   inside its near plane). gap= is the signed gap the family's group ran with. The capture holds the first kCaptureBufferBytes of a draw's output, so a long draw is a sample.
//   "srv turret hud disc: family=DF3503CD eye=E draw=K issues=N samples_passed=S alpha=A intensity=I (cb2[11].w, cb1[90].y)"   for the radar disc's sampled scope only: an occlusion query around
//   it (S: the samples that passed the depth and stencil tests, over all the scope's issues) and the two pixel-shader registers its alpha and colour scale are made of, copied to a staging buffer
//   at the end of the scope (n/a when a copy was refused). S = 0 with the pixel shader running (the statistics line's ps) is a depth or stencil test removing the disc; S above 0 with A about 0 is
//   the game fading it.
//   "srv turret hud state: family=F eye=E issue=K target=game|other WxH format=N depth_view=yes|no | depth: .. | stencil: .. | blend rt0: .."   each family's first scope, at each of its first eight
//   issues (the game's own, the UI layer's, the interface depth's): the target as 'game' (the one bound when the scope began) or 'other' (the UI layer's), the depth-stencil state and the blend state
//   of render target 0, as D3D11 enumeration numbers, then the rasterizer state (ScissorEnable, the number of scissor rectangles and the first, DepthClipEnable, CullMode,
//   FrontCounterClockwise) and the first viewport. Taken after the scissor was opened, so a scissored issue shows the rectangles it ran with.
//   "srv turret hud: family F draws with the scissor test on: N rect(s), first [l,t,r,b], target WxH; the scissor is opened to the whole target|kept as the game set it for its draws"   once for each
//   family found drawing with ScissorEnable (the game's first rectangle, and the extent it is opened to). The 10 s line's "scissor opened N" counts the draws whose rectangles were opened.
//   "srv turret hud: the game's scissor rectangles could not be put back after an issue; ..."   a restore that faulted (the boundary settles it; draws are refused until then).
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

// vscreen.cpp, at each place that issues the matched draw (the game's own issue and pureDrawReissue's re-issues): counts it while the sampled scope, or a family's first scope, is open
// (srvTurretHudNoteIssue, in srv_turret.cpp, which also says the state each of the first scope's issues ran with). One relaxed load otherwise.
void srvTurretHudNoteIssue(ID3D11DeviceContext* ctx);
inline void srvTurretHudCountIssue(ID3D11DeviceContext* ctx) {
    if (detail::g_srvTurretHudCounting.load(std::memory_order_relaxed)) srvTurretHudNoteIssue(ctx);
}

// True while the Scorpion's turret view is on (VR) and any of fix.srv_turret_hud_forward, fix.srv_turret_radar_forward, fix.srv_turret_panels_back, fix.srv_turret_radar_back and
// fix.srv_turret_hud_gap is above 0.
// Also a term of the draw gate (vscreen.cpp drawGateSubscribed).
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
