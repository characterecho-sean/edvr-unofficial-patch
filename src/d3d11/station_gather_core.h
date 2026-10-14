#pragma once
// The station's lock-free hand-offs (built v14.26, 2026-10-13; the gather rebuilt as a stamped pass ring in v14.27). Pure: no Windows, no engine, so the
// rig can drive them from several threads.
//   PassGather<E, kSlots, kCap>: the store callbacks (any game thread) open a Writer for the length of one callback and append the entries the tick needs
//     to their own thread's slot. The Writer reads the frame stamp once, after it is counted as busy, and every entry of that callback goes to the pass
//     of that stamp (a ring of three buffers per slot, keyed by stamp). The tick advances the stamp, then closes the passes older than the new stamp: a
//     pass is COMPLETE only when no writer that began before the advance is still inside (a busy count per slot). A pass whose writers have not all left
//     is not read: it stays where it is and the next tick tries again. The result says how many callbacks the passes held, so a pass with none is known
//     to be empty, and neither an empty pass nor an unfinished one is evidence about anything.
//   RigPubTable: the rig verdicts and collections the tick publishes, in two sets of cells. The tick stages the next publication in the set the callbacks
//     are not reading and commits it with one store, so a lookup sees either the whole old publication or the whole new one. A lookup returns the
//     verdict, the collection and the lifecycle epoch of the rig's cell. The run of vetoes per rig is an atomic counter moved by compare-and-swap, so a
//     rig's veto sequence is the one the locked version gave, one call at a time; a republication carries the run over.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "optimized_station_core.h"
#include "station_lockfree_core.h"

namespace edvr::sealed {

// The rig verdict codes the publication carries (the station's own values: unknown 0, mixed 1, outside 2).
constexpr uint8_t kRigVerdictUnknown = 0;
constexpr uint8_t kRigVerdictMixed = 1;
constexpr uint8_t kRigVerdictOutside = 2;

// What a drain found. firstStamp..lastStamp are the passes closed (none when passes == 0). complete: every pass up to the stamp before the current one was
// read; false means a writer was still inside and nothing was read this time. calls: the callbacks the closed passes held (Writer::call). ctx: the
// context the busiest thread's callbacks saw in those passes (0: none). lost: entries dropped since the previous drain (a full slot, a thread without a
// slot, or a pass overwritten before it was read).
struct PassDrain {
    bool complete = true;
    uint32_t passes = 0;
    uint32_t firstStamp = 0;
    uint32_t lastStamp = 0;
    uint64_t calls = 0;
    uint64_t entries = 0;
    uint64_t lost = 0;
    uintptr_t ctx = 0;
};

// E must have a uint32_t member `stamp`: the Writer sets it to the pass's stamp, so an entry always names the pass it sits in.
template <typename E, size_t kSlots, size_t kCap>
class PassGather {
    struct Slot;

public:
    static constexpr uint32_t kPasses = 3;

    // `stamp` is the frame stamp: the tick advances it (fetch_add) before it drains. The gather only reads it.
    explicit PassGather(std::atomic<uint32_t>& stamp) noexcept : stamp_(stamp), id_(nextId().fetch_add(1, std::memory_order_relaxed)) {
        next_ = stamp.load(std::memory_order_relaxed);
    }
    PassGather(const PassGather&) = delete;
    PassGather& operator=(const PassGather&) = delete;

    // One callback's extent. Counted as busy from construction to destruction; the stamp is read after that, so a pass is never closed under a writer
    // that read its stamp. A thread without a slot gets a Writer that drops everything it is given (counted as lost).
    class Writer {
    public:
        explicit Writer(PassGather& g) noexcept : g_(g), s_(g.slotOfThisThread()) {
            if (s_ != nullptr) {
                s_->busy.fetch_add(1, std::memory_order_seq_cst);
                stamp_ = g.stamp_.load(std::memory_order_seq_cst);
                idx_ = stamp_ % kPasses;
                if (s_->passStamp[idx_] != stamp_) {
                    // The buffer still holds a pass that was never read (the tick fell three passes behind): it is lost, not misread.
                    const uint32_t stale = s_->count[idx_].load(std::memory_order_relaxed);
                    if (stale != 0) g.lost_.fetch_add(stale, std::memory_order_relaxed);
                    s_->count[idx_].store(0, std::memory_order_relaxed);
                    s_->calls[idx_].store(0, std::memory_order_relaxed);
                    s_->ctx[idx_].store(0, std::memory_order_relaxed);
                    s_->passStamp[idx_] = stamp_;
                }
            }
        }
        ~Writer() {
            if (s_ != nullptr) s_->busy.fetch_sub(1, std::memory_order_release);
        }
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        // The frame stamp of this callback's pass (0 for a thread without a slot).
        uint32_t stamp() const noexcept { return s_ != nullptr ? stamp_ : 0; }

        // Counts this callback as a record the pass took, and remembers the context it saw (0 when it has none yet).
        void call(uintptr_t ctx) noexcept {
            if (s_ == nullptr) return;
            s_->calls[idx_].store(s_->calls[idx_].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            if (ctx != 0) s_->ctx[idx_].store(ctx, std::memory_order_relaxed);
        }

        // Names the context this callback saw (no count).
        void context(uintptr_t ctx) noexcept {
            if (s_ != nullptr && ctx != 0) s_->ctx[idx_].store(ctx, std::memory_order_relaxed);
        }

        // Appends an entry to this callback's pass. False when the slot is full or the thread has no slot: the entry is lost and counted.
        bool push(E e) noexcept {
            if (s_ == nullptr) {
                g_.lost_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            const uint32_t n = s_->count[idx_].load(std::memory_order_relaxed);
            if (n >= kCap) {
                g_.lost_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            e.stamp = stamp_;
            s_->items[idx_][n] = e;
            s_->count[idx_].store(n + 1, std::memory_order_relaxed);
            return true;
        }

        // Entries lost since the last drain (the callback keeps every record when any were lost).
        uint64_t lostSinceDrain() const noexcept { return g_.lost_.load(std::memory_order_relaxed); }

    private:
        PassGather& g_;
        Slot* s_;
        uint32_t stamp_ = 0;
        uint32_t idx_ = 0;
    };

    // The tick only (one thread), after it advanced the stamp. Closes the passes older than the current stamp, oldest first, and visits their entries in
    // pass order and slot order. Waits for the writers still inside (a bounded wait); when one stays, nothing is read and complete is false.
    template <typename F>
    PassDrain drain(F&& visit, uint32_t spins = 512) {
        PassDrain d;
        const uint32_t now = stamp_.load(std::memory_order_seq_cst);
        d.lost = lost_.exchange(0, std::memory_order_relaxed);
        if (static_cast<uint32_t>(now - next_) > kPasses - 1) {
            // Passes older than the ring holds were overwritten (their entries were counted lost by the writers that overwrote them).
            next_ = now - (kPasses - 1);
        }
        if (next_ == now) return d;   // no pass closed since the last drain
        for (size_t i = 0; i < kSlots; ++i) {
            uint32_t n = 0;
            while (slots_[i].busy.load(std::memory_order_seq_cst) != 0) {
                if (++n > spins) {
                    d.complete = false;
                    return d;
                }
                std::this_thread::yield();
            }
        }
        uint64_t best = 0;
        for (uint32_t s = next_; s != now; ++s) {
            const uint32_t idx = s % kPasses;
            for (size_t i = 0; i < kSlots; ++i) {
                Slot& sl = slots_[i];
                if (sl.passStamp[idx] != s) continue;
                const uint32_t n = sl.count[idx].load(std::memory_order_relaxed);
                const uint64_t c = sl.calls[idx].load(std::memory_order_relaxed);
                for (uint32_t k = 0; k < n; ++k) visit(sl.items[idx][k]);
                d.entries += n;
                d.calls += c;
                if (c > best) {
                    best = c;
                    d.ctx = sl.ctx[idx].load(std::memory_order_relaxed);
                }
                sl.count[idx].store(0, std::memory_order_relaxed);
                sl.calls[idx].store(0, std::memory_order_relaxed);
                sl.ctx[idx].store(0, std::memory_order_relaxed);
            }
            if (d.passes == 0) d.firstStamp = s;
            d.lastStamp = s;
            ++d.passes;
        }
        next_ = now;
        return d;
    }

    // Any thread: the entries lost since the last drain.
    uint64_t lostSinceDrain() const noexcept { return lost_.load(std::memory_order_relaxed); }

private:
    struct alignas(64) Slot {
        std::atomic<uint32_t> busy{0};
        uint32_t passStamp[kPasses] = {};
        std::atomic<uint32_t> count[kPasses] = {};
        std::atomic<uint32_t> calls[kPasses] = {};
        std::atomic<uintptr_t> ctx[kPasses] = {};
        E items[kPasses][kCap] = {};
    };

    static std::atomic<uint64_t>& nextId() noexcept {
        static std::atomic<uint64_t> next{1};
        return next;
    }

    // A thread claims a slot the first time it opens a Writer on this gather. A thread that alternates between gathers claims again on each switch.
    Slot* slotOfThisThread() noexcept {
        thread_local uint64_t tlsOwner = 0;
        thread_local uint32_t tlsSlot = 0;
        if (tlsOwner != id_) {
            tlsOwner = id_;
            tlsSlot = claimed_.fetch_add(1, std::memory_order_relaxed);
        }
        return tlsSlot < kSlots ? &slots_[tlsSlot] : nullptr;
    }

    std::atomic<uint32_t>& stamp_;
    const uint64_t id_;
    uint32_t next_ = 0;   // the oldest pass not read yet (the tick's)
    std::atomic<uint32_t> claimed_{0};
    std::atomic<uint64_t> lost_{0};
    Slot slots_[kSlots];
};

// The veto's outcome for one call against a rig's verdict.
enum class RigOutcome : uint8_t { kUnknown, kMixed, kVetoed, kRefresh };

// An outside rig is vetoed kRigRefreshN calls in a row, then let through once (the refresh) and the run starts again. A mixed or unknown rig resets the
// run. The run moves by compare-and-swap, so concurrent calls count exactly as serial calls would.
inline RigOutcome rigOutcomeOf(uint8_t verdict, std::atomic<uint32_t>& run) noexcept {
    if (verdict == kRigVerdictOutside) {
        uint32_t cur = run.load(std::memory_order_relaxed);
        for (;;) {
            if (cur >= kRigRefreshN) {
                if (run.compare_exchange_weak(cur, 0, std::memory_order_relaxed)) return RigOutcome::kRefresh;
            } else if (run.compare_exchange_weak(cur, cur + 1, std::memory_order_relaxed)) {
                return RigOutcome::kVetoed;
            }
        }
    }
    run.store(0, std::memory_order_relaxed);
    return verdict == kRigVerdictMixed ? RigOutcome::kMixed : RigOutcome::kUnknown;
}

// The rig verdicts of one publication. The tick (one thread) stages a publication and inserts each rig it publishes, then commits it. A callback looks a
// rig up in the committed publication. A rig the publication does not hold forwards, as a rig the tick never bounded did. Each cell's verdict,
// collection and epoch are one seqlock payload (four words), so a lookup never pairs a verdict with another publication's collection. The cell index a
// lookup returns names the set too (set * kCells + cell), and the run, the stamps and the asks live at that index.
class RigPubTable {
public:
    static constexpr size_t kCells = 4096;

    // The tick only: starts staging the next publication. The callbacks keep reading the committed one.
    void begin() noexcept { staging_ = gen_.load(std::memory_order_relaxed) + 1; }

    // The tick only: publishes one rig's verdict, collection and epoch in the staged publication. False when the bounded probe finds no cell (the rig then
    // forwards). The cell's run of vetoes starts at carriedRun (the run the rig had in the committed publication).
    bool insert(uintptr_t rig, size_t home, uint8_t verdict, uintptr_t coll, size_t* indexOut = nullptr, uint32_t epoch = 0,
                uint32_t carriedRun = 0) noexcept {
        const uint32_t w[4] = {verdict, static_cast<uint32_t>(static_cast<uint64_t>(coll) & 0xFFFFFFFFu),
                               static_cast<uint32_t>(static_cast<uint64_t>(coll) >> 32), epoch};
        const size_t set = staging_ & 1u;
        size_t idx = 0;
        if (!seqTableInsertWords(cells_[set], kCells, home, rig, staging_, w, nullptr, &idx)) return false;
        const size_t g = set * kCells + idx;
        run_[g].store(carriedRun, std::memory_order_relaxed);
        evalStamp_[g].store(0, std::memory_order_relaxed);
        askStamp_[g].store(0, std::memory_order_relaxed);
        if (indexOut != nullptr) *indexOut = g;
        return true;
    }

    // The tick only: the staged publication becomes the committed one, in one store.
    void commit() noexcept { gen_.store(staging_, std::memory_order_seq_cst); }

    // Any thread: the rig's verdict, collection and epoch in the committed publication, and its cell index. False: not published.
    bool lookup(uintptr_t rig, size_t home, uint8_t* verdict, uintptr_t* coll, size_t* index, uint32_t* epoch = nullptr) const noexcept {
        uint32_t w[4] = {};
        size_t idx = 0;
        const uint32_t gen = gen_.load(std::memory_order_seq_cst);
        const size_t set = gen & 1u;
        if (!seqTableFindWords(cells_[set], kCells, home, rig, gen, w, nullptr, &idx)) return false;
        *verdict = static_cast<uint8_t>(w[0]);
        *coll = static_cast<uintptr_t>(static_cast<uint64_t>(w[1]) | (static_cast<uint64_t>(w[2]) << 32));
        *index = set * kCells + idx;
        if (epoch != nullptr) *epoch = w[3];
        return true;
    }

    // Any thread: the veto decision for a found rig; records the stamp of its last call (the tally's "matched").
    RigOutcome decide(size_t idx, uint8_t verdict, uint32_t stamp) noexcept {
        if (evalStamp_[idx].load(std::memory_order_relaxed) != stamp) evalStamp_[idx].store(stamp, std::memory_order_relaxed);   // v14.29: no write on a repeat
        return rigOutcomeOf(verdict, run_[idx]);
    }

    // Any thread: records the stamp of a call that did not decide (the association moved).
    void touch(size_t idx, uint32_t stamp) noexcept {
        if (evalStamp_[idx].load(std::memory_order_relaxed) != stamp) evalStamp_[idx].store(stamp, std::memory_order_relaxed);
    }

    // Any thread: true for the first caller of this stamp on this cell (one association request per rig per frame).
    bool askOnce(size_t idx, uint32_t stamp) noexcept { return askStamp_[idx].exchange(stamp, std::memory_order_relaxed) != stamp; }

    // The stamp of the rig's last call in the publication.
    uint32_t evalStampAt(size_t idx) const noexcept { return evalStamp_[idx].load(std::memory_order_relaxed); }

    // The run of vetoes the rig has now (the tick carries it into the next publication).
    uint32_t runAt(size_t idx) const noexcept { return run_[idx].load(std::memory_order_relaxed); }

    // The tick only: the rig's run of vetoes starts afresh (its bounds were forgotten).
    void resetRun(size_t idx) noexcept { run_[idx].store(0, std::memory_order_relaxed); }

    // The committed generation (a counter of publications).
    uint32_t generation() const noexcept { return gen_.load(std::memory_order_relaxed); }

private:
    SeqCell cells_[2][kCells];
    std::atomic<uint32_t> gen_{0};
    uint32_t staging_ = 1;
    std::atomic<uint32_t> run_[2 * kCells] = {};
    std::atomic<uint32_t> evalStamp_[2 * kCells] = {};
    std::atomic<uint32_t> askStamp_[2 * kCells] = {};
};

}  // namespace edvr::sealed
