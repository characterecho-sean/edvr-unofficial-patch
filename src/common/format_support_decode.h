// Names for a D3D11 device's capability answers: a format-support mask as short bit
// names, a DXGI format or D3D11 feature number as its name, an HRESULT as its name,
// and the flags of the two option structs.
//
// PURE. No I/O, no globals, no Windows or D3D headers: every number below is copied
// from d3d11.h or dxgiformat.h, and tools\format_support_test holds each one against
// the SDK's own enumerator at build time, so a value mistyped here fails the build and
// not a flight. The log lines built from these names are format_query_log.h's; the
// calls that gather the numbers are src\d3d11\format_support_log.cpp's.
//
// Why any of it exists: under CrossOver the graphics runtime is DXMT, a D3D11-to-Metal
// layer, and Elite picks its render-target formats from what the device answers to
// CheckFormatSupport and CheckFeatureSupport. docs\macos-dxmt-2026-09-30.md.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {
namespace fsdetail {

// A bounded text writer. Appends never overrun, the buffer is always NUL-terminated,
// and a result that did not fit ends in '~' so a cut line cannot pass for a whole one.
struct TextOut {
    char*  buf;
    size_t cap;
    size_t len;
    bool   cut;

    TextOut(char* b, size_t c) : buf(b), cap(c), len(0), cut(false) {
        if (buf && cap) buf[0] = '\0';
    }
    void addc(char c) {
        if (!buf || cap == 0) return;
        if (len + 1 >= cap) { cut = true; return; }
        buf[len++] = c;
        buf[len] = '\0';
    }
    void adds(const char* s) { if (s) while (*s) addc(*s++); }
    void addu(uint64_t v) {
        char t[24];
        int n = 0;
        do { t[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (n) addc(t[--n]);
    }
    // `digits` upper-case hex digits, zero padded.
    void addh(uint64_t v, int digits) {
        static const char kHex[] = "0123456789ABCDEF";
        for (int i = digits - 1; i >= 0; --i) addc(kHex[(v >> (4 * i)) & 0xF]);
    }
    // Upper-case hex with no padding and at least one digit ("0", "1A2B3C").
    void addx(uint64_t v) {
        static const char kHex[] = "0123456789ABCDEF";
        char t[16];
        int n = 0;
        do { t[n++] = kHex[v & 0xF]; v >>= 4; } while (v);
        while (n) addc(t[--n]);
    }
    void add0x(uint32_t v) { adds("0x"); addh(v, 8); }
    size_t finish() {
        if (cut && len > 0) buf[len - 1] = '~';
        return len;
    }
};

// D3D11_FORMAT_SUPPORT, one name per bit (bit 31 is undefined). Short where the SDK's
// name runs long; the ones an investigation reads first keep their full words:
// RENDER_TARGET, BLENDABLE, TYPED_UAV, SHADER_LOAD, SHADER_SAMPLE, MSAA_*.
inline const char* const* supportBitNames() {
    static const char* const kNames[32] = {
        "BUFFER",       "IA_VERTEX",    "IA_INDEX",      "SO_BUFFER",              // 0-3
        "TEX1D",        "TEX2D",        "TEX3D",         "CUBE",                   // 4-7
        "SHADER_LOAD",  "SHADER_SAMPLE", "SAMPLE_CMP",   "SAMPLE_MONO",            // 8-11
        "MIP",          "MIP_AUTOGEN",  "RENDER_TARGET", "BLENDABLE",              // 12-15
        "DEPTH_STENCIL", "CPU_LOCKABLE", "MSAA_RESOLVE", "DISPLAY",                // 16-19
        "CAST_BIT_LAYOUT", "MSAA_RENDER_TARGET", "MSAA_LOAD", "SHADER_GATHER",     // 20-23
        "BACKBUF_CAST", "TYPED_UAV",    "GATHER_CMP",    "DECODER_OUT",            // 24-27
        "VIDEO_PROC_OUT", "VIDEO_PROC_IN", "VIDEO_ENCODER", nullptr,               // 28-31
    };
    return kNames;
}

// D3D11_FORMAT_SUPPORT2. Bits 11-13, 15 and 17-31 are undefined.
inline const char* const* support2BitNames() {
    static const char* const kNames[32] = {
        "UAV_ATOMIC_ADD", "UAV_ATOMIC_BITWISE", "UAV_ATOMIC_CMP_STORE_XCHG", "UAV_ATOMIC_EXCHANGE",   // 0-3
        "UAV_ATOMIC_SMINMAX", "UAV_ATOMIC_UMINMAX", "UAV_TYPED_LOAD", "UAV_TYPED_STORE",            // 4-7
        "OM_LOGIC_OP", "TILED", "SHAREABLE", nullptr,                                               // 8-11
        nullptr, nullptr, "MULTIPLANE_OVERLAY", nullptr,                                            // 12-15
        "DISPLAYABLE", nullptr, nullptr, nullptr,                                                   // 16-19
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,                     // 20-27
        nullptr, nullptr, nullptr, nullptr,                                                         // 28-31
    };
    return kNames;
}

// "NAME|NAME|..." for the set bits, lowest first; "-" for an empty mask; a set bit the
// SDK does not define appears as "bitN" (a driver answering with one is news).
inline void addMaskNames(TextOut& o, uint32_t mask, const char* const* names) {
    if (mask == 0) { o.adds("-"); return; }
    bool first = true;
    for (unsigned b = 0; b < 32; ++b) {
        if (!(mask & (1u << b))) continue;
        if (!first) o.addc('|');
        first = false;
        if (names[b]) {
            o.adds(names[b]);
        } else {
            o.adds("bit");
            o.addu(b);
        }
    }
}

}  // namespace fsdetail

// A D3D11_FORMAT_SUPPORT mask (CheckFormatSupport, FEATURE_FORMAT_SUPPORT) as short
// bit names. Always NUL-terminated; returns the length written.
inline size_t formatSupportNames(uint32_t mask, char* out, size_t cap) {
    fsdetail::TextOut o(out, cap);
    fsdetail::addMaskNames(o, mask, fsdetail::supportBitNames());
    return o.finish();
}

// A D3D11_FORMAT_SUPPORT2 mask (FEATURE_FORMAT_SUPPORT2): the UAV atomic and typed
// load/store bits among them.
inline size_t formatSupport2Names(uint32_t mask, char* out, size_t cap) {
    fsdetail::TextOut o(out, cap);
    fsdetail::addMaskNames(o, mask, fsdetail::support2BitNames());
    return o.finish();
}

// The DXGI_FORMAT enumerator without its prefix ("R11G11B10_FLOAT"), or nullptr for a
// value dxgiformat.h does not define. 0..115 are dense; a few later ones are listed.
inline const char* dxgiFormatName(uint32_t format) {
    static const char* const kDense[116] = {
        // 0..115, in enumerator order (dxgiformat.h, Windows SDK 10.0.26100.0)
        "UNKNOWN", "R32G32B32A32_TYPELESS", "R32G32B32A32_FLOAT",
        "R32G32B32A32_UINT", "R32G32B32A32_SINT", "R32G32B32_TYPELESS",
        "R32G32B32_FLOAT", "R32G32B32_UINT", "R32G32B32_SINT",
        "R16G16B16A16_TYPELESS", "R16G16B16A16_FLOAT", "R16G16B16A16_UNORM",
        "R16G16B16A16_UINT", "R16G16B16A16_SNORM", "R16G16B16A16_SINT",
        "R32G32_TYPELESS", "R32G32_FLOAT", "R32G32_UINT",
        "R32G32_SINT", "R32G8X24_TYPELESS", "D32_FLOAT_S8X24_UINT",
        "R32_FLOAT_X8X24_TYPELESS", "X32_TYPELESS_G8X24_UINT", "R10G10B10A2_TYPELESS",
        "R10G10B10A2_UNORM", "R10G10B10A2_UINT", "R11G11B10_FLOAT",
        "R8G8B8A8_TYPELESS", "R8G8B8A8_UNORM", "R8G8B8A8_UNORM_SRGB",
        "R8G8B8A8_UINT", "R8G8B8A8_SNORM", "R8G8B8A8_SINT",
        "R16G16_TYPELESS", "R16G16_FLOAT", "R16G16_UNORM",
        "R16G16_UINT", "R16G16_SNORM", "R16G16_SINT",
        "R32_TYPELESS", "D32_FLOAT", "R32_FLOAT",
        "R32_UINT", "R32_SINT", "R24G8_TYPELESS",
        "D24_UNORM_S8_UINT", "R24_UNORM_X8_TYPELESS", "X24_TYPELESS_G8_UINT",
        "R8G8_TYPELESS", "R8G8_UNORM", "R8G8_UINT",
        "R8G8_SNORM", "R8G8_SINT", "R16_TYPELESS",
        "R16_FLOAT", "D16_UNORM", "R16_UNORM",
        "R16_UINT", "R16_SNORM", "R16_SINT",
        "R8_TYPELESS", "R8_UNORM", "R8_UINT",
        "R8_SNORM", "R8_SINT", "A8_UNORM",
        "R1_UNORM", "R9G9B9E5_SHAREDEXP", "R8G8_B8G8_UNORM",
        "G8R8_G8B8_UNORM", "BC1_TYPELESS", "BC1_UNORM",
        "BC1_UNORM_SRGB", "BC2_TYPELESS", "BC2_UNORM",
        "BC2_UNORM_SRGB", "BC3_TYPELESS", "BC3_UNORM",
        "BC3_UNORM_SRGB", "BC4_TYPELESS", "BC4_UNORM",
        "BC4_SNORM", "BC5_TYPELESS", "BC5_UNORM",
        "BC5_SNORM", "B5G6R5_UNORM", "B5G5R5A1_UNORM",
        "B8G8R8A8_UNORM", "B8G8R8X8_UNORM", "R10G10B10_XR_BIAS_A2_UNORM",
        "B8G8R8A8_TYPELESS", "B8G8R8A8_UNORM_SRGB", "B8G8R8X8_TYPELESS",
        "B8G8R8X8_UNORM_SRGB", "BC6H_TYPELESS", "BC6H_UF16",
        "BC6H_SF16", "BC7_TYPELESS", "BC7_UNORM",
        "BC7_UNORM_SRGB", "AYUV", "Y410",
        "Y416", "NV12", "P010",
        "P016", "420_OPAQUE", "YUY2",
        "Y210", "Y216", "NV11",
        "AI44", "IA44", "P8",
        "A8P8", "B4G4R4A4_UNORM",
    };
    if (format < 116) return kDense[format];
    switch (format) {
        case 130: return "P208";
        case 131: return "V208";
        case 132: return "V408";
        case 189: return "SAMPLER_FEEDBACK_MIN_MIP_OPAQUE";
        case 190: return "SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE";
        case 191: return "A4B4G4R4_UNORM";
        default:  return nullptr;
    }
}

// The D3D11_FEATURE enumerator without its prefix ("D3D11_OPTIONS2"), or nullptr past
// the last one d3d11.h defines (D3D11_OPTIONS6, 21).
inline const char* d3d11FeatureName(uint32_t feature) {
    static const char* const kNames[22] = {
        "THREADING", "DOUBLES", "FORMAT_SUPPORT", "FORMAT_SUPPORT2",               // 0-3
        "D3D10_X_HARDWARE_OPTIONS", "D3D11_OPTIONS", "ARCHITECTURE_INFO",          // 4-6
        "D3D9_OPTIONS", "SHADER_MIN_PRECISION_SUPPORT", "D3D9_SHADOW_SUPPORT",     // 7-9
        "D3D11_OPTIONS1", "D3D9_SIMPLE_INSTANCING_SUPPORT", "MARKER_SUPPORT",      // 10-12
        "D3D9_OPTIONS1", "D3D11_OPTIONS2", "D3D11_OPTIONS3",                       // 13-15
        "GPU_VIRTUAL_ADDRESS_SUPPORT", "D3D11_OPTIONS4", "SHADER_CACHE",           // 16-18
        "D3D11_OPTIONS5", "DISPLAYABLE", "D3D11_OPTIONS6",                         // 19-21
    };
    return feature < 22 ? kNames[feature] : nullptr;
}

// The HRESULTs a capability query is seen to return, by name; nullptr for any other.
// A hex code in a bug report is a lookup somebody has to do before they can think.
inline const char* hresultText(int32_t hr) {
    switch (static_cast<uint32_t>(hr)) {
        case 0x00000000u: return "S_OK";
        case 0x00000001u: return "S_FALSE";
        case 0x80004001u: return "E_NOTIMPL";
        case 0x80004002u: return "E_NOINTERFACE";
        case 0x80004003u: return "E_POINTER";
        case 0x80004005u: return "E_FAIL";
        case 0x8007000Eu: return "E_OUTOFMEMORY";
        case 0x80070057u: return "E_INVALIDARG";
        case 0x887A0001u: return "DXGI_ERROR_INVALID_CALL";
        case 0x887A0002u: return "DXGI_ERROR_NOT_FOUND";
        case 0x887A0003u: return "DXGI_ERROR_MORE_DATA";
        case 0x887A0004u: return "DXGI_ERROR_UNSUPPORTED";
        case 0x887A0005u: return "DXGI_ERROR_DEVICE_REMOVED";
        default:          return nullptr;
    }
}

// D3D11_FEATURE_DATA_D3D11_OPTIONS: fourteen BOOLs, in struct order, as "Name=0/1 ...".
// A word the caller did not have prints as '?'.
inline size_t d3d11OptionsFlags(const uint32_t* words, size_t count, char* out, size_t cap) {
    static const char* const kNames[14] = {
        "OMLogicOp", "UAVOnlyForcedSampleCount", "DiscardAPIs", "FlagsForUpdateAndCopy",
        "ClearView", "CopyWithOverlap", "CBPartialUpdate", "CBOffsetting",
        "MapNoOverwriteCB", "MapNoOverwriteBufSRV", "MSAARTVForcedSampleCountOne",
        "SAD4", "ExtendedDoubles", "ExtendedResourceSharing",
    };
    fsdetail::TextOut o(out, cap);
    for (size_t i = 0; i < 14; ++i) {
        if (i) o.addc(' ');
        o.adds(kNames[i]);
        o.addc('=');
        if (words && i < count) o.addu(words[i]); else o.addc('?');
    }
    return o.finish();
}

// D3D11_FEATURE_DATA_D3D11_OPTIONS2: eight words, in struct order. TypedUAVLoadAdditionalFormats
// extends typed UAV loads beyond R32_FLOAT, R32_UINT and R32_SINT; which formats it
// reaches is FORMAT_SUPPORT2's UAV_TYPED_LOAD bit, asked per format.
inline size_t d3d11Options2Flags(const uint32_t* words, size_t count, char* out, size_t cap) {
    static const char* const kNames[8] = {
        "PSStencilRef", "TypedUAVLoadAdditionalFormats", "ROVs", "ConservativeRasterTier",
        "TiledResourcesTier", "MapOnDefaultTextures", "StandardSwizzle", "UMA",
    };
    fsdetail::TextOut o(out, cap);
    for (size_t i = 0; i < 8; ++i) {
        if (i) o.addc(' ');
        o.adds(kNames[i]);
        o.addc('=');
        if (words && i < count) o.addu(words[i]); else o.addc('?');
    }
    return o.finish();
}

}  // namespace edvr
