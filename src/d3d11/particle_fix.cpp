#include "temporal_shader_bytecode.h"
#include "particle_fix.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/runtime_profile.h"
#include "../common/guard.h"
#include "../common/log.h"
#include <string>

#include "../common/timing.h"
#include "binding_shadow.h"   // bindingShaderHash: the bound vertex shader's hash, set with the shader
#include "exposure_fix.h"   // lookupShaderHash
#include "particle_capture_core.h"
#include "billboard_rollfree.h"   // the roll-free copy of the scene block, and the table of watched blocks
#include "gpu_interval.h"         // sampled GPU time of that copy
#include "supercruise_lines.h"   // kUiVsSpaceDust, kUiPsSpaceDust: the dust's shader pair
// flare_vs.h is compiled by the build shader generator.
// particle_vs.h is compiled by the build shader generator.
#include "shader_swap.h"

namespace edvr {

// particleSteady reads this from the header with no call: asked per
// draw, and the build has no /GL to fold a cross-TU getter. ParticleMode
// itself is declared in the header, so the inline function there can
// name its kSteady value.
namespace detail {
ParticleMode g_particleMode = ParticleMode::kStock;
// Published for the draw path's inline first tests (particle_fix.h); the
// file-local names below are references to these.
bool g_particleHideStars = false;
// fix.supercruise_dust = off and fix.normal_space_dust = off, published for
// spaceDustHidden() (particle_fix.h).
bool g_supercruiseDustHidden = false;
bool g_normalSpaceDustHidden = false;
// The roll-free rebind (billboard_rollfree.h): this draw's pending flag, whether a rebind is on the context, and the
// identities of the watched scene blocks for the Map tee's inline compare.
bool g_rollFreePending = false;
bool g_rollFreeEngaged = false;
const void* g_watchedBlocks[4] = {};
}  // namespace detail
using detail::g_supercruiseDustHidden;   // the configure and skip code below reads them by their plain names
using detail::g_normalSpaceDustHidden;

namespace {
using Mode = detail::ParticleMode;

// The particle billboard vertex shader, by content hash. Found 2026-08-23
// by skipping shaders one at a time at a geyser field (census_skip's
// vs:HASH term) after both offline methods failed: the differential census
// had nothing to subtract, because a geyser field is never quiet in every
// frame, and every size-level signature collided with terrain and props.
constexpr uint64_t kPlumeVs = 0xEB787F983BC1F5A3ull;

// The SOLAR FLARE billboard, found the same way 2026-08-29: prominences
// erupting off star surfaces, riding the head. Same construction, same
// constants, different shader.
//
// WHY A TABLE AND NOT MORE HASHES. The family has at least seven members
// and they do NOT share a signature: the plume takes thirteen inputs and
// writes twelve outputs, the flare takes ten and writes five. A shader
// whose signature does not match the input layout and the pixel shader
// behind it cannot be substituted at all, so each signature group needs
// its own transcription and its own entry here.
constexpr uint64_t kFlareVs = 0x6041FD2D3D0164E1ull;

// ONE HASH PER TRANSCRIPTION. NEVER A SECOND ONE ON A LIKENESS.
//
// 0.12.3 shipped 9AEC596A2B036EA6 alongside kFlareVs as a "twin", on the
// claim that its instruction body was byte-identical and it would get the
// flare's replacement free. It is not a twin, and the claim came from a
// comparison that could not have detected the difference: it read only
// indented lines, so it skipped every dcl_output, and semantic INDICES do
// not appear in the instruction stream at all -- they live in the
// signature block, which no instruction diff reads.
//
// The two shaders write the same values to DIFFERENT output registers
// (max o2.y against max o1.y, utof o1.w against utof o0.w), declare
// different masks, and carry different TEXCOORD indices: the flare emits
// TEXCOORD 2/4/7/9 and 9AEC596A2B036EA6 emits 3/6/8/9. Substituting the
// flare's replacement handed its pixel shader the wrong registers.
//
// What it draws is the WITCHSPACE STARFIELD, and in 0.12.3 that starfield
// disappeared. Field-confirmed 2026-08-30 by census_skip on this hash.
//
// So: a hash earns a replacement only by having its own disassembly read
// and its own transcription written. Likeness is not evidence, and the
// cheap comparison that suggests it is the exact trap this paragraph is
// here to stop.

// The witchspace starfield, by the hash the twin mistake identified. It is
// NOT replaced -- it has no transcription -- but it is offered as something
// a player can turn off, because a user asked for the empty tunnel 0.12.3
// produced by accident and it is a reasonable thing to want.
//
// Skipping the draw is not how 0.12.3 removed it. That fed the draw a
// mismatched replacement and let the pixel shader read whatever the wrong
// registers held, which happened to come out empty. Not drawing it at all
// is the version that means what it says.
//
// Scope, measured: this hash appears ZERO times across three censuses at a
// star where the flare's own hash appears 24 times, so it is not the
// general background starfield. That is evidence it is specific to the
// jump tunnel, not proof it draws nowhere else.
constexpr uint64_t kWitchspaceStarsVs = 0x9AEC596A2B036EA6ull;
// Bound to the published flag witchspaceStarsHidden() reads (particle_fix.h).
bool& g_hideWitchspaceStars = detail::g_particleHideStars;
struct BillboardVariant {
    uint64_t    hash;
    const void* bytecode;
    size_t      bytecodeLen;
    const char* name;      // names the compile in the log
};

constexpr int kVariantCount = 2;
const BillboardVariant kVariants[kVariantCount] = {
    {kPlumeVs, kParticleWorldBytecode, sizeof(kParticleWorldBytecode),
     "particle_vs"},
    {kFlareVs, kFlareWorldBytecode, sizeof(kFlareWorldBytecode),
     "flare_vs"},
};
// The draw path's inline prefilter (particleOnDrawMayMatch, particle_fix.h)
// compares against its own copy of these hashes; a variant added here and
// not there would be silently never offered, so the two lists must agree.
static_assert(kVariantCount == 2 && detail::kParticleVariantVs[0] == kPlumeVs &&
                  detail::kParticleVariantVs[1] == kFlareVs,
              "particle_fix.h's kParticleVariantVs must list kVariants' hashes in order");

const char* variantLabel(int v) {
    return (v == 1) ? "solar flare" : "smoke plume";
}

// cb1 is 280 registers. The basis vectors live at 278 and 279 -- floats
// 1112..1119 -- and the neighbours are logged with them because "which
// register is the up" is exactly what this exists to measure rather than
// assume.
constexpr uint32_t kFirstReg = 276;
constexpr uint32_t kRegs = 4;

// The fix. steady replaces the basis's up vector with WORLD UP for the
// matched draw, turning a fully camera-locked billboard into a
// cylindrical one: still facing you, but with the smoke column held
// vertical in the world. Measured 2026-08-23: with the view level the
// game's own up reads (0.056, 0.981, 0.185) -- already world up to within
// the view's pitch -- so at a level view this substitution is a no-op,
// and everything it changes is roll and pitch coupling.
//
// Why a constant substitution is safe here when it was not for the sun
// glare: that buffer's rows were a VIEW MATRIX, and the elements' screen
// positions flowed through them, so every rewrite displaced them per eye.
// These are pure direction vectors used for nothing but the billboard
// basis -- position arrives separately through cb0[9..11] -- and cb1[277]
// holds a camera right the shader never even reads. [278] is the whole of
// the change.
// Which billboard variant is this draw, or -1 for none? By shader hash and
// nothing else: the geyser hunt established that kind, count, stride and
// every sampler size are shared with the terrain and prop pipelines.
// The bound vertex shader's hash: the binding shadow's, set with the
// shader (2026-09-09; a VSGetShader per draw was a millisecond a frame
// here), and the Get only when the shadow has seen no set.
uint64_t boundVsHashFast(ID3D11DeviceContext* ctx) {
    if (bindingGet(BindSlot::Vs)) {
        // A held zero is a shader the registry had not met at its set
        // (2026-09-09), and the context is asked instead.
        const uint64_t held = bindingShaderHash(BindSlot::Vs);
        if (held) return held;
    }
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return 0;
    const uint64_t h = lookupShaderHash(vs);
    vs->Release();
    return h;
}

// THE ROLL-FREE FAMILIES (docs/billboard-root-cause-2026-10-16.md). Camera-facing billboard vertex shaders that
// have no transcription: their quads are spanned on cb1[277]/[278] (right/up) and roll with the head. They are
// matched by hash, like the two transcriptions, and given a copy of the scene block with those two rows
// roll-free at VERTEX slot 1 for the draw (billboard_rollfree.h). Each hash below was disassembled and its only
// uses of cb1[277]/[278] are the quad span: the cross(cb1[278], cb1[279]) form (right = normalize of the cross),
// the direct form (position += cb1[277] * x + cb1[278] * y), or the fallback up of a shader that takes a supplied
// axis when it has one (1FBA99C4911D7ACF, DB113566B6A59C5B: movc axis, cb3[1], cb1[278]). None uses either row
// for a lighting or view-space vector. cb1[279] and everything else are left as the game wrote them.
constexpr int kRollFreeCount = 16;
constexpr uint64_t kRollFreeVs[kRollFreeCount] = {
    0x9AEC596A2B036EA6ull,   // the witchspace starfield (skipped while fix.witchspace_stars = off)
    0xAFBCD3ADB9092F78ull, 0xBBAD1CA808E1E292ull, 0x1B285CBC9F185D4Dull, 0x4BDE4DBF88C2BA27ull,
    0x78F5F08D02EE38CCull,   // explosion suspect
    0x9F4BBCFCD3B68BC9ull,   // explosion suspect (no alignment cascade at all)
    0x1FBA99C4911D7ACFull, 0x2BF557356A28AA0Dull, 0xDB113566B6A59C5Bull,
    0x68DDDEF04D9894AFull,   // the stretched sky billboard (direct span)
    0xF7A6E916F14A3B1Aull,   // direct span
    0x12382D2EA45E9632ull, 0x94EA991C3A03DBD9ull, 0xC31238331D8AC3D4ull, 0xF1502353BE051A2Dull};   // pool-record sprites (direct span)
constexpr bool rollFreeTableAgrees() {
    for (int i = 0; i < kRollFreeCount; ++i)
        if (kRollFreeVs[i] != detail::kBillboardVs[2 + i]) return false;
    return true;
}
static_assert(rollFreeTableAgrees(), "particle_fix.h's kBillboardVs must list the two transcriptions and then kRollFreeVs in order");

int rollFreeIndexOf(uint64_t h) {
    for (int i = 0; i < kRollFreeCount; ++i)
        if (h == kRollFreeVs[i]) return i;
    return -1;
}

// The shadow of the game's cb1, kept by the Map/Unmap tee, and our own
// buffer built from it. The contents cannot be read at the draw -- a
// dynamic buffer the GPU owns is write-only from here -- so the only way
// to substitute is to watch the game write it, which is the mechanism the
// panel distance fix has used since 0.3.
//
// The shadow keeps only rows 270..279 of the block (particle_capture_core.h says why and what reads
// them), laid out at the block's own offsets; `size` is the mapped size the readers gate on.
//
// Since 2026-10-16 there can be more than one scene block (each eye's, any view): billboard_rollfree.h's table
// holds up to four, each with its own shadow, write counter and roll-free copy. g_cur is the shadow of the block
// the draw being matched has bound at VS slot 1, which solveCamera and shapeOk read.
rollfree::Blocks     g_blocks;
particlecap::Shadow* g_cur = nullptr;
particlecap::Tracker g_track;
uint32_t g_retireNotes = 0;

// The roll-free rebind's state for the draw in flight, and its census for the 10 s line.
int      g_rfSlot = -1;
int      g_rfIndex = -1;
uint64_t g_rfSeen[kRollFreeCount] = {};
uint64_t g_rfBound[kRollFreeCount] = {};
bool     g_rfNoted[kRollFreeCount] = {};
uint64_t g_rfFrames = 0;
uint64_t g_rfNoteMs = 0;
rollfree::Counters g_rfAtNote;
GpuIntervals<8> g_copyGpu;               // the lazy copy's GPU time, one copy in 16, polled once a frame
ID3D11DeviceContext* g_copyGpuCtx = nullptr;
uint64_t g_copySeq = 0;
uint64_t g_cpuSampled = 0, g_cpuTicks = 0, g_rfBindCalls = 0;   // Begin's own CPU time, one in 8
unsigned g_gpuSamplesAtNote = 0;
double   g_gpuMsAtNote = 0.0;
FaultBudget g_rfBudget("particle.rollFree", 5);

void publishWatched() {
    const void* const* w = g_blocks.watched();
    for (int i = 0; i < 4; ++i) detail::g_watchedBlocks[i] = w[i];
}
uint64_t g_applied = 0;
uint64_t g_appliedAtNote = 0;
uint64_t g_noteMs = 0;
bool     g_learnNoted = false;

// Why the quads do not face their own emitter. Elite renders in
// CAMERA-RELATIVE world space -- the shader's own near-fade takes
// dot(forward, position) with no camera term, which is only a depth if
// positions are already relative to the eye -- so cb0[9..11]'s
// translation column is the vector from the viewer to the plume, and
// normalising it is the direction the quads should face. Measured at a
// geyser field: two emitters at (312.1, 37.2, -294.3) and (43.1, -47.0,
// 19.5), each steady while the view moved around them.
// The per-emitter facing is OFF, and it is a dead end rather than a
// pending one. MEASURED 2026-08-23: cb0 here is 208 bytes bound at
// register 0 -- no ring, no offset -- and 208 bytes is the
// engine-standard CAMERA block this project decoded during the sun-glare
// arc. So cb0[9..11] is not an emitter's model matrix; it is that
// block's world-frame rows, whose w components the glare arc already
// convicted as an accumulator rather than a position (its "distance"
// grew linearly forever). Aiming down that vector pointed every quad
// somewhere meaningless: the field saw the smoke go edge-on in stacks.
//
// There is no per-emitter position in these constants, so there is no
// per-draw facing to be had from them. Exact facing is per-PARTICLE and
// lives in the vertex stream, which only a replacement shader can read
// -- the same ceiling the glare's constant substitution hit before it
// became a shader swap. The key that repeated the measurement
// (advanced.particle_face_emitter) and the emitter-buffer shadow it needed
// retired 2026-09-23.
uint64_t g_facingUsed = 0;
float    g_lastFacing[3] = {};

// THE SWAP. The constant substitution that came before this could remove
// the roll -- world up in place of the camera's -- but never the
// foreshortening, because facing the viewer is a per-PARTICLE direction
// and a constant is per-draw. So the draw gets a different vertex shader
// instead: a transcription of the game's own, with the basis rebuilt per
// vertex. See particle_vs.h.
//
// The buffer copy the substitution needed every draw -- 5376 bytes,
// mapped and filled -- is gone with it. What remains per draw is a shader
// swap and a 32-byte constant write.
// One compiled replacement per signature group, compiled on first sight of
// a draw that needs it. A group nobody draws costs nothing.
ID3D11VertexShader* g_ourVs[kVariantCount] = {};
bool                g_vsTried[kVariantCount] = {};
uint64_t            g_appliedBy[kVariantCount] = {};
// Which variant the draw currently being matched belongs to. Set by
// particleOnDraw, read by particleBegin, -1 between them.
int                 g_activeVariant = -1;
ID3D11VertexShader* g_savedVs = nullptr;
ID3D11Buffer*       g_ourCb = nullptr;     // b3: viewer position, world up
ID3D11Buffer*       g_savedCb3 = nullptr;
bool                g_engaged = false;

// The viewer's position in the space the particles are transformed into,
// solved from the game's own clip rows. For any projective transform the
// camera annihilates the x, y and w rows -- dot(row, cam) = -row.w -- so
// three rows and a 3x3 solve give it, exactly the identity the sun-glare
// arc used to find the camera behind the glare train.
float g_cam[3] = {};
bool  g_camOk = false;

void solveCamera() {
    g_camOk = false;
    if (!g_cur || !g_cur->valid || g_cur->size < 274 * 16) return;
    const float* f = reinterpret_cast<const float*>(g_cur->bytes);
    const float* c0 = f + 270 * 4;   // the x contribution
    const float* c1 = f + 271 * 4;   // y
    const float* c2 = f + 272 * 4;   // z
    const float* t  = f + 273 * 4;   // translation
    // Rows of the 3x3, one per clip component that the camera kills.
    const float a[3][3] = {{c0[0], c1[0], c2[0]},
                           {c0[1], c1[1], c2[1]},
                           {c0[3], c1[3], c2[3]}};
    const float b[3] = {-t[0], -t[1], -t[3]};
    const float det =
        a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
        a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
        a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabsf(det) < 1e-12f) return;
    const float inv = 1.0f / det;
    g_cam[0] = inv * (b[0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                      a[0][1] * (b[1] * a[2][2] - a[1][2] * b[2]) +
                      a[0][2] * (b[1] * a[2][1] - a[1][1] * b[2]));
    g_cam[1] = inv * (a[0][0] * (b[1] * a[2][2] - a[1][2] * b[2]) -
                      b[0] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                      a[0][2] * (a[1][0] * b[2] - b[1] * a[2][0]));
    g_cam[2] = inv * (a[0][0] * (a[1][1] * b[2] - b[1] * a[2][1]) -
                      a[0][1] * (a[1][0] * b[2] - b[1] * a[2][0]) +
                      b[0] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]));
    g_camOk = true;
}

FaultBudget g_subBudget("particle.substitute", 5);

// Is the shadow shaped like the camera block this fix understands? Both
// basis vectors must be unit length; anything else is a buffer that is
// not what we think it is, and substituting into it would be writing a
// guess into the game's pipeline.
bool shapeOk(const float* f, uint32_t floats) {
    if (floats < 274 * 4) return false;
    if (floats < (kFirstReg + kRegs) * 4) return false;
    const float* up = f + 278 * 4;
    const float* fwd = f + 279 * 4;
    const float lu = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    const float lf = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (lu < 0.9f || lu > 1.1f) return false;
    if (lf < 0.9f || lf > 1.1f) return false;
    return true;
}

}  // namespace

uint64_t g_starsSkipped = 0;
uint64_t g_starsNoteMs = 0;
uint64_t g_dustSkipped = 0;        // spaceDustSkip's counts, one per pair: supercruise, normal space
uint64_t g_dustNormalSkipped = 0;
uint64_t g_dustNoteMs = 0;         // and the note's clock

bool witchspaceStarsSkip(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                         uint32_t instances) {
    if (!g_hideWitchspaceStars || !ctx) return false;
    if (kind != 'X' && kind != 'N') return false;
    if (instances == 0 || count < 6) return false;
    const uint64_t h = boundVsHashFast(ctx);
    if (h != kWitchspaceStarsVs) return false;
    ++g_starsSkipped;
    const uint64_t now = nowMs();
    if (now - g_starsNoteMs >= 30000) {
        g_starsNoteMs = now;
        Log::get().note(
            "witchspace stars: OFF -- %llu draw(s) of vs %016llX withheld so "
            "far. The tunnel is empty by choice; set witchspace_stars = on "
            "to have them back.",
            static_cast<unsigned long long>(g_starsSkipped),
            static_cast<unsigned long long>(kWitchspaceStarsVs));
    }
    return true;
}

namespace {
// The space dust's bound pixel shader hash: the binding shadow's when it has
// one, the context's otherwise (boundVsHashFast's pattern).
uint64_t boundPsHashFast(ID3D11DeviceContext* ctx) {
    if (bindingGet(BindSlot::Ps)) {
        const uint64_t held = bindingShaderHash(BindSlot::Ps);
        if (held) return held;
    }
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    if (!ps) return 0;
    const uint64_t h = lookupShaderHash(ps);
    ps->Release();
    return h;
}
}  // namespace

bool spaceDustSkip(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                   uint32_t instances) {
    (void)kind; (void)count; (void)instances;
    if (!ctx) return false;
    // The vertex shader first; the pixel shader is asked only for a vertex
    // shader that is one of the two dust pairs' (the pair is the key). Each
    // pair is withheld only by its own flag.
    const uint64_t vs = boundVsHashFast(ctx);
    if (vs == kUiVsSpaceDust) {
        if (!g_supercruiseDustHidden) return false;
        if (boundPsHashFast(ctx) != kUiPsSpaceDust) return false;
        ++g_dustSkipped;
    } else if (vs == kVsNormalSpaceDust) {
        if (!g_normalSpaceDustHidden) return false;
        if (boundPsHashFast(ctx) != kPsNormalSpaceDust) return false;
        ++g_dustNormalSkipped;
    } else {
        return false;
    }
    const uint64_t now = nowMs();
    if (now - g_dustNoteMs >= 30000) {
        g_dustNoteMs = now;
        Log::get().note(
            "space dust withheld so far: supercruise %llu, normal space %llu "
            "draw(s). Set fix.supercruise_dust or fix.normal_space_dust = on "
            "to have them back.",
            static_cast<unsigned long long>(g_dustSkipped),
            static_cast<unsigned long long>(g_dustNormalSkipped));
    }
    return true;
}

uint32_t particleCapture(const void* res, const void* data, uint32_t bytes) {
    return g_blocks.capture(res, detail::g_particleMode == Mode::kSteady, data, bytes,
                            [](void* dst, const void* src, size_t n) { memcpy(dst, src, n); });
}

namespace {
bool anyBlockWatched() {
    for (int i = 0; i < 4; ++i)
        if (detail::g_watchedBlocks[i]) return true;
    return false;
}

// Every 10 s while steady is on: which billboard hashes drew, how many were given a roll-free block, the lazy
// copy's rate and cost. "not exercised" when none of the sixteen drew in the window.
void rollFreeReport(uint64_t now) {
    if (g_rfNoteMs == 0) { g_rfNoteMs = now ? now : 1; return; }
    if (now - g_rfNoteMs < 10000) return;
    g_rfNoteMs = now;
    char line[1024];
    size_t n = 0;
    auto put = [&](const char* fmt, auto... a) {
        if (n >= sizeof line) return;
        const int k = std::snprintf(line + n, sizeof line - n, fmt, a...);
        if (k > 0) n += static_cast<size_t>(k) < sizeof line - n ? static_cast<size_t>(k) : sizeof line - n - 1;
    };
    const rollfree::Counters& c = g_blocks.counters;
    const uint64_t copies = c.copies - g_rfAtNote.copies, binds = c.binds - g_rfAtNote.binds;
    const uint64_t noBasis = c.noBasis - g_rfAtNote.noBasis, fails = c.failures - g_rfAtNote.failures;
    bool any = false;
    for (int i = 0; i < kRollFreeCount; ++i) any |= g_rfSeen[i] != 0;
    if (!any) {
        put("billboard roll-free (%llu frame(s)): not exercised, none of the %d vertex shaders drew; %s",
            static_cast<unsigned long long>(g_rfFrames), kRollFreeCount,
            anyBlockWatched() ? "a scene block is watched" : "no scene block is watched");
    } else {
        put("billboard roll-free (%llu frame(s)), rebound/seen by vertex shader:", static_cast<unsigned long long>(g_rfFrames));
        for (int i = 0; i < kRollFreeCount; ++i) {
            if (!g_rfSeen[i]) continue;
            put(" %016llX %llu/%llu;", static_cast<unsigned long long>(kRollFreeVs[i]),
                static_cast<unsigned long long>(g_rfBound[i]), static_cast<unsigned long long>(g_rfSeen[i]));
        }
        put(" block copies %.2f/frame for %llu rebound draw(s)", g_rfFrames ? static_cast<double>(copies) / static_cast<double>(g_rfFrames) : 0.0,
            static_cast<unsigned long long>(binds));
        if (noBasis) put(", %llu kept the game's block (looking within a degree of straight up)", static_cast<unsigned long long>(noBasis));
        if (fails) put(", %llu FAILED (copy or buffer creation)", static_cast<unsigned long long>(fails));
        if (g_cpuSampled) {
            put("; cpu %.2f us/draw (%llu timed)",
                1e6 * static_cast<double>(g_cpuTicks) / static_cast<double>(qpcFrequency()) / static_cast<double>(g_cpuSampled),
                static_cast<unsigned long long>(g_cpuSampled));
        } else {
            put("; cpu not timed");
        }
        const unsigned gs = g_copyGpu.totals.samples - g_gpuSamplesAtNote;
        if (gs) put("; gpu copy %.4f ms (%u timed)", (g_copyGpu.totals.ms - g_gpuMsAtNote) / gs, gs);
        else put("; gpu copy not timed (one copy in 16 is sampled)");
    }
    Log::get().note("%s", line);
    for (int i = 0; i < kRollFreeCount; ++i) { g_rfSeen[i] = 0; g_rfBound[i] = 0; }
    g_rfFrames = 0;
    g_rfAtNote = c;
    g_cpuSampled = 0;
    g_cpuTicks = 0;
    g_gpuSamplesAtNote = g_copyGpu.totals.samples;
    g_gpuMsAtNote = g_copyGpu.totals.ms;
}
}  // namespace

// Once a frame, from vScreenFrameBoundary (the owner thread). When no plume, flare or other billboard of the
// family has drawn for kRetireFrames frames, every watched block is let go: the Map tee stops remembering the
// buffers' pointers and the Unmap tee stops reading them. The next matched draw learns its block again
// (particleOnDraw), and the first write after that is what the substitution is built from, exactly as at the
// first learn of a session.
void particleFrameBoundary() {
    if (detail::g_particleMode != Mode::kSteady) return;
    ++g_rfFrames;
    if (g_copyGpuCtx) g_copyGpu.poll(g_copyGpuCtx);
    rollFreeReport(nowMs());
    if (!g_track.frameBoundary(anyBlockWatched())) return;
    g_blocks.clear();
    publishWatched();
    g_cur = nullptr;
    detail::g_rollFreePending = false;
    if (g_retireNotes < 4) {
        ++g_retireNotes;
        Log::get().note(
            "particle billboard: no plume, flare or other billboard drawn for %u frames; the scene constants at slot 1 are "
            "no longer watched until one is drawn again (retired %llu time(s) this session).",
            particlecap::kRetireFrames, static_cast<unsigned long long>(g_track.retired));
    }
}

void particleDescribeTarget(char* out, size_t cap) {
    if (!out || cap == 0) return;
    const char* state = detail::g_particleMode != Mode::kSteady ? "off (stock)"
                        : anyBlockWatched() ? "armed" : (g_track.retired ? "retired" : "not learned");
    std::snprintf(out, cap, "particle target %s, retired %llu, re-armed %llu", state,
                  static_cast<unsigned long long>(g_track.retired), static_cast<unsigned long long>(g_track.rearmed));
}

bool particleOnDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                    uint32_t instances) {
    if (detail::g_particleMode != Mode::kSteady || !ctx) return false;
    const uint64_t h = boundVsHashFast(ctx);
    int variant = -1;
    int rf = -1;
    if (h == kVariants[0].hash) variant = 0;
    else if (h == kVariants[1].hash) variant = 1;
    else rf = rollFreeIndexOf(h);
    if (variant >= 0) {
        // The two transcriptions keep their rule: instanced geometry of at least a quad.
        if (kind != 'X' && kind != 'N') return false;
        if (instances == 0 || count < 6) return false;
        g_activeVariant = variant;
    } else if (rf >= 0) {
        if (count < 3) return false;
        // fix.witchspace_stars = off withholds this draw further down the ladder; do not rebind a draw that is not made.
        if (h == kWitchspaceStarsVs && g_hideWitchspaceStars) return false;
        ++g_rfSeen[rf];
    } else {
        return false;
    }
    g_track.noteMatch();

    // Follow the buffer the game binds for these draws. Learning it here
    // rather than assuming means a build that moves the block simply never
    // matches, instead of substituting into the wrong buffer.
    ID3D11Buffer* cb = nullptr;
    ctx->VSGetConstantBuffers(1, 1, &cb);
    if (!cb) return false;
    int slot = g_blocks.find(cb);
    if (slot < 0) {
        slot = g_blocks.learn(cb);
        publishWatched();
        if (slot >= 0) {
            if (g_track.noteLearned() && g_retireNotes < 4) {
                ++g_retireNotes;
                Log::get().note(
                    "particle billboard: a billboard is drawn again; watching the constants at slot 1 once "
                    "more (re-armed %llu time(s) after a retirement).",
                    static_cast<unsigned long long>(g_track.rearmed));
            }
            if (!g_learnNoted) {
                g_learnNoted = true;
                Log::get().note(
                    "particle billboard: watching the scene constants at slot 1. The next write the game makes "
                    "there is what the substitution is built from; until then these draws go "
                    "through untouched.");
            }
        }
        cb->Release();
        return false;
    }
    cb->Release();
    particlecap::Shadow* sh = &g_blocks.at(slot)->shadow;
    g_cur = sh;
    if (!sh->valid) return false;
    if (variant >= 0) return shapeOk(reinterpret_cast<const float*>(sh->bytes), sh->size / 4);

    // A roll-free family: arm the rebind for forwardWithVerdict and leave the draw in the ladder as the game made it.
    if (sh->size < particlecap::kRangeEnd) return false;
    g_rfSlot = slot;
    g_rfIndex = rf;
    detail::g_rollFreePending = true;
    g_copyGpuCtx = ctx;
    return false;
}

void particleRollFreeBegin(ID3D11DeviceContext* ctx) {
    detail::g_rollFreePending = false;
    if (!ctx || detail::g_particleMode != Mode::kSteady || g_rfSlot < 0 || g_rfIndex < 0) return;
    guardedBudget(g_rfBudget, [&] {
        const bool timed = ((++g_rfBindCalls) & 7u) == 0;
        const int64_t t0 = timed ? qpcNow() : 0;
        ID3D11Buffer* cb = nullptr;
        ctx->VSGetConstantBuffers(1, 1, &cb);
        if (!cb) return;
        bool gpuOpen = false;
        if (g_blocks.needsCopy(g_rfSlot) && ((++g_copySeq) & 15u) == 0) gpuOpen = g_copyGpu.begin(ctx);
        ID3D11Buffer* ours = g_blocks.prepare(ctx, g_rfSlot, cb);
        if (gpuOpen) g_copyGpu.end(ctx);
        cb->Release();
        if (ours && g_blocks.bind(ctx, ours)) {
            detail::g_rollFreeEngaged = true;
            ++g_rfBound[g_rfIndex];
            if (!g_rfNoted[g_rfIndex]) {
                g_rfNoted[g_rfIndex] = true;
                Log::get().note(
                    "particle billboard: roll-free scene block first bound for vs %016llX (rows 277 and 278 at vertex "
                    "slot 1 only; the pixel stage keeps the game's block).",
                    static_cast<unsigned long long>(kRollFreeVs[g_rfIndex]));
            }
        }
        if (timed) { g_cpuTicks += static_cast<uint64_t>(qpcNow() - t0); ++g_cpuSampled; }
    });
}

void particleRollFreeEnd(ID3D11DeviceContext* ctx) {
    if (!detail::g_rollFreeEngaged) return;
    detail::g_rollFreeEngaged = false;
    g_blocks.restore(ctx);
}

void particleBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (!ctx || !g_cur || !g_cur->valid) return;
    const int variant = g_activeVariant;
    if (variant < 0 || variant >= kVariantCount) return;
    guardedBudget(g_subBudget, [&] {
        if (!g_vsTried[variant]) {
            g_vsTried[variant] = true;
            const BillboardVariant& d = kVariants[variant];
            g_ourVs[variant] = shaderSwapCreateVs(
                ctx, d.bytecode, d.bytecodeLen, d.name,
                "particle billboard");
            if (g_ourVs[variant]) {
                Log::get().note(
                    "particle billboard: replacement compiled for the %s "
                    "(vs %016llX). Each signature group needs its own -- "
                    "this family's shaders do not share one.",
                    variantLabel(variant),
                    static_cast<unsigned long long>(d.hash));
            }
        }
        if (!g_ourVs[variant]) return;   // compile failed: the game draws stock

        if (!g_ourCb) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 32;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            dev->CreateBuffer(&bd, nullptr, &g_ourCb);
            dev->Release();
            if (!g_ourCb) return;
        }

        solveCamera();
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(g_ourCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) ||
            !m.pData) {
            return;
        }
        float* f = static_cast<float*>(m.pData);
        f[0] = g_cam[0];
        f[1] = g_cam[1];
        f[2] = g_cam[2];
        // The flag the shader reads: without a solved viewer position it
        // keeps the game's own camera-plane basis rather than aiming at
        // a point nobody measured.
        f[3] = g_camOk ? 1.0f : 0.0f;
        f[4] = 0.0f;
        f[5] = 1.0f;
        f[6] = 0.0f;
        f[7] = 0.0f;
        ctx->Unmap(g_ourCb, 0);

        ctx->VSGetShader(&g_savedVs, nullptr, nullptr);
        ctx->VSGetConstantBuffers(3, 1, &g_savedCb3);
        ID3D11Buffer* ours = g_ourCb;
        ctx->VSSetConstantBuffers(3, 1, &ours);
        ctx->VSSetShader(g_ourVs[variant], nullptr, 0);
        g_engaged = true;
        ++g_applied;
        ++g_appliedBy[variant];
        if (g_camOk) ++g_facingUsed;
        g_lastFacing[0] = g_cam[0];
        g_lastFacing[1] = g_cam[1];
        g_lastFacing[2] = g_cam[2];
    });
}

void particleEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged || !ctx) return;
    g_engaged = false;
    ctx->VSSetShader(g_savedVs, nullptr, 0);
    if (g_savedVs) {
        g_savedVs->Release();
        g_savedVs = nullptr;
    }
    ctx->VSSetConstantBuffers(3, 1, &g_savedCb3);
    if (g_savedCb3) {
        g_savedCb3->Release();
        g_savedCb3 = nullptr;
    }
    const uint64_t now = nowMs();
    if (now - g_noteMs >= 10000) {
        Log::get().note(
            "particle billboard: steady -- %llu draw(s) in the last ten "
            "seconds through the replacement shader (%llu smoke plume, "
            "%llu solar flare), %llu of them with a solved viewer at "
            "(%.1f %.1f %.1f). Each quad now faces the viewer instead of "
            "the view axis. A zero in one of the two is not a fault -- it "
            "means you were nowhere near that effect.",
            static_cast<unsigned long long>(g_applied - g_appliedAtNote),
            static_cast<unsigned long long>(g_appliedBy[0]),
            static_cast<unsigned long long>(g_appliedBy[1]),
            static_cast<unsigned long long>(g_facingUsed),
            g_lastFacing[0], g_lastFacing[1], g_lastFacing[2]);
        g_noteMs = now;
        g_appliedAtNote = g_applied;
        g_facingUsed = 0;
        for (int i = 0; i < kVariantCount; ++i) g_appliedBy[i] = 0;
    }
}

void particleConfigure(Config& cfg) {
    // The witchspace starfield switch. "on" is the game's own behaviour and
    // the default; "off" empties the jump tunnel, which a player asked for
    // after 0.12.3 did it by accident.
    const std::string ws = runtimeVrProfile() ?
        cfg.getString("fix.witchspace_stars", "on") : "on";
    const bool wasHidden = g_hideWitchspaceStars;
    if (ws == "off") {
        g_hideWitchspaceStars = true;
    } else {
        if (ws != "on") {
            Log::get().note("witchspace stars: that is not on or off; "
                            "leaving them on.");
        }
        g_hideWitchspaceStars = false;
    }
    if (g_hideWitchspaceStars != wasHidden) {
        g_starsSkipped = 0;
        g_starsNoteMs = 0;
        Log::get().note(
            g_hideWitchspaceStars
                ? "witchspace stars: OFF. The streaking starfield inside the "
                  "hyperspace tunnel is not drawn -- the draw is withheld, "
                  "nothing else about the jump changes."
                : "witchspace stars: ON. The hyperspace tunnel keeps its "
                  "starfield, which is the game's own behaviour.");
    }

    // The two space dust switches, the same shape as the one above, each with
    // its own flag, counter and change note. Read through the same VR-profile
    // gate, so flat never sees "off" from a key its allow-list does not carry
    // (runtime_profile.h).
    auto applyDustKey = [](const std::string& v, const char* name, bool& hidden,
                           uint64_t& skipped, const char* offText,
                           const char* onText) {
        const bool wasHidden = hidden;
        if (v == "off") {
            hidden = true;
        } else {
            if (v != "on") {
                Log::get().note("%s: that is not on or off; leaving it on.", name);
            }
            hidden = false;
        }
        if (hidden != wasHidden) {
            skipped = 0;
            g_dustNoteMs = 0;
            Log::get().note("%s", hidden ? offText : onText);
        }
    };
    // The keys are read by their literal names, which the settings-schema
    // generator needs to see (tools/gen_settings_schema.py).
    const std::string sdv = runtimeVrProfile() ?
        cfg.getString("fix.supercruise_dust", "on") : "on";
    const std::string ndv = runtimeVrProfile() ?
        cfg.getString("fix.normal_space_dust", "on") : "on";
    applyDustKey(sdv, "supercruise space dust",
                g_supercruiseDustHidden, g_dustSkipped,
                "supercruise space dust: OFF. The supercruise streaks are not "
                "drawn -- the draw is withheld, nothing else about supercruise "
                "changes.",
                "supercruise space dust: ON. The supercruise streaks are drawn, "
                "as the game draws them.");
    applyDustKey(ndv, "normal-space dust",
                g_normalSpaceDustHidden, g_dustNormalSkipped,
                "normal-space dust: OFF. The normal-space specks are not drawn "
                "-- the draw is withheld, nothing else about the flight changes.",
                "normal-space dust: ON. The normal-space specks are drawn, as "
                "the game draws them.");

    const Mode wasMode = detail::g_particleMode;
    const std::string m = runtimeVrProfile() ?
        cfg.getString("fix.particle_billboard", "steady") : "stock";
    if (m == "steady") {
        detail::g_particleMode = Mode::kSteady;
    } else {
        if (m != "stock") {
            Log::get().note("particle billboard: that is not stock or "
                            "steady; running stock.");
        }
        detail::g_particleMode = Mode::kStock;
    }
    if (detail::g_particleMode != wasMode) {
        if (detail::g_particleMode == Mode::kSteady) {
            g_learnNoted = false;
            g_blocks.enabled = true;
            Log::get().note(
                "particle billboard: STEADY -- smoke and steam quads are "
                "built against world up instead of the view's up, so they "
                "face you without rolling when you tilt your head or swing "
                "the camera; the other camera-facing billboards (explosions "
                "among them) are given a roll-free camera basis at vertex "
                "slot 1. Read from the game's own shader: "
                "docs/particle-billboards.md.");
        } else {
            g_blocks.enabled = false;
            g_blocks.clear();
            publishWatched();
            g_cur = nullptr;
            detail::g_rollFreePending = false;
            Log::get().note("particle billboard: stock.");
        }
    }
}

bool particleWantsDraws() {
    return detail::g_particleMode == Mode::kSteady;
}

void particleShutdown() {
    for (int i = 0; i < kVariantCount; ++i) {
        if (g_ourVs[i]) {
            g_ourVs[i]->Release();
            g_ourVs[i] = nullptr;
        }
        g_vsTried[i] = false;
        g_appliedBy[i] = 0;
    }
    g_activeVariant = -1;
    if (g_ourCb) {
        g_ourCb->Release();
        g_ourCb = nullptr;
    }
    if (g_savedVs) {
        g_savedVs->Release();
        g_savedVs = nullptr;
    }
    if (g_savedCb3) {
        g_savedCb3->Release();
        g_savedCb3 = nullptr;
    }
    g_engaged = false;
    if (detail::g_rollFreeEngaged && g_copyGpuCtx) particleRollFreeEnd(g_copyGpuCtx);
    detail::g_rollFreePending = false;
    g_blocks.shutdown();
    publishWatched();
    g_cur = nullptr;
    g_copyGpuCtx = nullptr;
    g_copyGpu.reset();
}

}  // namespace edvr
