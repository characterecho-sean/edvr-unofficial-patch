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

// The bordered-panel widget: one fill plus four border strips. Both the
// backdrop and (when the box draws as its own panel) the box are this shape.
// Measured 2026-08-28.
constexpr uint32_t kPanelIndices = 30;

// A quad covering this fraction of both the widest and the tallest extent in
// the capture is a backdrop sheet, not content. Applied per QUAD: the
// backdrop's border strips are full-span in one axis only and are excluded
// from the target by the strip rule below instead.
constexpr float kBackdropFraction = 0.80f;

// A quad spanning this fraction of the reference in EITHER axis is an
// edge-riding strip or a separator, not part of the box: it must not drag
// the target out to the surface's edge the way the old union of "everything
// else" let one line of text drag it to a sliver.
constexpr float kStripFraction = 0.80f;

// The target must be at least this fraction of the backdrop in both axes.
// Below it, the "content" beside the backdrop was a cursor dot or a stray
// tick, and collapsing a full-view panel onto it would be far worse than
// stock.
constexpr float kMinTargetFraction = 0.02f;

// The box is assembled by GROWTH from a seed, not by union. The first
// flight (2026-08-28, 11:11) captured 5-8 solids per loader frame and their
// union spanned the surface every time: the loading screen keeps solid
// elements in its corners, and a union of everything is the whole screen
// even when each piece is small. So: seed on the box itself, then absorb
// only quads that touch the seed's neighbourhood -- the box's border
// strips and the bar inside it join; a badge in a far corner never does.
constexpr float    kGrowMargin = 0.02f;
constexpr uint32_t kGrowPasses = 4;

// Engage lines after the fourth are logged only when the box actually
// moved; the shader-prep dialog re-tessellates its percent text every tick
// and each tick is a fresh measurement of the same box.
constexpr float kRelogFraction = 0.01f;

// Frame-composition record: draw shapes per frame, and captured draws per
// measurement. The measured loader frame held about a dozen draws; these
// leave room without inviting a scan.
constexpr uint32_t kMaxSeq = 48;
constexpr uint32_t kMaxCaptures = 24;

// Backdrops substituted per frame. The evidence says one; a second
// full-surface panel would be another backdrop and gets the same treatment.
constexpr uint32_t kMaxBuilt = 4;

// Index bytes one measurement will hold. The loader's draws totalled about
// 3,600 indices; 64 KB is far above that and still trivial.
constexpr uint32_t kIbStageBytes = 64u << 10;

// The shared vertex buffer measured 4 MB; cap the staging copy at twice that
// so a bigger rig still measures and a runaway size cannot ask for hundreds.
constexpr uint32_t kVbStageCap = 8u << 20;

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

// One draw collected into the pending measurement.
struct CapDraw {
    uint32_t seqPos = 0;
    uint32_t count = 0;
    uint32_t ibOffset = 0;   // bytes into the index staging buffer
    int      baseVertex = 0;
    bool     i16 = false;    // this draw's own index format
};

// One backdrop's collapsed geometry, keyed by its position in the measured
// frame sequence.
struct Built {
    uint32_t pos = 0;
    uint32_t indices = 0;
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
};

// --- the current frame's composition -------------------------------------
uint32_t g_frame = 0;
uint32_t g_seqLen = 0;
SeqEnt   g_seq[kMaxSeq];
uint32_t g_hashAcc = 2166136261u;
bool     g_prefixOk = true;   // does this frame still match the measured one?
int      g_subSlot = -1;      // set by OnDraw for the Substitute that follows

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

// --- the measured result --------------------------------------------------
uint32_t g_measuredHash = 0;     // shape this verdict belongs to; 0 = none
uint32_t g_measuredLen = 0;
SeqEnt   g_measuredSeq[kMaxSeq];
Built    g_built[kMaxBuilt];
uint32_t g_builtCount = 0;
uint32_t g_builtStride = 0;
uint32_t g_measurements = 0;
Rect     g_lastTarget;   // for the engage log's moved-or-not test

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

void dropBuilt() {
    for (uint32_t i = 0; i < g_builtCount; ++i) {
        if (g_built[i].vb) g_built[i].vb->Release();
        if (g_built[i].ib) g_built[i].ib->Release();
        g_built[i] = Built{};
    }
    g_builtCount = 0;
}

void resetMeasured() {
    dropBuilt();
    g_measuredHash = 0;
    g_measuredLen = 0;
}

void resetFrameAcc() {
    g_seqLen = 0;
    g_hashAcc = 2166136261u;
    g_prefixOk = true;
    g_subSlot = -1;
}

// A verdict for a shape that yielded nothing to substitute. Recording the
// hash is what stops the same shape being re-measured -- and re-copying a
// 4 MB buffer -- every stable window until the dialog changes.
void recordNone(const char* why) {
    dropBuilt();
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
            "loading panel: %s. The full-view panel behind the loader's "
            "dialog is %s (docs/loading-panel-handoff.md).",
            g_on ? "FIT" : "stock",
            g_on ? "collapsed onto the dialog's own box, measured from the "
                   "game's draws; the box and its text are untouched"
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
    // definition; the box and the backdrop read none. A qualifying draw that
    // cannot be captured -- capacity, an overlong frame -- poisons the
    // collection: a verdict from a subset could put the box outside it.
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
                        g_caps[g_capCount].seqPos = p;
                        g_caps[g_capCount].count = count;
                        g_caps[g_capCount].ibOffset = g_ibFill;
                        g_caps[g_capCount].baseVertex = baseVertex;
                        g_caps[g_capCount].i16 = idxSize == 2u;
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

    // The substitution decision. Only a position the measurement marked as a
    // backdrop, and only while this frame still matches the measured one.
    g_subSlot = -1;
    if (g_prefixOk && g_builtCount) {
        for (uint32_t i = 0; i < g_builtCount; ++i) {
            if (g_built[i].pos == p && g_built[i].vb && g_built[i].ib) {
                g_subSlot = static_cast<int>(i);
                return true;
            }
        }
    }
    return false;
}

bool loaderPanelSubstitute(ID3D11DeviceContext* ctx, PfnDrawIndexedInstanced draw,
                           uint32_t instances, uint32_t startInstance) {
    if (!ctx || !draw || g_subSlot < 0 ||
        static_cast<uint32_t>(g_subSlot) >= g_builtCount) {
        return false;
    }
    const Built& b = g_built[g_subSlot];
    g_subSlot = -1;
    if (!b.vb || !b.ib || b.indices == 0) return false;
    bool done = false;
    guardedBudget(g_budget, [&] {
        // Save what the game had bound and put all of it back. Our vertices
        // are byte-identical in format to its own, so the input layout it
        // already set serves both.
        ID3D11Buffer* savedVb = nullptr;
        UINT savedStride = 0, savedOff = 0;
        ctx->IAGetVertexBuffers(0, 1, &savedVb, &savedStride, &savedOff);
        ID3D11Buffer* savedIb = nullptr;
        DXGI_FORMAT savedFmt = DXGI_FORMAT_UNKNOWN;
        UINT savedIbOff = 0;
        ctx->IAGetIndexBuffer(&savedIb, &savedFmt, &savedIbOff);

        const UINT stride = g_builtStride, zero = 0;
        ID3D11Buffer* vb = b.vb;
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &zero);
        ctx->IASetIndexBuffer(b.ib, DXGI_FORMAT_R16_UINT, 0);
        draw(ctx, b.indices, instances, 0, 0, startInstance);
        ctx->IASetVertexBuffers(0, 1, &savedVb, &savedStride, &savedOff);
        ctx->IASetIndexBuffer(savedIb, savedFmt, savedIbOff);
        if (savedVb) savedVb->Release();
        if (savedIb) savedIb->Release();
        done = true;
    });
    return done;
}

namespace {

// Retire a settled capture into a verdict: classify backdrops, take the
// union of the box's solids, build the collapsed geometry. Runs on the
// render thread inside the caller's budget guard; every exit that is not a
// build records the shape so it is not re-measured.
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

    // Every captured draw's quads, and the whole capture's reference extent.
    std::vector<std::vector<Rect>> quads(g_capCount);
    std::vector<Rect> whole(g_capCount);
    float refW = 0.0f, refH = 0.0f;
    for (uint32_t d = 0; d < g_capCount; ++d) {
        const CapDraw& cd = g_caps[d];
        const uint32_t n = cd.count / kIndicesPerQuad;
        quads[d].resize(n);
        for (uint32_t q = 0; q < n; ++q) {
            Rect& r = quads[d][q];
            for (uint32_t k = 0; k < kIndicesPerQuad; ++k) {
                const uint32_t at = q * kIndicesPerQuad + k;
                const uint8_t* ip = ibBase + cd.ibOffset + at * (cd.i16 ? 2 : 4);
                uint32_t vi = cd.i16 ? *reinterpret_cast<const uint16_t*>(ip)
                                     : *reinterpret_cast<const uint32_t*>(ip);
                const int64_t v = static_cast<int64_t>(vi) + cd.baseVertex;
                const int64_t off = v * g_capStride;
                if (v < 0 || off + 8 > static_cast<int64_t>(g_capVertexBytes)) {
                    continue;
                }
                float pos[2];
                memcpy(pos, vbBase + off, sizeof(pos));
                r.add(pos[0], pos[1]);
            }
            if (r.valid()) {
                whole[d].add(r);
                if (r.w() > refW) refW = r.w();
                if (r.h() > refH) refH = r.h();
            }
        }
    }

    if (refW <= 0.0f || refH <= 0.0f) {
        recordNone("no quad in the capture decoded to a rectangle");
        ctx->Unmap(g_vbStage, 0);
        ctx->Unmap(g_ibStage, 0);
        return;
    }

    // The backdrops: 30-index panels with a sheet spanning the capture.
    bool isBackdrop[kMaxCaptures] = {};
    int firstBackdrop = -1;
    uint32_t backdrops = 0;
    for (uint32_t d = 0; d < g_capCount; ++d) {
        if (g_caps[d].count != kPanelIndices) continue;
        for (const Rect& r : quads[d]) {
            if (r.valid() && r.w() >= refW * kBackdropFraction &&
                r.h() >= refH * kBackdropFraction) {
                isBackdrop[d] = true;
                ++backdrops;
                if (firstBackdrop < 0) firstBackdrop = static_cast<int>(d);
                break;
            }
        }
    }
    if (firstBackdrop < 0) {
        recordNone("no full-surface 30-index panel is in this frame");
        ctx->Unmap(g_vbStage, 0);
        ctx->Unmap(g_ibStage, 0);
        return;
    }

    // All bounds comparisons only mean anything inside one surface's
    // coordinate space; the backdrop's target names which.
    const SeqEnt anchor = g_capSeq[g_caps[firstBackdrop].seqPos];

    // When a verdict refuses, name every solid it saw: the first flight's
    // twenty identical refusals would have been one flight with this.
    auto dumpSolids = [&] {
        for (uint32_t d = 0; d < g_capCount && d < 10; ++d) {
            const Rect& w = whole[d];
            if (!w.valid()) continue;
            Log::get().note("  solid %u: %u indices at draw %u, x %.0f..%.0f "
                            "y %.0f..%.0f (%.0fx%.0f)%s",
                            d, g_caps[d].count, g_caps[d].seqPos,
                            w.x0, w.x1, w.y0, w.y1, w.w(), w.h(),
                            isBackdrop[d] ? " -- backdrop" : "");
        }
    };

    // Candidate quads for the box: solids in the backdrop's space that are
    // neither full sheets nor full-span strips, from draws that are not
    // backdrops themselves.
    struct Cand {
        Rect rect;
        uint32_t cap;
        bool used = false;
    };
    std::vector<Cand> cands;
    uint32_t strips = 0, sheets = 0;
    for (uint32_t d = 0; d < g_capCount; ++d) {
        if (isBackdrop[d]) continue;
        const SeqEnt& se = g_capSeq[g_caps[d].seqPos];
        if (se.w != anchor.w || se.h != anchor.h) continue;
        for (const Rect& r : quads[d]) {
            if (!r.valid()) continue;
            if (r.w() >= refW * kBackdropFraction &&
                r.h() >= refH * kBackdropFraction) {
                // A full sheet inside a batch cannot be substituted away by
                // this mechanism and must not become "content" either.
                ++sheets;
                continue;
            }
            if (r.w() >= refW * kStripFraction ||
                r.h() >= refH * kStripFraction) {
                ++strips;
                continue;
            }
            cands.push_back(Cand{r, d, false});
        }
    }

    if (cands.empty()) {
        recordNone("the backdrop is the only solid drawn -- the dialog has "
                   "not arrived yet");
        ctx->Unmap(g_vbStage, 0);
        ctx->Unmap(g_ibStage, 0);
        return;
    }

    // The seed: the box's own bordered panel when one draws (the strongest
    // structural signal there is), else the largest solid in any batch.
    Rect target;
    int seedCap = -1;
    bool seedIsPanel = false;
    float seedArea = 0.0f;
    for (uint32_t d = 0; d < g_capCount; ++d) {
        if (isBackdrop[d] || g_caps[d].count != kPanelIndices) continue;
        const SeqEnt& se = g_capSeq[g_caps[d].seqPos];
        if (se.w != anchor.w || se.h != anchor.h) continue;
        if (whole[d].valid() && whole[d].area() > seedArea) {
            seedArea = whole[d].area();
            seedCap = static_cast<int>(d);
        }
    }
    if (seedCap >= 0) {
        target = whole[seedCap];
        seedIsPanel = true;
    } else {
        for (const Cand& c : cands) {
            if (c.rect.area() > seedArea) {
                seedArea = c.rect.area();
                seedCap = static_cast<int>(c.cap);
                target = c.rect;
            }
        }
    }

    // Growth: absorb candidates that touch the box's neighbourhood, so its
    // border strips and the bar inside it join while a badge in a far
    // corner stays out. A few passes, because a strip can bridge to the
    // fill only after the fill has joined.
    const float gx = refW * kGrowMargin, gy = refH * kGrowMargin;
    uint32_t contributors = 0;
    for (uint32_t pass = 0; pass < kGrowPasses; ++pass) {
        bool grew = false;
        for (Cand& c : cands) {
            if (c.used) continue;
            if (c.rect.x0 <= target.x1 + gx && c.rect.x1 >= target.x0 - gx &&
                c.rect.y0 <= target.y1 + gy && c.rect.y1 >= target.y0 - gy) {
                const Rect before = target;
                target.add(c.rect);
                c.used = true;
                ++contributors;
                grew = grew || target.w() != before.w() ||
                       target.h() != before.h() || target.x0 != before.x0 ||
                       target.y0 != before.y0;
            }
        }
        if (!grew) break;
    }

    if (!target.valid() || target.w() < refW * kMinTargetFraction ||
        target.h() < refH * kMinTargetFraction) {
        dumpSolids();
        recordNone("the box seeded and grew to a sliver, not a dialog");
        ctx->Unmap(g_vbStage, 0);
        ctx->Unmap(g_ibStage, 0);
        return;
    }
    if (target.w() >= refW * kBackdropFraction &&
        target.h() >= refH * kBackdropFraction) {
        dumpSolids();
        recordNone("the box grew until it spanned the surface itself");
        ctx->Unmap(g_vbStage, 0);
        ctx->Unmap(g_ibStage, 0);
        return;
    }

    // Build each backdrop's collapsed twin: its own vertices verbatim,
    // positions mapped linearly from its bounds onto the target. Only the
    // float2 at offset 0 changes, so colour and everything else in the
    // 24-byte vertex survives and no encoding has to be understood.
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) {
        ctx->Unmap(g_vbStage, 0);
        ctx->Unmap(g_ibStage, 0);
        failOnce("the device was unreachable at build time");
        return;
    }
    dropBuilt();
    bool buildFailed = false;
    uint32_t skippedBackdrops = 0;
    for (uint32_t d = 0; d < g_capCount && !buildFailed; ++d) {
        if (!isBackdrop[d]) continue;
        if (g_builtCount >= kMaxBuilt) {
            ++skippedBackdrops;
            continue;
        }
        const CapDraw& cd = g_caps[d];
        const Rect& own = whole[d];
        if (!own.valid() || own.w() <= 0.0f || own.h() <= 0.0f) {
            ++skippedBackdrops;
            continue;
        }
        std::vector<uint8_t> verts(static_cast<size_t>(cd.count) * g_capStride);
        std::vector<uint16_t> idx(cd.count);
        bool ok = true;
        for (uint32_t k = 0; k < cd.count && ok; ++k) {
            const uint8_t* ip = ibBase + cd.ibOffset + k * (cd.i16 ? 2 : 4);
            uint32_t vi = cd.i16 ? *reinterpret_cast<const uint16_t*>(ip)
                                 : *reinterpret_cast<const uint32_t*>(ip);
            const int64_t v = static_cast<int64_t>(vi) + cd.baseVertex;
            const int64_t off = v * g_capStride;
            if (v < 0 ||
                off + g_capStride > static_cast<int64_t>(g_capVertexBytes)) {
                ok = false;
                break;
            }
            uint8_t* dst = &verts[static_cast<size_t>(k) * g_capStride];
            memcpy(dst, vbBase + off, g_capStride);
            float* pos = reinterpret_cast<float*>(dst);
            pos[0] = target.x0 + (pos[0] - own.x0) / own.w() * target.w();
            pos[1] = target.y0 + (pos[1] - own.y0) / own.h() * target.h();
            idx[k] = static_cast<uint16_t>(k);
        }
        if (!ok) {
            buildFailed = true;
            break;
        }
        D3D11_BUFFER_DESC bd{};
        D3D11_SUBRESOURCE_DATA sr{};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.ByteWidth = static_cast<UINT>(verts.size());
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        sr.pSysMem = verts.data();
        Built& b = g_built[g_builtCount];
        bool made = SUCCEEDED(dev->CreateBuffer(&bd, &sr, &b.vb));
        bd.ByteWidth = static_cast<UINT>(idx.size() * 2);
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sr.pSysMem = idx.data();
        made = made && SUCCEEDED(dev->CreateBuffer(&bd, &sr, &b.ib));
        if (!made) {
            if (b.vb) { b.vb->Release(); b.vb = nullptr; }
            if (b.ib) { b.ib->Release(); b.ib = nullptr; }
            buildFailed = true;
            break;
        }
        b.pos = cd.seqPos;
        b.indices = cd.count;
        ++g_builtCount;
    }
    dev->Release();
    ctx->Unmap(g_vbStage, 0);
    ctx->Unmap(g_ibStage, 0);

    if (buildFailed || g_builtCount == 0) {
        dropBuilt();
        recordNone("the collapsed geometry could not be built");
        return;
    }
    g_builtStride = g_capStride;
    g_measuredHash = g_armedHash;
    g_measuredLen = g_capSeqLen;
    memcpy(g_measuredSeq, g_capSeq, sizeof(SeqEnt) * g_capSeqLen);
    ++g_measurements;

    // Name what the box was seeded FROM, so a field report can validate the
    // pick against what the headset shows. After the first few, log again
    // only when the box moved: the shader-prep dialog re-measures on every
    // percent tick and the box holds still through all of them.
    const bool moved =
        fabsf(target.x0 - g_lastTarget.x0) > refW * kRelogFraction ||
        fabsf(target.x1 - g_lastTarget.x1) > refW * kRelogFraction ||
        fabsf(target.y0 - g_lastTarget.y0) > refH * kRelogFraction ||
        fabsf(target.y1 - g_lastTarget.y1) > refH * kRelogFraction;
    g_lastTarget = target;
    if (g_measurements <= 4 || moved) {
        char source[96];
        if (seedIsPanel) {
            snprintf(source, sizeof(source),
                     "the dialog's own 30-index panel");
        } else {
            snprintf(source, sizeof(source),
                     "the largest solid in a %u-index batch",
                     g_caps[seedCap].count);
        }
        const Rect& bdrop = whole[firstBackdrop];
        Log::get().note(
            "loading panel: FIT -- measurement %u from %u solids of %u "
            "interface draws. The backdrop at draw %u spans %.0fx%.0f; the "
            "box on top of it seeded from %s and grew to %.0fx%.0f at "
            "x %.0f..%.0f y %.0f..%.0f (%u quads joined%s%s). The backdrop "
            "is redrawn to those exact bounds; the box and its text are the "
            "game's own draws, untouched.%s",
            g_measurements, g_capCount, g_capSeqLen,
            g_caps[firstBackdrop].seqPos, bdrop.w(), bdrop.h(),
            source, target.w(), target.h(),
            target.x0, target.x1, target.y0, target.y1, contributors,
            strips ? ", full-span strips stayed out" : "",
            sheets ? ", a full sheet in a batch stayed out" : "",
            (backdrops > 1 || skippedBackdrops)
                ? " More than one backdrop was found; each is collapsed."
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
