# VR Supersampling gate (2026-10-10)

## Status

- **State:** step 2 (the hold) on branch `claude/vr-ssaa-gate`, on top of step 1 (28edab7b). Built, not merged to main. Step 1's flight is in; step 2 has not flown.
- **Decided goal (maintainer):** while Elite's 3D mode is not 0, the game's Supersampling field (render context +0x3564, read by every sizing and shader consumer) is held at 1.0, so HMD Image Quality is the only render control in VR. Flat (the flat profile) and 3D mode 0 are untouched. No new config key. Never write the user's settings files.
- **Startup mechanism (chosen):** an .rdata slot swap at DLL attach. Slot RVA 0x52E8368 holds the wrapper RVA 0x2862780, which calls the fxcfg loader RVA 0x2855A50 with the loader object in r8. The thunk calls the original, then writes 1.0 to the object's SS field (+0x13C) when the decision says held. Evidence in the investigation section below.
- **Menu:** the setter thunk (ui_panel_scale.cpp hookedSetter) passes 1.0 when held and remembers the requested value. The setter hook is installed at DLL attach too, so the hold does not wait for `fix.ui_quality`.
- **Mode tracking:** Settings.xml at the loader; re-read 1.5 s after each setter call, on the render thread at a frame boundary (one small read). A 3 -> 0 change is logged and not chased: the field stays at 1.0 until the next apply or restart. 0 -> on is held at the next apply.
- **Panel sizing:** chooseSupersampling takes 1.0 (source `held`) while held, so the factor and the size budget match the field.
- **Notice:** a toast "Supersampling <x> is held at 1.0 in VR: use HMD Image Quality to set resolution", once per change of a requested value other than 1.0, while held. The measured render-below-eye notice is kept for the cases the hold does not cover (3D mode 0, flat); it cannot fire under the hold.
- **Finding, not changed:** the flight's setter range is 0 to FLT_MAX, which is the game's own range. The read is right. EDVR's live check (ui_sizing_math.h uiLiveSupersamplingValid) rejects a maximum above 8.0, so the live value has never been believable and the panel factor has always used the .fxcfg value. The hold bypasses that check while held. Fixing the ceiling changes VR panel sizing with 3D off, so it is a separate decision.
- **Open hypotheses:**
  - H1 mode values: 3 = VR (confirmed at startup in the flight). 0 = off is inferred from the menu's 3D off and on passing through the setter; the mode read at that time is not yet in a log.
  - H2 order: the loader runs before the first Present (the hold log line should precede it; unverified). The first setter call in the flight was frame 6110, when the menu changed.
  - H3 live 3D mode: still not located (step 1). The hold uses the Settings.xml mode.
  - H4 object relation: render context = loader object + 0x3428 (so the SS field is ctx+0x3564). Each setter line prints ctx and the loader object; check ctx - object = 0x3428.
  - H5 file contents: the game may write its own SS value to the .fxcfg from the field. If so, a held session would save 1.0 over the user's value. Check SSAAMultiplier in the .fxcfg after a held apply.
- **Ruled out:** a static-address read of the 3D mode (step 1: the gate reads it through virtual calls on heap objects).
- **Next:** one VR flight on Frontier (the step-2 commit, named in the DLL's FileVersion): (a) start with 3D on and the .fxcfg at Supersampling 0.85: expect `holding ... at startup` before `first Present`; (b) change Supersampling in the menu: expect `holding ... at menu` and the toast; (c) switch 3D off: expect the mode re-read line; (d) read the .fxcfg after the apply (H5). Then `python tools\edvr_log.py --target frontier --expect-build HEAD --vr-supersampling`.
- **Doc rule:** dated entries below; keep this block current.

## 2026-10-10 step-1 flight (edvr_gfx_20261010_092344.log, build 28edab7b)

- Startup at 09:23:44.988: StereoscopicMode=3, file SSAAMultiplier 1.0000, HMDRenderTargetMultiplier 0.5000. First Present 09:23:45.081 (frame 1). Setter calls 0, getter not yet read.
- The first getter read and the first setter call both came at frame 6110 (09:24:30), when the menu changed Supersampling. So at startup the game sizes from the loader's stored value without calling either hooked function.
- Setter calls 1 to 4 passed 0.5, 0.85, 1.25, 1.0, with ctx+0x3564 following. Calls 5 to 7 passed 1.0 (HMD Quality 0.65, then 3D off and on). A 12.4 s runtime stall at 09:25:55; the OpenXR session survived.
- EDVR's "chosen from .fxcfg" lags one change: the game writes the .fxcfg after the setter.

## The startup mechanism: investigation (read-only, 2026-10-10)

Method: `python -I` over the Frontier exe's bytes (build 332841: PE stamp 1788384820, image 104894464 bytes, both installs); no game memory read or written.

- The fxcfg loader is RVA 0x2855A50. Its only direct caller is the wrapper RVA 0x2862780: `sub rsp,0x28; mov rcx,r8; call 0x2855A50; mov al,1; add rsp,0x28; ret`.
- The wrapper's address appears in exactly one place in the image: the .rdata slot at RVA 0x52E8368. So the wrapper is a virtual call, and its caller is some object's method. That caller is not identified in this step.
- The loader writes a table in its object: 5 entries of 12 bytes from +0x124 (entry 2, the SS entry, at +0x13C). Each entry has a min at +0x140 and a max at +0x144 for the SS entry. The clamp takes min(max, v) with floor min. The flight's range of 0 to FLT_MAX shows max is FLT_MAX by default.
- The setter's context: ctx = [self+0x18], SS at ctx+0x3564 = ctx+0x3568 min, +0x356C max. The loader's SS field +0x13C is ctx+0x3564 when the object is ctx-0x3428. That relation is the one H4 checks.
- Timing: DllMain runs before the game's entry point, so an install there precedes every game call. EDVR's graphics init (config, log, module pin) runs at the first device creation export (d3d11_proxy.cpp initOnceCallback). The loader's position relative to that export is not established, so the loader thunk asks for the init itself (edvrGraphicsEnsureInitialised) before it reads the profile.
- Install gate: the PE stamp and image size, the wrapper's 19 bytes, its call landing on the loader, and the slot holding the wrapper. Any mismatch writes nothing and the device-creation line says why.
- Not chosen: installing at device creation (hookDevice). The loader may already have run by then, so that hook would be too late. Not relied on.
- Not chosen: patching a mid-function instruction of the sizing (FUN_14288E3A0 / FUN_14284CB70). No safe point identified; the field is held at its one writer instead, which covers every consumer.

## The hold, as built (step 2)

- `src\d3d11\vr_ssaa_hold_math.h` (pure): `parseStereoscopicMode` (0..6, whitespace allowed, else refused), `decide(holdAllowed, modeKnown, mode, requested)` (held only for a VR profile, a known mode and mode != 0), and the toast formatter.
- `src\d3d11\vr_ssaa_hold.cpp/.h`: DllMain's `vrSsaaHoldEarlyInstall` (pure memory), the loader thunk, `vrSsaaHoldSetterValue`, `vrSsaaHoldFrameBoundary` (the re-read), `vrSsaaHoldNoticeDue`, `vrSsaaHoldReadSettings`, and the shutdown that puts the slot back.
- `src\d3d11\ui_panel_scale.cpp`: `uiPanelScaleEarlyHooks` (the setter and getter hooks at DLL attach, once), `hookedSetter` passes the hold's value first, `chooseSupersampling` takes kHeld while held.
- `src\d3d11\d3d11_proxy.cpp`: DllMain's attach calls the early install; `edvrGraphicsEnsureInitialised` wraps the once-only init for the loader thunk.
- `src\d3d11\menu.cpp`: the held toast.
- `tools\edvr_log.py --vr-supersampling`: the hold lines (holds, modes, notices, refusals, installed) with their own verdicts; the measured-notice reader is unchanged.
- `tools\vr_ssaa_hold_test`: the pure decision, the parse, the toast, run by build.bat.

## Log lines (step 2)

- `vr ssaa gate: hold installed at DLL load: loader wrapper 0x2862780 (slot 0x52E8368) ...` (at device creation), or `hold refused, nothing written: <why>`.
- `vr ssaa gate: holding Supersampling at 1.0 (requested <x>, 3D mode <m>, at startup); loader object 0x..., field 0x...`
- `vr ssaa gate: holding Supersampling at 1.0 (requested <x>, 3D mode <m>, at menu)` (once per change of the requested value).
- `vr ssaa gate: released: the game's Supersampling <x> passes again (3D mode <m>)`.
- `vr ssaa gate: 3D mode <a> -> <b> (Settings.xml, re-read 1.5 s after a Supersampling call)`.
- `vr ssaa gate: 3D mode is now 0; the held Supersampling stays at 1.0 until the next apply or restart (not chased)`.
- `vr ssaa gate: startup not held (<flat profile|3D mode unknown|3D mode 0>)`.
- Step 1's lines remain; the setter line now also prints the render context and the loader object.

## Verified in the build

(filled in after the build and the flight; see the commit message)
