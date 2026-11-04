// One zip on the Desktop with everything a bug report needs.
//
// The README asks people to attach edvr_logs\ -- both files -- plus
// edvr_breadcrumbs.txt if the game would not start, plus edvr_FATAL.txt if
// there is one. That is four things in three places, named in a paragraph
// somebody reads while annoyed, and what arrives on an issue is usually one of
// them, usually the wrong one, usually from the wrong session.
//
// So the installer collects them. It is the program already open when
// something is wrong, it already knows which folder the game is in, and it
// already knows where the log directory was moved to if it was.
//
// "Latest session" is meant literally: EDVR names its logs
// edvr_<gfx|vr>_YYYYMMDD_HHMMSS.log and writes two of them a few seconds apart
// at each launch, so the newest log and everything written within a couple of
// minutes of it is one session. Older sessions are left out -- an attachment
// with six launches in it is not more informative, it is harder to read.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace edvr::installer {

struct LogBundle {
    bool         ok = false;
    std::wstring zipPath;
    // The game's crash dump from the same session, in a zip of its own beside
    // the logs, or empty when there was none. Separate on purpose: a dump is
    // tens of MB and holds a copy of the game's memory, so it must not ride in
    // the zip that gets attached to a public issue.
    std::wstring dumpZipPath;
    std::vector<std::wstring> dumpEntries;  // what is in it
    std::vector<std::wstring> included;  // entry names, in the order written
    std::vector<std::string>  notes;     // what was found, and what was not
    std::string  error;
};

// One crash dump of the game found on this machine.
struct CrashDump {
    std::wstring path;
    std::wstring zipName;   // the entry name, a bare file name
    unsigned long long size = 0;
    FILETIME     written{};
    bool         eliteOwn = false;  // Elite's own reporter, against Windows Error Reporting
};

// The game's crash dumps written within `windowSeconds` of `around` (a real
// UTC time: the moment the session ended), newest of each kind:
//   Elite's own reporter  <tempDir | localAppData\Temp>\Frontier Developments\
//                         EliteDangerous\CrashDumps\<id>\<stamp>_MiniDump.dmp
//   Windows Error Reporting, when LocalDumps is on
//                         <localAppData>\CrashDumps\EliteDangerous64.exe.<pid>.dmp
// A dump over `maxBytes` is named in `skippedOut` (if given) and not returned.
std::vector<CrashDump> findCrashDumps(const std::wstring& localAppData, const std::wstring& tempDir,
                                      const FILETIME& around, long long windowSeconds,
                                      unsigned long long maxBytes,
                                      std::vector<std::string>* skippedOut = nullptr);

// A zip of `files` that is deflated when Windows' own tar.exe is there to do it
// (a minidump compresses about four to one, and the installer carries no
// compression library), and stored by writeZip when it is not or it fails.
// `deflatedOut`, if given, says which happened.
bool writeDumpZip(const std::wstring& zipPath, const std::vector<std::wstring>& files,
                  const std::vector<std::wstring>& names, std::string* error,
                  bool* deflatedOut = nullptr);

// Where to look for the game's crash dumps when it is not this Windows user's
// real profile: the tests point it at a fixture, so a run never picks up a
// dump from whatever the machine crashed on last.
struct DumpSearch {
    std::wstring localAppData;
    std::wstring tempDir;
};

// Everything from one install, zipped into `outDir` (the Desktop, normally).
// `gameDir` must be the folder holding EliteDangerous64.exe.
LogBundle collectLogs(const std::wstring& gameDir, const std::wstring& outDir,
                      const DumpSearch* dumpSearch = nullptr);

// The user's Desktop, or an empty string if Windows will not say where it is.
std::wstring desktopFolder();

// The clipboard half of "Save logs": the zip goes on the clipboard as a copied file (CF_HDROP), the
// same thing Explorer's Copy puts there, so Ctrl+V in Discord, a GitHub comment or an Explorer folder
// attaches or copies the zip. A file and not the logs' text because the bundle is several files and
// Discord turns a long paste into an attachment anyway. Wine and Proton map CF_HDROP to the host's
// text/uri-list, which is how a Linux Discord or file manager sees it.
//
// The bytes of that clipboard format: a DROPFILES header, then `path` and an empty string, both
// NUL-terminated and wide. Apart from the clipboard call so a rig can check it with the shell's own
// parser (DragQueryFileW) without touching the developer's clipboard.
std::vector<unsigned char> dropFilesBlock(const std::wstring& path);

// Replaces the clipboard with `path` as a copied file. `owner` is the installer's window (Wine ties the
// X11 selection to a window; Windows takes null). On failure `error` says why, and nothing the caller
// saved to disk is affected: the clipboard is a convenience beside the file on the Desktop.
bool copyFileToClipboard(HWND owner, const std::wstring& path, std::string* error);

// True under Wine, Proton included (ntdll exports wine_get_version). Only changes what the report says
// about the clipboard: on X11 a copy can go when the process that made it exits.
bool runningUnderWine();

// A zip of `files`, stored (not deflated): logs are small, every tool opens a
// stored zip, and this way the installer carries no compression library.
// `names` are the entry names, one per file.
// `skippedOut`, if given, receives the files that could not be read -- a log the
// game still has open, most likely, which is exactly when this gets pressed.
bool writeZip(const std::wstring& zipPath, const std::vector<std::wstring>& files,
              const std::vector<std::wstring>& names, std::string* error,
              std::vector<std::wstring>* skippedOut = nullptr);

}  // namespace edvr::installer
