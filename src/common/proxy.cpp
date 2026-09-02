#include "proxy.h"

#include <cstdio>
#include <vector>

#include "config.h"  // executableDirectory
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

std::string verifiedBuildList() {
    std::string out;
    for (const char* known : kVerifiedBuilds) {
        if (!out.empty()) out += ", ";
        out += known;
    }
    return out;
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
    const uint64_t tick = GetTickCount64();
    // Fixed-width decimal tick, written digit by digit.
    //
    // THE DIVISOR STARTED AT 100000000, WHICH IS NINE DIGITS, and a tick count
    // reaches ten of them at 11.57 days of uptime. Past that the leading digit
    // was simply dropped: 1,000,000,000 rendered as "0", and 1,036,800,000 as
    // "36800000" -- a stamp that reads as EARLIER than the lines before it.
    // The source was GetTickCount, a DWORD, so it also wrapped outright at
    // 49.7 days.
    //
    // This was survivable while these lines were only ever read as an ordered
    // sequence within one boot. The heartbeat makes them a QUANTITY -- the
    // whole point is comparing the last one against the log's own clock -- and
    // a reporter with a fortnight of uptime is not exotic; issue #19's was at
    // 9.3 days, which is inside a fortnight of tripping it. The two clocks now
    // agree, and 1e19 is the largest power of ten a uint64 holds -- twenty
    // digits, which is all of GetTickCount64. (1e18 was the first attempt and
    // drops the leading digit of a full-range tick: the same defect this hunk
    // is about, one order of magnitude further out.)
    for (uint64_t div = 10000000000000000000ull; div > 0; div /= 10) {
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

namespace {

// Appends one decimal number to a hand-built line. Same posture as breadcrumb
// itself: no CRT formatting, because the crash handler below runs in a process
// that is already failing and printf is not a thing to reach for there.
void appendNum(char* line, size_t& n, size_t cap, uint64_t v) {
    char tmp[24];
    size_t t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < sizeof(tmp)) { tmp[t++] = static_cast<char>('0' + (v % 10)); v /= 10; }
    while (t > 0 && n < cap - 1) line[n++] = tmp[--t];
}

void appendStr(char* line, size_t& n, size_t cap, const char* s) {
    for (; s && *s && n < cap - 1; ++s) line[n++] = *s;
}

void appendHex(char* line, size_t& n, size_t cap, uint64_t v) {
    appendStr(line, n, cap, "0x");
    bool lead = true;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const unsigned d = static_cast<unsigned>((v >> shift) & 0xF);
        if (d == 0 && lead && shift > 0) continue;
        lead = false;
        if (n < cap - 1) line[n++] = static_cast<char>(d < 10 ? '0' + d : 'A' + (d - 10));
    }
}

LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
bool g_filterInstalled = false;

LONG WINAPI edvrCrashFilter(EXCEPTION_POINTERS* info) {
    // A STACK OVERFLOW IS THE ONE CRASH THIS MUST NOT TOUCH.
    //
    // On EXCEPTION_STACK_OVERFLOW the filter runs on what is left of the last
    // guard page. This function's own frame is a char[256] and a wchar_t[260],
    // breadcrumb's is another 256 plus a MAX_PATH path buffer, and CreateFileW
    // below that is several kilobytes of ntdll. Overflowing again inside a
    // top-level filter is not a second chance: the process is terminated on the
    // spot, which means g_prevFilter never runs and the game's own crash
    // reporter never runs either. Writing one diagnostic line would cost the
    // player the dialog that actually reports the bug.
    //
    // This is not a hypothetical shape here. d3d11_proxy.cpp records a chained
    // proxy recursing "until the stack ran out ... STATUS_STACK_OVERFLOW,
    // 0xC00000FD, with nothing logged, which is exactly what EDHM users
    // reported" -- so the mod chain this feature exists to disambiguate is the
    // one that produces this code. Hand it straight on.
    if (info && info->ExceptionRecord &&
        info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        return g_prevFilter ? g_prevFilter(info) : EXCEPTION_CONTINUE_SEARCH;
    }

    // Guarded so a fault raised while reporting a fault cannot loop. One line
    // is the whole budget: the process is going down either way, and a handler
    // that tries to be thorough is a handler that hangs the crash dialog.
    static volatile long entered = 0;
    if (InterlockedExchange(&entered, 1) != 0) {
        return g_prevFilter ? g_prevFilter(info) : EXCEPTION_CONTINUE_SEARCH;
    }

    char line[256];
    size_t n = 0;
    appendStr(line, n, sizeof(line), "gfx: UNHANDLED exception ");
    const void* addr = nullptr;
    if (info && info->ExceptionRecord) {
        appendHex(line, n, sizeof(line), info->ExceptionRecord->ExceptionCode);
        addr = info->ExceptionRecord->ExceptionAddress;
        appendStr(line, n, sizeof(line), " at ");
        appendHex(line, n, sizeof(line), reinterpret_cast<uintptr_t>(addr));
    } else {
        appendStr(line, n, sizeof(line), "(no record)");
    }

    // WHOSE CODE IT WAS. The point of the whole exercise.
    appendStr(line, n, sizeof(line), " in ");
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH]{};
    if (addr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) &&
        mod && GetModuleFileNameW(mod, path, MAX_PATH) > 0) {
        const wchar_t* base = path;
        for (const wchar_t* p = path; *p; ++p) {
            if (*p == L'\\') base = p + 1;
        }
        // Module names are ASCII in practice; anything else is written as '?'
        // rather than risking a conversion call here.
        for (; *base && n < sizeof(line) - 1; ++base) {
            line[n++] = (*base < 0x80) ? static_cast<char>(*base) : '?';
        }
        appendStr(line, n, sizeof(line), " +");
        appendHex(line, n, sizeof(line),
                  reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod));
    } else if (addr) {
        appendStr(line, n, sizeof(line), "no module (address is not in a loaded image)");
    } else {
        // Distinct from the line above, which asserts something about an
        // address. There was no address to assert it about.
        appendStr(line, n, sizeof(line), "no module (no exception record)");
    }
    line[n] = 0;
    breadcrumb(line);

    // Elite installs its own reporter and the player expects its dialog. Ours
    // is a note in the margin, not a replacement.
    return g_prevFilter ? g_prevFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void breadcrumbInstallCrashHandler() {
    static volatile long done = 0;
    if (InterlockedExchange(&done, 1) != 0) return;
    g_prevFilter = SetUnhandledExceptionFilter(edvrCrashFilter);
    g_filterInstalled = true;
}

void breadcrumbRemoveCrashHandler() {
    // A FILTER LEFT BEHIND BY AN UNLOADED MODULE POINTS INTO FREED ADDRESS
    // SPACE, and the next unhandled exception in the process jumps into it --
    // so the cost of skipping this is not "no breadcrumb", it is that the game
    // loses its crash reporting entirely, in a process where a mod unloading
    // is exactly the sort of thing worth reporting. Everything else installed
    // here is undone on the FreeLibrary branch; this is not an exception.
    if (!g_filterInstalled) return;
    g_filterInstalled = false;
    // Only take ours down. If another module installed a filter after we did,
    // restoring g_prevFilter would throw THEIRS away as well, so the chain goes
    // back only when the top of it is still us.
    //
    // AND WHEN IT IS NOT, THIS FUNCTION DOES NOT ACHIEVE ITS PURPOSE. That
    // module saved a `prev` pointing at edvrCrashFilter, in an image that is
    // about to be unmapped, and nothing reachable from here can reach into its
    // private state to correct it. The choice is between one dangling pointer
    // and two; this takes the one. Said plainly because the alternative is a
    // comment claiming a hazard was removed when it was moved.
    LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(g_prevFilter);
    if (current != edvrCrashFilter) {
        // Someone else is on top. Undo our undo.
        SetUnhandledExceptionFilter(current);
    }
    g_prevFilter = nullptr;
}

void breadcrumbHeartbeat(uint64_t frameNo) {
    // Half a minute, by the clock rather than by a frame count -- the rate
    // varies by headset and a loading screen renders thousands of frames a
    // second, which is exactly the case a count would mis-time.
    //
    // Configurable, and 0 turns it off, because this is the one per-frame cost
    // in the render path that writes to DISK. Every other one in vscreen has a
    // key; a breadcrumb is an open/append/close rather than an append to the
    // log's already-open handle, and a file create in the game folder goes
    // through whatever filter driver is watching it. Thirty seconds is the
    // judgement that a hitch at that cadence is worth a crash landing
    // somewhere, and the key is there for anyone whose machine disagrees.
    static uint64_t intervalMs = 0;
    static bool     read = false;
    if (!read) {
        read = true;
        intervalMs = static_cast<uint64_t>(Config::get().getIntInRange(
                         "log.breadcrumb_heartbeat_seconds", 30, 0, 3600)) * 1000ull;
    }
    if (intervalMs == 0) return;
    static uint64_t lastMs = 0;
    const uint64_t now = GetTickCount64();
    // First call arms the clock without writing: the loader-phase crumbs
    // already cover this moment and a second line saying so is noise.
    if (lastMs == 0) { lastMs = now; return; }
    if (now - lastMs < intervalMs) return;
    lastMs = now;

    char line[128];
    size_t n = 0;
    appendStr(line, n, sizeof(line), "gfx: alive, frame ");
    appendNum(line, n, sizeof(line), frameNo);
    appendStr(line, n, sizeof(line), ", ");
    appendNum(line, n, sizeof(line), now / 1000);
    appendStr(line, n, sizeof(line), "s uptime");
    line[n] = 0;
    breadcrumb(line);
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
    // The same two candidates Config::init uses, in the same order: beside this
    // module, then beside the .exe.
    //
    // Only the first was tried. That is fine for d3d11.dll, which sits next to
    // the exe -- but the openvr proxy lives in Openvr\win64, and edvr.ini's own
    // header tells the user the file belongs next to EliteDangerous64.exe. So
    // advanced.real_openvr_dll, documented in that file, was read from a
    // directory the documentation never mentions: the early read came back
    // empty, the default DLL was loaded, and Config -- which DOES search both --
    // later logged the configured value as though it were in effect. Every other
    // advanced.* key from the same file worked, which made the one dead setting
    // look impossible.
    HANDLE f = INVALID_HANDLE_VALUE;
    wchar_t path[MAX_PATH];
    const std::wstring dirs[] = {moduleDir, executableDirectory()};
    for (const std::wstring& dir : dirs) {
        if (dir.size() + wcslen(iniName) + 2 > MAX_PATH) continue;
        _snwprintf_s(path, _TRUNCATE, L"%s\\%s", dir.c_str(), iniName);
        f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) break;
    }
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

    // Skip a UTF-8 BOM, as Config::parse does.
    //
    // It did not, and that is the copy drifting apart exactly as the comment
    // above warns it must not. A Notepad-saved ini starts with EF BB BF, which
    // glues itself to the first line -- so if that line is a section header it
    // stops looking like one and every key beneath it is filed under the wrong
    // name. Here that means the configured DLL is not found; through Config the
    // same bytes parse correctly, so the log then reports a value that was never
    // used.
    if (bytesRead >= 3 && static_cast<unsigned char>(p[0]) == 0xEF &&
        static_cast<unsigned char>(p[1]) == 0xBB &&
        static_cast<unsigned char>(p[2]) == 0xBF) {
        p += 3;
    }

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

        // Inline comments end the value, as they do in Config::parse.
        //
        // They did not here, one commit after that rule was added there -- the
        // two parsers drifting apart in exactly the way the comment above
        // forbids. The consequence was specific: with
        // `real_openvr_dll = openvr_api_orig.dll ; the game's own copy`, the
        // early reader kept the comment, LoadLibraryW failed, every export
        // resolved to the do-nothing stub, and the game got no VR at all.
        for (size_t i = 1; i < val.size(); ++i) {
            if ((val[i] == ';' || val[i] == '#') &&
                (val[i - 1] == ' ' || val[i - 1] == '	')) {
                val.erase(i);
                break;
            }
        }
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

// Is this module the one this code is running from?
//
// GetModuleHandleEx with FROM_ADDRESS on a local function is the only way to
// ask that does not depend on a filename: the proxy is deployed under the name
// of the DLL it replaces, so comparing paths would have to know which name it
// was wearing.
bool isSelf(HMODULE candidate) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&isSelf), &self)) {
        return false;
    }
    return self != nullptr && self == candidate;
}

HMODULE loadRealModule(const std::wstring& moduleDir, const std::string& configuredPath,
                       const wchar_t* systemFallback, const wchar_t* what) {
    if (!configuredPath.empty()) {
        std::wstring p = widen(configuredPath);
        if (!isAbsolutePath(p)) p = moduleDir + L"\\" + p;
        HMODULE m = LoadLibraryW(p.c_str());
        if (m && isSelf(m)) {
            // Pointed at us. LoadLibrary returns our own module, GetProcAddress
            // then hands back each thunk's OWN address, and the slot that thunk
            // jumps through now points at itself: a two-instruction infinite
            // loop at 100% CPU with no log, because the thunks bypass every
            // initialiser. The wrapped export recurses to a stack overflow
            // instead. Plausible while experimenting with chaining -- the value
            // to set really is the name of a DLL in this directory, and the
            // proxy's own name is right there.
            //
            // The d3d11 chain loader refuses this for itself; doing it here
            // covers both proxies through the shared helper.
            Log::get().note("real %S: the configured path %S is edvr itself; refusing, "
                            "because loading it would make every export jump to itself",
                            what, p.c_str());
            FreeLibrary(m);
            m = nullptr;
        }
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
