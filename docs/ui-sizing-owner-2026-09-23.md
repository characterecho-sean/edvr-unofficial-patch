# Where Elite sizes the cockpit interface surfaces

## Status

RESEARCH, static only (2026-09-23): no code, no build, no flight. Ghidra
headless (`-readOnly`) on `analysis\ghidra\EDAnalysis` plus capstone; every
RVA is for EliteDangerous64.exe build 332841 (SHA-256 e6be8bbe...) only.

Established (read in the disassembly; high confidence):
- One formula sizes every Scaleform render-to-texture panel, inlined twice
  (init `FUN_144570500`, recompute `FUN_144571180`):
  `s = (c/d < 16/9) ? c/1920 : d/1080; w = trunc(stageW*s); h = trunc(stageH*s)`.
  stage = the movie's own size; (c, d) = the UI view's width and height.
- (c, d) come from `FUN_142842A70` on both routes: at init through the
  panel's owner (the renderer interface, type id 0x5F02BB4, vtbl +0xA8 ->
  `[renderer+0x11D8]+0x10`), on a view change through `FUN_14288E3A0`'s
  broadcast. `c = W_ui*k, d = H_ui*k, k = tan(0.782)/tan(vFOV/2)`
  (`FUN_1408D25E0`, VR manager vtbl +0x110; vFOV from the OpenVR fov getter;
  k = 1 unless a mode value is 3/4 and the HMD gates pass).
- W_ui (section 5): in the stereo branch it is VR manager vtbl +0x70
  (`FUN_1408D2880`) = trunc(recommended x m): recommended is EDVR's
  GetRecommendedRenderTargetSize answer, m the manager's +0x4E4 (HMD Quality
  by the numbers). The renderer sizes its views from the same record, so W_ui
  is the scene's render width, and `s = W*tan(0.782)/(1920*tan(vFOV/2))`
  = 0.0010346*U: the U rule, derived. Checks: 1.9865U x 1.1174U for a
  1920x1080 stage (census 1.9856 x 1.1167); trim 0.8596 (flight 3: 0.858);
  Quest/Pimax 1.080.
- The same (w, h) create the movie, set its viewport, and size the render
  target and its depth partner ("RenderToTexture"/"...DepthTarget").
- The menu's 16:9 surface (6) is this path with a 1920x1080 stage:
  1566x880 = 1995 x 0.7853; 1254x705 = the trimmed 1597 x the old k;
  1346x757 = 1597 x the new k (0.8433). The vscreen patch cannot raise it.
- Glyphs (7): Scaleform's raster cache rasterises outline glyphs at the
  requested size while they fit MaxSlotHeight (stock 48 px), else vector. A
  bigger panel requests bigger glyphs.

Not established:
- No log carries an RVA for any panel size: the RVA line fires only on a
  resize.
- That EDVR's cockpit sizes and the menu surface are these targets: the
  numbers fit, no stack yet.
- The 2048x2048 A8 atlas's owner. Scaleform's cache defaults to 1024; the
  engine's frFront glyph texture is 1024x1024. So either Frontier's own
  cache params (sharper text reachable) or a static font texture (text only
  magnified).
- Which draw samples the menu render target. A888D510 samples the 2D screen.

Levers (5): a W_ui write is not a UI-only lever. W_ui is the eye render size,
and its record feeds cursor mapping, projection-to-pixel code and aspect math
in 28 functions. The DIVSS pair (3) stays the candidate: only
render-to-texture panels move. Its one side reader is the panel cursor
window `FUN_14453EF80`.

Next flight: section 8. Log 12-frame stacks on pending and resized surfaces,
plus the frame, W, vFOV and k; the atlas's chain and update count.
Confirmation per surface: 0x4551C7D + 0x45517F6. The atlas is Scaleform's
cache if its chain holds 0x30FC1A or 0x312450.

Ruled out: nothing by flight. Inference: vscreen_res.h's 0x288E495 is the
view-mode 5/6 branch of `FUN_14288E3A0`; the panels follow W, so it never
sized them.

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
a Scaleform GFx4 movie that `FUN_1407677C0` creates through the movie def's
vtbl +0xC0. The GFx4 strings, including "Scaleform render to texture" at
0x52E70B8, make that high confidence. One function pair sizes every panel;
panels differ only by stage size, which is data. Panel 1 (358x537) needs a
stage near 511x766, so stages need not be round.

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
vtable (0x5128238) holds `FUN_1408D25E0` at +0x110, with the call's
(this, float*, float*) shape. That function returns k = 1 when the byte at
0x5F2E0FC is set (it has no writer) or when a gate fails: vtbl +0x18,
+0x20, HMD slot 8, or recommended height < 0x632. Otherwise it returns
`k = tanf(0.782)/tanf(vFOV*0.5)` (0x8D269A-0x8D26C9), plus an unused
`kH = tanf(0.694)/tanf(hFOV*0.5)`. The angles come from `FUN_1404E3060`,
which is `jmp [hmd+0xC8]` (the fov getter). A tail jump is invisible to the
culling arc's CALL [+0xC8] scan, which is why it missed this. tanf is
`FUN_1448AE520`. `ui+0x30` is `ui+0x40` times a clamped scale at
render-context +0x3564; `ui+0x40` is the UI screen (section 5).

**Init path.** The panel's owner (panel+0x758 = the UI manager's +0x778,
looked up by type id 0x5F02BB4) is the renderer's interface at
renderer+0x950 (vtable 0x52E97F8; slot 0 `FUN_14283E660` returns that id).
Its vtbl +0xA8 (`FUN_142842F60`) returns `[this+0x888]+0x10` =
`[renderer+0x11D8]+0x10`, so panel init reads the same getter.

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
   the next dirty frame. Change the floats only at configure, never per
   frame, or a viewport and its target disagree until the next change.
2. The factor applies at the next panel init or view-change broadcast.
3. It multiplies every Scaleform render-to-texture panel, menus and station
   screens included. Cap t/q at 4.

**Alternative: the 0.782 constant** (0x51289DC, one reader at 0x8D269A).
`atan(f*tan 0.782)` scales k by f with the FOV term kept, but k also sizes
views that are not render-to-texture, which use (c, d) directly. The dead
override (0x5F2E0FC, value 0x5E74760) drops the FOV term. Neither is
recommended.

## 4. What one flight needs to log

Superseded by section 8.

## 5. W_ui's origin, and whether one write sizes the UI

**Writers** of the UI screen object `[renderer+0x11C8]` (vtable 0x52E8F88).
Its interface at +0x10 (0x52E8F90) returns +0x20/+0x30/+0x40/+0x50 from
slots +0x60/+0x68/+0x70/+0x78.
- `FUN_14284CB70` (view setup) sets +0x20, +0x40 and +0x50 to the display
  component's size: `[renderer+0xDE0]`, type id 0x5F02A78, vtables
  0x4DDC568 / 0x5119180, whose vtbl +0x90 copies its mode at +0x414. It sets
  +0x30 to that size times the scale at render-context +0x3564, and +0x60 to
  the back buffer.
- `FUN_14288E3A0` (configure) starts from +0x50. When the predicates at
  `[renderer+0x1188]+0x18` (vtbl +0x10, then +0x08) hold, it calls
  `FUN_1428587D0` -> `[renderer+0xE58]`, the VR manager (vtable 0x5128238;
  slot 0 returns type id 0x5F02BC4, the id the renderer finds it by):
  - its vtbl +0x70 `FUN_1408D2880` = `trunc(recommended * [mgr+0x4E4])`
    per axis. Recommended comes from HMD slot 24 (`FUN_1404E4270` at x1.0,
    the OpenVR answer); +0x4E4 is set by vtbl +0x100 and read by +0x108;
  - the same size goes to vtbl +0x78 (`FUN_1408D33F0`), which rebuilds the
    manager's output configuration when it changes;
  - then come the clamp `FUN_1428411D0` and the view-mode 5/6 override
    (1920x1080, 0x288E495); the result lands in +0x40, times the scale in
    +0x30;
  - the same configure's per-view loop resizes views 0 and 1 (and their
    bloom chains) to +0x30.

So in VR, W_ui = trunc(recommended x HMD Quality): 3070 x 0.65 -> 1995 and
3032 x 0.65 -> 1970, EDVR's logged internal size. It is not a table and not
the back buffer. The writer of +0x4E4 from the .fxcfg was not traced; the
numbers identify it.

**Readers.** 36 call sites in 28 functions read the record through the
renderer interface (vtbl +0x98, then +0x60/+0x68/+0x70/+0x78). The two that
go through render-context +0x950 are certain; the rest match by call shape.
By use:
- cursor: `FUN_1408F10E0` (mouse to [-1,1] by +0x70); `FUN_14452A5B0`
  (logs "%.5f %.5f", rescales y by UI/display height); `FUN_14453EF80`
  (panel vtbl +0x198, a cursor window inside a panel from panel size / UI
  screen);
- projection to pixels, next to the interface's world-to-clip helpers
  (vtbl +0x08 `FUN_1428924D0`, +0x18): `FUN_141A0E570`, `FUN_141AA0AD0`,
  `FUN_141AA1960`, `FUN_14108AE40`, `FUN_144728C80`, and six functions in
  0x3C82700-0x3CA8BB0;
- aspect and view setup: `FUN_1427E87E0`, `FUN_14281E110`,
  `FUN_1434AE6F0`, and "MainView" `FUN_1434B50E0`. The terrain LOD gate
  `FUN_141277370` reads the display size (+0x60);
- UI: the view getter, and the init size of panels that are not
  render-to-texture (`FUN_144570500`: owner +0x98 -> +0x70).

**Verdict: there is no single UI-only write here.**
- At the source (VR manager +0x70 / +0x4E4), W_ui is HMD Quality, and the
  scene's views move with it.
- At the record (+0x30/+0x40), every configure rewrites it. A changed value
  would pull the cursor mappings, projection-to-pixel code and view sizes
  out of step with the real eye size.
- The flat screens are not sized by W_ui either: the 2D screen is the
  view-mode 5/6 branch (vscreen_res.h).

The DIVSS pair is the smaller, safer patch. It changes s inside the
render-to-texture branch only: the panels' colour and depth targets and the
movie's layout and viewport. It leaves the scene, the eye targets, the
record and its readers, the other views and the 2D screen alone. Its one
known side reader is `FUN_14453EF80`: on a mouse-driven render-to-texture
panel, the cursor window scales with the panel. Whether any cockpit panel
is mouse-driven is not established.

## 6. The main menu's flat screen

The 16:9 menu surface is the render-to-texture formula with a 1920x1080
stage: w = W_ui*k, and h is 1080/1920 of it.
- 1566x880: 1995 x 0.7853 = 1566.6. The logged 880 needs tan(vFOV/2)
  >= 1.2652, against the logged +-1.2648; the 0.03% is unresolved.
- 1254x705: 1597 x 0.7853 = 1254.1. W_ui had the trimmed width; k was still
  untrimmed.
- 1346x757: 1597 x 0.8433 (tan 1.1778) = 1346.7, at 11:50:39, a later
  reconfigure.

So the site is `FUN_144570500`/`FUN_144571180`: not a constant times the
UI screen and not the vscreen branch. What Sean sees as "reverting to lower
res" after the trim is W_ui falling 20% (1995 -> 1597) while k lagged one
reconfigure; the next reconfigure won back about a third. The DIVSS pair
raises it and so would a W_ui write (with section 5's costs). The vscreen
patch does not: that branch sizes the 2D screen, which stayed 3840x2160 in
flight 3 while this surface followed W.

It is not the surface A888D510 samples: EDVR's logs call vs A888D510 the
screen's composite over the 2D screen (114958.log:1585, 3840x2160;
loading-scrim.md, ps 9107E72C sampling 4259x2395). Which draw samples the
menu render target, and whether the menu Sean judges comes from it, is not
established. Section 8 logs its consumer.

## 7. The glyph atlas

Established:
- Scaleform GFx4's raster glyph cache (its "Raster glyph is too big -
  increase GlyphCacheParams.MaxSlotHeight" warning is in the binary). Ctor
  `FUN_14030D490` writes the stock GlyphCacheParams at cache+0x20: 1024x1024,
  1 texture, MaxSlotHeight 48, SlotPadding 2, TexUpd 256x512, MaxRasterScale
  1.0. SetParams is `FUN_14030DD40` (vtable 0x4DECD90 +0x08). Init
  `FUN_14030F9E0` rounds the size to powers of two and makes the textures
  via `FUN_14030CFA0` (Scaleform image format 9, A8), returning to 0x30FC1A
  (init) or 0x312450 (re-create).
- `FUN_14030E520` sizes an outline glyph as (requested size in 1/16 px) /
  nominal size. If its height + 2*padding < MaxSlotHeight it is rasterised
  at that size (`FUN_1403117C0`/`FUN_1403119E0`); otherwise it is refused
  (status 3) and goes to the vector path. A bitmap glyph (glyph+0x68) is
  copied as-is, clamped to MaxSlotHeight (`FUN_140311130`).
- An engine-owned glyph texture, "frFront_GlyphTexture_GPU"
  (`FUN_140752DF0`, CPU twin `FUN_140754E80`), is made square at the size
  `FUN_1401F3C00` passes: 0x400, 1024x1024, engine format 0x44.

Not established: which texture the text draws' 2048x2048 A8_UNORM atlas is
(loading-panel-handoff.md). Neither default is 2048, and no 2048 constant
was found feeding SetParams; it may come from data. The SWF loader also
reads static texture glyphs ("PadPixels = %d, nominal glyph size = %d,
numTexGlyphs = %d", `FUN_140220445`).

What follows depends on which it is:
- If the atlas is the raster cache: a panel made bigger by the DIVSS pair
  requests bigger glyphs. They are rasterised bigger up to MaxSlotHeight
  (48 px stock; the live value comes from the unknown params) and drawn as
  vectors above it. Sharper text is reachable.
- If it is a static font texture: letters are fixed bitmaps, a bigger panel
  only magnifies them, and only the vector UI gains.

How the requested size follows the viewport is Scaleform's design, not
traced here.

## 8. The confirmation instrument

Logging only, in `ui_surfaces.cpp`:
1. Call `captureGameCallStack()` on every create that passes the surface
   shape test, whether pending, learned or made bigger, for colour and depth
   separately. Keep 12 game frames: raise the cut at :488 from 4; the
   128-char buffer holds 12. Store the stack with the candidate (:449) and
   print it on the pending note (:294) and the made-bigger note.
2. Write one line per distinct (WxH, colour or depth) per session, capped at
   32 (the flight has 13 GUI sizes, doubled by depth partners). Each line
   carries:
   - EDVR's present counter and the time;
   - WxH, DXGI format and bind flags;
   - the frame's render width W, its tangents, vFOV and
     k = tan(0.782)/tan(vFOV/2);
   - the implied stage, w*1920/(W*k);
   - the 12 RVAs;
   - a verdict computed in EDVR. `rtt` if both 0x4551C7D and 0x45517F6
     appear, tagged `init` (0x4570902) or `change` (0x456DA45) and `colour`
     (0x28148A9) or `depth` (0x2814686). `glyph-cache` if 0x30FC1A or
     0x312450 appears. Otherwise `other`.
3. Log the same stack and verdict for any A8_UNORM texture of 1024 or more
   a side, plus the count of UpdateSubresource/Map calls on it over 30 s.
4. For the menu's 16:9 surface, log the first draw that binds it as a
   shader resource: vs/ps and target size.

Confirmed: every cockpit panel size and the menu surface read `rtt`. Each
panel keeps one implied stage across the trim, and the 16:9 one reads
1920x1080. The full chain, innermost first: 0x51AF6B / 0x50EC69 / 0x5185B3
/ 0x28148A9 or 0x2814686 / 0x2833883 or 0x28338B0 / 0x4551C7D / 0x45517F6 /
0x4570902 or 0x456DA45.

Refuted, and recorded here as `ruled out`:
- a panel or menu size reading `other`: Scaleform render-to-texture is not
  its owner;
- an implied stage that moves across the trim: k is not the scale;
- an atlas reading `other` that is never updated: it is a static font
  texture, so a bigger panel cannot sharpen text.

No line at all means the code never ran, which is not a refutation.

## Method

`FUN_*` names are Ghidra's for the analysed program. The scratch scripts
were read-only and were not kept. They were: decompile, xref and asm dumps;
scans for float-to-int conversions next to render-target calls, for tan of
half-angle call sites, and for `x = a->vtbl[S1](); x->vtbl[S2]()` chains;
and type-id getters (`mov eax,[id]; mov [rdx],eax; ret`) mapped to their
vtables. Re-running vscreen_res.cpp's scan finds its six sites at
0x283F0F1, 0x288E495, 0x28A82B0, 0x28A883E, 0x28AABCD and 0x28AAC8C. To
reproduce the key reads, decompile 0x4570500, 0x4571180, 0x2842A70,
0x8D25E0, 0x288E3A0, 0x28587D0, 0x8D2880 and 0x30E520.
