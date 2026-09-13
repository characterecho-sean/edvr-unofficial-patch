# Separating game capture from OpenXR graphics ownership

The [Frontier shutdown flight](openxr-shutdown-lifetime-2026-09-13.md)
established that the render caller stayed alive outside Present throughout
forwarded shutdown. The [desktop
reproduction](openxr-stopped-present-2026-09-13.md) then exercised both points
where native cleanup can lose its required callback. Extending that callback's
deadline cannot supply missing service, and a thread-lifetime observation
cannot grant exclusive access to the game's context.

The next ownership design to qualify gives OpenXR a separate D3D11 device on
the runtime-required adapter. The game keeps its own context. Game images must
cross a synchronized shared resource before the XR owner can use them; raw
textures from one device cannot simply be submitted on another. This checkpoint
implements that transfer prerequisite. The native host still binds the existing
device, and its conservative shutdown policy is unchanged.

## API basis and boundaries

Microsoft distinguishes thread-safe device object creation from
immediate-context access, which needs serialization. Its [D3D11 threading
guidance](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro)
also restricts concurrent DXGI operations. Enabling multithread protection
alone would not close EDVR callback admission or resolve the existing
render/owner rendezvous. The current hook has no complete application rendering
exclusion protocol.

OpenXR's [D3D11 binding
contract](https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/specification/sources/chapters/extensions/khr/khr_d3d11_enable.adoc)
specifies the device's adapter LUID and minimum feature level. A future
separate device must satisfy both, as well as this transfer's same-adapter
requirement. This is an ownership design to test, not evidence that the earlier
Pimax shutdown stall is resolved.

The transfer uses an unnamed, non-inherited NT shared handle and a keyed mutex.
Microsoft documents the [shared-handle creation and
closure](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgiresource1-createsharedhandle)
and the consumer's
[OpenSharedResource1](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-opensharedresource1)
operation. Only an exact `S_OK` from
[AcquireSync](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgikeyedmutex-acquiresync)
grants access: `WAIT_TIMEOUT` and `WAIT_ABANDONED` are positive values that a
generic `SUCCEEDED` check would accept incorrectly.

## Transfer contract

`SharedTextureTransfer` keeps the game-side copy inside a synchronous
`ImmediateExecutor` callback. The caller must supply a real exclusive producer
boundary; the executor interface itself grants no game-context ownership. The
consumer thread owns its separate immediate context and all public transfer
operations. Single-threaded devices and same-device pairs are rejected.

The supported inputs are DEFAULT-usage, one-mip, one-slice, non-MSAA RGBA8 and
BGRA8 textures without CPU access, in their typeless, UNORM and sRGB variants.
A typed UNORM shared texture carries the bytes to a private typeless texture on
the consumer device. The exposed output is never the shared resource. Bounds,
eye identity, projection and color-space metadata belong to the higher-level
capture integration, which is still pending; this primitive performs no color
conversion or temporal processing.

The producer acquires key 0, copies and flushes, then releases key 1. The
consumer acquires key 1, copies into its private texture and flushes, then
releases key 0. Reuse and size/family changes must respect both GPU command
streams. Shutdown must finish an outstanding handoff and drain the consumer's
submitted copies using its own context, without a new producer callback. An
uncertain handoff or incomplete drain must not be reported as completed
retirement.

## Qualification and remaining integration

The first WARP run passed 216 checks; the RTX 5090 run exposed twelve
resize/family-change failures. The detailed repeat returned `0x800705b4` after
about 109-110 ms, with zero producer callbacks for every failed copy, no
published pending handoff and no terminal device fault. That localizes the
failure to the old consumer slot's event-query drain before allocation. Ruled
out: unsupported input color formats as the explanation for those failures,
because allocation and producer-copy callbacks were never reached and same-size
RGBA reuse succeeded.

Two candidate distinctions were query polling that permits driver flushing and
placing the transfer event before keyed-mutex release. The first controlled
desktop variant changed only `GetData` flags from `DONOTFLUSH` to zero.
Microsoft warns that [polling without flushing can prevent query
progress](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_async_getdata_flag).
The existing 100 ms wait and explicit `End`/`Flush` ordering stayed unchanged.
That variant passed all 216 checks on the RTX 5090; the eighteen logged copies
completed within 0-16 ms on the coarse clock. The implementation now uses that
polling mode. Moving the completion event was unnecessary. This supports the
change on this device without establishing an NVIDIA internal scheduling
mechanism or proving actual OpenXR teardown.

The fixture reads every byte of varied RGBA/BGRA rows for all six formats,
repeats the same size and changes dimensions and families. It completes a GPU
overwrite of the source, then verifies that the consumer copy still contains
the original pixels. It verifies the consumer-device identity and private
typeless output shape, rejected inputs and caller threads, executor
refusal/recovery, and work that completed before the executor reported failure.
For final retirement it submits a fresh copy without a staging readback, keeps
the producer worker alive with callback admission disabled, and shuts down on
the consumer. Producer invocation attempts remain at 75 before and after
shutdown. A separate test-owned output reference permits pixel verification
afterward; that reference means this check does not prove destruction of every
allocation at the shutdown call.

Parent review corrected mutable-source retries, borrowed output ownership,
producer-refusal handling, resize drain ordering and NT handle closure. Invalid
inputs and allocation failures preserve a usable transfer. A published consumer
timeout can be retried using only the copied shared image; producer acquisition
timeout retains no original source for a later copy. Terminal GPU/mutex faults
retain the generation for diagnosis. The destructor requires successful
explicit shutdown and never performs context work. An integration must retain
the entire object after unsuccessful shutdown; dropping it violates that
contract.

The absolute-path full `build.bat` completed with exit 0 and all 476 source
hashes unchanged. It passed the new 216-check WARP fixture, 63 stopped-Present
checks, 201 native-module export checks, 207 bootstrap checks, the transport
matrix and the 254-key configuration contract. The final binary then passed all
216 checks on the RTX 5090, adapter LUID `00000000:00013f22`, with producer
thread 38284 and consumer thread 31476. Its eighteen logged copies took 0-31 ms
on the coarse clock; these tiny textures and synchronous checks are not a
frame-performance measurement. The tested precommit version is
`v0.16.2-72-gae584c9-dirty`. Source and binary hashes, full build and hardware
logs, and the failing/control experiment are archived in
`build/openxr-shared-device-20260913/qualification.json` and its adjacent
files.

The D3D11 debug layer was unavailable in the desktop runs, so no debug-layer
validation is claimed. Device removal, mutex abandonment and forced
acquire-timeout paths have not been qualified on hardware. There was no OpenXR
runtime, application Present callback or headset in this fixture.

This prerequisite adds a shared allocation and another copy per retained image
compared with capture on the game device. It does not establish acceptable
memory use, latency or throughput at the Pimax eye size. There is no new
configuration option or game installation in this checkpoint.

The next integration must move both eye and six-face loading captures across
this boundary, retain their existing metadata and transactional publication
behavior, bind the new XR device to the session, and keep XR rendering and
teardown on its owner. It also needs to close and retire incoming game
callbacks before deleting their captures, and retain the generation on
uncertain cleanup. The native diagnostic must then stop application Present
before shutdown and complete actual runtime teardown before another Frontier
flight is qualified.
