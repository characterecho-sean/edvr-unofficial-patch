// The census of unkeyed pairs, the table alone (src\d3d11\engine_velocity_unkeyed.h):
// what it counts, how it names the busiest four, what overflow means, that the line
// is written when the window is empty, that a window ends where it is taken, and
// that the text fits a log line at its worst. The draw path's own feeding of it --
// the unkeyed pair named, a keyed pair never -- is lifecycle_tests.h (C1).
#pragma once
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/d3d11/engine_velocity_unkeyed.h"

namespace unkeyed_tests {
struct Context { void (*check)(bool, const char*); };

inline void run(const Context& c) {
    using edvr::engine_velocity_unkeyed::Table;
    using edvr::engine_velocity_unkeyed::kCapacity;
    static_assert(kCapacity == 4, "the census names four pairs, the rest are counted");
    constexpr uint64_t vsA = 0x66DE2CADB1F4AE6Bull, vsB = 0xAACFDCF2FB9AD809ull;
    constexpr uint64_t psA = 0x00000000000000A1ull, psB = 0x00000000000000B2ull;
    char text[600];

    // Empty: the line is still written, in either liveness.
    Table t;
    t.format(text, sizeof(text), true);
    c.check(std::strcmp(text, "flat engine motion unkeyed 5s: live=1 binds=0 distinct=0 overflow=0 pairs=[]") == 0,
            "U1 an empty window still writes its line, live");
    t.format(text, sizeof(text), false);
    c.check(std::strcmp(text, "flat engine motion unkeyed 5s: live=0 binds=0 distinct=0 overflow=0 pairs=[]") == 0,
            "U1 and when the engine-record velocity is not live");

    // Counting, and the busiest pair first.
    for (int i = 0; i < 3; ++i) t.note(vsA, psA);
    for (int i = 0; i < 5; ++i) t.note(vsA, psB);
    t.note(vsB, psA);
    c.check(t.total() == 9 && t.used() == 3 && t.overflow() == 0 &&
            t.bindsOf(vsA, psA) == 3 && t.bindsOf(vsA, psB) == 5 && t.bindsOf(vsB, psA) == 1 &&
            t.bindsOf(vsB, psB) == 0,
            "U2 each (vs, ps) pair counts its own bind events; a pair that never bound is zero");
    t.format(text, sizeof(text), true);
    c.check(std::strcmp(text,
                "flat engine motion unkeyed 5s: live=1 binds=9 distinct=3 overflow=0 pairs=["
                "vs_66DE2CADB1F4AE6B/ps_00000000000000B2:5 vs_66DE2CADB1F4AE6B/ps_00000000000000A1:3 "
                "vs_AACFDCF2FB9AD809/ps_00000000000000A1:1]") == 0,
            "U2 the line names the pairs busiest first, the vertex shader beside each pixel shader");

    // Capacity: the first four are named, every later distinct pair is counted only.
    t.note(vsB, psB);                          // fourth distinct: named
    t.note(0x1111ull, 0x2222ull);              // fifth: overflow
    t.note(0x1111ull, 0x2222ull);              // the same fifth again: overflow again, still unnamed
    t.note(0x3333ull, 0x4444ull);              // sixth: overflow
    c.check(t.used() == kCapacity && t.total() == 13 && t.overflow() == 3 &&
            t.bindsOf(0x1111ull, 0x2222ull) == 0,
            "U3 past four distinct pairs the rest are counted as overflow and never named");
    t.format(text, sizeof(text), true);
    c.check(std::strstr(text, "binds=13 distinct=4 overflow=3 pairs=[") != nullptr &&
            std::strstr(text, "0000000000001111") == nullptr,
            "U3 the line says how many binds the four named pairs do not account for");
    // A later, busier pair does not displace a named one: the table is first-seen, and says so.
    for (int i = 0; i < 50; ++i) t.note(0x5555ull, 0x6666ull);
    c.check(t.bindsOf(0x5555ull, 0x6666ull) == 0 && t.overflow() == 53, "U3 first-seen: a busier latecomer only adds to the overflow");

    // A window ends where it is taken.
    t.reset();
    c.check(t.total() == 0 && t.used() == 0 && t.overflow() == 0 && t.bindsOf(vsA, psA) == 0,
            "U4 reset empties the window");

    // The worst line fits a log line, and a small buffer is never overrun.
    for (int i = 0; i < 4; ++i) t.note(0xFFFFFFFFFFFFFFF0ull + i, 0xFFFFFFFFFFFFFFE0ull + i);
    t.format(text, sizeof(text), true);
    c.check(std::strlen(text) < 400, "U5 four named pairs fit well inside a log line");
    char tiny[40]; std::memset(tiny, 'x', sizeof(tiny));   // (windows.h defines "small" as a macro)
    const int n = t.format(tiny, sizeof(tiny), true);
    c.check(n >= static_cast<int>(sizeof(tiny)) - 1 && std::memchr(tiny, '\0', sizeof(tiny)) != nullptr,
            "U5 a buffer too small for the line is terminated and reports the length it wanted");
}
} // namespace unkeyed_tests
