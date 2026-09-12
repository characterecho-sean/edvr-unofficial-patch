# GPU query ownership and native desk validation

This work resumes the approved OpenXR GPU instrument after the verified
Frontier startup and exit fixes. The earlier drafts in the ignored
`build/rejected-gpu-drafts` directory remain rejected; their executables were
not rerun. These replacements are desk components, not active game hooks or new
Monitor values.

## Native query adapter

`src/d3d11/gpu_span_d3d11.*` implements the existing `GpuSpanDriver` contract
with real typed D3D11 APIs. Construction retains and checks the device and its
canonical immediate context, recording the actual OS thread. Operations reject
other threads before reading mutable query state. Explicit owner-thread
shutdown closes an open scope once and releases queries; destruction issues no
context commands. The D3D11 module and custom callback state must outlive all
COM objects held by the adapter.

Each of eight slots has empty, idle, open, pending and failed states. Query
creation cleans every partial allocation. A completed slot reuses its queries;
a pending slot cannot be reopened. Issued-marker and ready-marker masks retain
actual timestamp values across partial readiness and exclude unissued markers.
`S_FALSE` remains pending, only `S_OK` makes data ready, and other HRESULTs
fail. Every native GetData call uses `DONOTFLUSH`. An uncertain disjoint End
permanently stops new measurements while allowing cleanup without a second End.

These rules implement Microsoft's [GetData
contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata)
and [timestamp validity
contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_query_data_timestamp_disjoint).

## Shared disjoint ownership

`src/common/gpu_disjoint_clock.h` manages eight reusable disjoint records and
32 simultaneous leases per record. Inert tokens carry a unique clock identity,
record generation and individual lease serial. Copied, foreign, released or
stale tokens cannot end or release somebody else's interval.

An explicit frame or the first standalone interval owns the sole Begin/End.
Nested intervals borrow it and only end their own lease. Closing the parent
invalidates unfinished borrowers. Ended leases can read the same cached
frequency while other leases still exist; references prevent recycling, not
polling. Pending records stay pending until completion or a 2000 ms elapsed
timeout. Failed, disjoint, zero-frequency and stale results remain invalid.
Ring/lease exhaustion skips measurement. All mutable operations use the
backend's current device/context/thread identity. Shutdown is explicit and
invalidates outstanding tokens.

`src/d3d11/gpu_disjoint_d3d11.h` connects this policy to the native adapter in
frequency-only mode: one disjoint query per record, with no unused timestamp
allocation. Borrowers retain their own timestamp pairs. No game timer has been
converted to this API yet.

## Review and validation

Root review rejected the new initial Luna drafts before execution: one reused
pending queries and failed to cache timestamp values; the other made results
unreadable while leases existed and leaked records on reuse. The retained
implementations and tests replace those paths.

The first full build passed in `build/gpu-desk-reviewed.log`: 89 shared-clock
checks, 276 typed adapter checks, and real WARP clear/copy workloads in both
eye orders. The tests inject allocation failures at all seven positions,
including a non-null failed output and a null successful output; query failures
and unexpected success HRESULTs; partial readiness; and uncertain closure. They
check exact releases, OS-thread rejection, distinct-device and fresh
deferred-context rejection, reuse, and incomplete frames with unissued markers.

The WARP harness loads the absolute System32 D3D11 DLL and retains it until
after all COM references are released. Tests run in an owned hidden child with
a 30-second timeout and Windows error-dialog suppression. Dry-run exits before
module loading or GPU work. Real workloads run inside the measured intervals;
exact pixel readback and test-owned Flush/Map happen outside them. The adapter
itself never flushes or waits. The first run measured frame 101 at 0.5982 ms
outer (0.3125 ms left, 0.2851 ms right), and frame 102 in the reverse eye order
at 0.4950 ms outer (0.2444 ms left, 0.2501 ms right). These are WARP desk
measurements, not Elite performance figures.

The final full build passed in `build/gpu-shared-native-validation.log`,
including the native shared-clock bridge. The nested WARP workload used one
disjoint query, returned the same cached frequency to both borrowers, measured
0.3570 ms between the inner timestamps, and passed exact pixel readback. It
also reused that same query for a subsequent standalone interval. The two eye
orders passed again, as did all existing project gates. These components are
compiled into test executables only; no new GPU code has been installed into
Frontier.

## LiveCopy integration desk check

After main's PR #33 merge, the full build in
`build/livecopy-query-validation.log` passed the additional
`gpu_live_hook_test` gate. A WARP immediate context uses two stacked production
LiveCopy tables, with typed Begin/End hooks at SDK slots 27/28. The test checks
the complete base interface extent, stable device/context identity, one
physical disjoint Begin/End, two nested timestamp borrowers and a shared ready
frequency. All 42 checks passed, including exact texture readback after the
measured work, owner-thread clock shutdown and reverse hook removal. The 0.0250
ms nested interval is a WARP test result, not an Elite measurement.

Root review corrected the Luna draft's ownership comparison and callback
exception handling, strengthened interface coverage and result checks, and
extended readback from one pixel to the complete texture before running it. The
test retains System32 D3D11 through COM cleanup, runs in a hidden child with a
timeout and error-dialog suppression, and performs no module loading or work in
dry-run mode. It exercises query forwarding through LiveCopy; the existing
vtable regressions separately test runtime slot changes. The paired-proxy
census bridge also passed after the merge. None of these tests enables a new
GPU timer in the game.

## Remaining integration gate

The outer application bracket must share this clock with all existing timers:
door, temporal, DLAA, supersample, sharpening, menu, UI-content, stellar
coverage and main's celestial-motion sampler. Merely replacing the door timer
leaves nested disjoint scopes. The temporal ring retains each slot until both
timestamp polling and staging-buffer readback finish; a shared-clock migration
must preserve that dual completion. Sampled draw timers also need explicit
owner-thread closure before reset, including an interrupted active sample.
Preserve each existing metric and standalone behavior during that migration.
Owner-thread shutdown and callback/module lifetime must be established by the
integration, not assumed from the successful desk test.

After that migration, publish a CPU frame sequence at the pose boundary,
consume it before the first covered immediate-context command, and close after
the second distinct eye's final submit work. Incomplete/rejected pairs are
invalid, and later work outside that boundary stays outside the reported span.
Monitor results need original frame sequence, validity, source and age. Matched
SteamVR correlation and enabled/disabled overhead flights follow those gates;
no new flight is needed for these desk-only components.
