#include "cull_gate_probe.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace edvr {

CullGateProbe cullGateProbe;

namespace {

// Engine memory is read under SEH: a wild pointer counts a fault and drops
// the field, never the flight. POD locals only in the __try functions
// (kinematic_eval_hook.cpp's rule: /EHsc units cannot unwind C++ objects
// through __try).
bool rd(void* dst, uintptr_t src, size_t n) noexcept {
    if (!src) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool rdv(T* dst, uintptr_t src) noexcept { return rd(dst, src, sizeof(T)); }

// The engine's own FUN_1404F4E10, guarded: returns false on a fault.
bool frustumCall(CullGateProbe::FrustumFn fn, uintptr_t view, const float* point,
                 const float* interval, int32_t* out) noexcept {
    __try {
        *out = static_cast<int32_t>(static_cast<uint32_t>(fn(view, point, interval)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The builder's transformed centre, exactly as decomp_42B4420.txt:196-237
// forms it from the pose context: c = pose+0x80 (float4), q = pose+0x10,
// t = pose+0x20. The XOR with the sign mask (DAT_144e2f8e0) is a negation.
void builderCentre(const float q[4], const float t[4], const float c[4], float out[4]) noexcept {
    const float w2 = q[3] + q[3];
    const float dot = q[0] * c[0] + q[1] * c[1] + q[2] * c[2];
    const float ww = q[3] * q[3] + q[3] * q[3];
    out[0] = (c[2] * q[1] - c[1] * q[2]) * w2 + ww * c[0] + (-c[0]) + dot * (q[0] + q[0]) + t[0];
    out[1] = (c[0] * q[2] - c[2] * q[0]) * w2 + ww * c[1] + (-c[1]) + dot * (q[1] + q[1]) + t[1];
    out[2] = (c[1] * q[0] - c[0] * q[1]) * w2 + ww * c[2] + (-c[2]) + dot * (q[2] + q[2]) + t[2];
    out[3] = (c[3] * q[3] - c[3] * q[3]) * w2 + ww * c[3] + (-c[3]) + dot * w2 + t[3];
}

}  // namespace

bool CullGateProbe::arm(uint32_t first, FrustumFn frustum, uint8_t visGlobal) noexcept {
    armed_.store(false, std::memory_order_release);
    try {
        if (gate_.size() != kGateCap) gate_.assign(kGateCap, GateObs{});
        if (builder_.size() != kBuilderCap) builder_.assign(kBuilderCap, BuilderObs{});
        if (entries_.size() != kEntryCap) entries_.assign(kEntryCap, EntryObs{});
        if (subs_.size() != kSubItemCap) subs_.assign(kSubItemCap, SubItem{});
        if (parts_.size() != kPartCap) parts_.assign(kPartCap, PartObs{});
        if (!dumps_) dumps_.reset(new ViewDump[kMaxDumps]);
    } catch (...) {
        return false;
    }
    reset();
    first_ = first;
    frustum_ = frustum;
    visGlobal_ = visGlobal;
    partHooked_ = false;   // the caller says, after the attach
    armed_.store(true, std::memory_order_release);
    return true;
}

void CullGateProbe::reset() noexcept {
    for (auto& g : gate_) g.valid = 0;
    for (auto& b : builder_) b.valid = 0;
    for (auto& p : parts_) p.valid = 0;
    if (dumps_) for (uint32_t i = 0; i < kMaxDumps; ++i) dumps_[i].valid = 0;
    for (auto& k : dumpKey_) k.store(0, std::memory_order_relaxed);
    gateNext_.store(0); builderNext_.store(0); entryNext_.store(0); subNext_.store(0); partNext_.store(0);
    gateCalls_.store(0); builderCalls_.store(0); faults_.store(0); recordMismatch_.store(0);
    entriesDropped_.store(0); subItemsDropped_.store(0); dumpsDropped_.store(0); dumpCount_.store(0);
    partCalls_.store(0); partUnverified_.store(0); partForeign_.store(0); partUnlinked_.store(0);
}

bool CullGateProbe::inWindow(uint32_t* frame) const noexcept {
    if (!armed_.load(std::memory_order_acquire)) return false;
    const uint32_t f = frame_.load(std::memory_order_acquire);
    if (f < first_ || f >= first_ + kFrames) return false;
    *frame = f;
    return true;
}

void CullGateProbe::dumpViews(uintptr_t ctx, uint32_t frame) noexcept {
    if (!ctx || !dumps_) return;
    const uint64_t key = static_cast<uint64_t>(ctx) ^ (static_cast<uint64_t>(frame) << 48);
    for (uint32_t i = 0; i < kMaxDumps; ++i)
        if (dumpKey_[i].load(std::memory_order_acquire) == key) return;   // done or in progress
    uint32_t slot = UINT32_MAX;
    try {
        std::lock_guard<std::mutex> lock(dumpMutex_);
        for (uint32_t i = 0; i < kMaxDumps; ++i)
            if (dumpKey_[i].load(std::memory_order_relaxed) == key) return;
        const uint32_t n = dumpCount_.load(std::memory_order_relaxed);
        if (n >= kMaxDumps) { dumpsDropped_.fetch_add(1, std::memory_order_relaxed); return; }
        slot = n;
        dumpKey_[slot].store(key, std::memory_order_release);
        dumpCount_.store(n + 1, std::memory_order_release);
    } catch (...) {
        return;
    }
    ViewDump& d = dumps_[slot];
    d.ctx = ctx;
    d.frame = frame;
    d.faults = 0;
    uint64_t count = 0;
    if (!rdv(&count, ctx + 0x1A940)) ++d.faults;
    d.count = static_cast<uint32_t>(count > kMaxViews ? kMaxViews : count);
    if (!rdv(&d.lodScale, ctx + 0x30)) ++d.faults;
    if (!rd(d.bitTable, ctx + 0x1A840, sizeof(d.bitTable))) ++d.faults;
    for (uint32_t v = 0; v < d.count; ++v) {
        const uintptr_t view = ctx + 0x40 + uintptr_t(v) * kViewBytes;
        if (!rd(d.raw[v], view, kViewBytes)) { ++d.faults; continue; }
        uint64_t planes = 0;
        uint16_t pc = 0;
        std::memcpy(&planes, d.raw[v] + 0x30, 8);
        std::memcpy(&pc, d.raw[v] + 0x44, 2);
        d.planeCount[v] = pc > kMaxPlanes ? kMaxPlanes : pc;
        if (d.planeCount[v] && !rd(d.planes[v], static_cast<uintptr_t>(planes), size_t(d.planeCount[v]) * 16)) {
            ++d.faults;
            d.planeCount[v] = 0;
        }
    }
    if (d.faults) faults_.fetch_add(d.faults, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    d.valid = 1;
}

void CullGateProbe::noteGate(uintptr_t gateCtx, uintptr_t out, uintptr_t view) noexcept {
    uint32_t frame = 0;
    if (!inWindow(&frame)) return;
    gateCalls_.fetch_add(1, std::memory_order_relaxed);
    // decomp_4312040.txt:287-304: the gate context's [0] is the traversal's
    // param_1 (whose [0] is the render context), [2] the record (param_5).
    uintptr_t outer = 0, ctx = 0, record = 0;
    uint32_t lod = 0;
    uint8_t pass = 0;
    const bool ok = rdv(&outer, gateCtx) && rdv(&ctx, outer) && rdv(&record, gateCtx + 0x10) &&
                    rdv(&lod, out) && rdv(&pass, out + 4);
    if (!ok) { faults_.fetch_add(1, std::memory_order_relaxed); return; }
    dumpViews(ctx, frame);
    const uint32_t i = gateNext_.fetch_add(1, std::memory_order_relaxed);
    if (i >= gate_.size()) return;   // dropped: gateCalls - gateKept says how many
    GateObs& g = gate_[i];
    g.record = record;
    g.ctx = ctx;
    g.frame = frame;
    g.lod = lod;
    g.pass = pass;
    g.flags = 0;
    g.view = 0xFFFF;
    const uintptr_t base = ctx + 0x40;
    if (view >= base && (view - base) % kViewBytes == 0 && (view - base) / kViewBytes < kMaxViews)
        g.view = static_cast<uint16_t>((view - base) / kViewBytes);
    else
        g.flags |= 1u;
    std::atomic_thread_fence(std::memory_order_release);
    g.valid = 1;
}

uint32_t CullGateProbe::noteBuilder(uintptr_t pose, uintptr_t ctx, uintptr_t mask, uintptr_t nibbles) noexcept {
    uint32_t frame = 0;
    if (!inWindow(&frame)) return kNoRow;
    builderCalls_.fetch_add(1, std::memory_order_relaxed);
    dumpViews(ctx, frame);
    const uint32_t i = builderNext_.fetch_add(1, std::memory_order_relaxed);
    if (i >= builder_.size()) return kNoRow;
    BuilderObs& b = builder_[i];
    b = BuilderObs{};
    b.frame = frame;
    b.pose = pose;
    b.ctx = ctx;
    b.activeMask = mask;
    // Both callers (FUN_144320340:95, FUN_144321940:86) pass rec+0x210.
    const uintptr_t record = nibbles >= 0x210 ? nibbles - 0x210 : 0;
    b.record = record;
    uint64_t posePtr = 0;
    const bool recOk = rdv(&posePtr, record + 0x290) && rdv(&b.node, record + 0x18) &&
                       rdv(&b.recMask, record + 0x208) && rd(b.nibbles, record + 0x210, sizeof(b.nibbles)) &&
                       rd(b.position, record + 0x170, sizeof(b.position)) && rd(b.quat, record + 0x17C, sizeof(b.quat)) &&
                       rd(b.worldCentre, record + 0x240, 16) && rd(b.localCentre, record + 0x270, 16) &&
                       rd(b.radius, record + 0x280, 16);
    if (!recOk) b.flags |= kFlagRecordFault;
    else if (posePtr != pose) {
        b.flags |= kFlagPoseMismatch;
        recordMismatch_.fetch_add(1, std::memory_order_relaxed);
    }
    const bool poseOk = rd(b.poseQuat, pose + 0x10, 16) && rd(b.poseT, pose + 0x20, 16) &&
                        rd(b.poseCentre, pose + 0x80, 16) && rd(b.poseExtents, pose + 0x90, 16);
    if (!poseOk) b.flags |= kFlagPoseFault;
    else builderCentre(b.poseQuat, b.poseT, b.poseCentre, b.centre);
    // The builder's view loop (decomp_42B4420.txt:250-275), recomputed with
    // the engine's own plane test on the builder's own inputs.
    uint64_t count = 0;
    if (!rdv(&count, ctx + 0x1A940)) b.flags |= kFlagViewsFault;
    b.viewCount = static_cast<uint32_t>(count > kMaxViews ? kMaxViews : count);
    if (!frustum_) b.flags |= kFlagNoFrustum;
    for (uint32_t v = 0; v < b.viewCount; ++v) {
        const uintptr_t view = ctx + 0x40 + uintptr_t(v) * kViewBytes;
        uint64_t viewBits = 0;
        uint8_t viewFlags = 0;
        if (!rdv(&viewBits, view + 0x570) || !rdv(&viewFlags, view + 0x68D)) { b.flags |= kFlagViewsFault; continue; }
        if (!(viewBits & mask)) continue;
        b.bitOk |= 1ull << v;
        if (visGlobal_ && (viewFlags & 1u)) b.visApplies |= 1ull << v;
        if (!frustum_ || !poseOk) continue;
        int32_t r = 0;
        if (!frustumCall(frustum_, view, b.centre, b.poseExtents, &r)) { b.flags |= kFlagViewsFault; continue; }
        if (r != -1) b.frustumPass |= 1ull << v;
        if (r == 1) b.frustumInside |= 1ull << v;
    }
    // The pose context's instance entries and their 32-byte sub-items: the
    // parts whose world transforms become the t33 pool's per-part records.
    uint64_t entryCount = 0, entryBase = 0;
    if (!rdv(&entryCount, pose + 0x48) || !rdv(&entryBase, pose + 0x50)) b.flags |= kFlagEntriesFault;
    b.entryCount = static_cast<uint32_t>(entryCount > 0xFFFFFFFFull ? 0xFFFFFFFFu : entryCount);
    const uint32_t want = b.entryCount > kEntriesPerRecord ? kEntriesPerRecord : b.entryCount;
    if (want < b.entryCount) b.flags |= kFlagEntriesTruncated;
    const uint32_t e0 = want ? entryNext_.fetch_add(want, std::memory_order_relaxed) : 0;
    if (want && (e0 >= entries_.size() || want > entries_.size() - e0)) {
        entriesDropped_.fetch_add(want, std::memory_order_relaxed);
    } else if (want) {
        b.entryFirst = e0;
        for (uint32_t e = 0; e < want; ++e) {
            EntryObs& eo = entries_[e0 + e];
            eo = EntryObs{};
            const uintptr_t entry = static_cast<uintptr_t>(entryBase) + uintptr_t(e) * 0x58;
            uint64_t subBase = 0;
            if (!rdv(&eo.model, entry) || !rdv(&eo.subCount, entry + 0x20) || !rdv(&subBase, entry + 0x28)) {
                b.flags |= kFlagEntriesFault;
                continue;
            }
            // The model's local sphere, what every part test of this entry is
            // made of: +0x00 the centre, +0x10 the radius (param_1[1]'s copy).
            if (eo.model && (!rd(eo.modelCentre, static_cast<uintptr_t>(eo.model), 16) ||
                             !rd(eo.modelSphere, static_cast<uintptr_t>(eo.model) + 0x10, 16)))
                b.flags |= kFlagEntriesFault;
            const uint32_t take = eo.subCount > 4096 ? 4096 : eo.subCount;
            if (!take) continue;
            const uint32_t s0 = subNext_.fetch_add(take, std::memory_order_relaxed);
            if (s0 >= subs_.size() || take > subs_.size() - s0) {
                subItemsDropped_.fetch_add(take, std::memory_order_relaxed);
                continue;
            }
            if (!rd(&subs_[s0], static_cast<uintptr_t>(subBase), size_t(take) * sizeof(SubItem))) {
                b.flags |= kFlagEntriesFault;
                continue;
            }
            eo.subFirst = s0;
            eo.subCopied = take;
        }
        b.entriesCopied = want;
    }
    if (b.flags & (kFlagRecordFault | kFlagPoseFault | kFlagViewsFault | kFlagEntriesFault))
        faults_.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    b.valid = 1;
    return i;
}

void CullGateProbe::notePart(uintptr_t items, uintptr_t out, uintptr_t view, uint32_t builderRow,
                             bool fromBuilder) noexcept {
    if (!armed_.load(std::memory_order_acquire)) return;
    // A part test belongs to the builder call around it: while that call's
    // row was kept, the part takes the row's frame, so a stamp that moves
    // mid-call never splits a record's parts across the window's edge.
    const BuilderObs* owner = nullptr;
    uint32_t frame = 0;
    if (builderRow != kNoRow && builderRow < builder_.size() &&
        builderRow < builderNext_.load(std::memory_order_relaxed) && builder_[builderRow].valid) {
        owner = &builder_[builderRow];
        frame = owner->frame;
    } else if (!inWindow(&frame)) {
        return;
    }
    partCalls_.fetch_add(1, std::memory_order_relaxed);
    // The verdict FUN_1442B3FC0 just wrote (decomp_42B3FC0.txt:106-112) and
    // param_1's six pointers: [0] centre, [1] sphere, [2] pose, [3] the
    // model's +0x40 cell, [4] the LOD table, [5] the render context.
    uint64_t block[6] = {};
    uint32_t lod = 0;
    uint8_t pass = 0;
    if (!rd(block, items, sizeof(block)) || !rdv(&lod, out) || !rdv(&pass, out + 4)) {
        faults_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t i = partNext_.fetch_add(1, std::memory_order_relaxed);
    if (i >= parts_.size()) return;   // dropped: partCalls - partKept - its faults say how many
    PartObs& p = parts_[i];
    p = PartObs{};
    p.frame = frame;
    p.pose = block[2];
    p.lod = lod;
    p.pass = pass;
    const uintptr_t ctx = static_cast<uintptr_t>(block[5]);
    if (!owner) {
        partUnlinked_.fetch_add(1, std::memory_order_relaxed);
        if (fromBuilder) dumpViews(ctx, frame);   // no builder row made this (context, frame)'s dump
    } else {
        p.builderRow = builderRow;
        if (owner->pose != p.pose) p.flags |= kPartOwnerMismatch;
    }
    const uintptr_t base = ctx + 0x40;
    if (view >= base && (view - base) % kViewBytes == 0 && (view - base) / kViewBytes < kMaxViews)
        p.view = static_cast<uint16_t>((view - base) / kViewBytes);
    else
        p.flags |= kPartViewForeign;
    // The sphere as tested and the bits a pass would OR into the item's mask.
    float centre[4] = {}, sphere[4] = {};
    if (rd(centre, static_cast<uintptr_t>(block[0]), 16) && rd(sphere, static_cast<uintptr_t>(block[1]), 16) &&
        rdv(&p.viewBits, view + 0x570)) {
        std::memcpy(p.centre, centre, 12);
        p.radius = sphere[0];
    } else {
        p.flags |= kPartSphereFault;
    }
    // The part's identity: the builder's frame around the call, believed
    // only when every relation holds (kFrame*, cull_gate_probe.h).
    bool verified = false;
    if (!fromBuilder) {
        p.flags |= kPartForeignCaller;
        partForeign_.fetch_add(1, std::memory_order_relaxed);
    } else {
        const auto at = [items](intptr_t off) { return items + static_cast<uintptr_t>(off); };
        uint64_t frameView = 0, framePose = 0, frameCtx = 0, entry = 0, model = 0, sub = 0, meshCell = 0;
        uint64_t entryModel = 0, modelMesh = 0, entryBase = 0, entryCount = 0, subBase = 0, subCount = 0;
        const bool layout = block[0] == at(kFrameCentre) && block[1] == at(kFrameSphere) &&
                            block[3] == at(kFrameMeshCell) && block[4] == at(kFrameLodTable) &&
                            rdv(&frameView, at(kFrameView)) && frameView == view &&
                            rdv(&framePose, at(kFramePose)) && framePose == block[2] &&
                            rdv(&frameCtx, at(kFrameCtx)) && frameCtx == block[5] &&
                            rdv(&entry, at(kFrameEntry)) && rdv(&model, at(kFrameModel)) &&
                            rdv(&sub, at(kFrameSub)) && rdv(&meshCell, at(kFrameMeshCell));
        const bool arrays = layout && rdv(&entryModel, static_cast<uintptr_t>(entry)) && entryModel == model &&
                            rdv(&modelMesh, static_cast<uintptr_t>(model) + 0x40) && modelMesh == meshCell &&
                            rdv(&entryCount, static_cast<uintptr_t>(block[2]) + 0x48) &&
                            rdv(&entryBase, static_cast<uintptr_t>(block[2]) + 0x50) &&
                            rdv(&subCount, static_cast<uintptr_t>(entry) + 0x20) &&
                            rdv(&subBase, static_cast<uintptr_t>(entry) + 0x28);
        if (arrays && entry >= entryBase && (entry - entryBase) % 0x58 == 0 &&
            (entry - entryBase) / 0x58 < entryCount && sub >= subBase && (sub - subBase) % 0x20 == 0 &&
            (sub - subBase) / 0x20 < subCount) {
            verified = true;
            p.subItem = sub;
            p.entry = static_cast<uint32_t>((entry - entryBase) / 0x58);
            p.sub = static_cast<uint32_t>((sub - subBase) / 0x20);
        }
    }
    if (!verified) {
        p.flags |= kPartUnverified;
        partUnverified_.fetch_add(1, std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_release);
    p.valid = 1;
}

CullGateProbe::Counts CullGateProbe::counts() const noexcept {
    Counts c;
    c.gateCalls = gateCalls_.load();
    c.builderCalls = builderCalls_.load();
    const uint32_t gn = gateNext_.load(), bn = builderNext_.load();
    c.gateKept = gn < gate_.size() ? gn : static_cast<uint32_t>(gate_.size());
    c.builderKept = bn < builder_.size() ? bn : static_cast<uint32_t>(builder_.size());
    // Dropped = reserved past the cap; a faulted call never reserves (faults).
    c.gateDropped = gn - c.gateKept;
    c.builderDropped = bn - c.builderKept;
    c.entriesDropped = entriesDropped_.load();
    c.subItemsDropped = subItemsDropped_.load();
    c.faults = faults_.load();
    c.recordMismatch = recordMismatch_.load();
    c.dumps = dumpCount_.load();
    c.dumpsDropped = dumpsDropped_.load();
    c.partCalls = partCalls_.load();
    const uint32_t pn = partNext_.load();
    c.partKept = pn < parts_.size() ? pn : static_cast<uint32_t>(parts_.size());
    c.partDropped = pn - c.partKept;
    c.partUnverified = partUnverified_.load();
    c.partForeign = partForeign_.load();
    c.partUnlinked = partUnlinked_.load();
    return c;
}

uint32_t CullGateProbe::distinctRecords() const noexcept {
    const Counts c = counts();
    try {
        std::vector<uint64_t> r;
        r.reserve(c.builderKept);
        for (uint32_t i = 0; i < c.builderKept; ++i)
            if (builder_[i].valid) r.push_back(builder_[i].record);
        std::sort(r.begin(), r.end());
        return static_cast<uint32_t>(std::unique(r.begin(), r.end()) - r.begin());
    } catch (...) {
        return 0;
    }
}

bool CullGateProbe::write(const wchar_t* path) noexcept {
    FILE* f = nullptr;
    if (!path || _wfopen_s(&f, path, L"wb") || !f) return false;
    std::atomic_thread_fence(std::memory_order_acquire);
    const Counts c = counts();
    bool ok = fwrite("EDVRGATE", 1, 8, f) == 8;
    auto u8 = [&](uint8_t v) { ok = fwrite(&v, 1, 1, f) == 1 && ok; };
    auto u16 = [&](uint16_t v) { ok = fwrite(&v, 2, 1, f) == 1 && ok; };
    auto u32 = [&](uint32_t v) { ok = fwrite(&v, 4, 1, f) == 1 && ok; };
    auto u64 = [&](uint64_t v) { ok = fwrite(&v, 8, 1, f) == 1 && ok; };
    auto raw = [&](const void* p, size_t n) { if (n) ok = fwrite(p, 1, n, f) == n && ok; };
    u32(kVersion);
    u32(first_);
    u32(first_ + kFrames - 1);
    // Flags: bit 0 the builder's plane test ran, bit 1 FUN_1442B3FC0's patch
    // was live (no part rows with it clear is a stand-down, not a quiet run).
    u32((frustum_ ? 1u : 0u) | (partHooked_ ? 2u : 0u));
    u32(visGlobal_);
    // The counters first: a reader states what was dropped before it reads a row.
    for (uint32_t v : {c.gateCalls, c.gateKept, c.gateDropped, c.builderCalls, c.builderKept, c.builderDropped,
                       c.entriesDropped, c.subItemsDropped, c.faults, c.recordMismatch, c.dumps, c.dumpsDropped})
        u32(v);
    // Version 2: the part tests' counters.
    for (uint32_t v : {c.partCalls, c.partKept, c.partDropped, c.partUnverified, c.partForeign, c.partUnlinked})
        u32(v);
    // View dumps.
    uint32_t dumps = 0;
    for (uint32_t i = 0; i < c.dumps && i < kMaxDumps; ++i) dumps += dumps_ && dumps_[i].valid ? 1u : 0u;
    u32(dumps);
    for (uint32_t i = 0; i < c.dumps && i < kMaxDumps; ++i) {
        const ViewDump& d = dumps_[i];
        if (!d.valid) continue;
        u64(d.ctx);
        u32(d.frame);
        u32(d.count);
        raw(&d.lodScale, 4);
        u32(d.faults);
        raw(d.bitTable, sizeof(d.bitTable));
        for (uint32_t v = 0; v < d.count; ++v) {
            u32(d.planeCount[v]);
            raw(d.planes[v], size_t(d.planeCount[v]) * 16);
            raw(d.raw[v], kViewBytes);
        }
    }
    // Gate observations (valid ones only; an entry reserved past the cap was never written).
    uint32_t gates = 0;
    for (uint32_t i = 0; i < c.gateKept; ++i) gates += gate_[i].valid ? 1u : 0u;
    u32(gates);
    for (uint32_t i = 0; i < c.gateKept; ++i) {
        const GateObs& g = gate_[i];
        if (!g.valid) continue;
        u64(g.record); u64(g.ctx); u32(g.frame); u32(g.lod); u16(g.view); u8(g.pass); u8(g.flags);
    }
    // Builder observations. A part row names its builder by the index the
    // row has IN THE FILE (valid rows only), remapped here.
    std::vector<uint32_t> fileRow;
    try { fileRow.assign(c.builderKept, kNoRow); } catch (...) { fileRow.clear(); }
    uint32_t builders = 0;
    for (uint32_t i = 0; i < c.builderKept; ++i) {
        if (!builder_[i].valid) continue;
        if (i < fileRow.size()) fileRow[i] = builders;
        ++builders;
    }
    u32(builders);
    for (uint32_t i = 0; i < c.builderKept; ++i) {
        const BuilderObs& b = builder_[i];
        if (!b.valid) continue;
        u64(b.record); u64(b.pose); u64(b.ctx); u64(b.node); u64(b.activeMask); u64(b.recMask);
        for (uint64_t n : b.nibbles) u64(n);
        u64(b.bitOk); u64(b.frustumPass); u64(b.frustumInside); u64(b.visApplies);
        raw(b.position, 12); raw(b.quat, 8);
        raw(b.worldCentre, 16); raw(b.localCentre, 16); raw(b.radius, 16);
        raw(b.poseQuat, 16); raw(b.poseT, 16); raw(b.poseCentre, 16); raw(b.poseExtents, 16);
        raw(b.centre, 16);
        u32(b.frame); u32(b.viewCount); u32(b.flags); u32(b.entryCount); u32(b.entryFirst); u32(b.entriesCopied);
    }
    const uint32_t en = entryNext_.load();
    const uint32_t entries = en < entries_.size() ? en : static_cast<uint32_t>(entries_.size());
    u32(entries);
    for (uint32_t i = 0; i < entries; ++i) {
        const EntryObs& e = entries_[i];
        u64(e.model); u32(e.subCount); u32(e.subFirst); u32(e.subCopied);
        raw(e.modelCentre, 16); raw(e.modelSphere, 16);   // version 2
    }
    const uint32_t sn = subNext_.load();
    const uint32_t subs = sn < subs_.size() ? sn : static_cast<uint32_t>(subs_.size());
    u32(subs);
    raw(subs_.data(), size_t(subs) * sizeof(SubItem));
    // Version 2: the part tests (valid rows only).
    uint32_t parts = 0;
    for (uint32_t i = 0; i < c.partKept; ++i) parts += parts_[i].valid ? 1u : 0u;
    u32(parts);
    for (uint32_t i = 0; i < c.partKept; ++i) {
        const PartObs& p = parts_[i];
        if (!p.valid) continue;
        u64(p.pose); u64(p.subItem); u64(p.viewBits);
        raw(p.centre, 12); raw(&p.radius, 4);
        u32(p.frame);
        u32(p.builderRow < fileRow.size() ? fileRow[p.builderRow] : kNoRow);
        u32(p.entry); u32(p.sub); u32(p.lod);
        u16(p.view); u8(p.pass); u8(p.flags);
    }
    raw("EDVE", 4);
    ok = !ferror(f) && ok;
    return fclose(f) == 0 && ok;
}

void cullGateProbeGateObserver(uintptr_t gateCtx, uintptr_t out, uintptr_t view) noexcept {
    cullGateProbe.noteGate(gateCtx, out, view);
}

uint32_t cullGateProbeBuilderObserver(uintptr_t pose, uintptr_t ctx, uintptr_t mask, uintptr_t nibbles) noexcept {
    return cullGateProbe.noteBuilder(pose, ctx, mask, nibbles);
}

void cullGateProbePartObserver(uintptr_t items, uintptr_t out, uintptr_t view, uint32_t builderRow,
                               bool fromBuilder) noexcept {
    cullGateProbe.notePart(items, out, view, builderRow, fromBuilder);
}

}  // namespace edvr
