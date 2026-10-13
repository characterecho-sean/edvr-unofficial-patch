# Native on-foot VR (replacing Explorer Cam) — design, 2026-10-13

## Status

- **State:** F0-F4 FLOWN (Epic; journal). F5 BUILT e4f927bb (probes only), NOT FLOWN.
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
- **Key finding:** every static test of the mode treats 3..6 as one class, so the
  missing HUD is not a mode branch to flip. H1 (leading): the on-foot HUD is
  screen-space, built for the flat frame; EDVR must place it.
- **F3 findings (flown):** every cinema HUD draw comes through ONE stack; the path from
  0x2868C30 down is unconditional (static RE: scratchpad re_hudstack). The first-person
  AMC (mode 2) has EVASuit_Arms, EVASuit_Hands and FirstPersonSkeleton; its masks are
  0x60BC0000C05E. The 3P body (mode 3) has 0x60BD8000C05E.
- **Open hypotheses:**
  - F4 (flown): every group enable byte reads 1 in cinema and native, and no view drops
    out; the HUD items are never queued. The data-gate candidates are not the answer.
  - H7: the render-pass setup. Four slots (Primary, Secondary, Thumbnail, Auxiliary); the
    PostGUI view nodes are created by 0x28A4B90 only when a pass's [p+0x2E0] is non-null
    (the gate at 0x28A5577).
- **F5 instruments (e4f927bb, TEMPORARY; remove after F5):** read-only hooks P1
  +0x2854FA0 (pass enable), P2 +0x28A4D30 (the +0x2E0 gate), P3 +0x28A4B90 (view nodes),
  each stolen 5; lines `native on foot pass enable|slot|hudnode [state]`. F3's stack and
  avatar lines stay until the arc closes. F4's hooks are gone (history: cdae7a4d).
- **OPEN, RemLok on foot:** the on-foot visor overlay is not yet identified. It
  needs a skip-probe or a diff census that excludes the tonemap PS
  0x99C21CEB7A699821 (tonemap_admit.h:39).
- **Ruled out:** the cinema request is always 5 (see the end of this doc).
- **Temporary keys (both go when the arc picks one):** `fix.native_on_foot`
  (on/off, default off, VR, F8 Fixes page, live) and `fix.native_on_foot_method`
  (swap | gate, default gate, live, F8 Fixes page directly under the one above).
- **F2 changes (c884e218):** gate: the setter never substitutes; EDVR holds the byte
  at 0 while the key is on and a game session is live (identity and byte checked
  first). Native body: while native stereo is on foot, the fade global goes -1 to 0
  and the local head parts are zeroed, through Explorer Cam's code and its own
  guard. RemLok: unchanged from 0a99c094 (the on-foot matcher was removed).
- **F2 log lines:** `native on foot: fix.native_on_foot_method is gate|swap`;
  `native on foot: gate byte EliteDangerous64.exe+0x5E82B3E 1 -> 0 (method gate)`,
  `0 -> 1 (restored: <why>)`, or `NOT written -- <why>`; setter lines read
  `reason=method_gate` under gate; `native on foot body: on (fade mode -1 -> 0, head
  parts hidden)` / `off (fade restored -1: <why>)`. The F1 census lines are unchanged.
- **Install:** Epic only (Sean: Frontier carries another feature test). `edvr.ini`
  is not touched unless `--ini` says so.
- **Next:** F5 on Epic: key on, method gate, cinema first, then disembark, walk, open the
  menu, board. Read the `native on foot pass` lines per state; the first-sighting lines
  mark each mode and slot change.

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