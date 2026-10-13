#include "logbundle.h"

#include <windows.h>
#include <shlobj.h>

#include <new>

#include <algorithm>
#include <string>
#include <vector>
#include <wchar.h>

#include "detect.h"
#include "plan.h"
#include "../common/elite_graphics_folder.h"
#include "../common/iniedit.h"
#include "../common/install_scope.h"
#include "state.h"

namespace edvr::installer {
namespace {

// Two logs from one launch are seconds apart; the next launch is minutes or
// hours later. Anything written within this of the newest log is the same
// session.
const long long kSessionWindowSeconds = 180;

// The scope installed in this folder, as far as it can be told without a survey, in the planner's order (plan.cpp installedScope):
// a canonical descriptor first, so a record an older install left cannot outvote the files; then the record; then the descriptor's own
// text (schema 1 `profile = flat`). Everything else is VR.
std::string scopeOf(const std::wstring& gameDir) {
    const std::string descriptor = readTextFile(joinPath(gameDir, L"edvr_profile.ini"));
    const std::string canonical = canonicalDescriptorScope(descriptor);
    if (!canonical.empty()) return canonical;
    const InstallState state = readState(gameDir);
    if (state.present && !recordedScope(state).empty()) return recordedScope(state);
    const edvr::InstallScope parsed = edvr::parseInstallDescriptor(descriptor).scope;
    return parsed == edvr::InstallScope::Invalid ? std::string("vr") : std::string(edvr::scopeName(parsed));
}

// The log folder one settings file names: its log.dir, or edvr_logs beside the game when the file is absent or names none. The runtime
// resolves it the same way.
std::wstring logDirOf(const std::wstring& gameDir, const std::wstring& iniPath) {
    const std::string configured = iniValue(readTextFile(iniPath), "log.dir");
    return configured.empty() ? joinPath(gameDir, L"edvr_logs") : fromUtf8(configured);
}

// The zip prefix for a file from log folder `folder` (0-based) whose plain name the zip already holds: two folders can each hold a file
// of one name (a shader dumped by both runs, say), and the zip cannot hold two entries of one name.
std::wstring zipPrefixFor(size_t folder) { return L"log-folder-" + std::to_wstring(folder + 1) + L"/"; }

// The index of the log folder a path sits in, 0 when none of them is.
size_t folderIndexOf(const std::wstring& path, const std::vector<std::wstring>& dirs) {
    for (size_t i = 0; i < dirs.size(); ++i) {
        const size_t n = dirs[i].size();
        if (path.size() > n && path[n] == L'\\' && _wcsnicmp(path.c_str(), dirs[i].c_str(), n) == 0) return i;
    }
    return 0;
}

unsigned long crc32Of(const unsigned char* data, size_t size, unsigned long running) {
    // The table is built once, on first use: 1 KB and a few microseconds
    // against carrying 256 constants in the source.
    static unsigned long table[256];
    static bool ready = false;
    if (!ready) {
        for (unsigned long i = 0; i < 256; ++i) {
            unsigned long c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    unsigned long c = running ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put16(std::vector<unsigned char>& out, unsigned value) {
    out.push_back(static_cast<unsigned char>(value & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
}

void put32(std::vector<unsigned char>& out, unsigned long value) {
    out.push_back(static_cast<unsigned char>(value & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
}

// MS-DOS date and time, which is what a zip entry carries.
void dosStamp(const FILETIME& fileTime, unsigned* dosTime, unsigned* dosDate) {
    SYSTEMTIME utc{}, local{};
    // Unchecked, these leave `local` zeroed, and month 0 / day 0 is a date some
    // zip tools refuse outright. 1980-01-01 is the oldest a zip can express and
    // is obviously a fallback rather than a real time.
    if (!FileTimeToSystemTime(&fileTime, &utc) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        local = SYSTEMTIME{};
    }
    if (local.wYear < 1980) local.wYear = 1980;
    if (local.wMonth < 1 || local.wMonth > 12) local.wMonth = 1;
    if (local.wDay < 1 || local.wDay > 31) local.wDay = 1;
    *dosTime = (local.wHour << 11) | (local.wMinute << 5) | (local.wSecond / 2);
    *dosDate = ((local.wYear - 1980) << 9) | (local.wMonth << 5) | local.wDay;
}

struct Found {
    std::wstring path;
    std::wstring name;  // inside the zip
};

// EDVR names its logs edvr_<tag>_YYYYMMDD_HHMMSS.log, with _mmm_<pid> after
// the stamp when a second process opened the same tag in the same second
// (the native OpenXR trace always carries it). The stamp is the local
// wall-clock value throughout, though the trace's body says UTC. The session
// a file belongs to is written on the file itself. That is better evidence
// than the write time, which changes when a folder is copied, restored from
// a backup or pulled out of somebody else's zip -- exactly the things that
// happen to a folder on its way into a bug report.
bool stampFromName(const std::wstring& name, FILETIME* out) {
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot < 15) return false;
    size_t stampEnd = dot;
    // The suffix: three digits, then one or more, between the last two '_'.
    // A plain name cannot pass for it -- its last two '_' are nine apart.
    const size_t pidSep = name.rfind(L'_', dot - 1);
    const size_t msSep = pidSep == std::wstring::npos || pidSep == 0 ? std::wstring::npos
                                                                     : name.rfind(L'_', pidSep - 1);
    if (msSep != std::wstring::npos && msSep >= 15 && pidSep == msSep + 4 && pidSep + 1 < dot) {
        bool digits = true;
        for (size_t i = msSep + 1; i < dot && digits; ++i)
            if (i != pidSep && (name[i] < L'0' || name[i] > L'9')) digits = false;
        if (digits) stampEnd = msSep;
    }
    const std::wstring tail = name.substr(stampEnd - 15, 15);  // YYYYMMDD_HHMMSS
    if (tail[8] != L'_') return false;
    for (size_t i = 0; i < tail.size(); ++i) {
        if (i == 8) continue;
        if (tail[i] < L'0' || tail[i] > L'9') return false;
    }
    auto number = [&](size_t at, size_t count) {
        int value = 0;
        for (size_t i = 0; i < count; ++i) value = value * 10 + (tail[at + i] - L'0');
        return value;
    };
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(number(0, 4));
    st.wMonth = static_cast<WORD>(number(4, 2));
    st.wDay = static_cast<WORD>(number(6, 2));
    st.wHour = static_cast<WORD>(number(9, 2));
    st.wMinute = static_cast<WORD>(number(11, 2));
    st.wSecond = static_cast<WORD>(number(13, 2));
    if (st.wMonth < 1 || st.wMonth > 12 || st.wDay < 1 || st.wDay > 31) return false;
    return SystemTimeToFileTime(&st, out) != 0;
}

long long secondsBetween(const FILETIME& a, const FILETIME& b) {
    ULARGE_INTEGER x{}, y{};
    x.LowPart = a.dwLowDateTime;
    x.HighPart = a.dwHighDateTime;
    y.LowPart = b.dwLowDateTime;
    y.HighPart = b.dwHighDateTime;
    const long long diff = static_cast<long long>(x.QuadPart) - static_cast<long long>(y.QuadPart);
    return (diff < 0 ? -diff : diff) / 10000000ll;
}

// Elite keeps the user's graphics choices under the Windows profile, not the
// game folder: LocalAppData/Frontier Developments/Elite Dangerous/
// Options/Graphics. Several files live there and their NAMES shift with
// the game version (Custom.4.0.fxcfg against the older Custom.fxcfg,
// and so on), so the collector sweeps the folder rather than carrying
// a list that would quietly go stale.
std::wstring graphicsOptionsFolder() {
    PWSTR path = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) && path) {
        base = path;
        CoTaskMemFree(path);
    } else {
        wchar_t env[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH)) base = env;
    }
    // The composition is shared with the flat F8 panel's settings warning
    // (src/common/elite_graphics_folder.h): one spelling of where these files live.
    return edvr::eliteGraphicsFolderUnder(base);
}

}  // namespace

std::wstring desktopFolder() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path)) && path) {
        std::wstring out = path;
        CoTaskMemFree(path);
        return out;
    }
    // A roamed or redirected Desktop that the shell will not name is rare, but
    // "could not save the logs" is a poor answer when the folder is almost
    // certainly right there.
    wchar_t profile[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
        const std::wstring guess = joinPath(profile, L"Desktop");
        if (dirExists(guess)) return guess;
    }
    return std::wstring();
}

bool writeZip(const std::wstring& zipPath, const std::vector<std::wstring>& files,
              const std::vector<std::wstring>& names, std::string* error,
              std::vector<std::wstring>* skippedOut) {
    if (files.size() != names.size()) {
        if (error) *error = "internal: zip file list and name list differ in length";
        return false;
    }
    std::vector<std::wstring> skipped;

    constexpr unsigned long long kZipLimit = 0xF0000000ull;
    constexpr DWORD kChunk = 1u << 20;
    struct Entry {
        std::string name;
        unsigned long crc = 0;
        unsigned long size = 0;
        unsigned long offset = 0;
        unsigned dosTime = 0;
        unsigned dosDate = 0;
        unsigned flags = 0;
    };
    std::vector<Entry> entries;
    std::vector<unsigned char> chunk(kChunk);
    unsigned long long position = 0, directoryBytes = 0;
    HANDLE out = CreateFileW(zipPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        if (error) *error = "could not create the zip file";
        return false;
    }
    struct PartialZip {
        HANDLE file;
        const std::wstring& path;
        bool complete = false;
        ~PartialZip() { CloseHandle(file); if(!complete) DeleteFileW(path.c_str()); }
    } partial{out,zipPath};
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    auto write = [&](const unsigned char* bytes, size_t size) {
        size_t done = 0;
        while (done < size) {
            DWORD wrote = 0;
            const DWORD want = static_cast<DWORD>(std::min<size_t>(size - done, kChunk));
            if (!WriteFile(out, bytes + done, want, &wrote, nullptr) || !wrote) return false;
            done += wrote;
        }
        position += size;
        return true;
    };
    auto rollback = [&](unsigned long long offset) {
        LARGE_INTEGER where{}; where.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(out, where, nullptr, FILE_BEGIN) || !SetEndOfFile(out)) return false;
        position = offset;
        return true;
    };

    for (size_t i = 0; i < files.size(); ++i) {
        HANDLE source = CreateFileW(files[i].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const bool packetFile = names[i].compare(0, 18, L"flat_draw_packets/") == 0 ||
                                names[i].find(L"flat_pixels/resolve_inputs_") == 0;
        if (source == INVALID_HANDLE_VALUE) {
            if(packetFile) return fail("atomic capture became unreadable; no partial bundle was retained");
            skipped.push_back(names[i]); continue;
        }
        LARGE_INTEGER length{};
        FILETIME written{};
        if (!GetFileSizeEx(source, &length) || length.QuadPart < 0 ||
            !GetFileTime(source, nullptr, nullptr, &written)) {
            CloseHandle(source);
            if(packetFile) return fail("atomic capture metadata became unreadable; no partial bundle was retained");
            skipped.push_back(names[i]);
            continue;
        }
        Entry entry;
        entry.name = toUtf8(names[i]);
        if (entry.name.size() > 65535 || entries.size() >= 65535 ||
            static_cast<unsigned long long>(length.QuadPart) > kZipLimit ||
            position + 30ull + entry.name.size() + static_cast<unsigned long long>(length.QuadPart) +
                directoryBytes + 46ull + entry.name.size() + 22ull > kZipLimit) {
            CloseHandle(source);
            return fail("these logs are too large to package (ZIP32 limit)");
        }
        entry.size = static_cast<unsigned long>(length.QuadPart);
        entry.offset = static_cast<unsigned long>(position);
        dosStamp(written, &entry.dosTime, &entry.dosDate);

        // Bit 11 says the name is UTF-8. Without it a name outside ASCII is
        // decoded as CP437 by most tools; with it, correctly. Set only when it
        // is needed, so ordinary bundles stay byte-identical to before.
        bool asciiName = true;
        for (char c : entry.name) {
            if (static_cast<unsigned char>(c) > 0x7F) asciiName = false;
        }
        const unsigned flags = asciiName ? 0u : 0x0800u;
        entry.flags = flags;

        // A first bounded pass determines CRC before the local header. The
        // second pass writes the stored payload without a data descriptor.
        unsigned long long scanned = 0;
        bool readOk = true;
        while (scanned < entry.size) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<unsigned long long>(kChunk, entry.size-scanned));
            if (!ReadFile(source, chunk.data(), want, &got, nullptr) || got != want) { readOk=false; break; }
            entry.crc = crc32Of(chunk.data(), got, entry.crc);
            scanned += got;
        }
        LARGE_INTEGER zero{};
        if (!readOk || !SetFilePointerEx(source, zero, nullptr, FILE_BEGIN)) {
            CloseHandle(source);
            if(packetFile) return fail("atomic capture could not be scanned; no partial bundle was retained");
            skipped.push_back(names[i]); continue;
        }
        std::vector<unsigned char> header;
        put32(header, 0x04034b50); put16(header, 20); put16(header, flags);
        put16(header, 0); put16(header, entry.dosTime); put16(header, entry.dosDate);
        put32(header, entry.crc); put32(header, entry.size); put32(header, entry.size);
        put16(header, static_cast<unsigned>(entry.name.size())); put16(header, 0);
        header.insert(header.end(), entry.name.begin(), entry.name.end());
        if (!write(header.data(), header.size())) { CloseHandle(source); return fail("could not write the zip file"); }
        unsigned long long copied = 0;
        unsigned long copiedCrc = 0;
        while (copied < entry.size) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<unsigned long long>(kChunk, entry.size-copied));
            if (!ReadFile(source, chunk.data(), want, &got, nullptr) || got != want) { readOk=false; break; }
            if (!write(chunk.data(), got)) { CloseHandle(source); return fail("could not write the zip file"); }
            copiedCrc = crc32Of(chunk.data(), got, copiedCrc);
            copied += got;
        }
        LARGE_INTEGER finalLength{}; FILETIME finalWritten{};
        const bool unchanged = GetFileSizeEx(source,&finalLength) &&
            GetFileTime(source,nullptr,nullptr,&finalWritten) && finalLength.QuadPart==length.QuadPart &&
            CompareFileTime(&written,&finalWritten)==0;
        CloseHandle(source);
        if (!readOk || copiedCrc != entry.crc || (packetFile && !unchanged)) {
            if (!rollback(entry.offset)) return fail("could not roll back a changed capture file");
            if(packetFile) return fail("an atomic capture source changed during ZIP scan/copy; no partial bundle was retained");
            // Live logs may append while collected. A stable copied prefix
            // is valid; a prefix that changed is omitted with an explicit note.
            skipped.push_back(names[i]);continue;
        }
        directoryBytes += 46ull + entry.name.size();
        entries.push_back(entry);
    }

    if (skippedOut) *skippedOut = skipped;
    if (entries.empty()) {
        if (error) *error = "there was nothing to collect";
        return fail("there was nothing to collect");
    }

    const unsigned long directoryOffset = static_cast<unsigned long>(position);
    for (const Entry& entry : entries) {
        std::vector<unsigned char> zip;
        put32(zip, 0x02014b50);            // central directory header
        put16(zip, 20);                    // version made by
        put16(zip, 20);                    // version needed
        put16(zip, entry.flags);
        put16(zip, 0);                     // method: stored
        put16(zip, entry.dosTime);
        put16(zip, entry.dosDate);
        put32(zip, entry.crc);
        put32(zip, entry.size);
        put32(zip, entry.size);
        put16(zip, static_cast<unsigned>(entry.name.size()));
        put16(zip, 0);                     // extra
        put16(zip, 0);                     // comment
        put16(zip, 0);                     // disk number
        put16(zip, 0);                     // internal attributes
        put32(zip, 0);                     // external attributes
        put32(zip, entry.offset);
        zip.insert(zip.end(), entry.name.begin(), entry.name.end());
        if (!write(zip.data(),zip.size())) return fail("could not write the zip file");
    }
    const unsigned long directorySize = static_cast<unsigned long>(position - directoryOffset);

    std::vector<unsigned char> zip;
    put32(zip, 0x06054b50);                // end of central directory
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, static_cast<unsigned>(entries.size()));
    put16(zip, static_cast<unsigned>(entries.size()));
    put32(zip, directorySize);
    put32(zip, directoryOffset);
    put16(zip, 0);                         // comment length

    if (!write(zip.data(),zip.size())) return fail("could not write the zip file");
    if (!FlushFileBuffers(out)) return fail("could not finish the zip file");
    partial.complete=true;
    return true;
}

std::vector<CrashDump> findCrashDumps(const std::wstring& localAppData, const std::wstring& tempDir,
                                      const FILETIME& around, long long windowSeconds,
                                      unsigned long long maxBytes,
                                      std::vector<std::string>* skippedOut) {
    CrashDump ownBest, werBest;
    bool haveOwn = false, haveWer = false;

    // The newest .dmp in `dir` that was written inside the window, into `best`.
    auto scan = [&](const std::wstring& dir, const wchar_t* pattern, bool own, CrashDump& best, bool& have) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(joinPath(dir, pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (secondsBetween(fd.ftLastWriteTime, around) > windowSeconds) continue;
            if (have && CompareFileTime(&fd.ftLastWriteTime, &best.written) <= 0) continue;
            best.path = joinPath(dir, fd.cFileName);
            best.zipName = fd.cFileName;
            best.size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            best.written = fd.ftLastWriteTime;
            best.eliteOwn = own;
            have = true;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };

    // Elite's reporter keeps one folder per crash id under CrashDumps\. The
    // temp folder is asked for twice -- the process's, and LocalAppData\Temp --
    // since an elevated or redirected TEMP is not where the game ran from.
    std::vector<std::wstring> roots;
    if (!tempDir.empty()) roots.push_back(edvr::eliteCrashDumpsFolderUnder(tempDir));
    if (!localAppData.empty()) {
        const std::wstring second = edvr::eliteCrashDumpsFolderUnder(joinPath(localAppData, L"Temp"));
        if (roots.empty() || _wcsicmp(roots.front().c_str(), second.c_str()) != 0) roots.push_back(second);
    }
    for (const std::wstring& root : roots) {
        scan(root, L"*.dmp", true, ownBest, haveOwn);
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(joinPath(root, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            scan(joinPath(root, fd.cFileName), L"*.dmp", true, ownBest, haveOwn);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // Windows Error Reporting's LocalDumps, where the machine has it turned on.
    if (!localAppData.empty())
        scan(joinPath(localAppData, L"CrashDumps"), L"EliteDangerous64.exe.*.dmp", false, werBest, haveWer);

    std::vector<CrashDump> found;
    for (const CrashDump* dump : {haveOwn ? &ownBest : nullptr, haveWer ? &werBest : nullptr}) {
        if (!dump) continue;
        if (dump->size > maxBytes) {
            if (skippedOut) {
                char line[192];
                sprintf_s(line, "Crash dump left out (%.0f MB is over the %.0f MB limit): ",
                          static_cast<double>(dump->size) / 1048576.0, static_cast<double>(maxBytes) / 1048576.0);
                skippedOut->push_back(line + toUtf8(dump->path));
            }
            continue;
        }
        found.push_back(*dump);
    }
    return found;
}

namespace {

// Deflates through Windows' own tar.exe (bsdtar: `-a` picks zip from the file
// name). Every entry is named by its file's own name, so the caller's names must
// be those. False, with nothing left at zipPath, for any reason it did not work.
bool deflateWithTar(const std::wstring& zipPath, const std::vector<std::wstring>& files,
                    const std::vector<std::wstring>& names) {
    if (files.empty() || files.size() != names.size()) return false;
    wchar_t system[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    const std::wstring exe = joinPath(system, L"tar.exe");
    if (!fileExists(exe)) return false;

    std::wstring command = L"\"" + exe + L"\" -a -c -f \"" + zipPath + L"\"";
    for (size_t i = 0; i < files.size(); ++i) {
        const std::wstring leaf = leafOf(files[i]);
        if (leaf.empty() || leaf != names[i] || files[i].size() <= leaf.size() + 1) return false;
        command += L" -C \"" + files[i].substr(0, files[i].size() - leaf.size() - 1) + L"\" \"" + leaf + L"\"";
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(0);
    if (!CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    DWORD exitCode = 1;
    if (WaitForSingleObject(process.hProcess, 5 * 60 * 1000) != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    } else {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hProcess);
    if (exitCode == 0 && fileExists(zipPath)) return true;
    DeleteFileW(zipPath.c_str());
    return false;
}

}  // namespace

bool writeDumpZip(const std::wstring& zipPath, const std::vector<std::wstring>& files,
                  const std::vector<std::wstring>& names, std::string* error, bool* deflatedOut) {
    if (deflatedOut) *deflatedOut = false;
    if (deflateWithTar(zipPath, files, names)) {
        if (deflatedOut) *deflatedOut = true;
        return true;
    }
    return writeZip(zipPath, files, names, error);
}

LogBundle collectLogs(const std::wstring& gameDir, const std::wstring& outDir,
                      const DumpSearch* dumpSearch) try {
    LogBundle bundle;
    if (gameDir.empty()) {
        bundle.error = "No game folder chosen.";
        return bundle;
    }

    // Where the logs are is a setting, and each settings file names its own: edvr.ini
    // and edvr-flat.ini may point at different folders, so both are read and each
    // distinct folder is swept (the same folder twice is swept once).
    const std::wstring sharedIniPath = joinPath(gameDir, L"edvr.ini");
    const std::wstring flatIniPath = joinPath(gameDir, L"edvr-flat.ini");
    std::vector<std::wstring> logDirs;
    for (const std::wstring& dir : {logDirOf(gameDir, sharedIniPath), logDirOf(gameDir, flatIniPath)}) {
        const bool seen = std::any_of(logDirs.begin(), logDirs.end(),
                                      [&](const std::wstring& d) { return _wcsicmp(d.c_str(), dir.c_str()) == 0; });
        if (!seen) logDirs.push_back(dir);
    }

    // The scope is the one thing the summary of a bundle states: which settings file is this install's own.
    const std::string scope = scopeOf(gameDir);
    bundle.notes.push_back("Install scope: " + scope);

    std::vector<Found> take;

    // ---- the newest session's logs ------------------------------------
    struct LogFile {
        std::wstring name;
        std::wstring path;
        FILETIME     written{};
        FILETIME     actualWritten{};
    };
    FILETIME newestLog{};
    FILETIME sessionStart{}, sessionEnd{};
    bool haveNewestLog = false;
    std::vector<LogFile> logs;
    for (size_t folder = 0; folder < logDirs.size(); ++folder) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(joinPath(logDirs[folder], L"edvr_*.log").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            LogFile log;
            log.name = fd.cFileName;
            log.path = joinPath(logDirs[folder], log.name);
            log.actualWritten = fd.ftLastWriteTime;
            // The name first, the write time only when the name does not carry
            // a stamp -- a log from a custom log.dir, or one somebody renamed.
            if (!stampFromName(log.name, &log.written)) log.written = fd.ftLastWriteTime;
            logs.push_back(log);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (logs.empty()) {
        std::string where;
        for (const std::wstring& dir : logDirs) where += (where.empty() ? "" : " or ") + toUtf8(dir);
        bundle.notes.push_back("No logs in " + where + " -- has the game been started since EDVR was installed?");
    } else {
        const LogFile* newest = &logs.front();
        for (const LogFile& log : logs) {
            if (CompareFileTime(&log.written, &newest->written) > 0) newest = &log;
        }
        newestLog = newest->written;
        sessionStart = newestLog;
        sessionEnd = newestLog;
        haveNewestLog = true;
        int sessionCount = 0;
        for (const LogFile& log : logs) {
            if (secondsBetween(log.written, newest->written) > kSessionWindowSeconds) continue;
            if(CompareFileTime(&log.written,&sessionStart)<0) sessionStart=log.written;
            if(CompareFileTime(&log.actualWritten,&sessionEnd)>0) sessionEnd=log.actualWritten;
            take.push_back({log.path, log.name});
            ++sessionCount;
        }
        char line[160];
        sprintf_s(line, "%d log%s from the most recent session (of %d in the folder)",
                  sessionCount, sessionCount == 1 ? "" : "s", static_cast<int>(logs.size()));
        bundle.notes.push_back(line);
    }

    // ---- everything else worth having ---------------------------------
    struct Extra {
        std::wstring path;
        std::wstring name;
        const char*  missing;  // note when it is not there, or nullptr to stay quiet
    };
    // The native OpenXR runtime's startup file, beside its openvr_api.dll: the
    // folder the installer itself resolves (detect.cpp). It carries keys that
    // change what the runtime does -- frame_end_overlap, pose_cache, the runtime
    // manifest -- and none of them are in edvr.ini, so a log can only say what
    // the runtime did with the file, never what the file said. A flat install has
    // no such runtime, so it contributes nothing. A VR install without the file
    // is a valid one (the runtime then starts from its packaged defaults and its
    // log says source=packaged-default), so a missing file is not noted.
    std::wstring nativeIniPath;
    if (scope != "flat") {  // the runtime is installed for both and vr scopes
        const std::wstring openvrDir = findOpenvrDir(gameDir);
        nativeIniPath = joinPath(openvrDir.empty() ? joinPath(gameDir, L"Openvr\\win64") : openvrDir,
                                 L"edvr_openxr.ini");
    }
    const Extra extras[] = {
        {joinPath(gameDir, L"edvr_breadcrumbs.txt"), L"edvr_breadcrumbs.txt", nullptr},
        {joinPath(gameDir, L"edvr_FATAL.txt"), L"edvr_FATAL.txt", nullptr},
        // Each settings file is the one to miss when the scope installs it. A flat
        // scope's runtime falls back to edvr.ini while it has no edvr-flat.ini, so
        // there a missing edvr-flat.ini is only missed without an edvr.ini either.
        {sharedIniPath, L"edvr.ini", scopeHasVrIni(scope) ? "edvr.ini is not there" : nullptr},
        {flatIniPath, L"edvr-flat.ini",
         scopeHasFlatIni(scope) && (scope != "flat" || !fileExists(sharedIniPath)) ? "edvr-flat.ini is not there"
                                                                                    : nullptr},
        {statePath(gameDir), L"edvr_install_state.ini", nullptr},
        {nativeIniPath, L"edvr_openxr.ini", nullptr},
    };
    for (const Extra& extra : extras) {
        if (fileExists(extra.path)) {
            take.push_back({extra.path, extra.name});
        } else if (extra.missing) {
            bundle.notes.push_back(extra.missing);
        }
    }

    // ---- the F10 capture evidence --------------------------------------
    //
    // dump_draws (F10) and the flat capture probes write into subfolders of
    // the log dir: the frame-ring dump (traces\), the stage bytecode
    // (shaders\), and the pixel captures (flat_pixels\ and flat_draw_pixels\,
    // each holding one subfolder per capture). A support zip without them
    // already cost a round-trip: the six stage files and the frame trace had
    // to be chased by hand. Same session window as the logs. Raw pixel dumps
    // run to tens of MB each and the folders accumulate without bound, so
    // per-file and aggregate caps keep the bundle sendable -- and whatever a
    // cap leaves out is NAMED, never silently absent. Rides along with a
    // report like the settings do; captures without logs are not a report.
    if (!take.empty() && haveNewestLog) {
        const unsigned long long kCapFileBytes = 64ull << 20;   // one 4K dump fits
        const unsigned long long kCapTotalBytes = 1536ull << 20; // streamed ZIP, bounded memory
        const wchar_t* captureRoots[] = {L"shaders", L"traces", L"flat_pixels", L"flat_draw_pixels",
                                         L"flat_draw_packets"};
        struct CapFile {
            std::wstring path, zipName;
            std::wstring packetGroup;
            FILETIME written{};
            unsigned long long size = 0;
            bool unsafe = false;
        };
        // Every capture root of every log folder.
        struct CaptureRoot {
            std::wstring dir;
            const wchar_t* name;
        };
        std::vector<CaptureRoot> roots;
        for (const std::wstring& dir : logDirs)
            for (const wchar_t* name : captureRoots) roots.push_back({dir, name});
        std::vector<CapFile> caps;
        for (const CaptureRoot& captureRoot : roots) {
            const wchar_t* root = captureRoot.name;
            const bool packetRoot=wcscmp(root,L"flat_draw_packets")==0;
            const std::wstring rootPath = joinPath(captureRoot.dir, root);
            WIN32_FIND_DATAW cfd{};
            HANDLE ch = FindFirstFileW(joinPath(rootPath, L"*").c_str(), &cfd);
            if (ch == INVALID_HANDLE_VALUE) continue;
            do {
                if (wcscmp(cfd.cFileName, L".") == 0 || wcscmp(cfd.cFileName, L"..") == 0) continue;
                const bool resolveInputs=wcscmp(root,L"flat_pixels")==0 && wcsncmp(cfd.cFileName,L"resolve_inputs_",15)==0;
                const bool atomicCapture=packetRoot || resolveInputs;
                if(atomicCapture && (cfd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    bundle.notes.push_back("Capture omitted (reparse point): "+toUtf8(std::wstring(root)+L"/"+cfd.cFileName));
                    continue;
                }
                if (cfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    // One level of per-capture subfolders (the pixel roots);
                    // deeper nesting does not exist today, and a recursive
                    // sweep of gigabytes is not something to grow by accident.
                    const std::wstring sub = cfd.cFileName;
                    WIN32_FIND_DATAW sfd{};
                    HANDLE sh = FindFirstFileW(joinPath(joinPath(rootPath, sub), L"*").c_str(), &sfd);
                    if (sh == INVALID_HANDLE_VALUE) continue;
                    do {
                        if (sfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        if (!atomicCapture && secondsBetween(sfd.ftLastWriteTime, newestLog) > kSessionWindowSeconds)
                            continue;
                        caps.push_back({joinPath(joinPath(rootPath, sub), sfd.cFileName),
                                        std::wstring(root) + L"/" + sub + L"/" + sfd.cFileName,
                                        atomicCapture ? std::wstring(root)+L"/"+sub : L"",
                                        sfd.ftLastWriteTime,
                                        (static_cast<unsigned long long>(sfd.nFileSizeHigh) << 32) |
                                            sfd.nFileSizeLow,
                                        atomicCapture && (sfd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)!=0});
                    } while (FindNextFileW(sh, &sfd));
                    FindClose(sh);
                } else {
                    if(atomicCapture) {
                        bundle.notes.push_back("Capture omitted (capture folder required): "+toUtf8(std::wstring(root)+L"/"+cfd.cFileName));
                        continue;
                    }
                    if (secondsBetween(cfd.ftLastWriteTime, newestLog) > kSessionWindowSeconds)
                        continue;
                    caps.push_back({joinPath(rootPath, cfd.cFileName),
                                    std::wstring(root) + L"/" + cfd.cFileName, L"", cfd.ftLastWriteTime,
                                    (static_cast<unsigned long long>(cfd.nFileSizeHigh) << 32) |
                                        cfd.nFileSizeLow});
                }
            } while (FindNextFileW(ch, &cfd));
            FindClose(ch);
        }
        // Newest first: when the aggregate budget cuts, it cuts the oldest
        // evidence of the session, never the capture just taken.
        std::sort(caps.begin(), caps.end(), [](const CapFile& a, const CapFile& b) {
            return CompareFileTime(&a.written, &b.written) > 0;
        });
        unsigned long long spent = 0, leftBytes = 0;
        int taken = 0, leftOut = 0;
        std::vector<std::wstring> decidedGroups;
        for (const CapFile& cap : caps) {
            if(!cap.packetGroup.empty()) {
                if(std::find(decidedGroups.begin(),decidedGroups.end(),cap.packetGroup)!=decidedGroups.end())
                    continue;
                decidedGroups.push_back(cap.packetGroup);
                unsigned long long groupBytes=0;
                bool oversized=false, manifest=false, unsafe=false;
                FILETIME groupWritten=cap.written;
                for(const auto& part:caps) if(part.packetGroup==cap.packetGroup) {
                    groupBytes+=part.size;
                    oversized|=part.size>kCapFileBytes;
                    unsafe|=part.unsafe;
                    const bool isManifest=part.zipName.size()>=14 &&
                              part.zipName.substr(part.zipName.size()-14)==L"/manifest.json";
                    if(isManifest) {manifest=true;groupWritten=part.written;}
                }
                // Packet files are atomic evidence. The filename stamp of a
                // selected log is its session START; its actual mtime bounds
                // the session END. Never filter packet members individually.
                const bool session=CompareFileTime(&groupWritten,&sessionStart)>=0 &&
                    (CompareFileTime(&groupWritten,&sessionEnd)<=0 ||
                     secondsBetween(groupWritten,sessionEnd)<=kSessionWindowSeconds);
                const char* why=unsafe?"reparse point":!session?"outside newest log session":!manifest?"manifest missing":oversized?"file over 64 MiB":
                                groupBytes>kCapTotalBytes-spent?"bundle budget":"";
                for(const auto& part:caps) if(part.packetGroup==cap.packetGroup) {
                    if(*why) {
                        ++leftOut;leftBytes+=part.size;
                        bundle.notes.push_back("Capture omitted ("+std::string(why)+"): "+toUtf8(part.zipName));
                    } else {
                        take.push_back({part.path,part.zipName});
                        spent+=part.size;++taken;
                    }
                }
                continue;
            }
            if (cap.size > kCapFileBytes || cap.size > kCapTotalBytes-spent) {
                ++leftOut; leftBytes += cap.size;
                bundle.notes.push_back("Capture omitted ("+std::string(cap.size>kCapFileBytes?
                    "file over 64 MiB":"bundle budget")+"): "+toUtf8(cap.zipName));
                continue;
            }
            take.push_back({cap.path, cap.zipName});
            spent += cap.size;
            ++taken;
        }
        char line[192];
        if (taken > 0) {
            sprintf_s(line, "%d capture file%s from the most recent session (traces, shaders, pixels, draw packets)",
                      taken, taken == 1 ? "" : "s");
            bundle.notes.push_back(line);
        }
        if (leftOut > 0) {
            sprintf_s(line,
                      "%d capture file%s (%.1f GB) left out of the zip for size; still in the log folder",
                      leftOut, leftOut == 1 ? "" : "s",
                      static_cast<double>(leftBytes) / 1073741824.0);
            bundle.notes.push_back(line);
        }
    }

    // ---- the game's own graphics settings ------------------------------
    //
    // Half the reports in the black-planet hunt hinge on "why this machine
    // and not that one", and the per-rig variables live in two places: the
    // profile's Options/Graphics folder (quality, display, the override file
    // players hand-edit), swept whole into game_graphics/, and the master
    // GraphicsConfiguration.xml beside the exe -- which guides tell people
    // to edit directly, so the shipped copy on THIS machine is evidence too.
    // Zip layout keeps provenance readable: root entries come from the game
    // folder, game_graphics/ entries from the profile folder.
    //
    // Gated on something EDVR-side having been found first: the settings
    // ride along WITH a report, they are not a report. Without the gate a
    // press against a folder EDVR was never in would produce a zip of
    // nothing but the machine's game settings -- and "found nothing to
    // collect" is the honest answer there.
    if (!take.empty()) {
        const std::wstring master = joinPath(gameDir, L"GraphicsConfiguration.xml");
        if (fileExists(master)) {
            take.push_back({master, L"GraphicsConfiguration.xml"});
        } else {
            bundle.notes.push_back(
                "No GraphicsConfiguration.xml beside the game's exe.");
        }

        const std::wstring optDir = graphicsOptionsFolder();
        int swept = 0;
        bool sawFolder = false;
        if (!optDir.empty() && dirExists(optDir)) {
            sawFolder = true;
            std::vector<std::wstring> leaves;
            WIN32_FIND_DATAW gfd{};
            HANDLE gh = FindFirstFileW(joinPath(optDir, L"*").c_str(), &gfd);
            if (gh != INVALID_HANDLE_VALUE) {
                do {
                    if (gfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    // Everything in this folder is KB-sized XML or text. A
                    // megabyte here is not a settings file, and the archive
                    // is assembled in memory -- name it instead of taking it.
                    const unsigned long long size =
                        (static_cast<unsigned long long>(gfd.nFileSizeHigh) << 32) |
                        gfd.nFileSizeLow;
                    if (size > (1ull << 20)) {
                        bundle.notes.push_back(
                            toUtf8(gfd.cFileName) +
                            " in the graphics settings folder is over a "
                            "megabyte and was left out.");
                        continue;
                    }
                    leaves.push_back(gfd.cFileName);
                } while (FindNextFileW(gh, &gfd));
                FindClose(gh);
            }
            // Name order, so two bundles from two machines diff cleanly.
            std::sort(leaves.begin(), leaves.end());
            for (const std::wstring& leaf : leaves) {
                take.push_back({joinPath(optDir, leaf), L"game_graphics/" + leaf});
                ++swept;
            }
        }
        if (swept > 0) {
            char line[128];
            sprintf_s(line, "%d graphics settings file%s from the game's profile folder",
                      swept, swept == 1 ? "" : "s");
            bundle.notes.push_back(line);
        } else if (sawFolder) {
            bundle.notes.push_back(
                "The game's graphics settings folder is there but empty.");
        } else {
            bundle.notes.push_back(
                "No Elite graphics settings folder under this Windows "
                "user's LocalAppData -- is the game run as a different "
                "user?");
        }
    }

    if (take.empty()) {
        bundle.error = "Found nothing to collect: no logs, no breadcrumbs, no settings file.";
        return bundle;
    }

    std::wstring where = outDir;
    if (where.empty()) where = desktopFolder();
    if (where.empty() || !dirExists(where)) where = gameDir;

    const std::wstring stamp = timestampName();
    bundle.zipPath = joinPath(where, L"edvr-logs-" + stamp + L".zip");

    // Names are the files' own, except where the zip already holds one of that name: the later copy then carries its folder's prefix.
    std::vector<std::wstring> files, names;
    for (const Found& found : take) {
        std::wstring name = found.name;
        if (std::find(names.begin(), names.end(), name) != names.end())
            name = zipPrefixFor(folderIndexOf(found.path, logDirs)) + found.name;
        files.push_back(found.path);
        names.push_back(name);
    }
    std::string error;
    std::vector<std::wstring> skipped;
    if (!writeZip(bundle.zipPath, files, names, &error, &skipped)) {
        bundle.error = "Could not write " + toUtf8(bundle.zipPath) + ": " + error;
        return bundle;
    }
    for(const auto& name:names)
        if(std::find(skipped.begin(),skipped.end(),name)==skipped.end()) bundle.included.push_back(name);
    for (const std::wstring& name : skipped) {
        bundle.notes.push_back(toUtf8(name) +
                               " could not be read consistently (unreadable or changed during collection) and is not in the zip -- it is probably still "
                               "being written to.");
    }

    // ---- the game's crash dump -----------------------------------------
    //
    // When the session ended in a crash, Elite's own reporter (or Windows
    // Error Reporting, where LocalDumps is on) has written a minidump of the
    // game seconds after EDVR's last line. It holds what the logs cannot: the
    // memory at the fault, the threads, the modules that were unloaded. It goes
    // in a zip of its own, not the log zip: it is tens of MB and holds a copy
    // of the game's memory, so the log zip stays the one that is safe to
    // attach to a public issue. The end of the session is the later of the
    // last log write and the breadcrumbs' last write: the gfx log can hit its
    // size cap long before a crash, and the breadcrumb heartbeat runs to it.
    if (haveNewestLog) {
        FILETIME end = sessionEnd;
        WIN32_FILE_ATTRIBUTE_DATA crumbs{};
        if (GetFileAttributesExW(joinPath(gameDir, L"edvr_breadcrumbs.txt").c_str(),
                                 GetFileExInfoStandard, &crumbs) &&
            CompareFileTime(&crumbs.ftLastWriteTime, &end) > 0) {
            end = crumbs.ftLastWriteTime;
        }
        std::wstring localAppData, tempDir;
        if (dumpSearch) {
            localAppData = dumpSearch->localAppData;
            tempDir = dumpSearch->tempDir;
        } else {
            PWSTR known = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known) {
                localAppData = known;
                CoTaskMemFree(known);
            }
            wchar_t tempBuffer[MAX_PATH]{};
            const DWORD tempLength = GetTempPathW(MAX_PATH, tempBuffer);
            if (tempLength > 0 && tempLength < MAX_PATH) tempDir = tempBuffer;
            while (!tempDir.empty() && (tempDir.back() == L'\\' || tempDir.back() == L'/')) tempDir.pop_back();
        }

        const unsigned long long kDumpFileBytes = 512ull << 20;
        std::vector<std::string> leftOut;
        const std::vector<CrashDump> dumps =
            findCrashDumps(localAppData, tempDir, end, kSessionWindowSeconds, kDumpFileBytes, &leftOut);
        for (const std::string& note : leftOut) bundle.notes.push_back(note);
        if (!dumps.empty()) {
            std::vector<std::wstring> dumpFiles, dumpNames;
            unsigned long long total = 0;
            for (const CrashDump& dump : dumps) {
                dumpFiles.push_back(dump.path);
                dumpNames.push_back(dump.zipName);
                total += dump.size;
            }
            const std::wstring dumpZip = joinPath(where, L"edvr-crash-dump-" + stamp + L".zip");
            std::string dumpError;
            bool deflated = false;
            if (writeDumpZip(dumpZip, dumpFiles, dumpNames, &dumpError, &deflated)) {
                bundle.dumpZipPath = dumpZip;
                bundle.dumpEntries = dumpNames;
                char line[256];
                sprintf_s(line,
                          "%d crash dump%s of the game from this session (%.0f MB%s) saved separately as %s; "
                          "it holds a copy of the game's memory, so send it privately and not on a public issue",
                          static_cast<int>(dumps.size()), dumps.size() == 1 ? "" : "s",
                          static_cast<double>(total) / 1048576.0, deflated ? ", compressed" : ", not compressed",
                          toUtf8(leafOf(dumpZip)).c_str());
                bundle.notes.push_back(line);
            } else {
                bundle.notes.push_back("A crash dump from this session was found but could not be zipped (" +
                                       dumpError + "); it is still where the game wrote it: " +
                                       toUtf8(dumps.front().path));
            }
        }
    }
    bundle.ok = true;
    return bundle;
} catch (const std::bad_alloc&) {
    // Payloads stream through one MiB; allocation failure in metadata or the
    // chunk buffer should still produce an actionable installer error.
    LogBundle failed;
    failed.error = "Ran out of memory collecting the logs. Close the game and try again.";
    return failed;
}

}  // namespace edvr::installer
