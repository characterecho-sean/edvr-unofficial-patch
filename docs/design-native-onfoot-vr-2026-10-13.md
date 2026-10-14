# Native on-foot VR (replacing Explorer Cam) — design, 2026-10-13

## Status

- **State:** F0-F12 FLOWN (Epic; journal). F13, F14 BUILT (2484625d), F15 BUILT (202d39b9; green), NOT FLOWN.
  Branch `claude/native-onfoot-vr-completion-219ffa`; not pushed.
- **Origin:** a user's handoff. A Cinema-to-HMD swap on disembark gave native
  stereo on foot; the HUD, radar, left panel and inventory were missing.
- **Build:** Sean's installs hash to the handoff's exe (332841).
- **Verified offline:** the setter is vtable 0x52E9608 slot 5 = 0x28723C0. The
  selector is embedded in its owner at +0x60. No direct callers.
- **Requester gate (build 332841):** in 0x28873C0 the cinema partner mode is kept only
  if `call [rax+0xb0]` returns non-zero AND the byte at EliteDangerous64.exe+0x5E82B3E
  is non-zero. At 0 the requester falls back to the base HMD mode (0x2887B06 re-reads
  it), so the game requests HMD itself. The byte is in .data (initial 01), has one reader (0x2887AF9) and no
  writer. Its neighbours 0x5E82B3C and 0x5E82B3F have other readers: never write them.
- **F1 results (Epic):** avatar view masks of the local head parts non-zero; the
  fade global read -1 (auto) throughout, so nothing had forced the body opaque;
  neck: the stance drop is real, the walk bob is not (pilot); RemLok on foot is a
  different draw (see Ruled out: that identification was wrong).
- **Key finding (F8, corrected):** the game does test the selector mode in {3, 4} at
  about 50 sites; "all readers treat 3..6 alike" held only for settings+0x3694/0x3698.
  The HUD is held back by the first-person pass's enable (the requester at 0x2887A15
  reads the override dword at 0x5E82B48 and sets the FP node's enable byte).
- **F3 findings (flown):** every cinema HUD draw comes through ONE stack; the path from
  0x2868C30 down is unconditional (static RE: scratchpad re_hudstack). The first-person
  AMC (mode 2) has EVASuit_Arms, EVASuit_Hands and FirstPersonSkeleton; its masks are
  0x60BC0000C05E. The 3P body (mode 3) has 0x60BD8000C05E.
- **Open hypotheses:**
  - F4 (flown): every group enable byte reads 1 in cinema and native, and no view drops
    out; the HUD items are never queued. The data-gate candidates are not the answer.
  - H7 (ruled out, F5): the HUD view nodes are built once at startup in both modes, and the
    only per-mode pass difference, W+0x38, has no reader.
  - H8 (ruled out, F6): the stereo predicate and the eye-index query.
  - H10 (F8, flown): the override brings the FP node back, but only the reticle shows.
    F11: the HUD shows head-locked with the creation patch alone (the placement patch is wrong).
  - F15 (BUILT, E1, not flown): the 3P body is drawn but hidden behind or inside the FP body,
    and backface-culled from inside. E1 zeroes the FP masks. Discriminator: the 3P masks stay
    non-zero, and the body shows once the FP masks are 0.
- **F9-F13 instruments (TEMPORARY):** patches `fix.native_on_foot_panels`, `_hud_mesh` and
  `_hud_mesh_world` (off); probes P1-P3; `fix.native_on_foot_hud_follow_order` (A/B) and
  `fix.native_on_foot_visor_up` (RemLok test). The distance, size, follow and smoothing are user settings.
- **OPEN, RemLok on foot:** the on-foot visor overlay is not yet identified. It
  needs a skip-probe or a diff census that excludes the tonemap PS
  0x99C21CEB7A699821 (tonemap_admit.h:39).
- **Ruled out:** the cinema request is always 5 (see the end of this doc).
- **Temporary keys (all go when the arc picks one):** `fix.native_on_foot`, `_method`, `_hud`,
  `_panels`, `_hud_mesh`, `_hud_mesh_world`, `_hud_follow_order`, `_visor_up`, `_reticle_probe`
  and `_hide_fp_body` (F15), on the F8 page "Native on foot". All default off and live.
- **F2 changes (c884e218):** gate: the setter never substitutes; EDVR holds the byte
  at 0 while the key is on and a game session is live (identity and byte checked
  first). Native body: while native stereo is on foot, the fade global goes -1 to 0
  and the local head parts are zeroed, through Explorer Cam's code and its own
  guard. RemLok: unchanged from 0a99c094 (the on-foot matcher was removed).
- **Log lines:** the F2 gate and body lines and the F8 override and census lines are in
  the journal; the F1 census lines are unchanged.
- **Install:** Epic only (Sean: Frontier carries another feature test). `edvr.ini`
  is not touched unless `--ini` says so.
- **Next:** F15 on Epic: `fix.native_on_foot_hide_fp_body` on, then the 3P body in the eye views.
  If nothing appears, the read-only visibility-job probe at 0x3DA2C80. Still open from F13:
  head against body follow, smoothing, size, distance, the visor test (RemLok), the reticle depth.

## What "finished" means

Explorer Cam goes. Pressing nothing, the commander leaves the ship in native
stereo, with a usable HUD, inventory, weapons and menus, and returns aboard
unchanged. Six workstreams, in dependency order:

| # | Work | Size | Risk |
|---|---|---|---|
| W1 | Activation: hook the setter's vtable slot, swap 5→3 only when admitted | small (CodeHook pattern, ~300 lines + rig) | low; addresses verified |
| W2 | EDVR frame path: re-key on-foot gates from "journal on foot" to "eyes are stereo" | medium; ~6 gates | medium; regressions on the panel path |
| W3 | HUD: port the on-foot UI elements into projected 3D space (three stages, below) | **large**; the bulk of the arc | high; RE + several flights |
| W4 | Contexts: inventory cursor, radial menus, maps, contacts, SRV, elevators, Apex, loading, intro video, deaths | medium; mostly per-context admission | medium |
| W5 | Remove Explorer Cam (6 files, ~5.7k lines, 8 keys, F5 hotkey, F8 page) | small | none, once W1-W4 fly; keys quoted and asked first |
| W6 | Body and view: show a full body like Explorer Cam's, with a ~200 ms damper on the view (not body yaw) | medium | medium; the neck census decides the damper's shape |

### W1 — activation

Install pattern exactly as `skin_entity_hook.cpp:22-26` / `explorer_cam.cpp`:
PE timestamp + image size identity (`explorer_cam.cpp:410-423`), SEH-guarded
prologue compare (`sehCheckBytes`), refuse-and-log on mismatch. Hook the
vtable slot (`vtable_hook.h:115-146`) rather than the body: the entry is a
0x21-byte .pdata stub chained into two more ranges, so a 5-byte detour is
fine but the slot is simpler. The handoff's rules carry over: forward once,
same object, caller thread; never call the setter ourselves; never write
+0x20/+0x3694/+0x3698; unknown identity → passthrough.

Admission (the handoff's own warning: an incoming 5 alone is not an on-foot
detector). Swap only when the journal's `Disembark` arrived, or Status.json
says OnFoot, within the last N s; never for the intro video, galaxy map or
loading screens. W4 grows this list; F0 measures what the natural 5-requests
actually are.

Note from the disassembly: the setter dispatches by mode class (jump table
0x28724EC: 1 → 0x2872467, 2 → 0x287247E, 3..6 → 0x287248C), so 5→3 stays
inside the same backend class. That is consistent with the handoff seeing no
resource transition fault.

### W2 — EDVR's on-foot gates

From the code map, these key on "on foot" and assume a mono panel:
`vr_world_route.h:4-17` (route owns the frame on foot);
`native_temporal.cpp:384,453` (eye shift suppressed, screen draw taken);
`ui_layer.cpp:2366-2372` (journal on foot + screen-depth draws);
`temporal_pass.cpp:1545-1553` (footSplitNow); `native_frame.cpp:307-311`
(deferred pacing on foot); `vscreen.cpp:988-996,5819` (16:9 panel shape);
`transition_flash_eye_base.cpp:285-315`. Each needs a "native stereo on foot"
branch that behaves like the cockpit. The F2 skin join is NOT coupled to
Explorer Cam (`engine_velocity.cpp:2496-2502`); it stays.

RemLok, F9 (2026-10-14): in native stereo the RemLok lines are ALWAYS drawn, even in a
breathable station. F10 census_skip found no separate overlay draw: the on-foot lines are not
the cockpit overlay, and they likely sit inside a post pass (see the F10 journal). No code
change for RemLok in this build. The helmet-state handling may be another cinema-only path.

RemLok (F0 log, 10:50-10:54): in native stereo on foot the helmet lines show
the replicated nasal-line bug that the cockpit fix solved (`fix.remlok_lines =
outer`, angle 46, `remlok_fix.cpp`, matched by shape N:3 i=1 no-DSV PS0
1024x512 in `vscreen.cpp` ~2467). The startup line says `remlok lines: outer`,
but the GPU census reads `RemLok overlay -` in every 30 s window, so the match
never fired on foot. Hypotheses: (a) the on-foot overlay is a different draw
(texture size or shape); (b) the eye-draw verdict path is not reached on foot
in native mode, because an earlier on-foot gate turns it away. Discriminator:
a `dump_draws` pair on foot in native mode, helmet down and helmet up, diffed
with `tools\diff_draw_census.py`. No code change for this in F1.

### W3 — the HUD: port the on-foot UI into projected 3D space

Goal (Sean, 2026-10-13): the on-foot UI elements (radar, left panel,
inventory, radial menus, reticle, weapon label, prompts) placed in the
headset as projected 3D surfaces, not one flat HUD quad. Three stages:

1. **Does the game already place on-foot UI in 3D behind a gate?** The
   handoff restored the reticle and weapon label in both eyes in a later
   experiment, and the game re-requests cinema every frame in native mode (H4).
   F1's caller RVA locates the requester. Question: is there a 3D placement
   path, behind a gate, that the requester can reach?
2. **Element inventory from a `dump_draws` pair (cinema against native).** Per
   element: its draws, its source target and its screen rect, and whether the
   elements separate cleanly. Done offline from the dumps.
3. **Per-element placement and interaction.** Each element is body-locked,
   world-locked or head-lazy, at its own depth, composited by EDVR. Interaction
   maps a cursor or pointer onto the placed surfaces for inventory and radial
   menus. Placement choices are Sean's call after stage 2.

The machinery to composite a 2D layer into the eyes already exists (the UI
layer path, `ui_layer_math.h:8-13`, `kScreen` family at :583-587). The
handoff's runtime comparison saw the HUD resources still produced in mode 3,
with their usual PS-SRV composite consumers absent.

Fallback if stage 1 finds that the game skips the HUD render entirely in mode
3: find what skips it, starting at 0x28540B0 (mode 3..6 AND settings+0x2AE0==1
predicate, no direct callers).

### W6 — body and view

Sean: show a full body like Explorer Cam's, and damp the view. F1's census
shows the third-person body exists in native mode with zero view masks (the
F0 flight, 10:50:56: the local AMC with 15 of 54 parts instanced, including
EVASuit_Torso, EVASuit_Arms and EVASuit_Legs). The neck census (F1) says
whether the view rides the animated neck. The head joint and the camera are
not yet in one space, so the census logs both with their spaces named.
Plan: a ~200 ms damper on the view that follows the neck bob, not body yaw.
Read-only until W6 is designed.

## Proposed first flight F0 (one flight, several hypotheses)

Build on the branch, behind one temporary key `fix.native_on_foot`
(default off; listed above; removed when W1 ships as behaviour):

- the setter hook, logging every call (`native on foot: incoming=5
  effective=3 source_after=3 onfoot=… disembark_age_ms=…`) and refusing
  loudly on identity mismatch, so "never ran" is distinguishable from
  "ran and passed through";
- a once-a-second line from the runtime: submit shape per eye
  (stereo vs panel), so H3 is measured, not guessed;
- a counter per W2 gate showing which engaged while eyes were stereo.

Flight: VR in the ship, key on, disembark at a station, walk, draw a weapon,
open inventory, a radial menu and the map, board again. Then key off,
disembark again (cinema) and take `dump_draws` on foot in both modes for the
W3 census.

Decides: H3 (gate counters); W4's admission list (every natural incoming
mode and its context); W3 step 1-2 (dump comparison, offline).

## Ruled out

- ruled out: a mode branch that hides the HUD in 3 but shows it in 5, because
  all 17 static readers of settings+0x3694/0x3698 test the 3..6 range, never 3
  vs 5 (re_modes scan, 2026-10-13). Indirect readers are not covered.
- ruled out: the cinema request is always 5, because flight F0 (10:15:48,
  Epic, Pimax OpenXR) logged incoming=6 at disembark, with the VR world route
  engaging 2 s later. This rig's HMD mode is 4, and the main menu also runs in
  4 (10:14:01, incoming=4). The pairs are 5/3 and 6/4.

- ruled out: the on-foot RemLok overlay is the 3-vertex draw with PS 0x99C21CEB7A699821, because that is the tonemap (HDR-to-display) pass (tonemap_admit.h:39); the F1 census candidate was misidentified, caught before flight.

- ruled out: the HUD is gated on the cinema re-request loop (H4), because under the gate method the game requested HMD itself with no re-requests and still drew no HUD (F2 13:09:43-13:12:23).
- ruled out: EDVR's UI layer hides the HUD (H5), because fix.ui_quality was off from 13:25:06 through a native window 13:25:44-13:27:03, and still no HUD.
- ruled out: the HUD is gated on the stereo leaf predicate 0x28540B0, because it returns 1 in cinema (mode 6) too, from the same callers (F6).
- ruled out: a per-item eye-index match drops HUD items in stereo, because the eye-index query's only caller runs once per mode change, not per frame (F6).
- ruled out: the 0x1A36453 world-placement patch is needed, because with it off the HUD shows head-locked in camera space (F11).
- ruled out: vs:53211E8C072CD02E is the on-foot RemLok line's draw, because skipping its draws did not remove the lines (F10; flight 2026-10-14 08:20, 24888 draws dropped, the lines stayed).
- ruled out: vs:4AEC439CEC7FFDCE is the on-foot RemLok line's draw, because skipping its draws did not remove the lines (F10; flight 2026-10-14 08:20, 24888 draws dropped, the lines stayed).
- ruled out: vs:5E417E9DF2E7F9E6 is the on-foot RemLok line's draw, because skipping its draws did not remove the lines (F10; it also drives the smoke).
- ruled out: vs:3064D7F445192FDD is the on-foot RemLok line's draw, because skipping its draws did not remove the lines (F10).
- ruled out: vs:B75A6FF2CA9FA5D6 is the on-foot RemLok line's draw, because skipping its draws did not remove the lines (F10).
- ruled out: vs:D1281DF454A153AD is the on-foot RemLok line's draw, because skipping its draws did not remove the lines (F10).
- ruled out: vs:D95905C18B7FAD93 is the on-foot RemLok line's draw, because skipping its draws did not remove the lines (F10).
- ruled out: vs:CFA91824129ECBBC is the on-foot RemLok line's draw, because skipping its draws did not remove the lines (F10).
- ruled out: T slot +0xB0 is the HUD gate, because it is the counter R+0x780>0 and nothing in the HUD reads it (static).
- CORRECTION: the game code does test the selector mode in {3, 4} at about 50 sites (the "all readers treat 3..6 alike" claim held only for settings+0x3694/0x3698).
- CORRECTION: F4's "native serves a superset of cinema's views" is withdrawn (V is an OR of content masks).
- ruled out: the HUD view node is missing in HMD mode, because 0x28A4B90 built slot 0/1 nodes once at startup and never re-ran across mode switches (F5).
- ruled out: the cinema flag W+0x38 gates the HUD, because nothing reads it (static, 4 methods).
- ruled out: the HUD is gated by a per-group enable byte (+0x30), because all 25 group vtables read 1 in cinema and native (F4).
- ruled out: the HUD is dropped by the per-item view-mask test, because that test selects views (one-hot masks) and native serves a superset of cinema's views (F4).
- ruled out: the view masks exclude the 3P body from the native view, because its masks are a superset of the FP body's (F3 14:06:20).
- ruled out: forcing the avatar fade opaque shows the third-person body in native, because the fade was 0 from 13:10:55 and the 13:11:52 eye dump shows only the game's first-person body (arms, half torso with an open neck, legs), no third-person parts.

## Journal

- **2026-10-13, F0 flight 3 (Epic, Pimax OpenXR, build v0.19.1-25-g15eb62b5,
  10:36).** The 6 reached admission and was refused: `reason=selector_invalid`
  on both disembark requests. Cause: our check. It read owner+0x60 and
  compared it with the selector. But the selector is embedded in its owner at
  +0x60, and owner+0x60 holds the selector's vtable (the constructor 0x27FBFC0
  writes it there). The check now reads the selector's vtable, requires it to
  be the setter's (image base + 0x52E9608), and compares owner+0x60 with the
  selector by address only (d4875a12). Every setter line now shows `sel`, `vt`,
  `owner` and `owner_back`, so a refusal shows which part failed.
- **2026-10-13, F0 FLOWN OK (Epic, log edvr_gfx_20261013_104913.log, build
  v0.19.1-27-g88bba7b0).** From 10:50:56 the game's `incoming=6 effective=4
  source_after=4 admitted=yes owner_back=yes` was substituted every frame.
  Sean: full native stereo on foot; he can interact, open menus and talk to
  NPCs. NO HUD. The body shows arms and legs but no torso. Key off at 10:53:54,
  and cinema is back by 10:53:59 (the world route owns the frame). While
  substituted the game re-requested 6 about once a frame (14273 setter lines in
  three minutes, 4 MB). Gate counters at 10:54:06 read frames=16461
  world_route=1838 ui_layer=545 foot_split=28708 deferred_pacing=0
  panel_shape=26663 transition_flash=629. They are contaminated: the flag stayed
  set after the key went off (a forwarded 6 did not clear it). Fixed in F1.

- **2026-10-13, F1 FLOWN (Epic). Correction to the F1 notes:** the cinema NUMLOCK
  census held only 2 draws a frame, because cinema has no eye-sized targets. The
  cinema baseline therefore needs the INSERT eye dump, which arms an offscreen
  census.
- **2026-10-13, F2 BUILT d1e5ba54, NOT FLOWN.** Three parts, as in Status: the
  method key with the gate byte (identity checked, byte must read 1, restored on
  every way out), the native body (Explorer Cam's fade and head-part code, its own
  fade guard, Explorer Cam's claim wins; the head hooks are wanted from launch
  while the key is on), and the on-foot RemLok matcher (native stereo only).
  Decisions the maintainer should see: (1) `advanced.native_on_foot_method` is in
  [advanced], so it reaches the developer tier only, not the F8 Fixes page; moving
  it to [fix] is one line, if wanted. (2) Its values name the mechanism (swap,
  gate), against the ini rule; they are a temporary A/B and were specified so.
  (3) Unverified risk: PS 0x99C21CEB7A699821 is also the HDR-to-display pass named
  in eye_tonemap_snapshot.h. Nothing but native stereo and the slot-1 size tells
  the visor draw from that pass. First flight: the on-foot count should read 2 a
  frame, and the picture should be intact with `remlok_lines = stock` as the
  comparison.

- **2026-10-13, F2 correction (c884e218).** The on-foot RemLok matcher in the earlier F2 build (d1e5ba54) keyed on PS 0x99C21CEB7A699821, which is the flight-verified tonemap pass, not the visor. With remlok_lines = outer it would have scissored the whole eye. Removed; remlok_fix is back to 0a99c094. The method key moved from [advanced] to [fix] (F8 Fixes page) at the maintainer's request.

- **2026-10-13, F2 FLOWN (Epic).** The gate method works: no setter calls on foot, the
  game stays in mode 4, and its own requester asks for HMD. The swap method brings back
  about 440 re-requests per 5 s. Forcing the body fade opaque had no visible effect.
  RemLok on foot opens. HUD: in native the issuers 0x46546443FD3C3F88 and
  0x95D01BA609BF7500 have zero draws (cinema: about 39 and 28 a frame). The on-foot menu
  opens and accepts input (audio), and nothing is visible.
- **2026-10-13, F3 built (24009be8, log only, NOT FLOWN).** The HUD issuer stack probe
  sits at the top of beginPanelOverride (one hash compare per draw; the capture runs
  only on a match, once per shader per 10 s). The first-person avatar census reads each
  AMC the fade hook sees on foot, within 8 m of the local avatar, every 5 s. The
  requester comment is corrected (the cinema partner is kept while the byte is non-zero).
- **2026-10-13, F3 FLOWN (Epic; logs from ~13:58 and 14:06).** The HUD stack probe
  captured one stack for every cinema HUD draw: the same frames each time, the same
  path from 0x2868C30 down. Static RE of that path (scratchpad re_hudstack, hook_out.txt
  and ra_out.txt) finds it unconditional; the HUD is held back by a DATA gate. The
  avatar census: the first-person AMC is mode 2, with parts EVASuit_Arms, EVASuit_Hands
  and FirstPersonSkeleton, masks 0x60BC0000C05E; the 3P body is mode 3, masks
  0x60BD8000C05E. Ruled out: the view masks exclude the 3P body from the native view
  (see Ruled out).
- **2026-10-13, F4 built (cdae7a4d, instruments only, NOT FLOWN).** Two read-only hooks
  for the data gate: H1 at EliteDangerous64.exe+0x594D90 (per render item, V = [this+0x270],
  M = [item+0x30], E = [item+0x38]; first sight of each pair samples the recorder), and
  H2 at +0x58AF30 (per group, vt, B = [sub+0x30], N = [sub+0x88]). Both are temporary
  and log every 5 s in every state, with the calls per frame for pricing.- **2026-10-13, F4 FLOWN (Epic).** Groups and views are identical in cinema and native:
  all 25 group vtables read byte30 = 1 in both, and no render group or view drops out.
  The HUD items are never queued. So the HUD is not dropped by a group enable byte, nor
  by the per-item view-mask test: that test selects views (one-hot masks), and native
  serves a superset of cinema's views. String-led RE: the render graph has nodes for the
  HUD's views; 0x2854FA0 reads [[W+0x28]+0x9D4] and only changes the slot-1 registration
  flags (dl, r12) between modes 4 and 6; the PostGUI view nodes are created by 0x28A4B90,
  and that creation is gated on [p+0x2E0] at 0x28A5577.
- **2026-10-13, F5 built (e4f927bb, read-only probes, NOT FLOWN).** The F4 hooks are
  removed (history: cdae7a4d). Three probes, all VR profile only: P1 pass enable
  (0x2854FA0), P2 the +0x2E0 gate (0x28A4D30), P3 view-node creation (0x28A4B90). Each
  reads under SEH and forwards the registers unchanged. Aggregates every 5 s per state;
  the first sighting of each mode or slot change is written at the next frame boundary.
- **2026-10-13, F5 FLOWN (Epic).** The PostGUI view nodes are built once at startup in both
  modes. The only per-mode pass difference is W+0x38, and static RE finds nothing reads it.
  Next candidate: a stereo eye-index query, reached only through vtables. Ruled out: the HUD
  view node is missing in HMD mode (see Ruled out). The cinema bit gates only the
  CinemaPipelineFeature, its node and output, not the UI (+0x2D0) or PostGUI (+0x2E0)
  features. The 0x4C82A40 recorder is the generic gfModel batch executor. No game-side HUD
  code reads VR state directly.
- **2026-10-13, F6 built (95e491d7, merged main twice: b355af9a and c14e845b; green, NOT
  FLOWN).** The F5 pass probes are removed (history e4f927bb). Two read-only probes: E1 the
  eye-index query at 0x2842FD0 (returns 0, 1 or 2; classifies the node as slot0, slot1 or
  other against the two slot nodes) and E2 the stereo leaf predicate at 0x28540B0 (returns
  al; reads the mode and the flag byte at [S+0x2AE0]). Aggregates per 5 s, per state; a key
  is announced when first seen. The local debug layer is now present, so the debug-layer
  checks are real here (main fixed the isolation check in 3fd97a97).
- **2026-10-13, F6 FLOWN (Epic, log 20:27, build v0.19.1-175-g05993233).** The stereo leaf
  predicate 0x28540B0 returns 1 in mode 6 (cinema) as in mode 4, from every caller. The
  eye-index query 0x2842FD0 has one caller, +0x2840445, which runs once per mode change
  (slot0=1, slot1=2 in mode 4; slot0=0 in mode 6). Neither gates the HUD per item.
- **2026-10-13, F7 built (read only, NOT FLOWN).** The E1 and E2 probes are removed
  (history 95e491d7). A census of the request objects runs on the frame thread after each
  setter call and every 5 s in a session: owner = [sel+0x58]; R = [owner+0x1188]; T =
  [R+0x10]; S18 = [R+0x18]; the vtables and six slots of T and S18. Logged on change. The
  reads sit on the frame thread, not inside the setter, so the setter hook is unchanged apart
  from one counter.
- **2026-10-13, F7 FLOWN (Epic).** The request census (T and S18) did not single out the
  HUD. An Opus static pass then found the gate. In HMD modes the game disables the render
  graph node FirstPersonPostGUIViewNode every frame. The chain: the HUD gate test 0x1A6058C
  in 0x1A60560 (a HumanoidComponent interface, slot +0x30); the handler 0x19B1090 sets the
  FOV RC+0x16C (-1.0 in HMD); the requester at 0x2887A15 and 0x2887A39 reads the override
  dword at 0x5E82B48 (-1 auto, sole reader, no writer in the image); 0x28AAE60 then sets the
  FP node's enable byte [[F+0x30]+0x30] and the PostGUIViewNode collect flag
  [[F+0x38]+0x250] for slot0 and slot1; the stage loop at 0x6BF91A skips disabled nodes, so
  the FP lists (FP_WeaponAR, FP_PanelBackground, FP_PanelForeground, FP_HUD) are never drawn.
  Ruled out: `T slot +0xB0 is the HUD gate, because it is the counter R+0x780>0 and nothing
  in the HUD reads it (static)`.
- **2026-10-13, F8 built (e9823a26, NOT FLOWN).** With Sean's approval: the temporary key
  `fix.native_on_foot_hud` writes 1 to the dword at 0x5E82B48 while native stereo is on foot
  (identity checked, the value must read -1, four-byte write, refused if not writable), and
  restores -1 when the key goes off, native ends, the session ends, at shutdown, or on a
  failed check. A read-only census of the first-person pass state runs alongside it.
- **2026-10-14, F8 FLOWN (Epic).** The override (1 at 0x5E82B48) brings the first-person
  HUD node back on both slots. Only the reticle appears, and it moves opposite to the head,
  so it is anchored to the aim. Opus follow-up findings: the FOV value only overrides the
  projection (no skip); the panel gate is 0x19B1587, in 0x19B14E0 (HumanoidPanelComponent);
  the HRC camera-space placement branch is 0x1A36453, and its data byte 0x5E7A752 must NOT be
  written; the reticle path 0x1A14D80 has explicit HMD handling.
- **2026-10-14, F9 built (b1ba2714, NOT FLOWN).** Two code patches behind temporary keys,
  applied only while native stereo is on foot and the override is held, each verified byte
  for byte before the write and restored to its exact original bytes: panels 76 73 -> 90 90
  at +0x19B1587; mesh 74 -> EB (the first byte) at +0x1A36453. Read-only probes: P1 panel
  submit (by caller), P2 HRC submit (the three mesh instances, dep and its vtable), P3 the
  per-instance submit for those three instances (view id, camera position).
- **2026-10-14, F9 FLOWN (Epic).** The panels are visible with patch A (76 73 -> 90 90
  at +0x19B1587). The HUD mesh patch alone did nothing. P2 showed the HRC instances
  ([HRC+0x418/0x420/0x428]) at 0 in native; in a cinema check they were created about 10 ms
  after g291 went to 1.
- **2026-10-14, Opus gate finding (F9).** 0x1A14970 calls `call qword ptr [rax+0x80]` on
  dep = [HRC+0x1D0] (vtable slot 0x1A8D190: HUD visible AND [dep+0x380] == 0 AND the selector
  mode is not 3 or 4; `jbe` at 0x1A8D1CE returns 0 in modes 3 and 4). Then `je 0x1A14D4E`
  at 0x1A149AE skips both the creation and the submit. Site I1 is 0x1A149A8, 80 -> 78.
  Expected placement: like the reticle, about 0.19 m from the aim camera. Later placement
  could rewrite [HRC+0x430..0x46F] and [+0x480..0x4BF] at the 0x1A14970 entry, or use quad
  layers. The panel sizing is likely the generic GUI-panel path that ui_quality already
  patches (UNVERIFIED; test UI quality 125).
- **2026-10-14, F10 built (20a57e54, NOT FLOWN).** The mesh unit patches both sites, all or
  nothing: 0x1A36453 74 -> EB and 0x1A149A8 80 -> 78, verified together before either write,
  and restored together only while both still read as this unit left them.
- **2026-10-14, F10 FLOWN (Epic).** With the mesh unit on, the three HRC meshes are created,
  submitted, and their draws (VS 0x46546443..., 0x95D01BA6..., 8 per eye) reach the HDR eye
  targets. Nothing is visible. Captures: the HUD families draw 8 per eye, depth-tested
  (func 7, no depth write); the wheel opens and adds no eye draws, so the wheel is likely drawn
  inside the HumanoidHUD canvas on the HRC meshes. The elevator interaction panel is visible
  with the panel patch. UI quality 125 sharpens the on-foot panels.
- **2026-10-14, Opus correction (F10).** The EB patch at 0x1A36453 is WRONG. On foot the HRC and
  the reticle's marker component draw in camera space into list B (the data byte 0x5E7A752 is
  1). EB hands list B a world-space matrix, so the meshes land off-axis. The reticle is
  HumanoidMarkerRenderComponent (0x1A14D80), on the camera-space path. Predicate call sites:
  HumanoidARComponent (0x1A6DD4F, the +0x80 site); if AR overlays turn out missing, the I1-style
  byte is 0x1A6DD51. The near plane on foot is 0.025 m, which is fine.
- **2026-10-14, RemLok (F10).** The on-foot RemLok lines are NOT the cockpit overlay. census_skip
  of six draw families did not remove them: vs:5E417E9DF2E7F9E6 (which also drives the smoke),
  vs:3064D7F445192FDD, vs:B75A6FF2CA9FA5D6, vs:D1281DF454A153AD, vs:D95905C18B7FAD93 and
  vs:CFA91824129ECBBC (see Ruled out). The mirror (the post-tonemap eye copy) shows them. The
  hypothesis: the visor edge is inside a post pass, most likely the tonemap, driven by visor
  constants. Under investigation.
- **2026-10-14, F11 built (f9150ae5, NOT FLOWN).** The creation gate (fix.native_on_foot_hud_mesh,
  80 -> 78 at +0x1A149A8) and the placement A/B (fix.native_on_foot_hud_mesh_world, 74 -> EB at
  +0x1A36453) are separate keys, each verified before its write. P3 logs each view's rotation
  floats (+0x20..+0x4C) with its camera position; P2 logs the HRC's matrix (+0x430..+0x46F) once
  per 5 s per state. The six native on-foot keys now sit on the F8 page "Native on foot",
  after Explorer Cam.
- **2026-10-14, F11 FLOWN, HUD VISIBLE (Epic).** With fix.native_on_foot_hud_mesh on and
  fix.native_on_foot_hud_mesh_world off, the HUD elements (radar, compass, frame) are visible in
  native stereo and head-locked. The quick access menu (the utility wheel) shows too. The
  elevator interaction panel was visible earlier (panel patch). The pilot says the HUD sits a
  little too far out from the face. The placement patch (0x1A36453) is not needed for the HUD
  to show.
- **2026-10-14, F12 built (7440ec3b, NOT FLOWN).** A user setting, fix.native_on_foot_hud_distance
  (0.08..0.40 m, default 0.192, live, the Native on foot page under the mesh rows). While native
  stereo is on foot and the creation patch is in, the two HRC placement matrices (+0x430 and
  +0x480) are scaled by distance / 0.192: the basis and the translation's x, y, z, with the w
  column kept, so the angular size is kept and only the depth changes. Each matrix keeps the
  game's base values and what it last wrote, so a scale is never compounded. A fault stands the
  writes down for the session, logged once. At the default distance, EDVR puts the base back once
  and stops claiming the matrix.
- **2026-10-14, F12 FLOWN (Epic).** The HUD sits too far out (Sean wants under 0.08 m) and does
  NOT follow the head: it is anchored to the body or the aim camera, and it stays behind when he
  turns his head. A negative distance is not meaningful: it would put the HUD behind the eye. The
  distance slider keeps the angular size by design, so it cannot spread or narrow the layout. Size
  is the control for spread: F13 adds it as the HUD's basis scale at the same depth.
- **2026-10-14, evidence check for the head follow (F13).** The list-B view rotation rows
  (P3, the view's +0x20..+0x4C) change over time, so they carry the head pose. Samples from
  edvr_gfx_20261014_081619.log, view 2, instance 0: at 08:18:24.875 campos=(-0.044,0.014,-0.047)
  rot=(0.911,0.014,0.413,0.000,-0.090,0.982,0.167,0.000,-0.403,-0.189,0.896,0.000); at 08:19:04.920
  campos=(-0.327,-0.070,0.373) rot=(0.728,0.258,-0.635,0.000,-0.278,0.958,0.072,0.000,0.627,0.124,0.769,0.000);
  at 08:21:10.614 campos=(-0.047,0.006,-0.047) rot=(0.767,0.058,0.639,0.000,-0.108,0.993,0.039,0.000,-0.633,-0.099,0.768,0.000).
  They are not identity, so the views are head-rotated.
- **2026-10-14, the visor override RE (F13).** HumanoidVisorComponent update 0x1A1F630 and state
  0x1A18610 read visorDown = byte[0x5F741B7] ? byte[0x5F741B8] : this[+0x108]->vfunc+0x20(); the
  factor ramps at 3 per second. SuitUI (0x1A503D0) also follows the override, so its value is
  forced to 0 while it is on (an expected side effect). F13 writes B8 = 0 first, then B7 = 1, and
  restores B7 = 0 only while it still reads 1.
- **2026-10-14, F13 built (fe5fc500, NOT FLOWN).** The user settings: distance (minimum now 0.03),
  size (0.30..1.50, the basis rows only, the translation kept), follow (head or body), smoothing
  (0..500 ms, slerp and lerp, dt clamped to 0.1 s), and the temporary order A/B (after: M . C;
  before: C . M). The head transform C comes from the list-B views' rotation rows and the mean of
  the first two views' positions. The composition order is base, then distance, then size, then
  the head. The visor override is a temporary test key.
- **2026-10-14, F14 built (2484625d, NOT FLOWN).** Three pieces.
  (1) Spread, a user setting (0.0..0.45, default 0, live): while native stereo is on foot and the
  creation patch is in, the HUD margins +0x5E7A754 (x, game 0.027) and +0x5E7A758 (y, game 0.06) are
  written 0.027 + spread and 0.06 + spread (clamped below 0.489). The game re-sends the layout when
  the HRC's copies differ from these globals (0x1AB32F0), so nothing is forced. Restored on any stop.
  (2) Layout probe (read only) on 0x1A0DCD0: the prologue was read from the exe with capstone; the
  first five bytes are mov [rsp+0x20],rbx, with no rip-relative operand and no branch, so the steal
  is clean. Each HUD part's layout re-send logs one line (name, anchor, offset, align, scale, out,
  canvas size, the HRC's floats).
  (3) Reticle identification stub (temporary, fix.native_on_foot_reticle_probe): the call at
  0x1A15644 (call 0x434ECC0, preceded by mov rcx,[rsi+0x60] at 0x1A15640) was verified byte for
  byte, with its rel32 target at 0x434ECC0. A stub passes the instance and node to a read-only probe,
  then calls the real function. The probe writes nothing to the instance. The rel32 is one 4-byte
  store inside a single 64-byte cache line, so it is single-copy atomic on x86; a qword CAS was not used.
  Reticle setting: "Reticle follows: aim | head" is approved in principle; it is built once the
- **2026-10-14, F15 built (202d39b9, NOT FLOWN).** The body RE, static, from the F3 census and
  the visibility job: the local third-person AMC already passes the job in the eye views (view
  filter 1, alpha 1.0), and its masks are a superset of the first-person AMC's. Hypothesis: the
  3P body is drawn but hidden behind or inside the FP body, and backface-culled from inside.
  E1 tests it: `fix.native_on_foot_hide_fp_body` (temporary, default off) zeroes the four masks of
  EVASuit_Arms, EVASuit_Hands and FirstPersonSkeleton on the local mode-2 AMC, after the fade and
  after headHideStep (explorer_cam.cpp fpBodyStep; the frame side is explorerCamFpBody). Nothing is
  restored, and alpha, the view filter and the flag bytes are not touched.
  Identification: mode 2 (the creation parameter at +0x14). The code's note at isLocalPair says only
  the local player has a first-person avatar, so mode 2 is taken as local. The census row for that
  AMC carries its distance from the local 3P AMC; expect about 1.65 m.
  Log lines: `native on foot body: first-person body hidden (... on AMC 0x...)` once per transition;
  `... shown again (<why>)`; `no first-person AMC (mode 2) reached the fade hook ...` after 5 s; and
  every 5 s `first-person masks after the zero: ...`, read straight after the write. The frame
  thread's own census read can land after the game's next-frame rewrite, so the readback line is
  the evidence that the zero happened.
  Reading the flight: (a) readback 0 on the three parts and the body shows in the eye views: the
  hypothesis holds. (b) readback 0 and nothing shows: the 3P body is not hidden by the FP body;
  next is the read-only visibility-job probe at 0x3DA2C80. (c) no mode-2 line: the fade hook does not
  see the FP AMC; read the census. (d) a name missing from the table: logged once, that part is skipped.
  Gates: build.bat --wait-lock, full gates passed, receipt written. The census now sorts mode-2 rows
  first, so the FP AMC is printed.
  identification is known.