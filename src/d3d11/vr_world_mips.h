// The VR world route's mipped screen (design doc section 82, "Layer"): the game's screen texture, copied once a frame into
// a texture with a full mip chain, so the layer's re-issue of the eye composite minifies 5040 to about 3500 without aliasing.
//
// THE COLOUR-SPACE DECISION (section 82, 2026-09-30). The screen texture is R8G8B8A8_TYPELESS and the game views it as
// UNORM: display-encoded 8-bit, gamma-space numbers. The mips are generated through an _SRGB view of the mipped copy, so the
// box filter averages in linear light and re-encodes (energy preserving: a bright thin stroke, which is what HUD text is, is
// not dimmed as a gamma-space average dims it), and the layer samples it through the UNORM view the game's own shader
// expects (a sampled _SRGB view would hand the shader linear values and the layer would come out dark). Mip 0 is a
// byte copy either way.
#pragma once
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11SamplerState;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct D3D11_SAMPLER_DESC;

namespace edvr {

// Copy `screen` (a 2D, single-sample, single-mip R8G8B8A8-family texture) into the route's mipped texture and generate its
// mips, once per `frame`: a second call for the same frame and the same texture returns the same view without any GPU
// work. Returns the UNORM SRV over all mip levels to sample it through, valid until the next frame's call; null on any
// refusal (the reason is logged once), and the caller leaves the eye route to serve the eye. The GPU work is timed on
// GpuCensusSection::FrameWorldMips. A new size, a new source format or a new device remakes the mipped texture; another
// screen texture of the same size and format only redoes the frame's copy into the one there is. Render thread.
ID3D11ShaderResourceView* vrWorldMipsScreen(ID3D11DeviceContext* ctx, ID3D11Texture2D* screen, uint64_t frame);

// A sampler like `game` (the address modes, the comparison-free rest) but trilinear with the full LOD range, created once
// per distinct game sampler and cached: the game's own s0 may have MaxLOD 0 or a point mip filter, which would waste the
// mips. Null on failure. The table behind it holds 8 samplers (a game sampler that differs only in what the route
// overrides is the same entry); a ninth distinct one evicts the oldest, so a returned pointer is the table's, not the
// caller's, and stays valid until vrWorldMipsReset, a change of device, or that eviction.
ID3D11SamplerState* vrWorldMipsSampler(ID3D11Device* device, const D3D11_SAMPLER_DESC& game);

// Let go of everything (device change, the key turned off, a size change). Render thread.
void vrWorldMipsReset();

// ---- TEST SECTION: read by tools\vr_world_mips_test only; nothing in the DLL calls these ---------------------------
// Why vrWorldMipsScreen turned a texture away. The names are what the log and the rig say; None is "accepted".
enum class VrWorldMipsRefusal : uint8_t {
    None = 0,
    NullArgument,    // no context or no texture
    Multisampled,    // more than one sample per texel
    TextureArray,    // more than one array slice
    AlreadyMipped,   // more than one mip level
    NotRgba8,        // neither R8G8B8A8_TYPELESS nor R8G8B8A8_UNORM
    SrgbTyped,       // R8G8B8A8_UNORM_SRGB: the colour-space decision assumes display-encoded UNORM data
    CreateFailed,    // the mipped texture or one of its views could not be created
    Count
};
const char* vrWorldMipsRefusalName(VrWorldMipsRefusal why);

struct VrWorldMipsStats {
    // The GPU calls issued, and the calls answered without any.
    uint64_t copies = 0;             // CopySubresourceRegion: mip 0 of the screen into the mipped texture
    uint64_t generates = 0;          // GenerateMips, through the sRGB view
    uint64_t frameHits = 0;          // answered from this frame's earlier copy
    // The mipped texture and its two views.
    uint64_t creations = 0;          // made
    uint64_t releases = 0;           // let go: a size or format change, a device change, a reset
    uint32_t creationLines = 0;      // creation lines logged (the first 8; later creations are only counted)
    uint32_t width = 0, height = 0, levels = 0;   // the texture now; 0 when there is none
    // Refusals, by reason (index with VrWorldMipsRefusal), and the distinct ones logged (the first 8).
    uint64_t refusals[static_cast<int>(VrWorldMipsRefusal::Count)] = {};
    uint32_t refusalLines = 0;
    // The sampler table.
    uint64_t samplerCreates = 0;     // CreateSamplerState calls that succeeded
    uint64_t samplerHits = 0;        // answered from the table
    uint64_t samplerEvictions = 0;   // the oldest entry let go to make room
    uint64_t samplerFailures = 0;    // CreateSamplerState calls that failed
    uint32_t samplersLive = 0;       // entries held now (never more than 8)
};
VrWorldMipsStats vrWorldMipsStats();
// Zero the counters and forget which refusals were logged, so a test sees its own log lines again. Holds no GPU state.
void vrWorldMipsStatsClear();

}  // namespace edvr
