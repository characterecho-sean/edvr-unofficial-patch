// The flat runtime's camera table, and the draw path's kept answer from it.
//
// WHAT IT HOLDS. Every constant buffer the game has bound to VS b1 that is wide enough to carry the
// camera rows (rows 270..275 of the scene constants, flat_temporal_model.h), up to 64 of them, each with
// the last 96 bytes captured from a complete write of it: whether they are usable, for which frame, in
// which write sequence. flat_runtime.cpp's tees (Map, Unmap, UpdateSubresource, the copies into a buffer)
// and trackers (VSSetConstantBuffers, ClearState, the frame boundary) maintain it, and every draw takes
// the rows of the buffer bound at b1 out of it, for the frame's contract records.
//
// WHY A KEPT ANSWER. On foot in a hangar (flight 053745, Epic, build ce6d511a) every draw searched the table
// for the bound b1 (up to 64 pointer compares), copied the 96 bytes and hashed them (FNV-1a over 96 bytes):
// about 5,000 draws a frame, 0.44 ms of the render thread by the census's reckoning, its clocks included.
// The answer changes only when the table does, when the binding does or when the frame does: the game
// writes its camera buffers about a hundred times a frame and draws a few thousand. So the answer is kept
// and recomputed only when something it depends on has changed.
//
// WHAT IT DEPENDS ON. A kept answer is served only while all of these are what they were when it was made:
//   - the buffer bound at b1 (its identity);
//   - that slot's binding generation (binding_shadow.h: bumped by every VSSetConstantBuffers covering slot
//     1, even to the same buffer, by ClearState, and once a frame);
//   - the frame (a capture is current only in the frame it was made in);
//   - the table's own generation, which EVERY operation that can change an entry below bumps: claim (a new
//     or replaced entry), invalidate (a write into the buffer), setMapped, capture (Map/Unmap,
//     UpdateSubresource, a copy into it), invalidateAll (the game's state became unknown), newFrame and clear.
// Nothing else reaches the answer: it is a function of those four and the table. The table's entries are
// only changed through the operations above (find hands out const pointers), so there is no way to change
// one and forget to say so, and the rig (tools\flat_temporal_test\flat_camera_table_tests.h) holds the
// table to today's algorithm over long random sequences and breaks each bump in turn to see it noticed.
//
// The algorithm itself is the one flat_runtime.cpp always had, moved here unchanged so that it can be
// tested: the linear search of the first `count` entries, the replacement of the first entry of an
// earlier frame that is not mapped when all 64 are taken, capture's three assignments, the lookup's
// "valid and captured this frame" rule. The produced record is byte for byte the one it produced.
#pragma once

#include <cstdint>
#include <cstring>
#include <utility>

#include "flat_temporal_model.h"

namespace edvr {

// What a draw takes from the table: the camera rows of the buffer bound at VS b1, or nothing.
struct FlatCameraRows {
    bool have = false;
    unsigned char rows[kFlatCameraBytes]{};
    uint64_t hash = 0;       // flatCameraHash(rows)
    uint64_t epoch = 0;      // the frame the rows were captured in (the record's writeEpoch)
    uint32_t sequence = 0;   // the write sequence (the record's writeSeq)
};

// Nothing skipped. The rig instantiates the table with each of these set in turn, to see that its checks
// notice a bump or a key that is missing; production never does.
struct FlatCameraNoFaults {
    static constexpr bool skipBumpClaim = false, skipBumpInvalidate = false, skipBumpMapped = false,
                          skipBumpCapture = false, skipBumpInvalidateAll = false, skipBumpNewFrame = false,
                          skipBumpClear = false, ignoreB1 = false, ignoreBindGeneration = false, ignoreFrame = false;
};

// `Holder` keeps a buffer alive and names it: a COM smart pointer in the runtime (Get() is the buffer's
// address, and the reference means the address cannot be reused while the entry stands), a plain
// struct in the rig.
template <class Holder, class Faults = FlatCameraNoFaults>
class FlatCameraTable {
public:
    static constexpr uint32_t kCapacity = 64;

    struct Entry {
        Holder buffer;
        const void* identity = nullptr;   // buffer.Get(), what a search compares
        uint32_t width = 0;
        uint64_t frame = 0;               // the frame of the last capture
        uint32_t sequence = 0;            // its write sequence
        void* mapped = nullptr;           // while the game has the buffer mapped for writing
        bool valid = false;               // the rows below are a complete write's
        unsigned char rows[kFlatCameraBytes]{};
    };

    // ---- reading: never changes anything, never invalidates ------------------------------------------
    uint32_t count() const noexcept { return count_; }
    const Entry& entry(uint32_t i) const noexcept { return entries_[i]; }
    const Entry* find(const void* identity) const noexcept {
        if (!identity) return nullptr;
        for (uint32_t i = 0; i < count_; ++i)
            if (entries_[i].identity == identity) return &entries_[i];
        return nullptr;
    }

    // ---- changing: each says so ----------------------------------------------------------------------
    // A new entry for `buffer` (the caller has checked that it is a constant buffer wide enough for the
    // camera rows), or, with all 64 taken, in place of the first entry of an earlier frame that is not
    // mapped. nullptr when every entry is this frame's or mapped: nothing changed.
    Entry* claim(Holder buffer, uint32_t width, uint64_t frame) noexcept {
        uint32_t index = count_;
        if (index == kCapacity) {
            for (uint32_t i = 0; i < kCapacity; ++i) {
                if (entries_[i].frame != frame && !entries_[i].mapped) { index = i; break; }
            }
            if (index == kCapacity) return nullptr;
        } else {
            ++count_;
        }
        Entry& e = entries_[index];
        e = Entry{};
        e.buffer = std::move(buffer);
        e.identity = static_cast<const void*>(e.buffer.Get());
        e.width = width;
        bump<Faults::skipBumpClaim>();
        return &e;
    }
    // Something wrote into the buffer (Map, UpdateSubresource, a copy into it): the rows are stale until
    // the write is captured.
    void invalidate(const Entry& e) noexcept {
        at(e).valid = false;
        bump<Faults::skipBumpInvalidate>();
    }
    void setMapped(const Entry& e, void* mapped) noexcept {
        at(e).mapped = mapped;
        bump<Faults::skipBumpMapped>();
    }
    // A complete write was seen, whole: its rows, whether they were usable, the frame and the sequence
    // number it was given.
    void capture(const Entry& e, const void* bytes, uint64_t frame, uint32_t sequence) noexcept {
        Entry& m = at(e);
        m.valid = flatCaptureCameraRows(m.rows, bytes, m.width);
        m.frame = frame;
        m.sequence = sequence;
        bump<Faults::skipBumpCapture>();
    }
    // The state the tables were built from is unknown (ClearState, a command list): nothing is current.
    void invalidateAll() noexcept {
        for (uint32_t i = 0; i < kCapacity; ++i) entries_[i].valid = false;
        bump<Faults::skipBumpInvalidateAll>();
    }
    // The frame boundary: the buffers stay known, nothing is current, nothing is mapped.
    void newFrame() noexcept {
        for (uint32_t i = 0; i < count_; ++i) {
            entries_[i].valid = false;
            entries_[i].mapped = nullptr;
        }
        bump<Faults::skipBumpNewFrame>();
    }
    // The device or the context went away: every entry, and its reference, is dropped.
    void clear() noexcept {
        for (uint32_t i = 0; i < kCapacity; ++i) entries_[i] = Entry{};
        count_ = 0;
        bump<Faults::skipBumpClear>();
    }

    // ---- the draw path -------------------------------------------------------------------------------
    // The kept answer for a draw whose b1 is `b1`, bound at generation `bindGeneration`, in frame `frame`:
    // a few compares and no search, or nullptr when it must be made afresh.
    const FlatCameraRows* probe(const void* b1, uint32_t bindGeneration, uint64_t frame) const noexcept {
        if (!kept_.made) return nullptr;
        if constexpr (!Faults::ignoreB1) { if (kept_.b1 != b1) return nullptr; }
        if constexpr (!Faults::ignoreBindGeneration) { if (kept_.bindGeneration != bindGeneration) return nullptr; }
        if constexpr (!Faults::ignoreFrame) { if (kept_.frame != frame) return nullptr; }
        if (kept_.tableGeneration != generation_) return nullptr;
        return &kept_.rows;
    }
    // The answer made afresh, exactly as a draw has always been answered, and kept for the next one.
    const FlatCameraRows& refresh(const void* b1, uint32_t bindGeneration, uint64_t frame) noexcept {
        ++refreshes_;
        FlatCameraRows r;
        if (const Entry* c = find(b1)) {
            if (c->valid && c->frame == frame) {
                std::memcpy(r.rows, c->rows, sizeof(r.rows));
                r.have = true;
                r.hash = flatCameraHash(r.rows);
                r.epoch = c->frame;
                r.sequence = c->sequence;
            }
        }
        kept_.made = true;
        kept_.b1 = b1;
        kept_.bindGeneration = bindGeneration;
        kept_.frame = frame;
        kept_.tableGeneration = generation_;
        kept_.rows = r;
        return kept_.rows;
    }
    // Both, for a caller that does not time the second.
    const FlatCameraRows& rows(const void* b1, uint32_t bindGeneration, uint64_t frame) noexcept {
        if (const FlatCameraRows* kept = probe(b1, bindGeneration, frame)) return *kept;
        return refresh(b1, bindGeneration, frame);
    }
    // How many times the answer was made afresh: what the rig counts hits by.
    uint64_t refreshes() const noexcept { return refreshes_; }

private:
    template <bool Skip>
    void bump() noexcept {
        if constexpr (!Skip) ++generation_;
    }
    // The entry `e` points at, for changing. It came from find(), so it is one of ours.
    Entry& at(const Entry& e) noexcept { return entries_[static_cast<uint32_t>(&e - entries_)]; }

    struct Kept {
        bool made = false;
        const void* b1 = nullptr;
        uint32_t bindGeneration = 0;
        uint64_t frame = 0;
        uint64_t tableGeneration = 0;
        FlatCameraRows rows;
    };
    Entry entries_[kCapacity];
    uint32_t count_ = 0;
    uint64_t generation_ = 1;
    uint64_t refreshes_ = 0;
    Kept kept_;
};

}  // namespace edvr
