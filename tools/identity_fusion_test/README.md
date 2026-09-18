# Identity fusion test

This standalone D3D11 rig compares the current weapon-motion identity compute
dispatch plus post-VS capture with two programmable geometry-shader captures: a
directly sampled typed identity stream and a stream copied into the production
structured-SRV format.

```text
identity_fusion_test.exe --self-test
identity_fusion_test.exe --hardware
identity_fusion_test.exe --benchmark [--output DIR]
identity_fusion_test.exe --replay FIXTURE
identity_fusion_test.exe --replay FIXTURE --benchmark [--output DIR]
identity_fusion_test.exe --replay FIXTURE --cost-breakdown [--output DIR]
identity_fusion_test.exe --dry-run [--output DIR]
```

`--self-test` runs correctness on WARP. `--hardware` runs the same matrix on
the default hardware adapter. `--benchmark` uses a release hardware device, a
minimum 250 ms of both wall and valid GPU warmup work, and alternating
whole-batch timestamps. `--output` writes bounded JSON and CSV results.
`--dry-run` creates no device, directory, or file; the self-test also exercises
that no-write branch.

Correctness requests the D3D debug layer and checks its messages when the SDK
layer is installed. If it is unavailable, device creation falls back without
debug and the result reports `debug=unavailable`. Benchmark devices always use
`debug=false`.

The vertex shader is a small synthetic fixture with extra outputs in registers
0-3 and `SV_POSITION` in register 4, matching the observed output layout. It
does not model the instruction cost or resource access of Elite's full vertex
shaders, so benchmark numbers answer only the local identity/capture
comparison. They also exclude the immediate motion-raster consumer; separate
correctness checks read back both identity formats through GPU shaders.

The primary workload embeds the ordered 40-draw, 342,741-index counts from
historical capture `102403`. No capture files are needed to run it.
`build/identity-fusion/historical-workloads.json` is local derivation evidence,
not an input dependency. The full build creates the executable at
`build/obj/identityfusion/identity_fusion_test.exe` and runs its WARP
self-test.

`--replay` reads a bounded `EDVRIFR1` fixture prepared by
`tools/identity_fusion_capture.py`. It validates the stripped original DXBC
declarations, rebuilds the captured input layouts and resources, and compares
the production compute plus no-program stream-output path with direct typed
fusion. It then runs the production motion vertex/pixel shaders immediately
after each reconstructed opaque depth draw; the typed variant changes only the
five identity declarations from `StructuredBuffer<uint4>` to `Buffer<uint4>`.
Position buffers, identities and the complete 5120x2880 motion map must match
exactly. Replay benchmarks include the common depth/map clears, reconstructed
depth draw, producer and immediate motion raster in each timestamped batch.

`--cost-breakdown` is a separate controlled-removal experiment over five
whole-batch modes. Every mode performs the same motion/depth clears and the
same reconstructed per-draw original depth work. The modes then add identity,
identity plus capture, the full current baseline producer plus immediate motion
raster, or immediate motion raster using exact current outputs precomputed by
the source gate outside timing. The latter is an idealized bypass control, not
a production cache proposal. Source and frame preparation also remain outside
timing.

The cost report records raw batch samples, per-mode medians and signed medians
of same-round paired differences. Fifteen rotated rounds place each mode three
times in every order position. These differences include resource hazards,
barriers and state transitions, so they are not additive stage timestamps.
`full_current_baseline_minus_depth_clear_only` means the observed increment
from enabling the producer and consumer in this replay. It is not a total
feature ceiling or a whole-frame estimate because common clears remain and the
clean harness omits production Get/Restore state traffic. Before timing, the
gate poisons current outputs and proves that identity-only, capture and full
modes rewrite exactly their intended buffers. It also requires the full and
precomputed motion maps to be byte-identical with nonzero valid pixels.

Prepare a local fixture beside its JSON coverage manifest with:

```text
python tools/identity_fusion_capture.py CAPTURE --output build/identity-replay/fixture.bin --dry-run
python tools/identity_fusion_capture.py CAPTURE --output build/identity-replay/fixture.bin
build/obj/identityfusion/identity_fusion_test.exe --replay build/identity-replay/fixture.bin --benchmark --output build/identity-replay/results
build/obj/identityfusion/identity_fusion_test.exe --replay build/identity-replay/fixture.bin --cost-breakdown --output build/identity-replay/cost-results
```

The original `vs_HASH.dxbc` files must be beside `CAPTURE`. Keep captures and
generated fixtures under ignored local directories; game assets are not
required by the build's synthetic self-tests.

Replay is a controlled local comparison, not a full-game performance model. The
runtime requires exactly two adjacent frames, which is the exporter's default
for this test. It reuses first-frame geometry and binding descriptors only
where the exporter found an unambiguous stable match. The selected-mesh depth
reconstruction uses opaque `CullNone` draws and omits the rest of the scene,
material alpha and later occluders. It uses clean harness state and therefore
omits the production hook's Get/Restore state traffic. Fixture manifests record
accepted draws, skipped geometry, source hashes and every replay assumption.
The cost mode additionally records per-draw motion-raster occlusion samples as
read-only opportunity evidence. Those are EDVR replay draws under partial
selected-mesh opaque depth. They neither define a safe skip mask nor show that
Elite submits wastefully culled original draws; answering the latter needs a
separate original-draw census with the game's full material, pass and scene
state. The D3D debug SDK layer limitation described above also applies to
replay correctness.
