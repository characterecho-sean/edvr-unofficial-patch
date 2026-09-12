# OpenXR core policy checkpoint

The [Frontier semantic flight](openxr-semantic-flight-2026-09-12.md) supports
the next reusable core components: lifecycle/frame ordering and conversion of
supplied runtime FOVs to the historical OpenVR projection forms. Luna drafted
these components; parent review corrected ownership, failure handling,
arithmetic bounds and test coverage. They are desktop-tested building blocks,
not a linked game backend or native-runtime session harness.

## Session and frame ownership

`src/openxr/session_state.h` accepts typed OpenXR dispatch pointers, borrowed
instance/session handles and an explicitly selected environment blend mode. The
owner must create and destroy those resources and externally serialize access
to the policy. The class cannot be copied or moved, and its destructor calls no
runtime functions. An initialized policy requires all six lifecycle/frame/event
entry points.

Event polling is capped at 32 calls per tick. READY and STOPPING are returned
as actionable states so the caller can start or stop the session before polling
onward. Other events can be delivered to a synchronous observer for later
reference-space, input or event-bridge handling; their data is borrowed only
during that callback. This component does not map an OpenXR state to an Elite
quit event.

A session starts only after READY. STOPPING closes an open frame with zero
layers before ending the session. A subsequent IDLE/READY cycle may start the
same session again. Focus and visibility changes have distinct states. Session
loss, instance loss and their pending states remain distinguishable from
ordinary shutdown. These rules follow the Khronos
[begin-session](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrBeginSession.html)
and
[end-session](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrEndSession.html)
contracts.

Each frame follows wait, begin and one end attempt. The policy retains the
predicted time and no-render decision internally; caller mutation of a public
frame record cannot change them. Frame tokens identify the policy owner,
initialization generation and sequence. Stale tokens and overlapping frames are
rejected before dispatch. No-render, invalid pose, an unready renderer,
stopping or pending loss produces zero layers. Layer resource validation and
pose/FOV association still belong to the future renderer.

`XR_FRAME_DISCARDED` is a successful begin with an end obligation. A successful
wait reporting session-loss pending still reaches begin, and any opened frame
can be closed without erasing the pending-loss outcome. This follows the
Khronos
[wait-frame](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrWaitFrame.html)
and
[begin-frame](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrBeginFrame.html)
contracts.

Negative runtime errors stop further dispatch conservatively. The policy does
not retry a failed end with different layers; Khronos [recommends discarding
such a
frame](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrEndFrame.html).
The future owner must handle teardown/recovery rather than turning this policy
outcome into a game crash or automatic quit. Reset cannot silently forget
running resources; `abandonAfterOwnerDestruction` explicitly invalidates
outstanding tokens and clears borrowed handles only after the owner has
destroyed them.

## Projection conversion

`src/openxr/projection_math.h` accepts a supplied `XrFovf`, finite positive
near/far planes and the historical DirectX/OpenGL convention. It returns raw
tangents or a row-major OpenVR matrix. Horizontal and vertical asymmetry are
preserved. Valve documents the raw API's reversed vertical names: its top
parameter is the negative-Y edge and bottom is the positive-Y edge. See the
[driver
documentation](https://github.com/ValveSoftware/openvr/blob/master/docs/Driver_API_Documentation.md)
and [raw projection
formula](https://github.com/ValveSoftware/openvr/wiki/IVRSystem%3A%3AGetProjectionRaw).

This helper supports ordinary forward-facing frusta with edge angles strictly
inside +/- pi/2. Invalid, reversed or degenerate FOVs, unsupported conventions,
nonfinite/invalid clip planes and matrices unrepresentable as floats return
failure without changing output. Double intermediates prevent overflow in
otherwise representable large clip ranges; bounds are checked before narrowing
to floats. Infinite planes and wider/flipped frusta are not silently
approximated. The future interface layer must define their error policy if such
requests are observed.

The helper applies no jitter, crop, cant correction or fallback headset
geometry. The three observed clip ranges are regression vectors, not hardcoded
supported pairs. Native view poses/FOVs must be preserved separately through
all future EDVR image processing.

## Verification and remaining integration

The focused session harness checks 551 assertions, including typed arguments,
borrowed handles, exact call traces, READY/start guards, focus/visibility
changes, no-render frames, stop/restart, stale owner/generation tokens, failure
at all six dispatch stages, distinct lost-handle outcomes, positive loss codes,
discarded frames, explicit owner teardown and bounded event routing. The count
includes repeated fake-runtime contract checks, not 551 independent scenarios.

Projection tests compare independent expected matrices, horizontal/vertical
edge rays, DirectX/OpenGL depth endpoints, both captured eyes and all three
observed plane pairs. Invalid-input tests check that complete output objects
remain unchanged, including arithmetic overflow and tiny-angle cases. Both
tools have a read-only dry-run path and are included in `build.bat`.

The full absolute-path build passed with both new gates, the existing ABI and
paired-proxy WARP/LiveCopy checks, GPU timing and temporal/motion regressions,
Python tool self-tests and the 252-key config contract. This validates the
desktop checkpoint; it does not exercise a native OpenXR runtime.

A native loader/session owner, D3D11 binding, startup pose/FOV acquisition,
reference spaces, swapchains, stereo submission and EDVR feature integration
are still absent. Game property/event policy, six-face skybox handling and the
remaining hardware/timing gates also remain open. The current Frontier
installation stays on the verified `f3c205e` OpenVR census build; these
desktop-only components require no additional Frontier flight yet.
