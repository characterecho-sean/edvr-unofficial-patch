# OpenXR submission optimization — September 14, 2026

This implements the first wave of the [native performance
review](openxr-performance-review-2026-09-14.md): combine producer treatments,
reuse capture views, and simplify the final scene draw. The transport remains a
separate XR device with synchronous source snapshotting, keyed-mutex transfer,
private captures and the existing shutdown/drain protocol. Hidden-area masks
and copy elimination remain subsequent work.

## Changes and contracts

`NativeRuntimeHost::capture` sends FSS treatment, temporal AA/DLSS, sharpening
and EDVR menu composition through one synchronous render callback. Each
provider still runs when acquired, including passthrough bookkeeping. The
selected texture is retained until the existing separate copy callback has
consumed it. Failure ends the chain before capture; FSS healing still consumes
the temporal eye through `skip`; raw pixels keep their jittered FOV. (Superseded
2026-09-16: the heal now runs after the temporal pass on its output and no eye
is skipped; the skip had put a raw left eye beside a DLSS right for the whole
arrival window, see fss-scanner.md.) (superseded 2026-09-16: `healPair`
delivers the heal with this frame's other eye; the target eye's sharpen, menu
and capture are deferred to the other eye's submit) The ending
producer GPU marker remains after Submit, with the previous
acceptance/invalidation rules.

On the all-provider, non-healed, GPU-timed separate-device path, this removes
three callbacks per eye: six become three, or twelve become six per stereo pair
after resource warmup. Disabled GPU markers reduce the counts further.
Borrowed-device fixtures also have an explicit validation callback, which
remains separate. No game-context work is dispatched from an idle XR owner.

Each `EyeCapture` slot caches a linear and an sRGB shader-resource view lazily.
Views follow the owned texture when a previous pair is retained by swapping
buffers. Replacing the texture, resetting or closing the capture retires its
views. Metadata changes choose the correct interpretation without recreating an
already cached view. The final scene renderer continues to validate both inputs
before acquiring runtime images.

The fullscreen scene blit writes the whole target, so it no longer clears that
target first. Diagnostic triangle clears remain. Context-state restoration,
deferred command recording and both eye flushes remain unchanged in this wave;
altering them requires separate measurements.

## Measurement contract

The September 15 [submit-overlap
follow-up](openxr-submit-overlap-2026-09-15.md) replaces the single
early-session window with recurring windows and changes the transfer/compose
wall boundaries. The measurements below describe this September 14
implementation.

`temporalMs` and `menuMs` now measure treatment wall time inside the combined
callback, excluding its queue wait. They still include driver calls and are not
exclusive CPU or GPU busy time. Submit, transfer, compose and producer GPU
marker boundaries are unchanged. Comparisons with older temporal/menu wall
fields must account for their former separate rendezvous.

The host collects one bounded diagnostic window: skip 64 eligible complete
pairs, then retain 256. Only complete accepted non-withheld pairs with valid
clocks and nonzero input sizes are admitted. Changing submitted dimensions
restarts warmup before the window fills. No arrays grow, no GPU queries block,
and there is no per-frame file output. After the window fills, extra treatment
clocks stop and one `native_submit_window` line reports:

- The first/last sequence, sample count, submitted eye dimensions and XR target
  dimensions.
- Mean graphics callbacks per pair and cumulative scene SRV creations.
- p50/p95/p99 of Submit wall time, the combined treatment rendezvous and
  execution inside that callback. The rendezvous includes treatment execution;
  those fields must not be added together or have their percentiles subtracted
  to invent a wait percentile.

This is an early-session observation window, not a scene-wide benchmark. Keep
settings and the scene steady for the comparison, and match actual resolution,
runtime, headset, refresh rate and DLSS version. Cache counts include warmup;
two or four creates can be normal depending on whether retained-pair rotation
has allocated a second generation. Color/size changes can legitimately increase
them. Existing GPU timing remains available independently.

An isolated prototype attempted to compare restored and unrestored immediate
state on a hardware D3D11 device with a fake runtime. It targeted 4096 × 4096
eyes in A/B/B/A order, with 16 warmup pairs and 128 samples per round, draining
the GPU outside each timed CPU interval. It identified the NVIDIA GeForce RTX
5090 (driver 32.0.16.1692) but hit the 45-second watchdog before producing any
timing samples. This does not establish either a benefit or the precise cause
of the stall. No game or OpenXR session was launched.

The prototype's sources and executable are archived locally under the ignored
`build/openxr-submit-state-experiment/` directory. Its optional state mode and
benchmark entry point are excluded from the implementation. Keep a watchdog
when revisiting that experiment; production state restoration remains enabled.

## Qualification

The full absolute-path `build.bat` run passed all gates for
`v0.16.2-123-gb00edcb-dirty` on `codex/openxr-submit-performance`. Its final
log is `build/openxr-submit-performance-build-final.log`. The native host
passed 531 checks, stereo passed 12,254 and capture passed 151, with zero
failures. Installer payload verification passed, as did the configuration
contract covering all 255 settings. DLSS 310.7.0 was verified against the
pinned SDK and carried in the installer. Documentation reflow, local link
checks and `git diff --check` passed.

The Frontier package was installed through `tools/install_edvr.py --target
frontier --all`, following its successful dry run. A separate `--verify-only`
pass verified the native DLL pair, loader and bootstrap configuration. The
user's `edvr.ini` was byte-unchanged across installation. The Windows-selected
runtime remains configured. The installation receipt in the Frontier product
directory is `edvr_native_receipt.json.pre-b00edcb-20260914-183411.bak`.

The older, already-exited Steam process was terminated after preserving its
[investigation evidence](steam-missing-terrain-exit-2026-09-14.md); its DLLs
were not changed. Frontier is ready for the combined flight below. No game was
launched during installation, and no in-game speedup has been measured.

The new host fixture exports private test providers from the diagnostic
executable and exercises the real owner/render rendezvous. It checks stage
order and producer thread, distinct intermediate outputs, passthrough
bounds/jitter, healed-eye temporal skipping (since 2026-09-16: the heal reading
the temporal output, no skip), failure at each treatment stage,
and no-render submissions. Capture tests check view reuse, color changes,
resizing, reset and retention. Stereo tests cover RGBA/BGRA, gamma/linear,
crops/flips and target corners; existing borrowed-state and runtime failure
tests remain.

## Combined Frontier flight

Use the Windows-selected runtime and launch Frontier normally after
installation. Hold the initial menu steady for roughly ten seconds so the
diagnostic window can finish. Then check the cockpit and on-foot view while
still and moving the head, especially text and geometry with DLSS/TAA. Toggle
sharpening and the F8 menu/floating monitor, take an Insert eye dump, recenter,
and exit normally. Check a render-resolution change across its required restart
separately if convenient. The same build should later be checked on the
available Pimax/SteamVR OpenXR and Quest/Meta or VDXR paths.

Confirm the installed build with `tools/install_edvr.py --verify-only`, then
read matching `openxr` and `gfx` logs through `tools/edvr_log.py`. Look for
`native_submit_window`, active temporal/sharpen/menu treatment, valid GPU
timing and normal teardown. A correct flight qualifies integration; it does not
establish a speedup without a controlled baseline at matching dimensions.

The September 14 Frontier flight loaded the expected optimization pair and
reported six callbacks per stereo pair and four cached scene-view creations.
Sean reported normal general rendering, but Elite remained alive after exit and
landable-body LOD did not update. Native cleanup completed; overall flight
qualification remains incomplete. Further optimization is paused for the [LOD
and exit investigation](steam-missing-terrain-exit-2026-09-14.md).

The September 15 AA-off follow-up, with the frequency compatibility change,
restored landable-body LOD and normal process exit according to Sean. The
matched logs confirm an actual SteamVR-provided 89.9998627 Hz and complete
native shutdown return in approximately 379 ms. The traced game consumer
divides 1000 by the reported frequency without a zero guard, explaining the
invalid interval produced by the previous unavailable/zero response. This
resolves the reported LOD/exit reproduction; it does not establish a speedup or
replace the remaining temporal-treatment and runtime coverage above.
