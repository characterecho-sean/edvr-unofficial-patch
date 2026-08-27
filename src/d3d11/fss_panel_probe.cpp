#include "fss_panel_probe.h"

#include <cstdio>
#include <cstring>

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "device_hook.h"   // deviceHookFssModeLatch

namespace edvr {
namespace {

bool g_armed = false;
bool g_spent = false;

// The staged copies: record head (two records' worth), CB0 whole, the
// vertex buffer's head. Sized generously; a short source copies short.
constexpr uint32_t kRecBytes = 704;    // two 336-byte records + slack
constexpr uint32_t kCbBytes = 192;     // cb0[12]
constexpr uint32_t kVbBytes = 256;

ID3D11Buffer* g_stRec = nullptr;
ID3D11Buffer* g_stRecAt = nullptr;   // the window at the DRAW's record
ID3D11Buffer* g_stCb = nullptr;
ID3D11Buffer* g_stCb2 = nullptr;
ID3D11Buffer* g_stVb = nullptr;
// v6, THE SURVEY (v5 verified the whole addressing chain -- inst.x=2,
// FirstElement=0, stride 336 -- and still read a zeroed record: scale 0
// is a DEGENERATE quad, four corners collapsed to a point, invisible.
// The first matched draw is a hidden UI element; the FSS screen is MANY
// world-quads on the same atlas and the visible panel is a later one).
// So: snapshot the WHOLE t33 pool and the WHOLE instance stream in one
// frame, list every matched draw's startInstance, dump the buffers as
// binaries, and decode offline where the visible quad identifies itself
// by its sane scale.
ID3D11Buffer* g_stVb1 = nullptr;     // whole instance stream (vb slot 0)
uint32_t g_vb1Stride = 0, g_vb1Offset = 0, g_vb1SrcBytes = 0;
uint32_t g_vb1WinOffset = 0;
ID3D11Buffer* g_t33 = nullptr;
uint32_t g_t33First = 0;
uint32_t g_t33Stride = 0;
uint32_t g_instX = 0;
uint32_t g_recAtOffset = 0;
ID3D11Buffer* g_stT33All = nullptr;  // the whole record pool
uint32_t g_t33AllBytes = 0;
constexpr uint32_t kMaxSurvey = 24;
uint32_t g_surveyStart[kMaxSurvey];
uint32_t g_surveyCount = 0;

bool writeDump(const char* name, const void* data, uint32_t bytes) {
    CreateDirectoryA("edvr_logs", nullptr);
    CreateDirectoryA("edvr_logs\\dumps", nullptr);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "edvr_logs\\dumps\\%s", name);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(h, data, bytes, &written, nullptr);
    CloseHandle(h);
    return written == bytes;
}
uint32_t g_recSrcBytes = 0, g_vbSrcBytes = 0, g_vbStride = 0, g_vbOffset = 0;
uint32_t g_chromeW = 0, g_chromeH = 0, g_rtW = 0, g_rtH = 0;
uint32_t g_argStartIndex = 0, g_argStartInstance = 0;
int32_t  g_argBaseVertex = 0;

int g_copiedAtCountdown = -1;   // >=0: counting down to the readback
int g_phase = 0;   // 0 idle, 1 first copies queued, 2 record chase queued

FaultBudget g_budget("fssPanelProbe", 4);

ID3D11Buffer* makeStaging(ID3D11Device* dev, uint32_t bytes) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* b = nullptr;
    dev->CreateBuffer(&d, nullptr, &b);
    return b;
}

// Box-copy a range of src into a fresh staging buffer.
ID3D11Buffer* copyRange(ID3D11DeviceContext* ctx, ID3D11Device* dev,
                        ID3D11Buffer* src, uint32_t offset, uint32_t want,
                        uint32_t* srcBytes) {
    D3D11_BUFFER_DESC sd{};
    src->GetDesc(&sd);
    *srcBytes = sd.ByteWidth;
    if (offset >= sd.ByteWidth) return nullptr;
    const uint32_t avail = sd.ByteWidth - offset;
    const uint32_t n = avail < want ? avail : want;
    ID3D11Buffer* st = makeStaging(dev, n);
    if (!st) return nullptr;
    D3D11_BOX box{offset, 0, 0, offset + n, 1, 1};
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, src, 0, &box);
    return st;
}
ID3D11Buffer* copyHead(ID3D11DeviceContext* ctx, ID3D11Device* dev,
                       ID3D11Buffer* src, uint32_t want, uint32_t* srcBytes) {
    return copyRange(ctx, dev, src, 0, want, srcBytes);
}

void logBuffer(ID3D11DeviceContext* ctx, const char* name, ID3D11Buffer* st,
               uint32_t srcBytes) {
    if (!st) {
        Log::get().note("fss panel probe: %s was not captured.", name);
        return;
    }
    D3D11_BUFFER_DESC d{};
    st->GetDesc(&d);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) {
        Log::get().note("fss panel probe: %s refused to map.", name);
        return;
    }
    const uint8_t* p = static_cast<const uint8_t*>(m.pData);
    Log::get().note("fss panel probe: %s -- source %u bytes, %u captured:",
                    name, srcBytes, d.ByteWidth);
    char line[220];
    for (uint32_t off = 0; off < d.ByteWidth; off += 16) {
        const uint32_t n = d.ByteWidth - off < 16 ? d.ByteWidth - off : 16;
        int w = snprintf(line, sizeof(line), "  +%03u ", off);
        for (uint32_t i = 0; i < n; ++i) {
            w += snprintf(line + w, sizeof(line) - w, "%02X", p[off + i]);
        }
        w += snprintf(line + w, sizeof(line) - w, "  f:");
        for (uint32_t i = 0; i + 4 <= n; i += 4) {
            float f;
            memcpy(&f, p + off + i, 4);
            w += snprintf(line + w, sizeof(line) - w, " %.6g",
                          static_cast<double>(f));
        }
        Log::get().note("%s", line);
    }
    ctx->Unmap(st, 0);
}

}  // namespace

void fssPanelProbeConfigure(Config& cfg) {
    const bool was = g_armed;
    g_armed = cfg.getInt("advanced.fss_panel_probe", 0) != 0;
    if (g_armed && !was) {
        g_spent = false;
        Log::get().note(
            "fss panel probe: armed -- one capture of the scanner screen "
            "quad's record, constants and vertices at the next composite "
            "while the theater's mode latch is open.");
    }
}

bool fssPanelProbeWants() { return g_armed && !g_spent; }

void fssPanelProbeDrawArgs(uint32_t startIndex, int32_t baseVertex,
                           uint32_t startInstance) {
    g_argStartIndex = startIndex;
    g_argBaseVertex = baseVertex;
    g_argStartInstance = startInstance;
}

void fssPanelProbeOnComposite(ID3D11DeviceContext* ctx) {
    if (!fssPanelProbeWants() || !ctx) return;
    if (!deviceHookFssModeLatch()) return;   // never the loading screen

    guardedBudget(g_budget, [&] {
        if (g_copiedAtCountdown > 0) {
            if (g_surveyCount < kMaxSurvey) {
                g_surveyStart[g_surveyCount++] = g_argStartInstance;
            }
            --g_copiedAtCountdown;
            return;
        }
        if (g_copiedAtCountdown == 0 && g_phase == 1) {
            // v6: dump the whole pools as binaries and the survey table.
            g_spent = true;
            g_copiedAtCountdown = -1;
            g_phase = 0;
            Log::get().note(
                "fss panel probe: SURVEY -- %u matched draws; vb0 stride=%u "
                "offset=%u whole=%u bytes; t33 FirstElement=%u stride=%u "
                "whole=%u bytes; chrome %ux%u; eye %ux%u; cb0 follows.",
                g_surveyCount, g_vb1Stride, g_vb1Offset, g_vb1SrcBytes,
                g_t33First, g_t33Stride, g_t33AllBytes, g_chromeW, g_chromeH,
                g_rtW, g_rtH);
            {
                char line[200];
                int w = snprintf(line, sizeof(line),
                                 "fss panel probe: startInstances:");
                for (uint32_t i = 0; i < g_surveyCount; ++i) {
                    w += snprintf(line + w, sizeof(line) - w, " %u",
                                  g_surveyStart[i]);
                }
                Log::get().note("%s", line);
            }
            for (ID3D11Buffer** st : {&g_stVb1, &g_stT33All}) {
                if (!*st) continue;
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx->Map(*st, 0, D3D11_MAP_READ, 0, &m)) &&
                    m.pData) {
                    D3D11_BUFFER_DESC d{};
                    (*st)->GetDesc(&d);
                    const char* name = *st == g_stVb1
                                           ? "panel_probe_vb0.bin"
                                           : "panel_probe_t33.bin";
                    const bool ok = writeDump(name, m.pData, d.ByteWidth);
                    Log::get().note("fss panel probe: %s -- %u bytes %s.",
                                    name, d.ByteWidth,
                                    ok ? "written to edvr_logs\\dumps"
                                       : "COULD NOT BE WRITTEN");
                    ctx->Unmap(*st, 0);
                }
                (*st)->Release();
                *st = nullptr;
            }
            logBuffer(ctx, "cb0", g_stCb, kCbBytes);
            logBuffer(ctx, "cb2", g_stCb2, 128);
            if (g_stRec) { g_stRec->Release(); g_stRec = nullptr; }
            if (g_stCb) { g_stCb->Release(); g_stCb = nullptr; }
            if (g_stCb2) { g_stCb2->Release(); g_stCb2 = nullptr; }
            if (g_stVb) { g_stVb->Release(); g_stVb = nullptr; }
            if (g_t33) { g_t33->Release(); g_t33 = nullptr; }
            Log::get().note("fss panel probe: capture complete; standing "
                            "down for the session.");
            return;
        }
        if (false && g_copiedAtCountdown == 0 && g_phase == 1) {
            // Phase B: the first copies have executed. Decode inst.x from
            // the per-instance stream window and chase it into t33 --
            // queued NOW, read at the next countdown.
            g_copiedAtCountdown = -1;
            Log::get().note(
                "fss panel probe: draw startIndex=%u baseVertex=%d "
                "startInstance=%u; vb0 stride=%u offset=%u; vb1 stride=%u "
                "offset=%u window at %u; chrome %ux%u; eye target %ux%u.",
                g_argStartIndex, g_argBaseVertex, g_argStartInstance,
                g_vbStride, g_vbOffset, g_vb1Stride, g_vb1Offset,
                g_vb1WinOffset, g_chromeW, g_chromeH, g_rtW, g_rtH);
            logBuffer(ctx, "vb0 (instance stream) window", g_stVb1,
                      g_vb1SrcBytes);
            g_instX = 0;
            if (g_stVb1) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx->Map(g_stVb1, 0, D3D11_MAP_READ, 0, &m)) &&
                    m.pData) {
                    memcpy(&g_instX, m.pData, 4);
                    ctx->Unmap(g_stVb1, 0);
                }
            }
            logBuffer(ctx, "t33 record head", g_stRec, g_recSrcBytes);
            logBuffer(ctx, "cb0", g_stCb, kCbBytes);
            logBuffer(ctx, "cb2", g_stCb2, 128);
            logBuffer(ctx, "vertex head (vb0)", g_stVb, g_vbSrcBytes);
            if (g_stRecAt) { g_stRecAt->Release(); g_stRecAt = nullptr; }
            if (g_t33) {
                ID3D11Device* dev = nullptr;
                ctx->GetDevice(&dev);
                if (dev) {
                    g_recAtOffset = (g_t33First + g_instX) * g_t33Stride;
                    uint32_t dummy = 0;
                    g_stRecAt = copyRange(ctx, dev, g_t33, g_recAtOffset,
                                          kRecBytes, &dummy);
                    dev->Release();
                }
                g_t33->Release();
                g_t33 = nullptr;
            }
            Log::get().note(
                "fss panel probe: inst.x = %u, view FirstElement = %u, "
                "stride %u -> chasing t33 record at byte %u.",
                g_instX, g_t33First, g_t33Stride, g_recAtOffset);
            g_phase = 2;
            g_copiedAtCountdown = 3;
            return;
        }
        if (g_copiedAtCountdown == 0 && g_phase == 2) {
            // Phase C: the chased record.
            g_spent = true;
            g_copiedAtCountdown = -1;
            g_phase = 0;
            Log::get().note("fss panel probe: t33 record %u (byte %u):",
                            g_instX, g_recAtOffset);
            logBuffer(ctx, "t33 record at inst.x", g_stRecAt, g_recSrcBytes);
            if (g_stRec) { g_stRec->Release(); g_stRec = nullptr; }
            if (g_stRecAt) { g_stRecAt->Release(); g_stRecAt = nullptr; }
            if (g_stCb) { g_stCb->Release(); g_stCb = nullptr; }
            if (g_stCb2) { g_stCb2->Release(); g_stCb2 = nullptr; }
            if (g_stVb) { g_stVb->Release(); g_stVb = nullptr; }
            if (g_stVb1) { g_stVb1->Release(); g_stVb1 = nullptr; }
            Log::get().note("fss panel probe: capture complete; standing "
                            "down for the session.");
            return;
        }

        // First matched draw: queue the copies.
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;

        ID3D11ShaderResourceView* srv = nullptr;
        ctx->VSGetShaderResources(33, 1, &srv);
        if (srv) {
            ID3D11Resource* res = nullptr;
            srv->GetResource(&res);
            srv->Release();
            ID3D11Buffer* buf = nullptr;
            if (res) {
                res->QueryInterface(__uuidof(ID3D11Buffer),
                                    reinterpret_cast<void**>(&buf));
                res->Release();
            }
            if (buf) {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                ID3D11ShaderResourceView* srv2 = nullptr;
                ctx->VSGetShaderResources(33, 1, &srv2);
                g_t33First = 0;
                if (srv2) {
                    srv2->GetDesc(&vd);
                    if (vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) {
                        g_t33First = vd.Buffer.FirstElement;
                    } else if (vd.ViewDimension ==
                               D3D11_SRV_DIMENSION_BUFFEREX) {
                        g_t33First = vd.BufferEx.FirstElement;
                    }
                    srv2->Release();
                }
                D3D11_BUFFER_DESC bd{};
                buf->GetDesc(&bd);
                g_t33Stride = bd.StructureByteStride ? bd.StructureByteStride
                                                     : 336u;
                // The WHOLE pool, one staging copy.
                g_t33AllBytes = bd.ByteWidth;
                g_stT33All = makeStaging(dev, bd.ByteWidth);
                if (g_stT33All) ctx->CopyResource(g_stT33All, buf);
                buf->Release();
            }
        }

        ID3D11Buffer* cb = nullptr;
        ctx->VSGetConstantBuffers(0, 1, &cb);
        if (cb) {
            uint32_t src = 0;
            g_stCb = copyHead(ctx, dev, cb, kCbBytes, &src);
            cb->Release();
        }
        ID3D11Buffer* cb2 = nullptr;
        ctx->VSGetConstantBuffers(2, 1, &cb2);
        if (cb2) {
            uint32_t src = 0;
            g_stCb2 = copyHead(ctx, dev, cb2, 128, &src);
            cb2->Release();
        }

        ID3D11Buffer* vb = nullptr;
        ctx->IAGetVertexBuffers(0, 1, &vb, &g_vbStride, &g_vbOffset);
        if (vb) {
            g_stVb = copyHead(ctx, dev, vb, kVbBytes, &g_vbSrcBytes);
            vb->Release();
        }
        // The WHOLE instance stream (slot 0, uint2 inst per entry).
        ID3D11Buffer* vb1 = nullptr;
        ctx->IAGetVertexBuffers(0, 1, &vb1, &g_vb1Stride, &g_vb1Offset);
        if (vb1) {
            D3D11_BUFFER_DESC bd{};
            vb1->GetDesc(&bd);
            g_vb1SrcBytes = bd.ByteWidth;
            g_stVb1 = makeStaging(dev, bd.ByteWidth);
            if (g_stVb1) ctx->CopyResource(g_stVb1, vb1);
            vb1->Release();
        }
        g_surveyCount = 0;
        g_surveyStart[g_surveyCount++] = g_argStartInstance;

        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv) {
            ID3D11Resource* res = nullptr;
            rtv->GetResource(&res);
            rtv->Release();
            ID3D11Texture2D* tex = nullptr;
            if (res) {
                res->QueryInterface(__uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&tex));
                res->Release();
            }
            if (tex) {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                g_rtW = td.Width;
                g_rtH = td.Height;
                tex->Release();
            }
        }

        ID3D11ShaderResourceView* ps1 = nullptr;
        ctx->PSGetShaderResources(1, 1, &ps1);
        if (ps1) {
            ID3D11Resource* res = nullptr;
            ps1->GetResource(&res);
            ps1->Release();
            ID3D11Texture2D* tex = nullptr;
            if (res) {
                res->QueryInterface(__uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&tex));
                res->Release();
            }
            if (tex) {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                g_chromeW = td.Width;
                g_chromeH = td.Height;
                tex->Release();
            }
        }

        dev->Release();
        g_phase = 1;
        g_copiedAtCountdown = 20;   // keep surveying ~a frame or two
    });
}

void fssPanelProbeShutdownExtra() {
    if (g_stT33All) { g_stT33All->Release(); g_stT33All = nullptr; }
}
void fssPanelProbeShutdown() {
    fssPanelProbeShutdownExtra();
    if (g_stRec) { g_stRec->Release(); g_stRec = nullptr; }
    if (g_stRecAt) { g_stRecAt->Release(); g_stRecAt = nullptr; }
    if (g_stCb) { g_stCb->Release(); g_stCb = nullptr; }
    if (g_stCb2) { g_stCb2->Release(); g_stCb2 = nullptr; }
    if (g_stVb) { g_stVb->Release(); g_stVb = nullptr; }
    if (g_stVb1) { g_stVb1->Release(); g_stVb1 = nullptr; }
    if (g_t33) { g_t33->Release(); g_t33 = nullptr; }
}

}  // namespace edvr
