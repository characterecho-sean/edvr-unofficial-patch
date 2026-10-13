#include "plan.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <utility>

#include "../common/install_scope.h"

namespace edvr::installer {
namespace {

// The descriptors EDVR writes, by their exact bytes (CRLF, as every writer produces them). Schema 1 is what the installer and the
// flat edition wrote before the unified install; schema 2 is what install_edvr.py and the unified installer write. A file is EDVR's
// only as one of these bytes: a modified or unknown descriptor matches none of them, whatever it says.
const char* const kDescriptorTexts[] = {
    "[install]\r\nschema = 1\r\nprofile = vr\r\n",
    "[install]\r\nschema = 1\r\nprofile = flat\r\n",
    "[install]\r\nschema = 2\r\nscope = both\r\n",
    "[install]\r\nschema = 2\r\nscope = vr\r\n",
    "[install]\r\nschema = 2\r\nscope = flat\r\n",
};

// The scope a canonical descriptor declares, read through the same parser the DLL uses. False when the hash is none of EDVR's texts.
bool ownDescriptorScope(const std::string& sha, edvr::InstallScope* scope) {
    for (const char* text : kDescriptorTexts) {
        const std::string bytes(text);
        if (sha != sha256Bytes(bytes.data(), bytes.size())) continue;
        *scope = edvr::parseInstallDescriptor(bytes).scope;
        return true;
    }
    return false;
}

// The edition a scope installs: both and vr carry the VR runtime, so they are the VR edition; flat is the flat edition.
std::string editionOfScope(edvr::InstallScope scope) {
    return scope == edvr::InstallScope::Flat ? "flat" : "vr";
}

// Whether `sha` is one of EDVR's own descriptor texts for `edition`.
bool isOwnDescriptor(const std::string& sha, const std::string& edition) {
    edvr::InstallScope scope = edvr::InstallScope::Invalid;
    return ownDescriptorScope(sha, &scope) && editionOfScope(scope) == edition;
}

// The scopes the installer installs, by the names the descriptor and the record use.
bool knownScope(const std::string& scope) { return scope == "both" || scope == "vr" || scope == "flat"; }

// The descriptor a scope writes: the canonical schema-2 text, byte for byte (kDescriptorTexts holds the same bytes).
std::string descriptorTextFor(const std::string& scope) {
    return "[install]\r\nschema = 2\r\nscope = " + scope + "\r\n";
}

// The edition a scope belongs to, as the record's profile names it: flat for the flat scope, vr for the others.
std::string editionFor(const std::string& scope) { return scope == "flat" ? "flat" : "vr"; }

const wchar_t* kD3d11 = L"d3d11.dll";
const wchar_t* kProfile = L"edvr_profile.ini";
const wchar_t* kOpenvr = L"openvr_api.dll";
const wchar_t* kOpenvrOrig = L"openvr_api_orig.dll";
const wchar_t* kNgx = L"nvngx_dlss.dll";   // NVIDIA's DLSS runtime, beside the game's executable
const wchar_t* kXess = L"libxess.dll";
const wchar_t* kXessLicense = L"INTEL-XESS-LICENSE.txt";
const wchar_t* kXessThirdParty = L"INTEL-XESS-THIRD-PARTY.txt";

// Said when the game is running and it is not this folder's copy. Nothing is
// stopped, so it is a note rather than a problem -- but not silence either:
// being refused for a running game is what somebody expects here, and a report
// that says nothing about it looks like the check never ran.
const char* kRunningElsewhere =
    "Elite Dangerous is running, but from a different folder -- this install is not the one in "
    "use, so it can be changed.";

// Said when Windows would not list the running programs. A check that could not
// run is not a check that passed: a game that is running holds every file this
// installer touches, and a half-replaced install is worse than none.
const char* kRunStateUnknown =
    "Could not tell whether Elite Dangerous is running: Windows would not list the running "
    "programs. Nothing was changed. Close the game and try again; if this keeps happening, "
    "restart the installer.";

std::string say(const std::wstring& w) { return toUtf8(w); }

std::vector<std::wstring> subdirsOf(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::vector<std::wstring> filesLike(const std::wstring& dir, const std::wstring& pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(dir, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

const DllInfo* siblingNamed(const std::vector<DllInfo>& siblings, const std::wstring& leaf) {
    for (const DllInfo& d : siblings) {
        if (_wcsicmp(leafOf(d.path).c_str(), leaf.c_str()) == 0) return &d;
    }
    return nullptr;
}

// A filename out of edvr.ini, accepted only if it really is one.
//
// Values read from that file reach MoveFileEx as destinations, and edvr.ini is
// a text file people edit. A value carrying a path separator, a drive, a "..",
// or the name of the very file it stands in for is not a rename target; it is
// a way to move the game's own runtime somewhere else entirely and be told the
// install succeeded. Refused values fall back to the built-in name, which is
// what an install that never touched the setting uses anyway.
std::wstring safeSiblingName(const std::wstring& value, const std::wstring& mustNotEqual) {
    if (value.empty()) return std::wstring();
    if (value.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return std::wstring();
    if (value == L"." || value == L".." || value.find(L"..") != std::wstring::npos)
        return std::wstring();
    if (_wcsicmp(value.c_str(), mustNotEqual.c_str()) == 0) return std::wstring();
    // A rename target that is not a DLL is a typo, not a plan.
    if (value.size() < 5 || _wcsicmp(value.c_str() + value.size() - 4, L".dll") != 0)
        return std::wstring();
    return value;
}

// The Openvr folder as it is written into the install record: relative to the
// game folder, because that is the shape the record can hold safely.
std::wstring relativeOpenvr(const std::wstring& gameDir, const std::wstring& openvrDir) {
    if (openvrDir.empty()) return std::wstring();
    if (openvrDir.size() > gameDir.size() &&
        _wcsnicmp(openvrDir.c_str(), gameDir.c_str(), gameDir.size()) == 0) {
        size_t i = gameDir.size();
        while (i < openvrDir.size() && (openvrDir[i] == L'\\' || openvrDir[i] == L'/')) ++i;
        return openvrDir.substr(i);
    }
    return openvrDir;
}

bool isCanonicalDescriptor(const std::string& sha) {
    edvr::InstallScope scope = edvr::InstallScope::Invalid;
    return ownDescriptorScope(sha, &scope);
}

std::string trimmedSpaces(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return std::string();
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

// Whether `text` is an EDVR startup config that names the loader and graphics paths given. The grammar is the runtime's own
// (src\openxr\native_local_config.h): one [openxr] section, version=1, and loader, graphics, runtime and separate_device=1 each
// once; frame_thread_priority and frame_end_overlap are optional. LF and CRLF both parse.
bool isEdvrStartupConfig(const std::string& text, const std::wstring& loaderPath, const std::wstring& graphicsPath) {
    bool section = false;
    std::vector<std::string> keys;
    std::wstring loader, graphics;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trimmedSpaces(line);
        if (line.empty()) continue;
        if (line == "[openxr]") {
            if (section) return false;
            section = true;
            continue;
        }
        const size_t equal = line.find('=');
        if (!section || equal == std::string::npos) return false;
        const std::string key = trimmedSpaces(line.substr(0, equal));
        const std::string value = trimmedSpaces(line.substr(equal + 1));
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) return false;
        keys.push_back(key);
        if (key == "version") {
            if (value != "1") return false;
        } else if (key == "loader") {
            loader = fromUtf8(value);
        } else if (key == "graphics") {
            graphics = fromUtf8(value);
        } else if (key == "runtime") {
            if (value.empty()) return false;
        } else if (key == "separate_device") {
            if (value != "1") return false;
        } else if (key == "frame_thread_priority") {
            if (value != "high" && value != "normal") return false;
        } else if (key == "frame_end_overlap") {
            if (value != "on" && value != "off") return false;
        } else {
            return false;
        }
    }
    for (const char* required : {"version", "loader", "graphics", "runtime", "separate_device"}) {
        if (std::find(keys.begin(), keys.end(), std::string(required)) == keys.end()) return false;
    }
    return section && _wcsicmp(loader.c_str(), loaderPath.c_str()) == 0 &&
           _wcsicmp(graphics.c_str(), graphicsPath.c_str()) == 0;
}

// The Openvr folder the OpenXR files are written into: the surveyed one, else the default beside the game.
std::wstring nativeDirOf(const Survey& s) {
    return s.game.openvrDir.empty() ? joinPath(s.game.dir, L"Openvr\\win64") : s.game.openvrDir;
}

bool sameSha(const std::string& a, const std::string& b) {
    return !a.empty() && _stricmp(a.c_str(), b.c_str()) == 0;
}

// The tool's receipt lists this path as a file it placed. What the tool REPLACED is somebody else's by definition, so only the placed
// hash is a proof of EDVR's bytes.
bool toolListsPath(const Survey& s, const std::wstring& path) {
    for (const ToolReceipt& r : s.toolReceipts)
        for (const ToolReceiptFile& f : r.files)
            if (_wcsicmp(f.target.c_str(), path.c_str()) == 0) return true;
    return false;
}

bool toolPlacedHash(const Survey& s, const std::wstring& path, const std::string& sha) {
    for (const ToolReceipt& r : s.toolReceipts)
        for (const ToolReceiptFile& f : r.files)
            if (_wcsicmp(f.target.c_str(), path.c_str()) == 0 && sameSha(f.installedSha, sha)) return true;
    return false;
}

// A file is EDVR's to remove only with PROVENANCE -- the record or the tool's receipt lists it, or the proof the caller names -- AND its
// bytes confirmed: the ones the record kept, the tool placed, or this installer carries. Either part alone is not enough. Matching bytes
// alone are what another mod's copy of a standard vendor binary looks like, and a provenance alone is not proof the file is intact.
// A file that is absent or unreadable is not EDVR's by anything.
bool ownedFile(const Survey& s, const DllInfo& info, bool provenance, const std::string& recorded,
               const std::string& carried) {
    if (!provenance || info.kind == DllKind::Absent || info.kind == DllKind::Unreadable || info.sha256.empty())
        return false;
    return sameSha(info.sha256, recorded) || sameSha(info.sha256, carried) ||
           toolPlacedHash(s, info.path, info.sha256);
}

// The startup config names this folder's loader and graphics paths, the same paths the planner writes into it.
bool startupConfigIsEdvrs(const Survey& s) {
    if (s.nativeConfig.kind == DllKind::Absent || s.nativeConfig.kind == DllKind::Unreadable) return false;
    return isEdvrStartupConfig(s.nativeConfigText, joinPath(nativeDirOf(s), L"openxr_loader.dll"),
                               joinPath(s.game.dir, kD3d11));
}

// The OpenXR files beside the game's VR runtime have three proofs of provenance: the record lists them, the tool's receipt lists them,
// or EDVR's startup config for this folder names the loader they travel with.
bool openxrProvenance(const Survey& s, const DllInfo& info, const std::string& recorded) {
    return !recorded.empty() || toolListsPath(s, info.path) || startupConfigIsEdvrs(s);
}

bool loaderOwned(const Survey& s, const std::string& carriedSha) {
    return ownedFile(s, s.openxrLoader, openxrProvenance(s, s.openxrLoader, s.state.openxrLoaderSha),
                     s.state.openxrLoaderSha, carriedSha);
}

bool licenseOwned(const Survey& s, const std::string& carriedSha) {
    return ownedFile(s, s.openxrLicense, openxrProvenance(s, s.openxrLicense, s.state.openxrLicenseSha),
                     s.state.openxrLicenseSha, carriedSha);
}

// The startup config is EDVR's by its own text, which names this folder's paths; or by its record or the tool's receipt and its bytes.
bool startupConfigOwned(const Survey& s) {
    if (ownedFile(s, s.nativeConfig,
                  !s.state.nativeConfigSha.empty() || toolListsPath(s, s.nativeConfig.path),
                  s.state.nativeConfigSha, std::string()))
        return true;
    return startupConfigIsEdvrs(s);
}

// The note for a file whose bytes are EDVR's but which nothing shows EDVR placed: another mod's identical copy looks exactly like this.
const char* kMatchesNoProof = "matches EDVR's copy but nothing shows EDVR placed it; left alone";

// The game's original runtime that the tool kept when it installed over it: the native receipt's runtime entry, whose backup sits beside
// the runtime. It is adopted only when every proof holds: the receipt names this folder (readToolReceipts), the entry is this folder's
// openvr_api.dll, the backup is in the same Openvr folder, its hash is the one the receipt recorded, and probeDll reads it as a genuine
// OpenVR runtime (not EDVR's). Anything else adopts nothing and stays rejected, as before.
void adoptToolOriginal(Survey& s) {
    const std::wstring runtimePath = joinPath(s.game.openvrDir, kOpenvr);
    auto folderOf = [](const std::wstring& path) {
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    };
    for (const ToolReceipt& receipt : s.toolReceipts) {
        if (!receipt.native) continue;
        const ToolReceiptFile* runtime = nullptr;
        for (const ToolReceiptFile& file : receipt.files) {
            if (file.key == "runtime" || file.key == "native") { runtime = &file; break; }
        }
        if (!runtime || runtime->backup.empty() || runtime->beforeSha.empty()) return;
        if (_wcsicmp(runtime->target.c_str(), runtimePath.c_str()) != 0) return;
        if (_wcsicmp(folderOf(runtime->backup).c_str(), s.game.openvrDir.c_str()) != 0) return;
        if (!fileExists(runtime->backup) || !sameSha(sha256File(runtime->backup), runtime->beforeSha)) return;
        const DllInfo copy = probeDll(runtime->backup);
        if (copy.kind != DllKind::OpenVrRuntime) return;
        s.toolOriginal = copy;
        s.openvrOrigInBackups.insert(s.openvrOrigInBackups.begin(), runtime->backup);
        return;
    }
}

}  // namespace

// The descriptor on disk is what the DLL reads, so a canonical one names the scope and the record is only the fallback. A record
// left by an earlier install (a flat GUI install, say) must not outvote the files: that made a both install look flat.
std::string descriptorScopeOf(const Survey& s) {
    if (!s.descriptorPresent || s.descriptorSha.empty()) return std::string();
    edvr::InstallScope scope = edvr::InstallScope::Invalid;
    if (!ownDescriptorScope(s.descriptorSha, &scope)) return std::string();
    return edvr::scopeName(scope);
}

std::string canonicalDescriptorScope(const std::string& text) {
    edvr::InstallScope scope = edvr::InstallScope::Invalid;
    if (!ownDescriptorScope(sha256Bytes(text.data(), text.size()), &scope)) return std::string();
    return edvr::scopeName(scope);
}

std::string installedScope(const Survey& s) {
    const std::string fromDescriptor = descriptorScopeOf(s);
    if (!fromDescriptor.empty()) return fromDescriptor;
    if (s.state.present) return recordedScope(s.state);
    return std::string();
}

// The record names a different scope than the descriptor on disk: the install is older than the files here, and Update refreshes it.
bool installRecordStale(const Survey& s) {
    const std::string fromDescriptor = descriptorScopeOf(s);
    return s.state.present && !fromDescriptor.empty() && recordedScope(s.state) != fromDescriptor;
}

std::string defaultScope(const Survey& s) {
    const std::string installed = installedScope(s);
    return installed.empty() ? std::string("both") : installed;
}

std::string installedProfile(const Survey& s) {
    const std::string scope = installedScope(s);
    if (scope.empty()) return std::string();
    return editionFor(scope);
}

bool hasSettingsFor(const Survey& s, const std::string& profile) {
    if (profile == "flat") return s.flatIniPresent || s.iniPresent;
    return s.iniPresent;
}

Survey surveyTarget(const GameInstall& game) {
    Survey s;
    s.game = game;
    s.eliteKind = classifyEliteExecutable(joinPath(game.dir, L"EliteDangerous64.exe"),
                                          &s.eliteFileVersion);
    if (s.game.openvrDir.empty()) s.game.openvrDir = findOpenvrDir(game.dir);
    if (s.game.openvrDir.empty()) s.game.openvrDir = joinPath(game.dir, L"Openvr\\win64");

    // Asked about THIS folder, not about the executable's name: the other
    // install on the machine can be in a jump while this one is patched.
    const GameRunState running = gameRunState(s.game.dir);
    s.gameRunningHere = running == GameRunState::ThisFolder;
    s.gameRunningElsewhere = running == GameRunState::OtherFolder;
    s.gameRunStateUnknown = running == GameRunState::Unknown;

    s.d3d11 = probeDll(joinPath(game.dir, kD3d11));
    for (const std::wstring& name : filesLike(game.dir, L"d3d11_*.dll")) {
        s.otherD3d11.push_back(probeDll(joinPath(game.dir, name)));
    }

    // Both settings files, each as itself: which one an operation works on is the
    // planner's decision, made from the edition it is for.
    const std::wstring iniPath = settingsPathFor(game.dir, "vr");
    s.iniPresent = fileExists(iniPath);
    if (s.iniPresent) s.iniText = readTextFile(iniPath);
    const std::wstring flatIniPath = settingsPathFor(game.dir, "flat");
    s.flatIniPresent = fileExists(flatIniPath);
    if (s.flatIniPresent) s.flatIniText = readTextFile(flatIniPath);
    s.baseIniText = readTextFile(baseIniPath(game.dir));
    s.flatBaseIniText = readTextFile(flatBaseIniPath(game.dir));
    s.state = readState(game.dir);
    if (!game.dir.empty()) s.toolReceipts = readToolReceipts(game.dir);
    s.descriptorPresent = fileExists(joinPath(game.dir, kProfile));
    if (s.descriptorPresent) s.descriptorSha = sha256File(joinPath(game.dir, kProfile));
    s.ngx = probeDll(joinPath(game.dir, kNgx));
    auto notice = [&](const wchar_t* leaf) {
        DllInfo info; info.path = joinPath(game.dir, leaf);
        if (fileExists(info.path)) {
            info.sha256 = sha256File(info.path);
            info.kind = info.sha256.empty() ? DllKind::Unreadable : DllKind::Foreign;
        }
        return info;
    };
    // The pinned XeSS runtime is 75 MB, above probeDll's 64 MB PE cap.
    // Ownership here needs only a readable content hash, as for notices.
    s.xess = notice(kXess);
    s.xessLicense = notice(kXessLicense);
    s.xessThirdParty = notice(kXessThirdParty);
    s.nvidiaAdapter = nvidiaAdapterPresent(&s.nvidiaAdapterName);

    if (!s.game.openvrDir.empty()) {
        s.haveOpenvrDir = dirExists(s.game.openvrDir);
        s.openxrLoader = probeDll(joinPath(s.game.openvrDir, L"openxr_loader.dll"));
        auto surveyText = [&](const wchar_t* name) {
            DllInfo info; info.path=joinPath(s.game.openvrDir,name);
            if(fileExists(info.path)) {
                info.sha256=sha256File(info.path);
                info.kind=info.sha256.empty()?DllKind::Unreadable:DllKind::Foreign;
            }
            return info;
        };
        s.nativeConfig=surveyText(L"edvr_openxr.ini");
        if(s.nativeConfig.kind!=DllKind::Absent && s.nativeConfig.kind!=DllKind::Unreadable)
            s.nativeConfigText=readTextFile(s.nativeConfig.path);
        s.openxrLicense=surveyText(L"OPENXR-LOADER-LICENSE.txt");
        s.openvrCurrent = probeDll(joinPath(s.game.openvrDir, kOpenvr));

        // The original is always renamed to openvr_api_orig.dll: the name the
        // README documents and this installer writes. (Before 2026-09-16 a
        // hand-edited advanced.real_openvr_dll could choose a different name,
        // for the forwarding proxy's chaining; native OpenXR never chains, so
        // that key is retired and the name is no longer configurable.)
        s.openvrOrigName = kOpenvrOrig;
        s.openvrOrig = probeDll(joinPath(s.game.openvrDir, s.openvrOrigName));
    }

    // Backups this installer made, newest first. The only thing looked for is
    // a genuine OpenVR runtime: it is what makes "the game's original was
    // overwritten" a recoverable mistake instead of a trip through the
    // launcher's file verification.
    const std::wstring backups = backupRootPath(game.dir);
    if (dirExists(backups)) {
        std::vector<std::wstring> stamps = subdirsOf(backups);
        std::sort(stamps.rbegin(), stamps.rend());
        for (const std::wstring& stamp : stamps) {
            const std::wstring candidate = joinPath(joinPath(backups, stamp), kOpenvr);
            if (!fileExists(candidate)) continue;
            const DllInfo info = probeDll(candidate);
            if (info.kind == DllKind::OpenVrRuntime) s.openvrOrigInBackups.push_back(candidate);
        }
    }
    // The tool's backup of the original is adopted last and placed first: its receipt names exactly which runtime it replaced.
    if (s.haveOpenvrDir) adoptToolOriginal(s);
    return s;
}

Plan planInstall(const Survey& s, const Options& o, const PayloadInfo& p) {
    Plan plan;
    plan.backupDir =
        joinPath(backupRootPath(s.game.dir), o.backupStamp.empty() ? L"backup" : o.backupStamp);

    if (s.game.dir.empty()) {
        plan.blocked = true;
        plan.problems.push_back("No game folder chosen.");
        return plan;
    }
    if (s.gameRunningHere) {
        plan.blocked = true;
        plan.problems.push_back(
            "Elite Dangerous is running. Close it first -- Windows will not let anything replace a "
            "file the game has open, and a half-replaced install is worse than none.");
        return plan;
    }
    if (s.gameRunStateUnknown) {
        plan.blocked = true;
        plan.problems.push_back(kRunStateUnknown);
        return plan;
    }
    // The scope this run installs: the one asked for, else the one the folder already has, else both. A payload with no VR runtime
    // is refused below for a scope that needs one, rather than quietly installing flat.
    const std::string installed = installedScope(s);
    if (s.state.present && !knownScope(installed)) {
        plan.blocked = true; plan.problems.push_back("The existing install record has an unknown profile. Repair it before changing editions."); return plan;
    }
    const bool payloadHasRuntime = p.haveOpenvr && p.haveOpenxrLoader && p.haveOpenxrLicense && p.nativePairValid;
    const std::string scope = o.scope.empty() ? defaultScope(s) : o.scope;
    if (!knownScope(scope)) {
        plan.blocked = true; plan.problems.push_back("Unknown scope \"" + scope + "\": use both, vr or flat."); return plan;
    }
    const bool flat = scope == "flat";           // no VR runtime: the game keeps its own openvr_api.dll
    const bool wantsVrIni = scopeHasVrIni(scope);
    const bool wantsFlatIni = scopeHasFlatIni(scope);
    if (!flat && !payloadHasRuntime) {
        plan.blocked = true;
        plan.problems.push_back("This installer carries no VR runtime, so it cannot install scope " + scope +
                                ". Install scope flat, or use the VR installer. Nothing will be installed.");
        return plan;
    }
    if (!p.haveD3d11 || !p.nativeGraphicsValid || (wantsVrIni && !p.haveIni) || (wantsFlatIni && !p.haveFlatIni)) {
        plan.blocked = true;
        plan.problems.push_back("This installer's payload is incomplete or invalid for scope " + scope + ". Nothing will be installed.");
        return plan;
    }
    // Each scope's descriptor is the planner's own canonical text. Moving to flat from a scope with the VR runtime is the conversion
    // whose proofs follow; no confirmation is asked for it, and no settings file is deleted by it.
    const std::string descriptor = descriptorTextFor(scope);
    const std::string descriptorSha = sha256Bytes(descriptor.data(), descriptor.size());
    const std::string existingProfile = installedProfile(s);
    // Flat retires the VR runtime only when the file in Openvr\win64 is EDVR's own build. The descriptor and the record say what the last
    // run wrote; what is on disk decides whether anything needs retiring. A folder whose runtime is the game's original (a leftover
    // descriptor, a stale record) has nothing to prove, and its openvr_api.dll is left alone.
    const bool retireRuntime = flat && s.openvrCurrent.kind == DllKind::Edvr;
    // The file on disk is the one the record names, or the planner's own text for the scope, or any of EDVR's texts for the edition
    // this folder already has (schema 1 or 2, whichever an earlier install or the tool wrote). That last clause upgrades such a file in
    // place; a modified or unknown file matches none of them and is refused.
    const bool ownEdition = !existingProfile.empty() && isOwnDescriptor(s.descriptorSha, existingProfile);
    const bool recordAgrees = s.state.descriptorSha.empty() ? s.descriptorSha == descriptorSha
                                                            : s.descriptorSha == s.state.descriptorSha;
    if (s.descriptorPresent && (s.descriptorSha.empty() || (!recordAgrees && !ownEdition))) {
        plan.blocked = true; plan.problems.push_back("edvr_profile.ini differs from the recorded file; it was left untouched."); return plan;
    }
    if (retireRuntime) {
        const bool ownedVr = !s.state.openvrSha.empty() && s.openvrCurrent.sha256 == s.state.openvrSha &&
            (s.openvrOrig.kind == DllKind::OpenVrRuntime || !s.openvrOrigInBackups.empty()) &&
            (s.openvrOrig.kind != DllKind::OpenVrRuntime || s.state.openvrOrigSha.empty() ||
             s.openvrOrig.sha256 == s.state.openvrOrigSha);
        if (!s.state.present || !ownedVr) {
            plan.blocked = true; plan.problems.push_back("Cannot prove EDVR's VR runtime and the game's original; flat conversion left the VR files intact."); return plan;
        }
        // The OpenXR files beside the runtime are retired with it, so each must be EDVR's by a proof; a changed one refuses the conversion.
        if ((s.openxrLoader.kind != DllKind::Absent && !loaderOwned(s, p.openxrLoaderSha)) ||
            (s.openxrLicense.kind != DllKind::Absent && !licenseOwned(s, p.openxrLicenseSha)) ||
            (s.nativeConfig.kind != DllKind::Absent && !startupConfigOwned(s))) {
            plan.blocked = true; plan.problems.push_back("An OpenXR asset differs from EDVR's record; conversion cannot retire it safely."); return plan;
        }
    }
    switch (s.eliteKind) {
        case EliteExeKind::OdysseyQualified:
            break;
        case EliteExeKind::Legacy:
            plan.blocked = true;
            plan.problems.push_back(
                "This is Elite Dangerous (Horizons), not Elite Dangerous: Odyssey. "
                "EDVR supports Odyssey only; no EDVR build supports this game.");
            return plan;
        case EliteExeKind::Unreadable:
            plan.blocked = true;
            plan.problems.push_back(
                "EliteDangerous64.exe could not be read. Verify the game files in the launcher, then try again.");
            return plan;
        default: {
            plan.blocked = true;
            std::string msg = "This Elite Dangerous: Odyssey revision";
            if (!s.eliteFileVersion.empty())
                msg += " (version " + toUtf8(s.eliteFileVersion) + ")";
            msg += " is not one this EDVR build is qualified for. "
                   "Install an EDVR build qualified for this game revision.";
            plan.problems.push_back(msg);
            return plan;
        }
    }
    for (const DllInfo* item : {&s.d3d11,&s.openvrCurrent,&s.openxrLoader,&s.nativeConfig,&s.openxrLicense}) {
        if (flat && item != &s.d3d11 && !retireRuntime) continue;
        if(item->kind==DllKind::Unreadable) {
            plan.blocked=true;plan.problems.push_back("A native OpenXR destination cannot be read. Nothing will be installed.");return plan;
        }
    }
    const std::wstring nativeDir=s.game.openvrDir.empty()?joinPath(s.game.dir,L"Openvr\\win64"):s.game.openvrDir;
    if (s.gameRunningElsewhere) plan.notes.push_back(kRunningElsewhere);

    std::vector<Step> body;
    std::vector<std::pair<std::string, std::string>> forced;  // ini keys the installer must set
    bool wantBackupDir = false;
    bool changed = false;

    InstallState next = s.state;
    next.present = true;
    next.edvrVersion = p.version;
    next.installedUtc = o.nowUtc;
    next.profile = editionFor(scope);
    next.scope = scope;
    next.descriptorSha = descriptorSha;
    std::string components = "graphics,profile";
    if (wantsVrIni) components += ",ini";
    if (wantsFlatIni) components += ",flat-ini";
    if (!flat) components += ",openvr,openxr-loader,openxr-license,openxr-config";
    components += ",ngx-optional,xess-optional";
    next.components = components;
    next.openvrDir = flat ? std::wstring() : relativeOpenvr(s.game.dir, nativeDir);
    next.nativeInstalled=!flat;

    // The chain is DECIDED by this run, not inherited from the record. The
    // record says what was true last time, and whether it is still true is the
    // question this pass exists to answer -- carrying the value forward made
    // "the mod we chained to has been uninstalled" invisible, because the field
    // was never empty for the check below to notice.
    next.chainTarget.clear();
    next.chainMod.clear();

    auto backup = [&](const std::wstring& path, const std::string& why) {
        Step st;
        st.action = Action::Backup;
        st.from = path;
        st.to = joinPath(plan.backupDir, leafOf(path));
        st.why = why;
        body.push_back(st);
        wantBackupDir = true;
    };
    auto rename = [&](const std::wstring& from, const std::wstring& to, const std::string& why,
                      const std::string& expectSha = std::string()) {
        Step st;
        st.action = Action::Rename;
        st.from = from;
        st.to = to;
        st.why = why;
        st.expectSha = expectSha;
        st.required = true;   // a rename with nothing to rename is a stale plan
        body.push_back(st);
        changed = true;
    };
    auto writePayload = [&](const char* item, const std::wstring& to, const std::string& why) {
        Step st;
        st.action = Action::WritePayload;
        st.item = item;
        st.to = to;
        st.why = why;
        body.push_back(st);
        changed = true;
    };
    auto writeText = [&](const std::string& text, const std::wstring& to, const std::string& why) {
        Step st;
        st.action = Action::WriteText;
        st.text = text;
        st.to = to;
        st.why = why;
        body.push_back(st);
    };
    auto copyInto = [&](const std::wstring& from, const std::wstring& to, const std::string& why) {
        Step st;
        st.action = Action::Backup;  // a copy that leaves the source alone
        st.from = from;
        st.to = to;
        st.why = why;
        // This one is a restore, not a backup: if the file it reads has gone,
        // skipping it quietly would reinstall the proxy over nothing and call
        // the result a success.
        st.required = true;
        body.push_back(st);
        changed = true;
    };

    // ------------------------------------------------------------------
    // d3d11.dll -- the fixes. One name, and every mod in this space wants it.
    // ------------------------------------------------------------------
    if (p.haveD3d11) {
        const std::wstring dst = joinPath(s.game.dir, kD3d11);
        const DllInfo& cur = s.d3d11;

        if (cur.kind == DllKind::Unreadable) {
            plan.problems.push_back(
                "There is a d3d11.dll next to the game that cannot be read -- something has it "
                "open, or it is not a DLL. EDVR's own d3d11.dll was not installed; nothing else "
                "was touched.");
        } else if (cur.kind == DllKind::Edvr) {
            if (cur.sha256 == p.d3d11Sha && !o.repair) {
                plan.notes.push_back("d3d11.dll is already this build -- left alone.");
            } else {
                backup(dst, "the EDVR d3d11.dll being replaced");
                writePayload("d3d11", dst,
                             o.repair ? "reinstalls EDVR's d3d11.dll" : "updates EDVR's d3d11.dll");
                plan.notes.push_back(o.repair ? "Reinstalling d3d11.dll (the fixes)."
                                              : "Updating d3d11.dll (the fixes).");
            }
            next.d3d11Installed = true;
            next.d3d11Sha = p.d3d11Sha;
        } else if (cur.kind == DllKind::Absent) {
            writePayload("d3d11", dst, "installs EDVR's d3d11.dll");
            plan.notes.push_back(
                s.state.d3d11Installed
                    ? "Restoring d3d11.dll -- EDVR was installed here, and the file is gone. "
                      "(EDHM's uninstaller runs `del d3d11.dll`, which after an EDVR install is "
                      "ours.)"
                    : "Installing d3d11.dll (the fixes).");
            next.d3d11Installed = true;
            next.d3d11Sha = p.d3d11Sha;
        } else {
            // Somebody else is in the slot. Rename theirs, take the name, and
            // point EDVR at it -- the README's manual procedure, done by the
            // thing that knows which file it is looking at.
            const std::wstring chainLeaf = chainNameFor(cur);
            const std::wstring chainPath = joinPath(s.game.dir, chainLeaf);
            const std::wstring modName = modNameOf(cur);
            const std::string modSay = modName.empty() ? "That mod" : say(modName);

            const DllInfo* existing = siblingNamed(s.otherD3d11, chainLeaf);
            if (existing && existing->kind != DllKind::Absent) {
                // The name we want is taken -- which happens when that mod
                // reinstalled itself over EDVR while its earlier copy was
                // still parked there. Keep the newcomer (it is the newer build
                // of the same mod) and put the old one in the backup folder,
                // where nothing can mistake it for live.
                const std::wstring parkedMod = modNameOf(*existing);
                const bool sameMod = !modName.empty() && parkedMod == modName;
                backup(chainPath, sameMod
                                      ? "the older " + modSay + " copy already parked here"
                                      : "the file already parked under that name");
                if (!sameMod) {
                    plan.problems.push_back(
                        "There was already a " + say(chainLeaf) +
                        " here and it is not the same mod as the one in the d3d11.dll slot. It "
                        "has been copied into the backup folder and replaced, so check that "
                        "folder before deleting anything.");
                }
            }
            backup(dst, "the " + modSay + " d3d11.dll found in EDVR's place");
            rename(dst, chainPath, "moves " + modSay + " aside, keeping it installed",
                   cur.sha256);
            writePayload("d3d11", dst, "installs EDVR's d3d11.dll in its place");

            forced.emplace_back("advanced.real_dll", say(chainLeaf));
            next.chainTarget = chainLeaf;
            next.chainMod = modName;

            // EDVR passes calls through ONE other mod. If it was already
            // chained to a different one, that one is still on disk under a
            // name nothing loads -- and saying nothing about it is how
            // somebody's EDHM quietly stops working after installing ReShade.
            if (!s.state.chainTarget.empty() &&
                _wcsicmp(s.state.chainTarget.c_str(), chainLeaf.c_str()) != 0) {
                const DllInfo* previous = siblingNamed(s.otherD3d11, s.state.chainTarget);
                if (previous && previous->kind != DllKind::Absent) {
                    plan.problems.push_back(
                        "EDVR was passing calls through " + say(s.state.chainTarget) +
                        (s.state.chainMod.empty() ? std::string()
                                                  : " (" + say(s.state.chainMod) + ")") +
                        ", and now passes them through " + modSay +
                        " instead -- EDVR can only chain to one. The older one is still in the "
                        "folder but nothing loads it. If you want that one back, set "
                        "advanced.real_dll to it by hand.");
                }
            }
            next.d3d11Installed = true;
            next.d3d11Sha = p.d3d11Sha;

            if (s.state.d3d11Installed) {
                plan.notes.push_back(modSay +
                                     " has replaced EDVR's d3d11.dll since the last install. Both "
                                     "are kept: it becomes " + say(chainLeaf) +
                                     ", EDVR takes the d3d11.dll name back, and every call passes "
                                     "through to it.");
            } else {
                plan.notes.push_back(modSay + " is installed as d3d11.dll. It is renamed " +
                                     say(chainLeaf) + ", EDVR takes its place, and "
                                     "advanced.real_dll points back at it -- both mods run.");
            }
            if (!cur.is64) {
                plan.problems.push_back(
                    "The d3d11.dll found in the game folder is 32-bit, which the 64-bit game "
                    "cannot have been loading. It has been kept, but check what put it there.");
            }
        }

    }

    // A chain target that has gone away leaves advanced.real_dll pointing at
    // nothing: EDVR then writes a fatal note beside the game on every launch
    // and falls back to the system DLL. Whether the mod we were passing calls
    // through to is still there is asked on every run, including one that did
    // not touch d3d11.dll at all.
    if (next.chainTarget.empty() && !s.state.chainTarget.empty()) {
        const DllInfo* target = siblingNamed(s.otherD3d11, s.state.chainTarget);
        if (target && target->kind != DllKind::Absent) {
            next.chainTarget = s.state.chainTarget;
            next.chainMod = s.state.chainMod;
            plan.notes.push_back("Keeping the chain to " + say(s.state.chainTarget) + ".");
        } else {
            forced.emplace_back("advanced.real_dll", "");
            plan.notes.push_back("The mod EDVR was passing calls through to (" +
                                 say(s.state.chainTarget) +
                                 ") is no longer there, so advanced.real_dll has been cleared.");
        }
    }

    // ------------------------------------------------------------------
    // nvngx_dlss.dll -- NVIDIA's DLSS runtime, which temporal_aa = dlaa and
    // dlss need beside the game's executable (where NGX looks for it). The
    // driver does not ship it and the SDK's licence lets an application
    // carry it, which is what this installer is. Three rules: placed only
    // where an NVIDIA adapter is present (it is 59 MB, and on any other
    // card the pass falls back to its own history regardless); a copy that
    // is not ours -- the user's own, or one NVIDIA's updater replaced -- is
    // left alone; and only the copy we placed is ever replaced or removed,
    // by its hash in the install record.
    // ------------------------------------------------------------------
    if (p.haveNgx) {
        const std::wstring dst = joinPath(s.game.dir, kNgx);
        const DllInfo& cur = s.ngx;
        const bool present = cur.kind != DllKind::Absent && cur.kind != DllKind::Unreadable;
        const bool ours = present && s.state.ngxInstalled && !s.state.ngxSha.empty() &&
                          cur.sha256 == s.state.ngxSha;
        next.ngxInstalled = false;
        next.ngxSha.clear();
        if (cur.kind == DllKind::Unreadable) {
            plan.problems.push_back(
                "There is an nvngx_dlss.dll beside the game that cannot be read -- something has "
                "it open. NVIDIA's DLSS runtime was not touched.");
        } else if (present && cur.sha256 == p.ngxSha && !o.repair) {
            plan.notes.push_back(
                "nvngx_dlss.dll (NVIDIA's DLSS runtime) is already this build's -- left alone.");
            next.ngxInstalled = true;
            next.ngxSha = p.ngxSha;
        } else if (present && !ours && cur.sha256 != p.ngxSha) {
            plan.notes.push_back(
                "A different nvngx_dlss.dll is already beside the game (yours, or one NVIDIA's "
                "updater replaced) -- left alone; temporal_aa = dlaa uses it.");
        } else if (!s.nvidiaAdapter) {
            plan.notes.push_back(
                "No NVIDIA graphics card was found, so NVIDIA's DLSS runtime (nvngx_dlss.dll, "
                "59 MB) was not placed. temporal_aa = dlaa and dlss need one, and run EDVR's own "
                "history without it.");
            if (ours) {
                next.ngxInstalled = true;
                next.ngxSha = s.state.ngxSha;
            }
        } else {
            if (present) backup(dst, "the nvngx_dlss.dll EDVR placed, being replaced");
            writePayload("ngx", dst,
                         present ? "updates NVIDIA's DLSS runtime"
                                 : "places NVIDIA's DLSS runtime beside the game");
            plan.notes.push_back(
                present ? "Updating nvngx_dlss.dll (NVIDIA's DLSS runtime)."
                        : "Placing nvngx_dlss.dll (NVIDIA's DLSS runtime, for temporal_aa = dlaa).");
            next.ngxInstalled = true;
            next.ngxSha = p.ngxSha;
        }
    } else if (s.state.ngxInstalled) {
        // This installer carries no runtime (a build without the SDK): what an
        // earlier one placed stays, and stays recorded as ours.
        plan.notes.push_back(
            "This installer carries no DLSS runtime; the nvngx_dlss.dll a previous one placed "
            "is left where it is.");
    }

    // XeSS-SR ships with every scope, and is present only when the build
    // carried it. Preserve foreign or changed files; replacement and uninstall
    // are limited to hashes EDVR recorded. Like the DLSS runtime above, the
    // report says what happened to it in one line, whatever the outcome.
    if (p.haveXess) {
        if (p.xessLicenseSha.empty() || p.xessThirdPartySha.empty()) {
            plan.blocked = true;
            plan.problems.push_back("XeSS runtime lacks its license notices; nothing will be installed.");
            return plan;
        }
        struct XessFile { const char* item; const wchar_t* leaf; const DllInfo* cur;
                          const std::string* desired; std::string* recorded; };
        XessFile files[] = {
            {"xess", kXess, &s.xess, &p.xessSha, &next.xessSha},
            {"xess_license", kXessLicense, &s.xessLicense, &p.xessLicenseSha, &next.xessLicenseSha},
            {"xess_third_party", kXessThirdParty, &s.xessThirdParty, &p.xessThirdPartySha, &next.xessThirdPartySha},
        };
        const std::string previous[] = {s.state.xessSha, s.state.xessLicenseSha, s.state.xessThirdPartySha};
        unsigned placed = 0, updated = 0, current = 0;
        for (unsigned i=0; i<3; ++i) {
            const auto& file = files[i];
            const bool present = file.cur->kind != DllKind::Absent;
            const bool ours = present && !previous[i].empty() && file.cur->sha256 == previous[i];
            if (file.cur->kind == DllKind::Unreadable) {
                plan.problems.push_back("The XeSS destination " + say(file.leaf) +
                                        " cannot be read; this optional file was left untouched.");
                continue;
            }
            if (present && file.cur->sha256 == *file.desired && !o.repair) {
                *file.recorded = *file.desired;
                ++current;
            } else if (present && !ours && file.cur->sha256 != *file.desired) {
                plan.notes.push_back("A different " + say(file.leaf) + " is present; left alone.");
                file.recorded->clear();
            } else {
                const std::wstring dst = joinPath(s.game.dir, file.leaf);
                if (present) backup(dst, "the XeSS file EDVR placed, being replaced");
                writePayload(file.item, dst, "places Intel XeSS-SR runtime or notice");
                *file.recorded = *file.desired;
                ++(present ? updated : placed);
            }
        }
        if (placed || updated)
            plan.notes.push_back(placed && !updated
                ? "Placing libxess.dll (Intel's XeSS-SR runtime, for temporal_aa = xess) and its licence notices."
                : "Updating libxess.dll (Intel's XeSS-SR runtime) and its licence notices.");
        else if (current == 3)
            plan.notes.push_back("libxess.dll (Intel's XeSS-SR runtime) is already this build's -- left alone.");
    }

    if (!flat) {
    // The game-facing OpenVR ABI is always provided by native OpenXR. The
    // original DLL is retained for uninstall, never loaded by this backend.
    const std::wstring runtime=joinPath(nativeDir,kOpenvr);
    std::wstring originalName=safeSiblingName(s.openvrOrigName,kOpenvr);
    if(originalName.empty())originalName=kOpenvrOrig;
    const std::wstring original=joinPath(nativeDir,originalName);
    const DllInfo& current=s.openvrCurrent;
    const DllInfo& saved=s.openvrOrig;
    next.openvrOrigName=originalName;
    next.openvrOrigSha=saved.kind==DllKind::OpenVrRuntime?saved.sha256:std::string();
    // The tool kept the game's original under its own name. Copy it to the name this installer keeps it under, so the record and the
    // next survey find it as any other original, and a later flat conversion or uninstall can restore it. The tool's backup stays put.
    // Not when the game's own runtime is in place: that one is renamed to the original name below, as it always was.
    if(saved.kind==DllKind::Absent && current.kind!=DllKind::OpenVrRuntime && s.toolOriginal.kind==DllKind::OpenVrRuntime) {
        Step adopt;adopt.action=Action::Backup;adopt.from=s.toolOriginal.path;adopt.to=original;
        adopt.expectSha=s.toolOriginal.sha256;adopt.required=true;
        adopt.why="keeps the game's original VR library, from the backup the tool made, where EDVR can restore it";
        body.push_back(adopt);changed=true;
        next.openvrOrigSha=s.toolOriginal.sha256;
        plan.notes.push_back("Adopting the game's original VR library from the backup the tool made, so it can be restored later.");
    }
    if(current.kind==DllKind::Edvr && current.sha256==p.openvrSha && !o.repair) {
        plan.notes.push_back("Native OpenXR runtime is already this build -- left alone.");
    } else {
        if(current.kind!=DllKind::Absent) {
            backup(runtime,"the previous VR library, for recovery");
            body.back().required=true;body.back().expectSha=current.sha256;
            if(current.kind==DllKind::OpenVrRuntime &&
               (saved.kind==DllKind::Absent || (saved.kind==DllKind::OpenVrRuntime && saved.sha256!=current.sha256))) {
                if(saved.kind==DllKind::OpenVrRuntime) {
                    backup(original,"the superseded original runtime");
                    body.back().required=true;body.back().expectSha=saved.sha256;
                    plan.notes.push_back("The game updated its VR library; keeping the newer original for uninstall.");
                }
                rename(runtime,original,"preserves the original runtime for uninstall",current.sha256);
                next.openvrOrigSha=current.sha256;
            }
        }
        writePayload("openvr",runtime,"installs native OpenXR using the Windows-selected runtime");
        plan.notes.push_back("Installing native OpenXR. The original VR library is not used at runtime.");
    }
    next.openvrInstalled=true;next.openvrSha=p.openvrSha;
    next.nativeGraphicsSha=p.d3d11Sha;next.nativeRuntimeSha=p.openvrSha;
    next.nativeOriginalName=next.openvrOrigName;next.nativeOriginalSha=next.openvrOrigSha;
    auto nativeAsset=[&](const DllInfo& current,const wchar_t* name,const char* item,const std::string& wanted) {
        const auto destination=joinPath(nativeDir,name);
        if(current.kind!=DllKind::Absent && current.sha256==wanted && !o.repair)return;
        if(current.kind!=DllKind::Absent) {
            backup(destination,"the previous native OpenXR file");
            body.back().required=true;body.back().expectSha=current.sha256;
        }
        writePayload(item,destination,"installs the bundled OpenXR dependency");
    };
    nativeAsset(s.openxrLoader,L"openxr_loader.dll","openxr_loader",p.openxrLoaderSha);
    nativeAsset(s.openxrLicense,L"OPENXR-LOADER-LICENSE.txt","openxr_license",p.openxrLicenseSha);
    next.openxrLoaderSha=p.openxrLoaderSha;next.openxrLicenseSha=p.openxrLicenseSha;
    const std::string config="[openxr]\r\nversion=1\r\nloader="+
        toUtf8(joinPath(nativeDir,L"openxr_loader.dll"))+"\r\ngraphics="+
        toUtf8(joinPath(s.game.dir,kD3d11))+"\r\nruntime=system\r\nseparate_device=1\r\n";
    next.nativeConfigSha=sha256Bytes(config.data(),config.size());
    if(s.nativeConfig.sha256!=next.nativeConfigSha || o.repair) {
        const auto destination=joinPath(nativeDir,L"edvr_openxr.ini");
        if(s.nativeConfig.kind!=DllKind::Absent) {
            backup(destination,"the previous OpenXR startup configuration");
            body.back().required=true;body.back().expectSha=s.nativeConfig.sha256;
        }
        writeText(config,destination,"selects the Windows OpenXR runtime");changed=true;
    }
    } else {
        // Flat owns no VR runtime. What a VR install left in Openvr\win64 is retired only by its proofs: the runtime when it is EDVR's
        // build (proved above), and each OpenXR file by its hash or its content, whether or not this folder's record lists it.
        const std::wstring runtime = joinPath(nativeDir, kOpenvr);
        if (retireRuntime) {
            backup(runtime, "the EDVR VR runtime being retired");
            body.back().required = true; body.back().expectSha = s.openvrCurrent.sha256;
            Step removeRuntime; removeRuntime.action = Action::Delete; removeRuntime.from = runtime;
            removeRuntime.expectSha = s.openvrCurrent.sha256;
            removeRuntime.why = "retires EDVR's VR runtime"; body.push_back(removeRuntime); changed = true;
            const bool savedOriginal = s.openvrOrig.kind == DllKind::OpenVrRuntime;
            Step restoreOriginal;
            restoreOriginal.action = savedOriginal ? Action::Rename : Action::Backup;
            restoreOriginal.from = savedOriginal ? s.openvrOrig.path : s.openvrOrigInBackups.front();
            restoreOriginal.to = runtime;
            restoreOriginal.expectSha = savedOriginal ? s.openvrOrig.sha256 : sha256File(restoreOriginal.from);
            restoreOriginal.required = true;
            restoreOriginal.why = "restores the game's original VR library";
            body.push_back(restoreOriginal);
        }
        const struct NativeFile { const DllInfo* info; bool owned; bool matches; } nativeFiles[] = {
            {&s.openxrLoader, loaderOwned(s, p.openxrLoaderSha), sameSha(s.openxrLoader.sha256, p.openxrLoaderSha)},
            {&s.openxrLicense, licenseOwned(s, p.openxrLicenseSha), sameSha(s.openxrLicense.sha256, p.openxrLicenseSha)},
            {&s.nativeConfig, startupConfigOwned(s), false},
        };
        for (const NativeFile& file : nativeFiles) {
            if (file.info->kind == DllKind::Absent) continue;
            if (!file.owned) {
                plan.notes.push_back(file.matches
                    ? say(leafOf(file.info->path)) + " " + kMatchesNoProof
                    : "Keeping a changed OpenXR file: " + say(leafOf(file.info->path)));
                continue;
            }
            backup(file.info->path, "the EDVR OpenXR asset being retired");
            body.back().required = true; body.back().expectSha = file.info->sha256;
            Step retire; retire.action = Action::Delete; retire.from = file.info->path;
            retire.expectSha = file.info->sha256; retire.why = "retires EDVR's OpenXR asset";
            body.push_back(retire);
            changed = true;
        }
        next.openvrInstalled = false; next.openvrSha.clear();
        next.openvrOrigName.clear(); next.openvrOrigSha.clear();
        next.nativeRuntimeSha.clear(); next.openxrLoaderSha.clear();
        next.openxrLicenseSha.clear(); next.nativeConfigSha.clear();
        next.nativeOriginalName.clear(); next.nativeOriginalSha.clear();
        if (retireRuntime) {
            plan.notes.push_back("Converting VR to flat: the game's original VR library is in place and verified EDVR OpenXR assets are retired.");
        } else if (!installed.empty() && installed != "flat") {
            plan.notes.push_back("Converting to flat: the VR library here is not EDVR's, so it is left where it is.");
        }
    }
    if (flat) next.nativeGraphicsSha = p.d3d11Sha;

    // The descriptor is the scope's canonical text, not the payload's: a scope change rewrites it.
    const std::wstring descriptorPath = joinPath(s.game.dir, kProfile);
    if (!s.descriptorPresent || s.descriptorSha != descriptorSha || o.repair) {
        if (s.descriptorPresent) {
            backup(descriptorPath, "the previous EDVR profile descriptor");
            body.back().required = true; body.back().expectSha = s.descriptorSha;
        }
        writeText(descriptor, descriptorPath, "declares the installed EDVR scope");
        changed = true;
    }

    // ------------------------------------------------------------------
    // The settings files the scope installs: edvr.ini for vr and both, edvr-flat.ini for flat and both. Last, because the chain
    // decision above forces a value into them.
    //
    // A file the scope does not install is neither merged from nor written. The flat
    // runtime reads edvr-flat.ini and only falls back to edvr.ini while there is
    // none (config.cpp), so a flat install that merged into edvr.ini reported
    // success while the game went on reading the old flat file -- and, where
    // there was no flat file, changed the VR profile's tuning instead. A scope
    // change leaves the file it stops managing exactly where it is.
    // ------------------------------------------------------------------
    // Each settings file the scope installs is merged on its own, against its own base. The flat file is done first, so the VR
    // file's merge report (plan.merge) is the one left when both are installed.
    plan.settingsFile = wantsVrIni ? "edvr.ini" : "edvr-flat.ini";
    // The base is the shipped text of the version that last wrote the file: edvr.ini.base for edvr.ini, edvr-flat.ini.base for
    // edvr-flat.ini. Which version wrote edvr.ini.base is the RECORD's question, not the descriptor's: the descriptor says what this
    // run installs, and an older flat record left its flat defaults in that base. So the base vouches for the VR file only under a
    // VR-scope record (vr or both); a flat record, or none, vouches for nothing. Where a base does not describe the file, the merge
    // is two-way.
    const std::string baseWriter = s.state.present ? recordedScope(s.state) : std::string();
    const bool vrBaseDescribes = baseWriter == "vr" || baseWriter == "both";
    const std::string* vrBase = (vrBaseDescribes && !s.baseIniText.empty()) ? &s.baseIniText : nullptr;
    const std::string* flatBase = s.flatBaseIniText.empty() ? nullptr : &s.flatBaseIniText;
    std::string vrSha, flatSha;

    auto manageSettings = [&](bool isFlat) {
        const char* profile = isFlat ? "flat" : "vr";
        const std::string iniName = toUtf8(settingsLeafFor(profile));
        const std::string& payloadText = isFlat ? p.flatIniText : p.iniText;
        const std::wstring iniPath = settingsPathFor(s.game.dir, profile);
        const bool ownPresent = isFlat ? s.flatIniPresent : s.iniPresent;
        const std::string& ownText = isFlat ? s.flatIniText : s.iniText;

        // A flat install that finds only the shared edvr.ini starts its own file
        // from it, so the settings the flat runtime has been reading carry over
        // (install_edvr.py seeds the same way); edvr.ini itself is not touched.
        // Somebody who asked for fresh defaults gets them instead.
        const bool seed = isFlat && !ownPresent && s.iniPresent && o.keepSettings;
        // A seed is edvr.ini's text, so it merges against edvr.ini's base. The file's own base is the one
        // edvr-flat.ini.base the flat install that wrote it kept.
        const std::string* base = (isFlat && !seed) ? flatBase : vrBase;

        std::string merged;
        if (seed) {
            merged = mergeIni(payloadText, s.iniText, base, forced, &plan.merge);
            plan.notes.push_back(
                "Creating " + iniName + " from your edvr.ini: the flat edition keeps its own "
                "settings now, and edvr.ini is left exactly as it is.");
        } else if (!ownPresent) {
            merged = mergeIni(payloadText, std::string(), nullptr, forced, &plan.merge);
            plan.notes.push_back("Writing " + iniName + " (every setting at its default).");
        } else if (!o.keepSettings) {
            merged = mergeIni(payloadText, std::string(), nullptr, forced, &plan.merge);
            backup(iniPath, "your " + iniName + ", before it is replaced");
            plan.notes.push_back(
                "Replacing " + iniName + " with the shipped defaults. Your old file is in the "
                "backup folder.");
        } else {
            // The user's text is this edition's own file, so a setting this version dropped is
            // "carried over from your" that file: edvr-flat.ini for the flat edition.
            merged = mergeIni(payloadText, ownText, base, forced, &plan.merge, iniName.c_str());
            if (merged != ownText) {
                backup(iniPath, "your " + iniName + ", before it is updated");

                char line[256];
                sprintf_s(
                    line, "Updating %s: %zu of your settings kept, %zu new defaults adopted%s.",
                    iniName.c_str(), plan.merge.kept.size(), plan.merge.adopted.size(),
                    plan.merge.twoWay ? " (no record of which version you had, so anything that "
                                        "differs from the new defaults was treated as yours)"
                                      : "");
                plan.notes.push_back(line);
                if (!plan.merge.retired.empty()) {
                    plan.notes.push_back(
                        "Some settings you had set no longer exist in this version; they were "
                        "carried to the end of their section with a note rather than dropped.");
                }
            }
        }
        if (merged != ownText) {
            writeText(merged, iniPath, "writes " + iniName);
            changed = true;
        } else {
            // Said once, and only here: an "updating" note followed by "left
            // alone" is two lines contradicting each other about the same file.
            plan.notes.push_back(iniName + " already says what it should -- left alone.");
        }
        (isFlat ? flatSha : vrSha) = sha256Bytes(merged.data(), merged.size());
    };
    if (wantsFlatIni) manageSettings(true);
    if (wantsVrIni) manageSettings(false);
    // The record's settings digest is the VR file's when the scope has one (the file the older records named), else the flat file's.
    next.iniSha = wantsVrIni ? vrSha : flatSha;

    const bool ownershipChanged = next.xessSha != s.state.xessSha ||
                                  next.xessLicenseSha != s.state.xessLicenseSha ||
                                  next.xessThirdPartySha != s.state.xessThirdPartySha;
    const bool stateOnly = !changed && body.empty() && ownershipChanged && !o.repair;
    if (stateOnly)
        plan.notes.push_back("Optional runtime ownership changed; only the install record will be updated.");
    if (!changed && !ownershipChanged && !o.repair) {
        plan.nothingToDo = true;
        plan.notes.push_back("Everything is already in place. Nothing to do.");
    }

    // ------------------------------------------------------------------
    // Assemble: directories first, then the body, then the record. The record
    // is written LAST so that a run interrupted half way leaves the old one,
    // which still describes the folder better than a half-true new one.
    // ------------------------------------------------------------------
    if (!plan.nothingToDo) {
        if (wantBackupDir) {
            Step mk;
            mk.action = Action::MakeDir;
            mk.to = plan.backupDir;
            mk.why = "keeps a copy of everything replaced";
            plan.steps.push_back(mk);
        }
        if (!flat) {
            Step nativeFolder;nativeFolder.action=Action::MakeDir;nativeFolder.to=nativeDir;
            nativeFolder.why="holds the native OpenXR runtime and loader";plan.steps.push_back(nativeFolder);
        }
        plan.steps.insert(plan.steps.end(), body.begin(), body.end());

        Step mkState;
        mkState.action = Action::MakeDir;
        mkState.to = stateDirPath(s.game.dir);
        mkState.why = "holds the install record";
        plan.steps.push_back(mkState);

        // Each base is written from its own payload file, by the install that manages that file.
        if (!stateOnly) {
            if (wantsVrIni) {
                Step baseStep;
                baseStep.action = Action::WriteText;
                baseStep.text = p.iniText;
                baseStep.to = baseIniPath(s.game.dir);
                baseStep.why = "keeps this version's default edvr.ini, so the next update can tell your changes from a changed default";
                plan.steps.push_back(baseStep);
            }
            if (wantsFlatIni) {
                Step baseStep;
                baseStep.action = Action::WriteText;
                baseStep.text = p.flatIniText;
                baseStep.to = flatBaseIniPath(s.game.dir);
                baseStep.why = "keeps this version's default edvr-flat.ini, so the next update can tell your changes from a changed default";
                plan.steps.push_back(baseStep);
            }
        }

        Step rec;
        rec.action = Action::WriteText;
        rec.text = serializeState(next);
        rec.to = statePath(s.game.dir);
        rec.why = "records what was installed";
        plan.steps.push_back(rec);
    }

    plan.nextState = next;
    return plan;
}

Plan planUninstall(const Survey& s, const Options& o, const PayloadInfo& p) {
    Plan plan;
    if((s.state.nativeInstalled || s.openvrCurrent.nativeRuntimeExports) && s.openvrCurrent.kind==DllKind::Edvr &&
       s.openvrOrig.kind!=DllKind::OpenVrRuntime && s.openvrOrigInBackups.empty()) {
        plan.blocked=true;plan.problems.push_back("The original VR library is missing. Repair the game files before uninstalling; the native pair was left intact.");return plan;
    }
    plan.backupDir =
        joinPath(backupRootPath(s.game.dir), o.backupStamp.empty() ? L"backup" : o.backupStamp);

    if (s.gameRunningHere) {
        plan.blocked = true;
        plan.problems.push_back("Elite Dangerous is running. Close it first.");
        return plan;
    }
    if (s.gameRunStateUnknown) {
        plan.blocked = true;
        plan.problems.push_back(kRunStateUnknown);
        return plan;
    }
    if (s.gameRunningElsewhere) plan.notes.push_back(kRunningElsewhere);

    // Which scope this uninstall removes, and so which settings files are its own.
    // A folder with no record and no descriptor counts as a VR one: the flat
    // edition always writes a descriptor.
    const std::string installed = installedScope(s);
    const std::string scope = installed.empty() ? std::string("vr") : installed;
    const bool flatEdition = scope == "flat";
    // What the runtime reads for the chain: the flat one falls back to the shared
    // edvr.ini until it has a file of its own.
    const bool runtimeReadsFlatFile = flatEdition && s.flatIniPresent;
    const std::string& runtimeIniText = runtimeReadsFlatFile ? s.flatIniText : s.iniText;
    const std::string runtimeIniName = runtimeReadsFlatFile ? "edvr-flat.ini" : "edvr.ini";

    std::vector<Step> body;
    bool wantBackupDir = false;
    bool changed = false;

    auto backup = [&](const std::wstring& path, const std::string& why) {
        Step st;
        st.action = Action::Backup;
        st.from = path;
        st.to = joinPath(plan.backupDir, leafOf(path));
        st.why = why;
        body.push_back(st);
        wantBackupDir = true;
    };
    auto remove = [&](const std::wstring& path, const std::string& why,
                      const std::string& expectSha = std::string()) {
        Step st;
        st.action = Action::Delete;
        st.from = path;
        st.why = why;
        st.expectSha = expectSha;   // do not delete a file that changed under us
        body.push_back(st);
        changed = true;
    };
    auto rename = [&](const std::wstring& from, const std::wstring& to, const std::string& why,
                      const std::string& expectSha = std::string()) {
        Step st;
        st.action = Action::Rename;
        st.from = from;
        st.to = to;
        st.why = why;
        st.expectSha = expectSha;
        st.required = true;
        body.push_back(st);
        changed = true;
    };
    auto restore = [&](const std::wstring& from, const std::wstring& to, const std::string& why) {
        Step st;
        st.action = Action::Backup;  // a copy: the backup folder keeps its copy
        st.from = from;
        st.to = to;
        st.why = why;
        st.required = true;
        body.push_back(st);
        changed = true;
    };

    // ---- d3d11 --------------------------------------------------------
    const std::wstring d3d11Path = joinPath(s.game.dir, kD3d11);
    if (s.d3d11.kind == DllKind::Edvr) {
        backup(d3d11Path, "EDVR's d3d11.dll");
        remove(d3d11Path, "removes EDVR's d3d11.dll", s.d3d11.sha256);

        // Whatever we moved aside goes back under the name it had. Without
        // this, uninstalling EDVR silently uninstalls the other mod too: its
        // file is still on disk but under a name nothing loads.
        std::wstring chain = safeSiblingName(s.state.chainTarget, kD3d11);
        if (chain.empty())
            chain = safeSiblingName(fromUtf8(iniValue(runtimeIniText, "advanced.real_dll")), kD3d11);
        // A both install's flat file carries the same chain key; it answers when the VR file does not.
        if (chain.empty() && scope == "both")
            chain = safeSiblingName(fromUtf8(iniValue(s.flatIniText, "advanced.real_dll")), kD3d11);
        const DllInfo* target = chain.empty() ? nullptr : siblingNamed(s.otherD3d11, chain);
        if (target && target->kind != DllKind::Absent) {
            rename(target->path, d3d11Path,
                   "puts " + (s.state.chainMod.empty() ? std::string("the other mod")
                                                       : say(s.state.chainMod)) +
                       " back under its own name",
                   target->sha256);
            plan.notes.push_back("Removing EDVR's d3d11.dll and restoring " + say(chain) +
                                 " as d3d11.dll.");
        } else {
            plan.notes.push_back("Removing EDVR's d3d11.dll.");
            if (!chain.empty()) {
                plan.problems.push_back(
                    runtimeIniName + " pointed at " + say(chain) +
                    ", but that file is not there any more, so there is nothing to put back.");
            }
        }
    } else if (s.d3d11.kind == DllKind::Absent) {
        plan.notes.push_back("No d3d11.dll to remove.");
    } else {
        plan.notes.push_back("The d3d11.dll here is not EDVR's (" + say(describeDll(s.d3d11)) +
                             ") -- left alone.");
    }

    // ---- openvr -------------------------------------------------------
    if (s.haveOpenvrDir) {
        const std::wstring dst = joinPath(s.game.openvrDir, kOpenvr);
        std::wstring origName = safeSiblingName(s.openvrOrigName, kOpenvr);
        if (origName.empty()) origName = kOpenvrOrig;
        const std::wstring origPath = joinPath(s.game.openvrDir, origName);
        if (s.openvrCurrent.kind == DllKind::Edvr) {
            if (s.openvrOrig.kind == DllKind::OpenVrRuntime) {
                backup(dst, "EDVR's openvr_api.dll");
                remove(dst, "removes EDVR's openvr_api.dll", s.openvrCurrent.sha256);
                rename(origPath, dst, "puts the game's own openvr_api.dll back",
                       s.openvrOrig.sha256);
                plan.notes.push_back(
                    "Removing EDVR's openvr_api.dll and renaming the game's original back.");
            } else if (!s.openvrOrigInBackups.empty()) {
                // The renamed original is gone, but this installer kept a copy
                // of it before it ever renamed anything. Uninstalling from that
                // copy is a great deal better than sending somebody to their
                // launcher's file verification.
                const std::wstring& copy = s.openvrOrigInBackups.front();
                backup(dst, "EDVR's openvr_api.dll");
                restore(copy, dst, "puts the game's own openvr_api.dll back from an EDVR backup");
                plan.notes.push_back(
                    "The renamed original is missing, so the game's own openvr_api.dll is being "
                    "restored from the backup this installer made before it first renamed it.");
            } else {
                // Deleting ours here would leave NO openvr_api.dll at all --
                // worse than leaving EDVR installed, and done in the name of
                // removing it. The file stays until there is something to put
                // in its place.
                plan.problems.push_back(
                    "EDVR's openvr_api.dll has been left in place: the game's original is not "
                    "there to put back, and no backup of it was found, so removing ours would "
                    "leave that folder with no openvr_api.dll at all. Verify or repair the game "
                    "files in your launcher -- that restores the original -- and then uninstall "
                    "again.");
            }
        } else if (s.openvrCurrent.kind == DllKind::OpenVrRuntime &&
                   s.openvrOrig.kind == DllKind::OpenVrRuntime) {
            plan.notes.push_back(
                "The game's own openvr_api.dll is already in place; the spare copy at "
                "openvr_api_orig.dll was left where it is.");
        } else {
            plan.notes.push_back("No EDVR openvr_api.dll to remove.");
        }
    }

    // The OpenXR files EDVR placed beside the VR runtime are removed by their proofs, whether or not this folder's record lists them
    // (a record from an older flat install lists none); a changed one is kept.
    {
        auto retire=[&](const DllInfo& info,bool owned,bool matches) {
            if(info.kind==DllKind::Absent)return;
            if(!owned) {
                plan.notes.push_back(matches ? say(leafOf(info.path))+" "+kMatchesNoProof
                                             : "Keeping a changed OpenXR file: "+say(leafOf(info.path)));
                return;
            }
            backup(info.path,"the native OpenXR file being removed");
            remove(info.path,"removes EDVR's OpenXR dependency",info.sha256);
        };
        retire(s.openxrLoader,loaderOwned(s,p.openxrLoaderSha),sameSha(s.openxrLoader.sha256,p.openxrLoaderSha));
        retire(s.openxrLicense,licenseOwned(s,p.openxrLicenseSha),sameSha(s.openxrLicense.sha256,p.openxrLicenseSha));
        retire(s.nativeConfig,startupConfigOwned(s),false);
    }

    // ---- NVIDIA's DLSS runtime ----------------------------------------
    // Removed only when it is the copy this installer placed, by its hash: a
    // copy the user put there themselves, or one NVIDIA's updater replaced,
    // is theirs to keep. No backup of a 59 MB file the payload already holds.
    const std::wstring ngxPath = joinPath(s.game.dir, kNgx);
    if (s.state.ngxInstalled && !s.state.ngxSha.empty() && s.ngx.kind != DllKind::Absent &&
        s.ngx.kind != DllKind::Unreadable && s.ngx.sha256 == s.state.ngxSha) {
        remove(ngxPath, "removes NVIDIA's DLSS runtime, which EDVR placed", s.ngx.sha256);
        plan.notes.push_back("Removing nvngx_dlss.dll (NVIDIA's DLSS runtime, which EDVR placed).");
    } else if (s.ngx.kind != DllKind::Absent) {
        plan.notes.push_back(
            "The nvngx_dlss.dll beside the game is not the copy EDVR placed -- left alone.");
    }

    const DllInfo* xessFiles[] = {&s.xess, &s.xessLicense, &s.xessThirdParty};
    const std::string* xessOwned[] = {&s.state.xessSha, &s.state.xessLicenseSha,
                                     &s.state.xessThirdPartySha};
    const std::string* xessCarried[] = {&p.xessSha, &p.xessLicenseSha, &p.xessThirdPartySha};
    for (unsigned i=0; i<3; ++i) {
        const DllInfo& cur = *xessFiles[i];
        if (cur.kind == DllKind::Absent) continue;
        // EDVR's when the record or the tool's receipt lists it AND its bytes are the record's, the tool's or the payload's. A file with
        // matching bytes and no provenance is someone's to keep; a changed one is kept too.
        const bool provenance = !xessOwned[i]->empty() || toolListsPath(s, cur.path);
        if (!ownedFile(s, cur, provenance, *xessOwned[i], *xessCarried[i])) {
            const bool matches = sameSha(cur.sha256, *xessCarried[i]);
            plan.notes.push_back(matches ? say(leafOf(cur.path)) + " " + kMatchesNoProof
                                         : "Keeping a changed XeSS file: " + say(leafOf(cur.path)));
            continue;
        }
        remove(cur.path, "removes Intel XeSS-SR file EDVR placed", cur.sha256);
    }

    // ---- settings and record ------------------------------------------
    // The settings files of the scope being removed, and only those: the flat
    // profile's edvr-flat.ini and the VR profile's edvr.ini are separate people's
    // tuning, and an uninstall that took a file outside its scope destroyed
    // settings it had no business touching. edvr.ini for vr and both, edvr-flat.ini for flat and both.
    plan.settingsFile = flatEdition ? "edvr-flat.ini" : "edvr.ini";
    for (const bool flatFile : {false, true}) {
        if (flatFile ? scope == "vr" : scope == "flat") continue;
        const char* profile = flatFile ? "flat" : "vr";
        const std::wstring iniPath = settingsPathFor(s.game.dir, profile);
        const std::string iniName = toUtf8(settingsLeafFor(profile));
        const bool ownPresent = flatFile ? s.flatIniPresent : s.iniPresent;
        if (ownPresent) {
            if (o.removeSettings) {
                backup(iniPath, "your " + iniName);
                remove(iniPath, "removes " + iniName);
                plan.notes.push_back("Removing " + iniName + " (a copy is kept in the backup folder).");
            } else {
                plan.notes.push_back(
                    "Leaving " + iniName + " in place, so a reinstall finds your settings again.");
            }
        } else if (flatFile && flatEdition && s.iniPresent) {
            // A flat install that never got a file of its own -- one made before
            // edvr-flat.ini existed -- has been reading the shared edvr.ini, which the
            // VR profile uses too. It is not the flat edition's to remove.
            plan.notes.push_back(
                o.removeSettings
                    ? "This flat install has no edvr-flat.ini of its own: it was reading edvr.ini, "
                      "which the VR profile uses too, so edvr.ini was left in place."
                    : "Leaving edvr.ini in place, so a reinstall finds your settings again.");
        }
    }
    if (fileExists(statePath(s.game.dir))) remove(statePath(s.game.dir), "removes the install record");
    const std::wstring descriptorPath = joinPath(s.game.dir, kProfile);
    // Its bytes are one of EDVR's descriptor texts, or its hash is the one the record kept: a leftover from an install whose record is gone
    // says `scope = both` in exactly EDVR's bytes, and is EDVR's whatever the record says.
    if (s.descriptorPresent && ((!s.state.descriptorSha.empty() && s.descriptorSha == s.state.descriptorSha) ||
                                isCanonicalDescriptor(s.descriptorSha))) {
        remove(descriptorPath, "removes EDVR's profile descriptor", s.descriptorSha);
    } else if (s.descriptorPresent) {
        plan.notes.push_back("Keeping an unverified edvr_profile.ini; its ownership could not be proved.");
    }
    if (fileExists(baseIniPath(s.game.dir)))
        remove(baseIniPath(s.game.dir), "removes the kept default edvr.ini");
    if (fileExists(flatBaseIniPath(s.game.dir)))
        remove(flatBaseIniPath(s.game.dir), "removes the kept default edvr-flat.ini");

    if (!changed) {
        plan.nothingToDo = true;
        plan.notes.push_back("EDVR does not appear to be installed here.");
    } else {
        if (wantBackupDir) {
            Step mk;
            mk.action = Action::MakeDir;
            mk.to = plan.backupDir;
            mk.why = "keeps a copy of everything removed";
            plan.steps.push_back(mk);
        }
        plan.steps.insert(plan.steps.end(), body.begin(), body.end());
        plan.notes.push_back(
            "Logs in edvr_logs\\ and the backups in edvr_backup\\ are left where they are; delete "
            "them by hand if you want them gone.");
    }
    return plan;
}

std::string planSummary(const Plan& plan) {
    std::string out;
    for (const std::string& n : plan.notes) out += "  - " + n + "\r\n";
    if (!plan.problems.empty()) {
        out += "\r\n";
        for (const std::string& p : plan.problems) out += "  ! " + p + "\r\n";
    }
    return out;
}

}  // namespace edvr::installer
