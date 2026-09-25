#include "ui_depth.h"
#include "ui_depth_layer.h"
#include "stellar_coverage.h"
#include "planet_motion.h"
#include "gpu_interval.h"
#include "ui_content.h"
#include "holo_material.h"

#include <windows.h>

#include <d3d11.h>

#include <algorithm>  // std::sort: the hologram depth census's pixel-count p50
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstdlib>   // _strtoui64, strtod: the hash lists and the planes
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/temporal_mode.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "depth_probe.h"    // depthProbeIsSceneDepth, ...Format: where the pass reads
#include "exposure_fix.h"   // lookupShaderHash
#include "object_probe.h"   // objectProbeLedgerActive: the UI content census's eye-run signal
#include "shader_swap.h"    // shaderSwapCompilePs: the alpha-aware depth shaders
#include "temporal_pass.h"  // temporalPassPlanes: the scene's encoding
#include "vscreen.h"        // vScreenSetRenderTargetsRaw: the rebind past the shadow

namespace edvr {

// The state ui_depth.h reads without a call: the mode, the two arming bools
// and the two planet-pending flags. Out here only so the header can see
// them; written from this file exactly as before.
namespace detail {
UiDepthMode g_uiDepthMode = UiDepthMode::kNone;
bool g_uiDepthOn = false;          // the key and the pass, both
bool g_uiDepthStoodDown = false;
bool g_uiDepthPlanetPending = false, g_uiDepthPlanetSolarPending = false;
bool g_holoDepthOn = false;        // advanced.temporal_aa_hologram_depth
}  // namespace detail

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
// The generic hologram/icon depth pass's other built-in families (below,
// "GENERIC HOLOGRAM/ICON DEPTH COVERAGE"): the radar's star icon core, its
// two stalks, and the sun's corona family, which also paints the icon's
// glow. Unlike kHoloPanel these carry no per-family coverage shader at
// all -- eye dump eye_135907, frame 17847: the radar star icon over sky
// carried the sky's motion at 0% AA depth coverage.
constexpr uint64_t kHoloIconCore     = 0xF8D8A92E96419901ull;
constexpr uint64_t kHoloCoronaFamily = 0xD1281DF454A153ADull;
constexpr uint64_t kHoloIconStalkA   = 0xDF3503CD07F9B10Cull;
constexpr uint64_t kHoloIconStalkB   = 0x5453D19B6D362364ull;
// The target hologram's sphere (two premultiplied quads, ps EA02FAC2BD6C643C
// and E95634B0F61D218F) and the radar's five contact-marker families --
// flight 20260924_155636, eye dump eye_155832: unlisted, the target sphere
// carried the sky's motion at (-3.9,-6.1) px/frame rolling, and the contact
// bars (pool\draws_155832.bin, frame 8548, right after the two stalks in each
// eye's cockpit section) read 0% contribution.
constexpr uint64_t kHoloTargetSphere = 0x5559BD94B6852E83ull;
constexpr uint64_t kHoloContactA     = 0xA2C2D5510BF1926Dull;
constexpr uint64_t kHoloContactB     = 0x9B34C331902DC1EDull;
constexpr uint64_t kHoloContactC     = 0x9611A454527F7FEBull;
constexpr uint64_t kHoloContactD     = 0xB932058F26B76691ull;
constexpr uint64_t kHoloContactE     = 0x94D5C556DFD6D705ull;
// The target reticle's three 3D triangles (72 non-indexed vertices, three
// prisms -- pool\draws_163515.bin, frame 27528, right after the canopy):
// a WORLD MARKER, not a cockpit family. It tracks the targeted ship, which
// can be kilometres out, so it is never radius-clipped like the families
// above -- eye dump eye_163515: sky MV (+0.59,-0.89) against the bracketed
// ship's (+0.14,+0.27) at 1.65 km, going indistinct with speed.
constexpr uint64_t kHoloWorldMarkerReticle = 0x71DD8B8B09060A81ull;
// The two interface composites drawn through the interface projection: the
// menu's and the loader's panel (vs A888D51024D9798E, ps 9107E72CB016CC02)
// and the loader's curved screen (vs 4EF6DDB075A927FA, ps 85565E9261812E2F).
constexpr uint64_t kPanelVs  = 0xA888D51024D9798Eull;
constexpr uint64_t kPanelPs  = 0x9107E72CB016CC02ull;
constexpr uint64_t kScreenVs = 0x4EF6DDB075A927FAull;
constexpr uint64_t kScreenPs = 0x85565E9261812E2Full;
// Comms-panel gamma variant: same t0/s0 UV sample and unchanged alpha.
constexpr uint64_t kScreenGammaPs = 0x8ADB2A81A45E8A4Bull;
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
// Its alpha uses the screen shader's single t0/s0 sample, but its vertex
// shader forces device Z to one. It needs dedicated depth reconstruction
// (kSpriteDepthHlsl). Earlier suppression tests disputed target ownership;
// the 15:20 ledger now places both sprite records at the target, and its
// marked pixels have the exact forced depth from that vertex shader.
constexpr uint64_t kHudSprite = 0xE508648660A352B2ull;
constexpr uint64_t kSpritePs  = 0x63ABD86359B57D01ull;
// The cockpit holo panels' pixel shader, for the reactive mask's coverage:
// it samples the interface surface at t2 through s1 at TEXCOORD8 (its own
// disassembly, 2026-09-08 -- the same shape as the menu panel's, which
// takes t1 through s1 at TEXCOORD6).
constexpr uint64_t kHoloPanelPs = kHoloLitPs;
constexpr uint64_t kRingVs=0xB12F7A618E1BDE98ull, kRingPs=0x42AC0CACC9CDF72Bull;
constexpr uint64_t kOrbitalVs=0xC7FA0C0F5DD49180ull, kOrbitalPs=0x6EEF165A350DA30Full;
// THE DRIVES' SMOKE (fix.temporal_aa_smoke, 2026-09-09): the trail a ship
// leaves is a ribbon of fifty translucent quads -- vs 5E417E9DF2E7F9E6, ps
// BD801F2FB02522EB, additive, a 1024x512 streak scrolled twice and a
// soft-particle fade against the depth resolve -- with no depth of their
// own, so the temporal pass carried it at the sky's distance while the
// ship's motion moved it, and each segment's fade kept a different history
// from its neighbour's: "still seeing some rectangles in the smoke" once the
// heat haze was withheld (the census of 13:03, the dump of 14:56). Through
// the coverage pass the flight HUD uses, the smoke's dense core writes its
// own depth for the pass (kSmokeDepthHlsl) and the pass keeps it in place.
constexpr uint64_t kSmokeVs = 0x5E417E9DF2E7F9E6ull;
constexpr uint64_t kSmokePs = 0xBD801F2FB02522EBull;
bool g_smokeOn = true;
Microsoft::WRL::ComPtr<ID3D11PixelShader> g_smokeCoronaShader;
bool g_smokeCoronaCompileTried=false;
// A mesh draw whose pixel shader (258B95AC99520C1F) reads nothing and writes
// nothing; the interface surface in its slot 0 is a leftover binding, and
// the surface rule took it for a composite on the loading screen
// (2026-09-07). Never treated.
constexpr uint64_t kNullPsMesh = 0xB018D143700AB803ull;

constexpr uint32_t kMaxSurfaces = 64;      // a session showed thirteen
constexpr uint32_t kMaxHashes = 16;
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
float    g_cockpitMetres = kTemporalShipMetres; // same near-field domain as temporal AA
HoloMotion g_holoMotion[2];
PlanetCoverage g_planetCoverage;
bool g_planetNoted=false,g_solarNoted=false;
HoloDraw g_holoDraw;
bool g_holoBound=false, g_holoNoted=false;
bool g_coronaPending=false, g_coronaMotion=false, g_coronaNoted=false;
ID3D11Buffer* g_savedHoloInfo=nullptr;
ID3D11ShaderResourceView* g_savedSceneDepth=nullptr;
bool g_sceneDepthBound=false;
Microsoft::WRL::ComPtr<ID3D11VertexShader> g_orbitalVs,g_savedOrbitalVs;
Microsoft::WRL::ComPtr<ID3D11Buffer> g_savedOrbitalInfo;
ID3D11ClassInstance* g_savedOrbitalClasses[256]{};
UINT g_savedOrbitalClassCount=0;
bool g_orbitalBound=false,g_stellarNoted[2]{};
Microsoft::WRL::ComPtr<ID3D11Buffer> g_holoDump;
unsigned g_holoDumpCount=0;
// The scanner's chrome surfaces the tracker matched this frame, held for
// the frame (a reference each, released at the boundary) so an eye run
// staged at the pass can copy them; the copies go out with the run as
// Chrome0/Chrome1 (uiDepthLearnScannerChrome says why they are wanted).
ID3D11Resource* g_chromeHeld[2] = {};
uint32_t g_chromeHeldCount = 0;
Microsoft::WRL::ComPtr<ID3D11Texture2D> g_chromeDump[2];
uint32_t g_chromeDumpCount = 0;
float    g_reactive = 0.0f;     // advanced.ui_depth_reactive: the bias mask's value
float    g_ghostTolerance = 12.0f; // advanced.ui_ghost_tolerance: the UI-resolve clamp's bound tolerance, 8-bit colour steps (0..64)
float    g_coronaSmearLevel = 64.0f; // advanced.corona_smear_level: the corona-smear hold's brightness limit, 8-bit colour steps (0..255, 0 = off); always on under the temporal pass
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
    char     family = 0;   // 'V' vector, 'T' text, 'I' icons, 'C' scanner chrome
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
const char kPanelDepthHlsl[] = EDVR_UI_CHANGE_INPUT
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
    "float4 main(In i, out float edit : SV_Target2) : SV_Target0 {\n"
    "    float a = Surf.Sample(Smp, i.tc6).a;\n"
    "    edit = uiEdit(i.tc6);\n"
    "    clip(max(a - floorAndStrength.x, edit - 1.0/255.0));\n"
    "    return floorAndStrength.w;\n"
    "}\n";
// These direct screen composites do not add the holo material's glow.
// Inactive comms icons have alpha below the general 0.5 floor: dropping
// them gave the visible strokes sky motion (eye_164038). Keep all visible
// coverage down to one 8-bit alpha step, including their antialiased edges.
// Sprite/target-marker and hologram thresholds remain separate.
const char kScreenDepthHlsl[] = EDVR_UI_CHANGE_INPUT
    "Texture2D<float4> Surf : register(t0);\n"
    "SamplerState Smp : register(s0);\n"
    "cbuffer P : register(b13) { float4 floorAndStrength; };\n"
    "struct In { float2 tc0 : TEXCOORD0; };\n"
    "float4 main(In i, out float edit : SV_Target2) : SV_Target0 {\n"
    "    float a = Surf.Sample(Smp, i.tc0).a;\n"
    "    edit = uiEdit(i.tc0);\n"
    "    clip(max(a - min(floorAndStrength.x, 1.0 / 255.0), edit - 1.0/255.0));\n"
    "    return floorAndStrength.w;\n"
    "}\n";
// Unlike the screen composite, sprite VS E508648660A352B2 explicitly
// writes clip Z = abs(clip W) (instructions 83..85). Its raster depth is
// therefore 1 for every visible sprite, not its physical depth. Recover
// the scene encoding from SV_Position.w (the interpolated clip W on
// D3D11, verified by the sprite GPU test). The 15:20 capture's
// sprite records sit at 16.6 km, exactly over the chevrons with depth 1.
// The original sprite draw disables depth testing; preserve that visible
// coverage over nearer scene geometry without replacing its nearer depth.
const char kSpriteDepthHlsl[] = EDVR_UI_CHANGE_INPUT R"HLSL(
Texture2D<float4> Surf : register(t0);
Texture2D<float> SceneDeviceDepth : register(t2);
SamplerState Smp : register(s0);
cbuffer P : register(b13) { float4 floorAndStrength; float4 sceneProjection; };
cbuffer Motion : register(b12) { uint4 motionInfo; };
struct In { float2 tc0 : TEXCOORD0; float4 pos : SV_Position; };
float4 main(In i, out float depth : SV_Depth, out float2 motion : SV_Target1, out float edit : SV_Target2) : SV_Target0 {
    // This composite has no holo glow. Retain its faint antialiased strokes,
    // but never turn erased scrolling ticks into 32-frame terrain strips.
    // Departed text is already cleared by the post-resolve influence history.
    clip(Surf.Sample(Smp, i.tc0).a - min(floorAndStrength.x, 1.0/255.0));
    edit = uiEdit(i.tc0);
    float own = sceneProjection.x + sceneProjection.y / max(i.pos.w, 0.000001);
    depth = max(own, SceneDeviceDepth.Load(int3(int2(i.pos.xy),0)));
    motion = float2(motionInfo.x+1, depth);
    return floorAndStrength.w;
}
)HLSL";
// The holo material: the cockpit's panels and, instanced from the pool at
// the target, the target markers. Its depth was written in place by the
// game's own draw under the writing twin until 2026-09-09, this shader only
// marking the mask; but the material's alpha carries an eight-tap smear
// past every stroke of the surface (a glow the game discards only under
// 1e-5), and in place each marker corner wrote its depth over the station
// around it. Now this shader writes the depth too, through the second draw
// in the scene's projection (Mode::kReissueScene), under the surface's
// strokes at the floor and not under the glow. The captured rank labels
// peak at alpha 124/255: the surface stores dimming in alpha, so a 0.5
// cutoff removes every letter. Cockpit-distance surfaces instead retain
// nonzero source coverage, like the comms panel. This includes translucent
// panel backing; one composited pixel cannot carry both panel and sky
// motion. Distant markers retain the old cutoff and never stamp their glow.
const char kHoloDepthBody[] = EDVR_UI_CHANGE_INPUT
    "SamplerState Smp : register(s1);\n"
    "cbuffer P : register(b13) { float4 floorAndStrength; float4 sceneProjection; };\n"
    "cbuffer Motion : register(b12) { uint4 motionInfo; };\n"
    "struct In {\n"
    "    float4 tc0 : TEXCOORD0;\n"
    "    float3 tc4 : TEXCOORD4;\n"
    "    float3 tc6 : TEXCOORD6;\n"
    "    float3 tc7 : TEXCOORD7;\n"
    "    float2 tc8 : TEXCOORD8;\n"
    "    float4 pos : SV_Position;\n"
    "};\n"
    "float4 main(In i, out float2 motion : SV_Target1, out float edit : SV_Target2) : SV_Target0 {\n"
    "    float a = Surf.Sample(Smp, i.tc8).a;\n"
    "    float den = i.pos.z - sceneProjection.x;\n"
    "    bool cockpit = den > 0 && sceneProjection.y > 0 && sceneProjection.y / den < sceneProjection.z;\n"
    "    edit = uiEdit(i.tc8);\n"
    "    clip(max(a - (cockpit ? min(floorAndStrength.x, 1.0 / 255.0) : floorAndStrength.x), edit - 1.0/255.0));\n"
    "    motion = float2(motionInfo.x+1, i.pos.z);\n"
    "    return floorAndStrength.w;\n"
    "}\n";
const std::string kHoloDepthHlsl = "Texture2D<float4> Surf : register(t2);\n" + std::string(kHoloDepthBody);
const std::string kHoloUnlitDepthHlsl = "Texture2D<float4> Surf : register(t1);\n" + std::string(kHoloDepthBody);

// THE SMOKE'S COVERAGE (kSmokeVs): ps BD801F2FB02522EB register for register
// -- the sphere test and the soft fade against the depth resolve at t0
// (through s1), the two scrolled samples of the streak at t1 (through s0),
// the alpha their product -- then the pass's depth under the dense core
// alone. The floor is the interface's capped low: additive smoke is faint by
// design, and a core above eight percent is the part that shows.
const char kSmokeDepthHlsl[] =
    "Texture2D<float4> Depth : register(t0);\n"
    "Texture2D<float4> Streak : register(t1);\n"
    "SamplerState Smp0 : register(s0);\n"
    "SamplerState Smp1 : register(s1);\n"
    "cbuffer CB1 : register(b1) { float4 cb1[211]; };\n"
    "cbuffer CB2 : register(b2) { float4 cb2[3]; };\n"
    "#ifdef CORONA_MOTION\n"
    "Texture2D<float> SceneDepth : register(t2);\n"
    "cbuffer Motion : register(b12) { uint4 motionInfo; };\n"
    "#endif\n"
    "cbuffer P : register(b13) { float4 floorAndStrength; float4 proj; };\n"
    "struct In {\n"
    "    float3 tc0 : TEXCOORD0;\n"
    "    float3 tc1 : TEXCOORD1;\n"
    "    float3 tc2 : TEXCOORD2;\n"
    "    float2 tc3 : TEXCOORD3;\n"
    "    float4 pos : SV_Position;\n"
    "};\n"
    "#ifdef CORONA_MOTION\n"
    "float4 main(In i, out float oDepth : SV_Depth, out float4 motion : SV_Target1) : SV_Target0 {\n"
    "#else\n"
    "float4 main(In i, out float oDepth : SV_Depth) : SV_Target0 {\n"
    "#endif\n"
    "    float3 d = i.tc2 - i.tc0;\n"
    "    float dd = (dot(d, d) - cb1[126].x * cb1[126].x) * 4.0;\n"
    "    float3 n = normalize(i.tc2);\n"
    "    float a = dot(-n, d);\n"
    "    float b = a + a;\n"
    "    float disc = sqrt(b * b - dd);\n"
    "    bool hit = 0.0 < disc;\n"
    "    float t = hit ? (-a * 2.0 + disc) * 0.5 : 0.0;\n"
    "    float sphereZ = -n.z * t + i.tc2.z;\n"
    "    float2 uv = i.tc1.xy / i.tc1.z * float2(0.5, -0.5) + 0.5;\n"
    "    float sceneZ = Depth.Sample(Smp1, uv).x;\n"
    "    if (sceneZ - sphereZ + cb1[126].x * 0.0001 < 0.0) discard;\n"
    "    float fade = saturate((sceneZ - i.tc1.z) / (cb1[126].x * 0.4));\n"
    "    fade = hit ? 1.0 : fade;\n"
    "    fade *= cb1[126].z * cb2[1].z;\n"
    "    float2 uv1 = float2(i.tc3.x - cb1[210].y * cb2[1].w, (i.tc3.y + 1.0) * 0.5);\n"
    "    float2 uv2 = float2(i.tc3.x + cb1[210].y * cb2[2].x, i.tc3.y * 0.5);\n"
    "    float streak = Streak.Sample(Smp0, uv1).x + Streak.Sample(Smp0, uv2).x;\n"
    "    float alpha = fade * streak;\n"
    "    // The mask (the review of 2026-09-10): w is the strength at full\n"
    "    // opacity, and the value follows the smoke's own alpha up to it,\n"
    "    // quantised to an ODD quantum -- the pass keeps the camera's path\n"
    "    // under an odd one (floorBuffer). w of nought is the one-quantum\n"
    "    // mark of before, as good as unmarked to NVIDIA.\n"
    "    float q = floor(saturate(alpha * floorAndStrength.w) * 63.0 + 0.5);\n"
    "    // The dense core (alpha at the floor or above) writes its depth;\n"
    "    // the fringe under it writes none -- the target is EDVR's own,\n"
    "    // cleared to the far value, so a far depth changes nothing -- and\n"
    "    // marks the mask only where the strength gives it a quantum, so\n"
    "    // the mark fades with the smoke instead of stepping at the floor\n"
    "    // (the review's second note).\n"
    "    bool core = alpha >= floorAndStrength.z;\n"
    "    clip((core || q > 0.0) ? 1.0 : -1.0);\n"
    "    // The depth from the ribbon's own view depth (TEXCOORD1.z, the\n"
    "    // value its shader compares with the depth resolve) in the scene's\n"
    "    // encoding: the raster's z is the vertex shader's clip z plus a\n"
    "    // constant (15.01, before the divide) whose meaning rests on the\n"
    "    // matrix the game uploads (the review's lead), while this is exact.\n"
    "    // The raster's z when no projection is known.\n"
    "    float own = proj.y != 0.0 ? proj.x + proj.y / max(i.tc1.z, 0.01) : i.pos.z;\n"
    "    oDepth = core ? own : 0.0;\n"
    "#ifdef CORONA_MOTION\n"
    "    bool coronaVisible = core && own >= SceneDepth.Load(int3(int2(i.pos.xy),0));\n"
    "    motion = float4(motionInfo.x+1u, own, 0.0, coronaVisible ? 1.0 : 0.0);\n"
    "#endif\n"
    "    return (4.0 * q + 3.0) / 255.0; // class 3: smoke, not UI evidence\n"
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
// The opacity calculation follows the captured PS, including its noise,
// strength, ray length and fade. Only genuinely opaque cores own motion;
// transparent station detail under the glyph keeps its scene motion.
const char kHudDepthHlsl[] = R"HLSL(
Texture2D<float4> Depth : register(t0);
Texture2D<float4> Noise : register(t1);
Texture2D<float> SceneDeviceDepth : register(t2);
SamplerState Smp0 : register(s0);
SamplerState Smp1 : register(s1);
cbuffer CB1 : register(b1) { float4 cb1[205]; };
cbuffer P : register(b13) { float4 floorAndStrength; float4 sceneProjection; };
struct In {
    float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1;
    float4 tc2 : TEXCOORD2; float4 tc5 : TEXCOORD5;
    float4 tc9 : TEXCOORD9; float4 tc10 : TEXCOORD10;
    float4 tc13 : TEXCOORD13; float4 tc16 : TEXCOORD16;
    float2 tc17 : TEXCOORD17; float3 tc18 : TEXCOORD18;
    float4 pos : SV_Position;
};
// ps 8DEF46452FA459F5, instructions 120..131 (repeated for three octaves).
float hudNoise(float3 p) {
    float3 cell=floor(p), f=frac(p);
    float3 u=f*f*(3.0-2.0*f);
    float2 uv=(cell.xy+cell.z*float2(37,17)+u.xy+0.5)/256.0;
    float2 n=Noise.SampleLevel(Smp0,uv,-100.0).xy;
    return lerp(n.y,n.x,u.z);
}
float4 main(In i, out float oDepth : SV_Depth) : SV_Target {
    float2 uv=i.tc1.xy/i.tc1.z*0.5+0.5;
    float sceneZ=Depth.Sample(Smp1,float2(uv.x,1.0-uv.y)).x;
    clip(sceneZ-i.tc1.z); clip(i.tc5.w-0.01);
    float k=cb1[204].x/(cb1[204].x+0.00001);
    float fade=saturate(length(i.tc10.xyz)*0.05-i.tc13.w*0.5);
    float near=saturate(i.tc9.w/max(cb1[204].x,0.01));
    float opacity=k*(near-fade)+fade;
    float floorAlpha=max(floorAndStrength.x,0.7);
    clip(opacity-floorAlpha);
    float3 seg=i.tc5.xyz-i.tc16.xyz; clip(length(seg)-0.01);
    float3 e=i.tc18-i.tc5.xyz;
    float t=dot(e,-seg)/(seg.x*seg.x);
    float dist=t<0.0?length(e):t>1.0?length(i.tc18-i.tc16.xyz):length(e+t*seg);
    bool screen=i.tc2.w>0.5;
    float nd=screen?i.tc17.x*2.0-1.0:saturate(dist/i.tc13.w);
    float q=nd*nd; clip(1.0-q);
    float3 ray=normalize(i.tc18-i.tc0.xyz);
    float radius=max(i.tc16.w,0.176809);
    float halfSpan=max(sqrt(1.0-q)*i.tc13.w/radius,0.000001);
    float3 origin=i.tc18-ray*(i.tc17.y/radius);
    float start=-halfSpan, finish=halfSpan;
    if(screen) {
        float distance=length(origin-i.tc0.xyz);
        start=max(i.tc10.w-distance,-halfSpan);
        finish=min(i.tc1.w-distance,halfSpan);
    }
    clip(finish-start);
    int steps=asint(cb1[203].w); clip(float(steps)-0.5);
    float step=(finish-start)/float(steps);
    float strength=min(sqrt(i.tc5.w),1.0);
    strength=min(strength*strength*(3.0-2.0*strength),1.0);
    strength*=i.tc0.w*0.2*lerp(cb1[203].x,cb1[202].w,i.tc2.w);
    strength=saturate(strength*3.333333);
    strength=strength*strength*(3.0-2.0*strength);
    float3 axis=normalize(-seg);
    float transmission=1.0, travel=start;
    // The original opacity march, including its live noise table. A
    // geometric upper bound marks pixels whose real opacity may be zero.
    [loop] for(int n=0;n<steps;++n) {
        float3 p=ray*travel+origin;
        float3 offset=axis*length(i.tc13.xyz-p)*0.1;
        float noise=hudNoise(p*0.25+offset)+0.5*hudNoise(p*0.5+offset)+0.25*hudNoise(p+offset);
        float density=saturate(noise*0.571429-(travel/halfSpan)*(travel/halfSpan)-q);
        density=density*density*(3.0-2.0*density);
        transmission*=exp2(-density*strength*step);
        travel+=step;
    }
    clip(opacity*(1.0-transmission)-floorAlpha);
    // The resolved depth and clip W support the game's own occlusion test.
    // They need not share the temporal pass's metre encoding. Preserve the
    // actual device depth under floating strokes, without re-encoding it.
    bool attached=i.tc1.z*1.5>=sceneZ;
    oDepth=attached?i.pos.z:SceneDeviceDepth.Load(int3(int2(i.pos.xy),0));
    return attached?floorAndStrength.y:floorAndStrength.w;
}
)HLSL";

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
DepthShader g_depthShaders[9] = {
    {{kPanelPs, kPanelPsTinted, kPanelPsCheap, 0}, {kPanelVs, 0, 0, 0}, 1,
     kPanelDepthHlsl, sizeof(kPanelDepthHlsl) - 1, "ui_depth_panel_ps", nullptr, false},
    // The flight HUD's coverage (kHudDepthHlsl): its slot is the depth
    // resolve it tests against, not an interface surface, so no variant of
    // another family can borrow it by slot.
    {{kFlightHudPs, 0, 0, 0}, {kFlightHud, 0, 0, 0}, 0xFFFFu,
     kHudDepthHlsl, sizeof(kHudDepthHlsl) - 1, "ui_depth_hud_ps", nullptr, false},
    {{kScreenPs, kScreenGammaPs, 0, 0}, {kScreenVs, 0, 0, 0}, 0,
     kScreenDepthHlsl, sizeof(kScreenDepthHlsl) - 1, "ui_depth_screen_ps", nullptr, false},
    {{kHoloPanelPs, 0, 0, 0}, {kHoloPanel, 0, 0, 0}, 2,
     kHoloDepthHlsl.c_str(), kHoloDepthHlsl.size(), "ui_depth_holo_ps", nullptr, false},
    // The drives' smoke: its slot is the depth resolve, as the flight HUD's.
    {{kSmokePs, 0, 0, 0}, {kSmokeVs, 0, 0, 0}, 0xFFFFu,
     kSmokeDepthHlsl, sizeof(kSmokeDepthHlsl) - 1, "ui_depth_smoke_ps", nullptr, false},
    {{kSpritePs, 0, 0, 0}, {kHudSprite, 0, 0, 0}, 0,
     kSpriteDepthHlsl, sizeof(kSpriteDepthHlsl) - 1, "ui_depth_sprite_ps", nullptr, false},
    {{kRingPs,0,0,0},{kRingVs,0,0,0},0xFFFFu,
     kRingCoverage,sizeof(kRingCoverage)-1,"ring_coverage_ps",nullptr,false},
    {{kOrbitalPs,0,0,0},{kOrbitalVs,0,0,0},0xFFFFu,
     kOrbitalCoveragePs,sizeof(kOrbitalCoveragePs)-1,"orbital_coverage_ps",nullptr,false},
    {{kHoloUnlitPs,0,0,0},{kHoloPanel,0,0,0},1,
     kHoloUnlitDepthHlsl.c_str(),kHoloUnlitDepthHlsl.size(),"ui_depth_holo_unlit_ps",nullptr,false},
};
bool holoShader(const DepthShader* shader) {
    return shader == &g_depthShaders[3] || shader == &g_depthShaders[8];
}
struct FloorCb {
    float          cockpitMetres = -1.0f;
    ID3D11Buffer* cb = nullptr;
    float         floor = -1.0f;
    float         strength = -1.0f;
    float         nearDepth = -1.0f;   // the depth value at one metre (temporalPassDepthAt), for a floating stroke's core
    float         smokeFloor = -1.0f;  // slot 3, the smoke's: its opacity floor for the depth it writes (z)...
    float         smokeMax = -1.0f;    // ...and the mask's strength at full opacity (w); advanced.temporal_aa_smoke_*
    float         depthAt2 = -1.0f;    // the depth value at two metres, with nearDepth the pass's projection pair (the second float4)
};
// THE SMOKE'S COVERAGE, tunable (the review of 2026-09-10): the trail's
// rectangles trace its segments, each fading through the depth floor at
// its own time -- a hard step between the smoke's depth and the sky's --
// while its scrolling texture is accumulated under a one-quantum mark.
// The floor is the smoke's own now (advanced.temporal_aa_smoke_floor,
// 0.08 as before), and the mask can follow its opacity up to a strength
// (advanced.temporal_aa_smoke_reactive; 0 keeps the one-quantum mark).
float g_smokeFloor = 0.08f;
float g_smokeReactive = 0.0f;
// [4] the scanner's chrome: the interface proper at one alpha step. The
// spectral graph and its labels are drawn DIM -- their strokes sit at alpha
// 0.2-0.3 in the chrome (eye_194158_Chrome0: luma 20-40 at alpha 51-77 of
// 255, premultiplied) -- and the chrome carries no translucent area at all
// (97.7% alpha 0, the rest strokes), so under the general floor of 0.5 the
// graph took the sky's far-plane pan (docs/fss-scanner.md, 2026-09-16) and
// nothing is lost by writing down to one step while the scanner is up.
FloorCb g_floorCbs[5];   // [0] the interface proper, [1] the holo material and the sprite, [2] the flight HUD, [3] the smoke, [4] the scanner's chrome (g_reissueMaskSlot)
constexpr int kChromeFloorSlot = 4;
ID3D11DepthStencilState* g_reissueDss = nullptr;   // GEQUAL, write all
ID3D11DepthStencilState* g_overlayDss = nullptr;   // visible interface overlays replace private depth
bool          g_reissueDssFailedNoted = false;

// THE REACTIVE MASK (ui_depth.h): one per eye at the render size, cleared
// every frame, marked by the same second draw that writes the interface's
// depth, handed to NVIDIA by the temporal pass.
struct Mask {
    ID3D11Texture2D*        tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    uint32_t                w = 0, h = 0;
    bool                    marked = false;   // anything drawn since the clear
};
Mask     g_mask[2];
Mask     g_edits[2];
UiContent g_uiContent;
bool g_contentNoted=false;
// UI CONTENT CENSUS: one log line per surfaceComposite draw that calls
// g_uiContent.prepare, but only across the eye run's first frame, capped
// at 64 lines, plus one summary line once that frame closes. Gated on
// objectProbeLedgerActive() -- the eye-run-armed signal pixel_probe and
// object_probe's own ledger already key off (both armed from
// temporal_pass.cpp's beginEyeRun) -- which stays true for the whole
// multi-frame capture, so a rising edge here is what narrows it to the
// FIRST frame: g_uiContentCensusOn flips true at the frame boundary where
// the ledger is seen newly active (arming the frame about to start) and
// false at the next boundary (closing it, after printing the summary).
bool     g_uiContentCensusOn = false;
bool     g_uiContentCensusLedgerWasOn = false;
uint32_t g_uiContentCensusLines = 0;
uint32_t g_uiContentCensusHits = 0, g_uiContentCensusResets = 0, g_uiContentCensusUpdated = 0,
         g_uiContentCensusDeclined = 0, g_uiContentCensusEvicted = 0;
const char* uiContentDecisionWord(UiContent::Decision d) {
    using D = UiContent::Decision;
    switch (d) {
        case D::kHit: return "hit";
        case D::kReset: return "reset";
        case D::kUpdated: return "updated";
        default: return "declined";
    }
}
const char* uiContentReason(UiContent::Decision d, uint32_t age) {
    using D = UiContent::Decision;
    switch (d) {
        case D::kDeclinedNoSurface: return "no surface";
        case D::kDeclinedNotTexture2D: return "not Texture2D";
        case D::kDeclinedFormat: return "format";
        case D::kDeclinedNoRenderTarget: return "no RENDER_TARGET bind";
        case D::kDeclinedViewShape: return "array/MSAA/mips/view";
        case D::kDeclinedBudget: return "over budget";
        case D::kDeclinedNoFreeEntry: return "no free entry";
        case D::kDeclinedCreateFailed: return "creation failed";
        case D::kDeclinedShaderFailed: return "shader failed";
        case D::kHit: return "same frame";
        case D::kReset: return age == 0 ? "new entry" : "frame gap";
        case D::kUpdated: return "compared";
    }
    return "?";
}
bool     g_maskFailedNoted = false;
bool     g_maskSizeNoted = false;
// THE SMOKE'S OWN DEPTH TARGET (the review of 2026-09-10 on the trail's
// voids). The smoke's coverage draw wrote its depth into the SCENE's depth
// target in the middle of the frame, and the game draws on after the
// ribbon with the depth test on -- the trail's scattering volume right
// after it, tested GREATER_EQUAL -- so an opaque depth surface under a
// translucent effect cut out whatever came later behind it, segment by
// segment. The coverage now writes into a target of EDVR's, cleared each
// frame before its first draw, and the temporal pass folds it into the
// scene's depth as it reads (temporal_pass.cpp's zSceneAt: the nearer
// wins), so the game's depth is never touched and every consumer of the
// pass's depth -- the vectors, NVIDIA's depth input, the depth view --
// sees the smoke where it is. One per eye, the scene depth target's size.
struct SmokeDepth {
    ID3D11Texture2D*          tex = nullptr;
    ID3D11DepthStencilView*   dsv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    uint32_t                  w = 0, h = 0;
    bool                      written = false;   // cleared and drawn into this frame
};
SmokeDepth g_smokeDepth[2];
bool       g_smokeDepthFailedNoted = false;
bool       g_smokeDepthNoted = false;
struct SceneDepthRead { ID3D11Texture2D* tex=nullptr; ID3D11ShaderResourceView* srv=nullptr; DXGI_FORMAT fmt=DXGI_FORMAT_UNKNOWN; bool tried=false; };
SceneDepthRead g_sceneDepthRead[2];
ID3D11ShaderResourceView* sceneDepthReadView(ID3D11DeviceContext* ctx, ID3D11Texture2D* scene, int eye) {
    if(!ctx || !scene || eye<0 || eye>1) return nullptr;
    SceneDepthRead& cached=g_sceneDepthRead[eye];
    if(cached.tex==scene && cached.tried) return cached.srv;
    if(cached.srv) cached.srv->Release(); if(cached.tex) cached.tex->Release(); cached={};
    cached.tex=scene; cached.tex->AddRef(); cached.tried=true;
    D3D11_TEXTURE2D_DESC td{}; scene->GetDesc(&td); DXGI_FORMAT read=DXGI_FORMAT_UNKNOWN;
    switch(td.Format) {
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: read=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: read=DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: read=DXGI_FORMAT_R32_FLOAT; break;
        default: return nullptr;
    }
    if(td.SampleDesc.Count!=1 || td.ArraySize!=1 || td.MipLevels!=1 || !(td.BindFlags&D3D11_BIND_SHADER_RESOURCE)) return nullptr;
    ID3D11Device* dev=nullptr; ctx->GetDevice(&dev); if(!dev) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format=read; sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels=1;
    HRESULT hr=dev->CreateShaderResourceView(scene,&sd,&cached.srv); dev->Release();
    if(FAILED(hr) || !cached.srv) return nullptr;
    cached.fmt=read; return cached.srv;
}
ID3D11BlendState* g_maskBlend = nullptr;     // opaque, red only
ID3D11DepthStencilState* g_maskDss = nullptr; // test as the game, writes off

// Private UI depth, seeded from the matching scene eye once per frame.
constexpr uint32_t kMaxRtvs = 8;
UiDepthLayer g_uiDepth[2];
bool g_privateDepthNoted = false;
bool g_privateDepthFailedNoted = false;
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
// Both coverage modes write private depth. Scene mode keeps the original
// viewport; interface mode scales its depth encoding to the scene's planes.
using Mode = detail::UiDepthMode;

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
ID3D11ClassInstance* g_savedPsClasses[256]{};
UINT g_savedPsClassCount=0;
ID3D11Buffer*            g_savedFloorCb = nullptr;
ID3D11ShaderResourceView* g_savedHudScene = nullptr;
ID3D11ShaderResourceView* g_savedEdits = nullptr;
bool                     g_editsBound = false;
bool                     g_hudSceneBound = false;
// State restored after the private coverage draw.
ID3D11DepthStencilState* g_reSavedDss = nullptr;
UINT                     g_reSavedRef = 0;
ID3D11BlendState*        g_reSavedBlend = nullptr;
bool g_reBlendSaved = false;
FLOAT                    g_reSavedBlendFactor[4] = {};
UINT                     g_reSavedSampleMask = 0;
bool                     g_reissueOn = false;
ID3D11RenderTargetView*   g_reissueRtvs[3] = {};
UINT                     g_reissueRtvCount = 0;
ID3D11DepthStencilView*   g_reissueTarget = nullptr;
ID3D11Texture2D*          g_reissueScene = nullptr;

// Counters: this window, and the session.
uint32_t g_wComposite = 0, g_wDirect = 0, g_wWrote = 0,
         g_wNotScene = 0, g_wRebound = 0, g_wNoPair = 0, g_wReissued = 0,
         g_wNoShader = 0, g_wNoTwin = 0, g_wLearned = 0,
         g_wFrames = 0;
uint64_t g_sessionWrote = 0;
bool     g_maskNotedOnce = false;
struct StellarCpu { uint32_t calls=0,samples=0; int64_t ticks=0; } g_stellarCpu[2];
int g_stellarCpuActive=-1;
int64_t g_stellarCpuStart=0;
GpuIntervals<64> g_stellarGpu[2];

void resetWindow() {
    g_wComposite = g_wDirect = g_wWrote = g_wNotScene = 0;
    g_wRebound = g_wNoPair = g_wReissued = g_wNoShader = 0;
    g_wNoTwin = g_wLearned = 0;
    g_wFrames = 0;
    for(auto& sample:g_stellarCpu) sample={};
    for(auto& sample:g_stellarGpu) sample.totals={};
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

void addSurface(const void* res, uint32_t w, uint32_t h, uint32_t fmt, char family) {
    if (surfaceIndex(res) >= 0) return;
    Surface s;
    s.res = const_cast<void*>(res);
    s.w = w;
    s.h = h;
    s.fmt = fmt;
    s.family = family;
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
    // The binding shadow's, set with the shader (2026-09-09); the Get only
    // when the shadow has seen no set, which is before the first draw, or
    // holds no hash -- a shader the registry had not met at its set.
    if (bindingGet(BindSlot::Vs)) {
        const uint64_t held = bindingShaderHash(BindSlot::Vs);
        if (held) return held;
    }
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
    if (bindingGet(BindSlot::Ps)) {   // the shadow's (boundVsHash says)
        const uint64_t held = bindingShaderHash(BindSlot::Ps);
        if (held) return held;
    }
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

// The re-issue's depth state: the nearer wins, so a ship model in front of
// the loader's screen keeps its depth; stencil left off.
ID3D11DepthStencilState* reissueState(ID3D11DeviceContext* ctx, bool overlay = false) {
    auto*& state = overlay ? g_overlayDss : g_reissueDss;
    if (state) return state;
    if (g_reissueDssFailedNoted) return nullptr;
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = (g_testAlways || overlay) ? D3D11_COMPARISON_ALWAYS : D3D11_COMPARISON_GREATER_EQUAL;
    d.StencilEnable = FALSE;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    const HRESULT hr = dev->CreateDepthStencilState(&d, &state);
    dev->Release();
    if (FAILED(hr) || !state) {
        state = nullptr;
        g_reissueDssFailedNoted = true;
        Log::get().note("ui depth: the depth pass's state could not be made "
                        "(0x%08lX); interface-projection composites stay as the "
                        "game issued them.",
                        static_cast<unsigned long>(hr));
        return nullptr;
    }
    return state;
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
Mask* maskFor(ID3D11DeviceContext* ctx, int eye, uint32_t w, uint32_t h, Mask* masks = g_mask) {
    if (eye < 0 || eye > 1 || !w || !h) return nullptr;
    Mask& m = masks[eye];
    if (m.tex && m.w == w && m.h == h) return &m;
    if (m.rtv) { m.rtv->Release(); m.rtv = nullptr; }
    if (m.srv) { m.srv->Release(); m.srv = nullptr; }
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
    if (SUCCEEDED(hr) && m.tex) hr = dev->CreateShaderResourceView(m.tex, nullptr, &m.srv);
    dev->Release();
    if (FAILED(hr) || !m.tex || !m.rtv) {
        if (m.rtv) { m.rtv->Release(); m.rtv = nullptr; }
        if (m.srv) { m.srv->Release(); m.srv = nullptr; }
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
    const FLOAT zero[4]{};ctx->ClearRenderTargetView(m.rtv,zero);
    return &m;
}

SmokeDepth* smokeDepthFor(ID3D11DeviceContext* ctx, int eye, uint32_t w, uint32_t h) {
    if (eye < 0 || eye > 1 || !w || !h) return nullptr;
    SmokeDepth& s = g_smokeDepth[eye];
    if (s.tex && s.w == w && s.h == h) return &s;
    if (s.srv) { s.srv->Release(); s.srv = nullptr; }
    if (s.dsv) { s.dsv->Release(); s.dsv = nullptr; }
    if (s.tex) { s.tex->Release(); s.tex = nullptr; }
    s.w = 0;
    s.h = 0;
    s.written = false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    // A 32-bit depth of its own whatever the scene's format: the values are
    // the scene's encoding either way, and the pass reads it as a float.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &s.tex);
    if (SUCCEEDED(hr) && s.tex) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = dev->CreateDepthStencilView(s.tex, &dd, &s.dsv);
    }
    if (SUCCEEDED(hr) && s.dsv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = DXGI_FORMAT_R32_FLOAT;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        hr = dev->CreateShaderResourceView(s.tex, &vd, &s.srv);
    }
    dev->Release();
    if (FAILED(hr) || !s.tex || !s.dsv || !s.srv) {
        if (s.srv) { s.srv->Release(); s.srv = nullptr; }
        if (s.dsv) { s.dsv->Release(); s.dsv = nullptr; }
        if (s.tex) { s.tex->Release(); s.tex = nullptr; }
        if (!g_smokeDepthFailedNoted) {
            g_smokeDepthFailedNoted = true;
            Log::get().note("ui depth: the smoke's %ux%u depth target could not be made (0x%08lX); "
                            "the trail keeps the sky's depth under the pass.",
                            w, h, static_cast<unsigned long>(hr));
        }
        return nullptr;
    }
    s.w = w;
    s.h = h;
    return &s;
}

// GENERIC HOLOGRAM/ICON DEPTH COVERAGE (advanced.temporal_aa_hologram_depth).
//
// kHoloPanel's own coverage above (kHoloDepthHlsl) needs a per-family
// shader that knows what alpha to clip at, and its glow rarely clears
// that floor; the icon families have no transcription at all. This is
// the family-agnostic alternative: replay the game's OWN draw -- VS, PS,
// inputs untouched -- into a scratch target whose blend mirrors the
// game's own RT0 blend (so the accumulated colour is what the game's
// blend equation would actually have ADDED to the frame, not raw PS
// output), gated to the cockpit radius; then replay it again, depth-only,
// into a second scratch target for the nearest raster depth of the same
// geometry. A resolve once a frame per eye stamps that depth into the
// private AA copy wherever the accumulated light clears a floor and (when
// the eye's finished colour is in hand) is a real share of it, before the
// temporal pass reads that copy (uiDepthTemporalDepth).
constexpr uint64_t kHoloFamiliesBuiltIn[10] = {kHoloIconCore, kHoloCoronaFamily,
                                               kHoloIconStalkA, kHoloIconStalkB,
                                               kHoloTargetSphere, kHoloContactA, kHoloContactB,
                                               kHoloContactC, kHoloContactD, kHoloContactE};
// WORLD MARKERS: a second, separate built-in list for draws that must be
// covered wherever they are, not just inside the cockpit radius (the
// families above are all short-range panel/icon geometry; a world marker
// tracks something that can be kilometres out). Fixed, never extended by
// advanced.temporal_aa_hologram_families -- see holoWorldMarkerList below.
constexpr uint64_t kHoloWorldMarkers[1] = {kHoloWorldMarkerReticle};
constexpr uint32_t kHoloWorldMarkerCount = static_cast<uint32_t>(sizeof(kHoloWorldMarkers) / sizeof(kHoloWorldMarkers[0]));
// The canopy sits in front of the whole sky; covering it would smear the
// stars behind it. Refused even if named in advanced.
// temporal_aa_hologram_families (holoBuildFamilyList, below parseHashes).
constexpr uint64_t kHoloCanopy = 0x8C091FFD08644E02ull;
uint64_t g_holoFamilies[kMaxHashes];
uint32_t g_holoFamilyCount = 0;
float    g_holoFloor = 0.05f;    // advanced.temporal_aa_hologram_floor: display brightness, 0..1
float    g_holoShare = 0.5f;     // advanced.temporal_aa_hologram_share
FaultBudget g_holoBudget("uiDepthHolo", 5);

// This draw's classification, set by uiDepthHologramOnEyeDraw and read by
// the two reissues that follow the game's own draw (vscreen.cpp).
int      g_holoEye = -1;
uint32_t g_holoW = 0, g_holoH = 0;
uint64_t g_holoDrawVs = 0;
bool     g_holoIsWorldMarker = false;   // matched kHoloWorldMarkers, not the cockpit list

// Three scratch targets per eye: the game's own light, mirrored through
// its own blend (contribution); the nearest raster depth across every
// listed draw (elementDepth); and a radius-only depth (radius) that both
// per-draw passes test against, so admission never depends on the game's
// own bound depth (the icon and corona families draw with depth off) or on
// real scene occlusion (the resolve's own test against the private copy,
// already seeded from the real scene, is where that happens). All three
// are prepared once at the first listed draw of their eye each frame. A
// small ring of occlusion queries lets the census count resolved pixels
// without ever blocking on the GPU (never awaited: polled with
// DONOTFLUSH, a later frame's poll picks up a query still pending).
constexpr uint32_t kHoloQueryRing = 3;
struct HoloScratch {
    ID3D11Texture2D*           contribTex = nullptr;
    ID3D11RenderTargetView*    contribRtv = nullptr;
    ID3D11ShaderResourceView*  contribSrv = nullptr;
    ID3D11Texture2D*           depthTex = nullptr;
    ID3D11DepthStencilView*    depthDsv = nullptr;
    ID3D11ShaderResourceView*  depthSrv = nullptr;
    ID3D11Texture2D*           radiusTex = nullptr;
    ID3D11DepthStencilView*    radiusDsv = nullptr;
    uint32_t                   w = 0, h = 0;
    uint32_t                   preparedFrame = ~0u;          // g_frame at the last prepare
    uint32_t                   declinedProjectionFrame = ~0u; // counted once per eye-frame
    float                      radiusDepth = 0.0f;    // reversed-Z device value AT the cockpit radius, this eye/frame
    bool                       linearBlend = false;    // the game's own RTV0 view was sRGB (fallback floor only)
    // The game's own RT0 resource, tracked so the SHARE test can read it
    // back in its own space -- never the tonemapped display image, which
    // is a different resource at a different dynamic range (the review of
    // 2026-09-24, flight 20260924_155636). AddRef'd; released and
    // re-acquired whenever the identity changes. targetSrv is over the
    // SAME resource, in the SAME view format the game's RTV0 used, so a
    // read decodes exactly as the game's own write (and this pass's own
    // blend-mirrored accumulation) did; built lazily at the resolve.
    ID3D11Resource*            targetRes = nullptr;
    DXGI_FORMAT                targetViewFormat = DXGI_FORMAT_UNKNOWN;
    bool                       targetShaderResource = false;
    ID3D11ShaderResourceView*  targetSrv = nullptr;
    ID3D11Query*                occlusion[kHoloQueryRing] = {};
    bool                        occlusionPending[kHoloQueryRing] = {};
    uint32_t                    occlusionNext = 0;
    // A separate ring for the world-marker element-depth diagnostic below
    // (holoDepthWindowTick's "element-depth samples p50"): a resolved
    // sample count of 0 with the viewport override in place means the VS
    // itself writes z=0, independent of the game's own viewport.
    ID3D11Query*                markerOcclusion[kHoloQueryRing] = {};
    bool                        markerOcclusionPending[kHoloQueryRing] = {};
    uint32_t                    markerOcclusionNext = 0;
};
HoloScratch g_holoScratch[2];
constexpr UINT kHoloMaxViewports = 16;

// Saved OM/PS state around the two per-draw reissues. Sequential and
// non-overlapping (Contribution fully ends before ElementDepth begins),
// so one set of slots serves both; the RTV/DSV half reuses ui_depth's own
// g_savedRtvs/g_savedDsv/restoreOm, exactly as the family reissue above.
ID3D11BlendState*         g_holoSavedBlend = nullptr;
FLOAT                     g_holoSavedBlendFactor[4]{};
UINT                       g_holoSavedSampleMask = 0;
ID3D11DepthStencilState*  g_holoSavedDss = nullptr;
UINT                       g_holoSavedRef = 0;
ID3D11PixelShader*        g_holoSavedPs = nullptr;
ID3D11ClassInstance*      g_holoSavedPsClasses[256]{};
UINT                       g_holoSavedPsClassCount = 0;
ID3D11BlendState*         g_holoContribBlendToFree = nullptr;  // an uncached blend past the cache's size
bool                       g_holoContribOn = false, g_holoElementOn = false;
// World-marker-only additions to the element-depth pass: the game's own
// viewports, saved so a MinDepth=MaxDepth=0 draw can be overridden to
// 0..1 and put back afterward, and the occlusion query begun around it.
D3D11_VIEWPORT            g_holoSavedViewports[kHoloMaxViewports]{};
UINT                       g_holoSavedViewportCount = 0;
ID3D11Query*              g_holoElementQuery = nullptr;
bool                       g_holoWorldMarkerNoted = false;

// The contribution pass's fixed depth-test state for a COCKPIT family:
// GREATER against the RADIUS scratch (cleared to this eye/frame's
// radiusDepth), write ZERO. Never the draw's own state, and never the
// game's live depth, so a draw with depth off, or with no depth view
// bound, gets exactly this test.
ID3D11DepthStencilState* g_holoContribDss = nullptr;
// The same pass for a WORLD MARKER: DepthEnable FALSE, so every fragment
// contributes regardless of range -- a marker can track something
// kilometres out, well past the radius scratch's own clear value.
ID3D11DepthStencilState* g_holoWorldMarkerDss = nullptr;
// GREATER, write ALL: the element-depth pass's own accumulation (nearer
// of every listed draw's geometry this eye/frame, starting from 0, the
// reversed-Z far) and the resolve's write into the private copy (nearer of
// the element and whatever is already there) share this test.
ID3D11DepthStencilState* g_holoElementDss = nullptr;
ID3D11RasterizerState*   g_holoResolveRs = nullptr;

// The contribution blend's cache, keyed by the EFFECTIVE SrcBlend (after
// holoMapSrcBlend) and write mask -- DestBlend/BlendOp/alpha are always
// the same fixed values, and BlendEnable folds into "effective SrcBlend
// ONE" when the game's own blend was off. A combination past this size is
// built uncached (g_holoContribBlendToFree) and released after the draw.
struct HoloContribBlendEntry { D3D11_BLEND srcBlend = D3D11_BLEND_ONE; UINT8 writeMask = 0; ID3D11BlendState* state = nullptr; };
constexpr uint32_t kHoloContribBlendCacheSize = 16;
HoloContribBlendEntry g_holoContribBlendCache[kHoloContribBlendCacheSize];

// The resolve's own full-screen shaders and constant buffer, built once.
ID3D11VertexShader* g_holoResolveVs = nullptr;
ID3D11PixelShader*  g_holoResolvePs = nullptr;
bool                g_holoResolveTried = false;
ID3D11Buffer*       g_holoResolveCbBuf = nullptr;
// radiusDepth feeds the resolve's cockpitRange (the dark-pixel test). The
// struct stays 16 bytes, the multiple a D3D11 constant buffer's ByteWidth
// must be.
struct HoloResolveCb { float floorValue, share; uint32_t flags; float radiusDepth; };

// The periodic census (holoDepthWindowTick): a 30 s wall-clock window,
// unlike the neighbouring 20 s frame-counted one (kTotalsFrames) --
// framerate here swings with the scene (settlement/station drops to
// ~41 fps, docs/settlement-flicker), so wall time keeps the window
// meaningful. Printed even when nothing happened (deliberately unlike
// the neighbouring totals line at kTotalsFrames, which skips an all-zero
// window): a fixed cadence is a heartbeat that says the instrument ran.
// Printed only while the key itself is on (holoDepthWindowTick's own
// first line): off, the window's clock simply does not run.
uint64_t g_holoWindowStartMs = 0;
uint32_t g_holoWindowFrames = 0, g_holoWindowListed = 0, g_holoWindowResolved = 0;
// noTarget: the share test skipped outright (the game's own RT0 has no
// SHADER_RESOURCE bind, or its SRV failed). floorFallback: no usable
// display view, so the floor fell back to the contribution's own space.
uint32_t g_holoWindowNoTarget = 0, g_holoWindowFloorFallback = 0;
uint32_t g_holoWindowDeclinedNotCleared = 0, g_holoWindowDeclinedNoPrivate = 0,
         g_holoWindowDeclinedNoProjection = 0, g_holoWindowDeclinedFault = 0;
constexpr uint32_t kHoloPixelSamples = 512;
uint64_t g_holoPixelSamples[kHoloPixelSamples];
uint32_t g_holoPixelSampleCount = 0;
// World markers: draws/frame and the element-depth pass's own occlusion
// samples (holoDepthWindowTick) -- 0 there, with the viewport override in
// place, is the VS-writes-z=0 signature (mechanism ii).
uint32_t g_holoWindowMarkerDraws = 0;
uint64_t g_holoMarkerSamples[kHoloPixelSamples];
uint32_t g_holoMarkerSampleCount = 0;
bool     g_holoScratchFailedNoted = false, g_holoFirstDrawNoted = false, g_holoCanopyRefusedNoted = false;

bool holoIsSrgbFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default: return false;
    }
}

// Lazily (re)created at the eye target's size; a size change drops
// everything, including any pending query, whose result would otherwise
// describe the old target's pixels.
HoloScratch* holoScratchFor(ID3D11DeviceContext* ctx, int eye, uint32_t w, uint32_t h) {
    if (eye < 0 || eye > 1 || !w || !h) return nullptr;
    HoloScratch& s = g_holoScratch[eye];
    if (s.contribTex && s.depthTex && s.radiusTex && s.w == w && s.h == h) return &s;
    if (s.contribSrv) s.contribSrv->Release();
    if (s.contribRtv) s.contribRtv->Release();
    if (s.contribTex) s.contribTex->Release();
    if (s.depthSrv) s.depthSrv->Release();
    if (s.depthDsv) s.depthDsv->Release();
    if (s.depthTex) s.depthTex->Release();
    if (s.radiusDsv) s.radiusDsv->Release();
    if (s.radiusTex) s.radiusTex->Release();
    if (s.targetSrv) s.targetSrv->Release();
    if (s.targetRes) s.targetRes->Release();
    for (uint32_t i = 0; i < kHoloQueryRing; ++i) if (s.occlusion[i]) s.occlusion[i]->Release();
    s = HoloScratch();
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    D3D11_TEXTURE2D_DESC cd{};
    cd.Width = w; cd.Height = h; cd.MipLevels = cd.ArraySize = 1;
    cd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    cd.SampleDesc.Count = 1;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = dev->CreateTexture2D(&cd, nullptr, &s.contribTex);
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(s.contribTex, nullptr, &s.contribRtv);
    if (SUCCEEDED(hr)) hr = dev->CreateShaderResourceView(s.contribTex, nullptr, &s.contribSrv);
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = w; dd.Height = h; dd.MipLevels = dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_R32_TYPELESS;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&dd, nullptr, &s.depthTex);
    if (SUCCEEDED(hr)) {
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
        vd.Format = DXGI_FORMAT_D32_FLOAT;
        vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = dev->CreateDepthStencilView(s.depthTex, &vd, &s.depthDsv);
    }
    if (SUCCEEDED(hr)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = DXGI_FORMAT_R32_FLOAT;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        hr = dev->CreateShaderResourceView(s.depthTex, &vd, &s.depthSrv);
    }
    // The radius scratch is never sampled, only depth-tested -- D16, no
    // shader-resource bind.
    D3D11_TEXTURE2D_DESC rd{};
    rd.Width = w; rd.Height = h; rd.MipLevels = rd.ArraySize = 1;
    rd.Format = DXGI_FORMAT_R16_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Usage = D3D11_USAGE_DEFAULT;
    rd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&rd, nullptr, &s.radiusTex);
    if (SUCCEEDED(hr)) {
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
        vd.Format = DXGI_FORMAT_D16_UNORM;
        vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = dev->CreateDepthStencilView(s.radiusTex, &vd, &s.radiusDsv);
    }
    dev->Release();
    if (FAILED(hr) || !s.contribRtv || !s.contribSrv || !s.depthDsv || !s.depthSrv || !s.radiusDsv) {
        if (s.contribSrv) s.contribSrv->Release();
        if (s.contribRtv) s.contribRtv->Release();
        if (s.contribTex) s.contribTex->Release();
        if (s.depthSrv) s.depthSrv->Release();
        if (s.depthDsv) s.depthDsv->Release();
        if (s.depthTex) s.depthTex->Release();
        if (s.radiusDsv) s.radiusDsv->Release();
        if (s.radiusTex) s.radiusTex->Release();
        s = HoloScratch();
        if (!g_holoScratchFailedNoted) {
            g_holoScratchFailedNoted = true;
            Log::get().note("hologram depth: the %ux%u scratch targets could not be made "
                            "(0x%08lX); the generic hologram/icon coverage is declined for eye %d.",
                            w, h, static_cast<unsigned long>(hr), eye);
        }
        return nullptr;
    }
    s.w = w;
    s.h = h;
    return &s;
}

// The cockpit-radius admission test, shared by both per-draw passes: runs
// once at the first listed draw of the eye each frame, from whichever of
// the two Begins gets there first. radiusDepth is the reversed-Z device
// value AT the cockpit radius; the radius scratch clears to it, so a plain
// GREATER test against that scratch decides "nearer than the radius" for
// the cockpit families' contribution pass, with no metres conversion past
// this point. The element-depth scratch clears to 0 (reversed-Z far)
// instead: a world marker's own depth, at any range, must survive here,
// and a cockpit family's far fragment surviving too is harmless -- its
// contribution is still radius-gated above, so it stays at E=0 -- a bright
// far fragment fails the resolve's floor/share regardless, and a dark one
// fails its cockpitRange test (below): both reject it (docs\hologram-
// depth-2026-09-24.md, the world-marker entry). radiusDepth is kept on
// the scratch too, for that same cockpitRange test at the resolve.
bool holoScratchPrepare(ID3D11DeviceContext* ctx, int eye, uint32_t w, uint32_t h) {
    HoloScratch* sp = holoScratchFor(ctx, eye, w, h);
    if (!sp) return false;
    HoloScratch& s = *sp;
    if (s.preparedFrame == g_frame) return true;
    const float radiusDepth = temporalPassDepthAt(g_cockpitMetres);
    if (!(radiusDepth > 0.0f)) {
        if (s.declinedProjectionFrame != g_frame) {
            s.declinedProjectionFrame = g_frame;
            ++g_holoWindowDeclinedNoProjection;
        }
        return false;
    }
    const FLOAT zero[4]{};
    vScreenClearRenderTargetViewRaw(ctx, s.contribRtv, zero);
    ctx->ClearDepthStencilView(s.depthDsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
    ctx->ClearDepthStencilView(s.radiusDsv, D3D11_CLEAR_DEPTH, radiusDepth, 0);
    s.preparedFrame = g_frame;
    s.radiusDepth = radiusDepth;
    return true;
}

ID3D11DepthStencilState* holoContribDss(ID3D11DeviceContext* ctx) {
    if (g_holoContribDss) return g_holoContribDss;
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_GREATER;
    d.StencilEnable = FALSE;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    const HRESULT hr = dev->CreateDepthStencilState(&d, &g_holoContribDss);
    dev->Release();
    if (FAILED(hr)) g_holoContribDss = nullptr;
    return g_holoContribDss;
}

ID3D11DepthStencilState* holoWorldMarkerDss(ID3D11DeviceContext* ctx) {
    if (g_holoWorldMarkerDss) return g_holoWorldMarkerDss;
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = FALSE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.StencilEnable = FALSE;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    const HRESULT hr = dev->CreateDepthStencilState(&d, &g_holoWorldMarkerDss);
    dev->Release();
    if (FAILED(hr)) g_holoWorldMarkerDss = nullptr;
    return g_holoWorldMarkerDss;
}

ID3D11DepthStencilState* holoElementDepthState(ID3D11DeviceContext* ctx) {
    if (g_holoElementDss) return g_holoElementDss;
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = D3D11_COMPARISON_GREATER;
    d.StencilEnable = FALSE;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    const HRESULT hr = dev->CreateDepthStencilState(&d, &g_holoElementDss);
    dev->Release();
    if (FAILED(hr)) g_holoElementDss = nullptr;
    return g_holoElementDss;
}

ID3D11RasterizerState* holoResolveRs(ID3D11DeviceContext* ctx) {
    if (g_holoResolveRs) return g_holoResolveRs;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    const HRESULT hr = dev->CreateRasterizerState(&rd, &g_holoResolveRs);
    dev->Release();
    if (FAILED(hr)) g_holoResolveRs = nullptr;
    return g_holoResolveRs;
}

// A factor that reads the DESTINATION is meaningless against an
// accumulation buffer that starts at zero every frame; mapped to ONE, so
// it still counts as "this draw's own colour, in full". Every other
// factor -- including SRC1_* and BLEND_FACTOR -- passes through
// unchanged, and is bound with the game's own blend factor and sample
// mask so BLEND_FACTOR still means what the game meant by it.
D3D11_BLEND holoMapSrcBlend(D3D11_BLEND f) {
    switch (f) {
        case D3D11_BLEND_DEST_COLOR:
        case D3D11_BLEND_INV_DEST_COLOR:
        case D3D11_BLEND_DEST_ALPHA:
        case D3D11_BLEND_INV_DEST_ALPHA:
            return D3D11_BLEND_ONE;
        default: return f;
    }
}
// The contribution pass's blend: the game's own (mapped) SrcBlend, dest
// ONE, op ADD, so RGB accumulates exactly what the game's blend equation
// would have ADDED to an untouched destination -- an opaque draw (the
// game's blend disabled) contributes its raw colour in full. Alpha is a
// diagnostic max, never read by the resolve. Write mask is the game's own
// RGB bits (never inventing a channel the game itself does not write)
// plus alpha (this pass's own diagnostic channel, independent of the
// game's alpha write). IndependentBlendEnable and AlphaToCoverage stay
// off: one target, no coverage.
ID3D11BlendState* holoContribBlendFor(ID3D11DeviceContext* ctx, BOOL enable, D3D11_BLEND srcBlend,
                                      UINT8 writeMask, bool* uncached) {
    *uncached = false;
    const D3D11_BLEND effectiveSrc = enable ? srcBlend : D3D11_BLEND_ONE;
    for (auto& e : g_holoContribBlendCache) {
        if (e.state && e.srcBlend == effectiveSrc && e.writeMask == writeMask) return e.state;
    }
    D3D11_BLEND_DESC bd{};
    bd.AlphaToCoverageEnable = FALSE;
    bd.IndependentBlendEnable = FALSE;
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = effectiveSrc;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
    bd.RenderTarget[0].RenderTargetWriteMask = writeMask;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    ID3D11BlendState* state = nullptr;
    const HRESULT hr = dev->CreateBlendState(&bd, &state);
    dev->Release();
    if (FAILED(hr) || !state) return nullptr;
    for (auto& e : g_holoContribBlendCache) {
        if (!e.state) {
            e.srcBlend = effectiveSrc;
            e.writeMask = writeMask;
            e.state = state;
            return state;
        }
    }
    *uncached = true;
    return state;
}

// The resolve's full-screen triangle, entirely from SV_VertexID -- no
// vertex buffer, no input layout (eye_mask.cpp's ring uses the same
// SV_VertexID trick for a different shape) -- clockwise, so it survives
// the default (and the resolve's own, CULL_NONE either way) rasterizer
// state without relying on a y-flip.
constexpr char kHoloResolveVsHlsl[] =
    "float4 main(uint id : SV_VertexID) : SV_POSITION {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";
// Two reads of the frame, for two questions. The holograms draw into the
// HDR scene target before tonemapping, so their light is compared with
// that target and never with the tonemapped image: over sky in eye_155832
// the two differ about 3.6x (HDR luma p50 0.078, displayed 0.273).
//   Target (t2): the game's OWN RT0 resource -- the same one this pass's
//   contribution mirrors the blend of -- read back through an SRV in
//   THAT RTV's own view format, so it decodes in the SAME space the
//   accumulation happened in (sRGB view: linear; float HDR: raw float).
//   The SHARE test (share of the pixel's light this element supplies)
//   compares against this, unconverted on either side.
//   Display (t3): the actual submitted, tonemapped image (temporal_pass's
//   inSrv) -- what is actually on screen. The FLOOR test (must the pixel
//   be visibly lit) compares against this, encoded to display space only
//   if its own view is sRGB (a plain view is display-encoded already).
// Either can be absent (an unviewable target; a missing or wrong-sized
// display image); each absence is handled on its own, never disqualifying
// the other test. Without a display image the floor falls back to the
// contribution's own space (linearBlend: the TARGET's view was sRGB).
// ElementDepth clears to 0 (reversed-Z far), so d>0 alone only rejects a
// pixel nothing listed drew into -- a world marker's own depth survives
// it at any range, and so does a cockpit family's far fragment (the sun's
// corona), harmlessly: cockpitRange (radiusDepth, below) is false for it.
//
// A dark pixel (the displayed colour never clears the floor) inside a
// cockpit-range element's own footprint still takes its depth, skipping
// the share test: it is the gap between glyphs on a panel, or between
// rows of text, and giving it the sky's depth instead of its own panel's
// is what made rolling text blur (flight 20260924_175113/20260925_050051
// -- the gaps carry the sky's motion, glyphs the panel's, and a natural
// near-zero-world-motion frame in the same dump read crisp because the
// two motions briefly matched). Far dark fragments are excluded by
// cockpitRange, not the floor, so the corona still cannot claim dark
// pixels (the bracket-history regression of 2026-09-09 this guards
// against). A dark pixel skips the share test only when cockpitRange
// admits it; a bright one still needs the share it always did, so a
// bright background showing through a translucent gap (a star, a lit
// station behind a panel) is still excluded on its own light, not the
// element's.
constexpr char kHoloResolvePsHlsl[] =
    "Texture2D<float4> Contribution : register(t0);\n"
    "Texture2D<float> ElementDepth : register(t1);\n"
    "Texture2D<float4> Target : register(t2);\n"
    "Texture2D<float4> Display : register(t3);\n"
    "cbuffer HoloResolveCB : register(b0) { float floorValue; float share; uint flags; float radiusDepth; };\n"
    "float3 srgbEncode(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0/2.4) - 0.055; }\n"
    "void main(float4 pos : SV_POSITION, out float depth : SV_Depth) {\n"
    "    int3 p = int3(int2(pos.xy), 0);\n"
    "    float d = ElementDepth.Load(p);\n"
    "    if (!(d > 0.0)) discard;\n"
    "    float3 e = max(Contribution.Load(p).rgb, 0.0);\n"
    "    bool linearBlend = (flags & 1u) != 0;\n"
    "    bool haveTarget = (flags & 2u) != 0;\n"
    "    bool haveDisplay = (flags & 4u) != 0;\n"
    "    bool displaySrgb = (flags & 8u) != 0;\n"
    "    float3 dDisplay;\n"
    "    if (haveDisplay) {\n"
    "        float3 disp = Display.Load(p).rgb;\n"
    "        dDisplay = displaySrgb ? srgbEncode(disp) : saturate(disp);\n"
    "    } else {\n"
    "        dDisplay = linearBlend ? srgbEncode(e) : saturate(e);\n"
    "    }\n"
    "    bool dark = max(max(dDisplay.r, dDisplay.g), dDisplay.b) <= floorValue;\n"
    "    bool cockpitRange = d > radiusDepth;\n"
    "    if (dark && !cockpitRange) discard;\n"
    "    if (!dark && haveTarget) {\n"
    "        float3 f = max(Target.Load(p).rgb, 0.0);\n"
    "        float lumaE = dot(e, float3(0.299, 0.587, 0.114));\n"
    "        float lumaF = dot(f, float3(0.299, 0.587, 0.114));\n"
    "        if (lumaE < share * lumaF) discard;\n"
    "    }\n"
    "    depth = d;\n"
    "}\n";

bool holoResolveShaders(ID3D11DeviceContext* ctx, ID3D11VertexShader** vsOut,
                        ID3D11PixelShader** psOut) {
    if (!g_holoResolveTried) {
        g_holoResolveTried = true;
        g_holoResolveVs = shaderSwapCompileVs(ctx, kHoloResolveVsHlsl, sizeof(kHoloResolveVsHlsl) - 1,
                                              "main", "ui_depth_holo_resolve_vs", nullptr,
                                              "hologram depth");
        g_holoResolvePs = shaderSwapCompilePs(ctx, kHoloResolvePsHlsl, sizeof(kHoloResolvePsHlsl) - 1,
                                              "main", "ui_depth_holo_resolve_ps", nullptr,
                                              "hologram depth");
        if (!g_holoResolveVs || !g_holoResolvePs) {
            Log::get().note("hologram depth: the resolve shader could not be built; the "
                            "generic hologram/icon coverage never reaches the private depth copy.");
        }
    }
    *vsOut = g_holoResolveVs;
    *psOut = g_holoResolvePs;
    return g_holoResolveVs && g_holoResolvePs;
}

ID3D11Buffer* holoResolveCb(ID3D11DeviceContext* ctx) {
    if (g_holoResolveCbBuf) return g_holoResolveCbBuf;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(HoloResolveCb);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    const HRESULT hr = dev->CreateBuffer(&bd, nullptr, &g_holoResolveCbBuf);
    dev->Release();
    if (FAILED(hr)) g_holoResolveCbBuf = nullptr;
    return g_holoResolveCbBuf;
}

// A free ring slot for a new occlusion query, or null when every slot is
// still awaiting its GPU result -- never waited on; the census simply
// samples fewer eye-frames that window (holoDepthWindowTick).
ID3D11Query* holoAcquireQuery(ID3D11DeviceContext* ctx, HoloScratch& s) {
    for (uint32_t i = 0; i < kHoloQueryRing; ++i) {
        const uint32_t idx = (s.occlusionNext + i) % kHoloQueryRing;
        if (s.occlusionPending[idx]) continue;
        if (!s.occlusion[idx]) {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_OCCLUSION;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return nullptr;
            const HRESULT hr = dev->CreateQuery(&qd, &s.occlusion[idx]);
            dev->Release();
            if (FAILED(hr)) { s.occlusion[idx] = nullptr; continue; }
        }
        s.occlusionNext = (idx + 1) % kHoloQueryRing;
        return s.occlusion[idx];
    }
    return nullptr;
}
void holoQueryBegan(HoloScratch& s, ID3D11Query* q) {
    for (uint32_t i = 0; i < kHoloQueryRing; ++i) if (s.occlusion[i] == q) s.occlusionPending[i] = true;
}
// The same ring shape, kept separate from the pair above: a world
// marker's element-depth pass, not the resolve, and its own sample array.
ID3D11Query* holoAcquireMarkerQuery(ID3D11DeviceContext* ctx, HoloScratch& s) {
    for (uint32_t i = 0; i < kHoloQueryRing; ++i) {
        const uint32_t idx = (s.markerOcclusionNext + i) % kHoloQueryRing;
        if (s.markerOcclusionPending[idx]) continue;
        if (!s.markerOcclusion[idx]) {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_OCCLUSION;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return nullptr;
            const HRESULT hr = dev->CreateQuery(&qd, &s.markerOcclusion[idx]);
            dev->Release();
            if (FAILED(hr)) { s.markerOcclusion[idx] = nullptr; continue; }
        }
        s.markerOcclusionNext = (idx + 1) % kHoloQueryRing;
        return s.markerOcclusion[idx];
    }
    return nullptr;
}
void holoMarkerQueryBegan(HoloScratch& s, ID3D11Query* q) {
    for (uint32_t i = 0; i < kHoloQueryRing; ++i) if (s.markerOcclusion[i] == q) s.markerOcclusionPending[i] = true;
}
// Poll every pending query, DONOTFLUSH: a ready one feeds the census's
// pixel-count samples, a not-ready one is left for a later frame's poll.
void holoPollQueries(ID3D11DeviceContext* ctx) {
    for (auto& s : g_holoScratch) {
        for (uint32_t i = 0; i < kHoloQueryRing; ++i) {
            if (!s.occlusionPending[i] || !s.occlusion[i]) continue;
            UINT64 pixels = 0;
            const HRESULT hr = ctx->GetData(s.occlusion[i], &pixels, sizeof(pixels),
                                            D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE) continue;
            s.occlusionPending[i] = false;
            if (hr == S_OK && g_holoPixelSampleCount < kHoloPixelSamples)
                g_holoPixelSamples[g_holoPixelSampleCount++] = pixels;
        }
        for (uint32_t i = 0; i < kHoloQueryRing; ++i) {
            if (!s.markerOcclusionPending[i] || !s.markerOcclusion[i]) continue;
            UINT64 samples = 0;
            const HRESULT hr = ctx->GetData(s.markerOcclusion[i], &samples, sizeof(samples),
                                            D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE) continue;
            s.markerOcclusionPending[i] = false;
            if (hr == S_OK && g_holoMarkerSampleCount < kHoloPixelSamples)
                g_holoMarkerSamples[g_holoMarkerSampleCount++] = samples;
        }
    }
}

// Once per 30 s window (uiDepthFrameBoundary): see the state block's
// comment above for why this differs from the neighbouring 20 s line, and
// why it is silent while the key is off.
void holoDepthWindowTick(ID3D11DeviceContext* ctx) {
    if (!detail::g_holoDepthOn) return;
    if (ctx) holoPollQueries(ctx);
    ++g_holoWindowFrames;
    const uint64_t now = GetTickCount64();
    if (g_holoWindowStartMs == 0) g_holoWindowStartMs = now;
    if (now - g_holoWindowStartMs < 30000) return;
    const double seconds = static_cast<double>(now - g_holoWindowStartMs) / 1000.0;
    const double frames = static_cast<double>(g_holoWindowFrames ? g_holoWindowFrames : 1);
    uint64_t sorted[kHoloPixelSamples];
    uint32_t n = g_holoPixelSampleCount < kHoloPixelSamples ? g_holoPixelSampleCount : kHoloPixelSamples;
    memcpy(sorted, g_holoPixelSamples, n * sizeof(uint64_t));
    std::sort(sorted, sorted + n);
    const uint64_t p50 = n ? sorted[n / 2] : 0;
    const uint32_t declined = g_holoWindowDeclinedNotCleared + g_holoWindowDeclinedNoPrivate +
                              g_holoWindowDeclinedNoProjection + g_holoWindowDeclinedFault;
    uint64_t markerSorted[kHoloPixelSamples];
    const uint32_t markerN = g_holoMarkerSampleCount < kHoloPixelSamples ? g_holoMarkerSampleCount : kHoloPixelSamples;
    memcpy(markerSorted, g_holoMarkerSamples, markerN * sizeof(uint64_t));
    std::sort(markerSorted, markerSorted + markerN);
    const uint64_t markerP50 = markerN ? markerSorted[markerN / 2] : 0;
    // One combined note, not two: the rig (and anything else reading the
    // last logged line) expects a single "hologram depth:" note per tick.
    Log::get().note("hologram depth: %.0f s, %u frames, listed draws %.2f/frame, resolved "
                    "eye-frames %u, stamped pixels/eye-frame p50 %llu (occlusion, %u sampled), "
                    "share test skipped %u (no target view), floor on contribution %u (no display "
                    "view), declined %u (%u nothing listed, %u no private copy, %u no projection, "
                    "%u fault); world markers %.2f draws/frame, element-depth samples p50 %llu "
                    "(occlusion, %u sampled).",
                    seconds, g_holoWindowFrames, static_cast<double>(g_holoWindowListed) / frames,
                    g_holoWindowResolved, static_cast<unsigned long long>(p50), n,
                    g_holoWindowNoTarget, g_holoWindowFloorFallback, declined,
                    g_holoWindowDeclinedNotCleared, g_holoWindowDeclinedNoPrivate,
                    g_holoWindowDeclinedNoProjection, g_holoWindowDeclinedFault,
                    static_cast<double>(g_holoWindowMarkerDraws) / frames,
                    static_cast<unsigned long long>(markerP50), markerN);
    g_holoWindowStartMs = now;
    g_holoWindowFrames = g_holoWindowListed = g_holoWindowResolved = 0;
    g_holoWindowNoTarget = g_holoWindowFloorFallback = 0;
    g_holoWindowDeclinedNotCleared = g_holoWindowDeclinedNoPrivate = 0;
    g_holoWindowDeclinedNoProjection = g_holoWindowDeclinedFault = 0;
    g_holoPixelSampleCount = 0;
    g_holoWindowMarkerDraws = 0;
    g_holoMarkerSampleCount = 0;
}

void holoDepthShutdownImpl() {
    for (auto& s : g_holoScratch) {
        if (s.contribSrv) s.contribSrv->Release();
        if (s.contribRtv) s.contribRtv->Release();
        if (s.contribTex) s.contribTex->Release();
        if (s.depthSrv) s.depthSrv->Release();
        if (s.depthDsv) s.depthDsv->Release();
        if (s.depthTex) s.depthTex->Release();
        if (s.radiusDsv) s.radiusDsv->Release();
        if (s.radiusTex) s.radiusTex->Release();
        if (s.targetSrv) s.targetSrv->Release();
        if (s.targetRes) s.targetRes->Release();
        for (auto* q : s.occlusion) if (q) q->Release();
        for (auto* q : s.markerOcclusion) if (q) q->Release();
        s = HoloScratch();
    }
    if (g_holoElementQuery) { g_holoElementQuery = nullptr; }   // owned by a HoloScratch ring, just released above
    g_holoSavedViewportCount = 0;
    g_holoWorldMarkerNoted = false;
    g_holoWindowMarkerDraws = 0;
    g_holoMarkerSampleCount = 0;
    for (auto& e : g_holoContribBlendCache) { if (e.state) { e.state->Release(); e.state = nullptr; } }
    if (g_holoContribDss) { g_holoContribDss->Release(); g_holoContribDss = nullptr; }
    if (g_holoWorldMarkerDss) { g_holoWorldMarkerDss->Release(); g_holoWorldMarkerDss = nullptr; }
    if (g_holoElementDss) { g_holoElementDss->Release(); g_holoElementDss = nullptr; }
    if (g_holoResolveRs) { g_holoResolveRs->Release(); g_holoResolveRs = nullptr; }
    if (g_holoResolveVs) { g_holoResolveVs->Release(); g_holoResolveVs = nullptr; }
    if (g_holoResolvePs) { g_holoResolvePs->Release(); g_holoResolvePs = nullptr; }
    g_holoResolveTried = false;
    if (g_holoResolveCbBuf) { g_holoResolveCbBuf->Release(); g_holoResolveCbBuf = nullptr; }
    g_holoScratchFailedNoted = g_holoFirstDrawNoted = false;
    g_holoWindowStartMs = 0;
    g_holoWindowFrames = g_holoWindowListed = g_holoWindowResolved = 0;
    g_holoWindowNoTarget = g_holoWindowFloorFallback = 0;
    g_holoWindowDeclinedNotCleared = g_holoWindowDeclinedNoPrivate = 0;
    g_holoWindowDeclinedNoProjection = g_holoWindowDeclinedFault = 0;
    g_holoPixelSampleCount = 0;
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
    bd.IndependentBlendEnable = TRUE;
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
    bd.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    bd.RenderTarget[2].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
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
    slotIndex = slotIndex < 0 ? 0 : (slotIndex > kChromeFloorSlot ? kChromeFloorSlot : slotIndex);
    FloorCb& slot = g_floorCbs[slotIndex];
    const float nearDepth = temporalPassDepthAt(1.0f);
    const float depthAt2 = temporalPassDepthAt(2.0f);
    const bool smoke = slotIndex == 3;
    // The scanner's chrome writes down to one alpha step (g_floorCbs says
    // why); it is the interface proper otherwise, floating like slot 0.
    const bool chrome = slotIndex == kChromeFloorSlot;
    const float alphaFloor = chrome ? (g_alphaFloor < 1.0f / 255.0f ? g_alphaFloor : 1.0f / 255.0f) : g_alphaFloor;
    if (slot.cb && slot.floor == alphaFloor && slot.strength == strength && slot.nearDepth == nearDepth &&
        slot.cockpitMetres == g_cockpitMetres &&
        slot.depthAt2 == depthAt2 &&
        (!smoke || (slot.smokeFloor == g_smokeFloor && slot.smokeMax == g_smokeReactive))) {
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
    // Low two bits distinguish floating UI, attached UI and smoke. The
    // parity still selects motion; the upper six bits carry fixed bias.
    const int q = static_cast<int>(strength * 63.0f + 0.5f);
    const int qRide = 4*q+2;
    const int qFloat = 4*q+1;
    const float ride = static_cast<float>(qRide) / 255.0f;
    const float flt = static_cast<float>(qFloat) / 255.0f;
    // The smoke's slot carries its own floor in z and the mask's strength at
    // full opacity in w (kSmokeDepthHlsl quantises); the others as before.
    // The second float4 is the pass's projection pair (depth = a + b / metres,
    // temporalPassDepthAt), from the values at one and two metres, for the
    // smoke's encoding of its own view depth; zero when no projection is
    // known, and the smoke's shader falls back to the raster's z.
    const float projB = 2.0f * (nearDepth - depthAt2);
    const float projA = nearDepth - projB;
    const float data[8] = {alphaFloor, (slotIndex <= 0 || chrome) ? flt : ride, smoke ? g_smokeFloor : nearDepth,
                           smoke ? g_smokeReactive : flt, projA, projB, g_cockpitMetres, 0.0f};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(data);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = data;
    const HRESULT hr = dev->CreateBuffer(&bd, &sd, &slot.cb);
    dev->Release();
    if (FAILED(hr)) slot.cb = nullptr;
    slot.floor = alphaFloor;
    slot.cockpitMetres = g_cockpitMetres;
    slot.strength = strength;
    slot.nearDepth = nearDepth;
    slot.smokeFloor = g_smokeFloor;
    slot.smokeMax = g_smokeReactive;
    slot.depthAt2 = depthAt2;
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

// advanced.temporal_aa_hologram_depth/_families/_floor, read from
// uiDepthConfigure (g_cockpitMetres is already current by the time it
// calls this). advanced.ui_depth_exclude applies here too, through the
// shared uiDepthIsExcluded the classifier below calls.
// Pure past its string argument (no Config dependency), so the rig can
// drive it directly: the built-in families plus spec's extras, minus the
// canopy (kHoloCanopy, above -- refused and named once) and duplicates.
uint32_t holoBuildFamilyList(const std::string& extraSpec, uint64_t* out, uint32_t cap) {
    uint32_t count = 0;
    if (cap > 0) out[count++] = kHoloPanel;
    for (uint64_t built : kHoloFamiliesBuiltIn) {
        if (count < cap) out[count++] = built;
    }
    uint64_t extra[kMaxHashes];
    uint32_t extraCount = 0;
    parseHashes(extraSpec, extra, &extraCount, kMaxHashes, "advanced.temporal_aa_hologram_families");
    for (uint32_t i = 0; i < extraCount && count < cap; ++i) {
        if (extra[i] == kHoloCanopy) {
            if (!g_holoCanopyRefusedNoted) {
                g_holoCanopyRefusedNoted = true;
                Log::get().note("hologram depth: advanced.temporal_aa_hologram_families names the "
                                "canopy (vs %016llX); refused -- it sits in front of the whole sky, "
                                "and covering it would smear the stars behind it.",
                                static_cast<unsigned long long>(kHoloCanopy));
            }
            continue;
        }
        if (!inList(out, count, extra[i])) out[count++] = extra[i];
    }
    return count;
}

// The world-marker list (kHoloWorldMarkers, above): fixed, no Config, no
// advanced.temporal_aa_hologram_families extras -- a world marker's whole
// point is that it is not one of the (radius-clipped) cockpit families,
// so folding user-added extras into it would let a mistaken hash skip the
// radius test entirely. Pure, like holoBuildFamilyList, so the rig can
// drive it directly.
uint32_t holoWorldMarkerList(uint64_t* out, uint32_t cap) {
    uint32_t count = 0;
    for (uint64_t marker : kHoloWorldMarkers) {
        if (count < cap) out[count++] = marker;
    }
    return count;
}

void holoDepthConfigure(Config& cfg) {
    const bool on = cfg.getBool("advanced.temporal_aa_hologram_depth", true);
    uint64_t fam[kMaxHashes];
    const uint32_t famCount = holoBuildFamilyList(
        cfg.getString("advanced.temporal_aa_hologram_families", ""), fam, kMaxHashes);
    uint64_t world[kMaxHashes];
    const uint32_t worldCount = holoWorldMarkerList(world, kMaxHashes);
    float floor = cfg.getFloat("advanced.temporal_aa_hologram_floor", 0.05f);
    if (!std::isfinite(floor) || floor < 0.0f) floor = 0.0f;
    if (floor > 1.0f) floor = 1.0f;
    float share = cfg.getFloat("advanced.temporal_aa_hologram_share", 0.5f);
    if (!std::isfinite(share) || share < 0.0f) share = 0.0f;
    if (share > 1.0f) share = 1.0f;
    const bool changed = on != detail::g_holoDepthOn || famCount != g_holoFamilyCount ||
                         floor != g_holoFloor || share != g_holoShare ||
                         memcmp(fam, g_holoFamilies, famCount * sizeof(uint64_t)) != 0;
    detail::g_holoDepthOn = on;
    g_holoFamilyCount = famCount;
    memcpy(g_holoFamilies, fam, famCount * sizeof(uint64_t));
    g_holoFloor = floor;
    g_holoShare = share;
    if (changed) {
        Log::get().note("hologram depth: %s -- %u cockpit famil%s, %u world marker%s, "
                        "floor %.3f (display brightness), share %.2f, cockpit radius %.0f m.",
                        on ? "on" : "off", famCount, famCount == 1 ? "y" : "ies",
                        worldCount, worldCount == 1 ? "" : "s",
                        static_cast<double>(floor), static_cast<double>(share),
                        static_cast<double>(g_cockpitMetres));
    }
}

void releaseStates() {
    if (g_overlayDss) { g_overlayDss->Release(); g_overlayDss = nullptr; }
    if (g_reissueDss) {
        g_reissueDss->Release();
        g_reissueDss = nullptr;
    }
    g_reissueDssFailedNoted = false;
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

// fix.ui_quality's two questions (ui_depth.h says why they live here).
// Neither writes this pass's per-draw state; eyeIndexFor does register a
// new target in the frame's table, exactly as the pass's own classifier
// would at the same draw.
int uiDepthSampledSurfaceSlot() {
    if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown) return -1;
    static const BindSlot kSlots[4] = {BindSlot::PsSrv0, BindSlot::PsSrv1,
                                       BindSlot::PsSrv2, BindSlot::PsSrv3};
    for (int i = 0; i < 4; ++i) {
        if (viewIsSurface(bindingGet(kSlots[i]))) return i;
    }
    return -1;
}

int uiDepthEyeOfTarget(const void* res, uint32_t w, uint32_t h, uint32_t fmt) {
    if (!res) return -1;
    int eye = eyeIndexFor(res, w, h, fmt);
    if (eye >= 0 && g_eyesSwapped) eye = 1 - eye;
    return eye;
}

bool uiDepthIsExcluded(uint64_t vsHash) {
    return vsHash && inList(g_exclude, g_excludeCount, vsHash);
}

void uiDepthConfigure(Config& cfg) {
    float cockpit = cfg.getFloat("advanced.temporal_aa_ship_metres", kTemporalShipMetres);
    if (!std::isfinite(cockpit) || cockpit < 0) cockpit = 0;
    g_cockpitMetres = cockpit > 100000.0f ? 100000.0f : cockpit;
    // UI and smoke depth are required inputs of every temporal mode.
    const std::string aa = cfg.getString("fix.temporal_aa", "off");
    g_passOn = temporalModeEnabled(aa);
    g_keyOn = g_passOn;
    // The legacy fixed bias is NVIDIA-only. Coverage and adaptive history
    // are used by every external, trained engine (NVIDIA's or AMD's).
    g_trained = temporalExternalEngine(aa);
    // The direct list: the flight HUD built in, the ini's additions after.
    g_familyCount = 0;
    g_families[g_familyCount++] = kFlightHud;
    g_smokeOn = g_passOn;
    {
        float f = cfg.getFloat("advanced.temporal_aa_smoke_floor", 0.08f);
        if (f < 0.01f) f = 0.01f;
        if (f > 0.9f) f = 0.9f;
        float r = cfg.getFloat("advanced.temporal_aa_smoke_reactive", 0.0f);
        if (r < 0.0f) r = 0.0f;
        if (r > 1.0f) r = 1.0f;
        if (f != g_smokeFloor || r != g_smokeReactive) {
            Log::get().note("ui depth: the smoke's coverage writes depth above %.0f%% opacity and marks the mask %s "
                            "(advanced.temporal_aa_smoke_floor, advanced.temporal_aa_smoke_reactive).",
                            static_cast<double>(f) * 100.0,
                            r > 0.0f ? "up to the strength with its opacity" : "at one quantum, as good as unmarked");
        }
        g_smokeFloor = f;
        g_smokeReactive = r;
    }
    if (g_smokeOn) g_families[g_familyCount++] = kSmokeVs;   // the drives' smoke, a direct family (kSmokeVs says)
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
        float r = cfg.getFloat("advanced.ui_depth_reactive", 0.0f);
        if (!(r >= 0.0f)) r = 0.0f;
        if (r > 1.0f) r = 1.0f;
        if (r != g_reactive) {
            const bool was = g_reactive > 0.0f;
            g_reactive = r;
            if (r > 0.0f) {
                Log::get().note("ui depth: fixed NVIDIA UI bias %.2f; adaptive "
                                "history additionally detects UI changes. At 1, "
                                "fixed bias prevents stable UI accumulating.",
                                static_cast<double>(r));
            } else if (was) {
                Log::get().note("ui depth: fixed NVIDIA UI bias is zero; motion "
                                "classification and adaptive UI history remain active.");
            }
        }
    }
    {
        float t = cfg.getFloat("advanced.ui_ghost_tolerance", 12.0f);
        if (!std::isfinite(t)) t = 12.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 64.0f) t = 64.0f;
        g_ghostTolerance = t;
    }
    {
        float lvl = cfg.getFloat("advanced.corona_smear_level", 64.0f);
        if (!std::isfinite(lvl)) lvl = 64.0f;
        if (lvl < 0.0f) lvl = 0.0f;
        if (lvl > 255.0f) lvl = 255.0f;
        g_coronaSmearLevel = lvl;
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
    // Rebuild the private coverage depth state when its test changes.
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
    holoDepthConfigure(cfg);

    const bool was = detail::g_uiDepthOn;
    detail::g_uiDepthOn = g_keyOn && g_passOn;
    if (detail::g_uiDepthOn && !was) {
        g_announced = false;
        g_waitingNoted = false;
        resetWindow();
        Log::get().note("ui depth: ON -- the interface's composites and the flight HUD "
                        "will write their depth for the temporal pass (%u direct "
                        "famil%s, %u excluded, alpha floor %.2f). It says so again "
                        "when the first frame writes.",
                        g_familyCount, g_familyCount == 1 ? "y" : "ies",
                        g_excludeCount, static_cast<double>(g_alphaFloor));
    } else if (!detail::g_uiDepthOn && was) {
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


// The scanner's chrome (docs/fss-panel.md): two persistent 3408x1917
// surfaces updated damage-style, a draw or two a frame across four
// families of which one is the GUI renderer's, composited into the eye
// by the general world-quad pipeline sampling them at PS slot 1. The
// chrome tracker (vscreen.cpp) recognises the composite by content hash
// and the surface's size before this module sees the draw, and hands the
// surface over here: learned if the offscreen learner above had not
// (it asks a new target's vertex shader sixty-four times, and the graph's
// labels are drawn once), and held for the frame either way.
//
// The flight of b43e3ce (eye dump 190129, docs/fss-scanner.md) found the
// learn idle: both strata were already learned, both composites
// classified and reissued, and what left the graph's strokes and its
// "FILTERED SPECTRAL ANALYSIS" unmasked was the alpha floor -- in two
// dumps the mask is a clean threshold on the composite's brightness
// (luma 80 and up masked, under 60 never), the signature of clip(a -
// floor) on a stratum drawn dim. So this says which way it went, once
// per outcome, distinguishable from a learn; and the surfaces go out
// with an eye run (Chrome0/Chrome1) so the strokes' alpha is read off
// the chrome itself rather than inferred through the composite. Read
// (eye_194158_Chrome0): the dim strokes sit at alpha 51-77 of 255, the
// labels at 166 and up, nothing else is non-zero -- hence the held
// surface's composites take the one-step floor (kChromeFloorSlot).
// The tracker also matches the loading screen's panel (same pipeline,
// same size class), so a loader's dialog gets the one-step floor too:
// under fix.loading_dim = screen (the default) the intro's scrim is
// withheld and nothing translucent is in that surface; where the stock
// scrim draws (loading_dim = stock, or a loader after the intro) its 40%
// over the ship model writes depth for that dialog's duration -- the
// case the general floor was set for, brief and head-locked here.
uint32_t g_chromeSaid = 0;   // outcome bits, each said once
bool uiDepthLearnScannerChrome(ID3D11DeviceContext*, uint64_t vs,
                               ID3D11ShaderResourceView* view, ID3D11Resource* chrome) {
    if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown || !chrome) return false;
    // Held for the frame: an eye run staged at the pass copies what is held.
    if (g_chromeHeldCount < 2 && g_chromeHeld[0] != chrome && g_chromeHeld[1] != chrome) {
        chrome->AddRef();
        g_chromeHeld[g_chromeHeldCount++] = chrome;
    }
    ResourceInfo info;
    if (!bindingResolveResource(chrome, &info) || !info.isTexture2D) {
        if (!(g_chromeSaid & 1u)) {
            g_chromeSaid |= 1u;
            Log::get().note("ui depth: the scanner tracker's chrome surface could not be "
                            "described (vs %016llX); not learned here. Said once.",
                            static_cast<unsigned long long>(vs));
        }
        return false;
    }
    if (info.a < 2000 || info.b < 1000 || info.b >= info.a) {
        if (!(g_chromeSaid & 2u)) {
            g_chromeSaid |= 2u;
            Log::get().note("ui depth: the scanner tracker's chrome surface is %ux%u, not the "
                            "chrome's size; not learned here. Said once.", info.a, info.b);
        }
        return false;
    }
    if (surfaceMatches(info)) {
        if (!(g_chromeSaid & 4u)) {
            g_chromeSaid |= 4u;
            Log::get().note("ui depth: the scanner's chrome surface (%ux%u, DXGI format %u) was "
                            "already learned when the tracker offered it (%u surfaces known): "
                            "the composite (vs %016llX) is classified by the ordinary route, and "
                            "while this surface is held its strokes write depth down to one alpha "
                            "step rather than the general floor (%.3f). Said once.",
                            info.a, info.b, info.fmt, g_surfaceCount,
                            static_cast<unsigned long long>(vs), static_cast<double>(g_alphaFloor));
        }
        return false;
    }
    addSurface(info.resource, info.a, info.b, info.fmt, 'C');
    // The view may have been judged "not a surface" this frame or last;
    // the memo answers viewIsSurface for a hundred frames, so overwrite it.
    if (view) g_viewMemo.put(view, g_frame, 1u);
    if (!(g_chromeSaid & 8u)) {
        g_chromeSaid |= 8u;
        Log::get().note("ui depth: the scanner's chrome surface (%ux%u, DXGI format %u) is "
                        "learned from the screen's composite (vs %016llX), which the scanner "
                        "tracker recognised; its strata write depth and the mask from this "
                        "draw on, down to one alpha step while the surface is held, and the "
                        "temporal pass keeps them on the head's path while the scanner is up. "
                        "Said once; a second surface is learned silently.",
                        info.a, info.b, info.fmt, static_cast<unsigned long long>(vs));
    }
    return true;
}

// Does the composite being classified sample a surface the scanner tracker
// handed over this frame? Identity through the view's resource, against
// what is held (released at the frame boundary), so it holds for every
// composite of the chrome in the frame -- both eyes, both strata -- and
// costs nothing on a frame the tracker matched nothing.
bool samplesScannerChrome(int surfaceSlot) {
    if (g_chromeHeldCount == 0 || surfaceSlot < 0) return false;
    static const BindSlot kSlots[4] = {BindSlot::PsSrv0, BindSlot::PsSrv1,
                                       BindSlot::PsSrv2, BindSlot::PsSrv3};
    ResourceInfo info;
    if (!bindingResolve(bindingGet(kSlots[surfaceSlot]), &info) || !info.isTexture2D) return false;
    for (uint32_t i = 0; i < g_chromeHeldCount; ++i) {
        if (info.resource == g_chromeHeld[i]) return true;
    }
    return false;
}

float uiDepthReactive() { return detail::g_uiDepthOn && !detail::g_uiDepthStoodDown ? g_reactive : 0.0f; }

float uiDepthGhostTolerance() { return detail::g_uiDepthOn && !detail::g_uiDepthStoodDown ? g_ghostTolerance : 0.0f; }

float uiDepthCoronaHold() { return detail::g_uiDepthOn && !detail::g_uiDepthStoodDown ? g_coronaSmearLevel / 255.0f : 0.0f; }

void uiDepthNoteOffscreenDraw(ID3D11DeviceContext* ctx) {
    if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown) return;
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
        addSurface(g_rtvRes, g_rtvW, g_rtvH, g_rtvFmt,
                   h == kGuiVector ? 'V' : h == kGuiText ? 'T' : 'I');
        g_rtvKnown = true;
    } else if (g_rtvChecks >= kChecksPerTarget) {
        noteExhausted(g_rtvRes);
    }
}

bool uiDepthOnEyeDraw(ID3D11DeviceContext* ctx, const HoloDraw& draw) {
    detail::g_uiDepthPlanetPending=false;detail::g_uiDepthPlanetSolarPending=false;
    g_coronaPending=false;g_coronaMotion=false;
    g_holoDraw=draw;
    detail::g_uiDepthMode = Mode::kNone;
    g_wantRebind = false;
    g_wantMask = false;
    g_rebindEye = -1;
    g_drawEye = -1;
    g_reissueShader = nullptr;
    g_reissueMaskOffset = 0.0f;   // the interface proper unless the family below says otherwise
    g_reissueMaskSlot = 0;
    if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown) return false;
    // Cheapest first: no depth target, nothing to write (the post chain's
    // fullscreen draws, ten a frame).
    const void* dsv = bindingGet(BindSlot::Dsv0);
    if (!dsv) return false;
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
    const bool exactCorona = h==kSmokeVs && boundPsHash(ctx)==kSmokePs;
    // Opaque planets use the same affine-history consumer as the rings,
    // but neither the UI mask nor its private depth-writing path. Learn
    // the eye from scene depth in Begin: inserting the deferred colour
    // target into the UI colour-target table would misidentify later UI.
    if(h==kPlanetSurfaceVs || h==kSolarSurfaceVs) {
        const uint64_t ps=boundPsHash(ctx);
        if((h==kPlanetSurfaceVs && ps==kPlanetSurfacePs) || (h==kSolarSurfaceVs && ps==kSolarSurfacePs)) {
            detail::g_uiDepthPlanetPending=true;detail::g_uiDepthPlanetSolarPending=h==kSolarSurfaceVs;return false;
        }
    }
    const bool stellar=h==kRingVs || h==kOrbitalVs;
    if (!stellar && !composite && !(h && inList(g_families, g_familyCount, h))) return false;
    if (h && inList(g_exclude, g_excludeCount, h)) return false;

    // WHICH PROJECTION. The cockpit's families bind the scene pair and
    // test against it, so they share the scene's projection and write
    // depth into the private scene copy. A composite not binding the pair is drawn
    // through the interface projection (the menus, the loader, the modals;
    // measured 2026-09-07) and gets the alpha-aware depth pass instead,
    // whichever depth it binds.
    const bool scenePair = dsvIsSceneDepth(dsv);
    const bool sceneFamily = stellar || h == kHoloPanel || h == kHudSprite ||
                             inList(g_families, g_familyCount, h);
    if (scenePair && sceneFamily) {
        g_coronaPending=exactCorona;
        detail::g_uiDepthMode = Mode::kReissueScene;
        const bool wantLine = g_familyLoggedCount < kMaxFamilyLines;
        const bool wantMask = true; // motion classification also needed at zero reactivity and with native TAA
        // Every classified family needs a coverage shader. Unsupported
        // variants are declined, never treated by changing the original draw.
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
        // depth over the station (2026-09-09). Which of their pixels ride a
        // turning body's path is the mask's parity (floorBuffer says).
        const bool hud = h == kFlightHud || h == kHoloPanel || h == kHudSprite ||
                         (g_smokeOn && h == kSmokeVs);
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
        // The smoke is marked at one quantum -- odd, so it keeps the camera's
        // path at its own depth, and as good as unmarked to NVIDIA, since
        // its history is what smooths it.
        g_reissueMaskSlot = h == kFlightHud ? 2 : h == kSmokeVs ? 3 : hud ? 1
                          : samplesScannerChrome(surfaceSlot) ? kChromeFloorSlot : 0;
        g_reissueMaskOffset = h == kFlightHud ? -0.5f * g_reactive
                            : h == kSmokeVs ? (1.0f / 255.0f - g_reactive)
                            : (hud ? -3.0f / 255.0f : 0.0f);
        const uint64_t ph = boundPsHash(ctx);
        DepthShader* shader = depthShaderFor(ctx, ph, h, surfaceSlot);
        // Unknown coverage must not fall back to changing the game's draw.
        if (!shader) {
            ++g_wNoShader;
            noteFamily(h, ph, "no supported coverage shader; game draw left unchanged");
            detail::g_uiDepthMode = Mode::kNone;
            return false;
        }
        g_reissueShader = shader;
        // A supported coverage draw also marks the reactive mask when asked.
        // Both operations use the private depth copy for scene occlusion.
        if (shader) {
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
            noteFamily(h, ph, h == kSmokeVs
                ? "smoke coverage into private depth for AA"
                : h == kFlightHud ? "flight HUD coverage into private scene-depth copy for AA"
                : h == kHudSprite ? "sprite coverage into private scene-depth copy for AA"
                : "interface coverage into private scene-depth copy for AA");
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
    g_wantMask = g_drawEye >= 0;
    detail::g_uiDepthMode = Mode::kReissue;
    g_reissueShader = shader;
    // The scanner's chrome takes one alpha step, not the general floor
    // (g_floorCbs says why); its composites are recognised by the surface
    // they sample, which the scanner tracker held for the frame.
    const bool chrome = samplesScannerChrome(surfaceSlot);
    if (chrome) g_reissueMaskSlot = kChromeFloorSlot;
    noteFamily(h, ph, chrome ? "interface projection, the scanner's chrome; alpha-aware depth down "
                               "to one alpha step into private scene copy for AA"
                             : "interface projection; alpha-aware depth into private scene copy for AA");
    ++g_wComposite;
    return true;
}

bool uiDepthWantsReissue() {
    if (!g_reissueShader) return false;
    // Only supported, classified coverage draws can be reissued.
    return detail::g_uiDepthMode == Mode::kReissue || detail::g_uiDepthMode == Mode::kReissueScene;
}

void uiDepthEnd(ID3D11DeviceContext*) {
    // Also clear classification when the caller skipped or swallowed a draw.
    detail::g_uiDepthMode = Mode::kNone;
    g_reissueShader = nullptr;
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
bool uiDepthPlanetBegin(ID3D11DeviceContext* ctx) {
    const bool pending=detail::g_uiDepthPlanetPending,solar=detail::g_uiDepthPlanetSolarPending;detail::g_uiDepthPlanetPending=false;detail::g_uiDepthPlanetSolarPending=false;
    if(!pending || !detail::g_uiDepthOn || detail::g_uiDepthStoodDown || !ctx || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
       (solar ? (boundVsHash(ctx)!=kSolarSurfaceVs || boundPsHash(ctx)!=kSolarSurfacePs) :
                (boundVsHash(ctx)!=kPlanetSurfaceVs || boundPsHash(ctx)!=kPlanetSurfacePs)))return false;
    auto* dsv=static_cast<ID3D11DepthStencilView*>(bindingGet(BindSlot::Dsv0));if(!dsv)return false;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;dsv->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> scene;
    if(FAILED(resource.As(&scene)) || !depthProbeIsSceneDepth(scene.Get()))return false;
    D3D11_TEXTURE2D_DESC td{};scene->GetDesc(&td);int eye=-1;
    for(int i=0;i<2;++i) {
        ID3D11Texture2D* candidate=nullptr;uint32_t format=0;
        if(depthProbeSceneDepthFormat(td.Width,td.Height,i,&candidate,&format) && candidate==scene.Get()){eye=i;break;}
    }
    if(eye<0)return false;
    if(g_eyesSwapped)eye=1-eye;
    if(!g_planetCoverage.begin(ctx,scene.Get(),g_holoMotion[eye],g_holoDraw,solar))return false;
    if(solar) {
        if(!g_solarNoted) { g_solarNoted=true; Log::get().note("solar motion: exact surface coverage and affine approach motion active; stable art surface identity, original depth visibility, no UI marking or distance cutoff."); }
    } else if(!g_planetNoted) {
        g_planetNoted=true;
        Log::get().note("planet motion: opaque surface coverage and affine approach motion active; original VS/depth visibility, shared 128-record eye budget, no UI marking or distance cutoff.");
    }
    return true;
}
void uiDepthPlanetEnd(ID3D11DeviceContext* ctx) { g_planetCoverage.end(ctx); }

bool uiDepthReissueBegin(ID3D11DeviceContext* ctx) {
    g_stellarCpuActive=-1;
    g_reissueOn = false;
    g_rebound = false;
    if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown || !g_reissueShader) return false;
    const bool depthPass = detail::g_uiDepthMode == Mode::kReissue || detail::g_uiDepthMode == Mode::kReissueScene;   // this draw writes depth
    const bool sceneProjection = detail::g_uiDepthMode == Mode::kReissueScene;   // ...in the scene's own viewport
    const bool wantMask = g_wantMask;
    const bool rebind = g_wantRebind;
    g_wantRebind = false;
    g_wantMask = false;
    if (!depthPass && !wantMask) return false;
    DepthShader* shader = g_reissueShader;
    const int stellar=shader==&g_depthShaders[6]?0:shader==&g_depthShaders[7]?1:-1;
    ID3D11ShaderResourceView* coronaDepth=nullptr;
    if(stellar>=0) {
        ++g_stellarCpu[stellar].calls;
        // Include preparation, the extra draw and restoration. No GPU wait.
        if((g_frame&15u)==0u) {
            // Query creation is diagnostic setup, excluded from CPU samples.
            g_stellarGpu[stellar].begin(ctx);
            g_stellarCpuActive=stellar;g_stellarCpuStart=qpcNow();
        }
    }
    const bool ran = guardedBudget(g_budget, [&] {
        // The depth pass writes depth with the nearer-wins test; a
        // mask-only pass over a family whose depth is already written just
        // marks, with the same test and no writes.
        // Interface overlays whose original draw disables depth testing are
        // visible over the cockpit. A nearer-wins reissue leaves unrelated
        // cockpit depth (and its holo motion) under that visible text. The
        // 20:51 account capture contains 112 such bright profile pixels.
        // Only interface-projection overlays inherit this ordering; scene
        // geometry and depth-tested interfaces retain their visibility test.
        bool overlay = false;
        if (depthPass && !sceneProjection) {
            Microsoft::WRL::ComPtr<ID3D11DepthStencilState> original;
            UINT ref = 0; ctx->OMGetDepthStencilState(&original, &ref);
            if (original) { D3D11_DEPTH_STENCIL_DESC desc{}; original->GetDesc(&desc); overlay = !desc.DepthEnable; }
        }
        // Orbital HC is motion/visibility metadata, not a new private
        // occluder: keep the seeded DSV's GEQUAL test, but never write it.
        const bool orbitalDepth = shader==&g_depthShaders[7];
        ID3D11DepthStencilState* dss = depthPass ? (orbitalDepth ? maskDepthState(ctx) : reissueState(ctx, overlay)) : maskDepthState(ctx);
        ID3D11Buffer* cb = floorBuffer(ctx, g_reissueMaskSlot, g_reissueMaskOffset);
        if (!dss || !cb) {
            ++g_wNoTwin;
            return;
        }
        Mask* mask = wantMask ? maskFor(ctx, g_drawEye, g_rebindW, g_rebindH) : nullptr;
        if (wantMask && !mask && !depthPass) return;   // nothing left to do
        ctx->OMGetRenderTargets(kMaxRtvs, g_savedRtvs, &g_savedDsv);
        ID3D11Texture2D* scene = nullptr;
        if (rebind) {
            uint32_t fmt = 0;
            if (depthProbeSceneDepthFormat(g_rebindW, g_rebindH, g_rebindEye, &scene, &fmt) && scene)
                scene->AddRef();
            else scene = nullptr;
        } else if (g_savedDsv) {
            ID3D11Resource* res = nullptr;
            g_savedDsv->GetResource(&res);
            if (res) {
                res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&scene));
                res->Release();
            }
        }
        ID3D11DepthStencilView* target = nullptr;
        if (g_reissueMaskSlot != 3 && scene && g_drawEye >= 0 && g_drawEye < 2)
            target = g_uiDepth[g_drawEye].acquire(ctx, scene);
        bool holo=false;
        const bool ring=shader==&g_depthShaders[6],orbital=shader==&g_depthShaders[7],sprite=shader==&g_depthShaders[5];
        if(orbital && !g_orbitalVs) g_orbitalVs.Attach(shaderSwapCompileVs(ctx,kOrbitalCoverageVs,sizeof(kOrbitalCoverageVs)-1,"main","orbital coverage",nullptr,"stellar motion"));
        if (target && scene && mask && (holoShader(shader) || sprite || ring || (orbital && g_orbitalVs))) {
            holo=g_holoMotion[g_drawEye].prepare(ctx,scene,g_holoDraw,g_cockpitMetres,ring?1:orbital?2:sprite?3:0,shader->slot);
            if(holo && !g_holoNoted) {
                g_holoNoted=true;
                Log::get().note("holo motion: draw-time cockpit transform history active; 128 draws per eye, pool-slot-independent matching, TAA/DLSS.");
            }
        }
        if (g_reissueScene) { g_reissueScene->Release(); g_reissueScene=nullptr; }
        if (scene) { g_reissueScene=scene; g_reissueScene->AddRef(); scene->Release(); }
        // A stellar coverage draw requires its complete transform/VS path.
        // Never write anonymous depth if that path declined.
        if((ring || orbital) && !holo) { releaseSavedOm(); return; }
        if((ring || orbital) && !g_stellarNoted[orbital?1:0]) {
            g_stellarNoted[orbital?1:0]=true;
            Log::get().note("stellar motion: %s coverage and draw-transform history active; shared 128-record eye budget.",orbital?"orbital line":"opaque ring");
        }
        if (g_reissueMaskSlot != 3 && !target) {
            releaseSavedOm();
            ++g_wNoPair;
            if (!g_privateDepthFailedNoted) {
                g_privateDepthFailedNoted = true;
                Log::get().note("ui depth: private scene copy unavailable; coverage declined, game depth unchanged.");
            }
            return;
        }
        const bool needsSceneDepth = g_reissueMaskSlot == 2 || shader == &g_depthShaders[5];
        ID3D11ShaderResourceView* hudScene = needsSceneDepth ?
            g_uiDepth[g_drawEye].sourceView(ctx) : nullptr;
        if (needsSceneDepth && !hudScene) {
            releaseSavedOm(); ++g_wNoPair;
            if (!g_privateDepthFailedNoted) {
                g_privateDepthFailedNoted = true;
                Log::get().note("ui depth: HUD/sprite scene depth cannot be read; its private coverage is declined.");
            }
            return;
        }
        if (g_reissueMaskSlot != 3 && !g_privateDepthNoted) {
            g_privateDepthNoted = true;
            Log::get().note("ui depth: HUD and interface coverage now writes a PRIVATE scene-depth copy "
                            "for temporal AA only; later game draws keep the original depth (eye %d, %ux%u).",
                            g_drawEye, g_rebindW, g_rebindH);
        }
        if (rebind) ++g_wRebound;
        // The smoke's depth goes to EDVR's own target (SmokeDepth says why),
        // the size of the scene's depth target bound at the draw, cleared
        // once a frame before its first draw. Without one -- no eye for the
        // draw, a multisampled scene depth, a failed make -- the smoke is
        // left to the sky's depth this frame rather than written into the
        // game's.
        if (depthPass && g_reissueMaskSlot == 3) {
            SmokeDepth* sdp = nullptr;
            if (g_savedDsv) {
                ID3D11Resource* res = nullptr;
                g_savedDsv->GetResource(&res);
                ID3D11Texture2D* tex = nullptr;
                if (res) {
                    res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
                    res->Release();
                }
                if (tex) {
                    D3D11_TEXTURE2D_DESC td{};
                    tex->GetDesc(&td);
                    tex->Release();
                    if (td.SampleDesc.Count == 1) sdp = smokeDepthFor(ctx, g_drawEye, td.Width, td.Height);
                }
            }
            if (!sdp) {
                releaseSavedOm();
                ++g_wNoPair;   // no target for the depth pass, the rebind's word for it
                return;
            }
            if (!sdp->written) {
                ctx->ClearDepthStencilView(sdp->dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
                sdp->written = true;
            }
            target = sdp->dsv;
            if (!g_smokeDepthNoted) {
                g_smokeDepthNoted = true;
                Log::get().note("ui depth: the smoke's coverage writes its depth into a %ux%u target "
                                "of EDVR's for eye %d, which the temporal pass folds into the scene's "
                                "as it reads; the game's depth is no longer written under the trail "
                                "(the review of 2026-09-10).",
                                sdp->w, sdp->h, g_drawEye);
            }
            // Corona shares the historical smoke shader, but its exact
            // affine streak layout is a separate mode-5 motion source. The
            // original scene DSV is unbound while the private smoke DSV is
            // active, so a cached read-only SRV can preserve earlier Holo
            // coverage without copying the full scene.
            if (g_coronaPending && scene && mask) {
                coronaDepth=sceneDepthReadView(ctx,scene,g_drawEye);
                if (!g_smokeCoronaShader && !g_smokeCoronaCompileTried) {
                    g_smokeCoronaCompileTried=true;
                    std::string source="#define CORONA_MOTION 1\n";
                    source += kSmokeDepthHlsl;
                    g_smokeCoronaShader.Attach(shaderSwapCompilePs(ctx,source.c_str(),source.size(),"main","ui_depth_corona_ps",nullptr,"ui depth corona"));
                }
                if (coronaDepth && g_smokeCoronaShader && g_holoMotion[g_drawEye].prepare(ctx,scene,g_holoDraw,g_cockpitMetres,5,1)) {
                    holo=true; g_coronaMotion=true;
                    if(!g_coronaNoted) { g_coronaNoted=true; Log::get().note("corona motion: mode-5 affine streak history active; exact VS/PS, stride-44 layout, bounded geometry epochs and class-3 visibility."); }
                }
            }
        }
        const bool surfaceComposite=shader==&g_depthShaders[0] || shader==&g_depthShaders[2] ||
                                    holoShader(shader) || shader==&g_depthShaders[5];
        Mask* edits=nullptr;ID3D11ShaderResourceView* changes=nullptr;
        if(g_trained && mask && surfaceComposite) {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source;
            ctx->PSGetShaderResources(shader->slot,1,&source);
            // A declined source must bind null at t14 too; never sample a
            // game's unrelated binding as UI edit evidence.
            changes=g_uiContent.prepare(ctx,source.Get(),g_frame,(g_frame&15u)==0u);
            if (g_uiContentCensusOn) {
                switch (g_uiContent.lastDecision) {
                    case UiContent::Decision::kHit: ++g_uiContentCensusHits; break;
                    case UiContent::Decision::kReset: ++g_uiContentCensusResets; break;
                    case UiContent::Decision::kUpdated: ++g_uiContentCensusUpdated; break;
                    default: ++g_uiContentCensusDeclined; break;
                }
                g_uiContentCensusEvicted += g_uiContent.lastEvicted;
                if (g_uiContentCensusLines < 64) {
                    ++g_uiContentCensusLines;
                    uint32_t sw = 0, sh = 0, sfmt = 0, sbind = 0;
                    if (source) {
                        D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
                        source->GetDesc(&svd);
                        sfmt = static_cast<uint32_t>(svd.Format);
                        Microsoft::WRL::ComPtr<ID3D11Resource> sres;
                        source->GetResource(&sres);
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> stex;
                        if (sres && SUCCEEDED(sres.As(&stex))) {
                            D3D11_TEXTURE2D_DESC srcTd{};
                            stex->GetDesc(&srcTd);
                            sw = srcTd.Width; sh = srcTd.Height; sbind = srcTd.BindFlags;
                        }
                    }
                    const uint32_t blocksW = (sw + UiContent::kBlock - 1) / UiContent::kBlock;
                    const uint32_t blocksH = (sh + UiContent::kBlock - 1) / UiContent::kBlock;
                    Log::get().note("UI content census: frame %u eye %d vs %016llX ps %016llX source %ux%u "
                                    "fmt %u bind 0x%X blocks %ux%u -> %s (%s), entry age %u frames, evicted %u this call.",
                                    g_frame, g_drawEye, static_cast<unsigned long long>(boundVsHash(ctx)),
                                    static_cast<unsigned long long>(boundPsHash(ctx)), sw, sh, sfmt, sbind,
                                    blocksW, blocksH,
                                    uiContentDecisionWord(g_uiContent.lastDecision),
                                    uiContentReason(g_uiContent.lastDecision, g_uiContent.lastAge),
                                    g_uiContent.lastAge, g_uiContent.lastEvicted);
                }
            }
            if(changes) edits=maskFor(ctx,g_drawEye,g_rebindW,g_rebindH,g_edits);
            if(!edits)changes=nullptr;
            if(edits && !g_contentNoted) {
                g_contentNoted=true;
                Log::get().note("UI content: source-texel edit tracking active for DLSS; 24 surfaces, 64 MiB history cap, 32-frame edit expiry, shared across eyes. Changed/erased glyphs use fresh raster reconstruction; static UI retains DLSS.");
            }
        }
        // Capture every changed binding before any mutation. The failure
        // path uses the same restoration as a completed coverage draw.
        ctx->OMGetDepthStencilState(&g_reSavedDss, &g_reSavedRef);
        g_savedPsClassCount=256;
        ctx->PSGetShader(&g_savedPs, g_savedPsClasses, &g_savedPsClassCount);
        ctx->PSGetConstantBuffers(13, 1, &g_savedFloorCb);
        if(holo) ctx->PSGetConstantBuffers(12,1,&g_savedHoloInfo);
        if(orbital) {
            g_savedOrbitalClassCount=256;
            ctx->VSGetShader(&g_savedOrbitalVs,g_savedOrbitalClasses,&g_savedOrbitalClassCount);
            ctx->VSGetConstantBuffers(12,1,&g_savedOrbitalInfo);
        }
        if (hudScene) ctx->PSGetShaderResources(2, 1, &g_savedHudScene);
        if(surfaceComposite) ctx->PSGetShaderResources(14,1,&g_savedEdits);
        if (mask) ctx->OMGetBlendState(&g_reSavedBlend, g_reSavedBlendFactor, &g_reSavedSampleMask);
        g_reBlendSaved = mask != nullptr;
        g_reissueOn = true;
        g_rebound = true;
        if(g_coronaMotion && scene) {
            if(!coronaDepth) { g_coronaMotion=false; holo=false; }
            else {
                g_sceneDepthBound=true;
                ctx->PSGetShaderResources(2,1,&g_savedSceneDepth);
            }
        }
        // The mask as the only colour target when one is wanted, none
        // otherwise; through the original entry so the binding shadow keeps
        // describing the game's bindings.
        ID3D11RenderTargetView* rtv = mask ? mask->rtv : nullptr;
        ID3D11RenderTargetView* rtvs[3]={rtv,holo?g_holoMotion[g_drawEye].target():nullptr,edits?edits->rtv:nullptr};
        g_reissueRtvCount = edits ? 3 : holo ? 2 : (mask ? 1 : 0);
        for (UINT i=0;i<3;++i) g_reissueRtvs[i]=i<g_reissueRtvCount?rtvs[i]:nullptr;
        g_reissueTarget=target;
        vScreenSetRenderTargetsRaw(ctx, edits?3:holo?2:(mask?1:0), mask?rtvs:nullptr, target);
        if(g_coronaMotion) {
            ctx->PSSetShaderResources(2,1,&coronaDepth);
        }
        if(surfaceComposite) {
            g_editsBound=true;ctx->PSSetShaderResources(14,1,&changes);
            if(edits)edits->marked=true;
        }
        if(holo) {
            g_holoBound=true;
            ID3D11Buffer* info=g_holoMotion[g_drawEye].info(); ctx->PSSetConstantBuffers(12,1,&info);
            if(orbital) { g_orbitalBound=true;ctx->VSSetShader(g_orbitalVs.Get(),nullptr,0);ctx->VSSetConstantBuffers(12,1,&info); }
        }
        if (hudScene) {
            g_hudSceneBound = true;
            ctx->PSSetShaderResources(2, 1, &hudScene);
        }
        if (mask) {
            mask->marked = true;
            ID3D11BlendState* bs = g_coronaMotion ? g_holoMotion[g_drawEye].motionBlend() : maskBlend(ctx);
            const FLOAT one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            if (bs) ctx->OMSetBlendState(bs, one, 0xFFFFFFFFu);
            if (!g_maskNotedOnce) {
                g_maskNotedOnce = true;
                Log::get().note("ui depth: UI motion coverage is being marked at "
                                "%ux%u for eye %d; fixed NVIDIA bias %.2f. "
                                "Floating strokes keep the camera path even at zero "
                                "fixed bias; the temporal pass detects UI changes separately.",
                                g_rebindW, g_rebindH, g_drawEye,
                                static_cast<double>(g_reactive));
            }
        }
        ctx->OMSetDepthStencilState(dss, 0);
        ctx->PSSetShader(g_coronaMotion ? g_smokeCoronaShader.Get() : shader->shader, nullptr, 0);
        ctx->PSSetConstantBuffers(13, 1, &cb);
        if (depthPass && !sceneProjection) scaleViewportsForUi(ctx);
        if (depthPass) {
            ++g_wReissued;
            ++g_wWrote;
            ++g_sessionWrote;
        }
    });
    if (!ran && !g_budget.shouldRun() && !detail::g_uiDepthStoodDown) {
        detail::g_uiDepthStoodDown = true;
        Log::get().note("ui depth: STANDING DOWN for the session -- the depth pass "
                        "faulted repeatedly. The interface draws as the game issues it.");
    }
    if (!ran || !g_reissueOn) uiDepthReissueEnd(ctx);
    return g_reissueOn;
}

void uiDepthReissueEnd(ID3D11DeviceContext* ctx) {
    if (g_reissueOn) {
        g_reissueOn = false;
        guarded("uiDepth.reissueRestore", [&] {
            ctx->PSSetShader(g_savedPs, g_savedPsClasses, g_savedPsClassCount);
            ctx->PSSetConstantBuffers(13, 1, &g_savedFloorCb);
            if(g_holoBound) ctx->PSSetConstantBuffers(12,1,&g_savedHoloInfo);
            if(g_orbitalBound) {
                ctx->VSSetShader(g_savedOrbitalVs.Get(),g_savedOrbitalClasses,g_savedOrbitalClassCount);
                ctx->VSSetConstantBuffers(12,1,g_savedOrbitalInfo.GetAddressOf());
            }
            if (g_hudSceneBound) ctx->PSSetShaderResources(2, 1, &g_savedHudScene);
            if (g_sceneDepthBound) ctx->PSSetShaderResources(2, 1, &g_savedSceneDepth);
            if(g_editsBound) ctx->PSSetShaderResources(14,1,&g_savedEdits);
            ctx->OMSetDepthStencilState(g_reSavedDss, g_reSavedRef);
            if (g_reBlendSaved) {
                ctx->OMSetBlendState(g_reSavedBlend, g_reSavedBlendFactor,
                                     g_reSavedSampleMask);
            }
            restoreViewports(ctx);
        });
        if (g_savedFloorCb) { g_savedFloorCb->Release(); g_savedFloorCb = nullptr; }
        if(g_savedHoloInfo) { g_savedHoloInfo->Release(); g_savedHoloInfo=nullptr; }
        g_holoBound=false;
        g_coronaMotion=false; g_coronaPending=false;
        if(g_sceneDepthBound) { if(g_savedSceneDepth) g_savedSceneDepth->Release(); g_savedSceneDepth=nullptr; g_sceneDepthBound=false; }
        for(UINT i=0;i<g_savedPsClassCount;++i) if(g_savedPsClasses[i]) g_savedPsClasses[i]->Release();
        g_savedPsClassCount=0;
        if(g_orbitalBound) {
            for(UINT i=0;i<g_savedOrbitalClassCount;++i) g_savedOrbitalClasses[i]->Release();
            g_savedOrbitalClassCount=0;g_savedOrbitalVs.Reset();g_savedOrbitalInfo.Reset();g_orbitalBound=false;
        }
        if (g_savedHudScene) { g_savedHudScene->Release(); g_savedHudScene = nullptr; }
        g_hudSceneBound = false;
        if(g_savedEdits){g_savedEdits->Release();g_savedEdits=nullptr;}g_editsBound=false;
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
        g_reBlendSaved = false;
    }
    // Begin may retain the scene and subsequently decline before binding.
    // Its identity must not survive that failed reissue either.
    if (g_reissueScene) { g_reissueScene->Release(); g_reissueScene=nullptr; }
    g_reissueTarget=nullptr;g_reissueRtvCount=0;
    for(auto& target:g_reissueRtvs)target=nullptr;
    if (g_rebound) {
        g_rebound = false;
        guarded("uiDepth.reissueRestore", [&] { restoreOm(ctx); });
        releaseSavedOm();
    }
    detail::g_uiDepthMode = Mode::kNone;
    g_reissueShader = nullptr;
    if(g_stellarCpuActive>=0) {
        auto& sample=g_stellarCpu[g_stellarCpuActive];
        sample.ticks+=qpcNow()-g_stellarCpuStart;++sample.samples;
        g_stellarGpu[g_stellarCpuActive].end(ctx);g_stellarCpuActive=-1;
    }
}

// Classification for the generic hologram/icon depth pass (state block
// above the "GENERIC HOLOGRAM/ICON DEPTH COVERAGE" comment). Runs beside
// uiDepthOnEyeDraw at the same vscreen.cpp call site rather than inside
// it: this mechanism needs no coverage shader and applies to families
// uiDepthOnEyeDraw's own lists never claim -- the icon core has none.
bool uiDepthHologramOnEyeDraw(ID3D11DeviceContext* ctx) {
    g_holoEye = -1;
    if (!detail::g_holoDepthOn) return false;
    // Cheapest first: the vertex-shader hash alone rejects nearly every
    // draw, before the two binding-shadow reads below. Either list
    // qualifies; which one is recorded in g_holoIsWorldMarker, for
    // ContributionBegin's DSS choice below.
    const uint64_t h = boundVsHash(ctx);
    if (!h || uiDepthIsExcluded(h)) return false;
    const bool worldMarker = inList(kHoloWorldMarkers, kHoloWorldMarkerCount, h);
    if (!worldMarker && !inList(g_holoFamilies, g_holoFamilyCount, h)) return false;
    // A bound depth-stencil must still be the scene's. None bound is
    // accepted: the icon core and the corona draw with depth off, and
    // neither pass below reads the game's depth.
    const void* dsv = bindingGet(BindSlot::Dsv0);
    if (dsv && !dsvIsSceneDepth(dsv)) return false;
    ResourceInfo rt;
    if (!bindingResolve(bindingGet(BindSlot::Rtv0), &rt) || !rt.isTexture2D) return false;
    int eye = eyeIndexFor(rt.resource, rt.a, rt.b, rt.fmt);
    if (eye >= 0 && g_eyesSwapped) eye = 1 - eye;
    if (eye < 0) return false;
    g_holoEye = eye;
    g_holoW = rt.a;
    g_holoH = rt.b;
    g_holoDrawVs = h;
    g_holoIsWorldMarker = worldMarker;
    ++g_holoWindowListed;
    if (worldMarker) ++g_holoWindowMarkerDraws;
    return true;
}

// Pass (a): the game's own draw again, RTV0 rebound to a scratch
// contribution target whose blend mirrors the game's own (holoContribBlendFor)
// -- VS/PS/inputs untouched, so RGB accumulates exactly the light the
// game's own blend equation would have added. A cockpit family depth-tests
// against the RADIUS scratch (never the game's own depth, and never
// writes): every listed draw is admitted or excluded by the SAME
// cockpit-radius test, independent of whether this particular draw bound
// any depth at all. A world marker gets DepthEnable FALSE instead
// (holoWorldMarkerDss): it contributes regardless of range.
bool uiDepthHologramContributionBegin(ID3D11DeviceContext* ctx) {
    g_holoContribOn = false;
    if (g_holoEye < 0) return false;
    if (!holoScratchPrepare(ctx, g_holoEye, g_holoW, g_holoH)) return false;
    HoloScratch& s = g_holoScratch[g_holoEye];
    ID3D11DepthStencilState* dss = g_holoIsWorldMarker ? holoWorldMarkerDss(ctx) : holoContribDss(ctx);
    if (!dss) return false;
    ctx->OMGetRenderTargets(kMaxRtvs, g_savedRtvs, &g_savedDsv);
    ctx->OMGetBlendState(&g_holoSavedBlend, g_holoSavedBlendFactor, &g_holoSavedSampleMask);
    ctx->OMGetDepthStencilState(&g_holoSavedDss, &g_holoSavedRef);
    BOOL enable = FALSE;
    D3D11_BLEND srcBlend = D3D11_BLEND_ONE;
    UINT8 writeMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (g_holoSavedBlend) {
        D3D11_BLEND_DESC desc{};
        g_holoSavedBlend->GetDesc(&desc);
        enable = desc.RenderTarget[0].BlendEnable;
        srcBlend = holoMapSrcBlend(desc.RenderTarget[0].SrcBlend);
        writeMask = static_cast<UINT8>((desc.RenderTarget[0].RenderTargetWriteMask &
            (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE)) |
            D3D11_COLOR_WRITE_ENABLE_ALPHA);
    }
    bool uncached = false;
    ID3D11BlendState* blend = holoContribBlendFor(ctx, enable, srcBlend, writeMask, &uncached);
    if (!blend) {
        releaseSavedOm();
        if (g_holoSavedBlend) { g_holoSavedBlend->Release(); g_holoSavedBlend = nullptr; }
        if (g_holoSavedDss) { g_holoSavedDss->Release(); g_holoSavedDss = nullptr; }
        return false;
    }
    // The game's OWN RTV0 view format, for the sRGB detection the resolve
    // needs (linearBlend): a plain (non-sRGB) view means the game's own
    // write never gamma-encoded, so this scratch -- which never does
    // either -- already holds display-space values; an sRGB view means it
    // did, so this scratch (a plain RGBA16F target) holds LINEAR ones.
    bool linearBlend = false;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
    if (g_savedRtvs[0]) {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        g_savedRtvs[0]->GetDesc(&rtvDesc);
        rtvFormat = rtvDesc.Format;
        linearBlend = holoIsSrgbFormat(rtvFormat);
    }
    s.linearBlend = linearBlend;
    // The game's own RT0 resource, tracked for the resolve's SHARE test
    // (the state block's own comment says why: never the tonemapped
    // display image). GetResource AddRefs; an unchanged identity AND view
    // format just drops that extra ref. Either changing -- a new
    // resource, or the SAME resource through a different RTV format,
    // which a typeless target can be bound with from one draw to the next
    // -- retires targetSrv (built in that format, at the resolve) along
    // with any old resource, so a stale view is never read in the wrong
    // space (a UNORM view would read an sRGB write without decoding it).
    ID3D11Resource* rtRes = nullptr;
    if (g_savedRtvs[0]) g_savedRtvs[0]->GetResource(&rtRes);
    if (rtRes != s.targetRes || rtvFormat != s.targetViewFormat) {
        if (s.targetSrv) { s.targetSrv->Release(); s.targetSrv = nullptr; }
        if (rtRes != s.targetRes) {
            if (s.targetRes) s.targetRes->Release();
            s.targetRes = rtRes;
            s.targetShaderResource = false;
            if (s.targetRes) {
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(s.targetRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
                    D3D11_TEXTURE2D_DESC td{};
                    tex->GetDesc(&td);
                    s.targetShaderResource = (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
                    tex->Release();
                }
            }
        } else {
            rtRes->Release();   // same identity; drop the extra ref GetResource gave us
        }
    } else if (rtRes) {
        rtRes->Release();
    }
    s.targetViewFormat = rtvFormat;
    if (!g_holoFirstDrawNoted) {
        g_holoFirstDrawNoted = true;
        Log::get().note("hologram depth: first listed draw -- vs %016llX, eye %d, target %ux%u, "
                        "RTV format %u, %s blend space, target view %s.",
                        static_cast<unsigned long long>(g_holoDrawVs), g_holoEye, g_holoW, g_holoH,
                        static_cast<unsigned>(rtvFormat), linearBlend ? "linear" : "display",
                        s.targetShaderResource ? "yes" : "no");
    }
    g_holoContribBlendToFree = uncached ? blend : nullptr;
    ID3D11RenderTargetView* rtv = s.contribRtv;
    vScreenSetRenderTargetsRaw(ctx, 1, &rtv, s.radiusDsv);
    vScreenOMSetBlendStateRaw(ctx, blend, g_holoSavedBlendFactor, g_holoSavedSampleMask);
    ctx->OMSetDepthStencilState(dss, 0);
    g_holoContribOn = true;
    return true;
}
void uiDepthHologramContributionEnd(ID3D11DeviceContext* ctx) {
    if (!g_holoContribOn) return;
    g_holoContribOn = false;
    guardedBudget(g_holoBudget, [&] {
        vScreenOMSetBlendStateRaw(ctx, g_holoSavedBlend, g_holoSavedBlendFactor, g_holoSavedSampleMask);
        ctx->OMSetDepthStencilState(g_holoSavedDss, g_holoSavedRef);
    });
    if (g_holoContribBlendToFree) { g_holoContribBlendToFree->Release(); g_holoContribBlendToFree = nullptr; }
    if (g_holoSavedBlend) { g_holoSavedBlend->Release(); g_holoSavedBlend = nullptr; }
    if (g_holoSavedDss) { g_holoSavedDss->Release(); g_holoSavedDss = nullptr; }
    guarded("uiDepthHolo.restore", [&] { restoreOm(ctx); });
    releaseSavedOm();
}

// Pass (b): the same draw once more, null pixel shader, into a scratch
// depth target of its own (cleared to 0, reversed-Z far), depth func
// GREATER, write on -- the NEAREST raster depth of the family's geometry
// over its whole footprint, independent of alpha or the coverage floor
// (tested only at the resolve). The same test for both lists: a cockpit
// family's own radius gating lives in the contribution pass above, not
// here, so a far cockpit fragment can still write an element depth --
// harmless, per holoScratchPrepare's own comment.
bool uiDepthHologramElementDepthBegin(ID3D11DeviceContext* ctx) {
    g_holoElementOn = false;
    g_holoSavedViewportCount = 0;
    g_holoElementQuery = nullptr;
    if (g_holoEye < 0) return false;
    if (!holoScratchPrepare(ctx, g_holoEye, g_holoW, g_holoH)) return false;
    HoloScratch& s = g_holoScratch[g_holoEye];
    ID3D11DepthStencilState* dss = holoElementDepthState(ctx);
    if (!dss) return false;
    ctx->OMGetRenderTargets(kMaxRtvs, g_savedRtvs, &g_savedDsv);
    g_holoSavedPsClassCount = 256;
    ctx->PSGetShader(&g_holoSavedPs, g_holoSavedPsClasses, &g_holoSavedPsClassCount);
    ctx->OMGetDepthStencilState(&g_holoSavedDss, &g_holoSavedRef);
    if (g_holoIsWorldMarker) {
        // A world marker's raster depth read as 0 (sky) on every pixel its
        // contribution covers 100% of (flight 20260924_175113, eye_175314)
        // -- either the game's own viewport for this draw has MinDepth ==
        // MaxDepth == 0 (mechanism i, recovered by the override below), or
        // its VS writes z=0 regardless of viewport (mechanism ii, where
        // this override is a no-op: the occlusion query beneath tells
        // which one it was).
        g_holoSavedViewportCount = kHoloMaxViewports;
        ctx->RSGetViewports(&g_holoSavedViewportCount, g_holoSavedViewports);
        if (!g_holoWorldMarkerNoted) {
            g_holoWorldMarkerNoted = true;
            D3D11_DEPTH_STENCIL_DESC dssDesc{};
            if (g_holoSavedDss) g_holoSavedDss->GetDesc(&dssDesc);
            Microsoft::WRL::ComPtr<ID3D11RasterizerState> rs;
            ctx->RSGetState(&rs);
            D3D11_RASTERIZER_DESC rsDesc{};
            if (rs) rs->GetDesc(&rsDesc);
            Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
            FLOAT blendFactor[4]{};
            UINT sampleMask = 0;
            ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
            D3D11_BLEND_DESC blendDesc{};
            if (blend) blend->GetDesc(&blendDesc);
            Log::get().note("hologram depth: first world-marker draw -- vs %016llX, game viewport 0 "
                            "depth %.3f..%.3f, DSS enable %d func %u write %u, RS DepthClipEnable %d "
                            "bias %d, blend enable %d src %u dest %u.",
                            static_cast<unsigned long long>(g_holoDrawVs),
                            static_cast<double>(g_holoSavedViewportCount ? g_holoSavedViewports[0].MinDepth : -1.0f),
                            static_cast<double>(g_holoSavedViewportCount ? g_holoSavedViewports[0].MaxDepth : -1.0f),
                            dssDesc.DepthEnable, static_cast<unsigned>(dssDesc.DepthFunc),
                            static_cast<unsigned>(dssDesc.DepthWriteMask), rsDesc.DepthClipEnable, rsDesc.DepthBias,
                            blendDesc.RenderTarget[0].BlendEnable, static_cast<unsigned>(blendDesc.RenderTarget[0].SrcBlend),
                            static_cast<unsigned>(blendDesc.RenderTarget[0].DestBlend));
        }
        D3D11_VIEWPORT overridden[kHoloMaxViewports];
        for (UINT i = 0; i < g_holoSavedViewportCount; ++i) {
            overridden[i] = g_holoSavedViewports[i];
            overridden[i].MinDepth = 0.0f;
            overridden[i].MaxDepth = 1.0f;
        }
        vScreenRSSetViewportsRaw(ctx, g_holoSavedViewportCount, overridden);
    }
    vScreenSetRenderTargetsRaw(ctx, 0, nullptr, s.depthDsv);
    vScreenPSSetShaderRaw(ctx, nullptr, nullptr, 0);
    ctx->OMSetDepthStencilState(dss, 0);
    if (g_holoIsWorldMarker) {
        g_holoElementQuery = holoAcquireMarkerQuery(ctx, s);
        if (g_holoElementQuery) ctx->Begin(g_holoElementQuery);
    }
    g_holoElementOn = true;
    return true;
}
void uiDepthHologramElementDepthEnd(ID3D11DeviceContext* ctx) {
    if (!g_holoElementOn) return;
    g_holoElementOn = false;
    if (g_holoElementQuery) {
        ctx->End(g_holoElementQuery);
        holoMarkerQueryBegan(g_holoScratch[g_holoEye], g_holoElementQuery);
        g_holoElementQuery = nullptr;
    }
    guardedBudget(g_holoBudget, [&] {
        vScreenPSSetShaderRaw(ctx, g_holoSavedPs, g_holoSavedPsClasses, g_holoSavedPsClassCount);
        ctx->OMSetDepthStencilState(g_holoSavedDss, g_holoSavedRef);
        if (g_holoSavedViewportCount) vScreenRSSetViewportsRaw(ctx, g_holoSavedViewportCount, g_holoSavedViewports);
    });
    g_holoSavedViewportCount = 0;
    for (UINT i = 0; i < g_holoSavedPsClassCount; ++i) if (g_holoSavedPsClasses[i]) g_holoSavedPsClasses[i]->Release();
    g_holoSavedPsClassCount = 0;
    if (g_holoSavedPs) { g_holoSavedPs->Release(); g_holoSavedPs = nullptr; }
    if (g_holoSavedDss) { g_holoSavedDss->Release(); g_holoSavedDss = nullptr; }
    guarded("uiDepthHolo.restore", [&] { restoreOm(ctx); });
    releaseSavedOm();
}

// Once per eye per frame, before the temporal pass reads the private
// scene-depth copy (uiDepthTemporalDepth, called right after this in
// temporal_pass.cpp): a full-screen pass, entirely through vscreen.h's Raw
// wrappers and this module's own state (never the game's -- see the
// per-call save/restore below), that stamps each scratch pair's nearest
// element depth into that same private copy wherever the pixel is visibly
// lit and, when the game's own render target is viewable, the element
// supplies a real share of its light. The depth-stencil state (GREATER,
// write on) then keeps the nearer of that and whatever the private copy
// already held, so nearer real scene geometry is never overwritten.
// Declines (and counts why, for the periodic census) when nothing was
// listed this eye/frame, the private copy is unavailable, or no build ran
// (holoResolveShaders/holoResolveCb/holoResolveRs). display is the actual
// submitted, tonemapped image (temporal_pass's inSrv); it may be null or
// the wrong size, independently of whether the game's own target (tracked
// by ContributionBegin) is viewable.
bool uiDepthHologramResolve(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* scene,
                            uint32_t w, uint32_t h, ID3D11ShaderResourceView* display) {
    if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown || !detail::g_holoDepthOn) return false;
    if (!ctx || !scene || eye < 0 || eye > 1) return false;
    HoloScratch& s = g_holoScratch[eye];
    if (s.preparedFrame != g_frame || s.w != w || s.h != h) {
        ++g_holoWindowDeclinedNotCleared;
        return false;
    }
    ID3D11DepthStencilView* privateTarget = g_uiDepth[eye].acquire(ctx, scene);
    if (!privateTarget) {
        ++g_holoWindowDeclinedNoPrivate;
        return false;
    }
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = holoResolveCb(ctx);
    ID3D11DepthStencilState* dss = holoElementDepthState(ctx);
    ID3D11RasterizerState* rs = holoResolveRs(ctx);
    if (!holoResolveShaders(ctx, &vs, &ps) || !cb || !dss || !rs) {
        ++g_holoWindowDeclinedFault;
        return false;
    }
    // The SHARE test's own view, over the game's RT0 resource (tracked by
    // ContributionBegin) in that RTV's own view format -- built lazily,
    // once per resource identity, and reused every resolve until it
    // changes. No SHADER_RESOURCE bind, or a failed creation, skips the
    // share test outright; the floor still applies.
    bool haveTarget = false;
    if (s.targetShaderResource && s.targetRes) {
        if (!s.targetSrv) {
            D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
            svd.Format = s.targetViewFormat;
            svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            svd.Texture2D.MipLevels = 1;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev) {
                dev->CreateShaderResourceView(s.targetRes, &svd, &s.targetSrv);
                dev->Release();
            }
        }
        haveTarget = s.targetSrv != nullptr;
    }
    if (!haveTarget) ++g_holoWindowNoTarget;
    // The FLOOR's own view: the actual displayed image, at exactly this
    // eye's size; a size mismatch (or none at all) falls back to the
    // contribution's own space rather than failing the whole resolve.
    bool haveDisplay = false, displaySrgb = false;
    if (display) {
        ID3D11Resource* res = nullptr;
        display->GetResource(&res);
        if (res) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                haveDisplay = td.Width == w && td.Height == h;
                tex->Release();
            }
            res->Release();
        }
        if (haveDisplay) {
            D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
            display->GetDesc(&svd);
            displaySrgb = holoIsSrgbFormat(svd.Format);
        }
    }
    if (!haveDisplay) ++g_holoWindowFloorFallback;
    const uint32_t flags = (s.linearBlend ? 1u : 0u) | (haveTarget ? 2u : 0u) |
                           (haveDisplay ? 4u : 0u) | (displaySrgb ? 8u : 0u);
    const HoloResolveCb data{g_holoFloor, g_holoShare, flags, s.radiusDepth};
    vScreenUpdateSubresourceRaw(ctx, cb, 0, nullptr, &data, 0, 0);
    const bool ran = guardedBudget(g_holoBudget, [&] {
        ctx->OMGetRenderTargets(kMaxRtvs, g_savedRtvs, &g_savedDsv);
        Microsoft::WRL::ComPtr<ID3D11VertexShader> savedVs;
        ID3D11ClassInstance* savedVsClasses[256]{}; UINT savedVsClassCount = 256;
        ctx->VSGetShader(&savedVs, savedVsClasses, &savedVsClassCount);
        Microsoft::WRL::ComPtr<ID3D11PixelShader> savedPs;
        ID3D11ClassInstance* savedPsClasses[256]{}; UINT savedPsClassCount = 256;
        ctx->PSGetShader(&savedPs, savedPsClasses, &savedPsClassCount);
        Microsoft::WRL::ComPtr<ID3D11GeometryShader> savedGs;
        ID3D11ClassInstance* savedGsClasses[256]{}; UINT savedGsClassCount = 256;
        ctx->GSGetShader(&savedGs, savedGsClasses, &savedGsClassCount);
        Microsoft::WRL::ComPtr<ID3D11HullShader> savedHs;
        ID3D11ClassInstance* savedHsClasses[256]{}; UINT savedHsClassCount = 256;
        ctx->HSGetShader(&savedHs, savedHsClasses, &savedHsClassCount);
        Microsoft::WRL::ComPtr<ID3D11DomainShader> savedDs;
        ID3D11ClassInstance* savedDsClasses[256]{}; UINT savedDsClassCount = 256;
        ctx->DSGetShader(&savedDs, savedDsClasses, &savedDsClassCount);
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> savedDss; UINT savedRef = 0;
        ctx->OMGetDepthStencilState(&savedDss, &savedRef);
        Microsoft::WRL::ComPtr<ID3D11InputLayout> savedLayout;
        ctx->IAGetInputLayout(&savedLayout);
        D3D11_PRIMITIVE_TOPOLOGY savedTopo;
        ctx->IAGetPrimitiveTopology(&savedTopo);
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> savedRs;
        ctx->RSGetState(&savedRs);
        D3D11_VIEWPORT savedVps[16]; UINT savedVpCount = 16;
        ctx->RSGetViewports(&savedVpCount, savedVps);
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> savedSrv0, savedSrv1, savedSrv2, savedSrv3;
        ctx->PSGetShaderResources(0, 1, &savedSrv0);
        ctx->PSGetShaderResources(1, 1, &savedSrv1);
        ctx->PSGetShaderResources(2, 1, &savedSrv2);
        ctx->PSGetShaderResources(3, 1, &savedSrv3);
        Microsoft::WRL::ComPtr<ID3D11Buffer> savedCb0;
        ctx->PSGetConstantBuffers(0, 1, &savedCb0);

        // RTVs first: unbinds whatever was there (including the eye
        // target itself, if it happened to be bound), so the target/
        // display SRVs below are never fought over an output binding of
        // the same resource by the time they are set.
        vScreenSetRenderTargetsRaw(ctx, 0, nullptr, privateTarget);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->RSSetState(rs);
        const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
        vScreenRSSetViewportsRaw(ctx, 1, &vp);
        vScreenVSSetShaderRaw(ctx, vs, nullptr, 0);
        vScreenPSSetShaderRaw(ctx, ps, nullptr, 0);
        ctx->OMSetDepthStencilState(dss, 0);
        ID3D11ShaderResourceView* srvs[4] = {s.contribSrv, s.depthSrv, haveTarget ? s.targetSrv : nullptr,
                                             haveDisplay ? display : nullptr};
        ctx->PSSetShaderResources(0, 4, srvs);
        ctx->PSSetConstantBuffers(0, 1, &cb);

        ID3D11Query* q = holoAcquireQuery(ctx, s);
        if (q) ctx->Begin(q);
        vScreenDrawRaw(ctx, 3, 0);
        if (q) { ctx->End(q); holoQueryBegan(s, q); }

        ID3D11ShaderResourceView* nullSrvs[4]{};
        ctx->PSSetShaderResources(0, 4, nullSrvs);
        vScreenVSSetShaderRaw(ctx, savedVs.Get(), savedVsClasses, savedVsClassCount);
        vScreenPSSetShaderRaw(ctx, savedPs.Get(), savedPsClasses, savedPsClassCount);
        ctx->GSSetShader(savedGs.Get(), savedGsClasses, savedGsClassCount);
        ctx->HSSetShader(savedHs.Get(), savedHsClasses, savedHsClassCount);
        ctx->DSSetShader(savedDs.Get(), savedDsClasses, savedDsClassCount);
        for (UINT i = 0; i < savedVsClassCount; ++i) if (savedVsClasses[i]) savedVsClasses[i]->Release();
        for (UINT i = 0; i < savedPsClassCount; ++i) if (savedPsClasses[i]) savedPsClasses[i]->Release();
        for (UINT i = 0; i < savedGsClassCount; ++i) if (savedGsClasses[i]) savedGsClasses[i]->Release();
        for (UINT i = 0; i < savedHsClassCount; ++i) if (savedHsClasses[i]) savedHsClasses[i]->Release();
        for (UINT i = 0; i < savedDsClassCount; ++i) if (savedDsClasses[i]) savedDsClasses[i]->Release();
        ctx->OMSetDepthStencilState(savedDss.Get(), savedRef);
        ctx->IASetInputLayout(savedLayout.Get());
        ctx->IASetPrimitiveTopology(savedTopo);
        ctx->RSSetState(savedRs.Get());
        vScreenRSSetViewportsRaw(ctx, savedVpCount, savedVps);
        ctx->PSSetShaderResources(0, 1, savedSrv0.GetAddressOf());
        ctx->PSSetShaderResources(1, 1, savedSrv1.GetAddressOf());
        ctx->PSSetShaderResources(2, 1, savedSrv2.GetAddressOf());
        ctx->PSSetShaderResources(3, 1, savedSrv3.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, savedCb0.GetAddressOf());
        restoreOm(ctx);
    });
    releaseSavedOm();
    if (!ran) {
        ++g_holoWindowDeclinedFault;
        return false;
    }
    ++g_holoWindowResolved;
    return true;
}

// Borrowed view of this eye's raw contribution target, for the eye dump
// (temporal_pass.cpp's HoloContribution input) -- the same RGBA16F
// texture the resolve reads, so a dump shows exactly where coverage
// landed and at what strength.
bool uiDepthHologramContribution(uint32_t w, uint32_t h, int eye, ID3D11ShaderResourceView** srv) {
    if (!srv) return false;
    *srv = nullptr;
    if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown || !detail::g_holoDepthOn ||
        eye < 0 || eye > 1) return false;
    const HoloScratch& s = g_holoScratch[eye];
    if (s.preparedFrame != g_frame || s.w != w || s.h != h) return false;
    *srv = s.contribSrv;
    return *srv != nullptr;
}

ID3D11ShaderResourceView* uiDepthContentChanges(uint32_t w,uint32_t h,int eye) {
    if(!detail::g_uiDepthOn || !g_trained || detail::g_uiDepthStoodDown || eye<0 || eye>1)return nullptr;
    const auto& m=g_edits[eye];return m.marked && m.w==w && m.h==h?m.srv:nullptr;
}

bool uiDepthCoverageMask(uint32_t w, uint32_t h, int eye, ID3D11Texture2D** tex) {
    if (!tex) return false;
    *tex = nullptr;
    if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown || eye < 0 || eye > 1) return false;
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

bool uiDepthReactiveMask(uint32_t w, uint32_t h, int eye, ID3D11Texture2D** tex) {
    if (!tex) return false;
    *tex = nullptr;
    return g_reactive > 0.0f && uiDepthCoverageMask(w, h, eye, tex);
}

bool uiDepthSmokeDepth(uint32_t w, uint32_t h, int eye, ID3D11ShaderResourceView** srv) {
    if (!srv) return false;
    *srv = nullptr;
    if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown || !g_smokeOn || eye < 0 || eye > 1) return false;
    const SmokeDepth& s = g_smokeDepth[eye];
    if (!s.srv || !s.written || s.w != w || s.h != h) return false;
    *srv = s.srv;
    return true;
}

bool uiDepthTemporalDepth(uint32_t w, uint32_t h, int eye, ID3D11Texture2D* scene,
                          ID3D11ShaderResourceView** srv) {
    if (!srv) return false;
    *srv = nullptr;
    if (!detail::g_uiDepthOn || detail::g_uiDepthStoodDown || eye < 0 || eye > 1) return false;
    *srv = g_uiDepth[eye].view(scene, w, h);
    return *srv != nullptr;
}

void uiDepthHoloMotion(int eye, ID3D11Texture2D* scene, ID3D11ShaderResourceView** views) {
    views[0]=views[1]=nullptr;
    if(detail::g_uiDepthOn && !detail::g_uiDepthStoodDown && eye>=0 && eye<2) g_holoMotion[eye].views(scene,views);
}

void uiDepthMotionResourceWrittenImpl(ID3D11Resource* resource,uint64_t first,uint64_t end) {
    if(!resource && !detail::g_uiDepthOn) return;
    for(auto& motion:g_holoMotion) motion.resourceWritten(resource,first,end);
}

// The scanner's chrome surfaces held this frame, copied whole into staging
// for the eye run being staged; written by uiDepthHoloWriteDump. A frame
// without the scanner holds nothing and nothing is written.
void stageChromeDump(ID3D11DeviceContext* ctx) {
    for(auto& d:g_chromeDump) d.Reset();
    g_chromeDumpCount=0;
    if(g_chromeHeldCount==0) {
        // Said so the absence of Chrome files reads as "not held", never as
        // an instrument that did not run.
        Log::get().note("eye capture: no scanner chrome was held on the run's first frame (the tracker matched no composite before the pass); no Chrome files this run.");
        return;
    }
    Microsoft::WRL::ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
    if(!dev) return;
    for(uint32_t i=0;i<g_chromeHeldCount;++i) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        if(FAILED(g_chromeHeld[i]->QueryInterface(IID_PPV_ARGS(&tex)))) continue;
        D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
        td.Usage=D3D11_USAGE_STAGING; td.BindFlags=0; td.MiscFlags=0;
        td.CPUAccessFlags=D3D11_CPU_ACCESS_READ; td.MipLevels=1; td.ArraySize=1;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
        if(FAILED(dev->CreateTexture2D(&td,nullptr,&copy))) continue;
        // Mip 0, array 0 of the chrome; a mipped or arrayed surface would
        // fail CopyResource's like-for-like rule, hence the subresource copy.
        ctx->CopySubresourceRegion(copy.Get(),0,0,0,0,tex.Get(),0,nullptr);
        g_chromeDump[g_chromeDumpCount++]=copy;
    }
}
void uiDepthHoloStageDump(ID3D11DeviceContext* ctx, ID3D11Texture2D* scene) {
    stageChromeDump(ctx);
    g_holoDump.Reset(); g_holoDumpCount=0;
    ID3D11ShaderResourceView* views[2]{}; uiDepthHoloMotion(0,scene,views); if(!views[1]) return;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource; views[1]->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer; if(FAILED(resource.As(&buffer))) return;
    D3D11_BUFFER_DESC bd{}; buffer->GetDesc(&bd); bd.BindFlags=bd.MiscFlags=bd.StructureByteStride=0;
    bd.Usage=D3D11_USAGE_STAGING; bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Device> dev; ctx->GetDevice(&dev);
    if(SUCCEEDED(dev->CreateBuffer(&bd,nullptr,&g_holoDump))) {
        ctx->CopyResource(g_holoDump.Get(),buffer.Get()); g_holoDumpCount=g_holoMotion[0].recordCount();
    }
}
// The staged chrome copies, in the eye run's own texture format (EDVRTEX1:
// nine uint32 after the magic -- 1, width, height, DXGI format, row bytes,
// then zeros; four bytes a pixel, the chrome's RGBA8 -- eye_<stamp>_Chrome<i>.bin).
void writeChromeDump(ID3D11DeviceContext* ctx,const wchar_t* directory,const wchar_t* stamp) {
    for(uint32_t i=0;i<g_chromeDumpCount;++i) {
        auto& copy=g_chromeDump[i]; if(!copy) continue;
        D3D11_TEXTURE2D_DESC d{}; copy->GetDesc(&d);
        const uint32_t bytes=(d.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format==DXGI_FORMAT_R8G8B8A8_UNORM ||
                              d.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                              d.Format==DXGI_FORMAT_B8G8R8A8_UNORM || d.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)?4u:0u;
        D3D11_MAPPED_SUBRESOURCE map{};
        if(!bytes || FAILED(ctx->Map(copy.Get(),0,D3D11_MAP_READ,0,&map))) {
            Log::get().note("eye capture: %ls chrome %u (%ux%u, DXGI format %u) not written: %s.",stamp,i,d.Width,d.Height,unsigned(d.Format),bytes?"readback failed":"not a four-byte format");
            copy.Reset(); continue;
        }
        wchar_t path[MAX_PATH]; _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\eye_%s_Chrome%u.bin",directory,stamp,i);
        FILE* f=nullptr; _wfopen_s(&f,path,L"wb"); bool ok=false;
        if(f) {
            const uint32_t header[9]={1,d.Width,d.Height,static_cast<uint32_t>(d.Format),d.Width*bytes,0,0,0,0};
            ok=fwrite("EDVRTEX1",1,8,f)==8 && fwrite(header,sizeof(header),1,f)==1;
            for(uint32_t y=0;y<d.Height && ok;++y) ok=fwrite(static_cast<const char*>(map.pData)+y*map.RowPitch,1,d.Width*bytes,f)==d.Width*bytes;
            if(fclose(f)!=0) ok=false;
        }
        ctx->Unmap(copy.Get(),0);
        Log::get().note("eye capture: %ls chrome %u -- the scanner's chrome surface %ux%u DXGI format %u, as sampled by the screen's composite this run: %s.",stamp,i,d.Width,d.Height,unsigned(d.Format),ok?"written":"write failed");
        copy.Reset();
    }
    g_chromeDumpCount=0;
}
void uiDepthHoloWriteDump(ID3D11DeviceContext* ctx,const wchar_t* directory,const wchar_t* stamp) {
    writeChromeDump(ctx,directory,stamp);
    if(!g_holoDump) { Log::get().note("holo motion: eye run %ls has no records.",stamp); return; }
    D3D11_MAPPED_SUBRESOURCE map{}; HRESULT result=ctx->Map(g_holoDump.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
    if(SUCCEEDED(result)) {
        unsigned valid=0,matched=0;
        for(unsigned i=0;i<g_holoDumpCount;++i) { const auto* p=static_cast<const float*>(map.pData)+i*60; valid+=p[56]==1; matched+=p[59]==1; }
        wchar_t path[MAX_PATH]; _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\eye_%s_Holo.bin",directory,stamp);
        FILE* file=nullptr; _wfopen_s(&file,path,L"wb"); bool ok=false;
        if(file) { const uint32_t header[2]={g_holoDumpCount,240}; ok=fwrite("EDVRHLO1",1,8,file)==8 && fwrite(header,8,1,file)==1 && fwrite(map.pData,240,g_holoDumpCount,file)==g_holoDumpCount; if(fclose(file)!=0) ok=false; }
        ctx->Unmap(g_holoDump.Get(),0);
        Log::get().note("holo motion: eye run %ls matched %u/%u eligible transforms (%u total); record file %s.",stamp,matched,valid,g_holoDumpCount,ok?"written":"FAILED");
    } else Log::get().note("holo motion: eye run %ls readback unavailable (0x%08X).",stamp,unsigned(result));
    g_holoDump.Reset(); g_holoDumpCount=0;
}

void releaseChromeHeld() {
    for(uint32_t i=0;i<g_chromeHeldCount;++i) { g_chromeHeld[i]->Release(); g_chromeHeld[i]=nullptr; }
    g_chromeHeldCount=0;
}

void uiDepthFrameBoundary(ID3D11DeviceContext* ctx) {
    ++g_frame;
    releaseChromeHeld();   // the pass, which stages an eye run, has run for the frame
    g_uiContent.retire(g_frame);
    if(ctx)g_uiContent.gpu.poll(ctx);
    if(ctx) for(auto& sample:g_stellarGpu) sample.poll(ctx);
    for (UiDepthLayer& layer : g_uiDepth) layer.frameBoundary();
    for(auto& motion:g_holoMotion) motion.frameBoundary();
    // After the pruning above: the write guard in ui_depth.h may stand down
    // again once neither eye's map holds a corona's buffers (holo_motion.h).
    detail::g_holoGeometryTracked.store(g_holoMotion[0].tracksGeometry() || g_holoMotion[1].tracksGeometry(),
                                        std::memory_order_relaxed);
    g_frameTargetCount = 0;
    // The masks are marked during the frame and read at its submits, so
    // the clear belongs here, after both.
    if (ctx) {
        for(auto* masks:{g_mask,g_edits}) for(unsigned eye=0;eye<2;++eye) {
            Mask& m=masks[eye];
            if (!m.rtv || !m.marked) continue;
            const FLOAT zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            guardedBudget(g_budget, [&] { ctx->ClearRenderTargetView(m.rtv, zero); });
            m.marked = false;
        }
    }
    // The smoke's depth is cleared at its first draw of a frame (the pass
    // read this frame's at the submits); here the frame's writes are over.
    for (SmokeDepth& s : g_smokeDepth) s.written = false;
    if (!detail::g_uiDepthOn) return;
    holoDepthWindowTick(ctx);
    // UI content census: close the frame this armed (the summary), then
    // decide whether the frame about to start is the eye run's first --
    // objectProbeLedgerActive() newly true since the last check, a rising
    // edge that fires exactly once per run regardless of how many frames
    // the ledger itself stays armed for.
    if (g_uiContentCensusOn) {
        Log::get().note("UI content census: frame %u summary -- hit %u, reset %u, updated %u, "
                        "declined %u, evicted %u; %u bytes allocated, %u entries in use.",
                        g_frame, g_uiContentCensusHits, g_uiContentCensusResets,
                        g_uiContentCensusUpdated, g_uiContentCensusDeclined, g_uiContentCensusEvicted,
                        g_uiContent.allocated, g_uiContent.inUse());
        g_uiContentCensusOn = false;
    }
    const bool ledgerActive = objectProbeLedgerActive();
    if (ledgerActive && !g_uiContentCensusLedgerWasOn) {
        g_uiContentCensusOn = true;
        g_uiContentCensusLines = 0;
        g_uiContentCensusHits = g_uiContentCensusResets = g_uiContentCensusUpdated =
            g_uiContentCensusDeclined = g_uiContentCensusEvicted = 0;
    }
    g_uiContentCensusLedgerWasOn = ledgerActive;
    ++g_wFrames;
    if (!g_announced && g_wWrote > 0) {
        g_announced = true;
        Log::get().note("ui depth: engaged -- %u interface draws wrote their depth "
                        "this window (%u composites of %u learned surfaces, %u "
                        "direct, %u through the alpha-aware pass), into private depth only.",
                        g_wWrote, g_wComposite, g_surfaceCount, g_wDirect, g_wReissued);
    }
    if (g_wFrames >= kTotalsFrames) {
        const auto& edits=g_uiContent.totals;
        if(g_trained) Log::get().note("UI content totals: compared=%u reused=%u declined=%u reset=%u evicted=%u history=%.2f MiB (session totals; zero comparisons means inactive).",
            edits.updates,edits.hits,edits.declined,edits.resets,edits.evicted,double(g_uiContent.allocated)/(1024*1024));
        const auto& editGpu=g_uiContent.gpu.totals;
        if(g_trained)Log::get().note("UI content GPU: completed=%u skipped=%u invalid=%u, %.3f us/source update (one thread per 4x4 block; sampled every 16th frame, no wait or flush; separate from EDVR-at-door GPU).",
            editGpu.samples,editGpu.skipped,editGpu.invalid,editGpu.samples?editGpu.ms*1000/editGpu.samples:0.0);
        for(int i=0;i<2;++i) {
            const auto& s=g_stellarCpu[i];
            if(s.calls) Log::get().note("stellar coverage CPU: %s calls=%u sampled=%u frames=%u, %.3f us/call (prepare, reissue and restore; sampled every 16th frame; no GPU wait).",
                i==0?"ring":"orbital",s.calls,s.samples,g_wFrames,
                s.samples?double(s.ticks)*1e6/double(qpcFrequency())/s.samples:0.0);
            const auto& gpu=g_stellarGpu[i].totals;
            if(s.calls || gpu.samples || gpu.invalid || gpu.skipped)
                Log::get().note("stellar coverage GPU: %s completed=%u skipped=%u invalid=%u, %.3f us/call; estimated %.3f ms/frame from %u calls/%u frames. Separate from EDVR-at-door GPU; sampled every 16th frame, no wait or flush.",
                    i==0?"ring":"orbital",gpu.samples,gpu.skipped,gpu.invalid,
                    gpu.samples?gpu.ms*1000.0/gpu.samples:0.0,
                    gpu.samples?gpu.ms/gpu.samples*double(s.calls)/g_wFrames:0.0,s.calls,g_wFrames);
        }
        if (g_wWrote || g_wNotScene || g_wRebound || g_wNoPair || g_wNoShader ||
            g_wNoTwin || g_wLearned || g_evictions) {
            Log::get().note("ui depth totals: %.1f interface draws a frame wrote depth "
                            "(%.1f composites, %.1f direct; %.1f through the alpha-aware "
                            "pass, %.1f of those bound to the pass's depth); %u surfaces "
                            "known, %u learned this window, %u forgotten; left alone: "
                            "%.1f a frame with no depth shader for their family, %.1f "
                            "with no pair or planes, %.1f by the menus switch, %u with no state; %llu written "
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
                            g_wNoTwin,
                            static_cast<unsigned long long>(g_sessionWrote));
        }
        resetWindow();
    }
}

void uiDepthShutdown() {
    holoDepthShutdownImpl();
    g_uiContent.reset();g_contentNoted=false;
    g_uiContentCensusOn=g_uiContentCensusLedgerWasOn=false;g_uiContentCensusLines=0;
    g_uiContentCensusHits=g_uiContentCensusResets=g_uiContentCensusUpdated=g_uiContentCensusDeclined=g_uiContentCensusEvicted=0;
    if(g_savedEdits){g_savedEdits->Release();g_savedEdits=nullptr;}g_editsBound=false;
    for(auto& sample:g_stellarGpu) sample.reset();
    g_stellarCpuActive=-1;
    g_holoDump.Reset(); g_holoDumpCount=0;
    releaseChromeHeld();
    for(auto& d:g_chromeDump) d.Reset();
    g_chromeDumpCount=0;
    for(auto& motion:g_holoMotion) motion=HoloMotion{};
    if(g_savedHoloInfo) { g_savedHoloInfo->Release(); g_savedHoloInfo=nullptr; }
    g_holoBound=g_holoNoted=false;
    g_coronaPending=g_coronaMotion=false;g_coronaNoted=false;g_smokeCoronaShader.Reset();g_smokeCoronaCompileTried=false;
    if(g_savedSceneDepth){g_savedSceneDepth->Release();g_savedSceneDepth=nullptr;}g_sceneDepthBound=false;
    for(UINT i=0;i<g_savedPsClassCount;++i)if(g_savedPsClasses[i])g_savedPsClasses[i]->Release();g_savedPsClassCount=0;
    for(auto& cached:g_sceneDepthRead){if(cached.srv)cached.srv->Release();if(cached.tex)cached.tex->Release();cached={};}
    g_orbitalVs.Reset();g_stellarNoted[0]=g_stellarNoted[1]=false;
    if (g_savedFloorCb) { g_savedFloorCb->Release(); g_savedFloorCb = nullptr; }
    if (g_savedPs) {
        g_savedPs->Release();
        g_savedPs = nullptr;
    }
    if (g_reissueScene) { g_reissueScene->Release(); g_reissueScene=nullptr; }
    if (g_reSavedDss) {
        g_reSavedDss->Release();
        g_reSavedDss = nullptr;
    }
    if (g_reSavedBlend) {
        g_reSavedBlend->Release();
        g_reSavedBlend = nullptr;
    }
    g_reissueOn = false;
    g_rebound = false;
    detail::g_uiDepthMode = Mode::kNone;
    releaseSavedOm();
    for (UiDepthLayer& layer : g_uiDepth) layer.release();
    releaseStates();
    for(auto* masks:{g_mask,g_edits}) for(unsigned eye=0;eye<2;++eye) {
        Mask& m=masks[eye];
        if (m.rtv) m.rtv->Release();
        if (m.srv) m.srv->Release();
        if (m.tex) m.tex->Release();
        m = Mask();
    }
    for (SmokeDepth& s : g_smokeDepth) {
        if (s.srv) s.srv->Release();
        if (s.dsv) s.dsv->Release();
        if (s.tex) s.tex->Release();
        s = SmokeDepth();
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
    detail::g_uiDepthOn = false;
    detail::g_uiDepthPlanetPending=detail::g_uiDepthPlanetSolarPending=g_planetNoted=g_solarNoted=false;g_planetCoverage=PlanetCoverage{};
}

}  // namespace edvr
