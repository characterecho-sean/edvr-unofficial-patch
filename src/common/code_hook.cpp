#include "code_hook.h"

#include <windows.h>

#include <cstring>

#include "guard.h"
#include "log.h"

namespace edvr {

namespace {

// The instruction shapes this decoder will move, and nothing else.
//
// A compiled x86-64 function almost always opens with some arrangement of:
// saving a parameter register into its shadow slot, pushing a callee-saved
// register, subtracting the frame size from rsp, or moving one register into
// another. Those are what is listed here. Everything else -- and every relative
// operand of any length -- is refused, because a hook that relocates an
// instruction it has misread does not fail here, it fails in somebody else's
// code with EDVR nowhere on the stack.
//
// The returned length counts the whole instruction including prefixes.

bool isRexPrefix(uint8_t b) { return b >= 0x40 && b <= 0x4F; }

// ModRM displacement size, and whether the operand is rip-relative (mod == 00
// and rm == 101, which on x86-64 means [rip+disp32] and is NOT relocatable).
bool modrmSize(uint8_t modrm, bool* ripRelative, size_t* extra) {
    const uint8_t mod = static_cast<uint8_t>(modrm >> 6);
    const uint8_t rm = static_cast<uint8_t>(modrm & 7);
    *ripRelative = false;
    *extra = 1;  // the ModRM byte itself
    if (mod == 3) return true;              // register operand, nothing follows
    if (rm == 4) *extra += 1;               // SIB
    if (mod == 0) {
        if (rm == 5) { *ripRelative = true; *extra += 4; }
        // A SIB with base == 101 also carries a disp32, which this decoder does
        // not attempt to read; refuse rather than guess.
        else if (rm == 4) return false;
    } else if (mod == 1) {
        *extra += 1;                        // disp8
    } else if (mod == 2) {
        *extra += 4;                        // disp32
    }
    return true;
}

}  // namespace

size_t codeInstructionLength(const uint8_t* code, size_t available,
                             size_t* ripDispOffset) {
    if (ripDispOffset) *ripDispOffset = 0;
    if (!code || available == 0) return 0;
    size_t i = 0;

    // Prefixes. Only the operand-size prefix and REX are expected in a
    // prologue; a segment, lock or rep prefix means this is not the sort of
    // instruction being decoded and the answer is 0.
    if (i < available && code[i] == 0x66) ++i;
    if (i < available && isRexPrefix(code[i])) ++i;
    if (i >= available) return 0;

    const uint8_t op = code[i++];
    switch (op) {
        // push/pop r64 -- the classic prologue register saves.
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            return i;

        // mov r/m, r ; mov r, r/m ; and the 8-bit forms. This is
        // `mov [rsp+8], rcx` and `mov rbp, rsp`.
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        // lea r, m -- common, and refused below when rip-relative.
        case 0x8D:
        // add/or/and/sub/xor/cmp r/m, r and the reverse direction.
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        // inc/dec r/m and the group-5 forms. FF 05 is `inc [rip+disp32]`, how a
        // compiler increments a global -- and how the very first function this
        // class was ever pointed at happened to begin.
        case 0xFE: case 0xFF:
        case 0x84: case 0x85: {
            if (i >= available) return 0;
            bool rip = false;
            size_t extra = 0;
            if (!modrmSize(code[i], &rip, &extra)) return 0;
            // The displacement sits straight after the ModRM byte: the
            // rip-relative form is mod == 00, rm == 101, which excludes a SIB.
            if (rip && ripDispOffset) *ripDispOffset = i + 1;
            i += extra;
            return i <= available ? i : 0;
        }

        // Group 1 with an 8-bit immediate: `sub rsp, 0x28` and its relatives.
        case 0x83: {
            if (i >= available) return 0;
            bool rip = false;
            size_t extra = 0;
            if (!modrmSize(code[i], &rip, &extra)) return 0;
            if (rip && ripDispOffset) *ripDispOffset = i + 1;
            i += extra + 1;
            return i <= available ? i : 0;
        }

        // Group 1 with a 32-bit immediate: a large frame.
        case 0x81: {
            if (i >= available) return 0;
            bool rip = false;
            size_t extra = 0;
            if (!modrmSize(code[i], &rip, &extra)) return 0;
            if (rip && ripDispOffset) *ripDispOffset = i + 1;
            i += extra + 4;
            return i <= available ? i : 0;
        }

        // mov r32/r64, imm32 (B8+r). Ten bytes with REX.W as imm64, which is
        // the shape the D3D11 restore block itself uses.
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            const bool wide = i >= 2 && isRexPrefix(code[i - 2]) &&
                              (code[i - 2] & 0x08) != 0;
            i += wide ? 8 : 4;
            return i <= available ? i : 0;
        }

        // A one-byte no-op, and the multi-byte form compilers pad with.
        case 0x90:
            return i;
        case 0x0F: {
            if (i < available && code[i] == 0x1F) {
                ++i;
                if (i >= available) return 0;
                bool rip = false;
                size_t extra = 0;
                if (!modrmSize(code[i], &rip, &extra)) return 0;
                if (rip && ripDispOffset) *ripDispOffset = i + 1;
                i += extra;
                return i <= available ? i : 0;
            }
            return 0;   // every other two-byte opcode, including jcc rel32
        }

        default:
            // Everything unlisted, which includes E8 (call), E9/EB (jmp),
            // 7x (jcc rel8), C3 (ret) and every SSE form. A prologue that
            // begins with any of them is one this class will not touch.
            return 0;
    }
}

namespace {

// Reserve executable memory within +/-2GB of `near`, so a 5-byte relative jump
// can reach it. Walks outward from the target a page at a time; MEM_RESERVE
// fails harmlessly on anything already taken.
uint8_t* allocateNear(void* anchor, size_t bytes) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t granularity =
        si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    // `near` would have been the obvious name and is a windows.h macro.
    const uintptr_t at = reinterpret_cast<uintptr_t>(anchor);
    // Two gigabytes minus a margin, because the jump displacement is measured
    // from the END of the patched instruction and the trampoline's own length
    // has to fit inside the window too.
    constexpr uintptr_t kReach = 0x7FF00000;

    for (uintptr_t delta = granularity; delta < kReach; delta += granularity) {
        for (int direction = 0; direction < 2; ++direction) {
            const uintptr_t candidate = direction ? (at + delta) : (at - delta);
            if (direction == 0 && delta > at) continue;   // would wrap below zero
            void* const got = VirtualAlloc(
                reinterpret_cast<void*>(candidate & ~(granularity - 1)), bytes,
                MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (got) return static_cast<uint8_t*>(got);
        }
    }
    return nullptr;
}

}  // namespace

bool CodeHook::install(void* target, void* replacement, void** origOut,
                       const char* who) {
    const char* label = who ? who : "?";
    if (m_installed || !target || !replacement) return false;

    uint8_t* const code = static_cast<uint8_t*>(target);

    // The patch is one aligned eight-byte store, so no thread can ever see it
    // half-written -- which is what makes this safe to do to a function other
    // threads are calling right now, without suspending any of them. An
    // unaligned entry point cannot be patched that way, and rather than fall
    // back to a torn write this refuses. Compilers align function entries to
    // sixteen bytes, so refusing costs nothing in practice and removes a whole
    // class of race from the design.
    if (reinterpret_cast<uintptr_t>(code) % 8 != 0) {
        Log::get().note(
            "CodeHook %s: %p is not eight-byte aligned, so the patch could not "
            "be written as one atomic store. Not hooked -- a torn patch in code "
            "another thread is executing is not a risk worth taking.",
            label, target);
        return false;
    }

    // Decode whole instructions until at least the patch is covered. The
    // stolen bytes have to be a whole number of instructions or the trampoline
    // resumes in the middle of one.
    struct Piece {
        size_t offset = 0;
        size_t length = 0;
        size_t ripDisp = 0;   // offset of the disp32 within the instruction, or 0
    };
    Piece pieces[8];
    size_t pieceCount = 0;
    size_t stolen = 0;
    while (stolen < kCodeHookPatchBytes) {
        if (pieceCount >= 8) return false;
        size_t len = 0;
        size_t rip = 0;
        const bool read = guarded("CodeHook::install/decode", [&] {
            len = codeInstructionLength(code + stolen, 16 - stolen, &rip);
        });
        if (!read || len == 0) {
            Log::get().note(
                "CodeHook %s: the instruction at %p+%zu is one this build does "
                "not recognise or will not move (a jump or a call -- a function "
                "beginning with one is a linker thunk or somebody else's hook, "
                "and following it would cut them out). Not hooked, and nothing "
                "was changed.",
                label, target, stolen);
            return false;
        }
        pieces[pieceCount].offset = stolen;
        pieces[pieceCount].length = len;
        pieces[pieceCount].ripDisp = rip;
        ++pieceCount;
        stolen += len;
    }
    if (stolen > sizeof(m_original)) return false;

    // trampoline = the stolen instructions, then an absolute jump back to the
    // rest of the function. Absolute here, not relative: the trampoline is
    // within reach of the target by construction, but the code AFTER the patch
    // does not have to be within reach of the trampoline once both have been
    // placed by the allocator.
    const size_t trampolineBytes = stolen + 14;
    uint8_t* const tramp = allocateNear(target, trampolineBytes);
    if (!tramp) {
        Log::get().note(
            "CodeHook %s: no executable memory could be reserved within reach "
            "of %p, so a five-byte jump cannot get to a trampoline. Not hooked.",
            label, target);
        return false;
    }

    bool relocatable = true;
    bool built = guarded("CodeHook::install/build", [&] {
        memcpy(tramp, code, stolen);

        // REWRITE EVERY RIP-RELATIVE DISPLACEMENT. The instruction has moved,
        // and the whole meaning of the operand is "this far from where I am",
        // so copying the bytes unchanged points it at different memory --
        // silently, and at a place with no relation to the bug it causes. The
        // arithmetic is: work out what it addressed where it was, then say the
        // same thing from where it now is.
        for (size_t p = 0; p < pieceCount; ++p) {
            if (!pieces[p].ripDisp) continue;
            const size_t dispAt = pieces[p].offset + pieces[p].ripDisp;
            const size_t endAt = pieces[p].offset + pieces[p].length;
            int32_t was = 0;
            memcpy(&was, code + dispAt, sizeof(was));
            const uintptr_t addressed =
                reinterpret_cast<uintptr_t>(code) + endAt + was;
            const intptr_t now = static_cast<intptr_t>(addressed) -
                                 static_cast<intptr_t>(
                                     reinterpret_cast<uintptr_t>(tramp) + endAt);
            if (now > INT32_MAX || now < INT32_MIN) {
                relocatable = false;
                return;
            }
            const int32_t fixed = static_cast<int32_t>(now);
            memcpy(tramp + dispAt, &fixed, sizeof(fixed));
        }

        uint8_t* const back = tramp + stolen;
        back[0] = 0xFF;                       // jmp qword ptr [rip+0]
        back[1] = 0x25;
        back[2] = back[3] = back[4] = back[5] = 0x00;
        const uint64_t resume =
            reinterpret_cast<uintptr_t>(code) + stolen;
        memcpy(back + 6, &resume, sizeof(resume));
        memcpy(m_original, code, stolen);
    });
    if (!built || !relocatable) {
        if (!relocatable) {
            Log::get().note(
                "CodeHook %s: the prologue at %p reaches data more than two "
                "gigabytes from where the trampoline could be placed, so the "
                "moved instruction could not be made to address it. Not hooked.",
                label, target);
        }
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }

    // Build the replacement's eight bytes: E9 rel32, then whatever the original
    // held beyond the patch, so the store leaves the tail of the eight-byte
    // word exactly as it found it.
    const intptr_t displacement =
        reinterpret_cast<intptr_t>(replacement) -
        (reinterpret_cast<intptr_t>(code) + static_cast<intptr_t>(kCodeHookPatchBytes));
    if (displacement > INT32_MAX || displacement < INT32_MIN) {
        Log::get().note(
            "CodeHook %s: %p is more than two gigabytes from the replacement, "
            "which a five-byte jump cannot reach. Not hooked.",
            label, target);
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }

    uint64_t word = 0;
    memcpy(&word, code, sizeof(word));
    uint8_t patched[8];
    memcpy(patched, &word, sizeof(patched));
    patched[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(displacement);
    memcpy(patched + 1, &rel, sizeof(rel));
    uint64_t newWord = 0;
    memcpy(&newWord, patched, sizeof(newWord));

    DWORD oldProtect = 0;
    if (!VirtualProtect(code, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log::get().note("CodeHook %s: could not make %p writable (err %lu). Not "
                        "hooked.", label, target, GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    // ONE aligned store. Every other thread sees either the old eight bytes or
    // the new ones, never a mixture, so none of them has to be stopped first.
    const bool wrote = guarded("CodeHook::install/patch", [&] {
        *reinterpret_cast<volatile uint64_t*>(code) = newWord;
    });
    DWORD ignored = 0;
    VirtualProtect(code, 8, oldProtect, &ignored);
    if (!wrote) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), code, 8);

    m_target = target;
    m_replacement = replacement;
    m_trampoline = tramp;
    m_stolen = stolen;
    m_installed = true;
    if (origOut) *origOut = tramp;

    Log::get().note(
        "CodeHook %s: hooked the function at %p itself, moving %zu byte(s) of "
        "its prologue to a trampoline at %p. Whatever is written into the "
        "vtable slot that names this function, calls to it now arrive here -- "
        "which is the point: the table can be restored as often as its owner "
        "likes and there is nothing left to lose a race with.",
        label, target, stolen, static_cast<void*>(tramp));
    return true;
}

void CodeHook::uninstall() {
    if (!m_installed) return;
    uint8_t* const code = static_cast<uint8_t*>(m_target);

    // Only if the patch is still ours. Another tool that hooked this function
    // after EDVR owns those bytes now; putting the originals back would splice
    // it out, which is precisely the thing done to EDVR that this class was
    // built to survive.
    bool ours = false;
    guarded("CodeHook::uninstall/check", [&] {
        const intptr_t displacement =
            reinterpret_cast<intptr_t>(m_replacement) -
            (reinterpret_cast<intptr_t>(code) +
             static_cast<intptr_t>(kCodeHookPatchBytes));
        int32_t rel = 0;
        memcpy(&rel, code + 1, sizeof(rel));
        ours = code[0] == 0xE9 && rel == static_cast<int32_t>(displacement);
    });

    if (ours) {
        uint64_t word = 0;
        guarded("CodeHook::uninstall/read", [&] {
            memcpy(&word, code, sizeof(word));
        });
        uint8_t bytes[8];
        memcpy(bytes, &word, sizeof(bytes));
        memcpy(bytes, m_original, m_stolen < 8 ? m_stolen : 8);
        uint64_t restored = 0;
        memcpy(&restored, bytes, sizeof(restored));

        DWORD oldProtect = 0;
        if (VirtualProtect(code, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            guarded("CodeHook::uninstall/patch", [&] {
                *reinterpret_cast<volatile uint64_t*>(code) = restored;
            });
            DWORD ignored = 0;
            VirtualProtect(code, 8, oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), code, 8);
        }
    } else {
        Log::get().note(
            "CodeHook: the function at %p was hooked again after EDVR, so its "
            "first bytes belong to that tool now and are being left alone. "
            "EDVR's trampoline is deliberately leaked rather than freed -- "
            "their hook still runs through it, and freeing it would dangle.",
            m_target);
    }

    // Freed only when the patch was still ours; see above.
    if (ours && m_trampoline) VirtualFree(m_trampoline, 0, MEM_RELEASE);
    m_trampoline = nullptr;
    m_target = nullptr;
    m_replacement = nullptr;
    m_stolen = 0;
    m_installed = false;
}

}  // namespace edvr
