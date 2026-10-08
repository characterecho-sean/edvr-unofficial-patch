#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// RANGE-AWARE INVALIDATION OF THE FIRST-PERSON VERTEX HISTORY (design doc section 104, the 10-07 settlement walk).
//
// The real AnimatedVertexHistory on WARP, under the extended policy the flat adapter uses (capture(...,retainIndex,true)). Every case is a
// scenario and its control: the same records, the same frames, one factor changed (the write has a range, the range is one byte further, the
// extent was read, the policy is the default one) and the opposite outcome. A write that carried a range must spare the records whose bytes it
// did not touch, hit exactly the ones it did, and say which in the counters; a write with no range, or none to name, must still take every
// record that reads the resource, so the old behaviour stays the answer for everything the hooks cannot bound.
struct FlatRangeRow {
    uint64_t first, end;
    bool a, b;   // does the write touch key A / key B (the rig's two keys)
};

inline ComPtr<ID3D11Buffer> flatRangeBuffer(ID3D11Device* dev, const void* data, UINT bytes, UINT bind) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = bytes;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = data;
    ComPtr<ID3D11Buffer> result;
    hr(dev->CreateBuffer(&d, &sd, &result));
    return result;
}

// Is every touching and invalidating cell zero except the one named (and is that one what is wanted)?
inline bool flatRangeOnlyCell(const edvr::HistoryWriteStats& s, unsigned entry, unsigned role, unsigned timing,
                              uint64_t touching, uint64_t invalidating) {
    for (unsigned e = 0; e < edvr::kHistoryWriteEntries; ++e)
        for (unsigned r = 0; r < edvr::kHistoryWriteRoles; ++r)
            for (unsigned t = 0; t < edvr::kHistoryWriteTimings; ++t) {
                const bool here = e == entry && r == role && t == timing;
                if (s.touching[e][r][t] != (here ? touching : 0) || s.invalidating[e][r][t] != (here ? invalidating : 0)) return false;
            }
    return true;
}

template<class Bind>
inline void flatRangeInvalidateTests(ID3D11Device* dev, ID3D11DeviceContext* ctx, Bind bind) {
    using Hist = edvr::AnimatedVertexHistory;
    constexpr unsigned cap = Hist::maxRecords;
    constexpr uint64_t whole = ~uint64_t(0);
    const unsigned kUpdate = unsigned(edvr::HistoryWriteEntry::Update), kUnknown = unsigned(edvr::HistoryWriteEntry::Unknown);
    const unsigned kVertices = unsigned(edvr::HistoryWriteRole::Vertices), kIndices = unsigned(edvr::HistoryWriteRole::Indices);
    const unsigned kGap = unsigned(edvr::HistoryWriteTiming::Gap), kWindow = unsigned(edvr::HistoryWriteTiming::Window);
    ctx->ClearState();
    bind(Pose{}, false);

    // Two keys in one index buffer: A = indices {0,1,2} (bytes [0,12)), B = {0,2,3} (bytes [12,24)); the rest of the buffer is padding that no
    // key reads. The bound vertex buffer is the rig's four vertices at stride 16: A reads vertices 0..2 (bytes [0,48)), B 0..3 ([0,64)).
    std::vector<UINT> indices(40, 0);
    const UINT pair[6]{0, 1, 2, 0, 2, 3};
    std::memcpy(indices.data(), pair, sizeof(pair));
    auto ib = flatHistoryPressureIndexBuffer(dev, indices);
    ctx->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    ComPtr<ID3D11Buffer> vb;
    UINT stride = 0, voffset = 0;
    ctx->IAGetVertexBuffers(1, 1, &vb, &stride, &voffset);
    check(vb && stride == 16 && voffset == 0, "range rig: the bound vertex buffer is the rig's four vertices at stride 16");

    auto capturePair = [&](Hist& h, unsigned frame, int base, Hist::Capture& a, Hist::Capture& b) {
        const bool okA = h.capture(ctx, issue, 3, 1, 0, base, 0, frame, a, true, true);
        const bool okB = h.capture(ctx, issue, 3, 1, 3, base, 0, frame, b, true, true);
        check(okA && okB, "range rig: both keys are admitted by the extended policy");
    };
    // The frame's first act is the advance, as the adapter's beginFrame does; the captures that follow publish both keys.
    auto publishPair = [&](Hist& h, unsigned frame, int base = 0) {
        h.advance(frame);
        Hist::Capture a, b;
        capturePair(h, frame, base, a, b);
        check(a.candidateCount == 0 && b.candidateCount == 0, "range rig: a key drawn for the first time has no prior");
    };
    struct Probe { bool aKept, bKept; edvr::HistoryGap aGap, bGap; };
    // The next frame's draws of the two keys, after whatever was written in the gap: which still have their prior, and why the others do not.
    auto probePair = [&](Hist& h, unsigned frame, int base = 0) {
        Hist::Capture a, b;
        capturePair(h, frame, base, a, b);
        Probe p{};
        p.aKept = a.candidateCount == 1 && !a.missed;
        p.bKept = b.candidateCount == 1 && !b.missed;
        check(p.aKept || (a.candidateCount == 0 && a.missed), "range rig: a key without its prior is a counted miss, never a half-prior");
        check(p.bKept || (b.candidateCount == 0 && b.missed), "range rig: a key without its prior is a counted miss, never a half-prior (B)");
        p.aGap = a.miss.gap;
        p.bGap = b.miss.gap;
        return p;
    };

    // ---- 1. two keys in ONE index buffer: the write's range decides which loses its prior --------------------------------------------------
    {
        Hist h;
        publishPair(h, 100);
        h.advance(101);
        check(h.recordCount() == 2 && h.bytes() == 2 * 3 * 32 && h.writeStats().allocations == 2, "range 1: two keys hold two records of 96 bytes each");
        check(h.resourceWritten(ib.Get(), 12, 24, edvr::HistoryWriteEntry::Update) == 4,
              "range 1: an update of bytes [12,24) of the index buffer reports an index-buffer hit");
        const auto& s = h.writeStats();
        check(s.observed[kUpdate] == 1 && s.ranged[kUpdate] == 1 && s.savedWrites[kUpdate] == 0 && s.recordsInvalidated[kUpdate] == 1 &&
                  s.sparedRecords == 1 && s.vertexUnknown == 0 && s.vertexGenuine == 0 && flatRangeOnlyCell(s, kUpdate, kIndices, kGap, 1, 1),
              "range 1: the write is observed once, ranged, touched live records of the index buffer, invalidated one and spared the other");
        Hist::Capture a, b;
        check(h.capture(ctx, issue, 3, 1, 0, 0, 0, 101, a, true, true) && a.candidateCount == 1 && !a.missed,
              "range 1: key A [0,12) still has its prior: the write began where A ends");
        check(h.capture(ctx, issue, 3, 1, 3, 0, 0, 101, b, true, true) && b.candidateCount == 0 && b.missed &&
                  b.miss.gap == edvr::HistoryGap::InvalidatedIndices && !b.acrossOffset,
              "range 1: key B [12,24) is a miss named invalidated-indices: the record was retained, not reclaimed");
        check(h.recordCount() == 2 && h.writeStats().allocations == 2, "range 1: B reused its retained record, nothing was allocated");
    }
    // The control: the same two records and the same buffer, a range one byte either side, an empty range, and no range. Only the last takes both.
    {
        const FlatRangeRow rows[] = {
            {0, 12, true, false},    {12, 24, false, true},   {11, 13, true, true},    {0, 24, true, true},    {23, 24, false, true},
            {12, 13, false, true},   {0, 1, true, false},     {11, 12, true, false},   {24, 28, false, false}, {100, 104, false, false},
            {12, 12, false, false}, {0, 160, true, true}};
        for (const FlatRangeRow& row : rows) {
            Hist h;
            publishPair(h, 100);
            h.advance(101);
            const unsigned reasons = h.resourceWritten(ib.Get(), row.first, row.end, edvr::HistoryWriteEntry::Update);
            char what[200];
            std::snprintf(what, sizeof(what), "range 1 control: index bytes [%llu,%llu) hit A=%d B=%d: the report names %s",
                          (unsigned long long)row.first, (unsigned long long)row.end, row.a, row.b, (row.a || row.b) ? "indices" : "nothing");
            check(reasons == ((row.a || row.b) ? 4u : 0u), what);
            const auto& s = h.writeStats();
            const unsigned hits = unsigned(row.a) + unsigned(row.b);
            std::snprintf(what, sizeof(what), "range 1 control: index bytes [%llu,%llu): the counters say %u invalidated, %u spared, saved=%d",
                          (unsigned long long)row.first, (unsigned long long)row.end, hits, 2 - hits, hits == 0);
            check(s.recordsInvalidated[kUpdate] == hits && s.sparedRecords == 2 - hits && s.savedWrites[kUpdate] == (hits == 0 ? 1u : 0u) &&
                      s.touching[kUpdate][kIndices][kGap] == 1 && s.invalidating[kUpdate][kIndices][kGap] == (hits ? 1u : 0u), what);
            const Probe p = probePair(h, 101);
            std::snprintf(what, sizeof(what), "range 1 control: index bytes [%llu,%llu): A keeps its prior iff untouched, B likewise, and a lost one says invalidated-indices",
                          (unsigned long long)row.first, (unsigned long long)row.end);
            check(p.aKept == !row.a && p.bKept == !row.b &&
                      (p.aKept || p.aGap == edvr::HistoryGap::InvalidatedIndices) && (p.bKept || p.bGap == edvr::HistoryGap::InvalidatedIndices), what);
        }
        // The write that carries no range takes both, on the very bytes the ranged write at [100,104) spared.
        Hist h;
        publishPair(h, 100);
        h.advance(101);
        check(h.resourceWritten(ib.Get(), 100, 104, edvr::HistoryWriteEntry::Update) == 0 && h.writeStats().savedWrites[kUpdate] == 1 &&
                  h.writeStats().sparedRecords == 2,
              "range 1 control: bytes [100,104) of the buffer touch neither key; the write is counted as saved and both records as spared");
        const Probe kept = probePair(h, 101);
        check(kept.aKept && kept.bKept, "range 1 control: and both keys still have their priors");
        Hist w;
        publishPair(w, 100);
        w.advance(101);
        check(w.resourceWritten(ib.Get()) == 4, "range 1 control: the same buffer written with no range reports an index-buffer hit");
        check(w.writeStats().ranged[unsigned(edvr::HistoryWriteEntry::Other)] == 0 && w.writeStats().sparedRecords == 0 &&
                  w.writeStats().recordsInvalidated[unsigned(edvr::HistoryWriteEntry::Other)] == 2,
              "range 1 control: with no range both records fall and nothing is saved: the old rule");
        const Probe lost = probePair(w, 101);
        check(!lost.aKept && !lost.bKept && lost.aGap == edvr::HistoryGap::InvalidatedIndices && lost.bGap == edvr::HistoryGap::InvalidatedIndices,
              "range 1 control: and both keys are misses named invalidated-indices");
    }

    // ---- 1b. a write with no range is the whole resource, whatever the entry, role and timing -----------------------------------------------
    for (unsigned e = 0; e + 1 < edvr::kHistoryWriteEntries; ++e)   // every entry but Unknown, which only a nullptr resource has
        for (unsigned t = 0; t < edvr::kHistoryWriteTimings; ++t)
            for (unsigned role = 0; role < edvr::kHistoryWriteRoles; ++role) {
                Hist h;
                publishPair(h, 150);
                h.advance(151);
                ID3D11Resource* target = role == kVertices ? static_cast<ID3D11Resource*>(vb.Get()) : static_cast<ID3D11Resource*>(ib.Get());
                char what[200];
                std::snprintf(what, sizeof(what), "range 1b: an unranged %s write at the %s takes both records of the %s buffer and reports %u",
                              edvr::historyWriteEntryName(e), edvr::historyWriteTimingName(t), role == kVertices ? "vertex" : "index", role == kVertices ? 2u : 4u);
                check(h.resourceWritten(target, 0, whole, edvr::HistoryWriteEntry(e), edvr::HistoryWriteTiming(t)) == (role == kVertices ? 2u : 4u), what);
                const auto& s = h.writeStats();
                std::snprintf(what, sizeof(what), "range 1b: %s/%s/%s is counted in exactly its own cell: observed, touching, invalidating, two records, none ranged, none saved",
                              edvr::historyWriteEntryName(e), edvr::historyWriteTimingName(t), role == kVertices ? "vertices" : "indices");
                bool onlyEntry = true;
                for (unsigned e2 = 0; e2 < edvr::kHistoryWriteEntries; ++e2)
                    onlyEntry = onlyEntry && s.observed[e2] == (e2 == e ? 1u : 0u) && s.recordsInvalidated[e2] == (e2 == e ? 2u : 0u) &&
                                s.ranged[e2] == 0 && s.savedWrites[e2] == 0;
                // A write with no range is classified as nothing: none of the three-way vertex counters moves, whichever buffer it was.
                check(onlyEntry && flatRangeOnlyCell(s, e, role, t, 1, 1) && s.sparedRecords == 0 && s.vertexUnknown == 0 && s.vertexInGap == 0 &&
                          s.vertexOutside == 0 && s.vertexGenuine == 0 && s.unknownInvalidating[kGap] == 0 && s.unknownInvalidating[kWindow] == 0, what);
                const Probe p = probePair(h, 151);
                check(!p.aKept && !p.bKept && p.aGap == (role == kVertices ? edvr::HistoryGap::InvalidatedVertices : edvr::HistoryGap::InvalidatedIndices) &&
                          p.bGap == p.aGap, "range 1b: both keys are misses named for the buffer that was written");
            }
    // A write nothing could name: every record falls, and a range on a null resource cannot spare any.
    for (unsigned t = 0; t < edvr::kHistoryWriteTimings; ++t) {
        Hist h;
        publishPair(h, 160);
        h.advance(161);
        check(h.resourceWritten(nullptr, 12, 24, edvr::HistoryWriteEntry::Update, edvr::HistoryWriteTiming(t)) == 1,
              "range 1b: a write to a null resource reports unknown, ranged or not");
        const auto& s = h.writeStats();
        bool zeros = true;
        for (unsigned e2 = 0; e2 < edvr::kHistoryWriteEntries; ++e2)
            zeros = zeros && s.observed[e2] == (e2 == kUnknown ? 1u : 0u) && s.recordsInvalidated[e2] == (e2 == kUnknown ? 2u : 0u) && s.ranged[e2] == 0 &&
                    s.savedWrites[e2] == 0;
        check(zeros && flatRangeOnlyCell(s, 0, 0, 0, 0, 0) && s.unknownInvalidating[t] == 1 && s.unknownInvalidating[1 - t] == 0 && s.sparedRecords == 0,
              "range 1b: a null resource is filed under unknown, invalidating both records, by timing, and touches no buffer");
        const Probe p = probePair(h, 161);
        check(!p.aKept && !p.bKept && p.aGap == edvr::HistoryGap::InvalidatedUnknown && p.bGap == edvr::HistoryGap::InvalidatedUnknown,
              "range 1b: both keys are misses named invalidated-unknown");
    }
    // A write on records that are already invalidated is observed and reports its reasons, but it does not touch or invalidate a live record again.
    {
        Hist h;
        publishPair(h, 170);
        h.advance(171);
        check(h.resourceWritten(nullptr) == 1 && h.resourceWritten(nullptr) == 1 && h.resourceWritten(ib.Get()) == 4,
              "range 1b: writes after the records fell still report what they would have taken");
        const auto& s = h.writeStats();
        check(s.observed[kUnknown] == 2 && s.unknownInvalidating[kGap] == 1 && s.recordsInvalidated[kUnknown] == 2 &&
                  s.observed[unsigned(edvr::HistoryWriteEntry::Other)] == 1 && s.recordsInvalidated[unsigned(edvr::HistoryWriteEntry::Other)] == 0 &&
                  flatRangeOnlyCell(s, 0, 0, 0, 0, 0),
              "range 1b: the second null write and the buffer write after it are observed and nothing more: no live record was left to touch");
        // A write to something no record reads is observed and nothing else.
        const UINT other[4]{};
        auto stranger = flatRangeBuffer(dev, other, sizeof(other), D3D11_BIND_INDEX_BUFFER);
        Hist u;
        publishPair(u, 170);
        u.advance(171);
        check(u.resourceWritten(stranger.Get(), 0, 4, edvr::HistoryWriteEntry::Update) == 0 && u.resourceWritten(stranger.Get()) == 0,
              "range 1b: a buffer no record reads reports nothing, ranged or not");
        check(u.writeStats().observed[kUpdate] == 1 && u.writeStats().sparedRecords == 0 && u.writeStats().ranged[kUpdate] == 0 &&
                  u.writeStats().savedWrites[kUpdate] == 0 && flatRangeOnlyCell(u.writeStats(), 0, 0, 0, 0, 0),
              "range 1b: and it saves nothing: only a write that touched a live record can be saved by a range");
        const Probe p = probePair(u, 171);
        check(p.aKept && p.bKept, "range 1b: and both keys keep their priors");
    }
    // Reasons accumulate across writes, and the labels keep their order: unknown over vertices over indices.
    {
        Hist h;
        publishPair(h, 180);
        h.pollExtents(ctx, 183, true);
        h.advance(181);
        // The vertex write comes first: B's set is known, vertex 3 is B's and not A's, so the write is genuine for B and outside A's span.
        check(h.resourceWritten(vb.Get(), 48, 64, edvr::HistoryWriteEntry::Update) == 2 &&
                  h.resourceWritten(ib.Get(), 12, 24, edvr::HistoryWriteEntry::Update) == 4,
              "range 1c: B is hit by a vertex write and then an index write; A is touched by neither");
        const Probe p = probePair(h, 181);
        check(p.aKept && !p.bKept && p.bGap == edvr::HistoryGap::InvalidatedVertices,
              "range 1c: B's label is the vertex write's (the later write adds its reason and vertices outrank indices); A keeps its prior");
    }
    {
        // The other order: the index write takes B and ends the set read from those indices, so the vertex write that follows meets a record that is
        // already down and has no set to classify it by; it reports nothing and B keeps the label it has. (A's set is a different index range and
        // stays: the write to bytes [12,24) did not overlap it.)
        Hist h;
        publishPair(h, 185);
        h.pollExtents(ctx, 188, true);
        h.advance(186);
        check(h.resourceWritten(ib.Get(), 12, 24, edvr::HistoryWriteEntry::Update) == 4 &&
                  h.resourceWritten(vb.Get(), 48, 64, edvr::HistoryWriteEntry::Update) == 0,
              "range 1c: after an index write took B and ended its set, a vertex write finds a record that is down and reports nothing; A is outside its span");
        const Probe p = probePair(h, 186);
        check(p.aKept && !p.bKept && p.bGap == edvr::HistoryGap::InvalidatedIndices,
              "range 1c: B keeps the label of the write that took it (indices); A keeps its prior");
    }

    // ---- 2. the vertex set: read back from the draw's own indices, and until then the record is read as its indices alone -----------------
    {
        Hist h;
        publishPair(h, 200);
        check(h.writeStats().extentIssued == 2 && h.writeStats().extentRead == 0, "range 2: the vertex set of each new record is requested and not yet read");
        h.advance(201);
        check(h.resourceWritten(vb.Get(), 1000, 1004, edvr::HistoryWriteEntry::Update) == 0 && h.resourceWritten(vb.Get(), 0, 16, edvr::HistoryWriteEntry::Update) == 0,
              "range 2: before the set is read a ranged write to the vertex buffer is spared, one beyond its end and one on the first vertex alike");
        check(h.writeStats().vertexUnknown == 4 && h.writeStats().vertexGenuine == 0 && h.writeStats().recordsInvalidated[kUpdate] == 0 &&
                  h.writeStats().sparedRecords == 4 && h.writeStats().savedWrites[kUpdate] == 2 &&
                  flatRangeOnlyCell(h.writeStats(), kUpdate, kVertices, kGap, 2, 0),
              "range 2: and the four spares are counted as writes met with an unknown set, touching two writes' worth of live records, invalidating none");
        const Probe first = probePair(h, 201);
        check(first.aKept && first.bKept, "range 2: both keys keep their priors");
        check(h.writeStats().extentIssued == 2 && h.writeStats().setPending == 2,
              "range 2: the draws of the next frame asked again and found the key's read already in flight: no second copy");
        h.advance(202);
        h.pollExtents(ctx, 205, true);
        check(h.writeStats().extentRead == 2 && h.writeStats().extentFailed == 0 && h.writeStats().setCancelled == 0,
              "range 2: both sets are read");
        // Both writes of the unknown window were on vertex 0 or beyond the buffer: the one on vertex 0 met both sets, but both records were
        // published again at frame 201, after those writes (the write at frame 201 is not older than the stamp, so it is checked): the first
        // vertex is in both sets, so the deferred check takes both records.
        check(h.writeStats().deferredInvalidated == 2 && h.writeStats().deferredConservative == 0,
              "range 2: the write on vertex 0 spared while the sets were unknown met both when they arrived: the deferred check takes both records");
        const Probe afterDeferred = probePair(h, 202);
        check(!afterDeferred.aKept && !afterDeferred.bKept && afterDeferred.aGap == edvr::HistoryGap::InvalidatedVertices &&
                  afterDeferred.bGap == edvr::HistoryGap::InvalidatedVertices,
              "range 2: and both keys are misses named invalidated-vertices");
        h.advance(203);
        check(h.resourceWritten(vb.Get(), 1000, 1004, edvr::HistoryWriteEntry::Update) == 0 && h.writeStats().vertexOutside == 2,
              "range 2: once read, the same write beyond the buffer spares both and says it fell outside the sets");
        const Probe kept = probePair(h, 203);
        check(kept.aKept && kept.bKept && h.writeStats().extentIssued == 2, "range 2: both keep their priors, and the revived records were given their sets: no new read");
    }
    {
        // The same window, a write that met no vertex: nothing is taken when the sets arrive.
        Hist h;
        publishPair(h, 210);
        h.advance(211);
        check(h.resourceWritten(vb.Get(), 1000, 1004, edvr::HistoryWriteEntry::Update) == 0 && h.writeStats().vertexUnknown == 2,
              "range 2: a write beyond the buffer is spared while the sets are unknown");
        h.pollExtents(ctx, 214, true);
        check(h.writeStats().extentRead == 2 && h.writeStats().deferredInvalidated == 0 && h.writeStats().deferredConservative == 0,
              "range 2: and it is checked against the sets when they arrive, finds no vertex, and takes nothing");
        const Probe p = probePair(h, 211);
        check(p.aKept && p.bKept, "range 2: both keep their priors");
    }
    // The control table: the set is the vertices the indices name, placed by the vertex buffer offset and the draw's base; a write hits a record iff it meets one.
    const auto runRows = [&](const char* label, ID3D11Buffer* target, bool vertexRole, const FlatRangeRow* rows, size_t count, int base, bool polled) {
        for (size_t i = 0; i < count; ++i) {
            const FlatRangeRow& row = rows[i];
            Hist h;
            publishPair(h, 300, base);
            if (polled) {
                h.pollExtents(ctx, 303, true);
                check(h.writeStats().extentRead == 2 && h.writeStats().extentFailed == 0, "range rows: both extents are read before the write");
            }
            h.advance(301);
            const unsigned hits = unsigned(row.a) + unsigned(row.b);
            const unsigned reasons = h.resourceWritten(target, row.first, row.end, edvr::HistoryWriteEntry::Update);
            char what[240];
            std::snprintf(what, sizeof(what), "%s: bytes [%llu,%llu) hit A=%d B=%d: the report is %u", label, (unsigned long long)row.first,
                          (unsigned long long)row.end, row.a, row.b, hits ? (vertexRole ? 2u : 4u) : 0u);
            check(reasons == (hits ? (vertexRole ? 2u : 4u) : 0u), what);
            const auto& s = h.writeStats();
            std::snprintf(what, sizeof(what), "%s: bytes [%llu,%llu): %u records invalidated, %u spared, %s", label, (unsigned long long)row.first,
                          (unsigned long long)row.end, hits, 2 - hits, hits ? "the write is not saved" : "the write is saved");
            check(s.recordsInvalidated[kUpdate] == hits && s.sparedRecords == 2 - hits && s.savedWrites[kUpdate] == (hits ? 0u : 1u) &&
                      s.vertexUnknown == 0, what);
            if (vertexRole && polled) {
                // With the sets read, every live record the write reached is classified: genuine for a hit, in the gap or outside for a spare.
                // A write of no bytes is classified as outside (it meets no vertex and no envelope).
                std::snprintf(what, sizeof(what), "%s: bytes [%llu,%llu): the three-way counters add up to the records, %u genuine", label,
                              (unsigned long long)row.first, (unsigned long long)row.end, hits);
                check(s.vertexGenuine == hits && s.vertexInGap + s.vertexOutside == 2 - hits, what);
            }
            const Probe p = probePair(h, 301, base);
            const edvr::HistoryGap want = vertexRole ? edvr::HistoryGap::InvalidatedVertices : edvr::HistoryGap::InvalidatedIndices;
            std::snprintf(what, sizeof(what), "%s: bytes [%llu,%llu): A keeps its prior iff untouched, B likewise, a lost one is named for the buffer",
                          label, (unsigned long long)row.first, (unsigned long long)row.end);
            check(p.aKept == !row.a && p.bKept == !row.b && (p.aKept || p.aGap == want) && (p.bKept || p.bGap == want), what);
        }
    };
    {
        // Rig vertex buffer: A reads vertices {0,1,2} = bytes [0,48); B reads {0,2,3} = [0,16) and [32,64) -- vertex 1, bytes [16,32), is a gap in B.
        const FlatRangeRow rows[] = {
            {0, 16, true, true},   {0, 48, true, true},   {32, 48, true, true},  {47, 49, true, true},  {16, 17, true, false},  {48, 49, false, true},
            {48, 64, false, true}, {63, 64, false, true}, {64, 80, false, false}, {1000, 1004, false, false}, {48, 48, false, false}, {0, 64, true, true},
            {15, 17, true, true},  {16, 32, true, false}, {31, 32, true, false}, {31, 33, true, true},  {17, 31, true, false}, {15, 16, true, true}};
        runRows("range 2 vertex rows", vb.Get(), true, rows, sizeof(rows) / sizeof(rows[0]), 0, true);
    }
    {
        // The same buffer with the sets NOT read: a record is read as its indices alone, so every row is spared and filed as met with an unknown set.
        // The rows include vertices both keys read and the whole buffer; this is the behaviour before this build turned inside out (an unread extent
        // used to count as the whole buffer and take both records).
        const FlatRangeRow rows[] = {{48, 64, true, true}, {64, 80, true, true}, {1000, 1004, true, true}, {0, 64, true, true}, {0, 16, true, true}};
        for (const FlatRangeRow& row : rows) {
            Hist u;
            publishPair(u, 310);
            u.advance(311);
            check(u.resourceWritten(vb.Get(), row.first, row.end, edvr::HistoryWriteEntry::Update) == 0 && u.writeStats().vertexUnknown == 2 &&
                      u.writeStats().sparedRecords == 2 && u.writeStats().recordsInvalidated[kUpdate] == 0,
                  "range 2 control: an unread set makes every ranged vertex write a spare, filed as met with an unknown set");
            // And an unranged write still takes both, unclassified: the whole buffer is written whatever the set.
            Hist w;
            publishPair(w, 310);
            w.advance(311);
            check(w.resourceWritten(vb.Get()) == 2 && w.writeStats().vertexUnknown == 0 && w.writeStats().recordsInvalidated[unsigned(edvr::HistoryWriteEntry::Other)] == 2,
                  "range 2 control: with no range the same unread records both fall, unclassified");
        }
    }
    {
        // A vertex buffer of six vertices whose first two are padding. The draws reach the same bytes by three routes: a buffer offset of 32, a
        // base vertex of 2, or both halves (16 and 1). A reads bytes [32,80), B [32,96) every time.
        const float six[6][4] = {{-.6f, -.6f, 1.f, 0.f}, {-.6f, -.6f, 1.f, 0.f}, {-.6f, -.6f, 1.f, 0.f}, {.6f, -.6f, 1.f, 0.f},
                                 {.6f, .6f, 1.25f, 1.f}, {-.6f, .6f, 1.25f, 1.f}};
        auto vb6 = flatRangeBuffer(dev, six, sizeof(six), D3D11_BIND_VERTEX_BUFFER);
        struct Route { UINT offset; int base; const char* name; };
        const Route routes[] = {{32, 0, "range 2 offset 32 base 0"}, {0, 2, "range 2 offset 0 base 2"}, {16, 1, "range 2 offset 16 base 1"}};
        // B has a gap at the vertex after the first (bytes [48,64) here).
        const FlatRangeRow rows[] = {{80, 96, false, true}, {79, 80, true, true}, {80, 81, false, true}, {95, 96, false, true}, {16, 32, false, false},
                                     {0, 32, false, false}, {31, 33, true, true}, {96, 128, false, false}, {32, 33, true, true}, {48, 64, true, false},
                                     {63, 64, true, false}, {47, 49, true, true}, {49, 50, true, false}, {64, 65, true, true}};
        for (const Route& route : routes) {
            ID3D11Buffer* buffers[1] = {vb6.Get()};
            ctx->IASetVertexBuffers(1, 1, buffers, &stride, &route.offset);
            runRows(route.name, vb6.Get(), true, rows, sizeof(rows) / sizeof(rows[0]), route.base, true);
        }
        ctx->IASetVertexBuffers(1, 1, vb.GetAddressOf(), &stride, &voffset);
    }
    {
        // 16-bit indices: the index range is in two-byte units and the readback widens them.
        std::vector<unsigned short> narrow(40, 0);
        const unsigned short pair16[6]{0, 1, 2, 0, 2, 3};
        std::memcpy(narrow.data(), pair16, sizeof(pair16));
        auto ib16 = flatRangeBuffer(dev, narrow.data(), UINT(narrow.size() * 2), D3D11_BIND_INDEX_BUFFER);
        ctx->IASetIndexBuffer(ib16.Get(), DXGI_FORMAT_R16_UINT, 0);
        const FlatRangeRow indexRows[] = {{0, 6, true, false}, {6, 12, false, true}, {5, 7, true, true}, {12, 16, false, false}, {11, 12, false, true}};
        runRows("range 2 R16 index rows", ib16.Get(), false, indexRows, sizeof(indexRows) / sizeof(indexRows[0]), 0, false);
        const FlatRangeRow vertexRows[] = {{48, 64, false, true}, {0, 16, true, true}, {64, 80, false, false}, {16, 32, true, false}};
        runRows("range 2 R16 vertex rows", vb.Get(), true, vertexRows, sizeof(vertexRows) / sizeof(vertexRows[0]), 0, true);
        // An index buffer bound at a byte offset: the range starts there, and the extent is read from there (the two leading words are decoys that
        // would name vertices 7 and 7 and widen A to the whole buffer if the copy began at byte 0).
        std::vector<UINT> shifted(40, 0);
        shifted[0] = shifted[1] = 7;
        std::memcpy(shifted.data() + 2, pair, sizeof(pair));
        auto ibShift = flatRangeBuffer(dev, shifted.data(), UINT(shifted.size() * 4), D3D11_BIND_INDEX_BUFFER);
        ctx->IASetIndexBuffer(ibShift.Get(), DXGI_FORMAT_R32_UINT, 8);
        const FlatRangeRow shiftRows[] = {{0, 8, false, false}, {8, 20, true, false}, {19, 21, true, true}, {20, 32, false, true}, {32, 36, false, false}};
        runRows("range 2 index offset 8 index rows", ibShift.Get(), false, shiftRows, sizeof(shiftRows) / sizeof(shiftRows[0]), 0, false);
        runRows("range 2 index offset 8 vertex rows", vb.Get(), true, vertexRows, sizeof(vertexRows) / sizeof(vertexRows[0]), 0, true);
        ctx->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    }

    // ---- 3. retention: an invalidated record keeps its allocation until its key draws again or the keep window passes --------------------
    {
        Hist h;
        h.advance(300);
        Hist::Capture c;
        check(h.capture(ctx, issue, 3, 1, 0, 0, 0, 300, c, true, true) && c.missed && c.candidateCount == 0, "range 3: the first draw of a key has no prior");
        check(h.writeStats().allocations == 1 && h.bytes() == 96, "range 3: and made one record");
        check(h.resourceWritten(ib.Get(), 0, whole, edvr::HistoryWriteEntry::Map) == 4, "range 3: a mapped index buffer takes the record");
        h.advance(301);
        check(h.recordCount() == 1 && h.bytes() == 96 && h.writeStats().erased[unsigned(edvr::HistoryErase::AdvanceInvalidated)] == 0,
              "range 3: the frame boundary keeps the invalidated record under the extended policy");
        Hist::Capture again;
        check(h.capture(ctx, issue, 3, 1, 0, 0, 0, 301, again, true, true) && again.missed && again.candidateCount == 0 &&
                  again.miss.gap == edvr::HistoryGap::InvalidatedIndices,
              "range 3: the key drawn again is a miss named for the write: the retained record said so");
        check(h.recordCount() == 1 && h.writeStats().allocations == 1, "range 3: and it reused the record: nothing was allocated");
        h.advance(302);
        Hist::Capture third;
        check(h.capture(ctx, issue, 3, 1, 0, 0, 0, 302, third, true, true) && third.candidateCount == 1 && !third.missed && h.writeStats().allocations == 1,
              "range 3: the redraw published the revived record: the frame after, the key has its prior again");
    }
    {
        // The keep window: three frame boundaries after the write the record is still there; the fourth takes it.
        Hist h;
        h.advance(310);
        Hist::Capture c;
        check(h.capture(ctx, issue, 3, 1, 0, 0, 0, 310, c, true, true), "range 3: a key is captured to be invalidated and never drawn again");
        h.resourceWritten(ib.Get(), 0, whole, edvr::HistoryWriteEntry::Map);
        for (unsigned f = 311; f <= 313; ++f) {
            h.advance(f);
            check(h.recordCount() == 1 && h.writeStats().erased[unsigned(edvr::HistoryErase::AdvanceInvalidated)] == 0,
                  "range 3: three boundaries after the write the invalidated record is still held");
        }
        h.advance(314);
        const auto& s = h.writeStats();
        check(h.recordCount() == 0 && h.bytes() == 0 && s.erased[unsigned(edvr::HistoryErase::AdvanceInvalidated)] == 1 &&
                  s.erased[unsigned(edvr::HistoryErase::AdvanceAged)] == 0,
              "range 3: the fourth boundary erases it, counted as advance-invalidated and nothing else, and the bytes go with it");
    }
    {
        // A record nothing invalidated leaves by age, two frames after its last use, under the other counter.
        Hist h;
        h.advance(320);
        Hist::Capture c;
        check(h.capture(ctx, issue, 3, 1, 0, 0, 0, 320, c, true, true), "range 3: a key is captured and published");
        h.advance(321);
        h.advance(322);
        check(h.recordCount() == 1 && h.writeStats().erased[unsigned(edvr::HistoryErase::AdvanceAged)] == 0, "range 3: two frames after its last use it is still held");
        h.advance(323);
        const auto& s = h.writeStats();
        check(h.recordCount() == 0 && h.bytes() == 0 && s.erased[unsigned(edvr::HistoryErase::AdvanceAged)] == 1 &&
                  s.erased[unsigned(edvr::HistoryErase::AdvanceInvalidated)] == 0,
              "range 3: the third frame erases it as advance-aged, not as advance-invalidated");
    }
    {
        // The default policy (VR): the record is erased at the first boundary after a write, as it always was; no extent is ever requested.
        Hist h;
        h.advance(330);
        Hist::Capture c;
        check(h.capture(ctx, issue, 3, 1, 0, 0, 0, 330, c), "range 3: the default policy captures");
        check(h.writeStats().extentIssued == 0, "range 3: and requests no vertex extent: VR pays nothing for the instrument");
        check(h.resourceWritten(ib.Get()) == 4, "range 3: a write to its index buffer takes the record");
        h.advance(331);
        check(h.recordCount() == 0 && h.bytes() == 0 && h.writeStats().erased[unsigned(edvr::HistoryErase::AdvanceInvalidated)] == 1,
              "range 3 control: under the default policy the first boundary after the write erases it: the retention is the extended policy's only");
    }

    // ---- 4. pressure and the labels of a record that is gone ----------------------------------------------------------------------------
    {
        std::vector<UINT> many((cap + 16) * 3);
        for (size_t i = 0; i < many.size(); i += 3) { many[i] = 0; many[i + 1] = 1; many[i + 2] = 2; }
        auto big = flatHistoryPressureIndexBuffer(dev, many);
        ctx->IASetIndexBuffer(big.Get(), DXGI_FORMAT_R32_UINT, 0);
        const auto fill = [&](Hist& h, unsigned frame) {
            h.advance(frame);
            for (unsigned i = 0; i < cap; ++i) {
                Hist::Capture c;
                check(h.capture(ctx, issue, 3, 1, i * 3, 0, 0, frame, c, true, true), "range 4: the record cap's worth of distinct keys is admitted");
            }
            check(h.recordCount() == cap && h.writeStats().allocations == cap, "range 4: the history holds exactly the cap");
        };
        const auto fresh = [&](Hist& h, unsigned frame, unsigned start, Hist::Capture& c) { return h.capture(ctx, issue, 3, 1, start, 0, 0, frame, c, true, true); };
        {
            Hist h;
            fill(h, 400);
            check(h.resourceWritten(big.Get(), 0, whole, edvr::HistoryWriteEntry::Map) == 4, "range 4: a whole-buffer write invalidates every record");
            h.advance(401);
            check(h.recordCount() == cap && h.writeStats().erased[unsigned(edvr::HistoryErase::AdvanceInvalidated)] == 0 &&
                      h.writeStats().erased[unsigned(edvr::HistoryErase::PressureInvalidated)] == 0,
                  "range 4: every invalidated record is retained across the boundary");
            Hist::Capture n;
            check(fresh(h, 401, cap * 3, n) && !n.refusal && n.missed, "range 4: a new key needs budget and is admitted by reclaiming");
            const auto& s = h.writeStats();
            check(s.erased[unsigned(edvr::HistoryErase::PressureInvalidated)] == 1 && s.erased[unsigned(edvr::HistoryErase::PressureSpent)] == 0 &&
                      h.recordCount() == cap && s.allocations == cap + 1,
                  "range 4: exactly one invalidated record was taken for it, counted as pressure-invalidated");
            Hist::Capture second;
            check(fresh(h, 401, 6, second) && second.missed && second.miss.gap == edvr::HistoryGap::InvalidatedIndices && h.writeStats().allocations == cap + 1,
                  "range 4: the third key (still retained) is a miss named for the write, and takes no budget");
            Hist::Capture first;
            check(fresh(h, 401, 0, first) && first.missed && first.candidateCount == 0 && first.miss.gap == edvr::HistoryGap::ReclaimedByPressure,
                  "range 4: the first key, whose record pressure took, is a miss named reclaimed-by-pressure");
            check(h.writeStats().erased[unsigned(edvr::HistoryErase::PressureInvalidated)] == 2,
                  "range 4: and its own admission took the next invalidated record");
            Hist::Capture also;
            check(fresh(h, 401, 3, also) && also.missed && also.miss.gap == edvr::HistoryGap::ReclaimedByPressure,
                  "range 4: so did the second key's: both taken records are reported as taken, not as written");
        }
        {
            // Only some records are invalidated: pressure takes those, one per new key, and stops short of the ones nothing wrote.
            Hist h;
            fill(h, 410);
            check(h.resourceWritten(big.Get(), 0, 120, edvr::HistoryWriteEntry::Update) == 4 && h.writeStats().recordsInvalidated[kUpdate] == 10 &&
                      h.writeStats().sparedRecords == cap - 10,
                  "range 4: a ranged write over ten keys' index bytes invalidates ten records and spares the rest");
            h.advance(411);
            for (unsigned i = 0; i < 10; ++i) {
                Hist::Capture n;
                check(fresh(h, 411, (cap + i) * 3, n) && !n.refusal, "range 4: ten new keys are admitted, each by taking one invalidated record");
            }
            const auto& s = h.writeStats();
            check(s.erased[unsigned(edvr::HistoryErase::PressureInvalidated)] == 10 && s.erased[unsigned(edvr::HistoryErase::PressureSpent)] == 0 &&
                      h.recordCount() == cap,
                  "range 4: exactly the ten invalidated records were taken, none of the records published the frame before");
            Hist::Capture eleventh;
            check(!fresh(h, 411, (cap + 10) * 3, eleventh) && eleventh.refusal && !std::strcmp(eleventh.refusal, "history-budget"),
                  "range 4 control: with nothing invalidated and every record published the frame before or this frame, the next new key is refused for budget");
            check(h.writeStats().erased[unsigned(edvr::HistoryErase::PressureSpent)] == 0 &&
                      h.writeStats().erased[unsigned(edvr::HistoryErase::PressureInvalidated)] == 10 && h.recordCount() == cap,
                  "range 4 control: and the refusal took nothing: a prior is never reclaimed while it can still be used");
            Hist::Capture kept;
            check(fresh(h, 411, 10 * 3, kept) && kept.candidateCount == 1 && !kept.missed,
                  "range 4: the first key nothing wrote still has its prior after all of that");
        }
        {
            // Spent records (used two frames ago and not since) are the second kind pressure may take.
            Hist h;
            fill(h, 420);
            h.advance(422);
            check(h.recordCount() == cap, "range 4: two frames after its last use a record is still held by the boundary");
            Hist::Capture n;
            check(fresh(h, 422, cap * 3, n) && !n.refusal, "range 4: a new key is admitted over the spent records");
            check(h.writeStats().erased[unsigned(edvr::HistoryErase::PressureSpent)] == 1 &&
                      h.writeStats().erased[unsigned(edvr::HistoryErase::PressureInvalidated)] == 0,
                  "range 4: the taken one is counted as pressure-spent, not pressure-invalidated");
        }
        {
            // The advance label. The adapter always advances before it captures; here the first advance is skipped, so the write is stamped with a
            // stale frame and the next boundary ages the record out at once. A key published the frame before whose record the boundary took is
            // reported as reclaimed-by-advance, and it is not the pressure label.
            Hist h;
            Hist::Capture c;
            check(fresh(h, 500, 0, c), "range 4: a key is captured with no advance before it");
            check(h.resourceWritten(big.Get(), 0, whole, edvr::HistoryWriteEntry::Map) == 4, "range 4: and invalidated");
            h.advance(501);
            check(h.recordCount() == 0 && h.writeStats().erased[unsigned(edvr::HistoryErase::AdvanceInvalidated)] == 1,
                  "range 4: the boundary erased the record whose write was stamped long ago");
            Hist::Capture again;
            check(fresh(h, 501, 0, again) && again.missed && again.miss.gap == edvr::HistoryGap::ReclaimedByAdvance,
                  "range 4: the key published the frame before, with no record, is a miss named reclaimed-by-advance");
        }
        ctx->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    }

    // ---- 5. the instruments the history line is printed from -----------------------------------------------------------------------------
    {
        Hist h;
        publishPair(h, 600);
        unsigned records = 0, bytes = 0;
        h.takePeaks(records, bytes);
        check(records == 2 && bytes == 2 * 3 * 32, "range 5: the peak of a history that made two records is two records and 192 bytes");
        h.advance(601);
        check(h.resourceWritten(ib.Get(), 12, 24, edvr::HistoryWriteEntry::Update) == 4 && h.resourceWritten(ib.Get(), 100, 104, edvr::HistoryWriteEntry::Update) == 0 &&
                  h.resourceWritten(vb.Get(), 1000, 1004, edvr::HistoryWriteEntry::Update) == 0,
              "range 5: three writes: one that hit, one that was saved and one that was spared because the vertex sets were not read yet");
        edvr::HistoryWriteTop top[edvr::kHistoryTopResources]{};
        const unsigned n = h.takeTopResources(top, edvr::kHistoryTopResources);
        check(n == 2 && top[0].resource == ib.Get() && top[0].touching == 2 && top[0].invalidating == 1 && top[0].saved == 1 && top[1].resource == vb.Get() &&
                  top[1].touching == 1 && top[1].invalidating == 0 && top[1].saved == 1,
              "range 5: the resources written most come back most first with touching, invalidating and saved counts");
        check(h.takeTopResources(top, edvr::kHistoryTopResources) == 0, "range 5: and the table is forgotten once taken");
        h.advance(605);
        h.advance(606);
        h.takePeaks(records, bytes);
        check(records == 2 && bytes == 2 * 3 * 32 && h.recordCount() == 0, "range 5: the peak outlives the erase for one report");
        h.takePeaks(records, bytes);
        check(records == 0 && bytes == 0, "range 5: and then the gauge is what is held now");
        // The peak is the highest the history reached since the last report, even when it was erased again before the report: three records are
        // made (one more than were held at either end of the window), all aged out, and the next report still says three.
        h.advance(620);
        for (const unsigned start : {0u, 3u, 6u}) {
            Hist::Capture c;
            check(h.capture(ctx, issue, 3, 1, start, 0, 0, 620, c, true, true), "range 5: three keys are captured in one frame");
        }
        h.advance(630);
        check(h.recordCount() == 0, "range 5: and aged out");
        h.takePeaks(records, bytes);
        check(records == 3 && bytes == 3 * 3 * 32, "range 5: the report names the peak of three records and 288 bytes, not the empty history it ended with");
    }
    // ---- 6. the adapter's lists: a write drops only the draws whose bytes it touched --------------------------------------------------------
    // Two draws on one index buffer, two frames, then a write over the second draw's bytes in the window of the second frame. The frame is refused
    // (a current draw was touched), but the first draw stays in the list the next frame reads its prior from, and the second does not.
    for (const bool hitsB : {true, false}) {
        edvr::FlatForegroundMotion motion;
        const auto draw = [&](unsigned start, unsigned token, unsigned frame) {
            return motion.capture(ctx, issue, 3, 1, start, 0, 0, frame, flatHistoryPressureInputs(token));
        };
        check(draw(0, 1, 700) && draw(3, 2, 700) && draw(0, 1, 701) && draw(3, 2, 701), "range 6: two draws on one index buffer capture for two frames");
        check(motion.stats().priorsOne == 2 && motion.stats().noCandidate == 2, "range 6: the second frame's draws each found their one prior in the frame before");
        motion.resourceWritten(ib.Get(), edvr::historyRangedWrite(edvr::HistoryWriteEntry::Update, hitsB ? 12 : 100, hitsB ? 24 : 104));
        check(hitsB == (motion.refusal() && !std::strcmp(motion.refusal(), "foreground-captured-geometry-written")) && (hitsB || !motion.refusal()),
              "range 6: a write over the second draw's bytes refuses the frame as captured geometry written; one over neither does not");
        check(draw(0, 1, 702) && draw(3, 2, 702), "range 6: the third frame's draws capture");
        const auto st = motion.stats();
        check(st.priorsOne == (hitsB ? 3u : 4u) && st.noPriorAbsent == 0 && st.noCandidate == (hitsB ? 3u : 2u) &&
                  st.missBy[unsigned(edvr::HistoryGap::InvalidatedIndices)] == (hitsB ? 1u : 0u),
              hitsB ? "range 6: the untouched draw kept its place in the list and its prior; the touched one is a miss named invalidated-indices"
                    : "range 6 control: a write over neither draw leaves both their priors: no miss, no draw dropped from the list");
        check(st.history.recordsInvalidated[kUpdate] == (hitsB ? 1u : 0u) && st.history.sparedRecords == (hitsB ? 1u : 2u),
              "range 6: and the instrument says one record taken and one spared, or none taken and both spared");
    }
    ctx->ClearState();
    bind(Pose{}, false);
}
