#include "vscreen_res.h"

#include <windows.h>

#include <cstring>
#include <string>

#include "../common/log.h"
#include "../common/proxy.h"

namespace edvr {
namespace {

// The panel resolution is an OVERRIDE on a view mode, not a size with an owner.
//
// 6m closed this on "29 call sites, the resolution is threaded through the
// renderer, not owned anywhere". Reading the executable says otherwise: the
// exact instruction pair 6m patched -- `mov r8d, 0x780` / `mov r9d, 0x438` --
// occurs ONCE in the whole 81 MB of .text. Two view modes get a forced size and
// every other mode reads a real one from a struct. The 29 call sites 6m counted
// are downstream: they receive a size, they do not choose one. That is why
// patching one allocation changed 8 of 61 targets.
//
// What this touches: the width and height immediates. Not the comparison, not
// the branch, not the mode value, not the struct read.
//
// Nothing here is pinned to a version, deliberately.
//
// The obvious design pins to build 330683 and refuses on anything else. That is
// safe and it is also broken by every game update, which means the fix is off
// more often than on. Measurement says it is unnecessary: the SHAPE below --
// with no knowledge of 1920 or 1080 at all -- matches exactly six places in
// 81 MB of code, which are the six we want.
//
//     cmp  r32, 1
//     ja   .other
//     mov  r32, <width>        ; a 16:9 pair, forced for this view mode
//     mov  r32, <height>
//     jmp  .done
//   .other:
//     mov  r32, [reg + disp]   ; the size the object really has
//
// Recognising the thing being edited is stronger verification than trusting a
// version string, because a version string says nothing about whether the code
// still means what it meant.
//
// It is also safe in a way worth being explicit about: this replaces a 4-byte
// immediate operand with another 4-byte immediate. Instruction lengths do not
// change, control flow does not change, and the instruction stream cannot
// desynchronise. If the signature ever matched something unintended, the result
// is a surface rendering at an odd size -- visible, harmless, and gone on
// restart. That is not the risk profile of a patch that rewrites instructions.
constexpr uint32_t kMinWidth = 640;      // below this it is not a render size

// A band, not an exact count, so a build that adds or drops a call site still
// works. Zero means the shape is gone entirely; a large number means the shape
// stopped being distinctive. Either is a refusal.
constexpr size_t kMinSites = 3;
constexpr size_t kMaxExpected = 12;
constexpr size_t kMaxSites = 32;
constexpr size_t kWindow = 26;   // bytes after the cmp that may hold the movs

struct Site {
    uint8_t* width = nullptr;
    uint8_t* height = nullptr;
};
Site g_sites[kMaxSites];
size_t g_count = 0;
bool g_applied = false;
// What the game had before we touched it, so revert restores what was actually
// there rather than a number this file assumed.
uint32_t g_origW = 0, g_origH = 0;

bool textSection(const uint8_t** begin, size_t* size) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            *begin = base + sec->VirtualAddress;
            *size = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Every `mov r32, imm32` in a window: the immediate's address and its value.
size_t collectMovs(const uint8_t* p, size_t n, const uint8_t** at, uint32_t* val,
                   size_t cap) {
    size_t k = 0;
    for (size_t i = 1; i + 4 <= n && k < cap; ++i) {
        const uint8_t op = p[i - 1];
        if (op < 0xB8 || op > 0xBF) continue;
        uint32_t v = 0;
        memcpy(&v, p + i, 4);
        at[k] = p + i;
        val[k] = v;
        ++k;
    }
    return k;
}

// No destructors here: __try is illegal in a function needing unwinding, and
// reading another module's memory is exactly where a guard belongs.
size_t scanSites(const uint8_t* begin, size_t size, Site* out, size_t cap,
                 uint32_t* foundW, uint32_t* foundH) {
    size_t n = 0;
    *foundW = 0;
    *foundH = 0;
    __try {
        for (size_t i = 0; i + 3 + kWindow <= size; ++i) {
            // cmp r32, 1  ->  83 /7 ib with modrm F8..FF
            if (begin[i] != 0x83 || begin[i + 1] < 0xF8 || begin[i + 2] != 0x01) continue;

            const uint8_t* movAt[12];
            uint32_t movVal[12];
            const size_t m = collectMovs(begin + i + 3, kWindow, movAt, movVal, 12);

            // A 16:9 pair among them, wide enough to be a render size. The
            // aspect test is what makes this a resolution rather than any two
            // constants that happen to sit near a comparison.
            for (size_t a = 0; a < m; ++a) {
                for (size_t c = 0; c < m; ++c) {
                    if (a == c) continue;
                    const uint32_t w = movVal[a], h = movVal[c];
                    if (w < kMinWidth || h == 0) continue;
                    if (w * 9 != h * 16) continue;

                    // Every site must agree on the same forced size. Sites that
                    // disagree would mean the shape is catching more than one
                    // thing, and patching all of them would be a guess.
                    if (*foundW == 0) {
                        *foundW = w;
                        *foundH = h;
                    } else if (*foundW != w || *foundH != h) {
                        return static_cast<size_t>(-2);
                    }
                    if (n < cap) {
                        out[n].width = const_cast<uint8_t*>(movAt[a]);
                        out[n].height = const_cast<uint8_t*>(movAt[c]);
                    }
                    ++n;
                    a = m;   // one pair per site
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<size_t>(-1);
    }
    return n;
}

bool writeImm(uint8_t* at, uint32_t value) {
    DWORD prot = 0;
    if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &prot)) return false;
    memcpy(at, &value, 4);
    DWORD ignored = 0;
    VirtualProtect(at, 4, prot, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, 4);
    return true;
}

}  // namespace

bool applyVScreenModeResolution(uint32_t width, uint32_t height) {
    if (g_applied) return true;

    // The game allocates render targets from these numbers, so absurd values
    // cost VRAM at best and fail allocation at worst.
    if (width < kMinWidth || height < 360 || width > 8192 || height > 8192) {
        Log::get().note("vScreen resolution REFUSED: %ux%u is outside the sane range "
                        "(640x360 to 8192x8192). Nothing was written.", width, height);
        return false;
    }

    const uint8_t* begin = nullptr;
    size_t size = 0;
    if (!textSection(&begin, &size)) {
        Log::get().note("vScreen resolution: could not locate the executable section; "
                        "nothing was written");
        return false;
    }

    uint32_t srcW = 0, srcH = 0;
    const size_t found = scanSites(begin, size, g_sites, kMaxSites, &srcW, &srcH);

    if (found == static_cast<size_t>(-1)) {
        Log::get().note("vScreen resolution: faulted while scanning; nothing was written");
        return false;
    }
    if (found == static_cast<size_t>(-2)) {
        Log::get().note("vScreen resolution REFUSED: the sites disagree about which "
                        "resolution is being forced, so this shape is matching more than "
                        "one thing. Nothing was written.");
        return false;
    }
    if (found < kMinSites || found > kMaxExpected) {
        Log::get().note("vScreen resolution REFUSED: found %zu site(s), expected between "
                        "%zu and %zu. Nothing was written. Either this game version no "
                        "longer forces the resolution this way, or the pattern has stopped "
                        "being specific enough to trust -- both are reasons to do nothing.",
                        found, kMinSites, kMaxExpected);
        return false;
    }
    if (srcW == width && srcH == height) {
        Log::get().note("vScreen resolution: the game already renders that screen at "
                        "%ux%u; nothing to do.", srcW, srcH);
        return false;
    }

    // Report what was found BEFORE changing it. If a game update moves this to
    // some other resolution, the log records the real one instead of leaving
    // anyone to assume it was still 1920x1080.
    const std::string build = gameBuildVersion();
    Log::get().note("vScreen resolution: %zu site(s) forcing %ux%u, on game build %s%s",
                    found, srcW, srcH,
                    build.empty() ? "(unreadable)" : build.c_str(),
                    gameBuildIsVerified()
                        ? " -- the build this was developed against."
                        : " -- NOT the build this was developed against. Proceeding anyway: "
                          "the code being changed was identified by its shape, which says "
                          "more about whether it is the right code than a version string "
                          "does. If the picture looks wrong, set both values to 0.");

    // Warn, do not refuse. A different aspect stretches rather than widens,
    // because the screen's shape is set elsewhere -- but "we think this looks
    // wrong" is not grounds to override a deliberate choice.
    if (width * 9 != height * 16) {
        Log::get().note("vScreen resolution: %ux%u is not 16:9, while the game forces "
                        "%ux%u which is. Expect a stretched picture. Applying anyway.",
                        width, height, srcW, srcH);
    }

    // All or nothing. A half-applied resolution renders worse than none -- 6m.2
    // established that by shipping one.
    size_t done = 0;
    for (; done < found; ++done) {
        if (!writeImm(g_sites[done].width, width)) break;
        if (!writeImm(g_sites[done].height, height)) {
            writeImm(g_sites[done].width, srcW);
            break;
        }
    }
    if (done != found) {
        for (size_t i = 0; i < done; ++i) {
            writeImm(g_sites[i].width, srcW);
            writeImm(g_sites[i].height, srcH);
        }
        Log::get().note("vScreen resolution: a write failed at site %zu of %zu; every site "
                        "restored, nothing left changed.", done + 1, found);
        return false;
    }

    g_count = found;
    g_origW = srcW;
    g_origH = srcH;
    g_applied = true;
    Log::get().note("vScreen resolution: %ux%u -> %ux%u at %zu site(s). This writes to game "
                    "CODE -- to %zu pairs of numbers and nothing else. It reverts when the "
                    "game closes.",
                    srcW, srcH, width, height, found, found);
    return true;
}

void revertVScreenModeResolution() {
    if (!g_applied) return;
    for (size_t i = 0; i < g_count; ++i) {
        writeImm(g_sites[i].width, g_origW);
        writeImm(g_sites[i].height, g_origH);
    }
    Log::get().note("vScreen resolution reverted to %ux%u at %zu site(s)",
                    g_origW, g_origH, g_count);
    g_applied = false;
    g_count = 0;
}

}  // namespace edvr

