// Rig for the device capability log (src\d3d11\format_support_log.h and the pure
// headers behind it, src\common\format_support_decode.h and format_query_log.h).
//
// THREE PARTS: the pure half, the device half and an idle half.
//
//   Pure, in this process: the decode names, the lock-free ledger that picks which of
//   the game's queries get a line (run from eight threads at once), and the exact text
//   of every line. The SDK's own enumerators are the ground truth -- a mask bit, a DXGI
//   format or a D3D11 feature number mistyped in the decode header fails here, at build
//   time, and not in a flight read back against a table. The vtable slots the device
//   hook patches are held against the SDK's ID3D11DeviceVtbl in device_slots.cpp,
//   compiled into this exe.
//
//   Device, in a child process (--device-child): a real WARP device, the very
//   formatSupportCheckFormat / formatSupportCheckFeature the hooks call installed on
//   its CheckFormatSupport and CheckFeatureSupport slots through a real VTableHook, the
//   real Log writing to a scratch directory. It proves the four things the instrument
//   lives or dies by: every answer through the hook is bit-identical to the answer
//   without it (HRESULT and every byte the call may write, across every DXGI format and
//   every feature at several sizes, from several threads); a report that faults costs
//   the line and never the answer; the lines are the ones the doc promises, in the order
//   the game asked, capped, deduplicated and counted; and nothing is lost or spent
//   before the log is open. The child is a separate process because it patches a shared
//   vtable, and so a crash in it is this rig's failure and not its parent's.
//
//   Idle, in another child (--idle-child): no hook is ever entered. The closing count
//   must stay silent for 59 ticks and on the 60th say once, with zeros, that the hooks
//   ran 0 times -- so that a session with no game lines can be told from a game that
//   asked nothing.
//
//   --dry-run        does nothing, for the build gate's first step
//   --self-test      runs all three; exit 1 on any failure
//   --device-child   the device half alone (the parent starts it)
//   --idle-child     the idle half alone (the parent starts it)
//   --examples       prints the lines the doc quotes (docs\macos-dxmt-2026-09-30.md)
//   FST_SHOW_LOG=1   with --self-test or --device-child: also prints the instrument's
//                    own lines as the WARP device produced them
#include "../../src/common/d3d11_device_slots.h"
#include "../../src/common/format_query_log.h"
#include "../../src/common/format_support_decode.h"
#include "../../src/common/log.h"
#include "../../src/common/system_d3d11.h"
#include "../../src/common/vtable_hook.h"
#include "../../src/d3d11/format_support_log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace edvr;
using Microsoft::WRL::ComPtr;

namespace {

unsigned checks = 0, failures = 0;

#define CHECK(value)                                                        \
    do {                                                                    \
        ++checks;                                                           \
        if (!(value)) {                                                     \
            ++failures;                                                     \
            std::printf("FAIL: line %d: %s\n", __LINE__, #value);           \
        }                                                                   \
    } while (false)

// A string compare that says what it got, because a line format that differs by one
// character is otherwise a hunt.
#define CHECK_STR(got, want)                                                \
    do {                                                                    \
        ++checks;                                                           \
        const std::string g_ = (got), w_ = (want);                          \
        if (g_ != w_) {                                                     \
            ++failures;                                                     \
            std::printf("FAIL: line %d: %s\n  got:  %s\n  want: %s\n",      \
                        __LINE__, #got, g_.c_str(), w_.c_str());            \
        }                                                                   \
    } while (false)

std::string supportNames(uint32_t mask) {
    char buf[1024];
    formatSupportNames(mask, buf, sizeof(buf));
    return buf;
}
std::string support2Names(uint32_t mask) {
    char buf[1024];
    formatSupport2Names(mask, buf, sizeof(buf));
    return buf;
}

// The SDK's bit, and the name the decode header must give it.
struct BitName { uint32_t bit; const char* name; };

const BitName kSupportBits[] = {
    {D3D11_FORMAT_SUPPORT_BUFFER, "BUFFER"},
    {D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER, "IA_VERTEX"},
    {D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER, "IA_INDEX"},
    {D3D11_FORMAT_SUPPORT_SO_BUFFER, "SO_BUFFER"},
    {D3D11_FORMAT_SUPPORT_TEXTURE1D, "TEX1D"},
    {D3D11_FORMAT_SUPPORT_TEXTURE2D, "TEX2D"},
    {D3D11_FORMAT_SUPPORT_TEXTURE3D, "TEX3D"},
    {D3D11_FORMAT_SUPPORT_TEXTURECUBE, "CUBE"},
    {D3D11_FORMAT_SUPPORT_SHADER_LOAD, "SHADER_LOAD"},
    {D3D11_FORMAT_SUPPORT_SHADER_SAMPLE, "SHADER_SAMPLE"},
    {D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON, "SAMPLE_CMP"},
    {D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_MONO_TEXT, "SAMPLE_MONO"},
    {D3D11_FORMAT_SUPPORT_MIP, "MIP"},
    {D3D11_FORMAT_SUPPORT_MIP_AUTOGEN, "MIP_AUTOGEN"},
    {D3D11_FORMAT_SUPPORT_RENDER_TARGET, "RENDER_TARGET"},
    {D3D11_FORMAT_SUPPORT_BLENDABLE, "BLENDABLE"},
    {D3D11_FORMAT_SUPPORT_DEPTH_STENCIL, "DEPTH_STENCIL"},
    {D3D11_FORMAT_SUPPORT_CPU_LOCKABLE, "CPU_LOCKABLE"},
    {D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE, "MSAA_RESOLVE"},
    {D3D11_FORMAT_SUPPORT_DISPLAY, "DISPLAY"},
    {D3D11_FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT, "CAST_BIT_LAYOUT"},
    {D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET, "MSAA_RENDER_TARGET"},
    {D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD, "MSAA_LOAD"},
    {D3D11_FORMAT_SUPPORT_SHADER_GATHER, "SHADER_GATHER"},
    {D3D11_FORMAT_SUPPORT_BACK_BUFFER_CAST, "BACKBUF_CAST"},
    {D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW, "TYPED_UAV"},
    {D3D11_FORMAT_SUPPORT_SHADER_GATHER_COMPARISON, "GATHER_CMP"},
    {D3D11_FORMAT_SUPPORT_DECODER_OUTPUT, "DECODER_OUT"},
    {D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT, "VIDEO_PROC_OUT"},
    {D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT, "VIDEO_PROC_IN"},
    {D3D11_FORMAT_SUPPORT_VIDEO_ENCODER, "VIDEO_ENCODER"},
};

const BitName kSupport2Bits[] = {
    {D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD, "UAV_ATOMIC_ADD"},
    {D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS, "UAV_ATOMIC_BITWISE"},
    {D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE, "UAV_ATOMIC_CMP_STORE_XCHG"},
    {D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE, "UAV_ATOMIC_EXCHANGE"},
    {D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX, "UAV_ATOMIC_SMINMAX"},
    {D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX, "UAV_ATOMIC_UMINMAX"},
    {D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD, "UAV_TYPED_LOAD"},
    {D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE, "UAV_TYPED_STORE"},
    {D3D11_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP, "OM_LOGIC_OP"},
    {D3D11_FORMAT_SUPPORT2_TILED, "TILED"},
    {D3D11_FORMAT_SUPPORT2_SHAREABLE, "SHAREABLE"},
    {D3D11_FORMAT_SUPPORT2_MULTIPLANE_OVERLAY, "MULTIPLANE_OVERLAY"},
    {D3D11_FORMAT_SUPPORT2_DISPLAYABLE, "DISPLAYABLE"},
};

// Every DXGI format dxgiformat.h has had since the first SDK that shipped D3D11,
// as the SDK spells it. 0..115 are dense and must be in enumerator order.
struct FormatName { uint32_t value; const char* name; };
#define FMT(n) {static_cast<uint32_t>(DXGI_FORMAT_##n), #n}
const FormatName kFormats[] = {
    FMT(UNKNOWN), FMT(R32G32B32A32_TYPELESS), FMT(R32G32B32A32_FLOAT), FMT(R32G32B32A32_UINT),
    FMT(R32G32B32A32_SINT), FMT(R32G32B32_TYPELESS), FMT(R32G32B32_FLOAT), FMT(R32G32B32_UINT),
    FMT(R32G32B32_SINT), FMT(R16G16B16A16_TYPELESS), FMT(R16G16B16A16_FLOAT),
    FMT(R16G16B16A16_UNORM), FMT(R16G16B16A16_UINT), FMT(R16G16B16A16_SNORM),
    FMT(R16G16B16A16_SINT), FMT(R32G32_TYPELESS), FMT(R32G32_FLOAT), FMT(R32G32_UINT),
    FMT(R32G32_SINT), FMT(R32G8X24_TYPELESS), FMT(D32_FLOAT_S8X24_UINT),
    FMT(R32_FLOAT_X8X24_TYPELESS), FMT(X32_TYPELESS_G8X24_UINT), FMT(R10G10B10A2_TYPELESS),
    FMT(R10G10B10A2_UNORM), FMT(R10G10B10A2_UINT), FMT(R11G11B10_FLOAT),
    FMT(R8G8B8A8_TYPELESS), FMT(R8G8B8A8_UNORM), FMT(R8G8B8A8_UNORM_SRGB), FMT(R8G8B8A8_UINT),
    FMT(R8G8B8A8_SNORM), FMT(R8G8B8A8_SINT), FMT(R16G16_TYPELESS), FMT(R16G16_FLOAT),
    FMT(R16G16_UNORM), FMT(R16G16_UINT), FMT(R16G16_SNORM), FMT(R16G16_SINT),
    FMT(R32_TYPELESS), FMT(D32_FLOAT), FMT(R32_FLOAT), FMT(R32_UINT), FMT(R32_SINT),
    FMT(R24G8_TYPELESS), FMT(D24_UNORM_S8_UINT), FMT(R24_UNORM_X8_TYPELESS),
    FMT(X24_TYPELESS_G8_UINT), FMT(R8G8_TYPELESS), FMT(R8G8_UNORM), FMT(R8G8_UINT),
    FMT(R8G8_SNORM), FMT(R8G8_SINT), FMT(R16_TYPELESS), FMT(R16_FLOAT), FMT(D16_UNORM),
    FMT(R16_UNORM), FMT(R16_UINT), FMT(R16_SNORM), FMT(R16_SINT), FMT(R8_TYPELESS),
    FMT(R8_UNORM), FMT(R8_UINT), FMT(R8_SNORM), FMT(R8_SINT), FMT(A8_UNORM), FMT(R1_UNORM),
    FMT(R9G9B9E5_SHAREDEXP), FMT(R8G8_B8G8_UNORM), FMT(G8R8_G8B8_UNORM), FMT(BC1_TYPELESS),
    FMT(BC1_UNORM), FMT(BC1_UNORM_SRGB), FMT(BC2_TYPELESS), FMT(BC2_UNORM),
    FMT(BC2_UNORM_SRGB), FMT(BC3_TYPELESS), FMT(BC3_UNORM), FMT(BC3_UNORM_SRGB),
    FMT(BC4_TYPELESS), FMT(BC4_UNORM), FMT(BC4_SNORM), FMT(BC5_TYPELESS), FMT(BC5_UNORM),
    FMT(BC5_SNORM), FMT(B5G6R5_UNORM), FMT(B5G5R5A1_UNORM), FMT(B8G8R8A8_UNORM),
    FMT(B8G8R8X8_UNORM), FMT(R10G10B10_XR_BIAS_A2_UNORM), FMT(B8G8R8A8_TYPELESS),
    FMT(B8G8R8A8_UNORM_SRGB), FMT(B8G8R8X8_TYPELESS), FMT(B8G8R8X8_UNORM_SRGB),
    FMT(BC6H_TYPELESS), FMT(BC6H_UF16), FMT(BC6H_SF16), FMT(BC7_TYPELESS), FMT(BC7_UNORM),
    FMT(BC7_UNORM_SRGB), FMT(AYUV), FMT(Y410), FMT(Y416), FMT(NV12), FMT(P010), FMT(P016),
    FMT(420_OPAQUE), FMT(YUY2), FMT(Y210), FMT(Y216), FMT(NV11), FMT(AI44), FMT(IA44), FMT(P8),
    FMT(A8P8), FMT(B4G4R4A4_UNORM), FMT(P208), FMT(V208), FMT(V408),
};

// The D3D11_FEATURE enumerators, in the SDK's own spelling.
#define FEAT(n) {static_cast<uint32_t>(D3D11_FEATURE_##n), #n}
const FormatName kFeatures[] = {
    FEAT(THREADING), FEAT(DOUBLES), FEAT(FORMAT_SUPPORT), FEAT(FORMAT_SUPPORT2),
    FEAT(D3D10_X_HARDWARE_OPTIONS), FEAT(D3D11_OPTIONS), FEAT(ARCHITECTURE_INFO),
    FEAT(D3D9_OPTIONS), FEAT(SHADER_MIN_PRECISION_SUPPORT), FEAT(D3D9_SHADOW_SUPPORT),
    FEAT(D3D11_OPTIONS1), FEAT(D3D9_SIMPLE_INSTANCING_SUPPORT), FEAT(MARKER_SUPPORT),
    FEAT(D3D9_OPTIONS1), FEAT(D3D11_OPTIONS2), FEAT(D3D11_OPTIONS3),
    FEAT(GPU_VIRTUAL_ADDRESS_SUPPORT), FEAT(D3D11_OPTIONS4), FEAT(SHADER_CACHE),
    FEAT(D3D11_OPTIONS5), FEAT(DISPLAYABLE), FEAT(D3D11_OPTIONS6),
};

// ---- The fixtures the doc's example lines come from ---------------------------------

// Not any real adapter: a fixture with round, checkable numbers.
FqAdapter fixtureAdapter() {
    FqAdapter a;
    a.device = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x1F2A3B4C5D0ull));
    a.index = 0;
    a.description = "Test Adapter";
    a.vendorId = 0x10DE;
    a.deviceId = 0x2684;
    a.subSysId = 0x167B1458;
    a.revision = 161;
    a.dedicatedVideo = 24ull << 30;
    a.dedicatedSystem = 0;
    a.sharedSystem = 32ull << 30;
    a.luidHigh = 0;
    a.luidLow = 0xABCD;
    return a;
}

constexpr uint32_t kMaskFull =                                   // what a fully capable format answers
    D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD |
    D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_RENDER_TARGET |
    D3D11_FORMAT_SUPPORT_BLENDABLE | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
constexpr uint32_t kMaskNoUav =                                  // the same, without the typed UAV bit
    kMaskFull & ~static_cast<uint32_t>(D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW);
constexpr uint32_t kUavLoadStore =
    D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;

std::string adapterLine(const FqAdapter& a) {
    char buf[1024];
    formatAdapterLine(a, buf, sizeof(buf));
    return buf;
}
std::string selfLine(const FqSelfFormat& s) {
    char buf[1024];
    formatSelfQueryLine(s, buf, sizeof(buf));
    return buf;
}
std::string gameLine(const FqGameQuery& q) {
    char buf[1024];
    formatGameQueryLine(q, buf, sizeof(buf));
    return buf;
}

FqGameQuery gameFormat(uint32_t n, uint32_t format, int32_t hr, uint32_t mask, const char* from) {
    FqGameQuery q;
    q.kind = kFqCheckFormatSupport;
    q.callNumber = n;
    q.asked = format;
    q.hr = hr;
    q.answer = mask;
    q.thread = 14248;
    q.from = from;
    return q;
}

FqSelfOptions fixtureOptions() {
    FqSelfOptions o;
    o.hr1 = 0;
    o.words1[4] = 1;   // ClearView
    o.words1[5] = 1;   // CopyWithOverlap
    o.hr2 = 0;
    o.words2[1] = 1;   // TypedUAVLoadAdditionalFormats
    o.words2[7] = 1;   // UMA
    return o;
}

// ---- THE PURE CHECKS -------------------------------------------------------------

void checkMaskNames() {
    // Every SDK bit, alone, names itself; together the bits are the whole defined set.
    uint32_t all = 0;
    for (const BitName& b : kSupportBits) {
        CHECK(b.bit && !(b.bit & (b.bit - 1)));                 // a single bit
        CHECK(!(all & b.bit));                                   // none twice
        all |= b.bit;
        CHECK_STR(supportNames(b.bit), b.name);
    }
    CHECK(all == 0x7FFFFFFFu);                                    // bits 0-30, the SDK's whole set
    all = 0;
    for (const BitName& b : kSupport2Bits) {
        CHECK(b.bit && !(b.bit & (b.bit - 1)));
        CHECK(!(all & b.bit));
        all |= b.bit;
        CHECK_STR(support2Names(b.bit), b.name);
    }

    // The bits an investigation reads first, in combination, lowest bit first.
    CHECK_STR(supportNames(0), "-");
    CHECK_STR(support2Names(0), "-");
    CHECK_STR(supportNames(D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE),
              "RENDER_TARGET|BLENDABLE");
    CHECK_STR(supportNames(D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_SHADER_LOAD),
              "SHADER_LOAD|SHADER_SAMPLE");
    CHECK_STR(supportNames(D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW),
              "TEX2D|TYPED_UAV");
    CHECK_STR(supportNames(D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD |
                           D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET |
                           D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE),
              "MSAA_RESOLVE|MSAA_RENDER_TARGET|MSAA_LOAD");
    CHECK_STR(supportNames(kMaskFull),
              "BUFFER|TEX2D|SHADER_LOAD|SHADER_SAMPLE|RENDER_TARGET|BLENDABLE|TYPED_UAV");
    // The SUPPORT2 typed UAV load/store bits, which FORMAT_SUPPORT's single TYPED_UAV bit
    // does not separate.
    CHECK_STR(support2Names(D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD), "UAV_TYPED_LOAD");
    CHECK_STR(support2Names(D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE), "UAV_TYPED_STORE");
    CHECK_STR(support2Names(kUavLoadStore), "UAV_TYPED_LOAD|UAV_TYPED_STORE");
    CHECK(kUavLoadStore == 0xC0u);
    CHECK_STR(support2Names(D3D11_FORMAT_SUPPORT2_UAV_ATOMIC_ADD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD |
                            D3D11_FORMAT_SUPPORT2_SHAREABLE),
              "UAV_ATOMIC_ADD|UAV_TYPED_LOAD|SHAREABLE");

    // A bit the SDK does not define is named by number, not dropped.
    CHECK_STR(supportNames(0x80000000u), "bit31");
    CHECK_STR(supportNames(D3D11_FORMAT_SUPPORT_BUFFER | 0x80000000u), "BUFFER|bit31");
    CHECK_STR(support2Names(0x800u), "bit11");
    CHECK_STR(support2Names(kUavLoadStore | 0x80000000u), "UAV_TYPED_LOAD|UAV_TYPED_STORE|bit31");
    CHECK(supportNames(0xFFFFFFFFu).rfind("BUFFER|IA_VERTEX|", 0) == 0);
    CHECK(supportNames(0xFFFFFFFFu).size() > 300 && supportNames(0xFFFFFFFFu).size() < 1000);

    // A buffer that is too small is never overrun, always terminated, and says it was cut.
    struct Canary { char buf[12]; char after[8]; } c;
    std::memset(&c, 'Z', sizeof(c));
    const size_t n = formatSupportNames(0xFFFFFFFFu, c.buf, sizeof(c.buf));
    CHECK(n == sizeof(c.buf) - 1 && c.buf[sizeof(c.buf) - 1] == '\0' && c.buf[n - 1] == '~');
    CHECK(std::memcmp(c.after, "ZZZZZZZZ", 8) == 0);
    CHECK(std::strncmp(c.buf, "BUFFER|IA_", 10) == 0);
    char one[1] = {'Z'};
    CHECK(formatSupportNames(1, one, 1) == 0 && one[0] == '\0');
    CHECK(formatSupportNames(1, nullptr, 0) == 0);   // no buffer, no crash
}

void checkNumberNames() {
    CHECK(sizeof(kFormats) / sizeof(kFormats[0]) == 116 + 3);
    for (size_t i = 0; i < 116; ++i) CHECK(kFormats[i].value == i);   // dense, in order
    for (const FormatName& f : kFormats) {
        const char* got = dxgiFormatName(f.value);
        CHECK(got && !std::strcmp(got, f.name));
    }
    // Added after the first SDK: pinned by number, so an older SDK still compiles the rig.
    CHECK(!std::strcmp(dxgiFormatName(189), "SAMPLER_FEEDBACK_MIN_MIP_OPAQUE"));
    CHECK(!std::strcmp(dxgiFormatName(190), "SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE"));
    CHECK(!std::strcmp(dxgiFormatName(191), "A4B4G4R4_UNORM"));
    // The formats the world target is chosen from, by the numbers a log shows.
    CHECK(!std::strcmp(dxgiFormatName(26), "R11G11B10_FLOAT"));
    CHECK(!std::strcmp(dxgiFormatName(23), "R10G10B10A2_TYPELESS"));
    CHECK(!std::strcmp(dxgiFormatName(24), "R10G10B10A2_UNORM"));
    CHECK(!std::strcmp(dxgiFormatName(10), "R16G16B16A16_FLOAT"));
    // No name for what dxgiformat.h does not define.
    CHECK(dxgiFormatName(116) == nullptr && dxgiFormatName(129) == nullptr &&
          dxgiFormatName(133) == nullptr && dxgiFormatName(0xFFFFFFFFu) == nullptr);

    CHECK(sizeof(kFeatures) / sizeof(kFeatures[0]) == 22);
    for (size_t i = 0; i < 22; ++i) {
        CHECK(kFeatures[i].value == i);
        const char* got = d3d11FeatureName(kFeatures[i].value);
        CHECK(got && !std::strcmp(got, kFeatures[i].name));
    }
    CHECK(d3d11FeatureName(22) == nullptr && d3d11FeatureName(0xFFFFFFFFu) == nullptr);
    // The feature ids the ledger keys on.
    CHECK(kFqFeatureFormatSupportId == static_cast<uint32_t>(D3D11_FEATURE_FORMAT_SUPPORT));
    CHECK(kFqFeatureFormatSupport2Id == static_cast<uint32_t>(D3D11_FEATURE_FORMAT_SUPPORT2));
    CHECK(kFqFeatureOptionsId == static_cast<uint32_t>(D3D11_FEATURE_D3D11_OPTIONS));
    CHECK(kFqFeatureOptions2Id == static_cast<uint32_t>(D3D11_FEATURE_D3D11_OPTIONS2));
    // The option structs' sizes, which the decode reads as that many words.
    CHECK(sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS) == 14 * sizeof(uint32_t));
    CHECK(sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS2) == 8 * sizeof(uint32_t));
    // ...and fields the decode names, at the offsets the words are read from.
    CHECK(offsetof(D3D11_FEATURE_DATA_D3D11_OPTIONS, ClearView) == 4 * sizeof(uint32_t));
    CHECK(offsetof(D3D11_FEATURE_DATA_D3D11_OPTIONS, ExtendedResourceSharing) == 13 * sizeof(uint32_t));
    CHECK(offsetof(D3D11_FEATURE_DATA_D3D11_OPTIONS2, TypedUAVLoadAdditionalFormats) == 1 * sizeof(uint32_t));
    CHECK(offsetof(D3D11_FEATURE_DATA_D3D11_OPTIONS2, UnifiedMemoryArchitecture) == 7 * sizeof(uint32_t));

    CHECK(!std::strcmp(hresultText(0), "S_OK"));
    CHECK(!std::strcmp(hresultText(S_FALSE), "S_FALSE"));
    CHECK(!std::strcmp(hresultText(E_INVALIDARG), "E_INVALIDARG"));
    CHECK(!std::strcmp(hresultText(E_FAIL), "E_FAIL"));
    CHECK(!std::strcmp(hresultText(E_NOTIMPL), "E_NOTIMPL"));
    CHECK(!std::strcmp(hresultText(E_NOINTERFACE), "E_NOINTERFACE"));
    CHECK(!std::strcmp(hresultText(E_OUTOFMEMORY), "E_OUTOFMEMORY"));
    CHECK(!std::strcmp(hresultText(static_cast<int32_t>(0x887A0004u)), "DXGI_ERROR_UNSUPPORTED"));
    CHECK(hresultText(static_cast<int32_t>(0x8000FFFFu)) == nullptr);   // E_UNEXPECTED: not one of ours
}

void checkOptionFlags() {
    uint32_t w1[14] = {};
    w1[4] = 1;
    char buf[512];
    d3d11OptionsFlags(w1, 14, buf, sizeof(buf));
    CHECK_STR(buf,
              "OMLogicOp=0 UAVOnlyForcedSampleCount=0 DiscardAPIs=0 FlagsForUpdateAndCopy=0 "
              "ClearView=1 CopyWithOverlap=0 CBPartialUpdate=0 CBOffsetting=0 MapNoOverwriteCB=0 "
              "MapNoOverwriteBufSRV=0 MSAARTVForcedSampleCountOne=0 SAD4=0 ExtendedDoubles=0 "
              "ExtendedResourceSharing=0");
    // Words the caller did not have print as '?', never as garbage from past the array.
    d3d11OptionsFlags(w1, 5, buf, sizeof(buf));
    CHECK(std::strstr(buf, "ClearView=1 CopyWithOverlap=?") != nullptr);
    d3d11OptionsFlags(nullptr, 0, buf, sizeof(buf));
    CHECK(std::strstr(buf, "OMLogicOp=? ") == buf);
    uint32_t w2[8] = {0, 1, 0, 2, 3, 0, 0, 1};
    d3d11Options2Flags(w2, 8, buf, sizeof(buf));
    CHECK_STR(buf,
              "PSStencilRef=0 TypedUAVLoadAdditionalFormats=1 ROVs=0 ConservativeRasterTier=2 "
              "TiledResourcesTier=3 MapOnDefaultTextures=0 StandardSwizzle=0 UMA=1");
}

void checkKeys() {
    const uint64_t k = formatQueryKey(kFqCheckFormatSupport, 26, 0, 0xC321, 0);
    CHECK(k != 0);
    CHECK(k == formatQueryKey(kFqCheckFormatSupport, 26, 0, 0xC321, 0));   // the same triple, the same key
    // Every field is part of the key: change any one and it is a different query.
    CHECK(k != formatQueryKey(kFqFeatureFormatSupport, 26, 0, 0xC321, 0));  // which call asked
    CHECK(k != formatQueryKey(kFqCheckFormatSupport, 23, 0, 0xC321, 0));    // which format
    CHECK(k != formatQueryKey(kFqCheckFormatSupport, 26, E_FAIL, 0xC321, 0)); // the HRESULT
    CHECK(k != formatQueryKey(kFqCheckFormatSupport, 26, 0, 0xC320, 0));    // the answer
    CHECK(k != formatQueryKey(kFqFeatureOther, 26, 0, 0xC321, 4));          // the size
    CHECK(formatQueryKey(0, 0, 0, 0, 0) != 0);
    // A digest of the output bytes: equal bytes agree, one differing byte does not.
    const uint32_t a[4] = {1, 2, 3, 4};
    uint32_t b[4] = {1, 2, 3, 4};
    CHECK(fqFnv1a32(a, sizeof(a)) == fqFnv1a32(b, sizeof(b)));
    b[3] = 5;
    CHECK(fqFnv1a32(a, sizeof(a)) != fqFnv1a32(b, sizeof(b)));
    CHECK(fqFnv1a32(nullptr, 16) == fqFnv1a32(nullptr, 0));
}

void checkLedgerBasics() {
    FormatQueryLedger ledger;
    CHECK(ledger.calls() == 0 && ledger.distinct() == 0 && ledger.printed() == 0 &&
          ledger.suppressed() == 0 && ledger.untracked() == 0);

    // First sight prints, and numbers the call; the same key again is a repeat.
    const uint64_t k1 = formatQueryKey(kFqCheckFormatSupport, 26, 0, kMaskFull, 0);
    const uint64_t k2 = formatQueryKey(kFqCheckFormatSupport, 23, 0, kMaskNoUav, 0);
    FormatQueryLedger::Verdict v = ledger.note(k1);
    CHECK(v.isNew && v.print && v.callNumber == 1 && v.distinctNumber == 1);
    v = ledger.note(k2);
    CHECK(v.isNew && v.print && v.callNumber == 2 && v.distinctNumber == 2);
    v = ledger.note(k1);
    CHECK(!v.isNew && !v.print && v.callNumber == 3 && v.distinctNumber == 0);   // the call is still counted
    v = ledger.note(k2);
    CHECK(!v.isNew && !v.print && v.callNumber == 4);
    CHECK(ledger.calls() == 4 && ledger.distinct() == 2 && ledger.printed() == 2);
    // The same question with a different answer is a NEW line: the answer changing is
    // the finding.
    v = ledger.note(formatQueryKey(kFqCheckFormatSupport, 26, 0, kMaskNoUav, 0));
    CHECK(v.isNew && v.print && v.callNumber == 5 && v.distinctNumber == 3);

    // The cap: the first 40 distinct queries print, in first-call order; the rest are
    // counted and not printed, and a repeat of one that printed stays a repeat.
    FormatQueryLedger capped;
    uint32_t printedSeen = 0, lastDistinct = 0;
    for (uint32_t i = 0; i < 100; ++i) {
        v = capped.note(formatQueryKey(kFqCheckFormatSupport, i, 0, i * 7u, 0));
        CHECK(v.isNew && v.callNumber == i + 1 && v.distinctNumber == i + 1);
        CHECK(v.print == (i < FormatQueryLedger::kMaxLines));
        if (v.print) { ++printedSeen; CHECK(v.distinctNumber == lastDistinct + 1); lastDistinct = v.distinctNumber; }
    }
    CHECK(FormatQueryLedger::kMaxLines == 40);
    CHECK(printedSeen == 40 && capped.printed() == 40 && capped.distinct() == 100 &&
          capped.suppressed() == 60 && capped.untracked() == 0 && capped.calls() == 100);
    v = capped.note(formatQueryKey(kFqCheckFormatSupport, 3, 0, 21, 0));      // printed before the cap
    CHECK(!v.isNew && !v.print && capped.distinct() == 100);
    v = capped.note(formatQueryKey(kFqCheckFormatSupport, 77, 0, 77 * 7u, 0)); // suppressed past the cap
    CHECK(!v.isNew && !v.print && capped.distinct() == 100 && capped.suppressed() == 60);

    // A full table degrades to counting: every cell is used, the next key is untracked,
    // nothing crashes, nothing loops, and a key that is already in the table still
    // answers "repeat".
    FormatQueryLedger full;
    for (uint32_t i = 0; i < FormatQueryLedger::kTableSlots; ++i)
        full.note(formatQueryKey(kFqFeatureOther, 1000 + i, 0, i, 4));
    CHECK(full.distinct() == FormatQueryLedger::kTableSlots && full.untracked() == 0);
    v = full.note(formatQueryKey(kFqFeatureOther, 999999, 0, 1, 4));
    CHECK(!v.isNew && !v.print && full.untracked() == 1 && full.distinct() == FormatQueryLedger::kTableSlots);
    v = full.note(formatQueryKey(kFqFeatureOther, 1000, 0, 0, 4));
    CHECK(!v.isNew && full.untracked() == 1);
}

// The ledger is entered by whatever thread the game asks from, with no lock. Eight
// threads ask the same 120 queries in different orders, several times over: every
// query must be new exactly once, the cap must hold exactly, and no two prints may
// share an ordinal.
void checkLedgerThreads() {
    constexpr int kThreads = 8, kQueries = 120, kRounds = 50;
    FormatQueryLedger ledger;
    std::vector<uint64_t> keys;
    for (int i = 0; i < kQueries; ++i) keys.push_back(formatQueryKey(kFqCheckFormatSupport, i, 0, i + 1, 0));

    std::atomic<uint32_t> newCount{0}, printCount{0}, go{0};
    std::atomic<uint32_t> ordinalSeen[FormatQueryLedger::kMaxLines + 1];
    for (auto& o : ordinalSeen) o.store(0);
    std::atomic<uint32_t> badOrdinal{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load()) std::this_thread::yield();
            for (int round = 0; round < kRounds; ++round) {
                for (int i = 0; i < kQueries; ++i) {
                    const int index = (i * (2 * t + 1) + t * 13 + round) % kQueries;   // a different order per thread
                    const FormatQueryLedger::Verdict v = ledger.note(keys[static_cast<size_t>(index)]);
                    if (v.isNew) newCount.fetch_add(1);
                    if (v.print) {
                        printCount.fetch_add(1);
                        if (v.distinctNumber < 1 || v.distinctNumber > FormatQueryLedger::kMaxLines)
                            badOrdinal.fetch_add(1);
                        else
                            ordinalSeen[v.distinctNumber].fetch_add(1);
                    }
                }
            }
        });
    }
    go.store(1);
    for (auto& th : threads) th.join();

    CHECK(newCount.load() == static_cast<uint32_t>(kQueries));       // each query new exactly once
    CHECK(ledger.distinct() == static_cast<uint32_t>(kQueries));
    CHECK(printCount.load() == FormatQueryLedger::kMaxLines);         // the cap, exactly
    CHECK(ledger.printed() == FormatQueryLedger::kMaxLines);
    CHECK(ledger.suppressed() == static_cast<uint32_t>(kQueries) - FormatQueryLedger::kMaxLines);
    CHECK(ledger.calls() == static_cast<uint32_t>(kThreads * kRounds * kQueries));
    CHECK(ledger.untracked() == 0);
    CHECK(badOrdinal.load() == 0);
    bool everyOrdinalOnce = true;
    for (uint32_t n = 1; n <= FormatQueryLedger::kMaxLines; ++n) everyOrdinalOnce &= ordinalSeen[n].load() == 1;
    CHECK(everyOrdinalOnce);                                          // 1..40, each printed once
}

void checkLines() {
    // The adapter line, and the line for an adapter that would not be read.
    CHECK_STR(adapterLine(fixtureAdapter()),
              "D3D11 adapter: device=000001F2A3B4C5D0 (#1) description=\"Test Adapter\" vendor=0x10DE "
              "deviceId=0x2684 subsys=0x167B1458 revision=161 dedicatedVideoMemory=24576 MiB "
              "dedicatedSystemMemory=0 MiB sharedSystemMemory=32768 MiB luid=00000000:0000ABCD");
    char buf[1024];
    formatAdapterFailureLine(fixtureAdapter().device, 1, "IDXGIDevice::GetAdapter", E_NOINTERFACE, buf, sizeof(buf));
    CHECK_STR(buf, "D3D11 adapter: device=000001F2A3B4C5D0 (#2) not identified: IDXGIDevice::GetAdapter "
                   "hr=0x80004002 (E_NOINTERFACE)");

    // The self-query line: both masks, hex and names, then whether the other path agrees.
    FqSelfFormat s;
    s.format = DXGI_FORMAT_R11G11B10_FLOAT;
    s.support = kMaskFull;  s.support2 = kUavLoadStore;
    s.via = kMaskFull;
    CHECK_STR(selfLine(s),
              "format support (self-query): R11G11B10_FLOAT[26] support=0x0200C321 "
              "[BUFFER|TEX2D|SHADER_LOAD|SHADER_SAMPLE|RENDER_TARGET|BLENDABLE|TYPED_UAV] "
              "support2=0x000000C0 [UAV_TYPED_LOAD|UAV_TYPED_STORE] via-CheckFeatureSupport=same");
    s.format = DXGI_FORMAT_R10G10B10A2_TYPELESS;
    s.support = kMaskNoUav; s.support2 = 0; s.via = kMaskNoUav;
    CHECK_STR(selfLine(s),
              "format support (self-query): R10G10B10A2_TYPELESS[23] support=0x0000C321 "
              "[BUFFER|TEX2D|SHADER_LOAD|SHADER_SAMPLE|RENDER_TARGET|BLENDABLE] "
              "support2=0x00000000 [-] via-CheckFeatureSupport=same");
    s.via = D3D11_FORMAT_SUPPORT_TEXTURE2D;    // the other path disagrees
    CHECK_STR(selfLine(s),
              "format support (self-query): R10G10B10A2_TYPELESS[23] support=0x0000C321 "
              "[BUFFER|TEX2D|SHADER_LOAD|SHADER_SAMPLE|RENDER_TARGET|BLENDABLE] "
              "support2=0x00000000 [-] via-CheckFeatureSupport=DIFFERS support=0x00000020 [TEX2D]");
    FqSelfFormat refused;   // a device that refuses: no mask, the HRESULT instead
    refused.format = DXGI_FORMAT_R24G8_TYPELESS;
    refused.supportHr = refused.support2Hr = refused.viaHr = E_INVALIDARG;
    refused.support = refused.support2 = refused.via = 0xDEADBEEF;   // garbage in the out params: never printed
    CHECK_STR(selfLine(refused),
              "format support (self-query): R24G8_TYPELESS[44] support=FAILED hr=0x80070057 (E_INVALIDARG) "
              "support2=FAILED hr=0x80070057 (E_INVALIDARG) via-CheckFeatureSupport=same");

    FqSelfOptions o = fixtureOptions();
    formatOptionsLine(o, buf, sizeof(buf));
    CHECK_STR(buf,
              "device options (self-query): D3D11_OPTIONS hr=0x00000000 [OMLogicOp=0 "
              "UAVOnlyForcedSampleCount=0 DiscardAPIs=0 FlagsForUpdateAndCopy=0 ClearView=1 "
              "CopyWithOverlap=1 CBPartialUpdate=0 CBOffsetting=0 MapNoOverwriteCB=0 "
              "MapNoOverwriteBufSRV=0 MSAARTVForcedSampleCountOne=0 SAD4=0 ExtendedDoubles=0 "
              "ExtendedResourceSharing=0] D3D11_OPTIONS2 hr=0x00000000 [PSStencilRef=0 "
              "TypedUAVLoadAdditionalFormats=1 ROVs=0 ConservativeRasterTier=0 TiledResourcesTier=0 "
              "MapOnDefaultTextures=0 StandardSwizzle=0 UMA=1]");
    o.hr2 = E_INVALIDARG;
    formatOptionsLine(o, buf, sizeof(buf));
    CHECK(std::strstr(buf, "] D3D11_OPTIONS2 FAILED hr=0x80070057 (E_INVALIDARG)") != nullptr);
    CHECK(std::strstr(buf, "PSStencilRef") == nullptr);   // and no flags for an answer that was not one

    // The game's queries, each kind, each failure shape.
    CHECK_STR(gameLine(gameFormat(1, 26, 0, kMaskFull, "exe+0x2D4A1C3")),
              "format support (game #1): CheckFormatSupport(R11G11B10_FLOAT[26]) -> hr=0x00000000 "
              "support=0x0200C321 [BUFFER|TEX2D|SHADER_LOAD|SHADER_SAMPLE|RENDER_TARGET|BLENDABLE|TYPED_UAV] "
              "tid=14248 from=exe+0x2D4A1C3");
    CHECK_STR(gameLine(gameFormat(6, 200, E_INVALIDARG, 0xDEADBEEF, nullptr)),
              "format support (game #6): CheckFormatSupport(UNNAMED[200]) -> hr=0x80070057 (E_INVALIDARG) tid=14248");
    FqGameQuery q = gameFormat(2, 26, 0, 0, "exe+0x2D4A2F0");
    q.kind = kFqFeatureFormatSupport2;
    CHECK_STR(gameLine(q),
              "format support (game #2): CheckFeatureSupport(FORMAT_SUPPORT2, R11G11B10_FLOAT[26]) -> "
              "hr=0x00000000 support2=0x00000000 [-] tid=14248 from=exe+0x2D4A2F0");
    q.kind = kFqFeatureFormatSupport2;
    q.answer = kUavLoadStore;
    q.from = "0x00007FFB12345678";
    CHECK_STR(gameLine(q),
              "format support (game #2): CheckFeatureSupport(FORMAT_SUPPORT2, R11G11B10_FLOAT[26]) -> "
              "hr=0x00000000 support2=0x000000C0 [UAV_TYPED_LOAD|UAV_TYPED_STORE] tid=14248 "
              "from=0x00007FFB12345678");
    q.kind = kFqFeatureFormatSupport;
    q.asked = 23;
    q.answer = kMaskNoUav;
    q.from = "exe+0x2D4A300";
    CHECK_STR(gameLine(q),
              "format support (game #2): CheckFeatureSupport(FORMAT_SUPPORT, R10G10B10A2_TYPELESS[23]) -> "
              "hr=0x00000000 support=0x0000C321 [BUFFER|TEX2D|SHADER_LOAD|SHADER_SAMPLE|RENDER_TARGET|BLENDABLE] "
              "tid=14248 from=exe+0x2D4A300");

    FqGameQuery f;   // any other feature: its name, its size, its answer
    f.kind = kFqFeatureOther;
    f.callNumber = 9;
    f.asked = D3D11_FEATURE_D3D11_OPTIONS2;
    f.dataSize = 32;
    f.wordCount = 8;
    f.words[1] = 1; f.words[7] = 1;
    f.thread = 14248;
    f.from = "exe+0x2D4B000";
    CHECK_STR(gameLine(f),
              "format support (game #9): CheckFeatureSupport(D3D11_OPTIONS2[14], 32 bytes) -> hr=0x00000000 "
              "[PSStencilRef=0 TypedUAVLoadAdditionalFormats=1 ROVs=0 ConservativeRasterTier=0 "
              "TiledResourcesTier=0 MapOnDefaultTextures=0 StandardSwizzle=0 UMA=1] tid=14248 from=exe+0x2D4B000");
    f.asked = D3D11_FEATURE_THREADING;
    f.dataSize = 8;
    f.wordCount = 2;
    f.words[0] = 1; f.words[1] = 1; f.words[7] = 0;
    CHECK_STR(gameLine(f),
              "format support (game #9): CheckFeatureSupport(THREADING[0], 8 bytes) -> hr=0x00000000 "
              "words=[00000001 00000001] tid=14248 from=exe+0x2D4B000");
    f.asked = 40;   // past the last feature d3d11.h names
    f.dataSize = 64;
    f.wordCount = 16;
    for (uint32_t i = 0; i < 16; ++i) f.words[i] = 0xA0 + i;
    CHECK_STR(gameLine(f),
              "format support (game #9): CheckFeatureSupport(UNNAMED[40], 64 bytes) -> hr=0x00000000 "
              "words=[000000A0 000000A1 000000A2 000000A3 000000A4 000000A5 000000A6 000000A7 ...] "
              "tid=14248 from=exe+0x2D4B000");
    f.asked = D3D11_FEATURE_MARKER_SUPPORT;
    f.hr = E_INVALIDARG;
    f.dataSize = 4;
    CHECK_STR(gameLine(f),
              "format support (game #9): CheckFeatureSupport(MARKER_SUPPORT[12], 4 bytes) -> "
              "hr=0x80070057 (E_INVALIDARG) tid=14248 from=exe+0x2D4B000");

    // The closing count, and what its last clause says about the hooks.
    FqSummary sum;
    sum.calls = 412; sum.distinct = 77; sum.printed = 40; sum.suppressed = 37; sum.cap = 40;
    sum.hookRuns = 450;
    formatSummaryLine(sum, buf, sizeof(buf));
    CHECK_STR(buf,
              "format support (game queries): 412 call(s), 77 distinct; 40 logged, 37 suppressed past the "
              "40-line cap; 0 untracked (table full); 0 before the log opened; hooks ran 450 time(s), "
              "38 not reported");
    sum.preOpen = 8;   // calls before the log opened were reported to nothing, but they were reported
    formatSummaryLine(sum, buf, sizeof(buf));
    CHECK(std::strstr(buf, "8 before the log opened; hooks ran 450 time(s), 30 not reported") != nullptr);
    sum.hookRuns = 5;  // fewer runs than reports cannot happen; it must not wrap
    formatSummaryLine(sum, buf, sizeof(buf));
    CHECK(std::strstr(buf, "hooks ran 5 time(s), 0 not reported") != nullptr);
    FqSummary idle;    // the hooks were never reached: zeros, said out loud
    idle.cap = 40;
    formatSummaryLine(idle, buf, sizeof(buf));
    CHECK_STR(buf,
              "format support (game queries): 0 call(s), 0 distinct; 0 logged, 0 suppressed past the "
              "40-line cap; 0 untracked (table full); 0 before the log opened; hooks ran 0 time(s), "
              "0 not reported");

    // The line that says the hooks went on, and the one that says why they could not.
    formatHooksInstalledLine(fixtureAdapter().device, kDevSlotCheckFormatSupport, kDevSlotCheckFeatureSupport,
                             buf, sizeof(buf));
    CHECK_STR(buf,
              "format support: CheckFormatSupport (vtable slot 29) and CheckFeatureSupport (slot 33) are hooked "
              "on the game's device 000001F2A3B4C5D0; each forwards the real call first and returns its answer "
              "untouched. The game's queries follow as \"format support (game #N)\" lines.");
    formatHooksMissingLine(21, kDevSlotCheckFormatSupport, kDevSlotCheckFeatureSupport, buf, sizeof(buf));
    CHECK_STR(buf,
              "format support: the game's device table has 21 usable entries, too few for CheckFormatSupport "
              "(slot 29) and CheckFeatureSupport (slot 33): they are NOT hooked, so the game's queries are not "
              "logged this session. The adapter and self-query lines are unaffected.");

    // Bounded: a line into a short buffer is cut and says so, and never overruns. (Not
    // named "small": windows.h #defines that to char.)
    char tiny[40];
    std::memset(tiny, 'Z', sizeof(tiny));
    const size_t n = formatGameQueryLine(gameFormat(1, 26, 0, kMaskFull, "exe+0x2D4A1C3"), tiny, 30);
    CHECK(n == 29 && tiny[29] == '\0' && tiny[28] == '~' && tiny[30] == 'Z');
}

// How many lines can the whole instrument print in one session? The budget is under
// about 60. Counted from the limits the DLL itself applies (format_query_log.h).
void checkLineBudget() {
    CHECK(kFqMaxLinesPerSession == 58);
    CHECK(kFqMaxLinesPerSession <= 60);
    CHECK(kFqSelfFormatLines == 11 && kFqMaxAdapterLines == 3 && kFqMaxSummaryLines == 2 &&
          kFqInstallLines == 1 && kFqOptionsLines == 1);
    // The longest line either install message can make fits the buffer device_hook.cpp gives
    // it, with room: a line cut short there would lose its last words.
    char note[448];
    const size_t a = formatHooksInstalledLine(reinterpret_cast<const void*>(~static_cast<uintptr_t>(0)), 29, 33,
                                              note, sizeof(note));
    const size_t b = formatHooksMissingLine(4294967295u, 29, 33, note, sizeof(note));
    CHECK(a < sizeof(note) - 100 && b < sizeof(note) - 100 && note[b - 1] == '.');
}

void printExamples() {
    char buf[1024];
    std::printf("%s\n", adapterLine(fixtureAdapter()).c_str());
    FqSelfFormat s;
    s.format = DXGI_FORMAT_R11G11B10_FLOAT;
    s.support = kMaskFull; s.support2 = kUavLoadStore; s.via = kMaskFull;
    std::printf("%s\n", selfLine(s).c_str());
    s.format = DXGI_FORMAT_R10G10B10A2_TYPELESS;
    s.support = kMaskNoUav; s.support2 = 0; s.via = kMaskNoUav;
    std::printf("%s\n", selfLine(s).c_str());
    formatOptionsLine(fixtureOptions(), buf, sizeof(buf));
    std::printf("%s\n", buf);
    std::printf("%s\n", gameLine(gameFormat(1, 26, 0, kMaskFull, "exe+0x2D4A1C3")).c_str());
    FqGameQuery q = gameFormat(2, 26, 0, 0, "exe+0x2D4A2F0");
    q.kind = kFqFeatureFormatSupport2;
    std::printf("%s\n", gameLine(q).c_str());
    FqSummary sum;
    sum.calls = 412; sum.distinct = 77; sum.printed = 40; sum.suppressed = 37; sum.cap = 40;
    sum.hookRuns = 450;
    formatSummaryLine(sum, buf, sizeof(buf));
    std::printf("%s\n", buf);
    formatHooksInstalledLine(fixtureAdapter().device, kDevSlotCheckFormatSupport, kDevSlotCheckFeatureSupport,
                             buf, sizeof(buf));
    std::printf("%s\n", buf);
}

// ---- THE DEVICE HALF ---------------------------------------------------------------

// One call's whole observable result: its HRESULT, and the 256-byte buffer it was
// handed (a pattern before the call, so a byte the call did not write is a byte that
// still says so).
struct Answer {
    HRESULT hr = 0;
    unsigned char bytes[256];
    uint32_t size = 0;
    Answer() { std::memset(bytes, 0, sizeof(bytes)); }
};
bool sameAnswer(const Answer& a, const Answer& b) {
    return a.hr == b.hr && a.size == b.size && !std::memcmp(a.bytes, b.bytes, sizeof(a.bytes));
}

// What one call asked, enough to work out independently which ledger key it must have
// made.
struct Ask {
    bool format = false;       // CheckFormatSupport, else CheckFeatureSupport
    uint32_t value = 0;        // CheckFormatSupport: the DXGI_FORMAT asked
    uint32_t feature = 0;      // CheckFeatureSupport: the D3D11_FEATURE passed
    uint32_t size = 0;         // ...and the FeatureSupportDataSize
    bool nullData = false;
};
struct Call {
    Ask ask;
    Answer answer;
};

Answer askFormat(ID3D11Device* d, uint32_t format) {
    Answer a;
    UINT mask = 0xA5A5A5A5u;
    a.hr = d->CheckFormatSupport(static_cast<DXGI_FORMAT>(format), &mask);
    std::memcpy(a.bytes, &mask, 4);
    a.size = 4;
    return a;
}

// The same question to the ORIGINAL method, straight from the saved pointer: the
// reference that does not go through any hook.
PFN_CheckFormatSupport gRealFormat = nullptr;
PFN_CheckFeatureSupport gRealFeature = nullptr;

Answer askFormatDirect(ID3D11Device* d, uint32_t format) {
    Answer a;
    UINT mask = 0xA5A5A5A5u;
    a.hr = gRealFormat(d, static_cast<DXGI_FORMAT>(format), &mask);
    std::memcpy(a.bytes, &mask, 4);
    a.size = 4;
    return a;
}

Answer askFeature(ID3D11Device* d, uint32_t feature, uint32_t size, uint32_t firstWord, bool nullData) {
    Answer a;
    std::memset(a.bytes, 0xA5, sizeof(a.bytes));
    if ((feature == 2 || feature == 3) && size >= 4) std::memcpy(a.bytes, &firstWord, 4);
    a.hr = d->CheckFeatureSupport(static_cast<D3D11_FEATURE>(feature), nullData ? nullptr : a.bytes, size);
    a.size = size;
    return a;
}

// Every DXGI format, each asked three ways, then every feature at several sizes (most
// of them wrong, which is half the point: an error answer must pass through exactly as
// a good one does). No call passes a null buffer with a size: Windows' own runtime
// dereferences it (CheckFeatureSupport(FORMAT_SUPPORT, nullptr, 8) is an access
// violation in d3d11.dll with no hook anywhere), so it is not a question a game can ask
// and live. A size of 0 with no buffer is asked, and is refused as it should be.
constexpr uint32_t kExtraFormats[] = {130, 131, 132};
constexpr uint32_t kMatrixSizes[] = {0, 4, 8, 16, 32, 56, 64};
constexpr uint32_t kMatrixFeatures = 26;   // ids 0..25: the 22 d3d11.h names and four that it does not
constexpr uint32_t kMatrixFormats = 116 + 3;
constexpr uint32_t kMatrixCalls = kMatrixFormats * 3 + kMatrixFeatures * 7;   // 539

std::vector<uint32_t> matrixFormats() {
    std::vector<uint32_t> f;
    for (uint32_t i = 0; i < 116; ++i) f.push_back(i);
    for (uint32_t e : kExtraFormats) f.push_back(e);
    return f;
}

std::vector<Call> runMatrix(ID3D11Device* d) {
    std::vector<Call> calls;
    for (uint32_t f : matrixFormats()) {
        Call c1;
        c1.ask.format = true;
        c1.ask.value = f;
        c1.answer = askFormat(d, f);
        calls.push_back(c1);
        for (uint32_t feature = 2; feature <= 3; ++feature) {
            Call c;
            c.ask.feature = feature;
            c.ask.size = 8;
            c.answer = askFeature(d, feature, 8, f, false);
            calls.push_back(c);
        }
    }
    for (uint32_t feature = 0; feature < kMatrixFeatures; ++feature) {
        for (uint32_t size : kMatrixSizes) {
            Call c;
            c.ask.feature = feature;
            c.ask.size = size;
            c.ask.nullData = size == 0;
            c.answer = askFeature(d, feature, size, 0, size == 0);
            calls.push_back(c);
        }
    }
    return calls;
}

bool sameMatrix(const std::vector<Call>& a, const std::vector<Call>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!sameAnswer(a[i].answer, b[i].answer)) return false;
    return true;
}

// The ledger key a call MUST have made, worked out from the spec in
// format_support_log.h and not from format_support_log.cpp: the keys are what the
// closing count's "distinct" is built from, so this is the cross-check on the whole
// chain from the hook's arguments to the count.
uint64_t modelKey(const Call& c) {
    const int32_t hr = static_cast<int32_t>(c.answer.hr);
    if (c.ask.format) {
        uint32_t mask;
        std::memcpy(&mask, c.answer.bytes, 4);
        return formatQueryKey(kFqCheckFormatSupport, c.ask.value, hr, hr >= 0 ? mask : 0, 0);
    }
    const bool formatQuery = (c.ask.feature == 2 || c.ask.feature == 3) && c.ask.size == 8 && !c.ask.nullData;
    if (formatQuery) {
        uint32_t in, out;
        std::memcpy(&in, c.answer.bytes, 4);
        std::memcpy(&out, c.answer.bytes + 4, 4);
        return formatQueryKey(c.ask.feature == 2 ? kFqFeatureFormatSupport : kFqFeatureFormatSupport2,
                              in, hr, hr >= 0 ? out : 0, 0);
    }
    uint32_t digest = 0;
    if (hr >= 0 && !c.ask.nullData && c.ask.size)
        digest = fqFnv1a32(c.answer.bytes, c.ask.size < 256 ? c.ask.size : 256);
    return formatQueryKey(kFqFeatureOther, c.ask.feature, hr, digest, c.ask.size);
}

// The line the game's Nth call must have produced, from the call alone: the first
// occurrence of a key prints, the first 40 distinct keys print, and the call number is
// the ordinal among every reported call. `from` and `thread` are what the hooks below
// passed. Built with the pure formatter, so it checks what the glue collected and
// numbered and not how a line is spelled (checkLines does that).
FqGameQuery modelQuery(const Call& c, uint32_t callNumber, uint32_t thread, const char* from) {
    FqGameQuery q;
    q.callNumber = callNumber;
    q.thread = thread;
    q.from = from;
    q.hr = static_cast<int32_t>(c.answer.hr);
    if (c.ask.format) {
        q.kind = kFqCheckFormatSupport;
        q.asked = c.ask.value;
        uint32_t mask;
        std::memcpy(&mask, c.answer.bytes, 4);
        q.answer = q.hr >= 0 ? mask : 0;
        return q;
    }
    const bool formatQuery = (c.ask.feature == 2 || c.ask.feature == 3) && c.ask.size == 8 && !c.ask.nullData;
    if (formatQuery) {
        q.kind = c.ask.feature == 2 ? kFqFeatureFormatSupport : kFqFeatureFormatSupport2;
        uint32_t out;
        std::memcpy(&q.asked, c.answer.bytes, 4);
        std::memcpy(&out, c.answer.bytes + 4, 4);
        q.answer = q.hr >= 0 ? out : 0;
        return q;
    }
    q.kind = kFqFeatureOther;
    q.asked = c.ask.feature;
    q.dataSize = c.ask.size;
    if (q.hr >= 0 && !c.ask.nullData && c.ask.size) {
        const uint32_t bytes = c.ask.size < 256 ? c.ask.size : 256;
        q.wordCount = bytes / 4 < kFqWordsKept ? bytes / 4 : static_cast<uint32_t>(kFqWordsKept);
        std::memcpy(q.words, c.answer.bytes, q.wordCount * sizeof(uint32_t));
    }
    return q;
}

// ---- The hooks the device half installs -------------------------------------------
//
// The shape of device_hook.cpp's two, with `report` standing where
// `self == g_state->device && !addressInEdvr(caller)` does there, and the caller
// alternating between an address in this exe (what Elite's own call site looks like)
// and one in another module (what anything else looks like).

ID3D11Device* gDevice = nullptr;
std::atomic<bool> gReport{true};
std::atomic<uint32_t> gFormatHookCalls{0}, gFeatureHookCalls{0}, gParity{0};
const void* gExeCaller = nullptr;
const void* gOtherCaller = nullptr;

const void* nextCaller() {
    return (gParity.fetch_add(1, std::memory_order_relaxed) & 1u) ? gOtherCaller : gExeCaller;
}

HRESULT STDMETHODCALLTYPE hookFormat(ID3D11Device* self, DXGI_FORMAT format, UINT* support) {
    gFormatHookCalls.fetch_add(1, std::memory_order_relaxed);
    return formatSupportCheckFormat(gRealFormat, self, format, support,
                                    self == gDevice && gReport.load(std::memory_order_relaxed), nextCaller());
}

HRESULT STDMETHODCALLTYPE hookFeature(ID3D11Device* self, D3D11_FEATURE feature, void* data, UINT size) {
    gFeatureHookCalls.fetch_add(1, std::memory_order_relaxed);
    return formatSupportCheckFeature(gRealFeature, self, feature, data, size,
                                     self == gDevice && gReport.load(std::memory_order_relaxed), nextCaller());
}

// Stand-ins for a real call, to make the report path fault on purpose: each claims an
// HRESULT and writes nothing, so whatever pointer the caller handed in is never valid.
HRESULT STDMETHODCALLTYPE fakeFormatOk(ID3D11Device*, DXGI_FORMAT, UINT*) { return S_OK; }
HRESULT STDMETHODCALLTYPE fakeFormatFails(ID3D11Device*, DXGI_FORMAT, UINT*) { return E_FAIL; }
HRESULT STDMETHODCALLTYPE fakeFeatureFalse(ID3D11Device*, D3D11_FEATURE, void*, UINT) { return S_FALSE; }
HRESULT STDMETHODCALLTYPE fakeFeatureInvalid(ID3D11Device*, D3D11_FEATURE, void*, UINT) { return E_INVALIDARG; }

// ---- The log, read back ---------------------------------------------------------------

std::wstring scratchDir() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    wchar_t dir[MAX_PATH] = {};
    _snwprintf_s(dir, _TRUNCATE, L"%sedvr_fmtsupport_%lu", temp, GetCurrentProcessId());
    return dir;
}

void removeDirectory(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Every line of the newest file in `dir` matching `pattern`, without its
// "[hh:mm:ss.mmm] " stamp.
std::vector<std::string> readLogLines(const std::wstring& dir, const wchar_t* pattern = L"*.log") {
    std::vector<std::string> lines;
    std::wstring name;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return lines;
    do { name = fd.cFileName; } while (FindNextFileW(h, &fd));
    FindClose(h);
    HANDLE f = CreateFileW((dir + L"\\" + name).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return lines;
    std::string text;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) text.append(buf, got);
    CloseHandle(f);
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string one = text.substr(at, end - at);
        if (!one.empty() && one.back() == '\r') one.pop_back();
        const size_t stamp = one.find("] ");
        lines.push_back(stamp == std::string::npos ? one : one.substr(stamp + 2));
        at = end + 1;
    }
    return lines;
}

std::vector<std::string> linesStartingWith(const std::vector<std::string>& lines, const char* prefix) {
    std::vector<std::string> out;
    const size_t n = std::strlen(prefix);
    for (const std::string& l : lines)
        if (l.compare(0, n, prefix) == 0) out.push_back(l);
    return out;
}

size_t indexOfFirst(const std::vector<std::string>& lines, const char* prefix) {
    const size_t n = std::strlen(prefix);
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].compare(0, n, prefix) == 0) return i;
    return lines.size();
}

// The adapter as DXGI says it, asked here independently of the glue.
FqAdapter independentAdapter(ID3D11Device* device, uint32_t index, std::string& name) {
    FqAdapter a;
    a.device = device;
    a.index = index;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&desc))) {
        char utf8[400] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        for (char* c = utf8; *c; ++c)
            if (static_cast<unsigned char>(*c) < 0x20 || *c == '"') *c = '?';
        name = utf8;
        a.description = name.c_str();
        a.vendorId = desc.VendorId;
        a.deviceId = desc.DeviceId;
        a.subSysId = desc.SubSysId;
        a.revision = desc.Revision;
        a.dedicatedVideo = desc.DedicatedVideoMemory;
        a.dedicatedSystem = desc.DedicatedSystemMemory;
        a.sharedSystem = desc.SharedSystemMemory;
        a.luidHigh = static_cast<uint32_t>(desc.AdapterLuid.HighPart);
        a.luidLow = desc.AdapterLuid.LowPart;
    }
    return a;
}

std::string hexOf(uintptr_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(v));
    return b;
}

// ---- THE DEVICE HALF ------------------------------------------------------------------

int runDeviceChild() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    decltype(&D3D11CreateDevice) create = systemD3D11CreateDevice();
    CHECK(create != nullptr);
    if (!create) return 1;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const HRESULT made = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                &device, &level, &context);
    CHECK(SUCCEEDED(made));
    if (FAILED(made)) return 1;
    gDevice = device.Get();
    CHECK(reportSystemD3D11Only("format_support_test"));   // Windows' own d3d11 and no proxy of ours
    gExeCaller = reinterpret_cast<const void*>(&runDeviceChild);
    gOtherCaller = reinterpret_cast<const void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Sleep"));
    CHECK(gOtherCaller != nullptr);
    const std::wstring dir = scratchDir();
    removeDirectory(dir);
    CreateDirectoryW(dir.c_str(), nullptr);

    // PHASE 0. The reference: every question, asked of the device as Windows made it.
    const std::vector<Call> reference = runMatrix(device.Get());
    CHECK(reference.size() == kMatrixCalls);
    std::map<uint32_t, Answer> refFormat, refFeatureSupport, refFeatureSupport2;
    for (const Call& c : reference) {
        if (c.ask.format) {
            refFormat[c.ask.value] = c.answer;
        } else if ((c.ask.feature == 2 || c.ask.feature == 3) && c.ask.size == 8 && !c.ask.nullData) {
            uint32_t in;
            std::memcpy(&in, c.answer.bytes, 4);
            (c.ask.feature == 2 ? refFeatureSupport : refFeatureSupport2)[in] = c.answer;
        }
    }
    // A sanity check on the reference itself: WARP answers a real format, and refuses
    // nothing it should not (so a matrix of nothing but errors could not pass).
    uint32_t r11g11b10;
    std::memcpy(&r11g11b10, refFormat[26].bytes, 4);
    CHECK(SUCCEEDED(refFormat[26].hr) && (r11g11b10 & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0);
    bool sawError = false, sawSuccess = false;
    for (const Call& c : reference) { if (FAILED(c.answer.hr)) sawError = true; else sawSuccess = true; }
    CHECK(sawError && sawSuccess);   // both shapes of answer are in the matrix

    // The hooks go on, the way hookDevice puts them on: two in-place replacements on
    // the device's own table.
    void** vtable = *reinterpret_cast<void***>(device.Get());
    std::vector<void*> tableBefore(vtable, vtable + 43);
    VTableHook hook;
    CHECK(hook.attach(device.Get()) && hook.executablePrefix() > kDevSlotCheckFeatureSupport);
    CHECK(hook.replace(kDevSlotCheckFormatSupport, reinterpret_cast<void*>(&hookFormat),
                       reinterpret_cast<void**>(&gRealFormat)));
    CHECK(hook.replace(kDevSlotCheckFeatureSupport, reinterpret_cast<void*>(&hookFeature),
                       reinterpret_cast<void**>(&gRealFeature)));
    CHECK(hook.commit());
    CHECK(gRealFormat != nullptr && gRealFeature != nullptr);
    CHECK(vtable[kDevSlotCheckFormatSupport] == reinterpret_cast<void*>(&hookFormat) &&
          vtable[kDevSlotCheckFeatureSupport] == reinterpret_cast<void*>(&hookFeature));
    bool othersUntouched = true;   // and only those two slots: a miscount would show here
    for (size_t i = 0; i < tableBefore.size(); ++i) {
        if (i == kDevSlotCheckFormatSupport || i == kDevSlotCheckFeatureSupport) continue;
        othersUntouched = othersUntouched && vtable[i] == tableBefore[i];
    }
    CHECK(othersUntouched);
    CHECK(gRealFormat == reinterpret_cast<PFN_CheckFormatSupport>(tableBefore[kDevSlotCheckFormatSupport]) &&
          gRealFeature == reinterpret_cast<PFN_CheckFeatureSupport>(tableBefore[kDevSlotCheckFeatureSupport]));

    // PHASE 1. The log is CLOSED. Every call is reported to a hook that has nowhere to
    // say so: the answers must still be identical, the calls must be counted and not
    // remembered, and formatSupportLogDevice must spend nothing.
    CHECK(!Log::get().isOpen());
    const std::vector<Call> closed = runMatrix(device.Get());
    CHECK(sameMatrix(closed, reference));
    CHECK(gFormatHookCalls.load() + gFeatureHookCalls.load() == kMatrixCalls);   // the slots really are hooked
    CHECK(gFormatHookCalls.load() == kMatrixFormats && gFeatureHookCalls.load() == kMatrixCalls - kMatrixFormats);
    const uint32_t preOpenCalls = kMatrixCalls;
    formatSupportLogDevice(device.Get());
    formatSupportTick();                                     // nothing counted yet: nothing to say

    // PHASE 2. The log opens. The device's adapter and self-query go in first, as
    // attachToDevice does them (before hooks, in production; here the hooks are already
    // on, so EDVR's own calls are marked not-the-game's, which is what the return-address
    // test does for them there). Four devices: three adapter lines, one table.
    CHECK(Log::get().open(dir, L"fmtsupport"));
    gReport.store(false);
    for (int i = 0; i < 4; ++i) formatSupportLogDevice(device.Get());
    gReport.store(true);

    // PHASE 3. The matrix through the hooks, log open, from this thread.
    const uint32_t mainThread = GetCurrentThreadId();
    const uint32_t hookFormatBefore = gFormatHookCalls.load(), hookFeatureBefore = gFeatureHookCalls.load();
    const uint32_t parityAtPhase3 = gParity.load();
    const std::vector<Call> first = runMatrix(device.Get());
    CHECK(sameMatrix(first, reference));                   // bit-identical with the hook on, log open
    CHECK(gFormatHookCalls.load() - hookFormatBefore == kMatrixFormats &&
          gFeatureHookCalls.load() - hookFeatureBefore == kMatrixCalls - kMatrixFormats);
    std::set<uint64_t> expectedKeys;
    for (const Call& c : first) expectedKeys.insert(modelKey(c));
    uint32_t expectedCalls = kMatrixCalls;
    CHECK(expectedKeys.size() > 100);   // a matrix that repeats nothing much would test nothing

    // PHASE 4. Asked again, twice: every answer the same, and not one new line.
    for (int again = 0; again < 2; ++again) {
        CHECK(sameMatrix(runMatrix(device.Get()), reference));
        expectedCalls += kMatrixCalls;
    }

    // PHASE 5. The hook told the call is not the game's (another device, or EDVR's own
    // module): the answer is the same and the call is neither counted nor remembered.
    gReport.store(false);
    CHECK(sameMatrix(runMatrix(device.Get()), reference));
    gReport.store(true);

    // PHASE 6. Four threads at once, asking the same formats in bulk and then 50 that
    // nobody has asked yet (so the ledger is inserted into concurrently): no answer
    // differs from the original's, and the count is exact.
    constexpr uint32_t kThreads = 4, kIterations = 30, kFresh = 50;
    std::vector<Answer> directFresh;
    for (uint32_t k = 0; k < kFresh; ++k) {
        directFresh.push_back(askFormatDirect(device.Get(), 300 + k));
        Call c;
        c.ask.format = true;
        c.ask.value = 300 + k;
        c.answer = directFresh.back();
        expectedKeys.insert(modelKey(c));
    }
    std::atomic<uint32_t> mismatches{0}, go{0};
    std::vector<std::thread> threads;
    const std::vector<uint32_t> formats = matrixFormats();
    for (uint32_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            while (!go.load()) std::this_thread::yield();
            for (uint32_t i = 0; i < kIterations; ++i)
                for (uint32_t f : formats)
                    if (!sameAnswer(askFormat(device.Get(), f), refFormat.at(f))) mismatches.fetch_add(1);
            for (uint32_t k = 0; k < kFresh; ++k)
                if (!sameAnswer(askFormat(device.Get(), 300 + k), directFresh[k])) mismatches.fetch_add(1);
        });
    }
    go.store(1);
    for (auto& th : threads) th.join();
    CHECK(mismatches.load() == 0);
    expectedCalls += kThreads * (kIterations * kMatrixFormats + kFresh);

    // PHASE 7. The closing count: nothing until the queries have been quiet for five
    // ticks, one line then and none for the ticks after; a second when new queries have
    // come and gone quiet; never a third.
    const uint32_t calls1 = expectedCalls, distinct1 = static_cast<uint32_t>(expectedKeys.size());
    // The closing count also says how often the hooks ran: every entry, reported or not.
    // That is the reported calls, the ones before the log opened, the self-query's own 35
    // (11 formats three ways, and the two option structs: EDVR's, so not the game's),
    // and the whole matrix asked for a caller that was not the game's.
    const uint32_t hookRuns1 = gFormatHookCalls.load() + gFeatureHookCalls.load();
    CHECK(hookRuns1 == calls1 + preOpenCalls + 35 + kMatrixCalls);
    for (int i = 0; i < 12; ++i) formatSupportTick();
    for (uint32_t k = 0; k < 10; ++k) {
        const Answer a = askFormat(device.Get(), 400 + k);
        Call c;
        c.ask.format = true;
        c.ask.value = 400 + k;
        c.answer = a;
        expectedKeys.insert(modelKey(c));
        ++expectedCalls;
    }
    // A call that FAILS leaves the caller's buffer as it found it -- whatever garbage the
    // game's own variable held (WARP zeroes it, so the stand-ins do this). The same failing
    // question asked over two different garbages is ONE query, not two: the answer is not
    // read from a call that gave none. And the garbage must come back exactly as it went in.
    for (const uint32_t initial : {0xA5A5A5A5u, 0x5A5A5A5Au}) {
        UINT mask = initial;
        const HRESULT hr = formatSupportCheckFormat(&fakeFormatFails, device.Get(), static_cast<DXGI_FORMAT>(900),
                                                    &mask, true, gExeCaller);
        CHECK(hr == E_FAIL && mask == initial);
        Call c;
        c.ask.format = true;
        c.ask.value = 900;
        c.answer.hr = hr;
        std::memcpy(c.answer.bytes, &mask, 4);
        expectedKeys.insert(modelKey(c));
        ++expectedCalls;
    }
    for (const uint32_t garbagePair : {0xA5A5A5A5u, 0x5A5A5A5Au}) {
        uint32_t data[2] = {900, garbagePair};   // {InFormat, OutFormatSupport2}
        const HRESULT hr = formatSupportCheckFeature(&fakeFeatureInvalid, device.Get(), D3D11_FEATURE_FORMAT_SUPPORT2,
                                                     data, sizeof(data), true, gExeCaller);
        CHECK(hr == E_INVALIDARG && data[0] == 900 && data[1] == garbagePair);
        Call c;
        c.ask.feature = 3;
        c.ask.size = 8;
        c.answer.hr = hr;
        std::memcpy(c.answer.bytes, data, sizeof(data));
        expectedKeys.insert(modelKey(c));
        ++expectedCalls;
    }
    const uint32_t calls2 = expectedCalls, distinct2 = static_cast<uint32_t>(expectedKeys.size());
    // Those four went to the glue directly, not through a hook wrapper: they are hook runs
    // all the same.
    const uint32_t hookRuns2 = gFormatHookCalls.load() + gFeatureHookCalls.load() + 4;
    CHECK(hookRuns2 == calls2 + preOpenCalls + 35 + kMatrixCalls);
    for (int i = 0; i < 12; ++i) formatSupportTick();
    askFormat(device.Get(), 26);
    ++expectedCalls;
    for (int i = 0; i < 80; ++i) formatSupportTick();

    // PHASE 8. A report that faults costs the line and never the answer. The stand-ins
    // claim a success and write nothing, so the pointer the report then reads is
    // garbage: the access violation must be absorbed, and the HRESULT must still be the
    // real call's -- success, a non-error success, and a failure alike, and with
    // `report` off the garbage must not be read at all.
    UINT* const garbage = reinterpret_cast<UINT*>(static_cast<uintptr_t>(0x10));
    CHECK(formatSupportCheckFormat(&fakeFormatOk, device.Get(), DXGI_FORMAT_R11G11B10_FLOAT, garbage, true,
                                   gExeCaller) == S_OK);
    CHECK(formatSupportCheckFeature(&fakeFeatureFalse, device.Get(), D3D11_FEATURE_FORMAT_SUPPORT2,
                                    reinterpret_cast<void*>(static_cast<uintptr_t>(0x10)), 8, true,
                                    gExeCaller) == S_FALSE);
    CHECK(formatSupportCheckFormat(&fakeFormatFails, device.Get(), DXGI_FORMAT_R11G11B10_FLOAT, garbage, true,
                                   gExeCaller) == E_FAIL);
    CHECK(formatSupportCheckFormat(&fakeFormatOk, device.Get(), DXGI_FORMAT_R11G11B10_FLOAT, garbage, false,
                                   gExeCaller) == S_OK);
    // The device itself still answers, through the hook, after all of that.
    CHECK(sameAnswer(askFormat(device.Get(), 26), refFormat[26]));

    // The hooks come off, and the table is the runtime's own again.
    hook.uninstall();
    bool restored = true;
    for (size_t i = 0; i < tableBefore.size(); ++i) restored = restored && vtable[i] == tableBefore[i];
    CHECK(restored);
    Log::get().close();

    // ---- THE LOG ----------------------------------------------------------------------
    const std::vector<std::string> lines = readLogLines(dir);
    CHECK(!lines.empty());
    // FST_SHOW_LOG=1 prints the instrument's own lines as WARP produced them: the real
    // thing, for a doc or a review to quote.
    if (std::getenv("FST_SHOW_LOG")) {
        for (const std::string& l : lines)
            if (l.compare(0, 14, "D3D11 adapter:") == 0 || l.compare(0, 14, "format support") == 0 ||
                l.compare(0, 14, "device options") == 0)
                std::printf("LOG %s\n", l.c_str());
    }
    const std::string adapterPrefix = "D3D11 adapter: ";
    const std::vector<std::string> adapters = linesStartingWith(lines, "D3D11 adapter: ");
    const std::vector<std::string> selfs = linesStartingWith(lines, "format support (self-query): ");
    const std::vector<std::string> options = linesStartingWith(lines, "device options (self-query): ");
    const std::vector<std::string> games = linesStartingWith(lines, "format support (game #");
    const std::vector<std::string> sums = linesStartingWith(lines, "format support (game queries): ");
    std::printf("format_support_test: the log carries %zu adapter, %zu self-query, %zu options, %zu game, "
                "%zu closing line(s)\n", adapters.size(), selfs.size(), options.size(), games.size(), sums.size());

    // The adapter: three lines for four devices, each the line DXGI's own answer makes.
    CHECK(adapters.size() == kFqMaxAdapterLines);
    for (uint32_t i = 0; i < adapters.size() && i < kFqMaxAdapterLines; ++i) {
        std::string name;
        const FqAdapter a = independentAdapter(device.Get(), i, name);
        CHECK_STR(adapters[i], adapterLine(a));
    }
    CHECK(!adapters.empty() && adapters[0].find("(#1) description=\"") != std::string::npos);

    // The self-query: the 11 formats, in the order asked, each line what the pure
    // formatter makes of the answers the ORIGINAL methods give, and one options line.
    const uint32_t selfOrder[11] = {26, 24, 23, 10, 28, 29, 27, 41, 20, 19, 44};
    CHECK(selfs.size() == kFqSelfFormatLines);
    for (size_t i = 0; i < selfs.size() && i < 11; ++i) {
        FqSelfFormat s;
        s.format = selfOrder[i];
        const Answer& a = refFormat.at(selfOrder[i]);
        s.supportHr = a.hr;
        std::memcpy(&s.support, a.bytes, 4);
        const Answer& b = refFeatureSupport2.at(selfOrder[i]);
        s.support2Hr = b.hr;
        std::memcpy(&s.support2, b.bytes + 4, 4);
        const Answer& v = refFeatureSupport.at(selfOrder[i]);
        s.viaHr = v.hr;
        std::memcpy(&s.via, v.bytes + 4, 4);
        CHECK_STR(selfs[i], selfLine(s));
    }
    CHECK(options.size() == kFqOptionsLines);
    {
        FqSelfOptions so;
        D3D11_FEATURE_DATA_D3D11_OPTIONS o1{};
        so.hr1 = gRealFeature(device.Get(), D3D11_FEATURE_D3D11_OPTIONS, &o1, sizeof(o1));
        std::memcpy(so.words1, &o1, sizeof(o1));
        D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2{};
        so.hr2 = gRealFeature(device.Get(), D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2));
        std::memcpy(so.words2, &o2, sizeof(o2));
        char buf[1024];
        formatOptionsLine(so, buf, sizeof(buf));
        if (!options.empty()) CHECK_STR(options[0], buf);
    }
    // In the order the device half asked: the adapter first, the table after it.
    CHECK(indexOfFirst(lines, "D3D11 adapter: ") < indexOfFirst(lines, "format support (self-query): ") &&
          indexOfFirst(lines, "format support (self-query): ") < indexOfFirst(lines, "device options (self-query): ") &&
          indexOfFirst(lines, "device options (self-query): ") < indexOfFirst(lines, "format support (game #"));

    // The game's queries: EXACTLY the first 40 distinct, in first-call order, each the
    // line the model makes of that call -- its number among the reported calls, the
    // thread, and the caller as the hook passed it, alternating exe and not-exe.
    CHECK(games.size() == FormatQueryLedger::kMaxLines);
    {
        const uintptr_t exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const std::string exeFrom = "exe+0x" + hexOf(reinterpret_cast<uintptr_t>(gExeCaller) - exeBase);
        char other[32];
        std::snprintf(other, sizeof(other), "0x%016llX",
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(gOtherCaller)));
        std::set<uint64_t> seen;
        size_t printed = 0;
        for (size_t i = 0; i < first.size() && printed < games.size(); ++i) {
            if (!seen.insert(modelKey(first[i])).second) continue;
            const bool exe = ((parityAtPhase3 + i) & 1u) == 0;
            const FqGameQuery q = modelQuery(first[i], static_cast<uint32_t>(i + 1), mainThread,
                                             exe ? exeFrom.c_str() : other);
            CHECK_STR(games[printed], gameLine(q));
            ++printed;
        }
        CHECK(printed == FormatQueryLedger::kMaxLines);
    }
    // The first three lines are the first three calls, which were three different
    // questions about format 0: that is "log the first-call order" in one glance.
    if (games.size() >= 4) {
        CHECK(games[0].find("(game #1): CheckFormatSupport(UNKNOWN[0]) ") != std::string::npos);
        CHECK(games[1].find("(game #2): CheckFeatureSupport(FORMAT_SUPPORT, UNKNOWN[0]) ") != std::string::npos);
        CHECK(games[2].find("(game #3): CheckFeatureSupport(FORMAT_SUPPORT2, UNKNOWN[0]) ") != std::string::npos);
        CHECK(games[3].find("(game #4): CheckFormatSupport(R32G32B32A32_TYPELESS[1]) ") != std::string::npos);
    }
    bool sawExe = false, sawOther = false;
    for (const std::string& g : games) { sawExe |= g.find(" from=exe+0x") != std::string::npos; sawOther |= g.find(" from=0x0000") != std::string::npos; }
    CHECK(sawExe && sawOther);

    // The closing count: two lines, at the two moments, with the numbers the model
    // expects -- including the calls that came before the log opened, and none of the
    // calls made for a caller that was not the game's.
    CHECK(sums.size() == kFqMaxSummaryLines);
    if (sums.size() == kFqMaxSummaryLines) {
        char buf[448];
        FqSummary s;
        s.calls = calls1; s.distinct = distinct1; s.printed = 40; s.suppressed = distinct1 - 40;
        s.untracked = 0; s.preOpen = preOpenCalls; s.cap = 40; s.hookRuns = hookRuns1;
        formatSummaryLine(s, buf, sizeof(buf));
        CHECK_STR(sums[0], buf);
        s.calls = calls2; s.distinct = distinct2; s.suppressed = distinct2 - 40; s.hookRuns = hookRuns2;
        formatSummaryLine(s, buf, sizeof(buf));
        CHECK_STR(sums[1], buf);
    }

    // The report that faulted was absorbed, named, and did not crash the process.
    bool faultNamed = false;
    for (const std::string& l : lines)
        faultNamed |= l.find("FAULT ABSORBED exception=0xC0000005 site=formatSupport.gameQuery") != std::string::npos;
    CHECK(faultNamed);

    // And the whole instrument's output is bounded: nothing cut short, nothing past the
    // budget. The budget's last line is device_hook.cpp's, that the hooks went on, which
    // this rig's own hook install does not write (and the rig checks that it does not).
    const size_t instrumentLines = adapters.size() + selfs.size() + options.size() + games.size() + sums.size();
    CHECK(instrumentLines == kFqMaxLinesPerSession - kFqInstallLines);
    CHECK(linesStartingWith(lines, "format support: ").empty());
    std::printf("format_support_test: the instrument wrote %zu lines here, %u with device_hook.cpp's (budget 60)\n",
                instrumentLines, kFqMaxLinesPerSession);
    bool truncated = false;
    for (const std::string& l : lines) truncated |= l.find("...[truncated]") != std::string::npos;
    CHECK(!truncated);
    removeDirectory(dir);

    std::printf("format_support_test: device half: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

// The hooks are never reached: nothing calls either pass-through function. Until the
// sixtieth tick the closing count stays silent; on it, it says once, with zeros, that the
// hooks ran 0 times -- the line that turns "no game lines" from an ambiguity into a
// result. No device and no hook are involved, which is the point: this is the session
// whose hooks never went on, or never saw the game. Two logs, so the sixtieth tick can be
// read apart from the fifty-nine before it.
int runIdleChild() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring dir = scratchDir();
    removeDirectory(dir);
    CreateDirectoryW(dir.c_str(), nullptr);

    CHECK(Log::get().open(dir, L"idlea"));
    for (int i = 0; i < 59; ++i) formatSupportTick();
    Log::get().close();
    const std::vector<std::string> before = readLogLines(dir, L"edvr_idlea_*.log");
    CHECK(!before.empty());
    CHECK(linesStartingWith(before, "format support (game queries): ").empty());   // not a tick early

    CHECK(Log::get().open(dir, L"idleb"));
    formatSupportTick();                                   // the sixtieth
    for (int i = 0; i < 30; ++i) formatSupportTick();      // and no second line for the same nothing
    Log::get().close();
    const std::vector<std::string> after = readLogLines(dir, L"edvr_idleb_*.log");
    const std::vector<std::string> sums = linesStartingWith(after, "format support (game queries): ");
    CHECK(sums.size() == 1);
    FqSummary idle;
    idle.cap = FormatQueryLedger::kMaxLines;
    char buf[448];
    formatSummaryLine(idle, buf, sizeof(buf));
    if (sums.size() == 1) CHECK_STR(sums[0], buf);
    CHECK(linesStartingWith(after, "format support (game #").empty());
    CHECK(linesStartingWith(after, "D3D11 adapter: ").empty());
    removeDirectory(dir);
    std::printf("format_support_test: idle half: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

// The device half and the idle half each run in an owned child: the first patches a
// shared vtable, both hold process-wide state the pure half must not see, and a crash
// in either is this rig's failure, reported as such, and never takes the parent with it.
int runChild(const wchar_t* mode) {
    wchar_t executable[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return 2;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" " + mode;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable, &command[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process))
        return 2;
    DWORD code = 1;
    if (WaitForSingleObject(process.hProcess, 120000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &code);
    } else {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 1000);
        std::fputs("FAIL: a child timed out\n", stderr);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code != 0)
        std::printf("FAIL: the child (%ls) exited 0x%08lX\n", mode, static_cast<unsigned long>(code));
    return static_cast<int>(code);
}

}  // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 2) return 2;
    if (!std::strcmp(argv[1], "--dry-run")) {
        std::puts("format_support_test: dry-run (no device, no log, no files)");
        return 0;
    }
    if (!std::strcmp(argv[1], "--examples")) {
        printExamples();
        return 0;
    }
    if (!std::strcmp(argv[1], "--device-child")) return runDeviceChild();
    if (!std::strcmp(argv[1], "--idle-child")) return runIdleChild();
    if (std::strcmp(argv[1], "--self-test")) return 2;

    // The slot numbers device_slots.cpp held against the SDK at compile time.
    CHECK(kDevSlotCheckFormatSupport == 29 && kDevSlotCheckFeatureSupport == 33);

    checkMaskNames();
    checkNumberNames();
    checkOptionFlags();
    checkKeys();
    checkLedgerBasics();
    checkLedgerThreads();
    checkLines();
    checkLineBudget();
    std::printf("format_support_test: pure half: %u checks, %u failures\n", checks, failures);

    const int device = runChild(L"--device-child");
    const int idle = runChild(L"--idle-child");
    CHECK(device == 0);
    CHECK(idle == 0);
    std::printf("format_support_test: %u checks in this process, %u failures; device half %s, idle half %s\n", checks,
                failures, device == 0 ? "passed" : "FAILED", idle == 0 ? "passed" : "FAILED");
    return failures || device || idle ? 1 : 0;
}
