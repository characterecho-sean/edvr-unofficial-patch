// The window.
//
// One screen per job, no wizard: which install, which halves, and a pane that
// says exactly what is about to happen -- because the thing being modified is
// somebody's game folder, and an installer that does its work behind a progress
// bar is asking for trust it has not earned. Every button shows the plan and
// waits for a yes before a file moves.
//
// The look is drawn rather than inherited; see ui.h for why and how. What is
// still native here is deliberate: the install picker is a real combo box and
// the report is a real edit control, because a dropdown and a scrolling text
// view are the two things a hand-rolled widget gets subtly wrong -- keyboard
// selection, mouse wheel, text selection for copying into a bug report.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>

#include <string>
#include <vector>

#include "app.h"
#include "apply.h"
#include "payload.h"
#include "settings.h"
#include "settings_view.h"
#include "ui.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace edvr::installer {
namespace {

enum : int {
    kIdCombo = 1001,
    kIdBrowse = 1002,
    kIdChkKeep = 1012,
    kIdInstall = 1020,
    kIdRepair = 1021,
    kIdUninstall = 1022,
    kIdClose = 1023,
    kIdReport = 1030,
    kIdKofi = 1040,
    kIdDiscord = 1041,
    kIdTabInstall = 1050,
    kIdTabSettings = 1051,
    kIdSearch = 1060,
    kIdSettingsList = 1061,
};

const UINT kMsgFirstScan = WM_APP + 1;

const wchar_t* kTipUrl = L"https://ko-fi.com/seancharacterecho";
const wchar_t* kDiscordUrl = L"https://discord.gg/ynkdf6Gdua";
const wchar_t* kTipText = L"EDVR is free. If it saved you an evening, you can";

// The client area and every control position below are in 96-dpi units, scaled
// once at layout time.
const int kClientWidth = 720;
const int kClientHeight = 652;
const int kMargin = 24;
const int kCardPad = 20;

enum class Screen { Install, Settings };

enum class Tone { Good, Warn, Bad, Muted };

struct StatusRow {
    std::wstring label;
    std::wstring value;
    Tone         tone = Tone::Muted;
};

struct Place {
    HWND hwnd = nullptr;
    int  x = 0, y = 0, w = 0, h = 0;
    Screen screen = Screen::Install;
    bool bothScreens = false;
};

struct Gui {
    HWND window = nullptr;
    HWND combo = nullptr;
    HWND report = nullptr;
    HWND chkKeep = nullptr;
    HWND install = nullptr;
    HWND repair = nullptr;
    HWND uninstall = nullptr;
    HWND tabInstall = nullptr;
    HWND tabSettings = nullptr;
    HWND tip = nullptr;
    HWND discord = nullptr;
    HWND search = nullptr;
    HWND settingsList = nullptr;
    SettingsModel settings;
    UINT dpi = 96;

    std::vector<Place>      places;
    std::vector<StatusRow>  status;
    std::wstring            statusPath;   // the full folder, under the picker

    std::vector<GameInstall> installs;
    Survey                   survey;
    bool                     haveSurvey = false;
    Screen                   screen = Screen::Install;
    AppArgs                  args;
};

Gui g;

int dp(int v) { return ui::dp(v, g.dpi); }

void setText(HWND control, const std::string& utf8) {
    SetWindowTextW(control, fromUtf8(utf8).c_str());
}

// An edit control with WS_VSCROLL shows its scrollbar whether or not there is
// anything to scroll, and an empty pane with a full-height scrollbar down the
// side of it looks like a text box from 2003. Shown only when the text really
// is taller than the pane.
void updateReportScrollbar() {
    if (!g.report) return;
    RECT client{};
    GetClientRect(g.report, &client);

    HDC dc = GetDC(g.report);
    HFONT previous = static_cast<HFONT>(SelectObject(dc, ui::fonts().body));
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, previous);
    ReleaseDC(g.report, dc);

    const int lineHeight = metrics.tmHeight > 0 ? metrics.tmHeight : 1;
    const int visible = (client.bottom - client.top) / lineHeight;
    const int lines = static_cast<int>(SendMessageW(g.report, EM_GETLINECOUNT, 0, 0));
    ShowScrollBar(g.report, SB_VERT, lines > visible ? TRUE : FALSE);
}

void setReport(const std::string& utf8) {
    setText(g.report, utf8);
    updateReportScrollbar();
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------

void place(HWND hwnd, int x, int y, int w, int h, Screen screen, bool bothScreens = false) {
    Place p;
    p.hwnd = hwnd;
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    p.screen = screen;
    p.bothScreens = bothScreens;
    g.places.push_back(p);
}

// Everything moved and shown or hidden for the current screen and scale.
//
// The first version of this took the window's size from GetDpiForSystem and the
// control positions from GetDpiForWindow. On a 150% display those are different
// numbers, so the controls were laid out half as big again as the window meant
// to hold them, and the right-hand third was off the edge.
void applyLayout() {
    for (const Place& p : g.places) {
        const bool visible = p.bothScreens || p.screen == g.screen;
        ShowWindow(p.hwnd, visible ? SW_SHOW : SW_HIDE);
        if (visible) MoveWindow(p.hwnd, dp(p.x), dp(p.y), dp(p.w), dp(p.h), TRUE);
    }

    // The tip link finishes a sentence that is painted, not a control, so where
    // it goes depends on how wide that sentence renders -- which changes with
    // the font, the scale and the language of the machine it is running on.
    if (g.tip) {
        HDC dc = GetDC(g.window);
        const int prefix = ui::textWidth(dc, kTipText, ui::fonts().caption);
        ReleaseDC(g.window, dc);
        MoveWindow(g.tip, dp(kMargin) + prefix + dp(5), dp(621), dp(76), dp(20), TRUE);
    }
    InvalidateRect(g.window, nullptr, TRUE);
}

void sizeWindowToLayout() {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(g.window, GWL_STYLE));
    RECT wanted{0, 0, dp(kClientWidth), dp(kClientHeight)};
    AdjustWindowRectExForDpi(&wanted, style, FALSE, 0, g.dpi);
    SetWindowPos(g.window, nullptr, 0, 0, wanted.right - wanted.left, wanted.bottom - wanted.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// what the folder looks like, as rows rather than as a paragraph
// ---------------------------------------------------------------------------

Tone toneForOurs(DllKind kind) {
    switch (kind) {
        case DllKind::Edvr: return Tone::Good;
        case DllKind::Absent: return Tone::Muted;
        case DllKind::Unreadable: return Tone::Bad;
        default: return Tone::Warn;
    }
}

void rebuildStatus() {
    g.status.clear();
    if (!g.haveSurvey) {
        g.statusPath.clear();
        return;
    }
    const Survey& s = g.survey;
    g.statusPath = s.game.dir;

    StatusRow fixes;
    fixes.label = L"The fixes";
    fixes.tone = toneForOurs(s.d3d11.kind);
    if (s.d3d11.kind == DllKind::Edvr) {
        fixes.value = L"EDVR installed as d3d11.dll";
        if (!s.state.edvrVersion.empty()) fixes.value += L" \x00b7 " + fromUtf8(s.state.edvrVersion);
    } else if (s.d3d11.kind == DllKind::Absent) {
        fixes.value = L"not installed";
    } else {
        fixes.value = describeDll(s.d3d11) + L" holds the d3d11.dll name";
    }
    g.status.push_back(fixes);

    StatusRow vr;
    vr.label = L"Flash fix, Explorer Cam";
    if (!s.haveOpenvrDir) {
        vr.tone = Tone::Warn;
        vr.value = L"no Openvr folder found under this install";
    } else if (s.openvrCurrent.kind == DllKind::Edvr) {
        vr.tone = Tone::Good;
        vr.value = L"EDVR installed as openvr_api.dll";
    } else if (s.openvrCurrent.kind == DllKind::OpenVrRuntime) {
        vr.tone = Tone::Muted;
        vr.value = L"not installed \x00b7 the game's own runtime is in place";
    } else {
        vr.tone = toneForOurs(s.openvrCurrent.kind);
        vr.value = describeDll(s.openvrCurrent);
    }
    g.status.push_back(vr);

    if (s.haveOpenvrDir) {
        StatusRow orig;
        orig.label = L"The game's runtime";
        if (s.openvrOrig.kind == DllKind::OpenVrRuntime) {
            orig.tone = Tone::Good;
            orig.value = L"safe, renamed openvr_api_orig.dll";
        } else if (s.openvrCurrent.kind == DllKind::OpenVrRuntime) {
            orig.tone = Tone::Good;
            orig.value = L"in place, nothing renamed yet";
        } else if (s.openvrCurrent.kind == DllKind::Edvr) {
            orig.tone = Tone::Bad;
            orig.value = L"missing \x2014 VR cannot start; use Repair";
        } else {
            orig.tone = Tone::Muted;
            orig.value = L"not found";
        }
        g.status.push_back(orig);
    }

    StatusRow settings;
    settings.label = L"Your settings";
    settings.tone = s.iniPresent ? Tone::Good : Tone::Muted;
    settings.value = s.iniPresent ? L"edvr.ini is here and will be kept" : L"no edvr.ini yet";
    g.status.push_back(settings);

    if (!s.state.chainTarget.empty()) {
        StatusRow chain;
        chain.label = L"Alongside";
        chain.tone = Tone::Good;
        chain.value = (s.state.chainMod.empty() ? std::wstring(L"another mod")
                                                : s.state.chainMod) +
                      L", chained through " + s.state.chainTarget;
        g.status.push_back(chain);
    }

    if (s.gameRunning) {
        StatusRow running;
        running.label = L"Elite Dangerous";
        running.tone = Tone::Bad;
        running.value = L"running \x2014 close it before installing anything";
        g.status.push_back(running);
    }
}

COLORREF colourFor(Tone tone) {
    const ui::Theme& t = ui::theme();
    switch (tone) {
        case Tone::Good: return t.good;
        case Tone::Warn: return t.warn;
        case Tone::Bad: return t.bad;
        default: return t.muted;
    }
}

// ---------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------

void paintInstallScreen(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();

    RECT card{dp(kMargin), dp(84), dp(kClientWidth - kMargin), dp(304)};
    ui::paintCard(dc, card, g.dpi);

    RECT heading{dp(kMargin + kCardPad), dp(100), dp(kClientWidth - kMargin - kCardPad), dp(122)};
    ui::drawText(dc, L"Where Elite Dangerous is", heading, f.heading, t.text,
                 DT_LEFT | DT_SINGLELINE);

    if (!g.statusPath.empty()) {
        RECT path{dp(kMargin + kCardPad), dp(163), dp(kClientWidth - kMargin - kCardPad), dp(181)};
        ui::drawText(dc, g.statusPath, path, f.caption, t.subtext,
                     DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }

    int y = 196;
    for (const StatusRow& row : g.status) {
        const int centre = dp(y + 10);
        ui::fillCircle(dc, dp(kMargin + kCardPad + 5), centre, dp(4), colourFor(row.tone));

        RECT label{dp(kMargin + kCardPad + 20), dp(y), dp(kMargin + kCardPad + 180), dp(y + 20)};
        ui::drawText(dc, row.label, label, f.body, t.subtext,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT value{dp(kMargin + kCardPad + 190), dp(y), dp(kClientWidth - kMargin - kCardPad),
                   dp(y + 20)};
        ui::drawText(dc, row.value, value, f.body, t.text,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += 22;
    }

    RECT actions{dp(kMargin), dp(316), dp(kClientWidth - kMargin), dp(438)};
    ui::paintCard(dc, actions, g.dpi);

    RECT reassure{dp(kMargin + kCardPad), dp(408), dp(kClientWidth - kMargin - kCardPad), dp(428)};
    ui::drawText(dc,
                 L"Nothing is changed until you confirm it, and every file replaced is copied "
                 L"into edvr_backup\\ first.",
                 reassure, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT report{dp(kMargin), dp(450), dp(kClientWidth - kMargin), dp(612)};
    ui::paintCard(dc, report, g.dpi);
}

void paintSettingsScreen(HDC dc) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    RECT card{dp(kMargin), dp(84), dp(kClientWidth - kMargin), dp(612)};
    ui::paintCard(dc, card, g.dpi);

    RECT heading{dp(kMargin + kCardPad), dp(100), dp(kClientWidth - kMargin - kCardPad), dp(122)};
    ui::drawText(dc, L"Settings", heading, f.heading, t.text, DT_LEFT | DT_SINGLELINE);

    RECT note{dp(kMargin + kCardPad), dp(124), dp(kClientWidth - kMargin - kCardPad), dp(142)};
    const std::wstring where =
        g.settings.loaded()
            ? L"Written straight into edvr.ini. Most settings are live: the game picks a change "
              L"up within a second."
            : L"Pick an install on the Install tab first.";
    ui::drawText(dc, where, note, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    // The search box is a real edit control with its 3D border taken off, so
    // the frame around it is drawn here to match everything else.
    RECT search{dp(kMargin + kCardPad) - dp(6), dp(150), dp(kMargin + kCardPad) + dp(326),
                dp(182)};
    ui::fillRounded(dc, search, dp(6), t.control);
    ui::strokeRounded(dc, search, dp(6), t.controlBorder);
}

void paintWindow(HWND window) {
    PAINTSTRUCT ps;
    HDC screenDc = BeginPaint(window, &ps);

    RECT client;
    GetClientRect(window, &client);

    // Drawn into a bitmap and blitted: cards, text and dots painted straight
    // onto the window flicker visibly every time the report pane changes.
    HDC dc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, client.right, client.bottom);
    HBITMAP previous = static_cast<HBITMAP>(SelectObject(dc, bitmap));

    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    ui::fillRect(dc, client, t.windowBg);

    RECT title{dp(kMargin), dp(18), dp(400), dp(50)};
    ui::drawText(dc, L"EDVR", title, f.title, t.text, DT_LEFT | DT_SINGLELINE);

    const std::wstring subtitle =
        fromUtf8(payloadInfo().version) + L"  \x00b7  unofficial patch for Elite Dangerous in VR";
    RECT sub{dp(kMargin), dp(52), dp(kClientWidth - kMargin), dp(70)};
    ui::drawText(dc, subtitle, sub, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (g.screen == Screen::Install)
        paintInstallScreen(dc);
    else
        paintSettingsScreen(dc);

    RECT tip{dp(kMargin), dp(622), dp(kClientWidth - kMargin), dp(642)};
    ui::drawText(dc, kTipText, tip, f.caption, t.subtext, DT_LEFT | DT_SINGLELINE);

    BitBlt(screenDc, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(window, &ps);
}

// ---------------------------------------------------------------------------
// behaviour
// ---------------------------------------------------------------------------

void showSurvey() {
    rebuildStatus();
    if (!g.haveSurvey) {
        EnableWindow(g.install, FALSE);
        EnableWindow(g.repair, FALSE);
        EnableWindow(g.uninstall, FALSE);
        InvalidateRect(g.window, nullptr, TRUE);
        return;
    }

    const bool running = g.survey.gameRunning;
    EnableWindow(g.install, !running);
    EnableWindow(g.repair, !running);
    EnableWindow(g.uninstall, !running);

    const bool installed = g.survey.d3d11.kind == DllKind::Edvr ||
                           g.survey.openvrCurrent.kind == DllKind::Edvr || g.survey.state.present;
    SetWindowTextW(g.install, installed ? L"Update" : L"Install");
    InvalidateRect(g.window, nullptr, TRUE);
}

void selectInstall(size_t index) {
    if (index >= g.installs.size()) {
        g.haveSurvey = false;
        showSurvey();
        return;
    }
    g.survey = surveyTarget(g.installs[index]);
    g.haveSurvey = true;
    g.settings.load(g.survey.game.dir);
    settingsListSetModel(g.settingsList, &g.settings);
    showSurvey();
}

void addInstall(const GameInstall& game) {
    for (size_t i = 0; i < g.installs.size(); ++i) {
        if (_wcsicmp(g.installs[i].dir.c_str(), game.dir.c_str()) == 0) {
            SendMessageW(g.combo, CB_SETCURSEL, i, 0);
            selectInstall(i);
            return;
        }
    }
    g.installs.push_back(game);
    const std::wstring label = game.source + L"  \x00b7  " + game.dir;
    SendMessageW(g.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    const size_t index = g.installs.size() - 1;
    SendMessageW(g.combo, CB_SETCURSEL, index, 0);
    selectInstall(index);
}

void scanForInstalls() {
    SendMessageW(g.combo, CB_RESETCONTENT, 0, 0);
    g.installs.clear();
    for (const GameInstall& game : findInstalls()) {
        g.installs.push_back(game);
        const std::wstring label = game.source + L"  \x00b7  " + game.dir;
        SendMessageW(g.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }

    if (g.installs.empty()) {
        g.haveSurvey = false;
        setReport(
                "No Elite Dangerous install was found.\r\n\r\n"
                "Press Browse and point at the folder that holds EliteDangerous64.exe. For a "
                "Frontier launcher install that is usually a folder ending in\r\n"
                "    Products\\elite-dangerous-odyssey-64");
        showSurvey();
        return;
    }

    // With more than one install -- and three storefronts on one machine is not
    // unusual -- the one that already has EDVR in it is the one being updated,
    // and a better guess than whichever store answered first.
    size_t pick = 0;
    for (size_t i = 0; i < g.installs.size(); ++i) {
        const Survey candidate = surveyTarget(g.installs[i]);
        if (candidate.state.present || candidate.d3d11.kind == DllKind::Edvr ||
            candidate.openvrCurrent.kind == DllKind::Edvr) {
            pick = i;
            break;
        }
    }
    SendMessageW(g.combo, CB_SETCURSEL, pick, 0);
    selectInstall(pick);
}

void browseForFolder() {
    // The modern picker, which shows the path and lets it be pasted in --
    // SHBrowseForFolder's tree is not a reasonable way to find a folder eight
    // levels down a Steam library.
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Pick the folder that holds EliteDangerous64.exe");

    std::wstring picked;
    if (SUCCEEDED(dialog->Show(g.window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                picked = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (picked.empty()) return;

    GameInstall game;
    if (!describeDir(picked, L"Folder you chose", &game)) {
        MessageBoxW(g.window,
                    L"There is no EliteDangerous64.exe in that folder, or in a Products folder "
                    L"under it.\n\nPick the folder that holds EliteDangerous64.exe itself.",
                    L"EDVR installer", MB_OK | MB_ICONWARNING);
        return;
    }
    addInstall(game);
}

std::wstring confirmText(const Plan& plan, const wchar_t* verb) {
    std::wstring text = std::wstring(verb) + L"\n\n";
    for (const std::string& note : plan.notes) text += L"\x2022 " + fromUtf8(note) + L"\n";
    if (!plan.problems.empty()) {
        text += L"\nWorth knowing:\n";
        for (const std::string& p : plan.problems) text += L"! " + fromUtf8(p) + L"\n";
    }
    text += L"\nA copy of every file replaced goes into edvr_backup\\.\n\nGo ahead?";
    return text;
}

void runAction(AppArgs::Act action) {
    if (!g.haveSurvey) return;

    // The folder can change between the survey and the button -- the game gets
    // started, another installer runs. Ask the disk again rather than acting on
    // a picture that may be minutes old.
    g.survey = surveyTarget(g.survey.game);

    AppArgs args = g.args;
    args.action = action;
    args.dir = g.survey.game.dir;
    args.keepSettings = ui::checkboxChecked(g.chkKeep);
    args.removeSettings = false;

    const PayloadInfo& payload = payloadInfo();
    const Options options = optionsFor(args, action == AppArgs::Act::Repair);
    const Plan plan = action == AppArgs::Act::Uninstall
                          ? planUninstall(g.survey, options)
                          : planInstall(g.survey, options, payload);

    setReport(planReport(plan));

    if (plan.blocked || plan.nothingToDo) {
        MessageBoxW(g.window, fromUtf8(planSummary(plan)).c_str(), L"EDVR installer",
                    MB_OK | (plan.blocked ? MB_ICONWARNING : MB_ICONINFORMATION));
        showSurvey();
        return;
    }

    const wchar_t* verb = action == AppArgs::Act::Uninstall ? L"About to remove EDVR:"
                          : action == AppArgs::Act::Repair  ? L"About to repair this install:"
                                                            : L"About to install EDVR:";
    if (!args.autorun) {
        const int answer = MessageBoxW(g.window, confirmText(plan, verb).c_str(), L"EDVR installer",
                                       MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) return;
    }

    if (needsElevationFor(g.survey) && !isElevated()) {
        const int answer = MessageBoxW(
            g.window,
            L"This game folder can only be written to by an administrator (it is usually under "
            L"Program Files).\n\nRestart the installer with administrator rights and carry on?",
            L"EDVR installer", MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) return;
        AppArgs elevated = args;
        elevated.autorun = true;
        if (relaunchElevated(elevated)) {
            PostMessageW(g.window, WM_CLOSE, 0, 0);
        } else {
            MessageBoxW(g.window,
                        L"Windows would not start the installer with administrator rights, so "
                        L"nothing was changed.",
                        L"EDVR installer", MB_OK | MB_ICONWARNING);
        }
        return;
    }

    const ApplyResult result = applyPlan(plan, payloadItem);

    std::string text = planReport(plan);
    text += "\r\n----\r\n";
    for (const std::string& line : result.done) text += "  " + line + "\r\n";
    if (result.ok) {
        text += "\r\nDone. Start Elite Dangerous; EDVR writes what it managed to install into "
                "edvr_logs\\ next to the game.\r\n";
    } else {
        text += "\r\n" + result.error + "\r\n";
        if (result.rolledBack)
            text += "Everything this run had changed was put back, so the folder is as it was.\r\n";
    }
    setReport(text);

    MessageBoxW(g.window,
                result.ok ? L"Done.\n\nStart Elite Dangerous. What EDVR managed to install is "
                            L"written into edvr_logs\\ next to the game."
                          : fromUtf8(result.error).c_str(),
                L"EDVR installer", MB_OK | (result.ok ? MB_ICONINFORMATION : MB_ICONERROR));

    g.survey = surveyTarget(g.survey.game);
    showSurvey();
}

void showScreen(Screen screen) {
    g.screen = screen;
    ui::setButtonStyle(g.tabInstall,
                       screen == Screen::Install ? ui::ButtonStyle::TabActive : ui::ButtonStyle::Tab);
    ui::setButtonStyle(g.tabSettings, screen == Screen::Settings ? ui::ButtonStyle::TabActive
                                                                 : ui::ButtonStyle::Tab);
    applyLayout();
}

void createControls(HWND window) {
    g.dpi = GetDpiForWindow(window);
    ui::buildFonts(g.dpi);
    const ui::Fonts& f = ui::fonts();

    g.tabInstall = ui::makeButton(window, L"Install", kIdTabInstall, ui::ButtonStyle::TabActive,
                                  f.body);
    g.tabSettings = ui::makeButton(window, L"Settings", kIdTabSettings, ui::ButtonStyle::Tab,
                                   f.body);
    place(g.tabInstall, 528, 22, 84, 30, Screen::Install, true);
    place(g.tabSettings, 616, 22, 92, 30, Screen::Install, true);

    g.combo = CreateWindowExW(0, L"COMBOBOX", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                              0, 0, 10, 10, window,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCombo)), nullptr,
                              nullptr);
    SendMessageW(g.combo, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    place(g.combo, kMargin + kCardPad, 128, 512, 30, Screen::Install);

    HWND browse = ui::makeButton(window, L"Browse...", kIdBrowse, ui::ButtonStyle::Secondary,
                                 f.body);
    place(browse, 568, 128, 108, 30, Screen::Install);

    g.chkKeep = ui::makeCheckbox(window, L"Keep the settings I have changed", kIdChkKeep, true,
                                 f.body);
    place(g.chkKeep, kMargin + kCardPad, 334, 420, 24, Screen::Install);

    g.install = ui::makeButton(window, L"Install", kIdInstall, ui::ButtonStyle::Primary, f.body);
    g.repair = ui::makeButton(window, L"Repair", kIdRepair, ui::ButtonStyle::Secondary, f.body);
    g.uninstall = ui::makeButton(window, L"Uninstall", kIdUninstall, ui::ButtonStyle::Secondary,
                                 f.body);
    HWND close = ui::makeButton(window, L"Close", kIdClose, ui::ButtonStyle::Secondary, f.body);
    place(g.install, kMargin + kCardPad, 366, 124, 34, Screen::Install);
    place(g.repair, 172, 366, 104, 34, Screen::Install);
    place(g.uninstall, 284, 366, 104, 34, Screen::Install);
    place(close, 572, 366, 104, 34, Screen::Install, true);

    g.search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0,
                               10, 10, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSearch)), nullptr,
                               nullptr);
    SendMessageW(g.search, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    SendMessageW(g.search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search settings"));
    place(g.search, kMargin + kCardPad, 152, 320, 28, Screen::Settings);

    g.settingsList = createSettingsList(window, kIdSettingsList, g.dpi);
    place(g.settingsList, kMargin + 2, 192, kClientWidth - 2 * kMargin - 4, 416, Screen::Settings);

    g.report = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY, 0,
                               0, 10, 10, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdReport)), nullptr,
                               nullptr);
    SendMessageW(g.report, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    place(g.report, kMargin + kCardPad - 4, 464, 636, 134, Screen::Install);

    g.tip = ui::makeButton(window, L"leave a tip", kIdKofi, ui::ButtonStyle::Link, f.caption);
    place(g.tip, kMargin, 621, 76, 20, Screen::Install, true);  // x fixed up in applyLayout

    g.discord = ui::makeButton(window, L"Ask on Discord", kIdDiscord, ui::ButtonStyle::Link,
                               f.caption);
    place(g.discord, kClientWidth - kMargin - 110, 621, 110, 20, Screen::Install, true);

    // Dark mode does not reach the native controls on its own. These two theme
    // names are what File Explorer uses for exactly this, and on a light theme
    // (or an older Windows) they simply do nothing.
    if (ui::theme().dark) {
        SetWindowTheme(g.combo, L"DarkMode_CFD", nullptr);
        SetWindowTheme(g.report, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(g.search, L"DarkMode_CFD", nullptr);
        SetWindowTheme(g.settingsList, L"DarkMode_Explorer", nullptr);
    }

    // Deliberately not an explanation of what Install, Repair and Uninstall do.
    // A first reader of this window already knows, and the pane is where the
    // plan goes -- filling it with a lecture beforehand made the one thing it
    // is for look like more of the same text. (Field feedback, on the first
    // build anybody but its author had seen.)
    setReport("What each button is about to do appears here, before anything is changed.");

    showScreen(Screen::Install);
    sizeWindowToLayout();
}

void rescale(HWND window, UINT dpi, const RECT* suggested) {
    g.dpi = dpi;
    ui::buildFonts(dpi);
    const ui::Fonts& f = ui::fonts();
    for (const Place& p : g.places) {
        SendMessageW(p.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    }
    SendMessageW(g.report, WM_SETFONT, reinterpret_cast<WPARAM>(f.body), TRUE);
    settingsListRescale(g.settingsList, dpi);
    applyLayout();
    if (suggested) {
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    sizeWindowToLayout();
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            g.window = window;
            ui::applyWindowChrome(window);
            createControls(window);
            PostMessageW(window, kMsgFirstScan, 0, 0);
            return 0;

        case WM_PAINT:
            paintWindow(window);
            return 0;

        case WM_ERASEBKGND:
            return 1;  // the paint above covers every pixel

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (ui::drawOwnerDrawn(item, g.dpi)) return TRUE;
            break;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int code = HIWORD(wparam);
            if (id == kIdSearch && code == EN_CHANGE) {
                wchar_t buffer[256]{};
                GetWindowTextW(g.search, buffer, 256);
                settingsListSetFilter(g.settingsList, buffer);
                return 0;
            }
            if (id == kIdCombo && code == CBN_SELCHANGE) {
                const LRESULT index = SendMessageW(g.combo, CB_GETCURSEL, 0, 0);
                if (index != CB_ERR) selectInstall(static_cast<size_t>(index));
                return 0;
            }
            if (code != BN_CLICKED) return 0;
            switch (id) {
                case kIdBrowse: browseForFolder(); return 0;
                case kIdInstall: runAction(AppArgs::Act::Install); return 0;
                case kIdRepair: runAction(AppArgs::Act::Repair); return 0;
                case kIdUninstall: runAction(AppArgs::Act::Uninstall); return 0;
                case kIdClose: PostMessageW(window, WM_CLOSE, 0, 0); return 0;
                case kIdTabInstall: showScreen(Screen::Install); return 0;
                case kIdTabSettings: showScreen(Screen::Settings); return 0;
                case kIdKofi:
                    ShellExecuteW(window, L"open", kTipUrl, nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case kIdDiscord:
                    ShellExecuteW(window, L"open", kDiscordUrl, nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case kIdChkKeep:
                    ui::toggleCheckbox(reinterpret_cast<HWND>(lparam));
                    return 0;
                default: return 0;
            }
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX: {
            // The report pane sits inside a card and has to be the same colour
            // as it, or it reads as a sunken 1990s text box. The search box sits
            // inside a frame painted behind it, and takes that frame's fill.
            HDC dc = reinterpret_cast<HDC>(wparam);
            const ui::Theme& t = ui::theme();
            const bool isSearch = reinterpret_cast<HWND>(lparam) == g.search;
            const COLORREF fill = isSearch ? t.control : t.cardBg;
            SetTextColor(dc, t.text);
            SetBkColor(dc, fill);
            static HBRUSH brushes[2] = {nullptr, nullptr};
            static COLORREF colours[2] = {0, 0};
            const int slot = isSearch ? 1 : 0;
            if (!brushes[slot] || colours[slot] != fill) {
                if (brushes[slot]) DeleteObject(brushes[slot]);
                brushes[slot] = CreateSolidBrush(fill);
                colours[slot] = fill;
            }
            return reinterpret_cast<LRESULT>(brushes[slot]);
        }

        case WM_SETTINGCHANGE:
            // The system switched between light and dark while we were open.
            if (lparam && wcscmp(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0) {
                ui::refreshTheme();
                ui::applyWindowChrome(window);
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;

        case WM_DPICHANGED:
            rescale(window, HIWORD(wparam), reinterpret_cast<const RECT*>(lparam));
            return 0;

        case kMsgFirstScan: {
            // After the window is up, so the first paint is not held behind a
            // registry and filesystem sweep.
            if (!g.args.dir.empty()) {
                GameInstall game;
                if (describeDir(g.args.dir, L"Folder you chose", &game))
                    addInstall(game);
                else
                    scanForInstalls();
            } else {
                scanForInstalls();
            }
            if (g.args.autorun && g.args.action != AppArgs::Act::None && g.haveSurvey) {
                runAction(g.args.action);
            }
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            ui::releaseFonts();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int runGui(HINSTANCE instance, const AppArgs& args) {
    g.args = args;

    const INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ui::startup();

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = nullptr;  // painted, not erased
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpszClassName = L"EdvrInstallerWindow";
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    cls.hIconSm = cls.hIcon;
    RegisterClassExW(&cls);

    HWND window = CreateWindowExW(0, cls.lpszClassName, L"EDVR installer",
                                  (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) |
                                      WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, kClientWidth, kClientHeight,
                                  nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    ui::shutdown();
    CoUninitialize();
    return 0;
}

}  // namespace edvr::installer
