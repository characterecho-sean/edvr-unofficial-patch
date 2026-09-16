#include "luma_probe.h"
#include "ui_deferred.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>

namespace edvr {
namespace {

template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;

constexpr int kStages = 5;
constexpr int kGrid = 16;
constexpr const char* kStageNames[kStages] = {"game", "clean_hdr", "dlss_in", "dlss_out", "final"};

// A 5-bit-exponent, N-bit-mantissa unsigned mini-float, bias 15 -- the
// shape shared by half floats (10-bit mantissa, plus a sign this helper
// does not handle) and the two channel widths inside R11G11B10_FLOAT.
float unpackMiniFloat(uint32_t raw, int mantissaBits) {
    const uint32_t mantMask = (1u << mantissaBits) - 1u;
    const uint32_t exp = (raw >> mantissaBits) & 0x1Fu;
    const uint32_t mant = raw & mantMask;
    if (exp == 0u) {
        if (mant == 0u) return 0.0f;
        return ldexpf(static_cast<float>(mant), -mantissaBits - 14);
    }
    if (exp == 0x1Fu) {
        return mant == 0u ? std::numeric_limits<float>::infinity()
                           : std::numeric_limits<float>::quiet_NaN();
    }
    const float m = 1.0f + static_cast<float>(mant) / static_cast<float>(1u << mantissaBits);
    return ldexpf(m, static_cast<int>(exp) - 15);
}

float halfToFloat(uint16_t h) {
    const bool neg = (h & 0x8000u) != 0u;
    const float mag = unpackMiniFloat(h & 0x7FFFu, 10);
    return neg ? -mag : mag;
}

enum class FormatKind { Unsupported, Rgba8, Bgra8, R11G11B10F, Rgba16F, R10G10B10A2, Rgba32F, Rgba16Unorm };

FormatKind classifyFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return FormatKind::Rgba8;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return FormatKind::Bgra8;
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return FormatKind::R11G11B10F;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return FormatKind::Rgba16F;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return FormatKind::R10G10B10A2;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return FormatKind::Rgba32F;
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return FormatKind::Rgba16Unorm;
        default:
            return FormatKind::Unsupported;
    }
}

size_t bytesPerPixel(FormatKind kind) {
    switch (kind) {
        case FormatKind::Rgba8:
        case FormatKind::Bgra8:
        case FormatKind::R11G11B10F:
        case FormatKind::R10G10B10A2:
            return 4;
        case FormatKind::Rgba16F:
        case FormatKind::Rgba16Unorm:
            return 8;
        case FormatKind::Rgba32F:
            return 16;
        default:
            return 0;
    }
}

void decodePixel(const uint8_t* p, FormatKind kind, float& r, float& g, float& b) {
    switch (kind) {
        case FormatKind::Rgba8:
            r = p[0] / 255.0f; g = p[1] / 255.0f; b = p[2] / 255.0f;
            break;
        case FormatKind::Bgra8:
            b = p[0] / 255.0f; g = p[1] / 255.0f; r = p[2] / 255.0f;
            break;
        case FormatKind::R11G11B10F: {
            uint32_t v; memcpy(&v, p, 4);
            r = unpackMiniFloat(v & 0x7FFu, 6);
            g = unpackMiniFloat((v >> 11) & 0x7FFu, 6);
            b = unpackMiniFloat((v >> 22) & 0x3FFu, 5);
            break;
        }
        case FormatKind::Rgba16F: {
            uint16_t rr, gg, bb;
            memcpy(&rr, p + 0, 2); memcpy(&gg, p + 2, 2); memcpy(&bb, p + 4, 2);
            r = halfToFloat(rr); g = halfToFloat(gg); b = halfToFloat(bb);
            break;
        }
        case FormatKind::R10G10B10A2: {
            uint32_t v; memcpy(&v, p, 4);
            r = static_cast<float>(v & 0x3FFu) / 1023.0f;
            g = static_cast<float>((v >> 10) & 0x3FFu) / 1023.0f;
            b = static_cast<float>((v >> 20) & 0x3FFu) / 1023.0f;
            break;
        }
        case FormatKind::Rgba32F: {
            float rr, gg, bb;
            memcpy(&rr, p + 0, 4); memcpy(&gg, p + 4, 4); memcpy(&bb, p + 8, 4);
            r = rr; g = gg; b = bb;
            break;
        }
        case FormatKind::Rgba16Unorm: {
            uint16_t rr, gg, bb;
            memcpy(&rr, p + 0, 2); memcpy(&gg, p + 2, 2); memcpy(&bb, p + 4, 2);
            r = rr / 65535.0f; g = gg / 65535.0f; b = bb / 65535.0f;
            break;
        }
        default:
            r = g = b = 0.0f;
            break;
    }
}

enum class SlotStatus : uint8_t { Empty, Pending, Read, Absent, Unsupported };

struct StageSlot {
    Ptr<ID3D11Texture2D> staging;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    SlotStatus status = SlotStatus::Empty;
    int waitFrames = 0;
    float mean = 0.0f, maxLuma = 0.0f, blackPct = 0.0f;
    unsigned formatForLog = 0;
};

struct EyeState {
    bool armed = false;
    StageSlot stages[kStages];
    LARGE_INTEGER lastReportQpc{};
    bool haveLastReport = false;
    // Sentinel distinct from every real outcome (-1 = "none", 0..4 = a
    // stage index), so the confirming line fires on the very first
    // report even when that report's answer is "none".
    int firstBlackStagePrev = -2;
    // Passes ended since the round was armed. A round is armed at the end
    // of a pass, so the next frame's draws (where clean_hdr is sampled)
    // and the next pass (the other four stages) both belong to it.
    int endsSinceArm = 0;
};

EyeState g_eyes[2];
bool g_armedLogged = false;

void noteArmedOnce() {
    if (g_armedLogged) return;
    g_armedLogged = true;
    Log::get().note(
        "luma probe: armed -- five stages sampled on a 16x16 grid every 2 s per eye: "
        "game (the texture the game submits), clean_hdr (the deferred UI's world "
        "snapshot), dlss_in (what DLSS receives), dlss_out (DLSS output before the UI "
        "replay), final (the texture handed to the VR half).");
}

void decodeSlot(StageSlot& slot, const D3D11_MAPPED_SUBRESOURCE& mapped) {
    const FormatKind kind = classifyFormat(slot.format);
    if (kind == FormatKind::Unsupported) {
        slot.status = SlotStatus::Unsupported;
        return;
    }
    const uint8_t* base = static_cast<const uint8_t*>(mapped.pData);
    const size_t bpp = bytesPerPixel(kind);
    const int w = static_cast<int>(slot.width);
    const int h = static_cast<int>(slot.height);
    double sum = 0.0;
    float maxLuma = 0.0f;
    int blackCount = 0;
    for (int j = 0; j < kGrid; ++j) {
        int py = static_cast<int>((static_cast<float>(j) + 0.5f) / static_cast<float>(kGrid) * static_cast<float>(h));
        if (py < 0) py = 0; else if (py >= h) py = h - 1;
        for (int i = 0; i < kGrid; ++i) {
            int px = static_cast<int>((static_cast<float>(i) + 0.5f) / static_cast<float>(kGrid) * static_cast<float>(w));
            if (px < 0) px = 0; else if (px >= w) px = w - 1;
            const uint8_t* p = base + static_cast<size_t>(py) * mapped.RowPitch + static_cast<size_t>(px) * bpp;
            float r = 0.0f, g = 0.0f, b = 0.0f;
            decodePixel(p, kind, r, g, b);
            const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            sum += luma;
            if (luma > maxLuma) maxLuma = luma;
            if (luma < (1.0f / 255.0f)) ++blackCount;
        }
    }
    slot.mean = static_cast<float>(sum / static_cast<double>(kGrid * kGrid));
    slot.maxLuma = maxLuma;
    slot.blackPct = static_cast<float>(blackCount) * 100.0f / static_cast<float>(kGrid * kGrid);
}

void formatStage(const StageSlot& slot, char* out, size_t outSize) {
    switch (slot.status) {
        case SlotStatus::Read:
            snprintf(out, outSize, "%.3f/%.3f/%d%%", static_cast<double>(slot.mean),
                      static_cast<double>(slot.maxLuma), static_cast<int>(slot.blackPct + 0.5f));
            break;
        case SlotStatus::Unsupported:
            snprintf(out, outSize, "fmt=%u?", slot.formatForLog);
            break;
        default:
            snprintf(out, outSize, "-");
            break;
    }
}

void reportRound(int eye, EyeState& es, const LumaProbeState& state) {
    char bufs[kStages][32];
    for (int s = 0; s < kStages; ++s) formatStage(es.stages[s], bufs[s], sizeof(bufs[s]));
    Log::get().note(
        "luma probe: eye=%d game=%s clean_hdr=%s dlss_in=%s dlss_out=%s final=%s | "
        "deferred=%u sampled=%u aliases=%u draws=%u apply=%u",
        eye, bufs[0], bufs[1], bufs[2], bufs[3], bufs[4],
        state.deferredEnabled ? 1u : 0u, state.sampled ? 1u : 0u, state.aliases, state.draws,
        state.applied ? 1u : 0u);

    int firstBlack = -1;
    for (int s = 0; s < kStages; ++s) {
        const StageSlot& slot = es.stages[s];
        if (slot.status == SlotStatus::Read && slot.mean < 0.004f && slot.maxLuma < 0.02f) {
            firstBlack = s;
            break;
        }
    }
    if (firstBlack != es.firstBlackStagePrev) {
        es.firstBlackStagePrev = firstBlack;
        char meanBufs[kStages][16];
        for (int s = 0; s < kStages; ++s) {
            if (es.stages[s].status == SlotStatus::Read)
                snprintf(meanBufs[s], sizeof(meanBufs[s]), "%.3f", static_cast<double>(es.stages[s].mean));
            else
                snprintf(meanBufs[s], sizeof(meanBufs[s]), "-");
        }
        Log::get().note(
            "luma probe: eye=%d first black stage is %s (game %s clean_hdr %s dlss_in %s dlss_out %s final %s).",
            eye, firstBlack < 0 ? "none" : kStageNames[firstBlack],
            meanBufs[0], meanBufs[1], meanBufs[2], meanBufs[3], meanBufs[4]);
    }
}

// Arms the next round for an eye when its throttle allows: no round in
// progress, no readback outstanding, and at least two seconds since the
// last completed report (or no report yet). Called at the end of a pass,
// so the round covers the next frame's draws and the next pass together.
void armIfDue(EyeState& es) {
    if (es.armed) return;   // a round is already in progress; let it run to completion
    for (const auto& s : es.stages) {
        if (s.status == SlotStatus::Pending) return;   // readback still outstanding
    }
    if (es.haveLastReport) {
        LARGE_INTEGER now{}, freq{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        if (freq.QuadPart > 0) {
            const double secs = static_cast<double>(now.QuadPart - es.lastReportQpc.QuadPart) /
                                 static_cast<double>(freq.QuadPart);
            if (secs < 2.0) return;
        }
    }
    es.armed = true;
    es.endsSinceArm = 0;
    for (auto& s : es.stages) {
        s.status = SlotStatus::Empty;
        s.waitFrames = 0;
    }
}

}  // namespace

void lumaProbeBegin(int eye) {
    noteArmedOnce();
    // Arming happens in lumaProbeEnd, not here: the clean_hdr stage is
    // sampled during the game's draws, which precede this pass, so a
    // round armed here would always miss it for the frame the other four
    // stages describe. This call only marks the probe live in the log.
    (void)eye;
}

void lumaProbeSample(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int eye, int stage) {
    noteArmedOnce();
    if (eye < 0 || eye > 1 || stage < 0 || stage >= kStages) return;
    EyeState& es = g_eyes[eye];
    if (!es.armed) return;
    StageSlot& slot = es.stages[stage];
    if (slot.status != SlotStatus::Empty) return;   // already sampled (or resolved) this round
    if (!tex || !ctx) {
        slot.status = SlotStatus::Absent;
        return;
    }
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    slot.formatForLog = static_cast<unsigned>(td.Format);
    if (td.SampleDesc.Count > 1) {
        slot.status = SlotStatus::Unsupported;
        return;
    }
    if (classifyFormat(td.Format) == FormatKind::Unsupported) {
        slot.status = SlotStatus::Unsupported;
        return;
    }
    if (!slot.staging || slot.width != td.Width || slot.height != td.Height || slot.format != td.Format) {
        Ptr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (!dev) {
            slot.status = SlotStatus::Absent;
            return;
        }
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = td.Width;
        sd.Height = td.Height;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = td.Format;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.BindFlags = 0;
        sd.MiscFlags = 0;
        slot.staging.Reset();
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &slot.staging)) || !slot.staging) {
            slot.status = SlotStatus::Absent;
            return;
        }
        slot.width = td.Width;
        slot.height = td.Height;
        slot.format = td.Format;
    }
    // Subresource 0 only: CopyResource would need the staging texture to
    // match the source's mip and array counts as well, and a refused copy
    // would leave stale bytes to be read as a false black.
    ctx->CopySubresourceRegion(slot.staging.Get(), 0, 0, 0, 0, tex, 0, nullptr);
    slot.status = SlotStatus::Pending;
    slot.waitFrames = 0;
}

void lumaProbeEnd(ID3D11DeviceContext* ctx, int eye, const LumaProbeState& state) {
    noteArmedOnce();
    if (eye < 0 || eye > 1) return;
    EyeState& es = g_eyes[eye];
    if (!ctx) return;   // try again once a frame hands us a context
    if (es.armed) {
        ++es.endsSinceArm;
        bool allResolved = true;
        for (auto& slot : es.stages) {
            if (slot.status != SlotStatus::Pending) continue;
            ++slot.waitFrames;
            const bool forceBlock = slot.waitFrames > 30;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT hr = ctx->Map(slot.staging.Get(), 0, D3D11_MAP_READ,
                                         forceBlock ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (SUCCEEDED(hr) && mapped.pData) {
                decodeSlot(slot, mapped);
                ctx->Unmap(slot.staging.Get(), 0);
                if (slot.status == SlotStatus::Pending) slot.status = SlotStatus::Read;
            } else if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                allResolved = false;
            } else {
                // An unexpected Map failure must not hang the round forever.
                slot.status = SlotStatus::Absent;
            }
        }
        if (!allResolved) return;
        // Every slot that ever saw lumaProbeSample this round is now Read,
        // Absent or Unsupported; anything still Empty was never sampled at
        // all (its hook site did not run this round) and is absent too.
        for (auto& slot : es.stages) {
            if (slot.status == SlotStatus::Empty) slot.status = SlotStatus::Absent;
        }
        reportRound(eye, es, state);
        es.armed = false;
        QueryPerformanceCounter(&es.lastReportQpc);
        es.haveLastReport = true;
        for (auto& slot : es.stages) slot.status = SlotStatus::Empty;
    }
    armIfDue(es);
}

}  // namespace edvr
