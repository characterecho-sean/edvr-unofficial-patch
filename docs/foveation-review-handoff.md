# Pre-ship review of the foveation branch: what was found, what was fixed, what is left

An adversarial review of `claude/foveation-gaze-probe-2ff1267` against `main`,
2026-09-06, before any of it ships. The branch is 45 commits and about 9,000
lines: the DLSS fovea crop (feature 6), the gaze probe and its eye-tracking
plan, the shading-rate feature (feature 2) with its eye-tracked centre, and
one change to the already-shipped temporal pass.

This file is the standalone record. It says what the review checked, what it
found, what has been fixed since, and what a later hand should pick up. The
findings' own reasoning is kept, because a finding without its trigger is a
rumour.

## The one that mattered most: no regression for existing users

`fix.temporal_aa` shipped in v0.14.0. This branch changes its two HLSL
shaders, so a defect there reaches commanders who never enable anything new.
The claim under review was that the change is exactly behaviour-preserving.

**Confirmed, and by more than reading.** The group is 8x8 = 64 threads and
`if (gi < 40) gCount[gi] = 0;` zero-fills all forty slots before the first
barrier, so every `gCount[gi]` read at the end is initialised; both
`GroupMemoryBarrierWithGroupSync` calls sit outside the bounds test, so
out-of-image threads still reach them, as before. `Stats` accumulates by
`InterlockedAdd`, and skipping an add whose addend is zero changes no sum.
The old `mv` wrote only counters 15, 16 and 17, and nothing read the rest;
`main`'s array and loop are untouched; the directly written indices are
unchanged. The reviewer extracted all six shader segments and compiled the
old and new `main` and `mv` with the Windows Kit's `fxc` at `cs_5_0`. The rest
of the shipped path with the fovea off was traced too: the constant buffer
grew from 416 to 448 bytes and is sized from the struct, the new fields
zero-initialise to the old behaviour, and the new blend floor is unreachable
because the caller clamps the blend above it.

One deliberate change to shipped behaviour, noted rather than faulted: the
pass now sets an explicit NGX render preset. DLAA's driver default is already
that preset, so DLAA users are unaffected, but DLSS Performance and
UltraPerformance users move from the driver's choice to it. That is intended
and priced on the desk; what it lacks is a fallback if an older DLSS runtime
refuses an explicit preset at create, in which case the trained path stands
down to the pass's own history for the session. **Left open** -- see below.

## Fixed since the review

**A static buffer walked past its end.** `snprintf` returns what it *would*
have written, so `len += snprintf(...)` moves the cursor beyond the buffer as
soon as one field truncates, and the next call gets a pointer past the end and
a size that wrapped through zero. Three builders did this, one of them a
summary line that grows with the number of render-target sizes a session has
seen -- and what follows those statics is the rate table NvAPI reads by
pointer. Every builder now appends through one small `Text` helper that
clamps.

**An image with undefined contents could be bound.** A mask slot was marked
used before its first fill, and that fill fails while the openvr half has
published no tangents to centre on. The first call returned null, but every
later call for that size found the slot and skipped the fill, so a texture
whose texels had never been written was handed to the driver -- and those
texels are read as shading rates, one of which is cull. A slot with no
successful fill is now refused until the boundary refill succeeds.

**The target table failed shut when full.** It holds 64 resources by address;
once full it returned nothing, which left every later target -- the submitted
ones included -- unattributed and therefore at full rate for the rest of the
session, silently. It now evicts the least recently drawn into, and the census
counts evictions.

**The gaze source armed for everyone.** `experimental.foveation_centre` defaults to
`eyes`, and the openvr half read only that key, so it requested the IVRSystem
function table, validated it and made three runtime calls a frame on every
rig, for commanders who had never turned foveated shading on -- and printed an
ARMED line promising summaries that would never come. It now reads
`experimental.foveation` as well and arms only when the feature is on.

**A blink moved the rings.** A gaze published as lost was honoured
immediately, while a silence needed ninety frames. A blink is a short run of
lost publishes, so the disc snapped to straight ahead and back, with two
refills, every time. A loss now has to persist as long as a silence does.

**A forced sample count now stands the feature down.** With the image bound,
a rasteriser state carrying one removed the device outright on the desk. Every
sampled eye draw in six flights reads zero, so Elite does not appear to use
it, but the guard costs nothing: the moment the census sees one, the feature
disables itself with a line.

**A feature that never armed said nothing.** With no openvr half installed, or
an older one, on a headset whose eye textures fall to the size guess, nothing
is ever recognised as an eye target and the feature sat in silence forever.
It now says so once, after 1800 frames.

**The fixation distance was taking the gaze with it.** Found in flight rather
than by the review, and the third instance on this branch of the same class:
`centreOf` returned early when the distance was zero, before the gaze was
added, so setting the distance to zero -- which means "fuse at infinity", the
right choice under eye tracking -- silently nailed the rings to straight
ahead. The distance now governs only the nasal shift. The census line that
hid it, which reported the gaze and said "following the eyes", now prints the
centre the masks were actually built on beside it.

## Left open, deliberately

- **No fallback if an older DLSS runtime refuses the explicit NGX preset.**
  Low: EDVR ships the runtime it was measured against. A retry without the
  preset hint at create would close it.
- **Resource addresses are permanent identity.** An entry is never invalidated
  when a texture is destroyed, and a submitted-texture verdict pins its eye
  forever. If the game recreates its eye targets and a new texture lands on a
  freed address, it inherits a dead one's eye, and nothing corrects it unless
  it is itself submitted. The trigger is a resolution or quality change
  mid-session. The fix is to clear the table when the eye size changes.
- **The pairing rule is a heuristic.** Each size's targets are assumed to
  alternate eyes in bind order from a rooted member. A group used an odd
  number of times per eye, or bound left, left, right, right, is misassigned,
  and only a submitted texture corrects it. It is right on every flight so
  far and it is stated as an assumption in the code.
- **Budget exhaustion can leave the image bound while the log says off.** The
  stand-down's own unbind runs under the same exhausted fault budget.
- **Masks survive a device recreation.** If Elite ever recreates its device,
  `UpdateSubresource` would be called on textures from the old one.
- **Canted displays.** The gaze is added as head-frame tangents without the
  eye's cant, which the temporal pass composes and this does not. Zero on
  parallel panels; the Crystal Super's are parallel.
- **Two full-frame copies remain in the temporal pass**, about 0.3 ms an eye
  together with the dispatch's real work. The copy in exists because the
  game's texture may be typeless where NVIDIA needs a typed view; the copy out
  because the compositor needs the game's format back to make an sRGB view,
  which telling Submit the colour space outright would also solve.

## What the review confirmed as correct

The frustum mirroring, the y-up tile rows, the nearest-point rate rule, the
nasal shift's sign against the runtime's own eye-to-head translation, the
head-frame gaze addition against the formula the sweep protocol derived, the
dead band, and the boundary-only refill. Config reload runs on the render
thread, so the draw path and configure cannot race. The gaze channel is
interlocked and cannot tear. A lost gaze, a removed headset and a resolution
change are handled. Both contract checkers pass at 203 keys.

## The rule this branch keeps teaching

Four separate faults here were the same mistake: **a log line that reports an
input instead of the state it produced**. A cull setting that echoed its key
while the rate table said otherwise. A census that named the gaze while the
masks ignored it. A summary that grew past the log's line limit and dropped
the evidence it was carrying. A fixation distance that silently disabled the
thing it was meant to tune. Each cost a flight, and each was printed in the
log the whole time, describing the setting rather than the effect.
