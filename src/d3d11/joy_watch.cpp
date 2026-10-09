#include "joy_watch.h"

#include <atomic>
#include <cstring>

#include "../common/hotkey.h"

namespace edvr {
namespace {

// A device nobody has read for this long is not being read: its buttons count
// as up, and when the game starts reading it again whatever is already down is
// armed (ignored until released) so that a button held through a pause does not
// become a press.
constexpr uint64_t kStaleMs = 1500;
// A slot that has not been read for this long may be given to another device.
constexpr uint64_t kEvictMs = 5000;

constexpr uint64_t kAllCentred = ~0ull;   // four hats of 0xFFFF

struct Slot {
    std::atomic<const void*> device{nullptr};
    std::atomic<uint32_t>    id{0};
    std::atomic<uint64_t>    stampMs{0};
    std::atomic<uint64_t>    buttons[2];
    std::atomic<uint64_t>    pov{kAllCentred};   // 4 x 16 bits, hat 0 lowest
    std::atomic<uint64_t>    armed[3];            // held when reading (re)started
    Slot() {
        for (auto& b : buttons) b.store(0, std::memory_order_relaxed);
        for (auto& a : armed) a.store(0, std::memory_order_relaxed);
    }
};

Slot g_slot[kJoyWatchSlots];

std::atomic<uint64_t> g_stateCalls{0};
std::atomic<uint64_t> g_dataCalls{0};
std::atomic<uint64_t> g_rowsUsed{0};
std::atomic<uint64_t> g_rowsIgnored{0};
std::atomic<uint64_t> g_statesIgnored{0};
std::atomic<uint64_t> g_tableFull{0};
std::atomic<uint32_t> g_lastIgnoredSize{0};
std::atomic<uint32_t> g_lastIgnoredOfs{0};

uint16_t hatOf(uint64_t pov, int hat) { return static_cast<uint16_t>(pov >> (16 * hat)); }

// A hat value the layouts can hold: 0..35999, or centred (LOWORD 0xFFFF).
uint16_t normalHat(uint32_t raw) {
    const uint16_t lo = static_cast<uint16_t>(raw & 0xFFFFu);
    return (lo != 0xFFFF && lo > 35999) ? static_cast<uint16_t>(0xFFFF) : lo;
}

// The sixteen "hat n is toward d" bits, hat-major, direction Up, Right, Down, Left.
uint64_t dirBits(uint64_t pov) {
    uint64_t bits = 0;
    for (int hat = 0; hat < kJoyHatCount; ++hat) {
        const uint16_t v = hatOf(pov, hat);
        for (int dir = 0; dir < 4; ++dir) {
            if (joyPovMatches(v, dir)) bits |= 1ull << (hat * 4 + dir);
        }
    }
    return bits;
}

void heldBits(const Slot& s, uint64_t out[3]) {
    out[0] = s.buttons[0].load(std::memory_order_relaxed);
    out[1] = s.buttons[1].load(std::memory_order_relaxed);
    out[2] = dirBits(s.pov.load(std::memory_order_relaxed));
}

bool fresh(const Slot& s, uint64_t nowMs) {
    const uint64_t stamp = s.stampMs.load(std::memory_order_relaxed);
    return stamp != 0 && nowMs >= stamp && nowMs - stamp <= kStaleMs;
}

void initSlot(Slot& s, uint32_t id) {
    s.id.store(id, std::memory_order_relaxed);
    s.stampMs.store(0, std::memory_order_relaxed);
    s.buttons[0].store(0, std::memory_order_relaxed);
    s.buttons[1].store(0, std::memory_order_relaxed);
    s.pov.store(kAllCentred, std::memory_order_relaxed);
    for (auto& a : s.armed) a.store(0, std::memory_order_relaxed);
}

Slot* claim(const void* device, uint32_t id, uint64_t nowMs) {
    for (Slot& s : g_slot) {
        if (s.device.load(std::memory_order_acquire) == device) {
            s.id.store(id, std::memory_order_relaxed);
            return &s;
        }
    }
    for (Slot& s : g_slot) {
        const void* expect = nullptr;
        if (s.device.compare_exchange_strong(expect, device, std::memory_order_acq_rel)) {
            initSlot(s, id);
            return &s;
        }
    }
    // Full: a slot nobody has read for a while belongs to a device the game let
    // go of. Its readers see it as stale already, so handing it over is safe.
    Slot* oldest = nullptr;
    for (Slot& s : g_slot) {
        const uint64_t stamp = s.stampMs.load(std::memory_order_relaxed);
        if (nowMs >= stamp && nowMs - stamp > kEvictMs && (!oldest || stamp < oldest->stampMs.load())) oldest = &s;
    }
    if (oldest) {
        initSlot(*oldest, id);
        oldest->device.store(device, std::memory_order_release);
        return oldest;
    }
    g_tableFull.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

// Call BEFORE the new state is stored: a slot that was not being read arms
// whatever the new state holds.
bool wasIdle(const Slot& s, uint64_t nowMs) { return !fresh(s, nowMs); }

// After the new state is stored. A slot that was idle arms whatever is down; a slot
// that was being read lets go of the armed buttons it now sees released. The writer
// owns `armed` entirely, so the reader needs no store and a release is seen however
// briefly it lasted.
void settleArmed(Slot& s, bool idle) {
    uint64_t h[3];
    heldBits(s, h);
    for (int i = 0; i < 3; ++i) {
        if (idle) s.armed[i].store(h[i], std::memory_order_relaxed);
        else s.armed[i].fetch_and(h[i], std::memory_order_relaxed);
    }
}

}  // namespace

uint32_t joyDeviceIdFromProduct(uint32_t productData1) {
    return ((productData1 & 0xFFFFu) << 16) | (productData1 >> 16);
}

bool joyDeviceTypeIsController(uint32_t type) {
    switch (type) {
        case 0x11:   // DI8DEVTYPE_DEVICE
        case 0x14:   // JOYSTICK
        case 0x15:   // GAMEPAD
        case 0x16:   // DRIVING
        case 0x17:   // FLIGHT
        case 0x18:   // 1STPERSON
        case 0x1C:   // SUPPLEMENTAL
            return true;
        default:
            return false;
    }
}

bool joyPovMatches(uint16_t value, int direction) {
    if (value == 0xFFFF || value > 35999 || direction < 0 || direction > 3) return false;
    int diff = static_cast<int>(value) - direction * 9000;
    if (diff < 0) diff = -diff;
    if (diff > 18000) diff = 36000 - diff;
    return diff <= 4500;
}

bool joyWatchObserveState(const void* device, uint32_t deviceId, const void* data, uint32_t cb,
                          uint64_t nowMs) {
    if (!device || !data) return false;
    g_stateCalls.fetch_add(1, std::memory_order_relaxed);
    if (cb != kJoyStateSize && cb != kJoyState2Size) {
        g_statesIgnored.fetch_add(1, std::memory_order_relaxed);
        g_lastIgnoredSize.store(cb, std::memory_order_relaxed);
        return false;
    }
    Slot* s = claim(device, deviceId, nowMs);
    if (!s) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const bool idle = wasIdle(*s, nowMs);
    const uint32_t nb = cb == kJoyState2Size ? 128u : 32u;
    uint64_t b[2] = {0, 0};
    for (uint32_t i = 0; i < nb; ++i) {
        if (p[kJoyOfsButtons + i] & 0x80) b[i >> 6] |= 1ull << (i & 63);
    }
    uint64_t pov = 0;
    for (int hat = 0; hat < kJoyHatCount; ++hat) {
        uint32_t raw;
        memcpy(&raw, p + kJoyOfsPov + 4 * hat, sizeof(raw));
        pov |= static_cast<uint64_t>(normalHat(raw)) << (16 * hat);
    }
    s->buttons[0].store(b[0], std::memory_order_relaxed);
    s->buttons[1].store(b[1], std::memory_order_relaxed);
    s->pov.store(pov, std::memory_order_relaxed);
    settleArmed(*s, idle);
    s->stampMs.store(nowMs ? nowMs : 1, std::memory_order_release);
    return true;
}

bool joyWatchObserveData(const void* device, uint32_t deviceId, const void* rows, uint32_t count,
                         uint32_t stride, uint64_t nowMs) {
    if (!device) return false;
    g_dataCalls.fetch_add(1, std::memory_order_relaxed);
    if (count && (!rows || stride < 8)) return false;
    Slot* s = claim(device, deviceId, nowMs);
    if (!s) return false;
    const bool idle = wasIdle(*s, nowMs);
    const uint8_t* base = static_cast<const uint8_t*>(rows);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t ofs, data;
        memcpy(&ofs, base + static_cast<size_t>(i) * stride, 4);
        memcpy(&data, base + static_cast<size_t>(i) * stride + 4, 4);
        if (ofs >= kJoyOfsButtons && ofs < kJoyOfsButtons + 128) {
            const uint32_t n = ofs - kJoyOfsButtons;
            const uint64_t bit = 1ull << (n & 63);
            if (data & 0x80) s->buttons[n >> 6].fetch_or(bit, std::memory_order_relaxed);
            else s->buttons[n >> 6].fetch_and(~bit, std::memory_order_relaxed);
            g_rowsUsed.fetch_add(1, std::memory_order_relaxed);
        } else if (ofs >= kJoyOfsPov && ofs < kJoyOfsPov + 4 * kJoyHatCount && ((ofs - kJoyOfsPov) & 3) == 0) {
            const int hat = static_cast<int>((ofs - kJoyOfsPov) / 4);
            const uint64_t mask = 0xFFFFull << (16 * hat);
            uint64_t cur = s->pov.load(std::memory_order_relaxed);
            cur = (cur & ~mask) | (static_cast<uint64_t>(normalHat(data)) << (16 * hat));
            s->pov.store(cur, std::memory_order_relaxed);
            g_rowsUsed.fetch_add(1, std::memory_order_relaxed);
        } else if (ofs >= kJoyOfsPov) {
            // Past the buttons (velocities, forces) or a custom format's own
            // offsets: not a button. The axes below 32 are normal traffic and
            // are not counted as ignored.
            g_rowsIgnored.fetch_add(1, std::memory_order_relaxed);
            g_lastIgnoredOfs.store(ofs, std::memory_order_relaxed);
        }
    }
    settleArmed(*s, idle);
    s->stampMs.store(nowMs ? nowMs : 1, std::memory_order_release);
    return true;
}

namespace {

// The effective held bits of one slot: held, fresh, and not armed.
bool effective(const Slot& s, uint64_t nowMs, uint64_t out[3]) {
    if (!fresh(s, nowMs)) return false;
    uint64_t h[3];
    heldBits(s, h);
    for (int i = 0; i < 3; ++i) out[i] = h[i] & ~s.armed[i].load(std::memory_order_relaxed);
    return true;
}

}  // namespace

bool joyWatchHeld(uint32_t deviceId, uint16_t input, uint64_t nowMs) {
    if (input >= kJoyInputCount) return false;
    for (Slot& s : g_slot) {
        if (!s.device.load(std::memory_order_acquire) || s.id.load(std::memory_order_relaxed) != deviceId) continue;
        uint64_t e[3];
        if (!effective(s, nowMs, e)) continue;
        const bool down = input < kJoyPovBase ? ((e[input >> 6] >> (input & 63)) & 1) != 0
                                              : ((e[2] >> (input - kJoyPovBase)) & 1) != 0;
        if (down) return true;
    }
    return false;
}

void joyWatchSnapshot(JoySnapshot* out, uint64_t nowMs) {
    if (!out) return;
    *out = JoySnapshot();
    for (Slot& s : g_slot) {
        if (!s.device.load(std::memory_order_acquire)) continue;
        uint64_t e[3];
        if (!effective(s, nowMs, e)) continue;
        const uint32_t id = s.id.load(std::memory_order_relaxed);
        // Two units of one model share an id and their buttons merge.
        int at = -1;
        for (int i = 0; i < out->count; ++i) {
            if (out->dev[i].id == id) at = i;
        }
        if (at < 0) {
            if (out->count >= kJoyWatchSlots) continue;
            at = out->count++;
            out->dev[at].id = id;
        }
        for (int i = 0; i < 3; ++i) out->dev[at].bits[i] |= e[i];
    }
}

bool joyInputIn(const JoySnapshot::Device& d, uint16_t input) {
    if (input >= kJoyInputCount) return false;
    if (input < kJoyPovBase) return ((d.bits[input >> 6] >> (input & 63)) & 1) != 0;
    return ((d.bits[2] >> (input - kJoyPovBase)) & 1) != 0;
}

bool joyFirstNew(const JoySnapshot& prev, const JoySnapshot& cur, uint32_t* deviceId, uint16_t* input) {
    for (int i = 0; i < cur.count; ++i) {
        const JoySnapshot::Device& c = cur.dev[i];
        const JoySnapshot::Device* p = nullptr;
        for (int k = 0; k < prev.count; ++k) {
            if (prev.dev[k].id == c.id) p = &prev.dev[k];
        }
        for (uint16_t in = 0; in < kJoyInputCount; ++in) {
            if (joyInputIn(c, in) && !(p && joyInputIn(*p, in))) {
                if (deviceId) *deviceId = c.id;
                if (input) *input = in;
                return true;
            }
        }
    }
    return false;
}

void joyWatchStats(JoyWatchStats* out) {
    if (!out) return;
    *out = JoyWatchStats();
    for (const Slot& s : g_slot) {
        if (s.device.load(std::memory_order_relaxed)) ++out->devices;
    }
    out->stateCalls = g_stateCalls.load(std::memory_order_relaxed);
    out->dataCalls = g_dataCalls.load(std::memory_order_relaxed);
    out->rowsUsed = g_rowsUsed.load(std::memory_order_relaxed);
    out->rowsIgnored = g_rowsIgnored.load(std::memory_order_relaxed);
    out->statesIgnored = g_statesIgnored.load(std::memory_order_relaxed);
    out->tableFull = g_tableFull.load(std::memory_order_relaxed);
    out->lastIgnoredSize = g_lastIgnoredSize.load(std::memory_order_relaxed);
    out->lastIgnoredOfs = g_lastIgnoredOfs.load(std::memory_order_relaxed);
}

void joyWatchReset() {
    for (Slot& s : g_slot) {
        s.device.store(nullptr, std::memory_order_release);
        initSlot(s, 0);
    }
    g_stateCalls = 0;
    g_dataCalls = 0;
    g_rowsUsed = 0;
    g_rowsIgnored = 0;
    g_statesIgnored = 0;
    g_tableFull = 0;
    g_lastIgnoredSize = 0;
    g_lastIgnoredOfs = 0;
}

}  // namespace edvr
