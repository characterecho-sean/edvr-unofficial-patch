#include "app.h"

#include <shellapi.h>

#include <cstdio>

#include "apply.h"
#include "payload.h"

namespace edvr::installer {
namespace {

void writeOut(const std::string& utf8) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        const std::wstring w = fromUtf8(utf8);
        DWORD written = 0;
        WriteConsoleW(h, w.c_str(), static_cast<DWORD>(w.size()), &written, nullptr);
    } else {
        DWORD written = 0;
        WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

std::string bullets(const char* title, const std::vector<std::string>& items) {
    if (items.empty()) return std::string();
    std::string out = std::string(title) + "\r\n";
    for (const std::string& i : items) out += "    " + i + "\r\n";
    return out;
}

}  // namespace

std::string usageText() {
    return "EDVR installer\r\n"
           "\r\n"
           "  edvr-installer.exe                        open the window\r\n"
           "  edvr-installer.exe --install [--dir D]    install or update\r\n"
           "  edvr-installer.exe --repair  [--dir D]    put a clobbered install back together\r\n"
           "  edvr-installer.exe --uninstall [--dir D]  remove EDVR, restoring what it replaced\r\n"
           "\r\n"
           "  --dir <path>         the folder holding EliteDangerous64.exe. Without it, the\r\n"
           "                       installer finds your installs itself and uses the only one.\r\n"
           "  --dry-run            say what would happen; touch nothing.\r\n"
           "  --no-d3d11           leave the fixes half alone.\r\n"
           "  --no-openvr          leave the transition flash / Explorer Cam half alone.\r\n"
           "  --replace-settings   overwrite edvr.ini instead of keeping your values.\r\n"
           "  --remove-settings    with --uninstall, delete edvr.ini too.\r\n"
           "  --help               this.\r\n";
}

AppArgs parseArgs(int argc, wchar_t** argv) {
    AppArgs a;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&](std::wstring* out) {
            if (i + 1 < argc) {
                *out = argv[++i];
                return true;
            }
            a.badArg = true;
            a.badArgText = arg + L" needs a value";
            return false;
        };
        if (arg == L"--install")
            a.action = AppArgs::Act::Install;
        else if (arg == L"--repair")
            a.action = AppArgs::Act::Repair;
        else if (arg == L"--uninstall")
            a.action = AppArgs::Act::Uninstall;
        else if (arg == L"--dir")
            next(&a.dir);
        else if (arg.rfind(L"--dir=", 0) == 0)
            a.dir = arg.substr(6);
        else if (arg == L"--dry-run")
            a.dryRun = true;
        else if (arg == L"--no-d3d11")
            a.d3d11 = false;
        else if (arg == L"--no-openvr")
            a.openvr = false;
        else if (arg == L"--replace-settings")
            a.keepSettings = false;
        else if (arg == L"--remove-settings")
            a.removeSettings = true;
        else if (arg == L"--autorun")
            a.autorun = true;
        else if (arg == L"--help" || arg == L"-h" || arg == L"/?")
            a.help = true;
        else {
            a.badArg = true;
            a.badArgText = L"unknown argument: " + arg;
        }
    }
    return a;
}

Options optionsFor(const AppArgs& args, bool repair) {
    Options o;
    o.installD3d11 = args.d3d11;
    o.installOpenvr = args.openvr;
    o.keepSettings = args.keepSettings;
    o.removeSettings = args.removeSettings;
    o.repair = repair;
    o.backupStamp = timestampName();
    o.nowUtc = utcNow();
    return o;
}

std::string statusReport(const Survey& s, const PayloadInfo& payload) {
    std::string out;
    out += "Folder:  " + toUtf8(s.game.dir) + "\r\n";
    out += "Found by: " + toUtf8(s.game.source);
    if (!s.game.product.empty()) out += "   (" + toUtf8(s.game.product) + ")";
    out += "\r\n\r\n";

    out += "d3d11.dll         " + toUtf8(describeDll(s.d3d11)) + "\r\n";
    if (s.haveOpenvrDir) {
        // Relative to the folder printed above: the absolute path is the same
        // one again with a suffix, and it wraps across two lines of the pane
        // for no gain.
        std::wstring where = s.game.openvrDir;
        if (where.size() > s.game.dir.size() &&
            _wcsnicmp(where.c_str(), s.game.dir.c_str(), s.game.dir.size()) == 0) {
            where = where.substr(s.game.dir.size() + 1);
        }
        out += "openvr_api.dll    " + toUtf8(describeDll(s.openvrCurrent)) + "\r\n";
        out += "  the original    " + toUtf8(describeDll(s.openvrOrig)) + "\r\n";
        out += "  in              " + toUtf8(where) + "\\\r\n";
    } else {
        out += "openvr_api.dll    the game's own copy was not found under this folder\r\n";
    }
    out += std::string("edvr.ini          ") + (s.iniPresent ? "present" : "not present") + "\r\n";

    if (s.state.present) {
        out += "\r\nLast installed by this installer: " + s.state.edvrVersion;
        if (!s.state.installedUtc.empty()) out += " on " + s.state.installedUtc;
        out += "\r\n";
        if (!s.state.chainTarget.empty()) {
            out += "Passing calls through to " + toUtf8(s.state.chainTarget);
            if (!s.state.chainMod.empty()) out += " (" + toUtf8(s.state.chainMod) + ")";
            out += "\r\n";
        }
    }
    if (!payload.version.empty()) out += "\r\nThis installer carries EDVR " + payload.version + ".\r\n";
    if (s.gameRunning)
        out += "\r\n!  Elite Dangerous is running. Close it before installing anything.\r\n";
    return out;
}

std::string planReport(const Plan& plan) {
    std::string out = planSummary(plan);
    out += bullets("\r\n  Settings kept from your edvr.ini:", plan.merge.kept);
    out += bullets("  New defaults adopted (you had not changed these):", plan.merge.adopted);
    out += bullets("  Set by the installer:", plan.merge.forced);
    out += bullets("  Carried over, no longer used by this version:", plan.merge.retired);
    out += bullets("  Carried over, not an EDVR setting:", plan.merge.carried);
    out += bullets("  Lines you had removed, left commented out:", plan.merge.removed);
    return out;
}

bool needsElevationFor(const Survey& s) {
    if (!canWriteInto(s.game.dir)) return true;
    if (s.haveOpenvrDir && !canWriteInto(s.game.openvrDir)) return true;
    return false;
}

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool relaunchElevated(const AppArgs& args) {
    wchar_t self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return false;

    std::wstring cmd;
    switch (args.action) {
        case AppArgs::Act::Install: cmd += L"--install "; break;
        case AppArgs::Act::Repair: cmd += L"--repair "; break;
        case AppArgs::Act::Uninstall: cmd += L"--uninstall "; break;
        case AppArgs::Act::None: break;
    }
    if (!args.dir.empty()) cmd += L"--dir \"" + args.dir + L"\" ";
    if (!args.d3d11) cmd += L"--no-d3d11 ";
    if (!args.openvr) cmd += L"--no-openvr ";
    if (!args.keepSettings) cmd += L"--replace-settings ";
    if (args.removeSettings) cmd += L"--remove-settings ";
    if (args.dryRun) cmd += L"--dry-run ";
    cmd += L"--autorun";

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = self;
    info.lpParameters = cmd.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return false;  // declined at the UAC prompt, or failed
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

int runConsole(const AppArgs& args) {
    if (args.help || args.badArg) {
        if (args.badArg) writeOut(toUtf8(args.badArgText) + "\r\n\r\n");
        writeOut(usageText());
        return args.badArg ? 2 : 0;
    }

    GameInstall game;
    if (!args.dir.empty()) {
        if (!describeDir(args.dir, L"Folder you chose", &game)) {
            writeOut("No EliteDangerous64.exe under " + toUtf8(args.dir) + "\r\n");
            return 2;
        }
    } else {
        const std::vector<GameInstall> found = findInstalls();
        if (found.empty()) {
            writeOut("No Elite Dangerous install found. Pass --dir with the folder that holds "
                     "EliteDangerous64.exe.\r\n");
            return 2;
        }
        if (found.size() > 1) {
            std::string list;
            for (const GameInstall& g : found)
                list += "    " + toUtf8(g.source) + ": " + toUtf8(g.dir) + "\r\n";
            writeOut("More than one install found; name one with --dir:\r\n" + list);
            return 2;
        }
        game = found.front();
    }

    const Survey survey = surveyTarget(game);
    const PayloadInfo& payload = payloadInfo();
    writeOut(statusReport(survey, payload) + "\r\n");

    if (args.action == AppArgs::Act::None) return 0;

    const Options options = optionsFor(args, args.action == AppArgs::Act::Repair);
    const Plan plan = args.action == AppArgs::Act::Uninstall ? planUninstall(survey, options)
                                                             : planInstall(survey, options, payload);
    writeOut(planReport(plan));

    if (plan.blocked) return 1;
    if (plan.nothingToDo || args.dryRun) return 0;

    if (needsElevationFor(survey) && !isElevated()) {
        writeOut("\r\nThis folder needs administrator rights. Run the same command from an "
                 "elevated prompt.\r\n");
        return 3;
    }

    const ApplyResult result = applyPlan(plan, payloadItem);
    for (const std::string& line : result.done) writeOut("  " + line + "\r\n");
    if (!result.ok) {
        writeOut("\r\n" + result.error + "\r\n");
        if (result.rolledBack) writeOut("Everything this run had changed was put back.\r\n");
        return 1;
    }
    writeOut("\r\nDone.\r\n");
    return 0;
}

}  // namespace edvr::installer
