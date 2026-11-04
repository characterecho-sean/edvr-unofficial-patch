// The SRV turret view (issue 140): the camera and the HUD in the Scorpion's gunner view in VR. srv_turret_math.h says what is wrong, the ten experimental keys, the families and the
// maths; srv_turret_hud_shader.h and srv_turret_hud_binding.h are the private geometry shaders and their binding (tools/srv_turret_hud_test runs them on WARP);
// journal_vehicle_latch.h is the Scorpion latch; srv_radar_anchor.h is the hooks that move the radar's schematic in the game; srv_turret.cpp is this module.
//
// THE SCORPION ONLY. Status.json's turret-view bit is set in the Scarab too, whose turret view was fine, so both parts act only while the turret view is on AND the journal says the
// vehicle is the Scorpion (journalSrvScorpion: LoadGame's Ship or LaunchSRV's SRVType is combat_multicrew_srv_01; unknown, a training scenario included, is "not the Scorpion"; the
// main menu's music, a shutdown, DockSRV, SRVDestroyed and a load or launch of anything else clear it).
//
// PART 1, THE CAMERA. srvTurretFrameTick (the frame thread, at the frame boundary just before alignTick) reads Status.json's turret-view bit through the journal
// watcher and the key; alignTick adds srvTurretBackSeatedZ() to the seated z of the eye alignment it already publishes (one publisher, so the serials stay coherent) and
// tells srvTurretNotePublished what it published. Applied at once, with no ramp: the game itself cuts the camera on a view switch.
//
// PART 2, THE HUD. While the Scorpion's turret view is on and any of experimental.srv_turret_hud_forward, experimental.srv_turret_radar_forward, experimental.srv_turret_panels_back, experimental.srv_turret_radar_back,
// experimental.srv_turret_hud_gap and experimental.srv_turret_systems_near is above 0 the draw gate is open (srvTurretHudWantsDraws), forwardWithVerdict opens a scope for every draw, and a draw of one of the HUD families' shader pairs (srv_turret_math.h
// kFamilies: the two hologram families that draw the weapon panels from records 0.27 to 0.70 m out and the larger hologram panels from 1 m and deeper, and the radar-named billboards and the
// target hologram sphere that draw the radar from 0.11 to 0.44 m) into an eye target, as a triangle list or strip, whose group (panels or radar) has a push above 0, a gap above 0,
// its forward key above 0 (HUD forward for the panels, radar forward for the radar), is issued through that signature's private geometry shader, for EVERY issue the draw path makes of it (the game's own, the UI layer's, the interface
// depth's): the scope spans the whole function and puts the game's geometry stage back on every way out. experimental.srv_turret_systems_near, on the panels group only, writes a second radial window (vertices past 1 m and under 4 m). That window is not confirmed as the power and systems panel: a flight at 40 cm did not move it. The Escape menu is a different mesh and is not bound. The shader moves each vertex in 3D (srv_turret_math.h THE MOVE): every vertex
// forward by the group's forward key (the radar's own, so it can stay near field and be seen from above, not carried level with the head into a sliver), then, within 1 m of a fixed ANCHOR (the head's seated position when the view came on), along its ray from the anchor by the group's push, deeper (or
// the gap, nearer, for a group with no push). Each frame the frame tick (srvTurretFrameTick -> cacheGeometry) reads the host's head and eye matrices and tangents, steps the anchor and publishes each
// eye's frame (tangents with the jitter, the anchor and the two translations in the eye's space) for the log and as a fallback. That tick runs at the Present boundary and the host's
// beginFrame (the game's WaitGetPoses) publishes the pose of the frame about to be drawn: when that comes after the tick the tick's frames are a frame old, so the DRAW (drawEyeFrame) compares the host's frame sequence
// (nativeTemporalFrameSequence) with the one its frames were built from and, for a new one, rebuilds both eyes' frames once from that frame's pose and the tick's anchor (the judder was
// the HUD following the head a frame late). A draw of a group with none of its keys is the game's own and is not counted. How the moved vertices' depth is written is the seventh key's
// (experimental.srv_turret_hud_depth: front, game or real, srv_turret_math.h DepthMode). The game sets a scissor rectangle for the radar's draws from where the radar was, and the moved geometry was cropped
// to it: with the eighth key at open (experimental.srv_turret_hud_scissor, the default) a bound draw's scissor rectangles are the whole render target while it is issued and the game's are put back exactly
// after (srv_turret_hud_binding.h Scissor), refused draws and a boundary settle covering a restore that failed. The Scorpion's own hologram (vs 5559BD94, signature H) also needs the draw's vertex constants: its pixel shader
// divides an interpolant the vertex shader makes from the WORLD position by the fragment's view depth, so the geometry shader moves that interpolant too, by the camera's right, up and forward
// vectors (rows 277 to 279 of the draw's vertex constants, which the module binds at GS slot 1 for that signature and gives back). (The
// reticle's sprite, vs E508648660A352B2, is not a panel: an earlier build targeted it, at about 500 m, and moved nothing. The flight HUD's vectors, vs B7790CBFC6554097, are not in
// the pass.)
//
// THE RADAR IN THE GAME (experimental.srv_turret_radar_anchor: camera, on, reversed or off; srv_radar_anchor.h). The radar disc is a picture the game draws of its terrain schematic (vs 4EDE4BC9,
// and vs 5B0068AF for the Scorpion icon) from the live camera, shown by the hologram billboard vs 5559BD94 (family 7, ps EA02FAC2). At the stock turret camera the schematic's volume lies about 0.44 m
// behind the eye, so the game's own visibility gate leaves the picture empty. The key picks how the radar is moved in the game itself, and the frame tick (anchorTick) arms the hook its mode needs:
//   * camera (the default): a hook on the schematic slot's update (0x36BF3C0) gives the radar's slots the scene camera moved back by the radar forward key along the turret's forward u. The radar's own
//     picture billboards (family 7 on a texture the frame's radar writers drew into: srvTurretRadarWriterDraw, billboardKind) then go through the regular radar path of part 2, moved forward along the seat
//     with the rest of the radar; every other 5559BD94 draw is the game's own (cameraLeaves).
//   * on, reversed: a hook on the radar's placement setter (0x38563A0) adds the radar forward key along u (reversed: against it) to the placement, or (0, 0, s * F) in the anchor's axes when there is no u. While the
//     last setter call left our offset in the placement the draw path leaves the two 5559BD94 billboard families to the game (anchorLeaves), so the picture the game now frames is not moved a second time.
//   * off: the game's own radar.
// u is the turret's forward from the live camera's right, up and forward (rows 277 to 279 of the scene camera block): read on the CPU when the game writes the block (the Unmap tee,
// srvTurretNoteCameraWrite, kept by buffer with the host frame), else copied to a staging ring and read back at the frame boundary without stalling (anchorDirSample, anchorDirBoundary).
//
// WHAT THE LOG SHOWS (a flight in the turret view must tell "never ran" from "ran"):
//   "srv turret: the vehicle is the Scorpion|not the Scorpion (event E)"   each change of the latch (journal_watch.cpp, the worker thread): E is LoadGame, LaunchSRV, Music, DockSRV,
//     SRVDestroyed, Shutdown or NewJournal (another journal file).
//   "srv turret settings: ..."        the ten keys (the depth mode as front, game or real, the scissor as open or game, the radar anchor as on, reversed, off or camera, and the systems panel's pull in centimetres), at the first frame and at each change.
//   "srv turret anchor: latched at x,y,z m seated (view on|camera back changed|HUD forward changed|radar forward changed|recentre|head away)"      each time the anchor is taken (the head's seated position).
//     A frozen head pose (the same bits frame after frame) never latches it; "head away" is the safety net: the head stayed more than 35 cm from the anchor for 90 frames in a row.
//   "srv turret view: on|off at frame N (Status.json Flags 0x........, seat kind 0x.)"      each edge of the Scorpion's turret view (the view bit AND the latch).
//   "srv turret view: not the Scorpion at frame N (Status.json Flags ..., seat kind ...): ..."      the view bit came on in an SRV the latch does not call the Scorpion: nothing is changed.
//   "srv turret camera: back X m published (serial N)" / "srv turret camera: back cleared (serial N)"      each change of what alignTick published.
//   "srv turret hud: precompiled geometry shader ... created|FAILED" (shader_swap.cpp), once for each signature, at the first draw that asks for them.
//   "srv turret hud: first draw of family F ... the G group: a vertex A to B m from the anchor moves D m deeper|nearer|does not move radially, in 3D about the anchor (x,y,z m seated),
//     after the group's HUD moves H m forward ..." once for each family (deeper: the push-back, nearer: the pull); "srv turret hud: refused a draw (reason) ..." once per reason.
//   "srv turret hud: panels: gap=-1.10 m dMin=0.02 dMax=1.00; radar: ...; anchor=(x,y,z) head_offset=N cm T=(x,y,z) Tradar=(x,y,z) ipd_elite=.. P00[L,R]=.. draws[L,R]=.. bound=.. scissor opened N pose refreshed at draw: N of M frames
//     refused[...]=.. families[1989E6D3,...]=n,..." every 10 s while the Scorpion's turret view was on at any point of the window: each group's constants (gap and the window of anchor distances, or
//     "off"; a gap of 0 is the translation alone), the anchor, how far the head is from it, eye 0's translation for the panels (T) and for the radar (Tradar), the draws of each eye and the ones bound, the draws whose
//     scissor was opened, the frames in the window (M) whose pose the draw found newer than the one the frame tick built from (N; near M when the host's beginFrame runs after the boundary tick, near 0 when
//     it runs before it), the refusals by reason, the matched draws of each family (names, then counts, in kFamilies' order).
//   "srv turret hud state: family=F eye=E issue=K target=game|other WxH format=N depth_view=yes|no | depth: .. | stencil: .. | blend rt0: .."   each family's first scope, at each of its first eight
//   issues (the game's own, the UI layer's, the interface depth's): the target as 'game' (the one bound when the scope began) or 'other' (the UI layer's), the depth-stencil state and the blend state
//   of render target 0, as D3D11 enumeration numbers, then the rasterizer state (ScissorEnable, the number of scissor rectangles and the first, DepthClipEnable, CullMode,
//   FrontCounterClockwise) and the first viewport. Taken after the scissor was opened, so a scissored issue shows the rectangles it ran with.
//   "srv turret hud: family F draws with the scissor test on: N rect(s), first [l,t,r,b], target WxH; the scissor is opened to the whole target|kept as the game set it for its draws"   once for each
//   family found drawing with ScissorEnable (the game's first rectangle, and the extent it is opened to). The 10 s line's "scissor opened N" counts the draws whose rectangles were opened.
//   "srv turret hud: the game's scissor rectangles could not be put back after an issue; ..."   a restore that faulted (the boundary settles it; draws are refused until then).
//   "srv turret hud: the game's geometry stage could not be put back after an issue; ..." and "srv turret hud: the game's geometry stage and constant slot were put back at the frame boundary."   the same for the
//     binding: a restore that faulted, and the boundary settle that covered it.
//   Camera mode (srv_radar_anchor.cpp, said by the frame tick, never by the hook):
//   "srv turret radar camera: slot hook armed: ..." or "... slot hook stood down: <why>; nothing was patched"   once, the first time the view is on in camera mode;
//   "srv turret radar camera: moved back X m for slot 0x.. (index N, KIND (mode M), list L (K)), direction from the turret (CPU camera sample)|the camera's own forward, u (x,y,z)"   once for each kind (terrain,
//     icon) and list (A, B: the two eyes);
//   "srv turret radar camera: 10 s: slot calls N, radar slots M (terrain A .., terrain B .., icon A .., icon B .., other ..), moved N (direction turret .., camera forward ..), faults N; hook thread T,
//     camera writes on thread U[ -- NO radar slot in these 10 s: ...| -- the slot hook is not armed (...)]"   every 10 s in which camera mode was on with the view.
//   On and reversed (the setter hook):
//   "srv turret radar anchor: setter hook armed: EliteDangerous64.exe+0x38563A0 (...) at 0x.., stolen=N bytes, prologue 18/18 bytes verified, relay at 0x.." or "... setter hook stood down: <why>; nothing was
//     patched"   once, the first time the view is on with the key at on or reversed;
//   "srv turret radar anchor: placement moved at frame F: the game's (x,y,z) + (dx,dy,dz) -> (x,y,z) in the anchor's axes (key K), caller EliteDangerous64.exe+0x.., obj 0x..; the 5559BD94 billboards are left to the game"
//     each stay's first add;
//   "srv turret radar anchor: view off: moved in N calls[ -- NEVER RAN: ...| -- hook not armed: <why>| -- not asked: ...]"   the view's end.
#pragma once

#include "srv_turret_math.h"

#include <atomic>
#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

namespace detail {
// Asked per draw with no call: the build has no /GL to fold a cross-TU getter (target_sharp.h's reason).
extern std::atomic<bool> g_srvTurretHudWanted;
// True while a family's first scope is open and its issues say their state (srv_turret.cpp noteBegin and noteEnd).
extern std::atomic<bool> g_srvTurretHudCounting;
// The turret's direction for the radar's move: true while the Scorpion's turret view is on and the hook of the radar anchor mode is armed (the slot hook for camera, the setter hook for on and reversed; the frame tick sets it).
extern std::atomic<bool> g_srvTurretCameraRowsWanted;
// Camera mode's match of the radar's own picture billboard: true while the view is on in camera mode (the frame tick sets it).
extern std::atomic<bool> g_srvTurretRadarWritersOn;
// The early edge (srv_turret_sheet_latch.h): true while the Scorpion is in its SRV seat and the turret view is off. The draw gate is open for it, the gunner sheet's draws are timed
// (g_srvTurretSheetSeenMs, GetTickCount64) and every other draw is the game's.
extern std::atomic<bool> g_srvTurretWatchOnly;
extern std::atomic<uint64_t> g_srvTurretSheetSeenMs;
}  // namespace detail
inline bool srvTurretWatchOnly() { return detail::g_srvTurretWatchOnly.load(std::memory_order_relaxed); }

// The radar's picture writers, for camera mode's match of the radar's own picture billboard (srv_turret.cpp billboardKind). vscreen.cpp forwardWithVerdict, every owner-context draw while camera mode is on (one
// relaxed load otherwise): a draw of the terrain schematic (vs 4EDE4BC9) or of the Scorpion icon (vs 5B0068AF into a 256x256 target) records its render targets for the frame, which a radar billboard's t4 is
// matched against.
inline bool srvTurretRadarWritersOn() { return detail::g_srvTurretRadarWritersOn.load(std::memory_order_relaxed); }
void srvTurretRadarWriterDraw(ID3D11DeviceContext* ctx, uint64_t vs);

// The live camera's right, up and forward (rows 277..279 of the 5376-byte scene camera block) read on the CPU when the game writes them, for the turret's direction.
// vscreen.cpp hookedUnmap, the scene camera block's branch, before the real Unmap, while wanted (one relaxed load otherwise); srvTurretHudBegin looks the eye-0 draw's VS b1 buffer up in
// what this kept, so u needs no GPU readback. Owner context's thread.
inline bool srvTurretWantsCameraRows() { return detail::g_srvTurretCameraRowsWanted.load(std::memory_order_relaxed); }
void srvTurretNoteCameraWrite(const void* res, const void* data, uint32_t bytes);

// vscreen.cpp, at each place that issues the matched draw (the game's own issue and pureDrawReissue's re-issues): while a family's first scope is open, says the state each of its issues
// ran with (srvTurretHudNoteIssue, in srv_turret.cpp). One relaxed load otherwise.
void srvTurretHudNoteIssue(ID3D11DeviceContext* ctx);
inline void srvTurretHudCountIssue(ID3D11DeviceContext* ctx) {
    if (detail::g_srvTurretHudCounting.load(std::memory_order_relaxed)) srvTurretHudNoteIssue(ctx);
}

// True while the Scorpion's turret view is on (VR) and any of experimental.srv_turret_hud_forward, experimental.srv_turret_radar_forward, experimental.srv_turret_panels_back, experimental.srv_turret_radar_back,
// experimental.srv_turret_hud_gap and experimental.srv_turret_systems_near is above 0.
// Also a term of the draw gate (vscreen.cpp drawGateHolders).
inline bool srvTurretHudWantsDraws() { return detail::g_srvTurretHudWanted.load(std::memory_order_relaxed); }

// Part 1 and the state both parts read. Frame thread, once a frame, nativeOnFootFrameBoundary just before alignTick.
void srvTurretFrameTick(uint32_t frameNo);
// The seated z alignTick adds to its eye alignment: -D metres while the camera is moved back, else exactly 0. Frame thread.
double srvTurretBackSeatedZ();
// True while the Scorpion's turret view is on (the tick's `on`: Status.json's bit, the journal's Scorpion, VR). Frame thread. The entry fade (srv_turret_entry_fade.h) starts its hold on this edge.
bool srvTurretViewOn();
// alignTick, right after it published an alignment: the z it added and the serial it published under. Said in the log when it changed. Frame thread.
void srvTurretNotePublished(double seatedZ, uint32_t serial);

// Part 2, the draw path (vscreen.cpp, the render thread). Begin: the game's bindings are its own; family is the draw's shader pair's family (srv_turret_math.h familyOf), eye
// the draw's eye (0 left, 1 right, -1 unknown). True when the family's private shader is bound for the issues that follow; End puts the game's geometry stage and constant
// slot back and is called only after a true Begin.
bool srvTurretHudBegin(ID3D11DeviceContext* ctx, int family, int eye);
// The gunner camera's curved panel, before the HUD-family test. True only for its 2016-index sheet and its 288-index cards while experimental.srv_turret_systems_near is above 0.
bool srvTurretGunnerBegin(ID3D11DeviceContext* ctx, uint64_t vs, char kind, uint32_t indexCount, int eye);
void srvTurretHudEnd(ID3D11DeviceContext* ctx);
// Once a frame, render thread (vScreenFrameBoundary): settles a binding that did not restore.
void srvTurretHudFrameBoundary(ID3D11DeviceContext* ctx);
// DLL unload: the shader, the constants and the saved bindings released.
void srvTurretShutdown();

}  // namespace edvr
