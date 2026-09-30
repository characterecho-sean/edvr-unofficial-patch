// flat_camera_stub_test: the upstream camera injector's generated stubs, run
// for real and judged on the property they exist to keep, not on their bytes.
//
// The property: a stub that sits inside a game function and calls compiled C
// must hand the game back every register and every stack byte at and above
// its own entry stack pointer, whatever the C callee does with the memory the
// Windows x64 ABI gives it. That memory includes the callee's 32-byte home
// area, the four slots directly above its return address, which an MSVC
// prologue may fill with its own rcx/rdx/r8/r9 or with the non-volatiles it
// wants to save. The 2026-09-28/29 crashes were exactly this: stubA called
// refreshPre with no home area reserved, the callee's home slots were
// stubA's own saved r15..r12, and the pops gave the game r15 = the callee's
// first argument. Nothing about the bytes looked wrong; the contract was.
//
// So the stand-in callees below are compiled C++ that take the address of
// every register parameter (which makes the compiler home it at entry) and
// then overwrite all four home slots through volatile pointers. A harness
// generated in executable memory loads a distinct known value into every
// general register (and xmm0 for stubB), enters the real emitted stubs the
// way the relay and the game's redirected return do, and records what comes
// out the far side. A register that differs, a stack pointer that moved, or a
// saved slot above the entry pointer that changed fails the run.
//
//   flat_camera_stub_test --self-test

#include <windows.h>
#include <intrin.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "src/d3d11/flat_camera_stubs.h"

extern "C" DWORD _tls_index; // CRT-provided once a __declspec(thread) exists

using namespace edvr::camera_stubs;

namespace {

int g_checks = 0;
int g_failures = 0;

void report(bool ok, const char* what, const char* detail = nullptr) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("  FAIL: %s%s%s\n", what, detail ? " -- " : "", detail ? detail : "");
}

// Hardware register numbers, the order a modrm byte uses.
enum Reg { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
const char* const kRegName[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                  "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

// What the harness loads before it enters a stub and what it finds on the far
// side, indexed by hardware register number (gpr[RSP] is unused on input).
struct Block {
    uint64_t gpr[16];
    alignas(16) uint8_t xmm0[16];
    uint64_t siteRsp; // rsp at the instant control enters the stub
    uint64_t slot[3]; // [S], [S+8], [S+0x10] as control leaves the stub (stubA)
};
Block g_inA, g_outA, g_inB, g_outB;
uint64_t g_r11Seen; // stubA's incoming-r11 instrument writes here

// What every stand-in callee writes into its four home slots.
constexpr uint64_t kPoison[4] = {0xBAD0BAD0BAD00001ull, 0xBAD0BAD0BAD00002ull,
                                 0xBAD0BAD0BAD00003ull, 0xBAD0BAD0BAD00004ull};

struct Seen {
    uint64_t args[4];
    uint32_t calls;
};
Seen g_pre, g_post;

// The stand-ins for refreshPre and refreshPost. Taking each parameter's
// address makes MSVC keep the register in its home slot; the slots are then
// the callee's to destroy, exactly as the ABI says. The arguments are
// recorded before they are overwritten so the stub's argument setup is
// checked too.
__declspec(noinline) void testPre(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    volatile uintptr_t* home[4] = {&a0, &a1, &a2, &a3};
    for (int i = 0; i < 4; ++i) g_pre.args[i] = *home[i];
    for (int i = 0; i < 4; ++i) *home[i] = kPoison[i];
    ++g_pre.calls;
}

__declspec(noinline) void testPost(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    volatile uintptr_t* home[4] = {&a0, &a1, &a2, &a3};
    for (int i = 0; i < 4; ++i) g_post.args[i] = *home[i];
    for (int i = 0; i < 4; ++i) *home[i] = kPoison[i];
    ++g_post.calls;
}

__declspec(thread) uint64_t g_tlsRealRet;
uint64_t g_calibration[4];

// A tiny x86-64 emitter for the harness: only what the harness needs. The
// stubs under test come from flat_camera_stubs.h, never from here.
struct Asm {
    uint8_t* base;
    uint8_t* p;
    explicit Asm(uint8_t* at) : base(at), p(at) {}
    uint32_t here() const { return static_cast<uint32_t>(p - base); }
    void b(std::initializer_list<uint8_t> vs) { for (const uint8_t v : vs) *p++ = v; }
    void u32(uint32_t v) { std::memcpy(p, &v, 4); p += 4; }
    void u64(uint64_t v) { std::memcpy(p, &v, 8); p += 8; }
    void push(int r) { if (r >= 8) *p++ = 0x41; *p++ = static_cast<uint8_t>(0x50 + (r & 7)); }
    void pop(int r) { if (r >= 8) *p++ = 0x41; *p++ = static_cast<uint8_t>(0x58 + (r & 7)); }
    void movImm(int r, uint64_t v) {
        *p++ = static_cast<uint8_t>(0x48 | (r >= 8 ? 1 : 0));
        *p++ = static_cast<uint8_t>(0xB8 + (r & 7));
        u64(v);
    }
    // mov reg,[base+disp32] / mov [base+disp32],reg; base is never rsp or r12.
    void mem(uint8_t op, int reg, int baseReg, uint32_t disp) {
        *p++ = static_cast<uint8_t>(0x48 | (reg >= 8 ? 4 : 0) | (baseReg >= 8 ? 1 : 0));
        *p++ = op;
        *p++ = static_cast<uint8_t>(0x80 | ((reg & 7) << 3) | (baseReg & 7));
        u32(disp);
    }
    void load(int reg, int baseReg, uint32_t disp) { mem(0x8B, reg, baseReg, disp); }
    void store(int baseReg, uint32_t disp, int reg) { mem(0x89, reg, baseReg, disp); }
    // movups xmm0,[base+disp32] (op 0x10) / movups [base+disp32],xmm0 (op 0x11)
    void movups(uint8_t op, int baseReg, uint32_t disp) {
        if (baseReg >= 8) *p++ = 0x41;
        b({0x0F, op});
        *p++ = static_cast<uint8_t>(0x80 | (baseReg & 7));
        u32(disp);
    }
    void leaRaxRsp(uint8_t d) { b({0x48, 0x8D, 0x44, 0x24, d}); }   // lea rax,[rsp+d]
    void leaRcxRsp(uint8_t d) { b({0x48, 0x8D, 0x4C, 0x24, d}); }   // lea rcx,[rsp+d]
    void loadRcxRsp(uint8_t d) { b({0x48, 0x8B, 0x4C, 0x24, d}); }  // mov rcx,[rsp+d]
    void loadRaxRsp(uint8_t d) { b({0x48, 0x8B, 0x44, 0x24, d}); }  // mov rax,[rsp+d]
    void storeRspRax(uint8_t d) { b({0x48, 0x89, 0x44, 0x24, d}); } // mov [rsp+d],rax
    void storeMoffsRax(const void* at) { b({0x48, 0xA3}); u64(reinterpret_cast<uint64_t>(at)); }
    void jmpAbs(const void* target) { b({0xFF, 0x25}); u32(0); u64(reinterpret_cast<uint64_t>(target)); }
    void callViaRax(const void* fn) { movImm(RAX, reinterpret_cast<uint64_t>(fn)); b({0xFF, 0xD0}); }
    void addRsp(uint8_t n) { b({0x48, 0x83, 0xC4, n}); }
    void subRsp(uint8_t n) { b({0x48, 0x83, 0xEC, n}); }
    void ret() { *p++ = 0xC3; }
    uint8_t* callPlaceholder() { *p++ = 0xE8; uint8_t* at = p; u32(0); return at; }
    void patchRel(uint8_t* at, const uint8_t* target) {
        const int32_t rel = static_cast<int32_t>(target - (at + 4));
        std::memcpy(at, &rel, 4);
    }
    // The host's non-volatiles are saved around a harness and restored on the
    // way out; the extra 8 keeps the harness 16-aligned inside.
    void enterHarness() {
        for (const int r : {RBX, RBP, RSI, RDI, R12, R13, R14, R15}) push(r);
        subRsp(8);
    }
    void leaveHarness() {
        addRsp(8);
        for (const int r : {R15, R14, R13, R12, RDI, RSI, RBP, RBX}) pop(r);
        ret();
    }
    // Load every register from a Block through r11 (loaded last), so nothing
    // the harness needs is disturbed on the way.
    void loadAll(const Block* in, bool withXmm) {
        movImm(R11, reinterpret_cast<uint64_t>(in));
        for (int r = 0; r < 16; ++r) {
            if (r == RSP || r == R11) continue;
            load(r, R11, static_cast<uint32_t>(r * 8));
        }
        if (withXmm) movups(0x10, R11, static_cast<uint32_t>(offsetof(Block, xmm0)));
        load(R11, R11, static_cast<uint32_t>(R11 * 8));
    }
    // Record every register into a Block without disturbing any before it is
    // stored: rax is parked on the stack and becomes the base, and the entry
    // rsp (before the park) lands in gpr[RSP]. Leaves rax = the Block and the
    // parked value on the stack for finishRaxCapture.
    void captureAll(Block* out, bool withXmm) {
        push(RAX);
        movImm(RAX, reinterpret_cast<uint64_t>(out));
        for (int r = 0; r < 16; ++r) {
            if (r == RAX || r == RSP) continue;
            store(RAX, static_cast<uint32_t>(r * 8), r);
        }
        if (withXmm) movups(0x11, RAX, static_cast<uint32_t>(offsetof(Block, xmm0)));
        leaRcxRsp(8);
        store(RAX, static_cast<uint32_t>(RSP * 8), RCX);
    }
    void finishRaxCapture() {
        pop(RCX);
        store(RAX, 0, RCX);
    }
};

// Where everything lives in the one code page.
constexpr uint32_t kHarnessA = 0x400;
constexpr uint32_t kHarnessB = 0x800;
constexpr uint32_t kCalibrationPre = 0xC00;
constexpr uint32_t kCalibrationPost = 0xD00;
constexpr uint32_t kPageBytes = 0x1000;

struct Code {
    uint8_t* page = nullptr;
    uint32_t stubALiteral = 0;
    uint64_t contA = 0;    // the game's return address in the stubA harness
    uint64_t landingB = 0; // the real return address in the stubB harness
};

// stubA, entered the way the relay enters it: by jmp, with rsp = S and the
// game's live registers, after the game function's two pushes and its return
// address. The trampoline's stand-in records everything, then returns like
// the game function's own ret.
void emitHarnessA(Code& code) {
    uint8_t* at = code.page + kHarnessA;
    Asm a(at);
    a.enterHarness();
    a.loadAll(&g_inA, false);
    uint8_t* callAt = a.callPlaceholder();
    code.contA = reinterpret_cast<uint64_t>(at + a.here());
    a.leaveHarness();
    a.patchRel(callAt, at + a.here()); // fakeRefresh follows
    a.push(R13);
    a.push(R14); // [S] = r14, [S+8] = r13, [S+0x10] = the return address (contA)
    a.push(RAX);
    a.leaRaxRsp(8); // S
    a.storeMoffsRax(&g_outA.siteRsp);
    a.pop(RAX);
    a.jmpAbs(code.page + kStubAOffset); // the relay's jmp
    const uint32_t tramp = a.here();
    a.captureAll(&g_outA, false);
    a.loadRcxRsp(0x08);
    a.store(RAX, static_cast<uint32_t>(offsetof(Block, slot) + 0), RCX);  // [S]
    a.loadRcxRsp(0x10);
    a.store(RAX, static_cast<uint32_t>(offsetof(Block, slot) + 8), RCX);  // [S+8]
    a.loadRcxRsp(0x18);
    a.store(RAX, static_cast<uint32_t>(offsetof(Block, slot) + 16), RCX); // [S+0x10]
    a.finishRaxCapture();
    a.addRsp(0x10); // the game's two saved registers
    a.ret();        // the game's own ret: back to contA
    const uint64_t trampAddr = reinterpret_cast<uint64_t>(at + tramp);
    std::memcpy(code.page + kStubAOffset + code.stubALiteral, &trampAddr, 8);
}

// stubB, entered the way the body's redirected ret enters it: at a 16-aligned
// rsp with the game's return state in the registers, and left through the
// real return address it finds in thread-local storage.
void emitHarnessB(Code& code) {
    uint8_t* at = code.page + kHarnessB;
    Asm a(at);
    a.enterHarness();
    a.loadAll(&g_inB, true);
    a.push(RAX);
    a.leaRaxRsp(8);
    a.storeMoffsRax(&g_outB.siteRsp);
    a.pop(RAX);
    a.jmpAbs(code.page + kStubBOffset);
    code.landingB = reinterpret_cast<uint64_t>(at + a.here()); // the real return address
    a.captureAll(&g_outB, true);
    a.finishRaxCapture();
    a.leaveHarness();
}

// A caller-provided home area with a marker in it, then the callee, then the
// four slots copied out: proof that a stand-in callee really destroys its own
// home slots, taken without the stubs, so a compiler that stopped homing the
// parameters could not turn the rig green.
void emitCalibration(uint8_t* at, void (*fn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)) {
    Asm a(at);
    a.subRsp(0x28); // 32 bytes of home area, plus 8 to keep the call 16-aligned
    a.movImm(RAX, 0x7777777777777777ull);
    for (uint8_t i = 0; i < 4; ++i) a.storeRspRax(static_cast<uint8_t>(i * 8));
    a.callViaRax(reinterpret_cast<const void*>(fn));
    for (uint8_t i = 0; i < 4; ++i) {
        a.loadRaxRsp(static_cast<uint8_t>(i * 8));
        a.storeMoffsRax(&g_calibration[i]);
    }
    a.addRsp(0x28);
    a.ret();
}

void fillSentinels(Block& in, int round) {
    for (int i = 0; i < 16; ++i)
        in.gpr[i] = 0x5EED000000000000ull ^ (static_cast<uint64_t>(round + 1) << 40) ^
                    (static_cast<uint64_t>(i + 1) * 0x0101010101ull);
    in.gpr[RSP] = 0;
    for (int i = 0; i < 16; ++i) in.xmm0[i] = static_cast<uint8_t>(0xC0 + i * 3 + round);
    in.siteRsp = 0;
    std::memset(in.slot, 0, sizeof(in.slot));
}

bool guarded(const uint8_t* code, const char* what) {
    __try {
        reinterpret_cast<void (*)()>(const_cast<uint8_t*>(code))();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "exception 0x%08lX inside the harness", GetExceptionCode());
        report(false, what, msg);
        return false;
    }
}

void compareGprs(const char* tag, const Block& in, const Block& out, bool skipR11) {
    for (int r = 0; r < 16; ++r) {
        if (r == RSP || (skipR11 && r == R11)) continue;
        if (out.gpr[r] == in.gpr[r]) { ++g_checks; continue; }
        char what[64], detail[192];
        std::snprintf(what, sizeof(what), "%s: %s did not survive", tag, kRegName[r]);
        const char* leak = "";
        for (int k = 0; k < 4; ++k)
            if (out.gpr[r] == kPoison[k]) leak = " (the callee's home-slot sentinel: a saved slot was overwritten)";
        std::snprintf(detail, sizeof(detail), "was %016llX, came back %016llX%s",
                      static_cast<unsigned long long>(in.gpr[r]),
                      static_cast<unsigned long long>(out.gpr[r]), leak);
        report(false, what, detail);
    }
}

void runStubA(const Code& code, int round) {
    fillSentinels(g_inA, round);
    std::memset(&g_outA, 0, sizeof(g_outA));
    std::memset(&g_pre, 0, sizeof(g_pre));
    g_r11Seen = 0;
    char tag[32];
    std::snprintf(tag, sizeof(tag), "stubA round %d", round);
    if (!guarded(code.page + kHarnessA, tag)) return;

    report(g_pre.calls == 1, "stubA: the C callee ran exactly once");
    compareGprs(tag, g_inA, g_outA, false);
    report(g_outA.gpr[RSP] == g_outA.siteRsp, "stubA: rsp is back to S when the trampoline is entered");
    report(g_outA.slot[0] == g_inA.gpr[R14], "stubA: the game's saved r14 at [S] is untouched");
    report(g_outA.slot[1] == g_inA.gpr[R13], "stubA: the game's saved r13 at [S+8] is untouched");
    report(g_outA.slot[2] == code.contA, "stubA: the game's return address at [S+0x10] is untouched");
    report(g_pre.args[0] == g_outA.siteRsp + 0x10, "stubA: the callee gets R0, the return-address slot");
    report(g_pre.args[1] == g_inA.gpr[RCX], "stubA: the callee gets the incoming rcx as ctx");
    report(g_pre.args[2] == g_inA.gpr[RDX], "stubA: the callee gets the incoming rdx as p2");
    report(g_pre.args[3] == g_inA.gpr[R8], "stubA: the callee gets the incoming r8 as camera");
    report(g_r11Seen == g_inA.gpr[R11], "stubA: the incoming-r11 instrument records the game's r11");
}

void runStubB(const Code& code, int round) {
    fillSentinels(g_inB, round);
    std::memset(&g_outB, 0, sizeof(g_outB));
    std::memset(&g_post, 0, sizeof(g_post));
    g_tlsRealRet = code.landingB;
    char tag[32];
    std::snprintf(tag, sizeof(tag), "stubB round %d", round);
    if (!guarded(code.page + kHarnessB, tag)) return;

    report(g_post.calls == 1, "stubB: the C callee ran exactly once");
    compareGprs(tag, g_inB, g_outB, true); // r11 alone carries the TLS walk
    report(g_outB.gpr[R11] == code.landingB, "stubB: the TLS walk delivers the real return address");
    report(g_outB.gpr[RSP] == g_outB.siteRsp, "stubB: rsp is unchanged across the stub");
    report(std::memcmp(g_outB.xmm0, g_inB.xmm0, 16) == 0, "stubB: xmm0 (the game's float return) survives");
}

bool calibrated(const Code& code, uint32_t at) {
    std::memset(g_calibration, 0, sizeof(g_calibration));
    if (!guarded(code.page + at, "calibration")) return false;
    bool ok = true;
    for (int i = 0; i < 4; ++i) ok &= g_calibration[i] == kPoison[i];
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--self-test") != 0) {
        std::fprintf(stderr, "usage: flat_camera_stub_test [--self-test]\n");
        return 2;
    }
    std::printf("flat_camera_stub_test: the injector's generated stubs against callees that spend their home area\n");

    // Written read-write, run read-execute, like the production relay page.
    Code code;
    code.page = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!code.page) { std::printf("  FAIL: no memory for the harness\n"); return 1; }

    // The stubs go where the relay page puts them.
    code.stubALiteral = buildStubA(code.page + kStubAOffset, reinterpret_cast<const void*>(&testPre), &g_r11Seen);
    report(code.stubALiteral + 8 == kStubABytes, "stubA is as long as the layout constants say");
    report(kStubAOffset + kStubABytes <= kStubBOffset, "stubA ends before stubB begins");
    const auto tlsArray = reinterpret_cast<const uintptr_t*>(__readgsqword(0x58));
    const uintptr_t tlsBase = tlsArray[_tls_index];
    const uintptr_t tlsStruct = reinterpret_cast<uintptr_t>(&g_tlsRealRet);
    report(tlsStruct >= tlsBase && tlsStruct - tlsBase <= 0xFFFFFFFFull,
           "the thread-local is within disp32 of its TLS block");
    buildStubB(code.page + kStubBOffset, reinterpret_cast<const void*>(&testPost), _tls_index,
               static_cast<uint32_t>(tlsStruct - tlsBase));

    emitHarnessA(code);
    emitHarnessB(code);
    emitCalibration(code.page + kCalibrationPre, &testPre);
    emitCalibration(code.page + kCalibrationPost, &testPost);
    DWORD oldProtect = 0;
    if (!VirtualProtect(code.page, kPageBytes, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), code.page, kPageBytes)) {
        std::printf("  FAIL: could not make the harness executable\n");
        return 1;
    }

    // The stand-in callees must spend their home slots, or the rest proves nothing.
    report(calibrated(code, kCalibrationPre), "the refreshPre stand-in overwrites all four of its home slots");
    report(calibrated(code, kCalibrationPost), "the refreshPost stand-in overwrites all four of its home slots");

    for (int round = 0; round < 3; ++round) {
        runStubA(code, round);
        runStubB(code, round);
    }

    VirtualFree(code.page, 0, MEM_RELEASE);
    if (g_failures) {
        std::printf("flat_camera_stub_test: FAIL (%d of %d checks)\n", g_failures, g_checks);
        return 1;
    }
    std::printf("flat_camera_stub_test: PASS (%d checks)\n", g_checks);
    return 0;
}
