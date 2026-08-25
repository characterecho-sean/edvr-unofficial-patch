// The body composite's inputs, probed one slot at a time.
//
// WHY (docs/fss-scanner.md rounds 1-8, reopened 2026-08-25): the black
// squares survived every producer probe, and one place was never touched.
// The BODY COMPOSITE (vs 953C8123AD8DC13B, the draw that pastes the
// scanner's body into each eye) samples four textures: a 6x1 strip at s0,
// a 135x133 at s1, a 542x535 at s2, and the body layer itself at s3. The
// middle two were assumed glow pyramids and never probed -- and the
// loading hologram taught exactly this lesson once: its scan pattern rode
// the COMPOSITE's sampler slots, not the content underneath.
//
// THE PROBE substitutes one slot of exactly that draw with a flat colour,
// holo-style, restored after each draw. Magenta is the positive control
// the refuted rounds were missing: if "1:magenta" shows no magenta
// anywhere, slot 1 is invisible and a null on it means nothing. Black and
// white are the neutral candidates once a slot has proven visible.
//
// advanced.fss_composite_probe = "SLOT:COLOUR" -- slot 0..3, colour
// magenta|black|white; empty is off and the only shipped state. Live.
// Substitution only engages for slots resolving to the R11G11B10_FLOAT
// family the composite was measured to bind (the strip at s0 is typeless
// and refused with a note). The log counts engagements.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

void fssProbeConfigure(Config& cfg);

// One bool for the draw path's early-out set; also keeps the body-frame
// stamp maintained while a probe is armed.
bool fssProbeWants();

// Called for eye draws while armed: matches the composite by kind/count
// then vertex-shader hash. True means wrap in fssProbeBegin/End.
bool fssProbeOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances);

void fssProbeBegin(ID3D11DeviceContext* ctx);
void fssProbeEnd(ID3D11DeviceContext* ctx);

void fssProbeShutdown();

}  // namespace edvr
