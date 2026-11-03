# Legacy cockpit glass as a Visual option

## Status

State 2026-11-03: **OFFLINE ANALYSIS DONE, NOTHING BUILT.** The ask (Sean,
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

Open, needs a capture (section 5): the canopy draw's blend, depth and
target state; which permutations are bound in flight; the CB1/CB2 values
and the directional-light buffer layout the port reads. The existing
census cannot get these: `census_cb_watch` stops at 768 bytes, and the
registers needed run from CB1[55] to CB1[326].

Decisions for Sean (section 6):

- Is an approximation of the legacy look acceptable? Live has no RGB dirt
  texture, and live's shadow map is a moment map, not depth.
- Key name and values.
- Whether ice and condensation stay stock in the first version.

Next: build the glass probe (temporary key
`experimental.cockpit_glass_probe`, section 5) and fly once, flat, in a
lit cockpit. Then write the replacement shader against the logged values.

Temporary keys: none yet. The probe key above will be one; remove it when
the arc closes.

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

| # | Question | Log signature that settles it |
|---|---|---|
| C1 | Blend, depth and write mask of the canopy draw, and its target format | `glass probe: om` line: blend desc RT0, depth-stencil desc, RTV format and size |
| C2 | Permutations bound in flight | per-hash bind counts for all 9 live PS and 3 VS over the session |
| C3 | Frame constants the port reads | CB1 registers 55, 56, 61, 90, 245, 248, 257, 261, 275-279 and 290-326 on one sampled draw |
| C4 | Material constants | the whole of CB2 (18 registers) on the same draw |
| C5 | Directional-light buffer layout (colour at +0, direction at +16, stride 48, as read from the PS) | the first 96 bytes of t1, copied once |
| C6 | Slot formats | `srv` lines for t0 to t11 (dimension, format, size) and sampler descs for s0 to s2 |

To log: once at the first canopy draw, then at most once every 30 s,
with the PS hash and q on each line. If the probe never runs, the log
shows `glass probe: armed` and no `om` line, so that cannot pass for
success. Flat is the cheaper flight; a lit station hangar and open space
in sunlight give two light setups.

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

## Ruled out

- ruled out: swapping the legacy bytecode in, because it reads cb0 where
  live binds CB1/CB2, its texture slots differ, and its input signature
  does not match the live VS outputs (sections 2 and 3).
- ruled out: "the Effects archives hold no reachable DXBC"
  (`fss-scanner.md`), because the earlier scans used the standard FNV
  basis; under EDVR's basis 634 dumped shaders match archive blobs
  byte for byte.
