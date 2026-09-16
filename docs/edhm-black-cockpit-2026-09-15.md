# Black cockpit with EDHM chained under DLSS

## Status

Updated 2026-09-15 (evening). Cause NOT identified. One flight designed,
its instrument built on this branch, not yet flown.

Symptom: Frontier install, build `v0.17.0-rc.2-5-g749a4e9`, Pimax Crystal
Super on Pimax OpenXR, `fix.temporal_aa = dlss`, EDHM chained through
`[advanced] real_dll = d3d11_edhm.dll`. The headset goes black on cockpit
entry and stays black; the desktop mirror shows the EDHM-coloured cockpit.
Flight B is the black one (`edvr_gfx_20260915_194757.log`); flight A is the
control without chaining, same build, same evening
(`edvr_gfx_20260915_195200.log`), cockpit fine.

The one thing that changed at cockpit entry in B and persisted is the
Deferred UI (post-DLSS UI replay, `src\d3d11\ui_deferred.cpp`) going
active at 19:49:41.090 and applying on every eye frame for 41 s, until it
disabled itself at 19:50:22.181 when the escape menu changed the submit
route. In A it went active at 19:53:28.610 and disabled itself 1.3 s
later on `pixel shader fanout: unsupported opcode 77` (a world shader with
SINCOS). That shader never appears in B: under EDHM, EDVR hooks the real
device below 3Dmigoto and sees EDHM's replacement shader objects, so
several pixel-shader hashes differ and the SINCOS shader is gone. The
active path has never been looked at by a human: the Steam flight 194533
aborted before the tone pass, A ran it for 1.3 s, B ran it for 41 s and
was black. The deferred UI's own doc says "Headset visual quality is not
yet validated".

Open hypotheses:
- H1 (leading): the active path yields a black frame on its own; EDHM only
  unmasks it by removing the shader that tripped the disable. If true,
  main HEAD (eadd586 accepts opcode 77) blacks out every DLSS user in the
  cockpit, EDHM or not: a 0.17.0 blocker.
- H2: EDHM-specific. The post-tone copy draw (A: VS `20F383BBAC05C031`,
  PS `DED8796049C7BB4A`) appears in B with VS `01C3B84C82172B56` and the
  same PS. The deferred UI needs an exact VS+PS match, so in B `postTone`
  is never ready, DLSS receives `cleanLdr` instead of `cleanFinal`, and
  the UI layers skip the post-tone mapping. The code shows that as a wrong
  image, not a black one.

Ruled out: see the list below (void fix, XR copy, DLSS itself, chaining
loss, transition flash, the ini writer).

Next flight: main HEAD plus the luma probe (five stages, every 2 s per
eye) with EDHM chained; then the same build without chaining. The
confirming line is `luma probe: eye=N first black stage is <stage>`; the
signature table below maps each stage to a cause. `--expect-build HEAD`
first.

Ini note: after flight B the live Frontier ini and its mirror carried
`#real_dll = d3d11_edhm.dll` (line 1216), a manual control edit. The
settings writer cannot produce that line (`src\common\iniedit.cpp:547`
passes uncomment = the template line is commented, and the template's
line is live). Both files were restored to the live key on 2026-09-15
evening; comment it out again for the control flight.

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
