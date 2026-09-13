# Station performance regression, 2026-09-12

A release user reports a drop from roughly 60 fps to 25-30 fps in a
station, at HMD quality 0.75, using an RTX 4080 Super and Quest 3 via
Steam Link. Their AA mode/preset, absolute eye resolution, frametimes,
prior version and same-view AA-Off comparison are not yet available. The
report establishes a serious regression to investigate, not a complete
profile of its cause.

## Measured cost in the released mesh pass

The current release includes `3189a95`; main also has `6de325e`, which
changes night-vision brightness configuration. The per-object mesh pass
is enabled with EDVR temporal AA. It reissues eligible geometry for
coverage and originally performs a small default-buffer upload, an
instance-stream copy and a compute dispatch for every draw. Its
512-instance cap is per eye. Lower render resolution reduces raster
work, but does not remove these per-draw operations.

The preceding flight's logs already show about 260 reissues per game
frame in external view, with roughly 12 microseconds sampled per draw on
the local RTX 5090. That suggests several milliseconds even before a
dense station reaches the cap; multiplying sampled durations is an
estimate, not a substitute for complete-pass timing.

A controlled D3D11 hardware replay at 2774x2740 per eye repeats an
original 228-index ship draw. It brackets the entire workload with GPU
timestamps, including capture, coverage, history matching and frame
boundaries. It checks that all expected reissues and records were
actually produced. CPU timing covers submissions between the first
reissue and history consumption. These are isolated costs, not the
user's station frametime or a predicted frame rate.

| Attempts per eye | Released GPU ms/eye | Optimized GPU ms/eye | Released CPU ms/eye | Optimized CPU ms/eye |
|---|---|---|---|---|
| 32 | 0.306 | 0.076 | 0.039 | 0.060 |
| 128 | 1.364 | 0.135 | 0.119 | 0.128 |
| 512 | 4.902 | 0.360 | 1.037 | 0.280 |
| 1024, capped at 512 | 4.999 | 0.356 | 1.141 | 0.272 |

At the cap this reduces the measured cost from 9.804 to 0.720 ms for two
eyes, about 93%. Small CPU timings vary with scheduling and query
collection; the reduction is most pronounced in the heavy workload. A
separate original 10,929-index mesh measures 0.575 ms/eye for 512
reissues with the optimization. Neither fixture is a station census, and
the user's 4080 Super has not been profiled. Ruled out: the whole
per-object motion path being negligible merely because the batched
history matcher itself costs only a few microseconds. The earlier
32-draw sampled benchmark did not measure a full crowded eye.

## Batch without losing draw-time inputs

Transforms sharing unchanged scene constants, instance stream, pool view
and eye output now share one capture dispatch. Coverage still runs with
the original vertex shader immediately after the original draw. It reads
copied instance IDs directly, so it no longer needs the transform
capture to finish first. All original draw arguments, per-mesh geometry
generations, matching rules and visible-depth checks remain in place.

The instance stream is copied once per batch when it fits the fixed 1
MiB snapshot. Larger streams retain bounded per-draw copies; no geometry
is excluded because its stream exceeds that fast-path size. The per-draw
constant buffer uses WRITE_DISCARD renaming instead of an
UpdateSubresource copy, removing another repeated GPU upload. Moving the
record-cap check ahead of expensive state queries avoids paying those
queries for excess station draws.

Source writes flush pending captures before changing their inputs.
Copy/Update paths use the existing write relay; CopyStructureCount now
also notifies it for its four-byte destination. Map flushes before the
real Map, since dispatching against an already mapped buffer would be
invalid. Binding changes, eye changes, temporal consumption and frame
boundaries finish pending work. GPU-writable pool or instance buffers
retain immediate capture, because UAV writes need not pass through a CPU
write hook. Unknown command lists flush and invalidate history, as
before.

The preceding flight's object probe identifies the actual pool as a
688,128-byte dynamic buffer with shader-resource binding only (`bind
0x8`, CPU write access `0x10000`). It therefore uses batching, with its
Map writes providing the required flush boundaries.

Owned compute work temporarily disables and restores predication,
including uploads and query brackets. Microsoft documents that
predication can suppress copies, updates and dispatches, and that
Begin/End are invalid while the predicate is set. The regression checks
this state transition explicitly. [SetPredication
documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-setpredication)

There are no normal-play GPU readbacks, explicit waits, resolution
changes or new settings. The existing 512-instance bound remains. The
logs distinguish coverage timing from batched capture timing and report
instances per batch. The existing EDVR-at-door GPU timer starts after
mesh view preparation and cannot be treated as the complete cost of this
earlier draw work.

## Validation

The targeted production-code regression passes 1,405 WARP checks. New
cases cover all 512 queued records, pool and instance rewrites,
Map/Unmap ordering, eye switches, UAV-writable input fallback, large
instance streams, predication and CS slot restoration, and unknown
command-list invalidation. Existing coverage, geometry generation,
material-family, two-eye and temporal-consumer checks remain intact.

The NVIDIA replay compares 58,122 vertices from twelve actual ship
meshes across three captured frames. Maximum reconstruction error
remains 0.000306 input pixels, and every original raster depth sample
receives the matching coverage. The complete-pass benchmark now lives in
the optional local capture replay harness and checks actual record
counts, rather than inferring cost from a sample timer alone. Local
measurements and fixtures are in
`build/review_motion/sep12/station_perf/`; no game assets are included
in source control. Full-build and DLL smoke results follow below.

The expanded original-material replay also passes: 240,105 compared
vertices across the earlier hull shader variants, maximum error 0.001658
input pixels (within the established 0.003-pixel bound). The full
absolute-path worktree build passes the rendering/runtime regressions
and the 252-key configuration contract.

The built DLL also passes the NVIDIA hardware smoke test, including DLSS
conventions and sharpening checks.
