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
// renderer, not owned anywhere". Reading the executable says otherwise. The
// exact instruction pair that 6m patched -- `mov r8d, 0x780` / `mov r9d, 0x438`
// -- occurs ONCE in the whole 81 MB of .text. What occurs six times is this:
//
//     lea  eax, [rcx - 5]
//     cmp  eax, 1
//     ja   .other                       ; if the view mode is not 5 or 6...
//     mov  edi, 0x780                   ; 1920   <- forced, only for those modes
//     mov  r15d, 0x438                  ; 1080
//     jmp  .done
//   .other:
//     mov  edi, dword ptr [r14 + 0x318] ; ...use the size the object really has
//     mov  r15d, dword ptr [r14 + 0x31c]
//
// So two view-mode values get a hardcoded size and every other mode reads a
// real one from a struct. The 29 call sites 6m counted are downstream of this:
// they receive a size, they do not choose one. That is why patching one
// allocation changed 8 of 61 targets.
//
// This rewrites the two immediates at all six sites, so the game itself
// computes with the new numbers. Whether its shader constants follow is 6m.3 --
// recorded as untested, because testing it needed exactly this patch first.
//
// What it does NOT touch: the comparison, the branch, the mode value, or the
// struct read. Only the two immediates.
constexpr uint32_t kOldWidth = 1920;
constexpr uint32_t kOldHeight = 1080;

// Measured, not estimated: six, by scanning the whole executable section. It is
// hardcoded so a build that has moved this code fails to match rather than
// being patched partially -- and a partial patch is worse than none, which 6m.2
// established the expensive way.
constexpr size_t kExpectedSites = 6;
constexpr size_t kMaxSites = 16;
constexpr size_t kWindow = 26;   // bytes after the cmp that may hold the movs

struct Site {
    uint8_t* width = nullptr;
    uint8_t* height = nullptr;
};
Site g_sites[kMaxSites];
size_t g_count = 0;
bool g_applied = false;

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

// `mov r32, imm32` carrying `want`, opcode B8..BF, optionally REX-prefixed.
// Returns the address of the immediate, or null.
const uint8_t* findMovImm(const uint8_t* p, size_t n, uint32_t want) {
    for (size_t i = 1; i + 4 <= n; ++i) {
        uint32_t v = 0;
        memcpy(&v, p + i, 4);
        if (v != want) continue;
        const uint8_t op = p[i - 1];
        if (op >= 0xB8 && op <= 0xBF) return p + i;
    }
    return nullptr;
}

// No destructors here: __try is illegal in a function needing unwinding, and
// reading another module's memory is exactly where a guard belongs.
size_t scanSites(const uint8_t* begin, size_t size, Site* out) {
    size_t n = 0;
    __try {
        for (size_t i = 0; i + 3 + kWindow <= size; ++i) {
            // cmp r32, 1  ->  83 /7 ib with modrm F8..FF
            if (begin[i] != 0x83 || begin[i + 1] < 0xF8 || begin[i + 2] != 0x01) continue;
            const uint8_t* win = begin + i + 3;
            const uint8_t* w = findMovImm(win, kWindow, kOldWidth);
            const uint8_t* h = findMovImm(win, kWindow, kOldHeight);
            if (!w || !h) continue;
            if (n < kMaxSites) {
                out[n].width = const_cast<uint8_t*>(w);
                out[n].height = const_cast<uint8_t*>(h);
            }
            ++n;
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

    if (!gameBuildIsVerified()) {
        const std::string ver = gameBuildVersion();
        Log::get().note("vScreen mode resolution REFUSED: game build %s is not the build "
                        "this was read from (330683). Nothing was written. This is the "
                        "safeguard working, not a fault -- the fix edits code, and code "
                        "moves between builds.",
                        ver.empty() ? "(unreadable)" : ver.c_str());
        return false;
    }
    // A panel is 16:9 and the game allocates render targets from these numbers,
    // so absurd values cost VRAM at best and fail allocation at worst.
    if (width < 640 || height < 360 || width > 8192 || height > 8192) {
        Log::get().note("vScreen mode resolution REFUSED: %ux%u is outside the sane "
                        "range (640x360 to 8192x8192). Nothing was written.", width, height);
        return false;
    }
    // Warn, do not refuse. The ini says 16:9 because the panel's shape is
    // decided elsewhere and a different aspect stretches rather than widens --
    // but "we think this looks wrong" is not grounds to override a deliberate
    // choice, and someone may want to find out.
    if (width * kOldHeight != height * kOldWidth) {
        Log::get().note("vScreen mode resolution: %ux%u is not 16:9. The panel's shape is "
                        "set elsewhere, so expect a stretched image rather than a wider "
                        "one. Applying anyway.", width, height);
    }
    if (width == kOldWidth && height == kOldHeight) {
        Log::get().note("vScreen mode resolution: %ux%u is what the game already uses; "
                        "nothing to do.", width, height);
        return false;
    }

    const uint8_t* begin = nullptr;
    size_t size = 0;
    if (!textSection(&begin, &size)) {
        Log::get().note("vScreen mode resolution: could not locate the executable "
                        "section; nothing was written");
        return false;
    }

    const size_t found = scanSites(begin, size, g_sites);
    if (found == static_cast<size_t>(-1)) {
        Log::get().note("vScreen mode resolution: faulted while scanning; nothing was "
                        "written");
        return false;
    }
    if (found != kExpectedSites) {
        Log::get().note("vScreen mode resolution REFUSED: found %zu site(s), expected "
                        "exactly %zu. Nothing was written. A partial patch renders worse "
                        "than no patch -- 6m.2 established that by doing it.",
                        found, kExpectedSites);
        return false;
    }

    // All or nothing. A half-applied patch is the failure mode 6m.2 hit, so any
    // write that fails rolls the earlier ones back before returning.
    size_t done = 0;
    for (; done < found; ++done) {
        if (!writeImm(g_sites[done].width, width)) break;
        if (!writeImm(g_sites[done].height, height)) {
            writeImm(g_sites[done].width, kOldWidth);
            break;
        }
    }
    if (done != found) {
        for (size_t i = 0; i < done; ++i) {
            writeImm(g_sites[i].width, kOldWidth);
            writeImm(g_sites[i].height, kOldHeight);
        }
        Log::get().note("vScreen mode resolution: a write failed at site %zu of %zu; all "
                        "sites restored, nothing left changed.", done + 1, found);
        return false;
    }

    g_count = found;
    g_applied = true;
    Log::get().note("vScreen mode resolution: %ux%u -> %ux%u at %zu site(s). This is a "
                    "write to game CODE, the second such in the project. It reverts on "
                    "unload.",
                    kOldWidth, kOldHeight, width, height, found);
    return true;
}

void revertVScreenModeResolution() {
    if (!g_applied) return;
    for (size_t i = 0; i < g_count; ++i) {
        writeImm(g_sites[i].width, kOldWidth);
        writeImm(g_sites[i].height, kOldHeight);
    }
    Log::get().note("vScreen mode resolution reverted at %zu site(s)", g_count);
    g_applied = false;
    g_count = 0;
}

}  // namespace edvr
