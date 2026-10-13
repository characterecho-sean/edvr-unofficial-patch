# Native on-foot VR (replacing Explorer Cam) — design, 2026-10-13

## Status

- **State:** F0 REBUILT d4875a12 (the selector check fixed; see the journal at
  the end). Flight 3 (Epic, 10:36) reached admission for cinema 6 and refused
  it: selector_invalid, from our own check. Not flown with a substitution yet. Branch
  `claude/native-onfoot-vr-completion-219ffa`, off main until ready (Sean).
- **Origin:** a user's handoff (not in the repo). They hooked Elite's
  render-mode setter, swapped an incoming 5 (Cinema) for 3 (HMD) while
  disembarking, and got native stereo on foot, interactive, not the external
  camera. Missing: the normal HUD, top radar, left panel and inventory. A
  later experiment of theirs restored the reticle and weapon label.
- **Build:** Sean's installs hash to the handoff's exe (`e6be8bbe…8e988`, 332841).
- **Verified offline (capstone, 2026-10-13):** setter = vtable 0x52E9608
  slot 5 = 0x28723C0; it writes selector+0x20 at 0x2872441; selector =
  owner+0x60, selector+0x58 = owner (ctor 0x27FBFC0); 0x2890B30 copies
  requested settings+0x3698 into current +0x3694. No direct callers exist; the
  setter is reached only through the vtable.
- **Key finding:** every static test of the mode in the game treats 3..6 as
  one class (`sub 3; cmp 3; ja`). Nothing reading +0x3694 tells 3 from 5. So
  the missing HUD is not a mode branch to flip. Leading hypothesis (H1): the
  on-foot HUD is screen-space, built for the flat frame. Cinema mode composites
  it into the 2D image; HMD mode has no on-foot path that places it in the
  eyes. Finishing the feature means **EDVR places the HUD itself**.
- **Open hypotheses:** H1 (above); H2: the only 3→5 / 4→6 remap is in
  0x28873C0 (2887ABC..2887ACB), from a vcall on [owner+0x1188]+0x18. This may
  be the natural "this context wants cinema" decision, and a cleaner hook point
  than the setter (UNVERIFIED); H3: EDVR's on-foot gates key on the journal's
  OnFoot flag and will misfire on stereo eyes (world route, UI layer,
  footSplit, weapon pacing).
- **Ruled out:** the cinema request is always 5 (see the end of this doc).
- **Temporary keys:** `fix.native_on_foot` (on/off, default off, VR only),
  added by F0 (1535fb59) on the Fixes page. Removal is proposed, quoted, when
  W1 ships as behaviour; nothing removes it before then.
- **F0 instruments (1535fb59):** `src\d3d11\native_on_foot.cpp` (the setter
  hook), `native_on_foot_core.h` (admission), `src\openxr\submit_shape.h`.
  Hook: CodeHook on the setter at +0x28723C0, through a near relay (the
  five-byte jump reaches only 2 GB; the relay is placed within reach, as
  skin_entity_hook does). Prologue and vtable slot verified under SEH.
  A refusal quotes CodeHook's reason. Admission: key on, incoming 5 or 6
  (forwarded as its HMD partner, 3 or 4), selector frozen at the first valid
  call, `journalGameplay()` (LoadGame, no Shutdown).
- **Read after the flight, in the d3d11 log:**
  - `native on foot: installed --` or `NOT installed -- <reason>` (once, at the
    first VR frame). Refusal reasons: identity, slot, prologue, CodeHook, flat.
  - `native on foot: incoming=N effective=N source_after=N key=on|off
    admitted=yes|no reason=... onfoot=... seated=... t=ms`: one per setter
    call, key off or on. A natural incoming 3 or 4 is `admitted=no reason=not_cinema`.
  - `native on foot gates: frames=N world_route=N ui_layer=N foot_split=N
    deferred_pacing=N panel_shape=N transition_flash=N` every 5 s while native
    stereo is on. Cumulative since launch.
  - In the openvr runtime trace: `submit shape: left=WxH bounds=... right=WxH
    bounds=... same_texture=yes|no`, once a second while frames are submitted.
    Stereo eyes read as two eye-sized textures; the cinema panel as one shared
    16:9 texture (`same_texture=yes`).
- **Install:** Frontier and Epic get the `--dll-only` build stamped with the
  commit that carries this line. `edvr.ini` is never touched.
- **Next:** Sean flies F0 (the flight below). Nothing builds before that.

## What "finished" means

Explorer Cam goes. Pressing nothing, the commander leaves the ship in native
stereo, with a usable HUD, inventory, weapons and menus, and returns aboard
unchanged. Five workstreams, in dependency order:

| # | Work | Size | Risk |
|---|---|---|---|
| W1 | Activation: hook the setter's vtable slot, swap 5→3 only when admitted | small (CodeHook pattern, ~300 lines + rig) | low; addresses verified |
| W2 | EDVR frame path: re-key on-foot gates from "journal on foot" to "eyes are stereo" | medium; ~6 gates | medium; regressions on the panel path |
| W3 | HUD: find the on-foot HUD targets, composite them into the eyes as a placed layer | **large**; the bulk of the arc | high; RE + several flights |
| W4 | Contexts: inventory cursor, radial menus, maps, contacts, SRV, elevators, Apex, loading, intro video, deaths | medium; mostly per-context admission | medium |
| W5 | Remove Explorer Cam (6 files, ~5.7k lines, 8 keys, F5 hotkey, F8 page) | small | none, once W1-W4 fly; keys quoted and asked first |

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

### W3 — the HUD

The handoff's runtime comparison saw the HUD resources still produced in
mode 3, with their usual PS-SRV composite consumers absent. EDVR already has
the machinery to place a 2D layer in the eyes (the UI layer path,
`ui_layer_math.h:8-13`, `kScreen` family at :583-587). The work:

1. In cinema on foot, identify the HUD composite draws and their source
   targets by draw census (`dump_draws` already exists). Expect separate
   targets for radar, left panel, inventory and reticle.
2. In forced mode 3 on foot, confirm the same targets are written and not
   read by anything that reaches the eyes.
3. Composite them into each eye as a body- or head-locked quad at a
   comfortable depth (reticle at the aim depth, as fix.ui_depth does in the
   cockpit). Inventory and radial menus need the cursor mapped onto that quad.

Fallback if (2) fails, i.e. the game skips the HUD render entirely in mode 3:
find what skips it, starting at 0x28540B0 (mode 3..6 AND settings+0x2AE0==1
predicate, no direct callers).

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
