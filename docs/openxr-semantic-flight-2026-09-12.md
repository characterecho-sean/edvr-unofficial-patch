# Frontier semantic census flight

## Result and evidence

The user reported that the Frontier run seemed normal on the established
Pimax-through-SteamVR test setup. Both logs were independently checked with
`tools/edvr_log.py --expect-build f3c205e`, then their full contents retrieved
through the same tool. They identify `v0.15.1-61-gf3c205e`. Raw logs and the
local parsing artifacts remain outside the committed documentation.

All 14 observed export calls have matching begin/end records. The runtime
shutdown call returned normally. The capture contains 89 method records across
the same 19 historical methods seen previously, with 16 correlated matrix
records. There are no diagnostic-read fault flags, truncated records, log-cap
notices or wrapper-cache exhaustion messages. This supports the new census's
operation in this run; it does not establish every lifecycle or feature
acceptance case.

The graphics log confirms LiveCopy, the precompiled temporal shader warmup, and
a final reported render-to-submit cumulative count of 13,749 valid samples and
zero invalid samples. All 16 sampled eye textures match the published graphics
device and adapter. These texture samples are `2774x2740`, format 27, single
sample. The descriptor probe's existing late-validation boundary still applies.
These timing counters remain distinct from matched-frame SteamVR correlation
and overhead qualification.

## Export and caller contract

| Export | Observed calls | Result |
| --- | --- | --- |
| `VR_InitInternal` | 1 | Scene application type 1; token 1; error 0; approximately 47 ms |
| `VR_IsInterfaceVersionValid` | 1 | `IVRSystem_012` accepted |
| `VR_GetInitToken` | 5 | Token 1 throughout this initialization generation |
| `VR_GetGenericInterface` | 6 | System, ExtendedDisplay, Compositor, Chaperone, then two Compositor re-requests; all non-null and error 0 |
| `VR_ShutdownInternal` | 1 | Returned after approximately 52 ms |

The three Compositor getter results are identical within this generation. There
is no in-process re-init in the capture. Init, later System work and rendering
involve three different game threads; shutdown occurs on the later System
thread. All captured callers are attributed to the game executable. Geometry
and pose queries must be safely available outside the render thread and before
the first compositor wait. The first recommended-size response precedes that
wait by approximately 2.62 seconds.

## Geometry and tracking

The initial recommended render size is `4268x4216` per eye. Both hidden meshes
report zero triangles and no vertex pointer. `IsDisplayOnDesktop` returns
false. The sampled HMD is connected and valid; initial absolute-pose calls use
seated origin 0, a one-pose array, prediction 0 seconds and tracking result
200. The three observed tracking-space selections also use seated origin 0.

Both eyes supply all three of these DirectX near/far pairs:

- `0.025 .. 50000`
- `0.1 .. 1000`
- `1 .. 50000`

The third pair extends the historical two-pair inventory. A backend must use
the planes passed by the caller instead of special-casing the previously
recorded pair set. Projection calls also appear on two game threads.

Raw left-eye tangents are approximately `(-1.52926576, 1.03239238, -1.26479411,
1.26479411)` in left/right/top/bottom order; the right eye mirrors the
horizontal asymmetry. The initial eye-to-head matrices have identity rotation
and translations of approximately -0.03025 and +0.03025 metres along X.
Reconstructing the eight sampled projection matrices from their raw tangents
and supplied planes gives a maximum absolute element difference of
approximately `1.1e-7`.

These are game-facing observations through the existing EDVR hooks on SteamVR.
They are regression vectors, not native PiOpenXR geometry or a reason to
discard native cant/pose/FOV metadata. Later geometry changes can exceed the
census budget.

## Properties, events and skybox

All sampled property errors are success. Strings are intentionally omitted; the
following describes the call contract only.

| Property | Call/response |
| --- | --- |
| Manufacturer name, 1005 | String capacity 128; required length 1 |
| Model number, 1001 | String capacity 128; required length 14 |
| EDID vendor, 2011 | Integer result 53826 |
| EDID product, 2015 | Integer result 4121 |
| Display frequency, 2002 | Approximately 89.99986 Hz |

These device-specific values must not become hardcoded backend responses. Their
availability and semantics need a runtime-backed policy, including
unsupported-property errors where OpenXR cannot provide an equivalent.

The first four successful event polls all return numeric event type 114,
invalid device index and a 40-byte caller event buffer. That event value has no
enumerator in the pinned v0.9.20 declaration. Preserve this as an unidentified
observation; do not infer a quit/focus mapping from its number. The four
successful-event records are exhausted during startup, so this run says nothing
about later event types. Empty polls have their separate four-record budget.

All three `SetSkyboxOverride` observations pass six textures and return
success. The first texture has DirectX API type, automatic colour space and a
non-null handle. The census does not inspect all six faces or their D3D11
descriptors. The recorded count prevents assuming a one-texture skybox
contract; the earlier report of a 1x1 texture described dimensions, not a
complete array inventory. `SetSceneColor` supplies transparent black.

## Next implementation boundary

This is sufficient evidence to proceed with reusable OpenXR lifecycle and
supplied-FOV projection code under desktop tests. It is not sufficient to claim
a game-ready OpenXR backend. The core must preserve frame call order, explicit
no-render/loss outcomes and caller-provided near/far planes while keeping
device/session creation, initial native geometry, stereo resource submission
and game-event/property policy as explicit integration work.

The next native-runtime harness needs real session startup, spaces, format
enumeration, pose/FOV acquisition, frame submission and controlled shutdown.
Skybox descriptors, late events, re-init, focus/loss/recenter behaviour,
telemetry overhead and broader feature qualification remain open. The installed
Frontier DLLs remain the tested OpenVR census build until a subsequent
game-facing change is ready for review and testing.
