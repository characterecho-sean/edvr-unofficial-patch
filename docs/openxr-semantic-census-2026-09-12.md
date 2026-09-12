# OpenXR semantic census checkpoint

This remains Phase 0 preparation. The proxy still forwards to OpenVR; no OpenXR
session, tracking loop, swapchain or projection submission is implemented. The
user deferred the remaining stationary wing-line flicker investigation so port
work can continue.

## Implemented capture

The existing `advanced.openvr_census = on` setting now enables semantic records
on 28 of the 84 typed historical methods. These include render size, per-eye
projection matrices and tangents, eye transforms, tracking origin/prediction
and HMD validity, selected scalar/string property contracts, event headers,
basic controller state, hidden-mesh counts, fades, skybox texture metadata,
chaperone colour/size/visibility, and extended-display rectangles. Other
methods retain bounded call-name observations.

Each method and caller category has room for 16 exact integer discriminator
keys, with four samples per key. Eye keys keep the right eye visible after
repeated left-eye calls; property keys separate valid historical
device/property pairs. Unrepresentable device/property combinations share an
overflow key. Successful and empty event polls use separate keys. Budgets are
not reset during the process lifetime. Exhaustion is incomplete coverage, not
evidence that later values or calls are absent.

The record reserves its budget before reading caller-owned output storage.
Disabled and exhausted captures skip those reads. An SEH guard contains faults
in diagnostic reads while leaving the runtime invocation outside the guard. The
real method is called once with its original arguments and returns its original
value. Field masks distinguish unavailable output from measured zero. Matrix
continuation lines share the observation ID. Property strings, including serial
numbers, are never copied into evidence; their property ID, caller capacity,
required length and optional error are recorded.

The generated proxy exports now wrap `VR_InitInternal`, `VR_ShutdownInternal`,
`VR_GetGenericInterface`, `VR_IsInterfaceVersionValid` and `VR_GetInitToken`.
Each export/caller category reserves at most 64 complete calls, with one ID
covering begin and end records. Records include QPC, thread, caller category,
application type or bounded interface name, return value, and explicit
error-pointer presence/readability. Interface suppression continues to run
before the real getter. Missing Init returns zero without writing the optional
error, while missing Generic retains its existing error write. Re-entry during
lazy runtime loading skips diagnostic initialization and retains the unresolved
stub result.

Diagnostic configuration/log initialization happens on an ordinary export call,
never in the proxy's DllMain. Its once-callback catches diagnostic
initialization failures internally; a separate same-thread guard avoids
recursive diagnostic initialization. The underlying forwarding remains
available if diagnostics fail. Logging still has its existing buffer/file
limits: missing end lines need investigation and are not automatically runtime
failures.

## Verification

The focused ABI harness exercises the actual typed forwarding path for matrix
returns, both-eye evidence, property lengths/errors and caller-buffer canaries.
It checks production capture gating with a counted payload, guarded unreadable
output, exact-key capacity and concurrent saturation. One thousand empty polls
through the wrapper cannot exhaust the first successful event observation.

The export harness stages the actual built proxy in fresh child processes with
inert fake runtimes. It checks enabled and disabled modes, missing exports,
runtime-DllMain re-entry, success/failure/re-init, interface suppression,
unchanged null error arguments, invalid diagnostic pointers, repeated interface
identity, exact runtime call counts and paired budget limits under concurrent
validity polling. Children have a 20-second timeout and suppress interactive
error dialogs. No installed runtime or game file is used by these fixtures.

The full absolute-path build passed, including all four actual-proxy export
children, the typed ABI harness, paired-proxy WARP/LiveCopy checks, GPU timing
lifecycle tests, temporal shader and motion regressions, Python tool self-tests
and the 252-key config contract. Hardware startup with the new census remains
untested.

## Remaining evidence and next test

The next Frontier run should include normal startup, a short cockpit/on-foot
session, and normal exit using the existing Pimax-through-SteamVR setup. Check
both DLL build identities before interpreting the logs. Review export ordering,
init/error/token values, the requested property IDs and sizes, both eyes'
initial geometry, tracking-origin arguments and any captured event headers. The
game may not perform in-process re-init in an ordinary session; its absence
from that capture does not qualify re-init.

This capture is deliberately bounded. Four projection samples per eye can miss
later near/far conventions or geometry changes. Four successful events can miss
later event types and shutdown events. Controller evidence covers packet/button
state rather than every axis and pose, and skybox evidence covers count and the
first texture's type/colour-space/handle presence rather than D3D11 descriptors
or all faces. Submitted texture descriptor evidence still has its previously
documented validation boundary. These limits must remain explicit when deriving
backend contracts.

Native PiOpenXR is an available target on the test machine, as the separate
runtime inventory records. A real session harness must still establish startup
geometry, actual formats, frame ordering, tracking and lifecycle. Matched-frame
timing correlation, telemetry overhead and broader functional qualification
also remain open. The next major OpenXR milestone is a frame submitted directly
to the native runtime; enumeration and forwarding census tests do not achieve
it.
