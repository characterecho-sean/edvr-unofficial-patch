# Native OpenXR IPD and hidden-area masks

The successful September 15 refresh-rate flight resolved the reported terrain
LOD stall and process hang. The next compatibility additions supply effective
rendering IPD and runtime hidden-area meshes, and identify other unavailable
properties by exact ID. These changes retain Windows runtime selection and the
validated runtime-frequency query.

## Data and lifetime

`Prop_UserIpdMeters_Float` returns the Euclidean separation of the two
validated runtime eye origins. This is effective rendering IPD, not an
independently queried hardware dial setting. It uses the same eye placement as
the projection path, including asymmetric/canted eyes, and survives temporary
tracking loss through the session optics cache. Missing, nonfinite, zero and
retired values retain explicit property errors. It makes no OpenXR call from
the getter.

When advertised, `XR_KHR_visibility_mask` is enabled alongside the existing
optional refresh-rate extension. The owner queries both eyes at session
creation/start and after relevant runtime mask-change events. Event callbacks
invalidate publication and defer queries to the owner pump. Other-session,
other-view and foreign-thread events do not update the mask.

Queries use the two-call allocation protocol, allow three size retries, and
limit each eye to 8,192 vertices and 24,576 indices. Indices, counts, result
codes and finite coordinates are checked before publishing a complete stereo
revision. Missing extensions, empty masks and failed queries leave rendering
unmasked. A process admits at most 32 refresh attempts.

The legacy getter converts OpenXR z=-1 view-plane coordinates into normalized
OpenVR coordinates using that eye's unjittered asymmetric frustum. It expands
indices and insets each triangle by two pixels at EDVR's minimum 25% runtime
resolution. The guard remains fixed when the resolution setting changes. Thin
triangles may disappear and seams may render extra pixels; conversion never
expands hidden coverage. Unusually low game supersampling and temporal filter
edges still need visual qualification.

Returned vertices are immutable. Up to 64 converted eye/frustum/revision
entries remain alive until the interface is destroyed, including after a
runtime update invalidates the current publication. At the retention limit, new
queries return empty meshes rather than releasing a pointer Elite may still
use. Experimental cull modes receive empty meshes when queried because they can
subsequently change the game frustum.

The historical interface does not establish that Elite will request a new mesh
after a runtime update or a live experimental frustum change. We update future
queries and preserve old pointers; we do not claim that this refreshes an
already uploaded game mesh. Restart after changing runtime optical/FOV settings
until game refresh behavior is established. Recenter and ordinary resolution
changes do not themselves change normalized mask geometry.

## Evidence and qualification

`visibility_mask` logs extension/function availability, query outcome, revision
and triangle counts. `hidden_mesh_query` records each eye's actual returned
count/reason and bounded caller stack. An empty return is distinguishable from
an uncalled getter. `property_query` records exact getter slot, device index,
property ID and error, including with null caller error pointers. At most 128
distinct property tuples are recorded; successful IPD reads are included.
Property string contents are never logged by this observer.

Desktop cases exercise the real system facade and host with injected runtime
queries: IPD validity/retirement, asymmetric UV conversion, both triangle
windings, conservative coverage, immutable pointer lifetime, revision limits,
resolution/jitter stability, malformed query buffers, retry limits, optional
capabilities, owner affinity and events. The integrated absolute-path full
build passed every gate: 590 native, 194 system, 12,254 stereo and 151 capture
checks, plus the UI replay tests, Python tools, 255-key config contract and
actual installer resource verification. Sources matched their pre-build hashes.
This includes main through `0bab722`, with its per-headset resolution and
native UI replay changes.

The exact tested DLLs were installed and hash-verified in Frontier, with the
user's INI unchanged. Local build/source/binary and installation receipts are
under `build/frontier-lod-callers-20260915/attempt-5`. The binary identifies
its pre-commit source as `v0.16.2-131-g0bab722-dirty`; committing the verified
sources afterward does not relink it. Match the artifact hashes when reading
the next flight rather than assuming its embedded revision equals the later
merge commit.

| Installed file | SHA-256 |
| --- | --- |
| Native runtime | `8db54549baed92d29c7fec4d5ac7653763a0c4de4ba708f4bb2e91bd6e321e36` |
| Graphics proxy | `3689cd35eab5562ac6df254f7bfe6280ef33dbfb5ff7e448a5a697c2a98aebda` |

The next manual flight should check both eyes at the main menu and in the
cockpit, visible edges during head movement with AA off and DLSS/TAA enabled,
recenter, a live resolution change, landable-body LOD, and normal exit. Record
runtime, headset, actual per-eye input/output dimensions and DLSS version. The
preceding successful flight used SteamVR/OpenXR, 3984x3933 targets and AA off;
it does not qualify these newly supplied meshes.

Frontier has no saved `fix.openxr_resolution` entry at installation time.
Main's new per-headset control therefore starts at the runtime recommendation;
select the desired size in F8 before comparing performance with the earlier 80%
flight. The installer has not migrated or overwritten the live INI.

Runtime triangles and a getter call alone do not establish GPU savings. Elite
must actually draw the returned mesh before scene shading. The earlier legacy
census observed getter calls with empty meshes; draw consumption and savings
remain to be measured at fixed scene and resolution.

## References

- [OpenXR visibility-mask
  coordinates](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrVisibilityMaskKHR.html)
- [OpenComposite mask conversion and
  IPD](https://gitlab.com/znixian/OpenOVR/-/blob/openxr/DrvOpenXR/XrHMD.cpp)
- [Initial Elite API census](openxr-semantic-flight-2026-09-12.md)
- [Refresh-rate and shutdown
  investigation](steam-missing-terrain-exit-2026-09-14.md)
