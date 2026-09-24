// Per-headset settings: the grammar of a headset-keyed list, the sanitiser
// both DLLs key on, and the matching rule. Header-only, with no Config or Log
// dependency, so the graphics DLL (which resolves), the F8 menu (which
// writes), the native host (which traces the key) and the self-tests share one
// implementation and cannot disagree byte for byte
// (docs/openxr-resolution-per-headset-2026-09-14.md, "The shared header").
//
// An entry is `runtime[/system]:value`; entries are comma-separated, at most
// eight. Both halves of the key are what headsetToken() makes of the raw
// runtime and system names, and an entry's own halves go through the same
// function before comparison, so a hand-typed `Oculus/Meta-Quest-3:3283`
// matches. The split rule is fixed: the runtime half ends at the FIRST '/',
// the value begins after the LAST ':'.
//
// Each key names the range its own values live in: a render width in
// 1..16384 for fix.openxr_resolution, degrees in 0..30 for the field-of-view
// trims. A bare number on its own (180, 3283, 5, or the older 1.8) names no
// headset and is a malformed token, which the graphics log names once. The
// Resolution* names below are the width-ranged wrappers, so every call site
// that predates the second user reads exactly as it did.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "native_render_settings.h"

namespace edvr::native_render {

// 30+1+30 = 61 bytes for a full key, under the Status page's 63-byte line and
// under Log::note's 1200-byte buffer for eight entries with prose around them.
constexpr size_t kHeadsetTokenMax = 30;
constexpr size_t kHeadsetEntryMax = 8;
constexpr size_t kResolutionEntryMax = kHeadsetEntryMax;
// The legal values of the two keys that use this grammar: a render width in
// pixels per eye, and degrees taken off one edge of the field of view.
constexpr uint32_t kResolutionWidthMin = 1;
constexpr uint32_t kResolutionWidthMax = 16384;
constexpr uint32_t kTrimDegreesMax = 30;

// The sanitiser. Bytes up to the first NUL (at most maxBytes); ASCII letters
// lowercased; letters and digits kept; every run of anything else (space,
// '/', '-', '_', '.', ',', parentheses, every byte above 0x7F) becomes one
// '-'; no leading or trailing '-'; truncated to kHeadsetTokenMax with a
// trailing '-' trimmed again. Idempotent: a token sanitises to itself.
inline std::string headsetToken(const char* raw, size_t maxBytes) {
    std::string out;
    if (!raw) return out;
    bool pendingDash = false;
    for (size_t i = 0; i < maxBytes && raw[i]; ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        const bool upper = c >= 'A' && c <= 'Z';
        if (upper || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (pendingDash && !out.empty()) out.push_back('-');
            pendingDash = false;
            out.push_back(upper ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c));
        } else {
            pendingDash = true;
        }
    }
    if (out.size() > kHeadsetTokenMax) out.resize(kHeadsetTokenMax);
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

inline std::string headsetToken(const std::string& raw) {
    return headsetToken(raw.c_str(), raw.size());
}

// "rt/sys", or "rt" alone when the runtime reported no system name.
inline std::string headsetKey(const std::string& runtimeToken,
                              const std::string& systemToken) {
    if (systemToken.empty()) return runtimeToken;
    return runtimeToken + "/" + systemToken;
}

struct HeadsetEntry {
    std::string runtime, system;  // sanitised; system empty = runtime-only
    uint32_t value = 0;
};
// The width-ranged reading of the same entry; one type, so a list written by
// the menu and a list read by the DLL cannot drift apart.
using ResolutionEntry = HeadsetEntry;

inline std::string trimSpaces(const std::string& text) {
    size_t begin = 0, end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    return text.substr(begin, end - begin);
}

// One comma-separated piece, against the key's own value range. False for
// anything the grammar refuses: no ':', a value that is not an integer within
// [min, max], an empty runtime half, or a '/' with nothing (or only
// punctuation) after it. At most five digits whatever the range, and a value
// of 0 is legal only where min is 0 -- a hand-typed `key:0` on a trim parses
// (and means no trim), while `key:0` on a width does not.
inline bool parseHeadsetEntry(const std::string& piece, HeadsetEntry* out,
                              uint32_t min, uint32_t max) {
    if (!out) return false;
    const std::string text = trimSpaces(piece);
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos) return false;
    const std::string digits = trimSpaces(text.substr(colon + 1));
    if (digits.empty() || digits.size() > 5) return false;
    uint32_t value = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    if (value < min || value > max) return false;
    const std::string head = text.substr(0, colon);
    const size_t slash = head.find('/');
    HeadsetEntry entry;
    if (slash == std::string::npos) {
        entry.runtime = headsetToken(trimSpaces(head));
    } else {
        entry.runtime = headsetToken(trimSpaces(head.substr(0, slash)));
        entry.system = headsetToken(trimSpaces(head.substr(slash + 1)));
        if (entry.system.empty()) return false;
    }
    if (entry.runtime.empty()) return false;
    entry.value = value;
    *out = entry;
    return true;
}

// Parses the ini value into at most kHeadsetEntryMax entries, in order,
// duplicates kept (resolve takes the first; merge drops the later ones).
// Every refused piece, and every well-formed entry past the eighth, is
// appended to `skipped` (trimmed, as typed) so the caller can name it.
inline size_t parseHeadsetEntries(const char* value,
                                  HeadsetEntry out[kHeadsetEntryMax],
                                  std::vector<std::string>* skipped,
                                  uint32_t min, uint32_t max) {
    size_t count = 0;
    if (!value || !out) return 0;
    const std::string whole(value);
    size_t begin = 0;
    while (begin <= whole.size()) {
        size_t end = whole.find(',', begin);
        if (end == std::string::npos) end = whole.size();
        const std::string piece = trimSpaces(whole.substr(begin, end - begin));
        begin = end + 1;
        if (piece.empty()) continue;
        HeadsetEntry entry;
        if (!parseHeadsetEntry(piece, &entry, min, max) || count >= kHeadsetEntryMax) {
            if (skipped) skipped->push_back(piece);
            continue;
        }
        out[count++] = entry;
    }
    return count;
}

// The matching rule: the first runtime/system entry equal to the worn pair
// (matched 1), else the first runtime-only entry for the worn runtime
// (matched 2), else nothing (matched 0, returns 0: no entry, which the width
// reads as 100% of the runtime's recommendation and a trim as no trim). A
// runtime/system entry whose system differs is never used, and with an empty
// worn system token only rule 2 can match.
inline uint32_t resolveHeadsetValue(const HeadsetEntry* entries, size_t count,
                                    const std::string& rt, const std::string& sys,
                                    uint32_t* matched) {
    if (matched) *matched = 0;
    if (!entries || rt.empty()) return 0;
    if (!sys.empty()) {
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].runtime == rt && entries[i].system == sys) {
                if (matched) *matched = 1;
                return entries[i].value;
            }
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].runtime == rt && entries[i].system.empty()) {
            if (matched) *matched = 2;
            return entries[i].value;
        }
    }
    return 0;
}

inline size_t parseResolutionEntries(const char* value,
                                     ResolutionEntry out[kResolutionEntryMax],
                                     std::vector<std::string>* skipped) {
    return parseHeadsetEntries(value, out, skipped, kResolutionWidthMin,
                               kResolutionWidthMax);
}

inline uint32_t resolveResolutionWidth(const ResolutionEntry* entries, size_t count,
                                       const std::string& rt, const std::string& sys,
                                       uint32_t* matched) {
    return resolveHeadsetValue(entries, count, rt, sys, matched);
}

// width / recommended, 1.0 when either is 0. The host clamps it (0.25..2.0)
// and effectiveScale() caps it; both are unchanged by the width unit.
inline float widthToScale(uint32_t width, uint32_t recommendedWidth) noexcept {
    if (!width || !recommendedWidth) return EDVR_NATIVE_RENDER_SCALE_DEFAULT;
    return static_cast<float>(width) / static_cast<float>(recommendedWidth);
}

inline std::string formatHeadsetEntry(const HeadsetEntry& entry) {
    char digits[16];
    snprintf(digits, sizeof(digits), "%u", entry.value);
    return headsetKey(entry.runtime, entry.system) + ":" + digits;
}

// Canonical spacing: `rt/sys:V, rt/sys:V`.
inline std::string formatHeadsetEntries(const HeadsetEntry* entries, size_t count) {
    std::string out;
    if (!entries) return out;
    for (size_t i = 0; i < count; ++i) {
        if (i) out += ", ";
        out += formatHeadsetEntry(entries[i]);
    }
    return out;
}

// Rewrites the first entry keyed rt/sys in place, drops any later duplicate
// of that key, appends when absent, keeps every other well-formed entry in
// its order and drops malformed tokens. False, writing nothing, for an empty
// runtime token, a value outside [min, max], or a ninth key.
inline bool mergeHeadsetEntry(const std::string& list, const std::string& rt,
                              const std::string& sys, uint32_t value,
                              std::string* out, uint32_t min, uint32_t max) {
    if (!out || rt.empty() || value < min || value > max) return false;
    HeadsetEntry entries[kHeadsetEntryMax];
    const size_t count = parseHeadsetEntries(list.c_str(), entries, nullptr, min, max);
    HeadsetEntry kept[kHeadsetEntryMax + 1];
    size_t keptCount = 0;
    bool placed = false;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].runtime == rt && entries[i].system == sys) {
            if (placed) continue;
            kept[keptCount] = entries[i];
            kept[keptCount++].value = value;
            placed = true;
        } else {
            kept[keptCount++] = entries[i];
        }
    }
    if (!placed) {
        if (keptCount >= kHeadsetEntryMax) return false;
        kept[keptCount].runtime = rt;
        kept[keptCount].system = sys;
        kept[keptCount++].value = value;
    }
    *out = formatHeadsetEntries(kept, keptCount);
    return true;
}

// Removes every entry keyed exactly rt/sys. A runtime-only entry the headset
// would then fall back to is left alone.
inline std::string removeHeadsetEntry(const std::string& list, const std::string& rt,
                                      const std::string& sys, uint32_t min, uint32_t max) {
    HeadsetEntry entries[kHeadsetEntryMax];
    const size_t count = parseHeadsetEntries(list.c_str(), entries, nullptr, min, max);
    HeadsetEntry kept[kHeadsetEntryMax];
    size_t keptCount = 0;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].runtime == rt && entries[i].system == sys) continue;
        kept[keptCount++] = entries[i];
    }
    return formatHeadsetEntries(kept, keptCount);
}

inline std::string formatResolutionEntries(const ResolutionEntry* entries, size_t count) {
    return formatHeadsetEntries(entries, count);
}

inline bool mergeResolutionEntry(const std::string& list, const std::string& rt,
                                 const std::string& sys, uint32_t width,
                                 std::string* out) {
    return mergeHeadsetEntry(list, rt, sys, width, out, kResolutionWidthMin,
                             kResolutionWidthMax);
}

inline std::string removeResolutionEntry(const std::string& list, const std::string& rt,
                                         const std::string& sys) {
    return removeHeadsetEntry(list, rt, sys, kResolutionWidthMin, kResolutionWidthMax);
}

// The widest eye-0 width the host can actually build: effectiveScale takes
// the minimum over both eyes and both axes, so the height can bind before
// maxWidth does (4000x4500 under 8192x8192 reaches 7282 wide, not 8192).
// Same float arithmetic as effectiveScale/scaledDimension, minus the 2x
// clamp, so a width at the cap round-trips through them exactly.
inline uint32_t effectiveWidthCap(const EdvrNativeRenderViewBounds eyes[2]) noexcept {
    if (!eyes) return 0;
    float ratio = 0.f;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const EdvrNativeRenderViewBounds& b = eyes[eye];
        if (!b.originalWidth || !b.originalHeight || !b.maxWidth || !b.maxHeight) return 0;
        const float byWidth = static_cast<float>(b.maxWidth) / static_cast<float>(b.originalWidth);
        const float byHeight = static_cast<float>(b.maxHeight) / static_cast<float>(b.originalHeight);
        const float tightest = (std::min)(byWidth, byHeight);
        ratio = eye ? (std::min)(ratio, tightest) : tightest;
    }
    const double scaled = static_cast<double>(eyes[0].originalWidth) * static_cast<double>(ratio) + 0.5;
    if (scaled >= static_cast<double>(eyes[0].maxWidth)) return eyes[0].maxWidth;
    return static_cast<uint32_t>(scaled);
}

// Whole-string number parse, for the "bare number" log line only.
inline bool parseBareNumber(const char* value, double* out) {
    if (!value) return false;
    const std::string text = trimSpaces(value);
    if (text.empty()) return false;
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (!end || *end || end == text.c_str() || !std::isfinite(parsed)) return false;
    if (out) *out = parsed;
    return true;
}

// For one log line: what a legacy value would have meant on the worn
// headset. 1.8 (a fraction in 0.25..2.0) and 180 (a percent in 25..200) are
// converted to the width on eyes[0]'s recommendation; a plain integer that is
// neither (3283) is taken as the width. 0 when unparsable, when the result
// falls outside a quarter to twice the recommendation, or above the
// effective cap -- a bare 3..24 or 201..455 on a 1824 base would otherwise be
// offered as a width below the 0.25 clamp. `aboveCap` (optional) is set when
// the width was within the quarter-to-twice band and only the runtime's cap
// refused it, so the log can name the right bound. Never used to size
// anything.
inline uint32_t bareNumberToWidth(const char* value, const EdvrNativeRenderViewBounds eyes[2],
                                  bool* aboveCap = nullptr) {
    if (aboveCap) *aboveCap = false;
    double number = 0.0;
    if (!eyes || !eyes[0].originalWidth || !parseBareNumber(value, &number)) return 0;
    if (number <= 0.0) return 0;
    const double recommended = static_cast<double>(eyes[0].originalWidth);
    const bool whole = std::floor(number) == number;
    double width = 0.0;
    if (number >= 0.25 && number <= 2.0) {
        width = recommended * number;
    } else if (whole && number >= 25.0 && number <= 200.0) {
        width = recommended * number / 100.0;
    } else if (whole) {
        width = number;
    } else {
        return 0;
    }
    // Range-checked as a double first: strtod accepts 1e30 or an 11-digit
    // integer, and casting that to uint32_t is undefined behaviour.
    if (width < 0.5 || width + 0.5 >= static_cast<double>(kResolutionWidthMax) + 1.0) return 0;
    const uint32_t rounded = static_cast<uint32_t>(width + 0.5);
    if (rounded < 1 || rounded > kResolutionWidthMax) return 0;
    if (static_cast<double>(rounded) * 4.0 < recommended || static_cast<double>(rounded) > recommended * 2.0) return 0;
    const uint32_t cap = effectiveWidthCap(eyes);
    if (cap && rounded > cap) {
        if (aboveCap) *aboveCap = true;
        return 0;
    }
    return rounded;
}

// A percent rounded to 0.1 with a zero decimal dropped: 180 -> "180",
// 106.9 -> "106.9". The caller appends the sign; one formatter so the row,
// the hint, the tooltip and the graphics log cannot disagree on 180 vs 180.0.
inline std::string formatPercent(float percent) {
    const double rounded = std::round(static_cast<double>(percent) * 10.0) / 10.0;
    char text[32];
    if (std::fabs(rounded - std::round(rounded)) < 0.0001) {
        snprintf(text, sizeof(text), "%.0f", rounded);
    } else {
        snprintf(text, sizeof(text), "%.1f", rounded);
    }
    return text;
}

inline double megapixels(uint32_t w, uint32_t h) noexcept {
    return static_cast<double>(w) * static_cast<double>(h) / 1e6;
}

}  // namespace edvr::native_render
