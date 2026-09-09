#include "ui_depth.h"

#include <windows.h>

#include <d3d11.h>

#include <cstdint>
#include <cstdlib>   // _strtoui64, strtod: the hash lists and the planes
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "depth_probe.h"    // depthProbeIsSceneDepth, ...Format: where the pass reads
#include "exposure_fix.h"   // lookupShaderHash
#include "shader_swap.h"    // shaderSwapCompilePs: the alpha-aware depth shaders
#include "temporal_pass.h"  // temporalPassPlanes: the scene's encoding
#include "vscreen.h"        // vScreenSetRenderTargetsRaw: the rebind past the shadow

namespace edvr {
namespace {

// The GUI renderer's three families on game build 332753 (docs/
// crisp-ui-handoff.md, A1): the textureless vector widget shader, the
// glyph-atlas text shader, the BC7 icon shader. What they draw into is a UI
// surface.
constexpr uint64_t kGuiVector = 0x666EF0C4C616F67Eull;
constexpr uint64_t kGuiText   = 0x1012E00B3CB44469ull;
constexpr uint64_t kGuiIcons  = 0xA3E5D3FCBC1165F8ull;
// The flight HUD's vector family, drawn straight into the eye (hud_grain.h).
constexpr uint64_t kFlightHud = 0xB7790CBFC6554097ull;
// ...and its pixel shader, which is no vector rasteriser: it MARCHES a
// noise-modulated capsule for each stroke (kHudDepthHlsl says what it does
// before the march), and the empty corners of a stroke's bounding quad come
// out at alpha nought without a discard.
constexpr uint64_t kFlightHudPs = 0x8DEF46452FA459F5ull;
// The cockpit's holo-panel family (panel_upscale.h).
constexpr uint64_t kHoloPanel = 0x81216C77F90DEDD6ull;
// The two interface composites drawn through the interface projection: the
// menu's and the loader's panel (vs A888D51024D9798E, ps 9107E72CB016CC02)
// and the loader's curved screen (vs 4EF6DDB075A927FA, ps 85565E9261812E2F).
constexpr uint64_t kPanelVs  = 0xA888D51024D9798Eull;
constexpr uint64_t kPanelPs  = 0x9107E72CB016CC02ull;
constexpr uint64_t kScreenVs = 0x4EF6DDB075A927FAull;
constexpr uint64_t kScreenPs = 0x85565E9261812E2Full;
// The panel family's other two pixel shaders. The dump of 2026-09-06 holds
// 140 pixel shaders whose input signature the panel vertex shader can feed,
// and exactly three read the whole of it; the other two are this one with a
// colour matrix (cb1[85..87]) after the tone curve, and that one again with
// a 2-tap smear where the first has 8. Their disassemblies differ from
// kPanelPs's in nine lines, none of them in the sampling: all three take the
// interface surface from t1 through s1 at TEXCOORD6, so kPanelDepthHlsl is
// their transcription too. The in-flight escape menu draws through one of
// them, which is why it kept swimming while the main menu was fixed: the
// composite was classified, found no depth shader for its pixel stage and
// was left alone (Sean, 2026-09-08; the flight of c468661 counts 1.0 such
// draws a frame in the window its menu was up).
constexpr uint64_t kPanelPsTinted = 0x015EF9349EC097E8ull;
constexpr uint64_t kPanelPsCheap  = 0xF2F872B191F656D5ull;
// The sprite composite: the cockpit's other interface-surface family, and
// the one the flight of 2026-09-08 named as still swimming after the panel
// variants above went in. It draws into the lit HDR target the holo panels
// and the flight HUD use, so it belongs to the cockpit's UI pass and shares
// the scene's projection where it binds the scene's pair -- hence it counts
// as a scene family, like the holo panels, rather than through
// advanced.ui_depth_families (which would also claim its draws that sample
// no interface surface at all, and there are tens of thousands of those:
// hud_sprite.h).
//
// Its pixel shader wants no new transcription. target_sharp.h read the same
// shader out of its disassembly in September and states it in full: one
// bilinear sample of t0 through s0 at TEXCOORD0, an alpha discard, and a
// HUD colour matrix -- which is kScreenDepthHlsl's shape exactly, register
// for register. (target_sharp calls this family the target indicator;
// hud_sprite.h later disproved that by suppression -- dropping all 86k of
// its draws left the indicator on screen. The shader reading is sound
// whatever the family turns out to draw.)
constexpr uint64_t kHudSprite = 0xE508648660A352B2ull;
constexpr uint64_t kSpritePs  = 0x63ABD86359B57D01ull;
// The cockpit holo panels' pixel shader, for the reactive mask's coverage:
// it samples the interface surface at t2 through s1 at TEXCOORD8 (its own
// disassembly, 2026-09-08 -- the same shape as the menu panel's, which
// takes t1 through s1 at TEXCOORD6).
constexpr uint64_t kHoloPanelPs = 0xA2965EC2931A39C8ull;
// A mesh draw whose pixel shader (258B95AC99520C1F) reads nothing and writes
// nothing; the interface surface in its slot 0 is a leftover binding, and
// the surface rule took it for a composite on the loading screen
// (2026-09-07). Never treated.
constexpr uint64_t kNullPsMesh = 0xB018D143700AB803ull;

constexpr uint32_t kMaxSurfaces = 64;      // a session showed thirteen
constexpr uint32_t kMaxHashes = 16;
constexpr uint32_t kMaxStates = 16;        // the game has a handful of depth states
constexpr uint32_t kMemoSize = 1024;       // a power of two: the probe masks
constexpr uint32_t kMemoProbe = 4;
constexpr uint32_t kMemoLifeFrames = 120;  // a sampled view's answer
constexpr uint32_t kVsMemoLifeFrames = 600; // a shader's hash
constexpr uint32_t kChecksPerTarget = 64;  // hashes asked of a newly bound target
constexpr uint32_t kExhausted = 64;        // targets asked to exhaustion, remembered
constexpr uint32_t kExhaustedRearmFrames = 600;
constexpr uint32_t kMaxFamilyLines = 24;  // a pair per outcome, not per pair
constexpr uint32_t kTotalsFrames = 1800;   // about 20 s at 90 Hz

FaultBudget g_budget("uiDepth", 5);

bool     g_keyOn = false;      // fix.ui_depth = on
bool     g_passOn = false;     // fix.temporal_aa is not off
bool     g_trained = false;    // ...and it is NVIDIA's history, which reads the mask
bool     g_on = false;         // both
bool     g_stoodDown = false;
bool     g_announced = false;
bool     g_waitingNoted = false;
bool     g_testAlways = false; // advanced.ui_depth_test = always
bool     g_menus = true;       // advanced.ui_depth_menus: the interface-projection
                               // composites get the alpha-aware depth pass
bool     g_eyesSwapped = false; // advanced.ui_depth_eyes = swapped: the A/B for
                                // the order rule that names the eye
// THE ENCODING. The menu's and the loader's composites are drawn through
// the interface projection -- near 0.1 m, far 1000 m on build 332841 (the
// receiver logs every pair the game asks for) -- while the scene pair the
// pass reads is decoded with the scene's (0.025 m, 50000 m). A reversed-Z
// value is near/z to within a part in a thousand this side of ten metres,
// so the two encodings differ by the ratio of the nears: a composite's
// depth written as it comes decoded four times too near (measured
// 2026-09-07: the menu panel at 0.26 m where 1.03 m is right). The depth
// pass's VIEWPORT depth range carries the correction: MaxDepth =
// sceneNear / uiNear scales what the rasteriser writes, no maths in the
// shader.
float    g_uiNear = 0.1f;       // advanced.ui_depth_planes
float    g_uiFar = 1000.0f;
float    g_alphaFloor = 0.5f;   // advanced.ui_depth_alpha: below it, no depth
float    g_reactive = 0.0f;     // advanced.ui_depth_reactive: the bias mask's value
bool     g_scaleNoted = false;
constexpr uint32_t kMaxViewports = 16;
D3D11_VIEWPORT g_savedVps[kMaxViewports];
UINT     g_savedVpCount = 0;
bool     g_vpScaled = false;
uint32_t g_frame = 0;

uint64_t g_families[kMaxHashes];
uint32_t g_familyCount = 0;
uint64_t g_exclude[kMaxHashes];
uint32_t g_excludeCount = 0;

// A hashed memo from a pointer to a value, frame-stamped: one probe of a
// few slots, no lock. The pointer is an identity only, never dereferenced
// here (binding_shadow.h's bargain); an answer older than its life is
// asked again, so a recycled address cannot lie for long.
template <uint32_t N>
struct PtrMemo {
    struct Slot {
        const void* key = nullptr;
        uint32_t    frame = 0;
        uint64_t    value = 0;
    };
    Slot slots[N];

    static uint32_t home(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p) >> 4;
        return static_cast<uint32_t>(v * 2654435761u) & (N - 1);
    }
    bool get(const void* p, uint32_t now, uint32_t life, uint64_t* value) const {
        const uint32_t h = home(p);
        for (uint32_t i = 0; i < kMemoProbe; ++i) {
            const Slot& s = slots[(h + i) & (N - 1)];
            if (s.key != p) continue;
            if (now - s.frame >= life) return false;
            *value = s.value;
            return true;
        }
        return false;
    }
    void put(const void* p, uint32_t now, uint64_t value) {
        const uint32_t h = home(p);
        uint32_t victim = h & (N - 1);
        uint32_t oldest = 0;
        for (uint32_t i = 0; i < kMemoProbe; ++i) {
            const uint32_t idx = (h + i) & (N - 1);
            const Slot& s = slots[idx];
            if (s.key == p || s.key == nullptr) {
                victim = idx;
                break;
            }
            const uint32_t age = now - s.frame;
            if (age >= oldest) {
                oldest = age;
                victim = idx;
            }
        }
        slots[victim].key = p;
        slots[victim].frame = now;
        slots[victim].value = value;
    }
    void clear() {
        for (uint32_t i = 0; i < N; ++i) slots[i] = Slot();
    }
};

PtrMemo<kMemoSize> g_viewMemo;   // sampled view -> 1 surface / 0 not
PtrMemo<kMemoSize> g_vsMemo;     // vertex shader -> its bytecode hash
PtrMemo<kMemoSize> g_psMemo;     // pixel shader -> its bytecode hash

// Learned surfaces: resource identities with the shape they had when
// learned. A match is by identity AND shape, so an address the game
// recycled for a different texture drops out instead of lying (the ABA
// binding_shadow.h warns about). A ring past kMaxSurfaces, counted.
struct Surface {
    void*    res = nullptr;
    uint32_t w = 0, h = 0, fmt = 0;
};
Surface  g_surfaces[kMaxSurfaces];
uint32_t g_surfaceCount = 0;
uint32_t g_surfaceNext = 0;
uint32_t g_evictions = 0;      // ring overwrites + recycled-address drops
bool     g_ringNoted = false;

// The offscreen learner's state for the currently bound target.
uint32_t g_rtvGen = ~0u;
bool     g_rtvKnown = false;
uint32_t g_rtvChecks = 0;
void*    g_rtvRes = nullptr;
uint32_t g_rtvW = 0, g_rtvH = 0, g_rtvFmt = 0;

// Targets asked kChecksPerTarget times without a GUI draw: not asked again
// for kExhaustedRearmFrames, so a shadow map rebound a hundred times a
// frame does not cost a hundred hash queries a frame for the session.
struct Exhausted {
    void*    res = nullptr;
    uint32_t frame = 0;
};
Exhausted g_exhausted[kExhausted];
uint32_t  g_exhaustedNext = 0;

// The bound depth target, judged once per frame per view: is it the scene
// pair the pass reads?
const void* g_dsvJudged = nullptr;
uint32_t    g_dsvJudgedFrame = ~0u;
bool        g_dsvIsScene = false;

// Families seen, for the one-line-each log. Kept as the VERTEX shader and
// the PIXEL shader together: a family's pixel stage has variants, and one
// of them going untreated is exactly what a field log has to be able to
// say. Keyed on the vertex shader alone, the in-flight escape menu's
// composite was silent for two days behind the main menu's line
// (2026-09-08).
uint64_t    g_familyLoggedVs[kMaxFamilyLines];
uint64_t    g_familyLoggedPs[kMaxFamilyLines];
const char* g_familyLoggedHow[kMaxFamilyLines];
uint32_t    g_familyLoggedCount = 0;
// Pixel shaders adopted by another's transcription, named once each.
bool     g_variants = true;    // advanced.ui_depth_variants
uint64_t g_variantLogged[kMaxHashes];
uint32_t g_variantLoggedCount = 0;

// The game's depth-stencil state and its writing twin. The game's is held
// (AddRef) so its pointer stays a valid key for as long as the twin lives.
struct StatePair {
    ID3D11DepthStencilState* game = nullptr;
    ID3D11DepthStencilState* ours = nullptr;
    bool                     alreadyWrites = false;  // twin unneeded
};
StatePair g_states[kMaxStates];
uint32_t  g_stateCount = 0;
bool      g_statesFullNoted = false;
// SOFT PARTICLES (fix.temporal_aa_particles, 2026-09-09): a draw into the
// scene pair that SAMPLES the scene's depth -- the resource the flight HUD
// binds at t0 for its own depth test, which the particle shaders read to
// fade near geometry -- and is not the interface's. Elite's smoke trails
// are such draws: translucent quads that write their depth over the whole
// quad, clear part and all, so the temporal pass reprojected everything
// seen through a quad at the quad's distance and each quad showed as a
// smeared rectangle across the stars behind it, and near a moving ship on
// its own path the quads tore with the ship ("weird rectangular artifacts
// in the smoke trail", the ships' second flight). Their depth write is
// muted: the game's own state with DepthWriteMask ZERO, cached per game
// state as the writing twin is. The smoke then takes the motion of what
// is behind it, which blurs it as smoke should blur, and nothing behind
// it is torn.
bool      g_particlesOn = true;
constexpr uint32_t kMaxResolves = 4;
void*     g_resolves[kMaxResolves] = {};   // the depth resources the flight HUD samples at t0 (identities, never dereferenced)
uint32_t  g_resolveCount = 0;
struct MutePair {
    ID3D11DepthStencilState* game = nullptr;   // held (AddRef), the key
    ID3D11DepthStencilState* ours = nullptr;   // null when the game's writes nothing already
};
MutePair  g_mutes[kMaxStates];
uint32_t  g_muteCount = 0;
bool      g_mutesFullNoted = false;
struct ParticleFamily {
    uint64_t ps;
    uint64_t draws;
};
constexpr uint32_t kMaxParticleFamilies = 12;
ParticleFamily g_particleFamilies[kMaxParticleFamilies] = {};
uint32_t  g_particleFamilyCount = 0;
uint64_t  g_wParticles = 0;         // draws muted this window
uint64_t  g_sessionParticles = 0;
bool      g_createFailedNoted = false;

// THE ALPHA-AWARE DEPTH PASS, for a composite drawn through the interface
// projection. Its own draw is left exactly as the game issued it; a second
// draw of the same geometry follows with no colour target, EDVR's pixel
// shader in place of the game's -- the same surface sample the game's
// takes, clipped below the alpha floor, so the dialog's box and its text
// write depth and its 40% scrim over the ship model does not -- the
// pass's depth for the eye bound where the composite's own is not it,
// the game's nearer-wins test (a model in front of the screen keeps its
// depth), and the viewport depth range converting the encoding. The pixel
// shaders are transcriptions of the game's alpha path, from the dumps of
// 2026-09-06: ps 9107E72CB016CC02 samples the surface at TEXCOORD6 through
// slot 1; ps 85565E9261812E2F at TEXCOORD0 through slot 0.
// Each returns the reactive strength as its colour, so ONE shader serves
// both errands: with a depth target and no colour target it writes depth
// where the interface covers, and with the mask bound as its colour target
// it marks the same pixels for NVIDIA. b13 carries (alpha floor, strength);
// the game's composites declare b2 alone, so b13 is free.
const char kPanelDepthHlsl[] =
    "Texture2D<float4> Surf : register(t1);\n"
    "SamplerState Smp : register(s1);\n"
    "cbuffer P : register(b13) { float4 floorAndStrength; };\n"
    "struct In {\n"
    "    float4 tc0 : TEXCOORD0;\n"
    "    float3 tc2 : TEXCOORD2;\n"
    "    float3 tc4 : TEXCOORD4;\n"
    "    float3 tc5 : TEXCOORD5;\n"
    "    float2 tc6 : TEXCOORD6;\n"
    "};\n"
    "float4 main(In i) : SV_Target {\n"
    "    float a = Surf.Sample(Smp, i.tc6).a;\n"
    "    clip(a - floorAndStrength.x);\n"
    "    return floorAndStrength.w;\n"
    "}\n";
const char kScreenDepthHlsl[] =
    "Texture2D<float4> Surf : register(t0);\n"
    "SamplerState Smp : register(s0);\n"
    "cbuffer P : register(b13) { float4 floorAndStrength; };\n"
    "struct In { float2 tc0 : TEXCOORD0; };\n"
    "float4 main(In i) : SV_Target {\n"
    "    float a = Surf.Sample(Smp, i.tc0).a;\n"
    "    clip(a - floorAndStrength.x);\n"
    "    return floorAndStrength.w;\n"
    "}\n";
// The holo material: the cockpit's panels and, instanced from the pool at
// the target, the target markers. Its depth was written in place by the
// game's own draw under the writing twin until 2026-09-09, this shader only
// marking the mask; but the material's alpha carries an eight-tap smear
// past every stroke of the surface (a glow the game discards only under
// 1e-5), and in place each marker corner wrote its depth over the station
// around it. Now this shader writes the depth too, through the second draw
// in the scene's projection (Mode::kReissueScene), under the surface's
// strokes at the floor and not under the glow. A panel's translucent
// background under the floor keeps the scene's depth -- a flat dark colour,
// which no reprojection can smear visibly; its text and frame keep theirs.
const char kHoloDepthHlsl[] =
    "Texture2D<float4> Surf : register(t2);\n"
    "SamplerState Smp : register(s1);\n"
    "cbuffer P : register(b13) { float4 floorAndStrength; };\n"
    "struct In {\n"
    "    float4 tc0 : TEXCOORD0;\n"
    "    float3 tc4 : TEXCOORD4;\n"
    "    float3 tc6 : TEXCOORD6;\n"
    "    float3 tc7 : TEXCOORD7;\n"
    "    float2 tc8 : TEXCOORD8;\n"
    "};\n"
    "float4 main(In i) : SV_Target {\n"
    "    float a = Surf.Sample(Smp, i.tc8).a;\n"
    "    clip(a - floorAndStrength.x);\n"
    "    return floorAndStrength.w;\n"
    "}\n";

// THE FLIGHT HUD'S COVERAGE, for the depth pass in the scene's projection.
//
// Under the writing twin the HUD's own draw wrote depth over every pixel of
// each stroke's bounding quad, and once the temporal pass registered a
// station's turn (docs/per-object-motion.md, tier 2) the station under a
// target bracket showed "a quad that is blurred under the bracket" (the
// player, 2026-09-08): those pixels carried the bracket's depth and not the
// station's. The shader's disassembly (ps 8DEF46452FA459F5, the dump of
// 2026-09-06) says why no discard saves them: after a manual depth test
// against the eye-sized depth resolve at t0 (v1 holds the clip position),
// a fade from the distance and the element's own strength, and the
// geometry of one stroke -- a capsule from TEXCOORD5 to TEXCOORD16 of
// radius TEXCOORD13.w, the ray from TEXCOORD0 through the pixel's world
// point in TEXCOORD18 -- it marches cb1[203].w steps along the ray inside
// that capsule, sampling three octaves of value noise from t1, and the
// density at each step is a smoothstep of (noise * 0.571 - q), q being the
// normalised squared distance from the stroke's axis. Where q exceeds the
// noise the sum is nought, the transmittance stays one, and the pixel is
// emitted at alpha nought with its depth written all the same.
//
// This stand-in transcribes everything before the march register for
// register -- the same test, the same discards, the same q -- and stands in
// for the march with its own bound: the density is nought past q = 0.571 at
// full noise and about half that at the noise's mean, so the alpha is taken
// as the fade times a ramp that reaches nought at q = 0.35, and clipped
// below the floor. That writes depth under a stroke's bright core and not
// under its bounding quad, which is the whole of the errand; the glow's
// fringe, a few pixels of faint light, keeps the scene's depth beneath it.
// The screen-space mode of the same shader (TEXCOORD2.w over a half) takes
// its distance from TEXCOORD17.x, and is transcribed too.
const char kHudDepthHlsl[] =
    "Texture2D<float4> Depth : register(t0);\n"
    "SamplerState Smp1 : register(s1);\n"
    "cbuffer CB1 : register(b1) { float4 cb1[205]; };\n"
    "cbuffer P : register(b13) { float4 floorAndStrength; };\n"
    "struct In {\n"
    "    float4 tc0 : TEXCOORD0;\n"
    "    float4 tc1 : TEXCOORD1;\n"
    "    float4 tc2 : TEXCOORD2;\n"
    "    float4 tc5 : TEXCOORD5;\n"
    "    float4 tc9 : TEXCOORD9;\n"
    "    float4 tc10 : TEXCOORD10;\n"
    "    float4 tc13 : TEXCOORD13;\n"
    "    float4 tc16 : TEXCOORD16;\n"
    "    float2 tc17 : TEXCOORD17;\n"
    "    float3 tc18 : TEXCOORD18;\n"
    "    float4 pos : SV_Position;\n"
    "};\n"
    "float4 main(In i, out float oDepth : SV_Depth) : SV_Target {\n"
    "    // The game's own depth test against the resolve at t0.\n"
    "    float2 uv = i.tc1.xy / i.tc1.z * 0.5 + 0.5;\n"
    "    float sceneZ = Depth.Sample(Smp1, float2(uv.x, 1.0 - uv.y)).x;\n"
    "    if (sceneZ - i.tc1.z < 0.0) discard;\n"
    "    if (i.tc5.w - 0.01 < 0.0) discard;\n"
    "    // The fade: the element's strength against a distance term.\n"
    "    float k = cb1[204].x / (cb1[204].x + 0.00001);\n"
    "    float fade = saturate(length(i.tc10.xyz) * 0.05 - i.tc13.w * 0.5);\n"
    "    float near = saturate(i.tc9.w / max(cb1[204].x, 0.01));\n"
    "    float opacity = k * (near - fade) + fade;\n"
    "    if (opacity - 0.001 < 0.0) discard;\n"
    "    // One stroke's capsule, and the pixel's distance from its axis.\n"
    "    float3 seg = i.tc5.xyz - i.tc16.xyz;\n"
    "    if (length(seg) - 0.01 < 0.0) discard;\n"
    "    float3 e = i.tc18.xyz - i.tc5.xyz;\n"
    "    float t = dot(e, -seg) / (seg.x * seg.x);\n"
    "    float dist;\n"
    "    if (t < 0.0) dist = length(e);\n"
    "    else if (t > 1.0) dist = length(i.tc18.xyz - (i.tc5.xyz - seg));\n"
    "    else dist = length(i.tc18.xyz - (i.tc5.xyz - t * seg));\n"
    "    float nd = i.tc2.w > 0.5 ? (i.tc17.x * 2.0 - 1.0) : saturate(dist / i.tc13.w);\n"
    "    float q = nd * nd;\n"
    "    // The march's bound stands in for the march.\n"
    "    float a = opacity * saturate(1.0 - q / 0.35);\n"
    "    // The stroke's core alone -- its depth and its mask -- and nothing\n"
    "    // under the glow: those pixels are the scene's, with the scene's\n"
    "    // depth and the scene's motion, the glow blended over them. The eye\n"
    "    // dumps of 2026-09-09 tried the other two ways. With the glow given\n"
    "    // the stroke's own near depth the station seen through it reprojected\n"
    "    // as near and smeared; with the glow given the scene's depth but\n"
    "    // marked (reactive, riding the station's turn) the reticle 'swam';\n"
    "    // and marked but kept off that turn, the docking hologram's strokes\n"
    "    // over the hub's drum kept the whole drum off it and smeared it.\n"
    "    clip(a - max(floorAndStrength.x, 0.7));\n"
    "    // A core drawn AT the surface (its own depth within half again of\n"
    "    // the scene's) keeps its depth and turns with the body; a core that\n"
    "    // floats -- the reticle, the docking hologram's strokes -- takes the\n"
    "    // scene's depth behind it. A target's reticle tracks a far point,\n"
    "    // and at that point's depth the camera's path carries it right under\n"
    "    // the ship's turn and the head's alike, with no parallax to speak\n"
    "    // of. At its own depth of tens of metres the ship's own motion\n"
    "    // reprojected it by whole degrees a frame ('swim/shimmering on the\n"
    "    // hud sprites'); at one metre, on the ship's path, it held under the\n"
    "    // head but not under the ship's turn, and swam again (2026-09-09).\n"
    "    // ...and the mask says which: floorAndStrength.w is the floating\n"
    "    // value, its quantum odd, which keeps the temporal pass's body path\n"
    "    // off the stroke (floorBuffer says) -- a target bracket tracks the\n"
    "    // station's centre and does not turn with the station, and until\n"
    "    // 2026-09-09 its side over the station's silhouette rode the spin\n"
    "    // while its side over the sky did not: 'shimmering on just one side'.\n"
    "    bool attached = i.pos.z <= sceneZ * 1.5;\n"
    "    oDepth = attached ? i.pos.z : sceneZ;\n"
    "    return attached ? floorAndStrength.y : floorAndStrength.w;\n"
    "}\n";

// A transcription stands in for the pixel shaders it names -- and, when the
// game draws a variant none of them names, for any pixel shader of the same
// VERTEX family that takes the interface surface from the slot this one
// reads. The signature a replacement must match is the vertex shader's
// output, so a variant of the same family always fits; the slot is the part
// that could differ, and it is checked rather than assumed (the classifier
// already knows which slot held the learned surface). What is left unchecked
// is the TEXCOORD the variant samples at, which is why the fallback names
// every shader it adopts in the log and advanced.ui_depth_variants turns it
// off.
constexpr uint32_t kMaxStandIns = 4;
struct DepthShader {
    uint64_t            ps[kMaxStandIns];  // the game's pixel shaders it stands in for
    uint64_t            vs[kMaxStandIns];  // their vertex families, for a variant
    uint32_t            slot;              // the PS SRV slot its HLSL reads
    const char*         hlsl;
    size_t              len;
    const char*         name;
    ID3D11PixelShader*  shader;
    bool                tried;
};
DepthShader g_depthShaders[4] = {
    {{kPanelPs, kPanelPsTinted, kPanelPsCheap, 0}, {kPanelVs, 0, 0, 0}, 1,
     kPanelDepthHlsl, sizeof(kPanelDepthHlsl) - 1, "ui_depth_panel_ps", nullptr, false},
    // The flight HUD's coverage (kHudDepthHlsl): its slot is the depth
    // resolve it tests against, not an interface surface, so no variant of
    // another family can borrow it by slot.
    {{kFlightHudPs, 0, 0, 0}, {kFlightHud, 0, 0, 0}, 0xFFFFu,
     kHudDepthHlsl, sizeof(kHudDepthHlsl) - 1, "ui_depth_hud_ps", nullptr, false},
    // The loader's curved screen and the sprite composite are one shader's
    // work apart: both take one bilinear sample of t0 through s0 at
    // TEXCOORD0 and discard on its alpha, so this transcription is already
    // both their coverage.
    {{kScreenPs, kSpritePs, 0, 0}, {kScreenVs, kHudSprite, 0, 0}, 0,
     kScreenDepthHlsl, sizeof(kScreenDepthHlsl) - 1, "ui_depth_screen_ps", nullptr, false},
    {{kHoloPanelPs, 0, 0, 0}, {kHoloPanel, 0, 0, 0}, 2,
     kHoloDepthHlsl, sizeof(kHoloDepthHlsl) - 1, "ui_depth_holo_ps", nullptr, false},
};
struct FloorCb {
    ID3D11Buffer* cb = nullptr;
    float         floor = -1.0f;
    float         strength = -1.0f;
    float         nearDepth = -1.0f;   // the depth value at one metre (temporalPassDepthAt), for a floating stroke's core
};
FloorCb g_floorCbs[3];   // [0] the interface proper, [1] the holo material and the sprite, [2] the flight HUD (g_reissueMaskSlot)
ID3D11DepthStencilState* g_reissueDss = nullptr;   // GEQUAL, write all
bool          g_reissueDssFailedNoted = false;

// THE REACTIVE MASK (ui_depth.h): one per eye at the render size, cleared
// every frame, marked by the same second draw that writes the interface's
// depth, handed to NVIDIA by the temporal pass.
struct Mask {
    ID3D11Texture2D*        tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    uint32_t                w = 0, h = 0;
    bool                    marked = false;   // anything drawn since the clear
};
Mask     g_mask[2];
bool     g_maskFailedNoted = false;
bool     g_maskSizeNoted = false;
ID3D11BlendState* g_maskBlend = nullptr;     // opaque, red only
ID3D11DepthStencilState* g_maskDss = nullptr; // test as the game, writes off

// The pass's depth view for a rebind: EDVR's own view over the probe's
// texture, in the format the game binds it with; one per eye, remade when
// the pair moves.
constexpr uint32_t kMaxRtvs = 8;
struct PairView {
    ID3D11Texture2D*         tex = nullptr;   // identity only, the probe holds the reference
    ID3D11DepthStencilView*  dsv = nullptr;   // ours, over that texture
};
PairView g_pair[2];
bool     g_pairCreateFailedNoted = false;
bool     g_rebindNoted = false;
// The eye a treated draw belongs to, by the ORDER its colour target first
// appears in the frame among treated draws: the first is the left, the
// depth probe's own convention for the pair (first bound = left).
// WHICH EYE a treated draw's colour target is, by the order the targets
// appear in the frame -- but counted SEPARATELY FOR EACH SHAPE, which is
// the whole of the 2026-09-08 escape-menu bug.
//
// The rule was one two-slot table for the frame: first target seen is the
// left eye, second the right, anything after that has no eye and its draw
// is declined. That holds while every treated draw goes into the same pair
// of targets, which is true in the menus -- and false in the cockpit, where
// Elite renders through three pairs at the same size. The holo panels and
// the flight HUD are treated into the LIT HDR pair (R11G11B10_FLOAT), and
// they fill both slots early in the frame; the escape menu then composites
// into the TONEMAPPED pair (R8G8B8A8_TYPELESS) and finds the table full, so
// it is declined for want of an eye and never writes its depth. That is
// exactly the reported defect: the main menu is fixed because nothing else
// is treated there, and the same menu opened in flight is not.
//
// Counting per (width, height, format) is the same rule applied where it is
// actually true. Each pair gets its own left and right, a third target of
// one shape still has no eye, and the table is sized for a handful of
// shapes rather than one.
struct FrameTarget {
    const void* res = nullptr;
    uint32_t    w = 0, h = 0, fmt = 0;
    uint32_t    eye = 0;
};
constexpr uint32_t kMaxFrameTargets = 8;
FrameTarget g_frameTargets[kMaxFrameTargets];
uint32_t    g_frameTargetCount = 0;
bool        g_frameTargetsFullNoted = false;

// Per draw: what the classification decided, consumed by Begin/End and by
// the re-issue.
// kReissueScene: a scene-projection family whose depth goes through the
// second draw with a coverage shader instead of the writing twin -- the
// flight HUD, whose own draw would write every pixel of a stroke's
// bounding quad. No viewport change and no rebind: it already draws into
// the scene's pair with the scene's projection.
enum class Mode { kNone, kInPlace, kReissue, kReissueScene, kMuteDepth };
Mode                     g_mode = Mode::kNone;
bool                     g_engaged = false;
ID3D11DepthStencilState* g_savedDss = nullptr;   // the in-place swap's, owned by Begin/End
UINT                     g_savedRef = 0;
bool                     g_wantRebind = false;
bool                     g_wantMask = false;     // this draw marks the reactive mask
int                      g_drawEye = -1;
uint32_t                 g_rebindW = 0, g_rebindH = 0;
int                      g_rebindEye = -1;
bool                     g_rebound = false;      // the OM was swapped for this draw
ID3D11RenderTargetView*  g_savedRtvs[kMaxRtvs] = {};
ID3D11DepthStencilView*  g_savedDsv = nullptr;
DepthShader*             g_reissueShader = nullptr;
// What the reissue writes into the reactive mask, relative to the strength:
// nought for the interface proper (the composites: panels, labels), three
// quanta of 255 under it for the holo material's markers and the
// target-time sprite, and half the strength for the flight HUD's strokes
// -- their cores only; their glow is not marked. The mask's value is the
// only way the pass can tell one pixel's kind from another's, and a few
// quanta read the same to NVIDIA. Which pixels ride a turning body's path
// is the value's PARITY (floorBuffer), not its band: a flight HUD stroke's
// core drawn at the surface rides, everything that floats keeps the
// camera's path. Measured 2026-09-09 from an eye dump, when a band said
// which: with the chevrons excluded, the station under each and its few
// pixels of halo fell to the camera's path and smeared -- the glow was
// marked then; with the holo material alone brought across, no change --
// the chevrons are the flight HUD's.
float                    g_reissueMaskOffset = 0.0f;
int                      g_reissueMaskSlot = 0;   // which cached constant buffer carries it (floorBuffer)
ID3D11PixelShader*       g_savedPs = nullptr;
// The second draw's saved state, kept apart from the in-place swap's: the
// two nest, and sharing one slot let the re-issue's restore null the state
// the in-place End was still to put back.
ID3D11DepthStencilState* g_reSavedDss = nullptr;
UINT                     g_reSavedRef = 0;
ID3D11BlendState*        g_reSavedBlend = nullptr;
FLOAT                    g_reSavedBlendFactor[4] = {};
UINT                     g_reSavedSampleMask = 0;
bool                     g_reissueOn = false;

// Counters: this window, and the session.
uint32_t g_wComposite = 0, g_wDirect = 0, g_wWrote = 0, g_wDepthless = 0,
         g_wNotScene = 0, g_wRebound = 0, g_wNoPair = 0, g_wReissued = 0,
         g_wNoShader = 0, g_wAlreadyWrote = 0, g_wNoTwin = 0, g_wLearned = 0,
         g_wMarked = 0, g_wFrames = 0;
uint64_t g_sessionWrote = 0;
bool     g_maskNotedOnce = false;

void resetWindow() {
    g_wComposite = g_wDirect = g_wWrote = g_wDepthless = g_wNotScene = 0;
    g_wRebound = g_wNoPair = g_wReissued = g_wNoShader = 0;
    g_wAlreadyWrote = g_wNoTwin = g_wLearned = g_wMarked = 0;
    g_wFrames = 0;
}

int surfaceIndex(const void* res) {
    for (uint32_t i = 0; i < g_surfaceCount; ++i) {
        if (g_surfaces[i].res == res) return static_cast<int>(i);
    }
    return -1;
}

// A resolved texture: a learned surface, or an address a surface once had
// that now holds something else (dropped, counted).
bool surfaceMatches(const ResourceInfo& info) {
    const int i = surfaceIndex(info.resource);
    if (i < 0) return false;
    Surface& s = g_surfaces[i];
    if (s.w == info.a && s.h == info.b && s.fmt == info.fmt) return true;
    s = g_surfaces[g_surfaceCount - 1];
    g_surfaces[g_surfaceCount - 1] = Surface();
    --g_surfaceCount;
    if (g_surfaceNext >= g_surfaceCount) g_surfaceNext = 0;
    ++g_evictions;
    return false;
}

void addSurface(const void* res, uint32_t w, uint32_t h, uint32_t fmt) {
    if (surfaceIndex(res) >= 0) return;
    Surface s;
    s.res = const_cast<void*>(res);
    s.w = w;
    s.h = h;
    s.fmt = fmt;
    if (g_surfaceCount < kMaxSurfaces) {
        g_surfaces[g_surfaceCount++] = s;
    } else {
        g_surfaces[g_surfaceNext] = s;
        g_surfaceNext = (g_surfaceNext + 1) % kMaxSurfaces;
        ++g_evictions;
        if (!g_ringNoted) {
            g_ringNoted = true;
            Log::get().note("ui depth: more than %u interface surfaces learned; the "
                            "oldest are forgotten from here on and their panels "
                            "stop writing depth until learned again.",
                            kMaxSurfaces);
        }
    }
    ++g_wLearned;
}

bool inList(const uint64_t* list, uint32_t n, uint64_t h) {
    for (uint32_t i = 0; i < n; ++i) {
        if (list[i] == h) return true;
    }
    return false;
}

// The bound vertex shader's hash: one COM call, then the memo instead of
// the registry's lock on every draw.
uint64_t boundVsHash(ID3D11DeviceContext* ctx) {
    uint64_t h = 0;
    guardedBudget(g_budget, [&] {
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        if (!vs) return;
        uint64_t memo = 0;
        if (g_vsMemo.get(vs, g_frame, kVsMemoLifeFrames, &memo)) {
            h = memo;
        } else {
            h = lookupShaderHash(vs);
            g_vsMemo.put(vs, g_frame, h);
        }
        vs->Release();
    });
    return h;
}

// The bound pixel shader's hash, the same way; asked only of composites.
uint64_t boundPsHash(ID3D11DeviceContext* ctx) {
    uint64_t h = 0;
    guardedBudget(g_budget, [&] {
        ID3D11PixelShader* ps = nullptr;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        if (!ps) return;
        uint64_t memo = 0;
        if (g_psMemo.get(ps, g_frame, kVsMemoLifeFrames, &memo)) {
            h = memo;
        } else {
            h = lookupShaderHash(ps);
            g_psMemo.put(ps, g_frame, h);
        }
        ps->Release();
    });
    return h;
}

// Is this sampled view over a learned surface? Memoised by view identity;
// a miss resolves the view (guarded, binding_shadow's budget) while it is
// certainly bound.
bool viewIsSurface(const void* view) {
    if (!view) return false;
    uint64_t memo = 0;
    if (g_viewMemo.get(view, g_frame, kMemoLifeFrames, &memo)) return memo != 0;
    ResourceInfo info;
    const bool surface = bindingResolve(const_cast<void*>(view), &info) &&
                         info.isTexture2D && surfaceMatches(info);
    g_viewMemo.put(view, g_frame, surface ? 1u : 0u);
    return surface;
}

// Is the bound depth target the scene pair's? Once per frame per view.
bool dsvIsSceneDepth(const void* dsv) {
    if (dsv == g_dsvJudged && g_dsvJudgedFrame == g_frame) return g_dsvIsScene;
    g_dsvJudged = dsv;
    g_dsvJudgedFrame = g_frame;
    ResourceInfo info;
    g_dsvIsScene = bindingResolve(const_cast<void*>(dsv), &info) && info.isTexture2D &&
                   depthProbeIsSceneDepth(info.resource);
    return g_dsvIsScene;
}

bool exhaustedRecently(const void* res) {
    for (uint32_t i = 0; i < kExhausted; ++i) {
        if (g_exhausted[i].res == res) {
            return g_frame - g_exhausted[i].frame < kExhaustedRearmFrames;
        }
    }
    return false;
}

void noteExhausted(const void* res) {
    for (uint32_t i = 0; i < kExhausted; ++i) {
        if (g_exhausted[i].res == res) {
            g_exhausted[i].frame = g_frame;
            return;
        }
    }
    g_exhausted[g_exhaustedNext].res = const_cast<void*>(res);
    g_exhausted[g_exhaustedNext].frame = g_frame;
    g_exhaustedNext = (g_exhaustedNext + 1) % kExhausted;
}

// A family seen for the first time: one line with its target, the
// visibility the header promises, so a field log can say what was treated.
// Keyed on the OUTCOME as well as the pair. One family takes different
// paths in different places -- the menu panel is treated at the main menu
// and was declined for want of an eye in the cockpit -- and a log that
// names a pair once says only what happened the first time. That is the
// second thing to hide the escape menu, after the vertex-only key
// (2026-09-08). The message is the outcome: `how` is a string literal per
// site, so two sites that merge to one address are saying the same thing.
bool familySeen(uint64_t vh, uint64_t ph, const char* how) {
    for (uint32_t i = 0; i < g_familyLoggedCount; ++i) {
        if (g_familyLoggedVs[i] == vh && g_familyLoggedPs[i] == ph &&
            g_familyLoggedHow[i] == how) {
            return true;
        }
    }
    return false;
}

void noteFamily(uint64_t vh, uint64_t ph, const char* how) {
    if (!vh || g_familyLoggedCount >= kMaxFamilyLines) return;
    if (familySeen(vh, ph, how)) return;
    g_familyLoggedVs[g_familyLoggedCount] = vh;
    g_familyLoggedPs[g_familyLoggedCount] = ph;
    g_familyLoggedHow[g_familyLoggedCount] = how;
    ++g_familyLoggedCount;
    ResourceInfo rt;
    const bool haveRt = bindingResolve(bindingGet(BindSlot::Rtv0), &rt) && rt.isTexture2D;
    Log::get().note("ui depth: a new interface family -- vs %016llX ps %016llX draws "
                    "into %ux%u DXGI format %u: %s.",
                    static_cast<unsigned long long>(vh),
                    static_cast<unsigned long long>(ph),
                    haveRt ? rt.a : 0u, haveRt ? rt.b : 0u, haveRt ? rt.fmt : 0u, how);
}

// The muting twin of the game's depth-stencil state (fix.temporal_aa_particles):
// the same state with its depth write off. Null when the game's writes
// nothing already, or no twin can be made.
ID3D11DepthStencilState* mutingTwin(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* game) {
    for (uint32_t i = 0; i < g_muteCount; ++i) {
        if (g_mutes[i].game == game) return g_mutes[i].ours;
    }
    if (g_muteCount >= kMaxStates) {
        if (!g_mutesFullNoted) {
            g_mutesFullNoted = true;
            Log::get().note("ui depth: more than %u distinct depth states at soft-particle draws; "
                            "the ones past the table keep their depth write.",
                            kMaxStates);
        }
        return nullptr;
    }
    D3D11_DEPTH_STENCIL_DESC d{};
    if (game) {
        game->GetDesc(&d);
    } else {
        // A null state is D3D's default: depth on, LESS, write all.
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_LESS;
        d.StencilEnable = FALSE;
        d.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
        d.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
        const D3D11_DEPTH_STENCILOP_DESC keep = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                                                D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
        d.FrontFace = keep;
        d.BackFace = keep;
    }
    MutePair& m = g_mutes[g_muteCount];
    m.game = game;
    m.ours = nullptr;
    if (game) game->AddRef();
    const bool writes = d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
    if (writes) {
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            if (FAILED(dev->CreateDepthStencilState(&d, &m.ours))) m.ours = nullptr;
            dev->Release();
        }
    }
    ++g_muteCount;
    return m.ours;
}

// Does this eye draw sample one of the depth resources the flight HUD
// samples (learnResolve)? The pixel stage's first four slots, resolved
// through the binding shadow's cache, as the composite test above them.
bool samplesSceneDepth() {
    static const BindSlot kSlots[4] = {BindSlot::PsSrv0, BindSlot::PsSrv1,
                                       BindSlot::PsSrv2, BindSlot::PsSrv3};
    for (int i = 0; i < 4; ++i) {
        void* v = bindingGet(kSlots[i]);
        if (!v) continue;
        ResourceInfo info;
        if (!bindingResolve(v, &info) || !info.resource) continue;
        for (uint32_t r = 0; r < g_resolveCount; ++r) {
            if (g_resolves[r] == info.resource) return true;
        }
    }
    return false;
}

// The flight HUD's t0: the scene's depth, or a resolve of it, as an
// identity for samplesSceneDepth. Learned at the HUD's own draws, a few
// resources a session.
void learnResolve() {
    if (g_resolveCount >= kMaxResolves) return;
    ResourceInfo info;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &info) || !info.resource) return;
    for (uint32_t r = 0; r < g_resolveCount; ++r) {
        if (g_resolves[r] == info.resource) return;
    }
    g_resolves[g_resolveCount++] = info.resource;
    Log::get().note("ui depth: the flight HUD samples the scene's depth at t0 -- %ux%u, DXGI format %u%s; "
                    "a draw into the scene pair that samples it and is not the interface's is a soft "
                    "particle (a ship's trail, its exhaust), and its depth write is %s "
                    "(fix.temporal_aa_particles).",
                    info.a, info.b, info.fmt,
                    depthProbeIsSceneDepth(info.resource) ? " (the scene's own depth texture)"
                                                          : " (a resolve of it)",
                    g_particlesOn ? "muted" : "left as the game writes it (off)");
}

void noteParticleFamily(ID3D11DeviceContext* ctx) {
    const uint64_t ps = boundPsHash(ctx);
    for (uint32_t i = 0; i < g_particleFamilyCount; ++i) {
        if (g_particleFamilies[i].ps == ps) {
            ++g_particleFamilies[i].draws;
            return;
        }
    }
    if (g_particleFamilyCount < kMaxParticleFamilies) {
        g_particleFamilies[g_particleFamilyCount].ps = ps;
        g_particleFamilies[g_particleFamilyCount].draws = 1;
        ++g_particleFamilyCount;
    }
}

enum class TwinWhy { kOk, kAlreadyWrites, kNoTwin };

// The writing twin of the game's depth-stencil state. Null with the reason
// when the game already writes (nothing to do) or no twin can be made (the
// draw stays as the game issued it).
ID3D11DepthStencilState* writingTwin(ID3D11DeviceContext* ctx,
                                     ID3D11DepthStencilState* game, TwinWhy* why) {
    // A null state is D3D's default: depth on, LESS, write all. Writes.
    if (!game) {
        *why = TwinWhy::kAlreadyWrites;
        return nullptr;
    }
    for (uint32_t i = 0; i < g_stateCount; ++i) {
        if (g_states[i].game != game) continue;
        *why = g_states[i].alreadyWrites ? TwinWhy::kAlreadyWrites : TwinWhy::kOk;
        return g_states[i].ours;
    }
    // Room first, then the object: a full table must not create and
    // destroy a state per draw for the session (review finding 10).
    if (g_stateCount >= kMaxStates) {
        if (!g_statesFullNoted) {
            g_statesFullNoted = true;
            Log::get().note("ui depth: more than %u distinct depth states at "
                            "interface draws; the ones past the table are left as "
                            "the game issued them.",
                            kMaxStates);
        }
        *why = TwinWhy::kNoTwin;
        return nullptr;
    }
    D3D11_DEPTH_STENCIL_DESC d{};
    game->GetDesc(&d);
    const bool writes = d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL &&
                        !g_testAlways;
    ID3D11DepthStencilState* ours = nullptr;
    if (!writes) {
        // The game's test kept where it had one (the cockpit occludes its
        // panels); ALWAYS where it had none, or under the instrument.
        // Stencil exactly as the game's.
        if (!d.DepthEnable || g_testAlways) {
            d.DepthEnable = TRUE;
            d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        }
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) {
            *why = TwinWhy::kNoTwin;
            return nullptr;
        }
        const HRESULT hr = dev->CreateDepthStencilState(&d, &ours);
        dev->Release();
        if (FAILED(hr) || !ours) {
            if (!g_createFailedNoted) {
                g_createFailedNoted = true;
                Log::get().note("ui depth: CreateDepthStencilState failed (0x%08lX); "
                                "the draws that need that state stay as the game "
                                "issued them.",
                                static_cast<unsigned long>(hr));
            }
            *why = TwinWhy::kNoTwin;
            return nullptr;
        }
    }
    game->AddRef();
    StatePair& p = g_states[g_stateCount++];
    p.game = game;
    p.ours = ours;
    p.alreadyWrites = writes;
    *why = writes ? TwinWhy::kAlreadyWrites : TwinWhy::kOk;
    return ours;
}

// The re-issue's depth state: the nearer wins, so a ship model in front of
// the loader's screen keeps its depth; stencil left off.
ID3D11DepthStencilState* reissueState(ID3D11DeviceContext* ctx) {
    if (g_reissueDss) return g_reissueDss;
    if (g_reissueDssFailedNoted) return nullptr;
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = g_testAlways ? D3D11_COMPARISON_ALWAYS : D3D11_COMPARISON_GREATER_EQUAL;
    d.StencilEnable = FALSE;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    const HRESULT hr = dev->CreateDepthStencilState(&d, &g_reissueDss);
    dev->Release();
    if (FAILED(hr) || !g_reissueDss) {
        g_reissueDss = nullptr;
        g_reissueDssFailedNoted = true;
        Log::get().note("ui depth: the depth pass's state could not be made "
                        "(0x%08lX); interface-projection composites stay as the "
                        "game issued them.",
                        static_cast<unsigned long>(hr));
        return nullptr;
    }
    return g_reissueDss;
}

DepthShader* compiled(ID3D11DeviceContext* ctx, DepthShader& s) {
    if (!s.shader && !s.tried) {
        s.tried = true;
        s.shader = shaderSwapCompilePs(ctx, s.hlsl, s.len, "main", s.name, nullptr,
                                       "ui depth");
    }
    return s.shader ? &s : nullptr;
}

// The transcription for this draw's pixel stage: the one that names the
// shader, or -- for a variant of a family we know, sampling from the slot
// that transcription reads -- that family's. `slot` is where the classifier
// found the learned surface, or -1 for a draw that samples none.
DepthShader* depthShaderFor(ID3D11DeviceContext* ctx, uint64_t ps, uint64_t vs, int slot) {
    for (DepthShader& s : g_depthShaders) {
        for (const uint64_t named : s.ps) {
            if (named && named == ps) return compiled(ctx, s);
        }
    }
    if (!g_variants || !vs || slot < 0) return nullptr;
    for (DepthShader& s : g_depthShaders) {
        if (s.slot != static_cast<uint32_t>(slot)) continue;
        bool family = false;
        for (const uint64_t named : s.vs) {
            if (named && named == vs) family = true;
        }
        if (!family) continue;
        DepthShader* got = compiled(ctx, s);
        if (!got) return nullptr;
        if (!inList(g_variantLogged, g_variantLoggedCount, ps) &&
            g_variantLoggedCount < kMaxHashes) {
            g_variantLogged[g_variantLoggedCount++] = ps;
            Log::get().note("ui depth: ps %016llX is a variant of the %016llX family "
                            "this build has no transcription of its own for, and it "
                            "takes the interface surface from the slot that family's "
                            "reads (%u), so that one stands in. If its interface "
                            "gains depth where nothing is drawn, this is the draw to "
                            "suspect (advanced.ui_depth_variants = 0 declines it).",
                            static_cast<unsigned long long>(ps),
                            static_cast<unsigned long long>(vs), s.slot);
        }
        return got;
    }
    return nullptr;
}

// The reactive mask for one eye at this size, made on demand.
Mask* maskFor(ID3D11DeviceContext* ctx, int eye, uint32_t w, uint32_t h) {
    if (eye < 0 || eye > 1 || !w || !h) return nullptr;
    Mask& m = g_mask[eye];
    if (m.tex && m.w == w && m.h == h) return &m;
    if (m.rtv) { m.rtv->Release(); m.rtv = nullptr; }
    if (m.tex) { m.tex->Release(); m.tex = nullptr; }
    m.w = 0;
    m.h = 0;
    m.marked = false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &m.tex);
    if (SUCCEEDED(hr) && m.tex) hr = dev->CreateRenderTargetView(m.tex, nullptr, &m.rtv);
    dev->Release();
    if (FAILED(hr) || !m.tex || !m.rtv) {
        if (m.rtv) { m.rtv->Release(); m.rtv = nullptr; }
        if (m.tex) { m.tex->Release(); m.tex = nullptr; }
        if (!g_maskFailedNoted) {
            g_maskFailedNoted = true;
            Log::get().note("ui depth: the %ux%u reactive mask could not be made "
                            "(0x%08lX); the interface's depth is still written and "
                            "nothing is handed to NVIDIA.",
                            w, h, static_cast<unsigned long>(hr));
        }
        return nullptr;
    }
    m.w = w;
    m.h = h;
    return &m;
}

// The mask draw's states: colour written opaquely into the red channel,
// and the game's own depth test kept with writes off, so a panel behind
// the cockpit frame marks nothing where it is hidden.
ID3D11BlendState* maskBlend(ID3D11DeviceContext* ctx) {
    if (g_maskBlend) return g_maskBlend;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
    const HRESULT hr = dev->CreateBlendState(&bd, &g_maskBlend);
    dev->Release();
    if (FAILED(hr)) g_maskBlend = nullptr;
    return g_maskBlend;
}

ID3D11DepthStencilState* maskDepthState(ID3D11DeviceContext* ctx) {
    if (g_maskDss) return g_maskDss;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    d.StencilEnable = FALSE;
    const HRESULT hr = dev->CreateDepthStencilState(&d, &g_maskDss);
    dev->Release();
    if (FAILED(hr)) g_maskDss = nullptr;
    return g_maskDss;
}

// The alpha floor and the reactive strength, in a constant buffer of
// EDVR's at b13 (a slot the game's composites leave empty: their pixel
// stages declare b2 alone).
// ...one buffer per mask offset in use (g_reissueMaskOffset): the interface
// proper at the strength, the families that ride a body's path under it.
ID3D11Buffer* floorBuffer(ID3D11DeviceContext* ctx, int slotIndex, float maskOffset) {
    const float strength = g_reactive > 0.0f ? (g_reactive + maskOffset > 0.0f ? g_reactive + maskOffset : 0.0f)
                                             : 0.0f;
    // Three families with different offsets draw in one frame; each keeps
    // its own buffer rather than trading one back and forth.
    FloorCb& slot = g_floorCbs[slotIndex < 0 ? 0 : (slotIndex > 2 ? 2 : slotIndex)];
    const float nearDepth = temporalPassDepthAt(1.0f);
    if (slot.cb && slot.floor == g_alphaFloor && slot.strength == strength && slot.nearDepth == nearDepth) {
        return slot.cb;
    }
    if (slot.cb) {
        slot.cb->Release();
        slot.cb = nullptr;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    // The mask's value is NVIDIA's reactive strength, and its quantum's
    // PARITY is the temporal pass's word on the body's path (uiCovered
    // there): even rides a turning body where the pixel sits on it, odd
    // keeps the camera's path at the pixel's depth. y is the value a
    // family writes for a stroke drawn AT a surface (the flight HUD's
    // coverage decides per pixel), w for one that floats over the scene;
    // the interface proper (slot 0) floats always, and so do the holo
    // material's markers and the sprite, whose shaders write w. Until
    // 2026-09-09 a BAND of values said which, and every stroke's core
    // fell in the riding band: the station's target brackets rode its
    // spin where they crossed its silhouette and kept the camera's path
    // over the sky beside it -- "shimmering on just one side".
    const int q = static_cast<int>(strength * 255.0f + 0.5f);
    const int qRide = (q & 1) ? q - 1 : q;
    const int qFloat = (q & 1) ? q : q + 1;
    const float ride = static_cast<float>(qRide) / 255.0f;
    const float flt = static_cast<float>(qFloat) / 255.0f;
    const float data[4] = {g_alphaFloor, slotIndex <= 0 ? flt : ride, nearDepth, flt};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(data);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = data;
    const HRESULT hr = dev->CreateBuffer(&bd, &sd, &slot.cb);
    dev->Release();
    if (FAILED(hr)) slot.cb = nullptr;
    slot.floor = g_alphaFloor;
    slot.strength = strength;
    slot.nearDepth = nearDepth;
    return slot.cb;
}

void parseHashes(const std::string& spec, uint64_t* out, uint32_t* count,
                 uint32_t cap, const char* key) {
    *count = 0;
    const char* p = spec.c_str();
    while (*p && *count < cap) {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        if (!*p) break;
        char* end = nullptr;
        const uint64_t h = _strtoui64(p, &end, 16);
        if (end == p || h == 0) {
            Log::get().note("ui depth: %s holds \"%s\", which is not a list of "
                            "sixteen-hex-digit hashes; ignored from there on.",
                            key, p);
            break;
        }
        out[(*count)++] = h;
        p = end;
    }
}

void releaseStates() {
    for (uint32_t i = 0; i < g_stateCount; ++i) {
        if (g_states[i].ours) g_states[i].ours->Release();
        if (g_states[i].game) g_states[i].game->Release();
        g_states[i] = StatePair();
    }
    g_stateCount = 0;
    g_statesFullNoted = false;
    if (g_reissueDss) {
        g_reissueDss->Release();
        g_reissueDss = nullptr;
    }
    g_reissueDssFailedNoted = false;
}

void releasePairViews() {
    for (int e = 0; e < 2; ++e) {
        if (g_pair[e].dsv) g_pair[e].dsv->Release();
        g_pair[e] = PairView();
    }
}

// EDVR's depth view over the pass's texture for this eye, made once per
// texture. Null when there is no pair or the view cannot be made.
ID3D11DepthStencilView* pairViewFor(ID3D11DeviceContext* ctx, int eye, uint32_t w,
                                    uint32_t h) {
    ID3D11Texture2D* tex = nullptr;
    uint32_t fmt = 0;
    if (!depthProbeSceneDepthFormat(w, h, eye, &tex, &fmt) || !tex) return nullptr;
    PairView& p = g_pair[eye];
    if (p.tex == tex && p.dsv) return p.dsv;
    if (p.dsv) {
        p.dsv->Release();
        p.dsv = nullptr;
    }
    p.tex = tex;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC d{};
    d.Format = static_cast<DXGI_FORMAT>(fmt);
    d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipSlice = 0;
    const HRESULT hr = dev->CreateDepthStencilView(tex, &d, &p.dsv);
    dev->Release();
    if (FAILED(hr) || !p.dsv) {
        p.dsv = nullptr;
        if (!g_pairCreateFailedNoted) {
            g_pairCreateFailedNoted = true;
            Log::get().note("ui depth: a depth view over the pass's %ux%u target "
                            "(format %u) could not be made (0x%08lX); composites "
                            "outside the scene's depth stay as the game issued them.",
                            w, h, fmt, static_cast<unsigned long>(hr));
        }
        return nullptr;
    }
    return p.dsv;
}

int eyeIndexFor(const void* rtvRes, uint32_t w, uint32_t h, uint32_t fmt) {
    uint32_t ofShape = 0;
    for (uint32_t i = 0; i < g_frameTargetCount; ++i) {
        const FrameTarget& t = g_frameTargets[i];
        if (t.res == rtvRes) return static_cast<int>(t.eye);
        if (t.w == w && t.h == h && t.fmt == fmt) ++ofShape;
    }
    if (ofShape >= 2) return -1;   // a third target of this shape: no eye
    if (g_frameTargetCount >= kMaxFrameTargets) {
        if (!g_frameTargetsFullNoted) {
            g_frameTargetsFullNoted = true;
            Log::get().note("ui depth: more than %u distinct colour targets among the "
                            "interface draws of one frame; the ones past the table get "
                            "no eye and their depth pass is declined.",
                            kMaxFrameTargets);
        }
        return -1;
    }
    FrameTarget& t = g_frameTargets[g_frameTargetCount++];
    t.res = rtvRes;
    t.w = w;
    t.h = h;
    t.fmt = fmt;
    t.eye = ofShape;
    return static_cast<int>(ofShape);
}

// The viewport depth range that carries the encoding correction, and its
// undoing. Through the context's own entry: vscreen's hook touches only
// inflated FSS targets.
void scaleViewportsForUi(ID3D11DeviceContext* ctx) {
    float sceneNear = 0.0f, sceneFar = 0.0f;
    if (!temporalPassPlanes(&sceneNear, &sceneFar) || !(g_uiNear > 0.0f)) return;
    float scale = sceneNear / g_uiNear;
    if (!(scale > 0.0f)) return;
    if (scale > 1.0f) scale = 1.0f;
    g_savedVpCount = kMaxViewports;
    ctx->RSGetViewports(&g_savedVpCount, g_savedVps);
    if (g_savedVpCount == 0 || g_savedVpCount > kMaxViewports) {
        g_savedVpCount = 0;
        return;
    }
    D3D11_VIEWPORT scaled[kMaxViewports];
    for (UINT i = 0; i < g_savedVpCount; ++i) {
        scaled[i] = g_savedVps[i];
        scaled[i].MaxDepth = scaled[i].MinDepth + (scaled[i].MaxDepth - scaled[i].MinDepth) * scale;
    }
    ctx->RSSetViewports(g_savedVpCount, scaled);
    g_vpScaled = true;
    if (!g_scaleNoted) {
        g_scaleNoted = true;
        Log::get().note("ui depth: an interface-projection composite's depth is written "
                        "through a viewport depth range of %.4f -- the interface "
                        "projection's near %g m against the scene's %g m -- so its "
                        "reversed-Z value decodes in the scene's encoding "
                        "(advanced.ui_depth_planes).",
                        static_cast<double>(scale), static_cast<double>(g_uiNear),
                        static_cast<double>(sceneNear));
    }
}

void restoreViewports(ID3D11DeviceContext* ctx) {
    if (!g_vpScaled) return;
    g_vpScaled = false;
    if (g_savedVpCount) ctx->RSSetViewports(g_savedVpCount, g_savedVps);
    g_savedVpCount = 0;
}

void releaseSavedOm() {
    for (uint32_t i = 0; i < kMaxRtvs; ++i) {
        if (g_savedRtvs[i]) g_savedRtvs[i]->Release();
        g_savedRtvs[i] = nullptr;
    }
    if (g_savedDsv) g_savedDsv->Release();
    g_savedDsv = nullptr;
}

uint32_t savedRtvCount() {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMaxRtvs; ++i) {
        if (g_savedRtvs[i]) n = i + 1;
    }
    return n;
}

void restoreOm(ID3D11DeviceContext* ctx) {
    vScreenSetRenderTargetsRaw(ctx, savedRtvCount(), g_savedRtvs, g_savedDsv);
}

}  // namespace

void uiDepthConfigure(Config& cfg) {
    const std::string mode = cfg.getString("fix.ui_depth", "on");
    g_keyOn = mode == "on";
    if (!g_keyOn && mode != "off") {
        Log::get().note("ui depth: fix.ui_depth = \"%s\" is not on or off; off.",
                        mode.c_str());
    }
    // The gate: nothing to register without the pass. Re-read on every
    // configure because fix.temporal_aa is live (depth_probe.cpp's rule).
    const std::string aa = cfg.getString("fix.temporal_aa", "off");
    g_passOn = !aa.empty() && _stricmp(aa.c_str(), "off") != 0;
    // Only NVIDIA's history reads a bias mask; the pass's own does not, so
    // marking one under temporal_aa = on would be draws for nothing.
    g_trained = _stricmp(aa.c_str(), "dlaa") == 0 || _stricmp(aa.c_str(), "dlss") == 0;
    g_particlesOn = cfg.getBool("fix.temporal_aa_particles", true);
    // The direct list: the flight HUD built in, the ini's additions after.
    g_familyCount = 0;
    g_families[g_familyCount++] = kFlightHud;
    uint64_t extra[kMaxHashes];
    uint32_t extraCount = 0;
    parseHashes(cfg.getString("advanced.ui_depth_families", ""), extra, &extraCount,
                kMaxHashes, "advanced.ui_depth_families");
    for (uint32_t i = 0; i < extraCount && g_familyCount < kMaxHashes; ++i) {
        if (!inList(g_families, g_familyCount, extra[i])) g_families[g_familyCount++] = extra[i];
    }
    // The exclude list: the null-shader mesh built in, the ini's after.
    g_excludeCount = 0;
    g_exclude[g_excludeCount++] = kNullPsMesh;
    parseHashes(cfg.getString("advanced.ui_depth_exclude", ""), extra, &extraCount,
                kMaxHashes, "advanced.ui_depth_exclude");
    for (uint32_t i = 0; i < extraCount && g_excludeCount < kMaxHashes; ++i) {
        if (!inList(g_exclude, g_excludeCount, extra[i])) g_exclude[g_excludeCount++] = extra[i];
    }
    {
        // The interface projection's planes: "near, far" in metres.
        const std::string planes = cfg.getString("advanced.ui_depth_planes", "0.1, 1000");
        float n = 0.0f, f = 0.0f;
        char* end = nullptr;
        n = static_cast<float>(strtod(planes.c_str(), &end));
        while (end && (*end == ' ' || *end == ',' || *end == '\t')) ++end;
        if (end && *end) f = static_cast<float>(strtod(end, nullptr));
        if (n > 0.0f && f > n) {
            if (n != g_uiNear || f != g_uiFar) {
                g_uiNear = n;
                g_uiFar = f;
                g_scaleNoted = false;
            }
        } else if (planes != "0.1, 1000") {
            Log::get().note("ui depth: advanced.ui_depth_planes = \"%s\" is not "
                            "\"near, far\" in metres with far beyond near; the "
                            "interface projection is taken as 0.1..1000 m.",
                            planes.c_str());
            g_uiNear = 0.1f;
            g_uiFar = 1000.0f;
        }
    }
    {
        float a = cfg.getFloat("advanced.ui_depth_alpha", 0.5f);
        if (!(a >= 0.0f)) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        g_alphaFloor = a;
    }
    {
        float r = cfg.getFloat("advanced.ui_depth_reactive", 0.5f);
        if (!(r >= 0.0f)) r = 0.0f;
        if (r > 1.0f) r = 1.0f;
        if (r != g_reactive) {
            const bool was = g_reactive > 0.0f;
            g_reactive = r;
            if (r > 0.0f) {
                Log::get().note("ui depth: the interface is marked for NVIDIA at "
                                "strength %.2f -- where it is marked, this frame's "
                                "colour is favoured over the history, so a readout "
                                "that changes in place stops blending with the digit "
                                "before it. 1 stops the interface accumulating "
                                "altogether, which is sharp and shimmering; 0 is the "
                                "steady interface that blurs a changing digit.",
                                static_cast<double>(r));
            } else if (was) {
                Log::get().note("ui depth: the interface is no longer marked for "
                                "NVIDIA (advanced.ui_depth_reactive = 0); it "
                                "accumulates as the rest of the frame does.");
            }
        }
    }
    const std::string eyes = cfg.getString("advanced.ui_depth_eyes", "as_is");
    const bool swapped = eyes == "swapped";
    if (swapped != g_eyesSwapped) {
        g_eyesSwapped = swapped;
        Log::get().note("ui depth: the eye a rebound composite belongs to is %s.",
                        swapped ? "the REVERSE of its colour target's order in the "
                                  "frame (advanced.ui_depth_eyes = swapped)"
                                : "its colour target's order in the frame, first "
                                  "= left");
    }
    const bool menus = cfg.getBool("advanced.ui_depth_menus", true);
    if (menus != g_menus) {
        g_menus = menus;
        Log::get().note("ui depth: interface-projection composites (the menus, the "
                        "loading screen, the modals) %s.",
                        menus ? "get the alpha-aware depth pass"
                              : "are left alone (advanced.ui_depth_menus = 0)");
    }
    const bool variants = cfg.getBool("advanced.ui_depth_variants", true);
    if (variants != g_variants) {
        g_variants = variants;
        Log::get().note("ui depth: a pixel shader this build has no transcription "
                        "for %s.",
                        variants ? "is drawn by its vertex family's, when that one "
                                   "reads the slot the surface is in"
                                 : "is left alone (advanced.ui_depth_variants = 0)");
    }
    // The test instrument changes what a twin is, so the twins are remade.
    const std::string test = cfg.getString("advanced.ui_depth_test", "as_is");
    const bool always = test == "always";
    if (always != g_testAlways) {
        g_testAlways = always;
        releaseStates();
        Log::get().note("ui depth: the depth test at interface draws is %s.",
                        always ? "ALWAYS (advanced.ui_depth_test = always: the "
                                 "ordering A/B; the cockpit no longer occludes a panel)"
                               : "the game's own");
    }

    const bool was = g_on;
    g_on = g_keyOn && g_passOn;
    if (g_on && !was) {
        g_announced = false;
        g_waitingNoted = false;
        resetWindow();
        Log::get().note("ui depth: ON -- the interface's composites and the flight HUD "
                        "will write their depth for the temporal pass (%u direct "
                        "famil%s, %u excluded, alpha floor %.2f). It says so again "
                        "when the first frame writes.",
                        g_familyCount, g_familyCount == 1 ? "y" : "ies",
                        g_excludeCount, static_cast<double>(g_alphaFloor));
    } else if (!g_on && was) {
        Log::get().note("ui depth: off%s. The interface draws as the game issues it.",
                        g_keyOn ? " while temporal_aa is off" : "");
    } else if (g_keyOn && !g_passOn && !g_waitingNoted) {
        g_waitingNoted = true;
        Log::get().note("ui depth: on, but temporal_aa is off, so it waits -- there "
                        "is nothing to register without the pass, and the game's "
                        "depth is left exactly as it is.");
    }
    // Deliberately NOT re-arming after a stand-down: the budget that stood
    // it down is spent for the session (review finding 15).
}

bool uiDepthWantsDraws() { return g_on && !g_stoodDown; }

float uiDepthReactive() { return g_on && !g_stoodDown ? g_reactive : 0.0f; }

void uiDepthNoteOffscreenDraw(ID3D11DeviceContext* ctx) {
    if (!g_on || g_stoodDown) return;
    const uint32_t gen = bindingGeneration(BindSlot::Rtv0);
    if (gen != g_rtvGen) {
        g_rtvGen = gen;
        g_rtvKnown = false;
        g_rtvChecks = 0;
        g_rtvRes = nullptr;
        ResourceInfo info;
        if (bindingResolve(bindingGet(BindSlot::Rtv0), &info) && info.isTexture2D) {
            g_rtvRes = info.resource;
            g_rtvW = info.a;
            g_rtvH = info.b;
            g_rtvFmt = info.fmt;
            g_rtvKnown = surfaceMatches(info);
            if (!g_rtvKnown && exhaustedRecently(g_rtvRes)) g_rtvChecks = kChecksPerTarget;
        } else {
            g_rtvChecks = kChecksPerTarget;   // nothing here to learn
        }
    }
    if (g_rtvKnown || !g_rtvRes || g_rtvChecks >= kChecksPerTarget) return;
    ++g_rtvChecks;
    const uint64_t h = boundVsHash(ctx);
    if (h == kGuiVector || h == kGuiText || h == kGuiIcons) {
        addSurface(g_rtvRes, g_rtvW, g_rtvH, g_rtvFmt);
        g_rtvKnown = true;
    } else if (g_rtvChecks >= kChecksPerTarget) {
        noteExhausted(g_rtvRes);
    }
}

bool uiDepthOnEyeDraw(ID3D11DeviceContext* ctx) {
    g_mode = Mode::kNone;
    g_wantRebind = false;
    g_wantMask = false;
    g_rebindEye = -1;
    g_drawEye = -1;
    g_reissueShader = nullptr;
    g_reissueMaskOffset = 0.0f;   // the interface proper unless the family below says otherwise
    g_reissueMaskSlot = 0;
    if (!g_on || g_stoodDown) return false;
    // Cheapest first: no depth target, nothing to write (the post chain's
    // fullscreen draws, ten a frame).
    const void* dsv = bindingGet(BindSlot::Dsv0);
    if (!dsv) {
        ++g_wDepthless;
        return false;
    }
    // WHICH SLOT held the learned surface, not just whether one did: a
    // transcription reads a named register, so the slot is what says
    // whether it can stand in for a pixel shader it does not name.
    int surfaceSlot = -1;
    static const BindSlot kSlots[4] = {BindSlot::PsSrv0, BindSlot::PsSrv1,
                                       BindSlot::PsSrv2, BindSlot::PsSrv3};
    for (int i = 0; i < 4; ++i) {
        if (viewIsSurface(bindingGet(kSlots[i]))) {
            surfaceSlot = i;
            break;
        }
    }
    const bool composite = surfaceSlot >= 0;
    // The hash: for a composite, the family line and the exclude list
    // (a couple of dozen a frame); otherwise the direct list, which is the
    // only test left for the other draws.
    const uint64_t h = boundVsHash(ctx);
    if (!composite && !(h && inList(g_families, g_familyCount, h))) {
        // Not the interface's. A soft particle's draw, if it samples the
        // scene's depth into the scene pair (g_particlesOn says why): its
        // depth write is muted around the draw (uiDepthBegin).
        if (g_particlesOn && g_resolveCount && dsvIsSceneDepth(dsv) && samplesSceneDepth()) {
            g_mode = Mode::kMuteDepth;
            noteParticleFamily(ctx);
            ++g_wParticles;
            ++g_sessionParticles;
            return true;
        }
        return false;
    }
    if (h && inList(g_exclude, g_excludeCount, h)) return false;

    // WHICH PROJECTION. The cockpit's families bind the scene pair and
    // test against it, so they share the scene's projection and write
    // depth in place. A composite that does not bind the pair is drawn
    // through the interface projection (the menus, the loader, the modals;
    // measured 2026-09-07) and gets the alpha-aware depth pass instead,
    // whichever depth it binds.
    const bool scenePair = dsvIsSceneDepth(dsv);
    const bool sceneFamily = h == kHoloPanel || h == kHudSprite ||
                             inList(g_families, g_familyCount, h);
    if (scenePair && h == kFlightHud) learnResolve();
    if (scenePair && sceneFamily) {
        g_mode = Mode::kInPlace;
        const bool wantLine = g_familyLoggedCount < kMaxFamilyLines;
        const bool wantMask = g_reactive > 0.0f && g_trained;
        // The flight HUD's depth goes through the second draw with its
        // coverage shader (kHudDepthHlsl says why), so its pixel stage is
        // always looked up; the others only when the mask or the line
        // wants it.
        // The holo material goes the same way since 2026-09-09: it draws
        // the cockpit's panels AND the target markers (instanced from the
        // pool at the target), and its alpha carries an eight-tap smear
        // past every stroke -- a glow above the game's own discard at
        // 1e-5 -- so in place, under the writing twin, each marker corner
        // wrote its depth over the station around it, and the station
        // there reprojected as a point at the marker's depth near the axis,
        // which barely moves: "small blurry quads under each of the four
        // brackets". The coverage stand-in (kHoloDepthHlsl) takes the
        // surface's own alpha at the floor: the strokes, not the glow.
        // ...and the target-time sprite family (kHudSprite), which appears
        // in the log within a second of a target being taken: in place, its
        // own draw's alpha discard let its soft fringe write the target's
        // depth over the station (2026-09-09). All three ride a turning
        // body's path (g_reissueMaskOffset says how).
        const bool hud = h == kFlightHud || h == kHoloPanel || h == kHudSprite;
        // All three ride a turning body's path where their pixels sit on it
        // (a stroke drawn AT the surface -- the docking hologram over the
        // hub's drum, 2026-09-09 08:03 -- turns with it); a stroke's core
        // that sits near, the reticle's, reconstructs outside the body's
        // cells and takes the camera's path whatever the mask says. Their
        // glow is not marked at all now (kHudDepthHlsl), which is what
        // settled the reticle's 'swim' and the drum's smear both. The flight
        // HUD's strokes are marked at HALF the strength: they draw no text
        // that changes in place, their motion is the scene's own since
        // 2026-09-09, and at the full strength's half-fresh history the
        // target indicator was "not as steady/solid as it should be".
        // Which pixels ride is no longer the family's band but the mask
        // value's parity (floorBuffer): a flight HUD core drawn at the
        // surface rides, and everything that floats -- the holo material's
        // markers, the sprite, a floating core -- keeps the camera's path.
        g_reissueMaskSlot = h == kFlightHud ? 2 : (hud ? 1 : 0);
        g_reissueMaskOffset = h == kFlightHud ? -0.5f * g_reactive : (hud ? -3.0f / 255.0f : 0.0f);
        const uint64_t ph = (hud || wantMask || wantLine) ? boundPsHash(ctx) : 0;
        DepthShader* shader = (hud || wantMask) ? depthShaderFor(ctx, ph, h, surfaceSlot) : nullptr;
        if (hud && shader) {
            g_mode = Mode::kReissueScene;
            g_reissueShader = shader;
        }
        // The reactive mask, when one is asked for and this family's pixel
        // stage has a coverage shader: the second draw marks it. For the
        // in-place families the depth is already written by the game's own
        // draw; for the flight HUD the same second draw writes it.
        if (shader && (wantMask || g_mode == Mode::kReissueScene)) {
            ResourceInfo rt;
            if (bindingResolve(bindingGet(BindSlot::Rtv0), &rt) && rt.isTexture2D) {
                int eye = eyeIndexFor(rt.resource, rt.a, rt.b, rt.fmt);
                if (eye >= 0 && g_eyesSwapped) eye = 1 - eye;
                if (eye >= 0) {
                    g_wantMask = wantMask;
                    g_reissueShader = shader;
                    g_drawEye = eye;
                    g_rebindW = rt.a;
                    g_rebindH = rt.b;
                }
            }
        }
        if (wantLine) {
            noteFamily(h, ph,
                       g_mode == Mode::kReissueScene
                           ? (h == kFlightHud
                                  ? "the flight HUD; its depth written by the coverage pass in the "
                                    "scene's projection, under its strokes' cores alone (the glow "
                                    "keeps the scene's); its pixels ride a turning body's path where "
                                    "they sit on it"
                              : h == kHudSprite
                                  ? "the target-time sprite; its depth written by the coverage pass in "
                                    "the scene's projection, under its opaque core and not its fringe; "
                                    "its pixels ride a turning body's path"
                                  : "the holo material (the cockpit's panels, the target markers); "
                                    "its depth written by the coverage pass in the scene's "
                                    "projection, under the surface's strokes and not the glow; its "
                                    "pixels ride a turning body's path")
                       : composite ? "samples a learned surface; writes its depth in "
                                     "place, the scene's own encoding"
                                   : "named direct family; writes its depth in place");
        }
        if (composite) {
            ++g_wComposite;
        } else {
            ++g_wDirect;
        }
        return true;
    }
    if (!composite) return false;   // a direct family off the scene pair: left alone
    if (!g_menus) {
        ++g_wNotScene;
        return false;
    }
    // The alpha-aware pass needs a shader for this family's pixel stage,
    // the scene's planes for the encoding, and the pass's depth for the
    // eye when the composite's own is not it.
    const uint64_t ph = boundPsHash(ctx);
    DepthShader* shader = depthShaderFor(ctx, ph, h, surfaceSlot);
    if (!shader) {
        ++g_wNoShader;
        noteFamily(h, ph, "samples a learned surface but has no depth shader of "
                          "its own yet, and none of this build's stands in; "
                          "left alone -- this composite still swims");
        return false;
    }
    // Every decline below counts as "no pair or planes" in the totals, and
    // each says which once: a totals line reporting draws left alone with
    // nothing naming them is what hid the escape menu (2026-09-08).
    float sn = 0.0f, sf = 0.0f;
    if (!temporalPassPlanes(&sn, &sf)) {
        ++g_wNoPair;
        noteFamily(h, ph, "drawn through the interface projection, but the pass "
                          "has published no scene planes yet; left alone");
        return false;
    }
    if (!scenePair) {
        ResourceInfo rt;
        if (!bindingResolve(bindingGet(BindSlot::Rtv0), &rt) || !rt.isTexture2D) {
            ++g_wNoPair;
            noteFamily(h, ph, "drawn through the interface projection into "
                              "something that is not a 2D colour target; left alone");
            return false;
        }
        int eye = eyeIndexFor(rt.resource, rt.a, rt.b, rt.fmt);
        if (eye >= 0 && g_eyesSwapped) eye = 1 - eye;
        ID3D11Texture2D* tex = nullptr;
        uint32_t fmt = 0;
        if (eye < 0 || !depthProbeSceneDepthFormat(rt.a, rt.b, eye, &tex, &fmt)) {
            ++g_wNoPair;
            noteFamily(h, ph,
                       eye < 0 ? "drawn through the interface projection into a "
                                 "target that is not one of the eyes; left alone"
                               : "drawn through the interface projection into an "
                                 "eye the pass has no depth of this size for; "
                                 "left alone");
            return false;
        }
        g_wantRebind = true;
        g_rebindEye = eye;
        g_drawEye = eye;
        g_rebindW = rt.a;
        g_rebindH = rt.b;
    } else {
        ResourceInfo rt;
        if (bindingResolve(bindingGet(BindSlot::Rtv0), &rt) && rt.isTexture2D) {
            int eye = eyeIndexFor(rt.resource, rt.a, rt.b, rt.fmt);
            if (eye >= 0 && g_eyesSwapped) eye = 1 - eye;
            g_drawEye = eye;
            g_rebindW = rt.a;
            g_rebindH = rt.b;
        }
    }
    g_wantMask = g_reactive > 0.0f && g_trained && g_drawEye >= 0;
    g_mode = Mode::kReissue;
    g_reissueShader = shader;
    noteFamily(h, ph, scenePair ? "interface projection; its depth written by the "
                                  "alpha-aware pass into its own target, the pass's"
                                : "interface projection; its depth written by the "
                                  "alpha-aware pass into the pass's target for its eye");
    ++g_wComposite;
    return true;
}

bool uiDepthWantsReissue() {
    if (!g_reissueShader) return false;
    // The interface-projection composites always want it (their depth is
    // written by it), and so does the flight HUD in the scene's projection;
    // the in-place families only when a mask is asked for.
    return g_mode == Mode::kReissue || g_mode == Mode::kReissueScene ||
           (g_mode == Mode::kInPlace && g_wantMask);
}

void uiDepthBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (!g_on || g_stoodDown || (g_mode != Mode::kInPlace && g_mode != Mode::kMuteDepth)) return;
    const bool ran = guardedBudget(g_budget, [&] {
        ID3D11DepthStencilState* game = nullptr;
        UINT ref = 0;
        ctx->OMGetDepthStencilState(&game, &ref);
        ID3D11DepthStencilState* ours = nullptr;
        if (g_mode == Mode::kMuteDepth) {
            // A soft particle's draw: the game's state with its depth write
            // off (mutingTwin). Null when it writes nothing already.
            ours = mutingTwin(ctx, game);
            if (!ours) {
                if (game) game->Release();
                return;
            }
        } else {
            TwinWhy why = TwinWhy::kNoTwin;
            ours = writingTwin(ctx, game, &why);
            if (!ours) {
                if (game) game->Release();
                if (why == TwinWhy::kAlreadyWrites) {
                    ++g_wAlreadyWrote;
                } else {
                    ++g_wNoTwin;
                }
                return;
            }
        }
        g_savedDss = game;   // the AddRef from OMGet, released at End
        g_savedRef = ref;
        ctx->OMSetDepthStencilState(ours, ref);
        g_engaged = true;
        if (g_mode == Mode::kInPlace) {
            ++g_wWrote;
            ++g_sessionWrote;
        }
    });
    if (!ran && !g_budget.shouldRun() && !g_stoodDown) {
        g_stoodDown = true;
        Log::get().note("ui depth: STANDING DOWN for the session -- the depth "
                        "state swap faulted repeatedly. The interface draws as "
                        "the game issues it.");
    }
}

void uiDepthEnd(ID3D11DeviceContext* ctx) {
    // Under SEH but NOT under the budget: a budget spent between Begin and
    // End would skip a guardedBudget body, and a restore skipped leaves our
    // writing twin on every later draw in the frame (review finding 1;
    // resolve_probe.cpp restores the same way for the same reason).
    if (g_engaged) {
        g_engaged = false;
        guarded("uiDepth.restore", [&] { ctx->OMSetDepthStencilState(g_savedDss, g_savedRef); });
        if (g_savedDss) {
            g_savedDss->Release();
            g_savedDss = nullptr;
        }
    }
    g_mode = Mode::kNone;
}

// True when the second draw is set up to write DEPTH, the reactive mask,
// or both -- never colour. False means this call declined, and the caller
// must NOT issue the draw: every decline leaves the game's own state
// exactly as it was, so a draw issued anyway is the game's composite a
// second time, in full colour, over itself. The paths that decline -- no
// depth-stencil state or constant buffer, no pair view to rebind to, no
// mask for a draw that wanted only a mask -- are latched by their own
// one-shot notes, so once one starts failing it fails for every composite
// after, and the doubling would last the session (the pre-release review
// of 2026-09-07). splashDimBegin below has taken this shape all along.
bool uiDepthReissueBegin(ID3D11DeviceContext* ctx) {
    g_reissueOn = false;
    g_rebound = false;
    if (!g_on || g_stoodDown || !g_reissueShader) return false;
    const bool depthPass = g_mode == Mode::kReissue || g_mode == Mode::kReissueScene;   // this draw writes depth
    const bool sceneProjection = g_mode == Mode::kReissueScene;   // ...in the scene's own viewport
    const bool wantMask = g_wantMask;
    const bool rebind = g_wantRebind;
    g_wantRebind = false;
    g_wantMask = false;
    if (!depthPass && !wantMask) return false;
    DepthShader* shader = g_reissueShader;
    const bool ran = guardedBudget(g_budget, [&] {
        // The depth pass writes depth with the nearer-wins test; a
        // mask-only pass over a family whose depth is already written just
        // marks, with the same test and no writes.
        ID3D11DepthStencilState* dss = depthPass ? reissueState(ctx) : maskDepthState(ctx);
        ID3D11Buffer* cb = floorBuffer(ctx, g_reissueMaskSlot, g_reissueMaskOffset);
        if (!dss || !cb) {
            ++g_wNoTwin;
            return;
        }
        Mask* mask = wantMask ? maskFor(ctx, g_drawEye, g_rebindW, g_rebindH) : nullptr;
        if (wantMask && !mask && !depthPass) return;   // nothing left to do
        ctx->OMGetRenderTargets(kMaxRtvs, g_savedRtvs, &g_savedDsv);
        ID3D11DepthStencilView* target = g_savedDsv;
        if (rebind) {
            target = pairViewFor(ctx, g_rebindEye, g_rebindW, g_rebindH);
            if (!target) {
                releaseSavedOm();
                ++g_wNoPair;
                return;
            }
            ++g_wRebound;
            if (!g_rebindNoted) {
                g_rebindNoted = true;
                Log::get().note("ui depth: a composite whose own depth is not the pass's "
                                "has its depth pass bound to the pass's %ux%u depth "
                                "(eye %d by the order its colour target appeared this "
                                "frame among targets of ITS OWN size and format); the "
                                "game's bindings are put back after.",
                                g_rebindW, g_rebindH, g_rebindEye);
            }
        }
        // The mask as the only colour target when one is wanted, none
        // otherwise; through the original entry so the binding shadow keeps
        // describing the game's bindings.
        ID3D11RenderTargetView* rtv = mask ? mask->rtv : nullptr;
        vScreenSetRenderTargetsRaw(ctx, mask ? 1 : 0, mask ? &rtv : nullptr, target);
        g_rebound = true;
        if (mask) {
            mask->marked = true;
            ++g_wMarked;
            ctx->OMGetBlendState(&g_reSavedBlend, g_reSavedBlendFactor, &g_reSavedSampleMask);
            ID3D11BlendState* bs = maskBlend(ctx);
            const FLOAT one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            if (bs) ctx->OMSetBlendState(bs, one, 0xFFFFFFFFu);
            if (!g_maskNotedOnce) {
                g_maskNotedOnce = true;
                Log::get().note("ui depth: the reactive mask is being marked at "
                                "%ux%u for eye %d at strength %.2f -- where it is "
                                "set, NVIDIA favours this frame's colour, so a "
                                "readout that changes in place stops blending with "
                                "the digit before it (advanced.ui_depth_reactive).",
                                g_rebindW, g_rebindH, g_drawEye,
                                static_cast<double>(g_reactive));
            }
        }
        ctx->OMGetDepthStencilState(&g_reSavedDss, &g_reSavedRef);
        ctx->OMSetDepthStencilState(dss, 0);
        ctx->PSGetShader(&g_savedPs, nullptr, nullptr);
        ctx->PSSetShader(shader->shader, nullptr, 0);
        ctx->PSSetConstantBuffers(13, 1, &cb);
        if (depthPass && !sceneProjection) scaleViewportsForUi(ctx);
        g_reissueOn = true;
        if (depthPass) {
            ++g_wReissued;
            ++g_wWrote;
            ++g_sessionWrote;
        }
    });
    if (!ran && !g_budget.shouldRun() && !g_stoodDown) {
        g_stoodDown = true;
        Log::get().note("ui depth: STANDING DOWN for the session -- the depth pass "
                        "faulted repeatedly. The interface draws as the game issues it.");
    }
    if (!ran && g_rebound) {
        // A fault after the rebind: everything back now rather than at End,
        // which the caller still calls.
        guarded("uiDepth.reissueRestore", [&] {
            restoreViewports(ctx);
            restoreOm(ctx);
        });
        releaseSavedOm();
        g_rebound = false;
        g_reissueOn = false;
    }
    return g_reissueOn;
}

void uiDepthReissueEnd(ID3D11DeviceContext* ctx) {
    if (g_reissueOn) {
        g_reissueOn = false;
        guarded("uiDepth.reissueRestore", [&] {
            ctx->PSSetShader(g_savedPs, nullptr, 0);
            ID3D11Buffer* none = nullptr;
            ctx->PSSetConstantBuffers(13, 1, &none);
            ctx->OMSetDepthStencilState(g_reSavedDss, g_reSavedRef);
            if (g_reSavedBlend || g_reSavedSampleMask) {
                ctx->OMSetBlendState(g_reSavedBlend, g_reSavedBlendFactor,
                                     g_reSavedSampleMask);
            }
            restoreViewports(ctx);
        });
        if (g_savedPs) {
            g_savedPs->Release();
            g_savedPs = nullptr;
        }
        if (g_reSavedDss) {
            g_reSavedDss->Release();
            g_reSavedDss = nullptr;
        }
        if (g_reSavedBlend) {
            g_reSavedBlend->Release();
            g_reSavedBlend = nullptr;
        }
        g_reSavedSampleMask = 0;
    }
    if (g_rebound) {
        g_rebound = false;
        guarded("uiDepth.reissueRestore", [&] { restoreOm(ctx); });
        releaseSavedOm();
    }
    g_mode = Mode::kNone;
    g_reissueShader = nullptr;
}

bool uiDepthReactiveMask(uint32_t w, uint32_t h, int eye, ID3D11Texture2D** tex) {
    if (!tex) return false;
    *tex = nullptr;
    if (!g_on || g_stoodDown || !(g_reactive > 0.0f) || eye < 0 || eye > 1) return false;
    Mask& m = g_mask[eye];
    if (!m.tex || !m.marked) return false;
    if (m.w != w || m.h != h) {
        // The pass treats a region of the submitted texture; a mask drawn
        // at another size cannot be handed over as it is. Said once.
        if (!g_maskSizeNoted) {
            g_maskSizeNoted = true;
            Log::get().note("ui depth: the reactive mask is %ux%u but the pass treats "
                            "%ux%u, so it is not handed to NVIDIA. The interface's "
                            "depth is unaffected; this is the cull guard's crop or a "
                            "size change.",
                            m.w, m.h, w, h);
        }
        return false;
    }
    *tex = m.tex;
    return true;
}

void uiDepthFrameBoundary(ID3D11DeviceContext* ctx) {
    ++g_frame;
    g_frameTargetCount = 0;
    // The masks are marked during the frame and read at its submits, so
    // the clear belongs here, after both.
    if (ctx) {
        for (Mask& m : g_mask) {
            if (!m.rtv || !m.marked) continue;
            const FLOAT zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            guardedBudget(g_budget, [&] { ctx->ClearRenderTargetView(m.rtv, zero); });
            m.marked = false;
        }
    }
    if (!g_on) return;
    ++g_wFrames;
    if (!g_announced && g_wWrote > 0) {
        g_announced = true;
        Log::get().note("ui depth: engaged -- %u interface draws wrote their depth "
                        "this window (%u composites of %u learned surfaces, %u "
                        "direct, %u through the alpha-aware pass), the game's depth "
                        "test kept where it had one.",
                        g_wWrote, g_wComposite, g_surfaceCount, g_wDirect, g_wReissued);
    }
    if (g_wFrames >= kTotalsFrames) {
        if (g_wParticles) {
            char fam[400];
            size_t used = 0;
            fam[0] = 0;
            for (uint32_t i = 0; i < g_particleFamilyCount && used + 40 < sizeof(fam); ++i) {
                const int m = snprintf(fam + used, sizeof(fam) - used, "%sps %016llX x %.1f",
                                       i ? ", " : "",
                                       static_cast<unsigned long long>(g_particleFamilies[i].ps),
                                       static_cast<double>(g_particleFamilies[i].draws) / g_wFrames);
                if (m < 0) break;
                used += static_cast<size_t>(m);
            }
            Log::get().note("ui depth: %.1f soft-particle draws a frame had their depth write muted "
                            "(fix.temporal_aa_particles; %llu this session), by pixel shader a frame: %s.",
                            static_cast<double>(g_wParticles) / g_wFrames,
                            static_cast<unsigned long long>(g_sessionParticles), fam[0] ? fam : "none");
            g_wParticles = 0;
            g_particleFamilyCount = 0;
        }
        if (g_wWrote || g_wNotScene || g_wRebound || g_wNoPair || g_wNoShader ||
            g_wNoTwin || g_wLearned || g_evictions) {
            Log::get().note("ui depth totals: %.1f interface draws a frame wrote depth "
                            "(%.1f composites, %.1f direct; %.1f through the alpha-aware "
                            "pass, %.1f of those bound to the pass's depth); %u surfaces "
                            "known, %u learned this window, %u forgotten; left alone: "
                            "%.1f a frame with no depth shader for their family, %.1f "
                            "with no pair or planes, %.1f by the menus switch, %u already "
                            "writing, %u with no state; %u depth states; %llu written "
                            "this session.",
                            static_cast<double>(g_wWrote) / g_wFrames,
                            static_cast<double>(g_wComposite) / g_wFrames,
                            static_cast<double>(g_wDirect) / g_wFrames,
                            static_cast<double>(g_wReissued) / g_wFrames,
                            static_cast<double>(g_wRebound) / g_wFrames,
                            g_surfaceCount, g_wLearned, g_evictions,
                            static_cast<double>(g_wNoShader) / g_wFrames,
                            static_cast<double>(g_wNoPair) / g_wFrames,
                            static_cast<double>(g_wNotScene) / g_wFrames,
                            g_wAlreadyWrote, g_wNoTwin, g_stateCount,
                            static_cast<unsigned long long>(g_sessionWrote));
        }
        resetWindow();
    }
}

void uiDepthShutdown() {
    for (uint32_t i = 0; i < g_muteCount; ++i) {
        if (g_mutes[i].game) g_mutes[i].game->Release();
        if (g_mutes[i].ours) g_mutes[i].ours->Release();
        g_mutes[i] = MutePair();
    }
    g_muteCount = 0;
    g_resolveCount = 0;
    if (g_savedDss) {
        g_savedDss->Release();
        g_savedDss = nullptr;
    }
    if (g_savedPs) {
        g_savedPs->Release();
        g_savedPs = nullptr;
    }
    if (g_reSavedDss) {
        g_reSavedDss->Release();
        g_reSavedDss = nullptr;
    }
    if (g_reSavedBlend) {
        g_reSavedBlend->Release();
        g_reSavedBlend = nullptr;
    }
    g_engaged = false;
    g_reissueOn = false;
    g_rebound = false;
    g_mode = Mode::kNone;
    releaseSavedOm();
    releasePairViews();
    releaseStates();
    for (Mask& m : g_mask) {
        if (m.rtv) m.rtv->Release();
        if (m.tex) m.tex->Release();
        m = Mask();
    }
    if (g_maskBlend) {
        g_maskBlend->Release();
        g_maskBlend = nullptr;
    }
    if (g_maskDss) {
        g_maskDss->Release();
        g_maskDss = nullptr;
    }
    for (DepthShader& s : g_depthShaders) {
        if (s.shader) s.shader->Release();
        s.shader = nullptr;
        s.tried = false;
    }
    for (FloorCb& f : g_floorCbs) {
        if (f.cb) {
            f.cb->Release();
            f.cb = nullptr;
        }
    }
    g_surfaceCount = 0;
    g_surfaceNext = 0;
    g_viewMemo.clear();
    g_vsMemo.clear();
    g_psMemo.clear();
    for (uint32_t i = 0; i < kExhausted; ++i) g_exhausted[i] = Exhausted();
    g_familyLoggedCount = 0;
    g_on = false;
}

}  // namespace edvr
