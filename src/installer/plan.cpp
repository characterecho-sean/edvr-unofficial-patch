#include "plan.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace edvr::installer {
namespace {

const wchar_t* kD3d11 = L"d3d11.dll";
const wchar_t* kIni = L"edvr.ini";
const wchar_t* kOpenvr = L"openvr_api.dll";
const wchar_t* kOpenvrOrig = L"openvr_api_orig.dll";

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

Survey surveyTarget(const GameInstall& game) {
    Survey s;
    s.game = game;
    if (s.game.openvrDir.empty()) s.game.openvrDir = findOpenvrDir(game.dir);
    s.gameRunning = gameIsRunning();

    s.d3d11 = probeDll(joinPath(game.dir, kD3d11));
    for (const std::wstring& name : filesLike(game.dir, L"d3d11_*.dll")) {
        s.otherD3d11.push_back(probeDll(joinPath(game.dir, name)));
    }

    const std::wstring iniPath = joinPath(game.dir, kIni);
    s.iniPresent = fileExists(iniPath);
    if (s.iniPresent) s.iniText = readTextFile(iniPath);
    s.baseIniText = readTextFile(baseIniPath(game.dir));
    s.state = readState(game.dir);

    if (!s.game.openvrDir.empty()) {
        s.haveOpenvrDir = true;
        s.openvrCurrent = probeDll(joinPath(s.game.openvrDir, kOpenvr));

        // What the original was renamed to. openvr_api_orig.dll is what the
        // README says and what this installer writes, but advanced.
        // real_openvr_dll accepts any name, and somebody who installed by hand
        // may have used one. Looking only for the default name would read that
        // folder as "the original is missing" -- the one diagnosis that sends
        // people to verify their game files for no reason.
        //
        // It is a NAME, and this checks that, because the value ends up as a
        // rename destination. A value with a path in it -- "..\\..\\d3d11.dll",
        // by accident or otherwise -- would move the game's runtime out of the
        // folder and over another mod, and the report would call it "the game's
        // own copy is renamed openvr_api_orig.dll" while it happened. Anything
        // that is not a plain filename is refused back to the default.
        s.openvrOrigName = safeSiblingName(fromUtf8(iniValue(s.iniText,
                                                             "advanced.real_openvr_dll")),
                                           kOpenvr);
        if (s.openvrOrigName.empty()) s.openvrOrigName = kOpenvrOrig;
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
    if (s.gameRunning) {
        plan.blocked = true;
        plan.problems.push_back(
            "Elite Dangerous is running. Close it first -- Windows will not let anything replace a "
            "file the game has open, and a half-replaced install is worse than none.");
        return plan;
    }
    if (!p.haveD3d11 && !p.haveOpenvr) {
        plan.blocked = true;
        plan.problems.push_back("This installer carries no EDVR files. Nothing to install.");
        return plan;
    }

    std::vector<Step> body;
    std::vector<std::pair<std::string, std::string>> forced;  // ini keys the installer must set
    bool wantBackupDir = false;
    bool changed = false;

    InstallState next = s.state;
    next.present = true;
    next.edvrVersion = p.version;
    next.installedUtc = o.nowUtc;
    next.openvrDir = relativeOpenvr(s.game.dir, s.game.openvrDir);

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
    // openvr_api.dll -- the transition flash fix and Explorer Cam. This one
    // REPLACES a file the game owns, so the original has to survive the swap.
    // ------------------------------------------------------------------
    if (p.haveOpenvr) {
        if (!s.haveOpenvrDir) {
            plan.problems.push_back(
                "Could not find the game's own openvr_api.dll (looked in Openvr\\win64, Openvr, "
                "and the game folder). The transition flash fix and Explorer Cam need it and were "
                "not installed; every other fix is unaffected.");
        } else {
            const std::wstring dir = s.game.openvrDir;
            const std::wstring dst = joinPath(dir, kOpenvr);
            // The name this folder uses for the original, which is the default
            // unless a hand install chose another and said so in edvr.ini.
            std::wstring origName = safeSiblingName(s.openvrOrigName, kOpenvr);
            if (origName.empty()) origName = kOpenvrOrig;
            const std::wstring origPath = joinPath(dir, origName);
            const DllInfo& cur = s.openvrCurrent;
            const DllInfo& orig = s.openvrOrig;

            const bool curIsOurs = cur.kind == DllKind::Edvr;
            const bool curIsRuntime = cur.kind == DllKind::OpenVrRuntime;
            const bool origIsRuntime = orig.kind == DllKind::OpenVrRuntime;

            if (cur.kind == DllKind::Unreadable || orig.kind == DllKind::Unreadable) {
                plan.problems.push_back(
                    "The openvr_api.dll files in " + say(dir) +
                    " cannot be read -- something has them open. That half was not installed.");
            } else if (curIsRuntime) {
                // The game's own runtime is in place: either a first install,
                // or the game put its file back over ours (a game update, or a
                // file verification in the launcher).
                if (origIsRuntime) {
                    backup(origPath, "the previously renamed original, now superseded");
                    plan.notes.push_back(
                        "The game has restored its own openvr_api.dll since the last install. The "
                        "current one is kept as the original and EDVR is reinstalled in front of "
                        "it.");
                }
                if (!cur.is64) {
                    plan.problems.push_back(
                        "The openvr_api.dll being renamed aside is 32-bit, which the 64-bit game "
                        "cannot have been loading. Check what put it there before starting the "
                        "game.");
                }
                backup(dst, "the game's own openvr_api.dll, before it is renamed");
                rename(dst, origPath, "keeps the game's original as " + say(origName),
                       cur.sha256);
                writePayload("openvr", dst, "installs EDVR's openvr_api.dll");
                next.openvrOrigSha = cur.sha256;
                next.openvrOrigName = origName;
                next.openvrInstalled = true;
                next.openvrSha = p.openvrSha;
                if (!origIsRuntime) {
                    plan.notes.push_back(
                        "Installing openvr_api.dll in " + say(dir) +
                        ": the game's own copy is renamed openvr_api_orig.dll and EDVR passes "
                        "every call through to it.");
                }
                if (modNameOf(cur) == L"OpenComposite") {
                    plan.notes.push_back(
                        "That original is OpenComposite, not SteamVR. EDVR chains through it "
                        "normally; if it raises a dialog about an interface it does not "
                        "implement, advanced.suppress_interfaces in edvr.ini is the setting for "
                        "that.");
                }
            } else if (curIsOurs && origIsRuntime) {
                if (cur.sha256 == p.openvrSha && !o.repair) {
                    plan.notes.push_back("openvr_api.dll is already this build -- left alone.");
                } else {
                    backup(dst, "the EDVR openvr_api.dll being replaced");
                    writePayload("openvr", dst,
                                 o.repair ? "reinstalls EDVR's openvr_api.dll"
                                          : "updates EDVR's openvr_api.dll");
                    plan.notes.push_back(o.repair
                                             ? "Reinstalling openvr_api.dll (transition flash fix, "
                                               "Explorer Cam)."
                                             : "Updating openvr_api.dll (transition flash fix, "
                                               "Explorer Cam).");
                }
                next.openvrOrigSha = orig.sha256;
                next.openvrOrigName = origName;
                next.openvrInstalled = true;
                next.openvrSha = p.openvrSha;
            } else if (curIsOurs) {
                // Ours is installed and the original is NOT there. EDVR has
                // nothing to forward to, so VR does not start at all -- the
                // failure the README warns about in bold.
                if (!s.openvrOrigInBackups.empty()) {
                    copyInto(s.openvrOrigInBackups.front(), origPath,
                             "restores the game's original runtime from an EDVR backup");
                    backup(dst, "the EDVR openvr_api.dll being replaced");
                    writePayload("openvr", dst, "reinstalls EDVR's openvr_api.dll");
                    // Name the backup by its folder -- the timestamp is the
                    // only thing that tells one from another.
                    const std::wstring backupFile = s.openvrOrigInBackups.front();
                    const std::wstring stamp =
                        leafOf(backupFile.substr(0, backupFile.find_last_of(L"\\/")));
                    plan.notes.push_back(
                        "The game's original openvr_api.dll was missing, which stops VR from "
                        "starting at all. It has been restored from the backup this installer made "
                        "at " + say(stamp) + ", and EDVR reinstalled in front of it.");
                    next.openvrOrigName = origName;
                    next.openvrInstalled = true;
                    next.openvrSha = p.openvrSha;
                } else {
                    plan.problems.push_back(
                        "EDVR's openvr_api.dll is installed in " + say(dir) +
                        " but the game's original (openvr_api_orig.dll) is not there, and no "
                        "backup of it was found. VR cannot start: EDVR has nothing to pass calls "
                        "through to. Use your launcher to verify or repair the game files -- that "
                        "restores openvr_api.dll -- then run this installer again. Nothing in that "
                        "folder was changed.");
                }
            } else if (cur.kind == DllKind::Absent && origIsRuntime) {
                writePayload("openvr", dst, "reinstalls EDVR's openvr_api.dll");
                plan.notes.push_back(
                    "openvr_api.dll had gone missing while the game's original was still safely "
                    "renamed. Reinstalled.");
                next.openvrOrigSha = orig.sha256;
                next.openvrOrigName = origName;
                next.openvrInstalled = true;
                next.openvrSha = p.openvrSha;
            } else if (cur.kind == DllKind::Absent) {
                plan.problems.push_back(
                    "There is no openvr_api.dll in " + say(dir) +
                    " at all, and no renamed original. Verify the game files in your launcher "
                    "before installing the VR half.");
            } else {
                plan.problems.push_back(
                    "The openvr_api.dll in " + say(dir) +
                    " is not an OpenVR runtime and not EDVR's -- it was left alone rather than "
                    "renamed. Send the log or ask on Discord before going further.");
            }
        }
    }

    if (!p.haveOpenvr) {
        // Not a choice the user made -- a build made without the game's own
        // openvr_api.dll to generate an export table from. package.bat refuses
        // to ship one, so this can only be a developer build, and it says so
        // rather than quietly installing half a patch.
        plan.problems.push_back(
            "This installer was built without EDVR's openvr_api.dll, so the transition flash fix "
            "and Explorer Cam are not in it. That is a development build; a release always carries "
            "both files.");
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
    plan.backupDir =
        joinPath(backupRootPath(s.game.dir), o.backupStamp.empty() ? L"backup" : o.backupStamp);

    if (s.gameRunning) {
        plan.blocked = true;
        plan.problems.push_back("Elite Dangerous is running. Close it first.");
        return plan;
    }

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
