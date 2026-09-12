# Smoke captures at 12:02, 2026-09-10

**Follow-up:** the [12:19 capture review](review-smoke-capture-1219.md)
records the negative result of the particle-skip diagnostic below and its
replacement with a UI-depth comparison while AA remains enabled.

The two new runs are `eye_120240` and `eye_120244`, with full censuses in
`edvr_gfx_20260910_115647.log`. The installed takeover build identifies as
`v0.14.1-160-g794ee65-dirty`, linked at 17:50:59 UTC. Both `drives_smoke`
and `heat_haze` remain off. Temporal mode is DLSS during both captures.

## What the images and captures establish

Both whole-eye images show a ship and long brown twin trails. The trails,
including their segmented appearance, are visible in the raw input crops
as well as the treated whole eye. They are not solely an image created by
the final DLSS resolve. This does not rule out an interaction with earlier
EDVR processing, including jitter or depth changes; raw means before the
final temporal resolve, not a stock-game control.

The new census instrumentation worked: both censuses recorded eye and
offscreen work without truncation or resource overflow. Substituted
particles now appear in both the census and binary ledger.

| Evidence | 12:02:40 | 12:02:44 |
|---|---:|---:|
| Census eye draws, two frames | 1,691 | 1,433 |
| Census offscreen draws | 945 | 1,077 |
| Smoke-switch early skips | 27 | 27 |
| Heat-haze early skips | 8 | 0 |
| Recorded known smoke/haze VS draws | 0 | 0 |
| Recorded `B12F7A618E1BDE98` draws | 24 | 24 |
| Recorded `EB787F983BC1F5A3` plume draws | 16 | 2 |
| Ledger eye draws, nineteen populated frames | 16,044 | 13,533 |

Here "known smoke/haze" means the five hashes the installed switches
matched, not every effect the player calls smoke or haze. The continued
visibility of the trails proves that suppressing those hashes is
insufficient. Merely expanding the draw API/count gates did not solve it.

## A correction to the earlier shader identification

The other agent's addition of `203DF51758AADC4D` to `drives_smoke` was not
supported by an exclusive ship-exhaust identity. Both it and
`B12F7A618E1BDE98` were already recorded in the planetary body/ring target
of the FSS scanner: see [the measured layers](fss-scanner.md#the-layers-as-measured).
The scanner analysis also records the repeated 5,334-index geometry.

The new live census places six `B12F7A618E1BDE98` draws per eye beside
`9FFA5D5E79F04873`, another shader from that planetary pipeline. Their
pixel shader `42AC0CACC9CDF72B` samples 4,096-by-1 gradients, a cubemap and
a noise texture. Its disassembly uses radial-distance fades and lighting;
this is not evidence of a ship trail. The six draws persist identically
through both ledgers despite the changing ship view.

The firm conclusion is that these hashes are **not exclusive exhaust
selectors**. `203DF51758AADC4D` has been removed from the smoke switch;
`B12F7A618E1BDE98` has not been added. The exact division of planetary
surface, ring and atmosphere between them is not needed for that decision.
The previous review's "smoke volume" label and the original journal's
claim that its presence explained the surviving haze are retracted.

## Candidates that actually remain

Two other particle families draw into the eye using the same 1,024-by-1,024
texture-array resource and 92-byte particle vertex layout:

| Vertex shader | Pixel shader | First census, first left eye |
|---|---|---:|
| `9F4BBCFCD3B68BC9` | `2BAE3742FEB916D9` | 7 draws, 42–234 indices |
| `9AEC596A2B036EA6` | `3789CA2062E196FB` | 13 draws, 12–252 indices |

The first pair's captured bytecode explicitly constructs billboards,
interpolates two frames/layers of a particle atlas, multiplies by particle
color and alpha, and fades against the sampled scene depth. It does not
write depth. The census reports premultiplied-alpha blending and no depth
write. That is a materially different path from the additive ribbon the
current smoke-depth coverage shader handles.

The second vertex shader has previously been observed in the witchspace
starfield. The present normal-flight capture proves that label is not an
exclusive scene classification. Its bytecode and its current pixel
shader are absent from the available Steam shader dump, so a fresh
creation-time shader dump is useful before attempting any replacement.
Neither hash has been added to the permanent smoke switch or granted
another shader's transcription.

The originally suspected generic plume, `EB787F983BC1F5A3`, is present,
but only one 12-index draw in the first left eye of the second census
(and none in that frame's right eye). Its sampled diffuse resource in
the first census is a 1-by-1 texture. These are useful differences, not
enough information to assign a specific visible feature to that draw.

## Correction and next controlled test

The switch regression harness now explicitly requires the planetary
`203DF51758AADC4D` shader to forward even with drive smoke off. All 3,895
checks pass, including the existing indirect-draw and capture tests.
The settings help and runtime log no longer promise that the switch
removes every particle trail.

The next diagnostic uses the existing, reversible census skip facility:

```ini
[advanced]
census_skip = vs:9F4BBCFCD3B68BC9, vs:9AEC596A2B036EA6
glare_shader_dump = 1
```

This temporarily withholds both candidate billboard families from eye
draws. It can remove other particles using those families, including the
known witchspace use; it is a diagnostic, not a shipping fix. Keep the
scene and temporal mode comparable to these captures. If the brown
trails disappear, restore one family at a time to establish which one
contributes. If they remain, these candidates are eliminated and the
offscreen/composite paths remain available in the captured census.

After the diagnostic, clear `census_skip` and turn `glare_shader_dump`
back off. The shader dump setting is read at device creation, so the
prepared test is intended for the next game launch.

Analysis scripts, converted images, log snapshot and build results are
under `build/review_motion/smoke1202` in the takeover worktree. No temporal
history or coverage shader has been changed based on these candidates.

## Installed state after this review

The corrected build (DLL file time 12:12:42 local) passed the full build,
all 3,895 switch/capture regression checks and the GPU smoke test. Its
version still reads `v0.14.1-160-g794ee65-dirty` because the source changes
remain uncommitted; the link timestamp distinguishes it from the prior
takeover build.

Both patch DLLs and the diagnostic INI have been installed and verified by
SHA-256 against the prepared files. The only active setting changes are
the two diagnostic keys above. The AA mode, smoke/haze switches and all
other active settings were preserved. Prior files, candidate settings and
hashes are under `build/review_motion/smoke1202/deployment/plan.json` and
its adjacent `before-*` files. The diagnostic is pending the next live
comparison and must be cleared or narrowed after that result.
