# AMD FidelityFX Super Resolution 1.0, vendored

`ffx_a.h` and `ffx_fsr1.h` are **AMD's own sources, unmodified**, version
`v1.20210629`, from <https://github.com/GPUOpen-Effects/FidelityFX-FSR>.
Copyright (c) 2021 Advanced Micro Devices, Inc., MIT licensed; each file
carries the full notice, which must stay with it.

## Why they are copied in rather than described

The intro movie's resampler needs EASU (`docs/intro-video.md`). EASU is an
edge-adaptive kernel whose taps and constants are not something to reproduce
from memory -- a thing labelled FSR that is not FSR is worse than an honest
bicubic, and this project's rule is to transcribe what the source says rather
than assume it (`docs/particle-billboards.md` is the same rule applied to a
game shader; `docs/loading-scrim.md` is what guessing cost when the answer
had been sitting in a disassembly for three days).

So these are the real thing, and the integration uses AMD's documented
contract exactly: `FsrEasuCon` on the CPU with `A_CPU` defined, the three
gather callbacks and `FsrEasuF` on the GPU with `A_GPU`/`A_HLSL`/
`FSR_EASU_F`.

## How they reach the shader

`tools/gen_fsr_hlsl.py` turns both files into string chunks at build time
(`fsr_hlsl_gen.h`), which `intro_upscale.cpp` concatenates ahead of its own
entry point. HLSL is text: concatenation is what `#include` would have done,
without needing an include handler inside `D3DCompile`.

Nothing here is edited. If a newer FSR is wanted, replace both files from
upstream and rebuild -- there is no local patch to carry forward.
