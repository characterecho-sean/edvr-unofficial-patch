# Whole-station flicker after the smoke fix

The user confirmed that private UI depth fixed the rectangular smoke holes
in build `6AA2FDEB`, then reported flicker across the entire station during
ship/head movement. This review preserves that smoke fix and examines the
completed flight `edvr_gfx_20260910_130230.log` (13:02–13:07 local), with eye
runs `130647` and `130715` and their draw/pool ledgers.

## What the flight establishes

The station motion gate dropped correction for 17 scene frames in the
interval ending 13:06:30, and 11 in the interval ending 13:07:10. Neither
interval reported a new camera-translation jump. The intervening interval
reported four jump **eye evaluations**, five frames using carried camera
motion, and 13 frames carrying the body across an origin change. The camera
selector mostly used the bound scene block; the earlier persistent
auxiliary-camera lock is not apparent in these aggregates.

These are evidence of interruptions, not a timestamped match to the user's
flicker. The registration log aggregates 20 seconds at a time. Each eye run
contains 16 raw input crops and only one whole treated output. It cannot
show whether the AA output flickers on the same frames or expose the exact
motion transform supplied to AA. The 4 km agreement gate is left intact;
loosening it would hide the rejection without establishing compatible
coordinates.

## Reproduced error in the carried body transform

`temporalBodyPathCarried` composes the station's rotation with a carried
camera delta on rejected-camera and origin-jump frames. The caller already
rebased the station translation into the current origin, `t' = t + (I-R)s`.
The helper still evaluated `(R-I)c_prev + t'` with `c_prev` in the old
origin. Its uncompensated term is `(I-R)s`.

For a 13 km perpendicular shift and a 0.04-degree station turn, the
regression reproduces **9.076 m** of spurious history displacement. This
does not prove that every reported flicker was this event, especially the
longer gate dropouts without new jumps.

The correction passes the current frame's origin step separately from the
accumulated shift of an older body sample. It rebases the previous camera
position by that step, including when a new body sample already agrees
unshifted and clears the accumulated shift. The camera translation is also
rotated by the body delta, making the composition exact under simultaneous
head rotation and ship translation. The main body, second body, stepped
parts and moving-ship fallback all use the corrected helper.

The station sample is now frozen for each scene frame. An asynchronous
worker publication between eye submissions can no longer change the rates,
camera stamp or uploaded grid version for the second eye. This removes a
possible stereo inconsistency; its occurrence was not measurable in the
old capture format.

## Evidence available in the next flight

The usual eye-dump command now defaults to a paired capture:

- `eye_HHMMSS_C00..C15.bmp`: raw input, before either TAA or DLSS.
- `eye_HHMMSS_T00..T15.bmp`: the matching treated output frames.
- `eye_HHMMSS_L0.bmp`: first treated frame, whole, for context.
- `eye_HHMMSS_motion.csv`: scene frame, eye, crop index, input/requested-output sizes,
  history/reset inputs, camera rows, station sample age/version/fit/rates,
  origin shifts, and the camera/main-body/second-body transforms supplied
  to temporal processing. The trace records planned motion parameters; it
  is not a pixel-by-pixel dump of generated motion vectors or NVIDIA's
  internal history. Use the crop index to join the separate object ledger;
  its frame counter must not be assumed identical to the temporal counter.

C and T are centre crops at their respective native pixel scales. Use the
CSV dimensions when comparing a DLSS input crop with its output. Copies
are taken during the sequence; disk writes wait until both eyes finish
the last frame. A failed raw copy is marked explicitly in `rawCaptured`.
`advanced.eye_run_paired = 0` restores the earlier single-sequence selection
by `advanced.eye_run_treated`. Captures allocate additional staging memory
on request (retained for reuse) and issue GPU copies during capture; this
may affect capture timing.

The normal log also records the start and recovery of body-frame gate
rejections, with the camera/pair coordinates, accumulated shift, age and
grid version. A recurrence can therefore identify the rejected frame even
if a short eye run misses it.

## Validation

The temporal arithmetic suite passes. The new regression covers all three
rotation axes, both turn/shift directions, stationary and translating
cameras with head rotation, and shifted/unshifted coordinates. Its maximum
corrected error is **0.000759 m**, versus the 9.076 m old-origin control.
Calling the pure helper for both eyes also confirms the step is consumed
without mutation; this is not a full live stereo scheduling test.

Build `6AA3044B` passed the full NVIDIA SDK build, including all existing
checks, 3,895 drive/capture checks, 863 private UI-depth checks, and the
configuration contract. The proxy GPU smoke suite also passed. An initial
build caught the new paired-capture key missing from the INI documentation;
it was documented and the final complete build passed.

Both DLLs were installed with the game closed and verified against their
prepared SHA-256 hashes. The installed INI hash is unchanged. Verified
backups of confirmed smoke build `6AA2FDEB` and the deployment manifest are
under `build/review_motion/station1307/install`. Analysis, build and GPU
test logs are in the parent directory.

Live whole-station stability, and the cause of the longer gate
interruptions, still require the next headset flight. Repeat the approach
with ship and head movement and use the normal eye dump if flicker remains.
