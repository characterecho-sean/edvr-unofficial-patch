#include "logbundle.h"

#include <windows.h>
#include <shlobj.h>

#include <new>

#include <algorithm>
#include <vector>

#include "detect.h"
#include "iniedit.h"
#include "state.h"

namespace edvr::installer {
namespace {

// Two logs from one launch are seconds apart; the next launch is minutes or
// hours later. Anything written within this of the newest log is the same
// session.
const long long kSessionWindowSeconds = 180;

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

bool readWhole(const std::wstring& path, std::vector<unsigned char>* out, FILETIME* written) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart < 0 || size.QuadPart > (256ll << 20)) {
        CloseHandle(f);
        return false;
    }
    if (written) GetFileTime(f, nullptr, nullptr, written);

    out->resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < out->size()) {
        DWORD read = 0;
        const DWORD want = static_cast<DWORD>(
            std::min<size_t>(out->size() - done, 1u << 20));
        if (!ReadFile(f, out->data() + done, want, &read, nullptr) || read == 0) {
            CloseHandle(f);
            return false;
        }
        done += read;
    }
    CloseHandle(f);
    return true;
}

struct Found {
    std::wstring path;
    std::wstring name;  // inside the zip
};

// EDVR names its logs edvr_<tag>_YYYYMMDD_HHMMSS.log, so the session a file
// belongs to is written on the file itself. That is better evidence than the
// write time, which changes when a folder is copied, restored from a backup or
// pulled out of somebody else's zip -- exactly the things that happen to a
// folder on its way into a bug report.
bool stampFromName(const std::wstring& name, FILETIME* out) {
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot < 15) return false;
    const std::wstring tail = name.substr(dot - 15, 15);  // YYYYMMDD_HHMMSS
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
    if (base.empty()) return std::wstring();
    return joinPath(joinPath(joinPath(joinPath(base, L"Frontier Developments"),
                                      L"Elite Dangerous"),
                             L"Options"),
                    L"Graphics");
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

    std::vector<unsigned char> zip;
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

    for (size_t i = 0; i < files.size(); ++i) {
        std::vector<unsigned char> data;
        FILETIME written{};
        if (!readWhole(files[i], &data, &written)) {
            // The likeliest reason is a log the game is still writing to, which
            // is exactly when somebody presses Save logs. Say which file, rather
            // than handing over a bundle that is quietly missing one.
            skipped.push_back(leafOf(files[i]));
            continue;
        }

        // Stored entries, 32-bit offsets, no ZIP64. Logs are small and capped,
        // but a bundle that crossed 4 GB would produce a silently corrupt
        // archive rather than an error, and a corrupt archive attached to a bug
        // report is worse than no archive.
        if (zip.size() + data.size() > 0xF0000000ull) {
            if (error) *error = "these logs are too large to package (over 4 GB)";
            return false;
        }

        Entry entry;
        entry.name = toUtf8(names[i]);
        entry.crc = crc32Of(data.data(), data.size(), 0);
        entry.size = static_cast<unsigned long>(data.size());
        entry.offset = static_cast<unsigned long>(zip.size());
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

        put32(zip, 0x04034b50);            // local file header
        put16(zip, 20);                    // version needed
        put16(zip, flags);                 // flags
        put16(zip, 0);                     // method: stored
        put16(zip, entry.dosTime);
        put16(zip, entry.dosDate);
        put32(zip, entry.crc);
        put32(zip, entry.size);            // compressed == uncompressed
        put32(zip, entry.size);
        put16(zip, static_cast<unsigned>(entry.name.size()));
        put16(zip, 0);                     // extra length
        zip.insert(zip.end(), entry.name.begin(), entry.name.end());
        zip.insert(zip.end(), data.begin(), data.end());

        entries.push_back(entry);
    }

    if (skippedOut) *skippedOut = skipped;
    if (entries.empty()) {
        if (error) *error = "there was nothing to collect";
        return false;
    }

    const unsigned long directoryOffset = static_cast<unsigned long>(zip.size());
    for (const Entry& entry : entries) {
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
    }
    const unsigned long directorySize =
        static_cast<unsigned long>(zip.size()) - directoryOffset;

    put32(zip, 0x06054b50);                // end of central directory
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, static_cast<unsigned>(entries.size()));
    put16(zip, static_cast<unsigned>(entries.size()));
    put32(zip, directorySize);
    put32(zip, directoryOffset);
    put16(zip, 0);                         // comment length

    HANDLE f = CreateFileW(zipPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (error) *error = "could not create the zip file";
        return false;
    }
    size_t done = 0;
    while (done < zip.size()) {
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(zip.size() - done, 1u << 20));
        if (!WriteFile(f, zip.data() + done, want, &wrote, nullptr) || wrote == 0) {
            CloseHandle(f);
            if (error) *error = "could not write the zip file";
            return false;
        }
        done += wrote;
    }
    CloseHandle(f);
    return true;
}

LogBundle collectLogs(const std::wstring& gameDir, const std::wstring& outDir) try {
    LogBundle bundle;
    if (gameDir.empty()) {
        bundle.error = "No game folder chosen.";
        return bundle;
    }

    // Where the logs are is a setting, so ask the file rather than assume.
    const std::string iniText = readTextFile(joinPath(gameDir, L"edvr.ini"));
    const std::string configured = iniValue(iniText, "log.dir");
    const std::wstring logDir =
        configured.empty() ? joinPath(gameDir, L"edvr_logs") : fromUtf8(configured);

    std::vector<Found> take;

    // ---- the newest session's logs ------------------------------------
    struct LogFile {
        std::wstring name;
        FILETIME     written{};
    };
    std::vector<LogFile> logs;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(logDir, L"edvr_*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            LogFile log;
            log.name = fd.cFileName;
            // The name first, the write time only when the name does not carry
            // a stamp -- a log from a custom log.dir, or one somebody renamed.
            if (!stampFromName(log.name, &log.written)) log.written = fd.ftLastWriteTime;
            logs.push_back(log);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (logs.empty()) {
        bundle.notes.push_back("No logs in " + toUtf8(logDir) +
                               " -- has the game been started since EDVR was installed?");
    } else {
        const LogFile* newest = &logs.front();
        for (const LogFile& log : logs) {
            if (CompareFileTime(&log.written, &newest->written) > 0) newest = &log;
        }
        int sessionCount = 0;
        for (const LogFile& log : logs) {
            if (secondsBetween(log.written, newest->written) > kSessionWindowSeconds) continue;
            take.push_back({joinPath(logDir, log.name), log.name});
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
    const Extra extras[] = {
        {joinPath(gameDir, L"edvr_breadcrumbs.txt"), L"edvr_breadcrumbs.txt", nullptr},
        {joinPath(gameDir, L"edvr_FATAL.txt"), L"edvr_FATAL.txt", nullptr},
        {joinPath(gameDir, L"edvr.ini"), L"edvr.ini", "edvr.ini is not there"},
        {statePath(gameDir), L"edvr_install_state.ini", nullptr},
    };
    for (const Extra& extra : extras) {
        if (fileExists(extra.path)) {
            take.push_back({extra.path, extra.name});
        } else if (extra.missing) {
            bundle.notes.push_back(extra.missing);
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

    bundle.zipPath = joinPath(where, L"edvr-logs-" + timestampName() + L".zip");

    std::vector<std::wstring> files, names;
    for (const Found& found : take) {
        files.push_back(found.path);
        names.push_back(found.name);
        bundle.included.push_back(found.name);
    }
    std::string error;
    std::vector<std::wstring> skipped;
    if (!writeZip(bundle.zipPath, files, names, &error, &skipped)) {
        bundle.error = "Could not write " + toUtf8(bundle.zipPath) + ": " + error;
        return bundle;
    }
    for (const std::wstring& name : skipped) {
        bundle.notes.push_back(toUtf8(name) +
                               " could not be read and is not in the zip -- it is probably still "
                               "being written to.");
    }
    bundle.ok = true;
    return bundle;
} catch (const std::bad_alloc&) {
    // The whole archive is assembled in memory. Several large logs on a machine
    // already short of it should say so, not disappear.
    LogBundle failed;
    failed.error = "Ran out of memory collecting the logs. Close the game and try again.";
    return failed;
}

}  // namespace edvr::installer
