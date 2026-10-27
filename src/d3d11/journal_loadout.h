// F140 (native on-foot VR): the current suit loadout's three weapon slots from the journal, and the scope state of the held weapon. Header only, no game or D3D11 state
// and no windows.h, so native_frame_test runs the very parser the journal worker uses. The file reading is journal_loadout_reader.h.
//
// A weapon can be fitted with a scope modification (the journal's WeaponMods holds "weapon_scope"), which moves its aiming point and maybe its readout. Status.json names
// the held weapon but not its mods, so the loadout comes from the journal events that carry them:
//   SuitLoadout, SwitchSuitLoadout   all three slots: "Modules":[{"SlotName":"PrimaryWeapon1"|"PrimaryWeapon2"|"SecondaryWeapon","ModuleName":"wpn_m_sniper_plasma_charged",
//                                    ...,"WeaponMods":["weapon_range","weapon_scope",...]},...]; replaces the loadout
//   LoadoutEquipModule               one slot: "SlotName", "ModuleName", "WeaponMods"
//   LoadoutRemoveModule              one slot emptied: "SlotName", "ModuleName" (the journal's manual lists it; none was found in the pilot's 2745 journals)
// A LoadoutEquipModule or LoadoutRemoveModule that names another loadout than the current one (LoadoutID) is another loadout being edited, and is ignored.
// The journal's ModuleName "wpn_m_sniper_plasma_charged" is Status.json's "$wpn_m_sniper_plasma_charged_name;": the "$" and the "_name;" taken off, compared without case.
//
// The held weapon's scope state is yes or no when exactly one slot holds its name, or every slot holding it agrees; unknown when no loadout was read yet, when the name is in
// no slot, or when two slots hold it with different mods (the native primary selection carries an item id with no slot, so it cannot settle that).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace loadout {

enum Scope : int { kScopeUnknown = 0, kScopeNo = 1, kScopeYes = 2 };
inline const char* scopeName(int s) noexcept { return s == kScopeYes ? "yes" : s == kScopeNo ? "no" : "unknown"; }
// Why a held weapon's scope state is what it is.
enum Why : int { kWhyOk = 0, kWhyNoLoadout = 1, kWhyNotInSlots = 2, kWhyAmbiguous = 3, kWhyNoName = 4, kWhyCount = 5 };
inline const char* whyName(int w) noexcept {
    switch (w) {
        case kWhyOk: return "a slot holds it";
        case kWhyNoLoadout: return "no loadout read yet";
        case kWhyNotInSlots: return "the name is in no slot";
        case kWhyAmbiguous: return "two slots hold it with different mods";
        case kWhyNoName: return "no item name";
        default: return "?";
    }
}

constexpr int kSlotCount = 3;
constexpr const char* kSlotNames[kSlotCount] = {"PrimaryWeapon1", "PrimaryWeapon2", "SecondaryWeapon"};
constexpr size_t kModuleCap = 64, kModsCap = 112, kLoadoutNameCap = 32;
constexpr size_t kMaxLine = 16384;   // a SuitLoadout line is about 1 KB; a longer line is dropped
constexpr char kScopeMod[] = "weapon_scope";

struct Slot {
    bool have = false, scope = false;
    char module[kModuleCap] = {};   // as the journal writes it, lower case
    char mods[kModsCap] = {};       // the WeaponMods, comma separated, cut at the cap
    bool sameAs(const Slot& o) const noexcept {
        return have == o.have && scope == o.scope && std::strcmp(module, o.module) == 0 && std::strcmp(mods, o.mods) == 0;
    }
};
struct Loadout {
    bool known = false;                  // a loadout event was applied
    bool haveId = false;
    uint64_t id = 0;
    char name[kLoadoutNameCap] = {};
    Slot slot[kSlotCount];
    uint32_t generation = 0;             // +1 at every change
    int source = 0;                      // the last event that changed it (Event)
};

enum Event : int { kEvNone = 0, kEvSuitLoadout = 1, kEvSwitch = 2, kEvEquip = 3, kEvRemove = 4 };
inline const char* eventName(int e) noexcept {
    return e == kEvSuitLoadout ? "SuitLoadout" : e == kEvSwitch ? "SwitchSuitLoadout" : e == kEvEquip ? "LoadoutEquipModule" : e == kEvRemove ? "LoadoutRemoveModule" : "none";
}

// ---- the held weapon -----------------------------------------------------------------------------------------------------------------------------------------------
inline char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
// Status.json's "$wpn_m_sniper_plasma_charged_name;" against the journal's "wpn_m_sniper_plasma_charged".
inline bool statusNameMatches(const char* status, const char* module) noexcept {
    if (!status || !module || status[0] != '$' || !module[0]) return false;
    const size_t n = std::strlen(status);
    static const char kSuffix[] = "_name;";
    constexpr size_t kSuffixLen = sizeof(kSuffix) - 1;
    if (n < 1 + kSuffixLen + 1) return false;
    for (size_t i = 0; i < kSuffixLen; ++i)
        if (lowerAscii(status[n - kSuffixLen + i]) != kSuffix[i]) return false;
    const size_t mid = n - 1 - kSuffixLen;
    if (std::strlen(module) != mid) return false;
    for (size_t i = 0; i < mid; ++i)
        if (lowerAscii(status[1 + i]) != lowerAscii(module[i])) return false;
    return true;
}
struct HeldScope {
    int state = kScopeUnknown;
    int why = kWhyNoName;
    int slots = 0;   // slots whose module is the held item's
};
inline HeldScope scopeOfHeld(const Loadout& l, const char* statusName) noexcept {
    HeldScope h;
    if (!statusName || !statusName[0]) return h;
    if (!l.known) {
        h.why = kWhyNoLoadout;
        return h;
    }
    int yes = 0, no = 0;
    for (const Slot& s : l.slot) {
        if (!s.have || !statusNameMatches(statusName, s.module)) continue;
        ++h.slots;
        (s.scope ? yes : no) += 1;
    }
    if (!h.slots) {
        h.why = kWhyNotInSlots;
    } else if (yes && no) {
        h.why = kWhyAmbiguous;
    } else {
        h.state = yes ? kScopeYes : kScopeNo;
        h.why = kWhyOk;
    }
    return h;
}

// ---- the parser: one journal line at a time ----------------------------------------------------------------------------------------------------------------------
namespace detail {
inline const char* skipWs(const char* p, const char* e) noexcept {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    return p;
}
// The value after `"key":` (whitespace allowed around the colon), searched in [s, e); nullptr when the key is not there. A string value that happens to read like the key is
// not followed by a colon, so it is skipped.
inline const char* findKey(const char* s, const char* e, const char* key) noexcept {
    const size_t kl = std::strlen(key);
    for (const char* p = s; p + kl + 2 <= e; ++p) {
        if (*p != '"' || std::memcmp(p + 1, key, kl) != 0 || p[1 + kl] != '"') continue;
        const char* q = skipWs(p + kl + 2, e);
        if (q >= e || *q != ':') continue;
        q = skipWs(q + 1, e);
        return q < e ? q : nullptr;
    }
    return nullptr;
}
// A string at p (`"..."`) into out (cut at cap, lower-cased when asked); the pointer after the closing quote, nullptr when it is not a string or does not close.
inline const char* readString(const char* p, const char* e, char* out, size_t cap, bool lower) noexcept {
    if (!p || p >= e || *p != '"') return nullptr;
    ++p;
    size_t n = 0;
    while (p < e && *p != '"') {
        char c = *p;
        if (c == '\\' && p + 1 < e) c = *++p;
        if (n + 1 < cap) out[n++] = lower ? lowerAscii(c) : c;
        ++p;
    }
    if (p >= e) return nullptr;
    if (cap) out[n < cap ? n : cap - 1] = '\0';
    return p + 1;
}
inline bool readU64(const char* p, const char* e, uint64_t* out) noexcept {
    if (!p || p >= e || *p < '0' || *p > '9') return false;
    uint64_t v = 0;
    for (; p < e && *p >= '0' && *p <= '9'; ++p) v = v * 10u + static_cast<uint64_t>(*p - '0');
    *out = v;
    return true;
}
// WeaponMods at p (`[ "a", "b" ]`) into the slot: the comma list, and whether weapon_scope is among them.
inline void readMods(const char* p, const char* e, Slot& s) noexcept {
    s.scope = false;
    s.mods[0] = '\0';
    p = skipWs(p, e);
    if (p >= e || *p != '[') return;
    ++p;
    size_t n = 0;
    char one[48];
    for (;;) {
        p = skipWs(p, e);
        if (p >= e || *p == ']') break;
        if (*p == ',') {
            ++p;
            continue;
        }
        const char* q = readString(p, e, one, sizeof(one), true);
        if (!q) break;
        if (std::strcmp(one, kScopeMod) == 0) s.scope = true;
        const size_t l = std::strlen(one);
        if (n + (n ? 1u : 0u) + l + 1 <= sizeof(s.mods)) {
            if (n) s.mods[n++] = ',';
            std::memcpy(s.mods + n, one, l);
            n += l;
            s.mods[n] = '\0';
        }
        p = q;
    }
}
inline int slotIndex(const char* name) noexcept {
    for (int i = 0; i < kSlotCount; ++i)
        if (std::strcmp(name, kSlotNames[i]) == 0) return i;
    return -1;
}
// One module object [s, e): its slot index and contents. False when it names no weapon slot or no module.
inline bool readSlotObject(const char* s, const char* e, int* index, Slot* out) noexcept {
    char slotName[24];
    if (!readString(findKey(s, e, "SlotName"), e, slotName, sizeof(slotName), false)) return false;
    const int idx = slotIndex(slotName);
    if (idx < 0) return false;
    Slot v;
    if (!readString(findKey(s, e, "ModuleName"), e, v.module, sizeof(v.module), true) || !v.module[0]) return false;
    v.have = true;
    if (const char* mods = findKey(s, e, "WeaponMods")) readMods(mods, e, v);
    *index = idx;
    *out = v;
    return true;
}
// The `{...}` objects of an array at p (`[ {..}, {..} ]`), flat (the modules hold no objects): fn(objectStart, objectEnd) for each.
template <class F>
inline void forEachObject(const char* p, const char* e, F&& fn) noexcept {
    p = skipWs(p, e);
    if (p >= e || *p != '[') return;
    ++p;
    while (p < e) {
        while (p < e && *p != '{' && *p != ']') ++p;
        if (p >= e || *p == ']') return;
        const char* os = p;
        bool inString = false;
        for (; p < e; ++p) {
            if (inString) {
                if (*p == '\\' && p + 1 < e) ++p;
                else if (*p == '"') inString = false;
            } else if (*p == '"') {
                inString = true;
            } else if (*p == '}') {
                break;
            }
        }
        if (p >= e) return;
        fn(os, p + 1);
        ++p;
    }
}
}  // namespace detail

struct LineResult {
    int event = kEvNone;
    bool changed = false;
    bool otherLoadout = false;   // an equip or remove for a loadout that is not the current one: ignored
};
inline bool sameSlots(const Loadout& a, const Loadout& b) noexcept {
    for (int i = 0; i < kSlotCount; ++i)
        if (!a.slot[i].sameAs(b.slot[i])) return false;
    return true;
}
// One journal line (no newline). Only the four loadout events do anything.
inline LineResult applyLine(Loadout& l, const char* line, size_t len) noexcept {
    LineResult r;
    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n')) --len;
    const char* e = line + len;
    char ev[32];
    if (!detail::readString(detail::findKey(line, line + (len < 200 ? len : 200), "event"), e, ev, sizeof(ev), false)) return r;
    if (std::strcmp(ev, "SuitLoadout") == 0) r.event = kEvSuitLoadout;
    else if (std::strcmp(ev, "SwitchSuitLoadout") == 0) r.event = kEvSwitch;
    else if (std::strcmp(ev, "LoadoutEquipModule") == 0) r.event = kEvEquip;
    else if (std::strcmp(ev, "LoadoutRemoveModule") == 0) r.event = kEvRemove;
    else return r;
    uint64_t id = 0;
    const bool haveId = detail::readU64(detail::findKey(line, e, "LoadoutID"), e, &id);
    if (r.event == kEvSuitLoadout || r.event == kEvSwitch) {
        Loadout n;
        n.known = true;
        n.haveId = haveId;
        n.id = haveId ? id : 0;
        detail::readString(detail::findKey(line, e, "LoadoutName"), e, n.name, sizeof(n.name), false);
        if (const char* mods = detail::findKey(line, e, "Modules")) {
            detail::forEachObject(mods, e, [&](const char* os, const char* oe) {
                int idx = -1;
                Slot s;
                if (detail::readSlotObject(os, oe, &idx, &s)) n.slot[idx] = s;
            });
        }
        const bool changed = !l.known || l.haveId != n.haveId || l.id != n.id || std::strcmp(l.name, n.name) != 0 || !sameSlots(l, n);
        if (changed) {
            n.generation = l.generation + 1u;
            n.source = r.event;
            l = n;
        }
        r.changed = changed;
        return r;
    }
    // one slot
    if (l.haveId && haveId && l.id != id) {
        r.otherLoadout = true;
        return r;
    }
    int idx = -1;
    Slot s;
    if (r.event == kEvEquip) {
        if (!detail::readSlotObject(line, e, &idx, &s)) return r;
    } else {
        char slotName[24];
        if (!detail::readString(detail::findKey(line, e, "SlotName"), e, slotName, sizeof(slotName), false)) return r;
        idx = detail::slotIndex(slotName);
        if (idx < 0) return r;
        char module[kModuleCap] = {};
        detail::readString(detail::findKey(line, e, "ModuleName"), e, module, sizeof(module), true);
        if (module[0] && l.slot[idx].have && std::strcmp(module, l.slot[idx].module) != 0) return r;   // not what is in the slot: a stale event
        s = Slot{};
    }
    const bool changed = !l.known || !l.slot[idx].sameAs(s);
    if (changed) {
        l.known = true;
        if (!l.haveId && haveId) {
            l.haveId = true;
            l.id = id;
        }
        l.slot[idx] = s;
        ++l.generation;
        l.source = r.event;
    }
    r.changed = changed;
    return r;
}

// The journal split into lines across read chunks: a line wholly inside a chunk is passed without a copy, one that spans chunks is assembled (up to kMaxLine; a longer line is
// dropped to its newline). onLine(const char*, size_t) is called for each complete line.
struct LineFeeder {
    char buf[kMaxLine];
    size_t n = 0;
    bool dropping = false;
    void reset() noexcept {
        n = 0;
        dropping = false;
    }
    template <class OnLine>
    void feed(const char* d, size_t len, OnLine&& onLine) {
        size_t i = 0;
        while (i < len) {
            const char* nl = static_cast<const char*>(std::memchr(d + i, '\n', len - i));
            const size_t seg = nl ? static_cast<size_t>(nl - (d + i)) : len - i;
            if (n == 0 && !dropping && nl) {
                if (seg) onLine(d + i, seg);
            } else {
                if (!dropping) {
                    if (n + seg <= kMaxLine) {
                        std::memcpy(buf + n, d + i, seg);
                        n += seg;
                    } else {
                        dropping = true;
                        n = 0;
                    }
                }
                if (nl) {
                    if (!dropping && n) onLine(buf, n);
                    n = 0;
                    dropping = false;
                }
            }
            i += seg + (nl ? 1u : 0u);
        }
    }
};

// The loadout line for the log: "SuitLoadout, loadout "Combat" (id 4293000002): PrimaryWeapon1 wpn_m_x scope no [mods]; ...".
inline void describe(const Loadout& l, char* buf, size_t cap) noexcept {
    if (!buf || !cap) return;
    size_t n = 0;
    buf[0] = '\0';
    const int h = std::snprintf(buf, cap, "%s, loadout \"%s\" (id %llu): ", eventName(l.source), l.name, static_cast<unsigned long long>(l.haveId ? l.id : 0));
    if (h < 0 || static_cast<size_t>(h) >= cap) return;
    n = static_cast<size_t>(h);
    for (int i = 0; i < kSlotCount; ++i) {
        const Slot& s = l.slot[i];
        const int w = s.have ? std::snprintf(buf + n, cap - n, "%s%s %s scope %s [%s]", i ? "; " : "", kSlotNames[i], s.module, s.scope ? "yes" : "no", s.mods)
                             : std::snprintf(buf + n, cap - n, "%s%s empty", i ? "; " : "", kSlotNames[i]);
        if (w < 0 || static_cast<size_t>(w) >= cap - n) return;
        n += static_cast<size_t>(w);
    }
}

// ---- the publication: one writer (the journal worker), any reader --------------------------------------------------------------------------------------------------
struct Publication {
    std::atomic<uint32_t> seq{0};
    Loadout value;
    void publish(const Loadout& l) noexcept {
        seq.fetch_add(1, std::memory_order_acq_rel);
        value = l;
        seq.fetch_add(1, std::memory_order_release);
    }
    // False when nothing was published yet (or a write kept landing under the read).
    bool read(Loadout* out) const noexcept {
        for (int tries = 0; tries < 3; ++tries) {
            const uint32_t s1 = seq.load(std::memory_order_acquire);
            if (s1 == 0 || (s1 & 1u)) continue;
            std::memcpy(out, &value, sizeof(Loadout));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == s1) return true;
        }
        return false;
    }
};
inline Publication g_published;
// The published loadout's generation (0 while none was published): a log window closes when it moves.
inline uint32_t publishedGeneration() noexcept {
    Loadout l;
    return g_published.read(&l) ? l.generation : 0u;
}
// The held weapon's scope state from the published loadout (unknown while none was published); generation says which loadout it was read from.
inline HeldScope heldScope(const char* statusName, uint32_t* generation = nullptr) noexcept {
    Loadout l;
    if (!g_published.read(&l)) {
        HeldScope h;
        h.why = (statusName && statusName[0]) ? kWhyNoLoadout : kWhyNoName;
        if (generation) *generation = 0;
        return h;
    }
    if (generation) *generation = l.generation;
    return scopeOfHeld(l, statusName);
}

}  // namespace loadout
}  // namespace edvr
