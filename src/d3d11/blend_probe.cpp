// The blend probe (blend_probe.h; TEMPORARY, docs/dlss-blur-elements-2026-10-14.md). The rules, the row and grid text and the refusal
// reasons are in blend_probe_core.h (tools\blend_probe_test). This file collects: the candidate eye draws of 3 consecutive frames
// (vscreen.cpp offers them), each tested by a scissored duplicate that writes nothing and is counted by an occlusion query, and one
// staged 96x96 crop of the temporal pass's per-pixel decisions per frame (temporal_pass.cpp offers it). Phases: a press arms; the next
// frame boundary starts collecting; each following boundary closes a frame; after the third the answers are polled at every boundary
// until every query and staging copy has answered or kReadTicksMax boundaries have passed, and the report is written.
#include "blend_probe.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/log.h"
#include "binding_shadow.h"
#include "blend_probe_core.h"
#include "draw_state_describe.h"
#include "engine_velocity_families.h"
#include "ui_depth.h"
#include "ui_layer_math.h"
#include "vscreen.h"

namespace edvr {
namespace {

using namespace blend;

enum Phase : int { kIdle = 0, kArmed = 1, kCollecting = 2, kReading = 3 };
std::atomic<int> g_phase{kIdle};
std::atomic<bool> g_gridNeeded{false};
bool g_installed = false;
bool g_flat = false;

struct Cand {
    Row row;
    ID3D11Query* query = nullptr;
    bool replayed = false;     // the duplicate was issued and its query bracket closed
    bool answered = false;
    int64_t samples = kCoverageUnknown;
};

struct Grid {
    bool offered = false;      // the pass was asked, whether or not it staged
    bool staged = false;
    bool read = false;
    ID3D11Texture2D* staging[4] = {};   // decision, depth, coverage, bias
    bool have[4] = {};
    uint32_t winX = 0, winY = 0, winW = 0, winH = 0, inW = 0, inH = 0, sceneFrame = 0;
    float rotDeg = 0.0f, transLen = 0.0f;
    bool transValid = false;
    GridSample cells[kGrid * kGrid];
};

struct Slot {
    std::vector<Cand> cands;
    FrameCounts counts;
    Grid grid;
};

Slot g_slots[kFrames];
uint32_t g_cur = 0;                // the slot being collected
uint32_t g_readTicks = 0;
uint32_t g_startFrame = 0;
std::string g_trigger;
ID3D11DeviceContext* g_ctx = nullptr;   // AddRef'd while a query or a staging copy is outstanding
bool g_queryFailed = false;
HRESULT g_queryFailHr = S_OK;
uint32_t g_probeFaults = 0;
std::string g_gridWhy;             // the first reason the pass gave for a missing grid

// The probe's own state objects: the draw's rasterizer, blend and depth-stencil state with the one change that makes the duplicate safe.
std::map<std::string, ID3D11RasterizerState*> g_rsClones;
std::map<std::string, ID3D11BlendState*> g_bsClones;
std::map<std::string, ID3D11DepthStencilState*> g_dsClones;

template <typename T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() {
        if (p) p->Release();
    }
    T* get() const { return p; }
    T* operator->() const { return p; }
    T** put() { return &p; }
    T* release() {
        T* x = p;
        p = nullptr;
        return x;
    }
};
template <typename T, size_t N>
struct ComArray {
    T* a[N] = {};
    ComArray() = default;
    ComArray(const ComArray&) = delete;
    ComArray& operator=(const ComArray&) = delete;
    ~ComArray() {
        for (T* x : a)
            if (x) x->Release();
    }
};

const char* phaseName(int p) {
    switch (p) {
        case kArmed: return "armed";
        case kCollecting: return "collecting";
        case kReading: return "reading its answers";
        default: return "idle";
    }
}

void say(const char* line) { Log::get().note("%s", line); }

void refuse(Refusal r, const char* detail = nullptr) {
    Log::get().note("blend probe: refused (%s)%s%s", refusalText(r), detail ? ": " : "", detail ? detail : "");
}

void holdCtx(ID3D11DeviceContext* ctx) {
    if (!g_ctx && ctx) {
        g_ctx = ctx;
        ctx->AddRef();
    }
}

void copyText(char* dst, size_t cap, const char* src) {
    std::snprintf(dst, cap, "%s", src ? src : "");
}

// The names with their spaces made underscores: a row is read by eye and by script, and a space would split a token.
void copyToken(char* dst, size_t cap, const char* src) {
    copyText(dst, cap, src);
    for (char* c = dst; *c; ++c)
        if (*c == ' ') *c = '_';
}

const char* formatNameOf(DXGI_FORMAT f, char* buf, size_t cap) {
    const char* n = viewName(f);
    if (std::strcmp(n, "another format") != 0) return n;
    std::snprintf(buf, cap, "fmt%u", static_cast<unsigned>(f));
    return buf;
}

// ---- the duplicate's states ----------------------------------------------------------------------------------------------------

ID3D11RasterizerState* rasterizerClone(ID3D11Device* dev, const D3D11_RASTERIZER_DESC& src) {
    D3D11_RASTERIZER_DESC d = src;
    d.ScissorEnable = TRUE;
    const std::string key(reinterpret_cast<const char*>(&d), sizeof(d));
    const auto it = g_rsClones.find(key);
    if (it != g_rsClones.end()) return it->second;
    ID3D11RasterizerState* out = nullptr;
    if (FAILED(dev->CreateRasterizerState(&d, &out)) || !out) return nullptr;
    g_rsClones.emplace(key, out);
    return out;
}
ID3D11BlendState* blendClone(ID3D11Device* dev, const D3D11_BLEND_DESC& src) {
    D3D11_BLEND_DESC d = src;
    for (D3D11_RENDER_TARGET_BLEND_DESC& rt : d.RenderTarget) rt.RenderTargetWriteMask = 0;
    const std::string key(reinterpret_cast<const char*>(&d), sizeof(d));
    const auto it = g_bsClones.find(key);
    if (it != g_bsClones.end()) return it->second;
    ID3D11BlendState* out = nullptr;
    if (FAILED(dev->CreateBlendState(&d, &out)) || !out) return nullptr;
    g_bsClones.emplace(key, out);
    return out;
}
ID3D11DepthStencilState* depthClone(ID3D11Device* dev, const D3D11_DEPTH_STENCIL_DESC& src) {
    D3D11_DEPTH_STENCIL_DESC d = src;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.StencilWriteMask = 0;
    const std::string key(reinterpret_cast<const char*>(&d), sizeof(d));
    const auto it = g_dsClones.find(key);
    if (it != g_dsClones.end()) return it->second;
    ID3D11DepthStencilState* out = nullptr;
    if (FAILED(dev->CreateDepthStencilState(&d, &out)) || !out) return nullptr;
    g_dsClones.emplace(key, out);
    return out;
}

// D3D11's defaults for a state that is not bound (null).
D3D11_RASTERIZER_DESC defaultRasterizer() {
    D3D11_RASTERIZER_DESC d{};
    d.FillMode = D3D11_FILL_SOLID;
    d.CullMode = D3D11_CULL_BACK;
    d.DepthClipEnable = TRUE;
    return d;
}
D3D11_BLEND_DESC defaultBlend() {
    D3D11_BLEND_DESC d{};
    for (D3D11_RENDER_TARGET_BLEND_DESC& rt : d.RenderTarget) {
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_ZERO;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    return d;
}
D3D11_DEPTH_STENCIL_DESC defaultDepthStencil() {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = D3D11_COMPARISON_LESS;
    d.StencilReadMask = 0xFF;
    d.StencilWriteMask = 0xFF;
    d.FrontFace.StencilFunc = d.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    d.FrontFace.StencilFailOp = d.FrontFace.StencilDepthFailOp = d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    d.BackFace = d.FrontFace;
    return d;
}

void releaseClones() {
    for (auto& kv : g_rsClones)
        if (kv.second) kv.second->Release();
    g_rsClones.clear();
    for (auto& kv : g_bsClones)
        if (kv.second) kv.second->Release();
    g_bsClones.clear();
    for (auto& kv : g_dsClones)
        if (kv.second) kv.second->Release();
    g_dsClones.clear();
}

// ---- the row text from what the draw path knew -----------------------------------------------------------------------------------

void fillRowRoutes(Row& r, const BlendDrawInfo& i) {
    copyToken(r.verdict, sizeof(r.verdict), i.verdict);
    if (i.uiFamily > 0 && i.uiFamily < static_cast<int>(UiLayerFamily::kCount))
        copyToken(r.uiFamily, sizeof(r.uiFamily), uiLayerFamilyName(static_cast<UiLayerFamily>(i.uiFamily)));
    else
        copyText(r.uiFamily, sizeof(r.uiFamily), "-");
    if (!i.uiDecided) {
        copyText(r.uiDecision, sizeof(r.uiDecision), "none");
    } else if (i.uiTaken) {
        copyText(r.uiDecision, sizeof(r.uiDecision), "taken");
    } else {
        std::snprintf(r.uiDecision, sizeof(r.uiDecision), "left:%s",
                      uiLayerDecisionKey(static_cast<UiLayerDecision>(i.uiDecision)));
    }
    copyText(r.holo, sizeof(r.holo), i.holoMatched ? (i.holoWorld ? "world" : "cockpit") : "-");
    if (!i.udCoverage) {
        copyText(r.coverage, sizeof(r.coverage), "-");
    } else if (!i.udWantMask) {
        std::snprintf(r.coverage, sizeof(r.coverage), "%s:nomask", maskSlotName(i.udMaskSlot));
    } else {
        int f = 1, a = 1;
        maskQuanta(i.udMaskSlot, &f, &a);
        std::snprintf(r.coverage, sizeof(r.coverage), "%s:m%d/%d", maskSlotName(i.udMaskSlot), f, a);
    }
    copyText(r.engine, sizeof(r.engine), i.engineSubstituted ? "sub" : "plain");
}

void fillRowKeyed(Row& r) {
    namespace ev = engine_velocity_family;
    const int fam = ev::familyForProfile(r.vs, false);
    if (fam >= 0 && ev::keyedPs(fam, r.ps, false))
        std::snprintf(r.keyed, sizeof(r.keyed), "%s#%d", ev::skinPair(r.vs, r.ps) ? "skinned" : "rigid", fam);
    else if (ev::selfMarkingPair(r.vs, r.ps))
        copyText(r.keyed, sizeof(r.keyed), "self-marking");
    else if (fam >= 0)
        std::snprintf(r.keyed, sizeof(r.keyed), "vs-only#%d", fam);
    else
        copyText(r.keyed, sizeof(r.keyed), "-");
}

// ---- one draw ----------------------------------------------------------------------------------------------------------------

void drawImpl(ID3D11DeviceContext* ctx, const BlendDrawInfo& info, BlendReplayFn replay, void* arg) {
    if (!ctx || g_cur >= kFrames) return;
    Slot& slot = g_slots[g_cur];
    FrameCounts& c = slot.counts;
    if (c.gameFrame == 0) c.gameFrame = info.gameFrame;
    ++c.eyeDraws;

    ComArray<ID3D11RenderTargetView, 8> rtvs;
    ComArray<ID3D11UnorderedAccessView, 64> uavs;
    Com<ID3D11DepthStencilView> dsv;
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(8, rtvs.a, dsv.put(), 0, 64, uavs.a);
    if (!rtvs.a[0]) return;
    Com<ID3D11Resource> res;
    rtvs.a[0]->GetResource(res.put());
    Com<ID3D11Texture2D> tex;
    if (!res.get() || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(tex.put()))) || !tex.get())
        return;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    const int eye = uiDepthEyeOfTargetReadOnly(res.get());
    if (eye == 0) ++c.eye0;
    else if (eye == 1) ++c.eye1;
    else ++c.eyeUnknown;
    if (eye == 1) return;   // eye 0 is enough: the other eye's pass is the same draws

    // The state the draw runs with. A null state is D3D11's default.
    Com<ID3D11RasterizerState> rs;
    ctx->RSGetState(rs.put());
    Com<ID3D11BlendState> bs;
    FLOAT bf[4] = {};
    UINT bmask = 0xFFFFFFFFu;
    ctx->OMGetBlendState(bs.put(), bf, &bmask);
    Com<ID3D11DepthStencilState> ds;
    UINT dref = 0;
    ctx->OMGetDepthStencilState(ds.put(), &dref);
    D3D11_RASTERIZER_DESC rsd = defaultRasterizer();
    D3D11_BLEND_DESC bsd = defaultBlend();
    D3D11_DEPTH_STENCIL_DESC dsd = defaultDepthStencil();
    if (rs.get()) {
        std::memset(&rsd, 0, sizeof(rsd));
        rs->GetDesc(&rsd);
    }
    if (bs.get()) {
        std::memset(&bsd, 0, sizeof(bsd));
        bs->GetDesc(&bsd);
    }
    if (ds.get()) {
        std::memset(&dsd, 0, sizeof(dsd));
        ds->GetDesc(&dsd);
    }
    const D3D11_RENDER_TARGET_BLEND_DESC& rt0 = bsd.RenderTarget[0];
    BlendDesc bd;
    bd.enable = rt0.BlendEnable != FALSE;
    bd.src = static_cast<uint32_t>(rt0.SrcBlend);
    bd.dst = static_cast<uint32_t>(rt0.DestBlend);
    bd.op = static_cast<uint32_t>(rt0.BlendOp);
    bd.srcA = static_cast<uint32_t>(rt0.SrcBlendAlpha);
    bd.dstA = static_cast<uint32_t>(rt0.DestBlendAlpha);
    bd.opA = static_cast<uint32_t>(rt0.BlendOpAlpha);
    bd.writeMask = rt0.RenderTargetWriteMask;
    bd.alphaToCoverage = bsd.AlphaToCoverageEnable != FALSE;
    DepthDesc dd;
    dd.dsvBound = dsv.get() != nullptr;
    dd.test = dsd.DepthEnable != FALSE;
    dd.func = static_cast<uint32_t>(dsd.DepthFunc);
    dd.write = dsd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.stencil = dsd.StencilEnable != FALSE;
    if (!isCandidate((bd.writeMask & 0xFu) != 0, bd.enable, depthWrites(dd))) return;
    ++c.candidates;
    if (slot.cands.size() >= kMaxCandidates) {
        ++c.candidateOverflow;
        return;
    }
    slot.cands.emplace_back();
    Cand& cand = slot.cands.back();
    Row& row = cand.row;
    row.slot = g_cur;
    row.gameFrame = info.gameFrame;
    row.ordinal = info.ordinal;
    row.eye = eye;
    row.vs = bindingShaderHash(BindSlot::Vs);
    row.ps = bindingShaderHash(BindSlot::Ps);
    row.drawKind = info.kind;
    row.count = info.count;
    row.instances = info.instances;
    formatBlend(row.blend, sizeof(row.blend), bd);
    formatDepth(row.depth, sizeof(row.depth), dd);
    char fb[16];
    D3D11_RENDER_TARGET_VIEW_DESC rvd{};
    rtvs.a[0]->GetDesc(&rvd);
    copyText(row.rtFormat, sizeof(row.rtFormat), formatNameOf(rvd.Format != DXGI_FORMAT_UNKNOWN ? rvd.Format : td.Format, fb, sizeof(fb)));
    row.rtW = td.Width;
    row.rtH = td.Height;
    fillRowRoutes(row, info);
    fillRowKeyed(row);

    // The duplicate. Anything that stops it leaves the row unmeasured (cov=?), never absent.
    for (ID3D11UnorderedAccessView* u : uavs.a)
        if (u) {
            ++c.untestedState;
            return;   // a UAV write would be done twice
        }
    ComArray<ID3D11Buffer, 4> so;
    ctx->SOGetTargets(4, so.a);
    for (ID3D11Buffer* b : so.a)
        if (b) {
            ++c.untestedState;
            return;
        }
    if (!replay) {
        ++c.untestedState;
        return;
    }
    D3D11_VIEWPORT vps[16] = {};
    UINT nVp = 16;
    ctx->RSGetViewports(&nVp, vps);
    Rect win;
    if (nVp == 0 || !windowRect(vps[0].TopLeftX, vps[0].TopLeftY, vps[0].Width, vps[0].Height, td.Width, td.Height, &win)) {
        ++c.untestedState;
        return;
    }
    Com<ID3D11Device> dev;
    ctx->GetDevice(dev.put());
    if (!dev.get()) {
        ++c.untestedState;
        return;
    }
    ID3D11RasterizerState* rsC = rasterizerClone(dev.get(), rsd);
    ID3D11BlendState* bsC = blendClone(dev.get(), bsd);
    ID3D11DepthStencilState* dsC = depthClone(dev.get(), dsd);
    if (!rsC || !bsC || !dsC) {
        ++c.untestedState;
        return;
    }
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_OCCLUSION;
    Com<ID3D11Query> q;
    const HRESULT qhr = dev->CreateQuery(&qd, q.put());
    if (FAILED(qhr) || !q.get()) {
        if (!g_queryFailed) {
            g_queryFailed = true;
            g_queryFailHr = qhr;
        }
        ++c.untestedState;
        return;
    }
    UINT scN = 16;
    D3D11_RECT sc[16] = {};
    ctx->RSGetScissorRects(&scN, sc);
    const D3D11_RECT one = {win.x0, win.y0, win.x1, win.y1};
    ctx->RSSetState(rsC);
    ctx->RSSetScissorRects(1, &one);
    vScreenOMSetBlendStateRaw(ctx, bsC, bf, bmask);   // past the binding shadow: the duplicate is not the game's bind
    ctx->OMSetDepthStencilState(dsC, dref);
    ctx->Begin(q.get());
    replay(arg);
    ctx->End(q.get());
    ctx->RSSetState(rs.get());
    ctx->RSSetScissorRects(scN, sc);
    vScreenOMSetBlendStateRaw(ctx, bs.get(), bf, bmask);
    ctx->OMSetDepthStencilState(ds.get(), dref);
    cand.query = q.release();
    cand.replayed = true;
    ++c.tested;
    holdCtx(ctx);
}

// ---- polling and the report ---------------------------------------------------------------------------------------------------

void pollQueries() {
    if (!g_ctx) return;
    for (Slot& s : g_slots)
        for (Cand& c : s.cands) {
            if (!c.query || c.answered) continue;
            UINT64 samples = 0;
            if (g_ctx->GetData(c.query, &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
                c.answered = true;
                c.samples = static_cast<int64_t>(samples);
            }
        }
}

uint32_t outstandingQueries() {
    uint32_t n = 0;
    for (const Slot& s : g_slots)
        for (const Cand& c : s.cands)
            if (c.query && !c.answered) ++n;
    return n;
}

// Reads one staged crop's cells. false if the copy has not finished (the caller tries again at the next boundary).
bool tryReadGrid(Grid& g) {
    if (!g_ctx) return false;
    D3D11_MAPPED_SUBRESOURCE m[4] = {};
    bool mapped[4] = {};
    bool pending = false;
    for (int k = 0; k < 4 && !pending; ++k) {
        if (!g.have[k]) continue;
        const HRESULT hr = g_ctx->Map(g.staging[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m[k]);
        if (hr == S_OK) mapped[k] = true;
        else pending = true;   // DXGI_ERROR_WAS_STILL_DRAWING, or a failure that will not clear: the report's tick limit ends the wait
    }
    if (!pending) {
        for (uint32_t j = 0; j < kGrid; ++j) {
            for (uint32_t i = 0; i < kGrid; ++i) {
                GridSample& s = g.cells[gridCell(i, j)];
                s = GridSample{};
                const uint32_t x = gridCoord(i, g.winW), y = gridCoord(j, g.winH);
                s.z = std::numeric_limits<float>::quiet_NaN();
                if (g.have[0]) {
                    const float* f = reinterpret_cast<const float*>(static_cast<const char*>(m[0].pData) + y * m[0].RowPitch + x * 16);
                    s.valid = true;
                    s.mvx = f[0];
                    s.mvy = f[1];
                    s.flags = decodeFlags(f[3]);
                }
                if (g.have[1]) {
                    const float* f = reinterpret_cast<const float*>(static_cast<const char*>(m[1].pData) + y * m[1].RowPitch + x * 4);
                    s.z = f[0];
                }
                if (g.have[2]) s.coverage = static_cast<const uint8_t*>(m[2].pData)[y * m[2].RowPitch + x];
                if (g.have[3]) s.bias = static_cast<const uint8_t*>(m[3].pData)[y * m[3].RowPitch + x];
            }
        }
    }
    for (int k = 0; k < 4; ++k)
        if (mapped[k]) g_ctx->Unmap(g.staging[k], 0);
    if (!pending) g.read = true;
    return !pending;
}

void pollGrids() {
    for (Slot& s : g_slots)
        if (s.grid.staged && !s.grid.read) tryReadGrid(s.grid);
}

uint32_t outstandingGrids() {
    uint32_t n = 0;
    for (const Slot& s : g_slots)
        if (s.grid.staged && !s.grid.read) ++n;
    return n;
}

void releaseAll() {
    for (Slot& s : g_slots) {
        for (Cand& c : s.cands)
            if (c.query) c.query->Release();
        s.cands.clear();
        s.cands.shrink_to_fit();
        for (ID3D11Texture2D*& t : s.grid.staging)
            if (t) {
                t->Release();
                t = nullptr;
            }
        s.grid = Grid{};
        s.counts = FrameCounts{};
    }
    releaseClones();
    if (g_ctx) {
        g_ctx->Release();
        g_ctx = nullptr;
    }
}

void writeReport() {
    uint32_t rowsTotal = 0, gridsStaged = 0, unanswered = 0, eyeDrawsTotal = 0;
    struct Listed {
        std::vector<const Cand*> rows;
        uint32_t covering = 0, truncated = 0;
    } listed[kFrames];
    for (uint32_t k = 0; k < kFrames; ++k) {
        Slot& s = g_slots[k];
        eyeDrawsTotal += s.counts.eyeDraws;
        std::vector<const Cand*> keep;
        for (const Cand& c : s.cands) {
            if (c.query && !c.answered) {
                ++s.counts.unanswered;
                ++unanswered;
                keep.push_back(&c);   // unmeasured: listed, with cov=?
            } else if (!c.query) {
                keep.push_back(&c);   // never tested: listed, with cov=?
            } else if (coversWindow(c.samples)) {
                keep.push_back(&c);
                ++listed[k].covering;
            }
        }
        s.counts.covering = listed[k].covering;
        // The last kMaxRowsPerFrame in issue order: later draws are the ones that end up on screen over the earlier.
        size_t first = 0;
        if (keep.size() > kMaxRowsPerFrame) {
            first = keep.size() - kMaxRowsPerFrame;
            listed[k].truncated = static_cast<uint32_t>(first);
        }
        listed[k].rows.assign(keep.begin() + static_cast<std::ptrdiff_t>(first), keep.end());
        s.counts.listed = static_cast<uint32_t>(listed[k].rows.size());
        s.counts.truncated = listed[k].truncated;
        rowsTotal += s.counts.listed;
        if (s.grid.read) ++gridsStaged;
    }
    char line[kRowBuf];
    formatCapturedLine(line, sizeof(line), kFrames, rowsTotal, gridsStaged);
    say(line);
    if (eyeDrawsTotal == 0) refuse(Refusal::kNoEyePass);
    if (g_queryFailed) {
        char d[48];
        std::snprintf(d, sizeof(d), "CreateQuery 0x%08lX", static_cast<unsigned long>(g_queryFailHr));
        refuse(Refusal::kQueriesUnavailable, d);
    }
    if (unanswered) {
        char d[96];
        std::snprintf(d, sizeof(d), "%u queries, after %u frame boundaries", unanswered, g_readTicks);
        refuse(Refusal::kQueriesUnanswered, d);
    }
    if (gridsStaged < kFrames) refuse(Refusal::kNoGrid, g_gridWhy.empty() ? "no reason was given" : g_gridWhy.c_str());
    if (g_probeFaults) Log::get().note("blend probe: %u draw offers faulted and were skipped.", g_probeFaults);
    bool legendSaid = false;
    for (uint32_t k = 0; k < kFrames; ++k) {
        Slot& s = g_slots[k];
        formatFrameLine(line, sizeof(line), k, s.counts);
        say(line);
        for (const Cand* c : listed[k].rows) {
            Row r = c->row;
            r.covered = (c->query && c->answered) ? c->samples : kCoverageUnknown;
            if (formatRow(line, sizeof(line), r) < 0) std::snprintf(line, sizeof(line), "blend probe row: f=%u #%u (row text did not fit)", k, r.ordinal);
            say(line);
        }
        const Grid& g = s.grid;
        if (!g.read) {
            Log::get().note("blend probe grid f=%u: not available (%s)", k,
                            g.staged ? "the staged copy had not finished when the report was written"
                                     : (g_gridWhy.empty() ? "the temporal pass offered none" : g_gridWhy.c_str()));
            continue;
        }
        char inputs[96];
        std::snprintf(inputs, sizeof(inputs), "%sdecision %sdepth %scoverage %sbias", g.have[0] ? "" : "-", g.have[1] ? "" : "-",
                      g.have[2] ? "" : "-", g.have[3] ? "" : "-");
        GridHeader h;
        h.slot = k;
        h.gameFrame = s.counts.gameFrame;
        h.sceneFrame = g.sceneFrame;
        h.winX = g.winX;
        h.winY = g.winY;
        h.winW = g.winW;
        h.winH = g.winH;
        h.inputW = g.inW;
        h.inputH = g.inH;
        h.rotDeg = g.rotDeg;
        h.transLen = g.transLen;
        h.transValid = g.transValid;
        h.inputs = inputs;
        if (!legendSaid) {
            say(gridLegend());
            legendSaid = true;
        }
        formatGridHead(line, sizeof(line), h);
        say(line);
        for (uint32_t j = 0; j < kGrid; ++j) {
            formatGridRow(line, sizeof(line), k, j, g.cells);
            say(line);
        }
        formatGridSummary(line, sizeof(line), k, summarize(g.cells, kGrid * kGrid));
        say(line);
    }
}

void finishCapture() {
    writeReport();
    releaseAll();
    g_phase.store(kIdle, std::memory_order_release);
}

}  // namespace

void blendProbeInstall(bool flatProfile, bool keyBound, const char* key) {
    g_installed = true;
    g_flat = flatProfile;
    char line[kRowBuf];
    formatInstallLine(line, sizeof(line), flatProfile, keyBound, key, Config::get().iniName());
    say(line);
}
void blendProbeRequest(const char* trigger) {
    if (g_flat) {
        refuse(Refusal::kNotVr);
        return;
    }
    if (!g_installed) {
        refuse(Refusal::kNoHooks);
        return;
    }
    int expected = kIdle;
    if (!g_phase.compare_exchange_strong(expected, kArmed, std::memory_order_acq_rel)) {
        Log::get().note("blend probe: refused (%s): the capture is %s (trigger %s)", refusalText(Refusal::kBusy), phaseName(expected),
                        trigger ? trigger : "");
        return;
    }
    g_trigger = trigger ? trigger : "";
    Log::get().note("blend probe: armed (trigger %s): %u frames are captured from the next frame boundary.", g_trigger.c_str(), kFrames);
}

void blendProbeFrameBoundary(uint32_t gameFrame) noexcept {
    const int p = g_phase.load(std::memory_order_acquire);
    if (p == kIdle) return;
    try {
        if (p == kArmed) {
            releaseAll();
            for (Slot& s : g_slots) s.cands.reserve(kMaxCandidates);   // no reallocation mid-frame; freed again by releaseAll
            g_cur = 0;
            g_readTicks = 0;
            g_startFrame = gameFrame + 1;
            g_queryFailed = false;
            g_queryFailHr = S_OK;
            g_probeFaults = 0;
            g_gridWhy.clear();
            g_gridNeeded.store(true, std::memory_order_release);
            g_phase.store(kCollecting, std::memory_order_release);
            Log::get().note("blend probe: collecting %u frames from game frame %u.", kFrames, g_startFrame);
        } else if (p == kCollecting) {
            ++g_cur;
            if (g_cur >= kFrames) {
                g_gridNeeded.store(false, std::memory_order_release);
                g_phase.store(kReading, std::memory_order_release);
                g_readTicks = 0;
                pollQueries();
                pollGrids();
                if (outstandingQueries() == 0 && outstandingGrids() == 0) finishCapture();
            } else {
                g_gridNeeded.store(true, std::memory_order_release);
            }
        } else if (p == kReading) {
            ++g_readTicks;
            pollQueries();
            pollGrids();
            if ((outstandingQueries() == 0 && outstandingGrids() == 0) || g_readTicks >= kReadTicksMax) finishCapture();
        }
    } catch (...) {
        Log::get().note("blend probe: the capture faulted at a frame boundary and was abandoned.");
        releaseAll();
        g_gridNeeded.store(false, std::memory_order_release);
        g_phase.store(kIdle, std::memory_order_release);
    }
}

bool blendProbeWantsDraws() noexcept {
    return g_phase.load(std::memory_order_relaxed) == kCollecting;
}

void blendProbeDraw(ID3D11DeviceContext* ctx, const BlendDrawInfo& info, BlendReplayFn replay, void* arg) noexcept {
    if (g_phase.load(std::memory_order_relaxed) != kCollecting) return;
    try {
        drawImpl(ctx, info, replay, arg);
    } catch (...) {
        ++g_probeFaults;
    }
}

void blendProbeNoteIndirect() noexcept {
    if (g_phase.load(std::memory_order_relaxed) == kCollecting && g_cur < kFrames) ++g_slots[g_cur].counts.indirect;
}

bool blendProbeGridWanted() noexcept {
    return g_gridNeeded.load(std::memory_order_relaxed);
}

void blendProbeGridRefuse(const char* why) noexcept {
    if (g_phase.load(std::memory_order_acquire) != kCollecting) return;
    try {
        if (g_gridWhy.empty()) g_gridWhy = why ? why : "";
        if (g_cur < kFrames) g_slots[g_cur].grid.offered = true;
        g_gridNeeded.store(false, std::memory_order_release);
    } catch (...) {
    }
}

namespace {
// One input's crop, copied to a staging texture. false (and nothing kept) when the source is absent or not the expected format.
bool stageOne(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, DXGI_FORMAT expect, const D3D11_BOX& box,
              uint32_t w, uint32_t h, ID3D11Texture2D** out) {
    if (!src) return false;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (d.Format != expect) return false;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Width = w;
    sd.Height = h;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, out)) || !*out) {
        *out = nullptr;
        return false;
    }
    ctx->CopySubresourceRegion(*out, 0, 0, 0, 0, src, 0, &box);
    return true;
}
}  // namespace

void blendProbeGridOffer(ID3D11DeviceContext* ctx, const BlendGridIn& in) noexcept {
    if (g_phase.load(std::memory_order_acquire) != kCollecting || g_cur >= kFrames) return;
    try {
        g_gridNeeded.store(false, std::memory_order_release);
        Grid& g = g_slots[g_cur].grid;
        if (g.offered) return;
        g.offered = true;
        if (!ctx || in.w == 0 || in.h == 0 || !in.decision) {
            if (g_gridWhy.empty())
                g_gridWhy = !in.decision ? "the pass's decision texture was not built this frame (the capture-only shader variant or its texture was unavailable)"
                                         : "the pass offered an empty input";
            return;
        }
        const uint32_t cw = std::min(kWindow, in.w), ch = std::min(kWindow, in.h);
        const uint32_t x0 = (in.w - cw) / 2, y0 = (in.h - ch) / 2;
        D3D11_BOX box{x0, y0, 0, x0 + cw, y0 + ch, 1};
        Com<ID3D11Device> dev;
        ctx->GetDevice(dev.put());
        if (!dev.get()) {
            if (g_gridWhy.empty()) g_gridWhy = "no device";
            return;
        }
        g.have[0] = stageOne(dev.get(), ctx, in.decision, DXGI_FORMAT_R32G32B32A32_FLOAT, box, cw, ch, &g.staging[0]);
        g.have[1] = stageOne(dev.get(), ctx, in.depth, DXGI_FORMAT_R32_FLOAT, box, cw, ch, &g.staging[1]);
        g.have[2] = stageOne(dev.get(), ctx, in.coverage, DXGI_FORMAT_R8_UNORM, box, cw, ch, &g.staging[2]);
        g.have[3] = stageOne(dev.get(), ctx, in.bias, DXGI_FORMAT_R8_UNORM, box, cw, ch, &g.staging[3]);
        if (!g.have[0]) {
            if (g_gridWhy.empty()) g_gridWhy = "the decision texture could not be staged (format or allocation)";
            for (int k = 0; k < 4; ++k)
                if (g.staging[k]) {
                    g.staging[k]->Release();
                    g.staging[k] = nullptr;
                    g.have[k] = false;
                }
            return;
        }
        g.staged = true;
        g.winX = x0;
        g.winY = y0;
        g.winW = cw;
        g.winH = ch;
        g.inW = in.w;
        g.inH = in.h;
        g.sceneFrame = in.sceneFrame;
        g.rotDeg = rotationDegrees(in.rotTrace);
        g.transLen = in.transLen;
        g.transValid = in.transValid;
        holdCtx(ctx);
    } catch (...) {
        ++g_probeFaults;
    }
}

}  // namespace edvr
