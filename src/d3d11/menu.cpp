#include "menu.h"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/hotkey.h"
#include "../common/iniedit.h"
#include "../common/log.h"
#include "../common/proxy.h"
#include "../common/timing.h"
#include "input_gate.h"
#include "menu_panel.h"
#include "menu_schema.h"
#include "sharpen_pass.h"
#include "temporal_pass.h"

#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unknown"
#endif

namespace edvr {

// The generated row table: every [fix] row tagged `menu`, and every
// developer-tier key the code reads (tools/gen_settings_schema.py).
#include "menu_schema.inc"

namespace {

constexpr int kRowDefCount = static_cast<int>(sizeof(kMenuRows) / sizeof(kMenuRows[0]));

// How many rows a page shows at once; the rest scroll.
constexpr int kVisibleRows = 9;
// The fade, in and out.
constexpr uint64_t kFadeMs = 150;
// Hold-to-repeat: the first repeat, then the cadence.
constexpr uint64_t kRepeatFirstMs = 400;
constexpr uint64_t kRepeatMs = 83;
// The draw the gate follows: a panel whose export has not run within this
// long is not being seen, and the keyboard goes back to the game.
constexpr uint64_t kDrawnFreshMs = 250;
// ...and a menu that has not been drawn at all for this long after opening
// closes itself and says why.
constexpr uint64_t kNotDrawnCloseMs = 1500;
// A toast's life, and the pitch it sits below the look direction.
constexpr uint64_t kToastMs = 2500;
constexpr float    kToastPitchDeg = -12.0f;
// The Status page's live values are refreshed at this cadence while shown.
constexpr uint64_t kStatusRefreshMs = 500;
// R twice within this long resets a row to its shipped value.
constexpr uint64_t kResetArmMs = 3000;

enum class EntryKind { Setting, Action, Heading };

struct Entry {
    EntryKind   kind = EntryKind::Setting;
    int         def = -1;        // index into kMenuRows for Setting
    int         action = -1;     // index into g_actions for Action
    const char* text = "";       // for Heading
};

struct Page {
    const char*        name = "";
    std::vector<Entry> entries;
    int                highlight = 0;   // an entry index; never a heading when one exists
    int                scroll = 0;
    bool               status = false;
};

struct Action {
    std::string  label;
    std::string  hint;
    MenuActionFn fn = nullptr;
    void*        user = nullptr;
};

struct RowState {
    std::string value;     // the effective value, as Config reads it
    std::string snapshot;  // restart rows: the value at launch
    bool        pending = false;
    bool        auditNoted = false;
};

struct KeyRepeat {
    int      vk = 0;
    bool     down = false;
    uint64_t nextMs = 0;
};

struct State {
    bool configured = false;
    Hotkey summon;
    std::string summonName;
    bool privateWanted = true;
    bool developer = false;
    bool toasts = true;
    float distance = 1.4f;
    float curve = 0.2f;
    float textDeg = 1.1f;
    float widthDeg = 26.0f;
    int   idleSeconds = 20;
    int   aimMode = 2;   // 0 head, 1 keys, 2 both

    bool  open = false;
    float alpha = 0.0f;
    uint64_t openedMs = 0;
    uint64_t lastInputMs = 0;
    uint64_t lastTickMs = 0;
    float anchor[12] = {};
    bool  anchorValid = false;

    std::vector<Page> pages;
    int  page = 0;
    bool contentDirty = true;
    bool developerBuilt = false;

    KeyRepeat keys[12];
    bool shiftHeld = false;

    bool aimParked = false;
    int  aimSameCount = 0;
    int  aimLast = -1;

    uint32_t lastDrawn = 0;
    uint64_t lastDrawnMs = 0;
    bool     notDrawnNoted = false;
    bool     noConsumerNoted = false;
    bool     noPoseNoted = false;
    bool     pollRequest = false;

    // Toasts: queued texts, and the one on screen.
    std::vector<std::string> toastQueue;
    bool     toastUp = false;
    float    toastAlpha = 0.0f;
    uint64_t toastUntilMs = 0;
    std::string toastText;

    // Restart bookkeeping.
    bool snapshotTaken = false;
    std::string lastWrite;
    std::string menuWroteDotted;   // the change the menu itself made, so its reload does not toast

    // The reset-to-shipped arm.
    int      resetArmedEntry = -1;
    uint64_t resetArmedMs = 0;

    // Frame timing for the Status page.
    int64_t  lastQpc = 0;
    double   frameMsEma = 0.0;
    uint32_t longFrames = 0;
    uint32_t longFramesShown = 0;
    uint64_t longWindowMs = 0;
    uint64_t statusRefreshMs = 0;
    uint32_t markers = 0;
};

State g_s;
std::vector<Action> g_actions;
RowState g_rows[kRowDefCount > 0 ? kRowDefCount : 1];

FaultBudget g_budget("menuTick", 6);

// ---------------------------------------------------------------------------
// Values

std::string dottedOf(const MenuRowDef& d) {
    return std::string(d.section) + "." + d.key;
}

std::string rowValue(const MenuRowDef& d) {
    return Config::get().getString(dottedOf(d).c_str(), d.shipped);
}

bool boolOf(const std::string& v, bool def) {
    std::string s = v;
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return def;
}

// on/off in the file's own dialect, so a toggle written from the menu looks
// like the line the user (or the ini) already had.
std::string boolWord(bool on, const std::string& current) {
    std::string s = current;
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (s == "1" || s == "0") return on ? "1" : "0";
    if (s == "true" || s == "false") return on ? "true" : "false";
    if (s == "yes" || s == "no") return on ? "yes" : "no";
    return on ? "on" : "off";
}

std::string formatNumber(double v, int precision) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%.*f", precision > 0 ? precision : 0, v);
    if (precision > 0) {
        // Trim trailing zeros past the first decimal: 0.30 -> 0.3, 1.00 -> 1.0.
        std::string s(buf);
        const size_t dot = s.find('.');
        if (dot != std::string::npos) {
            while (s.size() > dot + 2 && s.back() == '0') s.pop_back();
        }
        return s;
    }
    return std::string(buf);
}

// A step a person would choose: the range in about twenty steps, rounded to
// 1, 2 or 5 times a power of ten. A number with no bounds steps by one, or a
// tenth for a decimal.
double stepOf(const MenuRowDef& d) {
    const bool haveBounds = d.lo[0] && d.hi[0];
    if (!haveBounds) return d.precision > 0 ? 0.1 : 1.0;
    const double range = atof(d.hi) - atof(d.lo);
    if (!(range > 0.0)) return d.precision > 0 ? 0.1 : 1.0;
    const double raw = range / 20.0;
    const double mag = pow(10.0, floor(log10(raw)));
    double best = mag;
    for (double m : {1.0, 2.0, 5.0, 10.0}) {
        if (fabs(m * mag - raw) < fabs(best - raw)) best = m * mag;
    }
    if (d.precision == 0 && best < 1.0) best = 1.0;
    return best;
}

struct ChoiceItem {
    std::string value;
    std::string label;
};

std::vector<ChoiceItem> choicesOf(const MenuRowDef& d) {
    std::vector<ChoiceItem> out;
    std::string packed(d.choices);
    size_t pos = 0;
    while (pos <= packed.size()) {
        size_t bar = packed.find('|', pos);
        if (bar == std::string::npos) bar = packed.size();
        std::string item = packed.substr(pos, bar - pos);
        if (!item.empty()) {
            ChoiceItem c;
            const size_t eq = item.find('=');
            if (eq != std::string::npos) {
                c.value = item.substr(0, eq);
                c.label = item.substr(eq + 1);
            } else {
                c.value = item;
                c.label = item;
            }
            out.push_back(c);
        }
        pos = bar + 1;
    }
    return out;
}

std::string displayValue(const MenuRowDef& d, const std::string& v) {
    switch (d.kind) {
        case MenuKind::Toggle:
            return boolOf(v, boolOf(d.shipped, false)) ? "on" : "off";
        case MenuKind::Number: {
            const double x = atof(v.c_str());
            if (d.percent) return formatNumber(x * 100.0, 0) + "%";
            return formatNumber(x, d.precision);
        }
        case MenuKind::Choice: {
            for (const ChoiceItem& c : choicesOf(d)) {
                if (_stricmp(c.value.c_str(), v.c_str()) == 0) return c.label;
            }
            return v.empty() ? "(empty)" : v;
        }
        case MenuKind::Text:
        default:
            if (v.empty()) return "(empty)";
            return v.size() > 28 ? v.substr(0, 27) + "~" : v;
    }
}

// ---------------------------------------------------------------------------
// The ini write (docs/settings-menu.md, "Persistence")

bool readWhole(const std::wstring& path, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE || size > (4u << 20)) {
        CloseHandle(f);
        return false;
    }
    out->resize(size);
    DWORD got = 0;
    const BOOL ok = size == 0 || ReadFile(f, &(*out)[0], size, &got, nullptr);
    CloseHandle(f);
    return ok && got == size;
}

bool writeWhole(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = text.empty() ||
                    WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    CloseHandle(f);
    return ok && wrote == text.size();
}

std::wstring stampName() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t buf[40];
    _snwprintf_s(buf, _TRUNCATE, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond);
    return buf;
}

bool dirExistsW(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring lowerW(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// The installer's mirror folder for this install (src/installer/mirror.h:
// %LOCALAPPDATA%\EDVR\<leaf>-<store>), found rather than invented where it
// can be: the store slug the installer would derive from the path, else the
// one folder that already carries this leaf, else made fresh.
std::wstring mirrorDir() {
    wchar_t env[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH)) return std::wstring();
    const std::wstring root = std::wstring(env) + L"\\EDVR";
    const std::wstring exeDir = executableDirectory();
    const size_t slash = exeDir.find_last_of(L"\\/");
    const std::wstring leaf = slash == std::wstring::npos ? exeDir : exeDir.substr(slash + 1);
    const std::wstring low = lowerW(exeDir);
    std::wstring slug;
    if (low.find(L"\\steamapps\\") != std::wstring::npos) slug = L"steam";
    else if (low.find(L"\\epic games\\") != std::wstring::npos) slug = L"epic";
    else if (low.find(L"frontier_developments\\products") != std::wstring::npos)
        slug = L"frontier-launcher";
    if (!slug.empty()) {
        const std::wstring exact = root + L"\\" + leaf + L"-" + slug;
        if (dirExistsW(exact)) return exact;
    }
    // One folder carrying this leaf already: the installer made it.
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\" + leaf + L"-*").c_str(), &fd);
    std::wstring found;
    int count = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                found = root + L"\\" + fd.cFileName;
                ++count;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (count == 1) return found;
    if (!dirExistsW(root)) CreateDirectoryW(root.c_str(), nullptr);
    const std::wstring made = root + L"\\" + leaf + L"-" + (slug.empty() ? L"folder-you-chose" : slug);
    if (!dirExistsW(made)) CreateDirectoryW(made.c_str(), nullptr);
    return dirExistsW(made) ? made : std::wstring();
}

bool g_backedUp = false;
bool g_mirrorNoted = false;

bool menuIniWrite(const std::string& dotted, const std::string& value, std::string* err) {
    const std::wstring path = Config::get().path();
    std::string source;
    // Re-read before every write, the settings window's 2026-08-28 lesson:
    // the file on disk is the source, never a copy cached when the panel
    // opened.
    if (!readWhole(path, &source) || source.empty()) {
        *err = "edvr.ini could not be read";
        return false;
    }
    if (!g_backedUp) {
        g_backedUp = true;   // tried once; a failure must not block editing
        const std::wstring root = executableDirectory() + L"\\edvr_backup";
        if (!dirExistsW(root)) CreateDirectoryW(root.c_str(), nullptr);
        const std::wstring dir = root + L"\\menu-" + stampName();
        if (CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
            CopyFileW(path.c_str(), (dir + L"\\edvr.ini").c_str(), FALSE);
        }
    }
    MergeReport report;
    const std::string updated = mergeIni(source, source, &source, {{dotted, value}}, &report);
    // Atomic: the whole new file beside the old one, then one replace, so
    // config.cpp's size check never meets half a save.
    const std::wstring tmp = path + L".menu-tmp";
    if (!writeWhole(tmp, updated)) {
        *err = "the temporary file could not be written beside edvr.ini";
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        *err = "edvr.ini could not be replaced (read-only, or held by an editor?)";
        return false;
    }
    // The mirror of last resort, refreshed so an update that wipes the
    // folder cannot lose an evening's tuning.
    const std::wstring mdir = mirrorDir();
    if (!mdir.empty()) {
        CopyFileW(path.c_str(), (mdir + L"\\edvr.ini").c_str(), FALSE);
        if (!g_mirrorNoted) {
            g_mirrorNoted = true;
            char utf8[MAX_PATH * 3] = {};
            WideCharToMultiByte(CP_UTF8, 0, mdir.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
            Log::get().note("menu: each write is mirrored to %s, the installer's copy outside "
                            "the game folder.",
                            utf8);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Pages

void addSettingRows(Page& p, MenuTier tier, const char* page, bool grouped) {
    const char* lastGroup = nullptr;
    for (int i = 0; i < kRowDefCount; ++i) {
        const MenuRowDef& d = kMenuRows[i];
        if (d.tier != tier) continue;
        if (page && _stricmp(d.page, page) != 0) continue;
        if (grouped && d.group[0] && (!lastGroup || strcmp(lastGroup, d.group) != 0)) {
            Entry h;
            h.kind = EntryKind::Heading;
            h.text = d.group;
            p.entries.push_back(h);
            lastGroup = d.group;
        }
        Entry e;
        e.kind = EntryKind::Setting;
        e.def = i;
        p.entries.push_back(e);
    }
}

void firstSelectable(Page& p) {
    p.highlight = 0;
    for (size_t i = 0; i < p.entries.size(); ++i) {
        if (p.entries[i].kind != EntryKind::Heading) {
            p.highlight = static_cast<int>(i);
            return;
        }
    }
}

void buildPages() {
    State& s = g_s;
    const int keepPage = s.page;
    s.pages.clear();
    {
        Page p;
        p.name = "Performance";
        addSettingRows(p, MenuTier::Fix, "performance", false);
        firstSelectable(p);
        s.pages.push_back(p);
    }
    {
        Page p;
        p.name = "Fixes";
        addSettingRows(p, MenuTier::Fix, "fixes", true);
        firstSelectable(p);
        s.pages.push_back(p);
    }
    {
        Page p;
        p.name = "Status";
        p.status = true;
        s.pages.push_back(p);
    }
    if (s.developer) {
        {
            Page p;
            p.name = "Advanced";
            addSettingRows(p, MenuTier::Advanced, nullptr, true);
            firstSelectable(p);
            s.pages.push_back(p);
        }
        {
            Page p;
            p.name = "Experimental";
            addSettingRows(p, MenuTier::Experimental, nullptr, true);
            firstSelectable(p);
            s.pages.push_back(p);
        }
        {
            Page p;
            p.name = "Instruments";
            for (size_t i = 0; i < g_actions.size(); ++i) {
                Entry e;
                e.kind = EntryKind::Action;
                e.action = static_cast<int>(i);
                p.entries.push_back(e);
            }
            firstSelectable(p);
            s.pages.push_back(p);
        }
    }
    s.developerBuilt = s.developer;
    s.page = keepPage < static_cast<int>(s.pages.size()) ? keepPage : 0;
    s.contentDirty = true;
}

void refreshRowValues() {
    for (int i = 0; i < kRowDefCount; ++i) g_rows[i].value = rowValue(kMenuRows[i]);
}

void takeSnapshot() {
    for (int i = 0; i < kRowDefCount; ++i) {
        const MenuRowDef& d = kMenuRows[i];
        if (d.applies == 2) g_rows[i].snapshot = g_rows[i].value;
        g_rows[i].pending = false;
    }
    g_s.snapshotTaken = true;
}

int pendingRestartCount() {
    int n = 0;
    for (int i = 0; i < kRowDefCount; ++i) n += g_rows[i].pending ? 1 : 0;
    return n;
}

// ---------------------------------------------------------------------------
// The Status page

void statusLine(MenuContent& c, const char* left, const char* right) {
    if (c.lineCount >= kMenuMaxLines) return;
    MenuLine& l = c.lines[c.lineCount++];
    strncpy(l.left, left, sizeof(l.left) - 1);
    l.left[sizeof(l.left) - 1] = 0;
    strncpy(l.right, right, sizeof(l.right) - 1);
    l.right[sizeof(l.right) - 1] = 0;
    l.style = kMenuInfo;
    l.badge = kBadgeNone;
}

void buildStatus(MenuContent& c) {
    char buf[200];
    snprintf(buf, sizeof(buf), "%s / game build %s", EDVR_VERSION_STRING, gameBuildVersion().c_str());
    statusLine(c, "EDVR", buf);
    const uint32_t rk = runtimeKind();
    statusLine(c, "Runtime",
               rk == 1 ? "SteamVR (Valve's own)"
               : rk == 2 ? "OpenComposite"
               : glitchConsumerPresent() ? "not identified" : "no openvr half hooked");
    uint32_t ew = 0, eh = 0;
    float outer = 0.0f, inner = 0.0f;
    if (eyeTextureSize(&ew, &eh) && eyeTangents(&outer, &inner)) {
        snprintf(buf, sizeof(buf), "%ux%u, tangents %.2f/%.2f", ew, eh,
                 static_cast<double>(outer), static_cast<double>(inner));
    } else {
        snprintf(buf, sizeof(buf), "not published yet");
    }
    statusLine(c, "Eye texture", buf);
    {
        const CullGuardState g = decodeCullGuardState(cullGuardStatePacked());
        if (g.stage == 0) {
            snprintf(buf, sizeof(buf), "off");
        } else {
            snprintf(buf, sizeof(buf), "stage %u, +%.1f%% / +%.1f%%", g.stage, g.hPerMille / 10.0,
                     g.vPerMille / 10.0);
        }
        statusLine(c, "Terrain guard", buf);
    }
    {
        const std::string mode = Config::get().getString("fix.temporal_aa", "off");
        uint32_t n = 0, resets = 0;
        double avg = 0.0, mx = 0.0, rej = 0.0, clip = 0.0;
        if (temporalPassDlaaTotals(&n, &avg, &mx, &resets) && n) {
            snprintf(buf, sizeof(buf), "%s, NVIDIA %.2f ms/eye", mode.c_str(), avg);
        } else if (temporalPassTotals(&n, &avg, &mx, &rej, &clip) && n) {
            snprintf(buf, sizeof(buf), "%s, %.2f ms/eye", mode.c_str(), avg);
        } else {
            snprintf(buf, sizeof(buf), "%s", mode.c_str());
        }
        statusLine(c, "Temporal AA", buf);
    }
    {
        uint32_t n = 0;
        double avg = 0.0, mx = 0.0;
        const std::string v = Config::get().getString("fix.render_sharpness", "0.0");
        if (sharpenPassTotals(&n, &avg, &mx) && n) {
            snprintf(buf, sizeof(buf), "%s, %.2f ms/eye", v.c_str(), avg);
        } else {
            snprintf(buf, sizeof(buf), "%s", atof(v.c_str()) > 0.0 ? v.c_str() : "off");
        }
        statusLine(c, "Sharpening", buf);
    }
    {
        const State& s = g_s;
        if (s.frameMsEma > 0.0) {
            snprintf(buf, sizeof(buf), "%.1f ms (%.0f fps), %u long in 30 s", s.frameMsEma,
                     1000.0 / s.frameMsEma, s.longFramesShown);
        } else {
            snprintf(buf, sizeof(buf), "measuring");
        }
        statusLine(c, "Frame time", buf);
    }
    {
        char line[240];
        inputGateStatusLine(line, sizeof(line));
        statusLine(c, "Keyboard", line);
    }
    {
        int w = 0, h = 0;
        double ms = 0.0;
        if (menuPanelStats(&w, &h, &ms)) {
            snprintf(buf, sizeof(buf), "%dx%d bitmap, %.1f ms to draw", w, h, ms);
        } else {
            snprintf(buf, sizeof(buf), "no raster yet");
        }
        statusLine(c, "This panel", buf);
    }
    {
        const int n = pendingRestartCount();
        if (n == 0) {
            snprintf(buf, sizeof(buf), "none");
        } else {
            std::string names;
            for (int i = 0; i < kRowDefCount && names.size() < 50; ++i) {
                if (!g_rows[i].pending) continue;
                if (!names.empty()) names += ", ";
                names += kMenuRows[i].key;
            }
            snprintf(buf, sizeof(buf), "%d: %s", n, names.c_str());
        }
        statusLine(c, "Waiting for a restart", buf);
    }
    statusLine(c, "Last write", g_s.lastWrite.empty() ? "none this session" : g_s.lastWrite.c_str());
}

// ---------------------------------------------------------------------------
// Content

void buildContent(MenuContent& c) {
    State& s = g_s;
    memset(&c, 0, sizeof(c));
    c.tabCount = static_cast<int>(s.pages.size());
    if (c.tabCount > kMenuMaxTabs) c.tabCount = kMenuMaxTabs;
    for (int i = 0; i < c.tabCount; ++i) {
        strncpy(c.tabs[i], s.pages[i].name, sizeof(c.tabs[i]) - 1);
    }
    c.activeTab = s.page;
    Page& p = s.pages[s.page];
    const int pendingN = pendingRestartCount();

    if (p.status) {
        buildStatus(c);
        snprintf(c.hint, sizeof(c.hint), "%s",
                 "The page a support thread will ask to see. Tab or PageDown for the next page.");
    } else {
        // Keep the highlight in the window.
        const int total = static_cast<int>(p.entries.size());
        if (p.highlight >= total) p.highlight = total ? total - 1 : 0;
        if (p.highlight < p.scroll) p.scroll = p.highlight;
        if (p.highlight >= p.scroll + kVisibleRows) p.scroll = p.highlight - kVisibleRows + 1;
        if (p.scroll < 0) p.scroll = 0;
        for (int i = p.scroll; i < total && c.lineCount < kVisibleRows; ++i) {
            const Entry& e = p.entries[i];
            MenuLine& l = c.lines[c.lineCount++];
            l.badge = kBadgeNone;
            if (e.kind == EntryKind::Heading) {
                strncpy(l.left, e.text, sizeof(l.left) - 1);
                l.style = kMenuHeading;
                continue;
            }
            const bool hi = (i == p.highlight);
            if (e.kind == EntryKind::Action) {
                const Action& a = g_actions[e.action];
                strncpy(l.left, a.label.c_str(), sizeof(l.left) - 1);
                strncpy(l.right, "run", sizeof(l.right) - 1);
                l.style = hi ? kMenuRowHi : kMenuRow;
                if (hi) snprintf(c.hint, sizeof(c.hint), "%s", a.hint.c_str());
                continue;
            }
            const MenuRowDef& d = kMenuRows[e.def];
            const RowState& r = g_rows[e.def];
            strncpy(l.left, d.label, sizeof(l.left) - 1);
            std::string v = displayValue(d, r.value);
            if (r.pending) {
                v = displayValue(d, r.snapshot) + " -> " + v;
                l.badge = kBadgePending;
            } else if (d.applies == 2) {
                l.badge = kBadgeRestart;
            } else if (d.applies == 0) {
                l.badge = kBadgeUnknown;
            }
            strncpy(l.right, v.c_str(), sizeof(l.right) - 1);
            l.style = d.kind == MenuKind::Text ? kMenuDim : (hi ? kMenuRowHi : kMenuRow);
            if (hi) {
                if (s.resetArmedEntry == i) {
                    snprintf(c.hint, sizeof(c.hint), "Press R again to reset to the shipped %s.",
                             displayValue(d, d.shipped).c_str());
                } else if (s.developer) {
                    snprintf(c.hint, sizeof(c.hint), "%s.%s (%s) -- %s", d.section, d.key,
                             d.applies == 1 ? "live" : d.applies == 2 ? "restart" : "when it applies is not documented",
                             d.hint);
                } else {
                    snprintf(c.hint, sizeof(c.hint), "%s%s", d.hint,
                             d.applies == 2 ? " Takes effect at the next launch." : "");
                }
            }
        }
        if (c.lineCount == 0) {
            MenuLine& l = c.lines[c.lineCount++];
            strncpy(l.left, "Nothing on this page yet.", sizeof(l.left) - 1);
            l.style = kMenuDim;
        }
    }

    std::string footer = "Up/Down pick   Left/Right change   Enter toggle   Tab page   Esc close";
    if (pendingN) {
        char pb[64];
        snprintf(pb, sizeof(pb), "   %d change%s at next launch", pendingN, pendingN == 1 ? "" : "s");
        footer += pb;
    }
    if (s.open && s.privateWanted && !inputGatePrivate()) footer += "   KEYS SHARED WITH THE GAME";
    if (!s.privateWanted) footer += "   keys shared (menu.keyboard)";
    strncpy(c.footer, footer.c_str(), sizeof(c.footer) - 1);

    // Sizing from the channel: pixels per degree at the panel.
    float ppd = 45.0f;
    uint32_t ew = 0, eh = 0;
    float outer = 0.0f, inner = 0.0f;
    if (eyeTextureSize(&ew, &eh) && eyeTangents(&outer, &inner) && ew > 0) {
        const float spanDeg = (atanf(outer) + atanf(inner)) * 57.2957795f;
        if (spanDeg > 20.0f) ppd = static_cast<float>(ew) / spanDeg;
    }
    if (ppd < 12.0f) ppd = 12.0f;
    if (ppd > 80.0f) ppd = 80.0f;
    c.widthPx = static_cast<int>(ppd * s.widthDeg + 0.5f);
    if (c.widthPx < 480) c.widthPx = 480;
    if (c.widthPx > 1600) c.widthPx = 1600;
    c.capPx = static_cast<int>(ppd * s.textDeg + 0.5f);
    if (c.capPx < 10) c.capPx = 10;
    if (c.capPx > 80) c.capPx = 80;
}

void buildToastContent(MenuContent& c, const std::string& text) {
    memset(&c, 0, sizeof(c));
    c.toast = true;
    c.lineCount = 1;
    strncpy(c.lines[0].left, text.c_str(), sizeof(c.lines[0].left) - 1);
    c.lines[0].style = kMenuInfo;
    float ppd = 45.0f;
    uint32_t ew = 0, eh = 0;
    float outer = 0.0f, inner = 0.0f;
    if (eyeTextureSize(&ew, &eh) && eyeTangents(&outer, &inner) && ew > 0) {
        const float spanDeg = (atanf(outer) + atanf(inner)) * 57.2957795f;
        if (spanDeg > 20.0f) ppd = static_cast<float>(ew) / spanDeg;
    }
    if (ppd < 12.0f) ppd = 12.0f;
    if (ppd > 80.0f) ppd = 80.0f;
    c.widthPx = static_cast<int>(ppd * 16.0f + 0.5f);
    if (c.widthPx < 320) c.widthPx = 320;
    if (c.widthPx > 1200) c.widthPx = 1200;
    c.capPx = static_cast<int>(ppd * g_s.textDeg + 0.5f);
    if (c.capPx < 10) c.capPx = 10;
    if (c.capPx > 80) c.capPx = 80;
}

// ---------------------------------------------------------------------------
// Editing

void applyChange(int defIndex, const std::string& fileValue) {
    State& s = g_s;
    const MenuRowDef& d = kMenuRows[defIndex];
    const std::string dotted = dottedOf(d);
    const std::string before = g_rows[defIndex].value;
    std::string err;
    if (!menuIniWrite(dotted, fileValue, &err)) {
        s.lastWrite = "FAILED: " + err;
        Log::get().note("menu: %s = %s could not be written: %s.", dotted.c_str(),
                        fileValue.c_str(), err.c_str());
        s.contentDirty = true;
        return;
    }
    s.menuWroteDotted = dotted;
    s.pollRequest = true;
    s.lastWrite = dotted + " = " + fileValue;
    Log::get().note("menu: %s %s -> %s (written to edvr.ini; %s).", dotted.c_str(),
                    before.empty() ? "(default)" : before.c_str(), fileValue.c_str(),
                    d.applies == 2 ? "takes effect at the next launch"
                    : d.applies == 1 ? "live"
                                     : "when it applies is not documented");
    // Show it now; the reload confirms it within the frame.
    g_rows[defIndex].value = fileValue;
    if (d.applies == 2) g_rows[defIndex].pending = (fileValue != g_rows[defIndex].snapshot);
    s.contentDirty = true;
}

void stepRow(int defIndex, int dir, int mult) {
    const MenuRowDef& d = kMenuRows[defIndex];
    const std::string& cur = g_rows[defIndex].value;
    switch (d.kind) {
        case MenuKind::Toggle: {
            const bool on = boolOf(cur, boolOf(d.shipped, false));
            applyChange(defIndex, boolWord(!on, cur));
            break;
        }
        case MenuKind::Choice: {
            const std::vector<ChoiceItem> items = choicesOf(d);
            if (items.empty()) return;
            int at = -1;
            for (size_t i = 0; i < items.size(); ++i) {
                if (_stricmp(items[i].value.c_str(), cur.c_str()) == 0) at = static_cast<int>(i);
            }
            const int n = static_cast<int>(items.size());
            int next = at < 0 ? 0 : ((at + (dir >= 0 ? 1 : n - 1)) % n);
            applyChange(defIndex, items[next].value);
            break;
        }
        case MenuKind::Number: {
            double v = atof(cur.empty() ? d.shipped : cur.c_str());
            const double step = stepOf(d) * mult;
            v += dir * step;
            // Snap to the step grid so 0.3 does not become 0.30000001 after a few presses.
            v = floor(v / step + 0.5) * step;
            if (d.lo[0] && v < atof(d.lo)) v = atof(d.lo);
            if (d.hi[0] && v > atof(d.hi)) v = atof(d.hi);
            applyChange(defIndex, formatNumber(v, d.precision));
            break;
        }
        case MenuKind::Text:
        default:
            break;
    }
}

void activateEntry() {
    State& s = g_s;
    Page& p = s.pages[s.page];
    if (p.status || p.entries.empty()) return;
    const Entry& e = p.entries[p.highlight];
    if (e.kind == EntryKind::Action) {
        const Action& a = g_actions[e.action];
        Log::get().note("menu: running \"%s\".", a.label.c_str());
        if (a.fn) a.fn(a.user);
        s.lastWrite = "ran: " + a.label;
        s.contentDirty = true;
        return;
    }
    if (e.kind == EntryKind::Setting) {
        const MenuRowDef& d = kMenuRows[e.def];
        if (d.kind == MenuKind::Toggle || d.kind == MenuKind::Choice) stepRow(e.def, +1, 1);
    }
}

void moveHighlight(int dir) {
    State& s = g_s;
    Page& p = s.pages[s.page];
    if (p.status || p.entries.empty()) return;
    int h = p.highlight;
    const int n = static_cast<int>(p.entries.size());
    for (int tries = 0; tries < n; ++tries) {
        h += dir;
        if (h < 0 || h >= n) return;
        if (p.entries[h].kind != EntryKind::Heading) {
            p.highlight = h;
            s.contentDirty = true;
            return;
        }
    }
}

void changePage(int dir) {
    State& s = g_s;
    const int n = static_cast<int>(s.pages.size());
    if (n == 0) return;
    s.page = ((s.page + dir) % n + n) % n;
    s.resetArmedEntry = -1;
    s.contentDirty = true;
}

// ---------------------------------------------------------------------------
// Keys

bool gameHasFocus() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Edge-and-repeat over EDVR's own import of GetAsyncKeyState, which the
// gate never patches. Returns how many presses this key delivered this tick.
int pollKey(KeyRepeat& k, uint64_t now, bool focused) {
    const bool down = focused && (GetAsyncKeyState(k.vk) & 0x8000) != 0;
    int presses = 0;
    if (down && !k.down) {
        presses = 1;
        k.nextMs = now + kRepeatFirstMs;
    } else if (down && now >= k.nextMs) {
        presses = 1;
        k.nextMs = now + kRepeatMs;
    }
    k.down = down;
    return presses;
}

enum KeyIndex {
    kUp, kDown, kLeft, kRight, kEnter, kSpace, kTab, kPgUp, kPgDn, kHome, kEnd, kReset
};

void initKeys() {
    const int vks[12] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_SPACE,
                         VK_TAB, VK_PRIOR, VK_NEXT, VK_HOME, VK_END, 'R'};
    for (int i = 0; i < 12; ++i) {
        g_s.keys[i].vk = vks[i];
        g_s.keys[i].down = false;
        g_s.keys[i].nextMs = 0;
    }
}

void handleKeys(uint64_t now) {
    State& s = g_s;
    const bool focused = gameHasFocus();
    s.shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool any = false;
    Page& p = s.pages[s.page];
    for (int i = 0; i < 12; ++i) {
        const int n = pollKey(s.keys[i], now, focused);
        if (!n) continue;
        any = true;
        switch (i) {
            case kUp: moveHighlight(-1); break;
            case kDown: moveHighlight(+1); break;
            case kLeft:
            case kRight:
                if (!p.status && !p.entries.empty() &&
                    p.entries[p.highlight].kind == EntryKind::Setting) {
                    stepRow(p.entries[p.highlight].def, i == kLeft ? -1 : +1, s.shiftHeld ? 5 : 1);
                }
                break;
            case kEnter:
            case kSpace: activateEntry(); break;
            case kTab: changePage(s.shiftHeld ? -1 : +1); break;
            case kPgUp: changePage(-1); break;
            case kPgDn: changePage(+1); break;
            case kHome:
                if (!p.status) { p.highlight = 0; moveHighlight(0); if (p.entries.size() && p.entries[0].kind == EntryKind::Heading) moveHighlight(+1); s.contentDirty = true; }
                break;
            case kEnd:
                if (!p.status && !p.entries.empty()) { p.highlight = static_cast<int>(p.entries.size()) - 1; s.contentDirty = true; }
                break;
            case kReset:
                if (!p.status && !p.entries.empty() && p.entries[p.highlight].kind == EntryKind::Setting) {
                    if (s.resetArmedEntry == p.highlight && now - s.resetArmedMs < kResetArmMs) {
                        const MenuRowDef& d = kMenuRows[p.entries[p.highlight].def];
                        applyChange(p.entries[p.highlight].def, d.shipped);
                        s.resetArmedEntry = -1;
                    } else {
                        s.resetArmedEntry = p.highlight;
                        s.resetArmedMs = now;
                        s.contentDirty = true;
                    }
                }
                break;
        }
    }
    if (any) {
        s.lastInputMs = now;
        s.aimParked = true;
    }
    if (s.resetArmedEntry >= 0 && now - s.resetArmedMs >= kResetArmMs) {
        s.resetArmedEntry = -1;
        s.contentDirty = true;
    }
}

// ---------------------------------------------------------------------------
// Head-aim

// Rotate and translate the current head into the anchor's frame: org is
// the head's position, dir its forward, both in anchor space.
bool headRayInAnchor(float org[3], float dir[3]) {
    const State& s = g_s;
    if (!s.anchorValid) return false;
    float c[12];
    if (!headPose(c)) return false;
    const float* a = s.anchor;
    const float dt[3] = {c[3] - a[3], c[7] - a[7], c[11] - a[11]};
    // World forward of the current head: R * (0,0,-1) = the negated third column.
    const float fw[3] = {-c[2], -c[6], -c[10]};
    for (int i = 0; i < 3; ++i) {
        // Ra^T * v: dot with Ra's i-th column.
        org[i] = a[0 * 4 + i] * dt[0] + a[1 * 4 + i] * dt[1] + a[2 * 4 + i] * dt[2];
        dir[i] = a[0 * 4 + i] * fw[0] + a[1 * 4 + i] * fw[1] + a[2 * 4 + i] * fw[2];
    }
    return true;
}

void handleAim(uint64_t now) {
    State& s = g_s;
    if (s.aimMode == 1) return;
    Page& p = s.pages[s.page];
    if (p.status || p.entries.empty()) return;
    float org[3], dir[3];
    if (!headRayInAnchor(org, dir)) return;
    const float aspect = menuPanelAspect();
    if (!(aspect > 0.0f)) return;
    const float halfW = s.distance * tanf(s.widthDeg * 0.5f * 0.0174532925f);
    const float halfH = halfW * aspect;
    float su = 0.0f, sv = 0.0f;
    int line = -1;
    if (menuPanelHit(org, dir, s.distance, s.curve, halfW, halfH, &su, &sv)) {
        line = menuPanelLineAt(su, 1.0f - sv);
    }
    // The line index is into the CONTENT's window; map it to an entry.
    int entry = -1;
    if (line >= 0) {
        entry = p.scroll + line;
        if (entry >= static_cast<int>(p.entries.size())) entry = -1;
        else if (p.entries[entry].kind == EntryKind::Heading) entry = -1;
    }
    if (entry == s.aimLast) {
        if (s.aimSameCount < 1000) ++s.aimSameCount;
    } else {
        s.aimLast = entry;
        s.aimSameCount = 0;
    }
    if (entry < 0) return;
    // Parked after a key press until the look moves two rows away.
    if (s.aimParked) {
        if (abs(entry - p.highlight) >= 2) s.aimParked = false;
        else return;
    }
    // Hysteresis: a resting head must not flicker the highlight. Three
    // consecutive ticks on the new row (about 33 ms), and the row must be
    // a neighbour crossed decisively or a jump.
    if (entry != p.highlight && s.aimSameCount >= 3) {
        p.highlight = entry;
        s.resetArmedEntry = -1;
        s.contentDirty = true;
        s.lastInputMs = now;
    }
}

// ---------------------------------------------------------------------------
// Open, close, anchor

void latchAnchor(float pitchDeg) {
    State& s = g_s;
    float c[12];
    if (!headPose(c)) {
        s.anchorValid = false;
        return;
    }
    // Rotate the anchor's frame about its own x axis by pitchDeg, for a toast
    // that sits below the look direction: R' = R * Rx(theta).
    const float th = pitchDeg * 0.0174532925f;
    const float cs = cosf(th), sn = sinf(th);
    float out[12];
    for (int r = 0; r < 3; ++r) {
        const float y = c[r * 4 + 1], z = c[r * 4 + 2];
        out[r * 4 + 0] = c[r * 4 + 0];
        out[r * 4 + 1] = y * cs + z * sn;
        out[r * 4 + 2] = -y * sn + z * cs;
        out[r * 4 + 3] = c[r * 4 + 3];
    }
    memcpy(s.anchor, out, sizeof(out));
    s.anchorValid = true;
    publishMenuAnchor(out);
}

void openMenu(uint64_t now) {
    State& s = g_s;
    if (!glitchConsumerPresent()) {
        if (!s.noConsumerNoted) {
            s.noConsumerNoted = true;
            Log::get().note("menu: the menu key was pressed, but no openvr_api.dll half has "
                            "hooked the compositor, so there is no door to draw the panel "
                            "at. The menu stays closed and the keyboard stays the game's. "
                            "Install the second file (README) to use it.");
        }
        return;
    }
    latchAnchor(0.0f);
    if (!s.anchorValid) {
        if (!s.noPoseNoted) {
            s.noPoseNoted = true;
            Log::get().note("menu: no head pose has been published yet, so the panel has "
                            "nowhere to anchor. Try again once the headset is tracking.");
        }
        return;
    }
    inputGateInstall();
    if (s.developer != s.developerBuilt) buildPages();
    refreshRowValues();
    s.open = true;
    s.openedMs = now;
    s.lastInputMs = now;
    s.lastDrawnMs = now;   // grace: the first draw has not had a chance yet
    s.aimParked = false;
    s.contentDirty = true;
    s.toastUp = false;
    s.toastAlpha = 0.0f;
    Log::get().note("menu: open (%s, %.1f m, keys %s).", s.pages[s.page].name,
                    static_cast<double>(s.distance), s.privateWanted ? "private" : "shared");
}

void closeMenu(const char* why) {
    State& s = g_s;
    if (!s.open) return;
    s.open = false;
    s.resetArmedEntry = -1;
    inputGateSetPrivate(false);
    Log::get().note("menu: closed (%s).", why);
}

}  // namespace

// ---------------------------------------------------------------------------

void menuConfigure(Config& cfg) {
    State& s = g_s;
    const std::string key = cfg.getString("hotkey.menu", "F8");
    if (key != s.summonName || !s.configured) {
        s.summonName = key;
        s.summon.setBinding(key.c_str());
        if (s.summon.key() == 0 && !key.empty()) {
            Log::get().note("menu: hotkey.menu = \"%s\" bound nothing (the line above says "
                            "why), so the menu cannot be summoned this session.",
                            key.c_str());
        }
    }
    const std::string kb = cfg.getString("menu.keyboard", "private");
    s.privateWanted = _stricmp(kb.c_str(), "shared") != 0;
    const std::string aim = cfg.getString("menu.aim", "both");
    s.aimMode = _stricmp(aim.c_str(), "head") == 0 ? 0 : _stricmp(aim.c_str(), "keys") == 0 ? 1 : 2;
    float d = cfg.getFloat("menu.distance", 1.4f);
    if (!(d >= 0.5f) || d > 5.0f) d = 1.4f;
    s.distance = d;
    float c = cfg.getFloat("menu.curve", 0.2f);
    if (!(c >= 0.0f) || c > 0.9f) c = 0.2f;
    s.curve = c;
    float t = cfg.getFloat("menu.text_degrees", 1.1f);
    if (!(t >= 0.6f) || t > 3.0f) t = 1.1f;
    s.textDeg = t;
    s.idleSeconds = cfg.getIntInRange("menu.idle_dismiss", 20, 0, 600);
    s.toasts = cfg.getBool("menu.toasts", true);
    const bool dev = cfg.getBool("menu.developer", false);
    if (dev != s.developer) {
        s.developer = dev;
        if (s.configured) buildPages();
    }
    inputGateConfigure(cfg);
    if (!s.configured) {
        s.configured = true;
        initKeys();
        refreshRowValues();
        takeSnapshot();
        buildPages();
        Log::get().note("menu: %d rows in the table (%d on the Fixes/Performance pages); "
                        "summon with %s. docs/settings-menu.md.",
                        kRowDefCount,
                        static_cast<int>(s.pages[0].entries.size() + s.pages[1].entries.size()),
                        key.c_str());
    }
}

void menuRegisterAction(const char* label, const char* hint, MenuActionFn fn, void* user) {
    Action a;
    a.label = label ? label : "";
    a.hint = hint ? hint : "";
    a.fn = fn;
    a.user = user;
    g_actions.push_back(a);
    if (g_s.configured && g_s.developer) buildPages();
}

bool menuTakeConfigPollRequest() {
    const bool r = g_s.pollRequest;
    g_s.pollRequest = false;
    return r;
}

bool menuOpen() { return g_s.open || g_s.alpha > 0.0f; }

void menuNoteConfigReloaded() {
    State& s = g_s;
    if (!s.configured) return;
    std::vector<std::string> changed;
    for (int i = 0; i < kRowDefCount; ++i) {
        const MenuRowDef& d = kMenuRows[i];
        RowState& r = g_rows[i];
        const std::string v = rowValue(d);
        const std::string dotted = dottedOf(d);
        const bool byMenu = (s.menuWroteDotted == dotted);
        if (v != r.value) {
            r.value = v;
            if (!byMenu) {
                changed.push_back(std::string(d.label) + ": " + displayValue(d, v) +
                                 (d.applies == 2 ? " (at next launch)" : ""));
            }
        }
        if (d.applies == 2 && s.snapshotTaken) {
            r.pending = (v != r.snapshot);
            if (r.pending && !r.auditNoted) {
                r.auditNoted = true;
                Log::get().note("edvr.ini: %s is now %s on disk but is read at launch; the running "
                                "value is still %s. Restart the game to apply it.",
                                dotted.c_str(), v.empty() ? "(default)" : v.c_str(),
                                r.snapshot.empty() ? "(default)" : r.snapshot.c_str());
            }
        }
    }
    s.menuWroteDotted.clear();
    if (!changed.empty()) s.contentDirty = true;
    if (s.toasts && !s.open) {
        for (size_t i = 0; i < changed.size() && i < 3; ++i) s.toastQueue.push_back(changed[i]);
    }
}

void menuTick(ID3D11Device* dev) {
    State& s = g_s;
    if (!s.configured) return;
    guardedBudget(g_budget, [&] {
        const uint64_t now = nowMs();
        const uint64_t dt = s.lastTickMs ? now - s.lastTickMs : 0;
        s.lastTickMs = now;

        // Frame timing, on the fine clock: Present to Present.
        {
            const int64_t q = qpcNow();
            if (s.lastQpc && qpcFrequency() > 0) {
                const double ms = static_cast<double>(q - s.lastQpc) * 1000.0 /
                                  static_cast<double>(qpcFrequency());
                if (ms > 0.0 && ms < 2000.0) {
                    if (s.frameMsEma <= 0.0) s.frameMsEma = ms;
                    else s.frameMsEma += (ms - s.frameMsEma) * 0.02;
                    if (s.frameMsEma > 2.0 && ms > s.frameMsEma * 1.5) ++s.longFrames;
                }
            }
            s.lastQpc = q;
            if (dueMs(s.longWindowMs, 30000)) {
                s.longWindowMs = stampMs();
                s.longFramesShown = s.longFrames;
                s.longFrames = 0;
            }
        }

        // The summon key: EDVR's own, focus-gated. With Shift, recentre.
        if (s.summon.pressed()) {
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (!s.open) {
                openMenu(now);
            } else if (shift) {
                latchAnchor(0.0f);
                s.lastInputMs = now;
                Log::get().note("menu: recentred where you are looking.");
            } else {
                closeMenu("the menu key");
            }
        }

        if (s.open) {
            // Escape, then the navigation keys, then the head.
            static bool escDown = false;
            const bool esc = gameHasFocus() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            if (esc && !escDown) closeMenu("Escape");
            escDown = esc;
        }
        if (s.open) {
            handleKeys(now);
            handleAim(now);
            if (s.idleSeconds > 0 && now - s.lastInputMs > static_cast<uint64_t>(s.idleSeconds) * 1000) {
                closeMenu("idle");
            }
        }

        // The draw the gate follows.
        const uint32_t drawn = menuDrawnValue();
        if (drawn != s.lastDrawn) {
            s.lastDrawn = drawn;
            s.lastDrawnMs = now;
        }
        if (s.open && now - s.openedMs > kNotDrawnCloseMs && now - s.lastDrawnMs > kNotDrawnCloseMs) {
            if (!s.notDrawnNoted) {
                s.notDrawnNoted = true;
                Log::get().note("menu: the panel is not being drawn -- the openvr half never "
                                "called the door export (a mismatched pair, or every submit "
                                "path refused). The menu closes and the keyboard stays the "
                                "game's; please report this log.");
            }
            closeMenu("not drawn");
        }

        // Fade toward the target.
        {
            const float target = s.open ? 1.0f : 0.0f;
            const float rate = dt > 0 ? static_cast<float>(dt) / static_cast<float>(kFadeMs) : 1.0f;
            if (s.alpha < target) s.alpha = s.alpha + rate > target ? target : s.alpha + rate;
            else if (s.alpha > target) s.alpha = s.alpha - rate < target ? target : s.alpha - rate;
        }

        // Toasts, when the menu is down.
        if (!s.open && s.alpha <= 0.0f) {
            if (!s.toastUp && !s.toastQueue.empty()) {
                s.toastText = s.toastQueue.front();
                s.toastQueue.erase(s.toastQueue.begin());
                latchAnchor(kToastPitchDeg);
                if (s.anchorValid) {
                    s.toastUp = true;
                    s.toastUntilMs = now + kToastMs;
                    MenuContent c;
                    buildToastContent(c, s.toastText);
                    menuPanelSubmit(c);
                }
            }
            if (s.toastUp) {
                const bool alive = now < s.toastUntilMs;
                const float target = alive ? 1.0f : 0.0f;
                const float rate = dt > 0 ? static_cast<float>(dt) / static_cast<float>(kFadeMs) : 1.0f;
                if (s.toastAlpha < target) s.toastAlpha = s.toastAlpha + rate > target ? target : s.toastAlpha + rate;
                else if (s.toastAlpha > target) s.toastAlpha = s.toastAlpha - rate < target ? target : s.toastAlpha - rate;
                if (!alive && s.toastAlpha <= 0.0f) {
                    s.toastUp = false;
                    s.contentDirty = true;   // the menu's content must be re-sent after a toast
                }
            }
        } else {
            s.toastUp = false;
            s.toastAlpha = 0.0f;
        }

        // Content, geometry, visibility, the gate.
        if (s.open || s.alpha > 0.0f) {
            Page& p = s.pages[s.page];
            if (p.status && dueMs(s.statusRefreshMs, kStatusRefreshMs)) {
                s.statusRefreshMs = stampMs();
                s.contentDirty = true;
            }
            if (s.contentDirty) {
                s.contentDirty = false;
                MenuContent c;
                buildContent(c);
                menuPanelSubmit(c);
            }
        }
        const bool showingMenu = s.alpha > 0.0f && !s.toastUp;
        MenuGeometry g;
        g.dist = s.distance;
        g.curve = s.toastUp ? 0.0f : s.curve;
        g.halfW = s.distance * tanf((s.toastUp ? 16.0f : s.widthDeg) * 0.5f * 0.0174532925f);
        g.alpha = s.toastUp ? s.toastAlpha : s.alpha;
        menuPanelSetGeometry(g);
        setMenuVisible(g.alpha);
        const bool drawnFresh = now - s.lastDrawnMs <= kDrawnFreshMs;
        inputGateSetPrivate(s.open && showingMenu && drawnFresh);
        inputGateTick();
        if (dev) menuPanelTick(dev);
    });
    if (!g_budget.shouldRun()) {
        // A faulting tick must not leave the keyboard taken.
        inputGateSetPrivate(false);
        setMenuVisible(0.0f);
    }
}

void menuShutdown() {
    inputGateSetPrivate(false);
    setMenuVisible(0.0f);
    inputGateShutdown();
    menuPanelShutdown();
}

}  // namespace edvr
