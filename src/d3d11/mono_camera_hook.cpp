#include "mono_camera_hook.h"
#include "mono_camera_core.h"
#include "../common/code_hook.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstring>

// The mono camera hooks (mono_camera_core.h). The same shape as object_record_writer_hook.cpp: a relay page within reach of the target
// holds `mov rax,&gate; cmp [rax],0; je original; jmp [detour]`, and the detour is entered by a JMP, so _ReturnAddress() inside it is
// the address the GAME will be returned to. With the gate at 0 the relay runs the trampoline, which is the target's own bytes.

namespace edvr::monocam {
namespace {

constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;
alignas(8) std::atomic<uint64_t> g_gate{0};   // 1: the relays enter the detours; 0: they run the original bytes
static_assert(sizeof(g_gate) == 8 && std::atomic<uint64_t>::is_always_lock_free, "The relay reads the aligned atomic directly.");
std::atomic<void*> g_getterForward{nullptr}, g_writerForward{nullptr};

using GetterFn = float (__fastcall*)(const void*);
using WriterFn = uintptr_t (__fastcall*)(void*, uint32_t, uint32_t, float, float*);

bool readFloat(const float* at, float* out) noexcept {
    __try {
        *out = *at;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The aspect getter: `movss xmm0,[rcx+70h]` and a return. The original's value comes back in xmm0 untouched except for the mono filler's
// call during a mono window (onAspectGetter decides, mono_camera_core.h).
__declspec(noinline) float __fastcall getterDetour(const void* camera) {
    const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const float original = reinterpret_cast<GetterFn>(g_getterForward.load(std::memory_order_acquire))(camera);
    return onAspectGetter(original, returnAddress);
}

// FUN_28634E0, observe-only: the original is called with the arguments it was given (rcx, edx, r8d, xmm3 and the fifth, a float*, on
// the stack) and what it returned is returned. *out is read before and after, never written.
__declspec(noinline) uintptr_t __fastcall writerDetour(void* camera, uint32_t width, uint32_t height, float minAspect, float* out) {
    const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
    float before = 0.0f, after = 0.0f;
    bool readable = readFloat(out, &before);
    const uintptr_t result = reinterpret_cast<WriterFn>(g_writerForward.load(std::memory_order_acquire))(camera, width, height, minAspect, out);
    if (readable) readable = readFloat(out, &after);
    g_observer.noteWriter(width, height, minAspect, readable, before, after, returnAddress);
    return result;
}

struct Relay {
    uint8_t* bytes = nullptr;
    std::atomic<void*>* forward = nullptr;
};
struct Hooked {
    CodeHook hook;
    Relay relay;
};
Hooked* g_getter = nullptr;
Hooked* g_writer = nullptr;

uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t floor = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t distance = uintptr_t(INT32_MAX) - 0x10000u;
    uintptr_t at = target > distance ? target - distance : floor;
    if (at < floor) at = floor;
    const uintptr_t limit = target > ceiling - distance ? ceiling : target + distance;
    while (at < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region))) break;
        const uintptr_t start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - start) break;
        const uintptr_t end = start + region.RegionSize;
        if (region.State == MEM_FREE) {
            uintptr_t candidate = at > start ? at : start;
            if (candidate > UINTPTR_MAX - (granularity - 1)) break;
            candidate = (candidate + granularity - 1) & ~(granularity - 1);
            if (candidate < limit && candidate < end && end - candidate >= 4096) {
                auto* p = static_cast<uint8_t*>(
                    VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline]. RAX and the flags are volatile
    // and neither function takes an argument in them (rcx, edx, r8d, xmm3 and the stack are untouched).
    const uint8_t body[kRelayBytes] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x83, 0x38, 0, 0x74, 0x0E, 0xFF, 0x25, 0, 0, 0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8);
    std::memcpy(code + 22, &callbackAddress, 8);
}

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& relay = *static_cast<Relay*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(relay.bytes + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(relay.bytes, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), relay.bytes, kRelayBytes))
        return false;
    relay.forward->store(trampoline, std::memory_order_release);
    return true;
}

const char* installOne(Hooked** slot, uintptr_t target, void* detour, std::atomic<void*>* forward, const char* who) noexcept {
    try {
        auto* hooked = new Hooked;
        hooked->relay.forward = forward;
        hooked->relay.bytes = allocateRelay(target);
        if (!hooked->relay.bytes) {
            delete hooked;
            return "no room for the relay page near the target";
        }
        buildRelay(hooked->relay.bytes, &g_gate, detour);
        if (!hooked->hook.install(reinterpret_cast<void*>(target), hooked->relay.bytes, nullptr, who, &prepareRelay, &hooked->relay)) {
            forward->store(nullptr, std::memory_order_release);
            VirtualFree(hooked->relay.bytes, 0, MEM_RELEASE);
            delete hooked;
            return "the code hook refused it (the CodeHook line above says why)";
        }
        *slot = hooked;   // process lifetime: a thread may be inside the trampoline when the key goes back to off
        return nullptr;
    } catch (...) {
        return "install failed";
    }
}

InstallResult installAll(uintptr_t base) {
    InstallResult result;
    g_observer.reset(base, base + kFillerReturnRva);
    g_fillerReturn.store(base + kFillerReturnRva, std::memory_order_relaxed);
    result.getter = installOne(&g_getter, base + kGetterRva, reinterpret_cast<void*>(&getterDetour), &g_getterForward,
                               "mono-camera-aspect-getter");
    if (result.getter) {
        g_fillerReturn.store(0, std::memory_order_relaxed);
        return result;
    }
    result.writer = installOne(&g_writer, base + kWriterRva, reinterpret_cast<void*>(&writerDetour), &g_writerForward,
                               "mono-camera-aspect-writer");
    return result;
}

// What the gate reads from the executable: the PE stamp and image size, and the bytes of both functions. Each read is guarded: another
// build's image may be smaller than the RVAs.
struct Image {
    bool headers = false, getterRead = false, writerRead = false;
    uint32_t stamp = 0, size = 0;
    uint8_t getter[sizeof(kGetterBytes)] = {};
    uint8_t writer[sizeof(kWriterPrologue)] = {};
};

bool copyBytes(uintptr_t from, void* to, size_t count) noexcept {
    __try {
        std::memcpy(to, reinterpret_cast<const void*>(from), count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

Image readImage(uintptr_t base) noexcept {
    Image image;
    IMAGE_DOS_HEADER dos{};
    if (copyBytes(base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0) {
        IMAGE_NT_HEADERS nt{};
        if (copyBytes(base + static_cast<uintptr_t>(dos.e_lfanew), &nt, sizeof(nt)) && nt.Signature == IMAGE_NT_SIGNATURE) {
            image.headers = true;
            image.stamp = nt.FileHeader.TimeDateStamp;
            image.size = nt.OptionalHeader.SizeOfImage;
        }
    }
    if (image.headers) {
        image.getterRead = copyBytes(base + kGetterRva, image.getter, sizeof(image.getter));
        image.writerRead = copyBytes(base + kWriterRva, image.writer, sizeof(image.writer));
    }
    return image;
}

}  // namespace

void frameFor(uintptr_t base, bool wanted, void (*sink)(const char*)) {
    g_lazy.frame(
        wanted,
        [&]() -> const char* {
            const Image image = readImage(base);
            return gateReason(image.headers, image.stamp, image.size, image.getterRead ? image.getter : nullptr,
                              image.writerRead ? image.writer : nullptr);
        },
        [&]() -> InstallResult { return installAll(base); }, sink);
    const bool on = wanted && g_lazy.live();
    g_gate.store(on ? 1u : 0u, std::memory_order_relaxed);
    cullcycle::g_monoHookLive.store(on, std::memory_order_relaxed);
    if (g_lazy.live()) g_observer.drain(sink);
}

void frame(bool wanted, void (*sink)(const char*)) {
    frameFor(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), wanted, sink);
}

void uninstallForTest() {
    g_gate.store(0, std::memory_order_relaxed);
    Hooked** slots[2] = {&g_getter, &g_writer};
    for (Hooked** slot : slots) {
        if (!*slot) continue;
        (*slot)->hook.uninstall();
        VirtualFree((*slot)->relay.bytes, 0, MEM_RELEASE);
        delete *slot;
        *slot = nullptr;
    }
    g_getterForward.store(nullptr, std::memory_order_release);
    g_writerForward.store(nullptr, std::memory_order_release);
    g_lie.store(false, std::memory_order_relaxed);
    g_fillerReturn.store(0, std::memory_order_relaxed);
    g_observer.reset(0, 0);
    g_lazy.reset();
    cullcycle::g_monoHookLive.store(false, std::memory_order_relaxed);
}

}  // namespace edvr::monocam
