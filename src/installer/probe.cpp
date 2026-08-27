#include "probe.h"

#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <vector>

namespace edvr::installer {
namespace {

// ---------------------------------------------------------------------------
// PE export table, read from a mapped view.
//
// Everything below treats the file as hostile: it is on disk because some other
// installer put it there, and a truncated or malformed one must produce
// "Unreadable", never a fault. Bounds are checked on every read, and the whole
// walk sits inside __try -- a mapped-file I/O error (a network drive going away
// mid-parse raises EXCEPTION_IN_PAGE_ERROR, not a return code) lands on the
// same answer.
//
// POD only in here, deliberately: SEH and objects with destructors do not mix
// in one function.
// ---------------------------------------------------------------------------

struct PeFacts {
    bool parsed         = false;
    bool is64           = false;
    bool hasEdvr        = false;
    bool hasVrInit      = false;
    bool hasD3d11Create = false;
};

bool rvaToOffset(const IMAGE_SECTION_HEADER* sections, unsigned count, DWORD rva, DWORD fileSize,
                 DWORD* out) {
    for (unsigned i = 0; i < count; ++i) {
        const IMAGE_SECTION_HEADER& s = sections[i];
        const DWORD vsize = s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
        if (rva >= s.VirtualAddress && rva < s.VirtualAddress + vsize) {
            const DWORD delta = rva - s.VirtualAddress;
            if (delta >= s.SizeOfRawData) return false;  // inside the section, absent from the file
            const DWORD off = s.PointerToRawData + delta;
            if (off >= fileSize) return false;
            *out = off;
            return true;
        }
    }
    return false;
}

void parsePe(const unsigned char* base, DWORD size, PeFacts* facts) {
    __try {
        if (size < sizeof(IMAGE_DOS_HEADER)) return;
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        if (dos->e_lfanew <= 0 ||
            static_cast<DWORD>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > size) {
            return;
        }

        const IMAGE_NT_HEADERS32* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;

        const WORD magic = nt->OptionalHeader.Magic;
        const bool is64 = (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);
        if (!is64 && magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return;

        IMAGE_DATA_DIRECTORY exportDir{};
        if (is64) {
            const IMAGE_NT_HEADERS64* nt64 =
                reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt64->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return;
            exportDir = nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        } else {
            if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return;
            exportDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        }

        facts->is64 = is64;
        facts->parsed = true;  // a valid PE, whatever its exports turn out to be

        if (exportDir.VirtualAddress == 0 || exportDir.Size == 0) return;

        const IMAGE_SECTION_HEADER* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
            base + dos->e_lfanew + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
            nt->FileHeader.SizeOfOptionalHeader);
        const unsigned sectionCount = nt->FileHeader.NumberOfSections;
        if (sectionCount == 0 || sectionCount > 96) return;
        if (reinterpret_cast<const unsigned char*>(sections + sectionCount) > base + size) return;

        DWORD expOff = 0;
        if (!rvaToOffset(sections, sectionCount, exportDir.VirtualAddress, size, &expOff)) return;
        if (expOff + sizeof(IMAGE_EXPORT_DIRECTORY) > size) return;

        const IMAGE_EXPORT_DIRECTORY* exp =
            reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + expOff);
        const DWORD nameCount = exp->NumberOfNames;
        if (nameCount == 0 || nameCount > 65535) return;

        DWORD namesOff = 0;
        if (!rvaToOffset(sections, sectionCount, exp->AddressOfNames, size, &namesOff)) return;
        if (namesOff + nameCount * sizeof(DWORD) > size) return;
        const DWORD* nameRvas = reinterpret_cast<const DWORD*>(base + namesOff);

        for (DWORD i = 0; i < nameCount; ++i) {
            DWORD off = 0;
            if (!rvaToOffset(sections, sectionCount, nameRvas[i], size, &off)) continue;
            const char* name = reinterpret_cast<const char*>(base + off);
            const DWORD room = size - off;  // the name may run to the end of the mapping
            if (room > 5 && memcmp(name, "edvr_", 5) == 0) facts->hasEdvr = true;
            if (room > 7 && memcmp(name, "edvrFss", 7) == 0) facts->hasEdvr = true;
            if (room > 15 && memcmp(name, "VR_InitInternal", 15) == 0) facts->hasVrInit = true;
            if (room > 22 && memcmp(name, "VR_GetGenericInterface", 22) == 0)
                facts->hasVrInit = true;
            if (room > 17 && memcmp(name, "D3D11CreateDevice", 17) == 0)
                facts->hasD3d11Create = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A file that faults while being read is a file we know nothing about,
        // which is exactly what an unparsed PeFacts says.
        facts->parsed = false;
    }
}

std::wstring versionString(const std::wstring& path, const wchar_t* field) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return std::wstring();
    std::vector<unsigned char> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return std::wstring();

    struct LangCp {
        WORD lang, cp;
    };
    LangCp* langs = nullptr;
    UINT langBytes = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&langs), &langBytes) ||
        langBytes < sizeof(LangCp)) {
        return std::wstring();
    }
    const unsigned langCount = langBytes / sizeof(LangCp);
    for (unsigned i = 0; i < langCount; ++i) {
        wchar_t sub[128];
        _snwprintf_s(sub, _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\%s", langs[i].lang, langs[i].cp,
                     field);
        wchar_t* value = nullptr;
        UINT valueLen = 0;
        if (VerQueryValueW(buf.data(), sub, reinterpret_cast<void**>(&value), &valueLen) && value &&
            valueLen > 0) {
            return std::wstring(value, wcsnlen(value, valueLen));
        }
    }
    return std::wstring();
}

bool containsNoCase(const std::wstring& hay, const wchar_t* needle) {
    if (hay.empty()) return false;
    std::wstring h = hay, n = needle;
    for (wchar_t& c : h) c = static_cast<wchar_t>(towlower(c));
    for (wchar_t& c : n) c = static_cast<wchar_t>(towlower(c));
    return h.find(n) != std::wstring::npos;
}

std::string hexDigest(const unsigned char* digest) {
    char out[65]{};
    for (int i = 0; i < 32; ++i) sprintf_s(out + i * 2, 3, "%02x", digest[i]);
    return std::string(out);
}

}  // namespace

std::string sha256Bytes(const void* data, size_t bytes) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return std::string();
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        if (BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)),
                           static_cast<ULONG>(bytes), 0) == 0) {
            unsigned char digest[32]{};
            if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) hex = hexDigest(digest);
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

std::string sha256File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::string();

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        CloseHandle(f);
        return std::string();
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> chunk(64 * 1024);
        bool ok = true;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(f, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)) {
                ok = false;
                break;
            }
            if (read == 0) break;
            if (BCryptHashData(hash, chunk.data(), read, 0) != 0) {
                ok = false;
                break;
            }
        }
        if (ok) {
            unsigned char digest[32]{};
            if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) hex = hexDigest(digest);
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}

DllInfo probeDll(const std::wstring& path) {
    DllInfo info;
    info.path = path;

    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        // "Not there" and "there but we cannot open it" are different answers:
        // a locked file -- the game running, an antivirus mid-scan -- must not
        // be planned around as though the slot were empty.
        info.kind = (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                        ? DllKind::Absent
                        : DllKind::Unreadable;
        return info;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart == 0 || size.QuadPart > (64ll << 20)) {
        CloseHandle(f);
        info.kind = DllKind::Unreadable;
        return info;
    }
    info.size = static_cast<unsigned long long>(size.QuadPart);

    PeFacts facts;
    HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (map) {
        const unsigned char* view =
            static_cast<const unsigned char*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
        if (view) {
            parsePe(view, static_cast<DWORD>(size.QuadPart), &facts);
            UnmapViewOfFile(view);
        }
        CloseHandle(map);
    }
    CloseHandle(f);

    if (!facts.parsed) {
        info.kind = DllKind::Unreadable;
        return info;
    }

    info.is64 = facts.is64;
    info.hasEdvrExports = facts.hasEdvr;
    info.hasVrInit = facts.hasVrInit;
    info.hasD3d11Create = facts.hasD3d11Create;

    // Ours first, and unconditionally. An EDVR proxy exports everything the DLL
    // it stands in for exports, so testing for VR_InitInternal or
    // D3D11CreateDevice first would file every one of our own installs as the
    // thing it replaced -- and the installer would then rename our own proxy
    // aside believing it had found the game's original.
    if (facts.hasEdvr)
        info.kind = DllKind::Edvr;
    else if (facts.hasVrInit)
        info.kind = DllKind::OpenVrRuntime;
    else if (facts.hasD3d11Create)
        info.kind = DllKind::D3d11Provider;
    else
        info.kind = DllKind::Foreign;

    info.product = versionString(path, L"ProductName");
    info.description = versionString(path, L"FileDescription");
    info.company = versionString(path, L"CompanyName");
    info.fileVersion = versionString(path, L"FileVersion");
    info.sha256 = sha256File(path);
    return info;
}

std::wstring modNameOf(const DllInfo& info) {
    const std::wstring fields[] = {info.product, info.description, info.company};
    for (const std::wstring& s : fields) {
        if (containsNoCase(s, L"reshade")) return L"ReShade";
        if (containsNoCase(s, L"3dmigoto")) return L"EDHM";  // EDHM ships 3Dmigoto's d3d11.dll
        if (containsNoCase(s, L"edhm")) return L"EDHM";
        if (containsNoCase(s, L"dxvk")) return L"DXVK";
        if (containsNoCase(s, L"opencomposite")) return L"OpenComposite";
        if (containsNoCase(s, L"special k")) return L"Special K";
    }
    return std::wstring();
}

std::wstring chainNameFor(const DllInfo& info) {
    const std::wstring mod = modNameOf(info);
    if (mod == L"ReShade") return L"d3d11_reshade.dll";
    if (mod == L"EDHM") return L"d3d11_edhm.dll";
    if (mod == L"DXVK") return L"d3d11_dxvk.dll";
    if (mod == L"Special K") return L"d3d11_specialk.dll";
    return L"d3d11_other.dll";
}

std::wstring describeDll(const DllInfo& info) {
    switch (info.kind) {
        case DllKind::Absent:
            return L"not present";
        case DllKind::Unreadable:
            return L"present, but unreadable (locked, or not a DLL)";
        default:
            break;
    }
    std::wstring s;
    const std::wstring mod = modNameOf(info);
    if (info.kind == DllKind::Edvr) {
        s = L"EDVR";
    } else if (!mod.empty()) {
        s = mod;
    } else if (!info.product.empty()) {
        s = info.product;
    } else if (info.kind == DllKind::OpenVrRuntime) {
        s = L"the game's OpenVR runtime";
    } else if (info.kind == DllKind::D3d11Provider) {
        s = L"another d3d11 mod";
    } else {
        s = L"an unrecognised DLL";
    }
    if (!info.fileVersion.empty()) s += L" " + info.fileVersion;
    wchar_t tail[64];
    _snwprintf_s(tail, _TRUNCATE, L" (%.1f MB%s)", info.size / (1024.0 * 1024.0),
                 info.is64 ? L"" : L", 32-bit");
    s += tail;
    return s;
}

}  // namespace edvr::installer
