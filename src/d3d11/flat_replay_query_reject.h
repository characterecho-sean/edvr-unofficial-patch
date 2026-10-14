#pragma once
// The flat foreground replay-safety instrument (2026-10-14; log only, no behaviour change). A foreground draw (a weapon, the hands) is refused
// by the stage=state reason `foreground-active-query` when replayQueriesSafe(ctx) says a count-bearing D3D11 query bracket is open on the
// immediate context (or the tracker is uncertain). The 09:34:54-09:37:34 flight refused every on-foot frame that way (`flat-resolve-foreground-
// contract-unqualified`, accepted-frames=0) and the log could not say WHICH query. This holds what the log needs, pure and Windows-free so
// tools\flat_temporal_test pins the text:
//
//   * one line per rejection for the first kFlatQueryRejectFirst of the session ("flat replay query rejected i/8: ..."), then
//   * one line per 5 s window, printed EVERY window while the foreground report runs, including a window with no rejection:
//       "flat replay query rejections 5s: checked=N rejected=M ..."
//     checked is how many foreground draws reached the safety test. So an absent line means the instrument never ran, checked=0 means the test
//     was never reached, and checked>0 with rejected=0 means no rejection. The window counts per (primary query kind, VS, PS).
//
// Query codes are D3D11_QUERY's numeric values (0 EVENT ... 15 SO_OVERFLOW_PREDICATE_STREAM3), kept as numbers so this header needs no D3D11.
// 100-103 are the tracker's own reasons for "unsafe" with no query to name.
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {

constexpr uint32_t kFlatQueryRejectFirst = 8;     // rejections logged one by one, per session
constexpr uint32_t kFlatQueryRejectTable = 16;    // distinct (query, VS, PS) keys per window; a 17th is counted in overflow
constexpr uint32_t kFlatQueryRejectCodes = 6;     // query codes an event keeps; more are counted in extra
constexpr uint32_t kFlatQueryRejectTop = 4;       // keys the window line lists

constexpr uint32_t kFlatQueryUncertain = 100;     // a bracket escaped observation (unpaired End, duplicate Begin, full table)
constexpr uint32_t kFlatQueryTableFull = 101;     // the per-context table is full or has no entry for this context
constexpr uint32_t kFlatQueryNotImmediate = 102;  // a null or deferred context
constexpr uint32_t kFlatQueryUnheld = 103;        // an open count-bearing bracket whose object could not be read back

inline const char* flatQueryCodeName(uint32_t c) {
    switch (c) {
    case 0: return "EVENT";
    case 1: return "OCCLUSION";
    case 2: return "TIMESTAMP";
    case 3: return "TIMESTAMP_DISJOINT";
    case 4: return "PIPELINE_STATISTICS";
    case 5: return "OCCLUSION_PREDICATE";
    case 6: return "SO_STATISTICS";
    case 7: return "SO_OVERFLOW_PREDICATE";
    case 8: return "SO_STATISTICS_STREAM0";
    case 9: return "SO_OVERFLOW_PREDICATE_STREAM0";
    case 10: return "SO_STATISTICS_STREAM1";
    case 11: return "SO_OVERFLOW_PREDICATE_STREAM1";
    case 12: return "SO_STATISTICS_STREAM2";
    case 13: return "SO_OVERFLOW_PREDICATE_STREAM2";
    case 14: return "SO_STATISTICS_STREAM3";
    case 15: return "SO_OVERFLOW_PREDICATE_STREAM3";
    case kFlatQueryUncertain: return "UNCERTAIN_BRACKET";
    case kFlatQueryTableFull: return "TABLE_FULL";
    case kFlatQueryNotImmediate: return "NOT_IMMEDIATE_CONTEXT";
    case kFlatQueryUnheld: return "UNREADABLE_QUERY";
    default: return "UNKNOWN_QUERY";
    }
}
// A predicate query (it can gate draws): OCCLUSION_PREDICATE and the SO overflow predicates (the odd codes 7..15).
inline bool flatQueryCodeIsPredicate(uint32_t c) { return c == 5 || (c >= 7 && c <= 15 && (c & 1u) != 0); }

struct FlatQueryReject {
    uint32_t codes[kFlatQueryRejectCodes] = {};
    uint32_t count = 0;    // codes kept
    uint32_t extra = 0;    // codes past the kept ones
    uint32_t active = 0;   // open brackets on the context, safe or not
    uint32_t primary() const { return count ? codes[0] : kFlatQueryUnheld; }
};

// "NAME#code[(predicate)]+NAME#code...", "+N more" past the kept codes. Never overruns size; always terminated.
inline void flatQueryRejectCodesText(char* out, size_t size, const FlatQueryReject& r) {
    if (!out || !size) return;
    out[0] = 0;
    size_t used = 0;
    auto put = [&](const char* fmt, const char* a, unsigned b, const char* c) {
        if (used + 1 >= size) return;
        const int n = std::snprintf(out + used, size - used, fmt, a, b, c);
        if (n > 0) used += (static_cast<size_t>(n) < size - used) ? static_cast<size_t>(n) : size - used - 1;
    };
    for (uint32_t i = 0; i < r.count; ++i)
        put(i ? "+%s#%u%s" : "%s#%u%s", flatQueryCodeName(r.codes[i]), r.codes[i], flatQueryCodeIsPredicate(r.codes[i]) ? "(predicate)" : "");
    if (!r.count) put("%s#%u%s", "none", 0u, "");
    if (r.extra && used + 1 < size) std::snprintf(out + used, size - used, "+%u more", r.extra);
}

// One rejection, individually: the query kind(s), the draw's shaders, the target size, the frame and the AA mode.
inline int flatQueryRejectFormatEvent(char* out, size_t size, uint32_t ordinal, uint64_t frame, const char* mode, uint64_t vs, uint64_t ps,
                                      uint32_t width, uint32_t height, const FlatQueryReject& r) {
    char codes[360];
    flatQueryRejectCodesText(codes, sizeof(codes), r);
    return std::snprintf(out, size,
        "flat replay query rejected %u/%u: frame=%llu mode=%s VS=%016llX PS=%016llX target=%ux%u queries=%s open-brackets=%u",
        ordinal, kFlatQueryRejectFirst, static_cast<unsigned long long>(frame), mode ? mode : "?", static_cast<unsigned long long>(vs),
        static_cast<unsigned long long>(ps), width, height, codes, r.active);
}

class FlatQueryRejectStats {
public:
    // A foreground draw reached the safety test.
    void noteChecked() { ++checked_; ++totalChecked_; }
    // The test said unsafe. Returns the 1-based ordinal when this rejection is one of the first kFlatQueryRejectFirst of the session (the
    // caller logs it), else 0. Always counts it in the window, by (primary query, VS, PS).
    uint32_t noteRejected(const FlatQueryReject& r, uint64_t vs, uint64_t ps) {
        ++rejected_; ++totalRejected_;
        const uint32_t code = r.primary();
        bool found = false;
        for (uint32_t i = 0; i < used_; ++i)
            if (key_[i].code == code && key_[i].vs == vs && key_[i].ps == ps) { ++key_[i].n; found = true; break; }
        if (!found) {
            if (used_ < kFlatQueryRejectTable) key_[used_++] = {code, vs, ps, 1};
            else ++overflow_;
        }
        return logged_ < kFlatQueryRejectFirst ? ++logged_ : 0;
    }
    uint64_t checked() const { return checked_; }
    uint64_t rejected() const { return rejected_; }
    uint64_t totalChecked() const { return totalChecked_; }
    uint64_t totalRejected() const { return totalRejected_; }
    uint32_t logged() const { return logged_; }
    uint32_t distinct() const { return used_; }

    // The 5 s line, then the window resets (the session totals stay). Printed every window, so zero is visible.
    int formatWindow(char* out, size_t size, const char* mode) {
        char top[480];
        top[0] = 0;
        size_t used = 0;
        bool taken[kFlatQueryRejectTable] = {};
        for (uint32_t k = 0; k < kFlatQueryRejectTop && k < used_; ++k) {
            int best = -1;
            for (uint32_t i = 0; i < used_; ++i)
                if (!taken[i] && (best < 0 || key_[i].n > key_[best].n)) best = static_cast<int>(i);
            if (best < 0) break;
            taken[best] = true;
            if (used + 1 >= sizeof(top)) break;
            const Key& e = key_[best];
            const int n = std::snprintf(top + used, sizeof(top) - used, "%s%s#%u%s VS=%016llX PS=%016llX n=%llu", k ? " | " : "",
                flatQueryCodeName(e.code), e.code, flatQueryCodeIsPredicate(e.code) ? "(predicate)" : "",
                static_cast<unsigned long long>(e.vs), static_cast<unsigned long long>(e.ps), static_cast<unsigned long long>(e.n));
            if (n > 0) used += (static_cast<size_t>(n) < sizeof(top) - used) ? static_cast<size_t>(n) : sizeof(top) - used - 1;
        }
        if (!used) std::snprintf(top, sizeof(top), "none");
        const int len = std::snprintf(out, size,
            "flat replay query rejections 5s: checked=%llu rejected=%llu distinct=%u overflow=%llu total-checked=%llu total-rejected=%llu "
            "logged=%u/%u mode=%s top=[%s]; checked=0: the safety test was never reached; rejected=0 with checked>0: none refused; no line: not run",
            static_cast<unsigned long long>(checked_), static_cast<unsigned long long>(rejected_), used_,
            static_cast<unsigned long long>(overflow_), static_cast<unsigned long long>(totalChecked_),
            static_cast<unsigned long long>(totalRejected_), logged_, kFlatQueryRejectFirst, mode ? mode : "?", top);
        checked_ = rejected_ = overflow_ = 0;
        used_ = 0;
        return len;
    }

private:
    struct Key { uint32_t code; uint64_t vs, ps, n; };
    Key key_[kFlatQueryRejectTable] = {};
    uint32_t used_ = 0;
    uint32_t logged_ = 0;
    uint64_t checked_ = 0, rejected_ = 0, overflow_ = 0, totalChecked_ = 0, totalRejected_ = 0;
};

}  // namespace edvr
