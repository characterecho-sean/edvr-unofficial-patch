#include "pose_latch_patch.h"

#include <windows.h>

#include <cstring>

namespace edvr::cullpose {
namespace {

Driver g_driver;

bool copyBytes(uintptr_t from, void* to, size_t count) noexcept {
    __try {
        std::memcpy(to, reinterpret_cast<const void*>(from), count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

LatchImage readLatchImage(uintptr_t base) noexcept {
    LatchImage image;
    IMAGE_DOS_HEADER dos{};
    if (base && copyBytes(base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0) {
        IMAGE_NT_HEADERS nt{};
        if (copyBytes(base + static_cast<uintptr_t>(dos.e_lfanew), &nt, sizeof(nt)) && nt.Signature == IMAGE_NT_SIGNATURE) {
            image.headers = true;
            image.stamp = nt.FileHeader.TimeDateStamp;
            image.imageSize = nt.OptionalHeader.SizeOfImage;
        }
    }
    // Only a matching image has these addresses to read; another build's image may end before them (the copy is guarded either way).
    if (image.headers) image.bytesRead = copyBytes(base + kLatchWordRva, image.bytes, sizeof(image.bytes));
    return image;
}

bool writeWordAtomic(void* address, uint64_t word) noexcept {
    if (!address || (reinterpret_cast<uintptr_t>(address) & 7u) != 0) return false;
    DWORD previous = 0;
    // Execute stays on while the store happens: other threads may be running this very code.
    if (!VirtualProtect(address, sizeof(word), PAGE_EXECUTE_READWRITE, &previous)) return false;
    storeWordAtomic(address, word);
    DWORD ignored = 0;
    VirtualProtect(address, sizeof(word), previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, sizeof(word));
    return true;
}

uint32_t frameFor(uintptr_t base, Mode requested, void (*sink)(const char*)) {
    return g_driver.frame(
        requested, [base] { return readLatchImage(base); },
        [base](uint64_t word) { return writeWordAtomic(reinterpret_cast<void*>(base + kLatchWordRva), word); }, sink);
}

uint32_t frame(Mode requested, void (*sink)(const char*)) {
    return frameFor(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), requested, sink);
}

const Driver& driver() { return g_driver; }

void resetForTest() { g_driver = Driver(); }

}  // namespace edvr::cullpose
