# World scale

## Status

- **State (2026-10-27):** design only. Nothing is built or flown. No issue
  yet.
- **What it is:** OpenXR Toolkit's "World scale" changes only the distance
  between the two eye cameras the game renders with, about their midpoint.
  A larger percentage moves the eyes closer together and makes the world
  look bigger. Head movement, orientation and FOV are untouched. The real
  eye poses go to the compositor (see "What OpenXR Toolkit does").
- **Recommendation:** EDVR offers the same thing natively as one live key,
  `world_scale` (percent, default 100). Phase 1 scales the eye separation
  only, matching the Toolkit. It has to be applied in one place, the
  geometry snapshot, so that Elite's answer and the temporal pass (DLSS,
  FSR, XeSS motion) agree. Every gate that measures Elite's eye pair in
  fixed centimetres scales with it. The layer pose stays the located view,
  so the Toolkit's "restore before xrEndFrame" step comes for free.
- **Why EDVR and not the Toolkit:** the Toolkit was discontinued in 2024.
  It reached Elite only through OpenComposite. It is unverified whether it
  still loads on EDVR's own OpenXR session, or whether SteamVR's per-app
  world scale override reaches Elite now that Elite talks to EDVR.
- **Decisions for Sean:**
  - D1: the menu page. The VR menu has `performance`, `fixes`,
    `native_on_foot` and `hotkeys`. None fits; a new `view` page would be
    the honest home.
  - D2: the range. Proposed 50..200 %, step 1. The Toolkit allowed 0.1..1000 %.
  - D3: whether phase 2 (head translation scaled too) is wanted at all. Its
    answer comes from flight F1.
- **Unknowns, answered by F1:**
  - U1: whether Elite re-reads `GetEyeToHeadTransform` every frame on the
    native path. The ~12 calls per frame were measured through the legacy
    proxy (`openxr-port.md`). The native path has no per-call counter.
  - U2: what Elite's full-screen quad pass does with the half-IPD it reads
    (slot 28, caller 0x1428219D0, `terrain-culling.md` 0x28219D0 entry).
    Its effect at a non-100 % value is unknown.
  - U3: whether leaning in the cockpit "swims" at 130 % with head
    translation left at 1:1.
- **Next:** build phase 1 on `claude/openxr-world-scale-option-b1032a`, then
  fly F1 below.

## What OpenXR Toolkit does

Read from its source, `mbucchia/OpenXR-Toolkit` main at 6b9ecb69
(2023-11-05, MIT), layer directory `XR_APILAYER_MBUCCHIA_toolkit/`:

- **`xrLocateViews` only** (`layer.cpp:1687-1830`), primary stereo, VR
  sessions. Before it changes anything it saves the true poses
  (`1713-1714`). It takes the vector between the two eye positions and
  rebuilds both eyes about its midpoint at `ipd * 1000 / world_scale`
  (`1773-1783`). Head position, orientation and FOV are left as the
  runtime located them, and so is `xrLocateSpace`.
- **`xrEndFrame` puts it back** (`layer.cpp:2953-2965`). "Restore the
  original IPD to avoid reprojection being confused": the submitted
  projection views are spread back out by the same factor. The compositor
  therefore sees true eye poses. This assumes the app submits exactly the
  poses it was given.
- **The setting** is `world_scale` (code name `SettingICD`, `interfaces.h:135`).
  It is stored per app in the registry, in units of 0.1 %; the default is
  1000 (100 %, no change). The range is 0.1 %..1000 % (`menu.cpp:306,1606`).
  The menu shows it as a percentage with the resulting ICD in mm, and it is
  read every frame, so the slider is live. A legacy `icd` key migrates to it.
- **The documented caveat** is a v1.2 hotfix: "Fix issue with World Scale
  when application submits depth information". Nothing else on reprojection
  or comfort.
- **Elite:** no primary source for the values Toolkit users chose. In
  search snippets only (Frontier forum, unconfirmed), players used SteamVR's
  per-app world scale override for Elite and needed a game restart after
  changing it. They also set a lower IPD than their real one to make planets
  look bigger. Elite has no world-scale setting of its own that we know of.

## How EDVR differs

The Toolkit is a layer between an app and a runtime. EDVR is both ends at
once: it answers Elite's OpenVR calls, and it is the OpenXR app that
submits frames. That makes the job smaller in one place and larger in
another.

- **Smaller: the restore is free.** The projection layer's pose is the view
  located for that frame (`d3d11_stereo.cpp:554`, `v.pose=views[i].pose`,
  fed from `native_runtime_host.h:1701` `frameViews = located.views`). It is
  never rebuilt from what Elite rendered with. EDVR submits no depth layer
  (`frame_boundary.h:271`, no `next` chain) and does no positional
  reprojection of its own. Scaling what Elite is told leaves the compositor
  with the true eyes, as the Toolkit's restore intends.
- **Larger: EDVR reads Elite's camera back.** Several EDVR features rebuild
  or check Elite's camera. Each must use the separation Elite was given,
  not the headset's.

## Design (phase 1: eye separation only)

### The value

`world_scale = 100`, in percent. Above 100 the world looks bigger, below
100 smaller. These are the Toolkit's and SteamVR's direction and units, so
a player's old number carries over. `100` is the game's own scale and
changes nothing.

```
# ui: World scale | range 50..200 | step 1 | unit % | recommended 100 | live | menu <D1>
world_scale = 100
```

VR only; it stays off the flat allow-list.

### Carrying it to the runtime

`Config` lives in the d3d11 half and reloads live
(`vscreen.cpp:6002`). The OpenXR half never reads it (its own
`edvr_openxr.ini` is read once at start). The value therefore travels the
way `fix.now_pose` does (`native_frame.cpp:627`). It is a new field in
`EdvrNativeFrameOutput` (`common/native_frame.h:125-195`, with a step on
its version ladder at `:304-313`). The runtime latches it at
`WaitGetPoses` into the geometry publication, next to `tangentShift`
(`system_publication.h:29,62`, published at `native_runtime_host.h:1698`).
One frame has one value, however many times Elite asks within it.

### One place to apply it

`geometry_snapshot.h:70-72` computes each eye's `eyeToHead` as
inv(head) * view. The scale is applied there. The two eye origins move
about their midpoint by `100 / world_scale`, and the rotations are not
touched: a canted panel keeps its yaw, and `gameHandedness`
(`canted_display.h:22`) still conjugates afterwards. Everything that reads
the snapshot then follows from that one point:

- `GetEyeToHeadTransform` (`openvr_system.cpp:221-233`).
- The IPD property (`openvr_system.cpp:308-317`). No census has shown
  Elite reading it, but if it does, it gets the same number.
- The temporal frame (`native_temporal_client.h:41-50`). It calls the same
  `makeGeometrySnapshot`. DLSS, FSR and XeSS motion are built from these
  eye offsets with Elite's depth (`native_temporal.cpp:496-514`), so a
  mismatch would smear on every head turn. A change of value resets
  temporal history.

That last point also makes the comment at `common/native_temporal.h:20`
("as given to Elite") true again. Today it is the located transform, which
only equals Elite's on parallel panels.

### What has to scale with it

Gates that measure Elite's eye pair against fixed centimetres would refuse
a scaled pair. Each takes the expected separation from the snapshot, not a
constant:

- On-foot ADS refuses a solved eye pair outside 5..8 cm
  (`native_on_foot.cpp:10216-10217`). At 150 % a 64 mm IPD becomes 43 mm.
- The on-foot model right eye uses the located IPD, with a 4..9 cm gate and
  a 6.3 cm fallback (`native_on_foot.cpp:10966`,
  `native_on_foot_core.h:2836`). It must use Elite's, or on-foot stereo
  ends up at a different scale from the cockpit.
- The camera chooser's "eye pair six cm apart" (`common/camera_pose_math.h:21-23`,
  `camera_nearest.h`).

The jump guard's step thresholds (3 cm, 1 cm, 5 cm veto) measure head
motion. Phase 1 does not scale head motion, so they stay.

### What stays real

EDVR's own drawing uses the located views, so its UI keeps its physical
size whatever the world does:

- the in-headset menu (`native_menu_client.h:35-44`)
- the intro panel (`intro_panel.cpp`: real head, fixed 31.5 mm half-IPD)
- plugin frame args (`plugin_frame_args.h`)
- the skybox and notice layers

Seat memory, recentre and the published physical head also stay real.

### Instrumentation

Ask what the log shows if the code never ran: no `world_scale` lines and an
ICD equal to the located one. The feature applying looks different:

- One line per value change: `world_scale: 130% located_icd=63.4mm elite_icd=48.8mm`.
- A per-frame count of `GetEyeToHeadTransform` answers, logged once
  after each change. If Elite cached the transform at start (U1), the log
  shows zero answers at the new value, and the slider needs a restart to
  take effect.
- A `--health` tag, so `edvr_log.py --health` says ran and applied.

## Not in phase 1: head translation

A complete world scale would also scale head translation about the seated
zero, by the same factor. With eye separation alone, leaning 10 cm moves
the eyes 10 cm through a world that stereo says is 30 % bigger. Near
cockpit objects could then appear to slide against the background. The
Toolkit never did this, and seated play is mostly lean, so phase 1 leaves
it out. U3 decides.

If F1 shows a swim, phase 2 scales translation and velocity at the OpenVR
boundary. The exits are `openvr_compositor.cpp:35-52` (copyPose, for
`WaitGetPoses` and `GetLastPoses`), `openvr_system.cpp:257`, `:365` and
the reset-event pose at `native_runtime_host.h:2955`. The poses are already
relative to the seated origin (`seated_origin.h:10-30`). Phase 2 then
reaches `publishNowHead` and the world markers, the temporal head
translation, and the jump guard's thresholds. It would be the same key,
not a second one.

## Interactions with other scalers

- **OpenXR Toolkit, if installed:** it would sit between EDVR and the
  runtime. If it loads on EDVR's session, its scale reaches the views EDVR
  locates, and its value multiplies with `world_scale`. Unverified. The
  two ICD numbers in the change line are what tell a log reader.
- **Runtime IPD offsets** (Pimax Play, and others; unverified per runtime)
  change the located views. `world_scale` is relative to whatever the
  runtime locates, as the Toolkit's was.
- **SteamVR's per-app world scale override** applied to Elite as an OpenVR
  app. Whether it reaches an OpenXR session is unverified. If it does not,
  players who relied on it lost it when they moved to EDVR.

## Flight F1

Environment: Pimax OpenXR (Sean's primary), DLSS on, per-eye size as
configured. Repeat on Quest 3 + VD for the second runtime if U1..U3 are
clean.

1. Cockpit, 100 % → 130 % → 80 % live from the menu. The log shows the
   change line and a non-zero answer count at each value (U1). The scale
   visibly changes without a restart.
2. At 130 %, turn the head quickly. No temporal smear compared with 100 %.
3. At 130 %, lean forward and sideways (U3).
4. Look for a full-screen effect that shifts or doubles at 130 % and 80 %
   but not at 100 % (U2).
5. On foot at 130 % and 80 %: ADS engages (the gates scaled), and on-foot
   stereo matches the cockpit.
6. Menu and intro panel are the same physical size at every value.
