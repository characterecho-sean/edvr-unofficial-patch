#include "proxy.h"

#include <cstdio>
#include <vector>

#include "log.h"

namespace edvr {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (need <= 0) return std::wstring();
    std::vector<wchar_t> buf(need, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, buf.data(), need);
    return std::wstring(buf.data());
}

bool isAbsolutePath(const std::wstring& p) {
    if (p.size() >= 2 && p[1] == L':') return true;
    if (p.size() >= 2 && (p[0] == L'\\' || p[0] == L'/')) return true;
    return false;
}

}  // namespace

namespace {
// Builds this fix has actually been verified against.
// Adding a build here is a claim that someone re-ran the capture and confirmed
// the hashes, not that it probably still works.
const char* const kVerifiedBuilds[] = {"330683"};
}  // namespace

std::string gameBuildVersion() {
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return std::string();

    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(exePath, &handle);
    if (size == 0) return std::string();

    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(exePath, handle, size, data.data())) return std::string();

    struct LangCodepage { WORD language, codePage; };
    LangCodepage* lang = nullptr;
    UINT langBytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&lang), &langBytes) ||
        langBytes < sizeof(LangCodepage)) {
        return std::string();
    }

    wchar_t query[128];
    _snwprintf_s(query, _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\FileVersion",
                 lang->language, lang->codePage);

    wchar_t* value = nullptr;
    UINT valueChars = 0;
    if (!VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&value),
                        &valueChars) ||
        !value) {
        return std::string();
    }

    std::string out;
    for (UINT i = 0; i < valueChars && value[i]; ++i) {
        out.push_back(static_cast<char>(value[i]));
    }
    return out;
}

bool gameBuildIsVerified() {
    const std::string build = gameBuildVersion();
    if (build.empty()) return false;
    for (const char* known : kVerifiedBuilds) {
        if (build == known) return true;
    }
    return false;
}

void breadcrumb(const char* stage) {
    if (!stage) return;

    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return;
    for (int i = static_cast<int>(wcslen(path)) - 1; i >= 0; --i) {
        if (path[i] == L'\\') { path[i] = 0; break; }
    }
    // Built by hand rather than with a formatting call: this runs under loader
    // lock and must not touch the CRT locale or heap.
    static const wchar_t kName[] = L"\\edvr_breadcrumbs.txt";
    if (wcslen(path) + wcslen(kName) >= MAX_PATH) return;
    wcscat_s(path, MAX_PATH, kName);

    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    char line[256];
    size_t n = 0;
    const DWORD tick = GetTickCount();
    // Fixed-width decimal tick, written digit by digit.
    for (int div = 100000000; div > 0; div /= 10) {
        const char digit = static_cast<char>('0' + ((tick / div) % 10));
        if (n || digit != '0' || div == 1) line[n++] = digit;
    }
    line[n++] = ' ';
    for (const char* p = stage; *p && n < sizeof(line) - 3; ++p) line[n++] = *p;
    line[n++] = '\r';
    line[n++] = '\n';

    DWORD written = 0;
    WriteFile(f, line, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(f);
}

void writeFatalNote(const std::wstring& dir, const wchar_t* text) {
    OutputDebugStringW(L"[edvr] ");
    OutputDebugStringW(text);
    OutputDebugStringW(L"\n");

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\edvr_FATAL.txt", dir.c_str());
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1024];
    const int n = _snwprintf_s(line, _TRUNCATE, L"[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n",
                               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                               st.wSecond, text);
    if (n > 0) {
        // UTF-8, to match the log. This wrote raw UTF-16LE with no BOM, on the
        // reasoning that a human in a text editor would cope. Half of them do
        // not: PowerShell's Get-Content renders it as every character separated
        // by a space, which is how it looked when this was found. Since this is
        // the file someone is asked to paste when the game will not start, it
        // has to survive being opened by whatever they happen to open it with.
        //
        // No BOM: the file is opened for append, so a second failure would put
        // one in the middle. The content is ASCII apart from paths, and UTF-8 is
        // what a modern editor assumes anyway.
        char utf8[1024 * 3];
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof(utf8),
                                              nullptr, nullptr);
        if (bytes > 0) {
            DWORD written = 0;
            WriteFile(f, utf8, static_cast<DWORD>(bytes), &written, nullptr);
        }
    }
    CloseHandle(f);
}

std::string readConfigStringEarly(const std::wstring& moduleDir, const wchar_t* iniName,
                                  const char* dottedKey) {
    if (moduleDir.size() + wcslen(iniName) + 2 > MAX_PATH) return std::string();
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\%s", moduleDir.c_str(), iniName);

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::string();  // absent file: nothing configured

    // The whole file, with the same 1 MB ceiling Config::parse uses.
    //
    // This read an 8 KB stack buffer at first, on the reasoning that an ini
    // bigger than that was not worth supporting this early. It was worth
    // supporting: a key past the cut-off is silently ignored, the loader takes
    // the system DLL, and Config -- which does read the whole file -- then logs
    // that chaining is in effect. The log ends up asserting the opposite of what
    // happened, which is the one outcome a support path must never produce. The
    // private repo's ini is already 9 KB.
    //
    // Heap here is not the hazard the stack buffer was avoiding: the loader lock
    // and the heap lock are different locks, this function's own parse loop
    // allocates a std::string per line regardless, and the caller has already
    // done `new std::wstring` by the time it gets here.
    const DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE || size > (1u << 20)) {
        CloseHandle(f);
        return std::string();
    }
    std::vector<char> text(size + 1, 0);
    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(f, text.data(), size, &bytesRead, nullptr);
    CloseHandle(f);
    if (!ok) return std::string();

    // Below is a copy of Config::parse's loop, and must stay one: the header
    // comment promises a value read here matches the same value read later
    // through Config, and that promise is only kept by the two agreeing line
    // for line.
    std::string section;
    const char* p = text.data();
    const char* end = p + bytesRead;
    while (p < end) {
        const char* lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') ++lineEnd;
        std::string line(p, lineEnd);
        p = lineEnd;
        while (p < end && (*p == '\n' || *p == '\r')) ++p;

        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        line = line.substr(first);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            const size_t close = line.find(']');
            if (close != std::string::npos) section = line.substr(1, close - 1);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            const size_t a = s.find_first_not_of(" \t");
            if (a == std::string::npos) { s.clear(); return; }
            const size_t b = s.find_last_not_of(" \t");
            s = s.substr(a, b - a + 1);
        };
        trim(key);
        trim(val);
        if (key.empty()) continue;

        const std::string flat = section.empty() ? key : section + "." + key;
        if (flat == dottedKey) return val;
    }
    return std::string();
}

HMODULE loadRealModule(const std::wstring& moduleDir, const std::string& configuredPath,
                       const wchar_t* systemFallback, const wchar_t* what) {
    if (!configuredPath.empty()) {
        std::wstring p = widen(configuredPath);
        if (!isAbsolutePath(p)) p = moduleDir + L"\\" + p;
        HMODULE m = LoadLibraryW(p.c_str());
        if (m) {
            Log::get().note("real %S loaded from configured path %S", what, p.c_str());
            return m;
        }
        wchar_t msg[768];
        _snwprintf_s(msg, _TRUNCATE,
                     L"could not load configured real %s at '%s' (error %lu)", what,
                     p.c_str(), GetLastError());
        writeFatalNote(moduleDir, msg);
        // Fall through to the system copy rather than failing outright.
    }

    if (systemFallback) {
        wchar_t sys[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring p = std::wstring(sys) + L"\\" + systemFallback;
            HMODULE m = LoadLibraryW(p.c_str());
            if (m) {
                Log::get().note("real %S loaded from %S", what, p.c_str());
                return m;
            }
            wchar_t msg[768];
            _snwprintf_s(msg, _TRUNCATE, L"could not load %s (error %lu)", p.c_str(),
                         GetLastError());
            writeFatalNote(moduleDir, msg);
        }
    }

    wchar_t msg[512];
    _snwprintf_s(msg, _TRUNCATE,
                 L"FATAL: no real %s could be loaded. The game will not work "
                 L"correctly. Remove the edvr proxy from the game directory.",
                 what);
    writeFatalNote(moduleDir, msg);
    return nullptr;
}

std::wstring widenUtf8(const std::string& s) { return widen(s); }

size_t resolveProcsChained(HMODULE preferred, HMODULE fallback, const char* const* names,
                           size_t count, void** procs, void* unresolvedStub,
                           size_t* fromFallback) {
    size_t missing = 0, fell = 0;
    for (size_t i = 0; i < count; ++i) {
        void* p = preferred ? reinterpret_cast<void*>(GetProcAddress(preferred, names[i]))
                            : nullptr;
        if (!p && fallback) {
            p = reinterpret_cast<void*>(GetProcAddress(fallback, names[i]));
            if (p) ++fell;
        }
        if (!p) {
            ++missing;
            p = unresolvedStub;
        }
        procs[i] = p;
    }
    if (fromFallback) *fromFallback = fell;
    return missing;
}

size_t resolveProcs(HMODULE real, const char* const* names, size_t count, void** procs,
                    void* unresolvedStub) {
    // No real module: every slot gets the stub, and every export is missing.
    //
    // Returning 0 and leaving the table as it was found meant leaving it full of
    // zeros, because that is how the generated table is defined. The thunks are
    // bare `jmp QWORD PTR [slot]` with no null check, so the first forwarded
    // call jumped through address 0 -- an access violation at startup, on the
    // exact path that exists to degrade gracefully. The stub is here precisely
    // so that "returning zero beats jumping through a null slot"; it just was
    // never installed on the one path that needed it most.
    if (!real) {
        for (size_t i = 0; i < count; ++i) procs[i] = unresolvedStub;
        return count;
    }
    size_t missing = 0;
    for (size_t i = 0; i < count; ++i) {
        void* p = reinterpret_cast<void*>(GetProcAddress(real, names[i]));
        if (!p) {
            ++missing;
            p = unresolvedStub;
        }
        procs[i] = p;
    }
    return missing;
}

}  // namespace edvr
