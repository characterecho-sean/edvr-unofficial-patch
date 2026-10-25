// The game-build resolver's pure half (issue 124; docs\design-game-update-signatures-2026-10-25.md). Free of any Windows API so
// tools\game_build_test can run it on a synthetic image and on the archived executables mapped into memory.
//
// The hooks bind fixed RVAs of one game build (the "known" build). On any other build this finds each of those addresses again by
// matching its whole code unit against the known build with the bytes a rebuild moves masked out, and accepts only a unique answer.
// It is a C++ copy of tools\hook_signatures.py's resolve, which is the reference: same units, same masked FNV-1a-64, same witnesses
// (caller reference, neighbour, clone fingerprints; for data a reference, the one pointer cell, the one string) and the same refusals
// (no candidate, ambiguous, layout-only, data that fails its checks). tools\game_build_test holds the two to the same answers.
//
// The table is generated (src\common\game_signatures.inc, tools\hook_signatures.py generate --emit-cpp): lengths, offsets, mask ranges
// and hashes, never code bytes or strings. The image is read only through ImageView::copy, which the DLL runs under SEH, so an
// unreadable page is a refusal and never a crash.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace edvr {
namespace gamesig {

// ---- The table ----------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kCppFormat = 1;   // hook_signatures.py CPP_FORMAT: the layout of these structs

struct Unit {
    uint32_t len;        // the unit's length in bytes
    uint32_t maskFirst;  // its first [offset, length] pair in Table::mask (index of the pair)
    uint32_t maskPairs;  // how many pairs
    uint64_t hash;       // FNV-1a-64 of the unit with the masked bytes zeroed
};
struct Ref { uint32_t unit, at, next; };   // a unique unit whose rel32/disp32 at `at` (next instruction at `next`) names the target
struct Nbr { uint32_t unit; int32_t delta; };   // the target is `delta` units (in address order) from this unique unit
enum : uint8_t { kFpCode = 1, kFpRodata = 2, kFpFall = 3 };
struct Fp {
    uint8_t kind;
    uint32_t at, next;   // code, rodata: the field and the next instruction's offset in the clone
    uint32_t unit;       // code: the unit landed in; fall: the unit fallen into
    uint32_t off;        // code: the offset landed on in that unit
    uint32_t len;        // rodata: the bytes hashed
    uint64_t hash;       // rodata: their FNV-1a-64
};
enum : uint8_t { kCode = 0, kData = 1 };
enum : uint8_t { kGuardNone = 0, kGuardStr = 1, kGuardCodePtr = 2 };
enum : uint8_t { kPteeNone = 0, kPteeTarget = 1, kPteeUnit = 2 };
constexpr uint8_t kNoSection = 0xFF;
struct Target {
    uint32_t rva;        // in the known build
    uint8_t kind;        // kCode or kData
    uint8_t sec;         // data: index into Table::sections of the section holding it (kNoSection: none)
    uint8_t guard;       // data: what it looks like, checked before it is accepted
    uint8_t ptee;        // data: a cell holding a function's address -- that function, by target index or by unit
    uint32_t unit, off;  // code: the unit and the offset in it
    uint32_t refFirst, refCount, nbrFirst, nbrCount, fpFirst, fpCount;
    uint32_t pteeIndex, pteeOff;   // kPteeTarget: target index; kPteeUnit: unit and offset
    uint32_t guardLen;   // kGuardStr: the string's length (without its NUL) ...
    uint64_t guardHash;  // ... and FNV-1a-64
};
struct Section { char name[9]; };
struct Table {
    uint32_t format, knownStamp, knownImageSize;
    const char* knownBuild;
    uint64_t tableHash;
    const Unit* units; uint32_t unitCount;
    const uint32_t* mask; uint32_t maskCount;   // flat offset, length pairs
    const Target* targets; uint32_t targetCount;
    const Ref* refs; uint32_t refCount;
    const Nbr* nbrs; uint32_t nbrCount;
    const Fp* fps; uint32_t fpCount;
    const Section* sections; uint32_t sectionCount;
};

// ---- The image ----------------------------------------------------------------------------------------------------------------------
// Copies n bytes at rva of the image (mapped as the loader maps it: sections at their virtual addresses) into dst; false when any byte
// is unreadable. The resolver never asks for bytes past SizeOfImage.
using CopyFn = bool (*)(const void* ctx, uint32_t rva, void* dst, size_t n) noexcept;
struct ImageView {
    const void* ctx;         // handed to copy (the DLL: the module base)
    CopyFn copy;
    uint64_t pointerBase;    // what the image's absolute pointers are relative to: the address it is loaded at
};

// ---- Results ------------------------------------------------------------------------------------------------------------------------
// Via bits, named as hook_signatures.py names them; viaText joins the set names in sorted order ("fp+nbr+ref").
enum : uint8_t { kViaContent = 1, kViaFp = 2, kViaHash = 4, kViaNbr = 8, kViaPtr = 16, kViaRef = 32 };
enum Reason : uint8_t { kFound = 0, kNoCandidate, kAmbiguous, kLayoutOnly, kDataRefFailed, kNoImage };
struct Result {
    uint32_t rva = 0;   // in the running build; 0: not found
    uint8_t via = 0;
    uint8_t reason = kNoImage;
};
struct Stats {
    uint32_t units = 0, leaf = 0, candidates = 0, found = 0;
    uint64_t bytes = 0;
    bool imageOk = false;
};

inline const char* reasonText(uint8_t reason) noexcept {
    switch (reason) {
    case kFound: return "-";
    case kNoCandidate: return "no-candidate";
    case kAmbiguous: return "ambiguous";
    case kLayoutOnly: return "layout-only";
    case kDataRefFailed: return "data-ref-failed";
    default: return "no-image";
    }
}
// Writes the via set as hook_signatures.py prints it ("-" for none) into out (at least 40 bytes).
inline void viaText(uint8_t via, char* out, size_t cap) noexcept {
    static const char* const kNames[] = {"content", "fp", "hash", "nbr", "ptr", "ref"};
    size_t n = 0;
    if (cap == 0) return;
    out[0] = '\0';
    for (int bit = 0; bit < 6; ++bit) {
        if (!(via & (1u << bit))) continue;
        const char* name = kNames[bit];
        if (n && n + 1 < cap) out[n++] = '+';
        for (const char* p = name; *p && n + 1 < cap; ++p) out[n++] = *p;
    }
    if (n == 0 && cap > 1) out[n++] = '-';
    out[n < cap ? n : cap - 1] = '\0';
}

constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull, kFnvPrime = 0x100000001B3ull;
inline uint64_t fnv1a64(const uint8_t* p, size_t n, uint64_t h = kFnvOffset) noexcept {
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * kFnvPrime;
    return h;
}

namespace detail {

constexpr uint32_t kScnExec = 0x20000000u, kScnWrite = 0x80000000u;

struct Sec {
    char name[9];
    uint32_t rva, span, rawSize, flags;   // span = max(VirtualSize, SizeOfRawData), as tools\openxr_pe.py has it
};

class Resolver {
public:
    Resolver(const Table& t, const ImageView& img) noexcept : t_(t), img_(img) {}

    bool run(Result* out, Stats* stats) {
        for (uint32_t i = 0; i < t_.targetCount; ++i) out[i] = Result{};
        if (!readHeaders()) return false;
        buildUnits();
        stats_.imageOk = true;
        stats_.units = static_cast<uint32_t>(st_.size());
        for (uint8_t l : leaf_) stats_.leaf += l;
        order_.resize(st_.size());
        for (uint32_t i = 0; i < order_.size(); ++i) order_[i] = i;
        std::stable_sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) { return len(a) < len(b); });
        state_.assign(t_.unitCount, -1);
        found_.assign(t_.unitCount, std::vector<uint32_t>());
        for (uint32_t i = 0; i < t_.targetCount; ++i)
            if (t_.targets[i].kind == kCode) resolveCode(t_.targets[i], out[i]);
        scanData(out);
        for (uint32_t i = 0; i < t_.targetCount; ++i)
            if (t_.targets[i].kind == kData) resolveData(i, out);
        for (uint32_t i = 0; i < t_.targetCount; ++i) stats_.found += out[i].reason == kFound;
        if (stats) *stats = stats_;
        return true;
    }

private:
    // ---- reads --------------------------------------------------------------------------------------------------------------------
    bool read(uint64_t rva, void* dst, size_t n) const noexcept {
        if (n == 0) return true;
        if (rva > imageSize_ || n > imageSize_ - rva) return false;
        return img_.copy(img_.ctx, static_cast<uint32_t>(rva), dst, n);
    }
    bool readU32(uint64_t rva, uint32_t* v) const noexcept { return read(rva, v, 4); }

    // The PE headers, all in the first page (only it is asked for until SizeOfImage is known).
    bool readHeaders() {
        imageSize_ = 0x1000;
        uint8_t dos[0x40];
        if (!read(0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z') return false;
        uint32_t nt = 0;
        std::memcpy(&nt, dos + 0x3C, 4);
        if (nt > 0x800) return false;
        uint8_t fh[24];
        if (!read(nt, fh, sizeof(fh)) || std::memcmp(fh, "PE\0\0", 4) != 0) return false;
        uint16_t machine = 0, count = 0, optSize = 0;
        std::memcpy(&machine, fh + 4, 2);
        std::memcpy(&count, fh + 6, 2);
        std::memcpy(&optSize, fh + 20, 2);
        if (machine != 0x8664 || optSize < 112 || count == 0 || count > 96) return false;
        const uint32_t opt = nt + 24;
        if (opt + optSize + 40u * count > imageSize_) return false;
        std::vector<uint8_t> o(optSize);
        if (!read(opt, o.data(), optSize)) return false;
        uint16_t magic = 0;
        std::memcpy(&magic, o.data(), 2);
        if (magic != 0x20B) return false;
        uint32_t sizeOfImage = 0, dirs = 0;
        std::memcpy(&sizeOfImage, o.data() + 56, 4);
        std::memcpy(&dirs, o.data() + 108, 4);
        if (dirs > (optSize - 112u) / 8u) return false;
        for (uint32_t i = 0; i < dirs && i < 16; ++i) {
            std::memcpy(&dirRva_[i], o.data() + 112 + i * 8, 4);
            std::memcpy(&dirSize_[i], o.data() + 116 + i * 8, 4);
        }
        std::vector<uint8_t> table(40u * count);
        if (!read(opt + optSize, table.data(), table.size())) return false;
        imageSize_ = sizeOfImage;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* s = table.data() + 40u * i;
            Sec sec{};
            std::memcpy(sec.name, s, 8);
            sec.name[8] = '\0';
            uint32_t vsize = 0;
            std::memcpy(&vsize, s + 8, 4);
            std::memcpy(&sec.rva, s + 12, 4);
            std::memcpy(&sec.rawSize, s + 16, 4);
            std::memcpy(&sec.flags, s + 36, 4);
            sec.span = std::max(vsize, sec.rawSize);
            if (uint64_t(sec.rva) + sec.span > 0x100000000ull) return false;
            secs_.push_back(sec);
        }
        return true;
    }

    // ---- units (build_diff.py _Units) ---------------------------------------------------------------------------------------------
    void add(uint32_t begin, uint32_t end, bool isLeaf) {
        st_.push_back(begin);
        en_.push_back(end);
        leaf_.push_back(isLeaf ? 1 : 0);
    }
    // Splits the leaf code in [a, b) on int3 runs that end on a 16-byte boundary (or at the gap's end). An unreadable gap adds nothing.
    void gap(uint32_t a, uint32_t b, bool sectionTail) {
        if (b <= a) return;
        const size_t n = b - a;
        buf_.resize(n);
        if (!read(a, buf_.data(), n)) return;
        const uint8_t* blk = buf_.data();
        size_t pos = 0, i = 0;
        while (i < n) {
            if (blk[i] != 0xCC) { ++i; continue; }
            size_t j = i;
            while (j < n && blk[j] == 0xCC) ++j;
            if ((uint64_t(a) + j) % 16 == 0 || j == n) {
                if (i > pos) add(a + uint32_t(pos), a + uint32_t(i), true);
                pos = j;
            }
            i = j;
        }
        if (pos < n) {
            size_t end = n;
            if (sectionTail)
                while (end > pos && blk[end - 1] == 0) --end;
            if (end > pos) add(a + uint32_t(pos), a + uint32_t(end), true);
        }
    }
    void buildUnits() {
        std::vector<std::pair<uint32_t, uint32_t>> entries;
        const uint32_t pdRva = dirRva_[3], pdSize = dirSize_[3];
        if (pdRva && pdSize) {
            const uint32_t count = pdSize / 12;
            std::vector<uint32_t> raw(size_t(count) * 3);
            if (count && read(pdRva, raw.data(), raw.size() * 4)) {
                entries.reserve(count);
                for (uint32_t k = 0; k < count; ++k)
                    if (raw[3 * k + 1] > raw[3 * k]) entries.emplace_back(raw[3 * k], raw[3 * k + 1]);
                std::sort(entries.begin(), entries.end());
                entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
            }
        }
        st_.reserve(entries.size() + entries.size() / 4);
        en_.reserve(entries.size() + entries.size() / 4);
        leaf_.reserve(entries.size() + entries.size() / 4);
        for (const Sec& s : secs_) {
            if (!(s.flags & kScnExec)) continue;
            const uint32_t limit = s.rva + std::min(s.span, s.rawSize);
            uint32_t cursor = s.rva;
            auto k = std::lower_bound(entries.begin(), entries.end(), std::make_pair(s.rva, 0u));
            for (; k != entries.end(); ++k) {
                uint32_t begin = k->first, end = k->second;
                if (begin >= limit) break;
                begin = std::max(begin, cursor);
                end = std::min(end, limit);
                if (end <= begin) continue;
                gap(cursor, begin, false);
                add(begin, end, false);
                cursor = end;
            }
            gap(cursor, limit, true);
        }
    }
    uint32_t len(uint32_t i) const noexcept { return en_[i] - st_[i]; }
    // The unit holding rva, or -1.
    int64_t at(int64_t rva) const noexcept {
        if (rva < 0 || rva > 0xFFFFFFFFll) return -1;
        auto it = std::upper_bound(st_.begin(), st_.end(), uint32_t(rva));
        if (it == st_.begin()) return -1;
        const int64_t i = (it - st_.begin()) - 1;
        return uint32_t(rva) < en_[size_t(i)] ? i : -1;
    }
    // Unit ui's bytes, masked as table unit uid masks, hashed; false when unreadable.
    bool maskedHash(uint32_t ui, const Unit& u, uint64_t* h) {
        const uint32_t n = len(ui);
        body_.resize(n);
        if (!read(st_[ui], body_.data(), n)) return false;
        for (uint32_t p = 0; p < u.maskPairs; ++p) {
            const uint64_t k = uint64_t(u.maskFirst + p) * 2;
            if (k + 1 >= t_.maskCount) return false;
            const uint32_t off = t_.mask[k], ln = t_.mask[k + 1];
            if (off >= n) continue;
            std::memset(body_.data() + off, 0, std::min<uint64_t>(ln, n - off));
        }
        *h = fnv1a64(body_.data(), n);
        return true;
    }
    bool unitShows(uint32_t ui, uint32_t uid) {
        if (uid >= t_.unitCount) return false;
        const Unit& u = t_.units[uid];
        uint64_t h = 0;
        return len(ui) == u.len && maskedHash(ui, u, &h) && h == u.hash;
    }
    // match(): the units of table unit uid's length whose masked hash is its hash. state 0 ok, 1 ambiguous, 2 no candidate.
    int match(uint32_t uid) {
        if (uid >= t_.unitCount) return 2;
        if (state_[uid] >= 0) return state_[uid];
        const Unit& u = t_.units[uid];
        auto lo = std::lower_bound(order_.begin(), order_.end(), u.len, [&](uint32_t i, uint32_t v) { return len(i) < v; });
        std::vector<uint32_t>& found = found_[uid];
        for (auto it = lo; it != order_.end() && len(*it) == u.len; ++it) {
            ++stats_.candidates;
            stats_.bytes += u.len;
            uint64_t h = 0;
            if (maskedHash(*it, u, &h) && h == u.hash) found.push_back(*it);
        }
        state_[uid] = found.size() == 1 ? 0 : found.empty() ? 2 : 1;
        return state_[uid];
    }
    bool rel32(uint64_t rva, int64_t* v) const noexcept {
        int32_t r = 0;
        if (!read(rva, &r, 4)) return false;
        *v = r;
        return true;
    }

    // ---- witnesses ---------------------------------------------------------------------------------------------------------------
    // The addresses the references name (ref_values). An unreadable field names nothing.
    void refValues(uint32_t first, uint32_t count, std::vector<int64_t>* values) {
        for (uint32_t k = first; k < first + count && k < t_.refCount; ++k) {
            const Ref& r = t_.refs[k];
            if (match(r.unit) != 0) continue;
            const uint32_t ui = found_[r.unit][0];
            int64_t rel = 0;
            if (r.at + 4 > len(ui) || !rel32(uint64_t(st_[ui]) + r.at, &rel)) continue;
            const int64_t v = int64_t(st_[ui]) + r.next + rel;
            if (std::find(values->begin(), values->end(), v) == values->end()) values->push_back(v);
        }
    }
    void neighbourStarts(uint32_t first, uint32_t count, std::vector<int64_t>* values) {
        for (uint32_t k = first; k < first + count && k < t_.nbrCount; ++k) {
            const Nbr& nb = t_.nbrs[k];
            if (match(nb.unit) != 0) continue;
            const int64_t j = int64_t(found_[nb.unit][0]) + nb.delta;
            if (j < 0 || j >= int64_t(st_.size())) continue;
            const int64_t v = st_[size_t(j)];
            if (std::find(values->begin(), values->end(), v) == values->end()) values->push_back(v);
        }
    }
    // rodata_bytes: the n bytes at rva into out when [rva, rva + n) lies in one read-only data section outside the import address table.
    bool rodata(int64_t rva, uint32_t n, std::vector<uint8_t>* out) {
        if (n == 0 || rva < 0) return false;
        const int64_t lo = dirRva_[12], hi = int64_t(dirRva_[12]) + dirSize_[12];
        if (lo < hi && rva < hi && lo < rva + n) return false;
        for (const Sec& s : secs_) {
            if (s.rva <= rva && rva < int64_t(s.rva) + s.span) {
                if (rva + n > int64_t(s.rva) + s.span || (s.flags & (kScnExec | kScnWrite))) return false;
                return readRva(s, uint32_t(rva), n, out);
            }
        }
        return false;
    }
    // _read_rva inside section s: n bytes at rva, zero past the raw data.
    bool readRva(const Sec& s, uint32_t rva, uint32_t n, std::vector<uint8_t>* out) {
        out->assign(n, 0);
        const uint32_t delta = rva - s.rva;
        const uint32_t avail = delta < s.rawSize ? std::min(n, s.rawSize - delta) : 0;
        return avail == 0 || read(rva, out->data(), avail);
    }
    bool fpOk(const Fp& fp, uint32_t ui) {
        if (fp.kind == kFpFall) {
            const size_t k = size_t(ui) + 1;
            return k < st_.size() && st_[k] == en_[ui] && unitShows(uint32_t(k), fp.unit);
        }
        if (uint64_t(fp.at) + 4 > len(ui)) return false;
        int64_t rel = 0;
        if (!rel32(uint64_t(st_[ui]) + fp.at, &rel)) return false;
        const int64_t target = int64_t(st_[ui]) + fp.next + rel;
        if (fp.kind == kFpCode) {
            const int64_t k = at(target);
            return k >= 0 && uint32_t(k) != ui && target - st_[size_t(k)] == fp.off && unitShows(uint32_t(k), fp.unit);
        }
        if (fp.kind != kFpRodata || !rodata(target, fp.len, &scratch_)) return false;
        return fnv1a64(scratch_.data(), scratch_.size()) == fp.hash;
    }
    // fp_values: the start of the one clone that shows every fingerprint; none or several name nothing.
    void fpValues(const Target& tg, const std::vector<uint32_t>& found, std::vector<int64_t>* values) {
        if (tg.fpCount == 0) return;
        int64_t hit = -1;
        uint32_t hits = 0;
        for (uint32_t ui : found) {
            bool all = true;
            for (uint32_t k = tg.fpFirst; k < tg.fpFirst + tg.fpCount && all; ++k)
                all = k < t_.fpCount && fpOk(t_.fps[k], ui);
            if (all) { ++hits; hit = st_[ui]; }
        }
        if (hits == 1) values->push_back(hit);
    }

    // ---- code -------------------------------------------------------------------------------------------------------------------
    void resolveCode(const Target& tg, Result& r) {
        const int state = match(tg.unit);
        if (state == 0) {
            r.rva = st_[found_[tg.unit][0]] + tg.off;
            r.via = kViaHash;
            r.reason = kFound;
            return;
        }
        if (state == 1 && (tg.refCount || tg.nbrCount || tg.fpCount)) {
            const std::vector<uint32_t> clones = found_[tg.unit];
            auto isClone = [&](int64_t v) {
                for (uint32_t ui : clones) if (st_[ui] == v) return true;
                return false;
            };
            uint8_t answered = 0;
            int64_t answer = -1;
            bool conflict = false, disagree = false;
            std::vector<int64_t> values;
            for (int w = 0; w < 3; ++w) {
                values.clear();
                if (w == 0) refValues(tg.refFirst, tg.refCount, &values);
                else if (w == 1) neighbourStarts(tg.nbrFirst, tg.nbrCount, &values);
                else fpValues(tg, clones, &values);
                const uint8_t bit = w == 0 ? kViaRef : w == 1 ? kViaNbr : kViaFp;
                if (values.size() == 1 && isClone(values[0])) {
                    if (answered && answer != values[0]) disagree = true;
                    answer = values[0];
                    answered |= bit;
                } else if (!values.empty()) {
                    conflict = true;
                }
            }
            if (answered && !conflict && !disagree && answered == kViaNbr) {
                r.reason = kLayoutOnly;
            } else if (answered && !conflict && !disagree) {
                r.rva = uint32_t(answer + tg.off);
                r.via = answered;
                r.reason = kFound;
            } else {
                r.reason = kAmbiguous;
            }
            return;
        }
        r.reason = state == 1 ? kAmbiguous : kNoCandidate;
    }

    // ---- data -------------------------------------------------------------------------------------------------------------------
    // The pointee of data target i, or -1 (pointee_of in resolve).
    int64_t pointee(const Target& tg, const Result* out) {
        if (tg.ptee == kPteeTarget)
            return tg.pteeIndex < t_.targetCount && out[tg.pteeIndex].reason == kFound ? int64_t(out[tg.pteeIndex].rva) : -1;
        if (tg.ptee == kPteeUnit && match(tg.pteeIndex) == 0) return int64_t(st_[found_[tg.pteeIndex][0]]) + tg.pteeOff;
        return -1;
    }
    // One pass over the data sections for every pointer and every string the data targets look for (pointer_cells, string_hits).
    void scanData(const Result* out) {
        for (uint32_t i = 0; i < t_.targetCount; ++i) {
            const Target& tg = t_.targets[i];
            if (tg.kind != kData) continue;
            if (tg.ptee != kPteeNone) {
                const int64_t p = pointee(tg, out);
                if (p >= 0) cells_.push_back({img_.pointerBase + uint64_t(p), 0, 0});
            }
            if (tg.guard == kGuardStr) strs_.push_back({tg.guardLen, tg.guardHash, 0, 0});
        }
        std::sort(cells_.begin(), cells_.end(), [](const Hit& a, const Hit& b) { return a.value < b.value; });
        cells_.erase(std::unique(cells_.begin(), cells_.end(), [](const Hit& a, const Hit& b) { return a.value == b.value; }), cells_.end());
        uint32_t maxLen = 0;
        for (const StrHit& s : strs_) maxLen = std::max(maxLen, s.len);
        constexpr uint32_t kChunk = 1u << 20;
        std::vector<uint8_t> chunk;
        for (const Sec& s : secs_) {
            if (s.flags & kScnExec) continue;
            const uint32_t n = std::min(s.span, s.rawSize);
            // Strings: NUL-delimited tokens, the last one ending at the section's end (Python's split keeps it).
            uint32_t tokStart = s.rva, tokLen = 0;
            bool printable = true;
            uint64_t h = kFnvOffset;
            auto endToken = [&]() {
                if (tokLen && printable && tokLen <= maxLen)
                    for (StrHit& sh : strs_)
                        if (sh.len == tokLen && sh.hash == h) { if (!sh.count++) sh.first = tokStart; }
            };
            for (uint32_t off = 0; off < n; off += kChunk) {
                const uint32_t m = std::min(kChunk, n - off);
                chunk.resize(m);
                if (!read(uint64_t(s.rva) + off, chunk.data(), m)) {
                    // An unreadable chunk breaks the token run and holds no cell.
                    tokLen = 0;
                    printable = true;
                    h = kFnvOffset;
                    tokStart = s.rva + off + m;
                    continue;
                }
                if (!cells_.empty()) {
                    uint32_t c = (8 - (s.rva + off) % 8) % 8;
                    for (; uint64_t(c) + 8 <= m; c += 8) {
                        uint64_t v = 0;
                        std::memcpy(&v, chunk.data() + c, 8);
                        auto it = std::lower_bound(cells_.begin(), cells_.end(), v, [](const Hit& a, uint64_t x) { return a.value < x; });
                        if (it != cells_.end() && it->value == v && !it->count++) it->first = s.rva + off + c;
                    }
                }
                if (!strs_.empty()) {
                    for (uint32_t b = 0; b < m; ++b) {
                        const uint8_t ch = chunk[b];
                        if (ch == 0) {
                            endToken();
                            tokStart = s.rva + off + b + 1;
                            tokLen = 0;
                            printable = true;
                            h = kFnvOffset;
                        } else {
                            ++tokLen;
                            printable = printable && ch >= 0x20 && ch <= 0x7E;
                            if (tokLen <= maxLen) h = (h ^ ch) * kFnvPrime;
                        }
                    }
                }
            }
            if (!strs_.empty()) endToken();
        }
    }
    const char* sectionNameAt(int64_t rva) const noexcept {
        for (const Sec& s : secs_)
            if (s.rva <= rva && rva < int64_t(s.rva) + s.span) return s.name;
        return nullptr;
    }
    bool guardOk(const Target& tg, int64_t rva) {
        if (tg.guard == kGuardNone) return true;
        const Sec* sec = nullptr;
        for (const Sec& s : secs_)
            if (s.rva <= rva && rva < int64_t(s.rva) + s.span) { sec = &s; break; }
        if (!sec) return false;
        const uint32_t room = uint32_t(int64_t(sec->rva) + sec->span - rva);
        if (tg.guard == kGuardStr) {
            const uint32_t want = tg.guardLen + 1;
            if (room < want || !readRva(*sec, uint32_t(rva), want, &scratch_)) return false;
            return scratch_[tg.guardLen] == 0 && fnv1a64(scratch_.data(), tg.guardLen) == tg.guardHash;
        }
        if (room < 8 || !readRva(*sec, uint32_t(rva), 8, &scratch_)) return false;
        uint64_t v = 0;
        std::memcpy(&v, scratch_.data(), 8);
        if (v < img_.pointerBase) return false;
        return at(int64_t(v - img_.pointerBase > 0xFFFFFFFFull ? -1 : int64_t(v - img_.pointerBase))) >= 0;
    }
    void resolveData(uint32_t i, Result* out) {
        const Target& tg = t_.targets[i];
        Result& r = out[i];
        r.reason = kDataRefFailed;
        uint8_t answered = 0;
        int64_t answer = -1;
        bool conflict = false, disagree = false;
        auto give = [&](uint8_t bit, int64_t v) {
            if (answered && answer != v) disagree = true;
            answer = v;
            answered |= bit;
        };
        if (tg.refCount) {
            std::vector<int64_t> values;
            refValues(tg.refFirst, tg.refCount, &values);
            if (values.size() == 1) give(kViaRef, values[0]);
            else if (!values.empty()) conflict = true;
        }
        if (tg.ptee != kPteeNone) {
            const int64_t p = pointee(tg, out);
            if (p >= 0) {
                const uint64_t want = img_.pointerBase + uint64_t(p);
                auto it = std::lower_bound(cells_.begin(), cells_.end(), want, [](const Hit& a, uint64_t x) { return a.value < x; });
                if (it != cells_.end() && it->value == want && it->count == 1) give(kViaPtr, it->first);
            }
        }
        if (tg.guard == kGuardStr) {
            for (const StrHit& sh : strs_)
                if (sh.len == tg.guardLen && sh.hash == tg.guardHash) {
                    if (sh.count == 1) give(kViaContent, sh.first);
                    break;
                }
        }
        if (!answered || conflict || disagree) return;
        const char* name = sectionNameAt(answer);
        const char* want = tg.sec < t_.sectionCount ? t_.sections[tg.sec].name : nullptr;
        if ((name == nullptr) != (want == nullptr) || (name && std::strcmp(name, want) != 0)) return;
        if (!guardOk(tg, answer)) return;
        r.rva = uint32_t(answer);
        r.via = answered;
        r.reason = kFound;
    }

    struct Hit { uint64_t value; uint32_t count, first; };
    struct StrHit { uint32_t len; uint64_t hash; uint32_t count, first; };

    const Table& t_;
    const ImageView& img_;
    uint64_t imageSize_ = 0x1000;
    uint32_t dirRva_[16] = {}, dirSize_[16] = {};
    std::vector<Sec> secs_;
    std::vector<uint32_t> st_, en_, order_;
    std::vector<uint8_t> leaf_, buf_, body_, scratch_;
    std::vector<int> state_;
    std::vector<std::vector<uint32_t>> found_;
    std::vector<Hit> cells_;
    std::vector<StrHit> strs_;
    Stats stats_;
};

}  // namespace detail

// Resolves every target of t in the image. out has t.targetCount entries. False (every target not found, reason kNoImage) when the
// headers are unreadable or not a PE32+ image, or when memory runs out.
inline bool resolveTable(const Table& t, const ImageView& img, Result* out, Stats* stats) noexcept {
    if (!out) return false;
    for (uint32_t i = 0; i < t.targetCount; ++i) out[i] = Result{};
    if (stats) *stats = Stats{};
    if (t.format != kCppFormat || !img.copy) return false;
    try {
        detail::Resolver r(t, img);
        if (r.run(out, stats)) return true;
    } catch (...) {
    }
    for (uint32_t i = 0; i < t.targetCount; ++i) out[i] = Result{};
    return false;
}

// ---- Pins on an unknown build --------------------------------------------------------------------------------------------------------
// The index of a found code target whose unit, in the known build, holds [knownRva, knownRva + n) whole; -1 when there is none.
inline int64_t coveringTarget(const Table& t, const Result* results, uint32_t knownRva, size_t n) noexcept {
    for (uint32_t i = 0; i < t.targetCount; ++i) {
        const Target& tg = t.targets[i];
        if (tg.kind != kCode || tg.unit >= t.unitCount || !results || results[i].reason != kFound) continue;
        const uint64_t start = uint64_t(tg.rva) - tg.off, end = start + t.units[tg.unit].len;
        if (knownRva >= start && uint64_t(knownRva) + n <= end) return i;
    }
    return -1;
}
// got (read at the running build's copy of knownRva) against pin (the known build's bytes there), inside covering target ti's unit:
// the bytes the unit's signature masks are skipped, every other byte must be equal.
inline bool maskedEqual(const Table& t, uint32_t ti, uint32_t knownRva, const uint8_t* got, const uint8_t* pin, size_t n) noexcept {
    if (ti >= t.targetCount || !got || !pin) return false;
    const Target& tg = t.targets[ti];
    if (tg.kind != kCode || tg.unit >= t.unitCount) return false;
    const Unit& u = t.units[tg.unit];
    const uint64_t start = uint64_t(tg.rva) - tg.off;
    if (knownRva < start || uint64_t(knownRva) + n > start + u.len) return false;
    const uint64_t base = knownRva - start;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t o = base + i;
        bool masked = false;
        for (uint32_t p = 0; p < u.maskPairs && !masked; ++p) {
            const uint64_t k = uint64_t(u.maskFirst + p) * 2;
            if (k + 1 >= t.maskCount) return false;
            masked = o >= t.mask[k] && o < uint64_t(t.mask[k]) + t.mask[k + 1];
        }
        if (!masked && got[i] != pin[i]) return false;
    }
    return true;
}

// ---- Log lines (game_build.cpp writes them; tools\game_build_test holds their text) ---------------------------------------------------
constexpr uint32_t kListCap = 16;   // addresses named per list

// "hash 170, ref 30, fp+nbr 6": the found targets by how they were found, most first.
inline std::string byHowText(const Table& t, const Result* results) {
    uint32_t count[64] = {};
    for (uint32_t i = 0; i < t.targetCount; ++i)
        if (results[i].reason == kFound) ++count[results[i].via & 63];
    std::string out;
    for (;;) {
        int best = -1;
        for (int v = 0; v < 64; ++v)
            if (count[v] && (best < 0 || count[v] > count[best])) best = v;
        if (best < 0) break;
        char via[48], item[80];
        viaText(static_cast<uint8_t>(best), via, sizeof(via));
        std::snprintf(item, sizeof(item), "%s%s %u", out.empty() ? "" : ", ", via, count[best]);
        out += item;
        count[best] = 0;
    }
    return out.empty() ? "none" : out;
}

// "0x8BEA20 (layout-only), 0x19B3F40 (ambiguous)": the targets not found, at most kListCap of them; *missing counts them all.
inline std::string notFoundText(const Table& t, const Result* results, uint32_t* missing) {
    std::string out;
    uint32_t n = 0;
    for (uint32_t i = 0; i < t.targetCount; ++i) {
        if (results[i].reason == kFound) continue;
        if (++n > kListCap) continue;
        char item[64];
        std::snprintf(item, sizeof(item), "%s0x%X (%s)", n > 1 ? ", " : "", static_cast<unsigned>(t.targets[i].rva), reasonText(results[i].reason));
        out += item;
    }
    if (n > kListCap) {
        char more[48];
        std::snprintf(more, sizeof(more), " and %u more", n - kListCap);
        out += more;
    }
    if (missing) *missing = n;
    return out;
}

inline std::string statsText(const Stats& s, double ms) {
    char text[160];
    std::snprintf(text, sizeof(text), "%.1f ms over %u units, %u candidates, %llu bytes hashed", ms, static_cast<unsigned>(s.units),
                  static_cast<unsigned>(s.candidates), static_cast<unsigned long long>(s.bytes));
    return text;
}

// The one line an unknown build's resolve writes, after "game build: ".
inline std::string resolveSummaryText(const Table& t, const Result* results, const Stats& s, bool ok, double ms, const char* build) {
    char head[256];
    if (!ok) {
        std::snprintf(head, sizeof(head), "%s: the running image could not be read as a PE image; no game address was found by "
                      "signature (%.1f ms). Every hook that needs one stands down.", build, ms);
        return head;
    }
    uint32_t missing = 0;
    const std::string absent = notFoundText(t, results, &missing);
    std::snprintf(head, sizeof(head), "%s: found %u of %u game addresses by signature (", build, static_cast<unsigned>(s.found),
                  static_cast<unsigned>(t.targetCount));
    return head + byHowText(t, results) + ") in " + statsText(s, ms) + (missing ? "; not found: " + absent : std::string());
}

// The one line the update check (advanced.game_update_check) writes, after "game update check: ": on the known build, whether every
// address the resolver found is the table's own. jumps: how many of the targets not found now start with a jmp (a hook installed first).
inline std::string updateCheckText(const Table& t, const Result* results, const Stats& s, bool ok, double ms, uint32_t jumps) {
    if (!ok) {
        char text[128];
        std::snprintf(text, sizeof(text), "the running image could not be read as a PE image (%.1f ms); nothing was checked.", ms);
        return text;
    }
    uint32_t differ = 0;
    std::string differList;
    for (uint32_t i = 0; i < t.targetCount; ++i) {
        if (results[i].reason != kFound || results[i].rva == t.targets[i].rva) continue;
        if (++differ > kListCap) continue;
        char via[48], item[96];
        viaText(results[i].via, via, sizeof(via));
        std::snprintf(item, sizeof(item), "%s0x%X found at 0x%X (%s)", differ > 1 ? ", " : "", static_cast<unsigned>(t.targets[i].rva),
                      static_cast<unsigned>(results[i].rva), via);
        differList += item;
    }
    if (differ > kListCap) differList += " and more";
    uint32_t missing = 0;
    const std::string absent = notFoundText(t, results, &missing);
    char head[96];
    std::snprintf(head, sizeof(head), "%u of %u found, ", static_cast<unsigned>(s.found), static_cast<unsigned>(t.targetCount));
    std::string out = head;
    if (differ) {
        char d[32];
        std::snprintf(d, sizeof(d), "%u differ: ", static_cast<unsigned>(differ));
        out += d + differList;
    } else {
        out += "all equal";
    }
    if (missing) out += "; not found: " + absent;
    if (jumps) {
        char j[112];
        std::snprintf(j, sizeof(j), "; %u of those now start with a jmp (a hook installed before the check read it)", static_cast<unsigned>(jumps));
        out += j;
    }
    return out + " (resolve " + statsText(s, ms) + ")";
}

}  // namespace gamesig
}  // namespace edvr
