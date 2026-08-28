#include "loader_panel.h"

#include <windows.h>

#include <d3d11.h>

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
// the invisible full-view sheet and the letterbox are all this one widget.
constexpr uint32_t kPanelIndices = 30;

// HOW THE WIDGET IS PLACED, from vs 666EF0C4C616F67E's own disassembly
// (docs/shaders/ui-widget-vs.asm): a per-element 4x4 matrix in a structured
// buffer at VS t0, stride 160, selected by a byte carried in the vertex --
// offset 12 or 16 of the 24-byte vertex, chosen by flag bits 0x4000/0x8000
// in VS cb2[2].x. The scrim's identity rests on this: its element must map
// its fill to the full view, read through the same table the shader reads.
constexpr uint32_t kElemStride = 160;
constexpr uint32_t kIdx1Off = 12;
constexpr uint32_t kIdx2Off = 16;
constexpr uint32_t kFlagIdx2 = 0x8000;
constexpr uint32_t kFlagIdx1 = 0x4000;

// The scrim's fill colour, RGBA8 at vertex byte offset 8, measured across
// seven flights: black at alpha 0x66 -- dark in every channel below the
// dark bound, translucent below the opaque bound. The other panels fail
// one or the other: the full-view sheet is opaque, the letterbox white.
constexpr uint32_t kDarkMax = 0x40;
constexpr uint32_t kOpaqueMin = 0xF0;

// A matrix that maps a panel's fill across at least this fraction of clip
// space (which spans 2.0) is a full-view element.
constexpr float kFullFraction = 0.90f;

// Frame-composition record: draw shapes per frame, and captured draws per
// measurement. The measured loader frame held about a dozen draws; these
// leave room without inviting a scan.
constexpr uint32_t kMaxSeq = 48;
constexpr uint32_t kMaxCaptures = 24;

// Scrim positions withheld per measurement. The evidence says one; a
// second translucent full-view panel would get the same treatment.
constexpr uint32_t kMaxScrims = 4;

// Index bytes one measurement will hold. The loader's draws totalled about
// 3,600 indices; 64 KB is far above that and still trivial.
constexpr uint32_t kIbStageBytes = 64u << 10;

// The shared vertex buffer measured 4 MB; cap the staging copy at twice that
// so a bigger rig still measures and a runaway size cannot ask for hundreds.
constexpr uint32_t kVbStageCap = 8u << 20;

// The widget table's staging window: 64 KB is 409 elements at stride 160,
// far beyond a loading screen's element count. An index past the window
// refuses rather than reads garbage.
constexpr uint32_t kSrvStageBytes = 64u << 10;

// cb2 as the shader declares it: three float4s.
constexpr uint32_t kCb2Bytes = 48;

// Frames to let the copies execute before mapping, so the map never stalls
// the render thread. panel_quad's number.
constexpr uint32_t kSettleFrames = 4;

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

// One draw collected into the pending measurement.
struct CapDraw {
    uint32_t seqPos = 0;
    uint32_t count = 0;
    uint32_t ibOffset = 0;   // bytes into the index staging buffer
    int      baseVertex = 0;
    bool     i16 = false;    // this draw's own index format
    void*    vsSrv = nullptr;   // identity of VS t0 at the draw, not held
};

// --- the current frame's composition -------------------------------------
uint32_t g_frame = 0;
uint32_t g_seqLen = 0;
SeqEnt   g_seq[kMaxSeq];
uint32_t g_hashAcc = 2166136261u;
bool     g_prefixOk = true;   // does this frame still match the measured one?
bool     g_subArm = false;    // set by OnDraw for the Substitute that follows

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
ID3D11Buffer* g_srvStage = nullptr;   // the widget table (VS t0)
ID3D11Buffer* g_cb2Stage = nullptr;   // the flag constants (VS b2)
CapDraw       g_caps[kMaxCaptures];
uint32_t      g_capCount = 0;
uint32_t      g_capDropped = 0;  // qualifying draws that did not fit
uint32_t      g_ibFill = 0;
uint32_t      g_capStride = 0;
uint32_t      g_capVertexBytes = 0;
uint32_t      g_srvCopied = 0;       // bytes of the table in staging
uint32_t      g_srvFirstElem = 0;    // the view's element offset
bool          g_cb2Copied = false;
SeqEnt        g_capSeq[kMaxSeq]; // the collection frame's composition
uint32_t      g_capSeqLen = 0;

// --- the measured result: scrim positions to withhold ---------------------
uint32_t g_measuredHash = 0;     // shape this verdict belongs to; 0 = none
uint32_t g_measuredLen = 0;
SeqEnt   g_measuredSeq[kMaxSeq];
bool     g_haveRoles = false;
uint32_t g_scrimPos[kMaxScrims];
uint32_t g_scrimCount = 0;
uint32_t g_measurements = 0;
uint32_t g_lastScrimPos = 0xFFFFFFFFu;   // engage-log rate limiting
uint32_t g_lastScrimCount = 0;

void failOnce(const char* why) {
    static bool noted = false;
    if (noted) return;
    noted = true;
    Log::get().note("loading panel: %s. The scrim draws stock.", why);
}

void dropPending() {
    if (g_ibStage) { g_ibStage->Release(); g_ibStage = nullptr; }
    if (g_vbStage) { g_vbStage->Release(); g_vbStage = nullptr; }
    if (g_srvStage) { g_srvStage->Release(); g_srvStage = nullptr; }
    if (g_cb2Stage) { g_cb2Stage->Release(); g_cb2Stage = nullptr; }
    g_capCount = 0;
    g_capDropped = 0;
    g_ibFill = 0;
    g_srvCopied = 0;
    g_srvFirstElem = 0;
    g_cb2Copied = false;
    g_capSeqLen = 0;
    g_settleAt = 0;
    g_collecting = false;
}

void resetMeasured() {
    g_haveRoles = false;
    g_scrimCount = 0;
    g_measuredHash = 0;
    g_measuredLen = 0;
}

void resetFrameAcc() {
    g_seqLen = 0;
    g_hashAcc = 2166136261u;
    g_prefixOk = true;
    g_subArm = false;
}

// A verdict for a shape that yielded nothing to withhold. Recording the
// hash is what stops the same shape being re-measured -- and re-copying a
// 4 MB buffer -- every stable window until the dialog changes.
void recordNone(const char* why) {
    g_haveRoles = false;
    g_scrimCount = 0;
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
            g_on ? "withheld -- the dialog's own black backing, an eye-level "
                   "layer, already carries the box, so no tint reaches "
                   "anything beyond it"
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

    // Withholding is positional, and a position only means anything while
    // the frame has matched the measured sequence at every step so far.
    if (g_measuredLen) {
        if (p >= g_measuredLen || !g_measuredSeq[p].same(count, targetW, targetH)) {
            g_prefixOk = false;
        }
    }

    // Collection: capture this draw if the frame is the armed one and the
    // draw is a solid quad batch -- text reads a texture; the panels this
    // module classifies read none. A qualifying draw that cannot be
    // captured -- capacity, an overlong frame -- poisons the collection.
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
                        // The widget table and flags, at the first panel of
                        // the frame -- the same GPU-timeline copy discipline
                        // as the vertex buffer. Later panels record only the
                        // view's identity, so a mixed-table frame can refuse.
                        if (count == kPanelIndices) {
                            ID3D11ShaderResourceView* srv = nullptr;
                            ctx->VSGetShaderResources(0, 1, &srv);
                            if (srv) {
                                cd.vsSrv = srv;
                                if (!g_srvStage) {
                                    ID3D11Resource* res = nullptr;
                                    srv->GetResource(&res);
                                    if (res) {
                                        D3D11_RESOURCE_DIMENSION dim;
                                        res->GetType(&dim);
                                        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
                                            ID3D11Buffer* tbl =
                                                static_cast<ID3D11Buffer*>(res);
                                            D3D11_BUFFER_DESC td{};
                                            tbl->GetDesc(&td);
                                            D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
                                            srv->GetDesc(&svd);
                                            if (svd.ViewDimension ==
                                                D3D11_SRV_DIMENSION_BUFFEREX) {
                                                g_srvFirstElem =
                                                    svd.BufferEx.FirstElement;
                                            } else if (svd.ViewDimension ==
                                                       D3D11_SRV_DIMENSION_BUFFER) {
                                                g_srvFirstElem =
                                                    svd.Buffer.FirstElement;
                                            }
                                            D3D11_BUFFER_DESC sd{};
                                            sd.Usage = D3D11_USAGE_STAGING;
                                            sd.CPUAccessFlags =
                                                D3D11_CPU_ACCESS_READ;
                                            sd.ByteWidth =
                                                td.ByteWidth > kSrvStageBytes
                                                    ? kSrvStageBytes
                                                    : td.ByteWidth;
                                            if (SUCCEEDED(dev->CreateBuffer(
                                                    &sd, nullptr, &g_srvStage))) {
                                                D3D11_BOX tb{};
                                                tb.right = sd.ByteWidth;
                                                tb.bottom = 1; tb.back = 1;
                                                ctx->CopySubresourceRegion(
                                                    g_srvStage, 0, 0, 0, 0,
                                                    tbl, 0, &tb);
                                                g_srvCopied = sd.ByteWidth;
                                            }
                                        }
                                        res->Release();
                                    }
                                }
                                srv->Release();
                            }
                            if (!g_cb2Stage) {
                                ID3D11Buffer* cb = nullptr;
                                ctx->VSGetConstantBuffers(2, 1, &cb);
                                if (cb) {
                                    D3D11_BUFFER_DESC sd{};
                                    sd.Usage = D3D11_USAGE_STAGING;
                                    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                                    sd.ByteWidth = kCb2Bytes;
                                    if (SUCCEEDED(dev->CreateBuffer(
                                            &sd, nullptr, &g_cb2Stage))) {
                                        D3D11_BOX cbb{};
                                        cbb.right = kCb2Bytes;
                                        cbb.bottom = 1; cbb.back = 1;
                                        ctx->CopySubresourceRegion(
                                            g_cb2Stage, 0, 0, 0, 0, cb, 0,
                                            &cbb);
                                        g_cb2Copied = true;
                                    }
                                    cb->Release();
                                }
                            }
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

    // The withhold decision. Only a position the measurement marked as the
    // scrim, and only while this frame still matches the measured one.
    g_subArm = false;
    if (g_haveRoles && g_prefixOk) {
        for (uint32_t i = 0; i < g_scrimCount; ++i) {
            if (g_scrimPos[i] == p) {
                g_subArm = true;
                return true;
            }
        }
    }
    return false;
}

bool loaderPanelSubstitute(ID3D11DeviceContext* ctx, PfnDrawIndexedInstanced draw,
                           uint32_t instances, uint32_t startInstance) {
    // The substitute for the scrim is NOTHING: the dialog's black backing is
    // an eye-level layer the interface surface never held, so inside the box
    // the scrim was invisible and outside it it was the defect. Withholding
    // the draw is pixel-identical to a perfect collapse onto an opaque box.
    (void)ctx;
    (void)draw;
    (void)instances;
    (void)startInstance;
    if (!g_subArm) return false;
    g_subArm = false;
    return true;
}

namespace {

// The clip-space footprint of one panel's fill under one element matrix:
// evaluate x' = r0.x*x + r0.y*y + r0.w and y' likewise at the fill's four
// corners (z is 0 and w is 1 in this vertex layout).
struct Foot {
    float cx0, cx1, cy0, cy1;
    bool valid = false;
};

Foot footprint(const float* elem, const Rect& fill) {
    Foot f{};
    if (!fill.valid()) return f;
    const float xs[2] = {fill.x0, fill.x1};
    const float ys[2] = {fill.y0, fill.y1};
    float cx0 = 1e30f, cx1 = -1e30f, cy0 = 1e30f, cy1 = -1e30f;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            const float cx = elem[0] * xs[i] + elem[1] * ys[j] + elem[3];
            const float cy = elem[4] * xs[i] + elem[5] * ys[j] + elem[7];
            if (cx < cx0) cx0 = cx;
            if (cx > cx1) cx1 = cx;
            if (cy < cy0) cy0 = cy;
            if (cy > cy1) cy1 = cy;
        }
    }
    f.cx0 = cx0; f.cx1 = cx1; f.cy0 = cy0; f.cy1 = cy1;
    f.valid = true;
    return f;
}

// Retire a settled capture into a verdict: which positions are the scrim.
// Runs on the render thread inside the caller's budget guard; every exit
// records the shape so it is not re-measured.
void analyze(ID3D11DeviceContext* ctx) {
    if (!g_srvStage || !g_srvCopied) {
        recordNone("no widget table was bound at the panels' draws");
        return;
    }
    if (!g_cb2Stage || !g_cb2Copied) {
        recordNone("the panels' flag constants could not be captured");
        return;
    }
    D3D11_MAPPED_SUBRESOURCE mi{}, mv{}, ms{}, mc{};
    if (FAILED(ctx->Map(g_ibStage, 0, D3D11_MAP_READ, 0, &mi)) || !mi.pData ||
        FAILED(ctx->Map(g_vbStage, 0, D3D11_MAP_READ, 0, &mv)) || !mv.pData ||
        FAILED(ctx->Map(g_srvStage, 0, D3D11_MAP_READ, 0, &ms)) || !ms.pData ||
        FAILED(ctx->Map(g_cb2Stage, 0, D3D11_MAP_READ, 0, &mc)) || !mc.pData) {
        if (mi.pData) ctx->Unmap(g_ibStage, 0);
        if (mv.pData) ctx->Unmap(g_vbStage, 0);
        if (ms.pData) ctx->Unmap(g_srvStage, 0);
        failOnce("the measurement could not be mapped");
        return;
    }
    const uint8_t* ibBase = static_cast<const uint8_t*>(mi.pData);
    const uint8_t* vbBase = static_cast<const uint8_t*>(mv.pData);
    const uint8_t* tbl = static_cast<const uint8_t*>(ms.pData);
    uint32_t flags = 0;
    memcpy(&flags, static_cast<const uint8_t*>(mc.pData) + 32, 4);

    auto unmapAll = [&] {
        ctx->Unmap(g_cb2Stage, 0);
        ctx->Unmap(g_srvStage, 0);
        ctx->Unmap(g_vbStage, 0);
        ctx->Unmap(g_ibStage, 0);
    };

    // Per captured panel: the fill quad's bounds, the RGBA8 at offset 8,
    // and the element-index bytes at offsets 12 and 16.
    std::vector<Rect> fill(g_capCount);
    std::vector<uint32_t> rgba(g_capCount, 0);
    std::vector<uint8_t> idx1(g_capCount, 0), idx2(g_capCount, 0);
    std::vector<bool> known(g_capCount, false);
    for (uint32_t d = 0; d < g_capCount; ++d) {
        const CapDraw& cd = g_caps[d];
        if (cd.count != kPanelIndices) continue;
        const uint32_t n = cd.count / kIndicesPerQuad;
        float bestArea = -1.0f;
        int64_t bestOff = -1;
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
            if (r.valid() && r.area() > bestArea) {
                bestArea = r.area();
                fill[d] = r;
                bestOff = firstOff;
            }
        }
        if (bestOff >= 0 && g_capStride >= 20) {
            uint32_t c;
            memcpy(&c, vbBase + bestOff + 8, sizeof(c));
            rgba[d] = c;
            idx1[d] = vbBase[bestOff + kIdx1Off];
            idx2[d] = vbBase[bestOff + kIdx2Off];
            known[d] = true;
        }
    }

    // The live element index, exactly as the shader selects it.
    auto liveIdx = [&](uint32_t d) -> uint32_t {
        if (flags & kFlagIdx2) return idx2[d];
        if (flags & kFlagIdx1) return idx1[d];
        return 0;
    };
    auto elemRows = [&](uint32_t index, float* out8) -> bool {
        const uint64_t off =
            static_cast<uint64_t>(g_srvFirstElem + index) * kElemStride;
        if (off + 32 > g_srvCopied) return false;
        memcpy(out8, tbl + off, 32);
        return true;
    };

    // When a verdict refuses, name every panel it saw.
    auto dumpPanels = [&] {
        for (uint32_t d = 0; d < g_capCount && d < 10; ++d) {
            const CapDraw& cd = g_caps[d];
            if (cd.count != kPanelIndices || !known[d]) continue;
            Log::get().note("  panel at draw %u, rgba %08X, element %u "
                            "(bytes %u/%u)",
                            cd.seqPos, rgba[d], liveIdx(d), idx1[d], idx2[d]);
        }
    };

    // Sanity: every panel must read the same widget table.
    void* srv0 = nullptr;
    bool mixed = false;
    for (uint32_t d = 0; d < g_capCount; ++d) {
        if (g_caps[d].count != kPanelIndices || !g_caps[d].vsSrv) continue;
        if (!srv0) srv0 = g_caps[d].vsSrv;
        else if (g_caps[d].vsSrv != srv0) mixed = true;
    }
    if (mixed) {
        dumpPanels();
        recordNone("the 30-index panels read different widget tables");
        unmapAll();
        return;
    }

    // The scrim: a standalone panel, dark and translucent, whose element
    // maps its fill to the full view -- verified through the same matrix
    // the shader will use, so a misclassification cannot survive its own
    // footprint.
    uint32_t scrimPos[kMaxScrims];
    uint32_t scrims = 0;
    uint32_t scrimRgba = 0, scrimElem = 0;
    for (uint32_t d = 0; d < g_capCount; ++d) {
        const CapDraw& cd = g_caps[d];
        if (cd.count != kPanelIndices || !known[d]) continue;
        const uint32_t c = rgba[d];
        const uint32_t r = c & 0xFF, gch = (c >> 8) & 0xFF,
                       b = (c >> 16) & 0xFF, a = (c >> 24) & 0xFF;
        if (r >= kDarkMax || gch >= kDarkMax || b >= kDarkMax) continue;
        if (a >= kOpaqueMin) continue;
        float rows[8];
        if (!elemRows(liveIdx(d), rows)) continue;
        const Foot f = footprint(rows, fill[d]);
        if (!f.valid) continue;
        if (f.cx1 - f.cx0 >= 2.0f * kFullFraction &&
            f.cy1 - f.cy0 >= 2.0f * kFullFraction && scrims < kMaxScrims) {
            if (scrims == 0) {
                scrimRgba = c;
                scrimElem = liveIdx(d);
            }
            scrimPos[scrims++] = cd.seqPos;
        }
    }
    unmapAll();

    if (scrims == 0) {
        dumpPanels();
        recordNone("no dark translucent panel maps to the full view");
        return;
    }

    memcpy(g_scrimPos, scrimPos, sizeof(uint32_t) * scrims);
    g_scrimCount = scrims;
    g_haveRoles = true;
    g_measuredHash = g_armedHash;
    g_measuredLen = g_capSeqLen;
    memcpy(g_measuredSeq, g_capSeq, sizeof(SeqEnt) * g_capSeqLen);
    ++g_measurements;

    // The engage line, rate-limited: after the first few, log again only
    // when the scrim moved position -- the percent text re-measures on
    // every tick and the scrim holds still through all of them.
    const bool moved =
        scrimPos[0] != g_lastScrimPos || scrims != g_lastScrimCount;
    g_lastScrimPos = scrimPos[0];
    g_lastScrimCount = scrims;
    if (g_measurements <= 4 || moved) {
        Log::get().note(
            "loading panel: FIT -- measurement %u from %u solids of %u "
            "interface draws. The scrim at draw %u (rgba %08X, element %u, "
            "full-view by its own matrix) is withheld; the dialog's black "
            "backing is the game's own eye-level layer and stays, so the "
            "box keeps its ground and nothing beyond it is tinted.%s",
            g_measurements, g_capCount, g_capSeqLen,
            scrimPos[0], scrimRgba, scrimElem,
            scrims > 1 ? " More than one scrim was found; each is withheld."
                       : "");
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
                "two consecutive frames in %u frames; the scrim draws "
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
