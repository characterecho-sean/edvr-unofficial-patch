// Intercept a FUNCTION rather than a table entry: own the code, not the slot.
//
// WHY THIS EXISTS, and it is the third mechanism this project has needed.
//
// VTableHook writes a pointer into a dispatch table. Both of its mechanisms
// assume the table settles: patch it in place and hope nobody else writes it,
// or copy it and hope the original never has to change. Issue #21 is the rig
// where neither holds. Windows' own d3d11.dll RESTORES the immediate context's
// table every frame -- proved on 2026-09-08 by watching the memory: four stores
// fourteen bytes apart at system32\d3d11.dll+0x44A14, putting hardcoded
// addresses back into slots 12, 13, 20 and 21, the whole draw family, 130 ms
// after the hooks went in and then again every frame after that.
//
// Against a per-frame restorer a patrol cannot win. EDVR re-patched every frame
// and still measured its hooks in the table on 0% of frames, with the visible
// result being a ship-launched fighter's shield flickering: the fixes ran on
// part of each frame's draws and not the rest. A fix that acts on some frames
// is worse than one that never acts, because it looks like a rendering bug.
//
// So stop contesting the slot. The runtime stores a hardcoded IMMEDIATE, the
// same address every time -- which is why the variant census read exactly one
// distinct entry per slot through eight thousand exchanges -- and that address
// is a function. Put the interception in the FUNCTION'S FIRST INSTRUCTION and
// the table becomes irrelevant: whatever the runtime writes into the slot, the
// call arrives at code EDVR is already inside. No exchange, no window, nothing
// to lose a race with.
//
// It also needs no address hunting, which is the part that makes it fit this
// codebase. The target is not found by signature or by a build-keyed offset --
// it is READ OUT OF THE SLOT. A Windows update or a driver change moves the
// function and the next launch reads its new address without noticing.
//
// WHAT IT COSTS, stated plainly because this is the most invasive thing EDVR
// does. It writes to executable memory in somebody else's module. That is the
// technique every code injector uses and it is what antivirus heuristics look
// for; EDVR already carries a Defender false positive without it. Everything
// below is therefore built to REFUSE rather than to try: an unfamiliar
// instruction in the prologue, a relative operand that would break when moved,
// an entry point that is not aligned, a trampoline that cannot be placed in
// range -- each one is a hook that does not happen and a line in the log
// saying why. The fix standing down is always better than the fix guessing.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

// How many bytes the patch occupies: E9 plus a 32-bit displacement.
//
// Five, not the fourteen an absolute indirect jump would need, and the reason
// is the prologue. Every byte of patch is a byte of somebody else's code that
// has to be decoded, moved and executed somewhere else; at five, the common
// prologues are one or two instructions, and at fourteen they are five or six
// with far more chance of one being relative. The cost is that the trampoline
// must land within +/-2GB of the target, which install() arranges or refuses.
constexpr size_t kCodeHookPatchBytes = 5;

class CodeHook {
public:
    CodeHook() = default;
    ~CodeHook() { uninstall(); }

    CodeHook(const CodeHook&) = delete;
    CodeHook& operator=(const CodeHook&) = delete;

    // Redirect `target` to `replacement`, and hand back a pointer that still
    // runs the original.
    //
    // `origOut` receives the trampoline: the target's stolen first instructions
    // followed by a jump to what comes after them. Calling it is calling the
    // function as it was. It stays valid until uninstall().
    //
    // `who` names the hook in the log lines. Returns false, having changed
    // nothing, if the target cannot be hooked safely -- and says why. Callers
    // must treat false as "run without this fix", never as "try harder".
    bool install(void* target, void* replacement, void** origOut,
                 const char* who);

    // Put the original bytes back, but ONLY if the patch is still ours: another
    // tool that hooked the same function after us owns those bytes now, and
    // restoring them would cut it out exactly the way this class exists to
    // avoid being cut out. The trampoline is then released.
    void uninstall();

    bool  installed() const { return m_installed; }
    void* target() const { return m_target; }

    // How many bytes of the target's prologue were relocated. For the log and
    // for the unit cells; a prologue that needed more than a couple of
    // instructions is worth noticing.
    size_t stolenBytes() const { return m_stolen; }

private:
    void*    m_target = nullptr;
    void*    m_replacement = nullptr;
    uint8_t* m_trampoline = nullptr;
    uint8_t  m_original[16] = {};
    size_t   m_stolen = 0;
    bool     m_installed = false;
};

// Length of the x86-64 instruction at `code`, or 0 if this decoder does not
// recognise it or it cannot be relocated.
//
// NOT a disassembler, and deliberately not: it understands the shapes that
// actually begin a compiled function -- register moves, stack adjustment,
// pushes, the load of a parameter into shadow space, and the rip-relative
// access to a global -- and answers 0 for everything else.
//
// RIP-RELATIVE INSTRUCTIONS ARE MEASURED, NOT REFUSED, and the first function
// this was ever pointed at is why. It began `FF 05 …` -- an increment of a
// global, which MSVC emits rip-relative -- so a decoder that refused those
// would have refused the very first ordinary prologue it met, and would refuse
// most of them. `ripDispOffset` receives the byte offset of the 32-bit
// displacement within the instruction, so a caller that moves the instruction
// can rewrite it to still reach the same data; it receives 0 when there is
// none. Moving one WITHOUT rewriting it addresses different memory, which is a
// bug with no symptom anywhere near its cause -- so this reports the offset and
// CodeHook::install does the arithmetic.
//
// Jumps and calls are still refused outright. Their length is obvious and
// relocating them by displacement would work, but a function whose first
// instruction is a jump is either a linker thunk or a function somebody else
// has already hooked, and following it would silently cut that tool out.
//
// Exposed for the unit cells, which feed it real prologues and known-awkward
// byte sequences and require the awkward ones to be refused.
size_t codeInstructionLength(const uint8_t* code, size_t available,
                             size_t* ripDispOffset);

}  // namespace edvr
