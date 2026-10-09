// skin_join_gpu_test: JoinCS and the pose passes (src/d3d11/skin_join_shader.h) on WARP, against their CPU twin (src/d3d11/skin_join.h cpuJoin).
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test  every check; the optional argument is the repository root (unused)
//
// Cases ("G<case>.<what>"; mutants.py names the case that must catch each mutation):
//   G1  the numbers in the HLSL text are the numbers in skin_join.h
//   G2  a steady world through hook and prefix: join table, by-base table, pose table and every counter, frame by frame, equal the CPU's
//   G3  scripted changes (insert, remove, children, disagreement, pose, cap, duplicate base, no history): the same
//   G4  two hundred frames of random worlds and random faults: the same
//   G5  the pose passes: records to the table, conflicts killed, out-of-range bases skipped
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "skin_join.h"
#include "skin_join_shader.h"
#include "../skin_join_test/skin_join_world.h"

using Microsoft::WRL::ComPtr;
using namespace edvr::skinjoin;
using namespace skin_join_world;

namespace {
unsigned g_checks = 0, g_failures = 0;
void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
        std::fflush(stdout);
    }
}

struct PoseElem { uint32_t a[4], b[4]; };

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed * 2654435761u + 99u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
    bool chance(uint32_t percent) { return below(100) < percent; }
};

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> join, poseClear, poseScatter, poseVerify;
    ComPtr<ID3D11Buffer> jobs, prevJobs, plan, info[2], joinTable, stats, owner, pose[2], pool, poseCb;
    ComPtr<ID3D11ShaderResourceView> jobsSrv, prevJobsSrv, planSrv, infoSrv[2], poseSrv[2], poolSrv;
    ComPtr<ID3D11UnorderedAccessView> joinUav, infoUav[2], statsUav, ownerUav, poseUav[2];
    uint32_t poolCapacity = 4096;
    bool ok = false;
    std::string why;

    ComPtr<ID3DBlob> compile(const char* entry) {
        ComPtr<ID3DBlob> code, errors;
        const HRESULT hr = D3DCompile(edvr::kSkinJoinCsHlsl, std::strlen(edvr::kSkinJoinCsHlsl), "skin_join", nullptr, nullptr, entry, "cs_5_0", 0, 0, &code, &errors);
        if (FAILED(hr)) {
            why = std::string(entry) + ": " + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
            return nullptr;
        }
        return code;
    }
    ComPtr<ID3D11Buffer> buffer(UINT bytes, UINT bind, UINT misc, UINT stride = 0) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = bytes;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = bind;
        d.MiscFlags = misc;
        d.StructureByteStride = stride;
        ComPtr<ID3D11Buffer> b;
        if (FAILED(dev->CreateBuffer(&d, nullptr, &b))) why = "CreateBuffer";
        return b;
    }
    ComPtr<ID3D11ShaderResourceView> rawSrv(ID3D11Buffer* b, UINT words) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        d.BufferEx.NumElements = words;
        d.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        ComPtr<ID3D11ShaderResourceView> v;
        if (FAILED(dev->CreateShaderResourceView(b, &d, &v))) why = "raw SRV";
        return v;
    }
    ComPtr<ID3D11ShaderResourceView> structuredSrv(ID3D11Buffer* b, UINT n) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        d.Buffer.NumElements = n;
        ComPtr<ID3D11ShaderResourceView> v;
        if (FAILED(dev->CreateShaderResourceView(b, &d, &v))) why = "structured SRV";
        return v;
    }
    ComPtr<ID3D11UnorderedAccessView> rawUav(ID3D11Buffer* b, UINT words) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = words;
        d.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        ComPtr<ID3D11UnorderedAccessView> v;
        if (FAILED(dev->CreateUnorderedAccessView(b, &d, &v))) why = "raw UAV";
        return v;
    }
    ComPtr<ID3D11UnorderedAccessView> structuredUav(ID3D11Buffer* b, UINT n) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = n;
        ComPtr<ID3D11UnorderedAccessView> v;
        if (FAILED(dev->CreateUnorderedAccessView(b, &d, &v))) why = "structured UAV";
        return v;
    }

    bool create() {
        D3D_FEATURE_LEVEL level{};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx))) {
            why = "WARP";
            return false;
        }
        struct { const char* name; ComPtr<ID3D11ComputeShader>* out; } entries[] = {
            {"join", &join}, {"poseClear", &poseClear}, {"poseScatter", &poseScatter}, {"poseVerify", &poseVerify}};
        for (auto& e : entries) {
            auto code = compile(e.name);
            if (!code || FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, e.out->GetAddressOf()))) {
                if (why.empty()) why = std::string("CreateComputeShader ") + e.name;
                return false;
            }
        }
        const UINT raw = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, structured = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        const UINT srvUav = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        jobs = buffer(kMaxJobs * 16, D3D11_BIND_SHADER_RESOURCE, structured, 16);
        prevJobs = buffer(kMaxJobs * 16, D3D11_BIND_SHADER_RESOURCE, structured, 16);
        plan = buffer(kPlanWords * 4, D3D11_BIND_SHADER_RESOURCE, raw);
        joinTable = buffer(kMaxRows * 4, srvUav, structured, 4);
        stats = buffer(kStatWords * 4, D3D11_BIND_UNORDERED_ACCESS, raw);
        owner = buffer(kMaxRows * 4, D3D11_BIND_UNORDERED_ACCESS, raw);
        for (int i = 0; i < 2; ++i) {
            info[i] = buffer(kMaxRows * 8, srvUav, raw);
            pose[i] = buffer(kMaxRows * 32, srvUav, structured, 32);
        }
        pool = buffer(poolCapacity * 336, D3D11_BIND_SHADER_RESOURCE, structured, 336);
        poseCb = buffer(16, D3D11_BIND_CONSTANT_BUFFER, 0);
        if (!jobs || !prevJobs || !plan || !joinTable || !stats || !owner || !info[0] || !info[1] || !pose[0] || !pose[1] || !pool || !poseCb) return false;
        jobsSrv = structuredSrv(jobs.Get(), kMaxJobs);
        prevJobsSrv = structuredSrv(prevJobs.Get(), kMaxJobs);
        planSrv = rawSrv(plan.Get(), kPlanWords);
        poolSrv = structuredSrv(pool.Get(), poolCapacity);
        joinUav = structuredUav(joinTable.Get(), kMaxRows);
        statsUav = rawUav(stats.Get(), kStatWords);
        ownerUav = rawUav(owner.Get(), kMaxRows);
        for (int i = 0; i < 2; ++i) {
            infoSrv[i] = rawSrv(info[i].Get(), kMaxRows * 2);
            infoUav[i] = rawUav(info[i].Get(), kMaxRows * 2);
            poseSrv[i] = structuredSrv(pose[i].Get(), kMaxRows);
            poseUav[i] = structuredUav(pose[i].Get(), kMaxRows);
        }
        ok = why.empty() && jobsSrv && prevJobsSrv && planSrv && poolSrv && joinUav && statsUav && ownerUav && infoSrv[0] && infoSrv[1] && infoUav[0] && infoUav[1] && poseSrv[0] &&
             poseSrv[1] && poseUav[0] && poseUav[1];
        const uint32_t zero[kStatWords]{};
        ctx->UpdateSubresource(stats.Get(), 0, nullptr, zero, 0, 0);
        return ok;
    }

    std::vector<uint8_t> read(ID3D11Buffer* src, UINT bytes) {
        D3D11_BUFFER_DESC d{};
        src->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.MiscFlags = 0;
        d.StructureByteStride = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> st;
        if (FAILED(dev->CreateBuffer(&d, nullptr, &st))) return {};
        ctx->CopyResource(st.Get(), src);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return {};
        std::vector<uint8_t> out(bytes);
        std::memcpy(out.data(), m.pData, bytes);
        ctx->Unmap(st.Get(), 0);
        return out;
    }

    // One chain dispatch: the plan the CPU built, the job table, then the join.
    void runJoin(const Plan& plan_, const std::vector<JobRow>& table) {
        std::vector<JobRow> padded(kMaxJobs);
        for (size_t i = 0; i < table.size() && i < kMaxJobs; ++i) padded[i] = table[i];
        ctx->UpdateSubresource(jobs.Get(), 0, nullptr, padded.data(), 0, 0);
        std::vector<uint32_t> words;
        plan_.words(words);
        ctx->UpdateSubresource(plan.Get(), 0, nullptr, words.data(), 0, 0);
        const uint32_t cur = plan_.parity & 1u, prev = cur ^ 1u;
        ID3D11ShaderResourceView* srvs[5] = {jobsSrv.Get(), prevJobsSrv.Get(), planSrv.Get(), infoSrv[prev].Get(), poseSrv[prev].Get()};
        ID3D11UnorderedAccessView* uavs[4] = {joinUav.Get(), infoUav[cur].Get(), statsUav.Get(), ownerUav.Get()};
        ctx->CSSetShader(join.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 5, srvs);
        ctx->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
        ctx->Dispatch(1, 1, 1);
        ID3D11UnorderedAccessView* none[4]{};
        ctx->CSSetUnorderedAccessViews(0, 4, none, nullptr);
        ID3D11ShaderResourceView* noSrv[5]{};
        ctx->CSSetShaderResources(0, 5, noSrv);
        ctx->CopyResource(prevJobs.Get(), jobs.Get());
    }

    // The pose table of `parity` from the pool records.
    void runPose(uint32_t parity, const std::vector<uint32_t>& records) {   // records: 84 words each
        const uint32_t n = uint32_t(records.size() / 84);
        if (n) {
            // a whole-buffer update reads the buffer's full size from the source
            std::vector<uint32_t> padded(size_t(poolCapacity) * 84, 0u);
            std::memcpy(padded.data(), records.data(), std::min(records.size(), padded.size()) * 4);
            ctx->UpdateSubresource(pool.Get(), 0, nullptr, padded.data(), 0, 0);
        }
        const uint32_t cb[4] = {n, kMaxRows, 0, 0};
        ctx->UpdateSubresource(poseCb.Get(), 0, nullptr, cb, 0, 0);
        ID3D11UnorderedAccessView* uavs[5] = {nullptr, nullptr, statsUav.Get(), nullptr, poseUav[parity].Get()};
        ctx->CSSetShaderResources(5, 1, poolSrv.GetAddressOf());
        ctx->CSSetConstantBuffers(0, 1, poseCb.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
        ctx->CSSetShader(poseClear.Get(), nullptr, 0);
        ctx->Dispatch((kMaxRows + 63) / 64, 1, 1);
        if (n) {
            ctx->CSSetShader(poseScatter.Get(), nullptr, 0);
            ctx->Dispatch((n + 63) / 64, 1, 1);
            ctx->CSSetShader(poseVerify.Get(), nullptr, 0);
            ctx->Dispatch((n + 63) / 64, 1, 1);
        }
        ID3D11UnorderedAccessView* none[5]{};
        ctx->CSSetUnorderedAccessViews(0, 5, none, nullptr);
        ID3D11ShaderResourceView* noSrv = nullptr;
        ctx->CSSetShaderResources(5, 1, &noSrv);
    }
};

std::vector<uint32_t> poolFor(const std::vector<JobRow>& jobs, bool all, uint32_t skipEvery = 0) {
    std::vector<uint32_t> records;
    uint32_t k = 0;
    for (const JobRow& j : jobs) {
        ++k;
        if (!all || (skipEvery && k % skipEvery == 0)) continue;
        std::vector<uint32_t> r(84, 0);
        r[0] = j.dst;
        r[1] = 0x3F800000u;
        r[2] = 0x80008000u;
        r[3] = 0xFFFF8000u;
        r[4] = j.dst * 3u; r[5] = j.dst * 5u; r[6] = j.dst * 7u;
        records.insert(records.end(), r.begin(), r.end());
        // a character's records share a base: a second record with identical words is no conflict
        if (j.dst % 3 == 0) records.insert(records.end(), r.begin(), r.end());
    }
    return records;
}

// The CPU twin of the pose passes.
std::vector<PoseElem> cpuPose(const std::vector<uint32_t>& records, uint32_t* scattered, uint32_t* conflicts) {
    std::vector<PoseElem> t(kMaxRows);
    std::memset(t.data(), 0, t.size() * sizeof(PoseElem));
    *scattered = *conflicts = 0;
    const uint32_t n = uint32_t(records.size() / 84);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t* r = &records[size_t(i) * 84];
        if (r[0] == 0 || r[0] >= kMaxRows) continue;
        std::memcpy(&t[r[0]], r, 32);
        ++*scattered;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t* r = &records[size_t(i) * 84];
        if (r[0] == 0 || r[0] >= kMaxRows) continue;
        if (std::memcmp(&t[r[0]], r, 28) != 0) { ++*conflicts; t[r[0]].a[0] = 0; }
    }
    return t;
}

// The two sides in lockstep.
struct Pair {
    Gpu gpu;
    Run cpu;
    uint32_t statsBefore[kStatWords]{};
    unsigned frames = 0;
    bool started = false;

    bool init() { return gpu.create(); }
    // One frame. Returns the first difference ("" when none).
    std::string frame(const Built& b, bool history = true, bool poseAll = true, bool offer = true, uint32_t skipPose = 0) {
        cpu.frame(b, history, poseAll, offer);
        // the CPU side fills its pose table from "all jobs"; skipPose makes the GPU pool and the CPU table agree on a subset
        const std::vector<uint32_t> records = poolFor(b.jobs, poseAll, skipPose);
        {
            uint32_t sc = 0, cf = 0;
            const auto expect = cpuPose(records, &sc, &cf);
            std::fill(cpu.prevPose.begin(), cpu.prevPose.end(), 0u);
            for (uint32_t i = 0; i < kMaxRows; ++i) cpu.prevPose[i] = expect[i].a[0];
        }
        const uint32_t parity = cpu.plan.parity;
        gpu.runJoin(cpu.plan, b.jobs);
        gpu.runPose(parity, records);
        ++frames;
        // compare
        const auto statsBytes = gpu.read(gpu.stats.Get(), kStatWords * 4);
        uint32_t after[kStatWords];
        std::memcpy(after, statsBytes.data(), sizeof(after));
        char msg[256];
        for (uint32_t i = 0; i < kStatWords; ++i) {
            if (i == kStatPoseRecords || i == kStatPoseConflicts) continue;
            const bool state = i == kStatPrevHookOk || i == kStatLastJobs || i == kStatLastEntities;
            const bool bits = i == kStatMismatchBits;
            const uint32_t gpuValue = state ? after[i] : bits ? (after[i] & ~statsBefore[i]) | (after[i] & cpu.result.stats[i]) : after[i] - statsBefore[i];
            const uint32_t want = cpu.result.stats[i];
            if (bits ? (after[i] != (statsBefore[i] | want)) : (gpuValue != want)) {
                std::snprintf(msg, sizeof(msg), "stat %u: gpu %u cpu %u", i, bits ? after[i] : gpuValue, bits ? (statsBefore[i] | want) : want);
                std::string all = msg;
                all += " [gpu after:";
                for (uint32_t k = 0; k < kStatWords; ++k) all += " " + std::to_string(after[k]);
                all += " | cpu:";
                for (uint32_t k = 0; k < kStatWords; ++k) all += " " + std::to_string(cpu.result.stats[k]);
                all += "]";
                {
                    const auto ob = gpu.read(gpu.owner.Get(), kMaxRows * 4);
                    const uint32_t* o = reinterpret_cast<const uint32_t*>(ob.data());
                    all += " owner:";
                    for (const JobRow& j : b.jobs) all += " [" + std::to_string(j.dst) + "]=" + std::to_string(o[j.dst]);
                }
                std::memcpy(statsBefore, after, sizeof(after));
                return all;
            }
        }
        std::memcpy(statsBefore, after, sizeof(after));
        const auto joinBytes = gpu.read(gpu.joinTable.Get(), kMaxRows * 4);
        const uint32_t* join = reinterpret_cast<const uint32_t*>(joinBytes.data());
        for (uint32_t i = 0; i < kMaxRows; ++i)
            if (join[i] != cpu.result.join[i]) {
                std::snprintf(msg, sizeof(msg), "join[%u]: gpu %u cpu %u", i, join[i], cpu.result.join[i]);
                return msg;
            }
        const auto infoBytes = gpu.read(gpu.info[parity].Get(), kMaxRows * 8);
        const uint32_t* info = reinterpret_cast<const uint32_t*>(infoBytes.data());
        for (uint32_t i = 0; i < kMaxRows; ++i)
            if (info[2 * i] != cpu.result.dstInfo[i].bind || info[2 * i + 1] != cpu.result.dstInfo[i].count) {
                std::snprintf(msg, sizeof(msg), "info[%u]: gpu (%u,%u) cpu (%u,%u)", i, info[2 * i], info[2 * i + 1], cpu.result.dstInfo[i].bind, cpu.result.dstInfo[i].count);
                return msg;
            }
        const auto poseBytes = gpu.read(gpu.pose[parity].Get(), kMaxRows * 32);
        uint32_t sc = 0, cf = 0;
        const auto expect = cpuPose(records, &sc, &cf);
        if (std::memcmp(poseBytes.data(), expect.data(), kMaxRows * 32) != 0) return "pose table differs";
        return "";
    }
};

// ---- G1 ------------------------------------------------------------------------------------------------------------------
unsigned defineOf(const char* name) {
    const std::string text = edvr::kSkinJoinCsHlsl;
    const std::string key = std::string("#define ") + name + " ";
    const size_t at = text.find(key);
    if (at == std::string::npos) return 0xDEADBEEFu;
    return unsigned(std::strtoul(text.c_str() + at + key.size(), nullptr, 10));
}
void caseNumbers() {
    check(defineOf("SJ_MAX_ROWS") == kMaxRows && defineOf("SJ_MAX_ENTRIES") == kMaxEntries && defineOf("SJ_MAX_JOBS") == kMaxJobs, "G1.a the table limits in the HLSL are the header's");
    check(defineOf("SJ_PLAN_RS") == kPlanRsAt && defineOf("SJ_PLAN_COUNT") == kPlanCountAt && defineOf("SJ_PLAN_PREVIDX") == kPlanPrevIdxAt && defineOf("SJ_PLAN_PREVRS") == kPlanPrevRsAt,
          "G1.b the plan's word offsets in the HLSL are the header's");
    check(defineOf("SJ_PLAN_HISTORY") == kPlanHistory && defineOf("SJ_PLAN_HOOK") == kPlanHook, "G1.c the plan's flags are the header's");
    const struct { const char* name; unsigned value; } stats[] = {
        {"SJ_STAT_FRAMES", kStatFrames}, {"SJ_STAT_HOOK_USED", kStatHookUsed}, {"SJ_STAT_PREFIX_USED", kStatPrefixUsed}, {"SJ_STAT_NO_HISTORY", kStatNoHistory},
        {"SJ_STAT_HOOK_DISAGREE", kStatHookDisagree}, {"SJ_STAT_PREV_NOT_VERIFIED", kStatPrevNotVerified}, {"SJ_STAT_JOBS", kStatJobs}, {"SJ_STAT_JOINED", kStatJoined},
        {"SJ_STAT_FAIL_NO_PREV", kStatFailNoPrevEntity}, {"SJ_STAT_FAIL_RANGE", kStatFailRange}, {"SJ_STAT_FAIL_LAYOUT", kStatFailLayout}, {"SJ_STAT_FAIL_PREFIX", kStatFailPrefix},
        {"SJ_STAT_FAIL_POSE", kStatFailPose}, {"SJ_STAT_FAIL_CAP", kStatFailCap}, {"SJ_STAT_DUP_BASE", kStatDupBase}, {"SJ_STAT_PREV_HOOK_OK", kStatPrevHookOk},
        {"SJ_STAT_MISMATCH_BITS", kStatMismatchBits}, {"SJ_STAT_POSE_RECORDS", kStatPoseRecords}, {"SJ_STAT_POSE_CONFLICTS", kStatPoseConflicts},
        {"SJ_STAT_LAST_JOBS", kStatLastJobs}, {"SJ_STAT_LAST_ENTITIES", kStatLastEntities}, {"SJ_STAT_FAIL_PREV_ROWS", kStatFailPrevRows}, {"SJ_STAT_WORDS", kStatWords}};
    bool all = true;
    for (const auto& s : stats) all = all && defineOf(s.name) == s.value;
    check(all, "G1.d the counters' indices in the HLSL are the header's");
    check(defineOf("SJ_MM_NOT_IN_RANGE") == kMmNotInRange && defineOf("SJ_MM_HEAD_COUNT") == kMmHeadCount && defineOf("SJ_MM_HEADS") == kMmHeads && defineOf("SJ_MM_SUM") == kMmSum &&
          defineOf("SJ_MM_ENTITY_SUM") == kMmEntitySum && defineOf("SJ_MM_NO_PLAN") == kMmNoPlan, "G1.e the disagreement bits in the HLSL are the header's");
}

// ---- G2 ------------------------------------------------------------------------------------------------------------------
void caseSteady(Pair& p) {
    const World w = steady();
    std::string first;
    for (unsigned i = 1; i <= 6 && first.empty(); ++i) first = p.frame(build(w, i));
    check(first.empty(), "G2.a six steady frames: the GPU's join, by-base table, pose table and counters equal the CPU's");
    if (!first.empty()) std::printf("    %s\n", first.c_str());
    check(p.cpu.result.stats[kStatHookUsed] == 1 && p.cpu.result.stats[kStatJoined] == 6, "G2.b (and the CPU side is in hook mode with every job joined)");
    std::string reversed;
    Pair q;
    check(q.init(), "G2.c a second device");
    for (unsigned i = 1; i <= 5 && reversed.empty(); ++i) reversed = q.frame(build(w, i, true));
    check(reversed.empty(), "G2.d a reversed table order: the same");
}

// ---- G3 ------------------------------------------------------------------------------------------------------------------
void caseScripted(Pair& p) {
    World a = steady();
    World removed{a[1], a[2]};
    World inserted;
    inserted.push_back(Ent{150, 0xA11CE, 0, {{5000, 25}}});
    for (const Ent& e : a) inserted.push_back(e);
    World grown = a;
    grown[1].jobs.push_back({2002, 5});
    World swapped = a;
    std::swap(swapped[0].jobs[1], swapped[0].jobs[2]);
    World replaced = a;
    replaced[1].key = 999;
    World rebound = a;
    rebound[0].jobs[1].first = 1111;           // a child replaced by another mesh of the same size
    World primaryRebound = a;
    primaryRebound[1].jobs[0].first = 2999;    // a primary job of another bind and the same size
    struct Step { const World* w; bool history, pose, offer; };
    const Step script[] = {
        {&a, true, true, true},       {&a, true, true, true},        {&a, true, true, true},       {&removed, true, true, true},   {&a, true, true, true},
        {&a, true, true, true},       {&inserted, true, true, true}, {&inserted, true, true, true}, {&grown, true, true, true},      {&a, true, true, true},
        {&swapped, true, true, true}, {&a, true, true, true},        {&replaced, true, true, true}, {&a, true, true, false},        {&a, true, true, true},
        {&a, false, true, true},      {&a, true, true, true},        {&a, true, false, true},       {&a, true, true, true},         {&a, true, true, true},
        {&a, true, true, true},       {&rebound, true, true, true},  {&a, true, true, true},        {&a, true, true, false},        {&primaryRebound, true, true, false},
        {&a, true, true, true},
    };
    std::string bad;
    unsigned seq = 100;
    unsigned index = 0;
    for (const Step& s : script) {
        ++index;
        bad = p.frame(build(*s.w, ++seq), s.history, s.pose, s.offer);
        if (!bad.empty()) { std::printf("    scripted step %u: %s\n", index, bad.c_str()); break; }
    }
    check(bad.empty(), "G3.a twenty-six scripted frames (insert, remove, children grown, traded and rebound, a replaced entity, a missing list, no history, no pose, a primary job rebound under the prefix): equal to the CPU's");
    // faults in the table itself
    Pair q;
    check(q.init(), "G3.b a second device");
    std::string t;
    for (unsigned i = 1; i <= 3 && t.empty(); ++i) t = q.frame(build(a, i));
    Built disagree = build(a, 4);
    disagree.jobs.erase(disagree.jobs.begin() + 1);
    if (t.empty()) t = q.frame(disagree);
    check(t.empty() && q.cpu.result.stats[kStatHookDisagree] == 1, "G3.c a table the list does not tile: the disagreement is found on the GPU as on the CPU");
    Built over = build(a, 5);
    over.jobs.push_back(JobRow{0, kMaxRows - 2, 9, 10});
    over.jobs.push_back(JobRow{0, 7, 9, 0});
    JobRow dup = over.jobs[1];
    dup.bind = 4242;
    over.jobs.push_back(dup);
    if (t.empty()) t = q.frame(over, true, true, false);
    check(t.empty() && q.cpu.result.stats[kStatFailCap] == 2 && q.cpu.result.stats[kStatDupBase] == 1, "G3.d a job past the tables, a job with no bones and a second job on a base: counted alike");
    for (unsigned i = 6; i <= 8 && t.empty(); ++i) t = q.frame(build(a, i));
    if (t.empty()) t = q.frame(build(a, 9), true, true, true, 2);   // every second job has no pose record
    check(t.empty() && q.cpu.result.stats[kStatJoined] == 0 ? true : t.empty(), "G3.e (a frame whose pose table lacks every second job)");
    if (t.empty()) t = q.frame(build(a, 10));
    check(t.empty() && q.cpu.result.stats[kStatFailPose] > 0, "G3.f the frame after it fails the missing bases on the pose, the same on both sides");
    // a primary job whose count differs from its entry's with the entity's total unchanged, and a job outside every entity's range
    for (unsigned i = 11; i <= 13 && t.empty(); ++i) t = q.frame(build(a, i));
    Built headCount = build(a, 14);
    headCount.jobs[0].count += 1;
    headCount.jobs[1].count -= 1;
    if (t.empty()) t = q.frame(headCount);
    check(t.empty() && q.cpu.result.stats[kStatHookDisagree] == 1 && q.cpu.result.stats[kStatMismatchBits] == kMmHeadCount, "G3.g a primary job with another count than its entry (the entity's total unchanged) is a disagreement on the GPU as on the CPU");
    for (unsigned i = 15; i <= 17 && t.empty(); ++i) t = q.frame(build(a, i));
    Built outside = build(a, 18);
    outside.jobs.push_back(JobRow{0, 300, 77, 5});
    if (t.empty()) t = q.frame(outside);
    check(t.empty() && q.cpu.result.stats[kStatHookDisagree] == 1 && q.cpu.result.stats[kStatMismatchBits] == kMmNotInRange, "G3.h a job outside every entity's range is a disagreement on the GPU as on the CPU");
    // the previous palette buffer holds only 64 rows (word 7 of the plan): the jobs whose previous rows run past it fail on that, the same on both sides
    for (unsigned i = 19; i <= 21 && t.empty(); ++i) t = q.frame(build(a, i));
    q.cpu.prevRows = 64;
    if (t.empty()) t = q.frame(build(a, 22));
    check(t.empty() && q.cpu.result.stats[kStatFailPrevRows] == 3 && q.cpu.result.stats[kStatJoined] == 3, "G3.i jobs whose previous rows run past the previous palette buffer are counted and not joined, alike on the GPU and the CPU");
    q.cpu.prevRows = kMaxRows;
    if (!t.empty()) std::printf("    %s\n", t.c_str());
}

// ---- G4 ------------------------------------------------------------------------------------------------------------------
World randomWorld(Rng& r, uint64_t& nextKey) {
    World w;
    const uint32_t n = 1 + r.below(10);
    for (uint32_t i = 0; i < n; ++i) {
        Ent e;
        e.key = nextKey += 1 + r.below(3) * 8;
        e.vtable = 0xA0 + r.below(2);
        const uint32_t jobs = 1 + r.below(4);
        for (uint32_t j = 0; j < jobs; ++j) e.jobs.push_back({1000 + r.below(6), 1 + r.below(60)});
        w.push_back(e);
    }
    return w;
}
void caseRandom(Pair& p) {
    Rng r(2026);
    Rng rowsRng(77);   // its own stream: the worlds above stay what they were
    uint64_t nextKey = 0x7000;
    World w = randomWorld(r, nextKey);
    uint64_t seq = 0;
    std::string bad;
    unsigned hookFrames = 0, prefixFrames = 0, disagree = 0, joined = 0, jobsTotal = 0;
    for (unsigned f = 0; f < 200 && bad.empty(); ++f) {
        // a change or two
        if (r.chance(15) && w.size() > 1) w.erase(w.begin() + r.below(uint32_t(w.size())));
        if (r.chance(15) && w.size() < 12) { World n = randomWorld(r, nextKey); w.insert(w.begin() + r.below(uint32_t(w.size() + 1)), n[0]); }
        if (r.chance(10)) { Ent& e = w[r.below(uint32_t(w.size()))]; e.jobs.push_back({1000 + r.below(6), 1 + r.below(60)}); }
        if (r.chance(10)) { Ent& e = w[r.below(uint32_t(w.size()))]; if (e.jobs.size() > 1) e.jobs.pop_back(); }
        if (r.chance(8)) { Ent& e = w[r.below(uint32_t(w.size()))]; if (e.jobs.size() > 2) std::swap(e.jobs[1], e.jobs[2]); }
        if (r.chance(6)) { Ent& e = w[r.below(uint32_t(w.size()))]; e.key = nextKey += 8; }
        Built b = build(w, ++seq, r.chance(30), 1);
        const bool history = !r.chance(8), pose = !r.chance(6), offer = !r.chance(10);
        p.cpu.prevRows = rowsRng.chance(10) ? 20 + rowsRng.below(120) : kMaxRows;   // now and then a previous palette buffer that is small
        if (r.chance(7)) b.jobs.erase(b.jobs.begin() + r.below(uint32_t(b.jobs.size())));        // the table loses a job the list still names
        if (r.chance(5) && b.jobs.size() > 1) b.jobs[1].count += 1 + r.below(3);                    // a count the list does not know
        if (r.chance(4)) b.snap.flags = kSnapFault;
        if (r.chance(5)) seq += 1 + r.below(2);                                                      // calls the dispatch never saw
        b.snap.seq = seq;
        bad = p.frame(b, history, pose, offer);
        const uint32_t* s = p.cpu.result.stats;
        hookFrames += s[kStatHookUsed]; prefixFrames += s[kStatPrefixUsed]; disagree += s[kStatHookDisagree]; joined += s[kStatJoined]; jobsTotal += s[kStatJobs];
        if (!bad.empty()) std::printf("    random frame %u: %s\n", f, bad.c_str());
    }
    check(bad.empty(), "G4.a two hundred random frames (changes, faults, missing lists, lost history and poses): the GPU equals the CPU on every table and counter");
    check(hookFrames > 20 && prefixFrames > 20 && disagree > 3 && joined > jobsTotal / 4, "G4.b (the run reached the hook join, the prefix join, a disagreement and many joins)");
    std::printf("  skin join gpu: random run %u hook frames, %u prefix frames, %u disagreements, %u of %u jobs joined\n", hookFrames, prefixFrames, disagree, joined, jobsTotal);
}

// ---- G5 ------------------------------------------------------------------------------------------------------------------
void casePose(Pair& p) {
    std::vector<uint32_t> records;
    auto add = [&](uint32_t base, uint32_t salt) {
        std::vector<uint32_t> r(84, 0);
        r[0] = base; r[1] = 0x3F800000u + salt; r[2] = 11; r[3] = 13; r[4] = 17 + salt; r[5] = 19; r[6] = 23; r[7] = 999 + salt;
        records.insert(records.end(), r.begin(), r.end());
    };
    add(5, 0); add(5, 0); add(9, 0); add(0, 0); add(kMaxRows, 0); add(kMaxRows + 77, 0); add(12, 0); add(12, 1);   // base 12: two records that disagree
    add(40, 0); add(40, 0); add(40, 0);
    // word 7 differs only: not a conflict (nothing reads it)
    { std::vector<uint32_t> r(84, 0); r[0] = 60; r[7] = 1; records.insert(records.end(), r.begin(), r.end()); r[7] = 2; records.insert(records.end(), r.begin(), r.end()); }
    const auto statsBefore = p.gpu.read(p.gpu.stats.Get(), kStatWords * 4);
    p.gpu.runPose(1, records);
    const auto got = p.gpu.read(p.gpu.pose[1].Get(), kMaxRows * 32);
    uint32_t sc = 0, cf = 0;
    const auto expect = cpuPose(records, &sc, &cf);
    const PoseElem* g = reinterpret_cast<const PoseElem*>(got.data());
    check(g[5].a[0] == 5 && g[9].a[0] == 9 && g[40].a[0] == 40, "G5.a records land at their base");
    check(g[12].a[0] == 0, "G5.b two records of one base that disagree kill it");
    check(g[60].a[0] == 60, "G5.c a difference in word 7 alone is not a conflict");
    check(g[0].a[0] == 0 && g[1].a[0] == 0, "G5.d base 0 is never written");
    check(expect[12].a[0] == 0 && expect[60].a[0] == 60 && expect[5].a[0] == 5, "G5.e (the CPU twin agrees: base 12 dead, bases 5 and 60 live)");
    const auto stats = p.gpu.read(p.gpu.stats.Get(), kStatWords * 4);
    const uint32_t* s = reinterpret_cast<const uint32_t*>(stats.data());
    const uint32_t* before = reinterpret_cast<const uint32_t*>(statsBefore.data());
    check(s[kStatPoseConflicts] - before[kStatPoseConflicts] >= 1, "G5.f the conflict is counted");
    check(s[kStatPoseRecords] - before[kStatPoseRecords] == sc, "G5.g the records are counted, one each");
    // word 7 may be either writer's; compare everything else exactly
    bool same = true;
    for (uint32_t i = 0; i < kMaxRows && same; ++i) same = std::memcmp(&g[i], &expect[i], 28) == 0;
    check(same, "G5.h every element's words 0..6 equal the CPU twin's");
}
}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--self-test" || a == "--dry-run") selfTest = true;
        else if (i == 2 && argv[1] == std::string("--self-test")) continue;
        else {
            std::fprintf(stderr, "usage: skin_join_gpu_test --self-test [repository root] | --dry-run\n");
            return 2;
        }
    }
    if (!selfTest) {
        std::fprintf(stderr, "usage: skin_join_gpu_test --self-test [repository root] | --dry-run\n");
        return 2;
    }
    caseNumbers();
    static Pair steadyPair, scriptPair, randomPair, posePair;
    if (!steadyPair.init() || !scriptPair.init() || !randomPair.init() || !posePair.init()) {
        std::printf("FAIL: G2.init the GPU rig could not start: %s %s %s %s\n", steadyPair.gpu.why.c_str(), scriptPair.gpu.why.c_str(), randomPair.gpu.why.c_str(), posePair.gpu.why.c_str());
        return 1;
    }
    caseSteady(steadyPair);
    caseScripted(scriptPair);
    caseRandom(randomPair);
    casePose(posePair);
    if (g_failures) {
        std::printf("FAIL: skin join gpu: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u skin join gpu checks\n", g_checks);
    return 0;
}
