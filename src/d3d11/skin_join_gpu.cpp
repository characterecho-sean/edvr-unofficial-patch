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

namespace {
constexpr uint32_t kInstCopyBytes = 1u << 22;   // the largest span of the instance stream one pose table copies (the game's whole stream is under 1 MB)
constexpr uint32_t kStatStages = 4;             // counter read-backs in flight (a chain frame each; the GPU is a few frames behind at most)
struct CpuAt {                                  // the CPU's counters as they stood at a chain frame
    JoinFeeder::Counters feeder;
    PaletteHistory::Counters history;
    uint64_t poseBuilds = 0, poseIncomplete = 0;
};
}  // namespace

struct SkinJoinGpu::Impl {
    // device-bound resources
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> join, poseClear, poseRefMark, poseScatter, poseScatterRest, poseVerify, poseFinish;
    ComPtr<ID3D11Buffer> jobs, plan, info[2], joinTable, stats, owner, pose[2], poseCb, nullJoin;
    ComPtr<ID3D11Buffer> instCopy, ranges, refBits, baseState;
    ComPtr<ID3D11ShaderResourceView> jobsSrv, planSrv, infoSrv[2], poseSrv[2], joinSrv, nullJoinSrv, instSrv, rangesSrv;
    ComPtr<ID3D11UnorderedAccessView> joinUav, infoUav[2], statsUav, ownerUav, poseUav[2], refUav, stateUav;
    // The counters' read-back ring: a copy of the GPU counters queued at EVERY chain frame into a free slot, tagged with that chain frame and the CPU's
    // counters as they stood then. A window is the span between two finished slots, so its GPU and CPU halves describe the same frames.
    struct Stage {
        ComPtr<ID3D11Buffer> buf;
        uint64_t at = 0;                          // the chain frame the copy was queued at (0 = free)
        CpuAt cpu;
    } stage[kStatStages];
    CpuAt cpuNewest, cpuLast;
    uint64_t framesNewest = 0;                    // the chain frame the newest finished read-back is of
    uint64_t poseBuilds = 0, poseIncomplete = 0;  // pose tables built / built with no complete reference list
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
    ComPtr<ID3D11Buffer> prevJobs;
    ComPtr<ID3D11ShaderResourceView> prevJobsSrv;
    ComPtr<ID3D11Buffer> curPalette, prevPalette;
    std::map<ID3D11Buffer*, std::pair<ComPtr<ID3D11Buffer>, ComPtr<ID3D11ShaderResourceView>>> paletteViews;
    // counters
    uint32_t chainRefused = 0, overJobs = 0, createFailed = 0;
    uint32_t statsLast[kStatWords] = {};       // the cumulative GPU counters at the last window
    uint32_t statsNewest[kStatWords] = {};     // the newest finished read-back
    bool statsHave = false;
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
        FAILED(dev->CreateComputeShader(kSkinPoseRefMarkBytecode, sizeof(kSkinPoseRefMarkBytecode), nullptr, &s.poseRefMark)) ||
        FAILED(dev->CreateComputeShader(kSkinPoseScatterBytecode, sizeof(kSkinPoseScatterBytecode), nullptr, &s.poseScatter)) ||
        FAILED(dev->CreateComputeShader(kSkinPoseScatterRestBytecode, sizeof(kSkinPoseScatterRestBytecode), nullptr, &s.poseScatterRest)) ||
        FAILED(dev->CreateComputeShader(kSkinPoseVerifyBytecode, sizeof(kSkinPoseVerifyBytecode), nullptr, &s.poseVerify)) ||
        FAILED(dev->CreateComputeShader(kSkinPoseFinishBytecode, sizeof(kSkinPoseFinishBytecode), nullptr, &s.poseFinish))) return false;
    s.jobs = nullptr;
    s.prevJobs = makeBuffer(dev, kMaxJobs * 16, D3D11_BIND_SHADER_RESOURCE, structured, 16);
    s.plan = makeBuffer(dev, kPlanWords * 4, D3D11_BIND_SHADER_RESOURCE, raw, 0);
    s.joinTable = makeBuffer(dev, kMaxRows * 4, srvUav, structured, 4);
    s.nullJoin = makeBuffer(dev, 64, D3D11_BIND_SHADER_RESOURCE, structured, 4);
    s.stats = makeBuffer(dev, kStatWords * 4, D3D11_BIND_UNORDERED_ACCESS, raw, 0);
    s.owner = makeBuffer(dev, kMaxRows * 4, D3D11_BIND_UNORDERED_ACCESS, raw, 0);
    s.poseCb = makeBuffer(dev, 32, D3D11_BIND_CONSTANT_BUFFER, 0, 0);
    s.instCopy = makeBuffer(dev, kInstCopyBytes, D3D11_BIND_SHADER_RESOURCE, raw, 0);
    s.refBits = makeBuffer(dev, (kRefWords + 1) * 4, D3D11_BIND_UNORDERED_ACCESS, raw, 0);
    s.baseState = makeBuffer(dev, kMaxRows * 4, D3D11_BIND_UNORDERED_ACCESS, raw, 0);
    {
        // the draw list: dynamic, written whole (a boxed UpdateSubresource on a buffer is dropped by WARP) by a discarding Map
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = kMaxRanges * 8;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        d.MiscFlags = structured;
        d.StructureByteStride = 8;
        if (FAILED(dev->CreateBuffer(&d, nullptr, &s.ranges))) return false;
    }
    for (int i = 0; i < 2; ++i) {
        s.info[i] = makeBuffer(dev, kMaxRows * 8, srvUav, raw, 0);
        s.pose[i] = makeBuffer(dev, kMaxRows * 32, srvUav, structured, 32);
    }
    if (!s.prevJobs || !s.plan || !s.joinTable || !s.nullJoin || !s.stats || !s.owner || !s.poseCb || !s.info[0] || !s.info[1] || !s.pose[0] || !s.pose[1] ||
        !s.instCopy || !s.refBits || !s.baseState || !s.ranges) return false;
    s.prevJobsSrv = structuredSrv(dev, s.prevJobs.Get(), kMaxJobs);
    s.planSrv = rawSrv(dev, s.plan.Get(), kPlanWords);
    s.joinSrv = structuredSrv(dev, s.joinTable.Get(), kMaxRows);
    s.nullJoinSrv = structuredSrv(dev, s.nullJoin.Get(), 16);
    s.joinUav = structuredUav(dev, s.joinTable.Get(), kMaxRows);
    s.statsUav = rawUav(dev, s.stats.Get(), kStatWords);
    s.ownerUav = rawUav(dev, s.owner.Get(), kMaxRows);
    s.instSrv = rawSrv(dev, s.instCopy.Get(), kInstCopyBytes / 4);
    s.rangesSrv = structuredSrv(dev, s.ranges.Get(), kMaxRanges);
    s.refUav = rawUav(dev, s.refBits.Get(), kRefWords + 1);
    s.stateUav = rawUav(dev, s.baseState.Get(), kMaxRows);
    for (int i = 0; i < 2; ++i) {
        s.infoSrv[i] = rawSrv(dev, s.info[i].Get(), kMaxRows * 2);
        s.infoUav[i] = rawUav(dev, s.info[i].Get(), kMaxRows * 2);
        s.poseSrv[i] = structuredSrv(dev, s.pose[i].Get(), kMaxRows);
        s.poseUav[i] = structuredUav(dev, s.pose[i].Get(), kMaxRows);
    }
    if (!s.prevJobsSrv || !s.planSrv || !s.joinSrv || !s.nullJoinSrv || !s.joinUav || !s.statsUav || !s.ownerUav || !s.instSrv || !s.rangesSrv || !s.refUav || !s.stateUav) return false;
    for (int i = 0; i < 2; ++i) if (!s.infoSrv[i] || !s.infoUav[i] || !s.poseSrv[i] || !s.poseUav[i]) return false;
    for (uint32_t i = 0; i < kStatStages; ++i) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = kStatWords * 4;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&d, nullptr, &s.stage[i].buf))) return false;
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

// Take every finished counter read-back, oldest first, without waiting; the newest becomes the window's end. A slot still in flight ends the scan (the
// later ones were queued after it).
void pollStats(SkinJoinGpu::Impl& s, ID3D11DeviceContext* ctx) {
    for (uint32_t pass = 0; pass < kStatStages; ++pass) {
        SkinJoinGpu::Impl::Stage* oldest = nullptr;
        for (auto& st : s.stage) if (st.at && (!oldest || st.at < oldest->at)) oldest = &st;
        if (!oldest) return;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(oldest->buf.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)) || !m.pData) return;
        std::memcpy(s.statsNewest, m.pData, sizeof(s.statsNewest));
        ctx->Unmap(oldest->buf.Get(), 0);
        s.cpuNewest = oldest->cpu;
        s.framesNewest = oldest->at;
        oldest->at = 0;
        s.statsHave = true;
    }
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
    // The hook's newest list, and what it says about the rows in use. The dispatch tells the hook first that its job table has jobs: an empty list is
    // evidence against the hook's offsets only against such a table (a stand-down it causes ends the newest list here).
    Snapshot& snap = *s.snapshot;
    skinEntityHookNoteChain(groups);
    const bool haveSnap = skinEntityHookLatest(snap);
    // The rows the list says are in use (0 = no usable list: the plan's prevRows, checked per job on the GPU, covers that case).
    const uint32_t rowsInUse = haveSnap && checkSnapshot(snap) ? snap.end : 0;
    const uint64_t previousBytes = s.history.lastBytes();   // the previous palette buffer, before this frame is noted
    const bool poseLast = s.poseBuiltPresent == present - 1u;
    const uint32_t verdict = s.history.note(present, reinterpret_cast<uint64_t>(paletteBuffer.Get()), pd.ByteWidth, rowsInUse, poseLast);
    s.prevPalette = s.curPalette;
    s.curPalette = paletteBuffer;
    const bool history = verdict == kHistoryOk && groups <= kMaxJobs;
    s.parity ^= 1u;
    Plan& plan = *s.plan_;
    s.feeder.step(haveSnap ? &snap : nullptr, GetCurrentThreadId(), history, jobs, s.parity, plan);
    plan.prevRows = uint32_t(std::min<uint64_t>(previousBytes / 48u, kMaxRows));
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
    // Counters back to the CPU at EVERY chain frame, never waited for: the finished read-backs are taken first, then this frame's copy goes into a free
    // slot with this chain frame's number and the CPU's counters as they stand now (this frame's history verdict and feeder step are in them). A
    // window is the span between two finished read-backs, so its GPU and CPU halves are the same frames (the F12 flight's line paired the GPU's counters
    // with those of a window later and read "no history 1320 of 1320" beside "views live 2315").
    pollStats(s, ctx);
    for (auto& st : s.stage) {
        if (st.at) continue;
        ctx->CopyResource(st.buf.Get(), s.stats.Get());
        st.at = chainFrames_;
        st.cpu.feeder = s.feeder.counters();
        st.cpu.history = s.history.counters();
        st.cpu.poseBuilds = s.poseBuilds;
        st.cpu.poseIncomplete = s.poseIncomplete;
        break;
    }
}

void SkinJoinGpu::buildPose(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* poolSrv, uint32_t records, uint32_t present, const SkinRefs& refs) {
    Impl& s = *impl_;
    if (!s.created || s.failed || !ctx || !poolSrv || !records) return;
    // Only the present frame the join ran in (the table [parity] is this frame's), once.
    if (s.joinPresent != present) return;
    ++poseScatters_;
    // Is there an exact list of what the frame's skinned draws read? Anything short of exact is "every record decides" (the rule before the list existed):
    // never a guess about which record is live.
    const char* why = "";
    uint32_t minStart = 0, maxEnd = 0;
    bool exact = refs.complete;
    if (!refs.complete) why = refs.why && *refs.why ? refs.why : "a skinned draw was seen that the list cannot hold";
    else if (records > kMaxPoolRecords) { exact = false; why = "the pool has more records than the bitmap covers"; }
    else if (refs.pairs > kMaxRanges) { exact = false; why = "more skinned draws than the list holds"; }
    else if (refs.pairs && (!refs.ranges || !refs.stream)) { exact = false; why = "no instance stream was named"; }
    if (exact && refs.pairs) {
        minStart = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < refs.pairs && exact; ++i) {
            const uint32_t start = refs.ranges[i * 2], count = refs.ranges[i * 2 + 1];
            if (count > kMaxRangeInstances || uint64_t(start) + count > 0x0FFFFFFFull) { exact = false; why = "a draw names too many instances"; break; }
            minStart = std::min(minStart, start);
            maxEnd = std::max(maxEnd, start + count);
        }
        if (exact && maxEnd <= minStart) { exact = false; why = "the listed draws name no instance"; }
    }
    D3D11_BOX box{};
    if (exact && refs.pairs) {
        D3D11_BUFFER_DESC sd{};
        refs.stream->GetDesc(&sd);
        const uint64_t from = uint64_t(refs.streamOffset) + uint64_t(minStart) * kInstanceStride, to = uint64_t(refs.streamOffset) + uint64_t(maxEnd) * kInstanceStride;
        if (to > sd.ByteWidth) { exact = false; why = "the instance stream is shorter than its draws say"; }
        else if (to - from > kInstCopyBytes) { exact = false; why = "the instance stream's span is larger than the copy"; }
        else box = D3D11_BOX{UINT(from), 0, 0, UINT(to), 1, 1};
    }
    if (exact && refs.pairs) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(s.ranges.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) { exact = false; why = "the draw list could not be uploaded"; }
        else {
            std::memcpy(m.pData, refs.ranges, size_t(refs.pairs) * 8u);
            ctx->Unmap(s.ranges.Get(), 0);
        }
    }
    ++s.poseBuilds;
    if (!refs.complete || !exact) { ++s.poseIncomplete; inexactWhy_ = why; }
    else inexactWhy_ = "";
    GpuCensusScope census(ctx, GpuCensusSection::FrameEngineVelocity);
    CsStageSave saved;
    saved.save(ctx);
    if (exact && refs.pairs) ctx->CopySubresourceRegion(s.instCopy.Get(), 0, 0, 0, 0, refs.stream, 0, &box);
    const uint32_t cur = s.parity & 1u;
    const uint32_t entries = exact && refs.pairs ? maxEnd - minStart : 0u;
    const uint32_t cb[8] = {records, kMaxRows, exact ? refs.pairs : 0u, minStart, exact ? 1u : 0u, entries, 0, 0};
    ctx->UpdateSubresource(s.poseCb.Get(), 0, nullptr, cb, 0, 0);
    ID3D11UnorderedAccessView* uavs[7] = {nullptr, nullptr, s.statsUav.Get(), nullptr, s.poseUav[cur].Get(), s.refUav.Get(), s.stateUav.Get()};
    ID3D11ShaderResourceView* srvs[3] = {poolSrv, s.instSrv.Get(), s.rangesSrv.Get()};
    ctx->CSSetShaderResources(5, 3, srvs);
    ctx->CSSetConstantBuffers(0, 1, s.poseCb.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0, 7, uavs, nullptr);
    ctx->CSSetShader(s.poseClear.Get(), nullptr, 0);
    ctx->Dispatch((kMaxRows + 63) / 64, 1, 1);
    if (exact && refs.pairs) {
        ctx->CSSetShader(s.poseRefMark.Get(), nullptr, 0);
        ctx->Dispatch((refs.pairs + 63) / 64, 1, 1);
    }
    ctx->CSSetShader(s.poseScatter.Get(), nullptr, 0);
    ctx->Dispatch((records + 63) / 64, 1, 1);
    ctx->CSSetShader(s.poseScatterRest.Get(), nullptr, 0);
    ctx->Dispatch((records + 63) / 64, 1, 1);
    ctx->CSSetShader(s.poseVerify.Get(), nullptr, 0);
    ctx->Dispatch((records + 63) / 64, 1, 1);
    ctx->CSSetShader(s.poseFinish.Get(), nullptr, 0);
    ctx->Dispatch((kMaxRows + 63) / 64, 1, 1);
    ID3D11UnorderedAccessView* none[7] = {};
    ctx->CSSetUnorderedAccessViews(0, 7, none, nullptr);
    ID3D11ShaderResourceView* noSrv[3] = {};
    ctx->CSSetShaderResources(5, 3, noSrv);
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
    // The newest finished read-back becomes the window's end, with the CPU counters of the same chain frame.
    pollStats(s, ctx);
    if (!s.statsHave) return w;
    for (uint32_t k = 0; k < kStatWords; ++k) {
        const bool state = k == kStatPrevHookOk || k == kStatLastJobs || k == kStatLastEntities;
        const bool bits = k == kStatMismatchBits;
        w.gpu[k] = state || bits ? s.statsNewest[k] : s.statsNewest[k] - s.statsLast[k];
    }
    std::memcpy(s.statsLast, s.statsNewest, sizeof(s.statsLast));
    const JoinFeeder::Counters& f = s.cpuNewest.feeder;
    const JoinFeeder::Counters& fl = s.cpuLast.feeder;
    w.cpu.steps = f.steps - fl.steps;
    w.cpu.offered = f.offered - fl.offered;
    w.cpu.sameThread = f.sameThread - fl.sameThread;
    w.cpu.otherThread = f.otherThread - fl.otherThread;
    for (uint32_t k = 0; k < kDeclineCount; ++k) w.cpu.decline[k] = f.decline[k] - fl.decline[k];
    const PaletteHistory::Counters& h = s.cpuNewest.history;
    const PaletteHistory::Counters& hl = s.cpuLast.history;
    for (uint32_t k = 0; k < kHistoryCount; ++k) w.cpu.history[k] = h.verdict[k] - hl.verdict[k];
    w.cpu.poseBuilds = s.cpuNewest.poseBuilds - s.cpuLast.poseBuilds;
    w.cpu.poseIncomplete = s.cpuNewest.poseIncomplete - s.cpuLast.poseIncomplete;
    s.cpuLast = s.cpuNewest;
    w.chainRefused = s.chainRefused;
    w.overJobs = s.overJobs;
    w.createFailed = s.createFailed;
    w.frames = s.framesNewest - s.framesLast;
    s.framesLast = s.framesNewest;
    w.valid = true;
    return w;
}

void SkinJoinGpu::release() {
    impl_ = std::make_unique<Impl>();
    chainFrames_ = poseScatters_ = 0;
}

}  // namespace edvr
