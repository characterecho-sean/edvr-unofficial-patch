#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

// THE VERTICES A DRAW READS, EXACTLY (design doc section 104, the third and fourth builds).
//
// The real AnimatedVertexHistory on WARP, under the extended policy the flat adapter uses. A record's vertices are the set its indices name, read
// back once per index range from a staging copy of the indices and cached beyond the record; a ranged write to its vertex buffer invalidates it only
// if it meets a vertex of the set (genuine), is spared when it falls between the vertices (in-gap) or outside the envelope (outside), and is spared
// and logged while the set is unknown (checked when the set arrives, or the record is invalidated conservatively). Every case here is a scenario and
// its control: the same records and frames with one factor changed and the opposite outcome. The pure function that decides a hit is held to a table
// in tools\flat_temporal_test; here it is held to the real thing: the readback, the cache, the placement by offset and base, the deferred check.
namespace flat_vertex_set_detail {
using Hist = edvr::AnimatedVertexHistory;
using Set = edvr::HistoryVertexSet;
enum RowCase { kHit = 0, kGapRow, kOutside };
struct Row { uint64_t first, end; int c; };
inline const char* caseName(int c) { return c == kHit ? "genuine" : c == kGapRow ? "in-gap" : "outside"; }
inline bool sameRuns(const Set* s, std::initializer_list<uint32_t> runs) {
    return s && s->runs.size() == runs.size() && std::equal(runs.begin(), runs.end(), s->runs.begin());
}
inline unsigned long long ull(uint64_t v) { return static_cast<unsigned long long>(v); }
}  // namespace flat_vertex_set_detail

template<class Bind>
inline void flatVertexSetTests(ID3D11Device* dev, ID3D11DeviceContext* ctx, Bind bind) {
    using namespace flat_vertex_set_detail;
    using edvr::HistoryGap;
    const unsigned kUpdate = unsigned(edvr::HistoryWriteEntry::Update);
    const unsigned kVertices = unsigned(edvr::HistoryWriteRole::Vertices);
    const unsigned kGap = unsigned(edvr::HistoryWriteTiming::Gap);
    constexpr uint64_t whole = ~uint64_t(0);
    ctx->ClearState();
    bind(Pose{}, false);

    // 64 vertices at stride 16 (the rig's four, repeated): the draws below read vertices the rig's four never had, so the vertex buffer is longer.
    const float rig[4][4] = {{-.6f, -.6f, 1.f, 0.f}, {.6f, -.6f, 1.f, 0.f}, {.6f, .6f, 1.25f, 1.f}, {-.6f, .6f, 1.25f, 1.f}};
    std::vector<float> verts;
    for (unsigned i = 0; i < 64; ++i) verts.insert(verts.end(), rig[i % 4], rig[i % 4] + 4);
    auto vb = flatRangeBuffer(dev, verts.data(), UINT(verts.size() * 4), D3D11_BIND_VERTEX_BUFFER);
    UINT stride = 16;
    const auto useVb = [&](ID3D11Buffer* buffer, UINT offset) {
        ID3D11Buffer* buffers[1] = {buffer};
        ctx->IASetVertexBuffers(1, 1, buffers, &stride, &offset);
    };
    useVb(vb.Get(), 0);
    const auto words = [](std::initializer_list<UINT> w, size_t pad = 40) {
        std::vector<UINT> v(w);
        if (v.size() < pad) v.resize(pad, 0);
        return v;
    };
    // The index buffer most cases use: key G = {0,1,2, 6,7,8} (runs [0,2] [6,8], vertices 3..5 are its gap) at words 0..5, key T = {3,4,5} at words 6..8.
    auto ibGap = flatHistoryPressureIndexBuffer(dev, words({0, 1, 2, 6, 7, 8, 3, 4, 5}));
    ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);

    // One row against a fresh history: the key is captured, its set read, then a write at the next frame. Counters, return and the next frame's prior.
    const auto oneRow = [&](const char* label, unsigned count, unsigned start, int base, std::initializer_list<uint32_t> runs, const Row& row) {
        Hist h;
        const unsigned F = 100;
        h.advance(F);
        Hist::Capture c;
        char what[320];
        check(h.capture(ctx, issue, count, 1, start, base, 0, F, c, true, true) && c.candidateCount == 0, "set rows: the key is admitted and has no prior");
        h.pollExtents(ctx, F + 1, true);
        std::snprintf(what, sizeof(what), "%s: the set the indices name is read: its runs are the draw's vertices", label);
        check(sameRuns(h.vertexSetOf(c), runs) && h.writeStats().extentIssued == 1 && h.writeStats().extentRead == 1, what);
        h.advance(F + 1);
        const unsigned reasons = h.resourceWritten(vb.Get(), row.first, row.end, edvr::HistoryWriteEntry::Update);
        const auto& s = h.writeStats();
        const bool hit = row.c == kHit;
        std::snprintf(what, sizeof(what), "%s: bytes [%llu,%llu) are %s: the report is %u", label, ull(row.first), ull(row.end), caseName(row.c), hit ? 2u : 0u);
        check(reasons == (hit ? 2u : 0u), what);
        std::snprintf(what, sizeof(what), "%s: bytes [%llu,%llu) are %s: counted once as that and as nothing else, %s", label, ull(row.first), ull(row.end), caseName(row.c),
                      hit ? "the record invalidated" : "the record spared and the write saved");
        check(s.vertexGenuine == (row.c == kHit) && s.vertexInGap == (row.c == kGapRow) && s.vertexOutside == (row.c == kOutside) && s.vertexUnknown == 0 &&
                  s.recordsInvalidated[kUpdate] == (hit ? 1u : 0u) && s.sparedRecords == (hit ? 0u : 1u) && s.savedWrites[kUpdate] == (hit ? 0u : 1u) &&
                  s.deferredInvalidated == 0 && s.deferredConservative == 0 && flatRangeOnlyCell(s, kUpdate, kVertices, kGap, 1, hit ? 1 : 0),
              what);
        Hist::Capture p;
        check(h.capture(ctx, issue, count, 1, start, base, 0, F + 1, p, true, true), "set rows: the key is captured again");
        std::snprintf(what, sizeof(what), "%s: bytes [%llu,%llu) are %s: the next frame's draw %s", label, ull(row.first), ull(row.end), caseName(row.c),
                      hit ? "is a miss named invalidated-vertices" : "keeps its prior");
        check(hit ? (p.candidateCount == 0 && p.missed && p.miss.gap == HistoryGap::InvalidatedVertices) : (p.candidateCount == 1 && !p.missed), what);
        std::snprintf(what, sizeof(what), "%s: bytes [%llu,%llu): the write to the vertices did not end the set: it is still held and nothing was read again", label, ull(row.first),
                      ull(row.end));
        check(sameRuns(h.vertexSetOf(p), runs) && h.writeStats().extentIssued == 1 && h.writeStats().allocations == 1, what);
    };

    // ---- 1. the exact set: {0,1,2, 6,7,8} at stride 16, the vertex buffer at byte 0 --------------------------------------------------------------
    {
        Hist h;
        h.advance(90);
        Hist::Capture c;
        check(h.capture(ctx, issue, 6, 1, 0, 0, 0, 90, c, true, true), "set 1: the key is admitted");
        check(h.vertexSetOf(c) == nullptr && h.writeStats().extentIssued == 1 && h.cachedSets() == 0, "set 1: its set is asked for and not known yet");
        h.pollExtents(ctx, 91, true);
        const Set* s = h.vertexSetOf(c);
        check(sameRuns(s, {0, 2, 6, 8}) && s->low == 0 && s->high == 8 && s->exact && h.cachedSets() == 1 && h.writeStats().extentRead == 1,
              "set 1: the set is the vertices the indices name: runs [0,2] and [6,8], low 0, high 8, exact, and cached");
        check(h.writeStats().setApproximate == 0 && h.writeStats().extentFailed == 0 && h.writeStats().setCancelled == 0, "set 1: no inexact set, no failure");
    }
    {
        const Row rows[] = {
            {112, 128, kHit},   {0, 16, kHit},      {47, 48, kHit},     {48, 49, kGapRow},  {48, 96, kGapRow}, {95, 96, kGapRow},  {95, 97, kHit},    {96, 97, kHit},
            {47, 49, kHit},     {143, 144, kHit},   {144, 145, kOutside}, {144, 160, kOutside}, {240, 256, kOutside}, {40, 56, kHit},   {50, 60, kGapRow}, {0, 1, kHit},
            {20, 90, kHit},     {64, 65, kGapRow},  {0, 144, kHit},     {1000, 1004, kOutside}, {112, 113, kHit}, {127, 128, kHit}, {5, 5, kOutside}, {16, 32, kHit}};
        for (const Row& row : rows) oneRow("set 1 rows", 6, 0, 0, {0, 2, 6, 8}, row);
    }
    // The key T = {3,4,5} (words 6..8) is the gap of G exactly: the same write that G spares, T reads.
    for (const Row& row : {Row{48, 96, kHit}, Row{47, 49, kHit}, Row{95, 97, kHit}, Row{96, 112, kOutside}, Row{16, 48, kOutside}, Row{80, 81, kHit}})
        oneRow("set 1 key T rows", 3, 6, 0, {3, 5}, row);

    // ---- 1b. three routes to the same bytes, and a base that is negative -----------------------------------------------------------------------
    {
        // G placed at byte 32: by the buffer offset, by the base, or by both halves. Every route reads vertices 3..5 at bytes [80,128), the gap.
        struct Route { UINT offset; int base; const char* name; };
        const Route routes[] = {{32, 0, "set 1b offset 32 base 0"}, {0, 2, "set 1b offset 0 base 2"}, {16, 1, "set 1b offset 16 base 1"}};
        const Row rows[] = {{32, 48, kHit}, {31, 32, kOutside}, {16, 32, kOutside}, {31, 33, kHit}, {47, 48, kHit}, {48, 80, kHit}, {80, 81, kGapRow}, {80, 128, kGapRow},
                            {127, 128, kGapRow}, {127, 129, kHit}, {128, 129, kHit}, {159, 160, kHit}, {175, 176, kHit}, {176, 177, kOutside}, {0, 32, kOutside}, {40, 120, kHit}};
        for (const Route& route : routes) {
            useVb(vb.Get(), route.offset);
            for (const Row& row : rows) oneRow(route.name, 6, 0, route.base, {0, 2, 6, 8}, row);
        }
        useVb(vb.Get(), 0);
    }
    {
        // A negative base that still names valid bytes: the indices are all 2 or more, so base -2 makes vertex i the bytes [16(i-2), 16(i-1)). With
        // a buffer offset of 64 as well, vertex i is [32+16i, 48+16i): the two halves of the place are opposite in sign.
        auto ibHigh = flatHistoryPressureIndexBuffer(dev, words({2, 3, 4, 8, 9, 10}));
        ctx->IASetIndexBuffer(ibHigh.Get(), DXGI_FORMAT_R32_UINT, 0);
        const Row negative[] = {{0, 16, kHit}, {16, 48, kHit}, {48, 96, kGapRow}, {96, 112, kHit}, {128, 144, kHit}, {143, 144, kHit}, {144, 160, kOutside}, {47, 49, kHit},
                                {95, 97, kHit}, {95, 96, kGapRow}};
        useVb(vb.Get(), 0);
        for (const Row& row : negative) oneRow("set 1b base -2 offset 0", 6, 0, -2, {2, 4, 8, 10}, row);
        const Row shifted[] = {{64, 80, kHit}, {32, 64, kOutside}, {112, 128, kGapRow}, {112, 160, kGapRow}, {160, 176, kHit}, {207, 208, kHit}, {208, 209, kOutside}, {63, 65, kHit}};
        useVb(vb.Get(), 64);
        for (const Row& row : shifted) oneRow("set 1b base -2 offset 64", 6, 0, -2, {2, 4, 8, 10}, row);
        useVb(vb.Get(), 0);
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
    }
    {
        // A vertex buffer that starts at byte 48: a write just before it is before every vertex (a division that truncates would call it vertex 0).
        useVb(vb.Get(), 48);
        const Row rows[] = {{32, 48, kOutside}, {40, 48, kOutside}, {47, 48, kOutside}, {47, 49, kHit}, {48, 49, kHit}, {0, 48, kOutside}, {96, 97, kGapRow}, {48, 144, kHit}};
        // Vertex i at 48 + 16 i: the first run is [48,96), the gap [96,144), the second run [144,192).
        for (const Row& row : rows) oneRow("set 1b offset 48", 6, 0, 0, {0, 2, 6, 8}, row);
        useVb(vb.Get(), 0);
    }

    // ---- 1c. 16-bit indices, and an index buffer bound at a byte offset -----------------------------------------------------------------------
    {
        std::vector<unsigned short> narrow(48, 0);
        const unsigned short data[9] = {0, 1, 2, 6, 7, 8, 3, 4, 5};
        std::memcpy(narrow.data(), data, sizeof(data));
        auto ib16 = flatRangeBuffer(dev, narrow.data(), UINT(narrow.size() * 2), D3D11_BIND_INDEX_BUFFER);
        ctx->IASetIndexBuffer(ib16.Get(), DXGI_FORMAT_R16_UINT, 0);
        const Row rows[] = {{112, 128, kHit}, {48, 96, kGapRow}, {47, 49, kHit}, {144, 160, kOutside}, {95, 96, kGapRow}, {96, 97, kHit}, {0, 16, kHit}};
        for (const Row& row : rows) oneRow("set 1c R16", 6, 0, 0, {0, 2, 6, 8}, row);
        // The same buffer bound at byte 4: two leading shorts are decoys (vertex 15), not in the draw. A copy that began at byte 0 would name vertex 15.
        std::vector<unsigned short> shifted(48, 0);
        shifted[0] = shifted[1] = 15;
        std::memcpy(shifted.data() + 2, data, sizeof(data));
        auto ibShift16 = flatRangeBuffer(dev, shifted.data(), UINT(shifted.size() * 2), D3D11_BIND_INDEX_BUFFER);
        ctx->IASetIndexBuffer(ibShift16.Get(), DXGI_FORMAT_R16_UINT, 4);
        const Row shiftRows[] = {{240, 256, kOutside}, {112, 128, kHit}, {48, 96, kGapRow}, {144, 160, kOutside}};
        for (const Row& row : shiftRows) oneRow("set 1c R16 index offset 4", 6, 0, 0, {0, 2, 6, 8}, row);
        // R32 bound at byte 8, two leading words are decoys (15).
        std::vector<UINT> shifted32(48, 0);
        shifted32[0] = shifted32[1] = 15;
        const UINT data32[9] = {0, 1, 2, 6, 7, 8, 3, 4, 5};
        std::memcpy(shifted32.data() + 2, data32, sizeof(data32));
        auto ibShift32 = flatHistoryPressureIndexBuffer(dev, shifted32);
        ctx->IASetIndexBuffer(ibShift32.Get(), DXGI_FORMAT_R32_UINT, 8);
        for (const Row& row : shiftRows) oneRow("set 1c R32 index offset 8", 6, 0, 0, {0, 2, 6, 8}, row);
        // ... and the key T of the shifted buffer, start 6, reads the three words after G.
        for (const Row& row : {Row{48, 96, kHit}, Row{0, 48, kOutside}, Row{96, 97, kOutside}}) oneRow("set 1c R32 index offset 8 key T", 3, 6, 0, {3, 5}, row);
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
    }

    // ---- 1d. a set too wide or too ragged to hold exactly is held as the envelope alone, and then behaves as the envelope ---------------------
    {
        // A span past four million vertices: {0,1,2} and {5000000..5000002}.
        auto ibFar = flatHistoryPressureIndexBuffer(dev, words({0, 1, 2, 5000000, 5000001, 5000002}));
        ctx->IASetIndexBuffer(ibFar.Get(), DXGI_FORMAT_R32_UINT, 0);
        Hist h;
        h.advance(110);
        Hist::Capture c;
        check(h.capture(ctx, issue, 6, 1, 0, 0, 0, 110, c, true, true), "set 1d: a mesh spanning five million vertices is admitted");
        h.pollExtents(ctx, 111, true);
        const Set* s = h.vertexSetOf(c);
        check(s && !s->exact && s->runs.size() == 2 && s->runs[0] == 0 && s->runs[1] == 5000002 && s->low == 0 && s->high == 5000002 && h.writeStats().setApproximate == 1 &&
                  h.writeStats().extentRead == 1,
              "set 1d: its set is the envelope alone [0,5000002], marked inexact, counted as approximate and as read");
        h.advance(111);
        check(h.resourceWritten(vb.Get(), 48, 96, edvr::HistoryWriteEntry::Update) == 2 && h.writeStats().vertexGenuine == 1 && h.writeStats().vertexInGap == 0,
              "set 1d: a write to vertices 3..5, which the indices never name, is met: the envelope covers it (the price of not holding the gaps)");
        Hist g;
        g.advance(110);
        Hist::Capture d;
        check(g.capture(ctx, issue, 6, 1, 0, 0, 0, 110, d, true, true), "set 1d control: the same mesh in a second history");
        g.pollExtents(ctx, 111, true);
        g.advance(111);
        check(g.resourceWritten(vb.Get(), 5000003ull * 16, 5000004ull * 16, edvr::HistoryWriteEntry::Update) == 0 && g.writeStats().vertexOutside == 1,
              "set 1d: a write past the envelope is outside it");
        // The boundary of the span: exactly kMaxSetSpan = 4194304 vertices (0 .. 4194303) is held exactly; one more is not.
        for (const bool over : {false, true}) {
            const UINT top = over ? 4194304u : 4194303u;
            auto ibEdge = flatHistoryPressureIndexBuffer(dev, words({0, top, 0}));
            ctx->IASetIndexBuffer(ibEdge.Get(), DXGI_FORMAT_R32_UINT, 0);
            Hist e;
            e.advance(120);
            Hist::Capture ec;
            check(e.capture(ctx, issue, 3, 1, 0, 0, 0, 120, ec, true, true), "set 1d: a mesh at the span bound is admitted");
            e.pollExtents(ctx, 121, true);
            const Set* es = e.vertexSetOf(ec);
            check(es && es->exact == !over && e.writeStats().setApproximate == (over ? 1u : 0u) && es->low == 0 && es->high == top && es->runs.size() == (over ? 2u : 4u),
                  over ? "set 1d: a span of 4194305 vertices is held as the envelope alone" : "set 1d: a span of exactly 4194304 vertices is held exactly (runs [0,0] and [top,top])");
            e.advance(121);
            check(e.resourceWritten(vb.Get(), 16, 32, edvr::HistoryWriteEntry::Update) == (over ? 2u : 0u) && (over ? e.writeStats().vertexGenuine : e.writeStats().vertexInGap) == 1,
                  over ? "set 1d: and a write to vertex 1, between the two, is met" : "set 1d: and a write to vertex 1, between the two, is in the gap and spared");
        }
        // The run bound: 4096 runs are held, 4097 are not. Vertices 0, 2, 4, ... (every other one), padded with repeats to a multiple of three.
        for (const unsigned runs : {4096u, 4097u}) {
            std::vector<UINT> indices;
            for (unsigned i = 0; i < runs; ++i) indices.push_back(2 * i);
            while (indices.size() % 3) indices.push_back(0);
            auto ibRagged = flatHistoryPressureIndexBuffer(dev, indices);
            ctx->IASetIndexBuffer(ibRagged.Get(), DXGI_FORMAT_R32_UINT, 0);
            Hist r;
            r.advance(130);
            Hist::Capture rc;
            check(r.capture(ctx, issue, unsigned(indices.size()), 1, 0, 0, 0, 130, rc, true, true), "set 1d: a ragged mesh is admitted");
            r.pollExtents(ctx, 131, true);
            const Set* rs = r.vertexSetOf(rc);
            const bool exact = runs == 4096;
            check(rs && rs->exact == exact && rs->runs.size() == (exact ? 8192u : 2u) && rs->high == 2 * (runs - 1) && r.writeStats().setApproximate == (exact ? 0u : 1u),
                  exact ? "set 1d: 4096 runs of one vertex are held exactly" : "set 1d: 4097 runs are held as the envelope alone");
            r.advance(131);
            // vertex 1 sits between the first two single-vertex runs; vertex 2*(runs-1) is the last one.
            const unsigned w1 = r.resourceWritten(vb.Get(), 16, 32, edvr::HistoryWriteEntry::Update);
            check(exact ? (w1 == 0 && r.writeStats().vertexInGap == 1) : (w1 == 2 && r.writeStats().vertexGenuine == 1),
                  exact ? "set 1d: a write to the odd vertex between two runs is spared" : "set 1d: and the same write is met once the set is the envelope");
            Hist r2;
            r2.advance(130);
            Hist::Capture rc2;
            check(r2.capture(ctx, issue, unsigned(indices.size()), 1, 0, 0, 0, 130, rc2, true, true), "set 1d: the same mesh again");
            r2.pollExtents(ctx, 131, true);
            r2.advance(131);
            check(r2.resourceWritten(vb.Get(), 2ull * (runs - 1) * 16, 2ull * (runs - 1) * 16 + 16, edvr::HistoryWriteEntry::Update) == 2 && r2.writeStats().vertexGenuine == 1,
                  "set 1d: a write to the last vertex of the set is met either way");
        }
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
    }

    // The helpers of the sections below: a key of ibGap, captured at a frame, with the set read at once when asked.
    const auto keyG = [&](Hist& h, unsigned frame, Hist::Capture& c) { return h.capture(ctx, issue, 6, 1, 0, 0, 0, frame, c, true, true); };
    const auto keyT = [&](Hist& h, unsigned frame, Hist::Capture& c) { return h.capture(ctx, issue, 3, 1, 6, 0, 0, frame, c, true, true); };
    const auto kept = [](const Hist::Capture& c) { return c.candidateCount == 1 && !c.missed; };
    const auto lost = [](const Hist::Capture& c, HistoryGap gap) { return c.candidateCount == 0 && c.missed && c.miss.gap == gap; };
    const auto update = edvr::HistoryWriteEntry::Update;

    // ---- 2. THE LOOP: a vertex buffer written every frame ---------------------------------------------------------------------------------------
    // The hypothesis this build tests (design doc section 104): the first-person parts' vertex buffer is written every frame, the writes land on
    // some records' vertices and not others', and what a record keeps across the writes is decided by whether its set survives them. G = {0,1,2,6,7,8}
    // and T = {3,4,5} share the vertex buffer. Every frame, in the gap before the captures: a write on vertex 12 (outside both), then one on vertices
    // 3..5 (the gap of G, all of T). T is invalidated every frame, retained, revived by its own draw; G must keep its prior frame after frame, and
    // neither may read its set again.
    {
        Hist h;
        const unsigned first = 200, frames = 12;
        h.advance(first);
        Hist::Capture g, t;
        check(keyG(h, first, g) && keyT(h, first, t), "loop: both keys are admitted at the first frame");
        h.pollExtents(ctx, first + 1, true);
        check(sameRuns(h.vertexSetOf(g), {0, 2, 6, 8}) && sameRuns(h.vertexSetOf(t), {3, 5}) && h.writeStats().extentIssued == 2 && h.writeStats().extentRead == 2,
              "loop: both sets are read once, before the writes begin");
        unsigned gKept = 0, tLost = 0;
        for (unsigned f = first + 1; f <= first + frames; ++f) {
            h.advance(f);
            h.resourceWritten(vb.Get(), 192, 208, update);    // vertex 12: outside both sets
            h.resourceWritten(vb.Get(), 48, 96, update);      // vertices 3..5: G's gap, T's whole set
            Hist::Capture cg, ct;
            check(keyG(h, f, cg) && keyT(h, f, ct), "loop: both keys are admitted every frame");
            gKept += kept(cg);
            tLost += lost(ct, HistoryGap::InvalidatedVertices);
        }
        const auto& s = h.writeStats();
        check(gKept == frames, "loop: G, whose vertices the writes missed, kept its prior in every one of twelve frames");
        check(tLost == frames, "loop: T, whose vertices they hit, was a miss named invalidated-vertices in every one of them");
        check(s.vertexOutside == 2 * frames && s.vertexInGap == frames && s.vertexGenuine == frames && s.vertexUnknown == 0,
              "loop: the writes were classified twelve times over: outside for both records, in the gap of G, on the vertices of T, and never against an unknown set");
        check(s.recordsInvalidated[kUpdate] == frames && s.sparedRecords == 3 * frames && s.savedWrites[kUpdate] == frames,
              "loop: one record invalidated per frame, three spares per frame, and the write that missed everything saved");
        check(s.extentIssued == 2 && s.extentRead == 2 && s.setFromCache == 0 && s.setPending == 0 && s.allocations == 2 && h.recordCount() == 2,
              "loop: and across all twelve frames nothing was read again, no set was taken from the cache (they were never lost) and no record was made again");
        check(s.deferredInvalidated == 0 && s.deferredConservative == 0 && h.bytes() == 6 * 32 + 3 * 32, "loop: the deferred check never ran: the sets were known throughout");
    }
    {
        // The control: the same loop with the sets never read (the unknown window held open). Every write is spared and logged, no record falls to
        // the writes, and the cost shows when the sets arrive: the writes that met T's vertices take it then.
        Hist h;
        const unsigned first = 230;
        h.advance(first);
        Hist::Capture g, t;
        check(keyG(h, first, g) && keyT(h, first, t), "loop control: both keys are admitted at the first frame");
        unsigned gKept = 0, tKept = 0;
        for (unsigned f = first + 1; f <= first + 3; ++f) {
            h.advance(f);
            h.resourceWritten(vb.Get(), 192, 208, update);
            h.resourceWritten(vb.Get(), 48, 96, update);
            Hist::Capture cg, ct;
            check(keyG(h, f, cg) && keyT(h, f, ct), "loop control: both keys are admitted every frame");
            gKept += kept(cg);
            tKept += kept(ct);
        }
        check(gKept == 3 && tKept == 3 && h.writeStats().vertexUnknown == 12 && h.writeStats().vertexGenuine == 0 && h.writeStats().recordsInvalidated[kUpdate] == 0,
              "loop control: with the sets unknown no write takes a record: T, whose vertices the writes hit, keeps a prior it should not have (the cost of the window); twelve writes met an unknown set (two records, two writes, three frames)");
        h.pollExtents(ctx, first + 4, true);
        check(h.writeStats().extentRead == 2 && h.writeStats().deferredInvalidated == 1,
              "loop control: when the sets arrive the deferred check takes T (a logged write met its vertices after its last publish) and leaves G alone");
        Hist::Capture cg, ct;
        h.advance(first + 4);
        check(keyG(h, first + 4, cg) && keyT(h, first + 4, ct) && kept(cg) && lost(ct, HistoryGap::InvalidatedVertices),
              "loop control: and the next frame G still has its prior and T is a miss named invalidated-vertices");
    }

    // ---- 3. the unknown window: a write spared while the set is unknown is checked when it arrives ------------------------------------------
    {
        // A write on a vertex of the set.
        Hist h;
        h.advance(300);
        Hist::Capture c;
        check(keyG(h, 300, c), "unknown 3: the key is admitted and published");
        h.advance(301);
        check(h.resourceWritten(vb.Get(), 112, 128, update) == 0, "unknown 3: a write on vertex 7 is spared while the set is unknown");
        check(h.writeStats().vertexUnknown == 1 && h.writeStats().sparedRecords == 1 && h.writeStats().vertexGenuine == 0 && h.writeStats().recordsInvalidated[kUpdate] == 0,
              "unknown 3: it is counted as met with an unknown set, and nothing was invalidated");
        h.pollExtents(ctx, 301, true);
        check(h.writeStats().extentRead == 1 && h.writeStats().deferredInvalidated == 1 && h.writeStats().deferredConservative == 0,
              "unknown 3: when the set arrives the logged write is found to have met vertex 7: the record is invalidated (deferred-invalidated)");
        Hist::Capture next;
        check(keyG(h, 301, next) && lost(next, HistoryGap::InvalidatedVertices), "unknown 3: the key's next draw is a miss named invalidated-vertices");
        check(sameRuns(h.vertexSetOf(next), {0, 2, 6, 8}) && h.writeStats().extentIssued == 1, "unknown 3: and the revived record has its set");
    }
    {
        // The check places the write by the same arithmetic as the live decision: the buffer offset, and the base times the stride (signed). G placed at
        // byte 32 by an offset, by a base, or by both halves, and at byte -32 + 16 i by a negative base over indices that are all 2 or more.
        struct Route { UINT offset; int base; unsigned vertex7; unsigned vertex4; const char* name; };
        const Route routes[] = {{32, 0, 32 + 112, 32 + 64, "offset 32"}, {0, 2, 32 + 112, 32 + 64, "base 2"}, {16, 1, 32 + 112, 32 + 64, "offset 16 base 1"}};
        for (const Route& route : routes) {
            for (const bool genuine : {true, false}) {
                useVb(vb.Get(), route.offset);
                Hist h;
                h.advance(380);
                Hist::Capture c;
                check(h.capture(ctx, issue, 6, 1, 0, route.base, 0, 380, c, true, true), "unknown routes: the key is admitted and published");
                h.advance(381);
                const unsigned at = genuine ? route.vertex7 : route.vertex4;
                check(h.resourceWritten(vb.Get(), at, at + 16, update) == 0 && h.writeStats().vertexUnknown == 1, "unknown routes: a write is spared while the set is unknown");
                h.pollExtents(ctx, 381, true);
                char what[200];
                std::snprintf(what, sizeof(what), "unknown routes: %s: a write on %s is %s when the set arrives", route.name, genuine ? "vertex 7" : "vertex 4 (the gap)",
                              genuine ? "found to have met it" : "found between the vertices");
                check(h.writeStats().deferredInvalidated == (genuine ? 1u : 0u) && h.writeStats().deferredConservative == 0, what);
            }
        }
        useVb(vb.Get(), 0);
        auto ibHigh = flatHistoryPressureIndexBuffer(dev, words({2, 3, 4, 8, 9, 10}));
        ctx->IASetIndexBuffer(ibHigh.Get(), DXGI_FORMAT_R32_UINT, 0);
        for (const bool genuine : {true, false}) {
            Hist h;
            h.advance(385);
            Hist::Capture c;
            check(h.capture(ctx, issue, 6, 1, 0, -2, 0, 385, c, true, true), "unknown routes: a key with a negative base is admitted and published");
            h.advance(386);
            const unsigned at = genuine ? 96 : 48;   // index 8 is bytes [96,112); index 5 is bytes [48,64)
            check(h.resourceWritten(vb.Get(), at, at + 16, update) == 0 && h.writeStats().vertexUnknown == 1, "unknown routes: a write is spared while the set is unknown");
            h.pollExtents(ctx, 386, true);
            check(h.writeStats().deferredInvalidated == (genuine ? 1u : 0u),
                  genuine ? "unknown routes: with a negative base a write on index 8 is found to have met it" : "unknown routes: with a negative base a write on index 5 (the gap) is found between the vertices");
        }
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
    }
    {
        // The log of spared writes is shared by every record; each is checked against the writes to its own vertex buffer only. Two records of one index
        // range on two vertex buffers: a write on vertex 7 of the second is logged (it spared the second record) and means nothing to the first.
        const std::vector<float> more(verts.begin(), verts.end());
        auto vb2 = flatRangeBuffer(dev, more.data(), UINT(more.size() * 4), D3D11_BIND_VERTEX_BUFFER);
        Hist h;
        h.advance(390);
        Hist::Capture c1, c2;
        check(keyG(h, 390, c1), "unknown buffers: the key is admitted on the first vertex buffer");
        useVb(vb2.Get(), 0);
        check(keyG(h, 390, c2), "unknown buffers: the same index range is admitted on the second vertex buffer: another record, the same read");
        useVb(vb.Get(), 0);
        check(h.writeStats().extentIssued == 1 && h.writeStats().setPending == 1, "unknown buffers: one read serves both");
        h.advance(391);
        check(h.resourceWritten(vb2.Get(), 112, 128, update) == 0 && h.writeStats().vertexUnknown == 1, "unknown buffers: a write on vertex 7 of the second buffer is spared and logged");
        h.pollExtents(ctx, 391, true);
        check(h.writeStats().extentRead == 1 && h.writeStats().deferredInvalidated == 1, "unknown buffers: when the set arrives the write takes the second record and only that one");
        Hist::Capture n1, n2;
        check(keyG(h, 391, n1) && kept(n1), "unknown buffers: the first record keeps its prior");
        useVb(vb2.Get(), 0);
        check(keyG(h, 391, n2) && lost(n2, HistoryGap::InvalidatedVertices), "unknown buffers: and the second is a miss named invalidated-vertices");
        useVb(vb.Get(), 0);
    }
    {
        // A write in the gap: spared for good.
        Hist h;
        h.advance(310);
        Hist::Capture c;
        check(keyG(h, 310, c), "unknown 3: the key is admitted and published");
        h.advance(311);
        check(h.resourceWritten(vb.Get(), 48, 96, update) == 0 && h.writeStats().vertexUnknown == 1, "unknown 3: a write on vertices 3..5 is spared while the set is unknown");
        h.pollExtents(ctx, 311, true);
        check(h.writeStats().extentRead == 1 && h.writeStats().deferredInvalidated == 0 && h.writeStats().deferredConservative == 0,
              "unknown 3: when the set arrives the write is found between its vertices: nothing is taken");
        Hist::Capture next;
        check(keyG(h, 311, next) && kept(next), "unknown 3: the key's next draw keeps its prior");
        h.advance(312);
        h.pollExtents(ctx, 312, true);
        Hist::Capture later;
        check(keyG(h, 312, later) && kept(later) && h.writeStats().deferredInvalidated == 0, "unknown 3: and so does the one after: a spare is for good");
    }
    {
        // A write older than the record's last publish is not its business: the stamp was made after it. A write at the stamp's own frame is.
        Hist h;
        h.advance(320);
        Hist::Capture c;
        check(keyG(h, 320, c), "unknown 3: published at frame 320");
        h.advance(321);
        Hist::Capture again;
        check(keyG(h, 321, again), "unknown 3: published again at 321");
        check(h.resourceWritten(vb.Get(), 112, 128, update) == 0, "unknown 3: a write on vertex 7 at frame 321, after the capture, is spared (the set is unknown)");
        h.advance(322);
        Hist::Capture third;
        check(keyG(h, 322, third), "unknown 3: published again at 322, after the write");
        h.pollExtents(ctx, 322, true);
        check(h.writeStats().extentRead == 1 && h.writeStats().deferredInvalidated == 0 && h.writeStats().deferredConservative == 0,
              "unknown 3: the write is older than the last publish: the stamp of 322 was made after it, and the record is not taken");
        Hist g;
        g.advance(320);
        Hist::Capture gc;
        check(keyG(g, 320, gc), "unknown 3 control: published at frame 320");
        g.advance(321);
        Hist::Capture gagain;
        check(keyG(g, 321, gagain), "unknown 3 control: published again at 321");
        check(g.resourceWritten(vb.Get(), 112, 128, update) == 0, "unknown 3 control: the same write at frame 321, after the capture, is spared");
        g.pollExtents(ctx, 322, true);
        check(g.writeStats().deferredInvalidated == 1, "unknown 3 control: with no publish after it, the write is at the stamp's frame and counts: the record is taken");
    }
    // The ring: 128 spared writes are all remembered; one more and the oldest is lost, and a record published before the oldest remembered write is
    // assumed hit (it cannot be shown otherwise).
    for (const unsigned writes : {127u, 128u, 129u, 200u}) {
        Hist h;
        h.advance(330);
        Hist::Capture c;
        check(keyG(h, 330, c), "unknown ring: the key is admitted and published");
        h.advance(331);
        for (unsigned i = 0; i < writes; ++i) h.resourceWritten(vb.Get(), 192, 208, update);   // vertex 12: met nothing in any case
        check(h.writeStats().vertexUnknown == writes, "unknown ring: every write was spared on an unknown set");
        h.pollExtents(ctx, 331, true);
        const bool wrapped = writes > 128;
        char what[200];
        std::snprintf(what, sizeof(what), "unknown ring: %u writes that met nothing: %s", writes, wrapped ? "the log has wrapped past the record's publish: it is taken conservatively" : "the log holds all of them: none met a vertex, nothing is taken");
        check(h.writeStats().deferredConservative == (wrapped ? 1u : 0u) && h.writeStats().deferredInvalidated == 0, what);
        Hist::Capture next;
        check(keyG(h, 331, next) && (wrapped ? lost(next, HistoryGap::InvalidatedVertices) : kept(next)), "unknown ring: and the key's next draw is a miss iff it was taken");
    }
    {
        // The ring wraps, but the record was published after the oldest write that was kept: the log reaches back far enough.
        Hist h;
        h.advance(340);
        Hist::Capture c;
        check(keyG(h, 340, c), "unknown ring: published at 340");
        h.advance(341);
        for (unsigned i = 0; i < 150; ++i) h.resourceWritten(vb.Get(), 192, 208, update);
        h.advance(342);
        Hist::Capture again;
        check(keyG(h, 342, again), "unknown ring: published again at 342, after all the writes");
        h.pollExtents(ctx, 342, true);
        check(h.writeStats().deferredConservative == 0 && h.writeStats().deferredInvalidated == 0, "unknown ring: a record published after the oldest write that was kept is not assumed hit");
    }
    {
        // A write to the indices while the read is in flight cancels it; the record asks again, and the set it gets is the new indices'.
        std::vector<UINT> contents = words({0, 1, 2, 6, 7, 8, 3, 4, 5});
        auto ib = flatHistoryPressureIndexBuffer(dev, contents);
        ctx->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        Hist h;
        h.advance(350);
        Hist::Capture c;
        check(keyG(h, 350, c) && h.writeStats().extentIssued == 1, "unknown cancel: the key is admitted and its read issued");
        // The game rewrites the keys' indices after the copy was queued: {3,4,5, 9,10,11}.
        const UINT fresh[6] = {3, 4, 5, 9, 10, 11};
        const D3D11_BOX box{0, 0, 0, 24, 1, 1};
        ctx->UpdateSubresource(ib.Get(), 0, &box, fresh, 0, 0);
        h.advance(351);
        check(h.resourceWritten(ib.Get(), 0, 24, update) == 4 && h.writeStats().recordsInvalidated[kUpdate] == 1, "unknown cancel: a ranged write over the key's index bytes invalidates the record");
        h.pollExtents(ctx, 351, true);
        check(h.writeStats().setCancelled == 1 && h.writeStats().extentRead == 0 && h.cachedSets() == 0,
              "unknown cancel: the read in flight was cancelled, not adopted: the copy was made before the write and holds the old indices");
        Hist::Capture next;
        check(keyG(h, 351, next) && lost(next, HistoryGap::InvalidatedIndices) && h.writeStats().extentIssued == 2, "unknown cancel: the key's next draw asks again");
        h.pollExtents(ctx, 352, true);
        check(sameRuns(h.vertexSetOf(next), {3, 5, 9, 11}) && h.writeStats().extentRead == 1 && h.writeStats().setCancelled == 1,
              "unknown cancel: and the set it gets is the new indices': runs [3,5] and [9,11], not the old [0,2] and [6,8]");
        // The control: a write over other bytes of the buffer does not cancel.
        Hist k;
        k.advance(360);
        Hist::Capture kc;
        check(keyG(k, 360, kc), "unknown cancel control: the key is admitted");
        k.advance(361);
        check(k.resourceWritten(ib.Get(), 100, 104, update) == 0 && k.resourceWritten(vb.Get(), 192, 208, update) == 0, "unknown cancel control: writes to other bytes are spared");
        k.pollExtents(ctx, 361, true);
        check(k.writeStats().setCancelled == 0 && k.writeStats().extentRead == 1 && k.cachedSets() == 1, "unknown cancel control: the read completes and its set is cached");
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
    }

    // ---- 4. the cache: a set is kept by its index range, beyond the record ----------------------------------------------------------------
    for (const bool invalidated : {false, true}) {
        // A record that leaves (aged out, or invalidated and not redrawn in the keep window) and is made again with the same index range is given its
        // set at once: nothing is read, and the very first write to its vertices finds it known.
        Hist h;
        h.advance(400);
        Hist::Capture c;
        check(keyG(h, 400, c), "cache: the key is admitted");
        h.pollExtents(ctx, 401, true);
        check(h.writeStats().extentIssued == 1 && h.writeStats().extentRead == 1 && h.cachedSets() == 1, "cache: its set is read once and cached");
        if (invalidated) {
            h.advance(401);
            check(h.resourceWritten(vb.Get(), 112, 128, update) == 2, "cache: a write on vertex 7 invalidates the record");
            h.advance(405);
            check(h.recordCount() == 0 && h.writeStats().erased[unsigned(edvr::HistoryErase::AdvanceInvalidated)] == 1, "cache: the keep window passes and the invalidated record is erased");
        } else {
            h.advance(403);
            check(h.recordCount() == 0 && h.writeStats().erased[unsigned(edvr::HistoryErase::AdvanceAged)] == 1, "cache: three frames unused and the record is aged out");
        }
        check(h.cachedSets() == 1, "cache: the set outlives the record");
        Hist::Capture again;
        const unsigned frame = invalidated ? 405 : 403;
        check(keyG(h, frame, again), "cache: the key is drawn again");
        check(h.writeStats().setFromCache == 1 && h.writeStats().extentIssued == 1 && h.writeStats().allocations == 2 && again.missed,
              "cache: a new record is made, given its set from the cache, and nothing is read again");
        check(sameRuns(h.vertexSetOf(again), {0, 2, 6, 8}), "cache: and it has the set at once, before any poll");
        h.advance(frame + 1);
        check(h.resourceWritten(vb.Get(), 48, 96, update) == 0 && h.writeStats().vertexInGap == 1 && h.writeStats().vertexUnknown == 0,
              "cache: so the first write to its vertices is classified (in the gap), not spared as unknown");
    }
    {
        // What ends a cached set, and what does not. The record is aged out after the write and made again: from the cache iff the entry survived.
        struct Event { const char* name; bool ends; };
        const std::vector<unsigned char> zeros(64, 0);
        auto stranger = flatRangeBuffer(dev, zeros.data(), 64, D3D11_BIND_INDEX_BUFFER);
        const auto scenario = [&](const Event& event, const auto& doWrite) {
            Hist h;
            h.advance(430);
            Hist::Capture c;
            check(keyG(h, 430, c), "cache ends: the key is admitted");
            h.pollExtents(ctx, 431, true);
            char what[300];
            check(h.cachedSets() == 1 && h.vertexSetOf(c) != nullptr, "cache ends: its set is read and cached");
            h.advance(431);
            doWrite(h);
            std::snprintf(what, sizeof(what), "cache ends: %s %s the cached set", event.name, event.ends ? "ends" : "does not end");
            check(h.cachedSets() == (event.ends ? 0u : 1u), what);
            std::snprintf(what, sizeof(what), "cache ends: %s: the record %s its set", event.name, event.ends ? "forgets" : "keeps");
            check((h.vertexSetOf(c) == nullptr) == event.ends, what);
            h.advance(440);
            Hist::Capture again;
            check(keyG(h, 440, again), "cache ends: the key is drawn again after the record left");
            std::snprintf(what, sizeof(what), "cache ends: %s: the new record %s", event.name, event.ends ? "reads its set again" : "is given its set from the cache");
            check(h.writeStats().setFromCache == (event.ends ? 0u : 1u) && h.writeStats().extentIssued == (event.ends ? 2u : 1u), what);
        };
        scenario({"a ranged write to the middle of the index range", true}, [&](Hist& h) { h.resourceWritten(ibGap.Get(), 12, 16, update); });
        scenario({"a ranged write to the first byte of the index range", true}, [&](Hist& h) { h.resourceWritten(ibGap.Get(), 0, 1, update); });
        scenario({"a ranged write to the last byte of the index range", true}, [&](Hist& h) { h.resourceWritten(ibGap.Get(), 23, 24, update); });
        scenario({"a ranged write that covers the end of the range and goes on past it", true}, [&](Hist& h) { h.resourceWritten(ibGap.Get(), 20, 200, update); });
        scenario({"a write to the whole index buffer", true}, [&](Hist& h) { h.resourceWritten(ibGap.Get(), 0, whole, edvr::HistoryWriteEntry::Map); });
        scenario({"a write with no range to the index buffer", true}, [&](Hist& h) { h.resourceWritten(ibGap.Get()); });
        scenario({"a write nothing could name (a null resource)", true}, [&](Hist& h) { h.resourceWritten(nullptr); });
        scenario({"a ranged write just after the index range", false}, [&](Hist& h) { h.resourceWritten(ibGap.Get(), 24, 28, update); });
        scenario({"a ranged write far from the index range", false}, [&](Hist& h) { h.resourceWritten(ibGap.Get(), 100, 104, update); });
        scenario({"a whole write to the vertex buffer", false}, [&](Hist& h) { h.resourceWritten(vb.Get(), 0, whole, edvr::HistoryWriteEntry::Map); });
        scenario({"a ranged write on a vertex of the set", false}, [&](Hist& h) { h.resourceWritten(vb.Get(), 112, 128, update); });
        scenario({"a ranged write between the vertices", false}, [&](Hist& h) { h.resourceWritten(vb.Get(), 48, 96, update); });
        scenario({"a write to a buffer no record reads", false}, [&](Hist& h) { h.resourceWritten(stranger.Get(), 0, 16, update); h.resourceWritten(stranger.Get()); });
        // Two index buffers: a write to one ends its sets and not the other's.
        auto ibTwin = flatHistoryPressureIndexBuffer(dev, words({0, 1, 2, 6, 7, 8, 3, 4, 5}));
        Hist h;
        h.advance(450);
        Hist::Capture a, b;
        check(keyG(h, 450, a), "cache ends: the key on the first buffer is admitted");
        ctx->IASetIndexBuffer(ibTwin.Get(), DXGI_FORMAT_R32_UINT, 0);
        check(keyG(h, 450, b), "cache ends: the same key on the second buffer is admitted: a different key, with a read of its own");
        h.pollExtents(ctx, 451, true);
        check(h.cachedSets() == 2 && h.writeStats().extentRead == 2, "cache ends: two buffers, two cached sets");
        h.advance(451);
        h.resourceWritten(ibTwin.Get(), 0, 24, update);
        check(h.cachedSets() == 1 && h.vertexSetOf(a) != nullptr && h.vertexSetOf(b) == nullptr, "cache ends: a write to the second buffer ends the second buffer's set and the first's stands");
        h.advance(460);
        Hist::Capture a2, b2;
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
        check(keyG(h, 460, a2), "cache ends: the first buffer's key is drawn again");
        check(h.writeStats().setFromCache == 1, "cache ends: and takes its set from the cache");
        ctx->IASetIndexBuffer(ibTwin.Get(), DXGI_FORMAT_R32_UINT, 0);
        check(keyG(h, 460, b2) && h.writeStats().setFromCache == 1 && h.writeStats().extentIssued == 3, "cache ends: the second buffer's key reads again");
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
    }
    {
        // The cache is keyed by the format as well as the bytes: the same four-byte-aligned range read as 16-bit and as 32-bit names other vertices.
        std::vector<unsigned short> shorts(48, 0);
        const unsigned short data[6] = {1, 0, 2, 0, 3, 0};
        std::memcpy(shorts.data(), data, sizeof(data));
        auto ibMixed = flatRangeBuffer(dev, shorts.data(), UINT(shorts.size() * 2), D3D11_BIND_INDEX_BUFFER);
        Hist h;
        h.advance(470);
        Hist::Capture c16, c32;
        ctx->IASetIndexBuffer(ibMixed.Get(), DXGI_FORMAT_R16_UINT, 0);
        check(h.capture(ctx, issue, 6, 1, 0, 0, 0, 470, c16, true, true), "cache format: six 16-bit indices are admitted");
        ctx->IASetIndexBuffer(ibMixed.Get(), DXGI_FORMAT_R32_UINT, 0);
        check(h.capture(ctx, issue, 3, 1, 0, 0, 0, 470, c32, true, true), "cache format: three 32-bit indices over the same twelve bytes are admitted");
        h.pollExtents(ctx, 471, true);
        check(h.writeStats().extentIssued == 2 && h.writeStats().extentRead == 2 && h.cachedSets() == 2, "cache format: two reads, two cached sets: the same bytes under two formats");
        check(sameRuns(h.vertexSetOf(c16), {0, 3}) && sameRuns(h.vertexSetOf(c32), {1, 3}),
              "cache format: the 16-bit reading names vertices 0..3 and the 32-bit reading 1..3");
        h.advance(475);
        Hist::Capture d16, d32;
        ctx->IASetIndexBuffer(ibMixed.Get(), DXGI_FORMAT_R16_UINT, 0);
        check(h.capture(ctx, issue, 6, 1, 0, 0, 0, 475, d16, true, true), "cache format: both are drawn again after their records left");
        ctx->IASetIndexBuffer(ibMixed.Get(), DXGI_FORMAT_R32_UINT, 0);
        check(h.capture(ctx, issue, 3, 1, 0, 0, 0, 475, d32, true, true), "cache format: both are drawn again after their records left (32-bit)");
        check(h.writeStats().setFromCache == 2 && sameRuns(h.vertexSetOf(d16), {0, 3}) && sameRuns(h.vertexSetOf(d32), {1, 3}),
              "cache format: each takes its own set from the cache");
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
    }
    {
        // The cache is bounded: 256 sets, and the one used longest ago goes. Three hundred keys of one index buffer, one read each.
        std::vector<UINT> many(330 * 3);
        for (size_t i = 0; i < many.size(); i += 3) { many[i] = 0; many[i + 1] = 1; many[i + 2] = 2; }
        auto ibMany = flatHistoryPressureIndexBuffer(dev, many);
        ctx->IASetIndexBuffer(ibMany.Get(), DXGI_FORMAT_R32_UINT, 0);
        const auto key = [&](Hist& h, unsigned frame, unsigned i, Hist::Capture& c) { return h.capture(ctx, issue, 3, 1, 3 * i, 0, 0, frame, c, true, true); };
        Hist h;
        const unsigned f = 500;
        for (unsigned i = 0; i < 300; ++i) {
            h.advance(f + i);
            Hist::Capture c;
            check(key(h, f + i, i, c), "cache bound: a key is admitted");
            h.pollExtents(ctx, f + i, true);
        }
        check(h.writeStats().extentIssued == 300 && h.writeStats().extentRead == 300 && h.cachedSets() == 256, "cache bound: three hundred reads and the cache holds 256 sets");
        h.advance(f + 310);
        Hist::Capture k299, k44, k43;
        check(key(h, f + 310, 299, k299) && h.writeStats().setFromCache == 1, "cache bound: the newest key is given its set from the cache");
        check(key(h, f + 310, 44, k44) && h.writeStats().setFromCache == 2, "cache bound: so is the oldest one that is still held (key 44 of 0..299: the first 44 were dropped)");
        check(key(h, f + 310, 43, k43) && h.writeStats().setFromCache == 2 && h.writeStats().extentIssued == 301, "cache bound: key 43 was dropped: it reads again");
        h.pollExtents(ctx, f + 310, true);
        check(h.cachedSets() == 256 && h.writeStats().extentRead == 301, "cache bound: and its set takes the place of the one used longest ago; the cache is still 256");
        h.advance(f + 320);
        Hist::Capture k45, k44b;
        check(key(h, f + 320, 45, k45) && h.writeStats().setFromCache == 2 && h.writeStats().extentIssued == 302,
              "cache bound: key 45, used longest ago when key 43 came back, is the one that went: it reads again");
        check(key(h, f + 320, 44, k44b) && h.writeStats().setFromCache == 3,
              "cache bound: key 44, used again just before, survived: the one dropped is the one used longest ago, not the one stored first");
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
    }
    {
        // The registry of index buffers is capped at 256: the 257th forgets everything held (a set no write could end is never kept), cancels the
        // reads in flight and takes every record's set away, then registers itself.
        std::vector<ComPtr<ID3D11Buffer>> buffers;
        for (unsigned i = 0; i < 257; ++i) buffers.push_back(flatHistoryPressureIndexBuffer(dev, words({0, 1, 2}, 3)));
        Hist h;
        const unsigned f = 800;
        Hist::Capture last, previous;
        for (unsigned i = 0; i < 256; ++i) {
            ctx->IASetIndexBuffer(buffers[i].Get(), DXGI_FORMAT_R32_UINT, 0);
            h.advance(f + i);
            Hist::Capture c;
            check(h.capture(ctx, issue, 3, 1, 0, 0, 0, f + i, c, true, true), "registry: a key on its own index buffer is admitted");
            h.pollExtents(ctx, f + i, true);
            if (i == 254) previous = c;
            if (i == 255) last = c;
        }
        check(h.cachedSets() == 256 && h.writeStats().extentRead == 256 && h.vertexSetOf(last) != nullptr && h.vertexSetOf(previous) != nullptr,
              "registry: 256 index buffers, 256 cached sets, and the live records have theirs");
        ctx->IASetIndexBuffer(buffers[256].Get(), DXGI_FORMAT_R32_UINT, 0);
        h.advance(f + 256);
        Hist::Capture c;
        check(h.capture(ctx, issue, 3, 1, 0, 0, 0, f + 256, c, true, true), "registry: the 257th index buffer is admitted");
        check(h.cachedSets() == 0 && h.vertexSetOf(last) == nullptr && h.vertexSetOf(previous) == nullptr && h.vertexSetOf(c) == nullptr,
              "registry: and it reset everything held: no cached set, no record with a set");
        h.pollExtents(ctx, f + 257, true);
        check(h.writeStats().setCancelled == 1 && h.writeStats().extentRead == 256 && h.cachedSets() == 0, "registry: its own read, in flight when the reset came, was cancelled");
        h.advance(f + 257);
        Hist::Capture again;
        check(h.capture(ctx, issue, 3, 1, 0, 0, 0, f + 257, again, true, true) && h.writeStats().extentIssued == 258 && h.writeStats().setFromCache == 0,
              "registry: its next draw asks again");
        h.pollExtents(ctx, f + 258, true);
        check(h.cachedSets() == 1 && h.vertexSetOf(again) != nullptr && h.writeStats().extentRead == 257, "registry: and the cache starts over with that one set");
        ctx->IASetIndexBuffer(buffers[0].Get(), DXGI_FORMAT_R32_UINT, 0);
        h.advance(f + 270);
        Hist::Capture old;
        check(h.capture(ctx, issue, 3, 1, 0, 0, 0, f + 270, old, true, true) && h.writeStats().setFromCache == 0 && h.writeStats().extentIssued == 259,
              "registry: the first buffer's key reads again: nothing it had survived the reset");
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
    }

    // ---- 5. the examples: a few writes that met a vertex buffer, with the record's set and where in it they fell --------------------------
    {
        auto ibFour = flatHistoryPressureIndexBuffer(dev, words({0, 1, 2, 6, 7, 8, 0, 1, 2, 6, 7, 8, 0, 1, 2, 6, 7, 8, 0, 1, 2, 6, 7, 8}));
        ctx->IASetIndexBuffer(ibFour.Get(), DXGI_FORMAT_R32_UINT, 0);
        const auto at = [&](Hist& h, unsigned frame, unsigned start, Hist::Capture& c) { return h.capture(ctx, issue, 6, 1, start, 0, 0, frame, c, true, true); };
        Hist h;
        h.advance(600);
        Hist::Capture k1, k2, k3, k4;
        check(at(h, 600, 0, k1) && at(h, 600, 6, k2) && at(h, 600, 12, k3), "examples: three keys of one shape are admitted");
        h.pollExtents(ctx, 601, true);
        h.advance(601);
        check(at(h, 601, 18, k4), "examples: a fourth key is admitted a frame later, its set not read");
        edvr::HistoryWriteExample none[16];
        check(h.takeWriteExamples(none, 16) == 0, "examples: nothing has met a vertex buffer yet");
        // In the gap of all four, then on vertex 7 of all four: three known records and one unknown each time.
        check(h.resourceWritten(vb.Get(), 48, 96, update) == 0, "examples: a write on vertices 3..5 is spared by all four records");
        check(h.resourceWritten(vb.Get(), 112, 128, update) == 2, "examples: a write on vertex 7 takes the three whose sets are known");
        check(h.writeStats().vertexInGap == 3 && h.writeStats().vertexGenuine == 3 && h.writeStats().vertexUnknown == 2,
              "examples: the counters saw three in-gap, three genuine and two unknown events");
        edvr::HistoryWriteExample ex[16];
        const unsigned n = h.takeWriteExamples(ex, 16);
        check(n == 6, "examples: but only two of each case are kept: six examples");
        check(ex[0].kind == edvr::HistoryWriteCase::Unknown && ex[1].kind == edvr::HistoryWriteCase::Unknown && ex[2].kind == edvr::HistoryWriteCase::InGap &&
                  ex[3].kind == edvr::HistoryWriteCase::InGap && ex[4].kind == edvr::HistoryWriteCase::Genuine && ex[5].kind == edvr::HistoryWriteCase::Genuine,
              "examples: unknown first, then in-gap, then genuine, two of each");
        check(ex[0].entry == edvr::HistoryWriteEntry::Update && ex[0].resource == vb.Get() && ex[0].first == 48 && ex[0].end == 96 && !ex[0].known && ex[0].spanFirst == 0 &&
                  ex[0].spanEnd == 0 && ex[0].runs == 0 && !ex[0].exact && ex[0].age == 0 && ex[0].key.start == 18 && ex[0].key.count == 6 && ex[0].key.vertices == vb.Get() &&
                  ex[0].key.indices == ibFour.Get() && ex[0].key.stride == 16 && ex[0].key.format == unsigned(DXGI_FORMAT_R32_UINT),
              "examples: the first unknown one names the write [48,96), the entry, the resource, no set, age 0 and the whole key of the record made that frame");
        check(ex[1].first == 112 && ex[1].end == 128 && !ex[1].known && ex[1].key.start == 18, "examples: the second names the later write on the same unknown record");
        check(ex[2].first == 48 && ex[2].end == 96 && ex[2].known && ex[2].spanFirst == 0 && ex[2].spanEnd == 144 && ex[2].exact && ex[2].runs == 2 && ex[2].age == 1 &&
                  ex[2].key.start == 0 && ex[3].key.start == 6 && ex[3].first == 48,
              "examples: an in-gap one names the write, the set's envelope [0,144) in bytes of the vertex buffer, exact, two runs, age 1, and the first two records in order");
        check(ex[4].first == 112 && ex[4].end == 128 && ex[4].known && ex[4].spanEnd == 144 && ex[4].exact && ex[4].runs == 2 && ex[4].age == 1 && ex[4].key.start == 0 &&
                  ex[5].key.start == 6,
              "examples: a genuine one names the write on vertex 7 and the same envelope");
        check(h.takeWriteExamples(ex, 16) == 0, "examples: a second take is empty: they are forgotten once taken");
        // Collected again after a take.
        Hist again;
        again.advance(602);
        Hist::Capture ac;
        check(at(again, 602, 0, ac), "examples: a key in a history that has been taken from");
        again.pollExtents(ctx, 603, true);
        again.advance(603);
        check(again.resourceWritten(vb.Get(), 48, 96, update) == 0 && again.takeWriteExamples(ex, 16) == 1 && ex[0].kind == edvr::HistoryWriteCase::InGap,
              "examples: a window collects again after a take");
        // A write outside the set is counted and has no example (it is the common case, and says nothing about the record).
        Hist out;
        out.advance(605);
        Hist::Capture oc;
        check(at(out, 605, 0, oc), "examples: a record for the outside case");
        out.pollExtents(ctx, 606, true);
        out.advance(606);
        check(out.resourceWritten(vb.Get(), 1000, 1004, update) == 0 && out.writeStats().vertexOutside == 1 && out.takeWriteExamples(ex, 16) == 0,
              "examples: a write outside the set is counted outside and keeps no example");
        // Two per case, however many writes: 20 writes on the gap of one record are two examples.
        Hist m;
        m.advance(610);
        Hist::Capture mc;
        check(at(m, 610, 0, mc), "examples: a record for the cap");
        m.pollExtents(ctx, 611, true);
        m.advance(611);
        for (unsigned i = 0; i < 20; ++i) m.resourceWritten(vb.Get(), 48 + i, 49 + i, update);
        check(m.writeStats().vertexInGap == 20 && m.takeWriteExamples(ex, 16) == 2 && ex[0].first == 48 && ex[1].first == 49,
              "examples: twenty events of one case are counted twenty times and kept twice, the first two");
        // Capacity: a caller with room for one gets one and loses nothing it did not take.
        Hist cap;
        cap.advance(620);
        Hist::Capture cc;
        check(at(cap, 620, 0, cc), "examples: a record for the capacity case");
        cap.pollExtents(ctx, 621, true);
        cap.advance(621);
        cap.resourceWritten(vb.Get(), 48, 96, update);
        cap.resourceWritten(vb.Get(), 50, 60, update);
        check(cap.takeWriteExamples(ex, 1) == 1 && cap.takeWriteExamples(ex, 16) == 0, "examples: a take with room for one returns one, and the rest are forgotten with it");
        // Only for counted writes. An uncounted notification (a second report of one write) invalidates and is logged, and collects nothing.
        Hist u;
        u.advance(630);
        Hist::Capture uc;
        check(at(u, 630, 0, uc), "examples: a record for the uncounted case");
        u.pollExtents(ctx, 631, true);
        u.advance(631);
        check(u.resourceWritten(vb.Get(), 48, 96, update, edvr::HistoryWriteTiming::Gap, false) == 0 && u.resourceWritten(vb.Get(), 112, 128, update, edvr::HistoryWriteTiming::Gap, false) == 2,
              "examples: uncounted notifications still decide: the gap write is spared and the write on vertex 7 invalidates");
        check(u.writeStats().vertexInGap == 0 && u.writeStats().vertexGenuine == 0 && u.writeStats().observed[kUpdate] == 0 && u.takeWriteExamples(ex, 16) == 0,
              "examples: and counts nothing and keeps no example");
        Hist v;
        v.advance(640);
        Hist::Capture vc;
        check(at(v, 640, 0, vc), "examples: a record whose set is not read, for the uncounted unknown case");
        v.advance(641);
        check(v.resourceWritten(vb.Get(), 112, 128, update, edvr::HistoryWriteTiming::Gap, false) == 0 && v.writeStats().vertexUnknown == 0 && v.takeWriteExamples(ex, 16) == 0,
              "examples: an uncounted write on an unknown set counts nothing and keeps no example");
        v.pollExtents(ctx, 641, true);
        check(v.writeStats().deferredInvalidated == 1, "examples: but it was logged: when the set arrives, the check finds it met vertex 7 and takes the record");
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
    }

    // ---- 6. the adapter's lists are judged as the history's records are ---------------------------------------------------------------------
    {
        Hist h;
        h.advance(700);
        Hist::Capture c;
        check(keyG(h, 700, c), "mirror: the key is admitted");
        const std::vector<unsigned char> zeros(64, 0);
        auto stranger = flatRangeBuffer(dev, zeros.data(), 64, D3D11_BIND_VERTEX_BUFFER);
        check(!h.captureHitBy(c, vb.Get(), 112, 128) && !h.captureHitBy(c, vb.Get(), 48, 96) && !h.captureHitBy(c, vb.Get(), 0, 16),
              "mirror: while the set is unknown a ranged write to the vertex buffer spares the draw, whatever it names");
        check(h.captureHitBy(c, ibGap.Get(), 0, 24) && h.captureHitBy(c, ibGap.Get(), 23, 24) && !h.captureHitBy(c, ibGap.Get(), 24, 28) && !h.captureHitBy(c, ibGap.Get(), 100, 104),
              "mirror: an index write hits the draw iff it overlaps its index range");
        check(h.captureHitBy(c, vb.Get()) && h.captureHitBy(c, ibGap.Get()) && h.captureHitBy(c, nullptr) && !h.captureHitBy(c, stranger.Get(), 0, 16) && !h.captureHitBy(c, stranger.Get()),
              "mirror: a write with no range, or none to name, hits it; a buffer it does not read does not");
        h.pollExtents(ctx, 701, true);
        check(h.captureHitBy(c, vb.Get(), 112, 128) && h.captureHitBy(c, vb.Get(), 47, 49) && !h.captureHitBy(c, vb.Get(), 48, 96) && !h.captureHitBy(c, vb.Get(), 144, 160) &&
                  !h.captureHitBy(c, vb.Get(), 95, 96) && h.captureHitBy(c, vb.Get(), 95, 97),
              "mirror: once the set is read the same capture is judged by it: vertex 7 and the boundary bytes hit, the gap and past the envelope do not");
        // The history's own decision for the same writes agrees.
        Hist g;
        g.advance(710);
        Hist::Capture gc;
        check(keyG(g, 710, gc), "mirror: the same key in a second history");
        g.pollExtents(ctx, 711, true);
        g.advance(711);
        check(g.resourceWritten(vb.Get(), 48, 96, update) == 0 && g.resourceWritten(vb.Get(), 112, 128, update) == 2, "mirror: and the history agrees with the list on both");
    }

    // ---- 7. what the design costs, and the four places where the first version of it was unsound (now fixed) -----------------------------------
    // The first run of this rig found four places where the history did not do what a sound reading of its design does. They are fixed in production
    // and each is a test below with the opposite outcome kept as its control: ring wrap in the frame of the publish, a failed read that forgot the
    // writes it had spared, a cache keyed by a buffer's address, and a record taken at birth by a write that came before its capture.
    {
        // THE COST OF THE WINDOW. A write on a vertex that is spared while the set is unknown is caught when the set arrives. The frames in between
        // have already been drawn: the draw of the very next frame is handed the prior stamped before the write.
        Hist h;
        h.advance(820);
        Hist::Capture c;
        check(keyG(h, 820, c), "window: the key is admitted and published");
        h.advance(821);
        check(h.resourceWritten(vb.Get(), 112, 128, update) == 0, "window: a write on vertex 7 is spared while the set is unknown");
        Hist::Capture next;
        check(keyG(h, 821, next) && kept(next), "window: and the next frame's draw is handed the prior from before the write (the cost of the window: one frame of stale motion)");
        h.pollExtents(ctx, 821, true);
        check(h.writeStats().deferredInvalidated == 1, "window: the check finds the write when the set arrives and takes the record, which cannot take back the frame that was drawn");
        h.advance(822);
        Hist::Capture after;
        check(keyG(h, 822, after) && lost(after, HistoryGap::InvalidatedVertices), "window: the frame after that has no prior");
    }
    {
        // FIX 1. The log of spared writes is a ring of 128. A record published at or before the oldest write the ring still holds is taken on trust
        // ("conservative"): the writes the ring lost were no newer than the oldest it kept, and one in the frame of the record's own stamp counts. Here
        // a write on vertex 7 is followed by 128 harmless ones, all in the frame of the publish: the first falls out of the ring.
        Hist h;
        h.advance(830);
        Hist::Capture c;
        check(keyG(h, 830, c), "ring same frame: the key is admitted and published at frame 830");
        check(h.resourceWritten(vb.Get(), 112, 128, update) == 0, "ring same frame: a write on vertex 7 at frame 830, spared while the set is unknown");
        for (unsigned i = 0; i < 128; ++i) h.resourceWritten(vb.Get(), 192, 208, update);
        h.advance(831);
        h.pollExtents(ctx, 831, true);
        check(h.writeStats().deferredConservative == 1 && h.writeStats().deferredInvalidated == 0,
              "ring same frame: the write on vertex 7 was lost from the ring in the frame of the publish: the record is taken conservatively");
        Hist::Capture next;
        check(keyG(h, 831, next) && lost(next, HistoryGap::InvalidatedVertices), "ring same frame: and the key's next draw is a miss named invalidated-vertices");
        // The control: with 127 harmless writes after it the ring holds the write on vertex 7 itself, and it is that write that takes the record.
        Hist k;
        k.advance(830);
        Hist::Capture kc;
        check(keyG(k, 830, kc), "ring same frame control: the key is admitted and published at frame 830");
        k.resourceWritten(vb.Get(), 112, 128, update);
        for (unsigned i = 0; i < 127; ++i) k.resourceWritten(vb.Get(), 192, 208, update);
        k.advance(831);
        k.pollExtents(ctx, 831, true);
        check(k.writeStats().deferredInvalidated == 1 && k.writeStats().deferredConservative == 0, "ring same frame control: with the ring not wrapped the write itself is found: met, not assumed");
        // The same ring one frame later is taken too.
        Hist g;
        g.advance(830);
        Hist::Capture gc;
        check(keyG(g, 830, gc), "ring next frame: the key is admitted and published at frame 830");
        g.advance(831);
        g.resourceWritten(vb.Get(), 112, 128, update);
        for (unsigned i = 0; i < 128; ++i) g.resourceWritten(vb.Get(), 192, 208, update);
        g.pollExtents(ctx, 831, true);
        check(g.writeStats().deferredConservative == 1, "ring next frame: the same writes one frame after the publish are taken conservatively");
    }
    {
        // FIX 2. A read that fails (the staging copy still holds its sentinel: here a 16-bit mesh that really names vertex 65535; or the map does not
        // come ready for 16 frames) gives the record the whole-buffer set, and the writes it spared while its set was unknown are checked on the way:
        // against the whole buffer, so any of them takes the record.
        const unsigned short maxIndices[24] = {0, 1, 2, 6, 7, 0xFFFF};
        auto ibMax = flatRangeBuffer(dev, maxIndices, sizeof(maxIndices), D3D11_BIND_INDEX_BUFFER);
        ctx->IASetIndexBuffer(ibMax.Get(), DXGI_FORMAT_R16_UINT, 0);
        Hist h;
        h.advance(840);
        Hist::Capture c;
        check(keyG(h, 840, c), "failed read: the key is admitted and published");
        h.advance(841);
        check(h.resourceWritten(vb.Get(), 112, 128, update) == 0 && h.writeStats().vertexUnknown == 1, "failed read: a write on vertex 7 is spared while the set is unknown");
        h.pollExtents(ctx, 841, true);
        check(h.writeStats().extentFailed == 1 && h.writeStats().extentRead == 0 && h.cachedSets() == 0, "failed read: the read failed (the sentinel survived)");
        check(h.writeStats().deferredInvalidated == 1 && h.writeStats().deferredConservative == 0,
              "failed read: the write it spared is checked on the way to the whole-buffer set and takes the record");
        Hist::Capture next;
        check(keyG(h, 841, next) && lost(next, HistoryGap::InvalidatedVertices), "failed read: the key's next draw is a miss named invalidated-vertices");
        // From then on the record is read as every vertex: a ranged write anywhere on its buffer takes it, and is not counted as a genuine rewrite.
        h.advance(842);
        check(h.resourceWritten(vb.Get(), 1000, 1004, update) == 2 && h.writeStats().vertexGenuine == 0 && h.writeStats().recordsInvalidated[kUpdate] == 1,
              "failed read: afterwards any ranged write to the record's vertex buffer takes it (the whole-buffer set), uncounted as a rewrite");
        // The retry is thirty frames after the failure, not sooner and not never.
        Hist::Capture drawn;
        for (unsigned f = 842; f <= 870; ++f) {
            if (f != 842) h.advance(f);
            check(keyG(h, f, drawn), "failed read: the key is drawn in every frame of the retry window");
        }
        check(h.writeStats().extentIssued == 1, "failed read: and nothing is asked for again in the thirty frames after the failure");
        h.advance(871);
        check(keyG(h, 871, drawn) && h.writeStats().extentIssued == 2, "failed read: on the thirtieth frame the read is asked for again");
        h.pollExtents(ctx, 871, true);
        check(h.writeStats().extentFailed == 2, "failed read: and fails again the same way");
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
    }
    {
        // FIX 2, the paths that decide at once that the indices cannot be read (here: the draw's index range runs past the end of its buffer). The
        // record is published when this happens, so a write of the same frame that came before is on the log and takes it: conservative, at birth.
        auto ibShort = flatHistoryPressureIndexBuffer(dev, words({0, 1, 2}, 3));   // 12 bytes; a draw of six indices reads 24
        for (const bool sameFrame : {true, false}) {
            Hist h;
            h.advance(870);
            Hist::Capture u;
            check(keyG(h, 870, u) && h.writeStats().extentIssued == 1, "early whole: a record with an unknown set is admitted");
            check(h.resourceWritten(vb.Get(), 112, 128, update) == 0 && h.writeStats().vertexUnknown == 1, "early whole: a write on vertex 7 is spared and logged, in frame 870");
            if (!sameFrame) h.advance(871);
            ctx->IASetIndexBuffer(ibShort.Get(), DXGI_FORMAT_R32_UINT, 0);
            Hist::Capture e;
            const unsigned frame = sameFrame ? 870 : 871;
            check(keyG(h, frame, e) && h.writeStats().extentIssued == 1 && h.vertexSetOf(e) == nullptr,
                  "early whole: a key whose range runs past its buffer is admitted and never asked to be read: it holds the whole-buffer set");
            check(h.writeStats().deferredInvalidated == (sameFrame ? 1u : 0u) && h.writeStats().extentFailed == 0,
                  sameFrame ? "early whole: the write of the same frame is checked against the whole buffer and takes the record at birth"
                            : "early whole control: a write of the frame before is older than the record's stamp and takes nothing");
            h.advance(frame + 1);
            Hist::Capture next;
            check(keyG(h, frame + 1, next) && (sameFrame ? lost(next, HistoryGap::InvalidatedVertices) : kept(next)),
                  sameFrame ? "early whole: and its next draw is a miss named invalidated-vertices" : "early whole control: and its next draw keeps its prior");
            ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
        }
    }
    {
        // FIX 3. The cache is keyed by the index buffer's identity, the byte range and the format; a cached set outlives the records and the buffers
        // they held. The identity is a number kept with the buffer itself (private data), not its address, which a buffer made after another was
        // released can reuse. First what is deterministic: every buffer a record reads gets a number, once, no two the same.
        static const GUID kId = {0x5d1c1f0a, 0x91c3, 0x4b7e, {0x8f, 0x54, 0x2a, 0x73, 0x0e, 0x66, 0xa1, 0x7d}};
        const auto idOf = [&](ID3D11Buffer* buffer) {
            uint64_t id = 0;
            UINT size = sizeof(id);
            if (FAILED(buffer->GetPrivateData(kId, &size, &id)) || size != sizeof(id)) return uint64_t(0);
            return id;
        };
        auto ibOne = flatHistoryPressureIndexBuffer(dev, words({0, 1, 2, 6, 7, 8, 3, 4, 5}));
        auto ibTwo = flatHistoryPressureIndexBuffer(dev, words({0, 1, 2, 6, 7, 8, 3, 4, 5}));
        check(idOf(ibOne.Get()) == 0 && idOf(ibTwo.Get()) == 0, "buffer id: a buffer nothing has read has no number");
        Hist h;
        h.advance(880);
        Hist::Capture a, b;
        ctx->IASetIndexBuffer(ibOne.Get(), DXGI_FORMAT_R32_UINT, 0);
        check(keyG(h, 880, a), "buffer id: a key on the first buffer is admitted");
        ctx->IASetIndexBuffer(ibTwo.Get(), DXGI_FORMAT_R32_UINT, 0);
        check(keyG(h, 880, b), "buffer id: the same key on the second buffer is admitted");
        const uint64_t first = idOf(ibOne.Get()), second = idOf(ibTwo.Get());
        check(first != 0 && second != 0 && first != second, "buffer id: each buffer has a number now, and they differ");
        h.pollExtents(ctx, 881, true);
        h.advance(885);
        ctx->IASetIndexBuffer(ibOne.Get(), DXGI_FORMAT_R32_UINT, 0);
        Hist::Capture a2;
        check(keyG(h, 885, a2) && h.writeStats().setFromCache == 1 && idOf(ibOne.Get()) == first && idOf(ibTwo.Get()) == second,
              "buffer id: the number stays with the buffer: the key drawn again is given the first buffer's cached set, and no number changed");
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
    }
    {
        // FIX 3, the reproduction. A buffer destroyed and another made at the same address with the same size, whose bytes are not the same, must NOT
        // be given the first one's set. This tries to make the address recur (the device did within a few attempts on every run so far); if it never
        // hands it back in a few hundred attempts the check is skipped (the number of buffers made, and so of checks run, varies from run to run).
        Hist h;
        const void* oldAddress = nullptr;
        {
            auto ibA = flatHistoryPressureIndexBuffer(dev, words({0, 1, 2, 6, 7, 8}, 6));
            oldAddress = ibA.Get();
            ctx->IASetIndexBuffer(ibA.Get(), DXGI_FORMAT_R32_UINT, 0);
            h.advance(850);
            Hist::Capture c;
            check(keyG(h, 850, c), "address reuse: the key is admitted on the first buffer");
            h.pollExtents(ctx, 851, true);
            check(sameRuns(h.vertexSetOf(c), {0, 2, 6, 8}) && h.cachedSets() == 1, "address reuse: its set is read and cached");
            ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
        }
        h.advance(855);
        check(h.recordCount() == 0, "address reuse: the record is aged out and the history holds no reference to the first buffer");
        ctx->Flush();
        std::vector<ComPtr<ID3D11Buffer>> keep;
        ComPtr<ID3D11Buffer> reused;
        for (unsigned attempt = 0; attempt < 300 && !reused; ++attempt) {
            auto candidate = flatHistoryPressureIndexBuffer(dev, words({3, 4, 5, 9, 10, 11}, 6));
            if (static_cast<const void*>(candidate.Get()) == oldAddress) reused = candidate; else keep.push_back(candidate);
        }
        if (reused) {
            ctx->IASetIndexBuffer(reused.Get(), DXGI_FORMAT_R32_UINT, 0);
            h.advance(856);
            Hist::Capture c2;
            check(keyG(h, 856, c2), "address reuse: the key is admitted on the second buffer, at the first one's address");
            check(h.vertexSetOf(c2) == nullptr && h.writeStats().setFromCache == 0 && h.writeStats().extentIssued == 2,
                  "address reuse: a buffer made at a destroyed buffer's address is given NO cached set: it is read for itself");
            h.pollExtents(ctx, 857, true);
            check(sameRuns(h.vertexSetOf(c2), {3, 5, 9, 11}), "address reuse: and the set it gets is its own: runs [3,5] and [9,11], not the destroyed buffer's [0,2] and [6,8]");
        }
        ctx->IASetIndexBuffer(ibGap.Get(), DXGI_FORMAT_R32_UINT, 0);
    }
    {
        // FIX 4. A record made again gets its set from the cache at the end of its own capture, after it was published. Every write logged so far came
        // before the draw's positions were captured, so none can be stale for it and none is checked: the record is not taken at birth.
        Hist h;
        h.advance(860);
        Hist::Capture g;
        check(keyG(h, 860, g), "birth: the key is admitted");
        h.pollExtents(ctx, 861, true);
        h.advance(863);
        check(h.recordCount() == 0 && h.cachedSets() == 1, "birth: its record is aged out and its set stays cached");
        Hist::Capture t;
        check(keyT(h, 863, t), "birth: another key of the buffer is admitted: its set is unknown");
        check(h.resourceWritten(vb.Get(), 112, 128, update) == 0 && h.writeStats().vertexUnknown == 1, "birth: a write on vertex 7 is spared by it and logged, in frame 863");
        Hist::Capture again;
        check(keyG(h, 863, again) && h.writeStats().setFromCache == 1, "birth: the first key is made again in the same frame, after the write, and given its set from the cache");
        check(h.writeStats().deferredInvalidated == 0 && h.writeStats().deferredConservative == 0 && sameRuns(h.vertexSetOf(again), {0, 2, 6, 8}),
              "birth: the write that came before its capture does not take it: the new stamp already holds it");
        h.advance(864);
        Hist::Capture next;
        check(keyG(h, 864, next) && kept(next), "birth: and the key's next draw keeps its prior");
        // A write AFTER the capture is still caught: it is logged for the other record, and the cached set is known, so the live decision classifies it.
        h.advance(865);
        check(h.resourceWritten(vb.Get(), 112, 128, update) == 2, "birth: a write on vertex 7 after the capture is a genuine rewrite of the record that has its set");
    }
    useVb(vb.Get(), 0);
    ctx->ClearState();
    bind(Pose{}, false);
}
