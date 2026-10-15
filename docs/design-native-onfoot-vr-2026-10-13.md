# Native on-foot VR (replacing Explorer Cam) — design, 2026-10-13

## Status

- **State:** F0-F18 FLOWN. F19, F20 FLOWN 10-14. **F21 and F22 FLOWN in part** (F22 flight 10-14 19:15; see F22 below). **F23 BUILT** (commit 1abc4899, full build passed, receipt 8b5c14e8...), installed to Epic `--scope vr`, **NOT FLOWN**. Branch `claude/native-onfoot-vr-completion-219ffa`, local only.
- **F22 (BUILT, NOT FLOWN):**
  1. **Session:** a training scenario writes no LoadGame, so native on foot and the Explorer Cam hotkey also admit Status.json on foot (`journalSessionOrOnFoot`, journal_watch). One line when the fallback admits: `native on foot: game session from Status.json on foot`. `keysMeanGame` (device_hook) is unchanged.
  2. **Look to aim** (`fix.native_on_foot_look_to_aim` on; `_smoothing_ms` 150), engaged only with reticle_follow = head on foot. The controller writes rotate-abs for one update call (hook +0x19973D0, 32 bytes verified); the processor (+0x1A9C370, read only) records comp and the yaw delta at comp+0x5F4, and our part is D * ours / (old + ours). The total goes to the runtime as frame-output version 6 (`lookYawTotal`, `lookYawGameFrame`). The runtime turns the seated space about the HEAD by the change at the next no-frame poll (`lookPoll`). The head keeps its place and its reported yaw falls by the body's turn. currentOrigin (seat memory's origin) never carries it; any origin replacement re-bases the total.
  3. **Probes, read only, always on in VR:** training load wait (+0x260D6B0, `training load wait:`); probe A, the camera driver (+0x28431D0, `native on foot rig probe A:`); probe B, the FP and 3P AMC roots via the getter at 0x3DD61AF (`native on foot rig probe B:`, from the P1 callback). The 5 s look line includes a pitch probe (log only).
  4. **Static RE (probe D):** the head pose is composed only into the eye and post-GUI cameras (driver 0x28431D0 -> composer 0x283D4C0, pose from VRDevice::GetEyePose 0x4E25A0), never into the game's mono camera or the aim basis. **Ruled out (static): the game camera includes the HMD pose and the FP rig reads it.** Open: H1 the FP root picks up head yaw by another path; H2 the root is body-anchored but the FP bucket draws with a head-locked view; H3 EDVR's own view writes touch the FP weapon camera. Probes A and B discriminate.
- **F22 TEMPORARY (go when the arc picks one):** `fix.native_on_foot_look_to_aim_yaw_sign` (1), `fix.native_on_foot_look_to_aim_comp_delay` (0). Instruments with no key: `training load wait:`, rig probes A and B, the look's pitch probe.
- **F22 doubts for the flight:** (a) the runtime sign assumes the game's view yaw and the seated yaw share a sign; the yaw_sign key corrects only the controller's input. If the world turns WITH the body, suspect the runtime's sign first (`look_to_aim_runtime` line). (b) comp+0x5F4 is the stored yaw delta (store verified, not flown). (c) the gain is the measured D/(old+ours), else the midpoint of comp+0x7D8/+0x7DC. (d) the look basis's forward row is row 2 (+0x5D0), which the pitch probe also reads.
- **F22 flight check (Epic, `--scope vr`; keys reticle_follow = head, look_to_aim = on):** the `look to aim: engaged` line, then error yaw falling toward 0 within about 2 s; the world holds still as the body turns; `look_to_aim_runtime` applies with game frame and runtime frame close together; `comp_delay` 0 vs 1 for the judder. If error grows, flip yaw_sign to -1 (one flight).
- **F22 FLOWN (10-14 19:15; gfx edvr_gfx_20261014_191539.log, runtime edvr_openxr_20261014_191541_305_43832.log):** Part A WORKED in Training ("game session from Status.json on foot", then the gate byte 1 -> 0). Part B installed, no blocked load seen. **Part C SPUN the commander:** a centred head read error ±160..180 deg, because the controller used atan2(C[8], C[10]), the backward axis. The controller chased the wrap at the 10 deg/frame clamp. In some samples the game yaw delta had the opposite sign to the input (input -0.1745, delta +9.998). The runtime did apply (base re-based at -167.994, -335.540). The basis element swung -42 -> +38 with the view pitch near 0, so it is not pitch. `native_post_submit_unavailable reason=provider_version` predates F22: ignore.
- **Ruled out:** forward = -row 2 of C (F23 read 176 deg with the head centred; F22's +row 2 read -3.6 deg). F22's spin is most likely the yaw sign: the error grew to the wrap, so F24 defaults yaw_sign -1.
- **Live ini (hybrid):** Sean switched `fix.native_on_foot_body_hybrid` on live at 19:20:20 and reports a torso. The F21 hybrid body works, at least for the torso. **Next-flight check: how the arms join the torso.**
- **F23 (BUILT, NOT FLOWN; commit 1abc4899):** yaw = atan2(-C[8], -C[10]), with the convention logged at engagement (a centred head must read about 0). Turn rate capped at 180 deg/s × dt. **Runaway guard:** disengages and logs `native on foot look to aim: runaway guard tripped (<reason>)` when the body turns more than 270 deg in any 3 s, or the output sits at the cap 1 s while the error does not fall; off until the key toggles or native on foot re-enters. Sign instrument `compensation sign check:` (mean |residual| and residual/our part, about 0 right, about 2 wrong; plus sign-mismatch frames). The pitch probe logs the full basis. Part A confirmed in Training. Sean's live ini keeps look_to_aim on; F23 keeps the default on with these guards.
- **F23 TEMPORARY:** `fix.native_on_foot_look_to_aim_comp_sign` (1; signs the published compensation). The F22 `yaw_sign` and `comp_delay` keys are still listed above.
- **Part E (BUILT, NOT FLOWN; in F23):** `load stages:`, read only on the dispatcher +0x1CA6510 (16-byte prologue verified, steal 8). Per-stage changes, the net gate (dword +0xC0 < 5), one summary per load. `dispatcher reached` and the 30 s "has not run" line tell a never-run hook from a quiet one. The 21 stage names are in the journal.
- **F23 FLOWN (19:40, edvr_gfx_20261014_194056.log):** the guard tripped 1.5 s after engaging (more than 270 deg in 3 s). The convention line read raw C row 2 = (-0.0613, -0.1530, 0.9863): F22's reading was right and F23's diagnosis was wrong. Load stage timings (Training): network stages 0 s; the pipeline about 8 s, with CreateLocationObjects 2.76 s and CreatePlayerVessels 3.69 s; 31 s between the menu closing (Shutdown_Deactivate) and the pipeline's first dispatcher call, with the game presenting at 89 fps. Waypoint and ammo display still follow the head with every setting, HUD follow off included; a Sonnet RE is on it.
- **F24 (UNBUILT; commit F24 (unbuilt), for merge before the build):** yaw = +row 2 again, yaw_sign default -1; passive sign instrument (`passive sign check:`: mouse-only turns, same-sign counts, implied yaw sign); the on-foot split's session = the Part A fallback (`temporal aa: game session from Status.json on foot`); the stage summary prints when EnterLocationFinalStep starts running and again when done, plus the wall time of each obj's first dispatcher call; the load sampler (`load sampler:`, render and window threads, 100 ms while the menu or a pipeline runs, no key). Part F, read only: probe A reads the mailbox through the transition-flash consumer (its own hook removed: 0x28431D0 is already hooked there); probe 1 = composer 0x283D4C0 (`native on foot view probe 1:`); probe 2 = bucket prep 0x594D90 (`... probe 2:`, aggregated per 5 s); probe 3 = marker entry 0x1A14D80 (`... probe 3:`, key 10 and key 43 with the scale inputs). Every probe line carries its call count, so "never ran" and "ran" differ.
- **F24 MERGED AND BUILT (merge 71ed8a3b; fixes 741130be):** origin/main merged into the branch (main itself has no on-foot files; its six small src changes are log, menu, perf, flat capture and native_trace). Full build of the merged tree: "all gates passed", receipt 9d2b8e75. Load sampler safety check: nothing allocates, logs or locks between SuspendThread and ResumeThread; the window thread is checked with GetProcessIdOfThread before any stop. Probe A's reader is reached through g_driverProbeSink (set at VR install), so the rigs link. Installed to Epic `--scope vr`, verified; **NOT FLOWN.**
- **F24 launch crash at +0x59F1B0 (probe 1, see diagnosis):** the composer (0x283D4C0) reads nine stack arguments; probe 1 forwarded four, so it read garbage pointers and the 0x40-byte copy at +0x59F1B0 faulted. Probe 2 (bucket, 0x594D90) takes rcx/rdx only and is never installed (not proved clear). Probes 1 and 3 and the driver sink are behind `fix.native_on_foot_view_probes` (default off, restart). Fix commit 806f8506; build receipt fdb8f64d; installed and verified; the stale `edvr_logs\d3d11_hooks.armed` (the sentinel's trip) was deleted after the install. **NOT FLOWN.**
- **F25 BUILT, INSTALLED to Epic (`--scope vr`, verified; commit 836a3b3b, receipt 98487701; NOT FLOWN).** Look compensation: a reset serial (`lookYawReset`, v6) returns the origin's compensation to 0 at once when the controller disengages or the guard trips; `look_to_aim_runtime` now logs the head's seated yaw before and after each step; `yaw_sign` default +1. Visor: the on-foot RemLok match (VS B553BB479B7C0B97, PS 68ABCB9FEF6CA66C, RT 2016 R11G11B10, SRV2/3) uses the existing remlok keys; `fix.native_on_foot_visor_shift` is a hidden TEMPORARY A/B (x,y at EliteDangerous64.exe+0x5FCA8B0/+4; verified 0 0 before the write; restored when off). Load sampler: a 5 s thread census (CPU and start address) and the top three CPU threads sampled, 8 frames per stack by the first game frame.
- **Visor lines confirmed:** VS B553BB479B7C0B97 (census_skip, F24 flight): removing it removed the on-foot RemLok edge lines.
- **Runaway, open:** F22-F24 runaway = compensation not reaching the game's head pose (residual ratio 1.03-1.09 with -1). Code audit: every head and eye locate (pose cache, locateGeometry, HeadLocator) is in the seated space the origin change moves, so no bypass is visible; the flight's seated-yaw trace decides.
- **Ruled out:** yaw_sign -1 (F24: the body turned away from the head).
- **Eye dump (F24, 20:58:53, after the guard):** the camera behind and above the third-person torso's neck, looking down at its back; consistent with a large accumulated total displacing the eye around the pivot.
- **F25 flight checks (Sean):** (1) `look to aim: engaged` with the convention, and the error falling toward 0 at yaw_sign +1; (2) `look_to_aim_runtime` seated yaw before/after moving by the step, and the reset line after a disengage; (3) did census_skip's removal also remove the visor tint and fog with the lines; (4) `remlok lines (on foot)` matches per eye, left and right; (5) `load sampler` census: the top CPU thread and its first game frame during the wait.
- **F24 scale instruction (static, not decoded to the constant):** camera-space branch (byte 0x5E7A752 != 0; the cmp is at 0x1A14ECF). `movss xmm14,[RC2+0x17C]` at 0x1A14F27 (bytes F3 44 0F 10 B0 7C 01 00 00); `divss xmm14,xmm0` at 0x1A14F33 (F3 44 0F 5E F0), xmm0 = a constant product from the image; the tan ratio `divss xmm13,xmm6` at 0x1A14FBE (F3 44 0F 5E EE), xmm6 = M+0x260 (movss at 0x1A14F4B). Other branch: `movss xmm14,[M+0x260]` at 0x1A14FE9, `divss xmm14,xmm6` at 0x1A14FF2. Fix candidate for next time: force the scale to 1 in HMD on foot.
- **Ruled out:** EDVR's HRC rewrite moves the waypoint (Sean: it follows the head with HUD follow off; static RE: the marker class never reads HRC data). **Ruled out (static):** 0x591F30 gives FP buckets a special view in HMD (it only overrides FOV, and HMD sets those to -1, so they are skipped).
- **F19/F20 FLOWN (10-14):** third mode gives the full body and the weapon in the hands, correct, but the HUD is lost and there is no ADS pose (the Flags2 ADS bit is never set in third). The game itself toggles the torso and arm vertex clip with the model mode, which confirms F19's unclip mechanism. The pilot prefers first-person mode with a hybrid body. The ammo display and the waypoint follow the head: the first-person rig is anchored to the head camera, and the HUD canvas icons are world-projected. Research running.
- **F21 (BUILT, NOT FLOWN):**
  1. Reticle: `fix.native_on_foot_reticle_follow = game` (EDVR writes nothing to any indicator; the aim centring and head placement are not run). Default stays aim.
  2. TEMPORARY `fix.native_on_foot_body_hybrid` (on | off, off). While native stereo is on foot and it is on: the first-person skeleton's view masks are zeroed on the local first-person AMC (mode 2; its arms and hands stay); on the local third-person AMC (mode 3) the EVASuit torso is unclipped (enable 0, dirty 1) and the view masks of the arms, hands and both shoulders are zeroed. It overrides `_hide_fp_body` and `_body_unclip` while on. Census line every 5 s: `native on foot body hybrid:`.
  3. Eye probe P1: no code bug found by reading. Candidates, each tested by the new `native on foot eye inputs:` line (raw camera, AMC origin, basis rows with their lengths, head joint in model space and in world space, and the distances |cam - origin| and |head world - origin|): (a) the job's camera floats may not be in the AMC origin's space; Explorer Cam's commander frame (explorer_cam_core.h commanderFrame) uses its own root point, not the AMC origin; (b) the avatar-root frame may not equal the AMC frame (the head is then not where P1 puts it); (c) the head sample is up to 100 ms old (minor for 0.7 m).
- **Origin:** a user's handoff. A Cinema-to-HMD swap on disembark gave native stereo on foot; the HUD, radar, left panel and inventory were missing.
- **Build:** Sean's installs hash to the handoff's exe (332841). Epic is the install target.
- **Verified offline:** the setter is vtable 0x52E9608 slot 5 = 0x28723C0. The selector is embedded in its owner at +0x60. No direct callers.
- **Requester gate (build 332841):** in 0x28873C0 the cinema partner mode is kept only if `call [rax+0xb0]` returns non-zero AND the byte at EliteDangerous64.exe+0x5E82B3E is non-zero. The byte has one reader and no writer. Never write its neighbours 0x5E82B3C and 0x5E82B3F.
- **Key findings:** the game tests the selector mode in {3, 4} at about 50 sites (F8); the HUD is held back by the first-person pass's enable (F8, fixed by the override). F3: the first-person AMC (mode 2) has the EVASuit arms, hands and FirstPersonSkeleton; the 3P body (mode 3) has masks 0x60BD8000C05E. F15: hiding the FP body leaves the 3P legs. F17: the HUD clip is the canvas edge (stage 2008x1206, RTT 4915x2952); a bigger canvas adds no room.
- **F18 FLOWN (10-14):** the torso was not visible with CULL_FRONT, so back-face culling is ruled out (Ruled out). The `none` mode matched 0 draws: a bug in the F18 count, so no `none` result is evidence. The cull code stays as it is; the cull is superseded.
- **F19 (BUILT, NOT FLOWN):**
  1. Body unclip, TEMPORARY `fix.native_on_foot_body_unclip` (on | off, off). While native stereo is on foot, after each fade call, the local third-person AMC's clip modifier (part+0x610: enable at +0xC, dirty at +0xD) is written enable 0, dirty 1 for EVASuit_Torso, _Arms, _Hands, Shoulder_Left, Shoulder_Right. Verified in the exe against the item-apply function 0x3DD9EA0 (the journal has the quoted instructions). Nothing is restored when it goes off.
  2. Clip probe (no key; on while native stereo is on foot): 5 s lines `native on foot body clip:` for the five parts plus EVASuit_Legs, EVASuit_Feet and FirstPersonSkeleton (only where the AMC has them); `native on foot body clip flags:` once per AMC ([AMC+0x321], [AMC+0x322], [AMC+0x378]). Read before the same call's write.
  3. Constants probe, TEMPORARY `fix.native_on_foot_body_cb_probe` (on | off, off), native or Explorer Cam on foot. Logs `native on foot body cb2:` rows 5..9 once per 5 s per (VS, PS, index count). No CPU copy of constant-buffer bytes exists (binding_shadow keeps pointers only), so it is a 352-byte GPU copy to a staging buffer, mapped with DO_NOT_WAIT on a later matched draw.
  4. HUD defaults (Sean's tuning, repo ini and code fallbacks): element scale 1.35, size 1.60, distance 0.01, spread 0.04, left x 0.18 and y 0.02, top y -0.07. Room stays 1.00.
- **F20 (BUILT, NOT FLOWN; flies with F19):** TEMPORARY `fix.native_on_foot_body_mode` (game | third; default game). While native stereo is on foot and not aiming down sights, the game's own model mode is third (first-person avatar hidden, third-person body and weapon shown); ADS (Flags2 bit 5) uses first-person models. Two hooks, installed when the key first reads third: a SetMode detour (EliteDangerous64.exe+0x1A112B0, 22-byte prologue verified) that rewrites the game's request 0 to 1 (0 under ADS) for the local manager only, and a povCamera entry hook (+0x19B1240, 32 bytes verified, steal 6) that holds the local manager's mode and restores the game's once when the hold ends. Explorer Cam's FindJoint sites (+0x19B12D5, +0x19B1359) are past the steal.
- **Open hypotheses:**
  - H-clip: the torso is hidden by its clip modifier (enable 1 with the amount above cb2[5].w). F19 tests it: unclip on should show the torso; the clip probe shows whether the game re-applies enable 1 between our writes.
  - H-threshold: cb2 row 5, w is the discard threshold. The cb2 probe reads it; if the amounts sit below it, H-clip is refuted.
  - H-eye: P1's cam_minus_head sizes an eye offset; the head-in-body assumption is unverified (F18 values looked implausible, not used).
  - H-room: whether the game's own S at [HRC+0x4C4] matches the element scale it lays out (not re-checked).
- **Ruled out:** the cinema request is always 5 (see the end of this doc). Back-face culling hides the native torso (F18).
- **Temporary keys (all go when the arc picks one):** `fix.native_on_foot`, `_method`, `_hud`, `_panels`, `_hud_mesh`, `_hud_mesh_world`, `_hud_follow_order`, `_reticle_probe`, `_hide_fp_body`, `_body_cull` (F18), `_body_unclip` and `_body_cb_probe` (F19), `_body_mode` (F20), `_body_hybrid` (F21). All default off or game, and live.
- **User settings:** `_breathable_visor` (up | game, up), `_reticle_follow` (aim | head, aim), the eight `_hud_<left|top|right|centre>_<x|y>` offsets (F19: left 0.18, 0.02; top y -0.07), `_hud_element_scale` (F19: 1.35; 0 = game, else 0.30..2.00), `_hud_room` (1.00..2.50), and the tuned defaults: distance 0.01 (min 0.01), size 1.60 (max 3.00), spread 0.04, smoothing 40 ms.
- **OPEN, RemLok on foot:** the on-foot visor overlay is not yet identified. It needs a skip-probe or a diff census that excludes the tonemap PS 0x99C21CEB7A699821 (tonemap_admit.h:39).
- **F2 (c884e218):** the gate holds the byte at 0 while the key is on; the native body zeroes the local head parts. Log lines and details are in the journal.
- **Install:** Epic only (Sean: Frontier carries another feature test). `edvr.ini` is not touched unless `--ini` says so. `--target` takes the game directory path.
- **Next:** flight F21 on Epic with `--scope vr`, keys `reticle_follow = game` and `body_hybrid = on` for one run. Check: the hybrid census lines (torso enable 0 and dirty 1; arms, hands and shoulder masks 0 on third; FirstPersonSkeleton mask 0 on first-person; arms and hands kept); the `eye inputs` line on foot, against the derived `eye vs body` line.

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
