#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>

// The generated code of the upstream camera injector (flat_camera_inject.cpp):
// the two stubs that carry the game's mid-function hook site into compiled C
// and back. Header-only so tools/flat_camera_stub_test runs the real
// emitters, not a copy of them.
//
// THE CONTRACT BOTH STUBS KEEP. They run inside a game function (stubA at its
// live stack, stubB as its redirected return) and call compiled C while the
// game's registers are live. The Windows x64 ABI gives that callee 32 bytes
// of home area directly above its return address, which it may use as
// scratch: MSVC prologues spill rcx/rdx/r8/r9 or the non-volatiles they
// need there. The caller must reserve it. The first stub build did not.
// stubA's callee homed rcx and rdx onto stubA's own saved r15 and r14, and
// the pops handed the game r15 = the callee's first argument (the 20:09,
// 03:50 and 04:23 crashes, design-flat-camera-integration.md, 2026-09-29);
// stubB's callee homed rbx and rsi onto its saved xmm0. Both stubs now
// reserve the 32 bytes inside their frame, right around the call.
namespace edvr {
namespace camera_stubs {

// The stubs share the relay's page (the relay is 46 bytes at offset 0);
// every cross-reference is absolute, so placement only needs to stay inside
// the allocation. The rig checks the byte counts against the emitters.
constexpr uint32_t kStubAOffset = 48;   // 8-aligned, right after the relay
constexpr uint32_t kStubABytes = 119;   // literal included: ends at 167
constexpr uint32_t kStubBOffset = 168;  // 8-aligned, after stubA
constexpr uint32_t kStubBBytes = 79;
static_assert(kStubAOffset + kStubABytes <= kStubBOffset, "stubA must end before stubB begins");
static_assert(kStubBOffset + kStubBBytes <= 4096, "the stubs must fit the relay page");

// A tiny emitter for the two stubs. Fixed byte sequences with immediates
// appended through u32/u64; every offset below is derived from the byte
// counts in the comments, and buildStubA returns the literal offset it
// actually used so prepareRelay patches the same place.
struct CodeCursor {
    uint8_t* p;
    void b(std::initializer_list<uint8_t> vs) { for (const uint8_t v : vs) *p++ = v; }
    void u32(uint32_t v) { std::memcpy(p, &v, 4); p += 4; }
    void u64(uint64_t v) { std::memcpy(p, &v, 8); p += 8; }
};

// stubA -- the relay's callback. Entered by jmp with rsp = S (the game's
// stack at the hook site; S = 8 mod 16, the ABI's post-call alignment
// minus the two pushes the game made before the patch). Saves every GPR
// (fifteen pushes keep the alignment), sets up refreshPre's arguments from
// the save slots, calls it with the ABI's 32-byte home area reserved (the
// sub/add rsp,0x20 sit right around the call, so the argument setup's
// offsets are unchanged and the alignment holds), then restores everything
// and joins the trampoline with rsp = S. The game body sees every register
// and every stack byte at and above S as the unhooked call left them; the
// stack below S is scratch, as it is for any callee. Returns the offset of
// the trampoline literal for prepareRelay.
inline uint32_t buildStubA(uint8_t* at, const void* preFn, void* r11Store) noexcept {
    CodeCursor c{at};
    c.b({0x50, 0x53, 0x51, 0x52, 0x56, 0x57, 0x55});           // push rax rbx rcx rdx rsi rdi rbp
    c.b({0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53,           // push r8 r9 r10 r11
         0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57});          // push r12 r13 r14 r15
    // rsp = S-0x78, 16-aligned. Slots: r15@[+0] .. rax@[+0x70].
    c.b({0x48,0x8B,0x44,0x24,0x20});                            // mov rax,[rsp+0x20]  = saved r11 (incoming!)
    c.b({0x48,0xA3}); c.u64(reinterpret_cast<uint64_t>(r11Store)); // mov [&g_lastIncomingR11],rax
    c.b({0x48,0x8D,0x8C,0x24}); c.u32(0x88);                    // lea rcx,[rsp+0x88]  = R0 (the game's retaddr slot)
    c.b({0x48,0x8B,0x54,0x24,0x60});                            // mov rdx,[rsp+0x60]  = saved rcx (ctx)
    c.b({0x4C,0x8B,0x44,0x24,0x58});                            // mov r8, [rsp+0x58]  = saved rdx (p2)
    c.b({0x4C,0x8B,0x4C,0x24,0x38});                            // mov r9, [rsp+0x38]  = saved r8  (camera)
    c.b({0x49,0xBB}); c.u64(reinterpret_cast<uint64_t>(preFn)); // mov r11,&refreshPre
    c.b({0x48,0x83,0xEC,0x20});                                 // sub rsp,0x20 (the callee's home area; still 16-aligned)
    c.b({0x41,0xFF,0xD3});                                      // call r11
    c.b({0x48,0x83,0xC4,0x20});                                 // add rsp,0x20
    c.b({0x41,0x5F, 0x41,0x5E, 0x41,0x5D, 0x41,0x5C,           // pop r15 r14 r13 r12
         0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58});          // pop r11 r10 r9 r8
    c.b({0x5D, 0x5F, 0x5E, 0x5A, 0x59, 0x5B, 0x58});            // pop rbp rdi rsi rdx rcx rbx rax
    c.b({0xFF,0x25}); c.u32(0);                                 // jmp [rip+0]
    const uint32_t literalOfs = static_cast<uint32_t>(c.p - at);
    c.u64(0);                                                   // trampoline literal, patched by prepareRelay
    return literalOfs;
}

// stubB -- the body's redirected return. Entered by the body's ret with
// rsp = R0+8 (16-aligned) and the game's return state in every register.
// Preserves rax/xmm0 and the scratch registers around refreshPost, then
// jumps to the real return address from TLS. r11 has no ABI role across a
// return, so it alone carries the TLS walk; _tls_index is process-
// constant after CRT init and is baked in as an immediate. The frame after
// seven pushes is 0x38 bytes: [rsp, rsp+0x20) is the callee's home area,
// [rsp+0x20, rsp+0x30) holds xmm0 (16-aligned), the last 8 keep the call
// aligned.
inline void buildStubB(uint8_t* at, const void* postFn, uint32_t tlsIndex, uint32_t tlsRealRetOfs) noexcept {
    CodeCursor c{at};
    c.b({0x50, 0x51, 0x52});                                    // push rax rcx rdx
    c.b({0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53});          // push r8 r9 r10 r11
    c.b({0x48,0x83,0xEC,0x38});                                 // sub rsp,0x38 (16-aligned from here)
    c.b({0x0F,0x29,0x44,0x24,0x20});                            // movaps [rsp+0x20],xmm0
    c.b({0x49,0xBB}); c.u64(reinterpret_cast<uint64_t>(postFn));// mov r11,&refreshPost
    c.b({0x41,0xFF,0xD3});                                      // call r11
    c.b({0x0F,0x28,0x44,0x24,0x20});                            // movaps xmm0,[rsp+0x20]
    c.b({0x48,0x83,0xC4,0x38});                                 // add rsp,0x38
    c.b({0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58});          // pop r11 r10 r9 r8
    c.b({0x5A, 0x59, 0x58});                                    // pop rdx rcx rax
    c.b({0x65,0x4C,0x8B,0x1C,0x25}); c.u32(0x58);               // mov r11,gs:[0x58] (TLS array)
    c.b({0x4F,0x8B,0x9B}); c.u32(tlsIndex * 8);                 // mov r11,[r11+tlsIndex*8] (disp32: the index is per-process, seen > 15)
    c.b({0x4F,0x8B,0x9B}); c.u32(tlsRealRetOfs);                // mov r11,[r11+tlsRealRetOfs]
    c.b({0x41,0xFF,0xE3});                                      // jmp r11
}

} // namespace camera_stubs
} // namespace edvr
