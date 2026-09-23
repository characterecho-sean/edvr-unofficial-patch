# Where Elite sizes the cockpit interface surfaces

## Status

RESEARCH, static only (2026-09-23): no code, no build, no flight. Ghidra
headless (`-readOnly`) on `analysis\ghidra\EDAnalysis` plus capstone; every
RVA is for EliteDangerous64.exe build 332841 (SHA-256 e6be8bbe...) only.

Established (read in the disassembly; high confidence):
- One formula sizes every Scaleform render-to-texture panel, inlined twice:
  panel init `FUN_144570500` (math 0x45706F3-0x4570795) and the view-change
  recompute `FUN_144571180` (0x4571201-0x457129B):
  `s = (c/d < 16/9) ? c/1920 : d/1080; w = trunc(stageW*s); h = trunc(stageH*s)`
  into panel+0x3B8/+0x3C0. stage = the movie's own size (vtbl +0x30/+0x38 of
  panel+0xF8); (c, d) = panel+0x6D0/+0x6D8, the UI view's width and height.
- (c, d) come from `FUN_142842A70` (UI view vtbl +0x118, reached as
  `[renderer+0x11D8]+0x10`, broadcast to every panel by `FUN_14288E3A0`):
  `c = W_ui*k, d = H_ui*k, k = tan(0.782) / tan(vFOV/2)`.
  k is `FUN_1408D25E0` (VR manager vtbl +0x110; matched by slot, signature
  and the numbers below): 0.782 rad is a constant (0x51289DC, one reader);
  vFOV = atan|up| + atan|down| from the OpenVR fov
  getter `FUN_1404E2F50` (terrain-culling.md), eye 0. k applies only when a
  mode value is 3 or 4, the HMD gates pass and its recommended height is
  >= 1586; otherwise k = 1.
- In VR `s = W_ui*tan(0.782)/(1920*tan(vFOV/2))`. With W_ui = the render
  width this is 0.0010346*U: the ui-layer doc's U rule, now derived rather
  than fitted. Cross-checks: a 1920x1080 stage predicts 1.9865U x 1.1174U
  (census 1.9856 x 1.1167; truncation takes <= 1 px);
  trimmed/untrimmed (1597/1.1778)/(1995/1.2648) = 0.8596 (flight 3: 0.858);
  Quest/Pimax 1.2648/1.1708 = 1.080 (the census's "1.08x wider").
- The same (w, h) create the movie instance (`FUN_1407677C0`), set its
  viewport on the next dirty frame (`FUN_1444F5050` ->
  `FUN_140776EE0(movie, 0, 0, w, h, w, h)`), and size the render target and
  its depth partner from one packed {w, h} (`FUN_1445517A0` ->
  `FUN_144551920` -> `FUN_1428336B0` -> "RenderToTexture" `FUN_1428147C0` +
  "RenderToTextureDepthTarget" `FUN_1428145A0`). Sizing it in the engine
  moves layout, viewport, colour and depth together. The glyph atlas is not
  sized here.

Not established:
- No log carries an RVA for any panel size. The RVA line fires only when a
  surface is made bigger, and no instrumented build has made one. The four
  RVAs in hud-quality-2026-09-23.md's "Log lines" are a format sample:
  0x2A1E8B0 lies inside no function.
- That EDVR's 1346x757 / 358x537 / ... surfaces are these targets: the
  numbers fit, no stack yet.
- W_ui = the render width: W_ui is the UI screen at `[renderer+0x11C8]`
  +0x30/+0x40 (set by `FUN_14284CB70`, `FUN_14288E3A0`); not traced to the
  OpenVR size. The census implies it.
- The init route `[panel+0x758]`->+0xA8->+0x118 is unresolved (change: 2).

Candidate hook (not built): retarget the `DIVSS [1080.0]` / `DIVSS [1920.0]`
operands (two sites: 0x4570703/0x4570710, 0x457120E/0x457121B) to EDVR
floats 1080*q/t and 1920*q/t. Only render-to-texture panels move (section 3).

Next flight: the planned ui_quality flight with the RVA cut raised from 4 to
12 frames and a stack on pending surfaces (section 4). Confirmed if each
cockpit panel's chain holds 0x4551C7D and 0x45517F6.

Ruled out: nothing by flight. Inference: vscreen_res.h's site 0x288E495
(view modes 5/6 force 1920x1080) is in `FUN_14288E3A0`; panels scale with W
at a fixed frustum, so the cockpit is not in that branch.

## 1. What the flight logs carry

`src\d3d11\ui_surfaces.cpp` (main 48dcb9b6): the stack is captured only in
the made-bigger branch (:483) and cut to four game frames (:488). A pending
surface is recorded at :443-460 with its size and basis and no stack, and
the pending note (:294) prints none. The logs agree: no "RVA" anywhere in
the 2026-09-23 gfx logs. 114958 (flight 3): 13 pending notes. 092848 and
093817: "0 candidate ratio(s)". 073409: build 7a47fd3c resized 1346x757
at 07:34:55, before the instrument existed. Distinct RVAs with sizes: none.

## 2. The owner

**The allocation chain** (return addresses, innermost first, as
`captureGameCallStack` would print them): 0x51AF6B (`FUN_14051AC80`, the
f3d texture create: `call [rax+0x28]` = CreateTexture2D; 0x51AFEA on its
retry) / 0x50EC69 (`FUN_14050EA10`, f3dResource_DX11.cpp) / 0x5185B3
(`FUN_140518320`, named create at device+0x148) / 0x28148A9 colour or
0x2814686 depth / 0x2833883 or 0x28338B0 (`FUN_1428336B0`, recreates when
the wanted size at rtt+0x10 differs from the current one at +0xB0) /
0x4551C7D (`FUN_144551920`) / 0x45517F6 (`FUN_1445517A0`) / 0x4570902
(panel init) or 0x456DA45 (`FUN_14456D910`, view change). The creates run on
the caller's thread (`FUN_14050EA10` calls `FUN_14051AC80` directly).

**The render target.** `FUN_144551920(mgr, source)` allocates a 0x1B0
RenderToTexture object, sets `rtt+0x10 = {source->vtbl+8(),
source->vtbl+0x10()}`, colour format enum 0x23, and a depth desc when
`source->vtbl+0x18()` (flag bit 23). The source is panel+0xE8 (vtable
0x55D95C8): +8 returns panel+0x3B8, +0x10 returns panel+0x3C0, +0 returns
the movie (panel+0x3A8), the handle `FUN_1445555A0` renders into the target.

**The size** (0x45706F3, same bytes at 0x4571201):

    divss  xmm0, xmm6                 ; c/d
    comiss xmm0, [0x52F2D84]          ; 16/9
    jb     +0A
    divss  xmm6, [0x4DDE660]          ; s = d/1080
    jmp    +0B
    movaps xmm6, xmm1
    divss  xmm6, [0x4DDE664]          ; s = c/1920
    call   [rax+0x30]                 ; stage width (panel+0xF8)
    mulss  xmm0, xmm6 ; cvttss2si     ; -> panel+0x3B8 (height: +0x38 -> +0x3C0)

The panel class (0x818 bytes, ctor `FUN_1444E20A0`, vtable 0x55D9368) wraps
a movie that `FUN_1407677C0` creates through the movie def's vtbl +0xC0.
The vtable sits beside "<ActionScript Closure>", and the binary carries
Scaleform's HAL strings. That it is Scaleform is circumstantial (medium-high)
and changes nothing below. One function pair sizes every panel; panels
differ only by stage size (data). Panel 1 (358x537) needs a stage near
511x766, so stages need not be round.

**(c, d) and the angle.** `FUN_142842A70` (0x2842A70):

    k = 1.0
    if (mode - 3 < 2) vrMgr->vtbl[0x110](&kH, &k)          ; FUN_1408D25E0
    W = max(ui[+0x30].x, ui[+0x40].x); H = max(.y, .y)      ; ui = [renderer+0x11C8]
    Hc = FUN_1428411D0(ui, W, H, minAspect)   ; H, or W/aspect when clamped
    *x = *y = 0; *c = W*k; *d = Hc*k; *bw = *c; *bh = *d

With Hc = H (W/H < 16/9 in VR) or Hc = W/aspect, s comes out W*k/1920
either way. The exception, unverified: if the back-buffer aspect behind the
clamp (`FUN_1428634E0`) exceeds 16/9, s drops to Hc*k/1080.

The callee is identified by slot, not by tracing view+0x28: the VR manager
vtable (0x5128238, ctor `FUN_1408CE520`) holds `FUN_1408D25E0` at +0x110,
with the call's (this, float*, float*) shape and 1.0 defaults.
`FUN_1408D25E0` (0x8D25E0) returns k = 1 when the byte at 0x5F2E0FC is set
(it has no writer in the binary) or when a gate fails: vtbl +0x18, vtbl
+0x20, HMD slot 8, or recommended height < 0x632. Otherwise
`k = tanf(0.782) / tanf(vFOV*0.5)` (0x8D269A-0x8D26C9) and
`kH = tanf(0.694)/tanf(hFOV*0.5)`, which is unused here. The triple comes
from `FUN_1404E3060` = `jmp [hmd+0xC8]`, the fov getter's slot 25. That tail
jump is why the culling arc's CALL [+0xC8] scan never found this consumer.
tanf is `FUN_1448AE520`. `ui+0x30` is `ui+0x40` times a clamped UI scale at
render-context +0x3564 (setter `FUN_1428767D0`). `ui+0x40` is the UI screen:
forced to 1920x1080 in view modes 5/6 at 0x288E495, otherwise the display
or VR size.

**Change path.** When a UI system is attached, `FUN_14288E3A0` (render
configure) calls the getter and then
`[renderer+0xF60]->vtbl+0x98(x, y, c, d, bw, bh, g)`. In the UI system's
interface (vtable 0x5604AB0) that slot is 0x4749AD0
(`mov rcx,[rcx+0xF8]; jmp FUN_14456DA60`), which calls every panel's
`FUN_14456D910` (panel vtbl +0xA8). That stores (c, d) and recomputes
(`FUN_144571180`). If the panel is registered, it unregisters and
re-registers, which rebuilds the target. The next dirty frame
(`FUN_1444F5050`, 0x44F5141) recomputes again and sets the viewport. Panels
that are not render-to-texture use (c, d) directly as their viewport there.

## 3. One hook point

**Preferred: retarget the divisors.** `.text` holds this shape exactly twice
(0x45706FA, 0x4571205): `0F 2F 05 <->0x52F2D84> 72 0A F3 0F 5E 35 <->0x4DDE660>
EB 0B 0F 28 F1 F3 0F 5E 35 <->0x4DDE664>`. Rewrite the disp32s at 0x4570707,
0x4570714, 0x4571212 and 0x457121F to point at two EDVR floats, 1080*q/t and
1920*q/t, in a page within +-2 GB (allocated near the module, as the
kinematic relays are). That is a four-byte operand swap with no length
change and no control-flow change: the vscreen_res.h risk profile. Every
render-to-texture panel is then made, laid out, viewported and
depth-partnered at s*t/q by the game itself. The scene, the 2D screen (its
own branch) and the fss_res viewport backstop are not involved.

Invariants before writing: the PE timestamp/size of 332841 (1788384820 /
104894464) or, version-free, the shape count == 2; each old disp32 resolves
to 16/9, 1080.0, 1920.0 (read the values); the prologues `40 55 56 57 48 81
EC B0 00 00 00` (0x4570500) and `40 53 48 83 EC 40 4C 8B 81 F8 00 00 00`
(0x4571180). Stand down on any mismatch, write nothing, and log it. Revert
on unload by restoring the saved disp32s. Write under VirtualProtect and
FlushInstructionCache, as `vscreen_res.cpp` writeImm does.

Hazards:
1. `FUN_144571180` runs twice per view change: in `FUN_14456D910`, then on
   the next dirty frame. Change EDVR's floats only at configure, never per
   frame, or one panel's viewport and its target disagree until the next
   change.
2. The factor takes effect at the next panel init or view-change broadcast:
   today's "trip through the main menu" rule.
3. s*t/q multiplies every Scaleform render-to-texture panel, menus and
   station screens included. Cap t/q at 4, as today.

**Alternative: the 0.782 constant** (0x51289DC, `.rdata`, one reader at
0x8D269A). Writing `atan(f*tan 0.782)` there scales k by f and keeps the FOV
term automatic. But k feeds (c, d) for every view, and movies that are not
render-to-texture use (c, d) as their size. Use it only if the flight shows
none of those live in modes 3/4. The dead override (0x5F2E0FC + value
0x5E74760) would replace k outright and drop the FOV term: not recommended.
Scaling the getter's outputs has the same objection as the constant.

## 4. What one flight needs to log

The instrument exists; it needs two changes and no hook:
1. `ui_surfaces.cpp:488`: cut at 12 game frames, not 4 (the 128-char buffer
   holds 12). The owner frames are the fifth to eighth.
2. Capture the stack in the pending branch (:443-460) and print it on the
   pending note (:294). Then a surface the U rule misses still names its
   chain.

What to read: each cockpit panel size should show 0x5185B3 / 0x28148A9 /
0x2833883 / 0x4551C7D / 0x45517F6, and its depth partner 0x2814686 /
0x28338B0. 0x4570902 means panel init; 0x456DA45 means the view-change
path. Any other chain goes in this doc as `ruled out: the Scaleform
render-to-texture path as the owner`. No chain line at all means the code
never ran, which reads differently from a mismatch.

Optional, only if the hook is next: a read-only relay at `FUN_1408D25E0`
(prologue `40 53 56 57 48 83 EC 60`) logging k once per change. Expect 0.8433
at the 7/5/2 trim (tan 1.1778), 0.7853 untrimmed (1.2648), 0.848 on the
Quest 3.

## Method

`FUN_*` names are Ghidra's for the analysed program. The scratch scripts
were read-only and are not kept: decompile, xref and asm dumps, a scan for
float-to-int conversions next to render-target calls, and a scan for tan of
half-angle call sites. Re-running vscreen_res.cpp's scan finds its six sites
at 0x283F0F1, 0x288E495, 0x28A82B0, 0x28A883E, 0x28AABCD and 0x28AAC8C. To
reproduce the key reads, decompile 0x4570500, 0x4571180, 0x2842A70, 0x8D25E0
and 0x288E3A0.
