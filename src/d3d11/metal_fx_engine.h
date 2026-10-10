// Apple MetalFX at the door -- the MetalFX temporal scaler, invoked natively
// by DXMT (CodeWeavers' D3D11-over-Metal) through its own private extension
// interface, so fix.temporal_aa = mfx hands it the same canonical inputs
// dlaaEvaluate and fsr3Evaluate are handed and MetalFX writes into the same
// output texture. Nothing here is a Metal call: no MetalFX header, no MetalFX
// symbol, no nvngx, no NVAPI, no DXMT_ENABLE_NVEXT.
//
// The division of labour is the point, and it is the reason this file is
// small. EDVR owns the Elite-specific temporal semantics -- which texture is
// the scene, which is depth, which is motion, what the jitter and reset are,
// which sizes, and where the result is presented -- all of which the working
// frontend already produces and none of which MetalFX has any opinion about.
// DXMT owns the native MetalFX invocation, because DXMT is the only layer that
// can: it holds the MTLTexture objects behind these same D3D11 resources, it
// owns the command buffer the encode has to be ordered into, and it owns the
// hazard and residency tracking that keeps the encode correctly ordered
// against the D3D11 work that produced the inputs. Its TemporalUpscale does
// the resource translation with no copy: the MTLTexture the scaler reads is
// the allocation EDVR's UAVs already wrote.
//
// The interface is DXMT's, not a public D3D11 one, so this file declares the
// ABI it needs rather than including anything of DXMT's (EDVR never includes
// DXMT headers; DXMT is read-only reference). The authoritative reference is
// dxmt/src/d3d11/d3d11_interfaces.hpp at the commit this was written against
// (db83f85 for Ext/Ext1), and the static_asserts below pin
// the layout those declarations produce. They catch a layout EDVR gets wrong,
// not one DXMT changes later: tools\check_metal_fx_backend.py re-derives the
// field offsets and the two IIDs from this file and, when a DXMT checkout is
// beside the tree, from DXMT's own header, so a divergence between the two
// fails the build.
//
// What is NOT here, deliberately:
//   - The macOS 27 temporal-scaler options this backend could want (reactive
//     mask, jittered motion vectors, output-resolution motion vectors) as
//     anything but a note. They are all on the MetalFX 3 descriptor, which
//     remains the single descriptor below.
//   - A reactive mask. MetalFX accepts one and EDVR's rejection mask is the
//     right shape (R8_UNORM), but DXMT's descriptor has no field for it and
//     DXMT is read-only for this milestone. EDVR's existing finish kernel
//     already consumes the rejection mask for every trained backend, so
//     passing nothing here loses nothing today.
//   - A history texture. MetalFX owns its temporal history inside the scaler
//     instance DXMT caches, exactly as it does behind nvngx. EDVR's own
//     reset() result is handed over and nothing else is.
//
// UNRESOLVED, and deliberately isolated: the sign of jitterOffsetX/Y. See
// mfxEvaluate. One constant, one line, and the comment there says what to
// flip. Not validated at runtime -- do not read this file's presence of the
// field as evidence the picture is right.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <unknwn.h>
#include <windef.h>

struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11Texture2D;

namespace edvr {
namespace mfx {

// ---- DXMT's private ABI, declared here rather than included -------------------------------------------
// IMTLD3D11ContextExt  {43ace3ce-1956-448b-a4eb-aee68bdeb283}
// IMTLD3D11ContextExt1 {19a8e35a-38be-418f-94e3-9f7323936870}
// (dxmt/src/d3d11/d3d11_interfaces.hpp:24 and :36). Spelled as fields, not
// __declspec(uuid(...)), so the constants are the same value in every
// compiler EDVR is built with and can be static_asserted below. The reference
// build encodes them from the same strings with dxmt::guid::make_guid; the
// GUID value is what QueryInterface compares, and these are those values.
constexpr GUID kIidContextExt = {0x43ace3ceu, 0x1956u, 0x448bu,
                                 {0xa4, 0xeb, 0xae, 0xe6, 0x8b, 0xde, 0xb2, 0x83}};
constexpr GUID kIidContextExt1 = {0x19a8e35au, 0x38beu, 0x418fu,
                                  {0x94, 0xe3, 0x9f, 0x73, 0x23, 0x93, 0x68, 0x70}};
// A GUID built from the two strings above, re-derived at compile time, so a
// typo in either constant is a build failure and not a QueryInterface that
// quietly returns E_NOINTERFACE at runtime.
static_assert(kIidContextExt.Data1 == 0x43ace3ceu && kIidContextExt.Data2 == 0x1956u &&
                  kIidContextExt.Data3 == 0x448bu && kIidContextExt.Data4[7] == 0x83u &&
                  kIidContextExt.Data4[0] == 0xa4u && kIidContextExt.Data4[1] == 0xebu,
              "kIidContextExt must be 43ace3ce-1956-448b-a4eb-aee68bdeb283");
static_assert(kIidContextExt1.Data1 == 0x19a8e35au && kIidContextExt1.Data2 == 0x38beu &&
                  kIidContextExt1.Data3 == 0x418fu && kIidContextExt1.Data4[7] == 0x70u &&
                  kIidContextExt1.Data4[0] == 0x94u && kIidContextExt1.Data4[1] == 0xe3u,
              "kIidContextExt1 must be 19a8e35a-38be-418f-94e3-9f7323936870");

// dxmt/src/d3d11/d3d11_interfaces.hpp:5. Field order is DXMT's, verbatim: a
// vtable of this struct crossing into DXMT is a memcpy of the whole thing
// field for field, so the order below is not EDVR's to choose.
struct TemporalUpscaleDesc {
    UINT InputContentWidth;          //  0; 0 means full width
    UINT InputContentHeight;         //  4; 0 means full height
    BOOL AutoExposure;               //  8
    BOOL InReset;                    // 12
    BOOL DepthReversed;              // 16
    BOOL MotionVectorInDisplayRes;   // 20
    ID3D11Resource *Color;           // 24 (the first 8-byte-aligned field)
    ID3D11Resource *Depth;           // 32
    ID3D11Resource *MotionVector;    // 40
    ID3D11Resource *Output;          // 48
    FLOAT MotionVectorScaleX;        // 56
    FLOAT MotionVectorScaleY;        // 60
    FLOAT PreExposure;               // 64
    ID3D11Resource *ExposureTexture; // 72 (four bytes of padding after PreExposure)
    FLOAT JitterOffsetX;             // 80
    FLOAT JitterOffsetY;             // 84
};

// 88 bytes on x64, with the padding DXMT's field order forces before
// ExposureTexture. A change here is an ABI change, and must not be a silent
// one: MetalFX would read whatever EDVR left in those four bytes.
static_assert(sizeof(void *) == 8, "the offsets below are x64's");
static_assert(sizeof(TemporalUpscaleDesc) == 88, "MTL_TEMPORAL_UPSCALE_D3D11_DESC must be 88 bytes on x64");
static_assert(offsetof(TemporalUpscaleDesc, InputContentWidth) == 0, "");
static_assert(offsetof(TemporalUpscaleDesc, InputContentHeight) == 4, "");
static_assert(offsetof(TemporalUpscaleDesc, AutoExposure) == 8, "");
static_assert(offsetof(TemporalUpscaleDesc, InReset) == 12, "");
static_assert(offsetof(TemporalUpscaleDesc, DepthReversed) == 16, "");
static_assert(offsetof(TemporalUpscaleDesc, MotionVectorInDisplayRes) == 20, "");
static_assert(offsetof(TemporalUpscaleDesc, Color) == 24, "");
static_assert(offsetof(TemporalUpscaleDesc, Depth) == 32, "");
static_assert(offsetof(TemporalUpscaleDesc, MotionVector) == 40, "");
static_assert(offsetof(TemporalUpscaleDesc, Output) == 48, "");
static_assert(offsetof(TemporalUpscaleDesc, MotionVectorScaleX) == 56, "");
static_assert(offsetof(TemporalUpscaleDesc, MotionVectorScaleY) == 60, "");
static_assert(offsetof(TemporalUpscaleDesc, PreExposure) == 64, "");
static_assert(offsetof(TemporalUpscaleDesc, ExposureTexture) == 72, "");
static_assert(offsetof(TemporalUpscaleDesc, JitterOffsetX) == 80, "");
static_assert(offsetof(TemporalUpscaleDesc, JitterOffsetY) == 84, "");
static_assert(sizeof(UINT) == 4 && sizeof(BOOL) == 4 && sizeof(FLOAT) == 4, "");
// UINT must be unsigned, not a typedef that MSVC could size differently: the
// desc is read by DXMT as the fields it declared, not as these.
static_assert((std::is_unsigned<UINT>::value) && sizeof(UINT) == 4, "UINT must be a 4-byte unsigned");
// And the descriptor must not be given any implicit padding of its own beyond
// what the declared order already forces: a memcmp against DXMT's struct is
// then only meaningful field for field, which is how the guard reads it.
static_assert(sizeof(TemporalUpscaleDesc) % alignof(TemporalUpscaleDesc) == 0, "no tail padding");

// dxmt/src/d3d11/d3d11_interfaces.hpp:32. The MetalFX 3 answer: whether this
// GPU supports temporal scaling (id<MTLFXTemporalScaler>). DXMT answers FALSE
// rather than an error when it does not, so a FALSE here is a normal outcome
// and not a fault.
enum class Feature : int { MetalFxTemporalScaler = 0 };

// dxmt/src/d3d11/d3d11_interfaces.hpp:24. The base is IUnknown with DXMT's
// three methods in DXMT's order, so TemporalUpscale is vtable slot 3 and
// CheckFeatureSupport slot 6 of IMTLD3D11ContextExt1.
//
// The slot numbers are the whole ABI risk of this chain, and they cannot be
// asserted here: sizeof() of an interface with no data members is one vtable
// pointer under MSVC and under clang, whatever the vtable holds, so a size
// assertion says nothing about which slot a method occupies and would pass on a
// declaration order that is completely wrong. What pins the order instead is
// that these two declarations are DXMT's, in DXMT's order, verbatim --
// checked against DXMT's own header by tools\check_metal_fx_backend.py, which
// fails the build if the two ever diverge.
struct ContextExt : public IUnknown {
    virtual void STDMETHODCALLTYPE TemporalUpscale(const TemporalUpscaleDesc *pDesc) = 0;
    virtual void STDMETHODCALLTYPE BeginUAVOverlap() = 0;
    virtual void STDMETHODCALLTYPE EndUAVOverlap() = 0;
};
struct ContextExt1 : public ContextExt {
    virtual HRESULT STDMETHODCALLTYPE CheckFeatureSupport(Feature feature, void *pFeatureSupportData,
                                                          UINT featureSupportDataSize) = 0;
};

} // namespace mfx

// Is MetalFX temporal scaling usable from this context? Asks the live DXMT
// extension interface -- not a guess from a driver or a module name -- and
// answers false with a stable reason string for the log when it is not:
//   "flat-mfx-no-dxmt-context-ext"        the context is not DXMT's, or the QI failed
//   "flat-mfx-no-dxmt-context-ext1"       the older interface, so no feature query
//   "flat-mfx-check-feature-support-failed" DXMT's own query refused
//   "flat-mfx-unsupported"                DXMT answered: MetalFX temporal scaling is unavailable
//   "flat-mfx-no-context"                 no context was handed
// Does not need DXMT_ENABLE_NVEXT, nvngx.dll or any NVIDIA registry key: this
// is DXMT's own interface, and the NVEXT gate that nvngx.dll needs governs
// only the vendor-extension entry point, not this one. Cheap after the first
// call; the extension pointer is cached per context.
bool mfxAvailable(ID3D11DeviceContext* ctx, const char** why);

// One frame. colour is fp16 RGBA at w x h (the canonical MetalFX colour
// format), depth r32f and mv rg16f at the same size, out fp16 RGBA at
// outW x outH -- all four are ordinary D3D11 textures with SRV and, where the
// frontend writes them, UAV, and DXMT hands MetalFX the MTLTexture objects
// that already back them, with no copy in either direction. jx/jy are
// EDVR's current-frame raster phase in render pixels; reset is the frontend's
// effective temporal reset (the same value FSR3 and NGX are given), not an
// MFX-specific one. autoExposure is the HDR route's flag. False on any
// refusal, with why.
//
// TemporalUpscale returns void: a dispatched call proves EDVR asked, not that
// DXMT created the scaler or that the GPU ran it (its implementation returns
// silently on bad inputs). The backend refuses everything it can see -- null
// resources, zero extents, missing interface, refused feature -- but absence
// of failure is not success; the flight judges the picture.
//
// THE JITTER SIGN, ANSWERED BY FLIGHT. EDVR's phase is positive right/down in
// [-0.5, 0.5) (src/common/temporal_math.h:90); MetalFX's jitterOffset is
// documented only as "the pixel offset this scaler samples to return to the
// frame's reference frame", so the sign could not be read out of either header.
// The first M5 Pro flight ran with kJitterSign = -1 -- the negation, chosen on
// the reasoning that DXMT passes DLSS's jitterOffset to this same scaler and so
// DLSS's convention was the intended one.
//
// That was an inference, and the flight refuted it. DXMT answered ContextExt1,
// MetalFX ran and treated frames continuously, and the presented image visibly
// RETAINED the projection jitter -- the scaler was handed an offset that undid
// nothing, or the wrong amount of it, so the jitter stayed in the picture
// instead of averaging away. MetalFX wants the phase EDVR has already computed,
// handed over as it stands.
//
// This is the whole A/B. JitterOffsetX/Y below are kJitterSign times the phase,
// so flipping this one constant flips both axes and moves nothing else in the
// backend: no input, no size, no format, no motion vector, no reset.
constexpr float kJitterSign = 1.0f;
inline float mfxJitterOffsetX(float jx) { return kJitterSign * jx; }
inline float mfxJitterOffsetY(float jy) { return kJitterSign * jy; }

// The call below is the ORIGINAL TemporalUpscale on the ORIGINAL Ext1
// interface: one descriptor in, no generation select, no fallback.
bool mfxEvaluate(ID3D11DeviceContext* ctx, ID3D11Texture2D* colour, ID3D11Texture2D* depth,
                 ID3D11Texture2D* mv, ID3D11Texture2D* out, uint32_t w, uint32_t h, uint32_t outW,
                 uint32_t outH, float jx, float jy, bool reset, const char** why, bool autoExposure);

} // namespace edvr