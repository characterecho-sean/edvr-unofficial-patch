# Eye tracking on the Pimax: what the driver sends, and how EDVR gets a gaze from it

*An investigation and a plan, written 2026-09-05 after the four gaze-probe
flights of that morning (branch `claude/foveation-gaze-probe-2ff1267`,
`src/openvr/gaze_probe.cpp`, Phase 0 item 15 of
[performance.md](performance.md)). It supersedes item 15's closing verdict,
"blocked on the driver, not on EDVR": the driver is feeding real eye data
through SteamVR's official eye-tracking component, in a mixed frame that
a client can undo. Claims are labelled measured (this rig's logs and
binaries), vendor-stated (headers, release notes, published source), or
believed, the convention of the parent document.*

## The finding

`GetEyeTrackedFoveationCenter` on this rig returns a direction that is
not the gaze, but it is made *from* the gaze: SteamVR forms the foveation
ray as `vGazeTarget - vGazeOrigin`, reading both in the head's frame, and
Pimax's SteamVR driver fills `vGazeOrigin` with the headset's position in
Pimax's own tracking universe (metres from an origin five metres above
and behind the chair) while `vGazeTarget` is the eye tracker's head-local
unit vector, with its x sign opposite to OpenVR's. The reported direction
is therefore `d - t`: a unit gaze vector `d` minus a five-metre position
`t`, which is why it sat at 37 degrees right and 55 up, why the eyes
moved it only a few degrees the wrong way, and why stepping half a metre
moved it and held. EDVR knows `t` every frame (the raw-universe HMD pose),
so `d` is recoverable -- by one addition per frame, `d = p + t`, because
the runtime hands the vector out with its scale intact (measured in the
first Phase 1 flight, 2026-09-05 12:31; the plan below was written for a
normalised centre, and its quadratic is now the check). The origin's
distance is a session's, not a constant: 5 m in the morning's flights,
15.6 m at noon. Independently, the same
tracker is readable directly from Pimax's runtime through the client
library its driver uses, the route mbucchia's SteamVR shim took on this
headset family.

## How it was established

### The plumbing, from the headers (vendor-stated, SDK 2.15.6 / 2.17.8)

- The driver side is `IVRDriverInput_004`: `CreateEyeTrackingComponent(container, "/eyetracking", &handle)` once, then `UpdateEyeTrackingComponent(handle, const VREyeTrackingData_t*, double fTimeOffset)` per sample, with `Prop_SupportsXrEyeGazeInteraction_Bool` (6009) set on the HMD.
- `VREyeTrackingData_t` is `{ bool bActive, bValid, bTracked; HmdVector3_t vGazeOrigin /* Ray origin */; HmdVector3_t vGazeTarget /* Gaze target (fixation point) */ }`. The header says nothing about the frame.
- The application side is two calls on `IVRSystem_026` (function-table entries 35 and 36) that return per-eye NDC points, and two on `IVRInput` (`GetEyeTrackingDataRelativeToNow`, `GetEyeTrackingDataForNextFrame`) that return the raw structure in a chosen tracking universe but need an eye-tracking *action handle*, which needs an action manifest. Elite has none, and registering one would move the game onto the action input system. So the NDC calls are the only route this game can use, and they are enough (below).
- vrserver.exe on this rig carries the component's strings (`InputValueType_EyeTracking`, the "eyetracking" binding arrays); vrcompositor.exe carries gaze-dependent features (`DrawSceneFoveated`, `gazeDepReprojectEnable`, an "Eye Tracking Debug" mode). Valve built this for its own eye-tracked headset; Pimax is a third-party driver of it.

### The frame SteamVR expects (vendor-stated: two published drivers)

Two SteamVR driver shims feed exactly this structure and are known to
work with Valve's OpenXR gaze test app: mbucchia's
`Pimax-EyeTracker-SteamVR` (Crystal, Crystal Super, Dream Air; archived
September 2025 because "SteamVR now exposes the necessary API through
IVRDriverInput") and Project-Babble's `VrcOsc-EyeTracker-SteamVR`. Both
leave `vGazeOrigin` at zero and set `vGazeTarget` to a **head-local unit
vector**: `(sin h cos v, sin v, -cos h cos v)`, with `bValid = bTracked =
bActive = true` and a time offset of 0. The README states it: "a unit
vector that originates from the center of the head and points forward
(z=-1)". The Pimax shim reads the tracker with
`pvr_getEyeTrackingInfo` and averages the two eyes' `GazeTan`.

### The driver on this rig implements it (measured)

`C:\Program Files\Pimax\SteamVRSupport\drivers\aapvr\bin\win64\driver_aapvr.dll`
(dated 2026-07-31, Pimax Play 2.0.3.286) contains the strings
`/eyetracking`, `CreateEyeTrackingComponent failed with:`, and
`IVRDriverInput_004`; it imports `libPVRClient64.dll` through its single
export `getPvrInterface`. Pimax's runtime log shows the tracker running
(`tobii_gaze: 120.48 FPS` throughout the flights). So the data reaching
SteamVR is Pimax's own driver's, not a shim's; the shim is not installed.

### The model against the flights (measured)

The probe's room test printed the head's position in SteamVR's standing
universe, which equals the raw universe on this rig (raw-to-standing came
back as the identity; the standing space was never calibrated for a
headset that tracks itself). Call that position `t`. If the reported
direction is `d - t` with `d` the unit gaze and the eyes straight ahead,
`d = (0, 0, -1)` and the prediction has no free parameter:

| Flight, window | `t` in the raw universe (m) | predicted yaw, pitch | observed |
|---|---|---|---|
| 4, first (seated) | (-2.730, -5.121, 2.548) | +37.6, +55.3 | +37.7, +55.6 |
| 4, second to fourth (seated, still) | (-2.700, -5.117, 2.566) | +37.1, +55.1 | +36.6 to +36.7, +55.2 to +55.3 |
| 4, sixth to eighth (standing, stepped right) | (-2.806, -4.664, 2.002) | +43.1, +57.2 | +43.1 to +43.3, +57.2 to +57.3 |
| 3, first to fourth (seated) | (-2.327, -4.534, 2.395) | +34.4, +53.2 | +35.5 to +36.0, +54.1 to +54.6 |

Three windows at two head positions agree to within 0.6 degrees on both
axes; the fourth is a degree off, which a gaze seven degrees from
straight ahead accounts for (the eyes were not controlled in a docked
window). The step right is the decisive one: a five-hundred-millimetre
translation plus standing up changed `t` and the prediction followed the
observation on both axes.

Two things the fit settles beyond the frame:

- **No rotation is applied.** The seated universe is yawed 73 degrees
  from the raw one (the seated-to-standing transform), so the head faced
  73 degrees off the raw axes throughout. The fit uses `t`'s raw-axis
  components verbatim in the head frame; rotating them into the head's
  frame puts the point behind the head. SteamVR subtracts and projects,
  it does not transform, which is consistent with it expecting head-local
  data, as the shims supply.
- **The target has unit length.** The eye sweeps in flight 2 moved the
  reported yaw by about +6 (eyes far left) and -5 degrees (eyes far
  right) around the constant. A unit target moved 30 degrees gives
  `atan((2.7 + 0.5) / 3.43) - atan(2.7 / 3.56) = +5.8` and
  `atan((2.7 - 0.5) / 3.43) - 37.2 = -4.5`. A target at the tracker's
  convergence distance would have moved the constant by tens of degrees
  between looking at a panel and looking out of the canopy; across three
  minutes of docked looking-around it never left 32 to 44 degrees.
- **x is mirrored.** Eyes left moved the reported centre right and vice
  versa, and the 50-degree head turn with the gaze held (eyes
  counter-rotating left) moved it right by 5 to 8 degrees. Whether y is
  mirrored too was not tested; the next flight does.
- **Blinks do not clear validity.** Eyes closed froze the value (step
  0.0001 NDC per frame, the stillest window of the flight) while every
  frame stayed valid: the driver keeps the last sample and its flags.

Why the room test itself read "behind": it projected `t` as a point in
the head's frame, whereas the observable is the direction from `t` to a
point one metre in front of the raw origin, `d - t`. Same data, opposite
sign; the test was one step short.

## What this means

1. **It is real, current eye data at tracker rate**, framed wrongly by
   the driver. The bug is Pimax's (origin in tracking space, target
   head-local, one axis mirrored); Valve's runtime does what its two
   published drivers expect and validates nothing.
2. **It is invertible on the client.** The unknown is `d` (a unit vector,
   two degrees of freedom); the observation is the direction of `d - t`
   (two numbers); `t` is known. See Route A.
3. **The tracker is also reachable directly**, in the game process,
   through the same client library the driver uses. See Route B.
4. **Pimax can fix it in one line**, and the OpenVR path then works with
   no EDVR code at all. The report is the highest-leverage action and
   costs an hour. See Phase 4.

## Routes to a usable gaze

### Route A: repair the frame inside EDVR (pure OpenVR)

**Reading the direction.** The plain call gives per-eye NDC through the
game's projection; the probe already inverts that through the eye's
tangents to a head-frame direction `n` (both eyes report one shared
direction: their NDC differ by exactly twice the frustum offset, so there
is no parallax to keep). Cleaner: entry 36
(`GetEyeTrackedFoveationCenterForProjection`) accepts *any* 4x4. With rows
`(1,0,0,0) (0,1,0,0) (0,0,1,0) (0,0,-1,0)` it returns `(x/-z, y/-z)`
directly, the tangents of `n`, independent of whichever projection
SteamVR would otherwise pick. With row 3 = `(0,0,0,1)` it returns `(x, y)`
un-divided *if* SteamVR treats the gaze as a point rather than a
direction, and a third call with row 0 = `(0,0,1,0)` then yields `z`: the
full vector, scale included. The zero parallax between the eyes says
SteamVR most likely normalises, in which case only the direction is
readable and the scale must come from the constraint below. The probe
finds out in one call at arming.

**Reading `t`.** Entry 12, `GetDeviceToAbsoluteTrackingPose(
TrackingUniverseRawAndUncalibrated, 0, poses, 1)`, once per frame at the
WaitGetPoses boundary: the HMD's position in the raw universe. (The probe
today composes it from the game's seated pose and entries 13 and 14; the
direct read does not depend on the seated zero, which moves at every
recentre.) The driver samples its origin at its own update time; the
difference from the pose EDVR reads is a few milliseconds of head
translation, millimetres, which is degrees of gaze only during a fast
head translation and acceptable for a fovea.

**The inversion.** `n` is a unit vector parallel to `d - t`, and `|d| =
1`. Solve `|λ n + t|² = 1`:

```
λ = -(n·t) ± sqrt((n·t)² - |t|² + 1)
d = λ n + t,  then d.x = -d.x  (and d.y = -d.y if the flight says so)
gaze tangents = (d.x / -d.z, d.y / -d.z), per-eye NDC through each eye's tangents
```

The ray from `t` meets the unit sphere twice. The true gaze is the *far*
root (larger λ) whenever `d·t < 1`, which for a five-metre `t` is every
gaze that does not point within about 80 degrees of the line from the
raw origin to the head, on this rig down, left and back, where nobody
looks while the game is in front of them. The two roots sit far apart
(a chord of the unit sphere), so tracking the root by continuity from
the far root at arming is robust; they merge only where the discriminant
touches zero, and the fovea holds its last centre for those frames. A
non-normalised 3D read, if SteamVR grants one, removes the ambiguity
outright.

**Conditioning.** An error of ε metres in `t` is an error of ε in `d`
directly (0.6 degrees per centimetre at unit length), and the NDC
precision is irrelevant beside that. The five-metre lever is what hid the
eyes in the first place; undone, it costs nothing.

**Gates**, because a repair that fires on a healthy driver is a bug:

- Arm only when the HMD's `Prop_TrackingSystemName_String` (entry 28,
  property 1000) is Pimax's driver, and log its
  `Prop_DriverVersion_String` (1031) beside the verdict so a report names
  the driver.
- At arming, over the first 300 valid frames: if the direct reading's
  mean lies inside the eye's frustum, use it directly (a fixed driver, or
  a compliant one on another headset) and never repair. If it lies
  outside and the inversion succeeds on at least 95% of frames with the
  far root inside the frustum, repair. Otherwise the fixed centre, one
  log line saying which and why, with `|t|` printed: a rig whose raw
  origin sits within a metre of the head would put the broken direction
  near the frustum and needs the dev override.
- In flight: a negative discriminant or a chosen root outside the frustum
  on more than 5% of the last 600 frames stands the source down to the
  fixed centre for the session, one line, no retry.
- Blinks: a per-frame step below 0.0001 NDC for more than 100 ms is a
  freeze, and the fovea holds; a blink never produces a log line
  (feature 3's privacy rule, unchanged).

**Sunset.** When Pimax ships the fix, the direct reading lands inside the
frustum and the arming test picks it; the repair code never runs. The
same arming test serves the Steam Frame and the Bigscreen Beyond 2e
without a Pimax branch.

### Route B: read the tracker from Pimax's runtime (vendor adapter)

`C:\Windows\System32\LibPVRClient64.dll` (Pimax installs it globally;
733 KB, 2026-08-06) exports one function, `getPvrInterface(major, minor)`,
which returns a table of function pointers for the version asked. The
published interface (Pimax's `PVR_Interface.h` as carried by PimaxXR's
`PVR` submodule; version 1.32; "Copyright 2017 Pimax, Inc. All Rights
reserved.") gives the positions EDVR would bind by:

| entry | member |
|---|---|
| 0 | `pvrResult initialise()` |
| 1 | `void shutdown()` |
| 2 | `pvrResult createHmd(pvrHmdHandle*)` |
| 3 | `void destroyHmd(pvrHmdHandle)` |
| 4 | `const char* getVersionString()` |
| 5 | `double getTimeSeconds()` |
| 65 | `pvrResult getEyeTrackingInfo(pvrHmdHandle, double absTime, pvrEyeTrackingInfo*)` |

with `pvrEyeTrackingInfo { pvrVector2f GazeTan[2] /* tan of eye gaze */;
double TimeInSeconds; float ConvergenceDistance; float blink[2]; }`
(8-byte aligned). `TimeInSeconds == 0` means no tracking (PimaxXR's
rule); PimaxXR needs no configuration call before reading it.

What it gives that Route A cannot: per-eye tangents, blink per eye, the
convergence distance, the sample's own timestamp, and no dependence on
the shape of anyone's bug. What it costs: a vendor library loaded into
the game process (only when the HMD's tracking system is Pimax's, only
on demand, behind the crash sentinel), an ABI transcription of seven
table positions from a header whose licence text reserves all rights
(transcribe the positions with attribution, never vendor the header; a
question for Sean before it ships), and two unknowns for the flight:
whether pi_server treats a client that never submits a frame as an
application (the shim created its HMD inside vrserver alongside the
driver's own, and Pimax Home and the calibration guide hold sessions
beside SteamVR's, so believed fine), and whether the tracker answers a
second client at all (believed yes: the shim was exactly that).

### Rejected

- **An eye-tracking action through `IVRInput`.** Needs an action
  manifest for the process, which switches Elite off the legacy input
  path it was written for.
- **An OpenXR session on the side.** A session that never renders never
  reaches FOCUSED, where `XR_EXT_eye_gaze_interaction` delivers; and the
  active runtime on a Pimax rig is Pimax's, not SteamVR's.
- **Tobii Stream Engine with Pimax's licence file** (what the VRChat
  face-tracking modules do). The key in `C:\Program Files\Pimax\Runtime`
  is Pimax's licence, not ours.
- **Pimax Play's OSC feed.** Pimax Play can emit VRChat's eye-tracking
  messages (`/tracking/eye/...`, head-relative pitch and yaw in degrees,
  UDP 9000). A four-second capture on this rig with the headset idle
  saw nothing, so it is unverified here; it is a third party's protocol
  on a contended port. Last resort only.
- **Waiting for Pimax.** Report, but do not wait.

## The plan

### Phase 1: one build, one flight, both routes probed

Extend `gaze_probe.cpp` (dev instrument, aggregates only, sentinel and
table validation unchanged):

1. **The 3D read.** At arming, call entry 36 with the point-form matrix
   (row 3 = `(0,0,0,1)`); log whether it answered finite values and, if
   so, `|p|` (1.000 means normalised). Per frame thereafter, the
   direction-form matrix for the tangents of `n`.
2. **`t` per frame** from entry 12 in the raw universe; print its mean
   and `|t|` per window beside the seated-derived value the probe prints
   today (they must agree; if they do not, the raw universe is not the
   driver's space and the seated route stays).
3. **The inversion**, both roots, printed per window as yaw and pitch of
   `d` for four sign variants (x mirrored or not, y mirrored or not), with
   the discriminant's minimum and the count of frames it went negative.
   The correct variant and root are the ones that read straight ahead in
   the first window and move with the eyes in the sweep windows.
4. **The PVR probe**, `advanced.gaze_probe_pvr = on`, off by default:
   load the library by its System32 name, `getPvrInterface(1, 32)` (a
   null answer for any other version ends it), `initialise`, `createHmd`,
   then per frame `getEyeTrackingInfo(hmd, getTimeSeconds(), &info)`;
   per window the mean and range of each eye's `GazeTan`, the mean blink,
   the convergence distance, and the sample-time advance rate; `destroyHmd`
   and `shutdown` at exit. Behind its own sentinel (`L"gaze_probe_pvr"`);
   any non-success result stands it down for the session with the code.
5. **Driver identity** in the ARMED line: entries 28's tracking-system
   and driver-version strings for the HMD.

Flight protocol (docked, calibrated, 450-frame windows, the probe's
existing five-second cadence): straight ahead 10 s; eyes far left, far
right, far up, far down, 5 s each; eyes closed 5 s; head turn 50 degrees
right with the gaze held on one spot, 5 s, and back; stand and step half
a metre right, hold 10 s; crouch and rise. Three minutes.

Success reads as: the PVR probe's sample time advancing at about 120 Hz
with `GazeTan` swinging about ±0.6 in the sweeps and blink rising with
the eyes shut; the repaired `d` in one sign variant and one root reading
within a few degrees of straight ahead at rest, swinging 25 to 35 degrees
with the sweeps, holding within 2 degrees through the head turn, the
step and the crouch, and agreeing with the PVR probe's averaged tangents
within about 2 degrees RMS. Two instruments that agree with each other
and with the protocol settle both routes in one flight.

**Built 2026-09-05 (this branch, `src/openvr/gaze_probe.cpp`).** The probe
gained the five items above, aggregates only: the direction-form and
point-form reads of entry 36 (the point-form once, at arming, its magnitude
logged), `t` from entry 12 every frame (its window mean and `|t|` beside the
room test's seated-derived head), the inversion for both roots and four sign
variants with the discriminant's minimum and negative count and each
variant's frustum share, the driver's identity from entry 28 in an ARMED
line, and Route B behind `advanced.gaze_probe_pvr` and its own sentinel
(`gaze_probe_pvr`): the library by its System32 name, `getPvrInterface(1,
32)` validated by a non-null table with the seven positions filled, a
readable version string and a plausible clock, then `initialise`,
`createHmd`, `getEyeTrackingInfo` once a frame, `destroyHmd` and `shutdown`
at exit; per window the tangents' range and mean per eye, the blink and
convergence means, the sample rate from the advancing sample time, and the
RMS angle between Route B's averaged tangents and each of Route A's far-root
variants. The Valve header confirmed the two new entries (12 and 28 in a
table of 51) and Pimax's header the interface positions (65 for
`getEyeTrackingInfo`; version 1.32; `pvrEyeTrackingInfo` 40 bytes at 8-byte
alignment). To fly it, in the Steam install's ini:

```
gaze_probe = on
gaze_probe_pvr = on
gaze_probe_summary_frames = 450
```

then the protocol above, and read the `frame repair` and `pvr` lines of
each window in the vr log.

**Flown 2026-09-05 12:31 (`v0.14.0-24-g363cfa4-dirty`; steps 4 and 5 of
the protocol swapped, no crouch).** Four findings, one of them changing
Route A:

1. *The centre carries its scale.* The point-form read answered finite
   values with `|p| = 15.85`, and `t` that session was `(-11.24, -10.71,
   1.89)`, `|t| = 15.6`: SteamVR hands out `d - t` itself, not its
   direction. The gaze is then `d = p + t`, one addition per frame, and
   `|d| = 1` turns from the constraint that had to supply the scale into
   a per-frame check of the model. The quadratic is ill-conditioned at
   this distance -- its discriminant `(n.t)^2 - |t|^2 + 1` came out at
   0.007 from a rounding-limited `n`, and the two roots landed fourteen
   degrees apart -- so it is demoted to the check for a runtime that
   normalises.
2. *The raw origin moves between sessions.* Five metres from the chair in
   the morning, 15.6 m at noon. `t` is read every frame (it is); nothing
   about it may be fixed at arming.
3. *The probe's own bounds rejected the data.* The runtime vouched for
   every one of the flight's 7,000 plain calls and the probe counted every
   one malformed: its sanity bound was 2 NDC, and a centre 15 m off
   projects at 2.9. The direction-form read's bound of 4 on the tangents
   let through only the windows where they dipped under it (3.87, 3.81 at
   rest, consistent with a gaze near straight ahead). Both bounds are 64
   now; finite is the shape that matters.
4. *Route B never armed*, by the probe's own doing: it asked the client
   library for its version and clock before `initialise`, and the clock
   read as nothing. It initialises first now and prints what it read if
   the check still fails. The tracking system is `aapvr`; the
   driver-version property did not answer, and its error code is printed
   next time.

**Flown 2026-09-05 14:54 (`v0.14.0-27-ge11153b-dirty`; Route B armed:
LibPVRClient64 interface 1.32, "1.33.1", clock 89 s).** The raw origin
sat 5.8 to 6.4 m from the head this session. Three findings:

1. *Route A is validated against Route B.* Over the first window's 599
   frames, and the two after it, the direction-form quadratic's FAR root
   with both axes mirrored agreed with Pimax's own tracker to 0.2, 0.1 and
   0.0 degrees RMS, and read within a few degrees of straight ahead (the
   discriminant 0.13 to 0.16, never negative; the near root 40 to 56
   degrees away and out of the frustum). Two independent instruments agree
   frame by frame: SteamVR's centre IS a unit gaze minus the raw-universe
   head position, and the repair recovers it at this distance.
2. *The subtraction did not.* Every frame's `p + t` fell outside 0.5..2
   and was set aside, so the point-form vector is not `d - t` in the frame
   and sign assumed (`|p|` was 6.5 at arming against `|t|` 5.8). The next
   build prints `|p|`, `|t|`, `|p + t|` and `|p - t|` per window so the
   right combination can be read off; until then the quadratic, which
   works, is Route A.
3. *The tracker froze.* From about 40 s in, SteamVR's centre stopped
   moving (per-frame step 0.0000 with every answer still vouched) and
   Route B held one value, (0.043, 0.134) in both eyes, for the rest of
   the flight, its blink mean 0.47 in the window it froze and 0 after,
   its sample time still advancing at the frame rate. Both routes read
   the same tracker, so the tracker itself stopped delivering. Sean: "I
   probably took the headset off when it froze" -- so a lifted headset
   holds the last sample with every flag still set, and nothing idled.

The rebuilt probe (`e230df2`) does the subtraction every frame from two
point-form reads (x and y with the identity-with-w matrix, z with the
first row swapped in) and prints per window: `|p|`'s mean, `|d|`'s mean
and range (1.000 says the model is exact), `d`'s mean direction for the
four sign variants with each one's frustum share, the RMS angle to Route
B, and the counts of frames the read declined or `|d|` fell outside
0.5..2. The next flight is the protocol in order. Success reads as: the
first window at rest straight ahead with `|d|` at 1.000 in one variant,
the sweeps swinging that variant 25 to 35 degrees, the head turn with the
gaze held swinging it the other way by the head's yaw, and Route B's
tangents agreeing within about 2 degrees RMS.

### Phase 2: the gaze source

A small module (`src/openvr/gaze_source.cpp`, working name) behind one
call, `gazeSample()`: a head-relative unit direction, a source tag, and
validity, consumed by features 6 and 2 through the tangents in use (true
or lied under the guard), exactly as feature 3 specified. Sources in
order of preference: the runtime's centre read directly (any compliant
driver); the runtime's centre repaired (Route A, gated as above); the
Pimax runtime (Route B) if the flight prefers it or Route A stands down;
none (the fixed centre). One line at arming names the source and the
driver; nothing per frame; blinks hold. Settings sketch, final names at
implementation and subject to the ini rule that values name what the
player gets:

```
[experimental]
eye_tracking = auto     ; auto | off. auto: the fovea follows the eyes when the
                        ; headset's driver gives a usable gaze, directly or after
                        ; the frame repair the log describes; off: the fixed centre.
[advanced]
gaze_repair  = auto     ; auto | on | off -- dev override of the arming test
gaze_probe_pvr = off    ; the Pimax-runtime probe
```

### Phase 3: features 6 and 2 take the moving centre

*Redirected 2026-09-05 (docs/performance.md, feature 6, "Where the crop
design stands").* The moving DLSS crop is off the plan: a crop resolves at
the start of every large look, and no crop meets "no blur where the eyes
look" under upscaling. Phase 3 is feature 2 alone -- the shading-rate
image's rings follow the gaze -- with the fixed-centre build of feature 2
(built the same day) as its base. The paragraphs below stand as written for
feature 2's part; feature 6's is history.


Unchanged from performance.md: feature 6's crop pans with a uniform
vector (measured, item 16), feature 2's mask regenerates when the centre
moves a tile. The fixed-centre versions still build first; the moving
centre is the delta this document was written to unblock.

### Phase 4: upstream

A report to Pimax, with the table above and the one-line diagnosis: in
`driver_aapvr`'s eye-tracking component, `vGazeOrigin` is the HMD's
tracking-space position while `vGazeTarget` is head-local, and the target's
x is mirrored; SteamVR reads both in the head's frame (its published
drivers pass a zero origin and a head-local unit target). Suggested fix:
a zero origin, or transform the target into the same space as the origin,
and un-mirror x. Environment: Pimax Play 2.0.3.286, `driver_aapvr.dll`
2026-07-31, SteamVR 2.17.8, Crystal Super, tracker calibrated. A note to
Valve as well: vrserver accepts an origin metres from the headset without
complaint, and its own OpenXR `XR_EXT_eye_gaze_interaction` on a Pimax
presumably reports the same ray. Optional supporting evidence, if Sean
wants it: SteamVR's compositor has an eye-tracking debug overlay
(`set_eye_tracking_debug_mode` in vrcompositor.exe), which would show
the runtime's own marker parked top-right.

## Two things noted on the way

- **Pimax's own foveation is off on this rig** (`enable_foveated_rendering
  : 0` in `%LOCALAPPDATA%\Pimax\runtime\profile.json`), so the "wrinkle"
  in item 15 about EDVR's VRS overlapping it does not currently apply.
  When it is on, Pimax injects NVIDIA VRS into the game through
  `LibMagicD3D1164.dll` (`MagicAttach_x64.exe`, both in `C:\Program
  Files\Pimax\Runtime`); feature 2 should look for that module in the
  process and stand down rather than double-shade. The
  `vrss_gaze_provider.exe` running beside it feeds NVIDIA's VRSS 2, which
  needs forward rendering with MSAA and cannot apply to Elite.
- **Pimax's OpenXR runtime does eye tracking properly** (`PiOpenXR_64.dll`
  carries `XR_EXT_eye_gaze_interaction` and calls
  `pvr_getEyeTrackingInfo`), which is why quad-views games work on this
  headset and why nobody noticed the SteamVR driver's frame: on a Pimax,
  OpenVR apps were the only consumers of that component, and there were
  none.

## Appendix: the function-table entries this plan uses

`FnTable:IVRSystem_026` (SDK 2.15.6, `openvr_capi.h`): 0
`GetRecommendedRenderTargetSize` (validation), 12
`GetDeviceToAbsoluteTrackingPose`, 13
`GetSeatedZeroPoseToStandingAbsoluteTrackingPose`, 14
`GetRawZeroPoseToStandingAbsoluteTrackingPose`, 28
`GetStringTrackedDeviceProperty`, 35 `GetEyeTrackedFoveationCenter`, 36
`GetEyeTrackedFoveationCenterForProjection`, 49 `GetRuntimeVersion`.
`pvrInterfaceV32` (`getPvrInterface(1, 32)`): 0, 1, 2, 3, 4, 5, 65 as
tabled above.
