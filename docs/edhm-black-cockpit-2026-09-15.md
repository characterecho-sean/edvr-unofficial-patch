# Black cockpit with EDHM chained under DLSS

## Status

Updated 2026-09-16 (later morning). Cause FOUND in code by a parallel
session on main; fix BUILT and INSTALLED to Frontier, NOT FLOWN.

Cause (main 2c6e820, "Retain shader inputs without reflection
metadata"): Elite's shipped shaders carry no RDEF chunk, so D3DReflect
succeeds and reports zero bound resources. The deferred UI's capture
mask (`uiDeferredReflect`, `src\d3d11\ui_deferred_draw.h`) came from
that reflection, so the tone-map replay bound no constant buffers
(exposure, constants) and no colour LUT: a fine snapshot went in, black
came out. The fix reads the SM5 `dcl_constantbuffer` / `dcl_resource`
declarations from the bytecode instead. Companion 9d30ac6 keeps the
path active through the dual-source cockpit glass (replays that draw
into clean HDR), so the "world blend unsupported" decline that made
Steam look healthy no longer trips: the feature now runs for every DLSS
user, and the flight below must show it working, not merely absent.

Symptom (for the record): with `fix.temporal_aa = dlss` the headset went
black on cockpit entry, desktop mirror fine; Frontier with EDHM chained
and Steam without EDHM alike. DLSS off restored the view; DLSS back on
re-enabled the path (leaving dlss/dlaa clears `failed`,
ui_deferred.cpp:354) and the black returned, except on Steam where the
glass draw declined the feature 9 ms later.

Where the black was made: the probe flight (`edvr_gfx_20260916_051454.log`,
build 16adaae, EDHM chained) read `game` about 0.05 and `clean_hdr`
about 0.03 on every report, both eyes, while `dlss_in` (= `cleanFinal`,
the post-tone replay of the tone-map replay) read 0.000/0.000/100%.
That matches the cause: the replay draws ran and wrote black.

Not the cause (evidence under `## Ruled out`): EDHM, the VR half and the
runtime, DLSS itself, the game frame, the snapshot, H2 (post-tone VS
mismatch), scissor, topology and input layout, null views, command lists
not executing.

The second instrument (eight stages, sentinels, reference replay; design
kept under Flight design) was never built: its implementer was stopped
when the fix landed. Build it only if the flight below is still black.

Next flight: Frontier, EDHM chained, `fix.temporal_aa = dlss`, build
v0.17.0-rc.2-16-g2c6e820 installed 2026-09-16. Confirming lines:
`luma probe: eye=N first black stage is none` on every report while
`deferred=1 apply=1`, and `Deferred UI: replayed dual-source glass into
clean HDR`; the cockpit visible in the headset with its HUD. Read with
`python tools\edvr_log.py --target frontier --expect-build HEAD --grep
"luma probe|Deferred UI"`. A still-black flight reads `dlss_in` 0.000
again and reopens `## Ruled out`; a decline line means the path was
off, which is not a pass.

Ini note: `fix.temporal_aa` restored to `dlss` on 2026-09-16 in the live
Frontier ini and its mirror; `real_dll = d3d11_edhm.dll` (line 1216)
stays; comment it out for a no-EDHM control.

## Evidence, 2026-09-15

Flight B (EDHM chained, black):

```
[19:47:58.202] chaining through ...\d3d11_edhm.dll -- 41 export(s) from it, 8 from the system d3d11.dll, 0 unresolved
[19:47:59.766] openxr resolution: ... 3964 wide = 3964x3913 per eye
[19:49:41.047] Deferred UI: captured X draw, gen=11739 VS=81216C77F90DEDD6 PS=16F88966C091FC55 eye=0 2576x2544 HDR=000002D22A60C760.
[19:49:41.090] Deferred UI: active. DLSS receives world colour; captured UI draws run at output resolution after reconstruction, with original shaders, tone map and depth/stencil. No deferred UI motion/history pass.
[19:50:19.424] ... captured=71284 applied=6520 declined=0, snapshots copied=156.03 MiB allocated=156.03 MiB
[19:50:22.180] ui depth: a new interface family -- vs 4EF6DDB075A927FA ps 8ADB2A81A45E8A4B ... interface projection
[19:50:22.181] Deferred UI: submit route not matched: submitted=000002D22A620660 tone0=0000000000000000 tone1=0000000000000000; original frame retained.
[19:50:22.181] Deferred UI: original frame retained: submitted colour has no complete matching replay (draws=11, VS=01C3B84C82172B56 PS=DED8796049C7BB4A).
```

Applied 6520 over about 38 s at 90 Hz is one apply per eye frame. The VR
half was healthy the whole session: `native_summary,waits=11503,
submits=23002,pairs=11501,copied_eyes=22998,...,pose_failures=0`,
`native_temporal_summary,frames=11503,left=11499,right=11499,failures=0`,
`device_module,route=mapped,path=C:\WINDOWS\system32\d3d11.dll`, no error
line, the void fix cleared 0 frames in every window, the transition-flash
latch withheld 2 frames in the whole session. DLSS treated every eye
submit from the menu onwards (21504 before the cockpit), and the menu was
visible, so DLSS itself is not black. The game exit sample is at
19:50:25.941, 3.7 s after the disable; whether the image came back in
those seconds is unknown.

Flight A (no chaining, control):

```
[19:53:28.610] Deferred UI: active. DLSS receives world colour; ...
[19:53:29.892] Deferred UI: original frame retained: pixel shader fanout: unsupported opcode 77 (draws=1, VS=8C091FFD08644E02 PS=3EAF4DB5B3E21089).
```

B has no line containing `fanout`, `opcode`, `8C091FFD08644E02` or
`3EAF4DB5B3E21089`. Shader pairs that differ between the flights, same
vertex shader, different pixel shader (EDHM's replacements as seen from
below 3Dmigoto):

| Draw | A | B |
|---|---|---|
| compact panel | VS 81216C77F90DEDD6 PS A2965EC2931A39C8 | PS 16F88966C091FC55 |
| flight HUD coverage | VS B7790CBFC6554097 PS 8DEF46452FA459F5 | PS B1CA8D8EEF7C886D |
| late composite | VS A888D51024D9798E PS 9107E72CB016CC02 | PS BBA33147981BC99A |
| post-tone copy | VS 20F383BBAC05C031 PS DED8796049C7BB4A | VS 01C3B84C82172B56, same PS |

The post-tone pair is the odd one: the vertex shader differs. Whether the
B pair is the same draw is what the new one-shot line settles.

## Evidence, 2026-09-16: the probe flight and the Steam flight

Frontier flight (`edvr_gfx_20260916_051454.log`, build
`v0.17.0-rc.2-13-g16adaae`, the probe, EDHM chained):

```
[05:16:16.001] Deferred UI: retained post-tone sampled draw gen=9791 eye=0 source=000001B195A9DF20 target=000001B195A9A2A0 tails=0.
[05:16:16.002] Deferred UI: post-tone pixel shader DED8796049C7BB4A bound with an unrecognised vertex shader 01C3B84C82172B56; the post-tone copy is not captured, so DLSS receives the unmapped world colour.
[05:16:16.012] Deferred UI: active. DLSS receives world colour; ...
[05:16:16.533] luma probe: eye=0 game=0.058/0.893/77% clean_hdr=0.033/1.522/82% dlss_in=0.000/0.000/100% dlss_out=0.000/0.000/100% final=0.000/0.000/100% | deferred=1 sampled=1 aliases=1 draws=9 apply=1
[05:16:16.533] luma probe: eye=0 first black stage is dlss_in (game 0.058 clean_hdr 0.033 dlss_in 0.000 dlss_out 0.000 final 0.000).
[05:16:19.123] menu: fix.temporal_aa dlss -> off (written to edvr.ini; live).
[05:16:20.442] menu: fix.temporal_aa on -> dlss (written to edvr.ini; live).
[05:16:23.069] menu: fix.temporal_aa dlss -> off (written to edvr.ini; live).
[05:16:25.165] luma probe: eye=0 game=- clean_hdr=- dlss_in=- dlss_out=- final=0.056/0.864/71% | deferred=0 sampled=0 aliases=0 draws=0 apply=0
[05:16:26.159] menu: fix.temporal_aa on -> dlss (written to edvr.ini; live).
[05:16:27.200] luma probe: eye=0 game=0.048/0.826/74% clean_hdr=0.043/1.686/69% dlss_in=0.000/0.000/100% dlss_out=0.000/0.000/100% final=0.000/0.000/100% | deferred=1 sampled=1 aliases=1 draws=11 apply=1
```

Every report while the feature was active (05:16:16 to 05:16:22 and
05:16:27 to 05:16:29, both eyes) reads the same: `game` 0.04 to 0.07,
`clean_hdr` 0.02 to 0.06 (linear HDR, max up to 2.3), `dlss_in`
0.000/0.000/100%, `dlss_out` and `final` the same, no HUD at all in
`final`. No decline line in the whole flight. `sampled=1` and the
retained post-tone lines show the post-tone draw captured, so
`prepare()` returned `cleanFinal` (ui_deferred.cpp:624) and that texture
is black in full. The one-shot line names a second draw that shares the
post-tone pixel shader; it is not the routed post-tone draw. With DLSS
off (05:16:23 to 05:16:26) the pass skipped its DLSS block and `final`
read 0.056: the view came back, as Sean saw; switching back to DLSS
re-enabled the feature and the black returned at once.

Steam flight (`edvr_gfx_20260916_050901.log`, build
`v0.17.0-rc.2-6-g377c520-dirty`, no probe, no chaining; main's fanout
change eadd586 is not in that build):

```
[05:10:43.957] Deferred UI: retained post-tone sampled draw gen=11282 eye=0 source=000001990E5188A0 target=000001990E5151A0 tails=0.
[05:10:43.968] Deferred UI: active. DLSS receives world colour; ...
[05:11:00.031] menu: fix.temporal_aa dlss -> off (written to edvr.ini; live).
[05:11:01.614] menu: fix.temporal_aa off -> on (written to edvr.ini; live).
[05:11:02.844] menu: fix.temporal_aa on -> dlss (written to edvr.ini; live).
[05:11:02.853] Deferred UI: disabled until AA is switched Off and back on, or the game restarts. ...
[05:11:02.853] Deferred UI: original frame retained: world blend unsupported (enabled=1, colour=2/16/1, alpha=2/18/1) (draws=6, VS=F512712C40D93C12 PS=4A71EB0D34E9F2EF).
```

Sean saw the black cockpit without EDHM in the 16 s between "active"
and switching DLSS off. Switching back re-enabled the feature
(ui_deferred.cpp:354 clears `failed` while the mode is not dlss/dlaa),
and 9 ms after the mode returned to dlss a dual-source-blend world draw
(D3D11_BLEND_ONE over SRC1_COLOR) tripped the decline, which lasts the
session. That is why DLSS was fine afterwards on Steam: the path was
switched off, not working. Under EDHM the same toggle re-enabled the
feature and nothing tripped it. The tone-map producer pair is the same
on both installs (`vs_642017A6FEDAE0E8`, `ps_99C21CEB7A699821`): EDHM
does not replace the shaders the replay runs. The decline that protected
flight A after 1.3 s (opcode 77) never fired on Steam in 16 s, so the
protection is scene-dependent even without eadd586.

## Ruled out

- ruled out: the black void fix painting black, because B cleared 0 void
  frames in every window.
- ruled out: an XR-side copy or swapchain failure, because copied_eyes is
  22998 of 23002 submits with 0 failures and no error line, on a separate
  device from the mapped System32 d3d11.
- ruled out: DLSS producing black in general, because 21504 treated eye
  submits showed the menu normally before the cockpit.
- ruled out: chaining lost mid-session, because the chaining line is
  present and the mirror showed EDHM's colours.
- ruled out: the transition-flash latch withholding layers, because only
  2 frames were withheld in the whole session.
- ruled out: the settings writer commenting out `real_dll`, because
  `mergeIni` uncomments a user-present key (iniedit.cpp:547-548) and its
  removed branch comments out the template's empty-valued line, neither
  of which yields `#real_dll = d3d11_edhm.dll`; B had no menu write.

Added 2026-09-16, from the probe flight and the Steam flight:

- ruled out: H2 (the post-tone vertex-shader mismatch leaving `postTone`
  unready under EDHM), because both flights logged `retained post-tone
  sampled draw` and every black probe report carries `sampled=1`; the
  one-shot names a second draw sharing the pixel shader, not the routed
  one.
- ruled out: EDHM as the cause, because the Steam flight without chaining
  ran the same path black for 16 s with the same producer shaders.
- ruled out: the VR half and the runtime, because `final` is black before
  the hand-off.
- ruled out: DLSS blackening a valid input, because `dlss_in` is already
  0.000/0.000/100%.
- ruled out: the game's eye frame and the clean snapshot, because `game`
  and `clean_hdr` read normal values on every black report.
- ruled out (by code, not flight): the replay draws losing topology or
  input layout after the recorder's ClearState, because
  ui_deferred_draw.h:238 refuses an undefined topology at capture and
  :266 restores layout, topology, vertex and index buffers in `bind`.
- ruled out (by code, not flight): the command lists never reaching the
  GPU, because `execute` (ui_deferred.cpp:260) goes through
  vscreen.cpp:5761, which calls the real ExecuteCommandList pointer for
  the owner context.

## What the code says (trace of 2026-09-15)

`prepare()` (ui_deferred.cpp:553-618) matches the submitted texture to
an alias of a complete eye (564-565, else "submit route not matched" and
a decline). It records one command list on the deferred `recorder`
context: a seed pass reprojecting `e.cleanHdr` (the R11G11B10 snapshot
copied at the first captured UI draw, line 446, extended by the fanned-out
world draws) into `renderer.hdr` at output size (590-591), the captured
tone shader over it into `renderer.base` (593-594), the captured UI draws
into `renderer.hdr` and `renderer.transmission` (597-602), the tone shader
again into `renderer.ui` (603-604), the post-tone mapping only when
`postTone.ready()` (606-611), the combine compute shader over
{`world` = an R8G8B8A8_UNORM view of the output texture, base, ui,
transmission} into `renderer.composite` (612-613), the tails (614), and
`CopyResource(output, composite)` (615). It returns `cleanFinal` when
sampled, else `cleanLdr` (617). In temporal_pass.cpp the pass calls
prepare at 4018 with `e.dlSubmit` as output, DLSS writes its output into
`e.dlSubmit`, and `uiDeferredApply` (4288) executes the list, so the
combine reads the DLSS output through `world` and the composite replaces
it. Every failure branch either declines (B declined 0 times) or returns
null for that frame (B applied on every frame), so no code line yields a
black image while reporting success.

Addendum 2026-09-16, the two replays that make `dlss_in`. Both run on the
deferred `recorder` context and are executed on the immediate context
inside prepare: the tone replay (526-545) and the post-tone replay
(728-747). Each does `recorder->ClearState()`, then `bind(recorder,
originalPixelShader(), target.rtv, ...)`; ui_deferred_draw.h:266 restores
input layout, topology, the 32 vertex-buffer slots and the index buffer,
and :238 refuses an undefined topology at capture. The source is set
explicitly at PS slot t1 for the tone replay (540, the same slot the
apply-time tone draw uses at 611) and t0 for the post-tone replay (742).
Then `draw`, `FinishCommandList(FALSE)` (252-262) and
`vScreenExecuteCommandListRaw` (vscreen.cpp:5761-5764), which calls the
real ExecuteCommandList for the owner context, so the list does reach
the GPU. `Surface::ensure` (33-47) creates texture, SRV and RTV together,
so no bound view is null. The tone and post-tone state checks (533/704)
require scissor disabled. None of this is black by construction; the
open question is what the capture did NOT record, such that the recorded
draw is legal but draws nothing or draws black. The doc's `## Status`
lists the candidates.

## Flight design: the luma probe

Five textures sampled on a 16x16 grid every 2 s per eye (mean/max luma
and the share of black samples), all in `src\d3d11\luma_probe.cpp`:

| Stage | Texture | Where |
|---|---|---|
| game | the texture the game submits | temporal_pass.cpp before prepare |
| clean_hdr | `e.cleanHdr` right after the seed copy | ui_deferred.cpp:446 |
| dlss_in | what prepare returned | after prepare |
| dlss_out | `e.dlSubmit` after DLSS, before apply | before 4288 |
| final | the pass's return value, what goes out to the VR half | end of the pass |

Signatures, reading `first black stage is`:

| First black stage | Cause |
|---|---|
| game | the game's own eye frame is black under EDHM (mirror shows another surface) |
| clean_hdr | the snapshot is taken before the world is drawn, or the copy fails |
| dlss_in | the tone replay into cleanLdr/cleanFinal does not run or writes black |
| dlss_out | DLSS turns a valid replayed input black (format or exposure) |
| final | the combine, tails or copy paint black over the DLSS output |
| none, headset black | the VR half or the runtime |

Each line also carries `deferred=`, `sampled=` (post-tone recognised),
`aliases=`, `draws=`, `apply=`. The control flight on the same build
shows the same line with `sampled=1`; if it also reads black at a stage,
H1 holds and the shipping line for 0.17.0 moves. If the code never ran,
no `luma probe:` line appears at all; if the deferred UI is inactive, the
stages print `-`.

Reading the numbers: a round is armed at the end of a pass, so clean_hdr
(sampled during the next frame's draws) and the other four (sampled in
the next pass) describe the same frame; a pass that returns early delays
the round by a frame. clean_hdr is linear HDR before tone mapping, so a
dark cockpit can read well under 0.01 there without being black: compare
it with the control flight's value, not with the threshold. A stage that
prints `fmt=N?` needs its format added to the decoder. The staging copies
take about 240 MB of system memory per eye at the flown resolution; they
are mapped without waiting, and blocked only after 30 passes. The
one-shot `post-tone pixel shader ... bound with an unrecognised vertex
shader` line fires from the per-draw entry point for any draw of a
generation that has captured UI, once per vertex shader.

### Second instrument, 2026-09-16: the discriminating flight

Built on the probe flight's answer (`dlss_in` black, snapshot fine). The
probe grows to eight stages in pipeline order: game, clean_hdr,
clean_ldr (the tone replay's target), clean_ldr_ref (the reference
replay below), dlss_in, dlss_out, ui_layer (`renderer.ui` after apply),
final. Three additions let one flight separate the remaining causes:

- Two-level sentinel clears on the replay targets that feed the picture
  (cleanLdr, cleanFinal, renderer.ui): the immediate context clears the
  target to grey A = 0.5 right before the command list executes, and the
  list itself clears it to grey B = 0.25 right before the replay draw. A
  stage reading 0.500 means the list never ran; 0.250 means it ran and
  the draw wrote nothing (geometry, viewport, target slot, arguments);
  0.000 means the draw ran and wrote black (its inputs); a normal value
  means the replay works.
- A reference replay: in the same tone hook, on the immediate context
  with the game's live state, only the render target (cleanLdrRef) and
  t1 (cleanHdr) swapped, the same DrawInstanced.
- One-shot capture detail at the tone and post-tone captures: the
  reflection masks, each captured SRV slot's resource kind/format/size
  with the first bytes of small snapshot textures, each captured
  constant buffer's first 16 floats read from the snapshot copy, the
  game's t1 view against cleanHdr's, viewport, raster and IA state.

| clean_ldr | clean_ldr_ref | ui_layer | Reading |
|---|---|---|---|
| 0.500 | any | any | the synchronous list does not execute (contradicts the raw pointer at vscreen.cpp:5761) |
| 0.250 | normal | 0.250 | bind() sets a state that draws nothing on the deferred context; the capture-detail line names it |
| 0.250 | 0.250 | any | the draw covers nothing even live: draw arguments or the target |
| 0.000 | normal | 0.000 | the snapshot inputs are wrong (a zero constant buffer or texture); the float/byte dumps name the slot |
| 0.000 | 0.000 | any | cleanHdr at t1 is not enough for this tone shader, or the live and recorded draws differ from the game's |
| normal | normal | black or grey | the synchronous replays work and the apply-time list is the fault; dlss_in then reads the post-tone sentinel |

The active note carries `Probe stages: 8, sentinels A=0.5 B=0.25,
reference replay on.` so a log from this build is self-identifying. The
sentinels and the reference replay are diagnostic and come out with the
fix; the reference replay costs one extra fullscreen draw per eye per
frame while the deferred UI is active.

## Journal

### 2026-09-15 evening

Two logs read against each other (B black with EDHM, A the control);
the deferred UI's active path isolated as the only persistent change at
cockpit entry; the code traced; nothing black by construction; the probe
designed. Ini restored to `real_dll = d3d11_edhm.dll` on both copies.

Probe implemented by a subagent and reviewed. Three changes in review:
the staging copy uses `CopySubresourceRegion` on subresource 0, because
`CopyResource` refuses a source with more mips or slices and the stale
staging bytes would then read as a false black; rounds are armed at the
end of a pass rather than its start, so the clean_hdr hook (which runs
during the draws, before the pass) is never skipped; the post-tone
one-shot moved from the route capture (only reached with a zero verdict)
to the per-draw entry point.

### 2026-09-16 morning

Probe flight flown in Frontier with EDHM chained (build 16adaae,
`--expect-build HEAD` clean): first black stage is `dlss_in` on every
report, both eyes, `game` and `clean_hdr` fine, `sampled=1`. H1 confirmed
by Sean's Steam flight without EDHM (377c520-dirty, no probe): black for
the 16 s until DLSS off; the re-enable declined 9 ms later on "world
blend unsupported", which is why DLSS then "worked" on Steam and not
under EDHM. H2 refuted: the post-tone draw was retained in both flights.
Branch fast-forwarded onto origin/main (fc18b4d). The code trace of the
replay path went to a subagent; the Status block carries the candidates
and the fallback instrument.

Later morning: a parallel session found the cause on main and pushed
9d30ac6 and 2c6e820 (Status block): the shaders have no RDEF, so the
reflection-derived capture mask was empty and the tone replay bound no
constant buffers and no LUT. The second instrument's implementer was
stopped before it wrote a line; the design stays under Flight design in
case the flight is still black. The pointer added to the star-navigation
review doc was dropped: that session rewrote the doc and it records the
black itself now. Branch fast-forwarded to 2c6e820.

Rebuilding for the flight tripped the rig pool twice on timing rigs:
openxr_present_test failed 13 checks at the first graphics acquire in an
8-wide pool, openxr_module_test failed 7 in a 4-wide one; each passes
alone (168, and 247/253 per variant, 0 failures) with no crash sentinel
and no other build on the machine. Built with the pool serialized
(`EDVR_JOBS=1`).
