// The Explorer Cam probe's pure half (advanced.explorer_cam_probe; docs\design-explorer-cam-free-camera-2026-10-07.md,
// "Phase 0b: instruments built"). Everything here is decisions and text, with no Windows, no game and no Config, so
// tools\explorer_cam_probe_test drives the same code the DLL compiles: the snapshot decode, the seqlock the hook
// publishes through, the state-change detection, the 1 Hz / 5 s cadence, the 5376-byte skinned-object fingerprint, the
// yaw and pitch decode, and the exact text of every log line.
//
// THE KEY IS TEMPORARY. It exists for flight F0 of the Explorer Cam redesign and is removed when the arc closes.
//
// THREE INSTRUMENTS, ALL LOG ONLY (nothing here writes a byte of the game's memory, a render state or a camera):
//   I3  an observation hook on the game's FreeCameraActivity update (EliteDangerous64.exe+0x1071980, build 332841):
//       the original runs FIRST, then the activity's pose and state bytes are copied and published through a seqlock.
//   I1  the VR camera census's refresh observer, tallied per (camera kind, call site): calls per frame, origin, axes.
//   I2  the 5376-byte scene blocks the game writes each frame, fingerprinted for a skinned object; the nearest one is
//       the candidate commander root.
// The consumer (Consumer, below) runs once a frame on the render thread and prints a 5 s heartbeat per instrument, a
// line at once on any change of the activity's state bytes, and detail lines at 1 Hz while the free camera's state byte
// (+0x48C) is non-zero and for 2 s after it returns to zero.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace edvr {
namespace ecp {

// ---- identity: build 332841 ---------------------------------------------------------------------------------------
constexpr uintptr_t kTargetRva = 0x1071980;
constexpr uint32_t kExpectedTimestamp = 1788384820u;   // the PE TimeDateStamp and SizeOfImage the other build-keyed hooks use
constexpr uint32_t kExpectedImageSize = 104894464u;
constexpr size_t kPrologueBytes = 28;
// `mov [rsp+20h],rbx; push rbp; push rdi; push r13; push r14; push r15; lea rbp,[rsp-2C0h]; sub rsp,3C0h`. No
// rip-relative byte in it; the instruction boundaries are 5, 6, 7, 9, 11, 13, 21, 28, so CodeHook steals 5.
inline constexpr uint8_t kPrologue[kPrologueBytes] = {
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
    0x8D, 0xAC, 0x24, 0x40, 0xFD, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xC0, 0x03, 0x00, 0x00};

// ---- the activity's fields (Phase 0a, "The object") ----------------------------------------------------------------
constexpr uint32_t kOffWorldPose = 0x70;      // 16 floats, row-major 4x4: axes rows 0-2, origin row 3 (+0xA0/+0xA4/+0xA8)
constexpr uint32_t kOffTarget = 0x2C8;        // a qword: the target's transform. RAW VALUE ONLY, never dereferenced.
constexpr uint32_t kOffLocalPose = 0x3B0;     // 16 floats, commander-local; origin row 3 (+0x3E0/+0x3E4/+0x3E8)
constexpr uint32_t kOffRelative = 0x470;      // 1 relative, 0 world
constexpr uint32_t kOffRotationLock = 0x471;  // 1 = live frame
constexpr uint32_t kOffPresetPending = 0x473;
constexpr uint32_t kOffState = 0x48C;         // a byte: 0 off, 3 free, 4 relative lock, 5 world lock, 6 variant

// What the hook copies out of the activity, before any decoding.
struct Raw {
    float world[16];
    float local[16];
    uint64_t target;
    uint8_t relative, rotationLock, presetPending, state;
};

// The four bytes whose change is reported, packed: byte 0 +0x470, byte 1 +0x471, byte 2 +0x473, byte 3 +0x48C.
constexpr uint32_t packFlags(uint8_t relative, uint8_t rotationLock, uint8_t presetPending, uint8_t state) {
    return static_cast<uint32_t>(relative) | (static_cast<uint32_t>(rotationLock) << 8) |
           (static_cast<uint32_t>(presetPending) << 16) | (static_cast<uint32_t>(state) << 24);
}
constexpr uint32_t flagRelative(uint32_t f) { return f & 0xFFu; }
constexpr uint32_t flagRotationLock(uint32_t f) { return (f >> 8) & 0xFFu; }
constexpr uint32_t flagPreset(uint32_t f) { return (f >> 16) & 0xFFu; }
constexpr uint32_t flagState(uint32_t f) { return (f >> 24) & 0xFFu; }
// Bits of changedMask().
constexpr uint32_t kChangedState = 1, kChangedRelative = 2, kChangedRotationLock = 4, kChangedPreset = 8;
constexpr uint32_t changedMask(uint32_t before, uint32_t after) {
    return (flagState(before) != flagState(after) ? kChangedState : 0u) |
           (flagRelative(before) != flagRelative(after) ? kChangedRelative : 0u) |
           (flagRotationLock(before) != flagRotationLock(after) ? kChangedRotationLock : 0u) |
           (flagPreset(before) != flagPreset(after) ? kChangedPreset : 0u);
}

// One published snapshot. Trivially copyable and a whole number of words (the seqlock stores it as words).
struct Sample {
    uint64_t activity;
    uint64_t target;
    uint64_t slotCalls;    // this activity's calls when the sample was taken
    uint64_t totalCalls;   // every activity's calls
    uint32_t threadId;
    uint32_t flags;        // packFlags
    float world[16];
    float local[16];
};
static_assert(std::is_trivially_copyable<Sample>::value, "a sample is published as raw words");
static_assert(sizeof(Sample) % 4 == 0, "a sample is published as 32-bit words");

inline Sample decodeSample(const Raw& raw, uint64_t activity, uint32_t threadId, uint64_t slotCalls, uint64_t totalCalls) {
    Sample s;
    std::memset(&s, 0, sizeof(s));
    s.activity = activity;
    s.target = raw.target;
    s.slotCalls = slotCalls;
    s.totalCalls = totalCalls;
    s.threadId = threadId;
    s.flags = packFlags(raw.relative, raw.rotationLock, raw.presetPending, raw.state);
    std::memcpy(s.world, raw.world, sizeof(s.world));
    std::memcpy(s.local, raw.local, sizeof(s.local));
    return s;
}

// ---- the seqlock the hook publishes through ---------------------------------------------------------------------------
// One writer at a time (lock() is a try: a second writer skips its sample and counts it, it never waits), any number
// of readers. The words are relaxed atomics, so a reader that overlaps a write is a well-defined torn copy that the
// sequence check then rejects, not a data race.
template <typename T>
class SeqSlot {
    static_assert(std::is_trivially_copyable<T>::value, "SeqSlot publishes raw words");
    static_assert(sizeof(T) % 4 == 0, "SeqSlot publishes 32-bit words");
    static constexpr size_t kWords = sizeof(T) / 4;
public:
    bool lock() noexcept { return m_busy.exchange(1, std::memory_order_acquire) == 0; }
    void unlock() noexcept { m_busy.store(0, std::memory_order_release); }
    // The caller holds lock().
    void store(const T& value) noexcept {
        uint32_t w[kWords];
        std::memcpy(w, &value, sizeof(T));
        const uint32_t s = m_seq.load(std::memory_order_relaxed);
        m_seq.store(s + 1, std::memory_order_relaxed);           // odd: a write is in flight
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < kWords; ++i) m_words[i].store(w[i], std::memory_order_relaxed);
        m_seq.store(s + 2, std::memory_order_release);           // even: stable again
    }
    bool tryPublish(const T& value) noexcept {
        if (!lock()) return false;
        store(value);
        unlock();
        return true;
    }
    // False before the first publish, or when no consistent copy was had in `tries` attempts.
    bool read(T& out, int tries = 8) const noexcept {
        for (int t = 0; t < tries; ++t) {
            const uint32_t s1 = m_seq.load(std::memory_order_acquire);
            if (s1 == 0) return false;
            if (s1 & 1u) continue;
            uint32_t w[kWords];
            for (size_t i = 0; i < kWords; ++i) w[i] = m_words[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (m_seq.load(std::memory_order_relaxed) == s1) {
                std::memcpy(&out, w, sizeof(T));
                return true;
            }
        }
        return false;
    }
    uint32_t publishes() const noexcept { return m_seq.load(std::memory_order_acquire) / 2u; }
private:
    std::atomic<uint32_t> m_seq{0};
    std::atomic<uint32_t> m_busy{0};
    std::atomic<uint32_t> m_words[kWords]{};
};

// ---- small lock-free sets, for the hook ------------------------------------------------------------------------------
// Find-or-insert a non-zero key; -1 when the table is full (never evicts). Keys are never removed.
template <size_t N>
class KeyTable {
public:
    int findOrInsert(uint64_t key) noexcept {
        if (!key) return -1;
        for (size_t i = 0; i < N; ++i) {
            const uint64_t cur = m_key[i].load(std::memory_order_acquire);
            if (cur == key) return static_cast<int>(i);
            if (cur == 0) {
                uint64_t expected = 0;
                if (m_key[i].compare_exchange_strong(expected, key, std::memory_order_acq_rel)) return static_cast<int>(i);
                if (expected == key) return static_cast<int>(i);
            }
        }
        return -1;
    }
    uint64_t key(size_t i) const noexcept { return i < N ? m_key[i].load(std::memory_order_acquire) : 0; }
private:
    std::atomic<uint64_t> m_key[N]{};
};

// The distinct values noted since the last drain (a window's activities, or its thread ids). A drain that races a note
// can list a value twice; the consumer de-duplicates what it gets.
template <size_t N>
class DistinctSet {
public:
    void note(uint64_t v) noexcept {
        if (!v) return;
        for (size_t i = 0; i < N; ++i) {
            const uint64_t cur = m_v[i].load(std::memory_order_acquire);
            if (cur == v) return;
            if (cur == 0) {
                uint64_t expected = 0;
                if (m_v[i].compare_exchange_strong(expected, v, std::memory_order_acq_rel)) return;
                if (expected == v) return;
            }
        }
        m_overflow.fetch_add(1, std::memory_order_relaxed);
    }
    size_t drain(uint64_t* out, size_t cap) noexcept {
        size_t n = 0;
        for (size_t i = 0; i < N; ++i) {
            const uint64_t v = m_v[i].exchange(0, std::memory_order_acq_rel);
            if (!v) continue;
            bool dup = false;
            for (size_t k = 0; k < n; ++k) dup = dup || out[k] == v;
            if (!dup && n < cap) out[n++] = v;
        }
        return n;
    }
    uint32_t takeOverflow() noexcept { return m_overflow.exchange(0, std::memory_order_relaxed); }
private:
    std::atomic<uint64_t> m_v[N]{};
    std::atomic<uint32_t> m_overflow{0};
};

// ---- state-change detection --------------------------------------------------------------------------------------------
// Per activity (an alternation between two activity objects must not read as a change). The first observation is
// reported with first = true.
struct ChangeTracker {
    bool have = false;
    uint32_t prev = 0;
    bool update(uint32_t now, uint32_t* before, bool* first) {
        if (!have) {
            have = true;
            prev = now;
            *before = now;
            *first = true;
            return true;
        }
        const bool changed = changedMask(prev, now) != 0;
        *before = prev;
        *first = false;
        prev = now;
        return changed;
    }
};

struct ChangeEvent {
    uint64_t activity;
    uint64_t slotCalls;
    uint32_t threadId;
    uint32_t before;
    uint32_t after;
    uint32_t first;
    uint32_t check;      // a hash of the other fields, set by EventRing::push and verified by take
    uint32_t reserved;
};
static_assert(std::is_trivially_copyable<ChangeEvent>::value && sizeof(ChangeEvent) % 4 == 0, "events are published as words");
static_assert(sizeof(ChangeEvent) == 40, "no padding bytes: the ring publishes every byte of an event");
// A torn event (two writers lapping each other in a full ring would mix their words) fails this and is counted lost.
inline uint32_t eventCheck(const ChangeEvent& e) {
    const uint64_t a = e.activity ^ (e.slotCalls * 0x9E3779B97F4A7C15ull);
    return static_cast<uint32_t>(a) ^ static_cast<uint32_t>(a >> 32) ^ (e.threadId * 2654435761u) ^ (e.before * 40503u) ^
           (e.after * 69069u) ^ (e.first * 0x5BD1E995u) ^ 0xA5A5A5A5u;
}

// Many writers (any thread the job runs on), one consumer. A full ring overwrites its oldest; the consumer counts what it lost.
template <size_t N>
class EventRing {
    static constexpr size_t kWords = sizeof(ChangeEvent) / 4;
    struct Entry {
        std::atomic<uint64_t> tag{0};
        std::atomic<uint32_t> w[kWords]{};
    };
public:
    void push(const ChangeEvent& event) noexcept {
        ChangeEvent e = event;
        e.check = eventCheck(e);
        e.reserved = 0;
        const uint64_t idx = m_head.fetch_add(1, std::memory_order_acq_rel);
        Entry& en = m_e[idx % N];
        uint32_t w[kWords];
        std::memcpy(w, &e, sizeof(e));
        en.tag.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < kWords; ++i) en.w[i].store(w[i], std::memory_order_relaxed);
        en.tag.store(idx + 1, std::memory_order_release);
    }
    // The consumer thread only. False when nothing (more) is ready.
    bool take(ChangeEvent* out) noexcept {
        for (;;) {
            const uint64_t head = m_head.load(std::memory_order_acquire);
            if (m_tail >= head) return false;
            if (head - m_tail > N) {
                m_lost += (head - N) - m_tail;
                m_tail = head - N;
            }
            Entry& en = m_e[m_tail % N];
            const uint64_t t1 = en.tag.load(std::memory_order_acquire);
            if (t1 == m_tail + 1) {
                uint32_t w[kWords];
                for (size_t i = 0; i < kWords; ++i) w[i] = en.w[i].load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (en.tag.load(std::memory_order_relaxed) == m_tail + 1) {
                    ChangeEvent got;
                    std::memcpy(&got, w, sizeof(ChangeEvent));
                    if (got.check == eventCheck(got)) {
                        *out = got;
                        ++m_tail;
                        return true;
                    }
                }
                ++m_lost;   // overwritten while it was being copied, or torn by two writers a lap apart
                ++m_tail;
                continue;
            }
            if (t1 > m_tail + 1) {   // a later lap already took the slot
                ++m_lost;
                ++m_tail;
                continue;
            }
            return false;            // its writer has reserved it and not finished: next tick
        }
    }
    uint64_t lost() const noexcept { return m_lost; }
    uint64_t pushed() const noexcept { return m_head.load(std::memory_order_acquire); }
private:
    Entry m_e[N];
    std::atomic<uint64_t> m_head{0};
    uint64_t m_tail = 0;
    uint64_t m_lost = 0;
};

// ---- the hook's shared state ---------------------------------------------------------------------------------------
constexpr size_t kSlots = 8;
constexpr size_t kRingEvents = 64;
constexpr uint32_t kMaxFaults = 64;   // after this many faulting reads the hook stops reading (it keeps forwarding)

struct Slot {
    std::atomic<uint64_t> calls{0};
    SeqSlot<Sample> sample;
    ChangeTracker tracker;   // touched only while sample.lock() is held
};

struct Shared {
    KeyTable<kSlots> keys;
    Slot slots[kSlots];
    std::atomic<uint64_t> totalCalls{0};
    std::atomic<uint32_t> slotsFull{0};   // calls from a 9th distinct activity: counted, not snapshotted
    std::atomic<uint32_t> dropped{0};     // publishes skipped because another writer held the slot
    std::atomic<uint32_t> faults{0};      // guarded reads that faulted
    DistinctSet<8> windowActivities;
    DistinctSet<8> windowThreads;
    EventRing<kRingEvents> events;
};

// One call of the observed function, as the hook records it. `raw` is null when the guarded read faulted. Takes no
// lock, allocates nothing and never waits.
inline void noteActivityCall(Shared& sh, uint64_t activity, uint32_t threadId, const Raw* raw) noexcept {
    const uint64_t total = sh.totalCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    sh.windowActivities.note(activity);
    sh.windowThreads.note(threadId);
    const int index = sh.keys.findOrInsert(activity);
    if (index < 0) {
        sh.slotsFull.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Slot& slot = sh.slots[index];
    const uint64_t n = slot.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!raw) {
        sh.faults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!slot.sample.lock()) {
        sh.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const Sample s = decodeSample(*raw, activity, threadId, n, total);
    uint32_t before = 0;
    bool first = false;
    if (slot.tracker.update(s.flags, &before, &first)) {
        ChangeEvent e;
        e.activity = activity;
        e.slotCalls = n;
        e.threadId = threadId;
        e.before = before;
        e.after = s.flags;
        e.first = first ? 1u : 0u;
        sh.events.push(e);
    }
    slot.sample.store(s);
    slot.sample.unlock();
}

// ---- cadence --------------------------------------------------------------------------------------------------------------
struct Cadence {
    uint64_t periodMs = 1000;
    uint64_t nextMs = 0;    // 0: due at once
    void reset(uint64_t nowMs) { nextMs = nowMs + periodMs; }
    void fireNext() { nextMs = 0; }
    bool due(uint64_t nowMs) {
        if (nowMs < nextMs) return false;
        nextMs = nowMs + periodMs;
        return true;
    }
};

// While the free camera's state byte is non-zero, and for 2 s after it returns to zero.
struct ActiveWindow {
    static constexpr uint64_t kHoldMs = 2000;
    enum Phase : int { Idle = 0, Active = 1, Hold = 2 };
    bool seen = false;
    uint64_t lastNonZeroMs = 0;
    int update(uint64_t nowMs, bool stateNonZero) {
        if (stateNonZero) {
            seen = true;
            lastNonZeroMs = nowMs;
            return Active;
        }
        if (seen && nowMs - lastNonZeroMs <= kHoldMs) return Hold;
        return Idle;
    }
};
inline const char* phaseName(int phase) { return phase == ActiveWindow::Active ? "active" : phase == ActiveWindow::Hold ? "hold" : "idle"; }

// ---- I2: the skinned-object fingerprint in the 5376-byte scene block ---------------------------------------------------
constexpr uint32_t kSceneBlockBytes = 5376;
constexpr size_t kSceneBlockFloats = kSceneBlockBytes / 4;
constexpr size_t kFingerprintFloats = 944;   // the highest float read is 943
enum class SkinClass { NotSkinned, Far, Skinned };
struct SkinHit {
    SkinClass cls = SkinClass::NotSkinned;
    float t[3] = {0, 0, 0};    // the translation (floats 935, 939, 943): the object's position in view space, metres
    float m[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};   // the 3x3 at floats 932..934, 936..938, 940..942, row-major
    float dist2 = 0;
};
// The fingerprint the unmerged head-look branch used (head_probe.cpp headAnchorNote5376): floats 536..538 equal the
// translation within 0.05, each of the three rows has a squared length in (0.81, 1.21), the translation is not zero and
// is under 50 m. Shape first, distance last, so a block of the right shape beyond the cap is counted, not lost.
inline SkinClass classifySkinned(const float* f, size_t floatCount, SkinHit* out) {
    if (!f || floatCount < kFingerprintFloats) return SkinClass::NotSkinned;
    const float tx = f[935], ty = f[939], tz = f[943];
    if (!(std::fabs(f[536] - tx) < 0.05f && std::fabs(f[537] - ty) < 0.05f && std::fabs(f[538] - tz) < 0.05f))
        return SkinClass::NotSkinned;
    for (int r = 0; r < 3; ++r) {
        const float* row = f + 932 + r * 4;
        const float l2 = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
        if (!(l2 > 0.81f && l2 < 1.21f)) return SkinClass::NotSkinned;
    }
    const float d2 = tx * tx + ty * ty + tz * tz;
    if (!(d2 > 1e-6f)) return SkinClass::NotSkinned;
    if (out) {
        out->t[0] = tx;
        out->t[1] = ty;
        out->t[2] = tz;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) out->m[r * 3 + c] = f[932 + r * 4 + c];
        out->dist2 = d2;
    }
    const SkinClass cls = d2 < 2500.0f ? SkinClass::Skinned : SkinClass::Far;
    if (out) out->cls = cls;
    return cls;
}

// The yaw and pitch of one direction in view space, in degrees. ASSUMES the view space is x right, y up, z forward
// (the order is the one thing Phase 0 has not measured; F0's log carries the 3x3 so any other convention can be
// recomputed): yaw = atan2(x, z), positive toward +x; pitch = asin(y), positive toward +y.
inline void axisYawPitch(float x, float y, float z, float* yawDeg, float* pitchDeg) {
    const float rad = 57.29577951308232f;
    const float len = std::sqrt(x * x + y * y + z * z);
    float s = len > 0.0f ? y / len : 0.0f;
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    *yawDeg = std::atan2(x, z) * rad;
    *pitchDeg = std::asin(s) * rad;
}
// The model's three axes are the COLUMNS of the row-major 3x3 (view = M x model + t): column c is (m[c], m[3+c], m[6+c]).
inline void modelAxisYawPitch(const float m[9], int column, float* yawDeg, float* pitchDeg) {
    axisYawPitch(m[column], m[3 + column], m[6 + column], yawDeg, pitchDeg);
}

constexpr size_t kSkinBlocks = 64;
struct SkinSecond {
    uint32_t seen = 0;          // 5376-byte blocks the tee offered
    uint32_t hits = 0;          // fingerprinted as skinned and within 50 m
    uint32_t beyond = 0;        // right shape, beyond 50 m (the "far" of the log line: windows.h defines far as a macro)
    uint32_t busySkips = 0;     // hits whose nearest/distinct bookkeeping was skipped because the lock was held
    uint32_t distinct = 0;      // distinct resources among the hits that were recorded
    uint32_t blockOverflow = 0;
    bool haveNearest = false;
    SkinHit nearest;
    uint64_t nearestResource = 0;
};
class SkinTee {
public:
    // Any thread that unmaps a 5376-byte scene block. The fingerprint is read-only on the (write-combined) mapped memory.
    void note(uint64_t resource, const float* f, size_t floatCount) noexcept {
        m_seen.fetch_add(1, std::memory_order_relaxed);
        SkinHit hit;
        const SkinClass cls = classifySkinned(f, floatCount, &hit);
        if (cls == SkinClass::NotSkinned) return;
        if (cls == SkinClass::Far) {
            m_far.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_hits.fetch_add(1, std::memory_order_relaxed);
        if (m_lock.exchange(1, std::memory_order_acquire)) {
            m_busy.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        bool known = false;
        for (uint32_t i = 0; i < m_nBlocks; ++i) known = known || m_blocks[i] == resource;
        if (!known) {
            if (m_nBlocks < kSkinBlocks) m_blocks[m_nBlocks++] = resource;
            else ++m_blockOverflow;
        }
        if (!m_haveNearest || hit.dist2 < m_nearest.dist2) {
            m_haveNearest = true;
            m_nearest = hit;
            m_nearestResource = resource;
        }
        m_lock.store(0, std::memory_order_release);
    }
    // The consumer: everything since the last take, and the accumulators cleared.
    SkinSecond take() noexcept {
        SkinSecond s;
        s.seen = m_seen.exchange(0, std::memory_order_relaxed);
        s.hits = m_hits.exchange(0, std::memory_order_relaxed);
        s.beyond = m_far.exchange(0, std::memory_order_relaxed);
        s.busySkips = m_busy.exchange(0, std::memory_order_relaxed);
        for (int spin = 0; spin < 100000; ++spin) {
            if (m_lock.exchange(1, std::memory_order_acquire) == 0) {
                s.distinct = m_nBlocks;
                s.blockOverflow = m_blockOverflow;
                s.haveNearest = m_haveNearest;
                s.nearest = m_nearest;
                s.nearestResource = m_nearestResource;
                m_nBlocks = 0;
                m_blockOverflow = 0;
                m_haveNearest = false;
                m_nearest = SkinHit{};
                m_nearestResource = 0;
                m_lock.store(0, std::memory_order_release);
                return s;
            }
        }
        s.busySkips += 1;   // the lock never came free: report the counters without the bookkeeping
        return s;
    }
private:
    std::atomic<uint32_t> m_seen{0}, m_hits{0}, m_far{0}, m_busy{0};
    std::atomic<uint32_t> m_lock{0};
    uint64_t m_blocks[kSkinBlocks] = {};
    uint32_t m_nBlocks = 0;
    uint32_t m_blockOverflow = 0;
    bool m_haveNearest = false;
    SkinHit m_nearest;
    uint64_t m_nearestResource = 0;
};

// ---- I1: the census's refresh calls, tallied per (kind, call site) ------------------------------------------------------
// The camera struct, as the census snapshots it (camera+0x20 .. +0x2AF): axes camera+0x20..+0x4C, origin +0x50..+0x58,
// kind +0x264.
constexpr uint32_t kCamSnapFrom = 0x20;
constexpr uint32_t kCamAxes = 0x20;
constexpr uint32_t kCamOrigin = 0x50;
constexpr uint32_t kCamKind = 0x264;
constexpr size_t kCamRows = 16;
constexpr size_t kCamKindSlots = 16;
struct CamRow {
    bool used = false;
    uint32_t kind = 0;
    uint32_t site = 0;       // the call site's offset from the game module
    uint32_t calls = 0;
    uint32_t frames = 0;     // distinct frames the row was called in
    uint64_t lastFrame = 0;
    uint32_t cameras = 0;    // distinct camera objects (first four remembered)
    uint64_t cam[4] = {0, 0, 0, 0};
    float origin[3] = {0, 0, 0};   // the last call's, read after the game's body ran
    float axes[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
};
struct CamSecond {
    CamRow rows[kCamRows];
    uint32_t used = 0;
    uint32_t overflow = 0;   // calls from a (kind, site) beyond the table
    uint32_t calls = 0;
    uint32_t kindCalls[kCamKindSlots] = {};
    uint32_t kindOverflow = 0;
};
// The census's observer runs on the owner (render) thread and so does the consumer: no lock.
class CamTee {
public:
    // `snap` is the census's copy of camera+0x20 onwards (at least 0x290 bytes).
    void note(uint64_t frame, uint32_t site, uint64_t camera, const uint8_t* snap) noexcept {
        uint32_t kind = 0;
        std::memcpy(&kind, snap + (kCamKind - kCamSnapFrom), sizeof(kind));
        ++m_cur.calls;
        if (kind < kCamKindSlots) ++m_cur.kindCalls[kind];
        else ++m_cur.kindOverflow;
        CamRow* row = nullptr;
        for (size_t i = 0; i < m_cur.used; ++i) {
            if (m_cur.rows[i].kind == kind && m_cur.rows[i].site == site) { row = &m_cur.rows[i]; break; }
        }
        if (!row) {
            if (m_cur.used >= kCamRows) { ++m_cur.overflow; return; }
            row = &m_cur.rows[m_cur.used++];
            row->used = true;
            row->kind = kind;
            row->site = site;
        }
        ++row->calls;
        if (row->frames == 0 || row->lastFrame != frame) { ++row->frames; row->lastFrame = frame; }
        bool known = false;
        const uint32_t remembered = row->cameras < 4 ? row->cameras : 4;
        for (uint32_t i = 0; i < remembered; ++i) known = known || row->cam[i] == camera;
        if (!known) {
            if (row->cameras < 4) row->cam[row->cameras] = camera;
            ++row->cameras;
        }
        std::memcpy(row->origin, snap + (kCamOrigin - kCamSnapFrom), sizeof(row->origin));
        std::memcpy(row->axes, snap + (kCamAxes - kCamSnapFrom), sizeof(row->axes));
    }
    CamSecond take() {
        CamSecond out = m_cur;
        m_cur = CamSecond{};
        return out;
    }
private:
    CamSecond m_cur;
};

// ---- text ----------------------------------------------------------------------------------------------------------------
constexpr size_t kLineBytes = 1100;   // under Log's 1200-byte line
struct Line {
    char* buf;
    size_t cap;
    size_t n = 0;
    Line(char* b, size_t c) : buf(b), cap(c) { if (cap) buf[0] = 0; }
    void put(const char* fmt, ...) {
        if (n + 1 >= cap) return;
        va_list ap;
        va_start(ap, fmt);
        const int w = std::vsnprintf(buf + n, cap - n, fmt, ap);
        va_end(ap);
        if (w < 0) return;
        if (static_cast<size_t>(w) >= cap - n) n = cap - 1;
        else n += static_cast<size_t>(w);
    }
};

// The line prefixes an F0 log reader greps for. Every instrument's armed (or stood-down) line is printed once, and its
// heartbeat every 5 s, so a zero is distinguishable from not running.
inline const char* prefixOn() { return "explorer cam probe: on"; }
inline const char* prefixOff() { return "explorer cam probe: off"; }
inline const char* prefixI3Armed() { return "explorer cam probe I3 armed:"; }
inline const char* prefixI3Down() { return "explorer cam probe I3 stood down:"; }
inline const char* prefixI3Heartbeat() { return "explorer cam probe I3 heartbeat:"; }
inline const char* prefixI3Change() { return "explorer cam probe I3 change:"; }
inline const char* prefixI3Pose() { return "explorer cam probe I3 pose:"; }
inline const char* prefixI3Stale() { return "explorer cam probe I3 stale:"; }
inline const char* prefixI1Armed() { return "explorer cam probe I1 armed:"; }
inline const char* prefixI1Heartbeat() { return "explorer cam probe I1 heartbeat:"; }
inline const char* prefixI1Cam() { return "explorer cam probe I1 cam:"; }
inline const char* prefixI2Armed() { return "explorer cam probe I2 armed:"; }
inline const char* prefixI2Heartbeat() { return "explorer cam probe I2 heartbeat:"; }
inline const char* prefixI2Root() { return "explorer cam probe I2 root:"; }

inline void putV3(Line& o, const float* v) { o.put("(%.3f,%.3f,%.3f)", v[0], v[1], v[2]); }
// A row-major 4x4's three axes rows (xyz of each), as "[(..)(..)(..)]".
inline void putBasis(Line& o, const float* m16) {
    o.put("[");
    for (int r = 0; r < 3; ++r) putV3(o, m16 + r * 4);
    o.put("]");
}

inline void formatChange(char* out, size_t cap, const ChangeEvent& e, uint32_t frame) {
    Line o(out, cap);
    const uint32_t mask = changedMask(e.before, e.after);
    o.put("%s act=0x%llX thread=%u call=%llu frame=%u %s +0x48C %u->%u +0x470 %u->%u +0x471 %u->%u +0x473 %u->%u changed=",
          prefixI3Change(), static_cast<unsigned long long>(e.activity), e.threadId,
          static_cast<unsigned long long>(e.slotCalls), frame, e.first ? "first-call" : "change",
          flagState(e.before), flagState(e.after), flagRelative(e.before), flagRelative(e.after),
          flagRotationLock(e.before), flagRotationLock(e.after), flagPreset(e.before), flagPreset(e.after));
    if (e.first) { o.put("-"); return; }
    bool any = false;
    if (mask & kChangedState) { o.put("%s48C", any ? "," : ""); any = true; }
    if (mask & kChangedRelative) { o.put("%s470", any ? "," : ""); any = true; }
    if (mask & kChangedRotationLock) { o.put("%s471", any ? "," : ""); any = true; }
    if (mask & kChangedPreset) { o.put("%s473", any ? "," : ""); any = true; }
    if (!any) o.put("-");
}

inline void formatPose(char* out, size_t cap, const Sample& s, int phase, int64_t ageMs) {
    Line o(out, cap);
    o.put("%s phase=%s act=0x%llX thread=%u call=%llu age_ms=%lld state=%u relative=%u rotation_lock=%u preset_pending=%u target=0x%llX ",
          prefixI3Pose(), phaseName(phase), static_cast<unsigned long long>(s.activity), s.threadId,
          static_cast<unsigned long long>(s.slotCalls), static_cast<long long>(ageMs), flagState(s.flags),
          flagRelative(s.flags), flagRotationLock(s.flags), flagPreset(s.flags),
          static_cast<unsigned long long>(s.target));
    o.put("local_origin(+0x3E0)=");
    putV3(o, s.local + 12);
    o.put(" local_basis(+0x3B0 rows)=");
    putBasis(o, s.local);
    o.put(" world_origin(+0xA0)=");
    putV3(o, s.world + 12);
    o.put(" world_basis(+0x70 rows)=");
    putBasis(o, s.world);
}

inline void formatCamRow(char* out, size_t cap, const CamRow& r, int phase) {
    Line o(out, cap);
    o.put("%s phase=%s kind=%u site=+0x%X calls=%u frames=%u calls_per_frame=%.2f cameras=%u origin(+0x50)=",
          prefixI1Cam(), phaseName(phase), r.kind, r.site, r.calls, r.frames,
          r.frames ? static_cast<double>(r.calls) / static_cast<double>(r.frames) : 0.0, r.cameras);
    putV3(o, r.origin);
    o.put(" axes(+0x20 rows)=[");
    for (int row = 0; row < 3; ++row) o.put("(%.3f,%.3f,%.3f,%.3f)", r.axes[row * 4], r.axes[row * 4 + 1], r.axes[row * 4 + 2], r.axes[row * 4 + 3]);
    o.put("]");
}

inline void formatRoot(char* out, size_t cap, const SkinSecond& s, int phase) {
    Line o(out, cap);
    o.put("%s phase=%s skinned_blocks=%u hits=%u far=%u offered=%u", prefixI2Root(), phaseName(phase), s.distinct, s.hits, s.beyond, s.seen);
    if (!s.haveNearest) {
        o.put(" nearest=none");
        return;
    }
    float yx, px, yy, py, yz, pz;
    modelAxisYawPitch(s.nearest.m, 0, &yx, &px);
    modelAxisYawPitch(s.nearest.m, 1, &yy, &py);
    modelAxisYawPitch(s.nearest.m, 2, &yz, &pz);
    o.put(" nearest_view_m=");
    putV3(o, s.nearest.t);
    o.put(" dist_m=%.3f block=0x%llX", std::sqrt(s.nearest.dist2), static_cast<unsigned long long>(s.nearestResource));
    o.put(" model_axes_in_view[+X yaw=%.1f pitch=%.1f | +Y yaw=%.1f pitch=%.1f | +Z yaw=%.1f pitch=%.1f] ",
          yx, px, yy, py, yz, pz);
    o.put("3x3_rows=[(%.2f,%.2f,%.2f)(%.2f,%.2f,%.2f)(%.2f,%.2f,%.2f)] ",
          s.nearest.m[0], s.nearest.m[1], s.nearest.m[2], s.nearest.m[3], s.nearest.m[4], s.nearest.m[5],
          s.nearest.m[6], s.nearest.m[7], s.nearest.m[8]);
    o.put("convention: M = floats 932,936,940 (row-major, translation in column 4) is model-to-view, so view = M x model + t "
          "and the model axes are M's COLUMNS; assumed view space x right, y up, z forward; yaw = atan2(x,z) deg toward +x, "
          "pitch = asin(y) deg toward +y");
}

// ---- the consumer ---------------------------------------------------------------------------------------------------------
using SinkFn = void (*)(void* ctx, const char* line);
struct Sink {
    SinkFn fn = nullptr;
    void* ctx = nullptr;
    void operator()(const char* line) const { if (fn) fn(ctx, line); }
};

// What the glue knows that the shared state does not.
struct TickIn {
    uint64_t nowMs = 0;
    uint32_t frame = 0;
    bool i3Armed = false;
    const char* i3Status = "not tried";   // "armed" | "stood down" | "not tried"
    bool censusWanted = false;            // the census is running (vrCameraCensusWanted)
    const char* injectStatus = "?";       // flatCameraInjectObserveStatus
    bool i2Armed = false;
};

class Consumer {
public:
    static constexpr uint64_t kFreshMs = 1500;   // a call within this long makes an activity "live"
    static constexpr uint64_t kBeatMs = 5000;

    void start(uint64_t nowMs, uint64_t totalCallsNow = 0) {
        m_lastTotal = totalCallsNow;
        m_sec.periodMs = 1000;
        m_sec.reset(nowMs);
        m_beat.periodMs = kBeatMs;
        m_beat.reset(nowMs);
        m_lastBeatMs = nowMs;
        m_phase = ActiveWindow::Idle;
        m_window = ActiveWindow{};
    }

    void tick(Shared& sh, SkinTee& skin, CamTee& cam, const TickIn& in, const Sink& sink) {
        char line[kLineBytes];
        // 1. State changes the hook saw, at once.
        ChangeEvent ev;
        while (sh.events.take(&ev)) {
            formatChange(line, sizeof(line), ev, in.frame);
            sink(line);
        }
        // 2. Which activities are live, and what each says.
        bool anyNonZero = false;
        int liveBest = -1;      // the live activity with a non-zero state and the most calls
        int recentBest = -1;    // else the most recently called
        for (size_t i = 0; i < kSlots; ++i) {
            m_valid[i] = false;
            if (!sh.keys.key(i)) continue;
            Sample s;
            if (!sh.slots[i].sample.read(s)) continue;
            m_cur[i] = s;
            m_valid[i] = true;
            if (s.slotCalls != m_lastCalls[i]) {
                m_lastCalls[i] = s.slotCalls;
                m_lastAdvanceMs[i] = in.nowMs;
            }
            const bool fresh = in.nowMs - m_lastAdvanceMs[i] <= kFreshMs;
            const bool nonZero = flagState(s.flags) != 0;
            if (fresh && nonZero) {
                anyNonZero = true;
                if (liveBest < 0 || s.slotCalls > m_cur[liveBest].slotCalls) liveBest = static_cast<int>(i);
                m_staleNoted[i] = false;
            } else if (!fresh && nonZero && !m_staleNoted[i]) {
                m_staleNoted[i] = true;
                Line o(line, sizeof(line));
                o.put("%s act=0x%llX stopped being called %lld ms ago with +0x48C=%u (its last snapshot; it is not treated as engaged)",
                      prefixI3Stale(), static_cast<unsigned long long>(s.activity),
                      static_cast<long long>(in.nowMs - m_lastAdvanceMs[i]), flagState(s.flags));
                sink(line);
            }
            if (fresh && recentBest < 0) recentBest = static_cast<int>(i);
            else if (fresh && m_lastAdvanceMs[i] > m_lastAdvanceMs[recentBest]) recentBest = static_cast<int>(i);
        }
        const int before = m_phase;
        m_phase = m_window.update(in.nowMs, anyNonZero);
        if (before == ActiveWindow::Idle && m_phase != ActiveWindow::Idle) m_sec.fireNext();   // the first detail line is at once

        // 3. 1 Hz: always drained (so a second means a second), printed only while the free camera is engaged.
        if (m_sec.due(in.nowMs)) {
            const SkinSecond skinSec = skin.take();
            const CamSecond camSec = cam.take();
            m_skinSeen5 += skinSec.seen;
            m_skinHits5 += skinSec.hits;
            m_skinFar5 += skinSec.beyond;
            m_skinBusy5 += skinSec.busySkips;
            if (skinSec.distinct > m_skinDistinctMax) m_skinDistinctMax = skinSec.distinct;
            m_camCalls5 += camSec.calls;
            for (size_t k = 0; k < kCamKindSlots; ++k) m_camKind5[k] += camSec.kindCalls[k];
            m_camKindOverflow5 += camSec.kindOverflow;
            if (camSec.used > m_camRowsMax) m_camRowsMax = camSec.used;
            m_camOverflow5 += camSec.overflow;
            if (m_phase != ActiveWindow::Idle) {
                const int pick = liveBest >= 0 ? liveBest : recentBest;
                if (pick >= 0) {
                    formatPose(line, sizeof(line), m_cur[pick], m_phase,
                               static_cast<int64_t>(in.nowMs - m_lastAdvanceMs[pick]));
                    sink(line);
                }
                for (uint32_t r = 0; r < camSec.used; ++r) {
                    formatCamRow(line, sizeof(line), camSec.rows[r], m_phase);
                    sink(line);
                }
                if (camSec.used == 0) {
                    Line o(line, sizeof(line));
                    o.put("%s phase=%s no refresh call reached the probe this second (census=%s inject_hook=%s)",
                          prefixI1Cam(), phaseName(m_phase), in.censusWanted ? "running" : "not running", in.injectStatus);
                    sink(line);
                }
                formatRoot(line, sizeof(line), skinSec, m_phase);
                sink(line);
            }
        }

        // 4. 5 s heartbeats: one per instrument, always.
        if (m_beat.due(in.nowMs)) {
            heartbeat(sh, in, sink);
            m_lastBeatMs = in.nowMs;
        }
    }

    int phase() const { return m_phase; }

private:
    void heartbeat(Shared& sh, const TickIn& in, const Sink& sink) {
        char line[kLineBytes];
        const double seconds = static_cast<double>(in.nowMs - m_lastBeatMs) / 1000.0;
        // I3
        {
            const uint64_t total = sh.totalCalls.load(std::memory_order_relaxed);
            const uint64_t calls = total - m_lastTotal;
            m_lastTotal = total;
            uint64_t acts[8], tids[8];
            const size_t nAct = sh.windowActivities.drain(acts, 8);
            const size_t nTid = sh.windowThreads.drain(tids, 8);
            const uint32_t actOverflow = sh.windowActivities.takeOverflow();
            const uint32_t tidOverflow = sh.windowThreads.takeOverflow();
            Line o(line, sizeof(line));
            o.put("%s window=%.1fs hook=%s calls=%llu total_calls=%llu activities=%zu threads=%zu",
                  prefixI3Heartbeat(), seconds, in.i3Status, static_cast<unsigned long long>(calls),
                  static_cast<unsigned long long>(total), nAct, nTid);
            if (nAct) {
                o.put(" activity_ptrs=[");
                for (size_t i = 0; i < nAct; ++i) o.put("%s0x%llX", i ? "," : "", static_cast<unsigned long long>(acts[i]));
                o.put("]");
            }
            if (nTid) {
                o.put(" thread_ids=[");
                for (size_t i = 0; i < nTid; ++i) o.put("%s%llu", i ? "," : "", static_cast<unsigned long long>(tids[i]));
                o.put("]");
            }
            if (actOverflow || tidOverflow) o.put(" set_overflow=%u/%u", actOverflow, tidOverflow);
            o.put(" dropped=%u faults=%u slots_full=%u events_lost=%llu phase=%s",
                  sh.dropped.load(std::memory_order_relaxed), sh.faults.load(std::memory_order_relaxed),
                  sh.slotsFull.load(std::memory_order_relaxed), static_cast<unsigned long long>(sh.events.lost()),
                  phaseName(m_phase));
            bool listed = false;
            for (size_t i = 0; i < kSlots; ++i) {
                if (!m_valid[i]) continue;
                const Sample& s = m_cur[i];
                o.put("%s act=0x%llX +0x48C=%u +0x470=%u +0x471=%u +0x473=%u calls=%llu age_ms=%lld",
                      listed ? " |" : " state:", static_cast<unsigned long long>(s.activity), flagState(s.flags),
                      flagRelative(s.flags), flagRotationLock(s.flags), flagPreset(s.flags),
                      static_cast<unsigned long long>(s.slotCalls), static_cast<long long>(in.nowMs - m_lastAdvanceMs[i]));
                listed = true;
            }
            if (!listed) {
                if (!in.i3Armed) o.put(" state=- (the hook is %s: no call can be seen)", in.i3Status);
                else if (total == 0) o.put(" state=- idle=no-call-yet (the activity does not exist, or the hook is not reached)");
                else o.put(" state=- (calls seen, no snapshot yet)");
            } else if (calls == 0) {
                o.put(" idle=no-call-in-window");
            }
            sink(line);
        }
        // I1
        {
            Line o(line, sizeof(line));
            o.put("%s window=%.1fs census=%s inject_hook=%s gate=%s census_calls=%u rows_max_1s=%u row_overflow=%u kinds=[",
                  prefixI1Heartbeat(), seconds, in.censusWanted ? "running" : "not running", in.injectStatus,
                  in.i3Armed ? "I3 +0x48C" : "I3 down (no detail lines)", m_camCalls5, m_camRowsMax, m_camOverflow5);
            bool any = false;
            for (size_t k = 0; k < kCamKindSlots; ++k) {
                if (!m_camKind5[k]) continue;
                o.put("%s%zu:%u", any ? "," : "", k, m_camKind5[k]);
                any = true;
            }
            if (m_camKindOverflow5) o.put("%sother:%u", any ? "," : "", m_camKindOverflow5);
            o.put("]");
            if (m_camCalls5 == 0) o.put(" idle=no-refresh-call-reached-the-probe");
            sink(line);
            m_camCalls5 = 0;
            for (size_t k = 0; k < kCamKindSlots; ++k) m_camKind5[k] = 0;
            m_camKindOverflow5 = 0;
            m_camRowsMax = 0;
            m_camOverflow5 = 0;
        }
        // I2
        {
            Line o(line, sizeof(line));
            o.put("%s window=%.1fs tee=%s blocks_offered=%u skinned=%u far=%u distinct_blocks_max_1s=%u busy_skips=%u",
                  prefixI2Heartbeat(), seconds, in.i2Armed ? "armed" : "not armed", m_skinSeen5, m_skinHits5, m_skinFar5,
                  m_skinDistinctMax, m_skinBusy5);
            if (m_skinSeen5 == 0) o.put(" idle=no-5376-byte-block-offered");
            else if (m_skinHits5 == 0) o.put(" idle=blocks-offered-none-skinned");
            sink(line);
            m_skinSeen5 = m_skinHits5 = m_skinFar5 = m_skinBusy5 = m_skinDistinctMax = 0;
        }
    }

    Cadence m_sec, m_beat;
    ActiveWindow m_window;
    int m_phase = ActiveWindow::Idle;
    uint64_t m_lastBeatMs = 0;
    uint64_t m_lastTotal = 0;
    Sample m_cur[kSlots] = {};
    bool m_valid[kSlots] = {};
    bool m_staleNoted[kSlots] = {};
    uint64_t m_lastCalls[kSlots] = {};
    uint64_t m_lastAdvanceMs[kSlots] = {};
    uint32_t m_skinSeen5 = 0, m_skinHits5 = 0, m_skinFar5 = 0, m_skinBusy5 = 0, m_skinDistinctMax = 0;
    uint32_t m_camCalls5 = 0, m_camRowsMax = 0, m_camOverflow5 = 0, m_camKindOverflow5 = 0;
    uint32_t m_camKind5[kCamKindSlots] = {};
};

}  // namespace ecp
}  // namespace edvr
