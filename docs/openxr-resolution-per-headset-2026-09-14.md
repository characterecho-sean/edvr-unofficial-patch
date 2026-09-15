# OpenXR resolution per headset

## Status

- **Current fix (2026-09-15):** Widened-only alignment missed the menu's odd
  output height. Initial and live game-facing recommendations now align both
  axes, including inactive guard states. Full build and desktop checks pass;
  installed/verified on both installs. Headset retest pending; see
  "Startup/menu odd-height recurrence" at the end.

- **Original sizing plan:** Revision 3, DECIDED and BUILT (ab48589 on native runtime
  d160499; every gate green incl. 205 native_render_settings_test checks)
  but NOT YET FLOWN. `openxr_render_scale` renamed `openxr_resolution`;
  the unit changes from percent/fraction to per-eye width in pixels,
  keyed on the sanitised (runtime name, system name) pair -- because
  2026-09-14's logs show one Quest 3's saved 180% resolving to 5530x5875
  per eye (32.5 MP) the moment Virtual Desktop supplied a bigger base.
- **Open:**
  - "Decisions for Sean" #2, 3, 5-9 carry no DECIDED tag (only #1, #4 do).
  - Whether SteamVR/OpenXR reports one `systemName` for two headsets
    (flight 2b; "Risks").
  - Every runtime's `systemName`, and PiOpenXR's exact bytes, unrecorded.
  - Whether Elite's Graphics Apply re-inits VR in-process (flight 1 looks
    for a shutdown/startup pair; decision 8 depends on it).
  - Decision 9: widen the 0.25-2.0 width clamp, or leave it.
- **Ruled out:**
  - Keying entries on the runtime's recommended `WxH` (rev. 2): one Pimax
    gave six different sizes on 2026-09-14 alone.
  - A single per-eye-megapixel budget: Sean's rigs sit at 11.6 MP and
    16-29 MP, so one number serves neither.
  - FOV signature (angular density) for startup sizing: needs a begun
    frame, which comes after the swapchains exist; extension unprobed.
  - A table of panel resolutions per headset model: no panel size is
    exposed and no `systemName` is on record.
  - A `[headset:<name>]` ini section per headset: breaks five consumers
    (checker, audit, installer merge, menu, dotted-key split).
  - A wildcard entry for unlisted headsets: reintroduces the
    cross-headset hazard the design exists to remove.
- **Next flight:** Pimax OpenXR, current Frontier settings, HMD quality 0.5.
  Check DLSS at startup/menu and in the cockpit, monitor padding, and normal
  exit. Verify the installed build first, then require explicit DLSS feature
  creation and no render-range rejection. The earlier cross-runtime plan is
  retained under "Retest plan".
- **Environment:** Native OpenXR host only (SteamVR/OpenComposite never
  call this query). Runtimes on record: Oculus 1.207.0, SteamVR/OpenXR
  2.17.9, VirtualDesktopXR 1.0.10, PiOpenXR (bytes unconfirmed). Two
  headsets/installs (Quest 3; Pimax Crystal Super), recommended sizes
  1824x1968 to 5424x5356. Fixed tables: D3D11's 16384 cap, the per-view
  max (8192 SteamVR, 16384 PiOpenXR), the 184-byte v2 ABI, 30-byte token.
- **Detail:** "The problem" has the flight-log tables and the 5530x5875
  incident. "What the runtime tells us, and what it does not" has the
  startup order. "Options considered" has every ruled-out identity/unit.
  "The recommended design" (all subsections) is the build itself.
  "Decisions for Sean" and "Claims to verify" close the doc. Linked:
  openxr-resolution-2026-09-14.md (design being replaced),
  openxr-airlink-menu-exit-2026-09-14.md and
  openxr-metrics-parity-2026-09-14.md (cited flights, one attribution
  corrected here), openxr-runtime-inventory-2026-09-12.md and
  openxr-native-discovery-2026-09-13.md (fixture values).

Design for setting the OpenXR render resolution per headset and runtime, so
that a value chosen on the Quest 3 cannot follow Sean onto the Pimax, or onto
the same Quest 3 through a different streaming app. It replaces the unreleased
`fix.openxr_render_scale` key described in
[openxr-resolution-2026-09-14.md](openxr-resolution-2026-09-14.md) with
`fix.openxr_resolution` (the rename was put to Sean and agreed on 2026-09-14)
and builds on the native runtime merged in d160499. Built 2026-09-14 (ab48589,
every gate green, 205 checks in native_render_settings_test) and not yet flown:
the retest plan below is the next step, and the system tokens in the ini
example are placeholders until a flight records the real strings.

Revision 3, after the flight logs of 2026-09-14. Revision 2 keyed entries on
the runtime's recommended per-eye size and stored a percent of it; the same
day's logs show six recommended sizes across two headsets, two of them for one
Pimax on one runtime, and show Sean's own saved 180% being multiplied into
5530x5875 per eye the moment Virtual Desktop supplied a larger base. Both the
identity and the unit change in this revision. Everything that did not depend
on them (the v2 in/out struct and its echo rule, the SEH layout, the ten menu
sites, the numerically compared restart badge, the pure request/validate
functions, the self-test and retest structure) is kept. Every file:line below
was re-read against HEAD d160499 for this revision. A review pass on the same
day corrected four things the first draft had wrong, each named where it sits:
the live ini values (`1.0` and `0.90`, not `1.8` and `1.0`), the tool that
merges (the installer executable, never `install_edvr.py --ini`), the
`retired-default:` annotation's leak through the schema generator, and the
attribution of the parity doc's 4068x4016 flight.

## The problem

The native runtime hands Elite one recommended per-eye size through
`IVRSystem::GetRecommendedRenderTargetSize` (src/openxr/openvr_system.cpp:
128-136), and the unreleased `fix.openxr_render_scale` multiplies the runtime's
own recommendation before the swapchains are created
(src/openxr/native_runtime_host.h:1250-1254, :1273). The key is one number for
the whole install (edvr.ini:442-450), read once per native host start
(src/d3d11/native_render_settings.cpp:44).

Sean flies two headsets from two installs, through four runtimes. All of the
following are native flight logs from 2026-09-14 (UTC), read with `python
tools\edvr_log.py --target <store> --tag openxr --nth N
--grep ' (runtime|size,0|openxr_render_scale|openxr_render_size,eye=0),'`,
where `--nth 0` is the newest log in that install and `--list` prints the
numbering (edvr_log.py:271-274). `--file` is not the way to name one of these:
it is resolved with `os.path.abspath` against the current directory and ignores
`--target` (:293-297), so a bare log name prints `no such log`; a full path
works. The `runtime,` field is `XrInstanceProperties.runtimeName` verbatim
(native_runtime_host.h:1196); `size,0` is eye 0's recommendation (:1220);
`requested=` is what the key asked for and `scaled=` what the host built (:251,
:266-268). The `build` column is each log's `module_init, version=` line; only
the Steam rows are HEAD (`--expect-build HEAD` exits 0 on them), so every
Frontier row is evidence about the runtime and the arithmetic, not about the
current DLLs.

Frontier install (`C:\Users\seanm\AppData\Local\Frontier_Developments\
Products\elite-dangerous-odyssey-64\edvr_logs\`; `-NN` is short for
`v0.16.2-NN-g...-dirty`):

| Log (edvr_openxr_20260914_...) | build | Runtime | size,0 | requested | scaled |
| --- | --- | --- | --- | --- | --- |
| 064629_154_21200 (12:46) | -88 | SteamVR/OpenXR | 4068x4016 | none | 4068x4016 |
| 065000_316_19352 (12:50) | -88 | SteamVR/OpenXR | 4068x4016 | none | 4068x4016 |
| 101036_329_25644 (16:10) | -94 | Oculus | 1824x1968 | 1.0 | 1824x1968 |
| 105839_707_26088 (16:58) | -95 | Oculus | 1824x1968 | 1.8 | 3283x3542 |
| 133614_592_27284 (19:36) | -97 | Oculus | 1824x1968 | 1.8 | 3283x3542 |
| 140558_020_19636 (20:05) | -97 | VirtualDesktopXR | 3072x3264 | 1.8 | 5530x5875 |
| 141341_825_13844 (20:13) | -97 | VirtualDesktopXR | `result,xrGetSystem,-35` | | |
| 141441_019_28896 (20:14) | -97 | SteamVR/OpenXR | 2528x2704 | 1.0 | 2528x2704 |
| 141917_679_2776 (20:19) | -97 | SteamVR/OpenXR | 2528x2704 | 1.0 | 2528x2704 |

Steam install (`c:\steam\steamapps\common\Elite Dangerous\Products\
elite-dangerous-odyssey-64\edvr_logs\`, all three logs build
v0.16.2-119-gd160499, the merged main):

| Log (edvr_openxr_20260914_...) | build | Runtime | size,0 | requested | scaled |
| --- | --- | --- | --- | --- | --- |
| 145030_828_23128 (20:50) | HEAD | SteamVR/OpenXR | 4980x4916 | 1.0 | 4980x4916 |
| 151657_808_8792 (21:16) | HEAD | (none) | `module_startup,graphics_unavailable=80004002` | | |
| 151813_548_6608 (21:18) | HEAD | SteamVR/OpenXR | 4980x4916 | 1.0 | 4980x4916 |

The 21:16 log is five lines and not a flight: `80004002` is `E_NOINTERFACE`
from the loading-boundary acquire at native_module.cpp:44-46, mapped to
`VRInitError_Init_HmdNotFound`, and it never reached `xrGetInstanceProperties`.
The 20:13 log is thirty lines and not a flight either: `runtime,
VirtualDesktopXR,` then `result,xrGetSystem,-35`
(`XR_ERROR_FORM_FACTOR_UNAVAILABLE`, third_party/openxr/include/openxr/
openxr.h:187: Virtual Desktop was not streaming) and `module_startup,...,
result=124`. That is the inventory's 09-12 VDXR finding
(docs/openxr-runtime-inventory-2026-09-12.md:23) repeated in flight, and it is
direct evidence about this design's identity read: the runtime name at :1196
was printed and the host failed at :1198, before any system name existed, so
the new `system,vendor=` and `headset_key,` lines described below are absent in
that case too. A Quest route has an identity only while its streaming app is
up.

Which headset each row was is an inference from the aspect ratio (Quest 3 rows
0.927-0.941, Pimax rows 1.010-1.013) and from Sean's account of the day; aspect
is an observation, not an identity, and nothing in this design matches on it.
Two rows and one correction follow from the tables.

The motivating incident is the 20:05 row. Sean's Frontier `edvr.ini` carried
`1.8` from the Air Link retest at 16:58
(docs/openxr-airlink-menu-exit-2026-09-14.md:217-224: 180%, 3283x3542 from
1824x1968, 11.6 MP per eye) until 20:06:28 UTC. At 20:05 the same Quest 3 was
streamed through Virtual Desktop instead of Air Link; VirtualDesktopXR
recommends 3072x3264, and the same 180% resolved to 5530x5875 per eye: 32.5 MP
per eye, 65 MP for the pair, before DLSS history buffers, from one change of
streaming app and no change to any EDVR setting. `effectiveScale` found no cap
(16384/3072 = 5.3; src/common/native_render_settings.h:94-108) and
`scaledDimension` (:83-92) did what it was told. Thirty seconds after that
launch the file was written back to `1.0` (the live file and its mirror both
carry `openxr_render_scale = 1.0` with a 20:06:28 UTC write time, an F8 write
by the look of it), the 20:13 launch failed as above, and the two after it were
the Quest 3 over Steam Link, on SteamVR/OpenXR, at 100%: the 20:14 and 20:19
rows already show `requested=1.0`.

The live values as of this revision, read with an explicit UTF-8 decode and not
modified: the Frontier `edvr.ini` says `openxr_render_scale = 1.0` (written
20:06:28 UTC) and the Steam `edvr.ini` says `openxr_render_scale = 0.90`
(written 21:20:24 UTC, after the 21:18 flight, which still ran at 1.0); both
`%LOCALAPPDATA%\EDVR\<leaf>-<store>\edvr.ini` mirrors agree. Revision 2 and the
first draft of this revision were written against `1.8` and `1.0`, values that
no longer exist, so every migration and retest statement below is stated
against `1.0` (Frontier) and `0.90` (Steam), and the retest re-reads both files
immediately before the desk pass rather than assuming either.

The correction: docs/openxr-metrics-parity-2026-09-14.md:66-71 describes "the
matching Pimax flight" (5,430 stereo pairs, build v0.16.2-87-g09b003a-dirty) at
4068x4016 per eye immediately after saying Windows selected PiOpenXR at
installation. No surviving log is that flight: the 12:46 log is build -88 with
`native_summary,...,pairs=5831` and the 12:50 log is build -88 with
`pairs=4627`, so the parity flight's own log is gone and its runtime is
unrecorded. What the surviving logs do show is that both 4068x4016 logs on disk
(12:46 and 12:50) say `runtime,SteamVR/OpenXR,563022967865353`, and no
surviving log shows PiOpenXR at 4068x4016. Revision 2 of this document and the
first draft of this one attributed 4068x4016 to PiOpenXR and built a drift
argument on it ("the same Pimax over PiOpenXR at 5424x5356 on 09-13 and
4068x4016 on 09-14"); that attribution is withdrawn. The supportable statement
is that the Pimax over SteamVR/OpenXR recommended 4068x4016 at 12:46 and 12:50
in the Frontier install and 4980x4916 at 20:50 in the Steam install, on the
same day, with the same runtime string; the only PiOpenXR sizes on record are
the desk probe's 5424x5356 on 09-12 (docs/openxr-runtime-inventory-2026-09-12.
md:21) and the 09-13 flight's 5424x5356 (docs/openxr-native-discovery-2026-09-
13.md:103). The parity doc is not edited by this design; its sentence is quoted
here so an implementer does not inherit it either way.

The second row that matters is the pair 20:14 and 20:50: two different headsets
(a Quest 3 over Steam Link, a Pimax over SteamVR) on the same runtime string
`SteamVR/OpenXR`, in the same afternoon. The runtime name alone cannot be the
identity.

The failure mode is arithmetic, and the same file that is right on one route is
a first-flight disaster on another. The key exists in no public release (`git
show v0.16.2:edvr.ini` has no `openxr_render_scale`), so its name, grammar,
unit and semantics can still change without a migration.

What Sean asked for is a target "per runtime/headset" that outputs "a
reasonable amount of pixels respective to the native resolution of the
headset". This revision pins both halves: the identity is the runtime's name
and the system's name, both read from the runtime before the size is chosen;
the unit is a per-eye width in pixels, which is the number a "reasonable amount
of pixels" is measured in and which no vendor slider or streaming app can
multiply.

## What the runtime tells us, and what it does not

Everything below is read from the code at HEAD d160499.

Order of startup in `NativeRuntimeHost::open()`
(src/openxr/native_runtime_host.h):

1. `xrGetInstanceProperties` gives the runtime name and version; the name is
   already traced as `runtime,<name>,<version>` (:1194-1196).
2. `xrGetSystem` then `xrGetSystemProperties` gives `systemName` and `vendorId`
   (:1197-1200). `systemName` is never traced; it is copied into the
   `SystemRead` metadata at :1261 (declared at system_source.h:30) and served
   as `Prop_ModelNumber_String` (src/openxr/openvr_system.cpp:233); the runtime
   name is served the same way as `Prop_TrackingSystemName_String` (:234),
   which is the one consumer of `runtimeName` beyond the trace. `vendorId` is
   fetched and never read.
3. `xrEnumerateViewConfigurationViews` gives both eyes' recommended and maximum
   image rects; they land in `sizes[]` and, with the maximum already clamped to
   `D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION` (16384), in `renderBounds[]`
   (:1211-1221). Each eye's recommendation is traced as `size,<eye>,<w>,<h>`
   (:1220). The maximum is not traced.
4. The D3D11 device is created (:1222-1249).
5. `captureRenderSettings()` asks the graphics DLL for the scale (:1250, body
   :223-253), `applyRenderScale()` rewrites `sizes[]` (:1251, body :254-270),
   the scaled sizes are re-validated (:1252) and published to the graphics DLL
   as `EdvrNativeRenderSizing` (:1254, body :271-299).
6. Only then: session creation (:1256), the `SystemRead` metadata (:1258-
   1261), `geometry.begin` (:1271) and the swapchains at `sizes[]` (:1273).

So at the moment the scale is chosen, the host knows the runtime name and
version, the system name and vendor id, and both eyes' recommended and maximum
sizes, with no reordering and no session.

The runtime names are now on record verbatim, from today's logs: `Oculus`
(version 282364034940928, which decodes through `XR_VERSION_MAJOR/MINOR/ PATCH`
(third_party/openxr/include/openxr/openxr.h:34-36) as 1.207.0),
`SteamVR/OpenXR` (563022967865353 = 2.17.9) and `VirtualDesktopXR`
(281474976710666 = 1.0.10). The fourth, PiOpenXR, is on record only as the
inventory table's paraphrase "Pimax OpenXR" 0.1.0
(docs/openxr-runtime-inventory-2026-09-12.md:21; the probe prints
`instance,<runtimeName>,<version>` at tools/openxr_probe/openxr_probe.cpp: 138
and the table's first column reproduces it) and the 09-13 discovery doc's prose
(:103); no 09-12 or 09-13 native log survives in either install's `edvr_logs\`,
so its exact bytes are confirmed on the desk pass below.

The system names are not on record for any runtime. The host fetches them
(:1200) and copies them (:1261) but never traces them, no archived probe output
survives in either checkout, the inventory's "Pimax Crystal Super" (:21) and
"HMD system" (:22) are paraphrases, and Elite's own `netLog.2026-09-14T*.log`
files (22 of them under the Frontier install's `Logs\`) carry no headset model
string either (no word-bounded `Quest`, `Pimax`, `Crystal`, `Oculus` or
`OpenXR` in any of them; a bare `Quest` grep matches `Request` 553 times and
proves nothing). The first build of this design traces them, the desk pass is
the first record, and flight 1 confirms them. Until then every system token in
this document is illustrative and is written so.

What OpenXR does not expose at all is the panel. The recommendation moves with
the vendor's own quality setting and with the streaming app, and the archive
plus today's logs show it moving on Sean's rigs: one Pimax over SteamVR/OpenXR
at 4068x4016 and 4980x4916 on one day (tables above), at 4268x4216 on the desk
(inventory :22), 4336x4284 (docs/performance.md:1111) and 4536x4480
(docs/openxr-port.md:121) earlier in the month; the same Pimax over PiOpenXR at
5424x5356 (inventory :21, discovery :103); the Quest 3 at 1824x1968 over
Oculus, 3072x3264 over Virtual Desktop and 2528x2704 over Steam Link, all on
09-14. A "percent of the panel" would need a table of panel sizes per headset
model keyed on a string nobody has recorded. That is a guess, and it is wrong
on every headset not in the table. The only guess-free notion of "native" is
the runtime's recommendation, and that is what 100% means throughout this
design; the unit chosen below is one the recommendation cannot multiply.

The field of view is not available either: the tangents come from
`xrLocateViews`, which the host performs only through `locateGeometry` inside a
begun frame (src/openxr/geometry_locator.h:18 returns
`XR_ERROR_CALL_ORDER_INVALID` otherwise), called from `waitPoses()` at :534 and
the loading path at :443, after the swapchains exist. The FOV signature that
`fix.cull_guard_headsets` keys on (edvr.ini:338-341; native match at
src/openxr/native_cull_guard.h:150-153) is therefore not available for startup
sizing without reordering `open()`. Two desk probes would settle whether it
could be: (a) whether each runtime answers `xrLocateViews` on a created,
not-yet-begun session (the locator's :18 precondition is EDVR's, not the
specification's); (b) whether each runtime advertises
`XR_EPIC_view_configuration_fov`, which returns the per-view FOV from
`xrEnumerateViewConfigurationViews` itself, before :1250, with no session.
Neither is needed for this design.

The cap the host applies is `XrViewConfigurationView.maxImageRectWidth/
Height`, clamped to 16384 (:1217-1219), not `XrSystemGraphicsProperties.
maxSwapchainImageWidth/Height`; the probe prints both (`max_size=` on its
`system,` line, openxr_probe.cpp:145-149; `max=` on its `view,` lines,
:174-177), and the inventory's "Limits per eye" column is the per-view value
(16384 on PiOpenXR, 8192 on SteamVR).

The query itself is one-way today. `edvrQueryNativeRenderSettings` takes
`(version, size, void* output)`, validates the two arguments, reads
`fix.openxr_render_scale` with `Config::getFloat` and overwrites the whole
16-byte struct (src/d3d11/native_render_settings.cpp:34-52;
src/common/native_render_settings.h:22-27, :68-69). The host pre-fills `{size,
version, 0, 0}` and re-validates the answer (:240-247). A wrong answer is a
hard startup failure, not a fallback: FALSE, a wrong size or version, a
non-zero reserved word or a non-finite scale returns
`result("native_render_settings_query", XR_ERROR_VALIDATION_FAILURE)`, which
prints `result,native_render_settings_query,-1` (`result()` at :110-113;
`XR_ERROR_VALIDATION_FAILURE = -1`, openxr.h:155; -2 is
`XR_ERROR_RUNTIME_FAILURE`), and `start()` maps that to
`VRInitError_Init_Internal` (:325). Only the unpaired diagnostic host (no
graphics proxy) falls back to 100% (:232-236).

The project's ABI conventions, stated exactly: `native_timing.h` replaced
`VERSION_1` with `VERSION_2` in place in a8a0e57, but `native_module.h:9-10`
keeps `EDVR_NATIVE_MODULE_VERSION_1` and `_2` and `edvrConfigureNativeRuntime`
accepts both (native_module.cpp:284-286). There is precedent for either. No
existing ABI in src/common passes one buffer as in/out through a `void*`:
`EdvrNativeFrameInput` and `EdvrNativeFrameOutput` are two structs passed as
`(const in*, out*)` (src/common/native_frame.h:18-39, :48-49); the nearest
single-buffer precedent is the caller-prefilled `out` whose `size/version` the
callee checks (src/d3d11/native_temporal.cpp:126). The in/out buffer with echo
validation proposed below is new in this codebase and is tested as such.

Two contracts downstream are scale-shaped and must stay satisfied.
`edvrPublishNativeRenderSizing` recomputes `effectiveScale` from
`requestedScale` and both eyes' bounds and recomputes each eye's active size as
`scaledDimension(original, max, effectiveScale)`, returning FALSE on any
mismatch (.cpp:77-86), and a FALSE there fails native startup (host :1254). The
menu applies the same check in `coherentNativeRenderSizing`
(src/d3d11/menu.cpp:309-339). So whatever a per-headset entry says, it has to
be reduced to one common scale before `publishRenderSizing`, or that ABI has to
change too. This design keeps the sizing ABI at v1 and scale-shaped: a width
becomes a scale in the graphics DLL and nothing after it changes.

Once per host, not once per process. `captureRenderSettings()` latches on
`renderSettingsCaptured` (:224) and `start()` refuses a second start of the
same host (`starts!=1`, :139, :323), but `ModuleBackend::start()` builds a new
`Generation` and a new `NativeRuntimeHost` for every VR_Init attempt, up to 16
(src/openxr/native_module.cpp:24-29, :60-64). The precise condition: a
`VR_Init` while the lifecycle is `Running` returns the existing token and
starts nothing (src/openxr/runtime_lifecycle.h:41); only `shutdown()` (:68-79,
from `VR_Shutdown`) or a failed start (:61-64) returns the state to `Idle`,
after which the next `VR_Init` builds a new host (:47-50). So `open()` re-runs,
the list is re-read and the swapchains are rebuilt without a game restart only
for a `VR_Shutdown` followed by a `VR_Init`, which the native trace shows as a
`module_shutdown,` line between two `module_startup,` lines. Whether Elite's
Apply in its Graphics options performs that pair on the native path is not
recorded in this repo; a second `module_startup,` with no `module_shutdown,`
before it cannot occur, and a second `openxr_resolution,` line without the
shutdown line in between would mean something else is wrong. The retest looks
for the pair. Until it is recorded the user-facing wording is the conservative
"after a game restart" (decision 8), and the restart badge below is keyed on
what the host actually published, which stays correct across a re-init if one
turns out to exist.

Elite composes two more factors on top of the number EDVR publishes. Elite
multiplies the recommendation by its own HMD Quality (src/openvr/
system_hook.cpp:842-849; docs/anti-aliasing.md:259-264: HMD Quality 1.25
submitted 6780x6695 against 5424x5356), and under `temporal_aa = dlss` the
game's smaller submission is upscaled to the recommendation (src/d3d11/
native_temporal.cpp:198-199). The culling guard multiplies the published
recommendation by its FOV factor while adopted (src/openxr/
native_cull_guard.h:98-102; 1.2425 on the Quest 3, airlink doc :224), and while
the guard is Live the temporal pass's output target is that guard-widened size
(src/openxr/native_temporal_client.h:52-53; host :589-594), not the swapchain.
EDVR already measures what Elite actually submits: `announceEyeTextureSize` at
native_temporal.cpp:178, shown on the Status page as `Eye texture`
(menu.cpp:948-956, which needs the tangents too). HMD Quality is one value for
the whole Elite install, not per headset; no EDVR key can change that, and the
doc says so here so it is not re-asked.

## Options considered

Five identities and three units were drafted and judged across the three
revisions. What survives and what is dead:

**Runtime name plus system name, per-eye width** (this design). Both strings
are read at :1196 and :1200, before `captureRenderSettings()` at :1250:
guess-free, no reordering, no table. The runtime name does not move with any
vendor slider or preset. The system name is required because SteamVR/OpenXR
served both headsets today. The width is what "a reasonable amount of pixels"
is measured in, and a change of streaming app or vendor slider cannot multiply
it. Costs: the names carry spaces, slashes and unknown bytes, so a sanitiser
both DLLs agree on byte for byte is needed; the system strings are unrecorded
and the first build must record them; the derived height follows the runtime's
aspect, so an entry means a slightly different height on a runtime that
recommends a different aspect for the same panel (Quest 3: 0.927 over Oculus,
0.941 over VDXR, 0.935 over Steam Link), which is the correct behaviour and is
shown in the row.

ruled out: keying entries on the runtime's recommended `WxH` (revision 2),
because 2026-09-14 alone produced six recommended sizes across two headsets,
and two of them (4068x4016 at 12:50 in Frontier, 4980x4916 at 20:50 in Steam)
for one Pimax on one runtime string, so the entry would go dead between two
flights of the same headset on the same route.

**Percent of the recommendation** (revision 2's unit, kept as the documented
alternative in decision 2). It composes with a vendor preset change in the
direction a user might expect ("still 180%"), but that is exactly the property
that turned 180% into 5530x5875 (32.5 MP per eye) at 20:05 when Virtual Desktop
supplied a 3072x3264 base for the same Quest 3. A percent is a multiplier on a
number the user does not control; a width is the number itself. Under the width
unit the same 3283 on VDXR resolves to 3283x3488 (11.4 MP), which is what Sean
set.

**Per-eye megapixel budget.** One absolute number, `openxr_eye_megapixels`,
from which the host derives a scale as sqrt(budget / (w*h)). No identity, no
list, no ABI input, ~220 lines, and the pixel count is invariant across
runtimes and presets. But it answers a different question: one budget gives the
Crystal Super 3486x3442 (64% of its 5424x5356 recommendation) when the Quest 3
is at 12 MP, so Sean would want two numbers, and then the identity question
returns unchanged. The unit is a GPU-cost unit rather than what the user gets,
and it needs a second concept (the conversion from a size he likes). Grafted:
megapixels are printed beside every per-eye size in the row, the tooltip, the
log and the ini comment, because on one card driving two headsets that number
is what the value costs.

ruled out: a single megapixel budget as the shipped semantics, because Sean's
two rigs sit at 11.6 MP and 16-29 MP and one number serves neither.

**Angular density (pixels per degree) keyed on the FOV signature.** The most
honest about the panel-table trap and the only identity shared with
`cull_guard_headsets`, but it stands on `xrLocateViews` succeeding on a
created, not-yet-begun session with a QPC-converted time, which today's only
locator refuses by construction (geometry_locator.h:18) and no runtime Sean
uses has been probed for. It reorders `open()` so a live session exists on
every failure path between :1250 and :1256, bumps two ABIs (104 and 136 bytes),
and introduces a unit users must learn in the headset, while the design itself
concedes equal px/deg is not equal panel fraction, so per-headset entries
return anyway. Grafted: its "probe on the desk, not in the air" discipline; the
unpaired native diagnostic runs `open()` with provider=0 and prints every new
trace line with no flight.

ruled out: keying startup sizing on the FOV signature now, because located
views need a begun frame (geometry_locator.h:18), which comes after the
swapchains are created at native_runtime_host.h:1273, and
`XR_EPIC_view_configuration_fov` support is unprobed.

ruled out: a table of panel resolutions per headset model, because OpenXR
exposes no panel size, no runtime's `systemName` is on record, and the table is
wrong on every headset not in it.

ruled out: a `[headset:<name>]` section per headset, because it would be the
first dynamic section and five consumers must agree on it: the contract checker
fails the build on any documented template
(tools/check_config_contract.py:274-292), the runtime audit names every unknown
key each launch (src/common/config.cpp:296-329), the installer's merge labels
it "not an EDVR setting this version knows" (src/common/ iniedit.cpp:501-510),
the menu would emit a lowercased orphan header (iniedit.cpp:646-649), and a
name with a dot splits the dotted key (iniedit.cpp:582-584).

ruled out: a wildcard entry for unlisted headsets, because it reintroduces
exactly the cross-headset hazard this design exists to remove; no entry means
100% of the runtime's recommendation and nothing else.

## The recommended design

One key under `[fix]`. Its value is a list of per-headset entries, each keyed
on the sanitised runtime name and the sanitised system name, and each a per-eye
width in pixels. A headset with no entry runs at 100% of the runtime's
recommendation. The graphics DLL resolves the entry, because Config lives
there; the host passes both names and both eyes' bounds in through a v2 in/out
settings struct and gets one scale back. The 80-byte sizing ABI,
`applyRenderScale`, `publishRenderSizing`, the cull guard and the temporal path
are untouched.

### Key name

The current line, quoted exactly from edvr.ini:450, is:

```
openxr_render_scale = 1.0
```

Its current behaviour: a fraction of the runtime's recommended per-eye size,
applied to width and height alike, clamped to 0.25..2.0
(native_render_settings.h:15-16, :75-81), read once per native host start with
`Config::getFloat` (native_render_settings.cpp:44) and applied at the next VR
start; the F8 row is labelled `OpenXR res.` and shows it as a percent
(menu.cpp:1308-1313). It shipped in no public release.

Decided: it is renamed to `fix.openxr_resolution`, because the value is no
longer a scale of anything, the row label already says `OpenXR res.`, and the
key is in no public release. The rename was put to Sean with the line above
quoted, as AGENTS.md requires, and he answered yes on 2026-09-14; the rest of
this document uses the new name throughout. No `moved-from`: the old grammar (a
bare number) is refused under the new key, so there is nothing to carry across.
What happens to the two live files that still carry the old line is under
"Migration of the unreleased key".

### Unit and semantics

An entry's number is the per-eye width in pixels EDVR hands Elite through
`GetRecommendedRenderTargetSize` and uses for the swapchain. The height follows
the runtime's recommended aspect for that launch: the graphics DLL computes
`scale = width / recommendedWidth` (eye 0's `originalWidth`), and the host
applies the unchanged `clampScale` (0.25..2.0), `effectiveScale` (runtime
maximum and the 16384 D3D11 limit, over both eyes, both axes) and
`scaledDimension` to both eyes. So:

- 3283 on the Quest 3 over Oculus (1824x1968): scale 1.799890, 3283x3542 per
  eye, 11.6 MP, 180.0% of the recommendation. The width round-trips exactly:
  `scaledDimension(1824, 16384, 1.799890) = 3283`.
- 3283 on the same Quest 3 over Virtual Desktop (3072x3264): scale 1.068685,
  3283x3488, 11.4 MP, 106.9%. Not 5530x5875.
- 4980 on the Pimax over SteamVR/OpenXR at the 20:50 base (4980x4916): 100.0%,
  4980x4916, 24.5 MP; at the 12:50 base (4068x4016): scale 1.224189, 4980x4916,
  122.4%. The same pixels either day.
- A typed width above 2x the recommendation is clamped by `clampScale`: 4000 on
  1824x1968 asks for 2.193 and gets 2.0, 3648x3936; the entry keeps the typed
  4000, the row shows the derived 3648x3936 with `(capped at 200%)`, and the
  log says so. Below 0.25x the same in the other direction: 400 on 1824x1968
  gets 0.25, 456x492.
- The runtime maximum caps through `effectiveScale`: 9000 on the Pimax over
  SteamVR/OpenXR (4980x4916, max 8192x8192) asks for 1.807, gets 1.644980,
  8192x8087, 66.3 MP, shown as `(runtime cap 164.5%)`, the existing wording at
  menu.cpp:420-421. `effectiveScale` takes the minimum over both eyes and both
  axes (native_render_settings.h:99-106), so on this runtime the width cap is
  reached at 8192 wide and the height is 8087, not 8192.
- The reachable width is therefore not `maxWidth` but `min(maxWidth,
  scaledDimension(originalWidth, maxWidth, maxHeight / originalHeight))` over
  both eyes (rounded, `+ 0.5` at :89), because the height axis can bind first:
  a fixture of 4000x4500 with max 8192x8192 asked for 8000 wide resolves
  through the height (8192/4500 = 1.8204) to 7282x8192, not 8000x9000. None of
  the six recorded fixtures below reaches that case (their height caps fall at
  15185, 15420, 7659, 8298, 8299 and 16592 wide, all past the width cap or the
  2x clamp), so the self-test adds a seventh fixture that does, the round-trip
  assertion (claim 24) is stated against this effective cap, and the menu's
  step clamps to it, not to `eyes[0].maxWidth`.
- Widths are integers in 1..16384; the parser refuses anything else as a
  malformed token.
- The width's invariance is bounded by the unchanged 0.25..2.0 clamp, and the
  ini comment says "held between a quarter and twice the recommendation"
  because these cases are real: Virtual Desktop's `Low` preset (about 1440
  wide) caps a 3283 entry at 2880 with `(capped at 200%)`, and a Pimax preset
  that raised the base past four times a saved width would lift the width to
  25% of the new base. Both are the clamp doing what it did before this design;
  whether the clamp should widen for width entries is decision 9, and it is not
  widened here because 2x of a 5424 base is already 116 MP per eye.

100% is the runtime's recommendation, which is what the runtime asked for after
its own quality setting, not the panel's pixel count; the ini comment and the
tooltip say so. The row shows, beside the width, the derived WxH, the
megapixels and the percent of this runtime's recommendation, so the user sees
what the width means on the runtime that is on.

Precision: the ABI carries the scale as a float (`openxrRenderScale`).
`float(3283/1824)` carries about seven significant digits, so `1824 * scale` is
within 0.001 px of 3283 and `+ 0.5` then truncation
(native_render_settings.h:89-91) returns 3283. The self-test asserts the
round-trip for every fixture width rather than trusting this paragraph (claim
24).

### Ini lines

Replace edvr.ini:442-450 under `[fix]` with the block below. The old key line
goes; nothing else in the file moves.

```
# Per-headset OpenXR render width in pixels per eye, as runtime/system:width
# entries such as oculus/meta-quest-3:3283. Set it from the F8 menu with the
# headset on; the key for the headset you are wearing is printed in the log
# ("openxr resolution: this headset is oculus/meta-quest-3") and is the
# runtime name and headset name lowercased with everything but letters and
# digits turned into "-" -- copy it from the log rather than typing the
# names, which contain slashes. Width is the per-eye width EDVR hands
# Elite; the height follows the headset's recommended shape, and the menu
# shows the resulting size, megapixels and percent of the runtime's
# recommendation. Entries are separated by commas, up to 8:
#
#   oculus/meta-quest-3:3283, steamvr-openxr/pimax-crystal-super:4980
#
# A headset with no entry runs at 100% of what its runtime recommends, and
# a bare number on its own (180, 3283, or the older 1.8) names no headset
# and is ignored, with the log saying what to write instead. An entry with
# only the runtime half (steamvr-openxr:4000) applies to any headset on
# that runtime that has no entry of its own. The width is fixed in pixels:
# changing the headset software's quality preset or streaming app changes
# what 100% would have been, not this, within limits -- it is held between
# a quarter and twice whatever the runtime now recommends, so a very low
# streaming preset caps it at 200% and a very high one floors it at 25%,
# and the runtime's own maximum still caps it. Elite's HMD Quality and
# DLSS still compose with this value. Swapchains are fixed when VR starts,
# so a change applies after a game restart. The Status page's "Eye
# texture" line is what Elite then submits. Megapixels of a size you
# already like: width x height / 1000000. Emptying this value clears every
# headset's entry; the F8 menu's R key clears only the headset you are
# wearing.
# ui: OpenXR resolution | restart | menu performance
openxr_resolution =
```

The system tokens in the example (`meta-quest-3`, `pimax-crystal-super`) are
the grammar's shape, not recorded strings: the implementing agent substitutes
the tokens the desk pass and flight 1 record before this block is committed,
because the example is what users copy.

The first sentence (116 characters, ending at "3283.") is written for two
consumers. `gen_settings_schema.py`'s `summarise` (:426-459) takes the prose up
to the first `. ` that falls within 200 characters and would otherwise truncate
at 197 with `...` (:449, :457-459); there is no ` -- ` before that first `. `
(:446; the ` -- ` in the third sentence is past the first cut and does not
matter). The desktop settings window then draws that summary in a description
rect of (720 - 2x24 - 4 - 240 - 2x18) = 392 dp by 34 dp (kClientWidth and
kMargin at src/installer/gui.cpp:64-66; kPad and kControlWidth at
settings_view.cpp:26-29; the rect at :234-235) in the 9-point caption font
(ui.cpp:258) with `DT_WORDBREAK | DT_END_ELLIPSIS` (:396), and the longest
summary it draws today is 141 characters (`fix.head_offset_view_bridge`). The
first draft's 185-character sentence would have been ellipsised before the
example; 116 sits under the longest existing one, and the rect is added to the
GDI measurement list (claim 25) so it is measured, not assumed. The window
shows this key as a text row written verbatim (src/installer/settings.cpp:
141-147), so the grammar and an example must be in that first sentence or a
user types `180` there and only the log says why nothing changed.

There is no `# retired-default:` line. Revision 2 carried one so that a literal
`1.0` left over from a private build would be dropped by the installer's merge
(`retiredDefaults`, iniedit.cpp:207-234; the known-key path at :522-530) rather
than refused at every launch. Under the rename it is idle: `openxr_resolution`
never shipped `1.0`, and the old key's line is handled by the unknown-key path
described under "Migration". Dropping it also sidesteps a generator bug the
review found: `gen_settings_schema.py`'s `parse_ini` special-cases only `ui:`,
`dev:` and `moved-from:` (:293-307) and appends every other comment line to the
prose (:308-309), which becomes `s.description` (:320), the F8 tooltip's ini
prose (menu.cpp:1407, `d.detail`) and the settings window's detail. The proof
is in the tree today: `menu.fps_overlay_pitch`'s comment block is the single
line at edvr.ini:633, so its generated row is `{"menu", "fps_overlay_pitch",
"Readout position up", "retired-default: -16", "retired-default: -16", ...}`
(build/gen/ menu_schema.inc:137-138) -- its hint and its detail are literally
the annotation. Revision 2 cited that row as proof the line does not leak; it
is the leak. The one-line `retired-default:` branch beside `moved-from:` at
:299 that would repair `fps_overlay_pitch` is a separate cleanup, outside this
design.

Grammar: `entry (',' entry)*`; entry = `runtime ['/' system] ':' width`;
runtime and system are tokens of `[a-z0-9-]` as the sanitiser produces them,
compared byte for byte after the sanitiser is applied to the entry's own tokens
too (so a hand-typed `Oculus/Meta-Quest-3:3283` matches); width is an unsigned
integer in 1..16384; whitespace allowed around `/`, `:` and `,`; at most 8
entries; a malformed token is skipped and named once in the graphics log; an
empty value, or a value with no well-formed entry (a legacy `1.0` or `0.90`),
is zero entries. The split rule is fixed: the runtime half ends at the first
`/` and the width begins after the last `:`. Its consequence is why the comment
says to copy the key from the log: a user who types the raw runtime name,
`SteamVR/OpenXR/<sys>:4980`, gets runtime token `steamvr` and system token
`openxr-<sys>`, which matches nothing, runs at 100%, and is logged with the
right key to write; the parser self-test pins that input. The comma-locale trap
of revision 2 is gone with the decimal: a width is an integer. The example line
inside the comment deliberately omits the key name so `keys_documented`
(tools/check_config_contract.py:116-150) cannot count it as a second key line.

The `# ui:` line carries no `range` and no `percent`: the code reads the key
with `getString`, and `gen_settings_schema.py` refuses `percent` on a key that
is not a number (:515-536). The installer's settings window therefore shows the
raw list in a text row; the F8 menu is the editor.

Everything else about the key is unchanged, which is the point: the contract
checker sees one string-literal read and one documented key under `[fix]`
(READ_RE at check_config_contract.py:47), so the count stays at 255 with no
checker change; the runtime audit's `kKnownKeys` holds it once the ini is
regenerated; the installer's three-way merge treats it as a known key and keeps
the user's value verbatim; the whole-file mirror to
`%LOCALAPPDATA%\EDVR\<leaf>-<store>\` carries it across a game-update wipe.
There is no new section, no dynamic key and no audit exemption.

The Steam and Frontier installs each carry their own `edvr.ini` and their own
mirror. An entry set on one store's install does not exist on the other:
today's Frontier file says `1.0` and today's Steam file says `0.90`, and after
this design each holds its own list. That was true of every setting before; it
is now true per headset as well, and a "setting lost" report should be checked
against the store first.

### Identity and the sanitiser

The identity is the pair (runtime token, system token), where the runtime token
is the sanitised `XrInstanceProperties.runtimeName` (:1196) and the system
token is the sanitised `XrSystemProperties.systemName` (:1200). It is chosen
because:

- both are on the stack at :1196 and :1200, before `captureRenderSettings()` at
  :1250, with no reordering and no session;
- the runtime name is already in every native trace (`runtime,<name>,
  <version>`), and the system name will be from the first build;
- the runtime name does not move with any vendor slider, preset, refresh rate
  or auto-resolution; today's logs show `SteamVR/OpenXR` recommending
  4068x4016, 2528x2704 and 4980x4916 under one string;
- the system name is the only value at hand that separates the two headsets
  `SteamVR/OpenXR` served today (20:14 versus 20:50);
- it needs no hardware table.

The sanitiser is one function, `headsetToken`, in the shared header described
below, used by the graphics DLL to match, by the menu to write and by the host
trace to print, so the three cannot disagree. Its rule: take the bytes up to
the first NUL; lowercase ASCII letters; keep ASCII letters and digits; turn
every run of any other byte (space, `/`, `-`, `_`, `.`, `,`, parentheses, and
every byte above 0x7F) into a single `-`; trim leading and trailing `-`;
truncate to `kHeadsetTokenMax = 30` bytes and trim a trailing `-` again. So
`SteamVR/OpenXR` is `steamvr-openxr`, `Oculus` is `oculus`, `VirtualDesktopXR`
is `virtualdesktopxr`, `Pimax OpenXR` is `pimax-openxr`, and a system name of
`Meta Quest 3` would be `meta-quest-3`. The function is idempotent (a token
sanitises to itself), so an entry's own tokens can be passed through it before
comparison.

The maximum is 30, not the 40 of the first draft, and it is derived from the
buffers the key has to fit: a full key is at most 30 + 1 + 30 = 61 bytes, which
fits the Status page's 63-byte `MenuLine.right` (menu_panel.h:64; `statusLine`
copies `sizeof - 1`, menu.cpp:931-932) on its own line with no cut, and eight
entries of at most 61 + 1 + 5 = 67 bytes plus separators are at most 552 bytes,
which keeps the graphics-log line under `Log::note`'s 1200-byte line buffer
(log.cpp:356) with its prose. The longest recorded runtime token is
`virtualdesktopxr` (16) and the longest plausible system token, a SteamVR
driver-shaped name (below), is about 25; 30 leaves room and no recorded name is
cut.

Two shapes of system name are pre-decided so flight 1 cannot stall on them.
First, SteamVR/OpenXR's `systemName` may be driver-shaped, `SteamVR/OpenXR :
lighthouse` or similar, which sanitises to `steamvr-openxr-lighthouse`; the
token is kept as the sanitiser yields it, with no stripping of a leading
runtime-token prefix, because a second rule is a second thing two DLLs must
agree on and the log prints the key either way. If it turns out that SteamVR
reports one driver-shaped name for both headsets (the Quest 3 over Steam Link
and the Pimax both arrive through a driver), the two share an entry on that
runtime and there is no third OpenXR identity field to split them: `vendorId`
names a vendor and `systemId` is a per-instance handle. Flight 2b records
which. Second, a name that sanitises to the empty string (all punctuation, or
empty) yields an empty token; the worn key is then the runtime token alone, the
parser still rejects `oculus/:W` (a slash with nothing after it is malformed),
so the only entry that can match is a runtime-only one under rule 2, and it is
reported as `matched=2`; the menu writes a runtime-only entry in that one case,
and the graphics-log line says `(runtime reported no system name)` so the shape
is visible.

The installer already has this rule in wide characters: `slugOf` at
src/installer/mirror.cpp:37-48 lowercases, keeps `iswalnum`, collapses other
runs to one `-` and trims the trailing one, for the mirror folder's
`<leaf>-<store>` name. The graphics DLL's `mirrorDir()` at menu.cpp:609-645,
used by `menuIniWrite` at :686-688 to refresh the mirror copy on every write,
does not repeat that rule: it hard-codes three store slugs from path substrings
(`\steamapps\`, `\epic games\`, `frontier_developments\products`, :618-621) and
otherwise looks for the one folder carrying the leaf. `slugOf` is
`iswalnum`-based (locale-dependent for bytes above 0x7F) and lives in the
installer, so it is not shared; the new function is ASCII-only, narrow,
header-only, and its tests pin the non-ASCII case explicitly (claim 22).

The token is computed from the 64-byte copy the host puts in the ABI (63 bytes
plus NUL), not from the full 128- or 256-byte string
(`XR_MAX_RUNTIME_NAME_SIZE` at openxr.h:138, `XR_MAX_SYSTEM_NAME_SIZE` at
:135), on both sides: the host sanitises its own `runtimeLabel[64]` /
`systemLabel[64]` for the trace, the graphics DLL sanitises the echoed fields.
A name longer than 63 bytes therefore yields the same token in the log and in
the DLL, by construction; the raw trace line carries the full string.

The two Pimax routes remain distinct (`pimax-openxr/<sys>` versus
`steamvr-openxr/<sys>`), as do the three Quest 3 routes (`oculus/<sys>`,
`virtualdesktopxr/<sys>`, `steamvr-openxr/<sys>`), and the F8 menu writes a
separate entry for each the first time it is stepped on that route. That is the
intended behaviour: the width a user wants over Virtual Desktop at its base may
differ from the one over Air Link, and each is set once.

### Matching rule and fallback

Given the worn pair (rt, sys), the resolver takes, in order:

1. the first entry whose runtime token equals rt and whose system token equals
   sys (`matched = 1`);
2. else the first entry whose runtime token equals rt and whose system half is
   absent (`matched = 2`), the hand-written escape hatch for a runtime a user
   would rather set once for every headset on it;
3. else no match (`matched = 0`): 100% of the runtime's recommendation.

A `runtime/system` entry beats a `runtime`-only entry wherever either sits in
the list. An entry whose system half is present but different is never used.
The F8 menu always writes the full `runtime/system` entry for the worn headset
(and never a runtime-only one, except the empty-token case above), so a
runtime-only entry exists only if a user typed it, and decision 3 asks whether
to accept them at all.

No match is 100% of the runtime's recommendation, always: the size the
headset's own runtime asked for, with no EDVR assumption layered on it. It is
deliberately not "the global value", "the last value set" or another entry's
value, each of which would carry a Quest 3's 3283 onto a Pimax or a Pimax's
4980 onto a Quest 3. There is no wildcard. `matched=0` is traced by the host,
and the graphics log names the key to add as a template with the width left to
the user and lists the entries already saved, so a new route reads as "this
headset is steamvr-openxr/<sys>; saved: oculus/<sys>:3283; add
\"steamvr-openxr/<sys>:<width>\" (4980 is 100%)" rather than as a silently lost
setting. A template rather than a filled-in width, because a width equal to the
recommendation would be an instruction to add a line that changes nothing.

### ABI: EdvrNativeRenderSettings v2

In `src/common/native_render_settings.h`, replacing `VERSION_1` in place (the
a8a0e57 shape; both DLLs ship together and nothing native is public, so the
dual-accept shape of native_module.h is not needed here):

```
#define EDVR_NATIVE_RENDER_SETTINGS_VERSION_2 2u

typedef struct EdvrNativeRenderSettings {
    uint32_t size;                       // in/out: sizeof == 184
    uint32_t version;                    // in/out: 2
    EdvrNativeRenderViewBounds eyes[2];  // in: host renderBounds[] (original
                                         //     and D3D-capped max per eye)
    char     runtimeName[64];            // in: XrInstanceProperties
                                         //     .runtimeName, truncated, NUL
    char     systemName[64];             // in: XrSystemProperties.systemName,
                                         //     truncated, NUL-terminated
    float    openxrRenderScale;          // out: width / eyes[0].originalWidth
                                         //     via clampScale; 1.0 when no
                                         //     entry matches
    uint32_t matchedEntry;               // out: 0 none, 1 runtime/system,
                                         //     2 runtime-only
    uint32_t entryCount;                 // out: well-formed entries parsed
    uint32_t reserved;                   // 0 in and out
} EdvrNativeRenderSettings;

static_assert(sizeof(EdvrNativeRenderSettings) == 184, "...");
```

The typedef `EdvrQueryNativeRenderSettings(version, size, void* output)` is
kept and the buffer becomes in/out. This single-buffer shape is new in this
codebase (the frame ABI uses two structs, native_frame.h:48-49); it is kept
because the host's capability check looks the export up by name only (:1235),
the call site stays one line, and the echo rule below turns a field-order slip
into a validation failure rather than a wrong size. The graphics side
SEH-copies the struct in (the `copySizingInput` shape at
native_render_settings.cpp:12-19), requires `size == 184`, `version == 2`,
`reserved == 0`, both eyes' bounds non-zero with original <= max, and both
names NUL-terminated within 64 bytes; anything else returns FALSE, which is an
ABI misuse and correctly fails startup at host :247 rather than flying a wrong
size. It resolves through the shared header and writes the whole struct back
with the input fields echoed unchanged. The host validates the answer as at
:244-247 plus `matchedEntry <= 2`, `entryCount <= 8`, and the echoed `eyes[]`
and names byte-equal to what it sent. The one change from revision 2's struct
is the meaning of `matchedEntry` (0/1/2 rather than 0/1), so the trace can say
which rule matched.

The SEH constraint shapes the file: MSVC refuses `__try` in any function that
needs object unwinding (C2712), and the v2 body uses `std::string`
(`Config::getString`, the parser's skipped-token list, the tokens) and
`Log::note`. The v1 body gets away with wrapping `Config::getFloat` directly
(:39-51); v2 cannot. The copy-in and copy-out live in two `noexcept` helpers
exactly like `copySizingInput` / `writeSizingOutput` (:12-28), and the parse,
the resolve and the log line sit in plain C++ between them.

`EdvrNativeRenderSizing` stays at v1, 80 bytes. The resolved width is reduced
to one common scale before it leaves the graphics DLL, so the coherence checks
at native_render_settings.cpp:77-86 and menu.cpp:309-339 hold unchanged, and
the menu reads the worn headset's recommendation from
`sizing.eyes[0].originalWidth/Height`, which the host already publishes at
:1254.

### The shared header

A new header-only file `src/common/openxr_resolution_entries.h`, namespace
`edvr::native_render`, with no Config or Log dependency so the graphics DLL,
the menu, the host trace and the tests share one implementation:

```
constexpr size_t kHeadsetTokenMax = 30;   // derived: 30+1+30 = 61 < 63
constexpr size_t kResolutionEntryMax = 8;

std::string headsetToken(const char* raw, size_t maxBytes);  // sanitiser
std::string headsetKey(const std::string& runtimeToken,
                       const std::string& systemToken);     // "rt/sys" or "rt"

struct ResolutionEntry { std::string runtime, system; uint32_t width; };
size_t parseResolutionEntries(const char* value, ResolutionEntry out[8],
                              std::string* skipped);       // skipped tokens
uint32_t resolveResolutionWidth(const ResolutionEntry*, size_t count,
                                const std::string& rt, const std::string& sys,
                                uint32_t* matched);         // 0 = no match
float widthToScale(uint32_t width, uint32_t recommendedWidth);
                                   // width / rec, 1.0 when either is 0
bool mergeResolutionEntry(const std::string& list, const std::string& rt,
                          const std::string& sys, uint32_t width,
                          std::string* out);
std::string removeResolutionEntry(const std::string& list,
                                  const std::string& rt,
                                  const std::string& sys);
std::string formatResolutionEntries(const ResolutionEntry*, size_t);
uint32_t bareNumberToWidth(const char* value,
                           const EdvrNativeRenderViewBounds eyes[2]);
                                   // 1.8 -> 180% of rec; 180 -> same;
                                   // 3283 (a plain width) -> 3283;
                                   // 0 when out of range or unparsable
std::string formatPercent(float);                    // "180", "106.9"
                                   // replaces menu.cpp:364-369 scalePercent
double megapixels(uint32_t w, uint32_t h);           // w*h/1e6
```

`mergeResolutionEntry` rewrites the first entry with that key in place, drops
any later duplicate of it (resolve takes the first, so a duplicate left behind
would show under `Saved:` and never apply), appends when absent, preserves the
order and bytes of every other well-formed entry and canonicalises spacing to
`rt/sys:W, rt/sys:W`; malformed tokens in the existing list are dropped on a
menu write and named in the write log line. It returns false, writing nothing,
when the key is absent and eight entries already exist; the menu then refuses
the edit with `Eight headsets saved; remove one in edvr.ini first.` (51
characters, under the Status page's 63-byte last-write field) in the Status
page's last write. `removeResolutionEntry` removes only the exact `rt/sys` key;
a runtime-only entry the headset would then fall back to is left alone and
named in the hint.

`bareNumberToWidth` exists for one log line: a legacy `1.8` (a fraction in
0.25..2.0) or `180` (a percent in 25..200) is converted to the width it would
have meant on the worn headset, and a plain integer that is neither (`3283`) is
taken as the width the user meant. It returns 0, and the log line says
`"<value>" is out of range for this headset` instead of offering a width,
whenever the result would fall outside a quarter to twice the recommendation or
above the effective cap: a bare `3..24` or `201..455` on a 1824 base would
otherwise be read as a width below the 0.25 clamp and offered as such, and a
bare `0.90` on a 4980 base is offered as 4482 only because that is inside the
range. It is never used to size anything.

`formatPercent` replaces `scalePercent` at menu.cpp:364-369 (round to 0.1, drop
a zero decimal, append `%`): the menu's copy is deleted and its callers include
the header, so the label, the hint, the tooltip and the graphics-log line
cannot disagree on `180` versus `180.0`. Two implementations of one format is
how they would.

### Host changes

`src/openxr/native_runtime_host.h`, `open()` and `captureRenderSettings()`
only; nothing moves.

- After :1200, trace the system properties. The probe's line
  (openxr_probe.cpp:145-149) puts the name in the middle of the CSV; here the
  name goes last, because `XrSystemProperties.systemName` is up to 256 bytes
  and may contain commas, and the fixed fields must not shift:
  `nativeTracePrintf("system,vendor=%u,max_size=%ux%u,name=%.*s\n",
  properties.vendorId, properties.graphicsProperties.maxSwapchainImageWidth,
  properties.graphicsProperties.maxSwapchainImageHeight,
  XR_MAX_SYSTEM_NAME_SIZE, properties.systemName);`. Placed there so the line
  is present even when a later step fails, and so the unpaired diagnostic
  prints it with no flight. Any grep for it anchors on `system,vendor=`:
  existing lines already start with `system_geometry_query,` (:309) and
  `system_unavailable,` (:1019).
- Immediately after it, the key line the user copies:
  `headset_key,<rt>/<sys>,runtime=<raw runtimeName>,system=<raw systemName>`,
  where `<rt>/<sys>` is `headsetKey(headsetToken( runtimeLabel, 64),
  headsetToken(systemLabel, 64))` computed from the same 64-byte copies the ABI
  carries, and the two raw fields are the full strings. Only the first two
  fields are machine-readable (a raw name may contain a comma); the raw fields
  are the tail and are for eyes. The sanitiser is the shared header's, included
  by the host, so the token in this line is byte-equal to the one the graphics
  DLL matched on.
- :1220 gains the per-view cap the host actually applies:
  `size,%u,%u,%u,max=%ux%u` with `renderBounds[eye].maxWidth/Height` (the
  D3D-clamped value). The first four fields are unchanged; no tool parses this
  line (grep of tools/ for `"size,` finds none).
- Two new members beside `requestedRenderScale`: `char runtimeLabel[64]` and
  `char systemLabel[64]`, filled from `ip.runtimeName` and
  `properties.systemName` at :1196 and :1200 (truncated, NUL-terminated), reset
  in `close()` next to `renderSettingsCaptured` (:1063-1066).
- `captureRenderSettings()` (:223-253): the v2 request is built by a pure
  function `buildRenderSettingsRequest(renderBounds, runtimeLabel,
  systemLabel)` and the answer checked by a pure
  `validateRenderSettingsAnswer(sent, answer)` (both in the header, no invoke,
  no handle), so the self-test can exercise them without the dispatcher.
  `buildRenderSettingsRequest` zeroes the whole 184-byte struct (`memset`)
  before the two `strncpy` calls, because the host's echo check compares the
  full 64 bytes of each name field and a name shorter than 63 bytes would
  otherwise leave stack bytes after the NUL that the graphics DLL's copy-back
  reproduces or not by accident; the self-test pins it with a name shorter than
  63 bytes and asserts every byte after the NUL is zero. :243 invokes with
  `VERSION_2`; :251 becomes a trace of the form
  `openxr_resolution,provider=1,key=<rt>/<sys>,headset=%ux%u,
  matched=%u,entries=%u,requested=%.6f` with
  `renderBounds[0].originalWidth/Height`, the answered `matchedEntry` and
  `entryCount`. The line is renamed from `openxr_render_scale,` so an old
  build's trace and a new one cannot be confused (never-ran table below). The
  unpaired branch (:232-236) prints `openxr_resolution,provider=0,
  key=<rt>/<sys>,headset=%ux%u,matched=0,entries=0,requested=1.000000`.
- `applyRenderScale()` (:254-270): after each eye's existing trace, one more
  field pair on the same line, `,megapixels=%.2f`, from the scaled size.
- The capture-once latch (:224) is unchanged. The list is read once per host,
  and a host lives for one VR start (see "Once per host, not once per process"
  above): a `VR_Shutdown` followed by a `VR_Init` builds a new host, re-reads
  the list and rebuilds the swapchains with no game restart; a `VR_Init` while
  running does nothing (runtime_lifecycle.h:41). The badge below stays honest
  through either because it compares against the published sizing, not against
  a string snapshot.

### Graphics DLL changes

`src/d3d11/native_render_settings.cpp`, `edvrQueryNativeRenderSettings`
(:34-52): accept `VERSION_2` / 184 only; SEH-copy the input in a `noexcept`
helper; validate; compute `rt = headsetToken(in.runtimeName, 64)` and `sys =
headsetToken(in.systemName, 64)`; read
`Config::get().getString("fix.openxr_resolution", "")` (a string literal, so
the contract checker still counts one read); parse; resolve for (rt, sys);
`width` is the resolved width or, with no match, `eyes[0].originalWidth`; fill
`openxrRenderScale = clampScale(widthToScale( width, eyes[0].originalWidth))`,
`matchedEntry`, `entryCount`; SEH-write back in the second helper. Then one
`Log::get().note` line, computing the per-eye result from the passed-in bounds
with the shared `effectiveScale` / `scaledDimension` so the number in the
graphics log is the number the host will use. The line is bounded by
construction: `Log::note` formats into a 1200-byte buffer with a truncation
marker (log.cpp:356-357), the `Saved:` list is at most 552 bytes with
`kHeadsetTokenMax = 30` (eight entries of at most 67 bytes plus separators),
and the prose around it is under 400, so the whole list is printed on the one
line and nothing is cut; the self-test formats the line for eight maximal
entries and asserts its length is under 1100.

The graphics log is open by then, settled: `initOnceCallback` opens it on the
first D3D11 export call (src/d3d11/d3d11_proxy.cpp:273-277, reached from
`D3D11CreateDevice` :556 and `D3D11CreateDeviceAndSwapChain` :583), and the
host is not even constructed until the game device has Presented
(native_module.cpp:48-51, :60). The one way the line is absent when the code
ran is `log.enabled = 0`, where `Log::open` returns without opening
(src/common/log.cpp:134) and `note` is a no-op by design; the host trace
carries `key=` / `matched=` regardless. The comment at :31-33 ("after Config,
before swapchains") stays true.

Beside `g_sizing` (:9-10), under the same mutex, a small non-exported record of
the last v2 input: `g_labels {runtimeName[64], systemName[64],
runtimeToken[31], systemToken[31], valid}`, written at query time and cleared
on exactly the condition that clears `g_sizing`: a `valid = 0` publish whose
`generation` matches the published one, or arriving when nothing is published
(`!g_sizing.valid || candidate.generation == g_sizing.generation`, :65). The
v2 settings input carries no generation of its own (the query runs at :1250,
before the sizing publish at :1254 mints one), so the labels borrow the
sizing's: a stale host's `valid = 0` publish for an older generation, which :65
already ignores for the sizing, then leaves a newer host's labels alone too,
and the two cannot fall out of step. The one window in which a matching `valid
= 0` could clear a newer host's labels, between that host's query at :1250 and
its publish at :1254, is closed by the lifecycle: the old host is stopped (and
its `close()` has published) before `RuntimeLifecycle::init` lets a new one
start (runtime_lifecycle.h:41-47, :61-64). The accessor is
`nativeRenderLabels(...)`, declared in a d3d11-local header for menu.cpp.
`SystemRead` is unreachable from src/d3d11 (grep for `systemName` in src finds
only native_runtime_host.h:1261, openvr_system.cpp:233 and system_source.h:30),
so this is the only source the menu has. Before the first query, and after
close, it reads `valid = 0` and the menu shows `unknown`, which is
distinguishable from any token a runtime could produce.

`edvrPublishNativeRenderSizing` and `edvrQueryNativeRenderSizing` are
unchanged.

### Menu

`src/d3d11/menu.cpp`, inside the existing `isOpenxrRenderScaleRow` special case
(:304-307; the `strcmp` against `"openxr_render_scale"` at :306 follows the
rename). The row keeps its slot on the Performance page and its shape; the
visible change is that its value is always the worn headset's, and is a width.
Every site that reads this row's value is listed, because the generated
`MenuRowDef` is kind Text with empty bounds and each of these otherwise falls
through to the Text wording.

- A helper `currentHeadset(std::string* rt, std::string* sys, uint32_t* w,
  uint32_t* h)` that calls `readNativeRenderSizing` (:341) and
  `nativeRenderLabels` itself, returning the tokens and
  `sizing.eyes[0].originalWidth/Height` when the sizing is coherent and the
  labels valid. It reads live rather than from `s.renderSizing`, because that
  snapshot is refreshed only while the menu is open (:2565-2576) and two of the
  sites below run with the menu closed.
- A helper `resolvedWidth(const std::string& value)`: parse, resolve for the
  current tokens, and return the width (the recommendation when no entry or no
  sizing), with the `matched` rule.
- A helper `renderScaleRowPending(const std::string& value)`: compute the
  active width the on-disk value would produce for the published bounds,
  `scaledDimension(eyes[0].originalWidth, eyes[0].maxWidth,
  effectiveScale(clampScale(widthToScale(resolvedWidth(value),
  eyes[0].originalWidth)), eyes, 2))`, and compare it as an integer with
  `sizing.activeWidth[0]`, the width the host built for this VR start. No
  tolerance is needed: both sides are integers from the same arithmetic. A
  typed width above 2x resolves to the same active width as 2x, so it badges
  nothing when nothing would change. With no coherent sizing it returns false:
  no restart is known to be needed, the row reads `unavailable`, and
  `pendingRestartCount` (:917, consumed at :1046 and :1250) does not count it.
- Label (:1308-1313): `OpenXR res. 3283 px` from the width resolved for the
  current tokens; `OpenXR res. 1824 px` when no entry matches (the
  recommendation); `OpenXR res.` with value `unavailable` when there is no
  native sizing (as today at :398). The 466-pixel label area held `OpenXR res.
  200%` at 344 pixels (docs/openxr-resolution-2026-09-14.md:71-74); `OpenXR
  res. 3283 px` is three characters longer and `OpenXR res. 16384 px` four, and
  both are re-measured with the same GDI check before the build is called done
  (claim 25). If the longer one clips, the label drops `px` and the hint
  carries the unit.
- Value column (:394-406): unchanged in shape, the per-eye pixels after restart
  from `eyeDimensions(sizing, scale)` with the resolved scale (`3283x3542`;
  `L16384x16384` measured 269 of 304 pixels, resolution doc :74-75).
- Concise hint (:1355-1358, the 94-pixel area): `After restart: 3283x3542 (11.6
  MP, 180%). Active: 3283x3542 / eye.` and, with no entry, `Not set for this
  headset: 100% = 1824x1968 (3.6 MP) / eye.`; when the width was capped, `(11.6
  MP, 200% cap)` or `(66.3 MP, runtime cap 164.5%)`. The hint box is two lines
  (menu_panel.cpp:278 `hintH`, :574-575 `DT_WORDBREAK` with no ellipsis, so a
  third line is clipped silently), and the longest current distinct-eye hint
  already uses 92 of 94 pixels (resolution doc :75-76). Decided up front: when
  the two eyes differ, the megapixels and percent move to the tooltip and the
  hint keeps the existing `L WxH R WxH` form; the same-eye variant, the
  distinct-eye variant and the longest same-eye variant, `After restart:
  8192x8087 (66.3 MP, runtime cap 164.5%). Active: 4980x4916 / eye.` (80
  characters against the current longest two-line hint of 92 of 94 pixels, so
  it is expected to fit and is not assumed to), are all re-measured with the
  GDI width check before the build is called done.
- Tooltip facts line (:1374-1387): for this row it prints `Range: a quarter to
  twice the recommended width, within runtime limits.  Shipped 100% (no entry).
  Applies after a game restart.` instead of the Text default (no Range,
  `Shipped (empty)` from `displayValue` :549-551).
- Tooltip body (:1388-1389) after the facts line:
  ```
  This headset: oculus/meta-quest-3 (Oculus, Meta Quest 3). Runtime
  recommends 1824x1968 per eye (3.6 MP); 100% is that, not the panel.
  After restart: 3283 wide = 3283x3542 per eye (11.6 MP, 180%).
  Active: 3283x3542.
  Elite submits 2134x2302 (0.65x of the active size).
  Saved for: oculus/meta-quest-3:3283 (this headset),
  steamvr-openxr/pimax-crystal-super:4980.
  ```
  The tokens and raw names come from `nativeRenderLabels` (`unknown` before the
  first query). `Elite submits` comes from `eyeTextureSize()` alone, not the
  Status line's pairing with `eyeTangents` (:950), so it is present even when
  the tangents were refused by the packing check
  (src/common/frame_flag.cpp:599-602); on such a rig the Status page's `Eye
  texture` line reads as not published while this clause is printed, and that
  is the two sources differing on presence, not a bug to chase in flight 2. The
  denominator is the size Elite was actually given: `sizing. activeWidth[0]`
  while the guard is off or size-only, and `activeWidth[0] x factorW` while it
  is Adopting or Live, because then `cullGuard.recommended()` is the runtime
  size times the guard factor (native_cull_guard.h:98-102) and the host
  publishes that to Elite through `geometry.recommend` (host :589-594), so
  Elite's submission is `active x guardFactor x HMD Quality` and dividing by
  the active size alone would read `x1.24` at HMD Quality 1.0 on the Quest 3
  (the airlink doc's 1.2425 factor). `decodeCullGuardState`
  (frame_flag.h:496-501) already gives menu.cpp the stage and both factors
  (menu.cpp:958), so the clause reads `Elite submits 2652x2302 (x0.65 of what
  it was given; guard x1.24)` when the guard is live (4079x3542 given, the
  airlink doc's :224 figure, times 0.65) and `Elite submits 2134x2302 (x0.65 of
  the active size)` when it is not, and the guard factor is named rather than
  folded so the two numbers can be checked against the Status page's guard
  line. The clause is omitted until published and never shown as 1.0x by
  default. The ratio is a measured linear fraction, not Elite's slider value
  (the repo does not pin the slider to the fraction: system_hook.cpp:848-851
  versus docs/performance.md:1246). `(runtime cap NN%)` stays as at :420-421.
  The popup buffer is 1024 bytes (`MenuContent.popup`, menu_panel.h:127; the
  comment at menu.cpp:1367-1369 still says seven hundred and is corrected in
  passing) and it scrolls, with the facts first and the ini prose last
  (:1367-1372, :1407). The facts and this body are about 140 and 330 bytes with
  two entries; a `Saved for:` list of eight 67-byte entries would add about 550
  and push the ini prose out of the buffer entirely, so the tooltip lists at
  most four entries and then `and N more (see edvr.ini)`; the log line carries
  the whole list.
- Editing: `stepRow` (:1552-1588) for this row steps the resolved width by 100
  px on the grid of round hundreds (`kResolutionStep`; Sean's choice on
  2026-09-15, replacing the first build's 5%-of-recommendation grid, which gave
  88 px on the Quest 3 and 248 on the Pimax and never landed on a number anyone
  would type). A press moves to the adjacent hundred in the direction pressed,
  `mult` hundreds with Shift (:1575), and the result is clamped to the
  quarter-to-double range and to the effective cap (`min` over both eyes and
  both axes, the Unit section above, not `eyes[0].maxWidth` alone); a press
  never moves against its own direction, so Right from a width already at or
  above the clamp writes nothing. Stated so a flight cannot misread it: from
  the 1824 recommendation Right gives 1900, 2000, ... 3200, 3300, and stops at
  3648 (the 2x clamp); a typed 3283 steps to 3300 or 3200. `beginEdit`
  (:1867-1883) seeds the buffer with the resolved width as digits, not the list
  (so `kEditMax = 40`, :137, :1872, never truncates it); `editBufferBad`
  (:1853-1865) and `commitEdit` (:1897-1916) treat it as an unsigned integer in
  1..16384; both write the result of `mergeResolutionEntry(r.value, rt, sys,
  width)` through `applyChange` (:1499-1515) and the existing one-value merge
  (`menuIniWrite` :650-699), so the other headsets' entries are preserved
  (re-serialised in the canonical `rt/sys:W` spacing and case, as the header
  section says -- `Oculus/Meta-Quest-3:3283` comes back as
  `oculus/meta-quest-3:3283`, the same key -- never resized or re-keyed).
  Stepping moves to the ADJACENT grid point in the direction pressed, Shift to
  the fifth, so an on-grid width never skips a point and Shift moves exactly
  five; a press never moves against its own direction (Right from a stored
  width above the clamp writes nothing rather than the lower clamped value).
  With no coherent sizing or invalid labels, editing is refused and
  `s.lastWrite` (the Status page's last-write line, :1511, a 63-byte field)
  reads `No headset sizing published yet; try again in a moment.` (55
  characters). The "moment" covers the F8-open-before-VR-init window: the
  per-tick refresh at :2565 picks the sizing up within a second of the host
  publishing it.
- Write log and last write, the eleventh site: `drainWrites` (:1533-1538)
  prints `w.job.before` and `w.job.value` whole, which for this row are the two
  lists, and `applyChange` (:1511) puts the whole new list in `s.lastWrite`,
  which `statusLine` cuts at 63 bytes without a marker. For this row the job
  carries the headset key and width beside the lists, the write log reads
  `menu: fix.openxr_resolution oculus/meta-quest-3 (none) -> 3283 (list now
  oculus/meta-quest-3:3283, steamvr-openxr/pimax-crystal-super: 4980; written
  to edvr.ini; applies after a game restart).`, and `s.lastWrite` reads
  `oculus/meta-quest-3 = 3283` (at most 61 + 8 = 69 bytes for two maximal
  tokens, cut by the field only in that pathological case; the key is on its
  own Status line above it in full).
- Reset (:1983-1992 and the prompt at :1394): for this row `R` writes
  `removeResolutionEntry(r.value, rt, sys)` for the current key, not
  `d.shipped`, which would clear every headset; the prompt reads `Press R again
  to remove this headset's entry (back to 100%).`, or `(back to
  steamvr-openxr:4000)` when a runtime-only entry would then apply. The badge
  after a removal compares the resulting width against the active width, so
  removing the entry on a rig running at 3283 badges a restart and removing a
  never-applied entry does not.
- Restart badge, all three string-compare sites: `applyChange` (:1509),
  `drainWrites` after a failed write (:1544-1546) and `menuNoteConfigReloaded`
  (:2499-2500, which runs on every config reload from vscreen.cpp:4280, menu
  open or closed). Each sets `r.pending = renderScaleRowPending(value)` for
  this row instead of `value != snapshot`. A fourth recompute runs in the
  per-tick block at :2571 when the sizing snapshot changes, so a re-init that
  republishes sizing while the menu is open clears or raises the badge without
  a write. A hand edit to another headset's entry, a canonicalising rewrite of
  the list, the snapshot ordering at :908-915 and an in-process VR re-init then
  cannot badge a restart that is not needed, and the badge cannot lie about
  what is running.
- Reload toast (:2495): `displayValue` truncates a Text value over 28
  characters to `substr(0, 27) + "~"` (:552), so a two-entry list would toast
  as `oculus/meta-quest-3:3283, s~`. For this row the toast reads `OpenXR res.:
  3283 wide for this headset (after restart)` from the resolved width, or
  `OpenXR res.: list changed (after restart)` with no sizing.
- Audit line (:2503-2506), armed by the pending flag above: for this row the
  wording is `edvr.ini: fix.openxr_resolution now gives this headset
  (oculus/meta-quest-3) 3283 wide on disk but is read when VR starts; the
  running value is 1824 wide (100%). Restart the game to apply it.` with the
  resolved width and `sizing.activeWidth[0]`, not the raw list and snapshot
  strings.
- Status page (:941-946): three new lines under `Runtime`, each sized for the
  63-byte `MenuLine.right` (menu_panel.h:64; `statusLine` :929-932 copies 63
  and cuts silently) at the longest recorded runtime plus a 30-byte system
  token, not for the Quest example. `Headset` / `Meta Quest 3 via Oculus` (raw
  names, for eyes; a raw pair longer than 63 is cut, and the key line below is
  the one that matters); `Headset key` / `oculus/meta-quest-3` (the sanitised
  key alone, at most 61 bytes by construction, never cut); `Recommended` /
  `1824x1968 / eye (3.6 MP)` (24 characters). Before the first query the first
  two read `unknown`. The key line is what a user with the desktop settings
  window or Notepad open copies; the first draft's single line (`Headset: Pimax
  Crystal Super via SteamVR/OpenXR (steamvr-openxr/pimax-crystal-super),
  4980x4916`, 87 characters) would have been cut mid-key. The three lines are
  drawn only while `nativeMenuActive()` (the Runtime line's own predicate): on
  SteamVR and OpenComposite no v2 query ever runs, so they would read `unknown`
  for the whole session, and the page shows one line, `Headset` / `native
  OpenXR only (not in use on this runtime)`, instead; the row's refusals on
  those runtimes read `Native OpenXR only; this runtime ignores it.` (hint) and
  `not written: native OpenXR only` (last write), never the "moment" wording,
  which is the native path's. Two constants the lines depend on, recorded so
  the next Status line does not push `Last write` off silently: `statusLine`
  drops lines past `kMenuMaxLines` (menu_panel.h), raised from 14 to 16 because
  the native Status page is now fifteen `statusLine` calls and the fifteenth is
  `Last write`, where this row's refusals and confirmations appear; and the
  page is drawn `compact` (row pitch 1.7 cap, the Monitor page's), because at
  the two-cap pitch the raster's 2048-px height guard (`rasterise`,
  menu_panel.cpp) trips from a 51-px cap with fifteen rows, which is the Pimax
  at the default 1.1 deg, and a page it refuses keeps its previous bitmap; the
  guard now logs once (`menu panel: a WxH layout ... is outside the raster's
  ... box`) so a stale page is distinguishable from a missing one.

That is eleven sites (label, value, hint, tooltip facts, tooltip body,
step/edit, reset, badge x3 recomputes plus the tick, toast, audit, Status,
write log), all in untested menu.cpp; a mistake there is found in the headset.
The generated `MenuRowDef` for the row comes out of `gen_settings_schema.py` as
kind Text with empty bounds, which is why the numeric behaviour is hard-coded
in the special case.

The installer's settings window (settings_view.cpp) shows the row as a text row
written verbatim through the same merge, with the grammar in its summary
sentence, and it needs one change. `recommendLink` (:363-367) shows a `reset`
link whenever the row's value differs from the recommended one and the
recommended value equals the shipped one (:365), which for this key is the
empty string, so the link appears as soon as any entry exists, and one click
(`applyValue(... def->recommended)`, :742) writes the empty value: every
headset's entry gone in one click, the operation the F8 `R` key was specialised
to avoid. The window cannot know which headset is worn (it runs with no VR
session), so it cannot offer a per-headset reset. The change is in
`recommendLink`: no link for a Text row whose recommended value is empty,
because "reset to nothing" is what clearing the field already does and a
one-click version of it is a hazard, not a convenience. The row keeps its text
box, so a user who wants the list gone can still empty it by hand, and `# ui:
hidden` is not an option: the generator drops a hidden key from the menu rows
as well as the window (gen_settings_schema.py:653, `not s.hidden`), which would
remove the F8 row. The ini comment names the window's behaviour either way, and
the settings app backs the file up to `settings-<timestamp>` before every write
(src/installer/settings.cpp:250-253), so a click that does land is recoverable.

### Migration of the unreleased key

The key is absent from v0.16.2, so the only files carrying it are Sean's two
installs (`1.0` in Frontier, `0.90` in Steam, as read above), their two
mirrors, and any tester who built main since 2026-09-14. Nobody is asked to
hand-edit a live `edvr.ini` before the retest; both installs start at 100% of
the recommendation on every route and one F8 step on each headset writes its
entry. The Steam file is the one to watch: Sean chose `0.90` there after the
last Pimax flight, and the first launch of this design runs the Pimax at 100%
(4980x4916) instead of the 90% he chose (4482x4424), a visible change the
retest expects and records. Under the rename the old line is unread rather than
refused, so that launch prints the "no entry" line with the 100% template, not
a bare-number line naming 4482; Sean's 90% is one F8 step to put back, and this
paragraph is where he is told so.

With the key renamed, `openxr_render_scale = 1.0` (Frontier) and `= 0.90`
(Steam) are lines this build does not read. The runtime audit names each once
per process, `edvr.ini: 1 line(s) name settings this build does not read:
fix.openxr_render_scale ...` (config.cpp:296-329). Both installs run at 100%
with the "no entry" line; the bare-number line below is reached only by a user
who types `180` or `1.8` into the new key. The new key has no line at all in
either live file, so the F8 write's forced-key merge appends `openxr_resolution
= oculus/<sys>:3283` bare at the end of `[fix]` with no comment block
(iniedit.cpp:597-598), while the old line stays under the old prose; that is
the shape the retest's read-back looks for, and it stands until an installer
merge writes the shipped block. When one does, the merge carries the old line
to the end of `[fix]` with a note whose wording depends on the base: `carried
over from your edvr.ini; this version no longer uses it` (into `rep.retired`)
when the base ini knew the key, which is the case for anyone upgrading from a
main build after 09-14, and `not an EDVR setting this version knows` (into
`rep.carried`) only for a v0.16.2 base (iniedit.cpp:502-509). The old line is
then removed by hand, once, with the Edit tool, from Sean's two files; a
`moved-from` is not used, per the design brief, because the old grammar is
refused under the new key and there is nothing to carry. Mirror hygiene for
that removal: the mirror at `%LOCALAPPDATA%\EDVR\<leaf>-<store>\edvr.ini` is
refreshed only by a menu write (menu.cpp:686-688) or by the installer, so an
Edit-tool removal from the live file alone leaves the mirror carrying the old
line and a game-update restore brings the audit line back; after the removal
make one F8 write or run the installer so the mirror follows.

Which tool merges. `python tools\install_edvr.py --target <store> --ini` does
not merge: `--ini` means "also overwrite the target's edvr.ini with the
repository's -- this discards tuned settings" (install_edvr.py:1065-1067), the
stage copies `root/edvr.ini` over the target with a backup (:984-991), and
`--verify-only` with `--ini` demands byte equality with the repository file
(:949-954). No `mergeIni` caller exists under `tools/`. The three-way merge
(iniedit.cpp:456-511) is reached only from the installer executable
(src/installer/plan.cpp:543-553, base from the installer state) and from the
settings app and the menu's forced-key path (settings.cpp:259, menu.cpp:670).
The first draft of this revision told the retest to run `--ini` after flight 1
and "confirm the value survived"; that would have replaced Sean's whole tuned
Frontier file with the shipped one and failed its own assertion.
`install_edvr.py --ini` is never pointed at a live file in this document. The
merge is exercised on the desk instead, on a copy: the iniedit self-test (or a
scratch run of the installer's merge on a copy of each live file taken after
flight 1) asserts that a file carrying the flight-1 entry merges into the
shipped block with the entry intact and the old line carried with the note
above. A live merge happens only when Sean next runs `edvr-installer.exe` with
keep-settings, which is his to schedule.

docs/openxr-resolution-2026-09-14.md:26-27 ("type `0.75` for 75%") becomes
wrong the moment this lands and is updated in the same commit, as is
tools/native_render_settings_test/native_render_settings_test.cpp:79-111, which
sets `fix.openxr_render_scale` by name in six places.

### Log lines

Native trace (openvr_api.dll side), in startup order. NEW or CHANGED lines are
marked; the rest exist today. The tokens shown are what the sanitiser yields
for the recorded runtime strings; the system tokens are the grammar's shape
until flight 1 records the strings.

```
runtime,Oculus,282364034940928                              (:1196)
system,vendor=<id>,max_size=WxH,name=<systemName>           NEW
headset_key,oculus/meta-quest-3,runtime=Oculus,system=<raw> NEW
size,0,1824,1968,max=16384x16384 / size,1,...               CHANGED (:1220)
openxr_resolution,provider=1,key=oculus/meta-quest-3,
    headset=1824x1968,matched=1,entries=2,requested=1.799890 CHANGED (:251)
openxr_render_size,eye=0,original=1824x1968,scaled=3283x3542,
    requested=1.799890,effective=1.799890,megapixels=11.63  CHANGED (:266)
```

(The two long CHANGED lines are each one line in the log; they are wrapped here
only for the page.)

The same Quest 3 over Virtual Desktop with only the Oculus entry saved:
`runtime,VirtualDesktopXR,281474976710666`;
`headset_key,virtualdesktopxr/<sys>,...`;
`openxr_resolution,provider=1,key=virtualdesktopxr/<sys>,headset=3072x3264,
matched=0,entries=1,requested=1.000000` then
`...original=3072x3264,scaled=3072x3264,...,megapixels=10.03`. That is the
20:05 incident not happening. The Pimax over SteamVR with only the Oculus
entry: `key=steamvr-openxr/<sys>,headset=4980x4916,matched=0,entries=1,
requested=1.000000`, `scaled=4980x4916`. Either live file as it stands today
(the old key's line only, nothing under the new key):
`matched=0,entries=0,requested=1.000000`.

Graphics log (d3d11.dll, what `python tools\edvr_log.py` reads by default),
exactly one of:

- `openxr resolution: this headset is oculus/meta-quest-3 (Oculus, "Meta Quest
  3"); edvr.ini sets it to 3283 wide = 3283x3542 per eye (11.6 MP, 180% of the
  runtime's 1824x1968). Saved: oculus/meta-quest-3:3283,
  steamvr-openxr/pimax-crystal-super:4980. Elite's HMD Quality multiplies
  this.`
- `openxr resolution: this headset is virtualdesktopxr/meta-quest-3
  (VirtualDesktopXR, "Meta Quest 3"); edvr.ini has no entry for it, so it runs
  at 100% = 3072x3264 per eye (10.0 MP). Saved: oculus/meta-quest-3:3283. Set
  it in F8 > Performance with this headset on, or add
  "virtualdesktopxr/meta-quest-3:<width>" to fix.openxr_resolution (3072 is
  100%).`
- `openxr resolution: this headset is steamvr-openxr/quest-3 (SteamVR/ OpenXR,
  "..."); edvr.ini sets it to 4000 wide from the runtime-only entry
  steamvr-openxr:4000 = 4000x4278 per eye (17.1 MP, 158% of the runtime's
  2528x2704). Saved: ...` (the `matched = 2` case, so a user can see which rule
  applied; with an empty system token the parenthesis reads `(SteamVR/OpenXR,
  runtime reported no system name)`).
- `openxr resolution: fix.openxr_resolution = "0.90" is a bare number and names
  no headset; this headset (steamvr-openxr/pimax-crystal-super, 4980x4916) runs
  at 100% = 4980x4916 per eye (24.5 MP). Set it in F8 > Performance with this
  headset on, or replace the value with
  "steamvr-openxr/pimax-crystal-super:4482" for 4482 wide (90%).` (a user who
  copied an old-style value into the new key; Sean's own `0.90` sits under the
  old key and is unread, so on his rigs this line never prints). For a bare
  `1.0` the tail is the 100% template of the "no entry" line, and for a bare
  number out of range it is `"<value>" is out of range for this headset (a
  quarter to twice 4980 wide)`.

The copy-paste entry in the "no entry" and bare-number lines is exactly what
the menu would write for this headset: `headsetKey(rt, sys)` and, where a width
is offered, the width, with the same formatter. The bare-number line's width
comes from `bareNumberToWidth`, so a Notepad user who wrote `180` the way every
other key works, or `3283` the way this one now does, is told what to type, not
only that it was refused. Plus, once per bad token, `openxr resolution: ignored
"abc" in fix.openxr_resolution (entries look like oculus/meta-quest-3:3283)`.
The menu write logs `menu: fix.openxr_resolution oculus/meta-quest-3 (none) ->
3283 (list now oculus/meta-quest-3:3283; written to edvr.ini; applies after a
game restart).` through `drainWrites` (:1533), the eleventh menu site above.

What the log shows if the new code never ran, each case distinguishable:

- Both DLLs old: `openxr_render_scale,provider=1,requested=` (the old line
  name), no `headset_key,` and no `system,vendor=` line.
- Either DLL stale, the other new: `result,native_render_settings_query,-1`
  (`XR_ERROR_VALIDATION_FAILURE`), no `openxr_render_size` lines, VR init fails
  with `VRInitError_Init_Internal`. An old host sends v1/16 bytes to a new
  graphics DLL that accepts only v2/184, and a new host sends v2/184 to an old
  graphics DLL that accepts only v1/16; both return FALSE and print the same
  line. The `-1` says one side is stale, not which; only `python
  tools\install_edvr.py --target <store> --verify-only` tells which, and it is
  run before every flight. Loud, not a silent 100%.
- New graphics DLL, export ran, log enabled: the `this headset is` line is
  present. Its absence with a `key=` trace line present means the export code
  did not run or `log.enabled = 0`, never "the log was not open yet" (settled
  above).
- New host, `headset_key,` present but `openxr_resolution,` absent: the host
  failed between :1200 and :1250 and the trace names the step.
- What Elite shows the player on `VRInitError_Init_Internal` is not recorded in
  this repo. A support thread starts from that screen, so the README's
  troubleshooting entry for "VR failed to start after an EDVR update" names
  `--verify-only` and the `-1` line.

### Self-tests and the contract check

All headset-free, all under gates `build.bat` already runs. Fixtures use the
recorded runtime strings and sizes; the system strings in fixtures are
fixtures, not claims about any runtime:

| Fixture | runtimeName | systemName (fixture) | eye 0 | max |
| --- | --- | --- | --- | --- |
| Q3-Oculus | Oculus | Meta Quest 3 | 1824x1968 | 16384 (assumed) |
| Q3-VDXR | VirtualDesktopXR | Meta Quest 3 | 3072x3264 | 16384 (assumed) |
| Q3-SteamLink | SteamVR/OpenXR | Quest 3 (Steam Link) | 2528x2704 | 8192 |
| Pimax-SteamVR-a | SteamVR/OpenXR | Pimax Crystal Super | 4068x4016 | 8192 |
| Pimax-SteamVR-b | SteamVR/OpenXR | Pimax Crystal Super | 4980x4916 | 8192 |
| Pimax-PiOpenXR | Pimax OpenXR | Pimax Crystal Super | 5424x5356 | 16384 |
| Tall-cap (synthetic) | SteamVR/OpenXR | Tall Fixture | 4000x4500 | 8192 |

The Oculus and VDXR maxima are assumed: the per-view maximum is not traced
today, and the 20:05 log's `effective=1.8` on a 3072 base proves only that
VDXR's is at least 5530. The SteamVR and PiOpenXR values are the inventory's
(:21-22). The new `size,...,max=` field records the real ones on the desk pass
and flight 1, and the two fixtures are corrected then. The seventh fixture is
synthetic and exists for the effective-cap case in the Unit section: its height
binds first, so 8000 wide resolves to 7282x8192.

`tools/native_render_settings_test/native_render_settings_test.cpp`
(build.bat:1389-1396, links native_render_settings.cpp, config.cpp and log.cpp,
and already calls `edvrQueryNativeRenderSettings` in-process with
`edvr::Config::get().set(...)` fixtures at :76-91, so the v2 export is tested
where the v1 one is, with no fixture DLL):

- line 45 becomes `sizeof == 184`; a helper `query(fixture)` fills the v2 input
  with the fixture's bounds and names and returns the answer, and a helper
  `active(fixture, answer)` runs `effectiveScale` / `scaledDimension` on it, so
  every assertion below can be stated in pixels.
- Sanitiser: `headsetToken` of `SteamVR/OpenXR` is `steamvr-openxr`, `Oculus`
  is `oculus`, `VirtualDesktopXR` is `virtualdesktopxr`, `Pimax OpenXR` is
  `pimax-openxr`, `  Meta   Quest 3 ` is `meta-quest-3`, `Quest 3 (Steam Link)`
  is `quest-3-steam-link`, `--Quest--` is `quest`, `Quest, 3` is `quest-3` (the
  comma-in-name case: the entry separator cannot appear in a token), a name
  with bytes above 0x7F collapses those bytes to one `-`, the empty string is
  empty, a string of only punctuation is empty, a 63-byte name truncates to 30
  with no trailing `-`, `SteamVR/OpenXR : lighthouse` is
  `steamvr-openxr-lighthouse` (no prefix stripping), and
  `headsetToken(headsetToken(x)) == headsetToken(x)` for every case
  (idempotence, which is what lets entries be sanitised before comparison).
- Config fixtures, with `active` asserted in pixels:
  - `""` at every fixture -> 1.0, matched 0, entries 0, active ==
    original.
  - `"oculus/meta-quest-3:3283"` at Q3-Oculus -> 3283x3542, matched 1,
    entries 1; at Q3-VDXR -> 3072x3264, matched 0 (named `a width saved
    for Oculus does not reach VirtualDesktopXR`, and asserting the active
    size is not 5530x5875); at Q3-SteamLink, Pimax-SteamVR-a, -b and
    Pimax-PiOpenXR -> original, matched 0 (`a width saved for Oculus does
    not reach SteamVR/OpenXR or Pimax OpenXR`).
  - `"steamvr-openxr/pimax-crystal-super:4980"` at Pimax-SteamVR-b ->
    4980x4916, matched 1, scale exactly 1.0; at Pimax-SteamVR-a ->
    4980x4916, matched 1 (`the entry survives the 4068 -> 4980 base
    change`); at Q3-SteamLink -> 2528x2704, matched 0 (`a width saved for
    one SteamVR/OpenXR system does not reach the other`).
  - `"steamvr-openxr:4000, steamvr-openxr/pimax-crystal-super:4980"` at
    Pimax-SteamVR-b -> 4980, matched 1 (the specific entry wins though it
    is second); at Q3-SteamLink -> 4000x4278, matched 2; the same list in
    the other order gives the same answers.
  - `"Oculus/Meta-Quest-3:3283"` at Q3-Oculus -> 3283, matched 1 (the
    entry's own tokens are sanitised before comparison).
  - `"1.8"`, `"1.0"`, `"0.90"`, `"180"`, `"3283"` at every fixture -> 1.0,
    matched 0, entries 0, active == original, named `first-flight disaster:
    a bare number cannot reach any headset`; `bareNumberToWidth` of each at
    Q3-Oculus gives 3283, 1824, 1642, 3283, 3283 and of `"0.90"` at
    Pimax-SteamVR-b gives 4482; and `bareNumberToWidth` of `"3"`, `"24"`,
    `"201"` and `"455"` at Q3-Oculus gives 0 (out of range: each would be a
    width below the 0.25 clamp), of `"456"` gives 456, and of `"9000"` at
    Pimax-SteamVR-b gives 0 (above the effective cap).
  - `"oculus/meta-quest-3:4000"` at Q3-Oculus -> scale 2.0, 3648x3936;
    `:400` -> 0.25, 456x492; `"steamvr-openxr/pimax-crystal-super:9000"`
    at Pimax-SteamVR-b -> effective 1.644980, 8192x8087;
    `"steamvr-openxr/tall-fixture:8000"` at Tall-cap -> 7282x8192 (the
    height binds first); `"pimax-openxr/pimax-crystal-super:3283"` at
    Pimax-PiOpenXR -> 3283x3242 (the Quest 3's width is 60.5% on the Crystal
    Super).
  - Round-trip: for every fixture and every width in {original, 3283,
    4980, original*2, original/4 rounded up, 8191, 7282}, an entry with that
    width yields `active` width equal to the entry width whenever it lies
    in the quarter-to-double range and at or under the effective cap
    `min(maxWidth, scaledDimension(originalWidth, maxWidth, maxHeight /
    originalHeight))` over both eyes, and equal to that cap when it lies
    above it (claim 24); the Tall-cap fixture is the one where the two caps
    differ.
  - `"abc, oculus/meta-quest-3:3283"` -> 3283, entries 1, `abc` skipped;
    `"oculus/meta-quest-3:62,5"` -> `62` is a width, `5` a malformed
    token; `"oculus/meta-quest-3:0"`, `":3283"`, `"oculus/:3283"` (empty
    system with the slash present) and `"oculus/meta-quest-3:20000"`
    malformed; `"oculus/meta-quest-3:1824x1968"` malformed (a revision 2
    entry does not parse); `"SteamVR/OpenXR/Pimax Crystal Super:4980"` (the
    raw runtime name typed by hand) parses as runtime `steamvr`, system
    `openxr-pimax-crystal-super`, and matches no fixture (the split rule's
    consequence, pinned so the ini comment's "copy it from the log" stays
    load-bearing); nine entries -> eight kept; a duplicate key -> first
    wins; version 1, size 16, reserved = 1, a zero bound, original > max,
    and an unterminated name all FALSE; the existing null and bad-pointer
    rejections kept. The `"0.5junk"` numeric-prefix case retires with the
    unit.
  - Empty system token: a fixture whose systemName is `"---"` (sanitises to
    empty) with `"steamvr-openxr:4000"` -> 4000, matched 2; with
    `"steamvr-openxr/x:4000"` -> original, matched 0.
  - The graphics-log line formatted for eight maximal entries (30-byte
    tokens, five-digit widths) is under 1100 bytes, so it fits `Log::note`'s
    1200-byte buffer (log.cpp:356) with the timestamp prefix and no
    truncation marker.
- Echo rule: every input field comes back byte-equal; a test that deliberately
  corrupts one echoed byte in a copied answer is rejected by
  `validateRenderSettingsAnswer` (below); and `buildRenderSettingsRequest` with
  a 10-byte runtime name and a 12-byte system name leaves every byte after each
  NUL zero (the `memset` the echo compare depends on).
- Header round-trips: `parse(format(x)) == x`; `mergeResolutionEntry` replaces
  in place preserving order, drops later duplicates of the key, appends when
  absent, canonicalises spacing, refuses a ninth key, writes a runtime-only
  entry only when the system token is empty; `removeResolutionEntry` removes
  the exact key and leaves a runtime-only entry alone; `formatPercent(106.9f)
  == "106.9"`, `formatPercent(180.f) == "180"`; `megapixels(3283, 3542)` within
  0.01 of 11.63, `megapixels(5530, 5875)` within 0.01 of 32.49.
- Publish/query of sizing v1 unchanged, including the "saved setting changes
  leave the active sizing unchanged until restart" case (:111-116); plus
  `nativeRenderLabels` reads `valid = 0` before any query and after a `valid =
  0` publish, and the tokens after a query equal `headsetToken` of the names
  sent.

`tools/openxr_native_test/openxr_native_test.cpp` (:657-694, gated at
build.bat:1116-1129): the unpaired branch stays provider=0 and the existing
`applyRenderScale` fixtures stand; add a bare host with `renderBounds
{3072,3264,16384,16384}` at scale 1.8 asserting 5530x5875 (the incident, pinned
from the host side) and at 1.068685 asserting 3283x3488, and
`{4980,4916,8192,8192}` at 1.807229 asserting 8192x8087, so the arithmetic the
design hinges on is pinned from both sides; assert the labels copied into
`runtimeLabel` / `systemLabel` are truncated and NUL-terminated for a 300-byte
input and that the token the host would trace equals `headsetToken` of the
64-byte copy; call `buildRenderSettingsRequest` on the bare host and assert the
184-byte struct's fields, then `validateRenderSettingsAnswer` against a correct
echo, a wrong size, a wrong version, `matchedEntry = 3`, `entryCount = 9`, a
changed bound and a changed name byte, asserting each rejection.

The paired call itself, `GetProcAddress` plus `graphicsCalls.invoke`, is not
exercised by these tests, and cannot be from a bare host: `invoke` goes through
`RenderThreadDispatcher::invoke` (src/openxr/
render_thread_dispatcher.h:43-52), which returns false unless the caller is the
`OwnerService` thread, a render thread is bound, and that thread is inside an
executing `invokeOwner` boundary; `CountedGraphics::invoke`
(render_route.h:29-38) adds its own thread check. The existing bare-host block
at :660-694 works only because the `!graphicsProxy` branch never invokes.
Standing that choreography up against a fixture DLL was costed and not taken:
it needs the `NativeBackend` owner/route scaffolding (:58-77 or the nesting at
:329), `openxr_export_fixture.dll` moved before :1116 in build.bat (it is built
at :1270-1274, after the native test runs), log.cpp added to its link, two
`fixture.def` entries, and a test-only export to set the fixture's own `Config`
singleton, which the test EXE's `Config::get().set()` cannot reach. The two
pure functions above cover the field-order class of failure. What remains
unexercised is plumbing unchanged from v1: exercised by every paired flight
and, when run by hand, by the present-boundary harness (`--present-boundary`,
:558-626).

`tools/gen_settings_schema.py --self-test`: one case that a live `[fix]`
key with an empty shipped value and `# ui: X | restart | menu performance`
(no `range`, no `percent`) generates a Text menu row with empty bounds, and
that `summarise` of the comment above returns the first sentence with the
grammar and the example in it, under 200 characters (:449) and under the 141
characters of the longest summary the window draws today. No `[fix]` key ships
with an empty live value and a ui: line today (the empty ones are
`journal_dir`, `real_dll`, `real_openvr_dll`, `exposure_shader`, none under an
exposed section), so this case is also claim 14.

`tools/check_config_contract.py`: no rule change. The read is a string literal
under `[fix]`, the key is documented, the count stays 255. Its self-test gains
nothing unless the example line in the comment is counted as a key line (claim
13), in which case the comment is reworded, not the checker.

`build.bat` by absolute path, tail read, before any claim of green.

### Retest plan

The two tools, named once: `python tools\install_edvr.py --target <store>
--verify-only` is the guard **before** every flight (it is what rules out the
stale-DLL `-1`); `python tools\edvr_log.py --target <store> --expect-build
HEAD` is the check **after** every flight, of the log the flight just produced
(it exits 2 on a build mismatch), and `--tag openxr` (edvr_log.py:269-270;
native logs live in `edvr_logs\` beside the executable, :120-128) reads the
host trace lines so nobody hand-greps the native log.

Desk first, no flight: re-read both live `edvr.ini` files and both mirrors with
an explicit UTF-8 decode (`[Text.Encoding]::UTF8.GetString([IO.File]::
ReadAllBytes(path))`, never `Get-Content`) and record the four
`openxr_render_scale` lines and write times as they are at that moment (as of
this revision: Frontier `1.0` at 20:06:28 UTC, Steam `0.90` at 21:20:24 UTC,
mirrors identical), because the first draft of this plan was written against
values that had already changed; then build; `python tools\install_edvr.py
--target <store>` then `--verify-only`, never with `--ini` (it overwrites the
live file, "Migration" above); leave `edvr.ini` untouched (that is the
migration under test); run the unpaired native diagnostic
(`tools/run_openxr_native.py --runtime ...`, provider=0 path) once per
installed runtime and record, from the trace, the `runtime,` line (the exact
bytes of PiOpenXR's name, first time on record), the `system,vendor=` line (the
exact system string, first time on record for every runtime), the
`headset_key,` line (the token the entries will use) and the `size,0,W,H,max=`
and `openxr_resolution, provider=0,key=` lines. The Quest routes need Air Link,
Virtual Desktop or Steam Link streaming active with the headset awake for
`xrGetSystem` to succeed: VDXR returned `XR_ERROR_FORM_FACTOR_UNAVAILABLE`
without one on the desk (inventory :23) and again in flight at 20:13 today
(`result,xrGetSystem,-35`, the Frontier table), where the host failed at :1198
before the system name at :1200 existed; only PiOpenXR over DisplayPort is a
true headset-on-the-desk run. That puts every runtime's name, system name,
token and recommended size on record, records the Oculus and VDXR per-view
maxima the fixture table assumes, and proves the new trace lines exist in the
build before a session is spent on them. Then substitute the recorded system
tokens into the ini comment's example and rebuild.

Every expected `headset=`, `original=` and `scaled=` value below means "the WxH
the `size,0` line printed for this runtime on this launch", and every system
token is recorded on flight 1, not expected. The runtime strings are expected
exactly: `Oculus`, `VirtualDesktopXR`, `SteamVR/OpenXR`, and whatever the desk
pass recorded for PiOpenXR. A correct flight must not read as a failure because
a size in this plan was the older one.

Flight 1, Quest 3, one session across two streaming apps, three launches.

Launch A, Air Link, file untouched (Frontier says `1.0`): expect
`runtime,Oculus,`, the `system,vendor=` line (record the system string),
`headset_key,oculus/<sys>,...` (record the token), `openxr_resolution,
provider=1,key=oculus/<sys>,headset=1824x1968,matched=0,entries=0,
requested=1.000000`; `scaled=1824x1968`, `megapixels=3.59`; graphics log, the
"no entry" line ending with the 100% template `add "oculus/<sys>:<width>" (1824
is 100%)` plus the audit's unknown-key line for `fix.openxr_render_scale`; F8
row `OpenXR res. 1824 px` / `1824x1968` with the hint `Not set for this
headset: 100% = 1824x1968 (3.6 MP) / eye.`; Status `Headset`, `Headset key` and
`Recommended` lines naming the runtime, system, token and size. Then set the
width, by one of two routes, and say in the log which was used, because they do
not reach the same number: type `3283` (Enter, digits, Enter), the exact width
the 180% flights used; or step Right until the row reads `3300 px` (fifteen
presses of 100 from 1824, 180.9%). Expect the write log `menu:
fix.openxr_resolution oculus/<sys> (none) -> 3283 (list now oculus/<sys>:3283;
...)` (or `-> 3300`), the pending badge, the preview `3283x3542 (11.6 MP,
180%)` (or `3300x3561 (11.8 MP, 180.9%)`), and the session size unchanged.
Then, still in the session, open Elite's Graphics options and Apply an
unrelated change: read the native trace for a `module_shutdown,` line followed
by a second `module_startup,` and a second `openxr_resolution,`. If the
shutdown line and the pair appear, Elite re-initialises VR in-process
(`VR_Shutdown` then `VR_Init`, runtime_lifecycle.h:68-79 then :41-47), Launch B
below is that Apply, the badge must clear by itself, and decision 8 flips the
wording to "at the next VR start"; a second `openxr_resolution,` with no
`module_shutdown,` before it is not a re-init and is a bug to name. If nothing
appears, record that too, the wording stays "after a game restart", and Launch
B is a relaunch. Quit through Elite's own menu. Read the ini back (decode with
`[Text.Encoding]::UTF8.GetString`, not `Get-Content`) and confirm the shape
"Migration" predicts: `openxr_resolution = oculus/<sys>:3283` appended bare at
the end of `[fix]` (iniedit.cpp:597-598) with `openxr_render_scale = 1.0` still
under the old prose. Confirm the mirror copy updated (its write time and
bytes). Do not run `install_edvr.py --ini`; the merge is checked on the desk on
a copy, as "Migration" says, and the old line is removed by hand only after
that.

Launch B, Air Link: `key=oculus/<sys>,headset=1824x1968,matched=1,
entries=1,requested=1.799890`; `scaled=3283x3542`, `megapixels=11.63` (or, if
Launch A stepped, `requested=1.809211`, `scaled=3300x3561`,
`megapixels=11.75`); `native temporal: engaged ... output=3283x3542`; the
guard's later recommendation about 4079x3542 as in the airlink doc :224; row
`3283 px` with no badge; tooltip shows `Elite submits WxH (x0.NN of what it was
given; guard x1.24)` while the guard is live, and that ratio is recorded in the
doc; the `engaged mode=` line is recorded for the absolute size it names, not
for a ratio (the Risks bullet on DLSS). No bare-number line.

Launch C, the same session or the next, Virtual Desktop: expect
`runtime,VirtualDesktopXR,`, `headset_key,virtualdesktopxr/<sys>` (record
whether VDXR's system string is the same bytes as Oculus's for the same
headset; it need not be), `openxr_resolution,provider=1,
key=virtualdesktopxr/<sys>,headset=3072x3264,matched=0,entries=1,
requested=1.000000`, `scaled=3072x3264`, `megapixels=10.03`. This is the
disaster assertion for the 20:05 incident: the Oculus entry does not follow the
headset to Virtual Desktop, and the file still holds it. Row `3072 px`, hint
`Not set for this headset`, tooltip `Saved for: oculus/<sys>:3283`. Optionally
set it here too (3283 again gives 3283x3488 at 106.9%; record whether Sean
prefers the same width or a larger one at VDXR's base) and confirm the file now
holds two entries.

Flight 2, Pimax Crystal Super over SteamVR/OpenXR, both installs, the Frontier
file from flight 1 untouched and the Steam file still `0.90` (re-read it first;
the desk pass recorded it).

Frontier: record `runtime,SteamVR/OpenXR,`, the `system,vendor=` line (the
Pimax's system string under SteamVR, the one that must differ from the Quest
3's under Steam Link) and `headset_key,steamvr-openxr/<sys>`; expect
`headset=<size,0>,matched=0,entries=1 or 2,requested=1.000000` and
`openxr_render_size,eye=0,original=<size,0>,scaled=<size,0>` with scaled equal
to original; record the megapixels. Row `<size,0 width> px`, hint `Not set for
this headset`, tooltip `Saved for: oculus/<sys>:3283, ...`. Set the width Sean
wants for this headset (4980 gives 100% at the 20:50 base; the 12:50 base
4068x4016 would give 4980x4916 at 122.4%, and either day's launch is expected
to produce the same `scaled=4980x4916` if the base moves between the two, which
is the drift assertion). Record Sean's judgement of the picture at whichever
value he lands on, and the `engaged mode=` line, so the number the doc quotes
is one that was looked at.

Steam: the same, starting from `0.90`, which is the launch that visibly changes
something: the 21:18 flight ran at 1.0 and Sean then chose 90%, and this launch
runs at 100% instead (`matched=0,entries=0`, scaled equal to original,
4980x4916 rather than the 4482x4424 that `0.90` meant at this base), so a
sharper, heavier picture than the last Pimax session is the expected result and
not a regression. The graphics log carries the "no entry" template plus the
audit's unknown-key line for the old key (the old `0.90` is unread, not
refused, so no line names 4482; the 90% figure comes from this plan). Set the
width Sean wants (4482 to get his 90% back, or whatever he lands on), confirm
the Steam file holds its own single entry and the Frontier file is untouched by
it.

Flight 2b, the Quest 3 over Steam Link, if Sean flies it again: expect
`runtime,SteamVR/OpenXR,`, `headset_key,steamvr-openxr/<quest sys>` with a
token different from the Pimax's, `headset=2528x2704,matched=0` (or `matched=2`
if a runtime-only entry was typed), `scaled=2528x2704`. This is the second
disaster assertion: the SteamVR/OpenXR entry for the Pimax system does not
reach the Quest 3 on the same runtime string. If the two system tokens turn out
equal (SteamVR reporting a generic name for both), that is the finding of the
day, recorded as `ruled out:` in this doc, and decision 3's runtime-only entry
becomes the only SteamVR entry either headset can have until a third field is
found.

Flight 3 falls out of the next ordinary Quest 3 session: 3283 must still be
there with no re-set, and every entry still in the file.

If any launch shows the old trace shape or
`result,native_render_settings_query,-1`, stop and run `--verify-only`: that is
an install mismatch, not evidence.

### Risks

- The system strings are unrecorded. If SteamVR/OpenXR reports the same generic
  `systemName` for the Quest 3 over Steam Link and the Pimax (the inventory's
  "HMD system" paraphrase at :22 hints it may), the two share one entry on that
  runtime and the design has no third field for them. Flight 2b measures it;
  the fallback (100%) is still never another headset's width, only a shared one
  on that route.
- A runtime that changes its `systemName` bytes between versions (a firmware
  string, a locale) makes the old entry unmatched: safe, logged with the new
  key to add, and the runtime-only entry is the hand-written bridge.
- The height follows the runtime's aspect. The same width gives 3542 tall over
  Oculus and 3488 over VDXR for one Quest 3; the row shows it and the ini
  comment says so. A user who wants the same pixels on both routes sets each
  once.
- ABI lockstep: a stale DLL on either side fails native startup with the same
  `-1` line instead of flying. The installer ships both; `--verify-only` is the
  guard; a half-install has cost a session before.
- Legacy files drop to 100% on first launch with only the log line (which
  carries the entry to write) and the F8 hint as notice; each headset needs one
  F8 step, and the old key's line draws the audit's unknown-key line once per
  launch until removed. On Sean's own Steam install that first launch discards
  a chosen `0.90` (4482 wide) for 4980, which is why the retest expects it.
- With `log.enabled = 0` the graphics-log line is absent by design
  (log.cpp:134) and the host trace is the only record.
- The installer's settings window shows the list as raw text; a typo there is a
  skipped token, logged once, and the headset falls to 100%. The row summary
  carries the grammar and an example, which is the only guard. The window's
  one-click `reset` link is removed for this row (above); until that change
  lands, one click there empties every headset's entry, recoverable from the
  settings app's `settings-<timestamp>` backup.
- Menu special-casing grows to eleven sites in untested menu.cpp, listed above;
  a mistake there is found in the headset. The list read-modify-write parses
  the whole value and re-serialises it through the one-value merge; a hand edit
  to the file made while F8 is open is overwritten by the menu's copy unless
  the one-second reload ran first.
- No VRAM guard: 2x on the Pimax over PiOpenXR at 5424x5356 is still
  10848x10712 per eye (116 MP) plus DLSS history, accepted as long as it fits
  16384. The megapixel readout is the only warning; the width unit at least
  makes such a number something the user typed.
- Under DLSS the GPU cost is dominated by the input (target x HMD Quality), and
  the DLSS mode is chosen by the input/output ratio and stepped down until the
  input fits (docs/anti-aliasing.md:954-965). The width does not change that
  ratio: the upscale's output target is `s->recW/recH` (native_temporal.cpp:
  198-199), which is the frame's `recommendedWidth/Height` from
  native_temporal_client.h:52-53, which is `gameGeometry` (host :598), the
  guard-adjusted scaled size the host publishes (:589-594), and Elite's input
  is that same size times HMD Quality (system_hook.cpp:842-849); input over
  output is HMD Quality whatever the width. What the width changes is the
  absolute size, and so the VRAM, at which the NGX feature is created. The
  retest records the `engaged mode=` line on both rigs for the size it names.
- The in/out single-buffer ABI is new here. Its echo rule is what makes a
  field-order slip loud; the pure request/validate functions are what make it
  testable without a headset.
- Two DLLs and a test must agree on the sanitiser byte for byte. It is one
  header-only function, the host traces its output, and the test asserts the
  host's and the DLL's tokens are equal for the same 64-byte input.

## Decisions for Sean

1. Key name. DECIDED 2026-09-14: renamed to `fix.openxr_resolution`. The line
   quoted to Sean was `openxr_render_scale = 1.0` (edvr.ini:450): a fraction of
   the runtime's recommended per-eye size, 0.25..2.0, applied at the next VR
   start, in no public release. He answered yes. The rename costs one
   hand-removal of the old line from his two files plus one F8 write or
   installer run so each mirror follows, one unknown-key audit line per launch
   until then, and the rename of the six `fix.openxr_render_scale` fixtures in
   tools/native_render_settings_test/ native_render_settings_test.cpp:79-111
   and of docs/openxr-resolution- 2026-09-14.md:26-27 in the same commit. No
   `moved-from`.
2. Unit. Per-eye width in pixels (recommended: it is the number you named when
   you asked for "a reasonable amount of pixels", it cannot be multiplied by a
   change of streaming app or vendor preset, and today's 20:05 log is the
   argument: 180% became 5530x5875 per eye, 32.5 MP, when Virtual Desktop
   supplied the base) or a percent of the recommendation, which composes with a
   preset change the way "still 180%" reads but is exactly what produced
   5530x5875. With the width, 3283 over Virtual Desktop is 3283x3488.
3. Runtime-only entries. Accept a hand-written `steamvr-openxr:4000` as the
   fallback for any headset on that runtime that has no entry of its own
   (recommended: it costs one rule in the matcher, the menu never writes one
   except when the runtime reports an empty system name, and it is the only
   bridge if flight 2b finds SteamVR reporting one system string for both
   headsets) or refuse them and require the full key.
4. Step size. DECIDED 2026-09-15: a fixed 100 px on the grid of round hundreds,
   Shift for 500. The first build shipped 5% of the recommended width rounded
   to a multiple of 8 (88 px on the Quest 3, 248 on the Pimax over SteamVR);
   Sean asked for about a hundred, which also means a stepped value is one a
   person would type. Typed values remain any integer, and a typed off-grid
   width steps to the neighbouring hundred.
5. Paired-query test. Two pure functions in the host header plus the in-process
   v2 export test (recommended: covers the field-order class with no fixture
   DLL and no dispatcher choreography) or additionally stand up the
   owner/route/fixture harness described under self-tests. With the first, the
   `GetProcAddress` + `invoke` plumbing stays covered only by paired flights
   and the hand-run present-boundary harness.
6. Tooltip content. Show `Elite submits WxH (x0.NN of the active size)` in the
   tooltip only (recommended, until the 94-pixel hint is re-measured) or also
   in the always-visible hint.
7. Your own files. Let the first launch on each route ignore the old key's
   value and set the width with one F8 step or a typed width (recommended: it
   is the migration under test, and on the Steam install it is the launch that
   drops your `0.90` to 100%; 4482 is the width that restores it) or edit the
   two files with the Edit tool before flight 1 (and then make one F8 write or
   run the installer so each mirror follows).
8. Restart wording. Ship "after a game restart" in the ini, tooltip, audit
   line, toast and write log (recommended: it is the untested-safe wording; the
   only mechanism for a cheaper apply is a `VR_Shutdown` / `VR_Init` pair that
   flight 1 looks for as a `module_shutdown,` line between two
   `module_startup,` lines) and flip it to "at the next VR start" only if
   flight 1 records that pair; or ship the weaker wording now on the
   mechanism's promise.
9. The clamp. Keep the unchanged 0.25..2.0 clamp for width entries
   (recommended: 2x of a 5424 base is already 116 MP per eye, and the clamp is
   what bounds a very low or very high streaming preset in the Unit section) or
   widen it for width entries so a saved width survives any preset.

Settled by today's evidence and no longer asked: the identity (names, not the
recommended size), the wildcard (none), the legacy bare number (refused, with
the entry to write in the log), percent-versus-fraction in the file (superseded
by the width), and the key name (decision 1, answered).

Adjacent and not part of this design: `menu.fps_overlay_pitch`'s generated F8
hint and detail are literally `retired-default: -16` (build/gen/
menu_schema.inc:137-138), because the generator treats the annotation at
edvr.ini:633 as prose and the block has no other line. A one-line
`retired-default:` branch beside `moved-from:` in gen_settings_schema.py (:299)
fixes it, as a separate commit.

## Claims to verify

Each is a code-level claim this design depends on, phrased so it can be
confirmed or refuted by reading the named file. Claims settled in an earlier
revision are marked and kept for the record.

1. `renderBounds[]` and `sizes[]` are filled at native_runtime_host.h:
   1215-1221, before `captureRenderSettings()` is called at :1250, so both
   eyes' recommended and maximum sizes can be copied into the query input
   without reordering `open()`. The maximum is
   `XrViewConfigurationView.maxImageRectWidth/Height` clamped to 16384
   (:1217-1219), not `XrSystemGraphicsProperties.maxSwapchainImage*`. SETTLED.
2. `ip.runtimeName` (:1194-1196) and `properties.systemName` /
   `properties.vendorId` (:1199-1200) are in scope at :1250 and
   `properties.systemName` is not traced anywhere today (grep for `systemName`
   in src finds three sites: the declaration at system_source.h:30, the copy at
   :1261 and the property read at openvr_system.cpp:233; `runtimeName` has one
   consumer beyond the trace, `Prop_TrackingSystemName_String` at :234).
   SETTLED.
3. `captureRenderSettings()` (:223-253) validates the answer by exact size and
   version, `reserved == 0` and a finite scale, and returns
   `result("native_render_settings_query", XR_ERROR_VALIDATION_FAILURE)`
   otherwise, which prints `result,native_render_settings_query,-1` (:110-113;
   openxr.h:155) and `start()` maps to `VRInitError_Init_Internal` at :325; the
   only 100% fallback is the `!graphicsProxy` branch at :232-236. SETTLED.
4. `edvrQueryNativeRenderSettings` (native_render_settings.cpp:34-52) reads no
   field of `*output` before overwriting the whole 16-byte struct, so turning
   the buffer into in/out requires the version bump and an SEH-guarded read,
   for which `copySizingInput` (:12-19) is the pattern; and the v2 body cannot
   keep `__try` inline because it holds `std::string` (C2712), so the SEH stays
   in `noexcept` helpers.
5. The project has both ABI conventions: `native_timing.h` replaced `VERSION_1`
   with `VERSION_2` in place (a8a0e57), while native_module.h: 9-10 keeps two
   versions and native_module.cpp:284-286 accepts both. SETTLED; the in-place
   replacement is chosen here, not required.
6. `edvrPublishNativeRenderSizing` (:77-86) and `coherentNativeRenderSizing`
   (menu.cpp:309-339) both recompute `effectiveScale` from `requestedScale` and
   each eye's active size from `scaledDimension` with one common scale, so a
   per-headset width reduced to one scale passes both unchanged and the 80-byte
   sizing ABI need not move.
7. `effectiveScale` (native_render_settings.h:94-108) and `scaledDimension`
   (:83-92) give: 1824x1968 at 3283/1824 -> 3283x3542; 3072x3264 at 1.8 ->
   5530x5875 and at 3283/3072 -> 3283x3488; 4980x4916 (max 8192) at 9000/4980
   -> effective 1.644980, 8192x8087; 4068x4016 at 4980/4068 -> 4980x4916;
   1824x1968 at 4000/1824 -> 2.0, 3648x3936, and at 400/1824 -> 0.25, 456x492;
   1824x1968 at 3300/1824 -> 1.809211, 3300x3561; 5424x5356 at 3283/5424 ->
   3283x3242; 4000x4500 (max 8192x8192) at 8000/4000 -> effective 1.820444,
   7282x8192 (the height binds first). Computed by hand from the header for
   this revision; to be confirmed in the self-test, not by hand.
8. `menu.cpp` reads the published sizing every tick while open (:2562-2576)
   into `s.renderSizing`, and only while open, so the two badge sites that run
   with the menu closed (:1509 via the reset key is open-only, but :2499-2500
   runs from vscreen.cpp:4280 on every reload) must read the sizing live
   through `readNativeRenderSizing` (:341).
9. `stepRow` (:1552-1588), `editBufferBad` (:1853-1865), `beginEdit`
   (:1867-1883) and `commitEdit` (:1897-1916) branch on `d.kind`, and a
   `getString`-read key generates `MenuKind::Text` (gen_settings_schema.py:
   189-190), for which stepping is a no-op and any text commits; hence the
   special cases listed under Menu are needed for numeric behaviour.
10. The restart badge is a raw string compare of `value` against
    `snapshot` at three sites, :1509, :1544-1546 and :2499-2500, with the
    snapshot taken at first `menuConfigure` (:908-915) and `pending`
    consumed by `pendingRestartCount` (:917) at :1046 and :1250; so a
    list-valued key needs its own compare at all three or a canonicalising
    rewrite would badge a restart.
11. `tools/check_config_contract.py` counts reads only through `READ_RE`
    (:47, a string-literal first argument) and documents keys through
    `keys_documented` (:116-150); a literal
    `getString("fix.openxr_resolution", "")` against a documented
    `openxr_resolution` line leaves the count at 255 with no checker
    change, and the renamed key needs the ini regenerated into
    `kKnownKeys` for the audit.
12. The graphics log is open before `edvrQueryNativeRenderSettings` can
    run: `initOnceCallback` opens it on the first D3D11 export
    (d3d11_proxy.cpp:273-277, from :556 and :583) and the native host is
    constructed only after the game device has Presented
    (native_module.cpp:48-51, :60). SETTLED; the only absent-line case
    with the code running is `log.enabled = 0` (log.cpp:134).
13. `keys_documented` does not count a comment line of the form
    `#   oculus/meta-quest-3:3283, ...` (no key name, no `=`) as a key
    line; and the phrase `fix.openxr_resolution` inside the graphics-log
    strings is matched by `MENTION_RE` (:60) only as a mention of a key the
    code does read, so it is not flagged.
14. `gen_settings_schema.py` accepts a live `[fix]` key with an empty
    shipped value and a `# ui:` line without `range` or `percent`,
    generating a Text row (the `missing` check at :576-592 requires only
    the annotation; the `percent` check at :515-536 is not triggered); and
    `summarise` (:426-459) returns the first sentence of the comment above
    intact, because its `. ` falls at 185 characters, under the 200-cut at
    :449, and it contains no ` -- ` (:446). No such key ships today, so
    this is untested until the self-test case is added.
15. `captureRenderSettings()`'s paired branch cannot be driven from a bare
    host: `graphicsCalls.invoke` is `CountedGraphics::invoke`
    (render_route.h:29-38) over `RenderThreadDispatcher::invoke`
    (render_thread_dispatcher.h:43-52), which refuses unless called on the
    owner thread with a bound render thread inside an executing
    `invokeOwner` boundary; the :660-694 block never reaches it. SETTLED.
    The testable surface is therefore the two pure functions
    (`buildRenderSettingsRequest`, `validateRenderSettingsAnswer`) and the
    in-process v2 export in native_render_settings_test, which already
    links config.cpp and log.cpp (build.bat:1392).
16. The installer's three-way merge (src/common/iniedit.cpp:456-511) keeps
    a known key's user value verbatim when the shipped value changes from
    `1.0` to empty, except a value listed under `# retired-default:`
    (:207-234, applied at :522-530, whose report line with an empty shipped
    value reads `it now ships  -- set it again if you meant it`), which it
    drops; carries an unknown key to the end of its section with the note
    `this version no longer uses it` into `rep.retired` when the base ini
    knew the key and `not an EDVR setting this version knows` into
    `rep.carried` when it did not (:502-509), so a tester upgrading from a
    main build sees the former and only a v0.16.2 base yields the latter;
    and `menuIniWrite`'s forced-key merge (menu.cpp:650-699, iniedit.cpp:
    580-601) rewrites a value in place when the key has a line (:594-596)
    and otherwise appends `key = value` bare at the end of the section
    (:597-598), so with the key renamed the F8 write lands with no comment
    block until an installer merge. The merge is reached only from
    src/installer/plan.cpp:543-553, settings.cpp:259 and menu.cpp:670;
    tools/install_edvr.py has no caller and its `--ini` overwrites
    (:984-991, :1065-1067).
17. `announceEyeTextureSize` (native_temporal.cpp:178, inside the eye
    treat at :170-190) runs before the `temporal_aa` on/off check (:190)
    whenever the temporal provider is acquired (host :1300), so
    `eyeTextureSize()` is published under `temporal_aa = off` too, and
    independently of `announceEyeTangents` (:182, refused by
    frame_flag.cpp:599-602 for an unpackable tangent), so the `Elite
    submits` clause is available on both rigs.
18. `nativeTracePrintf` at :1196 and :1220 formats exactly
    `runtime,<name>,<version>` and `size,<eye>,<w>,<h>` (confirmed against
    today's logs, which print `runtime,SteamVR/OpenXR,563022967865353` and
    `size,0,4980,4916`); the new `system,vendor=` line deliberately differs
    from the probe's field order (openxr_probe.cpp:145-149) to put the
    comma-capable name last, the new `headset_key,` line puts both raw
    names after the token for the same reason, and no tool parses the
    `size,` line, so appending `,max=` is safe.
19. A new `NativeRuntimeHost` is constructed per VR_Init attempt
    (native_module.cpp:28-29, :60-64, up to 16 at :24), but
    `RuntimeLifecycle::init` returns the existing token while `Running`
    (runtime_lifecycle.h:41) and only `shutdown()` (:68-79) or a failed
    start (:61-64) returns to `Idle`, so `open()` and the list read recur
    only for a `VR_Shutdown` followed by a `VR_Init`, visible as a
    `module_shutdown,` line between two `module_startup,` lines. Whether
    Elite's Graphics Apply performs that pair on the native path is
    unrecorded; flight 1 looks for the shutdown line, not only for a second
    `openxr_resolution,` line.
20. `XrSystemProperties.systemName` is 256 bytes (openxr.h:135) and
    `XrInstanceProperties.runtimeName` 128 (:138); the ABI's 64-byte
    fields truncate, the token is computed from the 64-byte copy on both
    sides, and the trace prints the full string beside the token, so a
    name longer than 63 bytes yields one token everywhere and two
    different raw strings only between the trace and the tooltip.
21. A `# retired-default:` line placed between the prose and the `# ui:`
    line, as `fps_overlay_pitch` does (edvr.ini:633-635), is read by the
    merge (iniedit.cpp:207-234) AND leaks into the generated schema:
    `gen_settings_schema.py`'s `parse_ini` special-cases only `ui:`, `dev:`
    and `moved-from:` (:293-307) and appends every other comment line to
    the prose (:308-309) that becomes `s.description` (:320); running it on
    the current ini gives `fps_overlay_pitch` the hint and detail
    `retired-default: -16` (build/gen/menu_schema.inc:137-138, reproduced
    for this revision), and on the proposed block it would have ended the
    tooltip and window description with `retired-default: 1.0`. REFUTED as
    first stated; the line is omitted from this design (idle under the
    rename), and the generator branch is the separate `fps_overlay_pitch`
    cleanup.
22. Sanitiser precedent: `slugOf` at src/installer/mirror.cpp:37-48 is the
    same rule in wide characters (lowercase, `iswalnum` kept, other runs
    collapsed to one `-`, trailing `-` trimmed; a leading `-` never forms
    because it appends `-` only when `out` is non-empty), used for the
    mirror folder name. `mirrorDir()` at menu.cpp:609-645 (used by
    `menuIniWrite` at :686-688 for the mirror copy on every write, not by
    the Status page) does not repeat the rule; it hard-codes three store
    slugs from path substrings (:618-621). Neither is shared with src/d3d11
    or src/openxr as a function, and `iswalnum` is locale-dependent above
    0x7F, so the new ASCII-only `headsetToken` is a second implementation
    of the idea, not a reuse; whether the installer should adopt it is out
    of scope.
23. The runtime version numbers in today's logs decode with
    `XR_VERSION_MAJOR/MINOR/PATCH` (openxr.h:34-36) as Oculus 1.207.0,
    SteamVR/OpenXR 2.17.9, VirtualDesktopXR 1.0.10; the inventory's
    version column (:19-23) agrees for the two it shares.
24. Width round-trip through a float scale: for every fixture width in
    the quarter-to-double range and at or under the effective cap,
    `scaledDimension(originalWidth, maxWidth, effectiveScale(clampScale(
    float(width) / float(originalWidth)), eyes, 2)) == width`, where the
    effective cap is `min(maxWidth, scaledDimension(originalWidth,
    maxWidth, maxHeight / originalHeight))` over both eyes because
    `effectiveScale` takes the minimum over both axes
    (native_render_settings.h:102-105), not `maxWidth` alone. Stated from
    the arithmetic at :83-108 (float has about seven significant digits,
    the product is within 0.001 px, `+ 0.5` then truncation); the self-test
    asserts it rather than this paragraph, with the Tall-cap fixture as the
    case where the two caps differ.
25. Label width: `OpenXR res. 200%` measured 344 pixels in the 466-pixel
    label area and `L16384x16384` 269 in the 304-pixel value area at the
    default card size (docs/openxr-resolution-2026-09-14.md:71-75).
    `OpenXR res. 3283 px` (three characters more) and `OpenXR res. 16384
    px` (four more) are expected under 466 by the same proportion and are
    re-measured with the GDI check before the build is called done; the
    concise hints `After restart: 3283x3542 (11.6 MP, 180%). Active:
    3283x3542 / eye.` and the longest same-eye variant `After restart:
    8192x8087 (66.3 MP, runtime cap 164.5%). Active: 4980x4916 / eye.` (80
    characters; the current longest two-line hint is 84 characters at 92 of
    94 pixels) are measured against the 94-pixel two-line box the same way
    (a third line clips silently, menu_panel.cpp:278, :574-575); and the
    settings window's 392x34 dp description rect at the 9-point caption
    (gui.cpp:64-66, settings_view.cpp:26-29, :234-235, ui.cpp:258,
    `DT_WORDBREAK | DT_END_ELLIPSIS` at :396) is measured with the
    116-character summary the same way.
26. `matchedEntry` widened to 0/1/2 changes the host's validation from
    `<= 1` to `<= 2` and nothing else in the v2 layout; the trace's
    `matched=` field carries it so a runtime-only match is visible in the
    log.
27. The runtime audit (config.cpp:296-329) names a key this build does not
    read once per process (`auditNoted`, :303) as `edvr.ini: N line(s)
    name settings this build does not read: ...`, which is what a
    renamed-away `openxr_render_scale` line produces until it is removed;
    `movedOld` (built at :241-246, tested at :302) suppresses it only for
    keys carrying a `moved-from`,
    which this design does not add.
28. `python tools\install_edvr.py --ini` overwrites: the help text says
    "also overwrite the target's edvr.ini with the repository's -- this
    discards tuned settings" (install_edvr.py:1065-1067), the stage copies
    `root/edvr.ini` over the target with a backup (:984-991), and
    `--verify-only` with `--ini` demands byte equality (:949-954); no
    `mergeIni` caller exists under tools/. SETTLED for this revision by
    reading; the retest never runs it on a live file.
29. The live files, read for this revision with an explicit UTF-8 decode:
    Frontier `openxr_render_scale = 1.0` (written 2026-09-14 20:06:28 UTC),
    Steam `openxr_render_scale = 0.90` (21:20:24 UTC), both mirrors under
    `%LOCALAPPDATA%\EDVR\` identical. To be re-read at the desk pass; the
    retest is written against these and says so.
30. `MenuLine.right` is 64 bytes (menu_panel.h:64) and `statusLine` copies
    63 with no marker (menu.cpp:929-932), and `Last write` goes through the
    same field (:1060); so every Status string this design adds is sized
    under 63 for a 30-byte system token, and the key line is at most 61 by
    construction.
31. `MenuContent.popup` is 1024 bytes (menu_panel.h:127) and scrolls; the
    comment at menu.cpp:1367-1369 saying seven hundred is stale. `Log::note`
    formats into 1200 bytes (log.cpp:356). The tooltip caps its `Saved for:`
    list at four entries; the log line carries all eight within 1100.
32. `recommendLink` (settings_view.cpp:363-367) shows `reset` whenever the
    recommended value equals the shipped one and the row's value differs,
    and a click applies `def->recommended` (:742); for this key that is the
    empty string, so the link would clear every entry, and `# ui: hidden`
    would also remove the F8 row (gen_settings_schema.py:653). Hence the
    one-condition change to `recommendLink`.
33. Under DLSS the output target is `s->recW/recH` (native_temporal.cpp:
    198-199) = `frame.recommendedWidth/Height` (native_temporal_client.h:
    52-53) = `gameGeometry` (host :598, filled at :589-594), and Elite's
    input is that times HMD Quality (system_hook.cpp:842-849), so the
    input/output ratio does not move with the width; only the absolute size
    does.
34. With the guard Adopting or Live, `cullGuard.recommended()` returns the
    runtime size times the guard factors (native_cull_guard.h:98-102) and
    that is what Elite is given (host :589-594), so `eyeTextureSize()`
    divided by `activeWidth[0]` alone reads `x1.24` at HMD Quality 1.0 on
    the Quest 3; `decodeCullGuardState` (frame_flag.h:496-501, used at
    menu.cpp:958) supplies the stage and factors the tooltip divides by.
35. `g_sizing` is cleared by a `valid = 0` publish only when
    `!g_sizing.valid || candidate.generation == g_sizing.generation`
    (native_render_settings.cpp:63-66); `g_labels` is cleared on the same
    condition and no other, and the lifecycle stops the old host before a
    new one starts (runtime_lifecycle.h:41-47, :61-64), which closes the
    query-to-publish window.
36. `edvr_log.py --file` resolves against the current directory with
    `os.path.abspath` and ignores `--target` (:293-297); `--nth N` (:271-272)
    or a full path names one of an install's logs. The 12:46 and 12:50 logs
    are build v0.16.2-88-gbe44d38-dirty with `pairs=5831` and `pairs=4627`,
    so neither is the parity doc's 5,430-pair flight on build -87 (parity
    doc :66, :71); the 20:13 log prints `result,xrGetSystem,-35` and
    `module_startup,...,result=124`; the 21:16 Steam log prints
    `module_startup,graphics_unavailable=80004002` and no `runtime,` line.
    All read for this revision.

## Half-quality DLSS rejection (2026-09-15)

The Frontier graphics log `edvr_gfx_20260915_094505.log` matches the installed
qualified build `v0.16.2-135-gd0971fd-dirty`. Environment: Pimax OpenXR, Pimax
Crystal Super, 90 Hz, native XR targets 4068x4016 per eye, DLSS 310.7.0 with
preset K. The symmetric game field of view expands the game recommendation
to 4857x4016 while the XR targets stay unchanged.

At 09:46:49.399, HMD Quality 0.5 produces 2428x2008. The log explicitly rejects
that input as outside every DLSS mode's range for a 4857x4016 output and runs
the pass's own history instead. Earlier successful feature creations report a
minimum of 2429x2008. The odd output width is halved downward by the game,
leaving its real input one pixel below the runtime's reported minimum.

Ruled out: changing HMD Quality switches the configured AA mode off, because
the native settings remain `dlss` through the size change and the range
rejection occurs before the later manual off/on/dlss cycle at 09:47:21-23.
Ruled out: missing depth caused this rejection, because the explicit failure
is the DLSS render-size range, and matching 2428x2008 depth is found at
09:46:49.416 without restoring DLSS.

The correction must keep the game recommendation and temporal output
consistent with Elite's integer half-size input, without weakening NGX range
checks or inventing pixels in the submitted texture. Desktop geometry and NGX
checks must qualify the exact odd-width case before the next flight.

`NativeCullGuard::recommended()` now aligns widened recommendations to even
dimensions, within the existing 16384 texture limit. The native host uses
that single result for the game's published recommendation and temporal AA's
output target. Thus 4857x4016 becomes 4858x4016, and Elite's half-size input is
2429x2008. The runtime swapchains and canonical crop calculations are
unchanged; NGX range checks remain strict. This correction is scoped to the
widened game targets that caused the failure, not the runtime's raw sizes
when the cull guard is inactive.

The regression fixture uses the flight's symmetric Pimax frusta to reproduce
the old 4857 width and checks both corrected dimensions. It also covers
asymmetric eyes, already-even output, odd height and texture-limit rejection.

Desktop NGX qualification used the RTX 5090 and pinned DLSS 310.7.0. For
Quality, Balanced and Performance, output 4857x4016 reports minimum
2429x2008; 4858x4016 retains that minimum, while 4856x4016 permits 2428x2008.
Ultra Performance instead reports its fixed 1619x1339 input. The exact
corrected pair, 2429x2008 to 4858x4016 with Performance/preset K, passed real
feature creation and evaluation (both NGX success `0x1`). The isolated
desktop probe is archived in `build/ngx_optimal_probe_4858.txt`. This proves
the corrected dimensions are accepted; visual headset quality remains the
next flight's check.

The absolute-path full build passed every gate, including 42 native cull
checks and 77 native overlay checks, archived in
`build/frontier-lod-callers-20260915/attempt-13/`. Qualified binaries stamped
`v0.16.2-139-ga97759b-dirty` were installed and verified in Steam and Frontier,
preserving settings. Retest HMD Quality 0.5 and the smaller single-line
monitor in the next Pimax flight. Expected evidence is DLSS feature creation
for 2429x2008 to 4858x4016, no render-range rejection, and normal exit.

## Startup/menu odd-height recurrence (2026-09-15)

Ruled out: aligning only widened cull-guard recommendations is sufficient,
because the next verified Frontier run used `cull=0` and rejected
1982x1956 input for 3964x3913 output before any widening. The earlier fix
qualified its widened case but left startup, Off, WaitingScene and Inert
recommendations unchanged.

Evidence: `edvr_gfx_20260915_102357.log` matches installed build
`v0.16.2-142-g11722af-dirty`. At 10:24:02.526 it explicitly reports the
DLSS render-range rejection. The accompanying OpenXR log reports Pimax
OpenXR, Crystal Super, 90 Hz, runtime base 4068x4016, and selected XR targets
3964x3913 (scale 0.974435). The native frame line reports `cull=0`. Matching
depth is found at 10:24:02.545, but TAA continues. Exit completes normally.

The generic `native temporal: engaged mode=dlss` and benchmark `aa=dlss`
labels describe the configured mode, not successful NGX evaluation. Use the
explicit `dlss: the feature is created` and rejection lines to judge this
failure; those timing windows are not evidence of DLSS rendering cost.

The RTX 5090 desktop NGX probe confirms minimum input 1982x1957 for both
3964x3913 and 3964x3914 outputs. Actual feature creation and evaluation for
1982x1957 to 3964x3914 with Performance/preset K both succeed (`0x1`). Odd
width and height together, 3965x3913, require 1983x1957; 3966x3914 accepts
that same input. Results are in `build/ngx_optimal_probe_3964.txt`, using
pinned NGX 310.7.0.

The revised policy aligns both axes of every game-facing recommendation,
including the initial query before a valid pose and every inactive guard
state. `SystemPublication` normalizes both bootstrap metadata and later
recommendations. The host gives the game and temporal provider the same
normalized `gameGeometry`, even without a feature provider. Raw runtime
swapchain sizes, field of view and canonical crop math stay separate.

Qualification: the absolute-path full build and all gates passed in
`build/frontier-lod-callers-20260915/attempt-16`. The cull fixture passes 58
checks, including every guard state, the failed odd menu height, odd widths,
unchanged even sizes, and the 16384 boundary. The system fixture passes 205
checks, including the actual OpenVR query before tracking, both eye maxima,
atomic rejection of invalid size pairs, and retention through tracking loss
and recenter. The separate NGX creation/evaluation probe above uses the same
310.7.0 DLL shipped in this build.

Build `v0.16.2-144-ge2a11e7-dirty` was installed into Frontier and Steam with
the sanctioned installer, then verified against the qualified source and
binary hashes. Both user INIs were preserved. Installation records are in
the same attempt directory. The next flight should retain HMD quality 0.5
and confirm DLSS feature creation for 1982x1957 to 3964x3914 at the current
menu setting, no render-range rejection, normal cockpit rendering and exit.
