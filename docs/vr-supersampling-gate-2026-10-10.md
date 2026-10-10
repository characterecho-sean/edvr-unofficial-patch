# VR Supersampling gate (2026-10-10)

## Status

- **State:** step 1 (instruments only) on branch `claude/vr-ssaa-gate`, cut from v0.19.0 (7960f016). Log-only: no game value changed, no EDVR sizing changed. Not merged to main.
- **Decided goal (maintainer):** while Elite's 3D mode is anything but off, the game sees its Supersampling (SSAAMultiplier) as 1.0, so HMD Quality is the only render-resolution control in VR. Flat (3D off) is untouched.
- **Recommended mechanism (step 2):** the existing setter/getter hook on the game's Supersampling (ui_panel_scale.cpp, the virtual slots 0x52E90B0 / 0x52E90B8, build-gated). The setter thunk passes 1.0 when the 3D mode is not off; the getter returns 1.0; the loaded value is set to 1.0 at startup.
- **Fallback (not chosen):** EDVR's runtime divides its recommended render size by S. This keeps S active inside the game and brings back the 19200-panel and 8192 DLSS ceilings (edvr-dlss-ceiling-8192).
- **Open hypotheses:**
  - H1 mode values: Settings.xml `<StereoscopicMode>` 3 = VR, 5 = HMD Cinema, 0 = off. Only 3 and 5 are observed. 0 is presumed, not verified. Discriminator: the startup line for a flight with 3D off.
  - H2 load order: the game sizes its render targets from Supersampling at startup, so a startup-only override would be too late. Discriminator: the order of the first setter call, first getter read and first Present in the step-1 log.
  - H3 live mode: the in-memory 3D mode field is not located (see the evidence section). Until it is, step 2 can read only the startup file, which the game does not re-read mid-session.
- **Ruled out:** none yet on a flight.
- **Instruments in this step (log only, behind the build gate):**
  - `vr ssaa gate: Settings.xml StereoscopicMode=...; fxcfg ... SSAAMultiplier=... HMDRenderTargetMultiplier=...` once at startup (hookDevice).
  - `vr ssaa gate: first Present ...` once (hookedPresent).
  - `vr ssaa gate: SS setter call N ...`: every call for the first 64, then only on a change of the passed, before or after value, hard cap 512 (ui_panel_scale.cpp setter thunk).
  - `vr ssaa gate: first SS getter read ...` once (ui_panel_scale.cpp getter thunk).
  - `vr ssaa gate: live 3D mode not located; using the startup file value` once at startup.
  - `vr ssaa gate: game build not checked; instruments off` once on any other build.
- **Caveat:** the setter and getter lines exist only while the hook is installed, which needs fix.ui_quality on (the default). With it off, no setter line appears and that absence is not evidence.
- **Next:** one VR flight on Frontier (build 332841, the same stamp as Epic), changing Supersampling and the 3D mode in the in-game menu, then reading the lines above with `tools\edvr_log.py --target frontier --expect-build HEAD`. Record H1 (values 0/3/5), H2 (order) and whether the setter line's passed value tracks the menu.
- **Doc rule:** append dated entries below; keep this block current.

## 2026-10-10 evidence (investigation facts, from the step brief)

Epic and Frontier executables are both build 332841: PE stamp 1788384820, image size 104894464 (checked 2026-10-10 by reading the headers). The build gate passes on both.

- **Settings.xml:** `Options\Graphics\Settings.xml`. `<StereoscopicMode>` is read by the game reader at RVA 0x2855C20 (clamped to [0,6]; the store `mov [rbx],eax` at 0x2855C6D; an allowed-list check restores the old value). Called from RVA 0x2845188 with rcx = rsi+0x20.
- **fxcfg float loader:** RVA 0x2855A50 stores SSAAMultiplier at ctx+0x3564 (clamp 0x2855BBA..0x2855BDF). EDVR's live read uses the same offsets (ui_sizing_math.h: kUiSsOffCur 0x3564, Min 0x3568, Max 0x356C).
- **Sizing:** FUN_14288E3A0 / FUN_14284CB70 write ui+0x30 = trunc(ui+0x40 x S). ui+0x40 = trunc(recommended x HMDQ) from FUN_1408D2880. FUN_1408D25E0 gates on mode 3/4 (from earlier docs, not re-traced in this step). EDVR does not read ui+0x30 or ui+0x40, so the step-1 lines report the sizing record as "not read".
- **Live 3D mode, step 4 (not located):** the panel-sizing gate function FUN_1408D25E0 is RVA 0x8D25E0. Disassembled read-only with capstone (`python -I` over the exe's file bytes; no game memory touched):
  - `cmp byte [RVA 0x5F2E0FC], 0` at 0x8D25F7 is a global flag. Nonzero: the function writes two constant floats to its out pointers and returns (fixed size, no mode check).
  - Zero: it calls the thunk at RVA 0x4E4460 (`mov rcx,[rcx]; jmp [rax+0xC0]`, a virtual call on the object at this+0x58) and compares the dword it returns at [rsp+0x34] with 0x632 (`setb bpl`).
  - Then virtual calls on `this` (vtable +0x18, +0x20) and RVA 0x4E4E00 (`mov rcx,[rcx]; mov rax,[rcx]; jmp [rax+0x40]`, a virtual call on the object at this+0x58). Only when all of them pass and bpl is clear does it compute the panel divisors; otherwise both are 1.0.
  - The mode is therefore read through virtual calls on game objects, not from a static address. RVA 0x4E4E10 reads a block of settings fields (byte at +0x328, dwords at +0x134, +0x1bc, +0x1f4, doubles at +0x1b4, +0x1ec) and calls RVA 0x4E89C0. Whether one of them is the 3D mode is not established.
  - A safe in-memory read needs the object model (vtables of this and this+0x58) or a live trace. Neither was done in this step, so the live read is not located and step 1 logs the startup file value only.
- **Ruled out:** a static-address read of the 3D mode. Reason: the gate reaches it through virtual calls on heap objects.

## Verified in the build

(to be filled after the full build and the flight)
