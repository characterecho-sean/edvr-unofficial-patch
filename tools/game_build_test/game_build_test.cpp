// game_build_test: the runtime signature resolver (src\common\game_build_core.h) against its reference, tools\hook_signatures.py.
//
//   (a) The self-test's synthetic exes (hook_signatures.py fixture writes them, their table as C++ and Python's answers), each mapped
//       into memory as the loader maps an image (sections at their virtual addresses, base relocations applied for this address, the
//       import address table bound to stand-in pointers): the named cases -- a function moved with its rel32 and RIP fields changed, a
//       changed struct offset, clones told apart by a caller, by fingerprints (callee, read-only constant, fall-through) or not at
//       all, data by reference, pointer cell and string, the refusals -- the masked pin compare, and an unreadable page.
//   (c) On every fixture exe, every target's answer (address, how, why not) equals Python's.
//   (b) When the archived executables are at the main checkout's analysis\ (or EDVR_ANALYSIS_DIR), each is mapped the same way and
//       resolved with the committed table (src\common\game_signatures.inc) against tools\game_build_test\expect_<build>.txt (the
//       ground truth and Python's answers): 0 WRONG, Python's EQUAL count, the same answer per target; the known build resolves to
//       itself. Without them it prints "skipped: no archived exes" and passes.
//
// Usage: game_build_test.exe --self-test <fixture dir> <repository root>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/common/game_build_core.h"
#include "../../src/d3d11/explorer_cam_core.h"

namespace fixture {
using namespace edvr::gamesig;
#include "fixture_signatures.inc"
}  // namespace fixture

namespace known {
using namespace edvr::gamesig;
#include "../../src/common/game_signatures.inc"
}  // namespace known

namespace gs = edvr::gamesig;

// The table's known build is the build every hook's constants are for: a retarget that moves one and not the other fails here.
static_assert(known::kTable.knownStamp == edvr::ecm::kExpectedTimestamp && known::kTable.knownImageSize == edvr::ecm::kExpectedImageSize,
              "src\\common\\game_signatures.inc is not generated from the build explorer_cam_core.h names; regenerate it after the retarget");

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

bool readFile(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0) return false;
    out->resize(size_t(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out->data()), n);
    return bool(f);
}

uint32_t u32(const std::vector<uint8_t>& d, size_t off) {
    uint32_t v = 0;
    if (off + 4 <= d.size()) std::memcpy(&v, d.data() + off, 4);
    return v;
}
uint16_t u16(const std::vector<uint8_t>& d, size_t off) {
    uint16_t v = 0;
    if (off + 2 <= d.size()) std::memcpy(&v, d.data() + off, 2);
    return v;
}

// PE stamp and SizeOfImage of a file's bytes (or of its first page).
bool fileIdentity(const std::vector<uint8_t>& d, uint32_t* stamp, uint32_t* size) {
    if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') return false;
    const uint32_t nt = u32(d, 0x3C);
    if (nt + 0x54 > d.size() || std::memcmp(d.data() + nt, "PE\0\0", 4) != 0) return false;
    *stamp = u32(d, nt + 8);
    *size = u32(d, nt + 0x50);
    return true;
}

// An exe file mapped as the loader maps an image: VirtualAlloc'd, sections at their virtual addresses, relocated to this address,
// the IAT's slots bound to stand-in pointers. Never executed (the memory is never made executable).
struct Mapped {
    uint8_t* base = nullptr;
    uint32_t size = 0;
    ~Mapped() {
        if (base) VirtualFree(base, 0, MEM_RELEASE);
    }
};

bool mapImage(const std::vector<uint8_t>& d, Mapped* m) {
    uint32_t stamp = 0, sizeOfImage = 0;
    if (!fileIdentity(d, &stamp, &sizeOfImage) || sizeOfImage == 0) return false;
    const uint32_t nt = u32(d, 0x3C), opt = nt + 24;
    const uint16_t count = u16(d, nt + 6), optSize = u16(d, nt + 20);
    uint32_t align = u32(d, opt + 32);
    if (align == 0) align = 0x1000;
    uint64_t preferred = 0;
    std::memcpy(&preferred, d.data() + opt + 24, 8);
    const uint32_t headers = std::min<uint32_t>(std::max<uint32_t>(u32(d, opt + 60), 0x400), uint32_t(d.size()));
    m->base = static_cast<uint8_t*>(VirtualAlloc(nullptr, sizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!m->base) return false;
    m->size = sizeOfImage;
    std::memcpy(m->base, d.data(), std::min(headers, sizeOfImage));
    for (uint32_t i = 0; i < count; ++i) {
        const size_t s = size_t(opt) + optSize + 40u * i;
        const uint32_t vsize = u32(d, s + 8), rva = u32(d, s + 12), rawSize = u32(d, s + 16), raw = u32(d, s + 20);
        const uint32_t extent = vsize ? (vsize + align - 1) / align * align : rawSize;
        uint64_t n = std::min(rawSize, extent);
        if (uint64_t(rva) + n > sizeOfImage) n = rva < sizeOfImage ? sizeOfImage - rva : 0;
        if (uint64_t(raw) + n > d.size()) n = raw < d.size() ? d.size() - raw : 0;
        if (n) std::memcpy(m->base + rva, d.data() + raw, size_t(n));
    }
    const uint32_t dirs = u32(d, opt + 108);
    auto dir = [&](uint32_t k, uint32_t* rva, uint32_t* sz) {
        *rva = k < dirs ? u32(d, opt + 112 + 8 * k) : 0;
        *sz = k < dirs ? u32(d, opt + 116 + 8 * k) : 0;
    };
    // Base relocations, as the loader applies them for a load at m->base.
    uint32_t relRva = 0, relSize = 0;
    dir(5, &relRva, &relSize);
    const uint64_t delta = uint64_t(reinterpret_cast<uintptr_t>(m->base)) - preferred;
    for (uint32_t off = 0; relRva && off + 8 <= relSize && uint64_t(relRva) + relSize <= sizeOfImage;) {
        uint32_t page = 0, block = 0;
        std::memcpy(&page, m->base + relRva + off, 4);
        std::memcpy(&block, m->base + relRva + off + 4, 4);
        if (block < 8) break;
        for (uint32_t k = 0; k < (block - 8) / 2; ++k) {
            uint16_t e = 0;
            std::memcpy(&e, m->base + relRva + off + 8 + 2 * k, 2);
            const uint32_t at = page + (e & 0xFFF);
            if ((e >> 12) == 10 && uint64_t(at) + 8 <= sizeOfImage) {
                uint64_t v = 0;
                std::memcpy(&v, m->base + at, 8);
                v += delta;
                std::memcpy(m->base + at, &v, 8);
            } else if ((e >> 12) == 3 && uint64_t(at) + 4 <= sizeOfImage) {
                uint32_t v = 0;
                std::memcpy(&v, m->base + at, 4);
                v += uint32_t(delta);
                std::memcpy(m->base + at, &v, 4);
            }
        }
        off += block;
    }
    // The import address table, bound: every non-zero slot now holds a function pointer outside the image.
    uint32_t iatRva = 0, iatSize = 0;
    dir(12, &iatRva, &iatSize);
    for (uint32_t off = 0; iatRva && off + 8 <= iatSize && uint64_t(iatRva) + off + 8 <= sizeOfImage; off += 8) {
        uint64_t v = 0;
        std::memcpy(&v, m->base + iatRva + off, 8);
        if (v) {
            v = 0x00007FF812340000ull + off * 0x10ull;
            std::memcpy(m->base + iatRva + off, &v, 8);
        }
    }
    return true;
}

bool sehCopy(const void* ctx, uint32_t rva, void* dst, size_t n) noexcept {
    __try {
        std::memcpy(dst, static_cast<const uint8_t*>(ctx) + rva, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

gs::ImageView viewOf(const Mapped& m) {
    return gs::ImageView{m.base, &sehCopy, uint64_t(reinterpret_cast<uintptr_t>(m.base))};
}

double nowMs() {
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1000.0 / double(f.QuadPart);
}

// ---- Expectation files (hook_signatures.py check --emit-expect, fixture) ------------------------------------------------------------
struct Row {
    uint32_t rva = 0;
    std::string kind, via, reason;
    int64_t gt = -1, py = -1;
};
struct Expect {
    uint32_t knownStamp = 0, knownSize = 0, exeStamp = 0, exeSize = 0;
    uint64_t table = 0;
    std::string file;
    std::vector<Row> rows;
    std::vector<std::pair<uint32_t, int64_t>> probes;   // "u" lines: an RVA off the table and Python's unit_rva for it (-1: none)
    bool ok = false;
};

int64_t hexOrNone(const std::string& s) { return s == "-" ? -1 : int64_t(std::stoull(s, nullptr, 16)); }

Expect readExpect(const std::string& path) {
    Expect e;
    std::ifstream f(path);
    std::string line;
    if (!f || !std::getline(f, line) || line.rfind("# hook_signatures expectations", 0) != 0) return e;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string w;
        in >> w;
        if (w == "known") {
            std::string a, b;
            in >> a >> b;
            e.knownStamp = uint32_t(hexOrNone(a));
            e.knownSize = uint32_t(hexOrNone(b));
        } else if (w == "exe") {
            std::string a, b;
            in >> a >> b;
            e.exeStamp = uint32_t(hexOrNone(a));
            e.exeSize = uint32_t(hexOrNone(b));
        } else if (w == "table") {
            std::string a;
            in >> a;
            e.table = uint64_t(hexOrNone(a));
        } else if (w == "file") {
            in >> e.file;
        } else if (w == "r") {
            Row r;
            std::string rva, gt, py;
            in >> rva >> r.kind >> gt >> py >> r.via >> r.reason;
            r.rva = uint32_t(hexOrNone(rva));
            r.gt = hexOrNone(gt);
            r.py = hexOrNone(py);
            e.rows.push_back(r);
        } else if (w == "u") {
            std::string rva, py;
            in >> rva >> py;
            e.probes.emplace_back(uint32_t(hexOrNone(rva)), hexOrNone(py));
        }
    }
    e.ok = !e.rows.empty();
    return e;
}

// Every unit probe's C++ answer (gamesig::unitRva: an RVA inside a found unit keeps its offset) against Python's unit_rva; returns how
// many agree, prints the first few that do not.
uint32_t unitAgreement(const gs::Table& t, const gs::Result* res, const Expect& e, const char* label) {
    uint32_t agree = 0, shown = 0;
    for (const auto& p : e.probes) {
        const uint32_t got = gs::unitRva(t, res, p.first);
        const int64_t mine = got ? int64_t(got) : -1;
        if (mine == p.second) {
            ++agree;
        } else if (shown++ < 8) {
            std::printf("  %s: unit probe 0x%X: C++ 0x%X, Python %lld\n", label, p.first, got, static_cast<long long>(p.second));
        }
    }
    return agree;
}

// gameRva's sorted index (gamesig::RvaIndex) against unitRva's scan: the same answer for every target, the bytes either side of it, each
// code unit's edges, the given probes and the image's ends. Returns how many RVAs were asked; *bad gets how many disagreed.
uint32_t indexAgreement(const gs::Table& t, const gs::Result* res, const Expect* e, uint32_t* bad) {
    gs::RvaIndex idx;
    idx.build(t, res);
    uint32_t asked = 0, shown = 0;
    *bad = 0;
    auto one = [&](uint32_t k) {
        ++asked;
        const uint32_t a = idx.rva(k), b = gs::unitRva(t, res, k);
        if (a != b) {
            ++*bad;
            if (shown++ < 8) std::printf("  index 0x%X: RvaIndex 0x%X, unitRva 0x%X\n", k, a, b);
        }
    };
    for (uint32_t i = 0; i < t.targetCount; ++i) {
        const gs::Target& tg = t.targets[i];
        one(tg.rva);
        one(tg.rva + 1);
        if (tg.rva) one(tg.rva - 1);
        if (tg.kind == gs::kCode && tg.unit < t.unitCount && tg.off <= tg.rva) {
            const uint32_t start = tg.rva - tg.off, len = t.units[tg.unit].len;
            one(start);
            if (start) one(start - 1);
            if (len) one(start + len - 1);
            one(start + len);
        }
    }
    if (e)
        for (const auto& p : e->probes) one(p.first);
    one(0);
    one(0xFFFFFFFFu);
    return asked;
}

// Every target's answer against Python's; returns how many agree, prints the first few that do not.
uint32_t agreement(const gs::Table& t, const gs::Result* res, const Expect& e, const char* label) {
    uint32_t agree = 0, shown = 0;
    if (e.rows.size() != t.targetCount) {
        std::printf("FAIL: %s: %zu expectation rows for %u targets\n", label, e.rows.size(), t.targetCount);
        return 0;
    }
    for (uint32_t i = 0; i < t.targetCount; ++i) {
        const Row& row = e.rows[i];
        char via[48];
        gs::viaText(res[i].via, via, sizeof(via));
        const int64_t got = res[i].reason == gs::kFound ? int64_t(res[i].rva) : -1;
        const bool same = row.rva == t.targets[i].rva && got == row.py && row.via == (got >= 0 ? via : "-") &&
                          row.reason == gs::reasonText(res[i].reason);
        if (same) {
            ++agree;
        } else if (shown++ < 8) {
            char cText[24] = "-", pyText[24] = "-";
            if (got >= 0) std::snprintf(cText, sizeof(cText), "0x%llX", static_cast<unsigned long long>(got));
            if (row.py >= 0) std::snprintf(pyText, sizeof(pyText), "0x%llX", static_cast<unsigned long long>(row.py));
            std::printf("  %s: 0x%X: C++ %s via %s (%s), Python %s via %s (%s)\n", label, t.targets[i].rva, cText, via,
                        gs::reasonText(res[i].reason), pyText, row.via.c_str(), row.reason.c_str());
        }
    }
    return agree;
}

// ---- (a) and (c): the fixture -------------------------------------------------------------------------------------------------------
struct Names {
    std::map<std::string, uint32_t> k, n, b;   // known, unknown, broken
};

Names readNames(const std::string& path) {
    Names names;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string w, name, a, b, c;
        in >> w >> name >> a >> b >> c;
        if (w != "name") continue;
        if (a != "-") names.k[name] = uint32_t(hexOrNone(a));
        if (b != "-") names.n[name] = uint32_t(hexOrNone(b));
        if (c != "-") names.b[name] = uint32_t(hexOrNone(c));
    }
    return names;
}

int64_t indexOf(const gs::Table& t, uint32_t rva) {
    for (uint32_t i = 0; i < t.targetCount; ++i)
        if (t.targets[i].rva == rva) return i;
    return -1;
}

std::string viaOf(const gs::Result& r) {
    char via[48];
    gs::viaText(r.via, via, sizeof(via));
    return via;
}

void fixtureChecks(const std::string& dir) {
    const gs::Table& t = fixture::kTable;
    const Names nm = readNames(dir + "\\fixture_names.txt");
    check(nm.k.count("F1") && nm.n.count("Z1"), "fixture names read");
    std::vector<gs::Result> res[3];
    gs::Stats stats[3];
    Mapped maps[3];
    const char* exes[3] = {"known", "unknown", "broken"};
    for (int e = 0; e < 3; ++e) {
        const Expect ex = readExpect(dir + "\\expect_" + exes[e] + ".txt");
        std::vector<uint8_t> bytes;
        check(ex.ok && ex.table == t.tableHash, "fixture expectations are for this fixture table");
        check(readFile(dir + "\\" + ex.file, &bytes) && mapImage(bytes, &maps[e]), "fixture exe mapped as an image");
        res[e].assign(t.targetCount, gs::Result{});
        gs::Stats& st = stats[e];
        check(gs::resolveTable(t, viewOf(maps[e]), res[e].data(), &st) && st.imageOk && st.units > 0, "fixture resolves");
        const uint32_t agree = agreement(t, res[e].data(), ex, exes[e]);
        std::printf("game_build_test: fixture %-7s %u targets, %u found, C++ and Python agree on %u (%u units, %u leaf, %u candidates)\n",
                    exes[e], t.targetCount, st.found, agree, st.units, st.leaf, st.candidates);
        check(agree == t.targetCount, "(c) C++ and Python resolve the fixture alike");
        const uint32_t unitAgree = unitAgreement(t, res[e].data(), ex, exes[e]);
        std::printf("game_build_test: fixture %-7s %zu unit probes, C++ and Python agree on %u\n", exes[e], ex.probes.size(), unitAgree);
        check(!ex.probes.empty() && unitAgree == ex.probes.size(), "(c) C++ and Python place RVAs inside a unit alike");
        uint32_t indexBad = 0;
        const uint32_t indexAsked = indexAgreement(t, res[e].data(), &ex, &indexBad);
        check(indexAsked > 0 && indexBad == 0, "(c) gameRva's sorted index answers as unitRva's scan does");
    }
    // The process check: only a main module named EliteDangerous64.exe is the game (game_build.cpp resolves nothing anywhere else).
    check(gs::isGameExecutable(L"C:\\Games\\Elite\\EliteDangerous64.exe") && gs::isGameExecutable(L"eLiTeDaNgErOuS64.EXE") &&
              gs::isGameExecutable(L"D:/steam/EliteDangerous64.exe"),
          "the game's executable is EliteDangerous64.exe, in any case, under any folder");
    check(!gs::isGameExecutable(L"C:\\Games\\Elite\\EDVR-Setup.exe") && !gs::isGameExecutable(L"EliteDangerous64.exe.bak") &&
              !gs::isGameExecutable(L"EliteDangerous64.ex") && !gs::isGameExecutable(L"C:\\EliteDangerous64.exe\\tool.exe") &&
              !gs::isGameExecutable(L"") && !gs::isGameExecutable(nullptr),
          "any other main module is not the game");
    auto R = [&](int e, const char* name) -> const gs::Result& {
        static gs::Result none;
        const int64_t i = indexOf(t, nm.k.at(name));
        return i >= 0 ? res[e][size_t(i)] : none;
    };
    auto at = [&](int e, const char* name) { return e == 0 ? nm.k.at(name) : e == 1 ? nm.n.at(name) : nm.b.at(name); };
    // The known build resolves to itself.
    bool self = true;
    for (uint32_t i = 0; i < t.targetCount; ++i)
        if (res[0][i].reason == gs::kFound && res[0][i].rva != t.targets[i].rva) self = false;
    check(self, "(a) known build resolves to itself");
    // The unknown build.
    check(R(1, "F1").rva == at(1, "F1") && viaOf(R(1, "F1")) == "hash", "(a) F1 moved with changed rel32, RIP and relocated fields");
    check(R(1, "F1+lea").rva == at(1, "F1") + 9, "(a) an offset inside the unit is kept");
    check(R(1, "F2").reason == gs::kNoCandidate, "(a) a changed struct offset is not found");
    check(R(1, "LEAF").rva == at(1, "LEAF") && R(1, "J").rva == at(1, "J") && R(1, "T").rva == at(1, "T"),
          "(a) leaf unit, inline jump table and trailing RVA table resolve");
    check(R(1, "C1").rva == at(1, "C1") && viaOf(R(1, "C1")) == "nbr+ref", "(a) clones told apart by a caller (and neighbours)");
    check(R(1, "Q1").reason == gs::kLayoutOnly, "(a) clones without fingerprints, named only by neighbours, are refused");
    for (const char* name : {"R1", "K1", "Z1"})
        check(R(1, name).rva == at(1, name) && viaOf(R(1, name)) == "fp+nbr", "(a) clones told apart by fingerprints");
    check(R(1, "W1").rva == at(1, "W1c"), "(a) a removed function resolves to its exact copy (the danger the survey measures)");
    check(R(1, "D1").rva == at(1, "D1") && R(1, "D2").rva == at(1, "D2"), "(a) data by reference");
    check(R(1, "D4").rva == at(1, "D4") && viaOf(R(1, "D4")) == "ptr" && R(1, "D8").rva == at(1, "D8"),
          "(a) data by its pointer cell (relocated in memory)");
    check(R(1, "D7").rva == at(1, "D7") && viaOf(R(1, "D7")) == "content", "(a) data by its string");
    check(viaOf(R(1, "D5")) == "content+ref" && viaOf(R(1, "D6")) == "ptr+ref", "(a) two mechanisms that agree");
    check(R(1, "D3").reason == gs::kDataRefFailed, "(a) data with no way in is not found");
    // The broken build.
    check(R(2, "C1").reason == gs::kAmbiguous && R(2, "R1").reason == gs::kAmbiguous,
          "(a) witnesses that name different clones are refused");
    bool dataRefused = true;
    for (const char* name : {"D4", "D5", "D6", "D7", "D8"}) dataRefused = dataRefused && R(2, name).reason == gs::kDataRefFailed;
    check(dataRefused, "(a) changed strings and null cells are refused");

    // The masked pin compare: F1's first 16 bytes hold its call rel32 (offset 5) and its lea disp32 (offset 12).
    const uint32_t f1 = nm.k.at("F1");
    uint8_t pin[16], got[16];
    std::memcpy(pin, maps[0].base + f1, 16);
    std::memcpy(got, maps[1].base + R(1, "F1").rva, 16);
    const int64_t cover = gs::coveringTarget(t, res[1].data(), f1, 16);
    check(cover >= 0 && std::memcmp(got, pin, 16) != 0, "(a) pin: the moved F1's bytes differ from the pin in the masked fields");
    check(cover >= 0 && gs::maskedEqual(t, uint32_t(cover), f1, got, pin, 16), "(a) pin: masked compare accepts the moved F1");
    got[5] ^= 0x55;
    check(cover >= 0 && gs::maskedEqual(t, uint32_t(cover), f1, got, pin, 16), "(a) pin: a masked byte is not compared");
    got[0] ^= 0x01;
    check(cover >= 0 && !gs::maskedEqual(t, uint32_t(cover), f1, got, pin, 16), "(a) pin: an unmasked byte must be equal");
    check(gs::coveringTarget(t, res[1].data(), f1 + 0x50, 32) < 0, "(a) pin: a range past the unit is not covered");
    check(gs::coveringTarget(t, res[1].data(), nm.k.at("F2"), 4) < 0, "(a) pin: a unit that was not matched covers nothing");
    // Addresses inside a matched unit, off the table (gameRva's in-unit path) and the masked compare over the same range.
    check(gs::unitRva(t, res[1].data(), f1 + 3) == at(1, "F1") + 3, "(a) an RVA inside a found unit keeps its offset");
    check(gs::unitRva(t, res[1].data(), nm.k.at("F2") + 1) == 0 && gs::unitRva(t, res[1].data(), 0x10) == 0,
          "(a) an RVA in a unit not found, or in none, has no answer");
    std::memcpy(got, maps[1].base + R(1, "F1").rva, 16);
    check(gs::maskedRangeEqual(t, res[1].data(), f1, got, pin, 16) && gs::coverRange(t, res[1].data(), f1, 16),
          "(a) pin: the range compare accepts the moved F1");
    {
        // Two fragments of one function (chained .pdata units) and a unit with two targets.
        const gs::Unit units[2] = {{16, 0, 0, 0}, {8, 0, 0, 0}};
        const gs::Target targets[3] = {{0x1000, gs::kCode, gs::kNoSection, 0, 0, 0, 0},
                                       {0x1010, gs::kCode, gs::kNoSection, 0, 0, 1, 0},
                                       {0x1004, gs::kCode, gs::kNoSection, 0, 0, 0, 4}};
        const uint32_t mask[2] = {0, 0};
        gs::Table syn{};
        syn.format = gs::kCppFormat;
        syn.units = units;
        syn.unitCount = 2;
        syn.mask = mask;
        syn.maskCount = 2;
        syn.targets = targets;
        syn.targetCount = 3;
        auto found = [](uint32_t rva) { gs::Result r; r.rva = rva; r.reason = gs::kFound; return r; };
        const gs::Result together[3] = {found(0x2000), found(0x2010), found(0x2004)};
        const gs::Result apart[3] = {found(0x2000), found(0x3000), found(0x2004)};
        const gs::Result split[3] = {found(0x2000), found(0x2010), found(0x5004)};
        uint32_t pieces = 0;
        const bool two = gs::coverRange(syn, together, 0x1008, 0x10, [&](uint32_t, uint32_t, size_t) { ++pieces; return true; });
        check(two && pieces == 2, "(a) a range over two fragments that moved together is covered, in two pieces");
        check(!gs::coverRange(syn, apart, 0x1008, 0x10), "(a) fragments that moved apart do not cover a range");
        check(!gs::coverRange(syn, together, 0x1008, 0x11), "(a) a range past the last unit is not covered");
        check(gs::unitRva(syn, together, 0x1014) == 0x2014 && gs::unitRva(syn, apart, 0x1014) == 0x3004,
              "(a) the second fragment's RVAs follow it");
        check(gs::unitRva(syn, split, 0x1002) == 0, "(a) targets of one unit that disagree give no answer");
        uint32_t bad[3] = {};
        indexAgreement(syn, together, nullptr, &bad[0]);
        indexAgreement(syn, apart, nullptr, &bad[1]);
        indexAgreement(syn, split, nullptr, &bad[2]);
        gs::RvaIndex idx;
        idx.build(syn, split);
        check(bad[0] == 0 && bad[1] == 0 && bad[2] == 0 && idx.rva(0x1002) == 0 && idx.rva(0x1014) == 0x2014,
              "(a) the sorted index: fragments, two targets of one unit and a unit split by its targets, as unitRva");
    }

    // An unreadable page: a refusal, never a crash. The fixture's code is all in its first .text page.
    DWORD old = 0;
    const uint32_t page = R(1, "F1").rva & ~0xFFFu;
    check(VirtualProtect(maps[1].base + page, 0x1000, PAGE_NOACCESS, &old) != 0, "(a) a page made unreadable");
    std::vector<gs::Result> dark(t.targetCount);
    gs::Stats st{};
    const bool ran = gs::resolveTable(t, viewOf(maps[1]), dark.data(), &st);
    VirtualProtect(maps[1].base + page, 0x1000, old, &old);
    const int64_t f1i = indexOf(t, f1), d7i = indexOf(t, nm.k.at("D7"));
    check(ran && f1i >= 0 && dark[size_t(f1i)].reason != gs::kFound && d7i >= 0 && dark[size_t(d7i)].rva == at(1, "D7"),
          "(a) code on an unreadable page is not found; readable data still is");
    // Not an image at all.
    std::vector<uint8_t> junk(0x2000, 0x90);
    Mapped jm;
    jm.base = static_cast<uint8_t*>(VirtualAlloc(nullptr, junk.size(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (jm.base) std::memcpy(jm.base, junk.data(), junk.size());
    gs::Result jr[64];
    check(jm.base && !gs::resolveTable(t, viewOf(jm), jr, nullptr) && jr[0].reason == gs::kNoImage, "(a) a non-image is refused");
    char via[48];
    gs::viaText(gs::kViaFp | gs::kViaNbr | gs::kViaRef, via, sizeof(via));
    check(std::strcmp(via, "fp+nbr+ref") == 0, "via text sorted as Python joins it");

    // The log lines, as game_build.cpp writes them after "game build: " and "game update check: ".
    char want[192];
    const std::string allEqual = gs::updateCheckText(t, res[0].data(), stats[0], true, 1.5, 0);
    std::snprintf(want, sizeof(want), "%u of %u found, all equal; not found: ", stats[0].found, t.targetCount);
    check(allEqual.rfind(want, 0) == 0 && allEqual.find("(layout-only)") != std::string::npos &&
              allEqual.find("(data-ref-failed)") != std::string::npos && allEqual.find(" (resolve 1.5 ms over ") != std::string::npos &&
              allEqual.find("bytes hashed)") == allEqual.size() - 13 && allEqual.find("jmp") == std::string::npos,
          "update check line: the known build, every found address equal");
    std::printf("game_build_test: update check line on the known fixture: game update check: %s\n", allEqual.c_str());
    const std::string moved = gs::updateCheckText(t, res[1].data(), stats[1], true, 2.0, 2);
    std::snprintf(want, sizeof(want), "%u of %u found, ", stats[1].found, t.targetCount);
    char first[64];
    std::snprintf(first, sizeof(first), "differ: 0x%X found at 0x%X (hash), ", nm.k.at("F1"), at(1, "F1"));
    check(moved.rfind(want, 0) == 0 && moved.find(first) != std::string::npos && moved.find("all equal") == std::string::npos &&
              moved.find("; 2 of those now start with a jmp") != std::string::npos,
          "update check line: addresses that differ are listed with how they were found");
    check(gs::updateCheckText(t, res[1].data(), stats[1], false, 3.0, 0).rfind("the running image could not be read", 0) == 0,
          "update check line: an unreadable image");
    const std::string summary = gs::resolveSummaryText(t, res[1].data(), stats[1], true, 4.0, "unknown build (test)");
    std::snprintf(want, sizeof(want), "unknown build (test): found %u of %u game addresses by signature (hash ", stats[1].found, t.targetCount);
    check(summary.rfind(want, 0) == 0 && summary.find(") in 4.0 ms over ") != std::string::npos &&
              summary.find("; not found: ") != std::string::npos,
          "resolve line: found N of M, by how, the time, the ones not found");
    std::printf("game_build_test: resolve line on the unknown fixture: game build: %s\n", summary.c_str());
}

// ---- (b): the archived executables --------------------------------------------------------------------------------------------------
std::string parentDir(const std::string& p) {
    const size_t k = p.find_last_of("\\/");
    return k == std::string::npos ? std::string() : p.substr(0, k);
}

std::string fullPath(const std::string& p) {
    char out[MAX_PATH * 2];
    const DWORD n = GetFullPathNameA(p.c_str(), sizeof(out), out, nullptr);
    return n && n < sizeof(out) ? std::string(out) : p;
}

// The main checkout's analysis\: EDVR_ANALYSIS_DIR, or beside the git common directory of root (a worktree's .git file names its
// gitdir, whose commondir names the main .git).
std::string analysisDir(const std::string& root) {
    char env[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableA("EDVR_ANALYSIS_DIR", env, sizeof(env));
    if (n && n < sizeof(env)) return env;
    const std::string dotGit = root + "\\.git";
    const DWORD attr = GetFileAttributesA(dotGit.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return std::string();
    std::string common = dotGit;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::ifstream f(dotGit);
        std::string line;
        std::getline(f, line);
        if (line.rfind("gitdir:", 0) != 0) return std::string();
        std::string gitdir = line.substr(7);
        gitdir.erase(0, gitdir.find_first_not_of(" \t"));
        while (!gitdir.empty() && (gitdir.back() == '\r' || gitdir.back() == ' ')) gitdir.pop_back();
        std::ifstream cd(gitdir + "\\commondir");
        std::string rel;
        common = std::getline(cd, rel) ? fullPath(gitdir + "\\" + rel) : gitdir;
        while (!common.empty() && (common.back() == '\r' || common.back() == '\\' || common.back() == '/')) common.pop_back();
    }
    return parentDir(fullPath(common)) + "\\analysis";
}

struct Archived {
    std::string path;
    uint32_t stamp = 0, size = 0;
};

std::vector<Archived> listExes(const std::string& dir) {
    std::vector<Archived> out;
    WIN32_FIND_DATAA fd{};
    const HANDLE h = FindFirstFileA((dir + "\\*.exe").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        Archived a;
        a.path = dir + "\\" + fd.cFileName;
        std::ifstream f(a.path, std::ios::binary);
        std::vector<uint8_t> head(0x1000);
        f.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()));
        head.resize(size_t(f.gcount()));
        if (fileIdentity(head, &a.stamp, &a.size)) out.push_back(a);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

const Archived* findExe(const std::vector<Archived>& exes, uint32_t stamp, uint32_t size) {
    for (const Archived& a : exes)
        if (a.stamp == stamp && a.size == size) return &a;
    return nullptr;
}

bool resolveFile(const std::string& path, const gs::Table& t, std::vector<gs::Result>* res, gs::Stats* st, double* ms) {
    std::vector<uint8_t> bytes;
    Mapped m;
    if (!readFile(path, &bytes) || !mapImage(bytes, &m)) return false;
    bytes.clear();
    bytes.shrink_to_fit();
    res->assign(t.targetCount, gs::Result{});
    const double a = nowMs();
    const bool ok = gs::resolveTable(t, viewOf(m), res->data(), st);
    *ms = nowMs() - a;
    return ok;
}

void archivedChecks(const std::string& root) {
    const gs::Table& t = known::kTable;
    const std::string dir = analysisDir(root);
    const std::vector<Archived> exes = dir.empty() ? std::vector<Archived>() : listExes(dir);
    if (exes.empty()) {
        std::printf("game_build_test: skipped: no archived exes (looked in %s)\n", dir.empty() ? "(no analysis directory)" : dir.c_str());
        return;
    }
    // The known build against itself: what advanced.game_update_check reports on the real game.
    if (const Archived* k = findExe(exes, t.knownStamp, t.knownImageSize)) {
        std::vector<gs::Result> res;
        gs::Stats st{};
        double ms = 0;
        const bool ok = resolveFile(k->path, t, &res, &st, &ms);
        uint32_t equal = 0;
        for (uint32_t i = 0; ok && i < t.targetCount; ++i) equal += res[i].reason == gs::kFound && res[i].rva == t.targets[i].rva;
        std::printf("game_build_test: build %s (known) against itself: %u of %u equal; resolve %.1f ms (%u units, %u candidates, %llu bytes)\n",
                    t.knownBuild, equal, t.targetCount, ms, st.units, st.candidates, static_cast<unsigned long long>(st.bytes));
        check(ok && equal == t.targetCount, "(b) the known build resolves to itself");
    } else {
        std::printf("game_build_test: skipped the known build %s: not in %s\n", t.knownBuild, dir.c_str());
    }
    const std::string tests = root + "\\tools\\game_build_test";
    WIN32_FIND_DATAA fd{};
    const HANDLE h = FindFirstFileA((tests + "\\expect_*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        check(false, "(b) tools\\game_build_test\\expect_*.txt are there");
        return;
    }
    do {
        const std::string name = fd.cFileName;
        const Expect e = readExpect(tests + "\\" + name);
        check(e.ok, "(b) expectation file reads");
        if (!e.ok) continue;
        if (e.knownStamp != t.knownStamp || e.knownSize != t.knownImageSize) {
            std::printf("game_build_test: skipped %s: made for another known build (PE stamp 0x%08X); regenerate it after the retarget\n",
                        name.c_str(), e.knownStamp);
            continue;
        }
        if (e.table != t.tableHash) {
            std::printf("FAIL: %s was made for table %016llX, src\\common\\game_signatures.inc is %016llX. Regenerate it:\n"
                        "      python tools\\hook_signatures.py check --table build\\hook_signatures.json --exe <that exe> "
                        "--expect tools\\game_build_test\\%s --emit-expect tools\\game_build_test\\%s\n",
                        name.c_str(), static_cast<unsigned long long>(e.table), static_cast<unsigned long long>(t.tableHash),
                        name.c_str(), name.c_str());
            ++g_failures;
            continue;
        }
        const Archived* a = findExe(exes, e.exeStamp, e.exeSize);
        if (!a) {
            std::printf("game_build_test: skipped %s: no archived exe with PE stamp 0x%08X in %s\n", name.c_str(), e.exeStamp, dir.c_str());
            continue;
        }
        std::vector<gs::Result> res;
        gs::Stats st{};
        double ms = 0;
        const bool ok = resolveFile(a->path, t, &res, &st, &ms);
        check(ok, "(b) archived exe maps and resolves");
        if (!ok) continue;
        uint32_t equal = 0, wrong = 0, unresolved = 0, pyEqual = 0;
        for (uint32_t i = 0; i < t.targetCount && i < e.rows.size(); ++i) {
            const Row& r = e.rows[i];
            if (r.gt < 0) continue;
            pyEqual += r.py == r.gt;
            if (res[i].reason != gs::kFound) ++unresolved;
            else if (int64_t(res[i].rva) == r.gt) ++equal;
            else ++wrong;
        }
        const uint32_t agree = agreement(t, res.data(), e, name.c_str());
        std::printf("game_build_test: %s (%s): %u targets: EQUAL %u, WRONG %u, UNRESOLVED %u; Python EQUAL %u; C++ and Python agree on "
                    "%u; resolve %.1f ms (%u units, %u leaf, %u candidates, %llu bytes)\n",
                    name.c_str(), a->path.c_str() + a->path.find_last_of('\\') + 1, t.targetCount, equal, wrong, unresolved, pyEqual,
                    agree, ms, st.units, st.leaf, st.candidates, static_cast<unsigned long long>(st.bytes));
        check(wrong == 0, "(b) no WRONG address on an archived build");
        check(equal == pyEqual, "(b) the same EQUAL count as Python");
        check(agree == t.targetCount, "(b) C++ and Python give every target the same answer");
        // An RVA inside a found unit, off the table: the byte after each found target with a ground truth must land on the byte after
        // its ground truth (the in-unit path gameRva takes for call sites, return addresses and site ends).
        uint32_t inUnit = 0, inUnitWrong = 0;
        for (uint32_t i = 0; i < t.targetCount && i < e.rows.size(); ++i) {
            const gs::Target& tg = t.targets[i];
            if (tg.kind != gs::kCode || e.rows[i].gt < 0 || res[i].reason != gs::kFound || tg.off + 1 >= t.units[tg.unit].len ||
                indexOf(t, tg.rva + 1) >= 0)
                continue;
            ++inUnit;
            inUnitWrong += int64_t(gs::unitRva(t, res.data(), tg.rva + 1)) != e.rows[i].gt + 1;
        }
        const uint32_t unitAgree = unitAgreement(t, res.data(), e, name.c_str());
        std::printf("game_build_test: %s: in-unit RVAs against the ground truth: %u checked, %u WRONG; %zu unit probes, C++ and Python "
                    "agree on %u\n", name.c_str(), inUnit, inUnitWrong, e.probes.size(), unitAgree);
        check(inUnit > 0 && inUnitWrong == 0, "(b) an RVA inside a found unit lands where the ground truth puts it");
        check(unitAgree == e.probes.size(), "(b) C++ and Python place RVAs inside a unit alike");
        uint32_t indexBad = 0;
        const uint32_t indexAsked = indexAgreement(t, res.data(), &e, &indexBad);
        std::printf("game_build_test: %s: gameRva's sorted index against unitRva: %u RVAs asked, %u differ\n", name.c_str(), indexAsked,
                    indexBad);
        check(indexAsked > 0 && indexBad == 0, "(b) gameRva's sorted index answers as unitRva's scan does");
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4 || std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: game_build_test.exe --self-test <fixture dir> <repository root>\n");
        return 2;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    static_assert(fixture::kTable.format == gs::kCppFormat && known::kTable.format == gs::kCppFormat, "table layout");
    try {
        fixtureChecks(argv[2]);
        archivedChecks(argv[3]);
    } catch (const std::exception& e) {
        std::printf("FAIL: %s (a fixture name or expectation is missing)\n", e.what());
        ++g_failures;
    }
    std::printf("game_build_test: %d checks, %s\n", g_checks, g_failures ? "FAILED" : "ok");
    return g_failures ? 1 : 0;
}
