// Finding Elite Dangerous, however it was installed.
//
// The README's install table lists three paths and then says: if none of those
// match, find EliteDangerous64.exe yourself. That last sentence is the part
// people get wrong -- the Frontier launcher alone has moved its Products folder
// between %LOCALAPPDATA% and Program Files, Steam libraries live on whichever
// drive had room, and Epic writes its own manifests somewhere neither of the
// others look.
//
// So nothing here trusts a fixed path. Each store is asked where it put the
// game in its own terms (Steam's library index, Epic's manifests, the
// uninstall registry, the launcher's own folders), every answer is then
// CONFIRMED by finding EliteDangerous64.exe underneath it, and anything that
// cannot be confirmed is not offered. The user can always point at a folder
// themselves, and that path goes through exactly the same confirmation.
#pragma once

#include <string>
#include <vector>

namespace edvr::installer {

struct GameInstall {
    std::wstring dir;        // the folder holding EliteDangerous64.exe
    std::wstring openvrDir;  // the folder holding the game's openvr_api.dll; empty if not found
    std::wstring source;     // "Steam", "Epic", "Frontier launcher", "Folder you chose"
    std::wstring product;    // the Products\ folder name, e.g. elite-dangerous-odyssey-64
    bool odyssey = false;    // Odyssey rather than the Horizons build
};

// Every install this machine can be shown to have, Odyssey first. Confirmed by
// the executable, deduplicated by path.
std::vector<GameInstall> findInstalls();

// Does this folder hold EliteDangerous64.exe?
bool gameDirLooksRight(const std::wstring& dir);

// Build the record for a folder the user picked. `dir` may be the game folder
// itself or a launcher root above it; the game folder underneath is found when
// there is exactly one.
bool describeDir(const std::wstring& dir, const std::wstring& source, GameInstall* out);

// Whichever of Openvr\win64, Openvr or the game folder holds the game's
// openvr_api.dll (or the openvr_api_orig.dll a previous install left). Empty
// when none of them do -- which is a fact the planner has to report, not
// paper over.
std::wstring findOpenvrDir(const std::wstring& gameDir);

// Is Elite Dangerous running, and does that stop us?
//
// Every file this installer touches is one the game holds open while it runs
// -- but only in the folder it was launched from. A machine with two installs
// (the Frontier launcher's and Steam's) plays one while the other is patched,
// and asking by executable NAME alone refused the second for the first one's
// sake.
enum class GameRunState {
    NotRunning,   // no EliteDangerous64.exe anywhere on this machine
    OtherFolder,  // running, and every copy of it came from some other install
    ThisFolder,   // running from the folder about to be written into
};

// EliteDangerous64.exe, weighed against the folder about to be written into.
GameRunState gameRunState(const std::wstring& gameDir);

// The same question about any executable name. It takes the name rather than
// fixing it because that is the only way to test the answer: no build machine
// has Elite running, and every one of them has the test process itself.
GameRunState runStateOf(const wchar_t* exeName, const std::wstring& dir);

// Path and text helpers shared with the rest of the installer.
std::wstring joinPath(const std::wstring& dir, const std::wstring& leaf);
std::wstring leafOf(const std::wstring& path);
bool         fileExists(const std::wstring& path);
bool         dirExists(const std::wstring& path);
std::wstring canonicalPath(const std::wstring& path);
std::string  toUtf8(const std::wstring& s);
std::wstring fromUtf8(const std::string& s);

// The whole file, or an empty string. Text only: everything read this way is
// an ini, a manifest or a launcher's index.
std::string readTextFile(const std::wstring& path, size_t limit = 4u << 20);

}  // namespace edvr::installer
