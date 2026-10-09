#include "skin_join_gpu.h"

#include "cs_stage_save.h"
#include "gpu_census.h"
#include "skin_entity_hook.h"
#include "temporal_shader_bytecode.h"
#include "../common/log.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace edvr {

using Microsoft::WRL::ComPtr;
using namespace skinjoin;

struct SkinJoinGpu::Impl {
    // device-bound resources
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> join, poseClear, poseScatter, poseVerify;
    ComPtr<ID3D11Buffer> jobs, plan, info[2], joinTable, stats, owner, pose[2], poseCb, nullJoin;
    ComPtr<ID3D11ShaderResourceView> jobsSrv, planSrv, infoSrv[2], poseSrv[2], joinSrv, nullJoinSrv;
    ComPtr<ID3D11UnorderedAccessView> joinUav, infoUav[2], statsUav, ownerUav, poseUav[2];
    ComPtr<ID3D11Buffer> statsStage[3];
    uint64_t statsStageAt[3] = {};            // the chain frame count when the copy was queued (0 = free)
    bool created = false, failed = false;
    // state
    JoinFeeder feeder;
    PaletteHistory history;
    std::unique_ptr<Snapshot> snapshot = std::make_unique<Snapshot>();
    std::unique_ptr<Plan> plan_ = std::make_unique<Plan>();
    uint32_t parity = 1;                       // flips at each chain dispatch: the tables of THIS frame are [parity], last frame's [parity ^ 1]
    uint32_t joinPresent = ~0u;                // the present frame JoinCS last ran in
    bool joinHistory = false;                  // ...with its history certified
    uint32_t poseBuiltPresent = ~0u;           // the present frame the pose table [parity] was built for
    uint32_t poseOwnerPresent = ~0u;           // the present frame whose first snapshot owns the scatter (a refresh of it scatters again)
    ComPtr<ID3D11Buffer> prevJobs;
    ComPtr<ID3D11ShaderResourceView> prevJobsSrv;
    ComPtr<ID3D11Buffer> curPalette, prevPalette;
    std::map<ID3D11Buffer*, std::pair<ComPtr<ID3D11Buffer>, ComPtr<ID3D11ShaderResourceView>>> paletteViews;
    // counters
    uint32_t chainRefused = 0, overJobs = 0, createFailed = 0;
    uint32_t statsLast[kStatWords] = {};       // the cumulative GPU counters at the last window
    uint32_t statsNewest[kStatWords] = {};     // the newest read-back
    bool statsHave = false;
    JoinFeeder::Counters feederLast;
    PaletteHistory::Counters historyLast;
    uint64_t framesLast = 0;
    bool chainRefusedNoted = false;
};

SkinJoinGpu::SkinJoinGpu() : impl_(std::make_unique<Impl>()) {}
SkinJoinGpu::~SkinJoinGpu() = default;

namespace {
ComPtr<ID3D11Buffer> makeBuffer(ID3D11Device* dev, UINT bytes, UINT bind, UINT misc, UINT stride) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    d.MiscFlags = misc;
    d.StructureByteStride = stride;
    ComPtr<ID3D11Buffer> b;
    if (FAILED(dev->CreateBuffer(&d, nullptr, &b))) return nullptr;
    return b;
}
ComPtr<ID3D11ShaderResourceView> rawSrv(ID3D11Device* dev, ID3D11Buffer* b, UINT words) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    d.BufferEx.NumElements = words;
    d.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    ComPtr<ID3D11ShaderResourceView> v;
    dev->CreateShaderResourceView(b, &d, &v);
    return v;
}
ComPtr<ID3D11ShaderResourceView> structuredSrv(ID3D11Device* dev, ID3D11Buffer* b, UINT n) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    d.Buffer.NumElements = n;
    ComPtr<ID3D11ShaderResourceView> v;
    dev->CreateShaderResourceView(b, &d, &v);
    return v;
}
ComPtr<ID3D11UnorderedAccessView> rawUav(ID3D11Device* dev, ID3D11Buffer* b, UINT words) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = words;
    d.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    ComPtr<ID3D11UnorderedAccessView> v;
    dev->CreateUnorderedAccessView(b, &d, &v);
    return v;
}
ComPtr<ID3D11UnorderedAccessView> structuredUav(ID3D11Device* dev, ID3D11Buffer* b, UINT n) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = n;
    ComPtr<ID3D11UnorderedAccessView> v;
    dev->CreateUnorderedAccessView(b, &d, &v);
    return v;
}

bool create(SkinJoinGpu::Impl& s, ID3D11Device* dev) {
    s.device = dev;
    const UINT raw = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, structured = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    const UINT srvUav = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateComputeShader(kSkinJoinBytecode, sizeof(kSkinJoinBytecode), nullptr, &s.join)) ||
        FAILED(dev->CreateComputeShader(kSkinPoseClearBytecode, sizeof(kSkinPoseClearBytecode), nullptr, &s.poseClear)) ||
        FAILED(dev->CreateComputeShader(kSkinPoseScatterBytecode, sizeof(kSkinPoseScatterBytecode), nullptr, &s.poseScatter)) ||
        FAILED(dev->CreateComputeShader(kSkinPoseVerifyBytecode, sizeof(kSkinPoseVerifyBytecode), nullptr, &s.poseVerify))) return false;
    s.jobs = nullptr;
    s.prevJobs = makeBuffer(dev, kMaxJobs * 16, D3D11_BIND_SHADER_RESOURCE, structured, 16);
    s.plan = makeBuffer(dev, kPlanWords * 4, D3D11_BIND_SHADER_RESOURCE, raw, 0);
    s.joinTable = makeBuffer(dev, kMaxRows * 4, srvUav, structured, 4);
    s.nullJoin = makeBuffer(dev, 64, D3D11_BIND_SHADER_RESOURCE, structured, 4);
    s.stats = makeBuffer(dev, kStatWords * 4, D3D11_BIND_UNORDERED_ACCESS, raw, 0);
    s.owner = makeBuffer(dev, kMaxRows * 4, D3D11_BIND_UNORDERED_ACCESS, raw, 0);
    s.poseCb = makeBuffer(dev, 16, D3D11_BIND_CONSTANT_BUFFER, 0, 0);
    for (int i = 0; i < 2; ++i) {
        s.info[i] = makeBuffer(dev, kMaxRows * 8, srvUav, raw, 0);
        s.pose[i] = makeBuffer(dev, kMaxRows * 32, srvUav, structured, 32);
    }
    if (!s.prevJobs || !s.plan || !s.joinTable || !s.nullJoin || !s.stats || !s.owner || !s.poseCb || !s.info[0] || !s.info[1] || !s.pose[0] || !s.pose[1]) return false;
    s.prevJobsSrv = structuredSrv(dev, s.prevJobs.Get(), kMaxJobs);
    s.planSrv = rawSrv(dev, s.plan.Get(), kPlanWords);
    s.joinSrv = structuredSrv(dev, s.joinTable.Get(), kMaxRows);
    s.nullJoinSrv = structuredSrv(dev, s.nullJoin.Get(), 16);
    s.joinUav = structuredUav(dev, s.joinTable.Get(), kMaxRows);
    s.statsUav = rawUav(dev, s.stats.Get(), kStatWords);
    s.ownerUav = rawUav(dev, s.owner.Get(), kMaxRows);
    for (int i = 0; i < 2; ++i) {
        s.infoSrv[i] = rawSrv(dev, s.info[i].Get(), kMaxRows * 2);
        s.infoUav[i] = rawUav(dev, s.info[i].Get(), kMaxRows * 2);
        s.poseSrv[i] = structuredSrv(dev, s.pose[i].Get(), kMaxRows);
        s.poseUav[i] = structuredUav(dev, s.pose[i].Get(), kMaxRows);
    }
    if (!s.prevJobsSrv || !s.planSrv || !s.joinSrv || !s.nullJoinSrv || !s.joinUav || !s.statsUav || !s.ownerUav) return false;
    for (int i = 0; i < 2; ++i) if (!s.infoSrv[i] || !s.infoUav[i] || !s.poseSrv[i] || !s.poseUav[i]) return false;
    for (int i = 0; i < 3; ++i) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = kStatWords * 4;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&d, nullptr, &s.statsStage[i]))) return false;
    }
    // Zero the counters and the tables once (the first frame reads them as "last frame's").
    const uint32_t zeros[kStatWords] = {};
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    ctx->UpdateSubresource(s.stats.Get(), 0, nullptr, zeros, 0, 0);
    const uint32_t clear[4] = {0, 0, 0, 0};
    for (int i = 0; i < 2; ++i) {
        ctx->ClearUnorderedAccessViewUint(s.infoUav[i].Get(), clear);
        ctx->ClearUnorderedAccessViewUint(s.poseUav[i].Get(), clear);
    }
    const uint32_t nullData[16] = {};
    ctx->UpdateSubresource(s.nullJoin.Get(), 0, nullptr, nullData, 0, 0);
    return true;
}

ComPtr<ID3D11ShaderResourceView> paletteView(SkinJoinGpu::Impl& s, ID3D11Buffer* buffer) {
    auto it = s.paletteViews.find(buffer);
    if (it != s.paletteViews.end()) return it->second.second;
    if (s.paletteViews.size() >= 6) s.paletteViews.clear();   // two are live; the game makes a new pair only on growth
    D3D11_BUFFER_DESC d{};
    buffer->GetDesc(&d);
    ComPtr<ID3D11ShaderResourceView> v = structuredSrv(s.device.Get(), buffer, d.ByteWidth / 48);
    s.paletteViews[buffer] = {ComPtr<ID3D11Buffer>(buffer), v};
    return v;
}
}  // namespace

void SkinJoinGpu::onChain(ID3D11DeviceContext* ctx, uint32_t present, uint32_t groups) {
    Impl& s = *impl_;
    s.joinHistory = false;
    if (s.failed || !ctx || !groups) return;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!s.created) {
        if (!create(s, dev.Get())) {
            s.failed = true;
            ++s.createFailed;
            Log::get().note("skin join: the GPU resources could not be made; the join is off (no history for any skinned character).");
            return;
        }
        s.created = true;
    } else if (s.device.Get() != dev.Get()) {
        s.failed = true;
        return;
    }
    // The game's own bindings at the chain dispatch: t0 the job table, u0 the palette it writes.
    ComPtr<ID3D11ShaderResourceView> t0;
    ctx->CSGetShaderResources(0, 1, &t0);
    ComPtr<ID3D11UnorderedAccessView> u0;
    ctx->CSGetUnorderedAccessViews(0, 1, &u0);
    ComPtr<ID3D11Buffer> jobsBuffer, paletteBuffer;
    D3D11_BUFFER_DESC jd{}, pd{};
    bool ok = t0 && u0;
    if (ok) {
        ComPtr<ID3D11Resource> r0, r1;
        t0->GetResource(&r0);
        u0->GetResource(&r1);
        ok = r0 && r1 && SUCCEEDED(r0.As(&jobsBuffer)) && SUCCEEDED(r1.As(&paletteBuffer));
    }
    if (ok) {
        jobsBuffer->GetDesc(&jd);
        paletteBuffer->GetDesc(&pd);
        ok = jd.StructureByteStride == 16 && (jd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) && jd.ByteWidth >= groups * 16u &&
             pd.StructureByteStride == 48 && (pd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
    }
    if (!ok) {
        ++s.chainRefused;
        if (!s.chainRefusedNoted) {
            s.chainRefusedNoted = true;
            Log::get().note("skin join: the palette chain's bindings are not what the ledger measured (t0 a 16-byte structured job table, u0 a 48-byte structured palette); no join this frame.");
        }
        return;
    }
    ++chainFrames_;
    const uint32_t jobs = std::min<uint32_t>(groups, kMaxJobs);
    if (groups > kMaxJobs) ++s.overJobs;
    // The hook's newest list, and what it says about the rows in use.
    Snapshot& snap = *s.snapshot;
    const bool haveSnap = skinEntityHookLatest(snap);
    const uint32_t rowsInUse = haveSnap && checkSnapshot(snap) ? snap.end : kMaxRows;
    const bool poseLast = s.poseBuiltPresent == present - 1u;
    const uint32_t verdict = s.history.note(present, reinterpret_cast<uint64_t>(paletteBuffer.Get()), pd.ByteWidth, rowsInUse, poseLast);
    s.prevPalette = s.curPalette;
    s.curPalette = paletteBuffer;
    const bool history = verdict == kHistoryOk && groups <= kMaxJobs;
    s.parity ^= 1u;
    Plan& plan = *s.plan_;
    s.feeder.step(haveSnap ? &snap : nullptr, GetCurrentThreadId(), history, jobs, s.parity, plan);
    std::vector<uint32_t> words;
    plan.words(words);
    // The pass.
    {
        GpuCensusScope census(ctx, GpuCensusSection::FrameEngineVelocity);
        ctx->UpdateSubresource(s.plan.Get(), 0, nullptr, words.data(), 0, 0);
        CsStageSave saved;
        saved.save(ctx);
        const uint32_t cur = s.parity & 1u, prev = cur ^ 1u;
        ID3D11ShaderResourceView* srvs[5] = {t0.Get(), s.prevJobsSrv.Get(), s.planSrv.Get(), s.infoSrv[prev].Get(), s.poseSrv[prev].Get()};
        ID3D11UnorderedAccessView* uavs[4] = {s.joinUav.Get(), s.infoUav[cur].Get(), s.statsUav.Get(), s.ownerUav.Get()};
        ctx->CSSetShader(s.join.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 5, srvs);
        ctx->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
        ctx->Dispatch(1, 1, 1);
        ID3D11UnorderedAccessView* none[4] = {};
        ctx->CSSetUnorderedAccessViews(0, 4, none, nullptr);
        ID3D11ShaderResourceView* noSrv[5] = {};
        ctx->CSSetShaderResources(0, 5, noSrv);
        saved.restore(ctx);
        // last frame's job table for the next frame's prefix compare (a boxed copy: a whole number of 16-byte rows)
        const D3D11_BOX box{0, 0, 0, jobs * 16u, 1, 1};
        ctx->CopySubresourceRegion(s.prevJobs.Get(), 0, 0, 0, 0, jobsBuffer.Get(), 0, &box);
    }
    s.joinPresent = present;
    s.joinHistory = history;
    // Counters back to the CPU, a few times a minute and never waited for.
    if (chainFrames_ % 120 == 0) {
        for (int i = 0; i < 3; ++i) if (!s.statsStageAt[i]) {
            ctx->CopyResource(s.statsStage[i].Get(), s.stats.Get());
            s.statsStageAt[i] = chainFrames_;
            break;
        }
    }
}

void SkinJoinGpu::scatterPose(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* poolSrv, uint32_t records, uint32_t present) {
    Impl& s = *impl_;
    if (!s.created || s.failed || !ctx || !poolSrv || !records) return;
    // Only the present frame the join ran in (the table [parity] is this frame's), once per snapshot.
    if (s.joinPresent != present) return;
    ++poseScatters_;
    GpuCensusScope census(ctx, GpuCensusSection::FrameEngineVelocity);
    CsStageSave saved;
    saved.save(ctx);
    const uint32_t cur = s.parity & 1u;
    const uint32_t cb[4] = {records, kMaxRows, 0, 0};
    ctx->UpdateSubresource(s.poseCb.Get(), 0, nullptr, cb, 0, 0);
    ID3D11UnorderedAccessView* uavs[5] = {nullptr, nullptr, s.statsUav.Get(), nullptr, s.poseUav[cur].Get()};
    ctx->CSSetShaderResources(5, 1, &poolSrv);
    ctx->CSSetConstantBuffers(0, 1, s.poseCb.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
    ctx->CSSetShader(s.poseClear.Get(), nullptr, 0);
    ctx->Dispatch((kMaxRows + 63) / 64, 1, 1);
    ctx->CSSetShader(s.poseScatter.Get(), nullptr, 0);
    ctx->Dispatch((records + 63) / 64, 1, 1);
    ctx->CSSetShader(s.poseVerify.Get(), nullptr, 0);
    ctx->Dispatch((records + 63) / 64, 1, 1);
    ID3D11UnorderedAccessView* none[5] = {};
    ctx->CSSetUnorderedAccessViews(0, 5, none, nullptr);
    ID3D11ShaderResourceView* noSrv = nullptr;
    ctx->CSSetShaderResources(5, 1, &noSrv);
    saved.restore(ctx);
    s.poseBuiltPresent = present;
}

SkinViews SkinJoinGpu::views(uint32_t present) const {
    const Impl& s = *impl_;
    SkinViews v;
    if (!s.created || s.failed) return v;
    v.join = s.nullJoinSrv.Get();
    const bool live = s.joinPresent == present && s.joinHistory && s.prevPalette;
    if (!live) return v;
    Impl& m = *impl_;
    v.prevPalette = paletteView(m, s.prevPalette.Get()).Get();
    v.join = s.joinSrv.Get();
    v.prevPose = s.poseSrv[(s.parity & 1u) ^ 1u].Get();
    v.live = v.prevPalette != nullptr;
    if (!v.live) v.join = s.nullJoinSrv.Get();
    return v;
}

SkinWindow SkinJoinGpu::takeWindow(ID3D11DeviceContext* ctx) {
    Impl& s = *impl_;
    SkinWindow w;
    if (!s.created || s.failed || !ctx) return w;
    // The newest finished read-back becomes the window's end.
    for (int i = 0; i < 3; ++i) {
        if (!s.statsStageAt[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(s.statsStage[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)) || !m.pData) continue;
        std::memcpy(s.statsNewest, m.pData, sizeof(s.statsNewest));
        ctx->Unmap(s.statsStage[i].Get(), 0);
        s.statsStageAt[i] = 0;
        s.statsHave = true;
    }
    if (!s.statsHave) return w;
    for (uint32_t k = 0; k < kStatWords; ++k) {
        const bool state = k == kStatPrevHookOk || k == kStatLastJobs || k == kStatLastEntities;
        const bool bits = k == kStatMismatchBits;
        w.gpu[k] = state || bits ? s.statsNewest[k] : s.statsNewest[k] - s.statsLast[k];
    }
    std::memcpy(s.statsLast, s.statsNewest, sizeof(s.statsLast));
    const JoinFeeder::Counters& f = s.feeder.counters();
    w.cpu.steps = f.steps - s.feederLast.steps;
    w.cpu.offered = f.offered - s.feederLast.offered;
    w.cpu.sameThread = f.sameThread - s.feederLast.sameThread;
    w.cpu.otherThread = f.otherThread - s.feederLast.otherThread;
    for (uint32_t k = 0; k < kDeclineCount; ++k) w.cpu.decline[k] = f.decline[k] - s.feederLast.decline[k];
    const PaletteHistory::Counters& h = s.history.counters();
    for (uint32_t k = 0; k < kHistoryCount; ++k) w.cpu.history[k] = h.verdict[k] - s.historyLast.verdict[k];
    s.feederLast = f;
    s.historyLast = h;
    w.chainRefused = s.chainRefused;
    w.overJobs = s.overJobs;
    w.createFailed = s.createFailed;
    w.frames = chainFrames_ - s.framesLast;
    s.framesLast = chainFrames_;
    w.valid = true;
    return w;
}

void SkinJoinGpu::release() {
    impl_ = std::make_unique<Impl>();
    chainFrames_ = poseScatters_ = 0;
}

}  // namespace edvr
