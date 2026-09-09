# Adversarial review: fix.ui_depth (2026-09-06)

Branch `claude/ui-text-aliasing-dlss-dd8735`, commits `2e201e8`, `aa353b5`,
`7e24c22` (the fix), `85a98bc`, `c8546cd` (the census), `1eecffb` (the reader).
Reviewed at HEAD `7e24c22`, which is one commit past the build flight 2 flew
(`aa353b5`).

Everything below is marked CONFIRMED (mechanism traced in code, with inputs
that reach it) or PLAUSIBLE (a real mechanism I could not fully close). Log
evidence is from
`edvr_gfx_20260906_105837.log` (five censuses; game build 332841, Pimax
2818x2784) and was re-derived independently of `tools/crisp_ui_gates.py`.

**Verdict up front: no crash or hang found. The mechanism is sound where it
was measured, and the cockpit evidence is genuinely good. But the feature as
built is inert in two of the four contexts it advertises, its classifier is
partial by construction, and its cost is paid by every draw in the frame. Do
not merge default ON without the temporal-pass gate. Merge default OFF now, or
default ON after fixes 1, 2, 8 and 9.**

---

## What holds (checked; do not re-check)

- **The build is clean at HEAD.** `build.bat` run 2026-09-06 11:47: exit 0, 0
  `error C`/`error LNK`/`warning C` lines, fresh `build\d3d11.dll`.
- **The contract, schema and tool self-tests pass.**
  `tools/check_config_contract.py`: 188 keys read, 188 documented.
  `tools/gen_settings_schema.py --root . --check`: ok, 28 exposed.
  `tools/diff_draw_census.py --self-test`: ok, including the new `vf=` regex
  (`(?: res=\S+)?(?: vf=\d+)?$` -- `\S+` cannot cross the space, so the
  optional groups do not fight).
- **The RAII scope brackets every return path.** `vscreen.cpp:2700-2711` is
  constructed before the first `return` in `forwardWithVerdict` and destroyed
  after the last statement, so `kSkip`, `kQuadSkip`, `kLoaderPanel` and the
  curve substitution all pair Begin with End.
- **No recursion and no nesting of the scope.** Every path that re-issues a
  draw goes through the saved `real*` entry points, not the hooked thunks:
  `kQuadSkip` (`vscreen.cpp:2747`, `:2799`, `:2810`), `loaderPanelSubstitute`
  and `panelCurveSubstitute` (both handed
  `g_state->realDrawIndexedInstanced`), and `splashDimBegin`'s second
  `draw()` (`:2856`), whose lambda body is `g_state->realDraw*`. So
  `uiDepthBegin` cannot be entered twice for one draw.
- **Nesting against the other OM-touching pairs is correct.** `fssProbeBegin`
  (`fss_probe.cpp:274`), `resolveProbeBegin` (`resolve_probe.cpp:346`) and
  `stencilProbeBegin` (`stencil_probe.cpp:160`) all save with
  `OMGetDepthStencilState` and restore at their End, which runs strictly
  inside the ui-depth scope. They save and restore our twin; we then restore
  the game's. Correct in both directions.
- **`OMSetDepthStencilState` is not a hooked vtable slot** (only
  `kSlotOMSetRenderTargets = 33`, `vscreen.cpp:121`), so the swap does not
  perturb the binding shadow.
- **The AddRef on the game's state does make key reuse impossible.**
  `ui_depth.cpp:214` holds a reference for the life of the table, and D3D11
  de-duplicates state objects, so an identical desc maps to the same live
  pointer. The cache cannot be fooled by a recycled address.
- **No reference leak on the ordinary paths.** `writingTwin` releases `ours`
  on the table-full path (`:204`), returns null without creating on the
  no-device and create-failed paths (`:187`, `:190-199`), and the caller
  releases the `OMGet` reference on every early return (`:360`).
  `releaseStates()` (`:242-249`) releases both pointers.
- **The eye depth is cleared every frame, before the UI.** Census 4:
  `DCL 0 #27 D dsv=@75 f=3 z=0.000` and `#28 dsv=@83` at q=698/703, with the
  UI at q=1741 onward, in all three frames. `f=3` is DEPTH|STENCIL, so
  neither the depth nor the stencil the UI writes can leak into the next
  frame.
- **G10 holds where it was measured, verified independently.** In the menu
  census and in both resolvable cockpit censuses, after the last classified
  draw the *only* thing that binds or samples that depth resource is EDVR's
  own motion-vector dispatch `6D94E9C00DCE909F`. Census 4 frame 0: 79 events
  after the last UI draw, 2 hits, both that dispatch.
- **No false positives in the censused contexts.** Deriving the learned set
  from the log and applying rule (2) by resource identity: census 1 classifies
  only `A888D51024D9798E` (6 draws / 3 frames), censuses 3 and 4 classify only
  `81216C77F90DEDD6` (72 and 66 / 3 frames). Nothing lands in the RGB10A2
  G-buffer target. The target indicator `5DA53D8B0133341E` samples a 1024x256
  BC7 atlas and 256x256 tables (`hud_sprite.h:5-9` says the same): **0 of ~300
  such draws in the whole log has a learned surface in slots 0..3**, so it is
  left alone as intended.
- **`kMaxStates = 16` is ample as shipped.** The classified set has 2 distinct
  `(ds, st)` signatures in the cockpit and 1 in the menu; flight 2 saw 5.
- **Case-sensitive `mode == "on"` (`ui_depth.cpp:256`) is the house norm**, not
  a bug: `hud_grain.cpp`, `panel_upscale.cpp` and `wake_pulse.cpp` all compare
  choice strings exactly, and `Config::set` lowercases keys only.
- **Every format specifier matches its argument** in all five log lines
  (`%08lX`/`unsigned long`, `%u`/`uint32_t`, `%.1f`/`double`,
  `%llu`/`unsigned long long`). `Log::note` has no printf attribute, so this
  was checked by hand; the build is also warning-free.
- **The ini obeys the prose rule.** No `# key = value` line in the new block;
  the two `[advanced]` keys use the commented-key form (`#ui_depth_families =`)
  the file already uses for `#surface_inflate =`.
- **`uiDepthConfigure` runs on the render thread**, from the per-frame poll at
  `device_hook.cpp:1028-1030` inside Present, not from a separate config
  thread. Attack item 6's race is not reachable through the reload path. (The
  only concurrency exposure left is item 17 below.)
- **The `!uiDepthWantsDraws()` addition to the early-out at `vscreen.cpp:1473`
  costs nothing on a stock install.** `transition_flash = 1` is the shipped
  default (`edvr.ini:264`), so `countForFlashFix` is already true and that
  early return already never fires.
- **`viewFormatOf` (`draw_census.cpp:240-294`) is reference-clean.** Each
  `QueryInterface` branch releases on success; it is called only for
  `Kind::kView` (short-circuit at `:320`), so a buffer pointer never reaches
  a view QI.

---

## Findings, ranked

### 1. CONFIRMED -- `uiDepthEnd`'s restore can be skipped entirely, leaving the game's depth state swapped

`src/d3d11/ui_depth.cpp:379-387`, against `src/common/guard.h:68-73`.

`guardedBudget` returns false **without running the body** when the budget is
already spent. The restore is inside it:

```cpp
guardedBudget(g_budget, [&] { ctx->OMSetDepthStencilState(g_savedDss, g_savedRef); });
```

So if `g_budget` is exhausted between a successful `uiDepthBegin` and its
`uiDepthEnd`, the game's depth-stencil state is never put back and our
**writing** twin stays bound for every subsequent draw until the game sets its
own -- writing depth for arbitrary world geometry with the UI's test.

Concrete route: the budget is charged by this module's own `boundVsHash`
(`:125`) and by `uiDepthBegin` (`:354`). Within one draw, `boundVsHash` runs
before Begin, so the single-threaded route needs a fault inside Begin's lambda
after `OMSetDepthStencilState(ours, ref)` succeeded but before `g_engaged =
true`, which is three increments and not reachable. The reachable route is
cross-thread: `FaultBudget::m_remaining` is a shared atomic, so a fault charged
on another thread (item 17's flag steal, or any second context) between our
Begin and End spends the last point and the restore is dropped.

The house pattern is the opposite, and says why: `resolve_probe.cpp:436-452`
restores **unguarded**, with the comment "Restore even where the saved pointer
is null: null IS a state the game can have been in, and leaving ours bound
would apply it to every later draw in the frame." `fss_probe.cpp:335` is the
same shape.

**Smallest fix:** drop the budget from the restore. Either call
`ctx->OMSetDepthStencilState(g_savedDss, g_savedRef);` bare, as the two
siblings do, or wrap it in `guarded("uiDepth.restore", ...)`, which has no
budget gate.

### 2. CONFIRMED -- default ON buys nothing for most users and is unguarded against `temporal_aa = off`

`src/d3d11/ui_depth.cpp:253-291`; `edvr.ini:447` (`temporal_aa = off`),
`edvr.ini:479-481`.

The shipped default for `fix.temporal_aa` is **off**. `uiDepthConfigure` reads
only `fix.ui_depth`, so with `ui_depth = on` shipped as the default, a stock
install runs the whole classifier -- 900-odd shader-hash queries and ~3000
memo lookups a frame (finding 8) -- and changes the contents of the game's
scene depth buffer, in exchange for nothing at all. The ini's own prose says
so in the line above the key: "Does nothing useful with temporal_aa off."

The gate exists and is two lines. `depth_probe.cpp:379-382` is the precedent,
verbatim:

```cpp
void depthProbeConfigure(Config& cfg) {
    const std::string mode = cfg.getString("fix.temporal_aa", "off");
    g_wanted = _stricmp(mode.c_str(), "off") != 0 && !mode.empty();
}
```

**Smallest fix:** in `uiDepthConfigure`, read `fix.temporal_aa` the same way
and `g_on = mode == "on" && aaOn;`. Say so once in the log when the key is on
and the pass is not, so a user who set it does not think it is broken. (Note
that `fix.temporal_aa` is itself live, so this must be re-evaluated on every
configure -- it is, since `uiDepthConfigure` is called from
`vScreenRefreshConfig`.)

### 3. CONFIRMED (measured) -- in the main menu the fix writes depth into a buffer the pass does not read

Evidence: census 1 of `edvr_gfx_20260906_105837.log`.

The classified menu composite is `vs A888D51024D9798E / ps 9107E72CB016CC02`,
2 draws a frame, and it binds depth `@413` (res `0000016804CC5720`) and `@427`
(res `0000016804CD14E0`):

```
DC 0 #618 X n=360 i=1 r=@379 d=@413 c=@188 s=@423,@424,-,- vh=A888D51024D9798E ... ds=02wA st=14 q=804
```

EDVR's motion-vector dispatch in the same frame reads a **different**
resource:

```
DCX 0 #30 n=353,348,1 ch=6D94E9C00DCE909F u=-,-,@440,@441,@442,-,-,- s=@443,@444,@445,-,-,-,-,- q=819
   @445 -> res 0000016804CBD5E0   (= @39, the scene depth the depth probe latched)
```

I searched every event in census 1 for a token over `0000016804CC5720` or
`0000016804CD14E0` in any `s=`, `x=`, `u=` or `cb=` field: **zero**. Nothing
reads the depth the menu composite writes -- not the game, and not the pass.

Two consequences. First, the menu half of the advertised behaviour ("the menus
and the loading text write their DEPTH ... the panels register like the cockpit
around them", `edvr.ini:469-475`; "testing ALWAYS where it had none (the
menus)", `ui_depth.h:20-21`) is, on this evidence, a no-op. Second, what it
does instead is not free: `ds=02wA` is `DepthEnable = FALSE`, so
`writingTwin` promotes it to `ALWAYS` + write ALL, and a `n=360` draw at
`vp=0,0+2818x2784` then stamps its own z across the whole viewport of a buffer
nobody reads.

The loading screen is worse: the loader's composites (`A888D51024D9798E`,
`4EF6DDB075A927FA`, `B018D143700AB803`, 24 eye draws over 3 frames) all have a
DSV bound, but they sample two 2212x1244 surfaces (res `0000016858211060`,
`0000016804CC6D20`) that no GUI-family draw writes to anywhere in the log, so
rule (2) never fires there. The "left alone: N with no depth target" counter
would read 0 and tell you nothing.

Flight 2 was flown "docked and after a jump" -- cockpit only. The menu and
loader claims have never been flown.

**Smallest fix:** none, at this size. Either narrow the ini prose and the
header to the cockpit and flight HUD, which is what is actually demonstrated,
or fly the menu and the loading screen with `temporal_aa_debug = depth` and
find out whether the pass's pair is ever the one the UI binds there. (If it is
not, that is the depth probe's draw-count rule, which the handoff doc already
names at line 269-271.)

### 4. CONFIRMED -- classification is partial and time-varying, so some panels carry depth and their neighbours do not

`src/d3d11/ui_depth.cpp:295-318` (learning) and `:320-349` (matching).

A surface is learned only when a GUI-family draw into it is *seen*. The
handoff doc's own flight-1 note says the loader's surfaces "are rebuilt only
when they change", and flight 2 recorded "9 surfaces known within a minute and
13 by the end as panels came into view". So after every scene change there is
a window in which some UI writes depth and some does not.

The census shows the steady state is also incomplete. Per-census coverage of
the intended families: menu 6/18 draws (33%), loader 0/24 (0%), cockpit 72/78
and 66/72 (92%). The menu holds **four distinct 2212x1244 RGBA8 resources**
rotating in one census while the GUI drew into one of them; the two cockpit
misses a frame both sample a 354x387 surface (res `0000016B6BE1E360`), and a
276x164 surface (res `0000016804CC8060`) is composited in four censuses
without a single GUI draw into it. The session-long set closes some of this
over time and cannot close a buffer that is only ever written outside the
window.

The result feeding NVIDIA is a depth channel in which two of twenty-six holo
panels a frame are at the far plane and the rest are at a metre, and which
panels those are changes as the game refreshes different surfaces. That is
precisely the misregistration this fix exists to remove, applied to a moving
subset.

**Smallest fix:** none available at B0's size -- this is inherent to learning
by observation. What is available and cheap is *visibility*: put the learned
surface count, the per-family classified counts and the miss count on the
totals line (see finding 6), so a session can tell "13 surfaces, 24 composites"
from "13 surfaces, 22 composites" without a census.

### 5. CONFIRMED (mechanism) -- the kept GEQUAL test creates an occlusion order between UI elements that did not exist before

`src/d3d11/ui_depth.cpp:178-184`.

With the game's `GEQUAL` kept and `DepthWriteMask` raised to ALL, a UI element
drawn later and farther is now clipped by an earlier, nearer one. The census
fixes the order: **per eye the flight HUD block is drawn first and the
composites after** (census 3, all three frames: 11 HUD draws at q 1344..1435,
then 12 composites at q 1387..1535; census 4 the same shape at 10 and 11). So
the flight HUD, which used to paint over everything, now wins the depth test
against every holo panel behind it -- and a panel behind the HUD loses its
overlap.

The classified set is many separate pieces of geometry, not one quad: index
counts in census 3 run 6, 12, 18, 19, 23, 24, 31, 36, 42, 91, 131, 215, 225,
279, 435. Whether any two of them overlap in screen space cannot be read from
the census (every classified line is `vp=0,0+2818x2784` with scissor
disabled). Flight 2 judged text crispness, not missing HUD elements.

The handoff doc names this as hazard (a) (`docs/crisp-ui-handoff.md:174-177`)
and says the copy form is the fallback "if either hazard shows". Nothing in
the shipped feature would let it show: there is no debug view, no per-family
counter, and no A/B beyond the whole key.

**Smallest fix:** an `advanced.ui_depth_test = as_is | always` instrument, two
lines in `writingTwin` (force `D3D11_COMPARISON_ALWAYS` unconditionally), so a
flight can A/B the ordering hazard against the occlusion it is there to
preserve. Cheaper than either building the copy form or discovering it in the
field.

### 6. CONFIRMED -- the header states two safety properties the code does not have

`src/d3d11/ui_depth.h:44-49` against `src/d3d11/ui_depth.cpp:320-349` and
`:389-418`.

> "So does anything whose target is not the lit HDR buffer or the tonemapped
> 8-bit one -- the log names each family's target, so a new case is visible."

Both halves are false.

- **There is no target check anywhere in the module.** `uiDepthOnEyeDraw`
  inspects SRV slots 0..3 and the vertex-shader hash, and requires a non-null
  `Dsv0`. It never looks at `Rtv0`'s format or identity. A composite drawn
  into any eye-sized target -- including the RGB10A2 G-buffer -- is treated.
  The G-buffer stays safe today only because nothing that samples a learned
  surface happens to draw into it (verified above), not because the code
  refuses it.
- **The log names nothing.** The engage line (`:394-398`) and the totals line
  (`:402-412`) carry counts only: no shader hash, no target, no size, no
  format. A new family arriving in the galaxy map, on foot or after a game
  update is invisible; the counters would move by a few and nothing would say
  what they were.

This matters because the header's whole argument for leaving the target
indicator alone -- "a depth written there would light and fog the space behind
the hologram" -- rests on a mechanism that does not exist. The exclude list is
empty by default and is a developer instrument the user will never set.

**Smallest fix:** (a) log each newly classified `(vh, rtv WxH, rtv fmt)` once,
capped at say 16 lines, which is the visibility the header promises and is
about fifteen lines of code; and (b) either implement the target check the
header describes, or rewrite those three sentences to say what is true -- that
the indicator is left alone because it samples an authored atlas and not a
learned surface.

### 7. CONFIRMED (absence of code) -- no scope restriction, and the doc's own "Do not" forbids exactly this

`docs/crisp-ui-handoff.md:935-940` says: "Do not write UI depth into the game's
own scene depth without G10 ... the menus and the loading screen need their own
G10 before B0 is trusted in them."

The built feature has no context restriction of any kind. It runs in the galaxy
map, station services, the system map, the FSS, on foot and the SRV, none of
which has ever been censused. Two premises the header rests on are measured for
four families only:

- "the UI's pixel shaders discard on alpha, so the depth lands on the strokes
  and the backing, not on empty quad" (`ui_depth.h:26-27`) -- measured for the
  holo panels (2 sites), the flight HUD (5) and the menu/loader composites (1
  each). A composite whose pixel shader does not discard stamps depth across
  its whole transparent quad, and with the test kept, everything drawn behind
  it afterwards is cut.
- G10 itself, which I re-verified for the menu and two cockpit censuses and
  which nobody has asked anywhere else.

**Smallest fix:** none that is small. The honest options are (i) ship default
OFF until the un-censused contexts are flown, or (ii) accept the risk
explicitly and give the totals line finding 6's per-family lines so a field
report can name what was treated.

### 8. CONFIRMED (measured) -- the memo cannot hold one frame's working set, so it thrashes; and its miss path shares a five-fault budget with `targetIsEyeSized`

`src/d3d11/ui_depth.cpp:33` (`kMaxMemo = 256`), `:137-158` (linear scan, FIFO
replacement), `:148` (`bindingResolve`), against
`src/d3d11/binding_shadow.cpp:27` and `src/d3d11/binding_shadow.h:51-56`.

Measured from the census, per frame:

| census | eye draws | offscreen draws | distinct PS SRV slot 0..3 views | `viewIsSurface` calls |
|---|---|---|---|---|
| 1 menu | 626 | 104 | 170 | 2058 |
| 3 cockpit | 784 | 755 | **264** | 2527 |
| 4 cockpit | 917 | 915 | **275** | 3032 |
| 5 cockpit | 759 | 705 | **257** | 2468 |

All three cockpit views exceed the 256-entry memo. Replacement is plain FIFO
round-robin (`:150-152`), which is the worst policy for a cyclic working set
slightly larger than the cache: it evicts the entry about to be reused. The
lookup itself is a linear scan of all 256 slots (`:140-145`), so a frame costs
up to ~3000 x 256 = 0.77 M pointer compares, plus one guarded `bindingResolve`
per miss.

The second half is the part that worries me more. `bindingResolve` runs under
**one shared budget** for the whole DLL:

```cpp
FaultBudget g_probeBudget("bindingShadow.resolve", 5);   // binding_shadow.cpp:27
```

and `binding_shadow.h:24-35` states the consequence: five faults and the probe
answers "unknown" for the rest of the session, for every consumer. Those
consumers include `targetIsEyeSized` (`vscreen.cpp:1026`), which is what feeds
the black void, the transition flash fix, Explorer Cam and ui_depth itself. A
thrashing memo pushes 2500-3000 pointers a frame through that probe, on slots
`PsSrv1..3` -- which `binding_shadow.h:51-56` says explicitly were added for
the census and that "No fix derives answers from them". ui_depth is the first
fix to do so, on the hot path.

I want to be fair about the probability: the log gives no evidence of actual
faults (the census's own `DC id @N ?` entries, 70-132 per census, are
`describeResource` returning false, not necessarily SEH faults, and the
budget clearly survived every census). But the exposure is a large multiple of
anything previously put through that probe, on the slots least likely to be
rebound and therefore most likely to hold a pointer the game has released.
CONFIRMED mechanism, unquantified trigger rate.

**Smallest fix:** make the memo fit and make the lookup O(1). Raise `kMaxMemo`
to 1024 and index it by `(reinterpret_cast<uintptr_t>(view) >> 4) & (kMaxMemo -
1)` with the stored `view` pointer as the tag -- one probe, one compare, and
with `kMemoLifeFrames = 120` the steady state becomes about 275/120 ~= 2
resolves a frame instead of thousands. That is roughly a ten-line change and it
removes both halves of this finding.

### 9. CONFIRMED -- `boundVsHash` runs on every eye draw, after the expensive test, and the cheap reject was moved last

`src/d3d11/ui_depth.cpp:320-349`.

`boundVsHash` (`:331`) is a `VSGetShader` (a COM call with AddRef/Release)
plus `lookupShaderHash`, which is `EnterCriticalSection` + `unordered_map::find`
+ `LeaveCriticalSection` (`exposure_fix.cpp:351-365`), inside a
`guardedBudget`. It runs unconditionally on 784-917 eye draws a frame -- about
82,000 lock-and-lookup pairs a second at 90 Hz -- and it runs even when
`composite` is already true and even when the exclude list is empty, which is
the shipped default.

This inverts the house rule, which two headers in this directory state in the
same words: "cheapest test first, and the hash last because it costs a
VSGetShader" (`hud_sprite.h:72-75`, `target_sharp.h:62-65`);
`hud_grain.cpp:141-161` implements it, rejecting most HUD draws on a slot-1
shape test before ever touching the shader.

`7e24c22` also moved the cheapest test in the function -- the one-array-read
`bindingGet(BindSlot::Dsv0)` -- from first to last (`:339`), so the ~10
depthless post-chain eye draws a frame now pay four memo lookups and a shader
hash before being dropped. That one is small (10 draws), but it is the wrong
direction and it was done for a counter.

Related, same file: `uiDepthNoteOffscreenDraw` (`:295-318`) re-resolves the
render target and re-arms a 64-hash budget on **every generation change** of
`Rtv0`. The generation bumps on every `OMSetRenderTargets` and once per frame,
so for a target that will never be a GUI surface the module asks up to 64
shader hashes per rebind, every frame, for the life of the session. The
cockpit censuses hold roughly 77 RTV bind changes a frame; a lower bound from
the log is 356 `boundVsHash` calls a frame from this path alone. The header's
claim "Cheap: the hash is asked only until a target is known"
(`ui_depth.h:70-71`) is true only for targets that *become* known.

**Smallest fix:** (a) restore the `Dsv0` reject to the top of
`uiDepthOnEyeDraw` and reword the totals line's "no depth target" figure to
"depthless eye draws"; (b) skip `boundVsHash` when `composite` is already true
and `g_excludeCount == 0`; (c) remember per-target that a target was checked to
exhaustion once, so the 64-hash budget is not re-armed every frame for the same
resource (a small "checked and not a surface" set beside `g_surfaces`).

### 10. CONFIRMED -- the state cache creates a D3D object before checking whether it has room, and is never reset except at shutdown

`src/d3d11/ui_depth.cpp:163-220`, `:420-431`.

`CreateDepthStencilState` is called at `:188`; the `g_stateCount >= kMaxStates`
check is at `:201`, after it. So once the table is full, **every UI draw
creates a depth-stencil state and immediately releases it** -- 63-82 creates
and releases a frame, forever, with the explanatory note logged once. The
comment at `:201-203` ("a twin made per draw would leak") shows the leak was
considered; the churn was not.

Reaching a full table is not exotic. The classified set has 2 signatures today,
but a cockpit census holds 27 distinct `(ds, st)` signatures among eye draws,
so any widening via `advanced.ui_depth_families` fills 16 quickly. The other
route is device recreation: `uiDepthShutdown` is called only from
`shutdownVScreenFixes`, which is called only from `shutdownDeviceHooks`
(`device_hook.cpp:1811`) at process teardown. After a device removal and
recreate the table still holds AddRef'd states belonging to the dead device --
which also keeps that device alive -- and the new device's states are appended
until the table fills.

**Smallest fix:** hoist the capacity check above the `CreateDepthStencilState`
call. Optionally also call `releaseStates()` when the device changes.

### 11. CONFIRMED (mechanism), no instance observed -- learned surfaces are never evicted, and no reference is held

`src/d3d11/ui_depth.cpp:53-55`, `:105-114`, `:420-431`, against
`docs/crisp-ui-handoff.md:300-306`, which designed the eviction the code does
not have ("Evict on a size change of the eye (the game recreates everything)
and when the shadow's resolve of the pointer fails").

Surfaces are kept as bare resource addresses for the session, with no
reference held (correctly, per the binding shadow's bargain). Nothing removes
them: not a resolution change, not a trip through the main menu -- which the
doc itself says recreates the cockpit surfaces (`:625-627`) -- not the cull
guard's restage, not a device recreate. A destroyed surface's address reused by
an unrelated texture is then a false positive that lasts the session, and a
non-UI eye draw sampling it would write depth. This is the exact ABA the
binding shadow's own header warns about ("an identical address after a rebind
is not evidence of an identical object, which is the bug that shipped as
0.5.2", `binding_shadow.h:99-102`).

I found no instance in the log: no learned resource later appears at a
different shape or as an eye RTV. Mechanism CONFIRMED, occurrence unobserved.

Separately, the ring at `:107-112` silently overwrites index 0 onward once 64
surfaces are known, so a live surface can be forgotten with no log line. Its
panel then stops writing depth and its flicker returns. 13 surfaces were seen
in flight 2, so this is latent.

**Smallest fix:** clear the surface set (and the memo) when the eye size
changes -- `vscreen.cpp` already knows, and `panel_upscale`/`fss_res` already
hang off that event -- and count evictions on the totals line.

### 12. PLAUSIBLE -- the stencil consequence of changing the depth-test outcome

`src/d3d11/ui_depth.cpp:172-184`.

The census says the UI draws run with stencil **enabled**: `ds=17wZ st=14` on
the holo panels (DepthEnable 1, GEQUAL, write ZERO, StencilEnable 1, ref 4) and
`ds=02wA st=14` on the menu composite. The twin copies the stencil fields
verbatim, which is right -- but by making earlier UI draws write depth it
changes whether *later* draws pass or fail the depth test, and that selects
`StencilDepthFailOp` instead of `StencilPassOp`. The census does not print the
stencil ops, so I cannot say whether they differ.

Mitigated, and this is why it is not higher: the eye depth-stencil pair is
cleared with `f=3` (DEPTH|STENCIL) every frame before the UI runs, so any
divergence is confined to the frame's post-UI draws, and G10 says nothing the
game owns runs there.

**Smallest fix:** none needed if G10 holds everywhere. Worth one line in the
header noting that the twin preserves stencil state but not stencil *outcome*.

### 13. CONFIRMED (bounded) -- a reference leak on a fault inside `uiDepthBegin`

`src/d3d11/ui_depth.cpp:354-370`. `game` is a local inside the guarded lambda;
a fault after `OMGetDepthStencilState` and before either the `Release` or the
store into `g_savedDss` leaks one reference. Bounded to five for the session by
the budget, and the same trade-off `binding_shadow.cpp:120-126` documents and
accepts. Reported for completeness, not for action.

### 14. CONFIRMED (cosmetic) -- `g_wNoState` conflates two different outcomes

`src/d3d11/ui_depth.cpp:359-362` increments `g_wNoState` whenever `writingTwin`
returns null, which covers three cases: no twin could be made, the table was
full, and **the game's own state already writes depth** (`:174`, `:219`). The
totals line calls all of it "with no writable state" (`:405-406`). A future
reader will mis-diagnose a healthy "already writes" as a failure. One extra
counter.

### 15. CONFIRMED (cosmetic) -- a live off/on toggle after a stand-down cannot re-arm, and re-logs

`src/d3d11/ui_depth.cpp:273` clears `g_stoodDown`, but `FaultBudget::m_remaining`
(`guard.h:52`) is never restored. So the next `uiDepthBegin` gets `false` from
`guardedBudget` without running, sees `!g_budget.shouldRun()`, and stands down
again -- printing the STANDING DOWN line a second time. Every toggle prints it
again. Either do not clear `g_stoodDown`, or give `FaultBudget` a reset and use
it deliberately.

### 16. PLAUSIBLE (pre-existing shape) -- a deferred-context draw can steal the per-draw flag

`src/d3d11/vscreen.cpp:1415-1424` returns for a foreign context **before** the
flag clear at `:1428`, and `UiDepthScope` (`:2700-2711`) consumes
`g_state->uiDepthThisDraw` unconditionally. If the render thread sets the flag
and a deferred-context draw on another thread reaches `forwardWithVerdict`
first, that thread runs `uiDepthBegin`/`uiDepthEnd` on the *deferred* context
and the real UI draw goes untreated. `g_engaged` and `g_savedDss` are plain
globals shared by both.

This is the same shape as the pre-existing `curveThisDraw`, and the handoff doc
records that no `ExecuteCommandList` was ever observed in the cockpit
(`:535-538`). Consequences are one lost UI draw and a pair of harmless
commands on a deferred context -- except that it is also the practical route to
finding 1. Low.

**Smallest fix:** clear both flags before the foreign early return, or (better)
have the scope take the flag from a thread-local.

### 17. Census changes: acceptable, with two notes

`src/d3d11/draw_census.cpp`.

- **The 2048-entry table is a linear scan** (`internOf`, `:301-322`), and the
  commit message's own estimate ("a few tens of milliseconds per census
  frame") is about right: censuses 3 and 4 interned 1108 and 924 objects, and a
  cockpit frame makes several thousand `internOf` calls. That is a three-frame
  hitch on an instrument nobody runs by accident, and the table upgrade was
  necessary -- census 1 interned 504 against the old 512 cap. Accept.
- **`vf=` is captured for every interned view but printed only on texture
  lines** (`:320` sets it for `Kind::kView`; `dumpInternTable`'s buffer branch
  has no `vf=`). A view over a buffer silently loses its format. Cosmetic.
- Not a code finding, but load-bearing for the evidence: **the log hit
  `log.max_mb` during census 5's intern table**, so that census has no `res=`,
  no sizes and no `vf=` and cannot be used for anything keyed on resource
  identity. Any claim of the form "verified across five censuses" is really
  four. Raise `log.max_mb` before the next census session (the handoff doc
  already says so at `:192-193`).

### 18. Reader tool: two divergences from the code it is used to reason about

`tools/crisp_ui_gates.py`.

- Its `DIRECT` list contains `5DA53D8B0133341E`; `ui_depth.cpp`'s built-in
  direct list is `kFlightHud` only (`:263`). Its G10 output for the menu census
  therefore reports a depth toucher that comes from the target indicator's
  scene depth -- a buffer `ui_depth` never binds. Do not read the tool's G10 as
  the classifier's G10.
- It parses no `DCL` depth-clear lines at all, so "nothing clears the depth
  mid-frame" is not something the tool can tell you. (I checked the clears
  separately; they are clean.)

Neither is a defect in the tool for its stated purpose -- it predates the
built classifier -- but the next session will reach for it as if it modelled
`ui_depth.cpp`, and it does not. Worth one comment at the top of the file.

---

## Doc claims the code does not make good

Collected for the implementing session; each is covered above.

| Claim | Where | Reality |
|---|---|---|
| "anything whose target is not the lit HDR buffer or the tonemapped 8-bit one" is left alone | `ui_depth.h:47-49` | no target check exists (finding 6) |
| "the log names each family's target, so a new case is visible" | `ui_depth.h:48-49` | the log carries counts only (finding 6) |
| the target indicator "stays depthless" | `ui_depth.h:44-47` | true today, but by accident of what it samples, not by code |
| the menus write depth so the panels register | `ui_depth.h:20-21`, `edvr.ini:469-475` | measured inert in the menu (finding 3) |
| "the hash is asked only until a target is known" | `ui_depth.h:70-71` | re-armed every rebind for targets that never become known (finding 9) |
| "Evict on a size change of the eye ... and when the shadow's resolve fails" | `docs/crisp-ui-handoff.md:300-306` | no eviction exists (finding 11) |
| "the menus and the loading screen need their own G10 before B0 is trusted in them" | `docs/crisp-ui-handoff.md:938-940` | no scope restriction exists (finding 7); I did verify the menu's G10 and it holds |
| "About forty lines" | `docs/crisp-ui-handoff.md:170` | 434 lines plus the wrap |

---

## Merge recommendation

**Merge after fixes 1, 2, 8 and 9; default OFF unless 3 is also resolved.**

The core mechanism is right and the cockpit evidence is better than most
things that ship here: G10 verified independently on three censuses, no false
positives in anything censused, the G-buffer untouched, the depth cleared every
frame, the state cache correctly reference-counted, the RAII scope airtight
across every re-issue path, a clean build and green contract, schema and
self-tests. Sean's "that looks much better" is backed by 63-82 draws a frame
writing depth with no faults, no stand-down and no frame-rate change. Nothing
here crashes or hangs.

What is not ready is the *default*. Four things argue against ON. (i) The
shipped `temporal_aa` default is `off`, so the majority of users would run the
classifier and change their scene depth for a benefit the ini's own prose says
does not exist -- and the gate is two lines with a precedent in
`depth_probe.cpp`. (ii) The feature is measurably inert in the main menu, and
classifies nothing in the loading screen, so half of what the key promises is
unflown and, on the evidence, non-functional. (iii) The cost is not
negligible for a feature that is on for everyone: a 256-entry FIFO memo against
a measured 257-275-view working set thrashes permanently, pushing 2500-3000
pointers a frame through a `bindingResolve` whose five-fault budget is shared
with `targetIsEyeSized` -- the answer four other fixes depend on. That is a
silent, session-long, cross-feature failure mode, and it is the one thing in
this change that could make a user's session worse in a way nobody would
diagnose. (iv) It runs in every context, including the six nobody has
censused, with no per-family logging to say what it treated when a field
report arrives.

None of that is hard. Fix 1 is one line, fix 2 is two, fix 10 is a moved
brace, and fix 8 is about ten lines that turn the memo into a hash probe and
make finding 8's exposure vanish. Fix 9 is three small edits. With 1, 2, 8, 9
and 10 in, and either finding 3 resolved or the ini prose narrowed to the
cockpit and flight HUD, default ON is defensible for the cockpit -- which is
where the defect Sean reported actually lives. I would still want the
per-family log line from finding 6 in the same release, because the first field
report from a galaxy map or an on-foot session is otherwise unanswerable.

If the choice is binary today: **merge with the key default OFF**, ship it in
the settings window as it stands, and flip the default in the next release
after a menu, a loading screen and one un-censused context have been flown.
This codebase's own ladder puts a one-flight fix under `[experimental]` first
(`hud_grain`, `holo_panels`, `target_indicator`, `hud_icons` all did that, and
`temporal_aa` moved to `[fix]` only when it shipped); `fix.ui_depth` default-on
after a single cockpit flight skips two rungs of it.
