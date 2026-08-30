#include "eye_split.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"

namespace edvr {
namespace {

// A measured frame from the field carries SIXTEEN eye-sized targets, not the
// six a deferred frame's headline stages suggest -- a geometry buffer, two
// depth-ish planes, an HDR image and a long post-process chain, each twice.
// The first cap here was sixteen, which that frame would have filled exactly,
// dropping anything a slightly different scene added. Thirty-two leaves room
// and still cannot run away.
constexpr uint32_t kMaxTargets = 32;

// Write every fourth texel of every fourth row.
//
// Those sixteen targets at full resolution are 322 MB, which is a file nobody
// wants to send and 322 MB of staging to hold besides. The question this
// instrument answers is "is the body in this eye's image at this stage", and
// a body fills a large part of the view -- at a quarter in each axis a
// 2324x2392 eye becomes 581x598, which is still hundreds of pixels across the
// disc and twenty megabytes for the whole frame.
//
// Decimating rather than averaging is deliberate: it needs no knowledge of
// the pixel format, so the C++ side stays dumb about the eleven formats the
// frame contains and the offline tool's existing decoders keep working
// unchanged. The cost is aliasing, which does not matter to a question this
// coarse.
constexpr uint32_t kStep = 4;

struct Target {
    ID3D11Resource*  res = nullptr;      // AddRef'd while remembered
    ID3D11Texture2D* staging = nullptr;
    uint32_t w = 0, h = 0, fmt = 0;
    uint32_t hits = 0;                   // draws seen into it this frame
    bool     filled = false;
};

Target   g_tg[kMaxTargets];
uint32_t g_count = 0;
uint32_t g_overflow = 0;

uint32_t g_want = 0;        // N from config; 0 = off
uint32_t g_frames = 0;      // scene frames since arming
bool     g_done = false;    // one dump per arming
bool     g_notedFirst = false;

FaultBudget g_budget("eyeSplit", 8);

// Bytes per texel, by DXGI_FORMAT, over the ranges the enum groups them in.
//
// fss_eye_dump answers this with "eight for R16G16B16A16, four for
// everything else", which is true of the four targets it was pointed at and
// false here. This instrument takes whatever the scene binds, and a measured
// frame already contains a 2324x2392 target at format 60 -- R8_TYPELESS, ONE
// byte a texel. Writing that at four would read three texels of the next
// row into every pixel and produce a file that looks like noise, with
// nothing at all to say it had gone wrong.
//
// Unknown formats fall back to four and are logged rather than guessed at
// silently, because a wrong stride is invisible in the output and this hunt
// has already paid twice for instruments that failed without saying so.
uint32_t texelBytes(uint32_t f) {
    if (f >= 1 && f <= 4)   return 16;  // R32G32B32A32
    if (f >= 5 && f <= 8)   return 12;  // R32G32B32
    if (f >= 9 && f <= 14)  return 8;   // R16G16B16A16
    if (f >= 15 && f <= 22) return 8;   // R32G32, R32G8X24
    if (f >= 23 && f <= 32) return 4;   // R10G10B10A2, R11G11B10, R8G8B8A8
    if (f >= 33 && f <= 38) return 4;   // R16G16
    if (f >= 39 && f <= 47) return 4;   // R32, R24G8
    if (f >= 48 && f <= 52) return 2;   // R8G8
    if (f >= 53 && f <= 59) return 2;   // R16
    if (f >= 60 && f <= 65) return 1;   // R8, A8
    if (f >= 87 && f <= 93) return 4;   // B8G8R8A8, B8G8R8X8
    return 4;
}

bool knownFormat(uint32_t f) {
    return (f >= 1 && f <= 65) || (f >= 87 && f <= 93);
}

void releaseAll() {
    for (uint32_t i = 0; i < kMaxTargets; ++i) {
        if (g_tg[i].staging) {
            g_tg[i].staging->Release();
            g_tg[i].staging = nullptr;
        }
        if (g_tg[i].res) {
            g_tg[i].res->Release();
            g_tg[i].res = nullptr;
        }
        g_tg[i] = Target{};
    }
    g_count = 0;
    g_overflow = 0;
}

// Forget the frame's targets without tearing down staging we could reuse --
// the shapes repeat frame to frame, so the staging textures are kept keyed
// by shape on the next frame's first sighting.
void clearFrame() {
    for (uint32_t i = 0; i < g_count; ++i) {
        if (g_tg[i].res) {
            g_tg[i].res->Release();
            g_tg[i].res = nullptr;
        }
        g_tg[i].hits = 0;
    }
    g_count = 0;
    g_overflow = 0;
}

void writeOut(ID3D11DeviceContext* ctx) {
    CreateDirectoryA("edvr_logs", nullptr);
    CreateDirectoryA("edvr_logs\\dumps", nullptr);

    // Index within shape, so the offline tool can pair them: two targets of
    // identical width, height and format are the two eyes of one stage.
    // Order is first-sighting order, which is the order the frame draws
    // them -- the first of a pair is the first eye rendered.
    uint32_t shapeIdx[kMaxTargets] = {};
    for (uint32_t i = 0; i < g_count; ++i) {
        uint32_t n = 0;
        for (uint32_t j = 0; j < i; ++j) {
            if (g_tg[j].w == g_tg[i].w && g_tg[j].h == g_tg[i].h &&
                g_tg[j].fmt == g_tg[i].fmt) {
                ++n;
            }
        }
        shapeIdx[i] = n;
    }

    uint32_t written = 0;
    for (uint32_t i = 0; i < g_count; ++i) {
        Target& t = g_tg[i];
        if (!t.filled || !t.staging) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(t.staging, 0, D3D11_MAP_READ, 0, &m)) ||
            !m.pData) {
            Log::get().note("eye split: map failed for %ux%u fmt=%u.", t.w,
                            t.h, t.fmt);
            continue;
        }
        if (!knownFormat(t.fmt)) {
            Log::get().note(
                "eye split: format %u is not one this knows the texel size "
                "of; writing it at four bytes a texel, which may be wrong. "
                "The %ux%u file is suspect -- say so when reporting it.",
                t.fmt, t.w, t.h);
        }
        // The file carries the DECIMATED size, because that is what is in it
        // and the offline tool should not have to be told twice. The manifest
        // keeps the source size and the step beside it.
        const uint32_t ow = (t.w + kStep - 1) / kStep;
        const uint32_t oh = (t.h + kStep - 1) / kStep;
        const uint32_t bpt = texelBytes(t.fmt);
        char path[160];
        _snprintf_s(path, sizeof(path), _TRUNCATE,
                    "edvr_logs\\dumps\\eyesplit_%ux%uf%u_eye%u.bin", ow, oh,
                    t.fmt, shapeIdx[i]);
        FILE* f = nullptr;
        fopen_s(&f, path, "wb");
        if (f) {
            const uint8_t* base = static_cast<const uint8_t*>(m.pData);
            for (uint32_t y = 0; y < t.h; y += kStep) {
                const uint8_t* row = base + static_cast<size_t>(y) * m.RowPitch;
                for (uint32_t x = 0; x < t.w; x += kStep) {
                    fwrite(row + static_cast<size_t>(x) * bpt, 1, bpt, f);
                }
            }
            fclose(f);
            ++written;
            // One manifest line per image, in the shape the offline tool
            // parses. draws= is what makes a lopsided stage visible from the
            // log alone, without opening a single pixel.
            Log::get().note(
                "EYESPLIT stage=%u shape=%ux%uf%u eye=%u draws=%u src=%ux%u "
                "step=%u pitch=%u res=%p file=%s",
                i, ow, oh, t.fmt, shapeIdx[i], t.hits, t.w, t.h, kStep,
                m.RowPitch, t.res, path);
        } else {
            Log::get().note("eye split: could not open %s.", path);
        }
        ctx->Unmap(t.staging, 0);
        t.filled = false;
    }
    if (g_overflow) {
        Log::get().note(
            "eye split: %u target(s) past the %u this records were DROPPED. "
            "The dump is incomplete; say so when reporting it.",
            g_overflow, kMaxTargets);
    }
    Log::get().note(
        "eye split: complete -- %u image(s) in edvr_logs\\dumps\\. Pair them "
        "by shape: two files of one shape are the two eyes of one stage, "
        "eye0 rendered first. Compare with tools\\diff_eye_split.py; the "
        "first stage whose two eyes disagree beyond ordinary stereo is the "
        "pass that breaks. A second dump needs a fresh arm -- clear "
        "advanced.eye_split, save, set it again.",
        written);
}

}  // namespace

void eyeSplitConfigure(Config& cfg) {
    const int n = cfg.getInt("advanced.eye_split", 0);
    const uint32_t want = n > 0 ? static_cast<uint32_t>(n) : 0;
    if (want == g_want) return;
    g_want = want;
    g_frames = 0;
    g_done = false;
    g_notedFirst = false;
    releaseAll();
    if (g_want) {
        Log::get().note(
            "eye split ARMED: on scene frame %u from now, every render "
            "target the scene draws into is copied and written to "
            "edvr_logs\\dumps\\ -- both eyes at every stage of one frame, at "
            "every %uth texel of every %uth row. A measured frame is sixteen "
            "targets and about 20 MB of files, with one visible hitch. Put "
            "the body that renders wrong in view and let it sit. One dump "
            "per arming.",
            g_want, kStep, kStep);
    } else {
        Log::get().note("eye split: off.");
    }
}

bool eyeSplitWantsDraws() { return g_want != 0 && !g_done; }

void eyeSplitOnEyeDraw(ID3D11DeviceContext* ctx) {
    if (!eyeSplitWantsDraws() || !ctx) return;
    guardedBudget(g_budget, [&] {
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (!rtv) return;
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        rtv->Release();
        if (!res) return;

        for (uint32_t i = 0; i < g_count; ++i) {
            if (g_tg[i].res == res) {
                ++g_tg[i].hits;
                res->Release();
                return;
            }
        }
        if (g_count >= kMaxTargets) {
            ++g_overflow;
            res->Release();
            return;
        }

        ID3D11Texture2D* tex = nullptr;
        res->QueryInterface(__uuidof(ID3D11Texture2D),
                            reinterpret_cast<void**>(&tex));
        if (!tex) {
            res->Release();
            return;
        }
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        tex->Release();

        Target& t = g_tg[g_count];
        // Staging is kept across frames when the shape matches, because the
        // same stages come back every frame and creating six eye-sized
        // staging textures per frame would cost more than the dump does.
        if (!t.staging || t.w != td.Width || t.h != td.Height ||
            t.fmt != static_cast<uint32_t>(td.Format)) {
            if (t.staging) {
                t.staging->Release();
                t.staging = nullptr;
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
                sd.ArraySize = 1;
                sd.SampleDesc.Count = 1;
                sd.SampleDesc.Quality = 0;
                dev->CreateTexture2D(&sd, nullptr, &t.staging);
                dev->Release();
            }
        }
        t.res = res;   // keep the GetResource ref until the boundary
        t.w = td.Width;
        t.h = td.Height;
        t.fmt = static_cast<uint32_t>(td.Format);
        t.hits = 1;
        t.filled = false;
        ++g_count;
    });
}

void eyeSplitFrameBoundary(ID3D11DeviceContext* ctx) {
    if (!eyeSplitWantsDraws() || !ctx) return;

    // A frame that drew nothing into an eye target is a menu or a loading
    // screen, and counting it would spend the arming before the player has
    // the body in view. Only frames that actually rendered a scene advance
    // the count, which is the same rule the FSS dump's body-frame gate uses
    // for the same reason.
    if (g_count == 0) return;

    ++g_frames;
    if (!g_notedFirst) {
        g_notedFirst = true;
        Log::get().note(
            "eye split: counting scene frames -- %u distinct target(s) this "
            "frame. The dump lands on frame %u.",
            g_count, g_want);
    }

    if (g_frames < g_want) {
        clearFrame();
        return;
    }

    guardedBudget(g_budget, [&] {
        for (uint32_t i = 0; i < g_count; ++i) {
            Target& t = g_tg[i];
            if (!t.staging || !t.res) continue;
            ctx->CopyResource(t.staging, t.res);
            t.filled = true;
        }
        writeOut(ctx);
    });
    g_done = true;
    clearFrame();
}

void eyeSplitShutdown() { releaseAll(); }

}  // namespace edvr
