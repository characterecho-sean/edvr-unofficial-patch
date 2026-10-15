# Native on-foot VR (replacing Explorer Cam) — design, 2026-10-13

## Status

- **State (2026-10-15):** F0-F22 flown in part (journal). F23 flown: the guard tripped. F24, F25 installed, not flown as a whole. F26 flown (log 043326). **F27 to F38 BUILT, NOT INSTALLED (F34 key cleanup; F35 body shift; F36 weapon stability steps aside; F37 body align fixes and the waypoint ring; F38 pov target, true head for the view, forward_lead waypoint; F39 neck joint by name on the local skeleton; F40 view map, calibrate mode, live now_pose, walk neck fallback, flown; F41 body and view filters, psi 0, built; F42 frame-locked head pose, built; F43 shoulder body anchor, built; F44 draw-time marker instrument, built; F45 part B, the instrument's every-draw record match, world view split and eye check, built; F46 part B, the per-eye NDC pair of the marker composite against P through the world rows b1 270..273, eyes from the UI layer, built; F47, the gated per-eye marker correction `fix.native_on_foot_marker_fix`, off, TEMPORARY, built; F48, the pov body anchor, built; F49, the pose freeze probe, built; F50, the marker lag delta and the body's vertical lock, built).** Branch `claude/native-onfoot-vr-completion-219ffa`, local only. Install: Epic `--scope vr` only, and only when Sean says.
- **F48 (built, not installed):** `fix.native_on_foot_body_align_anchor` = pov (default) | neck (F46) | clavicles (F45, ruled out). pov places the 3P def_c_povCamera_joint at the FP root, its height held at the standing value (captured at key-on and at rest). The trims default to 0 again (sliders unchanged). Census: the FP AMC's whole part table, the FP parent array, the 3P parents of 15, 34, 35, 36, 44. Temporary: the anchor key goes when the arc picks an anchor.
- **F49 (built, not installed):** a post-hook on frAnim Animate (0x741520) for the local avatar's evaluator only. `fix.native_on_foot_pose_freeze` = probe (default; measures, writes nothing) | on (holds the torso-side subtree of the hips at the rest pose while standing idle, blended over 0.3 s) | off. Hidden, live, TEMPORARY. Open: whether the skin reads the span the hook writes (the t2 row packer was not found).
- **F50 (built, not installed):** the marker fix's delta is now -(m - c), m = X0 through the marker's b0 minus P through the eye's world rows, c a per-eye baseline (30 slow-head frames, std under 0.5 px; measured about +0.036 NDC in y); gate 0.05 NDC; the 1 s line's honesty check says `not constant: masking` when c moves with pitch or roll. `fix.native_on_foot_body_align_vertical` = off (default: body height locked, up trim ignored) | on (F48). Hidden, live, TEMPORARY. Next flight: c ready in both eyes, the residual near 0 px at every head speed, no masking line; feet on the ground in body mode.
- **F51 (built, not installed):** body mode raises the view (seated y = -dh) and the FP AMC's records (+dh along the body's up row) by dh = 3P pov height - FP side height, low-passed 1 s, 0..0.5 m; `fix.native_on_foot_body_raise_fp` = on (default) | off, hidden, live, TEMPORARY. The weapon is not an AMC part and is not moved. Pose freeze: the local X is chosen against an Animate X census (P match fallback); the gate is now no locomotion (weapon out freezes, ADS excluded). Next flight: the `body raise (F51` and `pose freeze (F51` lines; arms at the shoulders, camera at the 3P eye.
- **F28 verdict (flown 10-15):** the menu exit Stops and destroys the CTSA after Succeeded at 05:32:31.39, 1.7 s before Deactivate. SGMM polls ReadyToAllocIDs about 90 times a second with no timeout. Stuck case = H1 (a click mid-handshake kills the CTSA; SGMM waits forever). H2 (Run count to 0 with no Stop) is not what the verdict shows: Stop did appear. Open option: a timeout on the ReadyToAllocIDs wait, pending RE.
- **F28 v2 (BUILT, NOT INSTALLED; read only; key `fix.training_connect_probe`, on, hidden):** ActivitySet ClearAll (+0x4C90C40) and Remove (+0x4C8FAF0) hooks, each with AM, act, RA and time; the CTSA owner AM at [act+8] on ctor return; the services at [act+0x40..+0x98] once at Start (twelve qwords; the brief said fourteen); SGMM per second (count, [this+0x138], [this+8]) while it changes or a CTSA was destroyed in 120 s; the StartTutorial fire as the change to state 1 (read after the call); the first SGMM call after each Deactivate; the IDProvider link each second while an SGMM wait polls (IsConnected = [idp+0x28] non-null and [ptr+0x60] >= 3).
- **F29 (BUILT, NOT INSTALLED):** the eye rule comes from the actual state. Gate method: HMD mode, gate byte written, on foot. Substitute method: the setter's substitution. Evaluated at the setter's mode switch and after each frame's gate-byte write, so the flip lands before the first frame in the new mode. Logged at every change: `ui quality: eye rule native (first = right): <why>` or `cockpit (first = left)`.
- **Waypoints (F29 item 2):** `native on foot world markers: C age ... list-B sample #..., eye 0 vs eye 1 rotation difference ...` every 5 s, with the marker's C. **Deferred:** same-frame head sourcing; it needs a list-B capture at marker entry 0x1A14D80, whose stack arguments are not yet known.
- **F27 (BUILT, NOT INSTALLED):** per-eye visor PS (68ABCB9FEF6CA66C; byte 1724 0x41 left, 0x01 right; checksum recomputed; no scissor). `remlok_swap_eyes` stays as the control. The eye order flips in native at the setter. World markers: `fix.native_on_foot_world_markers` forward (default) | fixed | game, live.
- **F26 (flown, log 043326):** the eye check stood down at the render-mode switch, before native on foot was active; the on-foot RemLok match fired 5009 per 30 s with the wrong parity. The sharp on-foot HUD family is `fix.native_on_foot_sharp_hud` (off, live).
- **Runaway, open:** F22-F24 spun. Look compensation does not reach the game's head pose (residual ratio 1.03-1.09 with yaw_sign -1). The F25 flight checks decide it: `look_to_aim_runtime` seated yaw before and after each step, and the reset line.
- **Ruled out:** forward = -row 2 of C (F23); the cockpit eye rule in native on foot (F27); yaw_sign -1 alone (F24: the body turned away); the back-face cull (F18); the EDVR HRC rewrite moving the waypoint (static RE).
- **Temporary keys (go when the arc picks one):** `fix.native_on_foot`, `_method`, `_hud`, `_panels`, `_hud_mesh`, `_hud_mesh_world`, `_hud_follow_order`, `_reticle_probe`, `_hide_fp_body`, `_body_cull`, `_body_unclip`, `_body_cb_probe`, `_body_mode`, `_body_hybrid`, `_look_to_aim_yaw_sign`, `_look_to_aim_comp_delay`, `_look_to_aim_comp_sign`, `_visor_shift` (hidden), `_view_probes` (off, restart), `_sharp_hud`, `_world_markers`, `training_connect_probe`.
- **Install:** Epic only; `--ini` never. `edvr.ini` is not touched. Requester gate: never write the neighbours 0x5E82B3C and 0x5E82B3F of the byte 0x5E82B3E.
- **F30 (BUILT, NOT INSTALLED; one commit, subject `F30`; receipt e3755f82...):**
  - **Part 1, the Training stall** (`fix.training_connect_fix`, on, hidden, TEMPORARY). A (the hold) was removed in F32: SGMM Run is never called in this flow. B: from the owner AM's Update (+0x4C8EEA0, 11 bytes, after the original returns), a CTSA destroyed (dtor, or the owner's Remove) with a load pending (a Deactivate after it), the link down (IsConnected by the RE rule) and 10 s quiet: set init, key insert, Instantiate, set free (the game's order), StartPending once if no Start within 1 s; two attempts per pending load, then gave up. Callee entry bytes are checked at install; a mismatch refuses recovery. Nothing here frees an object.
  - **Part 2, the sharp HUD's eye check:** every render-mode edge re-arms it (the setter counts edges on any change of effective mode); a swapped composite is skipped and counted, and the layer stands down only after 30 in a row; an eye-check stand-down is lifted on the next render-mode edge (logged). The diagnostic prints the key and the active state separately.
  - **Part 2, the waypoints:** the marker entry (0x1A14D80) is installed in every VR session. Its .pdata range runs to 0x1A157B7, so the post stub at 0x1A15644 is in the same function. The pre-hook reads this call's head from list B (r8) into a thread_local; the post stub uses it, and g_worldC is the fallback. The 5 s line gives the source counts, the sample lag, the QPC age, P = node+0x50 (read as three floats, an assumption) and |t' - P|.
- **F31 (BUILT, NOT INSTALLED; the F31 commit):**
  - **Eye rule: the cockpit's everywhere (first = left).** The native flip is gone (setter, gate, RemLok `remlokSwapNow` = `remlok_swap_eyes` only, ui_depth filing). Sean's swap test: `remlok_swap_eyes = 1` fixes the visor under the native rule, so first = right was wrong for the visor. The F30 layer swap was persistent under first = right (07:38:51). The F30 tolerance, the edge re-arm and the stand-down lift are kept; every render-mode edge re-arms (setter), and an eye-check stand-down lifts at the next edge.
  - **Waypoints: the runtime's "now" head (H_M).** EdvrNativeFrameInput version 2 (ladder step 7) carries `nowHead` (the rigid transform of the head the runtime answered to Elite's "now" call, the same 3x4 as physicalHead, seated space) and `nowSequence`. The d3d11 half forms H = C . (N^-1 R)^T from the render head R and the now head N, so the body transform cancels, and H = C when N = R. Mode `fix.native_on_foot_world_markers`: forward_now (H_M, default) | forward (C) | fixed | game. Logged per 5 s: the angle H_M vs C, the now answer and frame numbers, t' under both maps.
  - **Not verified:** the runtime "now" head and physicalHead are in the same space (both seated, by the locate code); the H_M sign and the delta order (the flight decides: H_M vs C angle at rest should be near 0; a turn should move H_M); a version 7 refusal that is not a shape refusal steps the runtime down to version 6 for the session (the ladder's existing rule).
  - **Not done:** the identity-vs-order diagnostic every 30 s (the existing `eyeSwapped` window counter is the identity check and already prints per 30 s); step 2's runtime pose timing beyond this; the visor is unchanged (its rule now follows the cockpit, with `remlok_swap_eyes` the control).
- **F31 flight 081349 (the F30 build, Epic).** The sharp HUD is FIXED (Sean: clear again). The waypoint forward_now never ran: the live `edvr.ini` in the game directory still says `native_on_foot_world_markers = forward` (installs never overwrite it), and the runtime's "now" head was read only in forward_now, so the 22 "no runtime now head" lines were the fallback, not a failed read. The runtime does publish: Elite's "now" calls from exe+0x4E3881 were answered (14:13:49.960). Sean's live ini must read `forward_now`; the live file is his, so we have not edited it.
- **F32 (BUILT, NOT INSTALLED).** Training, the F31 build (081034): the recovery reconnected fully (a new CTSA, states 1..10, missions and matchmaking joined) at click+35 s, but the load had stalled at about click+20 s. In OK runs the post-click digest step (click+10-16 s) is followed about 4 s later by "Calculated hash of 0 for Human_Tiny_Military_01_Tutorial" and the location load starts; in the stuck run that step fired at click+16 s with no sessions. SGMM Run (+0x1D1CDF0) is never called in this flow, so the hold (F30 A) and its hook are removed.
  - **Recovery timing.** Attempt 1 fires 8 s after the destruction with a load pending, whatever the link. Attempt 2 needs the link down, and fires after 10 s without a session or on the shortcut (Init 1 to 10 with none of states 2..8 between; logged as "shortcut: no sessions were made"). Progress is a session (states 7 or 8).
  - **Probe v4 (read only, behind `fix.training_connect_probe`).** Remove logs the activity's name (vtable slot 1, the `lea rax,[rip+rel32]; ret` stub). The started-array insert (+0x4C8E020, 18 bytes): the act is at r8+8, because the caller passes r8 = node+0x10 and the StartPending loop takes the act from node+0x18. +0x1D1D220 (16 bytes; the brief's 15 would split `lea rbp,[rsp-0x27]`): calls and [this+0x138] each second. AM Update: calls per AM each second for 120 s after a Deactivate (top 8).
- **Now-head fix (F32).** The provider no longer refuses a whole frame over an optional head: an unusable one is dropped for that frame and logged, because a refusal steps the runtime's ladder down for the session. The provider logs the first version 2 accept and any version 2 size refusal; the runtime logs its first publish, each ladder step-down and the negotiated version; the d3d11 side reads the now head at every marker post (any mode) and logs its first read and the first valid head.
- **F33 (BUILT, NOT INSTALLED; `fix.native_on_foot_body_align` = off by default, live).** Body align: the seated origin moves so the game's first-person camera sits at the third-person body's pov, so the first-person arms meet the torso. No game writes.
  - **Runtime.** Frame output version 8 (`eyeAlign`, a seated-space translation, and `eyeAlignSerial`; version 6 and 7 callers read zero). The runtime folds it into the seated origin as a translation through the look compensation's route (`seated.replace`), with no origin generation bump and no currentOrigin write, and a rebase (a new seated origin) resets the applied value. The same seated space carries the render, "now" and layer poses.
  - **Controller (d3d11, `alignTick`).** The error is (cam - HMD) - pov3P in body axes (right, up, forward), minus the trims. cam is the job camera; pov3P is the head joint (neck census) plus Explorer Cam's rest offset, through the AMC matrix. HMD is the eye position of the game's list-B view (world axes). The integral takes 0.2 of the seated error per frame (5 cm at most), clamped to 0.6 m; a critically damped second-order filter (tau 250 ms) follows it; the published value moves at 5 cm per frame at most. The guard: when the error has not fallen to 90% 2 s after the offset started, the offset goes to zero for good until the key is toggled. Held while ADS, and while an input is missing or the AMC sample is over 100 ms old.
  - **Deviations from the spec, for the flight.** (a) HMD_body comes from the list-B eye position in world axes, not from the runtime's physicalHead mapped into body axes: the list-B view already gives world axes, so no seated-to-world transform is needed for it. (b) The body-to-seated rotation uses the list-B view C (world from view) and the render head R, as F31's H_M does; sign and order unverified. (c) fp_root is logged "unread": the mode-2 root cross-check is not wired. (d) The head joint is the neck census head (as the P1 probe reads it), not a skeleton slot read in the hook. (e) The rest pov offset comes from Explorer Cam's skeleton walk when that has derived one; otherwise the pov is the head joint itself plus the trims. The once-a-second line's `pov:` field says which ("derived" or "head joint + trims"). The walk does not run on foot, so the fallback is the usual case until it is wired there. (f) The runtime folds the applied offset into lookSeated; with look-to-aim on, the applied value is rotated with the look so the commanded seated offset is kept.
  - **Keys.** `native_on_foot_body_align` (off | on, live); `..._trim_right_cm`, `_trim_up_cm`, `_trim_forward_cm` (hidden, clamped to +-30).
  - **Risks.** The shoulder gap when looking down; the lean decouples (the HMD's own lean is in the error, so most of it cancels, but the trims and the pov are fixed by the rest offset); the stance swing moves the eye about 20 cm; option A (the body's own placement) later.
- **Ruled out (F31):** the game renders the right eye first in native on foot (F27 to F29). The visor is correct with first = left (Sean, F30 swap test), and the layer swap was persistent under first = right (F30 07:38:51).
- **Next flight (F31, when Sean says):** no `eye rule cockpit (first = left)` change lines; no stand-down at a mode edge (or the `re-armed at native render-mode edge` line when one happens); the visor correct with `remlok_swap_eyes = 0`; the 5 s `H_M vs C: angle` line at rest near 0 degrees and rising in a turn; `t' (written)` and `forward under H_M` side by side for the marker stutter.

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

- ruled out: back-face culling hides the native torso, because CULL_FRONT on the matched 37080 torso draw changed nothing (F18).

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
- **2026-10-14, F13-F15 FLOWN (Epic; log edvr_gfx_20261014_103132; build v0.19.1-262-g0762bc87,
  the merge of main).** HUD head-follow works (F13). The visor override (F13's visor_up) REMOVES the
  RemLok edge lines: they are the visor's. F15's hide_fp_body removes the FP arms; the legs left are
  the third-person body's, which is fine. Reticle probe: the only indicator node is key 10 (about
  90 calls a second; f30=1 f31=1 f32=0; camera-space position about 5 m out, drifting off-axis as the
  head turns away from the aim): the aim reticle. Layout probe: 17 HUD parts. Sean's tuning: distance
  0.03, size 1.50 (it hit the maximum), spread 0.15, smoothing 40 ms, follow head, order after.
- **2026-10-14, F16 built (NOT FLOWN).** Four changes, all on the native on-foot page.
  (1) Breathable visor: `fix.native_on_foot_breathable_visor` (up | game, default up) replaces the
  visor_up test key. While up, native stereo is on foot, and Status.json's Flags2 bit 16
  (0x10000, BreathableAtmosphere, Frontier's documented bit; not yet checked against a sample) is
  set, the F13 override code path holds the visor up (B8 = 0, then B7 = 1; restored when the
  condition stops). journal_watch.cpp reads the bit beside OnFoot and publishes the raw Flags2;
  on_foot_split_test checks the bit (R2h, R2i). The log line `native on foot visor: breathable air
  on|off (Flags2 0x...)` is the check: it should read on at a planet surface on foot.
  (2) Reticle follows: `fix.native_on_foot_reticle_follow` (aim | head, default aim). Under head,
  the F14 stub (installed by the probe key or by head) zeroes key 10's translation x and y, keeping
  z, w, the basis and the scale. Log: `native on foot reticle: follows head (key 10 centred)` / aim.
  (3) Defaults: distance 0.03 (the setting's fallback is kHudDistanceSettingDefault; the game's 0.192
  stays the scale denominator), size 1.50 with max 3.00, smoothing 40, spread 0.15.
  (4) HUD group offsets: eight keys `fix.native_on_foot_hud_<left|top|right|centre>_<x|y>`, -0.30..0.30,
  default 0, a fraction of the canvas (W = [[HRC+0x1B8]+0x3E8], H = +0x3EC). The layout hook
  (0x1A0DCD0) is now a writer: after the original returns it adds (x * W, y * H) to out[0] and out[1]
  by the part's group (native_on_foot_core.h hudGroupOf; unknown names get no offset and are logged
  once). The layout is re-sent when the on-state, an offset or the spread changes: one write of 0 to
  [HRC+0x290] (RE: the game sets it back to 1 at 0x1AB385B). The layout log line now prints on change.
  Read the flight this way: a moved group after a setting change means the re-send took; if the HUD
  does not move, the layout lines show whether the offset reached out[0]/out[1].
  identification is known.
- **2026-10-14, F16 FLOWN (Epic, log edvr_gfx_20261014_1209*).** The breathable visor works: in a station,
  Status.json's Flags2 reads 0x00016009 (bit 16 set) and the override holds. The HUD group offsets work.
  The HUD head-follow works (C_s from the list-B views, applied as M' = M . C). The reticle follow does
  not. F16 zeroed key 10's translation x and y, but key 10 is in the body/aim camera space, the same space
  the HUD matrices are in before C is applied, so zeroing x and y only centres the dot on the aim line.
  Feedback: the HUD at 0.03 is wanted closer still (the minimum is now 0.01); the element size needs its
  own control (F17). The body capture is still pending: the flight was in a station, not on a surface.
- **2026-10-14, F17 built (NOT FLOWN; commit c77e1ff2).** Three changes on the native on-foot page.
  (1) Reticle follows head (key 10 only, fix.native_on_foot_reticle_follow = head): the translation becomes
  d * row2(C) + row3(C), where d = |t| is the game's own distance, and each basis row (+0x10, +0x20, +0x30)
  is multiplied by the rotation of C, the smoothed head transform the HUD follows (g_f13Smooth, under
  g_f9Lock). The game's values are the base. A call that finds our values in place composes from that base
  again, so nothing compounds whether or not the game recomputes the instance each frame. Without a head
  transform (the HUD follow is body, or no list-B views yet) the game's values are left alone. Log:
  `native on foot reticle: follows head (placed along the head direction at d=..m)`, once per switch.
  (2) fix.native_on_foot_hud_distance minimum 0.03 -> 0.01 (the default stays 0.03).
  (3) fix.native_on_foot_hud_element_scale (0 = the game's own; 0.30..2.00; a value under 0.30 is read as
  0.30; F8 slider `HUD element scale (0 = game)` under HUD size). RE, the layout routine 0x1AB32F0: a
  non-zero byte at EliteDangerous64.exe+0x5F741CA selects the layout scale S = the float at +0x5E7A760
  (1.0 in the file); zero selects S = min(1, aspect / 1.7222). S multiplies every part's scale and its
  spread around the centre. While the setting is not 0, native stereo is on foot and the HUD mesh patch is
  in, the float is written k first and the byte 1 second, and the layout is re-sent through the existing
  [HRC+0x290] = 0 path (the layout stamp now includes the element scale). It is put back (byte 0, then the
  float 1.0) when the setting goes to 0, native ends, the session ends, or the game shuts down. Both values
  must read as the game's own (0, 1.0) or ours, or nothing is written. Log lines: `native on foot hud element
  scale: <k> (override 0 -> 1)`, `(was <k>)`, `restored to the game's own`, `NOT written -- <why>`, `NOT
  restored -- <why>`.
  Read the flight this way: `override 0 -> 1` appears only with the mesh patch on and native on foot. A change
  of the element scale should move the elements toward the centre and scale them, not only their margins
  (spread). The layout re-send line prints the element scale it asked for. Unverified: the 0x1AB32F0 branch
  is from the design notes and was not re-read in this build; the first flight checks it.- **2026-10-14, F17 FLOWN (Epic).** The reticle on the head works (`follows head`
  line, the dot stays with the head). Sean: aim is the F16 behaviour (key 10's x and y zeroed), so F18 makes
  aim do that and head stays F17. HUD clip: the clip is the canvas edge (stage 2008x1206, RTT 4915x2952);
  enlarging the canvas adds no room, so "HUD room" couples the element scale and the mesh size (F18).
  Body capture: the local third-person body is 5 draws (the keys in F18); the torso is issued every frame and
  the census has no cull state for these draws. The head
  position sits in front of the torso: F18 probe P1 reads the camera position the visibility job uses
  (EliteDangerous64.exe+0x3DA2C80) to size an eye offset.
- **2026-10-14, F18 built (NOT FLOWN).** Four changes, all on the native on-foot page.
  (1) Reticle follows aim (default) now zeroes key 10's translation x and y, as F16 did; head is F17. The
  reticle stub is installed whenever native stereo is on foot. Log: `native on foot reticle: follows aim
  (key 10 centred on the aim axis: x and y zeroed)`, once per on-foot start.
  (2) Body torso cull, TEMPORARY `fix.native_on_foot_body_cull` (game | none | front). The local third-person
  body is five draws (the Status block). Match: DrawIndexedInstanced, one instance, the two shader pairs at
  their index counts, and stencil ref 21 read with OMGetDepthStencilState only after the hashes and count
  match (the binding shadow does not track it). The swap is a wrapper around forwardWithVerdict in vscreen.cpp
  (not a DrawVerdict, so engine velocity sees the same draws); the cloned RS (cached per game RS and mode,
  16 slots, shutdown release) differs only in CullMode; the game's RS is restored after the draw. Count line
  every 5 s, zero included: `native on foot body cull: <mode> matched N draws (by index count: 13260=..,
  30216=.., 23784=.., 7692=.., 37080=..)`.
  (3) HUD room, `fix.native_on_foot_hud_room` (1.00..2.50, live, F8 slider under HUD element scale). With the
  element scale at 0 the base is the game's own S read from [HRC+0x4C4] while our override is off (RE, not
  re-checked), so the room waits until S has been read. k_eff = base / room (clamped to 0.30..2.00); the mesh
  is multiplied by the room in f13PlaceHud (g_hudRoomEff). Log: `native on foot hud room: <r> (element scale
  <k>, size <s>)`, on change.
  (4) Probe P1 (TEMPORARY, no key): the avatar visibility job at EliteDangerous64.exe+0x3DA2C80 (rcx = AMC,
  rdx = job context). Prologue verified in the exe, 32 bytes, steal 5 (mov [rsp+0x10],rdx). The callback
  samples the local third-person AMC (explorer_cam's latch, localAmcPointer) once a second before the
  game runs, reads the camera from [rdx] and the AMC's world matrix (origin +0x30, basis rows 0-2), and takes
  the head from the neck census through that matrix. The camera and head are then expressed in the AMC's
  axes. Log: `native on foot eye vs body: cam_in_body=(..) head_in_body=(..) cam_minus_head=(..)`, one line a
  second while native stereo is on foot. Not checked in this build: the claim that no branch lands in
  +1..+16 of the entry; the head-through-the-AMC mapping is the assumption it tests.
  Build: the full build.bat gate passed (the self-tests' injected failures are the expected ERROR lines).

- **2026-10-14, F18 FLOWN (Epic; log edvr_gfx_20261014_141730.log).** Results, in order:
  (1) Torso: not visible with the front-face cull, so back-face culling is ruled out:
  `ruled out: back-face culling hides the native torso, because CULL_FRONT on the matched 37080 torso draw changed nothing (F18)`.
  (2) BUG: the `none` mode matched 0 draws (its count line says 0), so no `none` result is evidence. The F18 code is left as it is; the cull is superseded by F19.
  (3) The eye dump shows the full HUD. (4) The weapon floats with the first-person body hidden, so the weapon-attach RE is running.
  (5) Sean wants the game's ADS kept (a hybrid is planned). (6) P1's eye-vs-head values are implausible; not used, and H-eye stays open.
- **2026-10-14, F19 BUILT (NOT FLOWN).** Four changes on the native on-foot page; all temporary keys are live and default off.
  (1) Body unclip, `fix.native_on_foot_body_unclip` (on | off). The modifier that the game's item-apply function gives each body part, `pVertexClipAmount`,
  sits at part+0x610 (part = AMC + 0x11B0 + idx*0x680), with the float amount at +8, the enable byte at +0xC and the dirty byte at +0xD. The shader
  (PS 0DF03E64DF9DBEF1) discards a pixel whose clip value exceeds cb2[5].w, so enable 0 keeps those pixels. The write is after the fade
  returns, on the same thread, to the local third-person AMC only (identified as headHideStep does: isLocalAmc, mode 3). Parts by name:
  EVASuit_Torso, EVASuit_Arms, EVASuit_Hands, Shoulder_Left, Shoulder_Right.
  Verification in the exe (EliteDangerous64.exe build 332841, capstone, item-apply 0x3DD9EA0; scratch script, not in the repo):
  - `3DDA307: mov qword ptr [r14 + 0x610], rsi` (the modifier is stored at part+0x610; r14 = the part entry)
  - `3DDA30E: mov byte ptr [rsi + 0xd], 1` (dirty set on the new modifier)
  - `3DDA36A: movss xmm1, dword ptr [rdi + 0x54]` and `3DDA37B: movss dword ptr [rsi + 8], xmm0` (the amount, min of the source and xmm7)
  - `3DDA3B1: movzx edx, byte ptr [rbp + 0x321]`, `3DDA3B8: shr dl, 3`, `3DDA3BB: and dl, 1` (enable = [AMC+0x321] bit 3)
  - `3DDA3BE: mov rax, qword ptr [r14 + 0x610]`, `3DDA3C5: mov byte ptr [rax + 0xc], dl` (the enable byte at modifier+0xC)
  - Switch: the table at 0x3DDA8F8 / 0x3DDA8F0 resolves parts 17, 19, 21-23, 27-29, 33-42, 48 and 50, 51 to 3DDA3B1 (bit 3). Parts 30 (EVASuit_Legs),
    31 (EVASuit_Feet) and 49 (FirstPersonSkeleton) resolve to the default 3DDA3BE with dl = 1 from 3DDA388 (always on). The verification passed, so the
    code was written.
  - Not checked: at 3DDA35A the amount's pointer is reloaded from [AMC+0x17C0 + rax]. 0x17C0 = part 0's +0x610, so this is the same part's modifier
    if rax is idx*0x680 (inferred, not read).
  (2) Clip probe (no key; while native stereo is on foot, from the same hook): every 5 s, the five parts plus EVASuit_Legs, EVASuit_Feet and
  FirstPersonSkeleton where the AMC has them (`native on foot body clip: <part> mod=.. enable=.. dirty=.. amount=..`), and the flags once per AMC.
  It reads before that call's write, so enable 1 at a probe means the game re-applied after our last write.
  (3) Constants probe, `fix.native_on_foot_body_cb_probe` (native or Explorer Cam on foot, by the journal). binding_shadow keeps the
  buffer pointers, not the bytes, so there is no CPU copy to read. The draw thunk matches the body draws (the F18 pairs at any index count,
  one instance, stencil ref 21) and, once per 5 s per (VS, PS, index count), copies the first 352 bytes of PS cb2 to a staging buffer. The next
  matched draw maps it with D3D11_MAP_FLAG_DO_NOT_WAIT (no stall; a "still drawing" result waits for the next draw). Rows 5..9 go to a queue the
  frame thread logs: `native on foot body cb2: vs= ps= n= r5=(..) .. r9=(..)`.
  (4) HUD defaults from Sean's tuning, in the repo ini and the code fallbacks: element scale 1.35, size 1.60, distance 0.01, spread 0.04,
  left x 0.18 and y 0.02, top y -0.07. Room stays 1.00. The live ini is not touched.
  Reads of the F19 flight: `native on foot body unclip: on` once, then `N modifier writes in the last 5 s` (N > 0 while the body is drawn);
  the clip lines with enable 0 for the five parts (and enable 1 for the comparison parts, always); the cb2 lines, row 5 w against the clip amounts.
  Trace (before the flight): with the key on and no local AMC at the hook, the log says so once after 5 s (`has not reached the fade hook`);
  with no matched body draw, the cb2 probe logs `0 body draws matched in the last 5 s`. A silent probe is therefore not a quiet one.
  Build: source commit eab40235. The full build (`build.bat --wait-lock`) passed twice; the second run added the two trace lines above.
  Receipt: `build\full_build_receipt.json` written at 15:35 for this source tree (docs are not receipt inputs).

- **2026-10-14, F20 BUILT (NOT FLOWN; flies with F19).** The game's own model mode, with the native first-person view kept.
  Approved by Sean. Temporary key `fix.native_on_foot_body_mode` (game | third; default game; F8 row on the native on-foot page).
  RE, build 332841 (checked in the Epic exe with capstone; the scratch script is not in the repo):
  - `HumanoidModelManager::SetMode` is at EliteDangerous64.exe+0x1A112B0, vtable slot 0x30 of the manager whose vtable is +0x5200F90. Its first
    22 bytes `40 53 48 83 EC 20 48 8B D9 39 91 30 02 00 00 75 05 45 84 C0 74 1F` (push rbx; sub rsp,0x20; mov rbx,rcx; cmp [rcx+0x230],edx; ...).
    Args: rcx = manager, edx = mode (0 first-person models, 1 third-person), r8b = force. The mode is stored at [manager+0x230].
  - The per-frame hook is the povCamera joint handler at EliteDangerous64.exe+0x19B1240, entered for every humanoid. Its first 32 bytes
    `40 53 48 83 EC 20 48 85 D2 48 89 74 24 30 48 8D B2 B0 01 00 00 B8 A0 02 00 00 48 0F 44 F0 48 8D` match the brief. The manager is
    `[rdx+0x1B0]` (rdx = the receiver). Local player only: `[manager+0x238] != 0` and `[manager] == base+0x5200F90`.
  - Conflict check: Explorer Cam's FindJoint sites are return addresses +0x19B12D5 and +0x19B1359, both past the 6 bytes this hook steals
    (+0x19B1240..+0x19B1245, `push rbx; sub rsp,0x20`). No other hook in the tree touches either entry.
  Mechanism: the SetMode detour (a callback relay, ecm::buildCallbackRelay) records the game's requests for the captured local manager and,
  while the body is wanted as third person, forwards a request for 0 as 1 (as 0 while aiming down sights; ADS is Status.json Flags2 bit 5,
  `journalAds()` beside the F16 breathable flag). A request for 1 is never changed. The povCamera hook runs the original first, then holds
  the local manager at the desired mode through the SetMode trampoline, so EDVR's own calls never reach the detour. When the hold ends
  (key to game, native ended, session ended, shutdown) the game's own mode is restored once. The game's mode is the last game request,
  or the value read before the first hold when no request was seen. A captured manager not seen for 60 frames is dropped without a restore.
  The two hooks install when the key first reads third, after the identity check, the vtable slot check and both prologue checks; the
  relays are placed within two gigabytes; nothing is patched if any check fails. Flat profile: refused.
  Logs (the hook threads never log; they queue, and the frame boundary writes):
  - install: `native on foot body mode: installed -- SetMode at EliteDangerous64.exe+0x1A112B0 ..., stolen=N bytes, prologue 22 bytes verified, relay at 0x.. (thread T)` and the povCamera line with 32 bytes.
  - `native on foot body mode: game asked M (thread T) -> forwarded M'` (on a change only)
  - `native on foot body mode: set M (ADS a, frame thread T2)` (each EDVR call)
  - `native on foot body mode: restored to M (reason, frame thread T2)`
  Read it this way: a `game asked 0 -> forwarded 1` with `set 1` and no `restored` is the hold working; an install line with no `set` line
  means the local manager was never captured (the `local third-person humanoid seen` line says it was).
  Note: the first run of this build will not show the body's model change in the logs, only the calls; the visible result is the flight's.
  F19 keys are unchanged. F19 and F20 fly together: the body clip (F19) and the model mode (F20) are separate hooks.
  Build: source commit 816c1d80. The first full build failed on two duplicate `flags2` declarations in journal_watch.cpp, left by merging main into HEAD (65cdfc7b); the commit removes the duplicates (identical copies). The second full build passed (`all gates passed`, receipt written). The shared build lock was held by other worktrees for about an hour before it started.

### 2026-10-14: F19/F20 FLOWN, F21 BUILT

- **F19/F20 flight (log edvr_gfx_20261014_172807.log).** Third mode (F20) gave the full third-person body and the weapon in the hands, correct. The HUD was lost and there was no ADS pose: the Flags2 ADS bit (0x20) was never set in third. The game itself set the torso and arm vertex-clip enable to 0 on the third-person Torso, Arms, Hands and Shoulders in third mode and back to 1 in first-person mode. That confirms F19's unclip mechanism. The pilot prefers first-person mode with a hybrid body (F21).
- **Ammo display and waypoint.** Both follow the head. The first-person rig is anchored to the head camera, and the HUD canvas icons are world-projected. Research running.
- **Indicators on a planet.** Key 10 (about 5 m, the aim reticle and weapon display) and key 43 (waypoint, about 380 m). The reticle writer touches only key 10.
- **STEP 0 (merge check).** The full build on the merged HEAD 2cf59dbb passed unchanged; no merge fix was needed. The `FAIL:` lines printed by the screen-motion mutation run sit inside the gate's own output and the gate passed.
- **F21 (commit a68ba714, NOT FLOWN).**
  - Reticle `game`: EDVR writes nothing to key 10. The stub stays installed so a head placement made before a switch is handed back once.
  - Hybrid body `fix.native_on_foot_body_hybrid` (TEMPORARY, default off): the first-person skeleton's masks on the mode-2 AMC; the third-person torso unclip and the arms, hands and shoulders' masks on the mode-3 AMC. It overrides the two body keys while on.
  - Eye probe P1: raw-inputs line added (`native on foot eye inputs:`).
- **P1 eye-vs-body: bug not found.** Read against Explorer Cam's convention (explorer_cam_core.h: +0x3B0 rows right, up, forward; origin in row 3; dot with rows), the probe's arithmetic is right: the AMC matrix is row-vector with its origin at +0x30, and the head translation is read from row 3 of the +0x58 matrix. The open candidates, each tested by the new line:
  - (a) Frame of the job's camera floats. Explorer Cam's commander frame has its own `root` (commanderFrame: root = W.origin - L.origin x F), not the AMC origin. If the camera floats are commander-local or rebased, `|cam - origin|` will not be about eye height.
  - (b) Frame of the head joint. The +0x58 matrix is avatar-root space, and the AMC frame equals that root only if the root sits at the AMC origin. The line's `|head_model|` (the standing head is about 1.66 m in root space) must match `|head world - origin|` if the basis is orthonormal.
  - (c) The head sample is up to 100 ms old, which is too small to explain 0.7 m.
  - Scale: the row lengths in the line show whether the basis is unit length.
- **Next.** Flight F21 on Epic with `reticle_follow = game` and `body_hybrid = on`. Check the census lines and the `eye inputs` line on foot.
### 2026-10-14: F22 built (commit a2a544d4)

- **Part A, sessions.** `journalOnFoot()` reads Status.json Flags2 bit 0 (cleared when the file has no Flags2), so the fallback is the same signal the log already showed. `journalSessionOrOnFoot` admits on LoadGame or on foot now. Native on foot (setter and frame) and the Explorer Cam hotkey use it. Other `journalGameplay()` users are unchanged.
- **Part C, look to aim.** Bytes checked against the Epic exe with capstone before writing: update +0x19973D0 (32 bytes, steal 7), processor +0x1A9C370 (5 bytes, steal 5, single exit; store of the yaw delta at +0x5F4 and pitch at +0x5F8; basis rows 4-float stride at +0x5B0). The processor's return is unused at its call site (+0x1A19626), so a void wrapper is safe. The relay only uses RAX, so xmm1 (dt) reaches the callback.
- **Runtime.** Frame-output version 6 adds `lookYawTotal` and `lookYawGameFrame`. The client asks 6 first and steps down; test rigs (native_frame_test, comfort_fade_cases, explorer_cam_test) follow the new sizes. The compensation is a rotation about the head applied to a separate seated pose (`lookSeated`); `currentOrigin` stays the seat memory's origin, and `applySeatedResetCore` re-bases the total. A `look_to_aim_runtime` trace line carries the runtime frame and the game frame.
- **Build.** First two gate failures were mine and are fixed: the settings schema needs `live` on every temporary key's `ui:` line, and a trace format string had a raw newline. The full build then passed ("Native OpenXR build and all gates passed", receipt 88c79c36...).
- **Not verified in a headset.** Whether the runtime's yaw sign matches the game's (flight check (a) above), the processor's delta semantics beyond the store, and the gain's linearity. Each is a line in the flight log, not a claim.
- **Ruled out (static, Part D):** the game camera includes the HMD pose and the first-person rig reads it. Open hypotheses H1-H3 are in the Status block; probes A and B are the test.

### 2026-10-14: F22 flight and F23 built

- **F22 flight (19:15).** Part A confirmed in Training. Part C spun the commander: the first engaged error read -170.73 deg, and centred-head samples read ±160..180 deg, so forward was the backward axis (atan2(C[8], C[10]) is backward). Fixed in F23 as forward = -row 2. Ruled out: head-relative yaw = atan2(row2.x, row2.z) of C, because a centred head read ±170..180 deg (F22). The basis element swung -42 -> +38 with the view pitch near 0, so the pitch probe read a non-pitch element. The hybrid body (F21) gave Sean a torso at 19:20:20; the arms are the open question.
- **F23 (built, commit 1abc4899).** Yaw convention, rate cap 180 deg/s × dt, the runaway guard (270 deg in 3 s; at cap 1 s with the error not falling; stays off until the key toggles or on foot re-enters), the compensation sign instrument and `fix.native_on_foot_look_to_aim_comp_sign` (TEMPORARY), the full-basis pitch probe.
- **Part E (built, same commit).** Read-only stage timers on the load dispatcher, EliteDangerous64.exe+0x1CA6510 (16 bytes verified; steal 8; relay preserves rdx and xmm2; RAX only). Table at +0x5E7B460, 21 entries, stride 0x30; per-stage record at obj+0x2B0+0x18*i (int state 0 waiting, 1 running, 2 done; float start; float end); clock at obj+0x4C4; net gate dword at [obj+0x4B8]+0xC0 (>= 5 to start a stage). The stage names, in table order:
  0 WatchForQuit, 1 EstablishServers, 2 WaitForEDServer, 3 EnsureEDServerConnected, 4 CheckForDisconnectRequest, 5 PreviousLocationShutdown, 6 DestinationLocationFailure, 7 RootDataValidation, 8 VSOCalculation, 9 UpdateSkyBox, 10 ProvingGroundSetup, 11 ProvingGroundCheckForAbortLoad, 12 CreateLocationObjects, 13 CreatePlayerVessels, 14 WaitForPendingAISpawns, 15 EnterMultiCrew, 16 CreatePointOfFocus, 17 PlanetarySurfaceGeneration, 18 PersistentAsteroidGeneration, 19 WaitForRemoteMachines, 20 EnterLocationFinalStep.
- **Not verified in a headset.** The runtime sign (comp_sign), the controller's error after the convention change, and whether the guard holds under a real turn. Each is a log line in the F23 flight check.

### 2026-10-14: F23 flight, F24 unbuilt

- **F23 flight (19:40).** The guard tripped 1.5 s after engaging. Raw C row 2 = (-0.0613, -0.1530, 0.9863) with the head centred: forward = -row 2 read 176 deg, +row 2 read -3.6 deg. So F22's convention was right, and the F23 diagnosis was wrong. Ruled out: forward = -row 2 of C. F22's spin is most likely the yaw sign (the error grew to the wrap, which is the signature of a turn opposite to the input).
- **Load timings (Training).** Network stages 0 s; pipeline about 8 s (CreateLocationObjects 2.76 s, CreatePlayerVessels 3.69 s); 31 s between the menu closing and the first dispatcher call, with the game presenting at 89 fps. The F24 load sampler is the instrument for that gap.
- **Waypoint and ammo.** They move with the head with every setting, HUD follow off included. Ruled out: EDVR's HRC rewrite moves the waypoint. A Sonnet RE is on it.
- **Part F (F24, unbuilt).** Probe 3's key 10 and key 43 rows and the FOV scale inputs; the scale instructions are at 0x1A14F33 and 0x1A14FBE (the camera-space branch). Ruled out (static): 0x591F30 gives FP buckets a special view in HMD.

### 2026-10-14: F24 merged, built, installed

- The merge brought six small src changes from main (log, menu, perf, flat capture, native_trace). Nothing on-foot. Build: "all gates passed", receipt 9d2b8e75. Install: Epic `--scope vr`, verified (`edvr_native_receipt.json.pre-741130be-...`). The F24 flight is the next check: the look convention and the passive sign check first, then the load sampler's top locations during the 31 s gap, then probe 3's key 43 scale with FOV -1.

### 2026-10-14: F24 launch crash

- **Ruled out:** EDVR's HRC rewrite moves the waypoint (see above). Ruled out (static): 0x591F30 gives FP buckets a special view in HMD.
- **F24 launch crash at +0x59F1B0 (probe 1, see diagnosis):** exception 0xc0000005 in the 0x40-byte struct copy at +0x59F1B0 (movups [rcx] <- [rdx]), reached from the composer's caller at 0x281B1C1. The composer (0x283D4C0) reads nine stack arguments (entry+0x28 .. +0x48); the probe forwarded four. Probe 2's bucket prep takes rcx/rdx and reads no r8/r9 or stack argument before writing, so it is not the cause on the evidence; it is still never installed.
- **Sentinel:** the stale `edvr_logs\d3d11_hooks.armed` disables the hooks at the next launch. The installer does not touch edvr_logs; it was deleted after the install (the file alone).

### 2026-10-14: F25

- **Training load is blocked before the pipeline.** The netLog: ConnectToServerActivity at GettingServerAddress from 40.4 s; the menu closed at 45.2 s; no dispatcher call in the 55+ s after. So the wait is on the matchmaking server address, before any stage starts. The load sampler census is the instrument for the CPU side of that wait.
- **Runaway trip and reset:** the F24 flight's guard tripped at 20:58:43 with yaw_sign +1 (the body turned all the way back). The reset serial now returns the compensation to 0 on every disengage; whether the compensation reaches the game's head pose is the open question the seated-yaw trace answers.
- **Visor:** VS B553BB479B7C0B97 confirmed by census_skip; on-foot match added; A/B written as a hidden key.

### 2026-10-14: F26

- **Phase 0 (eye check):** the 'eyes swapped' stand-down at each Cinema/native switch was permanent for VR. F26 logs the compared pointers, re-arms the eye check on a native on-foot change, and stands down only for a swap that persists 30 composites after a re-arm. The flight will show which case happened.
- **Sharp on-foot HUD (design: Sonnet pass, notes in scratchpad\hud_sharp):** the on-foot family rides the cockpit's HDR take. Full hashes from the logs (not truncated ones). D005EBB14A22EA0E (text) is not taken yet.
- **RemLok on foot:** the F25 match never fired. F26 keeps the VS and the shape, and adds a diagnostic for the rest; the cause is the next flight's diagnostic line.

### 2026-10-15: F26 flight, F27

- **F26 flight (edvr_gfx_20261015_043326.log, Training, native on foot).** The eye check's stand-down came at the render-mode switch, before native on foot reported active, so the F26 re-arm was not in force yet. In native the game renders the right eye first: the rule (first target = left) was backwards for that mode, which also put the visor's nasal side on each eye. The on-foot RemLok match fired (5009 per 30 s) with the wrong parity. The waypoint fix read no C with HUD follow off.
- **Ruled out:** the cockpit's eye rule (first target = left) in native on foot.
- **F27 (built, not installed).** The eye order (eyeIndexFor in ui_depth.cpp, and the RemLok parity via remlokSwapNow) flips while the setter has native on foot substituted; the flip and the mode edges are set at the setter's mode switch, and each change is logged. The eye check re-arms on those mode edges. C for the world markers comes from the list-B sample whatever the HUD follow is. RemLok logs left and right match counts per 30 s.

### 2026-10-15: F28

- **Training connect probe (read only).** The CTSA's entries and state, the state setter, ReadyToAllocIDs, the menu's Deactivate and SpecialGameModeManager are probed behind `training_connect_probe` (default on, read at launch). Integer-argument hooks forward the registers and four stack slots; Run and SGMM (dt in xmm1) go through a shim that moves xmm2 to xmm1. Verdicts are in the Status block.

### 2026-10-15: F28 verdict, F28 v2, F29 (built, not installed)

- **F28 verdict (flown 10-15).** The menu exit Stops and destroys the CTSA after Succeeded at 05:32:31.39, 1.7 s before Deactivate. SGMM polls ReadyToAllocIDs about 90 times a second with no timeout. The stuck case is H1: a click mid-handshake kills the CTSA and SGMM waits forever. Open option: a timeout on the ReadyToAllocIDs wait, pending RE of the wait.
- **F28 v2 (built, not installed; read only).** Adds the ClearAll and Remove hooks. Their prologues were read from the Epic exe: ClearAll 20 bytes, Remove 19 bytes, whole instructions, no calls or RIP-relative bytes stolen. The services at [act+0x40..+0x98] are twelve qwords at stride 8, not fourteen. The StartTutorial fire is inferred from the change to state 1, read after the call; the fire point itself (+0x1D1D1D8) is not hooked.
- **F29 (built, not installed).** The eye rule is evaluated from the actual state (Status). The waypoint C age and the eye difference log every 5 s. Item 2's same-frame head sourcing is deferred.
- **Carried forward from the old Status (open, not re-tested):** F22 FP-root hypotheses H1 (picks up head yaw by another path), H2 (body-anchored root with a head-locked bucket view), H3 (EDVR's view writes touch the FP weapon camera). F21 eye candidates (a) the camera floats are not in the AMC origin's space, (b) the avatar-root frame is not the AMC frame, (c) the head sample is up to 100 ms old. F19 open: H-clip, H-threshold, H-eye, H-room. F21 next check: how the arms join the torso. F24 scale candidate (static): force the scale to 1 in HMD on foot; the instructions are at 0x1A14F27, 0x1A14F33 and 0x1A14FBE, with xmm0 and xmm6 not decoded. Origin: a user's handoff (a Cinema-to-HMD swap on disembark gave native stereo on foot; HUD, radar, left panel and inventory missing). Build: Sean's installs hash to the handoff's exe (332841).

### 2026-10-15: F30 (parts 1 and 2)

- **Verified in the Epic exe before the code.** SGMM (+0x1D1CDF0): the state-2 path is the one that waits; the exit at +0x1D1D1E2 returns `sete al` of (state == 0), with r15 = 0 from entry, so state 2 returns 0. The hold returns the same value and skips the body. AM Update (+0x4C8EEA0) prologue `41 56 48 83 EC 60 48 89 5C 24 70`, 11 bytes, whole instructions. SetInit (+0x4C8E500) takes (set, count) and allocates the set's buckets; SetFree (+0x4C8EA60) frees only the set's own array (a tail call when the array is not the inline one); Insert (+0x4C907A0) takes (set, key); Instantiate (+0x4C8F960) takes (AM, set); StartPending (+0x4C90B90) takes the AM. Caller setups for SetInit, Insert and Instantiate match (set on the stack; AM in rcx). The key base+0x5F7D7F8 lies inside the image; what it names is the RE's, not checked here.
- **Not verified:** whether Instantiate keeps a pointer to the set after it returns (the set is freed right after), whether Update's dt is used (no xmm1 reference in its first 60 instructions), and the P = node+0x50 layout. The recovery has not run in the game.
- **Part 2 metrics (from the coordinator's diagnosis):** the stand-down was one swapped composite 33 ms after a setter call (incoming 6, Training 4 to 6 to 4); the eye rule flipped a second later, and the re-arm keyed on the rule's flips only. The C age was tick-based and is replaced by QPC.
- **Open:** step 2 (the runtime's "now" pose) not started; the visor is unchanged until Sean's swap test; same-frame sourcing is now in (part 2), its effect on the stutter is not yet flown.

### 2026-10-15: F31

- **Evidence (F30 flight 073724, Epic).** Sean: `remlok_swap_eyes = 1` fixes the visor under the native rule, so first = right was wrong for the visor. The UI layer took the on-foot HUD at 07:38:46 under the native rule (07:38:46.259) and stood down at 07:38:51.183 on "eyes swapped after a native on-foot re-arm (persistent)". Under the cockpit rule the earlier stand-downs were one-composite blips at a mode transition (4 to 6 to 4 in Training). Conclusion: the game renders the left eye first in native on foot too; the single-composite mismatches were transients, now absorbed by the 30-composite tolerance and the edge re-arm.
- **Changes.** The eye-order flip is removed (native_on_foot.cpp, native_on_foot.h, remlok_fix.cpp, ui_depth.cpp); the rule logs as cockpit (first = left) at each render-mode edge. The sharp HUD diagnostic prints the key and the active state separately (F30 part 2).
- **Runtime "now" head (version 7 ladder step).** `now_head_publication.h` (openxr) holds the last answer to Elite's "now" call, published from `OpenVRSystem::GetDeviceToAbsoluteTrackingPose` for the display calls. The client sends it in EdvrNativeFrameInput version 2 (`nowHead`, `nowValid`, `nowSequence`); a provider that refuses version 2 gets the version 1 words, as before. The d3d11 provider keeps the frame's render head and the now head, read by `nativeFrameNowPose`. Version 1 inputs are read only through their words (the size is the prefix, or the full size as the rigs send it).
- **Verified by reading.** The "now" answer is `rigid(head.pose)` of the seated-space head located at the answer's instant, the same construction as `physicalHead` (`detail::rigid` of the geometry head pose, also seated). The post stub's marker and the list-B head are the same frame (F30 part 2).
- **Not verified:** the runtime answer's space matching the render head's in every origin change; H_M's sign; the flight decides both (the angle at rest and during a turn).

### 2026-10-15: F32 (the F31 flight, the probe v4, the now-head diagnosis)

- **Flight 081349 (the F30 build).** The sharp HUD is FIXED (Sean: clear again). forward_now never ran: the live `edvr.ini` says `forward`, and the now head was read only in mode 3. The runtime does answer and publish the "now" calls (the first sighting at 14:13:49.960). Whether the provider accepted version 2 cannot be read from the logs (no line), so the provider's optional-head refusal is removed and the negotiation is logged.
- **Training flight 081034 (the F31 build).** The recovery reconnected at click+35 s; the load had stalled at about click+20 s. SGMM Run is never called in this flow: the hold and its hook are removed. Attempt 1 now fires at 8 s regardless of the link; attempt 2 is link-gated.
- **Verified in the Epic exe.** The started insert (+0x4C8E020) prologue is 18 bytes, whole instructions; its r8 is node+0x10 (the caller at +0x4C90BC7), and the act is at node+0x18 (the StartPending loop calls the vtable on `[rbx+0x18]`). +0x1D1D220 is 16 bytes (the brief's 15 splits an instruction); its first instructions are the pushes and `lea rbp,[rsp-0x27]`, and it reads its dt from xmm1.
- **Not verified:** the activity name stub's shape (the probe checks `48 8D 05 rel32 C3` at run time and prints "?" otherwise); the shortcut's Init sequence (the probe logs it); the flight decides both.

### 2026-10-15: F33 (body align)

- **Design (overseer, Sean approved).** Move the seated eye to the third-person body's pov by a runtime seated translation, with no game writes. Measured: cam minus 3P head in the body frame, median (-0.03, -0.10, +0.34) m (log 081348); the FP AMC root is the camera within 1 to 5 cm after spawn; use the local AMC, not rig probe B's mode-3 NPC.
- **Built.** Output version 8 (`eyeAlign`, `eyeAlignSerial`) and the runtime fold (translation through `seated.replace`, reset on rebase); the d3d11 controller (`alignTick`) with the integral, the critically damped filter, the clamp, the cap, the hold and the guard; the rest pov offset published by Explorer Cam's walk (`explorerCamRestPovDelta`); the keys, the install, the waiting lines and the once-a-second line.
- **Not done, and why.** The rest pov offset is derived only by Explorer Cam's skeleton walk, which does not run on foot: the controller waits (logged) until it does. The mode-2 root cross-check and the skeleton-slot head read are not wired. The body-to-seated sign and order are unverified and the guard is the check.

### 2026-10-15: F34 (native on foot key cleanup)

- **Removed (code and ini).** `fix.native_on_foot_method` (the swap path: the setter's substitution, the admission reasons Admitted, SelectorInvalid, SelectorMismatch, NoGameSession, and the flag block; the gate is the only method). `fix.native_on_foot_hud_mesh_world` (the 0x1A36453 74 -> EB placement patch). `fix.native_on_foot_hud_follow_order` (the "before" composition; "after" is the behaviour). `fix.native_on_foot_reticle_probe` (the stub is on while native stereo is on foot). `fix.native_on_foot_body_cb_probe` (the constant-buffer 2 probe and its draw-path copy). `fix.native_on_foot_view_probes` (view probes 1 to 3, the driver probe A and its sink; the always-on marker entry 0x1A14D80 stays for the world-marker head read). `fix.native_on_foot_visor_shift` (the 0x5FCA8B0/B4 writes).
- **Folded.** `fix.native_on_foot_hud`, `_panels`, `_hud_mesh`: native_on_foot on implies all three (each write still verified). `fix.native_on_foot_sharp_hud`: always on in native on foot.
- **Retired keys.** Kept hidden in edvr.ini at their last shipping values, so a stock file reads clean; a value that differs from that is named once in the log ("is retired (F34): ignored").
- **New shipping defaults.** Element scale 1.15, room 1.25, left x 0.00, top y -0.09, right x -0.07, follow smoothing 0. The others were already Sean's values.
- **F35 (BUILT, NOT INSTALLED; `fix.native_on_foot_body_align` = off | view | body, live; default off).** Body shift, option A of the hybrid alignment: the third-person body's render records move by the world offset d, so the body's pov goes onto the first-person camera. `view` is F33 (the seated origin, through the runtime); `body` is F35. The trims apply to both.
  - **Hook.** Post-hook on the submit job 0x3DA1DE0, 31-byte prologue verified, 8 bytes stolen (`mov rax,rsp; mov [rax+8],rcx; push rbx`, whole instructions). The wrapper forwards all eight argument slots; the original runs first. Only the local third-person AMC (the fade hook's match), only when `[AMC+0x323] & 4`. For each (block, slot) with a non-zero instance pointer, a non-zero mask and `[inst+0x1A0] != 0`, a record whose xyz at +0x10 equals `AMC+0x2E0` (bitwise) gets d added to +0x10 and +0x124. Records that do not match are skipped and counted. Quaternions are not touched.
  - **Controller.** The error is (cam - pov) minus the trims, minus the offset already applied. The measured pov is unshifted (the render records move, the head joint does not), so the applied offset is subtracted to close the loop; without that the integral would wind up. The integral, guard, filter, clamp and cap are F33's.
  - **Hand-off.** d is a seqlock (three atomics and a sequence). The hook runs on the game thread for each AMC; the frame thread writes once a frame.
  - **Deviations and unverified.** (a) The brief's "+0x10..+0x18 equals +0x2E0..+0x2E8" is compared as the full 12 bytes (+0x10..+0x1B), the brief's own xyz. (b) The steal is 8 bytes and the prologue check is 31 bytes; the brief's list is 31, not 32. (c) "Mode 3" is the fade hook's local-AMC match (localAmcPointer), not a separate mode field. (d) The records' offsets come from the RE; the flight checks them through the skipped count. (e) VS 9D800F45B12A6AAC (the torso pieces) is not dumped, so its use of the t33 +0x10 is unverified: the eye dump will show whether the torso moves with the rest.
  - **Risks.** The shift moves the rendered body, so the shoulder gap and the stance swing come with it (F33's list); the lean is not subtracted in body mode (by design: the body goes where the camera is).- **F36 (BUILT, NOT INSTALLED; `fix.weapon_stability` untouched, default on, live).** While native on foot is active, weapon stability steps aside: no deferred frame pacing (native_frame.cpp: `deferredPacing` is 0) and the weapon's temporal-AA motion-vector gate reads as off (screen_motion.cpp: the weapon gate and `weaponMotionConfigure`, re-applied every frame). In Explorer Cam, the stock cinema view, or with native on foot off, it behaves as before. One line per change: "weapon stability: stepped aside (native on foot active)" / "resumed".
  - **Every reader of the key:** native_frame.cpp (pacing: changed), screen_motion.cpp (motion-vector gate: changed, and now per frame), vr_world_route.cpp (a comment only: unchanged), config_test and screen_motion_test (the default: unchanged), native_frame_test (a stepped-aside case added, after the F33 block so the sequence floor stays monotonic).- **F35 flight record (Epic, 091322).** The headset was resting on the desk for much of it: the start error was 101 cm, and no eye dump was taken during the align. The view mode's HMD term read 66 to 101 m (list-B's rebased frame), which made the error garbage and the guard trip on the head's own motion.
- **F37 (BUILT, NOT INSTALLED).** View: the HMD is the head's motion only: tS = the render head's translation less tS0 (captured when the view starts and at each reference generation, from nowPose.referenceGeneration), mapped to body axes (v = R^T tS, dw = C.rot v, hmd_i = row_i . dw). Body: the anchor is the FP root (the mode-2 AMC's root getter, read on each P1 call), not the job camera; the guard is off in body mode (view keeps it, with the fixed HMD term). The sign check logs the cam's motion toward the pov and along the applied step. The 1 s lines print tS, tS0, cam, pov on one line with the axes named; the body line prints job_cam minus fp_root, AMC+0x2E0 minus the root origin (expect 0), and the first shifted record before and after. Once per entry to body, a per-part table (block, slot, the exe's part name by index, mask, shifted yes/no). A post-hook check on the next P1 call reads one shifted record.
- **Waypoint ring (F37, step 3, BUILT).** The runtime's last eight render heads ride the frame input (version 2): each with its geometry sequence and QPC time; the d3d11 half reads them with the now pose. M (the game's marker camera: P1 at +0x1B0, the getters at vtable +0x58 then +0x28) is read at each marker entry, orthonormality checked. forward_hist (the default) takes the ring pose whose view best matches M's rotation (the k back from the newest, 0..7; the residual is logged); forward_k2 takes k = 2, and forward_hist falls back to k = 2 when M cannot be read. The 5 s line gives the k histogram, the mean residual, the fallbacks and the QPC ages. The earlier "H_M vs C = 0" was circular (two runtime poses compared with each other); M is now compared.- **Group 3 (kept, temporary):** world_markers, body_mode, hide_fp_body, body_cull, body_unclip, look_to_aim yaw_sign / comp_delay / comp_sign, training_connect_probe and training_connect_fix, body_align and its trims.

### 2026-10-15: F35 (body shift)

- Built as option A of the hybrid alignment (static RE, the scripts in the scratchpad's body_shift folder): the submit job 0x3DA1DE0's render records of the local third-person AMC move by the world offset d. Built, not installed; the flight's first look is the skipped count, the eye dump and whether the torso moves with the rest.

### 2026-10-15: F36 (weapon stability steps aside on native foot)

- Pacing and the weapon motion-vector gate now step aside while native on foot is active; the key is untouched. The rig's new case sits after the F33 block: the frame rig's sequence floor is monotonic, so an earlier insert refused the F33 checks (found and moved).

### 2026-10-15: F37 (body align fixes, waypoint ring)

- F35's flight: the view mode's HMD term was the list-B eye (a rebased frame, 66 to 101 m), and the guard tripped on the head's motion; the body mode anchored on the job camera, not the FP root. F37 fixes both, adds the verification fields and the post-hook check, and adds the ring (waypoint step 3). The eye dump during an align is still missing: the flight needs it.

### 2026-10-15: F38 (pov target, true head, forward_lead)

- **Built, not installed.** Full build green (`build.bat --wait-lock`, receipt 7af96949).
- **True head for the view.** tS0 and tS are the head's own translation: the render translation plus the applied offset (`nofoot::alignHeadTrue`). The F37 sign anomaly came from the applied offset moving the render translation. The rig's pure-math case pins the helper.
- **Pov target.** With the neck (the walk derived it: `explorerCamNeckModel`), the target is the neck plus the pov-minus-neck offset captured in body axes over the first second of an entry; otherwise the pov low-passed in body axes (tau 1 s). The body error and the view error both use the target, so the head's own swing does not drive the orbit. The body guard stays off. Camera minus FP root (body axes) is logged over the first second, not applied: F33 measured it at 1 to 5 cm.
- **Waypoint, forward_lead (new default, `fix.native_on_foot_world_markers`).** The newest ring pose carried L frames on: rotation D = R0 R1^T scaled by L in axis-angle, applied to R0; translation linear. `fix.native_on_foot_world_markers_lead` (hidden, 0 to 5, default 2.5). The rig pins a yaw spin: 0.12 rad with 0.10 rad one frame back gives 0.17 rad at L 2.5. Unknown mode values now read as forward_lead.
- **Flight reads.** `pov target from the neck|the low-passed head` with the offset and the neck state (every 5 s); `forward_lead: L ...` with the per-frame and lead angles; the 1 s head-reference line (tS, tS0). On foot the neck is usually unknown (the walk does not run), so the low-passed head is the expected source.
- **Deviations.** The camera-above-FP offset is logged, not applied. The forward_lead default has no flight behind it yet; forward_hist stays selectable.

### 2026-10-15: F39 (neck joint by name, no walk)

- **Built, not installed.** Full build green (`build.bat --wait-lock`).
- **Joint names in the exe.** Only two `def_c_` joint names exist: `def_c_head_joint` (0x554E210 file offset, RVA 0x554EE10) and `def_c_povCamera_joint` (0x51F8D30). The strings `Neck` (0x5545F04) and `Head` (0x5541188) are avatar morph-group and body-part names, not joints. No `neck` or `spine` joint name is in the exe, so the candidates are our own strings, asked of `FindJoint` the way Explorer Cam asks for the head (its own copy of the name).
- **Candidates, in order:** `def_c_neck_joint`, `def_c_neck`, then the upper spine (`def_c_spine2_joint`, `def_c_spine1_joint`, `def_c_spine_joint`) only when no neck name answers. The first that returns a plausible model matrix is kept, once per skeleton (cache by interface).
- **Where.** The neck census (`neckCensusStep`, the game's job thread every 100 ms, read-only, SEH-guarded) reads the neck after the head, through the same skeleton and `+0x58` matrix. `NeckCensusReading` carries `neckState`, `neck` and `neckName`. The walk-derived neck from F38 is removed: nothing depends on Explorer Cam's walk now.
- **Pov target.** The same as F38: neck plus the pov-minus-neck offset captured in body axes over the first second, used for body and view; the low-passed head (tau 1 s) is the fallback. The trims stay in the error.
- **Log.** `native on foot neck census: neck joint on the local third-person skeleton: <name>` (once per skeleton). The 5 s `pov target from <name>|the low-passed head` line adds the camera minus neck mean, body axes.
- **Interpretation to confirm.** The "neck-to-camera rest offset" is taken as pov minus neck (the target offset). Camera minus neck is logged, not applied; using it as the target would hold the camera at its starting place and remove the correction.
- **Flight reads.** Which candidate answered (or none), and the pov-minus-neck and camera-minus-neck means. Expect the neck name to answer on foot if the skeleton has one; if none does, the fallback is what runs.

### 2026-10-15: F40 (view map, calibration, walk neck, live now pose)

- **Built, not installed.** Full build green. The F40 rig cases pass (the map inverts; a 34 cm body error closes monotonically to under 1 cm in 180 frames at psi 0, 37 and 180 deg; psi; the head's body-axes map; the flag on version 9 output).
- **Neck fallback (no walk on foot).** When no candidate name answers `FindJoint`, the census reads the head joint's parent from the skeleton's pose arrays, once per skeleton (`neckWalkIndex`, its own buffers; Explorer Cam's walk is not called). The walk enumerates joint indices and parents, not names, so no joint-name list can be logged from it. The `neck joint ...` line names the walk parent when it is used.
- **Per-part table.** Once per body entry, the shift pass prints only the shifted records (block, slot, part name, mask), then a count line: parts present, parts shifted, records shifted and skipped.
- **View map (replaces the C-chain of F37).** seated = D . Ry(psi) . err_body, with D = diag(-1,-1,+1). err_body is the target minus the camera (the trims are in the target); the sign follows the measured plant (cam_body moves by D . applied at psi 0), so the loop closes on the rig's plant. The head's motion in body axes is Ry(psi)^-1 . diag(1,1,-1) . tS. psi = seated head yaw (render row 2) less the headset's yaw in the body (C row 2 less the AMC basis row 2). The psi sign and the z column are not measured: the calibrate flight decides them. The `view map:` line prints psi every 5 s.
- **Calibrate (`fix.native_on_foot_body_align = calibrate`, one flight).** Six 1 s steps: +5 cm seated X, rest, +5 cm Y, rest, +5 cm Z, rest, repeating. No loop, no guard: the offsets are scheduled. At the start of each step the step that just ended is measured: the camera's body position and tS, the mean over its last 0.5 s less the mean over the 0.5 s before it began (`calibrate step ...`). The body<-seated 3x3 comes from the three active steps.
- **Live now pose (`fix.now_pose = next | render`, live).** The d3d11 key goes to the runtime through the native frame output, version 9 (version 8 plus one word, `nowPoseRender`). The client asks 9 first and steps down to 8 for an older d3d11.dll. The runtime's `g_nowPoseRender` (head_pose_time.h) picks the answer: next (default) locates Elite's "now" request one display period past the latest frame's display time, as today; render locates it at that display time itself (the WaitGetPoses latch's instant). With render, the pose cache's display answers are not used (its slots are one period ahead), so every display call takes the owner path. Each change is logged on both sides (`now pose: ...`), and the runtime logs its default at start (`native_now_pose_config`).
- **pose_cache and frame_end_overlap** stay per-launch keys in edvr_openxr.ini (already read at startup, already logged at start). They are not made live; a relaunch flips them.
- **Marker angles.** Every 5 s in the marker path: M (read at entry, F37) against this call's list-B view, and against the runtime's now answer (`M against C ... M against the now answer ...`). That shows which pose M was built from.
- **Overseer note.** The `next` answer may not be what fixed terrain black squares (the TERRAIN CULL COVER may have). now_pose is an open choice; Sean watches for black terrain squares with render.
- **Deviations.** The `neck-to-camera` offset is logged, not applied (F39's interpretation stands). The view map's psi sign and the z column are unverified until the calibrate flight.

### 2026-10-15: F40 flight record (log 105206, dump eye_105603)

- **Calibration proved the map.** +X 5 cm gives cam_body (-0.050, 0, 0); +Y gives (0, -0.050, 0); +Z gives (0, 0, +0.050); off-diagonals under 4 mm. The head's motion maps as diag(1,1,-1) . tS. psi is zero; F40's viewPsi moved with head turns and was wrong.
- **The log's own calibration steps are noisy.** The rest steps drift 2 to 9 cm in cam_body, and psi moves with head turns (-24 to -49 deg). The dump analysis is the basis for the map, not the step deltas alone.
- **Body bobbing is the controller.** Body is open loop (the neck is unshifted, the records move). The F37 integral (0.2 a frame, about 14 a second) into a critically damped 250 ms filter has about -210 degrees of phase at crossover: a limit cycle (d swings +-20 cm up, +-12 cm right, period 1.6 to 8 s) with cam and neck constant.
- **Per-part table.** It did print: once, on the first shifted pass after the body entry (10:55:30.584): EVASuit_Torso (block 27), EVASuit_Legs (30), EVASuit_Feet (31), EVASuit_Gear_Back (48), with the count line. The F41 build prints the same table with the request line on entry.
- **Neck by name, confirmed.** `def_c_neck_joint` answered; pov minus neck (-0.005, 0.118, 0.049) m. The walk-parent fallback stays.

### 2026-10-15: F41 (view map, body filter, per-part table)

- **View map.** psi is zero (`nofoot::viewPsi` is removed): seated = D . err_body, D = diag(-1,-1,+1); the head's motion is diag(1,1,-1) . tS. Look_to_aim's seated yaw is not supplied yet (TODO in the code and the log).
- **View loop, instability check.** The view loop is closed (the camera follows the applied offset 250 ms late), so it cannot be made open loop. At the F37 gain 0.2 and a 250 ms filter it is unstable for the same reason as body (about 14 a second integral into a lag). Replaced: integral 0.02 a frame into one critically damped filter, tau 0.1 s, 5 cm a frame at most. The rig's delayed simulation (22 frames) closes a 34 cm error under 1 cm in 5 s.
- **Body filter (no integral).** target = (cam - pov) - trims in body axes, the camera low-passed at 0.1 s and the pov at 0.4 s, then one critically damped filter (tau 0.1 s, 5 cm a frame at most, 0.6 m clamp). The rig: a 20 cm step in camera height is monotone, overshoots under 5%, and settles within 1 s. `body axes` line once a second: target and d (right, up, forward).
- **Rig.** The three measured steps within 5 mm, seatedFromBody(err, 0) = (-ex, -ey, +ez), the head's diag(1,1,-1), the body step, and the delayed view loop.
- **Per-part table.** Requested on entering body (a line says so); printed on the next shifted pass, one line per shifted record plus the count line.
- **Not verified.** Whether standing up raises the body needs a flight: the rig checks the response to a camera step, not the head's own motion. The look-to-aim psi (TODO) is unverified.

### 2026-10-15: F42 (frame-locked head pose, now_pose = render_locked)

- **Why.** Flight evidence: now_pose render plus forward is the most stable marker; pose_cache off is much worse. The design pass found at least four head samples per frame: the latched render pose, the game's pose one period on, every "now" query re-located at display time (under render those bypass the cache's display slots), and the cache slots.
- **The third state.** `fix.now_pose = next | render | render_locked` (live). The frame output's version 9 word carries 0, 1 or 2; the runtime's `g_nowPoseMode` (head_pose_time.h) holds it. render_locked answers every display "now" call with the latched render pose of the frame.
- **The latch.** The render pose is kept in PoseFrame at the publish point of waitPoses. The pose cache's record gets a `frame` slot (target = the display time). publishPoseCache runs for render_locked even with pose_cache off, takes no locate for its now and next slots, and reuses the record's keys and invalidation, so a withdrawn publication is never served.
- **The answer.** locateHeadFor tries the frame slot first (display call, seated origin); a miss falls through to the cache and the hop and is counted as a fallback.
- **The game pose.** Under render_locked waitPoses sets `game = render` (time included), so the game array and GetLastPoses(ForTrackedDeviceIndex) read the same sample. The separate locate of the game's pose is skipped.
- **Proof line.** `head pose samples per frame: mean ..., max ... distinct; now answers: latched ..., other ...; fallbacks from the latched path ...; angle_delta p95 ... deg` once a second (to the trace, not the gap sink). The samples: the render latch, the game array and each display "now" answer, hashed per frame (the bytes of the pose). Expect 1 distinct, 0 fallbacks, 0.000 degrees at p95 under render_locked. GetLast* read the same snapshot as the render and game samples, so they are not hashed separately.
- **Deviations.** The proof line is on its own line at the gap's cadence (not appended to the gap line), so the rig's gap-line count is unchanged. The rig does not exercise render_locked (the default is next); the flight is the test.
- **Risk.** The terrain cull (display-time answers caused black squares on fast turns before the cull cover). Sean will fly a fast planet-surface turn with render_locked and watch for black squares.

### 2026-10-15: F43 (shoulder body anchor, joint probe)

- **Why.** Body mode anchors on the FP root, which excludes the HMD translation (correct for the arms, which are drawn there). The pov was the head joint, so the torso's neck sat about 13 cm below the eye and the body 10 to 20 cm high; the arms detached because the torso follows the head joint while the arms hang from the FP shoulders.
- **Joint probe (once per skeleton, census thread).** FindJoint on the 3P skeleton and on the FP skeleton (g_skelIface[1]) for 15 names: shoulders, clavicles, upper arms, arms, spine03, hand attach and hands, neck and head. The log prints each name's index (0xFFFF: none). When a side's shoulder does not resolve, the pose walk is dumped: the 30 joints nearest the neck by index, with name hash, depth and parent.
- **The anchor (ix.native_on_foot_body_align_anchor, live, default shoulders).** The FP shoulders (L and R means) through the FP root's yaw only, built from its row 2 projected onto the horizontal, plus the FP origin. The 3P shoulders through the AMC matrix. The target in body axes is (FP shoulder mean, low-passed 0.1 s) minus (3P shoulder mean, low-passed 0.4 s) minus the trims, through F41's single critically damped filter. Neck while a skeleton's shoulders do not resolve.
- **Neck anchor (the alternative).** The neck joint plus Explorer Cam's rest pov offset when derived, else the neck plus (0, 0.15, -0.08) m in body axes (the flown trims), in place of the bare head joint. The F38 neck-plus-residual term is gone.
- **Log, once a second.** The anchor in use, the FP and 3P shoulder means, their distance before and after d, d in body axes, the target in body axes, the FP root pitch, and a note when the shoulders do not resolve. The per-part shifted list prints on each body entry (F42's log has it: four shifted parts and the count line).
- **Not done.** The optional _arm_pitch_follow key (default 0) is not added. Whether the shoulder anchor leaves the torso and the arms joined needs the flight.

### 2026-10-15: F44 (draw-time marker instrument, measure only)

- **Key.** `fix.native_on_foot_marker_probe` (off, hidden, live, TEMPORARY; VR only). Nothing drawn changes; no constant buffer, pool or binding is written. The machinery is `src\d3d11\draw_record_tag.h`, meant for the weapon ammo readout next.
- **Tag.** reticleWorldPost writes each world marker's t' (not key 10) with a 24-bit sentinel in the lowest 8 mantissa bits of x, y and z: the key (8 bits) and the render frame (16 bits), salted. Under 2^-15 relative per component; a zero, denormal or non-finite component is left untagged (counted). The tag, t' as written and P = node+0x50 go to the render thread through per-key sequence-counted atomic cells.
- **Find.** A t33 pool is watched once an on-foot HUD draw (VS 95D01BA609BF7500 or 46546443FD3C3F88) binds it; it must be a structured buffer of stride 336. At each Unmap of a DISCARD or NO_OVERWRITE map, +16 of each record is checked for the sentinels of the last 8 frames: the last-found index per key first, then the whole mapped range with 16-byte streaming loads (at most 4 scans a frame; over 2 ms backs off 30 frames). Each find keeps (pool, record index, X0).
- **Match.** Each HUD draw whose pool has a find (this frame or the last) copies its b0 rows 9-11, b1 row 275 and instance stream entries (the single stride-8 buffer at StartInstanceLocation; v0.x) into a staging slot; the first pool-family draw of each eye a frame (eye by the depth probe) copies the world b0 rows 9-11 and b1 row 275. Read back 2 or more frames later without a wait. When FirstElement + v0.x is a find: v0 = V_hud (X0 - o_hud), v_target = V_w (P - o_w).
- **Log, once a second.** `marker record found at index N` (key, tag age, map type, finds at the last index, index same or changed, X0 == t' bit-exact or not, scan times), or `sentinel NOT found`; per eye the angle v0 vs v_target (mean, max), the V_hud vs V_w rotation, head speed from the ring (mean, max), and the frame check |P - o_w| beside |t'| and |X0 - o_hud|; a draws line with each refusal counted (stride, stream, constants, no find, no world view, rows not a rotation, and so on). Each refusal reason is also named once.
- **Deviations.** V_hud and V_w are GPU copies of what each draw binds (ranged binds included), not a binding shadow plus mapped contents: the copy is what the draw reads, and no CPU copy of every b0 write is made. A HUD draw with no scene depth bound takes the eye whose world rows are nearest (counted as "eye by nearest world rows").
- **Not done.** No correction. The flight is the test: whether the sentinel survives into the pool at all, the record index's stability, and the angle at rest and while turning.

### 2026-10-15: F43 flight (log 121531) and F45 part A

- **Joint probe resolved** (3P and FP): clavicles 3P 35/36, FP 19/20; hands 3P 58/59, FP 37/44; handAttach 3P 67 (L) and 66 (R), FP 52 (L) and 51 (R); neck 3P 34; head 3P 44. Shoulder, upperArm, arm and spine03 do not exist on either skeleton.
- **Why the anchor never ran.** The probe picked the clavicles, but the census read of their matrices failed the head's placement test (the test requires the head's own height and reach). The census shoulder bits stayed zero, so the log read "shoulders not resolved: neck anchor" every second. Now a joint matrix passes when every element is finite and of ordinary size.
- **Clavicle anchor.** The pair is the clavicles on both skeletons: the FP clavicles through the FP root's yaw only, the 3P clavicles through the AMC matrix, the target in body axes through F41's filter and the trims. The neck anchor stays the fallback.
- **Neck pov.** The baked (0, 0.15, -0.08) offset is gone (it pushed the body about 13 cm forward). The neck pov is F38's neck plus the captured pov-minus-neck offset again.
- **Probe print.** "none" only for index 65535; otherwise the index.
- **FP pitch.** Now the FP root's forward row dotted with the AMC's up row (row 1): the pitch against the body's horizontal, not the world y (which read 41.8 degrees at rest).
- **Body log, once a second.** Both clavicle means in body axes, their distance before and after d, the head and neck in body axes beside d, the hand and attach-point world positions on both skeletons with their L and R distances.
- **Per-part table.** It printed in F43's flight, but 96 seconds after body entry: the table was consumed only on a pass where d was nonzero, and d stayed zero until the anchor moved. Now the table is consumed at the first body pass, shifted or not (with d zero it lists the present parts and writes nothing).
- **Not verified.** Whether the clavicle anchor joins the torso to the arms needs the flight.

### 2026-10-15: F45 part B (marker instrument v2: who reads the record, measure only)

- **Why.** The F44 flight (log edvr_gfx_20261015_121531) found the sentinel every frame (record 0 equals t' bit-exact, stable index, DISCARD maps only), but no HUD draw matched: the HUD draws read records about 3400 to 3800, F44 read only 4 stream entries per draw, and only the two HUD VS were asked. The world views failed every eye-frame (staged 0, failed 180 a second).
- **What changed.** Same key, `fix.native_on_foot_marker_probe` (off, hidden, live, TEMPORARY). Every draw call kind is teed (Draw and DrawIndexed as one instance from 0; the indirect calls are counted only). Any draw whose VS t33 is a watched pool, in a frame with a find in that pool, copies its whole instance stream (up to 16384 entries; more counted as cut) into one 1 MB staging buffer per frame; HUD draws (at most 48 a frame) copy b0 rows 9-11 and b1 row 275 every frame; world family draws copy the same until one per eye a frame succeeds, with row 275 from engine motion's CB shadow (`engineVelocityShadowSceneRows`) when a watch follows that b1 from constant 0. The stream match, the view cache and the per-frame staging are in `draw_record_tag.h` for the ammo readout later; native_frame_test checks the match and the cache.
- **What it logs, once a second.** (1) `record N is read by M draws` with each reader's VS, PS, eye, call, instance range, entry and x or y hits, or `record N is read by no draw this second`, beside the draws bound to the pool, streams copied and read, entries, cut and indirect counts. (2) `world views:` eye-frames copied, failed, read; failed tries split into cb0 not covered, cb1 not covered, ring/stage failed, each failing VS with cb0 and cb1 ByteWidth and first constant (the first failure also said once in full); row 275 from the shadow or copied. (3) Every second on foot: the head speed from the runtime ring (mean, max), the angle v0 vs v_target with its eye and how the eye was known (HUD match: depth view or guessed by rows; otherwise v0 = V_w (X0 - o_w) from the last world view), and |P - o_w|. (4) The eye check: each HUD draw's rows against both eyes' world rows, agree or DISAGREE with the depth eye, no depth eye (which eye the rows chose), one eye's view only, no world view.
- **Not done.** No correction and no constant buffer write; the F46 patch is not built. Which draw reads N, why the world copies failed and whether the eyes agree are the flight's questions.

### 2026-10-15: F46 part B (the NDC pair per eye, measure only)

- **Why.** The F45 flight (log edvr_gfx_20261015_125317): the record is read by the interface composite B10B032BDFD46700 / DB899F4BD577F2E5 (a world-space billboard into the post-tonemap eye target the UI layer redirects), whose b0 matters only in rows 4, 5 and 7; its depth-view eye is -1. Every world capture failed on "cb0 not covered": the world VS reads b1 only (view-projection at b1 270..273, row-vector form; origin at b1 275). The record cap refused about 3450 records a second, and about 600k pool-bound draws a second were scanned.
- **What changed.** Same key, `fix.native_on_foot_marker_probe` (off, hidden, live, TEMPORARY). Only the on-foot HUD draws (they name the pool), the marker composite and the world pool draws (until both eyes are in for the frame) are looked at past the VS hash; the record cap is gone (per-frame record lists grow). World capture: b1 rows 270..275 from engine motion's CB shadow when it holds them, else one 96-byte GPU copy; no cb0. Marker capture: the B10B draw's stream, b0 rows 4..7 and b1 row 275 (CB shadow or 16 bytes). Both eyes are the UI layer's: the layer's redirect mapping (new read-only `uiLayerEyeOfGameTarget`), else ui depth's frame table read only, else the last answer for that target; a world target with none takes ui depth's own rule over the frame's eye-sized world targets (first of a shape = eye 0), with the depth view's eye logged beside it as a check. Pure math (`worldClip`, `markerClip`, `ndcAngleDeg`, and F47's `ndcOffsetRows`, unused) in `draw_record_tag.h`, checked in native_frame_test.
- **What it logs, once a second.** Per eye: |ndc_marker - ndc_P| in pixels of the marker's target (mean, max, last dx and dy) and in degrees (mean, max), the same against the other eye's world view, the head speed, |P - o_w| and |X0 - o_hud|, the last NDCs, r7 . p < 0 counts; or "no pair this second" with the marker side and the world side each present or MISSING with the first reason that holds. Also: who reads the record (marker composite only, eye now the UI layer's) with the eye sources; the world views (eye-frames per eye, shadow or staged, eye source, depth-eye agree/DISAGREE/unknown, failures); the cost (draw tee calls, draws scanned by kind, microseconds in them, records, bytes staged, refusals).
- **Not done.** F47 (the per-eye NDC offset on a private b0 copy around the redirected draw) is not built; nothing is written. Whether the UI layer's eye and the world target's eye agree, and the size of the per-eye NDC error at rest and in a turn, are the flight's questions.

### 2026-10-15: F46 part A (body: neck anchor, visible trims)

- **Hybrid kept (Sean).** The torso is placed by a fixed offset from the eye, not by matching the FP and 3P clavicles.
- **Anchor.** `fix.native_on_foot_body_align_anchor` defaults to `neck`: the pov is the neck plus the captured pov-minus-neck offset R0 (no baked offsets). The clavicle code runs for the log only.
- **Trims.** The three trims are numbers now (`getFloat`, which the schema generator requires), visible on the Native on foot page next to body align: Body height (positive lowers the body, flown +9 cm), Body forward/back (positive moves the body back, flown +4 cm), Body left/right (positive moves the body left, flown +4.5 cm). Range -30..+30 cm, 0.5 cm steps, live. Target = (camera - pov) - trims.
- **Ruled out: matching FP and 3P clavicles (F45).** The FP rig is a weapon-view rig whose clavicles sit 9 cm above, 19 cm behind and 7 cm right of the eye, so midpoint matching put the eye below the neckline.
- **Not verified.** The flight decides whether the hybrid's fixed offsets leave the arms and the torso joined.

### 2026-10-15: F47 (the gated per-eye marker correction)

- **Why.** The F46 flight: the capture works for eye 0 only (eye 1: "the record was read for the other eye only" every second). |ndc_marker - ndc_P| is a constant 36.3 px (the icon's design height above P, not corrected) plus about 0.2 px per deg/s of head speed (about one frame of lag).
- **What changed.** New key `fix.native_on_foot_marker_fix` (off | on, hidden, live, TEMPORARY; on turns the probe on with it). The draw tee decides at each B10B032BDFD46700 draw: both eyes' marker draws matched in the read-backs of the last 8 frames, this draw the eye's matching one (its place among the eye's B10B draws), b0 a whole bind from constant 0 with a CPU shadow (Map/Unmap and UpdateSubresource tees), o_hud from engine motion's CB shadow, the eye's world rows from the CB shadow this frame, P from the find of the matched key. (dx, dy) = ndc(P, world b1 270..273 at o_w) - ndc(P, b0 rows 4, 5, 7 at o_hud) (`drawtag::ndcDelta`, rig-checked); gate |dx|, |dy| < 0.1 NDC and both w > 0; the last good delta is held one frame on a miss. Inside the UI layer's bracket (vscreen's shared draw path, after `uiLayerBegin`), the game's whole b0 goes into a work buffer, rows 4 and 5 (+ d . row 7) are written over it, the work buffer is copied whole into a private DEFAULT b0, which is bound for the game's issue and the layer's second issues; the game's b0 goes back after them. Both buffers are made once. Every refusal draws unpatched.
- **Item 2 (eye 1).** Every B10B draw is recorded whatever its pool; a stride-336 pool it binds is learned and watched; one line a frame (the first 8 read-back frames each second) lists each B10B draw's eye and source, place, call, instances, t33, watched or not, and the record and key it read (or why none). A second post of a key in one frame is tagged with bit 7 in the key (32 tag cells now); every record carrying the newest tag is a find (a second copy is counted); last frames' find indices are hints, and a census scan runs every 30 frames.
- **What it logs, once a second.** `marker fix (F47, key on|off): eye N:` the unpatched delta of P (dx, dy mean and max in px of the target, degrees mean and max), head speed, patched draws (held), the delta through the patched rows read back (about zero when the patch took), each not-applied reason (eye 0 / eye 1 unmatched, another draw, ranged b0, no CPU b0, o_hud or world view, no P, w <= 0, gate, bind refused), whether the patched draw read the record at read-back, and the CPU b0 shadow against the GPU copy. Said once: "eye N unmatched: not applied". A census line: B10B draws, pools learned, second posts, second copies, census scans, shadow writes, stale and unissued decisions.
- **Deviations.** "Matched" is the read-back's verdict 2 to 8 frames old, not the same frame (a stream match needs the GPU copy). The bracket is in vscreen's draw path around the issue, not inside `uiLayerBegin`, so the layer's second issues get the same b0. The rows go in through a work buffer, not a boxed UpdateSubresource on the constant buffer (that needs D3D11.1 partial updates).
- **Not done.** Not flown. With F46's eye 1 the gate holds the patch off; the item 2 lines say how eye 1 gets its marker.

### 2026-10-15: F48 (the pov body anchor, standing height, part and parent census)

- **Why.** Both skeletons carry def_c_povCamera_joint (0x19B1240 attaches it on both). On 3P it is index 15, a child of the root, at rest the head plus (0, 0.038, 0.112). Placing it at the FP root should put the torso's neck at about eye + (0, -0.156, -0.161) with no trims.
- **Anchor.** `fix.native_on_foot_body_align_anchor` = pov (default) | neck | clavicles (the old value `shoulders` reads as clavicles), live; a mode, not a bool. pov: the joint probe asks FindJoint for def_c_povCamera_joint once per skeleton; the census reads its model matrix every 100 ms (shState bit 0x10, with its index) and the eye sample takes it to world through the AMC matrix, as the neck. Target = (FP root, low-passed 0.1 s) - (3P povCamera, low-passed 0.4 s) - trims in body axes, through F41's filter (tau 0.1 s, 5 cm a frame, 0.6 m clamp). Without the joint: the 3P head plus (0, 0.038, 0.112) m in body axes; the source is logged on each change. neck and clavicles are F46's and F45's paths unchanged.
- **Standing height (pov only).** The 3P side's vertical (body axes) is the captured standing value; x and z stay live. Captured at key-on (the first non-ADS frame of a body entry or an anchor change) and again whenever the stance is at rest: not ADS (journalAds) and the 3P head's height within 2 cm for 1 s; the capture is the anchor's mean height over that second. There is no weapon flag in the journal watcher, so a weapon held still for 1 s without ADS is a rest and recaptures the lowered height. `nofoot::StandingHeight` and `standingApply`, checked in native_frame_test.
- **Trims.** Default 0 again (code defaults, repo `edvr.ini` values and the sliders' `recommended`). The sliders are otherwise unchanged (-30..+30 cm, 0.5 cm steps, live, Native on foot page).
- **Log.** Once a second in body: the anchor, its 3P side and FP side in body axes, the 3P source, the standing height (applied, logged only, or not captured; capture count) beside the live height, and the neck after d minus the FP side (expected about (0, -0.156, -0.161) m). Once per skeleton: `skeleton parents (FP)` (joint count, then every joint<-parent, 24 a line) and `skeleton parents (3P)` (15, 34, 35, 36, 44). Once per FP (mode 2) AMC: `FP part table` lines, one per entry with any instance, mask or flag byte non-zero (all four instance pointers and masks, flags, state, pending, current), and a count line. The F1/F3 census lines are unchanged.
- **Not done.** Not flown. Whether the pov anchor joins the torso and the arms at zero trims, and whether the standing hold survives a weapon draw, are the flight's questions.

### 2026-10-15: F49 (the pose freeze: Animate post-hook, probe by default)

- **What changed.** A post-hook on the frAnim Animate handler at 0x741520 (vtable 0x510E370 slot +8; rdx = the job, job[0] = the evaluator X). The 15 prologue bytes are checked against the loaded image; 7 are stolen (sub rsp,0x38; mov r10,rdx; no RIP-relative operand); a mismatch installs nothing, with a line. The hook acts only when job[0] is the local avatar's X: the census (job thread, every 100 ms) reads X from the local 3P skeleton interface (RR *(iface+0x2B8), AO *(iface+0x120)) and, once per X, builds the plan: P = [X+8], the frozen set F (the torso-side child subtree of the lowest common ancestor a of the neck 34 and def_l_foot_joint; the root's child on the neck's path when the foot does not resolve) and each member's rest record (P+0x48). Output span: [job+0x28] when [X+0x210] is 0, else [[X+0x1F0]+0xA0] when count-1 < [[X+0x1F0]+0xE0]; 0x43FB740 is never called; count = the u16 at P, cap 0xC0. On the worker: no allocation, logging or locks (a try-flag skips a second local call), every read and write under SEH; statistics go through a sequence counter to the frame thread.
- **Key.** `fix.native_on_foot_pose_freeze` = off | probe (default) | on; hidden, live, TEMPORARY. probe writes no game memory. on writes M'_j = local_j x M'_parent(j) for F in parent-first order, anchored at M_anim(a), the local blended from the animated toward the rest record (quaternion nlerp, position lerp) by a weight that moves over 0.3 s; the weight rises only while not ADS (journalAds) and the animated head (joint 44, read in the hook before any write) has held its height within 2 cm for 1 s. A faulted write stands the writes down for the session. Pure math in `native_on_foot_core.h` (`pfzFrozenSet`, `pfzRestModel`, `pfzBlendRecord`, `PfzBlend`, `PfzHeadRest`), checked in native_frame_test.
- **What it logs.** Once per X: `pose freeze: plan for X=...` (P against GetPoseData, the joint count, the foot's index, a, the torso child, F's member count and list, whether 35, 36, 34 and 44 are in F); the rest chain against the game's own 0xFDE0D0 for 34 and 44 (a leaf that reads P and writes 64 bytes; called only when its entry bytes match). Every 5 s on foot: Animate calls, local calls, the thread id and thread changes, [X+0x210], [X+0x12C], [X+0x128], [X+0x212], calls per output buffer, the count, joints 34 and 44 animated minus the rest chain anchored at a (mean, min, max and mean vector, mm), the weight and frames written. Every 5 s from the census: GetModelMatrix(34) against the hook's newest span matrix of 34 (and the written one when on), with the age. Each hook refusal is named once.
- **Risks.** A weapon attached to a 3P hand would float away from a frozen hand. The skin is uploaded from the t2 rows (APPLY_BIND_POSE) and the function that packs those rows was not found, so the skin may read another buffer: then on changes the animated matrices and nothing visible changes (not confirmed either way; the span cross-check says only whether GetModelMatrix sees the write). The write lands after [X+0x12C] = 1, so a reader that rotates the ring in the meantime may miss it. The bind pose of the arms may be a T or A pose.
- **Not done.** Not flown, not installed. The frame thread drains the statistics every 5 s, not at 1 Hz. Only the 3P skeleton's X is used (the local AMC's own interface is not read separately).

### 2026-10-15: F50 (the marker lag delta against a per-eye baseline; the body's vertical lock)

- **Why (F47 flight, log edvr_gfx_20261015_141552).** Both eyes matched (eye 0 to record 8, eye 1 to record 9), but the fix was gated off every frame: F47 projected the world point P through the marker's b0, which lives in the tracking (seat) frame, about 54 degrees from the world's. ruled out: P through the marker's b0 as the delta, because b0 is in the seat frame, not the world's.
- **What changed (marker, `fix.native_on_foot_marker_fix`).** `drawtag::ndcLagDelta`: ndc_marker = the record's X0 through the draw's own b0 rows 4, 5, 7 at o_hud; ndc_P = P through the eye's world b1 rows 270..273 at o_w; m = ndc_marker - ndc_P; (dx, dy) = -(m - c), which moves the marker to ndc_P + c. c is a per-eye baseline (`drawtag::LagBaseline`): seeded from the first 30 frames with the head under 3 deg/s, then an EMA (alpha 0.05) of m on frames with the head under 3 deg/s and |m - c| < 0.01 NDC. No patch, and no hold, until it is seeded with a standard deviation under 0.5 px. Reset on a change of b0 row 5 or row 7 scale, the marker target's size, or the eye's matched draw place; 90 slow-head frames in a row over 0.01 NDC start the seed again. Gate |dx|, |dy| < 0.05 NDC. The one-frame hold, the both-eyes rule, the ordinal guard and the one patched B10B draw per eye are unchanged.
- **Rationale.** The flight measured c at about (0.0000, +0.03609) NDC in both eyes, constant across distance and position: a frustum-centre difference between the HUD's and the world's projections, a static calibration. What is left after it is the lag.
- **What changed (body, `fix.native_on_foot_body_align_vertical` = off | on, default off, hidden, live, TEMPORARY).** Body vertical locked to the ground (feet clipped under terrain with pov at F48). With vertical off, body mode drops the body-up component (AMC row 1) of the body run's position and velocity after the filter, so d moves the body only along its right and forward axes; the up trim is left out of the target; F48's standing height is logged, not applied. With vertical on, F48's behaviour.
- **What it logs.** Per eye each second (`marker fix (F50, ...)`): c in NDC and px, its standard deviation and state (seeding, seeded, ready), resets and the last reason; |m - c| in px by head speed (< 3, 3..20, 20..60, >= 60 deg/s); patched draws, held, refused as not ready, gated; the residual after the patch, X0 through the patched rows read back minus (P + c), by head speed (about 0 at every speed if the patch took). The honesty check: m on slow-head frames binned by head pitch (< -15, -15..15, > 15 deg) and roll (|roll| < 10, >= 10 deg) since the last reset; a spread over 2 px between two bins of 30 or more frames logs `the baseline is not constant: masking`, and the fix is not to be claimed. Body: the vertical-off note on each entry to body; once a second the up component d would have had (the target's up in body axes, mean and range) and the per-frame step dropped, beside the standing height.
- **Rig.** native_frame_test: the lag delta lands the marker on ndc_P + c and its gate refuses; the baseline seeds over 30 slow frames, refuses a fast head and a far m, restarts after 90 far frames; the vertical drop.
- **Not done.** Not flown, not installed. The honesty check logs only; it does not stop the patch. The would-be vertical is the target's up (what d converges to with vertical on), not a shadow filter's output. The eye-assignment reset keys on the eye's matched draw place, not on the record index.

### 2026-10-15: F51 (raise the FP rig and the view; the pose freeze's local X and gate)

- **Why (F50 flight, log edvr_gfx_20261015_153136).** With vertical off the feet are on the ground, but the camera sits at upper-chest height and the arms below the shoulders: 3P pov 1.708 m against FP side 1.477 m (body up), so the game's FP rig and camera are about 0.23 m below the 3P body's eye. The pose freeze never ran: "0 for the local X 0000000000000000; no measured call for the local X".
- **What changed (raise, `fix.native_on_foot_body_raise_fp` = on (default) | off; hidden, live, TEMPORARY; body mode only).** dh = (3P povCamera height, or the head plus 0.038 m) - (FP root height), body axes up, measured each frame the inputs and the FP root are fresh, clamped to 0..0.5 m and low-passed (tau 1 s) from 0; held under ADS or a stale input. The applied raise follows dh at 5 cm a frame at most and returns to 0 at the same cap outside body mode or with the key off. View: the eye-align output carries the view run plus a seated (0, -raise, 0) (seated y = -dh raises the camera by dh). FP arms: the body-shift post-hook (0x3DA1DE0) also takes the FP AMC (the mode-2 AMC fpRootTrack latches, the mode re-read in the hook) and moves each record at the AMC origin by raise times the body's up row (AMC row 1, the axis dh is measured on), under the 3P rule (`nofoot::shiftIfAtOrigin`: a moved record is not moved again). The body's vertical stays locked (F50).
- **Pose freeze.** The census takes the kind's X when the Animate hook has counted it in the last 5 to 10 s; else the one X counted whose [X+8] is the local iface's GetPoseData; else (no match, or several X share P, since P may be a skeleton asset every humanoid shares) the kind's X, F49's behaviour. The hook counts every X it sees in a two-bank lock-free table. The gate is now no locomotion: the 3P AMC origin's horizontal speed (low-passed 0.2 s) under 0.35 m/s for 0.3 s, so a weapon out freezes too; ADS stays excluded; F49's head-steady test is logged only. The key and the probe default are unchanged.
- **What it logs.** Once a second in body: `body raise (F51, key ...)`: dh measured, the 3P pov and FP side heights and the pov source, dh low-passed, the applied raise, the seated offset and the eye align published, the FP records shifted and skipped and the passes, the part names shifted, the first shifted record before and after, the up row; then the FP handAttach joints (52 L, 51 R) in world as the skeleton places them and with the arms' shift. Every 5 s with the freeze statistics: the Animate X table's top 8 by calls with P = [X+8] and what it equals (3P or FP GetPoseData, the local X, an iface's X); per latched skeleton, X = *(iface+0x2B8) and *(iface+0x120) with P and Animate counts, [iface+0x2C0], [iface+0xF8], GetPoseData, the X the census takes and why; the gate's speed, ADS and the head test.
- **The weapon.** Not moved by this build. It is not one of the AMC's 54 parts (no weapon name in the part table), so the record shift does not reach it, and what draws it at the handAttach joints is not found. If it follows the skeleton it draws dh below the shifted hands; the hand-attach line gives both places.
- **Rig.** native_frame_test: dh's clamp, low-pass from 0 and hold; seated y = -dh agrees with seatedFromBody and raises the camera; the 5 cm cap; the idempotent shift; the locomotion gate (standing rests, walking does not, a vertical fall is not locomotion, ADS ends a rest).
- **Not done.** Not flown, not installed. The weapon's renderer; the FP records equalling the FP AMC origin (the 3P rule's premise) is unverified for the FP AMC (the shifted and skipped counts say); a P match that is ambiguous takes nothing.
