// visual.cockpit_glass = legacy (docs\cockpit-glass-legacy-port.md): the HLSL the build compiles to fixed DXBC
// (tools/temporal_shader_build/fixed_cockpit_glass_variants.h) and cockpit_glass.cpp binds in place of the game's canopy pixel shader.
// Header-only text, no device.
//
// WHAT IT IS. The live canopy (CockpitGlass_Forward, intact, quality 3; the game's PS 4E4FF61E8A08FC7E) lights the glass with GGX, Fresnel and
// reflection probes. The Horizons 3.8 client lit the same glass far more simply: one directional light as Lambert times shadow, plus a
// normal-independent glare lobe toward the light, over an ambient the vertex shader took from a cube map. This shader gives the live glass
// that legacy model, with the legacy client's measured values (docs\cockpit-glass-legacy-port.md section 10). One text, three compiles (the macros in
// tools/temporal_shader_build/fixed_cockpit_glass_variants.h):
//   intact        replaces the game's intact PS. Premultiplied legacy blend: alpha a constant base, colour = radiance / alpha under live's straight blend.
//   ice           overlay for the game's ice PS (7F770D61DEA7CD30): the legacy dirt and glare light alone, added over the game's own ice draw.
//   condensation  overlay for the condensation PS (42747CC92CF844C4), the same.
// Each has its game vertex shader's output signature (the ice and condensation vertex shaders pass a fourth mask component, in different orders), so it
// links with it, and reads only what the game has already bound for that draw:
//   b1  the frame block (333 registers; this reads 56, 90, 227-228, 245, 253, 261, 277-279, 291-326)
//   b2  the material block (18 registers)
//   b13 EDVR's own one register, the dirt level (cockpit_glass.cpp binds it with the streaks and puts back what was there)
//   t0  per-cluster probe lists (raw), t1 the directional light (raw: colour at +0, view-space direction at +16, stride 48),
//   t2  the light and probe records (raw), t3 the probe cube array, t5 the moment shadow map
//   the crack normal and alpha: t6 (intact) or t7 (ice, condensation); the blemish map: t9 (intact) or t11 (ice, condensation)
//   s0  the quality sampler (anisotropic wrap), s1 point/clamp for the shadow gathers, s2 the probe cube sampler
// and EDVR's one texture, the dirt streaks (cockpit_glass_streaks.h), at t12 for every variant: the game binds nothing there on the canopy draw, and the
// ice and condensation shaders use t10 and t11, so t10 and t11 cannot hold it. The swap binds it each time and puts back what was there.
// What was taken from the live shader and what was measured or chosen is written out in sections 8 to 11 of the doc.
//
// THE BLEND (intact). The legacy client's blend was premultiplied (ONE, INV_SRC_ALPHA): colour added, alpha a constant 0.02 base. The canopy here
// draws with straight alpha (SRC_ALPHA, INV_SRC_ALPHA), so the colour written is the legacy radiance divided by the alpha written, and the blend adds the
// radiance once and darkens what is behind by the same alpha. The overlays add their radiance with ONE, ONE and leave alpha alone.
//
#pragma once

namespace edvr {

// Two adjacent raw literals: MSVC caps one literal near 16 KB, and the text is past half of that.
constexpr char kCockpitGlassLegacyPsHlsl[] = R"HLSL(
cbuffer Frame : register(b1) { float4 cb1[327]; }
cbuffer Material : register(b2) { float4 cb2[18]; }
// EDVR's own, bound by the swap: x is the dirt level (visual.cockpit_glass_dirt / 100, 1 = the look as shipped). Unbound it reads 0, which is no dirt.
cbuffer Level : register(b13) { float4 dirtLevel; }
ByteAddressBuffer ClusterLists : register(t0);
ByteAddressBuffer SunLight : register(t1);
ByteAddressBuffer ProbeRecords : register(t2);
TextureCubeArray<float4> Probes : register(t3);
Texture2D<float4> ShadowMoments : register(t5);
Texture2D<float4> CrackNormalAlpha : register(EDVR_T_CRACK);
Texture2D<float4> Blemish : register(EDVR_T_BLEMISH);
Texture2D<float> Streaks : register(t12);
SamplerState QualitySampler : register(s0);
SamplerState ClampSampler : register(s1);
SamplerState EnvSampler : register(s2);

// The mask component each variant's vertex shader puts the dirt and the imperfection in: intact (crack, dirt, imperfection), ice (crack, ice, dirt,
// imperfection), condensation (crack, dirt, imperfection, condensation).
#if EDVR_MASKS4
#define MaskType float4
#else
#define MaskType float3
#endif
struct PsIn {
    MaskType masks : __USER_KEY_COCKPITGLASSMASKS;
    nointerpolation uint faceInvariant : __USER_VERTEX_FACEINVARIANT;
    float3 lightingNormal : __USER_VERTEX_M_LIGHTINGNORMAL;
    float3 lightingPosition : __USER_VERTEX_M_LIGHTINGPOSITION;
    float3 lightingTangent : __USER_VERTEX_M_LIGHTINGTANGENT;
    float2 texcoord : __USER_VERTEX_M_TEXCOORD;
    float4 position : SV_POSITION;
};

// The sun's shadow: the game's moment shadow map (four moments, a B-spline filter built from four bilinear taps, a Hamburger solve). Ported from the live
// shader's arithmetic, register for register, so the canopy shadows where the rest of the cockpit does.
float4 bsplineWeights(float t) {
    float4 a = t * float4(-0.166667, 0.5, -0.5, 0.166667) + float4(0.25, -0.25, -0.25, 0.25);
    a = t * a + float4(-0.125, -0.625, 0.625, 0.125);
    return saturate(t * a + float4(0.020833, 0.479167, 0.479167, 0.020833));
}

// One 2x2 quad of the four moments, bilinear by hand (the map is sampled point/clamp). Gather order: x (i, j+1), y (i+1, j+1), z (i+1, j), w (i, j).
float4 momentQuad(float2 uv, float tx, float ty) {
    float4 r = ShadowMoments.GatherRed(ClampSampler, uv);
    float4 g = ShadowMoments.GatherGreen(ClampSampler, uv);
    float4 b = ShadowMoments.GatherBlue(ClampSampler, uv);
    float4 a = ShadowMoments.GatherAlpha(ClampSampler, uv);
    float4 rowJ = lerp(float4(r.w, g.w, b.w, a.w), float4(r.z, g.z, b.z, a.z), tx);
    float4 rowJ1 = lerp(float4(r.x, g.x, b.x, a.x), float4(r.y, g.y, b.y, a.y), tx);
    return lerp(rowJ, rowJ1, ty);
}

float momentShadow(float2 uv, float depth) {
    float2 size = cb1[297].xy;
    float2 inv = cb1[297].zw;
    float2 c = floor(uv * size + 0.5);
    float2 f = uv * size - c;
    float4 wx = bsplineWeights(f.x);
    float4 wy = bsplineWeights(f.y);
    float2 sx = float2(wx.x + wx.y, wx.z + wx.w);
    float2 sy = float2(wy.x + wy.y, wy.z + wy.w);
    float tx0 = wx.y / sx.x, tx1 = wx.w / sx.y;
    float ty0 = wy.y / sy.x, ty1 = wy.w / sy.y;
    float2 lo = c * inv - inv;
    float2 hi = c * inv + inv;
    float4 m = sy.x * sx.x * momentQuad(lo, tx0, ty0)
             + sy.x * sx.y * momentQuad(float2(hi.x, lo.y), tx1, ty0)
             + sy.y * sx.x * momentQuad(float2(lo.x, hi.y), tx0, ty1)
             + sy.y * sx.y * momentQuad(hi, tx1, ty1);
    m.x -= 0.035956;
    float4 q = float4(dot(m, float4(0.222774, 0.077197, 0.792699, 0.031942)),
                      dot(m, float4(0.154968, 0.139463, 0.796342, -0.172282)),
                      dot(m, float4(0.145199, 0.212020, 0.725869, -0.275801)),
                      dot(m, float4(0.163127, 0.259143, 0.653909, -0.337613)));
    q = q + cb1[261].y * (0.5 - q);
    float zf = 1.0 - depth;
    float d2 = q.y - q.x * q.x;
    float c1 = (q.z - q.y * q.x) / d2;
    float d3 = (q.w - q.y * q.y) - c1 * c1 * d2;
    float tt = q.x * c1 - q.y;
    float e1 = (zf - q.x) / d2;
    float e2 = (zf * zf + (tt - c1 * zf)) / d3;
    float p0 = 1.0 - q.x * e1 + tt * e2;
    float p1 = e1 - c1 * e2;
    float p2 = e2;
    float root = abs(sqrt(p1 * p1 - 4.0 * p2 * p0) / (2.0 * p2));
    float mid = -p1 / (2.0 * p2);
    float z2 = mid - root;
    float z3 = mid + root;
    bool nearer = z2 < zf;
    bool beyond = z3 >= zf;
    float lowZ = beyond ? zf : z2;
    float highZ = beyond ? z2 : zf;
    float num = q.y + lowZ * z3 - q.x * (z3 + lowZ);
    float g = num / ((z3 - highZ) * (zf - z2));
    float visible = beyond ? saturate(1.0 - g) : saturate(-g);
    visible = nearer ? visible : 1.0;
    visible = saturate((visible - cb1[261].x) / (1.0 - cb1[261].x));
    return visible * visible * (3.0 - 2.0 * visible);
}

// The cascades: the first one the point falls in, and the next one in line to blend with it across the edge (the live loop's two slots).
float sunShadow(float3 pos) {
    float3 sp = pos.y * cb1[299].xyz + pos.x * cb1[298].xyz + pos.z * cb1[300].xyz + cb1[301].xyz;
    uint count = asuint(cb1[326].x);
    bool have1 = false;
    bool have2 = false;
    float3 p1 = 0, p2 = 0;
    float w1 = 0, w2 = 0;
    [loop] for (uint i = 0; i < count; ++i) {
        float3 c = sp * cb1[302 + i].xyz + cb1[310 + i].xyz;
        bool inside = c.x >= -1.0 && c.y >= -1.0 && c.z >= 0.0 && c.x <= 1.0 && c.y <= 1.0 && c.z <= 1.0;
        float edge = min(min(1.0 - abs(c.x), 1.0 - abs(c.y)), 1.0 - abs(c.z * 2.0 - 1.0));
        float lo = cb1[326].y;
        float hi = (i + 1 == count) ? cb1[326].w : cb1[326].z;
        float w;
        if (hi != lo) {
            float t = saturate((edge - lo) / (hi - lo));
            w = t * t * (3.0 - 2.0 * t);
        } else {
            w = edge >= lo ? 1.0 : 0.0;
        }
        float weight = inside ? w : 0.0;
        if (weight != 0.0) {
            float2 uv = cb1[318 + i].xy * c.xy + cb1[318 + i].zw;
            if (!have1) {
                p1 = float3(uv, c.z);
                w1 = weight;
            } else if (!have2 && w1 != 1.0) {
                p2 = float3(uv, c.z);
                w2 = weight;
                have2 = true;
            }
            have1 = true;
        }
        if (have1 && (have2 || w1 == 1.0)) break;
    }
    float s1 = have1 ? momentShadow(p1.xy, p1.z) : 1.0;
    float s2 = have2 ? momentShadow(p2.xy, p2.z) : 1.0;
    float second = 1.0 + w2 * (s2 - 1.0);
    return second + w1 * (s1 - second);
}
)HLSL" R"HLSL(
// MEASURED from the legacy client by RenderDoc (docs\cockpit-glass-legacy-port.md section 10).
static const float kLegacyDirtMask = 0.025;    // the dirt mask multiplier on the vertex colour's blue channel (VS cb0[3].x)
static const float kLegacyDirtColour = 0.97;   // the dirt colour scale (PS cb0[35].z)
static const float kLegacyBaseAlpha = 0.02;    // the constant alpha base (PS cb0[35].y: 0.0212, 0.0211, 0.0185)
static const float kImperfectionSpec = 0.001;  // smudge glare strength (cb0[39].x)
static const float kImperfectionGloss = 120.0; // and its exponent (cb0[39].y)
static const float kCrackSpec = 0.02;          // crack glare strength (cb0[38].y), times the frame's alpha multiplier
static const float kCrackGloss = 450.0;        // and its exponent (cb0[38].z)
// The streak texture is stored normalised (its brightest texel 1, its mean 0.267); times this, the max is the legacy dirt texture's 0.09 and the mean its 0.024.
static const float kStreakMax = 0.09;
// CHOSEN, not measured: the dirt albedo is multiplied by this. Sean found 16 too much for a clean windshield, and then 8 "probably still too intense", with too much brightness in
// the dirt texture compared to legacy; section 12 of the doc measures about 4 to 6 times as matching legacy on the display. Dirt albedo only: the smudge and crack glare and the
// crack colour are not scaled. The player's visual.cockpit_glass_dirt scales it again, live, through the Level block (b13): 100 % is this value.
static const float kDirtStrength = 4.0;
// CHOSEN placement, not measured (Sean approved it, 2026-11-03): where the dirt sits. Legacy's dirt mask covered most of the pane, and the game's vertex mask reads as frost at the
// frame (a bright frame against the struts, faceted by the triangles, 37% of the visible pane at exactly 0 on the Vulture). So the grime never ends: a film everywhere on the pane,
// a small softened gather toward the frame from the vertex mask (no kink where it reaches 0), and patches from the streak texture itself, unrelated to the mesh. In the units of
// dirtWeight (live's mask over CB2[6].z * CB2[0].x, 0 to 0.8 on the ships measured).
// 0.16 and 0.2 kept the old average but halved the brightest areas, and Sean saw clear glass ("crystal clear"). These put the frame area back near the 0.7 he called subtle.
static const float kGrimeFilm = 0.4;            // everywhere, the main term
static const float kGrimeEdge = 0.3;            // the vertex mask's gather toward the frame, at most
static const float kEdgeFull = 0.8;             // dirtWeight at which the gather is full
static const float kPatchScale = 0.19;          // the patch sample's uv scale: not a simple ratio, so it does not line up with the main repeat
static const float kPatchMip = 5.0;             // of 10: 16 x 16 texels, stored 0.149 to 0.491
static const float kStreakStoredMean = 0.2687;  // the streak texture's stored mean (cockpit_glass_streaks.h)
static const float kPatchContrast = 0.7;        // the multiplier is 1 + this x (stored - mean) / mean: 0.69 to 1.58 over mip 5, mean 1; clamped to 0.4 .. 1.6
// Fixed choices.
static const float kInvPi = 0.318310;          // live's own factor on the direct light: its t1 light colour is in units where diffuse = albedo * ndl / pi * colour
static const float kAmbientMip = 7.0;          // the roughest mip the live shader samples (its sqrt(roughness) * 8, capped at 7)
static const uint kMaxProbes = 16;             // the live loop stops when the weights reach 1; this bounds a cluster that never does
static const float kMinCoverage = 1.0 / 1024.0;  // the overlays skip a pixel whose dirt, smudge and crack are all under this

// The ambient: the game's clustered reflection probes, looked up as the live shader does (the pixel's cluster from its tile and depth, the probes in it
// weighted by their volumes, summed until the weights reach 1), but sampled once at the roughest mip along the normal, scale 1.0. The ambient is the albedo times the probe's radiance; the legacy client's own ambient (its vertex shader's cube) was zero in space, so there is nothing measured to match. That is the stand-in for the
// legacy vertex shader's cube ambient, and the canopy stays lit in a hangar, where the sun is off and the frame's ambient colour is 0.
float3 probeAmbient(float3 viewPos, float3 nView, float2 pixel) {
    uint tile = max(asuint(cb1[228].w), 1u);
    uint2 tc = (uint2)floor(pixel / (float)tile);
    uint nx = asuint(cb1[228].x), ny = asuint(cb1[228].y), ns = asuint(cb1[228].z);
    int slice = clamp((int)(log(viewPos.z) * cb1[227].z + cb1[227].w), 0, (int)ns - 1);
    uint cluster = nx * tc.y + tc.x + (ny * nx) * (uint)slice;
    uint listBase = cluster * 2372u;
    uint count = ClusterLists.Load(listBase + 28u);
    uint first = asuint(cb2[13].z) != 0u ? max(count, 1u) - 1u : 0u;
    float3 acc = 0;
    float accW = 0;
    [loop] for (uint i = first; i < count && i < first + kMaxProbes; ++i) {
        uint id = ClusterLists.Load(listBase + 2212u + i * 4u);
        uint volume = id * 160u + asuint(cb1[253].y);
        uint header = id * 48u + asuint(cb1[253].z);
        float4 centre = asfloat(ProbeRecords.Load4(volume + 80u));
        float4 axisX = asfloat(ProbeRecords.Load4(volume + 112u));
        float3 axisY = asfloat(ProbeRecords.Load3(volume + 128u));
        float3 axisZ = asfloat(ProbeRecords.Load3(volume + 144u));
        uint4 head = ProbeRecords.Load4(header);
        float weight = 1.0;
        if (head.w != 0u) {
            float3 d = viewPos - centre.xyz;
            float blend = max(asfloat(head.z), 0.00001);
            if (head.w == 1u || head.w == 3u || head.w == 4u) {
                float4 ext = asfloat(ProbeRecords.Load4(volume + 96u));
                float3 loc = float3(dot(axisX.xyz, d), dot(axisY, d), dot(axisZ, d));
                float3 q = (abs(loc) - (ext.xyz - blend)) / blend;
                weight = saturate(1.0 - max(q.x, max(q.y, q.z)));
                if (head.w == 3u) {
                    float t = saturate((loc.z / ext.z + 1.0) * 0.5);
                    float widen = lerp(ext.w, axisX.w, t);
                    weight = min(weight, saturate((ext.x * widen - length(loc.xy)) / blend));
                } else if (head.w == 4u) {
                    float4 plane = asfloat(ProbeRecords.Load4(header + 32u));
                    weight = weight * saturate((dot(plane.xyz, viewPos) + plane.w) / length(plane.xyz) / blend);
                }
            } else {
                weight = saturate(1.0 - (length(d) - (centre.w - blend)) / blend);
            }
        }
        if (weight <= 0.0) continue;
        float3 dir = float3(dot(axisX.xyz, nView), dot(axisY, nView), dot(axisZ, nView));
        float3 c = Probes.SampleLevel(EnvSampler, float4(dir, (float)head.x), kAmbientMip).rgb * asfloat(head.y);
        float use = min(weight, 1.0 - accW);
        acc += c * use;
        accW += use;
        if (accW >= 1.0) break;
    }
    return acc * cb1[90].z;
}

float4 main(PsIn i) : SV_Target {
    // The tangent frame and the face flag, as the live shader builds them: the vertex normal, the tangent, and their cross product flipped for a back face.
    float3 N = normalize(i.lightingNormal);
    float3 T = i.lightingTangent;
    float3 B = cross(N, T);
    if ((i.faceInvariant & 0x80000000u) != 0u) B = -B;

    float2 uv = i.texcoord * cb2[15].xy;
    float4 crackTex = CrackNormalAlpha.Sample(QualitySampler, uv);
    float2 blemish = Blemish.Sample(QualitySampler, uv).xy;
    float streak = Streaks.Sample(QualitySampler, uv) * kStreakMax;

    // Coverage. crackMix drives the normal; crackA (scaled by the material's occlusion scale) drives the crack colour and, intact, alpha.
    float crackMix = saturate((cb2[17].z < i.masks.x ? 1.0 : 0.0) * crackTex.w);
    float crackA = saturate(crackMix * cb2[7].z);
    // The dirt mask is the legacy client's 0.025 times the vertex colour's blue channel, as far as live's vertex shader allows. It passes saturate((B + CB2[6].w) * CB2[6].z) *
    // CB2[0].x, so dividing by CB2[6].z * CB2[0].x gives max(B + CB2[6].w, 0): B minus its offset (B - 0.2 on the flown ship), which runs smoothly to 0. The offset must NOT be
    // added back: that jumps from 0 to the offset at the boundary of every triangle whose three vertices live's clamp took to 0, and the zero regions get hard polygon outlines.
    float liveDirt = i.masks[EDVR_MASK_DIRT];
    float denom = cb2[6].z * cb2[0].x;
    float dirtWeight = denom > 0.000001 ? max(liveDirt / denom, 0.0) : 0.0;
    // Placement (CHOSEN, see kGrimeFilm): the film everywhere, a softened gather from the vertex mask, and patches from the streak texture at an unrelated scale and a high mip.
    // With no streak texture bound (t12 not ready) the dirt is off, as the streak factor in the albedo says.
    float edge = smoothstep(0.0, 1.0, saturate(dirtWeight / kEdgeFull));
    float grime = kGrimeFilm + kGrimeEdge * edge;
    float patchStored = Streaks.SampleLevel(QualitySampler, i.texcoord * kPatchScale, kPatchMip);
    float patch = clamp(1.0 + kPatchContrast * (patchStored - kStreakStoredMean) / kStreakStoredMean, 0.4, 1.6);
    float dirtMask = kLegacyDirtMask * grime * patch;
    // The smudge mask: live's imperfection mask times the blemish map's y channel. The legacy client's was a vertex alpha channel (1.2 A - 0.2), which the
    // live vertex shader does not pass, so live's own is kept.
    float imp = saturate(blemish.y * i.masks[EDVR_MASK_IMP]);
#if EDVR_OVERLAY
    // Nothing to light: no dirt, no smudge, no crack. (The intact variant always draws: the legacy alpha base darkens the whole pane.)
    if (crackA + dirtMask + imp < kMinCoverage) return float4(0, 0, 0, 0);
#endif
    float tint = cb1[56].x;
    float alpha = saturate(crackA + max(tint, kLegacyBaseAlpha));

    // The lit normal: the legacy client's. Flat glass has the tangent-space normal (0, 0, 1); a crack bends it to the decoded crack normal. The glass is lit
    // from inside, so the vertex-normal share points the other way (Nw = x T + y B - z N). In this slot order (T, N, B) that is w = lerp((0, 1, 0), (nx, nz,
    // ny), crackMix), then x T - y N + z B. (Live's own lerp starts from the encoded flat value (0.5, 0.5, 1.0), which tilts intact glass by about 66 degrees;
    // that is not ported.)
    float2 nxy = crackTex.xy * 2.007874 - 0.999985;
    float nz = sqrt(max(1.0 - dot(nxy, nxy), 0.0));
    float3 w = lerp(float3(0.0, 1.0, 0.0), float3(nxy.x, nz, nxy.y), crackMix);
    float3 nIn = w.x * T - w.y * N + w.z * B;
    nIn = normalize(float3(dot(nIn, cb1[277].xyz), dot(nIn, cb1[278].xyz), dot(nIn, cb1[279].xyz)));
    float3 pv = float3(dot(cb1[277].xyz, i.lightingPosition), dot(cb1[278].xyz, i.lightingPosition), dot(cb1[279].xyz, i.lightingPosition));

    // The legacy model: one directional light, Lambert times shadow, a normal-independent glare lobe toward the light, and the ambient.
    float3 lightColour = 0;
    float3 lightDir = float3(0, 0, -1);
    if (asuint(cb1[245].y) > 0u) {
        lightColour = asfloat(SunLight.Load3(0));
        lightDir = asfloat(SunLight.Load3(16));
    }
    float shadow = 1.0;
    if (lightColour.x + lightColour.y + lightColour.z > 0.001) shadow = sunShadow(i.lightingPosition);
    float ndl = saturate(dot(nIn, -lightDir));
    float specAmt = lerp(imp * kImperfectionSpec, kCrackSpec * cb1[56].y, crackA);
    float gloss = lerp(kImperfectionGloss, kCrackGloss, crackA);
    float glare = pow(saturate(dot(-lightDir, normalize(pv))), gloss) * (gloss + 6.0) * 0.03125;
    // The dirt is grey (R = G = B): the streak texture times the mask times the colour scale.
    float3 albedo = lerp((streak * dirtMask * kLegacyDirtColour * kDirtStrength * dirtLevel.x).xxx, cb2[8].yzw * cb1[56].y, crackA);
    float3 ambient = probeAmbient(pv, nIn, i.position.xy);
    // The direct light is scaled by 1/pi, as live scales its Lambert and its lights loop; the probe ambient is radiance from the cube and is not.
    float3 direct = shadow * kInvPi * lightColour;
    float3 radiance = (ndl * direct + ambient) * albedo + specAmt * glare * direct;
    if (any(isnan(radiance))) radiance = 0;
#if EDVR_OVERLAY
    // Over the game's own ice or condensation draw: the legacy light alone, added (ONE, ONE, alpha untouched). The game's draw did its own coverage.
    return float4(min(radiance * cb1[90].y, 60000.0), 0.0);
#else
    // Straight alpha: the legacy radiance divided by the alpha written, so the blend adds the radiance once.
    return float4(min(radiance / alpha * cb1[90].y, 60000.0), alpha);
#endif
}
)HLSL";

}  // namespace edvr
