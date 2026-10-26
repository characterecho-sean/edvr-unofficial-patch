# Camera struct: fields, both bases, and their owners

## Status

- **What this is:** a lookup for the game camera struct's fields EDVR reads
  or writes. Each field has a camera-relative offset and, because game code
  often reaches the struct through a helper base at **camera+0x20**, a
  helper-relative one (helper+X = camera+(X+0x20)). Struct offsets have held
  across game builds 332841, 334473 and 334925.
- **Evidence:** the decompiled cache helpers in
  `design-flat-camera-integration.md`, "Cache-helper addendum"; this doc
  indexes that table and the source constants, computing the helper column
  for the regions the addendum gives only camera-relative.
- **Open:** two source files define the same fields (`kCam*` in
  `src\d3d11\flat_camera_inject.cpp`, `kVrCensus*` in
  `src\d3d11\vr_camera_census_core.h`); no check holds them to each other.

## Fields

| Field | Camera+ | Helper+ (camera+0x20) | Source constants |
| --- | --- | --- | --- |
| view axes (3x4) | 0x20..0x4C | 0x00..0x2C | |
| position (XYZ) | 0x50..0x58 | 0x30..0x38 | |
| view rows | 0x190..0x1CC | 0x170..0x1AC | |
| projection 4x4 | 0x1D0..0x20C | 0x1B0..0x1EC | kVrCensusProj |
| view-projection 4x4 (cached) | 0x210..0x24C | 0x1F0..0x22C | |
| dirty flags (bit 2 view, 4 projection, 8 view-projection) | 0x250 | 0x230 | kCamFlags, kVrCensusFlags |
| near, far | 0x254, 0x258 | 0x234, 0x238 | kCamNear/Far, kVrCensusNear/Far |
| compound-near adjustment | 0x25C | 0x23C | |
| aspect | 0x260 | 0x240 | kCamAspect, kVrCensusAspect |
| **projection kind** | **0x264** | **0x244** | kCamKind, kVrCensusKind |
| vertical field of view | 0x280 | 0x260 | kCamFov, kVrCensusFov |
| off-centre bounds X, Y | 0x28C, 0x290 | 0x26C, 0x270 | kCamBoundX/Y, kVrCensusBoundX/Y |
| viewport W, H (refresh reads; not the adjust flag) | 0x2A0, 0x2A4 | 0x280, 0x284 | kVrCensusViewportW/H |
| projection-adjust enable | 0x2C0 | 0x2A0 | |
| custom matrix data (kinds 4 and 5) | 0x2D0..0x30C | 0x2B0..0x2EC | |

Projection kind values: 1 orthographic, 3 trigonometric (the on-foot world
camera), 4 and 5 custom matrix (5, the headset eyes, adds near/far depth
terms).

## Reading an offset

- A decompiled `[reg+0x244]` is the kind only when `reg` holds camera+0x20;
  on the camera itself, +0x244 is inside the cached view-projection matrix.
  Find what the register was loaded from before naming the field.
- The eye-camera setter builds its record at the helper base, so its stack
  offsets are helper-relative (`game-binary-map.md`, "Projection kind", once
  that doc is on main).
