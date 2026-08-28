#include "intro_panel.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstring>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "binding_shadow.h"

namespace edvr {
namespace {

// cb2 is five float4s. Both the shader's declaration (CB2[5]) and the
// buffers the census found (80 bytes each) say so.
constexpr uint32_t kCbFloats = 20;
constexpr uint32_t kCbBytes = kCbFloats * 4;

// The constant buffer slot the transform lives in, from the disassembly.
constexpr uint32_t kVsSlot = 2;

// One buffer per eye is what the game uses; a third would mean the model is
// wrong and is worth refusing rather than guessing at.
constexpr uint32_t kMaxSlots = 2;

// Frames to let the GPU copy execute before mapping it. quad_probe's number,
// for the same reason: long enough that the map never stalls the render
// thread, short enough to retire within a blink.
constexpr uint32_t kSettleFrames = 4;

FaultBudget g_budget("introPanel", 4);

float    g_size = 1.0f;
bool     g_retired = false;      // the intro is over; stood down for good
bool     g_refused = false;      // the constants did not read as screen-space
uint32_t g_frame = 0;

// The frame's movie marker: the fill's target size, and the frame it was
// seen in. A composite is the movie's only if it samples that surface in
// that frame.
uint32_t g_fillW = 0, g_fillH = 0;
uint32_t g_fillFrame = 0;

struct Slot {
    void*         key = nullptr;   // the game's cb2 buffer for one eye
    ID3D11Buffer* stage = nullptr; // the readback copy, transient
    uint32_t      dueFrame = 0;    // 0 = nothing settling
    ID3D11Buffer* ours = nullptr;  // our replacement constants
    bool          ready = false;
};
Slot     g_slot[kMaxSlots];
uint32_t g_slotCount = 0;

void*    g_restore = nullptr;    // the game's buffer, for endDraw
uint32_t g_applied = 0;

// Is this buffer the screen-space placement, and not a world-space one?
//
// The discriminator is the perspective divide. A world-placed panel -- the
// splash, the menu, anything that goes through a view-projection -- has a
// varying w, which means non-zero w terms in cb2[1..3] and a cb2[4].w that
// is not 1. The movie's has neither: cb2[3] is all zeros and w is a constant
// 1. Measured on both, 2026-08-28.
//
// This is why the fix cannot wander into the splash even if the draw match
// were wrong: the splash's own numbers refuse it.
bool looksScreenSpace(const float* f) {
    auto zero = [](float v) { return v > -1e-9f && v < 1e-9f; };
    if (!zero(f[7]) || !zero(f[11])) return false;       // cb2[1].w, cb2[2].w
    for (uint32_t i = 12; i < 16; ++i) {                 // cb2[3] entirely
        if (!zero(f[i])) return false;
    }
    if (f[19] < 0.999f || f[19] > 1.001f) return false;  // cb2[4].w == 1
    // The scale must be a plausible half-size in pixels. A world-space quad
    // measured 4.4 by 2.5 units; a screen-space one measured 512 by 288.
    if (f[0] < 16.0f || f[1] < 16.0f) return false;
    return true;
}

Slot* findSlot(void* key) {
    for (uint32_t i = 0; i < g_slotCount; ++i) {
        if (g_slot[i].key == key) return &g_slot[i];
    }
    return nullptr;
}

void releaseSlot(Slot& s) {
    if (s.stage) { s.stage->Release(); s.stage = nullptr; }
    if (s.ours) { s.ours->Release(); s.ours = nullptr; }
    s = Slot();
}

}  // namespace

void introPanelConfigure(Config& cfg) {
    const float was = g_size;
    g_size = cfg.getFloat("fix.intro_video_size", 1.0f);
    if (g_size < 1.0f) g_size = 1.0f;
    if (g_size > 4.0f) g_size = 4.0f;
    if (g_size == was) return;
    if (g_size == 1.0f) {
        Log::get().note("intro video size: stock. The launch movie's panel is "
                        "left at the game's own size.");
        return;
    }
    Log::get().note(
        "intro video size: x%.2f. The launch movie's panel is drawn that much "
        "larger, about the point it already sits on -- straight ahead, both "
        "eyes, aspect kept. It stays head-locked: this changes its SIZE, not "
        "where it lives (docs\\intro-video.md). Stock is 1.0.",
        static_cast<double>(g_size));
}

bool introPanelWants() { return g_size != 1.0f && !g_retired && !g_refused; }

void introPanelNoteFill(uint32_t targetW, uint32_t targetH) {
    g_fillW = targetW;
    g_fillH = targetH;
    g_fillFrame = g_frame;
}

bool introPanelOnComposite(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                           uint32_t instances, uint32_t srvW, uint32_t srvH) {
    if (!introPanelWants() || !ctx) return false;
    // The composite's shape, from the census: a six-index instanced quad.
    if (kind != 'X' || count != 6 || instances != 1) return false;
    // ...sampling the surface the movie was converted into, THIS frame. The
    // fill draws before the composite in the same frame (census q ordering),
    // so a marker from this frame is the movie playing and nothing else.
    if (g_fillFrame != g_frame || !g_fillW) return false;
    if (srvW != g_fillW || srvH != g_fillH) return false;

    bool bound = false;
    guardedBudget(g_budget, [&] {
        ID3D11Buffer* cb = nullptr;
        ctx->VSGetConstantBuffers(kVsSlot, 1, &cb);
        if (!cb) return;
        Slot* s = findSlot(cb);
        if (!s && g_slotCount < kMaxSlots) {
            s = &g_slot[g_slotCount++];
            s->key = cb;
        }
        if (!s) { cb->Release(); return; }

        if (s->ready && s->ours) {
            g_restore = cb;
            ID3D11Buffer* ours = s->ours;
            ctx->VSSetConstantBuffers(kVsSlot, 1, &ours);
            bound = true;
            if (++g_applied == 1) {
                Log::get().note(
                    "intro video size: engaged -- the movie's panel is drawn "
                    "x%.2f its own size. Said once.",
                    static_cast<double>(g_size));
            }
            cb->Release();
            return;
        }
        if (s->stage || s->dueFrame) { cb->Release(); return; }

        // Copy the constants off the GPU. They are never written while the
        // movie plays -- measured -- so a single copy is the whole story.
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) { cb->Release(); return; }
        ResourceInfo info;
        const bool ok = bindingResolveResource(cb, &info) && info.isBuffer &&
                        info.a >= kCbBytes;
        if (ok) {
            D3D11_BUFFER_DESC sd{};
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.ByteWidth = kCbBytes;
            if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &s->stage)) && s->stage) {
                D3D11_BOX box{};
                box.right = kCbBytes;
                box.bottom = 1;
                box.back = 1;
                ctx->CopySubresourceRegion(s->stage, 0, 0, 0, 0,
                                           static_cast<ID3D11Resource*>(cb), 0,
                                           &box);
                s->dueFrame = g_frame + kSettleFrames;
            }
        }
        dev->Release();
        cb->Release();
    });
    return bound;
}

void introPanelEndDraw(ID3D11DeviceContext* ctx) {
    if (!ctx || !g_restore) return;
    ID3D11Buffer* orig = static_cast<ID3D11Buffer*>(g_restore);
    ctx->VSSetConstantBuffers(kVsSlot, 1, &orig);
    g_restore = nullptr;
}

void introPanelTick(ID3D11DeviceContext* ctx, bool sceneFrame) {
    ++g_frame;
    if (sceneFrame && !g_retired && (g_slotCount || g_applied)) {
        g_retired = true;
        Log::get().note(
            "intro video size: a rendered scene arrived -- the intro is over "
            "and this stands down for the session. It resized %u draw(s).",
            g_applied);
        introPanelShutdown();
        return;
    }
    if (!ctx) return;
    for (uint32_t i = 0; i < g_slotCount; ++i) {
        Slot& s = g_slot[i];
        if (!s.stage || !s.dueFrame || g_frame < s.dueFrame) continue;
        s.dueFrame = 0;
        guardedBudget(g_budget, [&] {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(ctx->Map(s.stage, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) {
                Log::get().note("intro video size: the constants could not be "
                                "read back; stock for this session.");
                g_refused = true;
                return;
            }
            float f[kCbFloats];
            memcpy(f, m.pData, sizeof(f));
            ctx->Unmap(s.stage, 0);

            if (!looksScreenSpace(f)) {
                // Not the movie's placement. Refusing the SESSION rather than
                // the slot: a world-space buffer here means the draw match is
                // reaching something it should not, and resizing that is how
                // a fix damages a screen nobody complained about.
                g_refused = true;
                Log::get().note(
                    "intro video size: the panel's constants do not read as a "
                    "screen-space placement (cb2[3] %.4g %.4g %.4g %.4g, "
                    "cb2[4].w %.4g) -- stock for this session, which is the "
                    "safe answer. docs\\intro-video.md says what the two "
                    "shapes look like.",
                    static_cast<double>(f[12]), static_cast<double>(f[13]),
                    static_cast<double>(f[14]), static_cast<double>(f[15]),
                    static_cast<double>(f[19]));
                return;
            }

            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return;
            float out[kCbFloats];
            memcpy(out, f, sizeof(out));
            out[0] = f[0] * g_size;
            out[1] = f[1] * g_size;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kCbBytes;
            bd.Usage = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA sr{};
            sr.pSysMem = out;
            if (SUCCEEDED(dev->CreateBuffer(&bd, &sr, &s.ours)) && s.ours) {
                s.ready = true;
                Log::get().note(
                    "intro video size: read the movie panel's own constants -- "
                    "half-size %.0f x %.0f pixels, centred at %.4f in NDC. "
                    "Drawing it at %.0f x %.0f instead.",
                    static_cast<double>(f[0]), static_cast<double>(f[1]),
                    static_cast<double>(f[16]), static_cast<double>(out[0]),
                    static_cast<double>(out[1]));
            }
            dev->Release();
        });
        if (s.stage) { s.stage->Release(); s.stage = nullptr; }
    }
}

void introPanelShutdown() {
    for (Slot& s : g_slot) releaseSlot(s);
    g_slotCount = 0;
    g_restore = nullptr;
}

}  // namespace edvr
