# Build time: where a full build's seconds go (2026-09-29)

What a full `build.bat` spends its time on, measured, and what is worth doing
about it. Follows the architecture review of 2026-09-29 (I-1, I-3) and the DLSS
performance review (X3-7, X3-8, X3-9). One machine, 8 logical cores, a build
alone on it unless said.

## Status

State: measured. Nothing has been done for speed yet; the I-8 fix and the
backup pruning of the same day are not speed changes. 2026-10-01: item 2
below is closed: the terrain_motion rig (104-115 s here, 70-158 s in the
builds of 2026-09-29 to 10-01, longer under load) was deleted with the terrain
motion hook (docs/terrain-motion-dispatch-cost-2026-09-17.md, top journal
entry), and tools\terrain_retired_test, which proves the deletion, takes 20 s
alone and 28-50 s in the pool. Measured on the first build after (198 s; a
pool of 105 jobs, 136.5 s, other builds on the machine): the wall is now
flat_mono_resolve_test, 136.5 s there and 33-174 s in the builds of this week
(it has grown with main), so the saving shows as CPU seconds, not as a shorter
wall, until that rig is looked at; it is the next item.

A full build, seconds of wall time. "Cold" was measured with a generated-shader
cache from an older tree, "warm" with the cache current; the source tree is the
same apart from two commits that do not touch build speed.

    phase                                        cold     warm
    serial head (self-tests, shaders, schema)    42.9     10.4
      of which precompiled temporal shaders      39.7      7.3
    DLL compile (d3d11, graphics, runtime)       30.8     30.7
    runner start (run_jobs self-test, plan)       1.7      1.6
    pool, 88 jobs, 8 at a time                  107.0    105.0
      summed rig seconds / 8                    100.0     96.5
    quiet rigs, one at a time                    16.3     16.2
    tail (contract, packaging checks, receipt)    2.0      1.9
    total                                       201.0    166.1

Slowest rigs, warm build (`build\rig_times.json`): terrain_motion 104.1,
engine_velocity_test 57.8, ui_quality_test 32.3, native_motion_rigs 30.6,
fsr3_engine_test 28.9.

Open, biggest first:

1. Head on a cold shader cache: +32 s. 27 s of it is two single-threaded HLSL
   compiles inside temporal_shader_build (temporal_aa_cs 18.4 s,
   temporal_aa_fast_cs 9.0 s), one after the other; they are independent. Any
   build after a shader-source change pays it. Seen in one cold build.
2. (Closed 2026-10-01: the rig was deleted; see the State line.) The pool's
   wall was terrain_motion, not the CPU: 105-107 s against summed/8 of
   96-100 s. terrain_motion is 1.73x, 1.80x and 1.88x the next rig in three
   builds. Timed apart from the pool it is 4 s of compile, about 90 s in one
   celestial_motion_test.exe run (three passes: CPU shadow via UpdateSubresource,
   CPU shadow via Map, GPU copy) and 0.2 s of Python.
3. Redundant compiles (I-3): 245 of 392 production compile events, about 138
   CPU-seconds. Plan below.
4. The pool starts only after both DLLs link (41 s into a warm build) although
   most rigs need neither DLL nor build\gen (I-1, X3-8). The head before the DLL
   compile is serial by construction (Python self-tests, then one
   single-threaded shader generator); core use was not measured.

Ruled out: splitting terrain_motion by mode now, because it is under 2x the next
rig in all three builds and the split can save at most the gap between the
pool's wall and summed/8, 5-7 s (3-4% of a warm build). Revisit if the ratio
passes 2x or once I-3 makes the pool CPU bound.

Next: run the temporal shader compiles side by side (item 1); I-3 steps 1-2
below; start CPU-light independent rigs at t=0 (item 4).

## How it was measured

A wrapper read build.bat's output line by line and stamped each with the
seconds since the start; phases are the `[edvr]` markers: `using` (start),
`=== d3d11.dll ===` (end of the head), `=== test rigs ===` (end of the DLL
compile), run_jobs' own `the pool of` line, and the last line. The rig split
of terrain_motion is the same cl command and run lines as `:rig_terrain_motion`
timed one after another, on a machine another build had at 100%, so its
absolute figures are inflated; the proportions are what is used.

## The compile census (I-3)

103 cl commands compile 392 production (`src\`) sources over 147 distinct
files: 245 compiles are repeats, `for` loops counted. Each file compiled twice
or more was timed with one single-threaded `cl /c` and generic rig flags, as
CPU seconds (kernel plus user of cl.exe), so another build on the machine does
not skew it. 63 files, mean 0.69 s. Redundant CPU, (times - 1) x cost:

    file                        times   cpu-s each   redundant   flag sets
    common\config.cpp             24       1.28         29.5        10
    common\log.cpp                23       0.55         12.0        10
    common\proxy.cpp              14       0.55          7.1         8
    d3d11\gpu_timing.cpp          13       0.55          6.6         8
    d3d11\gpu_span_d3d11.cpp      24       0.23          5.4        13
    openxr\shared_texture_transfer 8       0.61          4.3         5
    openxr\producer_gpu_timing     9       0.50          4.0         6
    common\guard.cpp              17       0.22          3.5         9
    common\vtable_hook.cpp         7       0.56          3.4         5
    openxr\eye_capture.cpp         7       0.44          2.6         4

Those ten are about 78 s; the five the review names (config, log,
gpu_span_d3d11, guard, proxy) are 58 s; all 63 files are 138 s.

## I-3 plan: one flag set and a common library (not implemented)

1. Audit the flag drift. For the ten files above, list the flag sets each is
   compiled under (the census records them) and which flags change the code
   (/DUNICODE, /utf-8, /Gy, /Z7, /EHsc against /EHs, /GR-). The review found
   these files use no ambiguous A/W APIs, so the common rig flags
   (`/nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX
   /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE`) should do for all of them;
   prove it by building and running every rig that uses them.
2. One `RIGFLAGS` variable in build.bat, beside CFLAGS, that each rig's cl
   line uses. A plan-time check in run_jobs (the shape of the d3d11.lib one)
   lists any rig line that spells its own.
3. `edvr_common.lib`: compile the ten files once with /MP into `%OBJ%\common\`,
   in the head or beside the DLL compile (which has spare cores), and archive
   them. Each rig's cl line drops those `.cpp` names and lists the library.
   Cost: about 5.5 CPU-s once (their single compile times summed), under a
   second of wall.
4. Guard it: run_jobs refuses, at the plan, a rig whose cl line names a source
   that is in the library. Leave out the 14 test files that `#include` a
   `src\` .cpp, and the `for %%T in (native stereo)` rig until its two
   variants are shown to use one flag set.

Expected saving, from the figures above, assuming a rig's seconds fall by the
CPU seconds saved: 78 CPU-s (ten files) is about 10 s of pool wall at 8 slots
when the pool is CPU bound, 7 s for the review's five, 17 s for all 63. While
terrain_motion sets the pool's wall (105 s against 96.5 s) it is 0 s on an idle
machine, and it does show when another build shares the machine (X3-9). With
the terrain_motion split as well, the warm pool would fall from 105 s to about
(771.6 - 78) / 8 = 87 s, -18 s (11% of 166 s); with all 63 files, about 79 s.
