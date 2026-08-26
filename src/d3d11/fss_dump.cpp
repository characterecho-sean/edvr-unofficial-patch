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
// The tonemap (round 25): the residual squares survived every HDR-stage
// draw skip, so the dump now brackets the LDR stage too -- its output at
// the tonemap, and again at frame end after the compose chain and the
// panel quads have drawn over it. Holes appearing between c4 and c6 name
// the LDR interval; holes already at c4 name the tonemap's input.
constexpr uint64_t kTonemapHash   = 0x2D78DC3FD2C0C543ull;

// Checkpoint layout: [checkpoint][eye]. c0/c1 = before/after the ring quad
// (HDR), c2/c3 = before/after the composite (HDR), c4 = after the tonemap
// (LDR), c5 = frame end HDR, c6 = frame end LDR.
constexpr uint32_t kCheckpoints = 7;
constexpr uint32_t kEyes = 2;
const char* const kTags[kCheckpoints] = {"pre-ring",  "post-ring",
                                         "pre-comp",  "post-comp",
                                         "post-tone", "end-hdr", "end-ldr"};

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

// The targets remembered for the frame-end captures: HDR at the composite,
// LDR at the tonemap.
ID3D11Resource* g_eyeTex[kEyes] = {};
ID3D11Resource* g_ldrTex[kEyes] = {};

// Per-frame occurrence counters for the matched families.
uint8_t g_occRing = 0;
uint8_t g_occComp = 0;
uint8_t g_occTone = 0;

// The pending action chosen in OnEyeDraw, taken in Begin/End.
// kind: 0 = ring quad (c0/c1), 1 = composite (c2/c3), 2 = tonemap (c4 at
// End only).
uint32_t g_pendingKind = 0;
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
        if (g_ldrTex[e]) {
            g_ldrTex[e]->Release();
            g_ldrTex[e] = nullptr;
        }
    }
}

// Copy the current RTV0's resource into the checkpoint slot's staging.
// remember: 0 = no, 1 = into g_eyeTex (HDR), 2 = into g_ldrTex (LDR).
void capture(ID3D11DeviceContext* ctx, uint32_t c, uint32_t e,
             int remember) {
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
    if (remember && e < kEyes) {
        ID3D11Resource** slot = remember == 1 ? &g_eyeTex[e] : &g_ldrTex[e];
        if (*slot) (*slot)->Release();
        *slot = res;   // keep the GetResource ref
        tex->Release();
        return;
    }
    tex->Release();
    res->Release();
}

// The frame-end capture of a remembered per-eye texture into checkpoint c.
void captureRemembered(ID3D11DeviceContext* ctx, ID3D11Resource* res,
                       uint32_t c, uint32_t e) {
    if (!res) return;
    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(&tex));
    if (!tex) return;
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
    tex->Release();
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
            "fss dump ARMED: on body frame %u after this note, both eyes "
            "are captured around the ring quad, the composite, the tonemap "
            "and at frame end (HDR and LDR) -- fourteen raw images in "
            "edvr_logs\\dumps\\, one hitch, one dump per arm. Zoom a "
            "ringed body and let the build play.",
            g_dumpFrame);
    }
}

bool fssDumpWantsDraws() { return g_dumpFrame != 0 && !g_done; }

bool fssDumpOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                      uint32_t instances) {
    if (!fssDumpWantsDraws() || !ctx) return false;
    if (kind != 'N' || instances != 1 ||
        (count != 3 && count != 4 && count != 6)) {
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
    const bool tone = (h == kTonemapHash && count == 3);
    if (!ring && !comp && !tone) return false;

    // Occurrence -> eye. The counters also run on non-dump frames so the
    // mapping is warm, and reset every boundary.
    const uint8_t occ = ring ? ++g_occRing : (comp ? ++g_occComp : ++g_occTone);
    if (occ > kEyes) return false;
    if (!g_dumping) return false;
    g_pendingKind = ring ? 0 : (comp ? 1 : 2);
    g_pendingEye = occ - 1;
    return true;
}

void fssDumpBegin(ID3D11DeviceContext* ctx) {
    if (!ctx || !g_dumping || g_pendingKind == 2) return;
    guardedBudget(g_budget, [&] {
        capture(ctx, g_pendingKind == 0 ? 0 : 2, g_pendingEye, 0);
    });
}

void fssDumpEnd(ID3D11DeviceContext* ctx) {
    if (!ctx || !g_dumping) return;
    guardedBudget(g_budget, [&] {
        // Post checkpoints; the composite's remembers the HDR eye texture,
        // the tonemap's remembers the LDR target.
        if (g_pendingKind == 0) {
            capture(ctx, 1, g_pendingEye, 0);
        } else if (g_pendingKind == 1) {
            capture(ctx, 3, g_pendingEye, 1);
        } else {
            capture(ctx, 4, g_pendingEye, 2);
        }
    });
}

void fssDumpFrameBoundary(ID3D11DeviceContext* ctx) {
    const bool sawBody = g_occRing != 0 || g_occComp != 0;
    g_occRing = 0;
    g_occComp = 0;
    g_occTone = 0;
    if (!fssDumpWantsDraws() || !ctx) return;
    guardedBudget(g_budget, [&] {
        if (g_dumping) {
            // The dump frame just ended: frame-end captures of the HDR and
            // LDR textures remembered mid-frame, then write it all.
            for (uint32_t e = 0; e < kEyes; ++e) {
                captureRemembered(ctx, g_eyeTex[e], 5, e);
                captureRemembered(ctx, g_ldrTex[e], 6, e);
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
