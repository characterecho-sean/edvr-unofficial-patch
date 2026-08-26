#include "fss_dump.h"

#include <cstdio>

#include <windows.h>

#include <d3d11.h>

#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "exposure_fix.h"   // lookupShaderHash

namespace edvr {
namespace {

constexpr uint64_t kRingQuadHash  = 0x7E38A6AA1269C901ull;
constexpr uint64_t kCompositeHash = 0x953C8123AD8DC13Bull;

// Checkpoint layout: [checkpoint][eye]. c0/c1 = before/after the ring quad,
// c2/c3 = before/after the composite, c4 = frame end (same resources as c3,
// re-copied at the boundary).
constexpr uint32_t kCheckpoints = 5;
constexpr uint32_t kEyes = 2;
const char* const kTags[kCheckpoints] = {"pre-ring", "post-ring",
                                         "pre-comp", "post-comp", "frame-end"};

uint32_t g_dumpFrame = 0;      // N from the key; 0 = off
uint32_t g_bodyFrames = 0;     // distinct frames with matched draws
uint32_t g_lastFrameNo = 0;
bool     g_dumping = false;    // this frame is the dump frame
bool     g_done = false;       // one dump per arming

struct Slot {
    ID3D11Texture2D* staging = nullptr;
    uint32_t w = 0, h = 0, fmt = 0;
    void*    srcPtr = nullptr;
    bool     filled = false;
};
Slot g_slots[kCheckpoints][kEyes];

// The eye targets remembered at the composite checkpoints, for c4.
ID3D11Resource* g_eyeTex[kEyes] = {};

// Per-frame occurrence counters for the two matched families.
uint8_t g_occRing = 0;
uint8_t g_occComp = 0;

// The pending checkpoint pair chosen in OnEyeDraw, taken in Begin/End.
uint32_t g_pendingPre = 0;
uint32_t g_pendingEye = 0;

uint64_t g_copies = 0;
bool     g_armedNoted = false;

FaultBudget g_budget("fssDump", 8);

void releaseAll() {
    for (uint32_t c = 0; c < kCheckpoints; ++c) {
        for (uint32_t e = 0; e < kEyes; ++e) {
            if (g_slots[c][e].staging) {
                g_slots[c][e].staging->Release();
                g_slots[c][e].staging = nullptr;
            }
            g_slots[c][e].filled = false;
        }
    }
    for (uint32_t e = 0; e < kEyes; ++e) {
        if (g_eyeTex[e]) {
            g_eyeTex[e]->Release();
            g_eyeTex[e] = nullptr;
        }
    }
}

// Copy the current RTV0's resource into the checkpoint slot's staging.
void capture(ID3D11DeviceContext* ctx, uint32_t c, uint32_t e,
             bool rememberEye) {
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    rtv->Release();
    if (!res) return;

    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(&tex));
    if (!tex) {
        res->Release();
        return;
    }
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);

    Slot& s = g_slots[c][e];
    if (!s.staging || s.w != td.Width || s.h != td.Height ||
        s.fmt != static_cast<uint32_t>(td.Format)) {
        if (s.staging) {
            s.staging->Release();
            s.staging = nullptr;
        }
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_TEXTURE2D_DESC sd = td;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.MipLevels = 1;
            dev->CreateTexture2D(&sd, nullptr, &s.staging);
            dev->Release();
        }
        s.w = td.Width;
        s.h = td.Height;
        s.fmt = static_cast<uint32_t>(td.Format);
    }
    if (s.staging) {
        ctx->CopyResource(s.staging, tex);
        s.srcPtr = res;
        s.filled = true;
        ++g_copies;
    }
    if (rememberEye && e < kEyes) {
        if (g_eyeTex[e]) g_eyeTex[e]->Release();
        g_eyeTex[e] = res;   // keep the GetResource ref
        tex->Release();
        return;
    }
    tex->Release();
    res->Release();
}

void writeOut(ID3D11DeviceContext* ctx) {
    CreateDirectoryA("edvr_logs", nullptr);
    CreateDirectoryA("edvr_logs\\dumps", nullptr);
    for (uint32_t c = 0; c < kCheckpoints; ++c) {
        for (uint32_t e = 0; e < kEyes; ++e) {
            Slot& s = g_slots[c][e];
            if (!s.filled || !s.staging) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(ctx->Map(s.staging, 0, D3D11_MAP_READ, 0, &m)) ||
                !m.pData) {
                Log::get().note("fss dump: map failed for %s eye%u.",
                                kTags[c], e);
                continue;
            }
            char path[128];
            _snprintf_s(path, sizeof(path), _TRUNCATE,
                        "edvr_logs\\dumps\\fssdump_c%u_%s_eye%u.bin", c,
                        kTags[c], e);
            FILE* f = nullptr;
            fopen_s(&f, path, "wb");
            if (f) {
                const uint8_t* row = static_cast<const uint8_t*>(m.pData);
                for (uint32_t y = 0; y < s.h; ++y) {
                    fwrite(row, 1, static_cast<size_t>(s.w) * 4, f);
                    row += m.RowPitch;
                }
                fclose(f);
                Log::get().note(
                    "FSSDUMP c=%u tag=%s eye=%u w=%u h=%u fmt=%u pitch=%u "
                    "res=%p file=%s",
                    c, kTags[c], e, s.w, s.h, s.fmt, m.RowPitch, s.srcPtr,
                    path);
            } else {
                Log::get().note("fss dump: could not open %s.", path);
            }
            ctx->Unmap(s.staging, 0);
            s.filled = false;
        }
    }
    Log::get().note("fss dump: complete -- %llu copies taken. The key stays "
                    "set but a second dump needs a fresh arm (clear it, "
                    "save, set it again).",
                    static_cast<unsigned long long>(g_copies));
}

}  // namespace

void fssDumpConfigure(Config& cfg) {
    const int n = cfg.getInt("advanced.fss_eye_dump", 0);
    const uint32_t want = n > 0 ? static_cast<uint32_t>(n) : 0;
    if (want == g_dumpFrame) return;
    g_dumpFrame = want;
    g_bodyFrames = 0;
    g_lastFrameNo = 0;
    g_dumping = false;
    g_done = false;
    g_armedNoted = false;
    if (g_dumpFrame) {
        Log::get().note(
            "fss dump ARMED: on body frame %u after this note, both eyes' "
            "render targets are captured before and after the ring quad, "
            "before and after the composite, and at frame end -- ten raw "
            "images in edvr_logs\\dumps\\, one hitch, one dump per arm. "
            "Zoom a ringed body and let the build play.",
            g_dumpFrame);
    }
}

bool fssDumpWantsDraws() { return g_dumpFrame != 0 && !g_done; }

bool fssDumpOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                      uint32_t instances) {
    if (!fssDumpWantsDraws() || !ctx) return false;
    if (kind != 'N' || instances != 1 || (count != 4 && count != 6)) {
        return false;
    }
    uint64_t h = 0;
    guardedBudget(g_budget, [&] {
        ID3D11VertexShader* vs = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        h = lookupShaderHash(vs);
        if (vs) vs->Release();
    });
    const bool ring = (h == kRingQuadHash && count == 4);
    const bool comp = (h == kCompositeHash && count == 6);
    if (!ring && !comp) return false;

    // Occurrence -> eye. The counters also run on non-dump frames so the
    // mapping is warm, and reset every boundary.
    const uint8_t occ = ring ? ++g_occRing : ++g_occComp;
    if (occ > kEyes) return false;
    if (!g_dumping) return false;
    g_pendingPre = ring ? 0 : 2;
    g_pendingEye = occ - 1;
    return true;
}

void fssDumpBegin(ID3D11DeviceContext* ctx) {
    if (!ctx || !g_dumping) return;
    guardedBudget(g_budget, [&] {
        capture(ctx, g_pendingPre, g_pendingEye, false);
    });
}

void fssDumpEnd(ID3D11DeviceContext* ctx) {
    if (!ctx || !g_dumping) return;
    guardedBudget(g_budget, [&] {
        // Post checkpoint; the composite's post also remembers the eye
        // texture for the frame-end capture.
        capture(ctx, g_pendingPre + 1, g_pendingEye, g_pendingPre == 2);
    });
}

void fssDumpFrameBoundary(ID3D11DeviceContext* ctx) {
    const bool sawBody = g_occRing != 0 || g_occComp != 0;
    g_occRing = 0;
    g_occComp = 0;
    if (!fssDumpWantsDraws() || !ctx) return;
    guardedBudget(g_budget, [&] {
        if (g_dumping) {
            // The dump frame just ended: frame-end capture of the eye
            // textures remembered at the composites, then write it all.
            for (uint32_t e = 0; e < kEyes; ++e) {
                if (!g_eyeTex[e]) continue;
                ID3D11Texture2D* tex = nullptr;
                g_eyeTex[e]->QueryInterface(__uuidof(ID3D11Texture2D),
                                            reinterpret_cast<void**>(&tex));
                if (!tex) continue;
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                Slot& s = g_slots[4][e];
                if (!s.staging || s.w != td.Width || s.h != td.Height ||
                    s.fmt != static_cast<uint32_t>(td.Format)) {
                    if (s.staging) {
                        s.staging->Release();
                        s.staging = nullptr;
                    }
                    ID3D11Device* dev = nullptr;
                    ctx->GetDevice(&dev);
                    if (dev) {
                        D3D11_TEXTURE2D_DESC sd = td;
                        sd.Usage = D3D11_USAGE_STAGING;
                        sd.BindFlags = 0;
                        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        sd.MiscFlags = 0;
                        sd.MipLevels = 1;
                        dev->CreateTexture2D(&sd, nullptr, &s.staging);
                        dev->Release();
                    }
                    s.w = td.Width;
                    s.h = td.Height;
                    s.fmt = static_cast<uint32_t>(td.Format);
                }
                if (s.staging) {
                    ctx->CopyResource(s.staging, tex);
                    s.srcPtr = g_eyeTex[e];
                    s.filled = true;
                    ++g_copies;
                }
                tex->Release();
            }
            writeOut(ctx);
            g_dumping = false;
            g_done = true;
            releaseAll();
            return;
        }
        if (sawBody) {
            ++g_bodyFrames;
            if (!g_armedNoted) {
                g_armedNoted = true;
                Log::get().note("fss dump: body frames counting.");
            }
            if (g_bodyFrames == g_dumpFrame) g_dumping = true;
        }
    });
}

void fssDumpShutdown() { releaseAll(); }

}  // namespace edvr
