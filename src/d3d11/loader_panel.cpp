#include "loader_panel.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"

namespace edvr {
namespace {

// Six indices to a quad at topology 4, which is what the census reports for
// this whole family.
constexpr uint32_t kIndicesPerQuad = 6;

// The bordered-panel widget: one fill plus four border strips. The scrim,
// the box and the loading screen's letterbox are all this one widget.
constexpr uint32_t kPanelIndices = 30;

// The widget system draws every panel in one normalized coordinate space
// (about +/-32765 across) and SIZES it with the VIEWPORT: flight 2
// (2026-08-28, 11:27) showed three 30-index panels whose vertices all span
// the full space while the screen shows one full-view scrim and one
// modal-sized box. Vertex bounds therefore mean nothing across draws; the
// viewport is the widget rect. A viewport at least this fraction of the
// surface is "full view"; at most the sub fraction in both axes is "a box".
constexpr float kVpFullFraction = 0.90f;
constexpr float kVpSubFraction = 0.80f;

// The panel fill's RGBA8 colour at byte offset 8 is the role's other half,
// also measured in flight: the scrim is black at alpha 0x66, the box black
// at 0xFF, the letterbox white. Dark means every channel below this; opaque
// means alpha at least this.
constexpr uint32_t kDarkMax = 0x40;
constexpr uint32_t kOpaqueMin = 0xF0;

// Engage lines after the fourth are logged only when the box moved; the
// shader-prep dialog re-measures on every percent tick and the box holds
// still through all of them.
constexpr float kRelogFraction = 0.01f;

// Frame-composition record: draw shapes per frame, and captured draws per
// measurement. The measured loader frame held about a dozen draws; these
// leave room without inviting a scan.
constexpr uint32_t kMaxSeq = 48;
constexpr uint32_t kMaxCaptures = 24;

// Scrim positions substituted per measurement. The evidence says one; a
// second translucent full-view panel would get the same treatment.
constexpr uint32_t kMaxScrims = 4;

// Index bytes one measurement will hold. The loader's draws totalled about
// 3,600 indices; 64 KB is far above that and still trivial.
constexpr uint32_t kIbStageBytes = 64u << 10;

// The shared vertex buffer measured 4 MB; cap the staging copy at twice that
// so a bigger rig still measures and a runaway size cannot ask for hundreds.
constexpr uint32_t kVbStageCap = 8u << 20;

// Frames to let the copies execute before mapping, so the map never stalls
// the render thread. panel_quad's number.
constexpr uint32_t kSettleFrames = 4;

// The box's rect is re-read at its own draw every frame; the scrim draws
// EARLIER in the frame, so it rides the rect from at most this many frames
// back. Staler than that means the box stopped drawing -- stock.
constexpr uint32_t kBoxFreshFrames = 2;

// Wanting a measurement this long without ever seeing two identical frames
// is worth one log line: it means the loader is animating continuously and
// the fix is standing down, correctly but invisibly.
constexpr uint32_t kStuckFrames = 600;

// Collection attempts abandoned because draws would not fit the capture
// before the same shape is recorded as unmeasurable. Guards against a
// measure-discard loop re-copying a 4 MB buffer every third frame.
constexpr uint32_t kMaxDropStreak = 3;

FaultBudget g_budget("loaderPanel", 6);

bool g_on = false;

struct Rect {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool valid() const { return x1 >= x0 && y1 >= y0; }
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
    float area() const { return valid() ? w() * h() : 0.0f; }
    void add(float x, float y) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    void add(const Rect& r) {
        if (!r.valid()) return;
        add(r.x0, r.y0);
        add(r.x1, r.y1);
    }
};

// One entry in a frame's composition: the shape of one draw into an
// interface-sized surface. Position in the sequence is the entry's identity.
struct SeqEnt {
    uint32_t count = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    bool same(uint32_t c, uint32_t tw, uint32_t th) const {
        return count == c && w == tw && h == th;
    }
};

// One draw collected into the pending measurement: its indices (the vertex
// buffer is copied once for all of them) and the rasterizer state that was
// live at the draw -- the viewport is where the widget rect actually lives.
struct CapDraw {
    uint32_t seqPos = 0;
    uint32_t count = 0;
    uint32_t ibOffset = 0;   // bytes into the index staging buffer
    int      baseVertex = 0;
    bool     i16 = false;    // this draw's own index format
    D3D11_VIEWPORT vp = {};
    bool     vpKnown = false;
    D3D11_RECT sc = {};
    bool     scKnown = false;
    bool     scEnabled = false;
};

// --- the current frame's composition -------------------------------------
uint32_t g_frame = 0;
uint32_t g_seqLen = 0;
SeqEnt   g_seq[kMaxSeq];
uint32_t g_hashAcc = 2166136261u;
bool     g_prefixOk = true;   // does this frame still match the measured one?

// The pending substitution, stashed by OnDraw for the Substitute call that
// immediately follows: the game's own draw arguments, re-issued through the
// box's viewport.
bool     g_subArm = false;
uint32_t g_subCount = 0;
uint32_t g_subStartIndex = 0;
int      g_subBaseVertex = 0;

// The last COMPLETED frame's hash; two consecutive equal hashes are the
// stability that arms a collection.
uint32_t g_liveHash = 0;

// --- the measurement lifecycle -------------------------------------------
bool     g_collecting = false;   // this frame's draws are being captured
uint32_t g_armedHash = 0;        // the stable shape the collection is of
uint32_t g_settleAt = 0;         // 0 = nothing pending
uint32_t g_wantSince = 0;
bool     g_stuckNoted = false;
uint32_t g_dropStreak = 0;

// --- the pending capture --------------------------------------------------
ID3D11Buffer* g_ibStage = nullptr;
ID3D11Buffer* g_vbStage = nullptr;
CapDraw       g_caps[kMaxCaptures];
uint32_t      g_capCount = 0;
uint32_t      g_capDropped = 0;  // qualifying draws that did not fit
uint32_t      g_ibFill = 0;
uint32_t      g_capStride = 0;
uint32_t      g_capVertexBytes = 0;
SeqEnt        g_capSeq[kMaxSeq]; // the collection frame's composition
uint32_t      g_capSeqLen = 0;

// --- the measured result: roles by position ------------------------------
uint32_t g_measuredHash = 0;     // shape this verdict belongs to; 0 = none
uint32_t g_measuredLen = 0;
SeqEnt   g_measuredSeq[kMaxSeq];
bool     g_haveRoles = false;
uint32_t g_scrimPos[kMaxScrims];
uint32_t g_scrimCount = 0;
uint32_t g_boxPos = 0;
bool     g_boxFromScissor = false;  // the box rect lives in its scissor,
                                    // not its viewport
uint32_t g_measurements = 0;

// --- the box's live rect, re-read at its draw every valid frame ----------
float    g_boxX = 0, g_boxY = 0, g_boxW = 0, g_boxH = 0;
float    g_boxMinZ = 0, g_boxMaxZ = 1;
uint32_t g_boxStamp = 0;   // g_frame when last read; 0 = never
Rect     g_lastBox;        // for the engage log's moved-or-not test

void failOnce(const char* why) {
    static bool noted = false;
    if (noted) return;
    noted = true;
    Log::get().note("loading panel: %s. The backdrop draws stock.", why);
}

void dropPending() {
    if (g_ibStage) { g_ibStage->Release(); g_ibStage = nullptr; }
    if (g_vbStage) { g_vbStage->Release(); g_vbStage = nullptr; }
    g_capCount = 0;
    g_capDropped = 0;
    g_ibFill = 0;
    g_capSeqLen = 0;
    g_settleAt = 0;
    g_collecting = false;
}

void resetRoles() {
    g_haveRoles = false;
    g_scrimCount = 0;
    g_boxStamp = 0;
}

void resetMeasured() {
    resetRoles();
    g_measuredHash = 0;
    g_measuredLen = 0;
}

void resetFrameAcc() {
    g_seqLen = 0;
    g_hashAcc = 2166136261u;
    g_prefixOk = true;
    g_subArm = false;
}

// A verdict for a shape that yielded nothing to substitute. Recording the
// hash is what stops the same shape being re-measured -- and re-copying a
// 4 MB buffer -- every stable window until the dialog changes.
void recordNone(const char* why) {
    resetRoles();
    g_measuredHash = g_armedHash;
    g_measuredLen = g_capSeqLen;
    memcpy(g_measuredSeq, g_capSeq, sizeof(SeqEnt) * g_capSeqLen);
    Log::get().note("loading panel: measured %u draw(s) and drew no "
                    "conclusion -- %s. Stock for this dialog state; the next "
                    "change re-measures.",
                    g_capCount, why);
}

}  // namespace

void loaderPanelConfigure(Config& cfg) {
    const bool was = g_on;
    const std::string m = cfg.getString("fix.loading_panel", "stock");
    if (m == "stock") {
        g_on = false;
    } else if (m == "fit") {
        g_on = true;
    } else {
        g_on = false;
        Log::get().note("loading_panel \"%s\" is not stock or fit; running "
                        "stock.", m.c_str());
    }
    if (was != g_on) {
        if (!g_on) {
            dropPending();
            resetMeasured();
        }
        Log::get().note(
            "loading panel: %s. The full-view scrim behind the loader's "
            "dialog is %s (docs/loading-panel-handoff.md).",
            g_on ? "FIT" : "stock",
            g_on ? "redrawn through the dialog box's own viewport, so it "
                   "sits exactly where the box sits; the box and its text "
                   "are untouched"
                 : "the game's own");
    }
}

bool loaderPanelWants() { return g_on; }

bool loaderPanelOnDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances, uint32_t startIndex, int baseVertex,
                       uint32_t targetW, uint32_t targetH, bool textured) {
    (void)instances;
    if (!g_on || !ctx || kind != 'X' || count == 0) return false;

    // This draw's place in the frame's composition. The hash folds shape AND
    // target size, so a render-scale change reads as a new composition.
    const uint32_t p = g_seqLen;
    g_hashAcc = (g_hashAcc ^ count) * 16777619u;
    g_hashAcc = (g_hashAcc ^ targetW) * 16777619u;
    g_hashAcc = (g_hashAcc ^ targetH) * 16777619u;
    if (p < kMaxSeq) {
        g_seq[p].count = count;
        g_seq[p].w = targetW;
        g_seq[p].h = targetH;
        ++g_seqLen;
    } else {
        // A frame too busy to record cannot be matched against; no
        // substitution past this point.
        g_prefixOk = false;
    }

    // Substitution is positional, and a position only means anything while
    // the frame has matched the measured sequence at every step so far.
    if (g_measuredLen) {
        if (p >= g_measuredLen || !g_measuredSeq[p].same(count, targetW, targetH)) {
            g_prefixOk = false;
        }
    }

    // Collection: capture this draw if the frame is the armed one and the
    // draw is a solid quad batch -- text reads a texture and is content by
    // definition; the panels this module cares about read none. A qualifying
    // draw that cannot be captured -- capacity, an overlong frame -- poisons
    // the collection: a verdict from a subset could miss the box.
    if (g_collecting) {
        const bool qualifies = !textured && count % kIndicesPerQuad == 0;
        if (qualifies && p < kMaxSeq && g_capCount < kMaxCaptures) {
            bool stored = false;
            guardedBudget(g_budget, [&] {
                ID3D11Buffer* ib = nullptr;
                DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
                UINT ibOff = 0;
                ctx->IAGetIndexBuffer(&ib, &ibFmt, &ibOff);
                ID3D11Buffer* vb = nullptr;
                UINT stride = 0, vbOff = 0;
                ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &vbOff);
                if (!ib || !vb || stride == 0) {
                    if (ib) ib->Release();
                    if (vb) vb->Release();
                    return;
                }
                const UINT idxSize =
                    (ibFmt == DXGI_FORMAT_R16_UINT) ? 2u : 4u;
                const UINT need = count * idxSize;
                ID3D11Device* dev = nullptr;
                ctx->GetDevice(&dev);
                if (dev && g_ibFill + need <= kIbStageBytes) {
                    bool ok = true;
                    if (!g_ibStage) {
                        D3D11_BUFFER_DESC sd{};
                        sd.Usage = D3D11_USAGE_STAGING;
                        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        sd.ByteWidth = kIbStageBytes;
                        ok = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_ibStage));
                    }
                    if (ok && !g_vbStage) {
                        D3D11_BUFFER_DESC vd{};
                        vb->GetDesc(&vd);
                        D3D11_BUFFER_DESC sd{};
                        sd.Usage = D3D11_USAGE_STAGING;
                        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        sd.ByteWidth = vd.ByteWidth > kVbStageCap ? kVbStageCap
                                                                  : vd.ByteWidth;
                        ok = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_vbStage));
                        if (ok) {
                            // The whole buffer, once. Every draw this frame
                            // indexes into it, and the game appends with
                            // no-overwrite maps, so a copy queued at the
                            // frame's first solid sees the frame's writes by
                            // the time the GPU executes it.
                            D3D11_BOX all{};
                            all.right = sd.ByteWidth;
                            all.bottom = 1; all.back = 1;
                            ctx->CopySubresourceRegion(g_vbStage, 0, 0, 0, 0,
                                                       vb, 0, &all);
                            g_capVertexBytes = sd.ByteWidth;
                            g_capStride = stride;
                        }
                    }
                    if (ok && g_ibStage && g_vbStage) {
                        D3D11_BOX box{};
                        box.left = ibOff + startIndex * idxSize;
                        box.right = box.left + need;
                        box.bottom = 1; box.back = 1;
                        ctx->CopySubresourceRegion(g_ibStage, 0, g_ibFill, 0, 0,
                                                   ib, 0, &box);
                        CapDraw& cd = g_caps[g_capCount];
                        cd = CapDraw{};
                        cd.seqPos = p;
                        cd.count = count;
                        cd.ibOffset = g_ibFill;
                        cd.baseVertex = baseVertex;
                        cd.i16 = idxSize == 2u;
                        // The rasterizer state, where the widget rect lives.
                        UINT nv = 1;
                        ctx->RSGetViewports(&nv, &cd.vp);
                        cd.vpKnown = nv >= 1;
                        UINT ns = 1;
                        ctx->RSGetScissorRects(&ns, &cd.sc);
                        cd.scKnown = ns >= 1;
                        ID3D11RasterizerState* rs = nullptr;
                        ctx->RSGetState(&rs);
                        if (rs) {
                            D3D11_RASTERIZER_DESC rd;
                            rs->GetDesc(&rd);
                            cd.scEnabled = rd.ScissorEnable != FALSE;
                            rs->Release();
                        }
                        ++g_capCount;
                        g_ibFill += need;
                        stored = true;
                    }
                }
                if (dev) dev->Release();
                ib->Release();
                vb->Release();
            });
            if (!stored) ++g_capDropped;
        } else if (qualifies) {
            ++g_capDropped;
        }
    }

    // The roles, played by position while the frame matches the measurement.
    g_subArm = false;
    if (g_haveRoles && g_prefixOk) {
        if (p == g_boxPos) {
            // Re-read the box's rect at its own draw, every frame: the
            // modal never has to be assumed static.
            bool got = false;
            if (g_boxFromScissor) {
                UINT ns = 1;
                D3D11_RECT rc;
                ctx->RSGetScissorRects(&ns, &rc);
                if (ns >= 1 && rc.right > rc.left && rc.bottom > rc.top) {
                    g_boxX = static_cast<float>(rc.left);
                    g_boxY = static_cast<float>(rc.top);
                    g_boxW = static_cast<float>(rc.right - rc.left);
                    g_boxH = static_cast<float>(rc.bottom - rc.top);
                    got = true;
                }
            } else {
                UINT nv = 1;
                D3D11_VIEWPORT vp;
                ctx->RSGetViewports(&nv, &vp);
                if (nv >= 1 && vp.Width > 0 && vp.Height > 0) {
                    g_boxX = vp.TopLeftX;
                    g_boxY = vp.TopLeftY;
                    g_boxW = vp.Width;
                    g_boxH = vp.Height;
                    g_boxMinZ = vp.MinDepth;
                    g_boxMaxZ = vp.MaxDepth;
                    got = true;
                }
            }
            if (got) g_boxStamp = g_frame;
        } else if (g_boxStamp &&
                   g_frame - g_boxStamp <= kBoxFreshFrames) {
            for (uint32_t i = 0; i < g_scrimCount; ++i) {
                if (g_scrimPos[i] == p) {
                    g_subArm = true;
                    g_subCount = count;
                    g_subStartIndex = startIndex;
                    g_subBaseVertex = baseVertex;
                    return true;
                }
            }
        }
    }
    return false;
}

bool loaderPanelSubstitute(ID3D11DeviceContext* ctx, PfnDrawIndexedInstanced draw,
                           uint32_t instances, uint32_t startInstance) {
    if (!ctx || !draw || !g_subArm) return false;
    g_subArm = false;
    bool done = false;
    guardedBudget(g_budget, [&] {
        // The game's own draw, re-issued through the box's viewport: the
        // widget system sizes panels with the viewport, so this renders the
        // scrim exactly into the box's rect -- geometry, colours and blend
        // untouched, no buffers of ours anywhere.
        UINT nv = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT saved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ctx->RSGetViewports(&nv, saved);
        if (nv == 0) return;   // no viewport state to restore into: stock
        D3D11_VIEWPORT vp;
        vp.TopLeftX = g_boxX;
        vp.TopLeftY = g_boxY;
        vp.Width = g_boxW;
        vp.Height = g_boxH;
        vp.MinDepth = g_boxFromScissor ? saved[0].MinDepth : g_boxMinZ;
        vp.MaxDepth = g_boxFromScissor ? saved[0].MaxDepth : g_boxMaxZ;
        ctx->RSSetViewports(1, &vp);
        draw(ctx, g_subCount, instances, g_subStartIndex, g_subBaseVertex,
             startInstance);
        ctx->RSSetViewports(nv, saved);
        done = true;
    });
    return done;
}

namespace {

// Retire a settled capture into roles: which position is the translucent
// full-view scrim, which is the opaque boxed panel it should ride. Runs on
// the render thread inside the caller's budget guard; every exit that is
// not a verdict records the shape so it is not re-measured.
void analyze(ID3D11DeviceContext* ctx) {
    D3D11_MAPPED_SUBRESOURCE mi{}, mv{};
    if (FAILED(ctx->Map(g_ibStage, 0, D3D11_MAP_READ, 0, &mi)) || !mi.pData ||
        FAILED(ctx->Map(g_vbStage, 0, D3D11_MAP_READ, 0, &mv)) || !mv.pData) {
        if (mi.pData) ctx->Unmap(g_ibStage, 0);
        failOnce("the measurement could not be mapped");
        return;
    }
    const uint8_t* ibBase = static_cast<const uint8_t*>(mi.pData);
    const uint8_t* vbBase = static_cast<const uint8_t*>(mv.pData);

    // Per captured draw: quad bounds (in the draw's OWN normalized space --
    // flight 2 proved these spaces are per-viewport and must never be
    // compared across draws), and the fill's RGBA8 at byte offset 8.
    std::vector<Rect> whole(g_capCount);
    std::vector<uint32_t> rgba(g_capCount, 0);
    std::vector<bool> rgbaKnown(g_capCount, false);
    for (uint32_t d = 0; d < g_capCount; ++d) {
        const CapDraw& cd = g_caps[d];
        const uint32_t n = cd.count / kIndicesPerQuad;
        Rect fill;
        float fillArea = -1.0f;
        int64_t fillVertOff = -1;
        for (uint32_t q = 0; q < n; ++q) {
            Rect r;
            int64_t firstOff = -1;
            for (uint32_t k = 0; k < kIndicesPerQuad; ++k) {
                const uint32_t at = q * kIndicesPerQuad + k;
                const uint8_t* ip = ibBase + cd.ibOffset + at * (cd.i16 ? 2 : 4);
                uint32_t vi = cd.i16 ? *reinterpret_cast<const uint16_t*>(ip)
                                     : *reinterpret_cast<const uint32_t*>(ip);
                const int64_t v = static_cast<int64_t>(vi) + cd.baseVertex;
                const int64_t off = v * g_capStride;
                if (v < 0 ||
                    off + g_capStride > static_cast<int64_t>(g_capVertexBytes)) {
                    continue;
                }
                if (firstOff < 0) firstOff = off;
                float pos[2];
                memcpy(pos, vbBase + off, sizeof(pos));
                r.add(pos[0], pos[1]);
            }
            if (r.valid()) {
                whole[d].add(r);
                if (r.area() > fillArea) {
                    fillArea = r.area();
                    fill = r;
                    fillVertOff = firstOff;
                }
            }
        }
        if (fillVertOff >= 0 && g_capStride >= 12) {
            uint32_t c;
            memcpy(&c, vbBase + fillVertOff + 8, sizeof(c));
            rgba[d] = c;
            rgbaKnown[d] = true;
        }
    }

    // When a verdict refuses, name every solid it saw -- extent in its own
    // space, colour, and the rasterizer rects that actually place it.
    auto dumpSolids = [&] {
        for (uint32_t d = 0; d < g_capCount && d < 10; ++d) {
            const CapDraw& cd = g_caps[d];
            const Rect& w = whole[d];
            char vps[64] = "vp ?";
            if (cd.vpKnown) {
                snprintf(vps, sizeof(vps), "vp %.0f,%.0f %.0fx%.0f",
                         cd.vp.TopLeftX, cd.vp.TopLeftY, cd.vp.Width,
                         cd.vp.Height);
            }
            char scs[80] = "";
            if (cd.scKnown) {
                snprintf(scs, sizeof(scs), ", scissor %ld,%ld-%ld,%ld %s",
                         cd.sc.left, cd.sc.top, cd.sc.right, cd.sc.bottom,
                         cd.scEnabled ? "on" : "off");
            }
            Log::get().note("  solid %u: %u indices at draw %u, "
                            "%.0fx%.0f own-units, rgba %08X, %s%s",
                            d, cd.count, cd.seqPos,
                            w.valid() ? w.w() : 0.0f,
                            w.valid() ? w.h() : 0.0f,
                            rgbaKnown[d] ? rgba[d] : 0u, vps, scs);
        }
    };

    // Roles. The scrim: a 30-index panel, dark and translucent, whose
    // viewport is the whole surface. The box: dark and opaque, drawn
    // through a viewport (or an enabled scissor) that is a fraction of it.
    g_scrimCount = 0;
    int scrimCap0 = -1;
    int boxCap = -1;
    bool boxFromScissor = false;
    float boxArea = 0.0f;
    for (uint32_t d = 0; d < g_capCount; ++d) {
        const CapDraw& cd = g_caps[d];
        if (cd.count != kPanelIndices || !rgbaKnown[d] || !cd.vpKnown) {
            continue;
        }
        const SeqEnt& se = g_capSeq[cd.seqPos];
        const float sw = static_cast<float>(se.w);
        const float sh = static_cast<float>(se.h);
        const uint32_t c = rgba[d];
        const uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
        const uint32_t a = (c >> 24) & 0xFF;
        const bool dark = r < kDarkMax && g < kDarkMax && b < kDarkMax;
        if (!dark) continue;
        const bool vpFull = cd.vp.Width >= sw * kVpFullFraction &&
                            cd.vp.Height >= sh * kVpFullFraction;
        const bool vpSub = cd.vp.Width <= sw * kVpSubFraction &&
                           cd.vp.Height <= sh * kVpSubFraction &&
                           cd.vp.Width > 0 && cd.vp.Height > 0;
        const bool scSub =
            cd.scEnabled && cd.scKnown &&
            static_cast<float>(cd.sc.right - cd.sc.left) <= sw * kVpSubFraction &&
            static_cast<float>(cd.sc.bottom - cd.sc.top) <= sh * kVpSubFraction &&
            cd.sc.right > cd.sc.left && cd.sc.bottom > cd.sc.top;
        if (a < kOpaqueMin && vpFull) {
            if (g_scrimCount < kMaxScrims) {
                if (scrimCap0 < 0) scrimCap0 = static_cast<int>(d);
                g_scrimPos[g_scrimCount++] = cd.seqPos;
            }
        } else if (a >= kOpaqueMin && (vpSub || (vpFull && scSub))) {
            const float area = vpSub
                ? cd.vp.Width * cd.vp.Height
                : static_cast<float>(cd.sc.right - cd.sc.left) *
                      static_cast<float>(cd.sc.bottom - cd.sc.top);
            if (area > boxArea) {
                boxArea = area;
                boxCap = static_cast<int>(d);
                boxFromScissor = !vpSub;
            }
        }
    }

    ctx->Unmap(g_vbStage, 0);
    ctx->Unmap(g_ibStage, 0);

    if (g_scrimCount == 0) {
        dumpSolids();
        recordNone("no dark translucent full-view 30-index panel is in this "
                   "frame");
        return;
    }
    if (boxCap < 0) {
        dumpSolids();
        recordNone("no dark opaque 30-index panel rides a boxed viewport or "
                   "scissor -- the modal's rect is not where this looks");
        return;
    }

    g_boxPos = g_caps[boxCap].seqPos;
    g_boxFromScissor = boxFromScissor;
    g_boxStamp = 0;   // the rect is read live at the box's next draw
    g_haveRoles = true;
    g_measuredHash = g_armedHash;
    g_measuredLen = g_capSeqLen;
    memcpy(g_measuredSeq, g_capSeq, sizeof(SeqEnt) * g_capSeqLen);
    ++g_measurements;

    // The engage line, rate-limited: after the first few, log again only
    // when the box moved -- the percent text re-measures constantly and the
    // box holds still through it.
    const CapDraw& bc = g_caps[boxCap];
    Rect boxNow;
    if (boxFromScissor) {
        boxNow.add(static_cast<float>(bc.sc.left), static_cast<float>(bc.sc.top));
        boxNow.add(static_cast<float>(bc.sc.right), static_cast<float>(bc.sc.bottom));
    } else {
        boxNow.add(bc.vp.TopLeftX, bc.vp.TopLeftY);
        boxNow.add(bc.vp.TopLeftX + bc.vp.Width, bc.vp.TopLeftY + bc.vp.Height);
    }
    const float sw = static_cast<float>(g_capSeq[bc.seqPos].w);
    const float sh = static_cast<float>(g_capSeq[bc.seqPos].h);
    const bool moved =
        fabsf(boxNow.x0 - g_lastBox.x0) > sw * kRelogFraction ||
        fabsf(boxNow.x1 - g_lastBox.x1) > sw * kRelogFraction ||
        fabsf(boxNow.y0 - g_lastBox.y0) > sh * kRelogFraction ||
        fabsf(boxNow.y1 - g_lastBox.y1) > sh * kRelogFraction;
    g_lastBox = boxNow;
    if (g_measurements <= 4 || moved) {
        Log::get().note(
            "loading panel: FIT -- measurement %u from %u solids of %u "
            "interface draws. The scrim at draw %u (rgba %08X) is redrawn "
            "through the box's %s; the box at draw %u (rgba %08X) sits at "
            "%.0f,%.0f %.0fx%.0f px of %.0fx%.0f. The box, its border and "
            "its text are the game's own draws, untouched.%s",
            g_measurements, g_capCount, g_capSeqLen,
            g_scrimPos[0], scrimCap0 >= 0 ? rgba[scrimCap0] : 0u,
            g_boxFromScissor ? "scissor rect" : "viewport",
            g_boxPos, rgba[boxCap],
            boxNow.x0, boxNow.y0, boxNow.w(), boxNow.h(), sw, sh,
            g_scrimCount > 1 ? " More than one scrim was found; each rides "
                               "the same box." : "");
    }
}

}  // namespace

void loaderPanelTick(ID3D11DeviceContext* ctx) {
    ++g_frame;
    if (!g_on) {
        if (g_ibStage || g_vbStage || g_collecting) dropPending();
        resetFrameAcc();
        g_liveHash = 0;
        return;
    }

    const uint32_t finishedHash = g_seqLen ? g_hashAcc : 0;

    // The collection frame just closed: keep it only if the composition it
    // captured is the one that was armed. A mismatch means the loader moved
    // mid-collection -- fade-in, progress re-tessellation -- and the capture
    // describes no stable state.
    if (g_collecting) {
        g_collecting = false;
        if (finishedHash == g_armedHash && g_capCount > 0 &&
            g_capDropped == 0) {
            memcpy(g_capSeq, g_seq, sizeof(SeqEnt) * g_seqLen);
            g_capSeqLen = g_seqLen;
            g_settleAt = g_frame + kSettleFrames;
            g_dropStreak = 0;
        } else {
            const bool dropped = g_capDropped != 0;
            dropPending();
            if (dropped && ++g_dropStreak >= kMaxDropStreak) {
                g_armedHash = finishedHash;
                recordNone("its draws would not fit the capture three times "
                           "running");
                g_dropStreak = 0;
            }
        }
    }

    // A settled capture is ready to read.
    if (g_settleAt && g_frame >= g_settleAt && ctx) {
        g_settleAt = 0;
        guardedBudget(g_budget, [&] { analyze(ctx); });
        dropPending();
    }

    // Want a measurement? Arm one only off the back of two identical
    // consecutive frames, so the capture describes a state the next frames
    // will still be in.
    const bool want = g_seqLen > 0 && finishedHash != g_measuredHash;
    if (want && !g_collecting && !g_settleAt) {
        if (finishedHash == g_liveHash) {
            dropPending();
            g_collecting = true;
            g_armedHash = finishedHash;
            g_wantSince = 0;
        } else if (!g_wantSince) {
            g_wantSince = g_frame;
        } else if (!g_stuckNoted && g_frame - g_wantSince > kStuckFrames) {
            g_stuckNoted = true;
            Log::get().note(
                "loading panel: the loader's draws have not held still for "
                "two consecutive frames in %u frames; the backdrop draws "
                "stock until they do.", kStuckFrames);
        }
    } else if (!want) {
        g_wantSince = 0;
    }

    g_liveHash = finishedHash;
    resetFrameAcc();
}

void loaderPanelShutdown() {
    dropPending();
    resetMeasured();
}

}  // namespace edvr
