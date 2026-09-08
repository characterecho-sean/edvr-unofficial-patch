#include "iat_hook.h"

#include <windows.h>

#include <cstring>

namespace edvr {

namespace {

// The EXE's import directory, or null when the headers do not read as a
// PE image. Every pointer here is validated against the image's mapped
// size before it is dereferenced: a malformed table must read as "not
// found", never as a fault.
const IMAGE_IMPORT_DESCRIPTOR* importDirectory(const BYTE* base, DWORD* imageSize) {
    if (!base) return nullptr;
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const IMAGE_NT_HEADERS* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return nullptr;
    *imageSize = nt->OptionalHeader.SizeOfImage;
    if (dir.VirtualAddress >= *imageSize) return nullptr;
    return reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
}

bool inImage(DWORD rva, DWORD imageSize) { return rva != 0 && rva < imageSize; }

}  // namespace

bool iatHookInstall(const char* module, const char* function, void* replacement,
                    IatPatch* patch) {
    if (!module || !function || !replacement || !patch) return false;
    if (patch->applied) return false;

    const BYTE* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    DWORD imageSize = 0;
    const IMAGE_IMPORT_DESCRIPTOR* desc = importDirectory(base, &imageSize);
    if (!desc) return false;

    for (; inImage(desc->Name, imageSize) && desc->Name; ++desc) {
        const char* name = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(name, module) != 0) continue;
        // OriginalFirstThunk names the functions; FirstThunk is the table the
        // loader wrote the addresses into. Walk them in step.
        if (!inImage(desc->OriginalFirstThunk, imageSize) ||
            !inImage(desc->FirstThunk, imageSize)) {
            continue;
        }
        const IMAGE_THUNK_DATA* names =
            reinterpret_cast<const IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA* addrs =
            reinterpret_cast<IMAGE_THUNK_DATA*>(const_cast<BYTE*>(base) + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (!inImage(static_cast<DWORD>(names->u1.AddressOfData), imageSize)) break;
            const IMAGE_IMPORT_BY_NAME* by =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(by->Name, function) != 0) continue;

            void** slot = reinterpret_cast<void**>(&addrs->u1.Function);
            void* current = *slot;
            if (current == replacement) return false;   // already ours
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
            // An exchange, so a concurrent reader sees the old pointer or the
            // new one and never a torn value; the original is what the slot
            // held at that instant, which is what chaining needs.
            void* was = InterlockedExchangePointer(slot, replacement);
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), old, &ignored);
            patch->slot = slot;
            patch->original = was;
            patch->replacement = replacement;
            patch->applied = true;
            return true;
        }
    }
    return false;
}

void iatHookUninstall(IatPatch* patch) {
    if (!patch || !patch->applied || !patch->slot) return;
    patch->applied = false;
    if (*patch->slot != patch->replacement) return;   // somebody else's now
    DWORD old = 0;
    if (!VirtualProtect(patch->slot, sizeof(void*), PAGE_READWRITE, &old)) return;
    InterlockedCompareExchangePointer(patch->slot, patch->original, patch->replacement);
    DWORD ignored = 0;
    VirtualProtect(patch->slot, sizeof(void*), old, &ignored);
}

void iatHookEntryModule(const void* entry, char* buf, size_t bufLen) {
    if (!buf || !bufLen) return;
    buf[0] = 0;
    HMODULE m = nullptr;
    if (entry &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(entry), &m) &&
        m) {
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(m, path, MAX_PATH)) {
            const wchar_t* leaf = wcsrchr(path, L'\\');
            leaf = leaf ? leaf + 1 : path;
            WideCharToMultiByte(CP_UTF8, 0, leaf, -1, buf, static_cast<int>(bufLen), nullptr,
                                nullptr);
            buf[bufLen - 1] = 0;
            return;
        }
    }
    strncpy(buf, "?", bufLen);
    buf[bufLen - 1] = 0;
}

}  // namespace edvr
