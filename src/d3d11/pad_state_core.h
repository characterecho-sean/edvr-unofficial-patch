#pragma once
// The pad-state probe's pure half (pad_state_probe.cpp). TEMPORARY (2026-10-13): it reads the docking state Elite's pad objects report, through
// the four docking-state readers (virtuals at RVAs 0x12C3C20, 0x12CAF80, 0x12CADE0, 0x12CB320, build 332841). Each reader takes its object in rcx.
// The probe records, per object, the code each reader returned, the state byte at this+0x3C8, and the owner's occupancy and two bytes (owner =
// [this-8]: dword at owner+0x10, bytes at owner+0x2C and +0x2F). No Windows, no engine memory: this header decides and formats only.
//
// LOG ONLY. Nothing here changes what the station does; the seal logic does not read this table.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "optimized_station_core.h"

namespace edvr::padstate {

constexpr size_t kReaders = 4;
constexpr size_t kObjects = 64;
constexpr uint32_t kChangeLineCap = 200;   // built v14.31 (was 2000): the own pad only, and few
constexpr uintptr_t kReaderRva[kReaders] = {0x12C3C20u, 0x12CAF80u, 0x12CADE0u, 0x12CB320u};
// push rbx; sub rsp,20h: the same six bytes in all four readers (build 332841).
constexpr uint8_t kReaderPin[6] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};

// ---- identity and prologue gate (the same rule as the store hook: the PE pair first, then the pin) ------------------------------------------
enum class PadInstall : uint8_t { kInstall = 0, kUnreadable, kWrongIdentity, kWrongPrologue };

inline bool padPinMatches(const uint8_t* got, const uint8_t* want, size_t n) noexcept {
    return got != nullptr && want != nullptr && std::memcmp(got, want, n) == 0;
}

// Identity first (a PE header that cannot be read, or another build's stamp or size, refuses whatever the prologue is), then the pin.
inline PadInstall padInstallDecision(bool identityRead, uint32_t stamp, uint32_t size, uint32_t wantStamp, uint32_t wantSize, const uint8_t* got,
                                     const uint8_t* pin, size_t n) noexcept {
    if (!identityRead) return PadInstall::kUnreadable;
    if (stamp != wantStamp || size != wantSize) return PadInstall::kWrongIdentity;
    if (got == nullptr || pin == nullptr) return PadInstall::kUnreadable;
    if (!padPinMatches(got, pin, n)) return PadInstall::kWrongPrologue;
    return PadInstall::kInstall;
}

// ---- what a detour records --------------------------------------------------------------------------------------------------------------------
// One read of the object's state (plain reads, taken under __try in the detour). ok false: the read faulted, and the fields are not valid.
struct PadReading {
    bool ok = false;
    uint8_t byte = 0;   // this+0x3C8
    uint32_t occ = 0;   // owner+0x10 (the occupancy dword)
    uint8_t o2c = 0;    // owner+0x2C
    uint8_t o2f = 0;    // owner+0x2F
};

inline bool sameReading(const PadReading& a, const PadReading& b) noexcept {
    return a.ok == b.ok && a.byte == b.byte && a.occ == b.occ && a.o2c == b.o2c && a.o2f == b.o2f;
}

// One object (keyed by `this`): its readers' last codes, the latest reading, and the tuple last printed (for change detection).
struct PadObject {
    uintptr_t self = 0;
    bool used = false;
    int code[kReaders] = {-1, -1, -1, -1};          // the last code each reader returned for this object (-1: never called)
    bool calledSinceLine[kReaders] = {};            // a reader was called on this object since the last printed line
    uint32_t callsSinceLine = 0;                     // all readers together, since the last printed line
    uint64_t lastQpc = 0;                            // the QPC time of the last call (any reader)
    uint64_t readerQpc[kReaders] = {};               // the QPC time of each reader's last call on this object (0: never)
    PadReading reading;                              // the latest read
    bool announced = false;                          // the first-seen line has been printed
    int lineCode[kReaders] = {-1, -1, -1, -1};      // the codes as last printed
    PadReading lineReading;                          // the reading as last printed
    uint64_t lineQpc = 0;                            // the QPC time of the last printed line (the calls/s base)
};

// The table and the session's counters. Guarded by the caller (the detour and the tick share it under a spinlock).
struct PadTable {
    PadObject slots[kObjects];
    uint32_t used = 0;
    uint32_t maxProbes = 0;          // the longest probe this window
    uint64_t dropped = 0;            // calls for objects that did not fit (counted, not stored)
    uint64_t readerCalls[kReaders] = {};   // calls per reader this window (the 30 s summary)
    uint32_t changeLines = 0;        // change lines written this session
    uint64_t suppressed = 0;         // change lines not written (the cap), this window
    uint64_t suppressedTotal = 0;    // ...and this session
    bool capNoticed = false;         // the cap notice has been written
};

inline size_t padHome(uintptr_t self) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(self) >> 4) * 0x9E3779B97F4A7C15ull >> 58) & (kObjects - 1);
}

// The object for `self`: the existing one, or a free slot claimed for it. Null when the bounded probe finds neither (dropped, counted).
inline PadObject* padClaim(PadTable& t, uintptr_t self) noexcept {
    const sealed::ProbeResult p = sealed::boundedProbe(
        padHome(self), kObjects, sealed::kTableProbeMax,
        [&t, self](size_t i) { return t.slots[i].used && t.slots[i].self == self; },
        [&t](size_t i) { return !t.slots[i].used; });
    if (p.probes > t.maxProbes) t.maxProbes = p.probes;
    if (p.found) return &t.slots[p.index];
    if (p.empty) {
        PadObject& o = t.slots[p.index];
        o = PadObject{};
        o.self = self;
        o.used = true;
        ++t.used;
        return &o;
    }
    ++t.dropped;
    return nullptr;
}

// One detour's call: the reader's code, the object's reading and the QPC time. Returns false when the object did not fit (the call is counted).
inline bool padNoteCall(PadTable& t, size_t reader, uintptr_t self, const PadReading& r, int code, uint64_t qpc) noexcept {
    if (reader < kReaders) ++t.readerCalls[reader];
    PadObject* o = padClaim(t, self);
    if (o == nullptr || reader >= kReaders) return o != nullptr;
    o->code[reader] = code;
    o->readerQpc[reader] = qpc;
    o->calledSinceLine[reader] = true;
    ++o->callsSinceLine;
    o->reading = r;
    o->lastQpc = qpc;
    return true;
}

// ---- change detection and the cap ---------------------------------------------------------------------------------------------------------------
// A change: the object was printed before, and its reading or one reader's last code differs from what was printed.
inline bool padChanged(const PadObject& o) noexcept {
    if (!o.announced) return false;
    if (!sameReading(o.reading, o.lineReading)) return true;
    for (size_t i = 0; i < kReaders; ++i) {
        if (o.code[i] != o.lineCode[i]) return true;
    }
    return false;
}

enum class ChangeLine : uint8_t { kWrite = 0, kSuppressed, kSuppressedFirst };

// Counts a change line against the session cap. kWrite: write it. kSuppressedFirst: the cap was just reached (write one notice, once).
inline ChangeLine padAllowChangeLine(PadTable& t) noexcept {
    if (t.changeLines < kChangeLineCap) {
        ++t.changeLines;
        return ChangeLine::kWrite;
    }
    ++t.suppressed;
    ++t.suppressedTotal;
    if (!t.capNoticed) {
        t.capNoticed = true;
        return ChangeLine::kSuppressedFirst;
    }
    return ChangeLine::kSuppressed;
}

// ---- formatting (the exact line texts) ------------------------------------------------------------------------------------------------------------
// The object's short key: the low six hex digits of `this`.
inline unsigned long long padKey(uintptr_t self) noexcept {
    return static_cast<unsigned long long>(static_cast<uint64_t>(self) & 0xFFFFFFull);
}

// One code: the number, or "-" when the reader was not called (or never returned).
inline void padCodeText(char* out, size_t cap, int code, bool show) noexcept {
    if (!show || code < 0) std::snprintf(out, cap, "-");
    else std::snprintf(out, cap, "%d", code);
}

inline void padValueText(char* out, size_t cap, bool ok, unsigned value) noexcept {
    if (!ok) std::snprintf(out, cap, "-");
    else std::snprintf(out, cap, "%u", value);
}

// The change line:
// pad state: obj @<low 6 hex> r0=<code|-> r1=.. r2=.. r3=.. byte=<n> occ=<n> o2c=<n> o2f=<n> calls/s=<n> (was r0=.. byte=.. occ=..)
inline void formatPadChange(char* out, size_t cap, const PadObject& o, double elapsedS) noexcept {
    char r[kReaders][12];
    for (size_t i = 0; i < kReaders; ++i) padCodeText(r[i], sizeof(r[i]), o.code[i], o.calledSinceLine[i]);
    char byteT[12], occT[16], o2cT[12], o2fT[12], wasR0[12], wasByte[12], wasOcc[16];
    padValueText(byteT, sizeof(byteT), o.reading.ok, o.reading.byte);
    padValueText(occT, sizeof(occT), o.reading.ok, o.reading.occ);
    padValueText(o2cT, sizeof(o2cT), o.reading.ok, o.reading.o2c);
    padValueText(o2fT, sizeof(o2fT), o.reading.ok, o.reading.o2f);
    padCodeText(wasR0, sizeof(wasR0), o.lineCode[0], o.lineCode[0] >= 0);
    padValueText(wasByte, sizeof(wasByte), o.lineReading.ok, o.lineReading.byte);
    padValueText(wasOcc, sizeof(wasOcc), o.lineReading.ok, o.lineReading.occ);
    const double rate = elapsedS > 0.0 ? static_cast<double>(o.callsSinceLine) / elapsedS : 0.0;
    std::snprintf(out, cap,
                  "pad state: obj @%06llx r0=%s r1=%s r2=%s r3=%s byte=%s occ=%s o2c=%s o2f=%s calls/s=%.1f (was r0=%s byte=%s occ=%s)",
                  padKey(o.self), r[0], r[1], r[2], r[3], byteT, occT, o2cT, o2fT, rate, wasR0, wasByte, wasOcc);
}

// The seal or release line, for the OWN pad only (built v14.23): the own object's tuple, or "own pad unknown" when the game has not named one this dock.
// `cap` must hold the line: the caller passes at least 256 bytes.
inline void formatPadEvent(char* out, size_t cap, const char* what, const PadTable& t, uintptr_t own) noexcept {
    const PadObject* o = nullptr;
    if (own != 0) {
        for (size_t i = 0; i < kObjects; ++i) {
            if (t.slots[i].used && t.slots[i].self == own) o = &t.slots[i];
        }
    }
    if (o == nullptr || !o->reading.ok) {
        std::snprintf(out, cap, "pad state at %s: own pad unknown (no r0 on this dock)", what);
        return;
    }
    char r[kReaders][12];
    for (size_t k = 0; k < kReaders; ++k) padCodeText(r[k], sizeof(r[k]), o->code[k], o->code[k] >= 0);
    std::snprintf(out, cap, "pad state at %s: own pad obj @%06llx r0=%s r1=%s r2=%s r3=%s byte=%u occ=%u o2c=%u o2f=%u", what, padKey(o->self), r[0],
                  r[1], r[2], r[3], static_cast<unsigned>(o->reading.byte), o->reading.occ, static_cast<unsigned>(o->reading.o2c),
                  static_cast<unsigned>(o->reading.o2f));
}
// The 30 s summary: objects seen, objects with occ>0, calls per second of each reader, the lines suppressed, and the table's state.
inline void formatPadSummary(char* out, size_t cap, const PadTable& t, double windowS) noexcept {
    uint32_t withOcc = 0;
    for (size_t i = 0; i < kObjects; ++i) {
        if (t.slots[i].used && t.slots[i].reading.ok && t.slots[i].reading.occ > 0) ++withOcc;
    }
    const double s = windowS > 0.0 ? windowS : 1.0;
    std::snprintf(out, cap,
                  "pad state: last %.0f s: objects seen %u (of %u), objects with occ>0 %u, calls/s r0=%.1f r1=%.1f r2=%.1f r3=%.1f, "
                  "change lines suppressed %llu, longest probe %u, dropped %llu.",
                  windowS, t.used, static_cast<unsigned>(kObjects), withOcc, static_cast<double>(t.readerCalls[0]) / s,
                  static_cast<double>(t.readerCalls[1]) / s, static_cast<double>(t.readerCalls[2]) / s,
                  static_cast<double>(t.readerCalls[3]) / s, static_cast<unsigned long long>(t.suppressed), t.maxProbes,
                  static_cast<unsigned long long>(t.dropped));
}

}  // namespace edvr::padstate
