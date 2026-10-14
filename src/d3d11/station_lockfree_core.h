#pragma once
// The lock-free pieces of the station's per-record path (built v14.25, 2026-10-13). Pure: no Windows, no engine, so the rig can drive them from
// several threads. Two primitives:
//   SnapshotRing<T, N>: the tick (the single publisher, under the station's state lock) publishes an immutable value; callers on any thread pin the
//     published slot, read it, and unpin. A slot is rewritten only when no caller holds it (a per-slot reference count), so nothing is freed or torn
//     under a reader, and no reader takes a lock.
//   SeqCell: one cached record (its record address, its generation and four floats) under a seqlock. Writers claim the cell with a compare-and-swap
//     on the sequence word and retry if another writer holds it; readers copy it and check the sequence, so a torn copy is never returned.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "optimized_station_core.h"

namespace edvr::sealed {

// Built v14.29: a caller can also pin with a per-thread hazard cell (pinThread): the cell is the thread's own cache line, so a pin writes no line another
// thread touches. The thread keeps its slot pinned until its next pinThread, so a batch of calls on one published slot costs two loads of the published
// index. The publisher skips a slot that a hazard cell names. A thread past the last cell pins by the shared count (pin / unpin), as before. A ring that
// hazard cells hold needs N above the number of threads that can each hold a different old slot (the station uses 40 for 16 gather slots).
template <typename T, size_t N, size_t kHazards = 48>
class SnapshotRing {
public:
    SnapshotRing() noexcept : id_(nextId().fetch_add(1, std::memory_order_relaxed)) {}
    // The publisher's slot, free for writing: the first slot after the published one that no caller holds (by count or by hazard cell). Single publisher only.
    T& beginWrite() noexcept {
        for (;;) {
            const size_t c = cur_.load(std::memory_order_seq_cst);
            for (size_t k = 1; k < N; ++k) {
                const size_t cand = (c + k) % N;
                if (refs_[cand].load(std::memory_order_seq_cst) == 0 && !hazarded(cand)) {
                    next_ = cand;
                    return slots_[cand];
                }
            }
        }
    }
    // Makes the slot from beginWrite the published one.
    void publish() noexcept {
        cur_.store(next_, std::memory_order_seq_cst);
        publishes_.fetch_add(1, std::memory_order_relaxed);
    }
    // A caller's view of the published value: pinned until unpin(idx). Never blocks.
    const T* pin(size_t* idx) noexcept {
        for (;;) {
            const size_t i = cur_.load(std::memory_order_seq_cst);
            refs_[i].fetch_add(1, std::memory_order_seq_cst);
            if (cur_.load(std::memory_order_seq_cst) == i) {
                *idx = i;
                return &slots_[i];
            }
            refs_[i].fetch_sub(1, std::memory_order_seq_cst);
        }
    }
    void unpin(size_t idx) noexcept { refs_[idx].fetch_sub(1, std::memory_order_seq_cst); }

    // The per-thread pin. Returns the published slot, valid until this thread's next pinThread. *counted is 0 when the thread's hazard cell holds the
    // slot (nothing to release), or idx + 1 when the thread had no cell and the shared count holds it (release with unpin(*counted - 1)).
    const T* pinThread(size_t* counted) noexcept {
        Hazard* h = hazardOfThisThread();
        if (h == nullptr) {
            size_t idx = 0;
            const T* p = pin(&idx);
            *counted = idx + 1;
            return p;
        }
        *counted = 0;
        for (;;) {
            const size_t i = cur_.load(std::memory_order_seq_cst);
            if (h->idx.load(std::memory_order_relaxed) != i) h->idx.store(static_cast<uint32_t>(i), std::memory_order_seq_cst);
            if (cur_.load(std::memory_order_seq_cst) == i) return &slots_[i];
        }
    }
    uint64_t publishes() const noexcept { return publishes_.load(std::memory_order_relaxed); }

private:
    static constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
    struct alignas(64) Hazard {
        std::atomic<uint32_t> idx{kNoSlot};
    };
    static std::atomic<uint64_t>& nextId() noexcept {
        static std::atomic<uint64_t> next{1};
        return next;
    }
    bool hazarded(size_t slot) const noexcept {
        const uint32_t used = claimed_.load(std::memory_order_seq_cst);
        const uint32_t n = used < kHazards ? used : static_cast<uint32_t>(kHazards);
        for (uint32_t i = 0; i < n; ++i) {
            if (haz_[i].idx.load(std::memory_order_seq_cst) == slot) return true;
        }
        return false;
    }
    Hazard* hazardOfThisThread() noexcept {
        thread_local uint64_t tlsOwner = 0;
        thread_local uint32_t tlsCell = 0;
        if (tlsOwner != id_) {
            tlsOwner = id_;
            tlsCell = claimed_.fetch_add(1, std::memory_order_seq_cst);
        }
        return tlsCell < kHazards ? &haz_[tlsCell] : nullptr;
    }

    const uint64_t id_;
    T slots_[N] = {};
    std::atomic<size_t> cur_{0};
    size_t next_ = 1;
    std::atomic<uint32_t> refs_[N] = {};
    std::atomic<uint32_t> claimed_{0};
    Hazard haz_[kHazards];
    std::atomic<uint64_t> publishes_{0};
};

// Built v14.31: the node class cache. One 64-bit word per slot (the node, its class, its flags and the lifecycle generation: optimized_station_core.h
// packSlot) and the node's name beside it. A lookup probes at most kTableProbeMax slots from the node's hash and stops at an empty one; an insert takes the
// first empty slot or slot of an older generation within the same bound (a full window is a miss the caller reads uncached). A writer claims a slot with a
// compare-and-swap to a busy word before it writes the name and publishes the word last, and a reader compares the word before and after it copies the name,
// so a name is never returned torn or under another node's word. A node's name is read once per node per generation.
// Built v14.32: each entry also holds a validator, a 64-bit hash of the node's name text pointer and the first 16 bytes of the text, taken by the caller with
// one guarded read on every lookup. A node freed and reallocated within a generation (a streamed asset) shows another validator: the hit is refused as stale and
// the caller classifies the node afresh, replacing the entry in place.
inline uint64_t nodeValidator(uint64_t textPtr, const uint8_t first16[16]) noexcept {
    uint64_t a = 0, b = 0;
    std::memcpy(&a, first16, 8);
    std::memcpy(&b, first16 + 8, 8);
    uint64_t h = (textPtr * 0x9E3779B97F4A7C15ull) ^ (a * 0xC2B2AE3D27D4EB4Full) ^ (b * 0xFF51AFD7ED558CCDull);
    h ^= h >> 31;
    return h | 1u;   // never 0: 0 stands for an unreadable node
}

template <size_t N>
class NodeCache {
public:
    static constexpr size_t kNameBytes = 64;
    static_assert((N & (N - 1)) == 0, "the cache size is a power of two");

    // True on a hit: the class and flags (and the name, when `name` is given: kNameBytes, always terminated) of this node under this generation, when the
    // entry's validator equals `validator`. An entry of this node with another validator is refused (*stale set): the node was reused.
    bool find(uintptr_t node, uint32_t gen, NameClass* cls, uint32_t* flags, char* name = nullptr, uint64_t validator = 0, bool* stale = nullptr) const noexcept {
        const size_t home = homeOf(node);
        const uint32_t key = classKey(gen);
        for (uint32_t p = 0; p < kTableProbeMax; ++p) {
            const size_t i = (home + p) & (N - 1);
            const uint64_t w1 = slots_[i].load(std::memory_order_acquire);
            if (w1 == 0) return false;
            if (w1 == kBusy) continue;
            NameClass c = NameClass::kNone;
            uint32_t f = 0;
            if (!unpackSlot(w1, node, key, &c, &f)) continue;
            if (name != nullptr) std::memcpy(name, names_[i], kNameBytes);
            const uint64_t sv = validators_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (slots_[i].load(std::memory_order_relaxed) != w1) return false;
            if (sv != validator) {
                if (stale != nullptr) *stale = true;
                return false;
            }
            if (name != nullptr) name[kNameBytes - 1] = '\0';
            *cls = c;
            *flags = f;
            return true;
        }
        return false;
    }

    // Inserts a node, or replaces its entry in place when it has one (a reused node). False when the probe window holds nothing reusable (the node is then
    // read uncached).
    bool put(uintptr_t node, uint32_t gen, NameClass cls, uint32_t flags, const char* name, uint64_t validator = 0) noexcept {
        const size_t home = homeOf(node);
        const uint32_t key = classKey(gen);
        for (int pass = 0; pass < 2; ++pass) {   // pass 0 looks for this node's own entry, pass 1 for a reusable slot
            for (uint32_t p = 0; p < kTableProbeMax; ++p) {
                const size_t i = (home + p) & (N - 1);
                uint64_t w = slots_[i].load(std::memory_order_acquire);
                if (w == kBusy) continue;
                if (pass == 0) {
                    if (w == 0) break;   // the chain ends: the node has no entry
                    NameClass c0 = NameClass::kNone;
                    uint32_t f0 = 0;
                    if (!unpackSlot(w, node, key, &c0, &f0)) continue;
                } else if (w != 0 && ((w >> 50) & kSlotKeyMask) == key) {
                    continue;   // a live entry of this generation (another node's)
                }
                if (!slots_[i].compare_exchange_strong(w, kBusy, std::memory_order_acq_rel, std::memory_order_acquire)) continue;
                char tmp[kNameBytes] = {};
                if (name != nullptr) std::strncpy(tmp, name, kNameBytes - 1);
                std::memcpy(names_[i], tmp, kNameBytes);
                validators_[i].store(validator, std::memory_order_relaxed);
                slots_[i].store(packSlot(node, cls, flags, key), std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    size_t used(uint32_t gen) const noexcept {
        size_t n = 0;
        const uint32_t key = classKey(gen);
        for (size_t i = 0; i < N; ++i) {
            const uint64_t w = slots_[i].load(std::memory_order_relaxed);
            if (w != 0 && w != kBusy && ((w >> 50) & kSlotKeyMask) == key) ++n;
        }
        return n;
    }

private:
    static constexpr uint64_t kBusy = ~uint64_t(0);
    static size_t homeOf(uintptr_t node) noexcept {
        return static_cast<size_t>(((uint64_t(node) >> 3) * 0x9E3779B97F4A7C15ull) >> 32) & (N - 1);
    }
    std::atomic<uint64_t> slots_[N] = {};
    std::atomic<uint64_t> validators_[N] = {};
    char names_[N][kNameBytes] = {};
};

// One cached record: the record's address, the frame generation it belongs to, and four floats (centre x, y, z and radius).
struct SeqCell {
    std::atomic<uint32_t> seq{0};   // even: stable; odd: a writer holds the cell
    std::atomic<uintptr_t> rec{0};
    std::atomic<uint32_t> gen{0};
    std::atomic<uint32_t> word[4] = {};
};

inline uint32_t floatBits(float f) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
inline float bitsFloat(uint32_t u) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

// Claims the cell and writes it. False when another writer holds the cell right now (the caller retries the insert).
inline bool seqCellWrite(SeqCell& c, uintptr_t rec, uint32_t gen, const float v[4]) noexcept {
    uint32_t s = c.seq.load(std::memory_order_acquire);
    if ((s & 1u) != 0) return false;
    if (!c.seq.compare_exchange_strong(s, s + 1, std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    c.rec.store(rec, std::memory_order_relaxed);
    c.gen.store(gen, std::memory_order_relaxed);
    for (size_t k = 0; k < 4; ++k) c.word[k].store(floatBits(v[k]), std::memory_order_relaxed);
    c.seq.store(s + 2, std::memory_order_release);
    return true;
}

// Copies the cell. False when a writer holds it or wrote it during the copy (the caller treats that as no entry).
inline bool seqCellRead(const SeqCell& c, uintptr_t* rec, uint32_t* gen, float v[4]) noexcept {
    const uint32_t s1 = c.seq.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0) return false;
    *rec = c.rec.load(std::memory_order_relaxed);
    *gen = c.gen.load(std::memory_order_relaxed);
    for (size_t k = 0; k < 4; ++k) v[k] = bitsFloat(c.word[k].load(std::memory_order_relaxed));
    std::atomic_thread_fence(std::memory_order_acquire);
    return c.seq.load(std::memory_order_relaxed) == s1;
}

// Built v14.26: the same cell on exact uint32 words (a verdict and a collection pointer are not floats). The float forms below are wrappers.
inline bool seqCellWriteWords(SeqCell& c, uintptr_t rec, uint32_t gen, const uint32_t w[4]) noexcept {
    uint32_t s = c.seq.load(std::memory_order_acquire);
    if ((s & 1u) != 0) return false;
    if (!c.seq.compare_exchange_strong(s, s + 1, std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    c.rec.store(rec, std::memory_order_relaxed);
    c.gen.store(gen, std::memory_order_relaxed);
    for (size_t k = 0; k < 4; ++k) c.word[k].store(w[k], std::memory_order_relaxed);
    c.seq.store(s + 2, std::memory_order_release);
    return true;
}

inline bool seqCellReadWords(const SeqCell& c, uintptr_t* rec, uint32_t* gen, uint32_t w[4]) noexcept {
    const uint32_t s1 = c.seq.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0) return false;
    *rec = c.rec.load(std::memory_order_relaxed);
    *gen = c.gen.load(std::memory_order_relaxed);
    for (size_t k = 0; k < 4; ++k) w[k] = c.word[k].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    return c.seq.load(std::memory_order_relaxed) == s1;
}

// The table: `cap` cells, bounded probing (kTableProbeMax), a generation per frame. A cell of an earlier generation is free. Returns false when the
// bounded probe finds no cell (the caller counts it and runs uncached). indexOut, when given, is the cell's index on success.
inline bool seqTableInsertWords(SeqCell* cells, size_t cap, size_t home, uintptr_t rec, uint32_t gen, const uint32_t w[4], uint32_t* probes,
                                size_t* indexOut = nullptr) noexcept {
    for (int attempt = 0; attempt < 64; ++attempt) {
        const ProbeResult p = boundedProbe(
            home, cap, kTableProbeMax,
            [&](size_t i) {
                uintptr_t r = 0;
                uint32_t g = 0;
                uint32_t tmp[4];
                return seqCellReadWords(cells[i], &r, &g, tmp) && g == gen && r == rec;
            },
            [&](size_t i) {
                uintptr_t r = 0;
                uint32_t g = 0;
                uint32_t tmp[4];
                return seqCellReadWords(cells[i], &r, &g, tmp) && g != gen;
            });
        if (probes != nullptr) *probes = p.probes;
        if (p.exhausted) return false;
        if (p.found || p.empty) {
            if (seqCellWriteWords(cells[p.index], rec, gen, w)) {
                if (indexOut != nullptr) *indexOut = p.index;
                return true;
            }
            // another writer holds that cell: try again (the cell may now hold another record, so the probe runs again)
        }
    }
    return false;
}

// The cell for `rec` in this generation, if cached. False when it is not (or a writer is in the way).
inline bool seqTableFindWords(const SeqCell* cells, size_t cap, size_t home, uintptr_t rec, uint32_t gen, uint32_t w[4], uint32_t* probes,
                              size_t* indexOut = nullptr) noexcept {
    const ProbeResult p = boundedProbe(
        home, cap, kTableProbeMax,
        [&](size_t i) {
            uintptr_t r = 0;
            uint32_t g = 0;
            uint32_t tmp[4];
            return seqCellReadWords(cells[i], &r, &g, tmp) && g == gen && r == rec;
        },
        [&](size_t i) {
            uintptr_t r = 0;
            uint32_t g = 0;
            uint32_t tmp[4];
            return seqCellReadWords(cells[i], &r, &g, tmp) && g != gen;
        });
    if (probes != nullptr) *probes = p.probes;
    if (!p.found) return false;
    uintptr_t r = 0;
    uint32_t g = 0;
    if (!seqCellReadWords(cells[p.index], &r, &g, w) || r != rec || g != gen) return false;
    if (indexOut != nullptr) *indexOut = p.index;
    return true;
}

// The float payload forms (the producer cells): the same table on the words of the floats.
inline bool seqTableInsert(SeqCell* cells, size_t cap, size_t home, uintptr_t rec, uint32_t gen, const float v[4], uint32_t* probes) noexcept {
    const uint32_t w[4] = {floatBits(v[0]), floatBits(v[1]), floatBits(v[2]), floatBits(v[3])};
    return seqTableInsertWords(cells, cap, home, rec, gen, w, probes, nullptr);
}

inline bool seqTableFind(const SeqCell* cells, size_t cap, size_t home, uintptr_t rec, uint32_t gen, float v[4], uint32_t* probes) noexcept {
    uint32_t w[4] = {};
    if (!seqTableFindWords(cells, cap, home, rec, gen, w, probes, nullptr)) return false;
    for (size_t k = 0; k < 4; ++k) v[k] = bitsFloat(w[k]);
    return true;
}

}  // namespace edvr::sealed
