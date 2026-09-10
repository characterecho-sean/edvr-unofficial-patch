"""Compile the actual drive-suppression functions and indirect hooks against
a minimal context double, then exercise the previously bypassed draw paths.

Run from a VS x64 developer shell: python tools/check_drive_switches.py
No game, headset, GPU or third-party Python packages required. Optional
--revision checks an earlier git revision with the same regression cases.
The source under test is extracted unchanged; the doubles only supply D3D
state/query and forwarding boundaries, not the effect recognition logic.
"""

import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def extract(source, declaration):
    start = source.index(declaration)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision")
    args = parser.parse_args()

    def read(path):
        if args.revision:
            return subprocess.check_output(
                ["git", "show", f"{args.revision}:{path}"], cwd=ROOT, text=True
            )
        return (ROOT / path).read_text(encoding="utf-8")

    particle = read("src/d3d11/particle_fix.cpp")
    vscreen = read("src/d3d11/vscreen.cpp")
    constants = "\n".join(
        re.search(r"constexpr uint64_t " + name + r"\[\d+\] = \{.*?\};", particle, re.S)[0]
        for name in ("kHeatHazeVs", "kDrivesSmokeVs")
    )
    functions = "\n".join(
        extract(source, declaration)
        for source, declaration in (
            (particle, "bool drivesSmokeSkip("),
            (particle, "bool heatHazeSkip("),
            (vscreen, "void STDMETHODCALLTYPE hookedDrawIndexedInstancedIndirect("),
            (vscreen, "void STDMETHODCALLTYPE hookedDrawInstancedIndirect("),
        )
    )
    # Exercise the actual early-return block too: a visible substituted
    # particle must not disappear from the eye ledger or draw census.
    functions += "\nDrawVerdict captureParticleDraw(ID3D11DeviceContext* self, " \
        "char kind, uint32_t count, uint32_t instances, const DrawArgs& args) {\n(void)args;\n" \
        + extract(vscreen, "if (particleSteady() && particleOnDraw(") \
        + "\nreturn DrawVerdict::kNone;\n}\n"
    output = ROOT / "build" / "drive_switch_test"
    output.mkdir(parents=True, exist_ok=True)
    cpp = output / "test.cpp"
    cpp.write_text(DOUBLES + constants + functions + CASES, encoding="utf-8")
    exe = output / "test.exe"
    subprocess.run(
        ["cl.exe", "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/O2",
         str(cpp), f"/Fe{exe}", f"/Fo{output / 'test.obj'}"], check=True, cwd=output
    )
    return subprocess.run([str(exe)], cwd=output).returncode


DOUBLES = r'''
#include <cstdint>
#include <cstdio>
#include <initializer_list>
using UINT = unsigned;
#define STDMETHODCALLTYPE
struct ID3D11VertexShader { uint64_t hash = 0; void Release() {} } shader;
struct ID3D11Buffer {};
struct ID3D11DeviceContext {
    uint64_t hash = 0;
    bool foreign = false;
    void VSGetShader(ID3D11VertexShader** out, void*, void*) { *out = &shader; }
} context;
enum class BindSlot { Vs, Dsv0, Rtv0 };
uint64_t boundVsHashFast(ID3D11DeviceContext* ctx) { return ctx->hash; }
uint64_t lookupShaderHash(ID3D11VertexShader* vs) { return vs->hash; }
void* bindingGet(BindSlot slot) { return slot == BindSlot::Vs ? &shader : nullptr; }
uint64_t bindingShaderHash(BindSlot) { return shader.hash; }
uint64_t nowMs() { return 60000; }
struct Log {
    static Log& get() { static Log log; return log; }
    template <class... Args> void note(const char*, Args...) {}
};
bool g_hideDrivesSmoke = false, g_hideHeatHaze = false;
uint64_t g_smokeSkipped = 0, g_smokeNoteMs = 0;
uint64_t g_hazeAsked = 0, g_hazeSkipped = 0, g_hazeNoteMs = 0;
uint64_t g_hazeShadowNull = 0, g_hazeShadowPtr = 0, g_hazeShadowHash = 0;
uint64_t g_hazeLastShadow = 0, g_hazeLastGet = 0;
bool censusArmed = false;
int forwarded = 0, depthNotes = 0, censusNotes = 0;
bool drawCensusArmed() { return censusArmed; }
bool foreignContext(ID3D11DeviceContext* ctx) { return ctx->foreign; }
void drawCensusDrawDirect(ID3D11DeviceContext*, char, uint32_t, uint32_t,
                         bool, ID3D11Buffer*, UINT) { ++censusNotes; }
void depthProbeNoteIndirectDraw(ID3D11DeviceContext*, void*) { ++depthNotes; }
void forward(ID3D11DeviceContext*, ID3D11Buffer*, UINT) { ++forwarded; }
struct State {
    decltype(&forward) realDrawIndexedInstancedIndirect = &forward;
    decltype(&forward) realDrawInstancedIndirect = &forward;
} state;
State* g_state = &state;
enum class DrawVerdict { kNone, kParticle };
struct DrawArgs { uint32_t startInstance = 17; };
bool particlesOn = true, ledgerOn = false, targetEye = true;
int earlyCensus = 0, earlyLedger = 0;
uint32_t capturedStartInstance = 0;
bool particleSteady() { return particlesOn; }
bool particleOnDraw(ID3D11DeviceContext*, char, uint32_t, uint32_t) { return true; }
bool objectProbeLedgerActive() { return ledgerOn; }
bool targetIsEyeSized(void*) { return targetEye; }
void drawCensusNoteUnseen(char) {}
void drawCensusEarlyDraw(ID3D11DeviceContext*, char, uint32_t, uint32_t, bool, const DrawArgs&) {
    ++earlyCensus;
}
void objectProbeNoteEarlyDraw(ID3D11DeviceContext*, char, uint32_t, uint32_t, uint32_t startInstance) {
    if (ledgerOn) { ++earlyLedger; capturedStartInstance = startInstance; }
}
'''

CASES = r'''
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        if (failures <= 12) std::printf("FAIL %s (VS=%016llX)\n", what,
                                      static_cast<unsigned long long>(context.hash));
    }
}
void select(uint64_t hash) { context.hash = shader.hash = hash; }
int main() {
    // Real census shapes plus low/high LOD, instanced, direct, and GPU-counted
    // forms. Count and API changes must not revive a known disabled effect.
    for (uint64_t hash : {kDrivesSmokeVs[0], uint64_t{0x203DF51758AADC4Dull},
                         kHeatHazeVs[0], kHeatHazeVs[1], kHeatHazeVs[2],
                         uint64_t{0}, uint64_t{0xEB787F983BC1F5A3ull},
                         uint64_t{0x6041FD2D3D0164E1ull}}) {
        select(hash);
        // The planetary/FSS shader formerly mislabeled as a smoke volume
        // must forward even with the smoke switch off.
        const bool smoke = hash == kDrivesSmokeVs[0];
        const bool haze = hash == kHeatHazeVs[0] || hash == kHeatHazeVs[1] || hash == kHeatHazeVs[2];
        for (bool enabled : {false, true}) {
            g_hideDrivesSmoke = g_hideHeatHaze = enabled;
            for (char kind : {'D', 'I', 'N', 'X', 'Y', 'Z'}) {
                for (uint32_t count : {0u, 6u, 36u, 600u, 5334u, 32768u}) {
                    for (uint32_t instances : {0u, 1u, 10u}) {
                        check(drivesSmokeSkip(&context, kind, count, instances) == (smoke && enabled),
                              "smoke switch respects shader across draw shapes");
                        check(heatHazeSkip(&context, kind, count, instances) == (haze && enabled),
                              "haze switch respects shader across draw shapes");
                    }
                }
            }
            for (bool foreign : {false, true}) {
                context.foreign = foreign;
                for (bool census : {false, true}) {
                    censusArmed = census;
                    for (auto hook : {&hookedDrawIndexedInstancedIndirect, &hookedDrawInstancedIndirect}) {
                        forwarded = depthNotes = censusNotes = 0;
                        hook(&context, nullptr, 0);
                        const bool skip = !foreign && enabled && (smoke || haze);
                        check(forwarded == (skip ? 0 : 1), "indirect hook actually suppresses/forwards");
                        check(depthNotes == ((!skip && !foreign) ? 1 : 0), "depth probe counts forwarded owner draws");
                        check(censusNotes == (census ? 1 : 0), "indirect census retains submitted draw");
                    }
                }
            }
        }
    }
    // Switching one effect off must leave the other switch independent.
    context.foreign = false;
    g_hideDrivesSmoke = true;
    g_hideHeatHaze = false;
    select(kHeatHazeVs[0]);
    forwarded = 0;
    hookedDrawInstancedIndirect(&context, nullptr, 0);
    check(forwarded == 1, "smoke off leaves haze on");
    g_hideDrivesSmoke = false;
    g_hideHeatHaze = true;
    select(kDrivesSmokeVs[0]);
    forwarded = 0;
    hookedDrawIndexedInstancedIndirect(&context, nullptr, 0);
    check(forwarded == 1, "haze off leaves smoke on");
    check(!drivesSmokeSkip(nullptr, 'X', 600, 1), "null smoke context");
    check(!heatHazeSkip(nullptr, 'X', 36, 1), "null haze context");
    check(g_hazeAsked > 64 && g_hazeShadowNull == 0 && g_hazeShadowPtr == 0 && g_hazeShadowHash == 0,
          "periodic context audit exercised without false mismatch");
    for (bool particles : {false, true}) {
        particlesOn = particles;
        for (bool census : {false, true}) {
            censusArmed = census;
            for (bool ledger : {false, true}) {
                ledgerOn = ledger;
                for (bool eye : {false, true}) {
                    targetEye = eye;
                    earlyCensus = earlyLedger = 0;
                    capturedStartInstance = 0;
                    auto result = captureParticleDraw(&context, 'X', 600, 10, DrawArgs{});
                    check((result == DrawVerdict::kParticle) == particles, "capture preserves particle verdict");
                    check(earlyCensus == (particles && census ? 1 : 0), "visible particle reaches census once");
                    check(earlyLedger == (particles && ledger && eye ? 1 : 0), "eye particle reaches ledger once");
                    if (earlyLedger) check(capturedStartInstance == 17, "ledger preserves draw arguments");
                }
            }
        }
    }
    std::printf("drive switches: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
'''


if __name__ == "__main__":
    raise SystemExit(main())
