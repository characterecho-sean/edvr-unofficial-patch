# OpenXR eye capture and frame ownership checkpoint

This checkpoint connects Submit-style eye pairing to the standalone native
diagnostic. It does not install or advertise a game-facing compositor. Frontier
remains on the previously tested `f3c205e` OpenVR proxy pair. Work stays on
`codex/openxr-port`.

## Implemented path

`frame_boundary.h` wraps the existing session frame policy. A wait closes any
preceding incomplete frame with zero layers, then performs the next wait/begin.
Each accepted eye is validated and copied before Submit returns. Either order
works; a duplicate cannot replace the accepted eye. The second distinct eye
composes and ends exactly once. Clear after a completed pair is idempotent.
Frames with invalid geometry or `shouldRender=false` still validate submissions
and close normally, without copies or composition. The exact predicted time
belongs to the internal frame token.

The historical OpenVR declarations do not contain a modern `AlreadySubmitted`
result. This component deliberately uses the existing `InvalidTexture` value
for an unusable submission order and preserves more specific texture/device
errors. An eventual ABI facade needs diagnostics around these results.
`FrameBoundary` itself is not the 29-method `IVRCompositor_014` object and does
not implement WaitGetPoses output arrays, tracking-space changes, skyboxes,
fades or timing queries.

`eye_capture.*` retains the supplied D3D11 device and immediate context. It
validates a live COM Texture2D, canonical device identity,
DirectX/default-submit flags, color space, finite nonempty bounds, and
supported single-sample, single-mip, single-array RGBA/BGRA8 descriptors. It
enqueues a private per-eye copy on the same context before returning. Private
textures are typeless within their submitted format family, so the subsequent
view can interpret Gamma/Auto as sRGB or Linear as UNORM. Invalid input
preserves previously accepted pixels and metadata. A no-render validation
performs no allocation, copy or metadata replacement.

Copies reuse allocations when shape and format family match. The caller must
consume the preceding frame and reject duplicate eyes before replacing a
capture. These are not immutable snapshots across frames or retained EDVR
history shadows. CopyResource leaves pipeline bindings alone and supports this
same-format-family copy; it does not itself crop, flip, resize or convert
channels. See [Microsoft's CopyResource
contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource).

The diagnostic draws each eye into an offscreen sRGB target, captures it
through this policy, then blits the captured pair to the runtime swapchains. It
alternates eye order each frame. Bounds, including reversed axes, are applied
once by the blit; filtering is clamped inside the crop to avoid sampling the
adjacent half of a double-wide source. Gamma/Auto and Linear inputs use their
corresponding SRV interpretation. The output uses the selected sRGB RTV or
explicit encoding for a UNORM swapchain. Both input views and captures are
validated before any image acquisition. Timeout or another unexpected positive
acquisition/wait/release result retires the renderer without writing or
releasing an unwaited image. A hard external XR failure prevents further
session dispatch; a positive incomplete composition closes the frame empty and
retires the boundary.

This renderer owns its entire diagnostic context state and explicitly clears it
before drawing. Its blit is not safe to insert into Elite as a production pass
until D3D11 state and EDVR's binding shadow are preserved. The extra offscreen
render/copy work and 8-bit intermediate are diagnostic choices, not a
performance or image-quality claim about the eventual game path.

## Lifetime and thread contract

`runtime_gate.h` supplies a monotonic generation and one move-only operation
lease. The mutex covers only short state transitions. A stop request can return
while the lease is inside `xrWaitFrame`; it prevents new operations and defers
resource destruction until the outstanding operation releases its lease. A
single teardown owner must serialize the final destruction and generation
retirement. The gate does not interrupt the runtime wait, join threads or keep
its own C++ owner alive. Those remain responsibilities of the host. The [OpenXR
wait
contract](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrWaitFrame.html)
requires external synchronization for concurrent frame waits.

The native diagnostic now holds a lease across
poll/wait/locate/draw/capture/end, releases it before separate System pose
queries, and protects those handle-using queries with their own lease. Cached
SystemPublication reads retain their separate short copy lock and remain
available during a blocked wait. Shutdown occurs after the diagnostic's
operations return, retires publication, releases captures and swapchains, then
destroys binding, device, instance and loader resources.

The frame boundary pins the thread on which it is constructed. A future game
host must construct it on the established frame/context owner, not assume that
the VR-init thread is that owner. The gate alone is not thread marshalling, the
paired graphics capability handshake, or a complete game-runtime owner.
Shutdown during a permanently stalled runtime still cannot destroy in-use
handles safely; the standalone runner's existing watchdog is not a production
shutdown policy.

## Review and desktop evidence

Luna drafted the lifetime gate, capture component and diagnostic renderer
extension. Parent review connected them to the native frame loop, corrected
typed color interpretation, nontexture COM validation and self-copy handling,
fixed incomplete graphics state setup and crop-edge filtering, and replaced or
expanded weak fixtures. In particular, an initial capture test passed a scalar
to a whole-texture upload; the corrected fixture uploads complete arrays and
reads every pixel.

The full repository build passes with Frontier's original OpenVR DLL: lifetime
18 checks, frame boundary 87, capture 136, stereo renderer 3470, and native
argument/policy checks 24, alongside the existing regression gates and 252-key
config contract. New tests are included in build.bat with no-write dry-run
modes. The final build includes closing a recoverable local failure's partial
frame before diagnostic shutdown; uncertain XR failures still prevent further
dispatch.

The fixtures cover both eye orders; same-source rewrites; all six supported
typed/typeless RGBA/BGRA formats; sRGB and linear view creation; malformed
descriptors and foreign devices; allocation reuse/resize; no-render validation;
full, half and flipped bounds; crop edges; midgray color conversion on sRGB and
UNORM outputs; inherited graphics state; offscreen/captured triangle projection
against the earlier direct renderer and independent asymmetric ray oracle;
partial image acquisition and positive timeout; duplicate/capture retry;
incomplete/no-submit frames; hard loss and uncertain end without retry; and a
real injected blocked frame wait under the lifetime lease while cached System
data remains readable.

## Native gate and remaining work

The `57e82ca` copied-eye diagnostic passed its 20-second PiOpenXR run with the
executable, sources, loader and runtime manifest matched to the full-build
record. SteamVR and Frontier were absent before and after. The runtime reported
Pimax OpenXR 0.1.0, D3D11.1 and two 5424 x 5356 sRGB eye swapchains. The child
exited 0 after approximately 20.38 seconds, without the watchdog firing.

It published startup geometry on frame 1 before stereo, passed the existing
historical System metadata and QPC-to-OpenXR absolute-pose query, then produced
3596 successful eye captures and 1798 composed stereo pairs with alternating
eye order. There were 1800 begun frames, two zero-layer frames, 1799 valid
view/head samples and no invalid view sample. Normal session stop and cleanup
both succeeded. These counters demonstrate that the new capture/pair/blit path
ran; they are not a game performance comparison.

The user confirmed the triangle in both eyes, stability during head movement,
normal color and clarity, and normal visible closure. This supplies the visual
check for the copied-eye path in addition to the earlier direct-renderer
result. Exact executable/source hashes and run receipts remain local under
`build`.

After this gate, the next work is the owned historical compositor facade and
game-runtime integration, including pose arrays/cache, origin/recenter and
event behavior, explicit missing-method policies, export initialization, the
paired device/feature handshake and preserving EDVR's rendering paths. A
standalone pass is not permission to install an incomplete OpenXR backend in
Frontier. Runtime loss, doff/don, broader runtime coverage, game image quality
and performance remain separate qualification gates.
