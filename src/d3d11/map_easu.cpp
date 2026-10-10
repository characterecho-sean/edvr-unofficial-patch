#include "map_easu.h"

#include <cmath>   // the C math functions AMD's CPU helpers call (fabs, floor, exp2, ...), which ffx_a.h does not include itself

// AMD's own FSR1, CPU side: FsrEasuCon computes the constants the EASU pass reads. The same arrangement as intro_upscale.cpp and
// sharpen_pass.cpp: the vendored files stay byte-identical, and their unused helpers' C4505s are silenced here.
#define A_CPU 1
#pragma warning(push)
#pragma warning(disable : 4505)
#include "fsr/ffx_a.h"
#include "fsr/ffx_fsr1.h"
#pragma warning(pop)

namespace edvr {

void mapEasuConstants(uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH, uint32_t out[20]) {
    AU1 con0[4] = {}, con1[4] = {}, con2[4] = {}, con3[4] = {};
    FsrEasuCon(con0, con1, con2, con3,
               static_cast<AF1>(renderW), static_cast<AF1>(renderH),
               static_cast<AF1>(renderW), static_cast<AF1>(renderH),
               static_cast<AF1>(outW), static_cast<AF1>(outH));
    for (int i = 0; i < 4; ++i) {
        out[0 + i] = con0[i];
        out[4 + i] = con1[i];
        out[8 + i] = con2[i];
        out[12 + i] = con3[i];
    }
    out[16] = outW;
    out[17] = outH;
    out[18] = renderW;
    out[19] = renderH;
}

}  // namespace edvr
