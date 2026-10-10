# VR Supersampling gate (2026-10-10)

## Status

- **State:** steps 1-3 committed on `claude/vr-ssaa-gate` (28edab7b, c5491ab3, cf11d33c), flown in two Frontier sessions on cf11d33c (logs `edvr_gfx_20261010_102911.log`, `..._103029.log`). Step 4 (this commit): the sizing watch (H7), the window IAT observers (H6), GAP 2 fix, startup-line change-only. Not merged.
- **Goal (decided):** while the 3D mode is not 0, hold the game's Supersampling field at 1.0 so HMD Image Quality is the only VR render control. Flat and 3D mode 0 untouched. No config key. Never write the user's settings files.
- **Works (flights):** hold installed at DLL load; startup loader hold line (session 2 at 0.85, requested 0.85); menu hold at 0.85 with the toast; re-read saw 3 -> 0 and 0 -> 4; the game saves the menu value to the .fxcfg (0.85), not the held field (H5 answered).
- **Gap 1, FALSIFIED: "one field covers every consumer".** Session 1, after `holding ... at menu` (10:29:52.278), new depth targets 2227x2153 appeared at 10:29:53.379 (278 and 308 draws). 2227 = trunc(2620 x 0.85). The eye-sized targets stayed 2620x2533 (HMD Quality 0.65 x 4032). The field stayed 1.0, so some consumer sizes from the requested value by another path. H7 names it.
- **Gap 2, FIXED here:** on a 0 -> on mode change the hold re-applies at once, to the render context the setter last named (`at mode change` line).
- **Startup loader, UNPROVEN:** session 2's loader ran about 4.4 s after the first Present, and about 12 times (presets, then Custom). So the startup write is not before the first sizing. See H8.
- **Open hypotheses:**
  - H1 modes: 3 = VR (startup, both sessions). 0 and 4 seen in re-reads. 4 is not identified; the 0 -> 4 switch is the windowed switch in H6. 5 (HMD Cinema) not seen.
  - H4 REFUTED: render context and loader object are different allocations. Session 1: ctx 0x1728C199820, loader 0x1C110FF1D0. Session 2: ctx 0x...EC860, loader 0x...F610 (the coordinator's abbreviated values). Neither difference is 0x3428. So the loader write does not touch the field the setter reads.
  - H5 ANSWERED: the game writes the menu's value to the .fxcfg. The held field is not saved.
  - H6 PARTIAL: at the 0 -> 4 switch (session 1 and 2), window style 0x94020000 at 3840x2160 became 0x14C80000 at 1280x768 (frame 4572), then ResizeBuffers 1280x768 format 28 flags 0x2 from game RVA 0x521CE5. No ResizeTarget. SetFullscreenState(FALSE, null target) from game RVA 0x50B30B about 10-20 s later in both sessions (likely at quit). So the switch is a window restyle plus a buffer resize, not a DXGI fullscreen change. Who restyles is open: the IAT observers (below) test it.
  - H7 OPEN: which game code sized the 2227 targets. Test: the sizing watch's `vr sizing:` lines (callers as game RVAs) after a menu change.
  - H8 OPEN: does the startup hold reach ctx+0x3564? Test: the first setter call's `before` value in a flight that changes nothing in the menu before it. 0.85 means no; 1.0 means yes. The copy from loader table to render context is not located.
  - H9 mode 4: what it is (the menu's 3D mode name for it).
- **Ruled out:** a static-address read of the 3D mode (step 1: virtual calls on heap objects). H4 (same-object relation), above.
- **Next flight (VR, Frontier, the step-4 commit):** (a) menu Supersampling change: read `vr sizing:` lines and their callers (H7). (b) 3D Off to HMD: read `vr window:` lines (who restyles, H6) and `vr sizing:` lines. (c) start with the .fxcfg at 0.85, then the first setter call's `before` (H8). Then `python tools\edvr_log.py --target frontier --expect-build HEAD --vr-supersampling`.
- **Doc rule:** dated entries below; keep this block current.

## Investigation: the startup mechanism (2026-10-10, read-only)

Frontier exe, build 332841 (PE stamp 1788384820, image 104894464). Byte scans and capstone; no game memory touched.

- The fxcfg loader is RVA 0x2855A50. Its only direct caller is the wrapper RVA 0x2862780 (`sub rsp,28; mov rcx,r8; call loader; mov al,1; add rsp,28; ret`). The wrapper's address is in one place: .rdata slot RVA 0x52E8368. So it is a virtual call.
- The loader writes a table in its object: SS entry at object+0x13C, min +0x140, max +0x144 (the flight's range 0 to FLT_MAX is the real max).
- The hold writes the loader object's SS field at DLL-load-time swap (vr_ssaa_hold.cpp). H4 refutes that this object is the render context. So the startup write may not reach the sizing. H8 tests it.
- Rejected: hooking at device creation (the loader can run after it, as session 2 shows). Patching a sizing instruction mid-function: no safe point identified.

## Flight evidence (step 1 and step 2)

- Step 1 flight (build 28edab7b, edvr_gfx_20261010_092344.log): startup StereoscopicMode=3, file SSAAMultiplier 1.0, HMDRenderTargetMultiplier 0.5, first Present 09:23:45.081 (frame 1). First getter and setter at frame 6110 (09:24:30), the menu change. Setter calls 1-4 passed 0.5, 0.85, 1.25, 1.0, with ctx+0x3564 following. Calls 5-7 passed 1.0 (3D off and on). A 12.4 s stall at 09:25:55; the OpenXR session survived. EDVR's "chosen from .fxcfg" lagged one change (the game writes the .fxcfg after the setter).
- Range: the setter's range prints 0 to FLT_MAX, the game's own range. EDVR's live check (ui_sizing_math.h uiLiveSupersamplingValid, hi <= 8) rejects it, so the live value was never believable and the panel factor used the .fxcfg value. Pre-existing, not changed; the hold bypasses it while held.
- Step 2 flight, session 1 (cf11d33c, 10:29:11): startup hold lines at 10:29:18 (file 1.0, requested 1.0, loader object 0x1C110FF1D0, field 0x1C110FF30C). Menu hold at 10:29:52.278 (requested 0.85), setter call 1 at frame 4426: passed 1.0; ctx 0x1728C199820; before 1.0; after 1.0; range 0 to FLT_MAX; HMD Quality 0.650; panel factor 0.5198; chosen Supersampling 1.0 from held.
- Step 2 flight, session 2 (10:30:29): first Present 10:30:29.439. Loader hold lines from 10:30:33.818 (file 0.85; requested values 0.67, 0.77, 1.25, 1.0 across presets), loader object 0xED6E2FF610. The loader ran about 4.4 s after the first Present.

## The hold, as built

- `vr_ssaa_hold_math.h` (pure): `parseStereoscopicMode` (0..6), `decide` (held only for a VR profile, a known mode and mode != 0), the toast formatter.
- `vr_ssaa_hold.cpp/.h`: DllMain install (pure memory; a refused VirtualProtect is recorded and nothing is written); the loader thunk (change-only logging with the run number); `vrSsaaHoldSetterValue` (1.0 while held, arms the re-read and the sizing watch); `vrSsaaHoldFrameBoundary` (the re-read, the mode-change line, the watch, GAP 2's re-hold on the setter's context); the notice gate; `vrSizingWatchTexture` (H7); shutdown restores the slot only when the write lands.
- `ui_panel_scale.cpp`: the setter and getter hooks at DLL load; the setter's value first; the panel factor takes 1.0 while held; the setter and getter note the render context.
- `device_hook.cpp`: pass-through hooks on SetFullscreenState (slot 10), ResizeTarget (slot 14), ResizeBuffers (VR, slot 13); the window observers (H6); the texture create notes the sizing watch.
- `vr_display_observer.cpp/.h`: the notes, caps, caller labels, and the user32 IAT observers.

## Log lines

- `vr ssaa gate: hold installed at DLL load: loader wrapper 0x2862780 (slot 0x52E8368) and the Supersampling setter, build 332841 checked`
- `vr ssaa gate: hold refused, nothing written: VirtualProtect failed (<n>); the game's Supersampling is not held`
- `vr ssaa gate: holding Supersampling at 1.0 (requested <x>, 3D mode <m>, at startup); loader object 0x..., field 0x...; loader run <n>` (change-only)
- `vr ssaa gate: holding Supersampling at 1.0 (requested <x>, 3D mode <m>, at menu)`
- `vr ssaa gate: holding Supersampling at 1.0 (requested <x>, 3D mode <m>, at mode change); context 0x...` (GAP 2)
- `vr ssaa gate: 3D mode <a> -> <b> (Settings.xml, re-read 1.5 s after a Supersampling call)`
- `vr ssaa gate: 3D mode is now 0; ...` and `vr ssaa gate: released: ...`
- `vr sizing: <render target | depth-stencil | render and depth-stencil> <w>x<h> format <f> bind 0x<b> created <s> s after a <Supersampling setter call | 3D mode change>; callers <up to 6 frames: game RVA 0x... or module+0x...>` (cap 96)
- `vr window: IAT observer on user32 <fn>: patched (observe only) | not imported by the game, or the slot is not ours to take`
- `vr window: SetWindowPos(...)`, `SetWindowLongPtrW(...)`, `ShowWindow(...)`, `MoveWindow(...)`, `AdjustWindowRect(Ex)(...)`, each with arguments, result, a chain of up to 4 callers, the frame (cap 64 each)
- `vr display: SetFullscreenState(...)`, `ResizeTarget ...`, `ResizeBuffers ...` (VR and flat), `game window ...` (step 3; unchanged)
- `vr ssaa gate: display observer installed (SetFullscreenState, ResizeTarget)`

## Verified in the build

(filled in from the build and install output of this commit)
