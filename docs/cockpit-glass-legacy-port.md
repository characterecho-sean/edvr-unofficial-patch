# Legacy cockpit glass as a Visual option

## Status

State 2026-11-03: **OFFLINE ANALYSIS DONE; CAPTURE PROBE FLOWN, ALL ANSWERED.** The ask (Sean,
2026-11-03): port the legacy client's cockpit glass shader and offer it as a
Visual-tab option in both flat and VR. Branch
`claude/cockpit-glass-shader-port-1852e3`.

Settled offline (sections 1 to 3):

- Both builds' glass shaders are in `Win64\EffectsBinary\*.arc`, which
  `tools\effects_arc.py` now reads. Legacy (Horizons 3.8, build 312425):
  effect `CockpitGlassEffects3`. Live: effect `CockpitGlass_Forward`.
  CONFIRMED that it is the canopy: its VS is byte-identical to
  `8C091FFD08644E02`, the canopy VS EDVR logs (`kHoloCanopy`). The flight
  dump also holds its PS `4E4FF61E8A08FC7E` (state 0, quality 3).
- **The legacy shader cannot be dropped in.** Legacy is a small `ps_4_1`
  that reads one per-draw cb0. Live reads the frame block CB1 (333
  registers, dynamically indexed), a material block CB2, raw light
  buffers, a reflection-probe cube array and a moment shadow map. The VS
  outputs differ too. A port is a new EDVR pixel shader that works the
  legacy lighting out from live's own bindings (section 4).
- EDVR's shader hash (`src\common\log.cpp`) uses offset basis
  1469598103934665603, not the standard FNV basis. That is why
  `fss-scanner.md` and `bloom-artifact-2026-10-20.md` found nothing in
  these archives. Their "no DXBC offline" conclusion is withdrawn.

Probe FLOWN 2026-11-03 (Steam, flat, docked and in sunlight; section 7).
It answered C1 to C6. What it changes: only intact q3 draws; blend is
straight alpha; t1 holds one light, direction in view space; docked, the
sun is off and CB1[257] ambient is 0 everywhere, so a legacy model on t1
alone leaves the canopy unlit in a hangar and the port must read the
probes. Live's dirt on this ship: a constant warm grey at 7.5% of its mask.

Decisions for Sean (section 6):

- Is an approximation of the legacy look acceptable? Live has no RGB dirt
  texture, and live's shadow map is a moment map, not depth.
- Key name and values.
- Whether ice and condensation stay stock in the first version.
- Legacy RGB dirt (item 4): live side measured; the legacy side needs a
  legacy-client capture.

Next: no flight. The decisions above, then the replacement shader.

Temporary: `experimental.cockpit_glass_probe` (`cockpit_glass_probe.*`,
`tools\cockpit_glass_probe_test`, the `:rig_cockpit_glass_probe_test`
rig, its flat allow-list line). Remove all of it when the shader is
written.

Analysis: `analysis\decomp\verify_20261103_cockpit_glass_legacy.txt`
(312425) and `..._live.txt` (334925), filed 2026-11-03. They hold the
permutation tables, the cb0 layout and the legacy PS written out.
`..._live_mapping.txt` holds the legacy-to-live input table; the decode
scripts are `..._*.py` beside them (scratch, not runnable as filed).

Ruled out: see the end of this doc.

## 1. The archives and the hash

`Win64\EffectsBinary\` holds `Effects.arc` in the legacy client and
`Effects_Win64_SM50.arc` plus `Effects2_Win64_SM50.arc` in Odyssey. Each
is a 16-byte header and then a raw-deflate stream. Inflated, it is a run
of records, each a u16 version, a two-character tag, a 4cc (`sd3f` shader,
`xd3f` effect) and a u32 size. A shader record carries one DXBC blob and a
16-byte id. An effect record carries its parameter names, its own name,
and pass tables that name shaders by that id. All the format notes are in
`tools\effects_arc.py`.

Counts: legacy 884 effects and 3,841 shaders; live 976 effects and 2,728
shaders.

The flight dump (`edvr_logs\shaders`, 1,187 blobs on Frontier, newest
2026-11-03) shares no hash with the archives under the standard FNV
basis. It shares 634 under EDVR's. An archive blob is byte for byte what
the game passes to `CreatePixelShader`, so

    python tools\effects_arc.py find --arc <Effects_Win64_SM50.arc> --arc <Effects2_Win64_SM50.arc> --hash <EDVR hash>

names the effect behind any hash in an EDVR log. The basis itself is not
a bug to fix: every shader constant in `src\` is keyed on it.

## 2. The two effects

**Legacy `CockpitGlassEffects3`: 144 passes.** The selectors are the
directional-light count (0 to 2), the directional-shadow count, a
high-resolution screen-capture flag, `gCockpitGlassState` (0 intact, 1
ice, 2 condensation) and `gMaterialQualityLevel` (0 to 3). Shadows apply
only at quality 2 or above. For the intact canopy, the VS is
`BD3029C5D0C713EF` with lights, or `682879948FBB8604` with none. The PS
depends on the light and shadow counts:

| Lights, shadows | PS (EDVR hash) |
|---|---|
| 1, 1 (likely in a lit cockpit) | `A816DE90739BF4C0` |
| 2, 1 | `B66C914B1CF50AC0` |
| 1, 0 | `AD41A6B822371609` |
| 2, 0 | `88CCD990D3DA2280` |
| 0 | `E8E2A17AD8C64F5E` |

**Live `CockpitGlass_Forward`: 24 passes**, 12 distinct ones: state 0 to 2
by quality. The VS depends on state only. Quality 0 to 2 read the
directional-light buffer alone. Quality 3 adds the clustered light lists,
the reflection-probe cube array and the specular LUT. The flight dump
holds only state 0 at quality 3 (VS `8C091FFD08644E02`, PS
`4E4FF61E8A08FC7E`). Its slots: t0 light lists, t1 directional lights, t2
lights, t3 reflection cube array, t4 specular LUT, t5 shadow map, t6
crack normal and alpha, t7 crack interior normal, t8 crack id, t9 packed
blemish maps.

`Metalness_Roughness_CockpitSimpleGlass_Forward` and the `GlassAdvanced_*`
and `GlassSimple_*` effects are other glass, not the canopy.

## 3. What the legacy look is

All of this was read from the legacy PS for the intact canopy. The
decompiled form goes to `analysis\` (Status), not into this repository.

- **Coverage.** A dirt layer (an RGB dirt texture times a vertex dirt
  mask), an imperfection mask, and cracks gated by a vertex crack mask
  against a threshold. The crack normal map tilts the normal.
- **Lighting.** Up to two directional lights, as Lambert times shadow,
  plus an ambient term the VS samples from a cube map. A
  normal-independent glare lobe toward each light is added on top. The
  shadow uses up to 8 cascades with a 4x4 PCF of a depth map.
- **No** reflection probes, GGX or Fresnel, local lights, fog, exposure
  or tone-map in the PS. Exposure enters only through the VS ambient
  scale.
- **Output.** Colour already weighted by coverage. Alpha is the crack
  term plus the tint alpha. That reads as premultiplied, but the blend
  state is not in the archive.

Live's canopy instead lights the glass with GGX and Fresnel, adds the
reflection probes at quality 3, scales albedo and roughness by one minus
the tint alpha, and writes in HDR units (`gHDRScalingWrite`, CB1[90].y).
The probe reflections and the specular model are where the two looks
part.

## 4. Design of the port

- **Key.** `visual.cockpit_glass`, `odyssey` (default) or `legacy`, in the
  Visual tab, both profiles: add it to the flat allow-list
  (`runtime_profile.h`) and to `edvr.ini` (`check_config_contract`). The
  key can change live: the swap is made at bind time.
- **Where.** `hookedCreatePS` notes the live canopy permutations by hash.
  The `hookedPSSetShader` path binds EDVR's replacement in their place
  while the key is `legacy`. That is a pointer compare per bind and no
  new D3D call per draw, as the plugin design requires. It belongs in the
  cockpit-visuals plugin.
- **The replacement.** An EDVR HLSL pixel shader with the live VS's
  output signature, so it links with `8C091FFD08644E02`. It reads only
  what the game has already bound: CB1, CB2, t1, t5, t6, t9, s0 and s1. It
  computes the section 3 model: the live crack and blemish masks, two
  directional lights from t1 times live's shadow resolve, the legacy
  glare lobe, ambient from CB1[257], legacy alpha, and output scaled by
  CB1[90].y. Quality 0 to 2 bind different slot layouts, so each layout
  needs its own variant. The first version covers state 0 only, and ice
  and condensation stay stock.
- **Gaps** (section 6): live has no RGB dirt texture, only a constant
  dirt colour `pDirtDiffuse` (CB2[11]) times a blemish channel. The
  legacy depth PCF becomes live's moment-shadow resolve.
- **Acceptance.** Screenshots from the same ship and station: the legacy
  client, EDVR `legacy`, and EDVR `odyssey`, flat first, then VR. Plus
  frame time at the same spot: with the probes gone the replacement
  should cost less than stock, never more.

Environment: game build 334925 with `Effects2_Win64_SM50.arc` as
rewritten 2026-10-20. A game update that rebuilds the archive changes the
hashes. `effects_arc.py find` re-identifies them offline. Runtime and
headset independent: one draw, both eyes, flat or VR.

## 5. The capture flight

One flight answers all of these together.

Every line starts `glass probe:`. A sample's lines all carry
`sample N`.

| # | Question | Log signature that settles it |
|---|---|---|
| C1 | Blend, depth and write mask of the canopy draw, and its target format | `sample N om blend rt0` / `rt1`, `om depth`, `om raster`, `om rtvK`, `om dsv` |
| C2 | Permutations drawn in flight | `window 30 s draws=… ps: <hash>(intact q3)=… vs: … other-ps=…` |
| C3 | Frame constants the port reads | `sample N cb1 [r] x y z w …` for registers 55-56, 61, 90, 245, 248, 257, 261, 275-279, 290-326 |
| C4 | Material constants | `sample N cb2 [0]…[17]` |
| C5 | Directional-light buffer layout (colour at +0, direction at +16, stride 48, as read from the PS) | `sample N t1 [0]…[11]`: the first 192 bytes from the view's first element |
| C6 | Slot formats | `sample N srv tK …` (t0 to t11) and `sample N smp sK …` (s0 to s2); `cb bK bytes= range=` says whether register 0 is byte 0 |

A component that is an integer in the shader's eyes (its bits are a
denormal) prints `#n`, and a register past the copied range prints `-`.

When it logs: the first canopy draw (`sample 1 draw`), then at most once
every 30 s, 12 at most. The constants and t1 are GPU copies made at the
draw and read at a frame boundary two or more frames later
(`sample N read back`). What a failure looks like:

- the key never read: no `armed` line;
- no canopy draw reached the hooks: `window` lines with `draws=0`;
- a readback that failed: `readback map failed` or `dropped`.

Flat is the cheaper flight; a lit station hangar and open space in
sunlight give two light setups.

Not answerable from the CPU: the sign of the live `faceinvariant` input
and whether the lighting normal faces in or out. The replacement's first
build carries a debug view for those.

## 6. Decisions for Sean

1. **Approximate look.** The option gives legacy lighting on live's glass
   material, not legacy pixels. The dirt texture and the shadow filter
   differ (section 4). Proceed on that basis?
2. **Key.** `visual.cockpit_glass = odyssey | legacy`?
3. **Scope.** Is the intact canopy only enough for the first version,
   with ice and condensation left stock?
4. **Legacy RGB dirt.** The shader side ports. The blocker is the
   texture. The legacy client's Win64 asset files (a 400-file sample:
   70% `FREA`, 30% zlib with an unknown inner format, no texture names)
   are not decoded, and the texture is Frontier's artwork, so EDVR does
   not ship it. Shippable routes: read it at run time from the player's
   own legacy install, or an EDVR-made look-alike. A legacy-client frame
   capture in flat would show it. Decided 2026-11-03: wait for the probe
   flight's `pDirtDiffuse` (CB2[11]) and t9 values, which say how far
   live's flat-colour dirt already is from legacy, then choose. The
   flight (section 7) gives the live side only.

## 7. Probe flight 2026-11-03 (Steam, flat)

Log `edvr_gfx_20261103_090854.log`, build `v0.22.0-rc.5-50-g625de17e`
(matches). Flat 3840x2160. Eight samples: 1 to 4 docked in a station
hangar, 5 leaving, 6 to 8 in open space in sunlight. No readback was
dropped. One ship throughout.

- **C2, permutations.** Only PS `4E4FF61E8A08FC7E` (intact, q3) with VS
  `8C091FFD08644E02`, one draw a frame (about 3,050 per 30 s), docked
  and in space. `other-ps=0`. Ice, condensation and q0 to q2 never drew
  on this rig.
- **C1, output state.** Blend is straight alpha on colour and alpha
  (`SRC_ALPHA`, `INV_SRC_ALPHA`), write mask 0xF, one RTV,
  R11G11B10_FLOAT. Depth test `GREATER_EQUAL` (reverse Z), no depth
  write. Stencil writes 5 under mask 0x5. Back faces culled. The blend
  multiplies colour by alpha again, though live's albedo already carries
  coverage, so live's dirt colour is scaled by alpha twice.
- **C5, the light buffer.** t1 is 48 bytes: one light, and
  `g_numDirectionalLights` CB1[245].y = 1. Offset +0 is the colour in HDR
  units, the same as CB1[293] (about 800 to 2,360 in sunlight). Offset
  +16 is the direction in view space: CB1[277] and [278] dotted with the
  world sun direction CB1[291] give t1's x and y (checked on sample 6).
  Offset +32 reads (1, 0, 0, int 1). Legacy's second light has no source.
- **Docked, the sun is off.** Its colour is 1e-13 in every docked
  sample. A hangar is lit only by local lights and the reflection probes
  (t0, t2, t3), none of which the legacy model reads.
- **No ambient constant.** `gAmbientColour` CB1[257] is 0 in all eight
  samples. The legacy VS ambient has to come from the probes (t3) or not
  at all.
- **C3, other frame constants.** `gHDRScalingWrite` CB1[90].y = 1/256
  (read scale .z = 256). Shadow: t5 3072x2048 R16G16B16A16, six cascades
  (CB1[326].x) in a 3x2 atlas (CB1[318..323]). CB1[55] = (1, 0, 0, 0):
  no cracks, ice or condensation. CB1[56] = (0, 1): tint alpha 0, alpha
  multiplier 1.
- **C4, material** (CB2, unchanged across samples): `pDirtDiffuse` (0.274,
  0.246, 0.208), a warm grey; dirt alpha multiplier CB2[0].x 0.5; mask
  scale and offset CB2[6].zw 0.5 and -0.2; dirt roughness CB2[13].y 0.8.
  CB2[17].w = 0.8787, so dirt and imperfection coverage is scaled by
  1 - saturate(0.8787 x 1.0526) = 0.075. On this ship live's dirt is a
  constant warm grey at 7.5% of its mask. What CB2[17].w is (a per-ship
  wear value, or fixed per material) is unknown from one ship.
- **C6, slots.** t3 is a BC6H 256x256 cube array of 64 cubes, t4 a
  512x512 R16G16_FLOAT LUT, t6 and t7 BC7, t8 BC4, t9 BC7 1024x1024. s0 anisotropic
  x16 wrap; s1 and s2 trilinear clamp. b1 is 5,376 bytes, b2 288, both
  bound from byte 0.

What this changes in section 4: the replacement needs the probes (t0,
t2, t3) for ambient and for any light in a hangar, one directional
light, and output written for a straight-alpha blend. Only the q3
intact variant is needed on this rig.

## Ruled out

- ruled out: swapping the legacy bytecode in, because it reads cb0 where
  live binds CB1/CB2, its texture slots differ, and its input signature
  does not match the live VS outputs (sections 2 and 3).
- ruled out: "the Effects archives hold no reachable DXBC"
  (`fss-scanner.md`), because the earlier scans used the standard FNV
  basis; under EDVR's basis 634 dumped shaders match archive blobs
  byte for byte.
- ruled out: ambient from `gAmbientColour` CB1[257], because it was 0 in
  all eight probe samples, docked and in sunlight (section 7).
- ruled out: lighting the canopy from t1 alone in a hangar, because the
  sun's colour there is 1e-13; hangar light reaches the glass only
  through local lights and probes (section 7).
