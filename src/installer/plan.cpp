#include "plan.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <utility>

namespace edvr::installer {
namespace {

const wchar_t* kD3d11 = L"d3d11.dll";
const wchar_t* kIni = L"edvr.ini";
const wchar_t* kProfile = L"edvr_profile.ini";
const wchar_t* kOpenvr = L"openvr_api.dll";
const wchar_t* kOpenvrOrig = L"openvr_api_orig.dll";
const wchar_t* kNgx = L"nvngx_dlss.dll";   // NVIDIA's DLSS runtime, beside the game's executable

// Said when the game is running and it is not this folder's copy. Nothing is
// stopped, so it is a note rather than a problem -- but not silence either:
// being refused for a running game is what somebody expects here, and a report
// that says nothing about it looks like the check never ran.
const char* kRunningElsewhere =
    "Elite Dangerous is running, but from a different folder -- this install is not the one in "
    "use, so it can be changed.";

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

}  // namespace

std::string installedProfile(const Survey& s) {
    if (s.state.present) return s.state.profile;
    if (!s.descriptorPresent || s.descriptorSha.empty()) return std::string();
    for (const char* profile : {"flat", "vr"}) {
        const std::string text = std::string("[install]\r\nschema = 1\r\nprofile = ") + profile + "\r\n";
        if (s.descriptorSha == sha256Bytes(text.data(), text.size())) return profile;
    }
    return std::string();
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

    s.d3d11 = probeDll(joinPath(game.dir, kD3d11));
    for (const std::wstring& name : filesLike(game.dir, L"d3d11_*.dll")) {
        s.otherD3d11.push_back(probeDll(joinPath(game.dir, name)));
    }

    const std::wstring iniPath = joinPath(game.dir, kIni);
    s.iniPresent = fileExists(iniPath);
    if (s.iniPresent) s.iniText = readTextFile(iniPath);
    s.baseIniText = readTextFile(baseIniPath(game.dir));
    s.state = readState(game.dir);
    s.descriptorPresent = fileExists(joinPath(game.dir, kProfile));
    if (s.descriptorPresent) s.descriptorSha = sha256File(joinPath(game.dir, kProfile));
    s.ngx = probeDll(joinPath(game.dir, kNgx));
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
    const bool flat = p.profile == "flat";
    const std::string expectedDescriptor = "[install]\r\nschema = 1\r\nprofile = " + p.profile + "\r\n";
    if ((p.profile != "flat" && p.profile != "vr") || !p.haveD3d11 ||
        (flat ? !p.nativeGraphicsValid
              : (!p.haveOpenvr || !p.haveOpenxrLoader || !p.haveOpenxrLicense || !p.nativePairValid)) ||
        p.iniText.empty() || p.descriptorText != expectedDescriptor ||
        p.descriptorSha != sha256Bytes(expectedDescriptor.data(), expectedDescriptor.size())) {
        plan.blocked = true;
        plan.problems.push_back("This installer's profile payload is incomplete or invalid. Nothing will be installed.");
        return plan;
    }
    if (s.state.present && s.state.profile != "vr" && s.state.profile != "flat") {
        plan.blocked = true; plan.problems.push_back("The existing install record has an unknown profile. Repair it before changing editions."); return plan;
    }
    const std::string existingProfile = installedProfile(s);
    const bool conversion = !existingProfile.empty() && existingProfile != p.profile;
    if (conversion && !o.convertProfile) {
        plan.blocked = true;
        plan.problems.push_back("This folder has the " + existingProfile + " edition. Use --convert-profile or confirm edition conversion in the window.");
        return plan;
    }
    const std::string oldDescriptor = "[install]\r\nschema = 1\r\nprofile = " +
                                      existingProfile + "\r\n";
    if (s.descriptorPresent && (s.descriptorSha.empty() ||
        (!s.state.descriptorSha.empty() && s.descriptorSha != s.state.descriptorSha) ||
        (s.state.descriptorSha.empty() && s.descriptorSha != p.descriptorSha &&
         s.descriptorSha != sha256Bytes(oldDescriptor.data(), oldDescriptor.size())))) {
        plan.blocked = true; plan.problems.push_back("edvr_profile.ini differs from the recorded file; it was left untouched."); return plan;
    }
    if (flat && (conversion || (!s.state.present && s.openvrCurrent.nativeRuntimeExports))) {
        const bool stockRestored = s.openvrCurrent.kind == DllKind::OpenVrRuntime &&
            !s.state.openvrOrigSha.empty() && s.openvrCurrent.sha256 == s.state.openvrOrigSha;
        const bool ownedVr = s.openvrCurrent.kind == DllKind::Edvr &&
            !s.state.openvrSha.empty() && s.openvrCurrent.sha256 == s.state.openvrSha &&
            (s.openvrOrig.kind == DllKind::OpenVrRuntime || !s.openvrOrigInBackups.empty()) &&
            (s.openvrOrig.kind != DllKind::OpenVrRuntime || s.state.openvrOrigSha.empty() ||
             s.openvrOrig.sha256 == s.state.openvrOrigSha);
        if (!s.state.present || (!stockRestored && !ownedVr)) {
            plan.blocked = true; plan.problems.push_back("Cannot prove EDVR's VR runtime and the game's original; flat conversion left the VR files intact."); return plan;
        }
        for (const auto& asset : {std::make_pair(&s.openxrLoader, &s.state.openxrLoaderSha),
                                  std::make_pair(&s.openxrLicense, &s.state.openxrLicenseSha),
                                  std::make_pair(&s.nativeConfig, &s.state.nativeConfigSha)}) {
            if (asset.first->kind != DllKind::Absent &&
                (asset.second->empty() || asset.first->sha256 != *asset.second)) {
                plan.blocked = true; plan.problems.push_back("An OpenXR asset differs from EDVR's record; conversion cannot retire it safely."); return plan;
            }
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
        if (flat && item != &s.d3d11 && !conversion) continue;
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
    next.profile = p.profile;
    next.descriptorSha = p.descriptorSha;
    next.components = flat ? "graphics,profile,ini,ngx-optional" :
        "graphics,profile,ini,openvr,openxr-loader,openxr-license,openxr-config,ngx-optional";
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
    } else if (conversion) {
        // Only the files proved to belong to the recorded VR edition are retired.
        const std::wstring runtime = joinPath(nativeDir, kOpenvr);
        if (s.openvrCurrent.kind == DllKind::Edvr) {
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
        for (const DllInfo* asset : {&s.openxrLoader, &s.openxrLicense, &s.nativeConfig}) {
            if (asset->kind == DllKind::Absent) continue;
            backup(asset->path, "the EDVR OpenXR asset being retired");
            body.back().required = true; body.back().expectSha = asset->sha256;
            Step retire; retire.action = Action::Delete; retire.from = asset->path;
            retire.expectSha = asset->sha256; retire.why = "retires EDVR's OpenXR asset";
            body.push_back(retire);
        }
        next.openvrInstalled = false; next.openvrSha.clear();
        next.openvrOrigName.clear(); next.openvrOrigSha.clear();
        next.nativeRuntimeSha.clear(); next.openxrLoaderSha.clear();
        next.openxrLicenseSha.clear(); next.nativeConfigSha.clear();
        next.nativeOriginalName.clear(); next.nativeOriginalSha.clear();
        plan.notes.push_back("Converting VR to flat: the game's original VR library is in place and verified EDVR OpenXR assets are retired.");
    }
    if (flat) next.nativeGraphicsSha = p.d3d11Sha;

    const std::wstring descriptorPath = joinPath(s.game.dir, kProfile);
    if (!s.descriptorPresent || s.descriptorSha != p.descriptorSha || o.repair) {
        if (s.descriptorPresent) {
            backup(descriptorPath, "the previous EDVR profile descriptor");
            body.back().required = true; body.back().expectSha = s.descriptorSha;
        }
        writePayload("profile", descriptorPath, "declares the installed EDVR profile");
    }

    // ------------------------------------------------------------------
    // edvr.ini. Last, because the chain decision above forces a value into it.
    // ------------------------------------------------------------------
    if (p.iniText.empty()) {
        plan.problems.push_back("This installer carries no edvr.ini.");
    } else {
        const std::wstring iniPath = joinPath(s.game.dir, kIni);
        std::string merged;
        if (!s.iniPresent) {
            merged = mergeIni(p.iniText, std::string(), nullptr, forced, &plan.merge);
            plan.notes.push_back("Writing edvr.ini (every setting at its default).");
        } else if (!o.keepSettings) {
            merged = mergeIni(p.iniText, std::string(), nullptr, forced, &plan.merge);
            backup(iniPath, "your edvr.ini, before it is replaced");
            plan.notes.push_back(
                "Replacing edvr.ini with the shipped defaults. Your old file is in the backup "
                "folder.");
        } else {
            const std::string* base = s.baseIniText.empty() ? nullptr : &s.baseIniText;
            merged = mergeIni(p.iniText, s.iniText, base, forced, &plan.merge);
            if (merged != s.iniText) {
                backup(iniPath, "your edvr.ini, before it is updated");

                char line[256];
                sprintf_s(
                    line, "Updating edvr.ini: %zu of your settings kept, %zu new defaults adopted%s.",
                    plan.merge.kept.size(), plan.merge.adopted.size(),
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
        if (merged != s.iniText) {
            writeText(merged, iniPath, "writes edvr.ini");
            changed = true;
        } else {
            // Said once, and only here: an "updating edvr.ini" note followed by
            // "left alone" is two lines contradicting each other about the same
            // file.
            plan.notes.push_back("edvr.ini already says what it should -- left alone.");
        }
        next.iniSha = sha256Bytes(merged.data(), merged.size());
    }

    if (!changed && !o.repair) {
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

        Step base;
        base.action = Action::WriteText;
        base.text = p.iniText;
        base.to = baseIniPath(s.game.dir);
        base.why = "keeps this version's default edvr.ini, so the next update can tell your "
                   "changes from a changed default";
        plan.steps.push_back(base);

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

Plan planUninstall(const Survey& s, const Options& o) {
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
    if (s.gameRunningElsewhere) plan.notes.push_back(kRunningElsewhere);

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
            chain = safeSiblingName(fromUtf8(iniValue(s.iniText, "advanced.real_dll")), kD3d11);
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
                    "edvr.ini pointed at " + say(chain) +
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

    if(s.state.nativeInstalled) {
        auto retire=[&](const DllInfo& info,const std::string& owned) {
            if(info.kind==DllKind::Absent)return;
            if(owned.empty() || info.sha256!=owned) {
                plan.notes.push_back("Keeping a changed OpenXR file: "+say(leafOf(info.path)));return;
            }
            backup(info.path,"the native OpenXR file being removed");
            remove(info.path,"removes EDVR's OpenXR dependency",info.sha256);
        };
        retire(s.openxrLoader,s.state.openxrLoaderSha);
        retire(s.openxrLicense,s.state.openxrLicenseSha);
        retire(s.nativeConfig,s.state.nativeConfigSha);
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

    // ---- settings and record ------------------------------------------
    const std::wstring iniPath = joinPath(s.game.dir, kIni);
    if (s.iniPresent) {
        if (o.removeSettings) {
            backup(iniPath, "your edvr.ini");
            remove(iniPath, "removes edvr.ini");
            plan.notes.push_back("Removing edvr.ini (a copy is kept in the backup folder).");
        } else {
            plan.notes.push_back(
                "Leaving edvr.ini in place, so a reinstall finds your settings again.");
        }
    }
    if (fileExists(statePath(s.game.dir))) remove(statePath(s.game.dir), "removes the install record");
    const std::wstring descriptorPath = joinPath(s.game.dir, kProfile);
    if (s.descriptorPresent && !s.state.descriptorSha.empty() &&
        s.descriptorSha == s.state.descriptorSha) {
        remove(descriptorPath, "removes EDVR's profile descriptor", s.descriptorSha);
    } else if (s.descriptorPresent) {
        plan.notes.push_back("Keeping an unverified edvr_profile.ini; its ownership could not be proved.");
    }
    if (fileExists(baseIniPath(s.game.dir)))
        remove(baseIniPath(s.game.dir), "removes the kept default edvr.ini");

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
