# Anti-aliasing and the shimmer: a design

*A design document, written before the code, as a companion to
[performance.md](performance.md). Claims about EDVR cite the source; claims
about the game, runtimes and SDKs are labelled measured (established in this
repo's field logs or code), vendor-stated (their documentation or release
notes), or believed; what can only be settled at implementation time or in a
live session is collected under Phase 0. Feature A's passive mode was built
on 2026-09-02, field-verified on both rigs by 2026-09-03 and ships as `auto`
by default — its section records what was built and measured; everything
else here is design.*

## The ask

Two questions, asked together. The anti-aliasing Elite offers is widely
held to be bad, worst of all in a headset — could EDVR supply its own MSAA
when the player turns the game's off? And more broadly: what can be done
about the shimmer VR players see?

**The ask, verbatim from the field:** *"some way to attack the shimmering
via some custom post processing — guess that's what ReShade is for, but
nice if it would be part of EDVR. A super-tuned processing just to tackle
the shimmering we all see so much of. A kind of Elite-specific anti-shimmer
effect. Could we do something like this performantly?"* It came with a
proposal worked out elsewhere: pull the game's real view and projection
matrices out of its constant buffers, the way the from-scratch upscaler
injections do for games with no native upscaler, combine them with the
depth buffer into exact motion vectors for camera motion and static
geometry, and accept that independently moving objects — other ships,
station rings, planets — would need their per-object matrices too, which
is per-game reverse engineering that breaks on engine updates; "a real
project, not a shader tweak". That proposal is answered under
[The field's proposal](#the-fields-proposal-motion-vectors-from-the-games-own-matrices):
it is the right shape, it is feature B below, and EDVR already holds
most of what it needs.

The short answers, argued below:

- **MSAA: no.** Not "hard" — unavailable, from outside a deferred
  renderer, for reasons that do not move. What MSAA is *for* (more samples
  per pixel) is reachable from outside in exactly one form, supersampling,
  and EDVR can do that better than it is done today (feature A).
- **The shimmer is a temporal problem, and the game has no temporal
  anti-aliasing.** Every option in Elite's menu is a post-process edge
  filter (believed; Phase 0 checks it), which by construction cannot touch
  sub-pixel flicker. The feature that addresses the complaint is a
  temporal anti-aliasing pass at submit (feature B), and EDVR already owns
  every hook it needs — including, unusually for an injected TAA, the
  per-frame projection edit that gives it a real sub-pixel jitter.
- Two small levers (a texture LOD bias, feature C; a sharpen, already
  designed) and one shader-level one (specular anti-aliasing in the
  lighting resolve, feature D) round it out.
- Until any of that ships, there is guidance the README and `edvr.ini` can
  carry today, under [Guidance for players now](#guidance-for-players-now).

## What the shimmer is, and why an edge filter cannot touch it

A rendered pixel is one sample of the scene. Anything narrower than a pixel
— a station's girders, the HUD's hairlines and text, the specular glint on
a hull panel, the noise of a detail map on terrain, a distant ship — is
either hit by that sample or missed by it, and which one changes as the
sample grid moves over the scene. On a monitor the grid moves when the
camera does. In a headset it never stops moving: the head is never still,
pixels are large in degrees (the field Quest 3 renders 1456 pixels across
a 94° horizontal frustum at the eye size it measured over Steam Link —
about fifteen per degree; `compositor_hook.cpp` and `system_hook.cpp`),
and the eye is drawn to motion. That is the shimmer — content flickering
on and off the grid, frame after frame.

Post-process anti-aliasing (FXAA, SMAA, MLAA and their relatives) runs on
the *finished* image. It finds edges among pixels that have already been
resolved and blends along them. It can smooth a staircase; it cannot know
that a girder existed between two samples, because nothing in its input
records that. Applied to shimmer, an edge filter turns flicker into
smoothly-blended flicker. This is why the field lore around Elite in VR —
"only HMD Quality does anything", "turn AA off and supersample" — is right:
the game's AA is not bad at what it does, it is the wrong kind for the
problem.

Only two things address sub-pixel flicker: more samples per pixel, or the
same samples integrated over time.

| Technique | What it samples | Where it must live | Reach from a proxy DLL |
|---|---|---|---|
| MSAA | extra coverage and depth samples per pixel, shading once | the rasterisation of every geometry pass, and the lighting that reads them | none — [below](#msaa-from-outside-and-why-the-answer-is-no) |
| SSAA (supersampling) | a bigger image, filtered down | any size answer plus a filter at the door | full — feature A |
| TAA (temporal) | the previous frames, reprojected | after rendering, before display | full — feature B |
| post-process (FXAA/SMAA/MLAA) | the finished image's edges | anywhere | full, and useless alone |

**What the game offers.** Elite's anti-aliasing menu offers Off, FXAA,
SMAA and the two MLX modes (MLX2, MLX4) — believed; the menu itself is the
check. All four are believed to be post-process filters of the same family
— none multisampled, none temporal — which is consistent with the field
lore above and with their near-zero cost. Frontier announced a fuller
anti-aliasing solution for Odyssey in 2021 and later cancelled it; none
has shipped since (believed, from the public record). Two things this
repo has measured bear on it, and both point the same way:

- The deferred lighting resolve reads its four geometry-buffer inputs with
  `ld_indexable` — ordinary `Texture2D` loads, not multisample loads
  (measured: `src/d3d11/resolve_probe.cpp`, from the shader's own
  disassembly).
- Nothing in this repo's field write-ups records a multisampled render
  target or a `ResolveSubresource` on the main view, though the census has
  recorded resolves as `V` lines since 2026-08-25 (`src/d3d11/vscreen.cpp`)
  and every copy path refuses `SampleDesc.Count > 1` with a log line that
  has never been reported. A limited claim — nobody was looking, and the
  AA setting those sessions ran is unrecorded — and the Phase 0 item that
  makes it a measurement is one flight with each menu option in turn.

## MSAA from outside, and why the answer is no

How MSAA works: at rasterisation the hardware tests coverage and depth at
several positions inside each pixel, runs the pixel shader once, and stores
one colour per *covered sample*; a resolve afterwards averages them. It
anti-aliases geometry edges, and only those. In a forward renderer that is
the whole story. In a deferred renderer — Elite's (measured: the frame is a
geometry buffer, a lighting resolve that reads it, an HDR image and a long
post chain, each twice; `src/d3d11/eye_split.cpp` and
`docs/scanner-body.md`) — the geometry buffer itself would have to be
multisampled, and every pass that reads it would have to read *per sample*
at edges and shade each one, or the edges come back aliased with the cost
already paid. Engines that do MSAA in deferred pipelines build it in from
the start, with edge-detection masks and per-sample lighting branches.

What a proxy could try is the transparent trick: at `CreateTexture2D`,
raise `SampleDesc.Count` on the eye-sized targets. It breaks four times
over:

1. **Views.** A shader-resource, render-target or depth view over a
   multisampled resource must be created with the `TEXTURE2DMS` view
   dimension; the game's `TEXTURE2D` views fail to create (vendor-stated,
   the D3D11 view rules). EDVR would have to rewrite every view
   description too.
2. **Shaders.** Every shader that reads those targets declares
   `Texture2D` and uses `ld`/`sample`; a multisampled resource needs
   `Texture2DMS` and `ld_ms` with a sample index. Those are the game's
   shaders — hundreds of them — and each one would need transcribing, the
   way the sun-glare and panel vertex shaders were, but for every reader in
   the post chain.
3. **The lighting.** Even with the reads fixed, the resolve shades once per
   pixel. Correct MSAA in a deferred renderer means per-sample shading at
   edges, which is a change to the game's lighting shader, not to its
   resource descriptions.
4. **Everything else the sixteen targets go through** — copies, the
   tonemap chain, the temporal reconstruction the scanner runs — would meet
   a resource kind it was never written for.

This is exactly the trick the graphics drivers' control-panel overrides
("enhance the application setting") perform, and it is exactly why those
overrides do nothing on deferred D3D11 titles — a known limitation, and the
same one that disqualifies NVIDIA's VRSS for this game (performance.md,
"What translates"). Elite's own MLX modes, if they were ever multisampled,
would be the engine doing this from inside; the menu's near-zero cost says
they are not.

**Where MSAA could be bolted on, and why it is declined.** One class of
draw is MSAA-able from outside: forward-rendered overlays — the holographic
HUD's lines and text, a genuine source of shimmer. Redirect those draws
(recognised by shader hash, the census's currency) into an EDVR-owned
multisampled target, with the scene's depth copied into a multisampled
depth buffer so the HUD still occludes correctly, resolve, and composite
back with the game's own blend. It is real machinery — a depth
replication pass, a redirected target per eye, a composite that must match
the game's blend state exactly — and it anti-aliases the HUD's edges only,
while features A and B below anti-alias the HUD *and* everything behind it
with none of that. Declined on those grounds, not on feasibility.

So: the honest answer to "our own MSAA" is no. What survives of the idea
is the half of it EDVR can actually do — more samples per pixel, by
rendering more of them and resolving them well.

## What EDVR already owns

The reason both real features are tractable:

- **The size answer.** `hookedGetRecommendedRenderTargetSize`
  (`src/openvr/system_hook.cpp`) multiplies the size handed to the game,
  and the game rebuilds its targets as for any supersampling change
  (measured; the cull guard's stage 1). Feature A's render half is this
  hook with a factor above 1.0 — performance.md's `render_scale`, the
  other side of 1.0.
- **The projection edit, per frame.** Elite re-queries
  `GetProjectionRaw` and `GetProjectionMatrix` every frame (measured,
  ~1080 calls a second each), and the cull guard already edits both
  consistently — tangents in the raw thunk, the four tangent-carrying
  matrix elements in the member-shaped receiver, switched only at the
  frame boundary (`system_hook.cpp`). A sub-pixel jitter is the same edit
  with a *shift* instead of a widening: `l`, `r`, `t`, `b` all moved by one
  small delta, and the matrix's `m02`/`m12` following from the shifted
  tangents exactly as they follow from the lied ones today. This is what
  makes feature B a real TAA rather than a temporal smoother.
- **The submit door.** `hookedSubmit` (`src/openvr/compositor_hook.cpp`)
  already substitutes an EDVR-owned texture for the game's on three paths,
  with `applyCullGuard`'s discipline — one lambda, every forwarding path,
  no path ships an untreated frame. Both features are one more treatment
  inside that lambda.
- **Both sizes, in one DLL.** The openvr half holds the size the runtime
  recommended (`trueSizeW/H`) and the size the game actually submitted per
  eye (`noteEyeTextureSize`, re-read every six seconds). Their ratio is
  the supersampling factor in force, whoever set it — Elite's HMD Quality
  or EDVR's own lie — and feature A keys on it directly.
- **The compute-pass shape.** `intro_upscale.cpp` runs a chain of compute
  passes (deband, EASU, RCAS) over textures at submit-adjacent points, with
  constants computed on the CPU and the HLSL desk-compiled before it ships.
  A resolve filter and a TAA pass are two more passes of that shape. RCAS
  — the sharpen that offsets any filter's softness — is vendored and running.
- **The depth buffer, identified.** The census records the depth view
  bound with every eye draw (`d=`, `docs/scanner-body.md`), the eye-split
  dump copies the "two depth-ish planes" per eye (`eye_split.cpp`), and
  the terrain work measured the convention: reversed-Z, near 1, far 50000
  (`system_hook.cpp`, `scanner-body.md`). Feature B's reprojection needs
  exactly this.
- **The camera, twice over.** The head pose at `WaitGetPoses` and the
  eye-to-head transform (observed on slot 4, never lied about) give the
  head's motion between frames; and the flash detector already reads the
  game's own camera buffer every frame, recognised by size
  (`src/d3d11/glitch_frame.h`) — the source for the *ship's* motion, which
  the head pose does not carry.
- **Single-threaded rendering** (measured: Submit and Present on one
  thread, no `ExecuteCommandList` ever seen), so both eyes' passes run
  sequentially on the immediate context from inside the submit hook.
- **Shader substitution by content hash**, including the lighting resolve
  itself — `resolve_probe.cpp` has already swapped that exact pixel shader
  for a replacement, for one draw, and put it back. Feature D is that
  mechanism with a different replacement.

## Feature A — supersample resolve at the door

**What it is.** When the game submits an eye image larger than the runtime
asked for, EDVR filters it down to exactly the recommended size itself, with
a kernel chosen for anti-aliasing, and submits that with full bounds. The
compositor then samples 1:1 through its distortion pass instead of
decimating a large texture on the fly.

**Why it is worth doing when supersampling already exists.** Today a
supersampled frame is filtered down by whoever runs the distortion pass.
SteamVR's default is a bilinear tap (believed — it is what performance.md
already relies on for the upscale direction), which at a factor of 2.0 is a
2×2 box and at 1.4 skips texels and aliases; its *Advanced Supersample
Filtering* option applies a better kernel, "less aliasing at the cost of a
slightly softer image", and by its own description needs a high resolution
value to notice (vendor-stated, the setting's text). OpenComposite
installs get whatever the runtime underneath does, which nobody here has
measured. EDVR's resolve is the same filter on every stack, its kernel is a
setting rather than a fixed choice, the sharpen composes after it, and the
crop for the cull guard fuses with it — the argument feature 1 of
performance.md makes for the upscale direction, made for the downscale one.

**Mechanism.** Two modes of engagement, same pass:

1. *Passive (v1):* the submit hook sees per-eye size > recommended size and
   arms — no size lie, nothing new asked of the game. Whatever produced
   the supersampling (Elite's HMD Quality above 1.0, which scales the
   game's targets past the recommended size and leaves the downsample to
   the compositor — measured 2026-09-02 on the Pimax over native SteamVR:
   HMD Quality 1.25 submitted 6780x6695 per eye against a recommended
   5424x5356, exactly 1.25x on each axis, one texture per eye with null
   bounds) is left as it is; only the resolve changes hands.
2. *Active (v2):* `render_scale` above 1.0 tells the game the headset wants
   more pixels (the stage-1 size lie, measured to work) and the same pass
   resolves them back to native. EDVR then controls both ends: the player
   leaves HMD Quality at 1.0 and turns one knob, in the same place they
   turn it down for FSR. Adoption follows the guard's changed-size test,
   frames forward untouched until both eyes submit at the new size.

The kernel: a separable Gaussian in output-pixel units by default (calm,
never rings on the HUD's hairlines), a Mitchell/Lanczos option for players
who prefer crisp over calm, width as a setting. RCAS after it, at
`render_sharpness`, recovers what the Gaussian softens. Both the crop and
the resolve take an input sub-rectangle, so guard+resolve is one dispatch
once the edge-tap subtlety performance.md already lists is settled.

**What it does and does not fix.** It is real anti-aliasing — the pixel
count is the anti-aliasing, and the filter is how much of it reaches the
eye — but the pixels still have to be rendered, which is the expensive
part players already pay. It is worth about one resolve's difference over
today's supersampling, not a new category. Its structural value is that it
is the door TAA walks through next, and that TAA over a supersampled input
is the best anti-aliasing available anywhere.

**Settings** (`[fix]`, the names as built; performance.md's `render_scale`
is not in this tree yet and is not invented here — the active mode waits
on it):

```
supersample_resolve = auto   ; off | auto | on. auto engages whenever the
                             ; game submits larger than the runtime asked
                             ; for, however that came about, and stays
                             ; quiet otherwise; on is the same and says so
                             ; when it finds nothing to do. Live.
supersample_filter  = calm   ; calm (gaussian) | crisp (mitchell). Live.
supersample_width   = 1.0    ; kernel radius in output pixels, 0.5..3.0;
                             ; wider is calmer and softer. Live.
```

Pixel cost quoted the way performance.md quotes it: HMD Quality 1.4
renders ~196% of native pixels, and that is the game's price, paid
already. The pass's own price is measured by timestamp query and printed
in the graphics log; believed well under a millisecond per eye at headset
sizes until the field says otherwise.

**What was built (2026-09-02: the passive mode; `auto` by default since
2026-09-03).** The
openvr half decides (`src/openvr/supersample_resolve.cpp`). Every
forwarded submit's per-eye size — the texture's size narrowed by its
bounds, which is the post-crop size when the cull guard is live — is
judged at the frame boundary against the recommendation the system hook
captured (`systemHookRecommendedSize`): the resolve arms once both eyes
submit the same size, larger than the recommendation by more than
rounding (one percent or two pixels, whichever is larger), disarms on any
change of either size, and re-adopts at the same boundary when the new
size still qualifies, so a resolution change costs no untreated frame
(`src/common/supersample_math.h` holds the state machine and the kernel
maths, header-only, so a test drives them without a headset — the
guard's changed-size adoption discipline, mirrored). The d3d11 half
filters (`edvrSupersampleResolve`, `src/d3d11/supersample_pass.cpp`):
two compute dispatches per eye, horizontal then vertical through a
float16 intermediate, the taps clamped inside the eye's own region so
the other eye's half of the double-wide texture and any guard margin are
never read, gamma content decoded to linear light around the filter
(what the compositor's own sRGB view does — the same operation with a
better kernel, not a different one) and re-encoded after, into an
EDVR-owned texture of the recommended size in the source's own format,
submitted with full bounds in the original orientation. The treatment
runs on every forwarding path of `hookedSubmit` — the game's frame, the
guard's crop, the theater's rendering, the withhold's shadow — after the
guard's crop where one runs: crop first, resolve second, sequentially;
the fused dispatch is noted in the code as future work. Any refusal — a
one-file install (the theater's "mismatched pair?" voice), an unlisted or
sRGB-typed format, an MSAA or array source, a failed compile or view, a
spent fault budget — stands the resolve down for the session with one
line and forwards the game's own frame. The sharpen that seam waited for
is built (2026-09-03, after feature B's first flight): `render_sharpness`
runs AMD's RCAS as the last pass at the door, `src/d3d11/sharpen_pass.cpp`
behind `edvrSharpen`, on whatever the passes before it produced, its taps
clamped inside the eye's region the way the resolve's are. Pinned at the
desk by `tools/supersample_test` (both
kernels' weights sum to one on every output pixel, a flat image stays
flat at every width and ratio, taps never leave the region, the textbook
Mitchell values at width 2, the arm/disarm verdict frame by frame
including the double-wide and flipped bounds) and by `tools/smoke`,
which runs the pass on a real device against a double-wide source and
asserts no bleed across the seam.

What the log says, and which Phase 0 item each line answers:

- `supersample resolve: engaged -- each eye arrives at WxH against the
  runtime's recommended WxH (…x horizontal, …x vertical …)`, in the vr
  log: the detected ratio, the kernel and its radius.
- `supersample resolve: first resolved frame -- the game submits <format>
  (DXGI_FORMAT n) …`, in the graphics log: Phase 0 item 2, the submitted
  format and whether it is read as gamma.
- `supersample resolve: measured … ms per eye on average (max …)`, after
  120 timed passes, and the periodic `supersample resolve totals:` line:
  Phase 0 item 6's price half, by timestamp query.
- `supersample resolve STANDING DOWN: …` or `… stands down`, anywhere: a
  refusal, with its reason.

**First flight (2026-09-02, Pimax Crystal Super over native SteamVR,
`on`, the cull guard live at h 0.35).** Three things measured, one of
them a bug:

- Elite's HMD Quality 1.25 submits 6780x6695 per eye against the runtime's
  recommended 5424x5356: exactly 1.25x on each axis (the belief in the
  mechanism paragraph above, now measured). The game rebuilds its targets
  the moment the setting is applied, mid-session, at 1.25x the size it is
  currently being told.
- The resolve armed on the wrong supersampling. The cull guard's stage 1
  asks the game for 7 % wider targets and does not crop them yet, so for
  two seconds every eye arrived at 5792x5356; the resolve saw "1.07x",
  engaged, and the next frame the guard's crop delivered exactly the
  recommended size — which the pass refused as a 1:1 resolve, and the
  door read the refusal as a stand-down for the session. Fixed the same
  day: reports made while the guard is staging are withheld from the
  arming decision, and a region that no longer exceeds the target by more
  than rounding forwards untouched (the boundary disarms) instead of
  standing anything down. A resolve armed before the guard stages keeps
  treating through stage 1, where the game renders true projections into
  the wider targets, so the whole region is the true view at a higher
  density and shrinking it is right.
- Changing HMD Quality mid-session takes the cull guard down: its crop
  snaps to the size the session adopted, the rebuilt textures are 25 %
  off it, and the guard refuses and stands down as designed — silently,
  until the same day's fix gave that refusal its log line. Set HMD Quality
  before launching and the guard stages against the supersampled size
  from the start (stage 1 at 1.25x1.07, the crop at 6780x6695, the resolve
  from there to 5424x5356).

The pass itself compiled and warmed one second into the session and
linked to the door; it never treated a frame on this flight, so the
format and price lines were still unread.

**Second flight (2026-09-02 20:21, same rig, same settings, HMD Quality
set before launch).** The resolve engaged two seconds into the session,
at the menu, once the game had rebuilt its targets to 6780x6695 against
the recommended 5424x5356 (1.25x horizontal, 1.25x vertical), and stayed
engaged: 868 eye-submits resolved in the first forty seconds. Measured:

- **Phase 0 item 2, the format:** the game submits `R8G8B8A8_TYPELESS`
  (DXGI_FORMAT 27), read and written through `R8G8B8A8_UNORM` views, in
  linear light — the same format the render-scale work measured on the
  Quest 3, so both field rigs agree.
- **Phase 0 item 6, the price:** 0.48 ms per eye on average (max 0.80)
  resolving 6780x6695 to 5424x5356 with the calm kernel at radius 1.0 —
  the two dispatches over a 45-megapixel eye. "Well under a millisecond"
  was the belief; half of one is the number.
- **The cull guard, at HMD Quality above 1.0, had a bug of its own.** Its
  stage 1 adopted after 77 ms, against the not-yet-rebuilt 6780-wide
  targets: the pre-stage sizes it requires a change from were zero at a
  session's first staging (the probe only runs during stage 1), zero had
  "moved" to 6780, and 6780 cleared the 0.97 x 5792 threshold that the
  native 5424 never had. The guard froze a canonical of 6349 the game was
  about to abandon, cropped to it for four seconds (a true-FOV image at a
  slightly lower density, geometrically right), and stood down when the
  real 7240-wide targets arrived — now with its own log line. Fixed the
  same day: the pre-stage sizes are seeded by the first submissions after
  stage 1 begins, which are the sizes the game was submitting before it
  rebuilt (it takes about two seconds to do so, measured). The resolve
  followed every one of those sizes faithfully — 6349, 7240, 6780,
  re-adopted at each boundary without a stand-down — which is the
  composition working; the guard just gave it the wrong sizes.

**Third and fourth flights (2026-09-03).** The third, fifteen seconds at
the menu with the servers down, repeated the second's numbers. The fourth
was the Quest 3 over Virtual Desktop at HMD Quality 1.5, four and a half
minutes in a cockpit with the cull guard on: engaged at 3096x3312 against
a recommended 2064x2208 (one texture per eye on that route), 0.09 ms per
eye on average (max 0.48), every frame at a steady 90 fps; the guard's
stage 1 adopted after 1.8 seconds against the rebuilt 3358-wide targets
and its crop landed at 3095x3312 — the seeding fix verified, and the
composed steady state the design describes; eleven transition-flash
withholds went through crop-then-resolve on the shadow path without a
line. One cosmetic finding: the crop's canonical rounds a pixel under the
pre-guard size and the armer re-adopted on it, so the armer now treats two
pixels as rounding, as `supersampleExceeds` does. With the engage line,
the ratio and the price confirmed on both rigs, `auto` became the shipped
default the same day. Still unflown: the double-wide texture with per-eye
bounds (the Steam Link route, desk-verified in the GPU harness) and the
theater path.

## Feature B — temporal anti-aliasing at the door

**What it is.** Each frame, the eye image is blended with a history of the
frames before it, each reprojected to where its content sits now. Content
that flickers on and off the sample grid is averaged across frames into a
stable value; edges converge toward their true coverage. This is the
technique every modern engine uses against exactly this complaint, and the
one Elite lacks. It is also the technique with a reputation — softness,
ghosting — and the design's job is to earn the first without paying the
second.

**Where it runs.** At submit, on the game's finished LDR eye image, before
any other treatment (crop, resolve, theater, heal): the first line of the
door lambda, so every path is treated once. Running inside the game's post
chain — between tonemap and HUD, where an engine would put it — would mean
hooking passes by hash and is not needed: filtering after the HUD is a
feature here, because the HUD's hairlines are among the worst offenders.

**The pass, per eye.** For each output pixel: read this frame's depth,
reconstruct the view-space position through this frame's projection,
transform by the camera's motion since last frame, project through last
frame's projection, and sample the history there (Catmull-Rom, so the
history does not blur under repeated resampling). Clamp the sampled
history to the current frame's 3×3 neighbourhood in YCoCg (variance
clipping — the standard bound on ghosting: history that disagrees with
everything around the pixel is pulled to the nearest agreeing value).
Blend with a weight, write the result as both the output and the new
history. A reprojection that lands off the image, or across a depth
discontinuity, takes the current pixel unblended.

**The camera motion, in two grades:**

1. *v1 — head motion only.* The head pose at `WaitGetPoses` composed with
   the eye-to-head transform, this frame against last. Exact for the
   cockpit, which moves only with the head relative to the camera; wrong
   for the world outside the canopy by whatever the *ship* did in one
   frame. Distant content barely moves under ship translation, but ship
   rotation shifts the whole view — a roll at 60°/s is about ten pixels a
   frame at 90 Hz at the field Quest 3's measured eye size — and the clamp
   then rejects most of the history. The pass degrades to no anti-aliasing
   during fast turns, which is when aliasing is hardest to see, and works
   fully when the ship is steady, which is when the shimmer is worst.
   Acceptable for a first flight; not the end state.
2. *v2 — the game's camera.* The flash detector already reads the game's
   camera buffer every frame; if it carries the view matrix (Phase 0
   reads it), the reprojection is exact for every static thing in the
   world — planets, stations, terrain, the cockpit alike — and only
   independently moving objects (other ships, NPCs, particles) are left
   to the clamp. That is the state every injected TAA lives in, and the
   clamp is what makes it presentable.

**The jitter, which is what makes this anti-aliasing rather than
smoothing.** Without it, the history holds only the sample positions the
head's own motion happened to visit: real integration while the head
moves, nothing new while it is held still — flicker freezes rather than
resolves, which is tolerable but not the goal. With it, each frame's
projection is shifted by a sub-pixel offset from a low-discrepancy
sequence (Halton 2,3, the conventional choice), so consecutive frames
sample different positions inside every pixel by construction, and the
history converges to a genuine supersample of the scene even at rest. The
mechanism is the cull guard's, with a different number: the raw tangents
shifted by `jx·(r−l)/width` horizontally (one pixel is `(r−l)/width` in
tangent units) and the vertical equivalent, the matrix elements following,
the offset advanced at the frame boundary and held for the whole frame —
the consistency rule `system_hook.cpp` already enforces, unchanged. The
compositor never sees the jitter as a wobble: the pass blends each frame
*as rendered* with a history that sits on the unjittered grid, so what is
submitted is nine parts settled history to one part of a sample under
half a pixel off, the convention since Karis 2014. (v1 resampled the
current frame onto the grid instead, and the first flight found text a
little fuzzy: a bilinear resample at a half-pixel offset is a half-pixel
tent every frame, and the history converged to that blur. Measured
2026-09-03, Quest 3, HMD Quality 1.5.)

**Interactions, each decided here:**

- **The game's own AA must be off.** SMAA and FXAA soften sub-pixel detail
  the temporal filter would otherwise resolve, and stacking two softeners
  is the blur everyone dislikes about TAA. EDVR cannot read the game's
  setting, but it can recognise the setting's *passes*: SMAA is three
  distinctive full-screen draws (edge detection, blend weights,
  neighbourhood blend), FXAA one. Phase 0 collects their hashes; the pass
  then says once in the log "Elite's anti-aliasing appears to be on —
  turn it off for this feature", which is the whole of the player's
  "turn off AA in game" step, verified rather than trusted.
- **The transition-flash withhold.** A withheld frame breaks continuity;
  the history resets on the withhold and rebuilds over the next few
  frames. One frame of extra aliasing where the player was about to see a
  lurch is not a regression.
- **The cull guard.** Jitter is applied to the lied frustum, in the
  lied image's pixel units; the pass runs on the full wide image before
  the crop, so the history has margin at the edges and reprojection near
  the border lands on content instead of off the image. An unlooked-for
  benefit of rendering the margin.
- **`render_scale` below 1.0.** TAA at the render size, then EASU up: the
  filter sees the pixels that were rendered and the upscaler sees a calm
  input. (A fused temporal upscale — what FSR 2 and DLSS are — is
  declined below.)
- **The on-foot flat screen.** First person on foot is a texture on a
  panel; the eye projection's jitter moves the panel by a sub-pixel, not
  the content rendered into it, so on foot the pass smooths without
  integrating. The on-foot picture's own sharpness is `vscreen_res`, the
  existing fix, and the pass does not change that.
- **Elements that do not go through the projection.** A full-screen quad
  drawn in clip space does not jitter with the scene: its sample is the
  same every frame while the history around it integrates a jittered
  scene, so it neither wobbles nor supersamples — it is smoothed only by
  the head's motion. The census names which draws are which, and Phase
  0's jitter A/B is where it is looked for. The sun-glare draws EDVR
  already re-authors are the likeliest members of this class.
- **Two eyes.** Separate histories, filtered identically; each eye's
  output is a deterministic function of its own inputs, so the only
  binocular differences the pass can produce are the ones already in the
  content. The project's rule that two eyes must never disagree is
  respected by construction, and the SubmitPairLatch logic is untouched.
- **The runtime's reprojection.** Motion smoothing and ASW act on the
  submitted frames; they neither see nor care that those frames were
  temporally filtered.

**Cost.** Per eye: one compute dispatch reading the frame, the depth and
the history (a handful of taps per pixel) and writing two textures; two
history textures and one depth copy per eye resident. Believed a fraction
of a millisecond per eye at Quest-3 sizes; measured by timestamp query in
Phase 0 and printed in the log and, once it exists, the monitor. The
jitter costs nothing. The depth copy is the one price that depends on the
game: if the depth target is readable as a shader resource it is free, if
not it is one `CopyResource` per eye.

**Fail-safes.** Both features need both files, like the theater: the door
is in `openvr_api.dll`, the passes are exports of `d3d11.dll` (the
cross-DLL pattern), and the jitter is the system hook's. An install with
one file stands down and says so in the theater's "mismatched pair?"
voice. Any refused format, any failed compile, any depth target the
classifier cannot name: the pass stands down for the session, says why
once, and every frame forwards as today. A jitter with the pass down is a
frame drawn a sub-pixel off — invisible, but pointless — so the jitter
arms only after the pass has treated a frame, and disarms with it.

**Settings** (`[fix]`, as built):

```
temporal_aa        = off   ; off | on. On needs a restart (the projection
                           ; edit installs at launch); the rest are live.
temporal_aa_jitter = on    ; the sub-pixel projection jitter. Off =
                           ; smoothing only.
temporal_aa_blend  = 0.90  ; history weight, 0.50..0.95: higher is calmer
                           ; and slower to change. Live.
temporal_aa_clamp  = 1.0   ; how far history may stray from the current
                           ; neighbourhood, in standard deviations: lower
                           ; means less ghosting and more flicker. Live.
render_sharpness   = 0.0   ; AMD's RCAS on every outgoing frame, the last
                           ; pass at the door, 0 (off) to 1: what the
                           ; history's resampling softens, handed back.
                           ; Start at 0.3 to 0.5. Live.
```

and under `[advanced]`, for the field's A/B: `temporal_aa_motion = head |
camera | none` and `temporal_aa_view_transpose`.

**What was built (2026-09-03: v1, off by default; flown once, below).** The
openvr half (`src/openvr/temporal_aa.cpp`) owns the jitter and the head:
each frame boundary advances a Halton (2,3) offset over eight frames and
tells the system hook the shift of every tangent it implies (`src/common/
temporal_math.h`, `temporalJitterToTangents`); the system hook applies it
in both the raw thunk and the matrix receiver, or in neither — the
receiver installs at launch when `temporal_aa` is on, exactly as it does
for the guard, and a session where it did not (the key turned on live, or
the runtime's matrix failing the tangent-formula check) runs the pass as a
smoother and says so once. The head pose at `WaitGetPoses`, before any
EDVR offset, is kept as a pair, and its rotation delta (`temporalHeadDelta`)
is handed to the pass per eye. The treatment runs first at the door, on
the game's own frame at render size, on the forward path only; the
theater's and heal's renderings and a withheld frame's shadow each mark
the history broken, and the next real frame starts it afresh. The d3d11
half (`edvrTemporalAa`, `src/d3d11/temporal_pass.cpp`) does the pass: per
pixel, this frame's pixel as rendered, the pixel's direction
through this frame's frustum rotated by the delta and projected through
last frame's (`temporalReproject`, transcribed), the history fetched there
with a nine-tap Catmull-Rom, clipped to the current 3x3 neighbourhood's
mean ± γσ in YCoCg, blended at the configured weight, and written as the
output (the game's own format) and the new history (R10G10B10A2, float16
where that cannot be stored to; two per eye). Rotation only, no depth: v1
as designed, with the game's camera as an experiment behind
`temporal_aa_motion = camera` — the view rows the sun-glare fix measured
at float 932 of the scene block, latched at each frame's first eye draw,
with `temporal_aa_view_transpose` for the reading the field has to settle.
The pass counts, per frame, the pixels whose history was rejected (off the
image or none yet) and clipped, and the graphics log's totals line prints
both as percentages beside the price: with the head turning and the ship
steady, a correct reprojection keeps both low, which is the instrument
Phase 0 item 5 and the camera question both read. Desk-verified:
`tools/temporal_test` pins the sequence, the shift's sign, the mapping on
the Quest 3's frustum, both deltas and the reprojection against a known
turn; `tools/smoke` runs the pass on a real device.

**Flown (2026-09-03, Quest 3 over Virtual Desktop at HMD Quality 1.5,
Elite's SMAA off, motion from the head, blend 0.90, clamp 1.0).** The
pass engaged on the first forwarded frame and ran the session at 0.20 ms
per eye at 3096x3312 (max 0.65), with the resolve after it at 0.09; the
history was rejected for 0.5–0.7% of pixels and clipped for 17–19%, the
head turning in a cockpit — the reprojection by the head is right, which
was the question. The jitter went live a few frames after the pass
configured, once the game's first `GetProjectionMatrix` per eye had
passed the tangent-formula check; the first build asked once, was told
"not yet", and printed that the session could not be jittered, then
jittered anyway (that line now waits for a verdict, and a "jitter is
live" line says when). The one complaint: text "a little fuzzy". Two
causes, both answered the same day. The first build resampled the
current frame bilinearly onto the unjittered grid, a half-pixel tent
every frame that the history converged to; the frame is now blended as
rendered (the design text above). And every temporal filter softens by
resampling its history each frame, which is what the sharpen at the door
is for: `render_sharpness`, the seam feature A had marked, built now.

**Flown again the same day (the second build, sharpen at 0.4 then 0.8
live).** Jitter live from the first frame, 0.21 ms per eye, the sharpen
0.04 ms. The world was right and the cockpit was not: text ghosted and
jittered when the head moved, and stayed a little soft under a sharpen
strong enough to ring, while the clip share rose to 24% of pixels. That
is what a history which does not land on NEAR content looks like, and
rotation-only cannot land it: the head's translation, which v1 ignores,
moves a panel at 0.6 m by about two pixels a frame during a 30°/s turn at
Quest 3 densities (24 px/deg at the centre of a 3096-wide eye), and by a
fraction of a pixel from tracking noise alone while the world at infinity
does not move at all. A misregistered history is a soft one, which no
sharpen recovers; and where the clip rejects it, the raw jittered frame
shows through with its half-pixel wobble. Two other explanations were
worth a measurement before building on depth: a pipelined renderer that
lands each frame a pose late, and the game's own camera rows being the
better source. So the third build carries the instruments, not a fix:
every treated frame also judges four candidate deltas by the same clip
(the head's as used, the head's one frame earlier, the camera rows as
world→view, the same rows transposed) and prints their clip shares, each
with the mean size of its clips in luma so a nudge on a text edge and a
misplaced history read apart, beside the used delta's split by head speed
(still, slow, fast) as a `temporal aa registration` line; and a depth probe (Phase 0 item 3,
`src/d3d11/depth_probe.cpp`) reports which texture the eye draws bind as
their depth target, its format and bind flags, whether a shader view can
be made over it or it must be copied, which eye's it is by the order they
are bound, and a 16x16 grid of its values read at the moment the game
clears it, with the clear value that says standard or reversed. **v2's
shape, from this:** per-pixel depth turns the head's full pose delta
(rotation and translation, per eye) into an exact reprojection of the
cockpit, and the game's camera delta into the world's; the two are told
apart by depth (the cockpit is within a few metres), which is also the
first thing the probe measures.

## Feature C — texture LOD bias, the small lever

Not all shimmer is geometry. Detail maps and normal maps sampled a mip
level too sharp for the pixel density alias on their own — terrain at
altitude and hull panelling at a distance are the usual places — and the
cure is a fraction of a mip level of bias toward the softer level. D3D11
samplers carry this as `MipLODBias`, set once when the sampler is created.

**Mechanism.** Hook `CreateSamplerState` on the device (slot 23, to be
SDK-verified the way `CreateTexture2D` was) and add the configured bias
to every sampler's own. The game's anisotropic setting is left alone. A
positive bias trades texture sharpness for calm; a negative one is what
upscaling tools apply to *restore* detail at reduced render sizes, and
`render_scale` below 1.0 may want exactly that — one setting, both signs.

Samplers are created early, so v1 takes effect at restart. Live tuning
would mean substituting biased clones at bind time — one more context
slot on the hot path, which this project spends reluctantly — and waits on
the field wanting it.

**Settings sketch** (`[fix]`):

```
texture_lod_bias = 0.0   ; -1.0..+1.0 added to every sampler's mip bias.
                         ; +0.3 calms detail-map shimmer; negative
                         ; sharpens. Restart.
```

## Feature D — specular anti-aliasing in the lighting resolve

The sparkle on hulls and station skins is specular aliasing: a normal map's
sub-pixel structure produces highlights narrower than a pixel, and the
lighting samples them at one point. Engines treat it in the lighting
shader by widening the surface's roughness in proportion to how fast the
shading normal varies across the pixel (geometric specular anti-aliasing;
the screen-space derivative of the normal is the measure), or by a plain
roughness floor. Neither needs the temporal machinery, and each removes
the residual glint that even TAA leaves during motion.

EDVR has swapped the deferred lighting resolve's pixel shader before
(`resolve_probe.cpp`, hash `7CECABDE34FFBE9E`, for one draw, put back
after), so the mechanism exists. The cost is the replacement: the probe
emitted a constant, while this would be the game's whole lighting shader
transcribed from disassembly with one branch added — the largest
transcription this project would have attempted, and it covers only
what that one resolve lights (the scanner and any forward-lit surfaces
are separate passes; Phase 0 names them). Listed here because it is the
one lever that reaches the *cause* of the specular shimmer rather than its
appearance, and deliberately last: it is justified only by residual
sparkle measured after feature B, not before.

## The field's proposal: motion vectors from the game's own matrices

The proposal is correct in every particular, and it is feature B's v2. Where
this design differs from the ReShade-addon framing it arrived in is in how
much of the work is already done, and in one thing an addon cannot do at
all:

- **The camera is already read.** The flash detector reads the game's
  camera buffer every frame, recognised by size and validated by the shape
  of its contents (`src/d3d11/glitch_frame.h`) — no RenderDoc session, no
  shader-layout reverse engineering, and it is the read that has already
  survived every game update this project re-verifies per build
  (`docs/build-332753.md` is the procedure). Feature B's exposure to
  updates is the flash fix's, which the project carries today. Whether
  that buffer holds the full view matrix or only the position is Phase 0
  item 4; if the latter, the search is one census with the constant-buffer
  watch (`advanced.census_cb_watch`) pointed at the scene's vertex shaders.
- **The projection is not reverse-engineered; EDVR hands it to the game.**
  Each eye's projection comes from the system hook's own answers, per
  frame, exactly — including the jitter EDVR added. And the submit hook
  names the eye. Neither is knowable from inside a post-process injection,
  which sees one texture and must guess both.
- **The depth buffer is named, not guessed.** The census classifier ties
  the depth view to the eye draws it serves; a generic depth heuristic
  picking the on-foot screen's depth, or the scanner's, is the kind of
  failure that instrument exists to exclude.
- **The jitter.** No post-process injection can move the game's sample
  positions. EDVR can, through the answers it already edits, and that is
  the difference between temporally *smoothing* the shimmer and
  anti-aliasing it: without new sample positions the history converges to
  the same aliased image the game drew; with them it converges to a
  supersample of the scene.

**Per-object matrices, declined on purpose.** The proposal's caveat is
right about the cost and the fragility, and the design's answer is that
they are not needed. Unmatched motion is what the neighbourhood clamp is
for: history that disagrees with the pixels around it is bounded to them,
so an object the reprojection missed shows a frame or two of its own
aliasing rather than a trail. Elite's moving population is also kind to
this: a station ring turns slowly in pixels per frame, planets slower
still, and other ships are small in the frame at any range where their
edges could shimmer. If v2 leaves visible ghosting on a passing ship, the
next step is a reactive weight — blending less where the clamp is doing
the most work, which the pass can compute for itself — not a per-draw
matrix capture and an object-ID pass, which is the engine's own velocity
buffer built from outside and re-built after every update.

**Performantly: yes.** The pass is one compute dispatch per eye over the
eye image, a handful of taps per pixel; believed a fraction of a
millisecond per eye at Quest-3 sizes, measured by timestamp query in Phase
0 and printed. That is cheaper than any supersampling players already run
against the same shimmer, and the two compose — a calmer image at a lower
HMD Quality is the trade this feature exists to offer.

**"Elite-specific" is the frame anatomy, not the filter.** The temporal
pass is the industry's; what makes it Elite's is everything this document
decides around it: filtering after the HUD so the hairlines are treated,
resetting on the flash withhold, running wide under the cull guard, the
on-foot panel's behaviour, the theater and heal paths, the game's own AA
detected by its passes, and feature D for the specular sparkle that even
a temporal pass leaves during motion. That is the "super-tuned" part, and
it is the part only something living where EDVR lives can do.

## What the plumbing opens: DLSS, and FSR 2

The question follows naturally: with a jittered render, a named depth
buffer and camera-derived motion vectors in hand, is DLSS within reach?
Yes. Those three, plus the jitter offsets and the projection, are the
input contract of DLSS Super Resolution, and DLAA is the same feature at a
render scale of 1.0. NGX exposes it to D3D11 applications through the
`NVSDK_NGX_D3D11_*` entry points (vendor-stated, the SDK's headers), so
nothing about Elite being D3D11 stands in the way. What the plumbing does
not change:

- **NVIDIA RTX only.** Turing and later — the same slice performance.md's
  foveation serves; a capability check at init and one log line where it
  cannot run.
- **The motion vectors are still camera-derived.** DLSS is trained on true
  per-pixel vectors and offers no clamp of EDVR's to tune: where the
  vectors are wrong — a passing ship, an NPC — it ghosts in its own way,
  and the remedy is the injection mods' remedy, a reactive mask or living
  with it. Feature B's own pass keeps that trade in EDVR's hands, which is
  why it comes first.
- **The input is post-HUD LDR.** The programming guide wants DLSS before
  UI compositing and prefers linear HDR input (vendor-stated); the door
  hands it a tonemapped frame with the HUD already on it. That is how every
  injected DLSS runs, and it works, out of spec — a Phase 0 look at the
  HUD's text is the price of knowing how well.
- **No frame generation.** It is D3D12 and Vulkan only (vendor-stated),
  and it is not wanted in a headset anyway, where the runtime already
  reprojects and latency is the thing being protected.
- **Licensing.** The SDK's binaries ship under NVIDIA's RTX SDKs licence,
  believed to permit redistribution inside an application on its terms;
  the licence text decides, and it would travel alongside the way
  `src/d3d11/fsr/README.md` carries AMD's and performance.md proposes for
  NVAPI's.

**What it would be worth.** The prize is not the anti-aliasing — feature B
is that, on every vendor — but performance.md's feature 1: at a render
scale of 0.67–0.75 a temporal reconstruction is in a different class from
FSR 1's spatial one, and DLSS is the best of them. On RTX hardware it
would become the preferred engine behind `render_scale`, with DLAA the
preferred engine at 1.0, both behind the same door and the same settings.
FSR 2 (MIT, with a D3D11 backend believed from 2.2) is the every-vendor
engine of the same shape — a step behind in quality, a step ahead in
tunability — and rides identical plumbing.

**The order, therefore.** Feature B's own pass first: all vendors, the
clamp in hand, the smallest change to measure the plumbing against. Then,
with Phase 0 items 3–5 answered (depth, camera, jitter — the same three
measurements DLSS needs), DLSS as the RTX engine behind the door and FSR 2
as the every-vendor one, each an engine choice on the existing setting
(`temporal_aa = edvr | dlss | fsr2`) rather than a new feature. Cost
believed about a millisecond per eye at Quest-3 sizes for DLSS, measured
before it ships, and repaid many times over by the pixels not rendered
when it is used as the upscaler.

## What it does for foveated rendering

Asked alongside the DLSS question: does any of this improve foveation?
Not the hardware side. performance.md's feature 2 is variable-rate
shading through NVAPI, NVIDIA-only on D3D11, and feature 3's gaze centre
is the runtime's own API; nothing here changes what either can reach. Two
things it does change:

- **It makes the rings pushable.** performance.md names VRS's failure
  mode itself: coarse shading of the deferred lighting and post shows as
  peripheral shimmer. A temporal pass at the door is the standard
  companion to VRS for exactly that reason — engines ship the two
  together, and the accumulation hides the coarser rate (believed; it is
  standard practice). With the jitter, the coarse region is even partly
  recovered over frames. Same look at coarser rates, or a calmer
  periphery at the same rates: better foveation on the hardware that has
  it, at no cost to the rings' design.
- **It opens an every-vendor foveation the performance doc declined for
  lack of a D3D11 VRS path.** A radial density mask — Valve's technique
  from The Lab, shipped for non-NVIDIA GPUs by vrperfkit (believed) —
  discards a checkerboard of peripheral pixels before shading, by a depth
  or stencil mask laid ahead of the game's draws, and fills them back at
  the door. From outside, the fill is a door pass of exactly feature A's
  shape; and with the mask alternating each frame, feature B's history
  fills the holes temporally instead of spatially — checkerboard rendering
  of the periphery, reconstructed by the pass that already exists. Two
  honest caveats: the savings on a deferred renderer are smaller than
  VRS's, since a full-screen lighting resolve skips only where the mask
  reaches its stencil (believed; vrperfkit's field experience says as
  much), and laying the mask means a per-draw depth-stencil intervention
  on the hot path, which this project spends reluctantly. A Phase 0
  question — the census can name the passes the mask would have to reach —
  not a promise.

The eye-tracked centre is unchanged. A foveated *resolve* — full-quality
temporal filtering at the fovea, cheaper in the periphery — is a small
saving the same gaze point could drive; noted, not designed.

## Considered and declined

- **A ReShade preset instead.** ReShade has applied effects in the
  headset since 5.0 — a separate VR effect runtime, configured from the
  SteamVR dashboard (vendor-stated, the 5.0 release notes) — and it runs
  alongside EDVR (README). What it can do today: sharpening, and temporal
  effects driven by *estimated* motion, the optical-flow helpers of the
  Launchpad class computing per-pixel motion from colour and depth
  (believed, from their documentation). What it cannot do: jitter the
  projection, know each eye's true projection, or read the game's camera;
  its depth is found by heuristic. A ReShade anti-shimmer is therefore
  feature B's v1 with estimated motion and no jitter — a smoother whose
  ghosting-versus-flicker trade is set by the estimate's quality. Players
  who want to try one today can; the case for building it into EDVR is the
  jitter and the truths EDVR already holds. (For the record, of the two
  injection families the proposal cites: OptiScaler swaps one upscaler for
  another in games that already feed one, and the from-scratch matrix
  extraction is the per-game work of the PureDark-style mods — believed,
  from their own compatibility notes.)
- **A post-process AA pass of EDVR's own** (SMAA, CMAA2, FXAA — all
  permissively licensed and small). It would duplicate what the game's
  menu already offers, and the argument above is that the offering is the
  wrong kind, not a poor instance of the right one. Its one plausible role
  is as the spatial half of feature B (SMAA T2x's shape), which is a
  tuning question for after B exists.
- **A vendored temporal upscaler as the starting point** — DLSS or FSR 2
  before EDVR's own pass. Declined for the reasons in the section above:
  the plumbing is measured against the pass whose every knob is EDVR's,
  and the engines follow it.
- **MSAA for the HUD only.** Above; feasible, expensive, subsumed.
- **Driver-level overrides.** Do nothing on this renderer, as above; the
  guidance says so rather than sending players to try them.
- **Lying about the size to the runtime.** Everything here edits the
  game's answers and submits at the size the runtime asked for, the
  guard's posture; the runtime never learns anything changed.

## The order at the door

With performance.md's features, the treatment inside the one submit lambda
is, in order: **temporal AA** (on the game's texture, at render size, wide
if the guard is live) → **crop** (the guard) → **scale** (EASU up for
`render_scale` below 1.0; the supersample resolve down for above it) →
**sharpen** (RCAS) → **menu, toasts, monitor** composited onto the
native-size outgoing frame. Fusions come later, in the order their edge-tap
questions are answered; v1 may run them sequentially, since each already
exists or is one dispatch. Built so far (2026-09-03): temporal AA, the
crop, the supersample resolve and the sharpen, sequential, each its own
pass on the texture the one before produced; the fusions are noted in the
code as future work.

## Guidance for players now

Feature A ships (`auto` by default since 2026-09-03); the rest does not.
Some of the shimmer has answers today, and the README and `edvr.ini` have
carried them since the same date:

- **Supersample through HMD Quality, not Elite's Supersampling slider**,
  in VR. The first hands the compositor a bigger frame to filter down as
  part of its distortion pass; the second has the game filter down before
  its post chain sees the result (believed, and it is the field's settled
  advice). Feature A, switched on, makes the choice moot; with it off the
  compositor's filter is the better one.
- **On SteamVR, turn on Advanced Supersample Filtering** (in the
  advanced video settings). It is the better downsample, and by its own
  description it is only noticeable at high supersampling — which is
  where VR players of this game already are.
- **Set Elite's anti-aliasing to Off or SMAA, and stop expecting it to
  address flicker.** It costs nearly nothing either way; it is not the
  lever.
- **Anisotropic filtering at 16x**, in the game's own menu, for surfaces
  at grazing angles — planet terrain at altitude especially.

## Phase 0 — what must be measured, not assumed

1. **The menu options, under the census.** One flight per AA option,
   watching for `SampleDesc.Count > 1` at creation and `V` lines in a
   census: turns "believed post-process" into measured, and collects the
   SMAA/FXAA pass hashes feature B's warning line needs.
2. **The eye-texture formats** (shared with performance.md's item 1): the
   exact formats submitted, sRGB or not, for the resolve's and the TAA's
   views. *Measured 2026-09-02 by the resolve's `first resolved frame`
   line: R8G8B8A8_TYPELESS (DXGI_FORMAT 27) on the Pimax Crystal Super
   over native SteamVR, submitted as gamma content; the render-scale work
   measured the same format on the Quest 3 over Virtual Desktop the same
   day. Both rigs agree; the sRGB-typed variants have not been seen.*
3. **The depth target.** Its format, whether it is created with
   shader-resource binding, and that the classifier names the same target
   the eye draws bind — one census with `d=` against one `CreateTexture2D`
   log.
4. **The camera buffer's contents.** Whether the buffer the flash detector
   reads carries the full view (or view-projection) matrix per eye, which
   decides whether v2 of feature B's reprojection is a read or a search.
5. **Jitter through the lie path.** That the game renders through the
   shifted projection the same frame it is told it (the ~1080/s query rate
   suggests per-frame reads, but the frame the values land in is a
   measurement), and which draws do not follow it — the un-jittered
   full-screen class, found by a jitter A/B with the census running.
6. **Price and softness.** Timestamp queries for both passes on both rigs;
   and a held-view comparison — the tile-difference method
   `docs/eye-brightness.md` used — of native, supersampled-bilinear,
   supersampled-resolved, and TAA, so "sharper" and "calmer" are numbers.
   *The resolve's price is measured: 0.48 ms per eye on average, max
   0.80, resolving 6780x6695 to 5424x5356 on the Pimax (2026-09-02), and
   0.09 ms per eye, max 0.48, resolving 3096x3312 to 2064x2208 on the
   Quest 3 (2026-09-03), calm kernel at radius 1.0 both times — timestamp
   queries around its two dispatches, printed after 120 samples and in the
   totals line. Softness is not, and the held-view comparison is still to
   be flown.*
7. **The HUD under TAA.** Text legibility with jitter on and off, and
   ghosting on a passing ship at the default clamp: the two failure modes
   that would decide the defaults.
8. **The resolve kernel's edge behaviour under the crop** —
   performance.md's item 2, which now has two consumers.
9. **Which passes feature D would need to cover**, if it is ever built:
   the census's list of lighting draws beyond the one resolve.
10. **NGX on the target GPUs**, when the engine choice is built: init and
    per-eye feature creation on D3D11, the flags for a post-HUD LDR input,
    the HUD's text under it, and the price — measured the way NVAPI's
    caps are in performance.md's item 4.
11. **The density mask's reach**, if every-vendor foveation is pursued:
    which passes a peripheral depth/stencil mask actually skips on this
    renderer (the census names them), the saving it buys against VRS's,
    and the temporal fill's look at the ring boundary.

## Phasing

1. **Feature A, passive** — **built 2026-09-02, field-verified on both
   rigs and `auto` by default from 2026-09-03**: the resolve at the door,
   arming itself when the game submits larger than asked, with the
   guidance text in the README and `edvr.ini`. Small, all vendors, and it
   built the door machinery every later pass uses.
2. **Feature C** — a day's work whenever a slot is free; it does not wait
   on anything.
3. **Feature B v1** (head-motion reprojection, with the jitter), then
   **v2** (the game's camera, once Phase 0 item 4 answers). This is the
   feature the ask is really about, and it goes second only because A's
   door and Phase 0's measurements make it a smaller, better-instrumented
   change.
4. **Feature A, active** — `render_scale` above 1.0 — once performance.md's
   feature 1 has the size-lie choreography in place for the direction
   below 1.0; the two share every line but the factor.
5. **Feature D**, if and only if residual specular sparkle is measured
   after 3.
6. **DLSS and FSR 2 as engines behind the door**, once 3's plumbing is
   measured — DLSS first, by the size of its win as the upscaler on RTX
   hardware, and as performance.md's feature 1 gains a temporal engine.

Each off by default, each with its own stand-down and its price in the
log. None of it reads or writes game memory or code: the passes treat
frames at the door, the jitter and the size are edited answers the guard
already edits, the LOD bias is a number added to a description on its way
to the driver. A player who wants none of it leaves the settings at their
defaults and runs a build identical in behaviour to today's.
