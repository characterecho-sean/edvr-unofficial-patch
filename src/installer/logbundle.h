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

#include <string>
#include <vector>

namespace edvr::installer {

struct LogBundle {
    bool         ok = false;
    std::wstring zipPath;
    std::vector<std::wstring> included;  // entry names, in the order written
    std::vector<std::string>  notes;     // what was found, and what was not
    std::string  error;
};

// Everything from one install, zipped into `outDir` (the Desktop, normally).
// `gameDir` must be the folder holding EliteDangerous64.exe.
LogBundle collectLogs(const std::wstring& gameDir, const std::wstring& outDir);

// The user's Desktop, or an empty string if Windows will not say where it is.
std::wstring desktopFolder();

// A zip of `files`, stored (not deflated): logs are small, every tool opens a
// stored zip, and this way the installer carries no compression library.
// `names` are the entry names, one per file.
bool writeZip(const std::wstring& zipPath, const std::vector<std::wstring>& files,
              const std::vector<std::wstring>& names, std::string* error);

}  // namespace edvr::installer
