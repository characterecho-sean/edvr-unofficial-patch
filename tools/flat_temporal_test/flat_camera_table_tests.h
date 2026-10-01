#pragma once

// The flat runtime's camera table and the draw path's kept answer from it (src\d3d11\flat_camera_table.h).
//
// Every draw used to search the table for the bound b1, copy the 96 bytes and hash them; on foot that
// was about 5,000 draws a frame. The answer is now kept, and made afresh only when something it depends
// on has changed. The ways that could lie are a stale answer served after a change (the record a draw
// gets would then carry the rows of a camera that is gone, and the reducer would judge the frame on
// them) and a change the answer was never told of. What this holds to:
//
//   - the kept answer is, byte for byte, the answer today's algorithm gives. The reference below is
//     that algorithm, transcribed from flat_runtime.cpp as it stood before the table moved: over
//     tens of thousands of random operations -- binds and rebinds, writes, maps, unmaps, updates, the
//     game's state going unknown, frame boundaries, resets, more buffers than the 64 entries hold --
//     every draw's lookup and every entry of the table agree with the reference
//   - a hit is a hit: with nothing changed the answer is served and no fresh lookup is made
//   - each thing the answer depends on forces a fresh lookup when it changes: the bound buffer, that
//     slot's binding generation (a rebind to the very same buffer too), the frame, and every operation
//     that can change an entry: a claim, a write into the buffer, a map, a capture (Map/Unmap,
//     UpdateSubresource), the game's state going unknown, the frame boundary, a reset
//   - each of those, switched off in turn in a copy of the table, is noticed by the same checks: ten
//     tables that skip one bump or one key each fail

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/d3d11/flat_camera_table.h"

namespace flat_camera_rig {
using namespace edvr;

struct Holder {
    const void* p = nullptr;
    const void* Get() const { return p; }
};

// Today's algorithm: the camera table, its lookup and the draw path's block, as flat_runtime.cpp had them.
struct Reference {
    struct Camera {
        const void* buffer = nullptr;
        uint32_t width = 0;
        uint64_t frame = 0;
        uint32_t sequence = 0;
        void* mapped = nullptr;
        bool valid = false;
        unsigned char rows[kFlatCameraBytes]{};
    };
    Camera cameras[64]{};
    uint32_t cameraCount = 0;
    bool uncertain = false;   // camera(add) found every entry this frame's or mapped

    Camera* camera(const void* resource, bool add, uint32_t width, uint64_t frame) {
        for (uint32_t i = 0; i < cameraCount; ++i)
            if (cameras[i].buffer == resource) return &cameras[i];
        if (!add || !resource) return nullptr;
        uint32_t index = cameraCount;
        if (index == 64) {
            for (uint32_t i = 0; i < 64; ++i)
                if (cameras[i].frame != frame && !cameras[i].mapped) { index = i; break; }
            if (index == 64) { uncertain = true; return nullptr; }
        } else {
            ++cameraCount;
        }
        Camera& c = cameras[index];
        c = Camera{};
        c.buffer = resource;
        c.width = width;
        return &c;
    }
    void capture(Camera& c, const void* bytes, uint64_t frame, uint32_t sequence) {
        c.valid = flatCaptureCameraRows(c.rows, bytes, c.width);
        c.frame = frame;
        c.sequence = sequence;
    }
    // The draw scope's block: the rows of the buffer bound at b1, when they are valid and of this frame.
    FlatCameraRows lookup(const void* b1, uint64_t frame) {
        FlatCameraRows r;
        if (Camera* c = camera(b1, false, 0, frame)) {
            if (c->valid && c->frame == frame) {
                std::memcpy(r.rows, c->rows, sizeof(r.rows));
                r.have = true;
                r.hash = flatCameraHash(r.rows);
                r.epoch = c->frame;
                r.sequence = c->sequence;
            }
        }
        return r;
    }
    void invalidateAll() { for (Camera& c : cameras) c.valid = false; }
    void newFrame() {
        for (uint32_t i = 0; i < cameraCount; ++i) { cameras[i].valid = false; cameras[i].mapped = nullptr; }
    }
    void clear() {
        for (Camera& c : cameras) c = Camera{};
        cameraCount = 0;
    }
};

constexpr unsigned kBuffers = 100;                               // more than the table holds
constexpr uint32_t kWide = kFlatCameraOffset + kFlatCameraBytes + 32;
constexpr uint32_t kNarrow = kFlatCameraOffset + kFlatCameraBytes - 16;   // too small to carry the rows

// The game's side of it, and the runtime's: one table under test, the reference beside it, and what the
// runtime keeps around the table (the b1 binding and its generation, the frame, the write sequence).
template <class Table>
class Driver {
public:
    Table table;
    Reference ref;
    const void* b1 = nullptr;
    uint32_t bindGeneration = 1;
    uint64_t frame = 1;
    uint32_t sequence = 0;
    std::string why;
    bool ok = true;
    uint64_t lookups = 0, fresh = 0;

    Driver() {
        for (auto& m : memory_) m.assign(kWide, 0);
        scribbleAll(0x1234567u);
    }

    const void* id(unsigned i) const { return &slot_[i % kBuffers]; }

    // --- the runtime's operations, applied to the table under test and to the reference alike ----------
    // VSSetConstantBuffers to slot 1: the shadow's generation moves, and a buffer that can carry the rows joins.
    void bind(unsigned i, uint32_t width = kWide) {
        b1 = id(i);
        ++bindGeneration;
        expectFresh_ = true;
        join(i, width);
    }
    // A different buffer under an unchanged generation: not something the shadow does, a contract of the key.
    void bindIdentityOnly(unsigned i) {
        if (id(i) == b1) return;   // the same buffer: nothing moved
        b1 = id(i);
        expectFresh_ = true;
    }
    // The same buffer bound again: the generation moves anyway.
    void rebindSame() { ++bindGeneration; expectFresh_ = true; }
    // The frame moves without the table being told (the key alone must notice).
    void frameOnly() { ++frame; expectFresh_ = true; }
    // The camera(add) path: a buffer joins the table (or was there).
    void join(unsigned i, uint32_t width = kWide) {
        if (width < kFlatCameraOffset + kFlatCameraBytes) return;   // the runtime refuses a buffer too narrow
        if (table.find(id(i))) {
            (void)ref.camera(id(i), true, width, frame);
            return;
        }
        Holder h;
        h.p = id(i);
        const auto* claimed = table.claim(h, width, frame);
        Reference::Camera* rc = ref.camera(id(i), true, width, frame);
        if ((claimed != nullptr) != (rc != nullptr)) fail("a claim and the reference's camera(add) disagree on whether the table was full");
        if (claimed) expectFresh_ = true;
    }
    // Map, WRITE_DISCARD: the write invalidates the rows and the buffer is mapped until Unmap.
    void map(unsigned i) {
        written(i);
        if (const auto* c = table.find(id(i))) {
            table.setMapped(*c, mem(i));
            expectFresh_ = true;
        }
        if (Reference::Camera* rc = ref.camera(id(i), false, 0, frame)) rc->mapped = mem(i);
    }
    // Unmap: the write the map carried is captured, then the buffer is no longer mapped.
    void unmap(unsigned i) {
        if (const auto* c = table.find(id(i))) {
            if (c->mapped) table.capture(*c, c->mapped, frame, ++sequence);
            table.setMapped(*c, nullptr);
            expectFresh_ = true;
        }
        if (Reference::Camera* rc = ref.camera(id(i), false, 0, frame)) {
            if (rc->mapped) ref.capture(*rc, rc->mapped, frame, sequence);
            rc->mapped = nullptr;
        }
    }
    // UpdateSubresource: a write, then, whole, the capture of its bytes.
    void update(unsigned i, bool whole) {
        written(i);
        if (const auto* c = table.find(id(i))) {
            if (whole) table.capture(*c, mem(i), frame, ++sequence);
            expectFresh_ = true;
        }
        if (Reference::Camera* rc = ref.camera(id(i), false, 0, frame)) {
            if (whole) ref.capture(*rc, mem(i), frame, sequence);
        }
    }
    // The table's own contracts, each apart from the operations the runtime always pairs it with: a capture
    // with no write or map beside it, and a map noted with no write beside it. The runtime never issues
    // these two alone, so a bump that only they carry would be redundant there; the table promises it
    // anyway, and the rig holds it to the promise.
    void captureOnly(unsigned i) {
        if (const auto* c = table.find(id(i))) {
            table.capture(*c, mem(i), frame, ++sequence);
            expectFresh_ = true;
        }
        if (Reference::Camera* rc = ref.camera(id(i), false, 0, frame)) ref.capture(*rc, mem(i), frame, sequence);
    }
    void mappedOnly(unsigned i) {
        if (const auto* c = table.find(id(i))) {
            table.setMapped(*c, mem(i));
            expectFresh_ = true;
        }
        if (Reference::Camera* rc = ref.camera(id(i), false, 0, frame)) rc->mapped = mem(i);
    }
    // A copy or clear into the buffer: the write alone.
    void written(unsigned i) {
        if (const auto* c = table.find(id(i))) {
            table.invalidate(*c);
            expectFresh_ = true;
        }
        if (Reference::Camera* rc = ref.camera(id(i), false, 0, frame)) rc->valid = false;
    }
    void unknown() {
        table.invalidateAll();
        ref.invalidateAll();
        expectFresh_ = true;
    }
    void newFrame() {
        ++frame;
        table.newFrame();
        ref.newFrame();
        expectFresh_ = true;
    }
    // The table told the frame boundary but the frame number did not move: what only the table's own bump catches.
    void newFrameSameNumber() {
        table.newFrame();
        ref.newFrame();
        expectFresh_ = true;
    }
    void reset() {
        table.clear();
        ref.clear();
        expectFresh_ = true;
    }
    // The game writes camera constants into the buffer's memory (between its Map and Unmap, or before an Update).
    void scribble(unsigned i, uint32_t seed) {
        uint32_t x = seed ? seed : 1u;
        for (uint32_t k = 0; k < kWide; ++k) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            memory_[i % kBuffers][k] = static_cast<unsigned char>(x);
        }
    }

    // --- the check: one draw's lookup, judged three ways ------------------------------------------------
    // The answer against today's, the table against today's, and whether the lookup was made afresh exactly
    // when something changed. Then a second lookup, which must be a hit and the same answer.
    bool check(const char* after) {
        ++lookups;
        const uint64_t before = table.refreshes();
        const FlatCameraRows got = table.rows(b1, bindGeneration, frame);
        const bool madeFresh = table.refreshes() != before;
        if (madeFresh) ++fresh;
        if (madeFresh != expectFresh_)
            fail(std::string(after) + (expectFresh_ ? ": the answer was kept across a change it depends on"
                                                    : ": the answer was made afresh though nothing had changed"));
        expectFresh_ = false;
        compareAnswer(got, after);
        const uint64_t again = table.refreshes();
        const FlatCameraRows second = table.rows(b1, bindGeneration, frame);
        if (table.refreshes() != again) fail(std::string(after) + ": a second lookup with nothing changed was not a hit");
        compareAnswer(second, after);
        compareTables(after);
        return ok;
    }

private:
    void fail(const std::string& m) {
        if (ok) why = m;
        ok = false;
    }
    void* mem(unsigned i) { return memory_[i % kBuffers].data(); }
    void scribbleAll(uint32_t seed) { for (unsigned i = 0; i < kBuffers; ++i) scribble(i, seed + i * 7919u); }
    void compareAnswer(const FlatCameraRows& got, const char* after) {
        const FlatCameraRows want = ref.lookup(b1, frame);
        if (got.have != want.have || got.hash != want.hash || got.epoch != want.epoch || got.sequence != want.sequence ||
            std::memcmp(got.rows, want.rows, sizeof(got.rows)) != 0)
            fail(std::string(after) + ": the answer differs from today's algorithm's");
    }
    void compareTables(const char* after) {
        if (table.count() != ref.cameraCount) {
            fail(std::string(after) + ": the table holds a different number of entries than today's");
            return;
        }
        for (uint32_t i = 0; i < table.count(); ++i) {
            const auto& e = table.entry(i);
            const auto& r = ref.cameras[i];
            if (e.identity != r.buffer || e.width != r.width || e.frame != r.frame || e.sequence != r.sequence ||
                e.mapped != r.mapped || e.valid != r.valid || std::memcmp(e.rows, r.rows, sizeof(e.rows)) != 0) {
                fail(std::string(after) + ": an entry differs from today's");
                return;
            }
        }
    }
    char slot_[kBuffers]{};
    std::vector<unsigned char> memory_[kBuffers];
    bool expectFresh_ = true;   // the first lookup of anything is fresh
};

// The faults: each is a table that skips one of its bumps or one of its key's parts.
using NoFaults = FlatCameraNoFaults;
struct FaultSkipClaim : FlatCameraNoFaults { static constexpr bool skipBumpClaim = true; };
struct FaultSkipInvalidate : FlatCameraNoFaults { static constexpr bool skipBumpInvalidate = true; };
struct FaultSkipMapped : FlatCameraNoFaults { static constexpr bool skipBumpMapped = true; };
struct FaultSkipCapture : FlatCameraNoFaults { static constexpr bool skipBumpCapture = true; };
struct FaultSkipInvalidateAll : FlatCameraNoFaults { static constexpr bool skipBumpInvalidateAll = true; };
struct FaultSkipNewFrame : FlatCameraNoFaults { static constexpr bool skipBumpNewFrame = true; };
struct FaultSkipClear : FlatCameraNoFaults { static constexpr bool skipBumpClear = true; };
struct FaultIgnoreB1 : FlatCameraNoFaults { static constexpr bool ignoreB1 = true; };
struct FaultIgnoreBindGeneration : FlatCameraNoFaults { static constexpr bool ignoreBindGeneration = true; };
struct FaultIgnoreFrame : FlatCameraNoFaults { static constexpr bool ignoreFrame = true; };

// One scripted scene: every event that must force a fresh lookup, each after a lookup that was a hit, and
// the answers they change. Then a long random run.
template <class Faults>
bool runAll(std::string* why) {
    using Table = FlatCameraTable<Holder, Faults>;
    {
        Driver<Table> d;
        auto step = [&](const char* what) { return d.check(what); };
        // Two cameras join, A is bound and written: A's rows are the answer.
        d.bind(1);            step("bind A");
        d.join(2);            step("B joins");
        d.map(1);             step("map A");
        d.scribble(1, 11);
        d.unmap(1);           step("unmap A (the capture)");
        d.check("a hit");
        d.check("another hit");
        // Each event that must be noticed, after hits.
        d.bind(2);            step("bind B (another buffer, a new generation)");
        d.map(2); d.scribble(2, 22); d.unmap(2);  step("B captured");
        d.bind(1);            step("bind A again");
        d.rebindSame();       step("A bound again: the generation alone moves");
        d.bindIdentityOnly(2); step("B under A's generation: the identity alone moves");
        d.bind(1);            step("bind A");
        d.frameOnly();        step("the frame alone moves");
        d.newFrame();         step("the frame boundary");
        d.newFrameSameNumber(); step("the boundary without a new frame number");
        d.update(1, true);    step("UpdateSubresource, whole");
        d.check("a hit again");
        d.update(1, false);   step("UpdateSubresource, partial: a write, no capture");
        d.written(1);         step("a copy into A");
        d.map(1); d.scribble(1, 33); d.unmap(1); step("A captured again");
        d.scribble(1, 55);
        d.captureOnly(1);     step("a capture with nothing beside it");
        d.mappedOnly(1);      step("a map noted with nothing beside it");
        d.mappedOnly(3);      step("a map noted for a buffer that is not in the table: nothing changes");
        d.written(3);         step("a write into a buffer that is not in the table: nothing changes");
        d.unknown();          step("the game's state went unknown");
        d.map(1); d.scribble(1, 44); d.unmap(1); step("A captured after that");
        d.join(4);            step("D joins");
        d.join(4);            step("D joins again: already there, nothing changes");
        d.reset();            step("the tables are reset");
        d.bind(5, kNarrow);   step("a buffer too narrow to carry the rows is not admitted");
        if (!d.ok) { if (why) *why = "scripted: " + d.why; return false; }
    }
    {
        // A full table, and today's replacement rule: the 65th buffer takes the first entry of an earlier frame
        // that is not mapped (the first entry is mapped, so the second goes); once every entry is this frame's
        // or mapped, nothing is replaced and the claim says so.
        Driver<Table> d;
        for (unsigned i = 0; i < 64; ++i) d.join(i);
        d.bind(0);
        d.check("64 buffers joined");
        d.map(0);
        d.check("the first entry mapped");
        d.join(64);
        d.check("the 65th replaces the first entry that is not mapped");
        for (unsigned i = 1; i <= 64; ++i) { d.map(i); d.scribble(i, 900 + i); d.unmap(i); }
        d.check("every other entry captured this frame");
        d.join(65);
        d.check("nothing replaceable: the claim fails and nothing changes");
        d.newFrame();
        d.join(65);
        d.check("a new frame: the earlier frame's entries can be replaced again");
        if (!d.ok) { if (why) *why = "full table: " + d.why; return false; }
    }
    {
        // The random run: 100 buffers for 64 entries, so the table fills and replaces; a tenth of the
        // buffers too narrow to join; more than half the operations change something, far more than on foot,
        // so that every kind of change follows every other.
        Driver<Table> d;
        uint32_t x = 0x2545F491u;
        auto next = [&] { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; };
        for (int op = 0; op < 30000 && d.ok; ++op) {
            const unsigned i = next() % kBuffers;
            const unsigned pick = next() % 1000;
            const char* what = "a draw";
            if (pick < 420) { what = "a draw"; }
            else if (pick < 500) { d.bind(i, i % 10 == 9 ? kNarrow : kWide); what = "bind"; }
            else if (pick < 530) { d.rebindSame(); what = "rebind"; }
            else if (pick < 600) { d.join(i, i % 10 == 9 ? kNarrow : kWide); what = "join"; }
            else if (pick < 700) { d.map(i); d.scribble(i, next()); d.unmap(i); what = "map, write, unmap"; }
            else if (pick < 760) { d.update(i, next() % 3 != 0); d.scribble(i, next()); what = "update"; }
            else if (pick < 820) { d.written(i); what = "a write"; }
            else if (pick < 820 + 5) { d.captureOnly(i); what = "a capture alone"; }
            else if (pick < 830) { d.map(i); what = "map without an unmap yet"; }
            else if (pick < 840) { d.mappedOnly(i); what = "a map noted alone"; }
            else if (pick < 850) { d.unmap(i); what = "unmap"; }
            else if (pick < 860) { d.unknown(); what = "unknown"; }
            else if (pick < 900) { d.newFrame(); what = "a frame boundary"; }
            else if (pick < 904) { d.reset(); what = "a reset"; }
            else if (pick < 930) { d.frameOnly(); what = "the frame alone"; }
            else if (pick < 950) { d.bindIdentityOnly(i); what = "the identity alone"; }
            else { what = "a draw"; }
            d.check(what);
        }
        if (!d.ok) { if (why) *why = "random: " + d.why; return false; }
    }
    return true;
}

}  // namespace flat_camera_rig

inline int flatCameraTableTests() {
    using namespace edvr;
    using namespace flat_camera_rig;
    int failures = 0;
    auto expect = [&](bool ok, const std::string& name) {
        if (!ok) { std::printf("FAIL: camera table %s\n", name.c_str()); ++failures; }
    };

    std::string why;
    const bool agrees = runAll<NoFaults>(&why);
    expect(agrees, "the kept answer agrees with today's algorithm, and is made afresh exactly when it must be: " + why);

    // The tables that skip one thing each: every one is noticed.
    struct Case { bool (*run)(std::string*); const char* what; };
    const Case faults[] = {
        {&runAll<FaultSkipClaim>, "a claim that does not bump the table's generation"},
        {&runAll<FaultSkipInvalidate>, "a write into the buffer that does not"},
        {&runAll<FaultSkipMapped>, "a map that does not"},
        {&runAll<FaultSkipCapture>, "a capture (Map/Unmap, UpdateSubresource) that does not"},
        {&runAll<FaultSkipInvalidateAll>, "the game's state going unknown, without a bump"},
        {&runAll<FaultSkipNewFrame>, "the frame boundary without a bump"},
        {&runAll<FaultSkipClear>, "a reset without a bump"},
        {&runAll<FaultIgnoreB1>, "a key without the bound buffer's identity"},
        {&runAll<FaultIgnoreBindGeneration>, "a key without the binding generation (a rebind of the same buffer)"},
        {&runAll<FaultIgnoreFrame>, "a key without the frame"},
    };
    for (const Case& c : faults) {
        std::string reason;
        expect(!c.run(&reason), std::string("the rig does not notice: ") + c.what);
    }

    // The numbers the doc quotes: on a draw-heavy stream the answer is made afresh for a small share of the draws.
    {
        using Table = FlatCameraTable<Holder>;
        Driver<Table> d;
        d.bind(1);
        d.map(1); d.scribble(1, 5); d.unmap(1);
        for (int i = 0; i < 100; ++i) {
            if (i % 50 == 0) { d.map(1); d.scribble(1, 100 + i); d.unmap(1); }   // two writes in a hundred draws
            d.check("a draw");
        }
        expect(d.ok && d.fresh == 2,
               "a hundred draws with two writes among them make the answer afresh only for the writes: " + std::to_string(d.fresh));
    }
    return failures;
}
