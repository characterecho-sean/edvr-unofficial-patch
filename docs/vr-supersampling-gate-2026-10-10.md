# VR Supersampling gate (2026-10-10)

## Status

- **State:** the Supersampling hold and the game's window request rewrite are MERGED to main (4f626825) and flown OK on Frontier. The sections below are the record; this block says what is built now.
- **Review fix, branch `claude/review-0191-window` (not merged, not installed):** the window request rewrite and its install are VR profile only. A flat request is forwarded byte for byte, and the saved mode is not read for it. The install publishes the original-function forwards through CodeHook's Prepare callback. Rig `tools\vr_ssaa_hold_test` (in build.bat's gate) covers the flat, VR and install-ordering cases.
- **Goal:** while Settings.xml's StereoscopicMode is not 0, hold the game's Supersampling at 1.0 so HMD Image Quality is the only VR render control. Flat is never held and its requests are never touched. No config key. The user's settings files are never written.
- **Built (VR profile only; `vrSsaaGateStartup` installs the window hooks only when `runtimeVrProfile()`):**
  - Startup loader: `afterLoader` writes 1.0 into the fxcfg loader's Supersampling temporary when the mode is not 0. Whether that reaches the sizing is H8, open.
  - Settings copy hold, W at 0x281C0D0 (`vrSsaaHoldAfterCopy`, entered through a near relay): after the game copies its settings into the render context, the field is set to 1.0 while VR is live.
  - Setter (`vrSsaaHoldSetterValue`, `ui_panel_scale.cpp`): passes 1.0 while VR is live, and remembers the game's own value for release.
  - Live VR signal (`vrSsaaHoldFrameBoundary`, once per Present): the runtime's render sizing record (`edvrQueryNativeRenderSizing`, valid while an OpenXR session has published its eye sizes). Live sets the field to 1.0; not live restores the game's value.
  - Window request rewrite (`requestReplacement`, the mode request at 0x7E9D50): while VR is not live, a non-windowed request is remembered whole (request +0x08 to +0x27). A windowed request (kind 0) gets the remembered block restored before the original runs, when VR is live, or before the live session exists if the saved mode is not 0. This keeps the user's display mode when 3D is on (the maintainer's decision, H6).
  - Not held: the DisplayConfig save (0x7F9F00). Its object's field layout is not proven.
- **Settled by flights:** the startup hold, the menu hold with its toast, and the live decision. The game saves the menu value to the .fxcfg, not the held field (H5). The 3D-on path requests kind 0 at 1280x768 and the request is saved (H6 answered). Mode 4 shows in re-reads at the windowed switch.
- **Open:**
  - Gap 1 (falsified): the one field does not cover every consumer. New depth targets were sized from the menu value (2227 = trunc(2620 x 0.85)) while the field read 1.0. H7 names the sizing caller and is open.
  - H8: does the startup write reach the render context's field? The first setter call's `before` value decides it (0.85 means no, 1.0 means yes).
  - H9: what mode 4 is.
- **Ruled out (2026-10-12):** the corruption's PresentInterval 2 pacing VR, because 22 Frontier VR sessions (10-11 16:00 to 10-12 13:58) and the two Steam flights after it logged `present_sync_nonzero=0` over about 600,000 Presents: with VSync false Elite presents with sync interval 0 in VR. VSync true is unmeasured. Sean: no enforcement needed. Entry at the end.
- **Ruled out:** H4, the loader object and the render context are different allocations, so the startup write does not touch the field the setter reads. A static-address read of the 3D mode: the mode sits behind virtual calls on heap objects.
- **Retired:** the write watchpoint and the settings and sizing watches of the earlier steps. No watch code is in `vr_ssaa_*.cpp` or `vr_window_trampolines.cpp`; do not reintroduce them. `vr_display_observer` still logs the game window's style and size, read only.
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

## Sizing chain (read-only disassembly, 2026-10-10; PROVEN from the bytes)

- E = 0x6C7C40 copies its request descriptor from [r10+0x00..0x7C] (r10 = [rcx+0x10]) and calls G4 = 0x6C7A90 at 0x6C7D2D (return 0x6C7D32). E has about 25 callers, at 0x28AA8xx..0x28AAD36, 0x2875B79..0x2875BB1, 0x5A7140 and 0x36BBDAB.
- G4 = 0x6C7A90 calls G3 = 0x518421 (return 0x6C7B50).
- G3 = 0x518421 zeroes [rbx+0x158] (qword, 0x51858E) and calls G2 = 0x50EA10 at 0x5185AE (return 0x5185B3).
- G2 = 0x50EA10 copies the dwords [rbx+0x158], [+0x15C], [+0x160], [+0x164] of its argument object O (0x50EA96..0x50EABC) and calls G1 at 0x50EC69.
- G1 = 0x51AC80..0x51B1D8 creates the render targets. Its CreateTexture2D calls are through [rax+0x28] at 0x51AF68, 0x51AFE7 and 0x51B0E6. The descriptor dimensions come from r9 = O: [r9+0x158] (0x51AEF1), [+0x15C] (0x51AF05), [+0x160] (0x51AEFB), [+0x164] (0x51AF0F), and [+0xD8] (0x51AF19), [+0xDC] (0x51AF23).
- No instruction in E, G4, G3, G2 or G1 reads render context +0x3564. The disp32 reads of 0x3564 are at 0x284CC69, 0x2887517 and 0x288E4B8, in other functions.
- The setter (0x28767D0, store at 0x28767EF) is the only disp32 store to +0x3564 in the image. It has no direct callers: only the vtable slot 0x52E90B0 refers to it. So the second writer of 0.85 is a pointer-based or memcpy write, not found.
- Not located: the writer of O's dwords +0x158..0x164 (the 2227 and 3264 inputs), and where 0.85 enters trunc(2620 x 0.85).

## Settings watch (H8 test, read-only)

- `vr ssaa gate: watch: settings field <old|first read> -> <new> (loader object+0x13C), ctx+0x3564 <v|unknown>, frame N, <ms|no setter call yet> after the last setter call`. Logged on any change of either field, once per Present, capped at 128.
- Reading the result: the settings field becoming 0.85 right after a menu apply supports the hypothesis that the apply writes the settings object, which the game then copies into ctx+0x3564 and O. If the settings field stays at its held 1.0 across the apply while ctx+0x3564 becomes 0.85, the second writer is outside the settings object.
- If the settings field does NOT change at the apply: the next instrument is the observe-only trampoline at G2's entry (0x50EA10), logging O's address and the four dwords on each call, to read the 2227 and 3264 values and O's identity at runtime.
- If the settings field does become 0.85: the fix candidate is to keep loader object+0x13C at 1.0 while held (re-write on change at the frame boundary and inside the setter hook). Not implemented.

## Window-mode path (H6, read-only disassembly, build 332841; the trampolines are observe only)

- 0x5589B0 is the window-apply wrapper (rcx = window object, rdx = state). It calls SetWindowLongPtrA(GWL_STYLE) at 0x558A13, AdjustWindowRectEx at 0x558A88 and SetWindowPos at 0x558AF9 (flags 0x44). State: [state+0x20] = kind (0 windowed, using the client size in [state+0x18]/[state+0x1C]; 1 or 2 use the monitor rect from table global 0x5F1B3F8, entry [state+0x28], with a popup style).
- 0x7E9D50 builds the state and calls the wrapper at 0x7EA046 (rcx = [rbx+0x5F8]). Its request is rdx (mode index [rdx], kind [rdx+0x20], mode table [rbx+0x498]). It contains an override: if kind == 1 and the window is not foreground or focused (test function 0x54B7D0: GetForegroundWindow or GetFocus == [obj+0x50], gated by the flag [obj+0x501]), the kind becomes 0 (windowed).
- The chain: 0x5D6960, then a vtable call at 0x5D6A7E into 0x7FD990 (runs when [rbx+0x500] is pending), which calls 0x7E9D50 with rdx = &[rbx+0x4D0].
- MAINTAINER DECISION (H6): when 3D turns on, keep the user's display mode (borderless 3840x2160 from DisplaySettings.xml), not windowed 1280x768, accepting the unmeasured 4K desktop swapchain cost in VR. No behaviour change in this build. The fix follows the window-apply observer flight, which shows which path produces kind 0.
- H6 hypothesis, OPEN: the 1280x768 windowed switch comes from the kind-1-to-0 override when the game window is not foreground at the request. Test: the `vr window: mode request` and `vr window: apply` lines (kind, foreground, StereoscopicMode) at the 0 -> 4 switch.
- The trampolines (vr_window_trampolines.cpp, CodeHook from src/common/code_hook.h) install after the build gate. Each target's first 11 bytes are pinned to build 332841, and CodeHook refuses a prologue it cannot relocate.
- Lines (capped at 64 each): `vr window: mode request (0x7E9D50): window <p>, mode index <n|unreadable>, kind <k|unreadable>; StereoscopicMode <m|unknown>; game window foreground <yes|no|no game window yet>; frame N`; `vr window: apply (0x5589B0): window <p>, kind <k>, client <w> x <h>, monitor entry <0x...>; StereoscopicMode <m>; game window foreground <...>; frame N`; and the startup line `vr window: trampolines: 0x7E9D50 (mode request) installed|not installed; 0x5589B0 (window apply) installed|not installed; observe only`, with a refusal line before it when CodeHook refuses. The mode table's entry layout is not verified, so the mode's w/h are not logged.

## Flight 58a54aa5 and this build (defects and the write watch)

- DEFECT 1, fixed: the trampolines' replacements were more than 2 GB from the targets (0x7E9D50 and 0x5589B0), so CodeHook's five-byte jump could not reach them. The replacement is now a relay within 2 GB of each target (`jmp qword ptr [rip+0]` plus the replacement's address, allocated the way kinematic_eval_hook.cpp allocates its relays).
- DEFECT 2, fixed: the settings watch read the loader's object, which is a stack temporary (H8). Removed. The watch reads ctx+0x3564 only, and logs a change of its bits (a failed read is "unknown").
- H7 TEST, new (vr_context_watch.cpp, log only): armed once, from the first setter or getter that names the render context, on ctx+0x3564 (DR0, DR7 write, 4 bytes), set on every other thread of the process by a worker thread (SuspendThread, GetThreadContext and SetThreadContext with CONTEXT_DEBUG_REGISTERS, ResumeThread). A vectored handler catches the single step (DR6 bit 0), logs, clears DR6 and continues. Other single steps are passed on. After 16 writes the render thread disarms it. Shutdown disarms it. Threads created later have no debug register (stated in the arming line).
- Writer RIP: the instruction after the write, as a game RVA (or module+offset). Labels: the setter's own store (RVA 0x28767D0..0x2876800), game code, or EDVR or another module (our own writes are labelled that way). Callers are a stack scan of the game image's return addresses (a heuristic, not an unwind).
- Lines (cap as stated): see below.
- The flight: VR with 3D on; change Supersampling to 0.85 and apply; then 3D off and back on. The watch shows the writer of ctx+0x3564 at the apply, and the window trampolines show the mode request and apply.

Lines:
- `vr ssaa gate: write watch armed on ctx+0x3564 0x<addr>: <n> thread(s) set, <f> could not be stopped or set; 16 writes at most, then disarmed; threads created after this have no debug register`
- `vr ssaa gate: write watch not armed on ctx+0x3564 0x<addr>: no thread took a debug register (<f> could not be stopped or set)` (refusal)
- `vr ssaa gate: write watch not armed: AddVectoredExceptionHandler failed (<e>)` / `...: CreateThread failed (<e>)` / `...: the field 0x<addr> is not a 4-byte aligned address`
- `vr ssaa gate: write watch hit <n>: writer <game RVA 0x... | module+0x...> (the instruction after the write); value <v>; thread <tid>; frame <N>; label <the setter's own store | game code | EDVR or another module>; callers (stack scan) <chain | none in the game image>`
- `vr ssaa gate: write watch disarmed (16 writes logged, the cap | shutdown): <n> write(s) seen; <k> thread(s) cleared, <f> could not be`
- `vr ssaa gate: watch: ctx+0x3564 <first read | old> -> <new>, frame <N>, <ms | no setter call yet> after the last setter call` (change only, cap 128)

## The menu-apply writer (read-only disassembly after flight b677656c; PROVEN unless marked)

- Flight: the write watch's one hit, value 0.8500 at RIP 0x281C391 on thread 17964, which is the render thread (the log's "Present is running on thread 17964"). The size chain made the 1713x1656 set 2 ms later.
- W = 0x281C0D0..0x281C3EB is a block copy. It takes S = [arg+0x1180] (0x281C0F8) and copies 0x80-byte chunks from S into the render context's block at r8+0x3428 (r8 = [rbp+0x1180] = ctx). The store at 0x281C38D (movups [rax+0x30]) lands on ctx+0x3564; its load is movups from [rcx+0x30] at 0x281C381. The source offset 0x13C is INFERRED from the identity of the strides, not verified by the loop count.
- C2 = 0x284CB70 is the size-record writer (the FUN_14284CB70 of the notes). It calls W at 0x284D0E2, then reads ctx+0x3564 at 0x284CC67 (movss xmm0, [[arg+0x1180]+0x3564]) and computes trunc(size x scale) into its record (0x284CCC9..0x284CCE7). So the copy and the sizing run in sequence in C2.
- C2 is called through a vtable slot (.rdata 0x52E94E8). C1 = 0x2815710 calls W directly at 0x2815722 and is called directly from 0x284E931 (inside 0x284E218) and 0x288727D.
- W does not write the size record O+0x158..0x164 itself; C2 does not write O either (its stores are to its own record at rcx+0x20.., r8+0x30..0x58).
- Hold options. A: pre-write S+0x13C = 1.0 at W's entry while held. Risk: S may be the object the fxcfg save and the menu read, which would save 1.0 (not verified; the save path is not located). B (recommended): a relay on W's entry that calls the original and then writes ctx+0x3564 = 1.0 while held. C2 reads the field after W returns, so the sizes use 1.0; S is untouched, so the menu and the fxcfg are unchanged. Risks: other readers of S or of ctx's shader globals (gSSAAMultiplier) are not covered; the write is on the render thread, after the copy and before C2's read; all three callers pass through the one relay.
- Next instrument: the watch's hit after B would be labelled EDVR; the size lines should show 2620 x 1.0 after the apply. Before choosing B over A, locate the fxcfg writer and check whether it reads S+0x13C.

## Option B (implemented): hold after the settings copy

- Correction to the findings above: W's DESTINATION is ctx = [arg+0x1180] (mov rbp,rcx at 0x281C0F1; mov r8,[rbp+0x1180] at 0x281C247; the copy lands at r8+0x3428). The SOURCE block is not resolved; it is not ctx, because the field holds 1.0 before the copy.
- W = 0x281C0D0 copies in 0x80-byte chunks to ctx+0x3428. C2 = 0x284CB70 (called through the vtable slot 0x52E94E8) calls W at 0x284D0E2, then reads ctx+0x3564 and writes trunc(size x scale). C1 = 0x2815710 calls W at 0x2815722. The apply runs on the render thread (thread 17964).
- Option B: a near-relay CodeHook on W's entry (prologue pinned: 4C 8B DC 53 48 81 EC D0 04 00 00). It forwards all eight argument slots, reads ctx = [arg+0x1180] before the call, calls the original, then, while held (VR profile, known mode, mode != 0), writes 1.0 to ctx+0x3564. C2 reads that field after W returns, so the sizes use 1.0. When not held, nothing is written beyond the original.
- Why the source stays untouched: the menu display and the .fxcfg save keep the user's value. Option B changes only the render context's copy. CAVEAT: this is an assumption until the source and the save path are located; the flight checks the .fxcfg after a held apply.
- Log, once per change of the copied value while held: `vr ssaa gate: holding Supersampling at 1.0 after the settings copy (W 0x281C0D0): copied <x>, held 1.0, 3D mode <m>`. A failed write says `... after the settings copy failed: the field at 0x... could not be written`.
- The write watch stays armed for this flight: expect the game's copy hit (RIP 0x281C391, label game code) followed by EDVR's restore hit (label EDVR or another module).
- Window trampolines stay observe only.
- Flight: VR, 3D on; Supersampling 0.85, apply. The resolution must not drop: the size lines should show the full eye size. Then 3D off and back on to HMD (the window data and the mode re-hold).

## Flight 8a591fba and the live decision (this commit)

- LAG (fixed): the Settings.xml re-read 1.5 s after a setter call read the file before the game rewrote it. At 3D on (12:30:21) the gate logged "3D mode 4 -> 0"; the held state followed the stale value. Removed: Settings.xml is read only at startup (the loader's hold).
- LIVE SIGNAL (new): the runtime's published render sizing. `edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1, ...)` with `valid == 1` (src\common\native_render_settings.h:76; the struct at :54). The runtime sets it on session start (src\openxr\native_runtime_host.h:2811, `publishRenderSizing(true)`) and clears it at teardown (native_runtime_host.h:2523, `publishRenderSizing(false)`). The hold reads it once a frame (vrSsaaHoldFrameBoundary).
- TRANSITIONS: VR live -> the field is set to 1.0 and the notice/state follow; VR not live -> the game's own remembered value (the last setter value or the copied value) is restored, so flat uses the user's Supersampling and panel factor (chooseSupersampling, which reads vrSsaaHoldActive) follows the same decision.
- Log lines: `vr ssaa gate: VR active -> holding at 1.0 (game's <x>)`; `vr ssaa gate: VR active -> not held yet (game's <x>; no render context or write failed)`; `vr ssaa gate: VR inactive -> released: the game's <x> restored`; `vr ssaa gate: VR inactive -> released; the game's value could not be restored (...)`.
- WINDOW REWRITE (the maintainer's decision: keep the user's display mode in VR). The mode request is 0x7E9D50 (the relay in vr_window_trampolines.cpp). Disassembly of 0x7E9D50 (read only): the request is rdx on entry (mov rsi,rdx at 0x7E9D81); kind is `dword [rsi+0x20]` (cmp at 0x7E9E3F, and the kind-1 focus override); the mode-table index is `qword [rsi+0x18]` (used as an index into the table at [rdx+r8*8+0x18] and [rdx+r9*8+0x28]); [rsi+0x8] (r8) is the table selector (inferred); [rsi+0x24] and [rsi+0x25] are byte flags. `[rsi]` is a locked compare-exchange target (0x7E9D29: `lock cmpxchg [rsi], rdi`), so it is not a mode field and is never written.
- What the rewrite changes: `[rsi+0x20]` (kind) and `[rsi+0x18]` (mode index), from the last non-windowed request accepted while VR was inactive. Not changed: [rsi+0x8] and the flags (unverified). Whether rewriting the request before the original call is enough for the apply (0x5589B0) is INFERRED: the apply takes the state the request builds in 0x7E9D50; it is not verified in this step.
- Trigger: a windowed request (kind 0) while VR is live, or while Settings.xml's StereoscopicMode is nonzero at that request (the file is read only for kind-0 requests, because the live session may not be up at the switch). While VR is inactive nothing is rewritten.
- Nothing remembered (the startup request was windowed, for example): the request is left as the game made it, and one line says so (DisplaySettings size). DisplaySettings gives the size, not a mode index, so the index cannot be mapped (the table layout is not verified).
- Log: `vr window: kept the display mode at 3D on: requested kind 0 <w>x<h> (windowed), applied kind <k> <w>x<h> (remembered; mode index <i>; DisplaySettings size); frame <N>` (cap 64). The DisplaySettings size is the size the user set, not a measured resolve.
- Startup line: `vr window: trampolines: 0x7E9D50 (mode request, window rewrite) installed|not installed; 0x281C0D0 (settings copy, hold) installed|not installed`.
- REMOVED (debug-only instruments): the hardware write watch (vr_context_watch.cpp/.h, deleted); the sizing texture log (vrSizingWatchTexture and its call in hookedCreateTexture2D); the settings watch (vrSsaaHoldWatchSettings); the IAT window observers (vr_display_observer: SetWindowPos, SetWindowLongPtrW, ShowWindow, MoveWindow, AdjustWindowRect(Ex)); the display-call observers (SetFullscreenState and ResizeTarget hooks, the VR ResizeBuffers observer, and the note added to the flat ResizeBuffers hook); the apply observer (0x5589B0). Kept: the startup hold, the setter hold, the W relay (now with the live decision), the step-1 setter/getter instruments, the window-state line (`vr display: game window ...`, cheap, change-only), and the window rewrite.

## The resolution bug (flight 2f5e6817, 13:24-13:25): what the request carries, what the save writes

- Symptom: after 3D on, the window stayed borderless, but the saved resolution became 1280x768 (the next flat session came up borderless at a low resolution). The rewrite restored kind and index only.
- The request (0x7E9D50, rdx): the fields it reads are [+0x08] (a table selector), [+0x10], [+0x18] (a table index), [+0x20] (kind, a dword), and [+0x24], [+0x25] (flags). [+0x00] is a locked compare-exchange target and is not a mode field. The table lookups in that function also use object fields [rbx+0x4A8], [rbx+0x4B0], [rbx+0x4B8] (the lookup chain at 0x7E9EE0..0x7E9F17 yields a packed width and height and their ratio). So the request names the mode together with the object's selection state, and the object fields are NOT restored by this change.
- The saved DisplaySettings.xml comes from the DisplayConfig writer 0x7F9F00, which takes a config object (rdi) and writes its fields by name: ScreenWidth [+0x08] (the name is passed right after the load at 0x7F9FF3..0x7F9FF6), ScreenHeight [+0x0C], FullScreen [+0x10], PresentInterval [+0x14], Adapter [+0x18], Monitor [+0x1C], DX11_RefreshRateNumerator [+0x20], DX11_RefreshRateDenominator [+0x24], LimitFrameRate [+0x28]. Its callers are at 0x7F8E78 and 0x7FA7C6 (not traced further). The object is the game's own state, which the windowed apply sets to the 1280x768 client size.
- 0x7FD990 (the request's caller, the pending path) copies the request block [rbx+0x4D0..] to [rbx+0x4A0..0x4C0] after success (read; not traced further).
- FIX 1 (request): while VR is inactive the whole remembered block (+0x08..+0x27, four qwords) and kind are kept; at the 3D-on rewrite all four qwords are written back (the kind and the index included). The log gives the before and after of each qword and the DisplaySettings size.
- FIX 2 (save): the DisplayConfig writer is held while VR is live: ScreenWidth, ScreenHeight (the user's size, from DisplaySettings.xml, refreshed unless the file says 1280x768) and FullScreen (the remembered kind) are written into the object before the original save, and the game's values are put back after it. Log: `vr window: 3D on: the display config save was held to the user's size: ScreenWidth <w> ScreenHeight <h> FullScreen <k> (the game's object had <gw>x<gh> FullScreen <gf>; put back after the save); frame <N>`.
- FIX 3 (fallback): DisplaySettings.xml's size is kept only when it is not the 1280x768 windowed signature. A size alone does not name a mode (the table lookup is not reproduced), so with nothing remembered the request is left as the game made it.
- NOT VERIFIED: whether restoring the request block alone selects the remembered mode in the apply (the table depends on the object fields above); the writer hold covers the saved file only. The flight decides: DisplaySettings.xml after 3D on and off, and the flat start.
- The wording of the rewrite line is corrected: the request block was set to the remembered one; the size in that line is DisplaySettings', not the applied state's.

## The save corruption (flight 55c0fe3d, 13:58; the save hold is OFF from this commit)

- SYMPTOM: DisplaySettings.xml diff against the pre-flight backup: VSync false -> true, FullScreen 2 -> 3840, PresentInterval 1 -> 2. ScreenWidth 3840 and ScreenHeight 2160 unchanged. The window stayed 4K borderless, so the request-block restore worked.
- CAUSE: the hold wrote 3840 to object +0x08, 2160 to +0x0C and 2 to +0x10, and took them to be ScreenWidth, ScreenHeight and FullScreen at +0x00..? My field pairing was misaligned: I named +0x08 ScreenWidth and +0x0C ScreenHeight, and both pairings came from the name-lea order. The flight shows +0x08 is FullScreen, +0x0C is VSync, +0x10 is PresentInterval.
- ORDERED LAYOUT OF THE WRITER (0x7F9F00, read-only; every name and its field access in order): ScreenWidth (value from [rdi+0x08] at 0x7F9FF3, name at 0x7F9FF6; the pairing is contested by the flight), then `lea r8,[rdi+0x04]` -> ScreenHeight (name at 0x7FA016), `lea r8,[rdi+0x0C]` -> VSync (name at 0x7FA029), FullScreen (name at 0x7FA040; its value source is NOT identified by this listing), `lea r8,[rdi+0x10]` -> PresentInterval (name at 0x7FA053), then Adapter +0x14, Monitor +0x18, DX11_RefreshRateNumerator +0x1C, DX11_RefreshRateDenominator +0x20, LimitFrameRate +0x24, MaxFramesPerSecond +0x28. The writer's name order matches the saved XML order (ScreenWidth, ScreenHeight, VSync, FullScreen, PresentInterval, ...).
- CONTRADICTION: the 55c0fe3d log read the object as +0x0C = 1064042496 (a float's bits, not a VSync bool) and +0x08 = 2 (the saved FullScreen). So the layout of the object the save reads is not what the listing gives for VSync, and the value for FullScreen is at +0x08 by the flight, not by the listing. NOT PROVEN.
- GUARD: src\d3d11\vr_display_config_guard.h (pure): a side must be 640..16384, FullScreen 0..2, VSync 0..1, PresentInterval 0..4. Rig cases are in tools\vr_ssaa_hold_test (they refuse the values that corrupted the save).
- ACTION: the 0x7F9F00 hold is removed from the trampolines (no install, no write). The request-block restore stays. The corrupted DisplaySettings.xml is NOT touched by EDVR; the maintainer fixes VSync and FullScreen in-game before the next flight.
- NEXT (to re-enable the save hold): prove the object's field offsets for ScreenWidth, ScreenHeight and FullScreen (trace the value source of each name's call in 0x7F9F00, and the object's writers at its callers 0x7F8E78 and 0x7FA7C6), and write only those three, guarded by displaycfg::plausibleObject.

## PresentInterval 2 left by the corruption (2026-10-12)

- The shared DisplaySettings.xml still reads `<VSync>false</VSync>` and `<PresentInterval>2</PresentInterval>` (written 2026-10-12 13:31); FullScreen was fixed in-game, PresentInterval was not.
- Question: does interval 2 make Elite's monitor Present wait for every second vertical blank in VR, pacing the headset by the monitor?
- Evidence: the runtime's `native_post_submit_window` lines count Presents with a nonzero sync interval (`present_sync_nonzero`, from the Present trace in `hookedPresent`, the game's own argument). Frontier, 22 sessions from 10-11 16:00 to 10-12 13:58: 0 of about 590,000. Steam, 16:38 and 16:58 on 10-10 (after the 13:58 corruption): 0.
- ruled out: PresentInterval 2 pacing VR with VSync false, because Elite passed sync interval 0 on every timed Present. Sean (2026-10-12): Elite already presents unsynced in VR, so EDVR does not enforce it. Unmeasured: VSync true; a report of VR paced by the monitor would show as a nonzero `present_sync_nonzero` in that user's openxr log.

## Verified in the build

(filled in from the build and install output of this commit)
