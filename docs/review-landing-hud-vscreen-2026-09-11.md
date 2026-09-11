# Landing HUD and on-foot screen, 2026-09-11

The Steam flight `edvr_gfx_20260911_123355.log` identifies
`0.15.1-9-g8727935`, graphics build `6AA444D7`, game build 332841. It
uses SteamVR and DLSS 310.7.0.0, preset K, 2268x2240 input and 4536x4480
output per eye. The on-foot source is 5120x2880. The user confirms
improved changing cockpit text and planet approach. There is no new
exit-profile capture.

## Evidence before changes

Eight paired runs span approach, landing and walking: 123854, 123857,
123936, 123958, 124027, 124132, 124242 and 124244. CSV/input-map frame
numbers are one less than the object ledger/drawstate numbering: draws
precede the boundary at which the object probe increments its counter.
For example, drawstate frame 34550 is CSV frame 34549 / C00, not C01.

Ruled out: UI source-cache failure, because this flight records zero
declined source comparisons and zero invalid GPU intervals. The last
active totals are 137965 comparisons and 603781 same-frame reuses. The
measured cumulative source-update cost is 22.755 microseconds each.

The target-circle atlas and pitch ladder both use sprite VS
E508648660A352B2 / PS 63ABD86359B57D01. This is a single texture sample
with alpha discard, without the holo material's extended glow. The
captured quad vertices lie on local Y=0. Unlike cockpit holograms, these
sprites do not participate in draw-transform motion tracking.

In 124132, surface 3 is the 742x742 pitch/altitude UI. Its geometry lies
at clip W 14.18--15.60, beyond the generic cockpit motion split. The
submitted vectors on its bright strokes range approximately -15 to +14
input pixels horizontally. Its captured next-frame geometry moves only
about (0.19, 0.09) pixels after jitter removal. Ship/world translation
at this depth is inappropriate for the ship-attached ladder.

The same capture's UI and UiEdits masks contain filled rectangles down
the two tick columns. In a 400x290 input-pixel ladder region, 1706 of
6514 edited pixels sample transparent current source pixels. The
previous change retained erased source strokes for 32 frames through the
alpha discard, causing cleared parts of a scrolling ladder to acquire UI
depth and fresh spatial reconstruction over terrain. Current source
alpha shows separate thin ticks, rather than those rectangles.

The distant target-circle quad also changes orientation in its captured
pool transform. Its four corners span clip W 54--61 million in 123936.
Head-only projection differs from captured draw motion by up to about
0.11 input pixels in the two available consecutive vertex pairs, even
after removing the correct frames' jitter. Its source alpha contains
antialiased edges below the old 0.5 coverage cutoff.

## On-foot path

Runs 124242 and 124244 have no scene/UI/hologram motion inputs. The only
eye draws are the virtual-screen composites, VS 5C36AF051B98B9F1 / PS
CFE84157BC76E921, sampling the 5120x2880 source. Census 8/9 include
1380/1370 offscreen draws and four eye draws over two frames, without
overflow or truncation. Current eye snapshots watch neither that VS nor
the offscreen source pipeline, so their zero draws are not an empty
scene.

DLSS currently processes the completed VR eyes, after the moving on-foot
world has been projected onto the screen. Headset reprojection does not
describe walking within that image. Scene camera acceptance also rejects
the on-foot camera because it does not follow the headset, as intended
for the outer eye camera. A source-scene DLSS/DLAA pass needs separate
on-foot camera/depth/history and source jitter, followed by correct
screen composition; feeding it headset vectors would reproduce the same
error.

Working analysis and original, unredistributed shader/capture replays
are under `build/review_motion/sep11/planet-landing` and its parent
directory.

## Changes and validation

The sprite composite now participates in the existing draw-transform
history. Its atlas UV tile is part of identity; brightness is not. The
captured local Y=0 plane recovers its physical clip W when a nearer
scene depth is retained under the intentionally depth-disabled sprite.
This avoids substituting foreground depth or the VS's forced Z=W into
motion reconstruction. The existing visibility check still rejects
coverage overwritten by later foreground geometry.

Sprite coverage retains faint current alpha down to one 8-bit step,
matching the direct screen path. Source edit age can no longer bypass
the sprite alpha discard. Existing post-resolve influence clears
departed text, while current changing digits still use fresh
reconstruction. The cockpit holo/menu/screen edit policies are
unchanged.

WARP replays pass 282 captured sprite transform pairs from six runs and
72 existing cockpit pairs, with a 0.015-input-pixel tolerance against
double-precision projection. Synthetic near/far sprite tests cover a
foreground depth different from the sprite plane, stale coverage,
changed brightness and different UV tiles. UI tests cover scrolling-tick
erasure, dim strokes, current changes and the existing dynamic-text
resolver.

On-foot rendering is not changed by this build. The next eye run also
captures one source camera per frame from the known world/terrain VS,
the screen VS/PS, its original draw constants/vertices, and source
colour plus completed scene depth at its first composite. Source records
use ordinal UINT32_MAX; depth surfaces carry their DSV format. The
existing snapshot layout remains version
3. The larger explicit on-foot capture has a 128 MiB per-surface / 256
   MiB
total limit; normal UI surface limits remain 16/64 MiB. There are no
source copies outside an armed eye run. The log reports camera, screen
and depth counts, including zero cameras or depth so a missed path
cannot look like a successful capture.

The next flight must verify target-circle/pitch-ladder stability,
absence of the two terrain bands and retained changing-text clarity. A
stationary then walking on-foot capture is needed to validate source
motion before implementing source DLSS/DLAA; the present dumps cannot
validate that path.

The full NVIDIA SDK build and all automatic gates pass, including 19784
UI checks, source-depth capture timing/deduplication, snapshot reader
round-trip and the 243-key configuration contract. The separate NVIDIA
smoke test passes native TAA, DLAA, DLSS, foveated paths and projection
conventions. These are desk validations; headset appearance and live
cost of the sprite tracking still require the next flight.

## Follow-up flight, 13:26

`edvr_gfx_20260911_132608.log` matches `439d9bf`, with the same SteamVR,
2268x2240 input, 4536x4480 output and preset K. Runs 132823 and 132827
are stationary and walking; 132914 and 132945 show the landing HUD.

Ruled out: sprite transform tracking alone fixes the HUD's low
resolution, because both HUD runs match all 16 eligible transforms while
the user still sees pixelation, also visible in the raw crops before
DLSS.

Both on-foot captures contain 19 source camera frames, 38 screen draws,
5120x2880 colour and completed D32/S8 scene depth, without failed
copies. The depth covers 58.7% of the source; the submitted eye depth is
zero everywhere. In the walking run the supplied eye vectors stay within
0.23 pixels. Reconstructing source positions with the captured camera
and depth, then projecting through the actual 64-segment curved screen,
predicts terrain image motion reaching 16 pixels over the sampled
frames. Registration of 123--124 textured patches has median residual
below 0.29 input pixels and 90th percentile below 0.96. The stationary
control has median residual below 0.32 pixels. This validates using the
source camera independently of the outer headset camera; source DLSS
itself would additionally need source projection jitter.

The pitch/altitude texture is 742x742. The spool-up animation is on the
1424x306 hologram surface. Both contain the corresponding graphics
before the eye composite. The source textures, source projection and the
post-resolve handling must be checked separately from motion; sharpening
the final eye cannot recover detail lost at source rasterization.

### Walking correction

The next build supplies source-scene motion to the existing eye TAA/DLSS
pass. It snapshots source camera constants on the first recognized
world/terrain draw and reads the completed source depth when the virtual
screen is drawn. GPU reprojection places the previous source UV on the
previous screen mesh, including its actual curvature segments, size,
distance transform and eye projection. The resulting eye-sized map
carries raster motion, source depth and validity; both temporal
consumers remove the raster jitter delta once. Pixels outside the screen
retain the existing eye motion. Source-image disocclusions reject
history, and missing/consecutive-history transitions reset temporal
accumulation.

There are two RGBA16F eye maps, approximately 77.5 MiB together at
2268x2240, plus small GPU constant/size buffers. Source depth is
retained by reference; normal frames do not copy the 5120x2880 colour or
depth and do not read them back to the CPU. Tracking wakes only after
the known screen composite is observed with temporal AA enabled, and
resources expire after 120 frames without it. Screen correction does not
modify cockpit HUD source rendering, add configuration keys or jitter
the source projection.

The WARP test exercises the production capture lifecycle and both
temporal shader consumers. It checks completed-depth timing, independent
eyes, missing history, region offsets, jitter convention, screen
boundaries and disocclusion. The source motion override also clears the
unrelated eye-space mover rejection. Captured-camera replay covers 1152
points from both eyes of the stationary and walking runs, checked
against double-precision curved-screen projection within 0.005 input
pixels. Desk image registration validates the source camera's
correspondence separately from that mathematical replay.

The NVIDIA shader-only benchmark is approximately 0.03 ms per eye at
2268x2240 with the captured 5120x2880 depth. This excludes clearing the
map, resource copies, CPU draw overhead and additional temporal input
bandwidth; it is not the measured cost of the complete in-game change.
Headset appearance and total flight cost remain to be verified.

### HUD source capture

The remaining ladder/altimeter, spool icon and terrain-behind-UI
complaints are not declared fixed. The existing dumps include completed
GUI textures and their eye composites but lack the original GUI drawing
inputs needed to validate a source-resolution change. Prior surface
inflation alone was inconclusive; there is no new arbitrary sharpening
or inflation setting in this build.

An explicit eye dump now adds `pool/gui_HHMMSS.bin`. It captures the
first matching source frame for the known GUI vector, glyph and icon
shader families on square/wide targets up to 2048 pixels. Records
include draw arguments, original input layout, vertex/index streams,
structured transform pool, constant buffers, atlas mip levels, samplers,
render state and initial colour/depth. Shader bytecode is saved beside
the dump. The reader is `tools/gui_draw_snapshot.py`; capture data is
never treated as instructions.

The capture is limited to 512 draws and 96 MiB, with 16 MiB per texture
including all mip levels. It runs only during an explicitly requested
eye capture. The log reports draws, declined ranges/formats/budgets,
failed copies or missing shaders, missing layouts and file-write status,
including zero-draw results. GPU/reader fixtures verify initial contents
before subsequent changes, original constants, layout, BC7 mip blocks
and first-frame selection. The next ladder/altimeter and engine-spool
dumps should permit direct replay of source quality and compositing
separately.

Source-scene DLSS/DLAA remains a separate step: it requires validated
source projection jitter and source-HUD treatment. This build corrects
missing walking motion in the existing eye reconstruction; it does not
introduce a second DLSS evaluation on the virtual-screen source.

The full SDK build and its regression gates pass, including 19785 UI
checks, 13601 screen-motion/consumer checks, both GPU snapshot fixtures,
their readers and the unchanged 243-key configuration contract. The
optional captured-camera run passes 18216 checks. NVIDIA smoke passes
TAA, DLAA, DLSS, foveated reconstruction and motion/jitter conventions.
These desk checks do not establish the final appearance of the next
headset flight.

## Follow-up flight, 14:36

`edvr_gfx_20260911_143626.log` matches `c7774fd`. The run uses SteamVR,
2268x2240 input, 4536x4480 output, preset K and a 5120x2880
virtual-screen source. Captures 143911/143933 show the flight HUD;
144027/144054/144106 are on foot. The user reports good walking scene
clarity and remaining target-box blur. The 144054 raw/DLSS pair visibly
separates crisp source nameplate letters from their doubled
reconstructed edges.

Ruled out: missing flight HUD transforms, because both flight captures
match all 12 eligible hologram/sprite records. The new GUI snapshots
contain 41 and 44 source draws without missing inputs, failed copies or
declined ranges. An offline WARP replay of all 41 original shader draws
reproduces the 742x742 source with 99.12% of pixels identical and
maximum channel difference 3/255. Replaying at twice the source
dimensions makes some vector edges finer but does not restore new detail
in the baked font atlas. That experiment does not establish an
improvement at the actual eye projection, so it does not justify a
source-size change.

Ruled out: a reciprocal-W error in the sprite coverage shader, because
the existing production GPU test supplies physical clip W and verifies
0.025/W against near and distant scene depths in all three depth
formats. Do not invert that expression based on a remembered semantic
convention.

The flight HUD's remaining vertical softness is not declared fixed. The
source replay and exact current sprite transform are available; mixed
foreground UI/background reconstruction and source raster quality remain
distinct questions. No sharpening or blanket fresh-frame policy is added
to the flight HUD.

### Source UI coverage

The on-foot captures have source-camera motion (ScreenMotion flag 32),
but no eye UI coverage. Final LDR source draws 932--938 in the 144054
census composite GUI canvases after the scene: B10B032BDFD46700 /
DB899F4BD577F2E5, C4B4B334B26E81A9 / 0146ABCC53240479 and
A888D51024D9798E / 015EF9349EC097E8. They use straight or premultiplied
alpha, depth disabled and unconditional stencil. Their pixels previously
inherited the scene depth and motion beneath the UI. The exact
individual nameplate draw is not established by the older source-camera
snapshot; the correction covers all three observed late GUI families.

For those shader pairs and verified render-state contracts, the original
draw is reissued to an R8 transparency target. The original shader,
discard, UVs and alpha are retained; zero source colour and inverse
source alpha multiply a target cleared to one. This unions visible GUI
coverage independently of its colour and premultiplication. No game
colour/depth is copied or modified. The mask is associated with the
exact source colour resource and frame, then sampled through the actual
screen geometry. ScreenMotion.w=3 identifies these pixels in the eye
dump.

Source UI uses the outer screen's motion and fresh current
reconstruction after DLSS, including the existing departing-UI influence
cleanup. Native TAA rejects history there. Source scene pixels retain
the previous motion path. This does not claim motion tracking of the
individual nameplate: fresh reconstruction also handles its changing
source-image position.

The mask costs about 14.1 MiB at this source size, one clear per frame
with matching GUI, and one reissue per matching draw (seven observed).
There is no CPU readback or full source colour copy in normal rendering.
The additional cost and visible nameplate result need flight validation.
The mask's frame and resource checks prevent absent or unrelated UI from
reusing stale coverage.

The full SDK build and all gates pass: 20439 UI checks, 32899 screen
motion/consumer checks, snapshot fixtures and the unchanged 243-key
configuration contract. Replaying the previous 1152 captured camera
points passes 37514 checks. Coverage tests verify actual shader alpha
and discard, opacity accumulation, premultiplied blends, all three
observed shader pairs, both eyes, frame expiry, resource identity,
original colour/depth/state preservation and exclusion of world draws.
Resolve tests include a cropped eye region and noninteger output sizes.
NVIDIA smoke passes TAA, DLAA, DLSS, foveated reconstruction and
motion/jitter conventions. Headset confirmation of the target-box fix is
still required.

A NVIDIA shader-only measurement at 2268x2240 is 0.032--0.043 ms per eye
with a synthetic source UI mask, versus 0.026--0.029 ms without the
mask. This measures screen reprojection, not the mask clear, original
GUI shader reissues, post-resolve work or total in-game overhead.
