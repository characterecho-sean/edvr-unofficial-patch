// The format-support log's two pure halves: the ledger that decides which of the
// game's capability queries get a line, and the line formats themselves.
//
// PURE and header-only, no Windows or D3D types, so tools\format_support_test drives
// the very ledger and the very formatters the DLL runs. src\d3d11\format_support_log.cpp
// owns the D3D calls and the log; docs\macos-dxmt-2026-09-30.md has the why.
//
// THE LEDGER IS LOCK-FREE. The game's queries arrive on whatever thread asks, at
// startup, and the hook must not take a lock there. A query is reduced to one 64-bit
// key (kind, what was asked, HRESULT, what came back), and a fixed open-addressing
// table of atomics remembers which keys have been seen: one compare-exchange claims an
// empty cell, and whoever wins the cell owns the key's first occurrence. Identical
// (query, format, answer) triples therefore log once, in first-call order, and the line
// cap is a counter the winners draw from.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "format_support_decode.h"

namespace edvr {

// What was asked. FORMAT_SUPPORT and FORMAT_SUPPORT2 through CheckFeatureSupport are
// kept apart from CheckFormatSupport because an implementation can answer them from
// different code, and that difference is itself a finding.
enum FormatQueryKind : uint32_t {
    kFqCheckFormatSupport    = 1,   // ID3D11Device::CheckFormatSupport(format)
    kFqFeatureFormatSupport  = 2,   // CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT)
    kFqFeatureFormatSupport2 = 3,   // CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2)
    kFqFeatureOther          = 4,   // CheckFeatureSupport(any other D3D11_FEATURE)
};

constexpr uint32_t kFqFeatureFormatSupportId  = 2;    // D3D11_FEATURE_FORMAT_SUPPORT
constexpr uint32_t kFqFeatureFormatSupport2Id = 3;    // D3D11_FEATURE_FORMAT_SUPPORT2
constexpr uint32_t kFqFeatureOptionsId        = 5;    // D3D11_FEATURE_D3D11_OPTIONS
constexpr uint32_t kFqFeatureOptions2Id       = 14;   // D3D11_FEATURE_D3D11_OPTIONS2
constexpr size_t   kFqWordsKept               = 16;   // output words of an "other" feature that are kept

inline uint32_t fqFnv1a32(const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; data && i < bytes; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

// The key a query is remembered by. `answer` is the support mask for the format
// queries and a digest of the output bytes for any other feature; `size` is that
// feature's FeatureSupportDataSize (0 for the format queries). Never zero: zero is the
// table's empty cell.
inline uint64_t formatQueryKey(uint32_t kind, uint32_t asked, int32_t hr, uint32_t answer,
                               uint32_t size) {
    const uint32_t parts[5] = {kind, asked, static_cast<uint32_t>(hr), answer, size};
    uint64_t h = 1469598103934665603ull;              // FNV-1a, 64 bit
    for (uint32_t part : parts)
        for (int i = 0; i < 4; ++i) { h ^= (part >> (8 * i)) & 0xFFu; h *= 1099511628211ull; }
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;   // the low bits pick the cell
    return h ? h : 1;
}

class FormatQueryLedger {
public:
    // Distinct queries remembered; a power of two, and 8 KB of zeroed static storage. A
    // game that sweeps every DXGI format through all three queries makes about 350;
    // past this the table stops remembering (so stops counting distinct) and the calls
    // are counted as untracked. Printing never depends on it: the first 40 distinct
    // queries print long before any table could fill.
    static constexpr uint32_t kTableSlots = 1024;
    static constexpr uint32_t kMaxLines   = 40;    // distinct queries that get a line

    struct Verdict {
        uint32_t callNumber = 0;       // 1-based ordinal of this call among every call noted
        uint32_t distinctNumber = 0;   // 1-based ordinal among the distinct keys; 0 = seen before
        bool     isNew = false;        // the first time this exact key was seen
        bool     print = false;        // isNew, and inside the line cap
    };

    // Safe from any thread at once; no lock, no allocation. A key that finds the table
    // full is counted as untracked and never prints (and, being untracked, cannot be
    // told from a repeat).
    Verdict note(uint64_t key) {
        Verdict v;
        v.callNumber = calls_.fetch_add(1, std::memory_order_relaxed) + 1;
        uint32_t cell = static_cast<uint32_t>(key) & (kTableSlots - 1);
        for (uint32_t probe = 0; probe < kTableSlots; ++probe, cell = (cell + 1) & (kTableSlots - 1)) {
            uint64_t seen = slots_[cell].load(std::memory_order_acquire);
            if (seen == 0) {
                uint64_t expected = 0;
                if (slots_[cell].compare_exchange_strong(expected, key, std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
                    v.isNew = true;
                    v.distinctNumber = distinct_.fetch_add(1, std::memory_order_relaxed) + 1;
                    v.print = v.distinctNumber <= kMaxLines;
                    if (v.print) printed_.fetch_add(1, std::memory_order_relaxed);
                    return v;
                }
                seen = expected;   // another thread took the cell: was it this key?
            }
            if (seen == key) return v;
        }
        untracked_.fetch_add(1, std::memory_order_relaxed);
        return v;
    }

    uint32_t calls() const { return calls_.load(std::memory_order_relaxed); }
    uint32_t distinct() const { return distinct_.load(std::memory_order_relaxed); }
    uint32_t printed() const { return printed_.load(std::memory_order_relaxed); }
    uint32_t untracked() const { return untracked_.load(std::memory_order_relaxed); }
    // Distinct queries past the cap: seen, counted, not printed.
    uint32_t suppressed() const {
        const uint32_t d = distinct(), p = printed();
        return d > p ? d - p : 0;
    }

private:
    std::atomic<uint64_t> slots_[kTableSlots]{};
    std::atomic<uint32_t> calls_{0};
    std::atomic<uint32_t> distinct_{0};
    std::atomic<uint32_t> printed_{0};
    std::atomic<uint32_t> untracked_{0};
};

// The most lines the whole instrument can print in one session, from the limits the
// DLL applies: adapter lines (the first three devices), one self-query line per
// format in the table, the options line, the one line that says the hooks went on (or
// why they could not), the game's queries (the ledger's cap), and the closing count
// (printed at most twice). The budget is under about 60 a session.
constexpr uint32_t kFqMaxAdapterLines = 3;
constexpr uint32_t kFqSelfFormatLines = 11;
constexpr uint32_t kFqOptionsLines    = 1;
constexpr uint32_t kFqInstallLines    = 1;
constexpr uint32_t kFqMaxSummaryLines = 2;
constexpr uint32_t kFqMaxLinesPerSession = kFqMaxAdapterLines + kFqSelfFormatLines +
                                           kFqOptionsLines + kFqInstallLines +
                                           FormatQueryLedger::kMaxLines + kFqMaxSummaryLines;

// ---- THE LINES ------------------------------------------------------------------
//
// Every line is built here, from plain values, so the rig pins the exact text and the
// doc's example lines are the rig's expected strings. They share two habits: a mask
// prints as hex AND as names, and a failed call prints its HRESULT (named when the
// name is known) and no mask, because an out parameter the call did not fill says
// nothing.

namespace fsdetail {

inline void addHr(TextOut& o, int32_t hr) {
    o.adds("hr=");
    o.add0x(static_cast<uint32_t>(hr));
    const char* name = hresultText(hr);
    if (name && hr != 0) { o.adds(" ("); o.adds(name); o.addc(')'); }
}

inline void addFormat(TextOut& o, uint32_t format) {
    const char* name = dxgiFormatName(format);
    o.adds(name ? name : "UNNAMED");
    o.addc('['); o.addu(format); o.addc(']');
}

// "label=0x.... [NAMES]", or "label=FAILED hr=0x.... (NAME)".
inline void addMaskAnswer(TextOut& o, const char* label, int32_t hr, uint32_t mask,
                          const char* const* names) {
    o.adds(label);
    o.addc('=');
    if (hr < 0) { o.adds("FAILED "); addHr(o, hr); return; }
    o.add0x(mask);
    o.adds(" [");
    addMaskNames(o, mask, names);
    o.addc(']');
}

}  // namespace fsdetail

// One adapter, as the game's device reports it through IDXGIDevice::GetAdapter.
struct FqAdapter {
    const void* device = nullptr;
    uint32_t    index = 0;              // 0 for the first device of the session
    const char* description = "";       // UTF-8
    uint32_t    vendorId = 0, deviceId = 0, subSysId = 0, revision = 0;
    uint64_t    dedicatedVideo = 0, dedicatedSystem = 0, sharedSystem = 0;   // bytes
    uint32_t    luidHigh = 0, luidLow = 0;
};

// D3D11 adapter: device=000001F2A3B4C5D0 (#1) description="..." vendor=0x106B deviceId=0x0000 ...
inline size_t formatAdapterLine(const FqAdapter& a, char* out, size_t cap) {
    fsdetail::TextOut o(out, cap);
    o.adds("D3D11 adapter: device=");
    o.addh(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(a.device)), 16);
    o.adds(" (#"); o.addu(a.index + 1u); o.adds(") description=\"");
    o.adds(a.description);
    o.adds("\" vendor=0x"); o.addh(a.vendorId, 4);
    o.adds(" deviceId=0x"); o.addh(a.deviceId, 4);
    o.adds(" subsys="); o.add0x(a.subSysId);
    o.adds(" revision="); o.addu(a.revision);
    o.adds(" dedicatedVideoMemory="); o.addu(a.dedicatedVideo >> 20);
    o.adds(" MiB dedicatedSystemMemory="); o.addu(a.dedicatedSystem >> 20);
    o.adds(" MiB sharedSystemMemory="); o.addu(a.sharedSystem >> 20);
    o.adds(" MiB luid="); o.addh(a.luidHigh, 8); o.addc(':'); o.addh(a.luidLow, 8);
    return o.finish();
}

// The same line when the adapter could not be read: which step failed, and what it said.
inline size_t formatAdapterFailureLine(const void* device, uint32_t index, const char* step,
                                       int32_t hr, char* out, size_t cap) {
    fsdetail::TextOut o(out, cap);
    o.adds("D3D11 adapter: device=");
    o.addh(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(device)), 16);
    o.adds(" (#"); o.addu(index + 1u); o.adds(") not identified: ");
    o.adds(step);
    o.addc(' ');
    fsdetail::addHr(o, hr);
    return o.finish();
}

// What the device says about one format when EDVR asks it, three ways.
struct FqSelfFormat {
    uint32_t format = 0;
    int32_t  supportHr = 0;   uint32_t support = 0;    // CheckFormatSupport
    int32_t  support2Hr = 0;  uint32_t support2 = 0;   // CheckFeatureSupport(FORMAT_SUPPORT2)
    int32_t  viaHr = 0;       uint32_t via = 0;        // CheckFeatureSupport(FORMAT_SUPPORT)
};

// format support (self-query): R11G11B10_FLOAT[26] support=0x... [NAMES] support2=0x... [NAMES]
//     via-CheckFeatureSupport=same
inline size_t formatSelfQueryLine(const FqSelfFormat& s, char* out, size_t cap) {
    fsdetail::TextOut o(out, cap);
    o.adds("format support (self-query): ");
    fsdetail::addFormat(o, s.format);
    o.addc(' ');
    fsdetail::addMaskAnswer(o, "support", s.supportHr, s.support, fsdetail::supportBitNames());
    o.addc(' ');
    fsdetail::addMaskAnswer(o, "support2", s.support2Hr, s.support2, fsdetail::support2BitNames());
    // The FORMAT_SUPPORT query through CheckFeatureSupport should agree with
    // CheckFormatSupport; where it does not, say what it answered.
    const bool same = s.viaHr == s.supportHr && (s.supportHr < 0 || s.via == s.support);
    if (same) {
        o.adds(" via-CheckFeatureSupport=same");
    } else {
        o.adds(" via-CheckFeatureSupport=DIFFERS ");
        fsdetail::addMaskAnswer(o, "support", s.viaHr, s.via, fsdetail::supportBitNames());
    }
    return o.finish();
}

// D3D11_FEATURE_D3D11_OPTIONS and OPTIONS2 as EDVR's own query saw them.
struct FqSelfOptions {
    int32_t  hr1 = 0;
    uint32_t words1[14] = {};
    int32_t  hr2 = 0;
    uint32_t words2[8] = {};
};

// device options (self-query): D3D11_OPTIONS hr=0x00000000 [OMLogicOp=0 ...] D3D11_OPTIONS2 hr=... [...]
inline size_t formatOptionsLine(const FqSelfOptions& s, char* out, size_t cap) {
    char flags[512];
    fsdetail::TextOut o(out, cap);
    o.adds("device options (self-query): D3D11_OPTIONS ");
    if (s.hr1 < 0) {
        o.adds("FAILED "); fsdetail::addHr(o, s.hr1);
    } else {
        fsdetail::addHr(o, s.hr1);
        d3d11OptionsFlags(s.words1, 14, flags, sizeof(flags));
        o.adds(" ["); o.adds(flags); o.addc(']');
    }
    o.adds(" D3D11_OPTIONS2 ");
    if (s.hr2 < 0) {
        o.adds("FAILED "); fsdetail::addHr(o, s.hr2);
    } else {
        fsdetail::addHr(o, s.hr2);
        d3d11Options2Flags(s.words2, 8, flags, sizeof(flags));
        o.adds(" ["); o.adds(flags); o.addc(']');
    }
    return o.finish();
}

// One of the game's own queries, as the device hook saw it.
struct FqGameQuery {
    uint32_t    kind = 0;           // FormatQueryKind
    uint32_t    callNumber = 0;     // 1-based ordinal among all the game's queries
    uint32_t    asked = 0;          // DXGI_FORMAT (kinds 1-3) or D3D11_FEATURE (kind 4)
    int32_t     hr = 0;
    uint32_t    answer = 0;         // kinds 1-3: the support mask
    uint32_t    dataSize = 0;       // kind 4: the FeatureSupportDataSize the game passed
    uint32_t    wordCount = 0;      // kind 4: how many of words[] are filled
    uint32_t    words[kFqWordsKept] = {};
    uint32_t    thread = 0;
    const char* from = nullptr;     // "exe+0x1A2B3C", or the raw address of another module
};

// format support (game #7): CheckFormatSupport(R11G11B10_FLOAT[26]) -> hr=0x00000000
//     support=0x... [NAMES] tid=1234 from=exe+0x1A2B3C
inline size_t formatGameQueryLine(const FqGameQuery& q, char* out, size_t cap) {
    fsdetail::TextOut o(out, cap);
    o.adds("format support (game #");
    o.addu(q.callNumber);
    o.adds("): ");
    if (q.kind == kFqCheckFormatSupport) {
        o.adds("CheckFormatSupport(");
        fsdetail::addFormat(o, q.asked);
        o.adds(") -> ");
        fsdetail::addHr(o, q.hr);
        if (q.hr >= 0) { o.addc(' '); fsdetail::addMaskAnswer(o, "support", q.hr, q.answer, fsdetail::supportBitNames()); }
    } else if (q.kind == kFqFeatureFormatSupport || q.kind == kFqFeatureFormatSupport2) {
        const bool second = q.kind == kFqFeatureFormatSupport2;
        o.adds(second ? "CheckFeatureSupport(FORMAT_SUPPORT2, " : "CheckFeatureSupport(FORMAT_SUPPORT, ");
        fsdetail::addFormat(o, q.asked);
        o.adds(") -> ");
        fsdetail::addHr(o, q.hr);
        if (q.hr >= 0) {
            o.addc(' ');
            fsdetail::addMaskAnswer(o, second ? "support2" : "support", q.hr, q.answer,
                                    second ? fsdetail::support2BitNames() : fsdetail::supportBitNames());
        }
    } else {
        const char* name = d3d11FeatureName(q.asked);
        o.adds("CheckFeatureSupport(");
        if (name) o.adds(name); else o.adds("UNNAMED");
        o.addc('['); o.addu(q.asked); o.adds("], "); o.addu(q.dataSize); o.adds(" bytes) -> ");
        fsdetail::addHr(o, q.hr);
        if (q.hr >= 0 && q.wordCount) {
            char flags[512];
            o.addc(' ');
            if (q.asked == kFqFeatureOptionsId) {
                d3d11OptionsFlags(q.words, q.wordCount, flags, sizeof(flags));
                o.addc('['); o.adds(flags); o.addc(']');
            } else if (q.asked == kFqFeatureOptions2Id) {
                d3d11Options2Flags(q.words, q.wordCount, flags, sizeof(flags));
                o.addc('['); o.adds(flags); o.addc(']');
            } else {
                o.adds("words=[");
                const uint32_t shown = q.wordCount < 8 ? q.wordCount : 8;
                for (uint32_t i = 0; i < shown; ++i) { if (i) o.addc(' '); o.addh(q.words[i], 8); }
                if (q.wordCount > shown) o.adds(" ...");
                o.addc(']');
            }
        }
    }
    o.adds(" tid="); o.addu(q.thread);
    if (q.from && q.from[0]) { o.adds(" from="); o.adds(q.from); }
    return o.finish();
}

// The closing count, so a reader can tell "the game asked 40 things" from "the game
// asked 400 and 360 were not printed" -- and, with the last clause, "the game asked
// nothing" from "the hooks never saw it": a session whose hooks were never reached
// says so here instead of saying nothing.
struct FqSummary {
    uint32_t calls = 0, distinct = 0, printed = 0, suppressed = 0, untracked = 0;
    uint32_t preOpen = 0;   // calls that arrived before the log was open
    uint32_t cap = 0;
    uint32_t hookRuns = 0;  // every entry into either hook, reported or not
};

// format support (game queries): 412 call(s), 77 distinct; 40 logged, 37 suppressed past the
//     40-line cap; 0 untracked (table full); 0 before the log opened; hooks ran 450 time(s),
//     38 not reported
//
// "Not reported" is what the hooks ran for and did not log: EDVR's own queries, another
// device's, and the game's own once the report's fault budget is spent.
inline size_t formatSummaryLine(const FqSummary& s, char* out, size_t cap) {
    fsdetail::TextOut o(out, cap);
    o.adds("format support (game queries): ");
    o.addu(s.calls); o.adds(" call(s), ");
    o.addu(s.distinct); o.adds(" distinct; ");
    o.addu(s.printed); o.adds(" logged, ");
    o.addu(s.suppressed); o.adds(" suppressed past the ");
    o.addu(s.cap); o.adds("-line cap; ");
    o.addu(s.untracked); o.adds(" untracked (table full); ");
    o.addu(s.preOpen); o.adds(" before the log opened; hooks ran ");
    o.addu(s.hookRuns); o.adds(" time(s), ");
    const uint64_t reported = static_cast<uint64_t>(s.calls) + s.preOpen;
    o.addu(s.hookRuns > reported ? s.hookRuns - reported : 0); o.adds(" not reported");
    return o.finish();
}

// The one line device_hook.cpp writes once the two hooks are on the game's device. It
// is the positive half of every other line here: without it, a session with no "game
// #N" lines cannot be told from one whose hooks were never installed.
inline size_t formatHooksInstalledLine(const void* device, size_t formatSlot, size_t featureSlot,
                                       char* out, size_t cap) {
    fsdetail::TextOut o(out, cap);
    o.adds("format support: CheckFormatSupport (vtable slot "); o.addu(formatSlot);
    o.adds(") and CheckFeatureSupport (slot "); o.addu(featureSlot);
    o.adds(") are hooked on the game's device ");
    o.addh(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(device)), 16);
    o.adds("; each forwards the real call first and returns its answer untouched. The game's "
           "queries follow as \"format support (game #N)\" lines.");
    return o.finish();
}

// ...and the same line when they could not go on, which leaves the self-query as the
// only evidence this session.
inline size_t formatHooksMissingLine(size_t usableEntries, size_t formatSlot, size_t featureSlot,
                                     char* out, size_t cap) {
    fsdetail::TextOut o(out, cap);
    o.adds("format support: the game's device table has "); o.addu(usableEntries);
    o.adds(" usable entries, too few for CheckFormatSupport (slot "); o.addu(formatSlot);
    o.adds(") and CheckFeatureSupport (slot "); o.addu(featureSlot);
    o.adds("): they are NOT hooked, so the game's queries are not logged this session. The adapter "
           "and self-query lines are unaffected.");
    return o.finish();
}

}  // namespace edvr
