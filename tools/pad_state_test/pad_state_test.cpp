// Test rig for the pad-state probe's pure half (src/d3d11/pad_state_core.h): the identity and prologue decision, change detection (no line when
// nothing changed, one line per change), the exact line texts, the bounded table and its full-table fallback (drop and count), and the line cap.
// TEMPORARY, log only. No DLL and no game.
#include "../../src/d3d11/explorer_cam_core.h"
#include "../../src/d3d11/pad_state_core.h"

#include <cstdio>
#include <cstring>

namespace pad = edvr::padstate;
namespace ecm = edvr::ecm;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// Simulates the tick's print of one object: its codes and reading become the last printed ones.
void printedNow(pad::PadObject& o) {
    o.announced = true;
    for (size_t k = 0; k < pad::kReaders; ++k) {
        o.lineCode[k] = o.code[k];
        o.calledSinceLine[k] = false;
    }
    o.lineReading = o.reading;
    o.callsSinceLine = 0;
}

pad::PadReading reading(uint8_t byte, uint32_t occ, uint8_t o2c, uint8_t o2f) {
    pad::PadReading r;
    r.ok = true;
    r.byte = byte;
    r.occ = occ;
    r.o2c = o2c;
    r.o2f = o2f;
    return r;
}

// An object whose low 24 bits of `this` hash to home slot 0 (the probe's collision test): found by scanning the aligned addresses.
uintptr_t selfWithHomeZero(size_t skip) {
    size_t seen = 0;
    for (uintptr_t a = 0x100000; a < 0x100000 + (1u << 30); a += 16) {
        if (pad::padHome(a) == 0) {
            if (seen == skip) return a;
            ++seen;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: pad_state_test --self-test\n");
        return 2;
    }

    // ---- identity and prologue (the same rule as the store hook: the PE pair first, then the pin) ------------------------------------
    {
        const uint8_t pin[6] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
        uint8_t wrong[6] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x21};
        const uint32_t wantStamp = ecm::kExpectedTimestamp, wantSize = ecm::kExpectedImageSize;
        check(pad::kReaderRva[0] == 0x12C3C20u && pad::kReaderRva[1] == 0x12CAF80u && pad::kReaderRva[2] == 0x12CADE0u &&
                  pad::kReaderRva[3] == 0x12CB320u,
              "the four reader RVAs are the build 332841 readers");
        check(pad::padInstallDecision(true, wantStamp, wantSize, wantStamp, wantSize, pin, pin, 6) == pad::PadInstall::kInstall,
              "positive: build 332841's identity and the prologue pin install the reader");
        check(pad::padInstallDecision(true, wantStamp + 1, wantSize, wantStamp, wantSize, pin, pin, 6) == pad::PadInstall::kWrongIdentity,
              "wrong stamp with the matching pin: refused (the identity comes first)");
        check(pad::padInstallDecision(true, wantStamp, wantSize + 4096, wantStamp, wantSize, pin, pin, 6) == pad::PadInstall::kWrongIdentity,
              "wrong image size with the matching pin: refused");
        check(pad::padInstallDecision(true, wantStamp, wantSize, wantStamp, wantSize, wrong, pin, 6) == pad::PadInstall::kWrongPrologue,
              "right identity with a wrong prologue byte: refused");
        check(pad::padInstallDecision(false, 0, 0, wantStamp, wantSize, pin, pin, 6) == pad::PadInstall::kUnreadable,
              "an unreadable PE header: refused");
        check(pad::padInstallDecision(true, wantStamp, wantSize, wantStamp, wantSize, nullptr, pin, 6) == pad::PadInstall::kUnreadable,
              "an unreadable prologue: refused");
    }

    // ---- change detection: no line when nothing changed, one line per change -----------------------------------------------------------
    {
        pad::PadTable t;
        const uintptr_t self = 0x7FF0000ABC40u;
        // The first calls: reader 0 returns 0 on this object, the reading is the same every call.
        for (int k = 0; k < 5; ++k) pad::padNoteCall(t, 0, self, reading(0, 0, 0, 0), 0, 1000 + static_cast<uint64_t>(k));
        pad::PadObject* o = nullptr;
        for (size_t i = 0; i < pad::kObjects; ++i) {
            if (t.slots[i].used && t.slots[i].self == self) o = &t.slots[i];
        }
        check(o != nullptr && o->callsSinceLine == 5 && !pad::padChanged(*o), "an object that was never printed is not a change (it is first seen)");
        printedNow(*o);
        for (int k = 0; k < 20; ++k) pad::padNoteCall(t, 0, self, reading(0, 0, 0, 0), 0, 2000 + static_cast<uint64_t>(k));
        check(!pad::padChanged(*o), "no change: twenty identical calls after the print write no line");
        pad::padNoteCall(t, 1, self, reading(0, 0, 0, 0), 2, 3000);
        check(pad::padChanged(*o), "reader 1 returning 2 is one change");
        printedNow(*o);
        check(!pad::padChanged(*o), "after that line is printed the same tuple is no change: exactly one line per change");
        pad::padNoteCall(t, 2, self, reading(2, 0, 0, 0), 0, 3100);
        check(pad::padChanged(*o), "a state byte change (0 to 2) is a change");
        printedNow(*o);
        pad::padNoteCall(t, 2, self, reading(2, 5, 0, 0), 0, 3200);
        check(pad::padChanged(*o), "an occupancy change (0 to 5) is a change");
        printedNow(*o);
        pad::padNoteCall(t, 3, self, reading(2, 5, 0, 1), 0, 3300);
        check(pad::padChanged(*o), "an owner byte +0x2F change is a change");
        printedNow(*o);
        pad::padNoteCall(t, 0, self, pad::PadReading{}, 0, 3400);   // a faulted read: ok false
        check(pad::padChanged(*o), "a reading that faults (ok false) against a good one is a change");
    }

    // ---- the exact line texts ------------------------------------------------------------------------------------------------------------
    {
        pad::PadTable t;
        const uintptr_t self = 0x7FF0000ABC40u;   // low six hex digits: 0abc40
        pad::padNoteCall(t, 0, self, reading(0, 0, 0, 0), 0, 1000);
        pad::PadObject* o = nullptr;
        for (size_t i = 0; i < pad::kObjects; ++i) {
            if (t.slots[i].used && t.slots[i].self == self) o = &t.slots[i];
        }
        printedNow(*o);   // the last printed: r0=0, byte 0, occ 0
        pad::padNoteCall(t, 0, self, reading(2, 5, 0, 1), 1, 2000);
        pad::padNoteCall(t, 0, self, reading(2, 5, 0, 1), 1, 2500);
        pad::padNoteCall(t, 0, self, reading(2, 5, 0, 1), 1, 3000);
        pad::padNoteCall(t, 0, self, reading(2, 5, 0, 1), 1, 3500);
        char line[512] = {};
        pad::formatPadChange(line, sizeof(line), *o, 2.0);
        std::printf("line: %s\n", line);
        check(std::strcmp(line,
                          "pad state: obj @0abc40 r0=1 r1=- r2=- r3=- byte=2 occ=5 o2c=0 o2f=1 calls/s=2.0 (was r0=0 byte=0 occ=0)") == 0,
              "the change line text: the codes (- for a reader not called since the last line), the reading, calls/s, and the 'was' values");
        char ev[2048] = {};
        pad::formatPadEvent(ev, sizeof(ev), "seal", t, 0x7FF0000ABC40u);
        check(std::strstr(ev, "pad state at seal: own pad obj @0abc40 r0=1 r1=- r2=- r3=- byte=2 occ=5 o2c=0 o2f=1") != nullptr,
              "the seal line lists the object with occ>0 and its tuple");
        pad::PadTable empty;
        char none[2048] = {};
        pad::formatPadEvent(none, sizeof(none), "release", empty, 0);
        check(std::strcmp(none, "pad state at release: own pad unknown (no r0 on this dock)") == 0, "a release with no occupied object says so");
        char sum[512] = {};
        pad::formatPadSummary(sum, sizeof(sum), t, 30.0);
        check(std::strstr(sum, "objects seen 1") != nullptr && std::strstr(sum, "objects with occ>0 1") != nullptr,
              "the 30 s summary counts the objects seen and the objects with occ>0");
    }

    // ---- the bounded table: at the bound, the full-table fallback drops and counts, and the stored objects still answer ------------------------
    {
        pad::PadTable t;
        // 32 objects whose home slot is 0: they fill probes 0..31; the 33rd cannot be stored within the bound.
        uintptr_t selves[33] = {};
        for (size_t k = 0; k < 33; ++k) selves[k] = selfWithHomeZero(k);
        bool allFirst32 = true;
        for (size_t k = 0; k < 32; ++k) {
            if (pad::padNoteCall(t, 0, selves[k], reading(0, 0, 0, 0), 0, 1) == false) allFirst32 = false;
        }
        check(allFirst32 && t.used == 32 && t.maxProbes == 32, "32 objects sharing a home slot fill the 32-probe run; the longest probe is 32");
        const bool stored33 = pad::padNoteCall(t, 0, selves[32], reading(0, 0, 0, 0), 0, 2);
        check(!stored33 && t.dropped == 1 && t.used == 32, "the 33rd object at the bound is dropped and counted: no walk of the table, nothing stored");
        check(t.readerCalls[0] == 33, "the dropped call is still counted in the reader's calls (the summary's rate)");
        bool stillFound = true;
        for (size_t k = 0; k < 32; ++k) {
            bool hit = false;
            for (size_t i = 0; i < pad::kObjects; ++i) {
                if (t.slots[i].used && t.slots[i].self == selves[k]) hit = true;
            }
            stillFound = stillFound && hit;
        }
        check(stillFound, "the stored objects still answer after a drop");
        // Beyond the table: a second run of objects with another home is stored in free slots (the table is not full), then the table is full
        // everywhere and a new object is dropped rather than walked.
        pad::PadTable full;
        size_t stored = 0;
        for (size_t k = 0; k < 4 * pad::kObjects; ++k) {
            const uintptr_t s = 0x7FF0000000u + static_cast<uintptr_t>(k) * 16;
            if (pad::padNoteCall(full, 0, s, reading(0, 0, 0, 0), 0, 1)) ++stored;
        }
        check(stored <= pad::kObjects && full.used == stored && full.dropped == 4 * pad::kObjects - stored,
              "more objects than the table holds: at most 64 are stored, the rest are dropped and counted");
        check(full.maxProbes <= edvr::sealed::kTableProbeMax, "no probe walks past the 32-probe bound");
    }

    // ---- the line cap: 200 change lines, one notice, then counted only ---------------------------------------------------------------------
    {
        pad::PadTable t;
        size_t written = 0;
        size_t notices = 0;
        for (size_t k = 0; k < 2100; ++k) {
            const pad::ChangeLine v = pad::padAllowChangeLine(t);
            if (v == pad::ChangeLine::kWrite) ++written;
            if (v == pad::ChangeLine::kSuppressedFirst) ++notices;
        }
        check(written == pad::kChangeLineCap && t.changeLines == pad::kChangeLineCap, "exactly kChangeLineCap (200) change lines are written in a session");
        check(notices == 1 && t.capNoticed, "the cap is announced once");
        check(t.suppressed == 2100 - pad::kChangeLineCap && t.suppressedTotal == 2100 - pad::kChangeLineCap, "the lines past the cap are counted, not logged");
    }

    std::printf("pad_state_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
