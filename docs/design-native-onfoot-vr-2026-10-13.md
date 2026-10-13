# Native on-foot VR (replacing Explorer Cam) — design, 2026-10-13

## Status

- **State:** F0 FLOWN OK (Epic, 10:50). F1 FLOWN (Epic). F2 FLOWN 2026-10-13 (journal).
  F3 BUILT 24009be8 (log only; full build green), NOT FLOWN. Branch
  `claude/native-onfoot-vr-completion-219ffa`, off main until ready (Sean). Not pushed.
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
- **Open hypotheses:**
  - H1 (flat-frame HUD) is not refuted: the issuers have zero draws in native, which
    is what H6 also predicts.
  - H6: the on-foot HUD's issuing code skips in HMD mode behind a gate. Discriminator:
    F3's stacks of 0x46546443FD3C3F88 and 0x95D01BA609BF7500 in cinema, then the same
    frames absent in native.
  - Body route 1 (add the HMD view's bit to the 3P body masks, hide the FP body) needs
    the first-person masks from F3's avatar census.
- **F3 changes (24009be8, log only):** `native on foot hud stack: vs=.. ps=.. frames: ..`
  (any key state, once per shader per 10 s, from beginPanelOverride); `native on foot
  avatars: amc=.. mode=.. parts=N dist=.. [name masks ..]` (every 5 s on foot).
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
- **Next:** Sean flies F3 on Epic: key on, method gate, cinema first (the reference
  stacks), then disembark, walk, open the menu, board. Read the hud stack lines in
  both modes and the avatar lines on foot.

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