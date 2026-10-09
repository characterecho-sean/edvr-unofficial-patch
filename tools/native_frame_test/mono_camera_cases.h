#pragma once
// The mono camera hooks (src/d3d11/mono_camera_core.h and mono_camera_hook.cpp; docs\terrain-culling.md, round 5): the detour's decision
// (return address x window x factor, bit-identical otherwise), the build gate and its refusals, the lazy install, what the observer
// writes and where it stops, and then the REAL hooks: a synthetic executable image with the getter, the writer and two stub callers laid
// out at the build's RVAs, so the relay, the code-hook decoder, _ReturnAddress() and the stolen prologues are all exercised for real.
#include "../../src/d3d11/mono_camera_core.h"
#include "../../src/d3d11/mono_camera_hook.h"
#include "cull_cycle_cases.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace mono_cases {
namespace mc = edvr::monocam;
using cull_cases::Lines;

inline uint32_t bitsOf(float v) {
    uint32_t b;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}
inline float floatOf(uint32_t b) {
    float v;
    std::memcpy(&v, &b, sizeof(v));
    return v;
}

// A reason that is null (the gate opened) is a mismatch, not a crash: a mutant that opens the gate must fail a check, not fault.
inline bool same(const char* got, const char* want) { return got != nullptr && std::strcmp(got, want) == 0; }

inline Lines*& sinkTarget() {
    static Lines* target = nullptr;
    return target;
}
inline void sinkFn(const char* line) {
    if (sinkTarget()) (*sinkTarget())(line);
}

struct Emit {
    std::vector<uint8_t> b;
    Emit& op(std::initializer_list<uint8_t> l) {
        b.insert(b.end(), l.begin(), l.end());
        return *this;
    }
    Emit& u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
        return *this;
    }
    Emit& u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
        return *this;
    }
    size_t size() const { return b.size(); }
};

// A caller shaped like the mono filler: rax = a table whose slot 8 (offset 0x40) holds the getter, rcx = the camera, then
// `call qword ptr [rax+40h]` (FF 50 40) placed at offset 38 so that, started at RVA ...D60, the call is at ...D86 and returns to ...D89.
inline std::vector<uint8_t> fillerStub(uintptr_t table, uintptr_t camera, size_t* returnOffset) {
    Emit e;
    e.op({0x48, 0x83, 0xEC, 0x28});                  // sub rsp,28h
    e.op({0x48, 0xB8}).u64(table);                   // mov rax,table
    e.op({0x48, 0xB9}).u64(camera);                  // mov rcx,camera
    while (e.size() < 38) e.op({0x90});              // nop
    e.op({0xFF, 0x50, 0x40});                        // call qword ptr [rax+40h]
    *returnOffset = e.size();
    e.op({0x48, 0x83, 0xC4, 0x28, 0xC3});            // add rsp,28h ; ret
    return e.b;
}

// A caller of the aspect writer: rcx = camera, edx = width, r8d = height, xmm3 = the minimum aspect, and the float* on the stack at the
// slot the callee finds at [rsp+28h]. Returns whatever the writer returned in rax.
inline std::vector<uint8_t> writerStub(uintptr_t writer, uintptr_t camera, uintptr_t out, uint32_t width, uint32_t height, float minAspect,
                                       size_t* returnOffset) {
    Emit e;
    e.op({0x48, 0x83, 0xEC, 0x38});                  // sub rsp,38h
    e.op({0x48, 0xB8}).u64(out);                     // mov rax,out
    e.op({0x48, 0x89, 0x44, 0x24, 0x20});            // mov [rsp+20h],rax
    e.op({0x48, 0xB9}).u64(camera);                  // mov rcx,camera
    e.op({0xBA}).u32(width);                         // mov edx,width
    e.op({0x41, 0xB8}).u32(height);                  // mov r8d,height
    e.op({0xB8}).u32(bitsOf(minAspect));             // mov eax,minAspect
    e.op({0x66, 0x0F, 0x6E, 0xD8});                  // movd xmm3,eax
    e.op({0x48, 0xB8}).u64(writer);                  // mov rax,writer
    e.op({0xFF, 0xD0});                              // call rax
    *returnOffset = e.size();
    e.op({0x48, 0x83, 0xC4, 0x38, 0xC3});            // add rsp,38h ; ret
    return e.b;
}

// An image of the shape the gate reads, mapped at a base of its own: DOS and NT headers (the stamp and size), the getter at +2841190, the
// writer at +28634E0 (its real prologue, then a body that writes 16/9 through *out and returns the camera), and the two filler stubs and
// the writer's caller on the page at +2871000.
struct FakeExe {
    static constexpr size_t kSize = 0x2880000;
    static constexpr uint32_t kFillerA = 0x2871D60, kFillerB = 0x2871F60, kWriterCaller = 0x2871C00;
    uint8_t* base = nullptr;
    uintptr_t table[9] = {};
    alignas(16) float camera[64] = {};
    alignas(16) float out = 0.0f;
    size_t fillerReturn = 0, writerReturn = 0;
    using FloatFn = float (*)();
    using WriterCallerFn = uintptr_t (*)();
    using GetterFn = float (__fastcall*)(const void*);
    FloatFn fillerA() const { return reinterpret_cast<FloatFn>(base + kFillerA); }
    FloatFn fillerB() const { return reinterpret_cast<FloatFn>(base + kFillerB); }
    WriterCallerFn writerCaller() const { return reinterpret_cast<WriterCallerFn>(base + kWriterCaller); }
    GetterFn getter() const { return reinterpret_cast<GetterFn>(base + mc::kGetterRva); }
    ~FakeExe() {
        if (base) VirtualFree(base, 0, MEM_RELEASE);
    }
};

// tamper: 0 none, 1 the getter's displacement byte, 2 the writer's prologue. commitCode false leaves the getter and writer pages unmapped
// (an image smaller than the RVAs).
inline bool buildExe(FakeExe& x, uint32_t stamp, uint32_t imageSize, bool commitCode, int tamper) {
    x.base = static_cast<uint8_t*>(VirtualAlloc(nullptr, FakeExe::kSize, MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!x.base) return false;
    const auto commit = [&](size_t rva) { return VirtualAlloc(x.base + rva, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE) != nullptr; };
    if (!commit(0) || !commit(0x2871000)) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(x.base);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(x.base + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.TimeDateStamp = stamp;
    nt->OptionalHeader.SizeOfImage = imageSize;
    x.camera[0x70 / 4] = 1.7777778f;
    x.table[8] = reinterpret_cast<uintptr_t>(x.base) + mc::kGetterRva;
    if (commitCode) {
        if (!commit(mc::kGetterRva & ~0xFFFu) || !commit(mc::kWriterRva & ~0xFFFu)) return false;
        std::memset(x.base + (mc::kGetterRva & ~0xFFFu), 0xCC, 0x1000);
        std::memset(x.base + (mc::kWriterRva & ~0xFFFu), 0xCC, 0x1000);
        uint8_t getter[sizeof(mc::kGetterBytes)];
        std::memcpy(getter, mc::kGetterBytes, sizeof(getter));
        if (tamper == 1) getter[4] = 0x78;
        std::memcpy(x.base + mc::kGetterRva, getter, sizeof(getter));
        Emit w;
        w.op({0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29, 0x74, 0x24, 0x40, 0x48, 0x8B, 0xD9, 0x44, 0x0F, 0x29, 0x44, 0x24, 0x20, 0x44, 0x0F, 0x28, 0xC3});
        if (tamper == 2) w.b[6] = 0x0E;
        w.op({0x48, 0x8B, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00});   // mov rax,[rsp+80h]         (the float* the caller put on the stack)
        w.op({0xC7, 0x00, 0x39, 0x8E, 0xE3, 0x3F});               // mov dword ptr [rax],3FE38E39h (16/9)
        w.op({0x0F, 0x28, 0x74, 0x24, 0x40});                     // movaps xmm6,[rsp+40h]
        w.op({0x44, 0x0F, 0x28, 0x44, 0x24, 0x20});               // movaps xmm8,[rsp+20h]
        w.op({0x48, 0x83, 0xC4, 0x50});                           // add rsp,50h
        w.op({0x48, 0x89, 0xD8});                                 // mov rax,rbx               (the camera)
        w.op({0x5B, 0xC3});                                       // pop rbx ; ret
        std::memcpy(x.base + mc::kWriterRva, w.b.data(), w.b.size());
    }
    const auto place = [&](uint32_t rva, const std::vector<uint8_t>& code) {
        std::memcpy(x.base + rva, code.data(), code.size());
    };
    place(FakeExe::kFillerA, fillerStub(reinterpret_cast<uintptr_t>(x.table), reinterpret_cast<uintptr_t>(x.camera), &x.fillerReturn));
    place(FakeExe::kFillerB, fillerStub(reinterpret_cast<uintptr_t>(x.table), reinterpret_cast<uintptr_t>(x.camera), &x.fillerReturn));
    place(FakeExe::kWriterCaller, writerStub(reinterpret_cast<uintptr_t>(x.base) + mc::kWriterRva, reinterpret_cast<uintptr_t>(x.camera),
                                             reinterpret_cast<uintptr_t>(&x.out), 3840, 2160, 1.0f, &x.writerReturn));
    FlushInstructionCache(GetCurrentProcess(), x.base, FakeExe::kSize);
    return true;
}

template <class Check>
void runMonoCameraCases(Check&& check) {
    // ---- the detour's decision: return address x window x factor ----------------------------------------------------------------------
    {
        const uintptr_t filler = 0x7FF602871D89ull;
        const float aspect = 1.7777778f;
        const float lied = mc::decide(aspect, filler, filler, true);
        check(bitsOf(lied) == bitsOf(aspect * 1.30f) && std::fabs(lied - 2.3111112f) < 1e-6f && mc::kFactor == 1.30f,
              "the mono filler's return address in a mono window gets the aspect times 1.30");
        const float samples[] = {aspect, 0.0f, -0.0f, 1.0f, 16.0f / 9.0f, floatOf(0x7FC12345u), floatOf(0x7F800000u), floatOf(0x00000001u), -3.5f};
        bool identical = true;
        for (float v : samples) {
            identical = identical && bitsOf(mc::decide(v, filler + 1, filler, true)) == bitsOf(v) &&      // any other address
                        bitsOf(mc::decide(v, filler - 1, filler, true)) == bitsOf(v) &&
                        bitsOf(mc::decide(v, 0x7FF602871D00ull, filler, true)) == bitsOf(v) &&
                        bitsOf(mc::decide(v, filler, filler, false)) == bitsOf(v) &&                          // no mono window
                        bitsOf(mc::decide(v, filler + 1, filler, false)) == bitsOf(v);
        }
        check(identical, "any other return address, or no mono window, returns the original bit for bit (NaN payloads, zeros, denormals included)");
        check(bitsOf(mc::decide(aspect, 0, 0, true)) == bitsOf(aspect) && bitsOf(mc::decide(aspect, 0, filler, true)) == bitsOf(aspect),
              "an unset filler address never matches (return address 0 does not)");
    }
    // ---- the gate ---------------------------------------------------------------------------------------------------------------------
    {
        const uint32_t stamp = edvr::cullcycle::kBuildStamp, size = edvr::cullcycle::kBuildImageSize;
        const uint8_t* g = mc::kGetterBytes;
        const uint8_t* w = mc::kWriterPrologue;
        check(mc::gateReason(true, stamp, size, g, w) == nullptr, "the gate opens on build 332841's stamp and size and both functions' bytes");
        check(same(mc::gateReason(false, stamp, size, g, w), "the executable's headers could not be read"), "...refuses headers that cannot be read");
        check(same(mc::gateReason(true, stamp + 1, size, g, w), "not build 332841") && same(mc::gateReason(true, stamp, size + 1, g, w), "not build 332841") &&
                  same(mc::gateReason(true, 0, 0, g, w), "not build 332841"),
              "...refuses another stamp, another image size, or neither");
        bool getterRefused = true, writerRefused = true;
        for (size_t i = 0; i < sizeof(mc::kGetterBytes); ++i) {
            uint8_t copy[sizeof(mc::kGetterBytes)];
            std::memcpy(copy, g, sizeof(copy));
            copy[i] ^= 0x01;
            getterRefused = getterRefused && same(mc::gateReason(true, stamp, size, copy, w), "the aspect getter's bytes differ");
        }
        for (size_t i = 0; i < sizeof(mc::kWriterPrologue); ++i) {
            uint8_t copy[sizeof(mc::kWriterPrologue)];
            std::memcpy(copy, w, sizeof(copy));
            copy[i] ^= 0x01;
            writerRefused = writerRefused && same(mc::gateReason(true, stamp, size, g, copy), "the aspect writer's prologue differs");
        }
        check(getterRefused, "...refuses the getter with any one of its six bytes changed");
        check(writerRefused, "...refuses the writer with any one of its 24 prologue bytes changed");
        check(same(mc::gateReason(true, stamp, size, nullptr, w), "the aspect getter's bytes differ") &&
                  same(mc::gateReason(true, stamp, size, g, nullptr), "the aspect writer's prologue differs"),
              "...and bytes that could not be read are bytes that differ");
        check(same(mc::gateReason(true, stamp + 1, size, nullptr, nullptr), "not build 332841"), "...the build is judged first");
        check(mc::kGetterRva == 0x2841190 && mc::kFillerCallRva == 0x2871D86 && mc::kFillerReturnRva == 0x2871D89 && mc::kWriterRva == 0x28634E0 &&
                  mc::kFillerReturnRva == mc::kFillerCallRva + 3 && mc::kGetterBytes[0] == 0xF3 && mc::kGetterBytes[5] == 0xC3,
              "the RVAs are the build's (getter 2841190, filler call 2871D86 returning to 2871D89, writer 28634E0)");
    }
    // ---- the lazy install -------------------------------------------------------------------------------------------------------------
    {
        mc::Lazy z;
        Lines l;
        int gates = 0, installs = 0;
        const auto gateOk = [&]() -> const char* { ++gates; return nullptr; };
        const auto installOk = [&]() { ++installs; return mc::InstallResult{}; };
        z.frame(false, gateOk, installOk, l);
        z.frame(false, gateOk, installOk, l);
        check(!z.attempted() && gates == 0 && installs == 0 && l.v.empty(), "no mono-using key: nothing is read, nothing is installed, nothing is logged");
        z.frame(true, gateOk, installOk, l);
        check(z.attempted() && z.live() && z.writerLive() && gates == 1 && installs == 1 && l.v.size() == 2 &&
                  l.v[0] == "cull probe: mono windows multiply the mono camera's aspect by 1.30 (hook live)" &&
                  l.v[1] == "mono camera: aspect writer observe live (up to 50 calls logged)",
              "the first frame on which a key wants it: the gate is read once, the hooks go in once, two lines say how");
        z.frame(true, gateOk, installOk, l);
        z.frame(false, gateOk, installOk, l);
        z.frame(true, gateOk, installOk, l);
        check(gates == 1 && installs == 1 && l.v.size() == 2, "...and never again, whatever the key does afterwards");
        mc::Lazy refused;
        Lines rl;
        int refusedInstalls = 0;
        refused.frame(true, []() -> const char* { return "not build 332841"; }, [&]() { ++refusedInstalls; return mc::InstallResult{}; }, rl);
        refused.frame(true, []() -> const char* { return "not build 332841"; }, [&]() { ++refusedInstalls; return mc::InstallResult{}; }, rl);
        check(refused.attempted() && !refused.live() && refusedInstalls == 0 && rl.v.size() == 1 &&
                  rl.v[0] == "cull probe: mono windows multiply the mono camera's aspect by 1.30 (hook inert: not build 332841)" &&
                  same(refused.reason(), "not build 332841"),
              "a gate that refuses installs nothing and writes one line saying why, once");
        mc::Lazy failed;
        Lines fl;
        failed.frame(true, []() -> const char* { return nullptr; },
                     []() { mc::InstallResult r; r.getter = "install failed"; return r; }, fl);
        check(!failed.live() && fl.v.size() == 1 && fl.v[0] == "cull probe: mono windows multiply the mono camera's aspect by 1.30 (hook inert: install failed)",
              "a getter hook that fails to go in is inert, with one line");
        mc::Lazy half;
        Lines hl;
        half.frame(true, []() -> const char* { return nullptr; },
                   []() { mc::InstallResult r; r.writer = "install failed"; return r; }, hl);
        check(half.live() && !half.writerLive() && hl.v.size() == 2 && hl.v[0].find("(hook live)") != std::string::npos &&
                  hl.v[1] == "mono camera: aspect writer observe inert: install failed",
              "a writer hook that fails leaves the getter's live and says so on a second line");
        z.reset();
        check(!z.attempted() && !z.live(), "reset forgets the attempt");
    }
    // ---- what the observer writes, and where it stops --------------------------------------------------------------------------------
    {
        const uintptr_t base = 0x7FF600000000ull;
        mc::Observer o;
        Lines l;
        o.reset(base, base + mc::kFillerReturnRva);
        o.noteGetterCaller(base + 0x28719A4);
        o.noteGetterCaller(base + mc::kFillerReturnRva);
        o.noteGetterCaller(base + 0x28719A4);
        o.noteGetterCaller(base + mc::kFillerReturnRva);
        o.drain(l);
        check(l.v.size() == 2 && l.v[0] == "mono camera: aspect getter called from exe+0x28719A4 (caller 1)" &&
                  l.v[1] == "mono camera: aspect getter called from exe+0x2871D89 (caller 2) -- the mono filler",
              "the first sight of each distinct caller is one line, in order, with the mono filler named; a repeat writes nothing");
        o.drain(l);
        o.noteGetterCaller(base + 0x2A03F51);
        o.drain(l);
        check(l.v.size() == 3 && l.v[2] == "mono camera: aspect getter called from exe+0x2A03F51 (caller 3)", "...a drain writes only what is new");
        Lines cap;
        mc::Observer c;
        c.reset(base, base + mc::kFillerReturnRva);
        for (uintptr_t i = 0; i < 40; ++i) c.noteGetterCaller(base + 0x1000 + i * 16);
        c.drain(cap);
        check(c.callersSeen() == 16 && cap.count("mono camera: aspect getter called from") == 16 &&
                  cap.has("mono camera: more than 16 distinct aspect-getter callers; the rest are not logged") && cap.v.size() == 17,
              "the callers stop at 16, with one line saying more were seen");
        c.noteGetterCaller(base + 0x9000);
        c.drain(cap);
        check(cap.v.size() == 17, "...and the cap line is written once");
        c.noteGetterCaller(base + 0x1000);
        check(c.callersSeen() == 16, "(a repeat of a logged caller does not count as new)");
        Lines w;
        mc::Observer wo;
        wo.reset(base, base + mc::kFillerReturnRva);
        wo.noteWriter(3840, 2160, 1.0f, true, 0.5f, 1.7777778f, base + 0x2871C2E);
        wo.noteWriter(0, 0, 0.0f, false, 0.0f, 0.0f, base + 0x2871D10);
        wo.drain(w);
        check(w.v.size() == 2 &&
                  w.v[0] == "mono camera: aspect writer call 1: width 3840, height 2160, min aspect 1.000000, out 0.500000 -> 1.777778, return exe+0x2871C2E" &&
                  w.v[1] == "mono camera: aspect writer call 2: width 0, height 0, min aspect 0.000000, out unreadable, return exe+0x2871D10",
              "a writer call is one line: width, height, the minimum aspect, *out before and after (or unreadable), the return address");
        for (int i = 0; i < 60; ++i) wo.noteWriter(1, 2, 3.0f, true, 4.0f, 5.0f, base + 0x10);
        wo.drain(w);
        check(w.count("mono camera: aspect writer call ") == 50 && w.has("mono camera: aspect writer: 50 calls logged, the rest are not logged") && w.v.size() == 51 &&
                  wo.writerCalls() == 62,
              "the writer's calls stop at 50, with one line saying more were seen");
        wo.drain(w);
        check(w.v.size() == 51, "...written once");
        o.reset(base, base + mc::kFillerReturnRva);
        Lines again;
        o.drain(again);
        o.noteGetterCaller(base + 0x28719A4);
        o.drain(again);
        check(again.v.size() == 1 && o.callersSeen() == 1, "reset forgets everything");
    }
    // ---- the real hooks, on a synthetic image ------------------------------------------------------------------------------------------
    {
        mc::uninstallForTest();
        edvr::cullcycle::g_monoReads.store(0);
        FakeExe x;
        const bool built = buildExe(x, edvr::cullcycle::kBuildStamp, edvr::cullcycle::kBuildImageSize, true, 0);
        check(built, "(the synthetic executable image is built)");
        if (built) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(x.base);
            const float aspect = 1.7777778f;
            uint8_t getterBefore[16], writerBefore[24];
            std::memcpy(getterBefore, x.base + mc::kGetterRva, sizeof(getterBefore));
            std::memcpy(writerBefore, x.base + mc::kWriterRva, sizeof(writerBefore));
            check(x.fillerReturn == 41 && x.fillerA()() == aspect && x.fillerB()() == aspect,
                  "(before any hook) both stubs read the aspect through the getter's table slot, and the filler's call returns at offset 41, so at ...D89");
            Lines l;
            sinkTarget() = &l;
            mc::frameFor(base, false, &sinkFn);
            check(!mc::g_lazy.attempted() && l.v.empty() && std::memcmp(x.base + mc::kGetterRva, getterBefore, 16) == 0 && !edvr::cullcycle::g_monoHookLive.load(),
                  "with no mono-using key nothing is read or patched, whatever else happens");
            mc::g_lie.store(false);
            mc::frameFor(base, true, &sinkFn);
            check(mc::g_lazy.live() && mc::g_lazy.writerLive() && l.v.size() == 2 &&
                      l.v[0] == "cull probe: mono windows multiply the mono camera's aspect by 1.30 (hook live)" &&
                      l.v[1] == "mono camera: aspect writer observe live (up to 50 calls logged)" && edvr::cullcycle::g_monoHookLive.load() &&
                      mc::g_fillerReturn.load() == base + mc::kFillerReturnRva,
                  "the first wanted frame installs both hooks through the gate (stamp, size, bytes) and says so");
            check(x.base[mc::kGetterRva] == 0xE9 && x.base[mc::kWriterRva] == 0xE9 && std::memcmp(x.base + mc::kGetterRva + 5, getterBefore + 5, 3) == 0,
                  "...and the entry of each function is a jump, the bytes after the five left as they were");
            // No window: every caller gets the original, and the filler's reads are counted.
            check(bitsOf(x.fillerA()()) == bitsOf(aspect) && bitsOf(x.fillerB()()) == bitsOf(aspect) && bitsOf(x.getter()(x.camera)) == bitsOf(aspect) &&
                      edvr::cullcycle::g_monoReads.load() == 1,
                  "no mono window: the filler, another caller and a direct call all read the original aspect, and only the filler's call counted as a mono read");
            x.fillerA()();
            x.fillerA()();
            x.fillerB()();
            check(edvr::cullcycle::g_monoReads.load() == 3, "...reads from the mono filler's return address are counted per call, other callers' are not");
            // A NaN with a payload goes through untouched when there is no window.
            x.camera[0x70 / 4] = floatOf(0x7FC12345u);
            check(bitsOf(x.fillerA()()) == 0x7FC12345u && bitsOf(x.fillerB()()) == 0x7FC12345u, "...a NaN payload comes back bit for bit");
            x.camera[0x70 / 4] = aspect;
            mc::g_lie.store(true);
            check(bitsOf(x.fillerA()()) == bitsOf(aspect * 1.30f) && bitsOf(x.fillerB()()) == bitsOf(aspect) && bitsOf(x.getter()(x.camera)) == bitsOf(aspect),
                  "a mono window: ONLY the call that returns to ...D89 gets 1.30 times the aspect; a stub returning to ...F89 and a direct call get the original");
            check(bitsOf(x.fillerA()()) == bitsOf(aspect * 1.30f) && bitsOf(x.fillerA()()) == bitsOf(aspect * 1.30f), "...every time, not once");
            mc::g_lie.store(false);
            check(bitsOf(x.fillerA()()) == bitsOf(aspect), "the window ends: the original again");
            // The relay gate follows the key: closed, every call runs the game's own bytes and nothing is counted or observed.
            mc::frameFor(base, true, &sinkFn);   // drain what the calls above recorded
            const uint32_t readsBefore = edvr::cullcycle::g_monoReads.load();
            const size_t linesBefore = l.v.size();
            mc::frameFor(base, false, &sinkFn);
            mc::g_lie.store(true);
            check(bitsOf(x.fillerA()()) == bitsOf(aspect) && bitsOf(x.fillerB()()) == bitsOf(aspect) && edvr::cullcycle::g_monoReads.load() == readsBefore &&
                      !edvr::cullcycle::g_monoHookLive.load() && l.v.size() == linesBefore,
                  "the key back at off: even with the lie flag set every call returns the original and nothing is counted (the relay runs the trampoline)");
            mc::g_lie.store(false);
            mc::frameFor(base, true, &sinkFn);
            check(bitsOf(x.fillerA()()) == bitsOf(aspect) && edvr::cullcycle::g_monoReads.load() == readsBefore + 1 && edvr::cullcycle::g_monoHookLive.load() && l.count("cull probe: mono windows") == 1,
                  "...and the key set again reopens it, without a second install or a second line");
            // First sights: the filler, the other stub, and the two places the rig calls the getter directly from.
            check(l.count("mono camera: aspect getter called from") == 4 &&
                      l.has("mono camera: aspect getter called from exe+0x2871D89 (caller 1) -- the mono filler") &&
                      l.has("mono camera: aspect getter called from exe+0x2871F89 (caller 2)"),
                  "the observer logged each distinct caller once: ...D89 named as the mono filler, ...F89, and the rig's two direct call sites");
            // The writer: arguments through, the original runs, its return value comes back, *out is observed and never written by the hook.
            x.out = 0.5f;
            const uintptr_t returned = x.writerCaller()();
            check(returned == reinterpret_cast<uintptr_t>(x.camera) && x.out == aspect,
                  "the writer, hooked and observe-only: the original ran (it wrote 16/9 through *out) and its return value (rax) came back");
            mc::frameFor(base, true, &sinkFn);
            char want[256];
            std::snprintf(want, sizeof(want), "mono camera: aspect writer call 1: width 3840, height 2160, min aspect 1.000000, out 0.500000 -> 1.777778, return exe+0x%llX",
                          static_cast<unsigned long long>(FakeExe::kWriterCaller + x.writerReturn));
            check(l.has(want), "...and logged edx, r8d, xmm3 and *out before and after, and where it was called from");
            for (int i = 0; i < 60; ++i) x.writerCaller()();
            mc::frameFor(base, true, &sinkFn);
            check(l.count("mono camera: aspect writer call ") == 50 && l.has("mono camera: aspect writer: 50 calls logged, the rest are not logged"), "...fifty calls, then the cap line");
            // Uninstall restores the bytes.
            mc::uninstallForTest();
            check(std::memcmp(x.base + mc::kGetterRva, getterBefore, 16) == 0 && std::memcmp(x.base + mc::kWriterRva, writerBefore, 24) == 0 && bitsOf(x.fillerA()()) == bitsOf(aspect) &&
                      !mc::g_lazy.attempted(),
                  "(uninstall puts both functions' bytes back and forgets the attempt)");
            sinkTarget() = nullptr;
        }
        mc::uninstallForTest();
    }
    // ---- the gate, through the real read of an image ------------------------------------------------------------------------------------
    {
        struct Variant {
            const char* name;
            uint32_t stampDelta, sizeDelta;
            bool commit;
            int tamper;
            const char* reason;
        };
        const Variant variants[] = {
            {"another build's stamp", 1, 0, true, 0, "not build 332841"},
            {"another build's image size", 0, 16, true, 0, "not build 332841"},
            {"a getter whose displacement changed", 0, 0, true, 1, "the aspect getter's bytes differ"},
            {"a writer whose prologue changed", 0, 0, true, 2, "the aspect writer's prologue differs"},
            {"an image smaller than the RVAs", 0, 0, false, 0, "the aspect getter's bytes differ"},
        };
        for (const Variant& v : variants) {
            mc::uninstallForTest();
            FakeExe x;
            const bool built = buildExe(x, edvr::cullcycle::kBuildStamp + v.stampDelta, edvr::cullcycle::kBuildImageSize + v.sizeDelta, v.commit, v.tamper);
            Lines l;
            sinkTarget() = &l;
            bool refused = false;
            if (built) {
                uint8_t before[8] = {};
                if (v.commit) std::memcpy(before, x.base + mc::kGetterRva, 8);
                mc::g_lie.store(true);
                const float originalA = v.commit ? x.fillerA()() : 0.0f;   // an unmapped getter must not be called
                mc::frameFor(reinterpret_cast<uintptr_t>(x.base), true, &sinkFn);
                mc::frameFor(reinterpret_cast<uintptr_t>(x.base), true, &sinkFn);
                char want[256];
                std::snprintf(want, sizeof(want), "cull probe: mono windows multiply the mono camera's aspect by 1.30 (hook inert: %s)", v.reason);
                refused = l.v.size() == 1 && l.v[0] == want && !mc::g_lazy.live() && !edvr::cullcycle::g_monoHookLive.load() &&
                          (!v.commit || (std::memcmp(x.base + mc::kGetterRva, before, 8) == 0 && bitsOf(x.fillerA()()) == bitsOf(originalA)));
                mc::g_lie.store(false);
            }
            char name[160];
            std::snprintf(name, sizeof(name), "the gate on %s: nothing is patched, the lie cannot reach the game, and one line says why", v.name);
            check(built && refused, name);
            sinkTarget() = nullptr;
            mc::uninstallForTest();
        }
        // The production entry on this test executable (not Elite): refused at the build, once.
        Lines l;
        sinkTarget() = &l;
        mc::frame(false, &sinkFn);
        check(!mc::g_lazy.attempted() && l.v.empty(), "frame(): an unwanted frame does nothing");
        mc::frame(true, &sinkFn);
        mc::frame(true, &sinkFn);
        check(mc::g_lazy.attempted() && !mc::g_lazy.live() && l.v.size() == 1 &&
                  l.v[0] == "cull probe: mono windows multiply the mono camera's aspect by 1.30 (hook inert: not build 332841)",
              "frame() on an executable that is not build 332841 (this rig): one line, inert");
        sinkTarget() = nullptr;
        mc::uninstallForTest();
    }
}

}  // namespace mono_cases
