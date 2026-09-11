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
existing snapshot layout remains version 3. The larger explicit on-foot
capture has a 128 MiB per-surface / 256 MiB total limit; normal UI
surface limits remain 16/64 MiB. There are no source copies outside an
armed eye run. The log reports camera, screen and depth counts,
including zero cameras or depth so a missed path cannot look like a
successful capture.

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
