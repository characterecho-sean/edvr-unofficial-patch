#include "menu.h"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/config.h"
#include "../common/runtime_profile.h"
#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/hotkey.h"
#include "../common/iniedit.h"
#include "../common/log.h"
#include "../common/native_render_settings.h"
#include "../common/openxr_resolution_entries.h"
#include "../common/proxy.h"
#include "../common/timing.h"
#include "../common/temporal_mode.h"
#include "device_hook.h"
#include "elite_binds.h"
#include "flat_runtime.h"
#include "input_gate.h"
#include "menu_keys.h"
#include "menu_panel.h"
#include "native_menu.h"
#include "native_render_labels.h"
#include "vr_runtime.h"
#include "menu_schema.h"
#include "perf_monitor.h"
#include "sharpen_pass.h"
#include "temporal_pass.h"
#include "vscreen_res.h"
// fsr3_engine.h is deliberately NOT included: the Temporal AA status line
// reaches AMD's price and its name through temporal_pass.h's
// temporalPassTrainedTotals, which answers for the engine in force (F6).

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
// (Hold-to-repeat's timings and the tracker live in menu_keys.h now, so
// the test can step them.)
// The draw the gate follows: a panel whose export has not run within this
// long is not being seen, and the keyboard goes back to the game.
constexpr uint64_t kDrawnFreshMs = 250;
// ...and a menu that has not been drawn at all for this long after opening
// closes itself and says why.
constexpr uint64_t kNotDrawnCloseMs = 1500;
// A toast's life, and the pitch it sits below the look direction.
constexpr uint64_t kToastMs = 2500;
constexpr float    kToastPitchDeg = -12.0f;
// The Status page's live values are refreshed at this cadence while shown;
// the Monitor page's at four a second, so its digits read as a gauge and
// not a flicker.
constexpr uint64_t kStatusRefreshMs = 500;
constexpr uint64_t kMonitorRefreshMs = 250;
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
    bool               status = false;    // an information page: no rows to pick
    bool               monitor = false;   // ...and this one is the performance monitor
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

// The keys a typed value can be built from: the digits (top row and the
// numeric pad), the letters, and the punctuation a value in this ini can
// contain -- a decimal point, a minus, a comma for a list, a space, and
// Backspace. Nothing else is admitted, so a stray key cannot corrupt a
// value, and the gate keeps every one of them from the game meanwhile.
struct EditKey {
    int  vk;
    char plain;    // the character it types, or 0 for Backspace
    char shifted;  // what Shift makes of it, when that differs
};
constexpr EditKey kEditKeys[] = {
    {'0', '0', ')'}, {'1', '1', '!'}, {'2', '2', '@'}, {'3', '3', '#'}, {'4', '4', '$'},
    {'5', '5', '%'}, {'6', '6', '^'}, {'7', '7', '&'}, {'8', '8', '*'}, {'9', '9', '('},
    {VK_NUMPAD0, '0', '0'}, {VK_NUMPAD1, '1', '1'}, {VK_NUMPAD2, '2', '2'},
    {VK_NUMPAD3, '3', '3'}, {VK_NUMPAD4, '4', '4'}, {VK_NUMPAD5, '5', '5'},
    {VK_NUMPAD6, '6', '6'}, {VK_NUMPAD7, '7', '7'}, {VK_NUMPAD8, '8', '8'},
    {VK_NUMPAD9, '9', '9'}, {VK_DECIMAL, '.', '.'}, {VK_SUBTRACT, '-', '-'},
    {'A', 'a', 'A'}, {'B', 'b', 'B'}, {'C', 'c', 'C'}, {'D', 'd', 'D'}, {'E', 'e', 'E'},
    {'F', 'f', 'F'}, {'G', 'g', 'G'}, {'H', 'h', 'H'}, {'I', 'i', 'I'}, {'J', 'j', 'J'},
    {'K', 'k', 'K'}, {'L', 'l', 'L'}, {'M', 'm', 'M'}, {'N', 'n', 'N'}, {'O', 'o', 'O'},
    {'P', 'p', 'P'}, {'Q', 'q', 'Q'}, {'R', 'r', 'R'}, {'S', 's', 'S'}, {'T', 't', 'T'},
    {'U', 'u', 'U'}, {'V', 'v', 'V'}, {'W', 'w', 'W'}, {'X', 'x', 'X'}, {'Y', 'y', 'Y'},
    {'Z', 'z', 'Z'},
    {VK_OEM_PERIOD, '.', '>'}, {VK_OEM_MINUS, '-', '_'}, {VK_OEM_COMMA, ',', '<'},
    {VK_SPACE, ' ', ' '}, {VK_BACK, 0, 0},
};
constexpr int kEditKeyCount = static_cast<int>(sizeof(kEditKeys) / sizeof(kEditKeys[0]));
// A typed value is never longer than this: the longest thing in the ini is
// a key name, and the row has to show it.
constexpr size_t kEditMax = 40;

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
    float widthDeg = 30.0f;
    int   idleSeconds = 0;
    int   aimMode = 2;   // 0 head, 1 keys, 2 both
    // The tooltip beside the highlighted row: how long a row must be held
    // before it appears, 0 for never. The row it is showing, when it is up.
    float    tooltipDelayS = 1.5f;
    uint64_t highlightSinceMs = 0;
    bool     tooltipUp = false;
    bool     tooltipsWere = true;    // to notice a live change of shape
    bool     tipWidthNoted = false;
    int      tooltipScroll = 0;      // lines of the tooltip's body scrolled past
    uint64_t tickMs = 0;

    bool  open = false;
    bool  flatEscapeDown = false;
    bool  flatKeysReadyShown = false;
    bool  flatNativeScaleShown = false;
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

    // The graphics provider publishes the live OpenXR view bounds after the
    // native session starts.  Keep the last coherent snapshot so the menu
    // can preview a restart value without constructing a runtime of its own.
    EdvrNativeRenderSizing renderSizing{};
    bool renderSizingAvailable = false;

    KeyRepeat keys[12];
    bool shiftHeld = false;

    // Elite's own panel keys as aliases of the menu's actions (menu_keys.h
    // holds the rules; docs/settings-menu.md, "Your Elite keys"). The raw
    // answers are cached so a hotkey.menu change re-resolves without a
    // file read; one tracker per adopted alias, its vk copied from the
    // table. The aliases are never Hotkey bindings.
    EliteKeySlots  aliasSlots[kMenuUiElementCount] = {};
    MenuAliasTable aliases = {};
    KeyRepeat      aliasKeys[kMenuAliasMax];
    // 0 never read, 1 read off, 2 no files, 3 read but nothing adopted,
    // 4 adopted.
    uint8_t aliasSource = 0;
    bool    readGameBindings = true;      // hotkey.read_game_bindings as last configured
    bool    aliasesLiveShown = false;     // what the last raster's legend assumed; a change marks contentDirty
    bool    sharedWarnShown = false;      // whether the last raster carried KEYS SHARED WITH THE GAME; same
    bool    aliasHeldBackNoted = false;   // the held-back line, once per session
    int     footerLogged = 0;             // the footer's measured widths, at most four per session
    std::string footerLoggedText;         // ...and the last footer they were logged for

    // Typing a value (a number, or a free string): the row being typed
    // into, the buffer, and the caret's blink. Enter opens and commits,
    // Escape cancels, Backspace deletes.
    int         editEntry = -1;      // an entry index on the current page, or -1
    int         editDef = -1;
    std::string editBuf;
    bool        editBad = false;     // the buffer is out of bounds or not a number
    KeyRepeat   editKeys[kEditKeyCount];

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

    // The head-locked readout (menu.fps_overlay), shown while the menu is
    // down: re-anchored to the head every frame, re-rasterised twice a
    // second.
    bool     overlay = false;
    bool     overlayLock = false;
    float    overlayYaw = 0.0f;
    float    overlayPitch = -16.0f;
    bool     overlayUp = false;
    float    overlayAlpha = 0.0f;
    uint64_t overlayTextMs = 0;
    std::string overlayText;
    float    overlayTextDeg = 0.0f;
    // The menu-embedded copy's own refresh clock (menu.fps_overlay_lock).
    // Kept apart from overlayTextMs: the floating readout's own block runs
    // first each tick and, on its own cadence, resets that field to "now"
    // before this one's check ever sees it due -- a shared field starved
    // the embedded copy's refresh outright (found 2026-09-25).
    uint64_t menuOverlayTextMs = 0;

    // Restart bookkeeping.
    bool snapshotTaken = false;
    std::string lastWrite;
    std::string menuWroteDotted;   // the change the menu itself made, so its reload does not toast

    // The reset-to-shipped arm.
    int      resetArmedEntry = -1;
    uint64_t resetArmedMs = 0;

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
    if (runtimeFlatProfile() && strcmp(d.section, "fix") == 0 &&
        strcmp(d.key, "temporal_aa") == 0)
        return Config::get().requestedTemporalMode();
    return Config::get().getString(dottedOf(d).c_str(), d.shipped);
}

bool flatDlssSelected() {
    for (int i = 0; i < kRowDefCount; ++i)
        if (strcmp(kMenuRows[i].section, "fix") == 0 &&
            strcmp(kMenuRows[i].key, "temporal_aa") == 0)
            return _stricmp(g_rows[i].value.c_str(), "dlss") == 0 ||
                   _stricmp(g_rows[i].value.c_str(), "dlaa") == 0;
    return false;
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

bool isOpenxrRenderScaleRow(const MenuRowDef& d) {
    return strcmp(d.section, "fix") == 0 &&
           strcmp(d.key, "openxr_resolution") == 0;
}

// The generic per-headset list row (`# ui: ... | headset`): one integer per
// headset in a `runtime/system:value` list, of which the row shows and edits
// only the worn headset's entry. The three field-of-view trims are these; the
// resolution row is a per-headset list too, but it keeps its own width
// semantics (percentages, megapixels, the runtime's cap) and is handled by
// everything above rather than by the generic integer row below.
bool isHeadsetRow(const MenuRowDef& d) {
    return d.headset && !isOpenxrRenderScaleRow(d);
}

// fix.temporal_aa_model (the DLSS preset row): NVIDIA's reconstruction
// model, which only DLSS and DLAA read (temporal_mode.h). It stays on the
// Performance page under every mode -- rather than dropping out of the list
// on a mode change, which read as the setting vanishing -- and dims and
// stops responding to input instead.
bool isDlssPresetRow(const MenuRowDef& d) {
    return strcmp(d.section, "fix") == 0 && strcmp(d.key, "temporal_aa_model") == 0;
}

bool dlssPresetDisabled() {
    if (runtimeFlatProfile()) return !flatDlssSelected();
    return temporalEngineFor(Config::get().getString("fix.temporal_aa", "off")) !=
           TemporalEngine::Nvidia;
}

// The on-foot panel's width: "auto", or a width in pixels -- never a list, so
// this is not a headset row despite living beside one in the ini. The height
// is not a setting any more; it is derived from whichever width applies.
bool isVscreenResolutionRow(const MenuRowDef& d) {
    return strcmp(d.section, "fix") == 0 && strcmp(d.key, "vscreen_res_width") == 0;
}

constexpr uint32_t kVscreenWidthLo = 640, kVscreenWidthHi = 8192;

// "auto" (any case), or digits within the panel patch's own sane range --
// vscreen_res.cpp refuses outside it anyway, so a value this rejects would
// only be refused later with a less specific message.
bool parseVscreenWidth(const std::string& text, uint32_t* out) {
    std::string v = text;
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    while (!v.empty() && v.back() == ' ') v.pop_back();
    if (_stricmp(v.c_str(), "auto") == 0) {
        if (out) *out = 0;
        return true;
    }
    if (v.empty() || v.size() > 5) return false;
    uint32_t value = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    if (value < kVscreenWidthLo || value > kVscreenWidthHi) return false;
    if (out) *out = value;
    return true;
}

bool coherentNativeRenderSizing(const EdvrNativeRenderSizing& sizing) {
    if (sizing.size != sizeof(sizing) ||
        sizing.version != EDVR_NATIVE_RENDER_SIZING_VERSION_1 ||
        sizing.valid != 1 || sizing.generation == 0 ||
        !std::isfinite(sizing.requestedScale) ||
        !std::isfinite(sizing.effectiveScale) || sizing.reserved != 0) {
        return false;
    }
    const float expected = edvr::native_render::effectiveScale(
        sizing.requestedScale, sizing.eyes, 2);
    if (std::fabs(expected - sizing.effectiveScale) > 0.0001f) return false;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const EdvrNativeRenderViewBounds& bounds = sizing.eyes[eye];
        if (!bounds.originalWidth || !bounds.originalHeight ||
            !bounds.maxWidth || !bounds.maxHeight ||
            bounds.originalWidth > bounds.maxWidth ||
            bounds.originalHeight > bounds.maxHeight ||
            !sizing.activeWidth[eye] || !sizing.activeHeight[eye] ||
            sizing.activeWidth[eye] > bounds.maxWidth ||
            sizing.activeHeight[eye] > bounds.maxHeight) {
            return false;
        }
        if (sizing.activeWidth[eye] != edvr::native_render::scaledDimension(
                bounds.originalWidth, bounds.maxWidth, sizing.effectiveScale) ||
            sizing.activeHeight[eye] != edvr::native_render::scaledDimension(
                bounds.originalHeight, bounds.maxHeight, sizing.effectiveScale)) {
            return false;
        }
    }
    return true;
}

bool readNativeRenderSizing(EdvrNativeRenderSizing* out) {
    if (!out) return false;
    EdvrNativeRenderSizing candidate{};
    if (!edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1,
                                     sizeof(candidate), &candidate) ||
        !coherentNativeRenderSizing(candidate)) {
        return false;
    }
    *out = candidate;
    return true;
}

// The shared formatter, so the label, the hint, the tooltip and the
// graphics log cannot disagree on 180 versus 180.0.
std::string percentText(float scale) {
    return edvr::native_render::formatPercent(scale * 100.0f) + "%";
}

std::string dimensionsText(uint32_t w, uint32_t h) {
    char text[32];
    snprintf(text, sizeof(text), "%ux%u", w, h);
    return text;
}

std::string megapixelsText(uint32_t w, uint32_t h) {
    char text[32];
    snprintf(text, sizeof(text), "%.1f MP", edvr::native_render::megapixels(w, h));
    return text;
}

std::string eyeDimensions(const EdvrNativeRenderSizing& sizing, float scale) {
    const float effective = edvr::native_render::effectiveScale(
        scale, sizing.eyes, 2);
    const uint32_t width[2] = {
        edvr::native_render::scaledDimension(sizing.eyes[0].originalWidth,
                                             sizing.eyes[0].maxWidth, effective),
        edvr::native_render::scaledDimension(sizing.eyes[1].originalWidth,
                                             sizing.eyes[1].maxWidth, effective)};
    const uint32_t height[2] = {
        edvr::native_render::scaledDimension(sizing.eyes[0].originalHeight,
                                             sizing.eyes[0].maxHeight, effective),
        edvr::native_render::scaledDimension(sizing.eyes[1].originalHeight,
                                             sizing.eyes[1].maxHeight, effective)};
    char text[96];
    if (width[0] == width[1] && height[0] == height[1]) {
        snprintf(text, sizeof(text), "%ux%u", width[0], height[0]);
    } else {
        snprintf(text, sizeof(text), "L%ux%u R%ux%u", width[0], height[0],
                 width[1], height[1]);
    }
    return text;
}

// ---------------------------------------------------------------------------
// fix.openxr_resolution: a list of per-headset widths, of which the row shows
// only the worn headset's (docs/openxr-resolution-per-headset-2026-09-14.md,
// "Menu"). Everything below reads the sizing and the labels LIVE rather than
// from s.renderSizing: that snapshot is refreshed only while the menu is open,
// and two of the sites (the reload badge and its audit line) run with it
// closed.

// The worn headset: its tokens and eye 0's recommendation, when the sizing
// is coherent and the last query's labels are valid. False before the first
// query and after the host closes, which the Status page shows as `unknown`.
bool currentHeadset(std::string* rt, std::string* sys, uint32_t* w, uint32_t* h,
                    EdvrNativeRenderSizing* sizingOut = nullptr,
                    NativeRenderLabels* labelsOut = nullptr) {
    EdvrNativeRenderSizing sizing{};
    NativeRenderLabels labels{};
    if (!readNativeRenderSizing(&sizing) || !nativeRenderLabels(&labels)) return false;
    if (rt) *rt = labels.runtimeToken;
    if (sys) *sys = labels.systemToken;
    if (w) *w = sizing.eyes[0].originalWidth;
    if (h) *h = sizing.eyes[0].originalHeight;
    if (sizingOut) *sizingOut = sizing;
    if (labelsOut) *labelsOut = labels;
    return true;
}

// What one on-disk value means for the worn headset, computed once so the
// label, the value column, the hint, the tooltip, the badge, the step and
// the log line all derive from the same numbers. The scale chain is the
// host's own: clampScale (0.25..2.0) then effectiveScale (the runtime's
// maximum over both eyes and both axes) then scaledDimension.
struct ResolutionView {
    bool        headset = false;   // sizing coherent and labels valid
    std::string rt, sys, key;      // the worn tokens and rt/sys
    NativeRenderLabels labels{};
    EdvrNativeRenderSizing sizing{};
    edvr::native_render::ResolutionEntry entries[edvr::native_render::kResolutionEntryMax];
    size_t      entryCount = 0;
    uint32_t    matched = 0;       // 0 none, 1 runtime/system, 2 runtime-only
    uint32_t    entryWidth = 0;    // the matched entry's width, 0 when none
    uint32_t    width = 0;         // resolved: the entry's, else the recommendation
    float       asked = 1.0f;      // width / recommendation, unclamped
    float       requested = 1.0f;  // clampScale(asked)
    float       effective = 1.0f;  // after the runtime cap
    uint32_t    afterW[2] = {};    // per eye, after a restart
    uint32_t    afterH[2] = {};
    int         clamp = 0;         // 0 none, 1 capped at 200%, -1 floored at 25%, 2 runtime cap
};

ResolutionView resolutionView(const std::string& value) {
    using namespace edvr::native_render;
    ResolutionView v;
    v.headset = currentHeadset(&v.rt, &v.sys, nullptr, nullptr, &v.sizing, &v.labels);
    v.entryCount = parseResolutionEntries(value.c_str(), v.entries, nullptr);
    if (!v.headset) return v;
    v.key = headsetKey(v.rt, v.sys);
    v.entryWidth = resolveResolutionWidth(v.entries, v.entryCount, v.rt, v.sys, &v.matched);
    const uint32_t rec = v.sizing.eyes[0].originalWidth;
    v.width = v.entryWidth ? v.entryWidth : rec;
    v.asked = widthToScale(v.width, rec);
    v.requested = clampScale(v.asked);
    v.effective = effectiveScale(v.requested, v.sizing.eyes, 2);
    for (unsigned eye = 0; eye < 2; ++eye) {
        v.afterW[eye] = scaledDimension(v.sizing.eyes[eye].originalWidth,
                                        v.sizing.eyes[eye].maxWidth, v.effective);
        v.afterH[eye] = scaledDimension(v.sizing.eyes[eye].originalHeight,
                                        v.sizing.eyes[eye].maxHeight, v.effective);
    }
    // The runtime's cap is named first: it binds whatever the 2x clamp did.
    if (v.effective + 0.0005f < v.requested) v.clamp = 2;
    else if (v.asked > EDVR_NATIVE_RENDER_SCALE_MAXIMUM + 0.0005f) v.clamp = 1;
    else if (v.asked < EDVR_NATIVE_RENDER_SCALE_MINIMUM - 0.0005f) v.clamp = -1;
    return v;
}

// The width the on-disk value gives the worn headset: the matched entry's,
// else the recommendation; 0 with no headset.
uint32_t resolvedWidth(const std::string& value, uint32_t* matched = nullptr) {
    const ResolutionView v = resolutionView(value);
    if (matched) *matched = v.matched;
    return v.headset ? v.width : 0;
}

// The restart badge for this row: whether the width the on-disk value would
// build differs from the width the host DID build for this VR start, as an
// integer compare of the same arithmetic. A typed width above 2x resolves
// to the same active width as 2x and badges nothing; a canonicalising
// rewrite of the list, or a hand edit to another headset's entry, badges
// nothing either. With no coherent sizing no restart is known to be needed.
bool renderScaleRowPending(const std::string& value) {
    const ResolutionView v = resolutionView(value);
    return v.headset && v.afterW[0] != v.sizing.activeWidth[0];
}

// Whether an entry keyed on the WORN key exists for R to remove: the exact
// runtime/system entry, or -- when the system token is empty, so the worn
// key is the runtime alone -- the runtime-only entry the menu itself wrote
// for this headset (matched 2 with no system). A runtime-only fallback under
// a non-empty system token is another key's entry and is left alone. One rule
// for every per-headset list, so R cannot mean two things on two rows.
bool wornEntryRemovable(bool headset, uint32_t matched, const std::string& sys) {
    return headset && (matched == 1 || (matched == 2 && sys.empty()));
}

bool resolutionEntryRemovable(const ResolutionView& v) {
    return wornEntryRemovable(v.headset, v.matched, v.sys);
}

bool sameEyes(const ResolutionView& v) {
    return v.afterW[0] == v.afterW[1] && v.afterH[0] == v.afterH[1] &&
           v.sizing.activeWidth[0] == v.sizing.activeWidth[1] &&
           v.sizing.activeHeight[0] == v.sizing.activeHeight[1];
}

std::string activeDimensions(const EdvrNativeRenderSizing& sizing) {
    if (sizing.activeWidth[0] == sizing.activeWidth[1] &&
        sizing.activeHeight[0] == sizing.activeHeight[1]) {
        return dimensionsText(sizing.activeWidth[0], sizing.activeHeight[0]);
    }
    return "L" + dimensionsText(sizing.activeWidth[0], sizing.activeHeight[0]) + " R" +
           dimensionsText(sizing.activeWidth[1], sizing.activeHeight[1]);
}

// "(11.6 MP, 180%)", or with the clamp that applied: "(11.6 MP, 200% cap)",
// "(0.2 MP, 25% floor)", "(66.2 MP, runtime cap 164.5%)".
std::string afterRestartFacts(const ResolutionView& v) {
    std::string p = "(" + megapixelsText(v.afterW[0], v.afterH[0]) + ", ";
    if (v.clamp == 2) p += "runtime cap " + percentText(v.effective);
    else if (v.clamp == 1) p += "200% cap";
    else if (v.clamp == -1) p += "25% floor";
    else p += percentText(v.effective);
    return p + ")";
}

std::string openxrResolutionLabel(const ResolutionView& v) {
    if (!v.headset) return "OpenXR res.";
    char text[48];
    snprintf(text, sizeof(text), "OpenXR res. %u px", v.width);
    return text;
}

std::string openxrResolutionValue(const ResolutionView& v) {
    if (!v.headset) return "unavailable";
    const std::string dimensions = eyeDimensions(v.sizing, v.requested);
    // The value column carries pixels. Distinct eyes are expanded in the
    // always-visible selected-row hint; keep the explicitly labelled left
    // eye here so neither number is clipped by the fixed-width column.
    const size_t rightEye = dimensions.find(" R");
    return dimensions.substr(0, rightEye);
}

// Why the row has no headset to key on. On the native path the host has
// not published its sizing yet (the per-tick refresh picks it up within a
// second of VR init, so "a moment" is honest); off the native path no
// query ever runs and the key is inert for the whole session, which is
// said plainly so nobody there waits, retries or hunts for a publish that
// never comes. nativeMenuActive() is the predicate the Status page's
// Runtime line already uses.
constexpr const char* kNoHeadsetSizing = "No headset sizing published yet; try again in a moment.";
constexpr const char* kNotNativeWrite = "not written: native OpenXR only";
const char* noHeadsetWriteText() { return nativeMenuActive() ? kNoHeadsetSizing : kNotNativeWrite; }

// The two-line hint under the rows. When the eyes differ the megapixels and
// percent move to the tooltip and the hint keeps the L/R form, because the
// longest distinct-eye hint already fills the box (measured 2026-09-14).
std::string openxrResolutionHint(const ResolutionView& v) {
    if (!v.headset) {
        return nativeMenuActive() ? "Headset sizing not published yet. Changes apply after a game restart."
                                  : "Native OpenXR only; this runtime ignores it.";
    }
    const uint32_t recW = v.sizing.eyes[0].originalWidth, recH = v.sizing.eyes[0].originalHeight;
    std::string hint;
    if (!v.entryWidth) {
        hint = "Not set for this headset: 100% = ";
        if (sameEyes(v)) {
            hint += dimensionsText(recW, recH) + " (" + megapixelsText(recW, recH) + ")";
            // Only after an entry was removed on a rig still running it:
            // the fresh case has nothing running but 100%. (Measured
            // 2026-09-14: two lines at 16384x16384; the distinct-eye form
            // with this clause would be three, so it leaves it to the
            // tooltip.)
            if (v.afterW[0] != v.sizing.activeWidth[0]) hint += ". Active: " + activeDimensions(v.sizing);
        } else {
            hint += eyeDimensions(v.sizing, 1.0f);
        }
        return hint + " / eye.";
    }
    hint = "After restart: ";
    if (sameEyes(v)) {
        hint += dimensionsText(v.afterW[0], v.afterH[0]) + " " + afterRestartFacts(v);
    } else {
        hint += eyeDimensions(v.sizing, v.requested);
    }
    return hint + ". Active: " + activeDimensions(v.sizing) + " / eye.";
}

// The tooltip's facts line, in place of the Text default (no range,
// `Shipped (empty)`).
const char* openxrResolutionFacts() {
    return "Range: a quarter to twice the recommended width, within runtime limits.  "
           "Shipped 100% (no entry).  Applies after a game restart.";
}

// "Saved for: " and the list, for any per-headset row's tooltip: at most four
// entries, so the ini prose after them is not pushed out of the 1024-byte
// popup (the log line carries the whole list). The entry in effect for the
// worn headset -- the exact key, or the runtime-only fallback -- is marked,
// and is always among the four shown: past the fourth it takes the last slot,
// so its marker cannot be hidden behind `and N more`. With an empty system
// token the runtime-only entry IS this headset's own (the key is the runtime
// alone), so it is not called a fallback.
std::string savedForText(const edvr::native_render::HeadsetEntry* entries, size_t count,
                         bool headset, const std::string& rt, const std::string& sys,
                         uint32_t matched, uint32_t value) {
    std::string body = "\nSaved for: ";
    if (!count || !entries) return body + "none.";
    size_t matchedAt = count;
    for (size_t i = 0; i < count && matchedAt == count; ++i) {
        const edvr::native_render::HeadsetEntry& e = entries[i];
        if (headset && e.runtime == rt && e.value == value &&
            ((matched == 1 && e.system == sys) || (matched == 2 && e.system.empty()))) {
            matchedAt = i;
        }
    }
    const size_t shown = (std::min)(count, static_cast<size_t>(4));
    for (size_t i = 0; i < shown; ++i) {
        const size_t at = (i + 1 == shown && matchedAt >= shown) ? matchedAt : i;
        if (i) body += ", ";
        body += edvr::native_render::formatHeadsetEntry(entries[at]);
        if (at == matchedAt) {
            body += (matched == 2 && !sys.empty()) ? " (this headset, runtime-only)"
                                                   : " (this headset)";
        }
    }
    if (count > shown) {
        char more[48];
        snprintf(more, sizeof(more), " and %u more (see edvr.ini)",
                 static_cast<unsigned>(count - shown));
        body += more;
    }
    return body + ".";
}

// The tooltip's body: the headset, the recommendation, the after-restart and
// active sizes, what Elite actually submits against what it was given, and
// the saved entries.
std::string openxrResolutionTooltip(const ResolutionView& v) {
    using namespace edvr::native_render;
    std::string body;
    if (!v.headset) {
        body = nativeMenuActive() ? "This headset: unknown (no headset sizing published yet)."
                                  : "This headset: not read (native OpenXR only; this runtime ignores the key).";
    } else {
        const uint32_t recW = v.sizing.eyes[0].originalWidth, recH = v.sizing.eyes[0].originalHeight;
        body = "This headset: " + v.key + " (" + v.labels.runtimeName + ", ";
        body += v.sys.empty() ? std::string("runtime reported no system name)")
                              : std::string(v.labels.systemName) + ")";
        body += ". Runtime recommends " + dimensionsText(recW, recH) + " per eye (" +
                megapixelsText(recW, recH) + "); 100% is that, not the panel.";
        char digits[16];
        snprintf(digits, sizeof(digits), "%u", v.width);
        if (!v.entryWidth) {
            body += "\nAfter restart: no entry, 100% = " + eyeDimensions(v.sizing, 1.0f) +
                    " per eye (" + megapixelsText(recW, recH) + ").";
        } else {
            body += std::string("\nAfter restart: ") + digits + " wide";
            if (v.matched == 2) body += " (runtime-only entry " + v.rt + ":" + digits + ")";
            body += " = " + eyeDimensions(v.sizing, v.requested) + " per eye " + afterRestartFacts(v) + ".";
        }
        body += "\nActive: " + activeDimensions(v.sizing) + ".";
        // What Elite submits, against the size it was GIVEN: while the
        // cull guard is adopting or live the host recommends the active
        // size times the guard's factor (native_cull_guard.h:98-102), so
        // dividing by the active size alone would read x1.24 at HMD
        // Quality 1.0. The factor is named rather than folded so both
        // numbers can be checked against the Status page's guard line.
        uint32_t ew = 0, eh = 0;
        if (eyeTextureSize(&ew, &eh) && ew && v.sizing.activeWidth[0]) {
            const CullGuardState g = decodeCullGuardState(cullGuardStatePacked());
            const double guard = g.stage ? 1.0 + g.hPerMille / 1000.0 : 1.0;
            const double given = static_cast<double>(v.sizing.activeWidth[0]) * guard;
            char line[120];
            if (guard > 1.0005) {
                snprintf(line, sizeof(line), "\nElite submits %ux%u (x%.2f of what it was given; guard x%.2f).",
                         ew, eh, static_cast<double>(ew) / given, guard);
            } else {
                snprintf(line, sizeof(line), "\nElite submits %ux%u (x%.2f of the active size).", ew, eh,
                         static_cast<double>(ew) / given);
            }
            body += line;
        }
    }
    body += savedForText(v.entries, v.entryCount, v.headset, v.rt, v.sys, v.matched,
                         v.entryWidth);
    return body;
}

// One Left/Right step: 100 px, on the grid of round hundreds (Sean's
// choice, 2026-09-15; the earlier 5%-of-recommendation grid gave 88 px on
// the Quest 3 and 248 on the Pimax and never landed on a number anyone
// would type). A press moves to the ADJACENT hundred in the direction
// pressed (3283 -> 3300 or 3200; the 1824 recommendation -> 1900 or 1800),
// `mult` hundreds with Shift, and the result is clamped to a quarter..twice
// the recommendation and to the effective cap over both eyes and both axes
// (the height can bind before maxWidth does). A press never moves against
// its own direction: Right from a width already above the clamp leaves it
// alone rather than writing the lower clamped value.
constexpr uint32_t kResolutionStep = 100;
uint32_t steppedResolution(const ResolutionView& v, int dir, int mult) {
    const uint32_t rec = v.sizing.eyes[0].originalWidth;
    const double count = static_cast<double>(mult > 0 ? mult : 1);
    const double pos = static_cast<double>(v.width) / kResolutionStep;
    const double target = dir > 0 ? std::floor(pos) + count : std::ceil(pos) - count;
    double snapped = target * kResolutionStep;
    const double lo = std::ceil(rec * 0.25);
    double hi = rec * 2.0;
    const uint32_t cap = edvr::native_render::effectiveWidthCap(v.sizing.eyes);
    if (cap && cap < hi) hi = cap;
    if (snapped < lo) snapped = lo;
    if (snapped > hi) snapped = hi;
    if (snapped < 1.0) snapped = 1.0;
    if (snapped > edvr::native_render::kResolutionWidthMax) snapped = edvr::native_render::kResolutionWidthMax;
    if (dir > 0 && snapped < v.width) return v.width;
    if (dir < 0 && snapped > v.width) return v.width;
    return static_cast<uint32_t>(snapped);
}

// A typed width for this row: digits only, 1..16384.
bool parseTypedWidth(const std::string& text, uint32_t* out) {
    std::string digits = text;
    while (!digits.empty() && digits.front() == ' ') digits.erase(digits.begin());
    while (!digits.empty() && digits.back() == ' ') digits.pop_back();
    if (digits.empty() || digits.size() > 5) return false;
    uint32_t width = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return false;
        width = width * 10 + static_cast<uint32_t>(c - '0');
    }
    if (width < 1 || width > edvr::native_render::kResolutionWidthMax) return false;
    if (out) *out = width;
    return true;
}

constexpr const char* kEightHeadsets = "Eight headsets saved; remove one in edvr.ini first.";
// A runtime whose name has no ASCII letter or digit yields an empty runtime
// token, which the grammar cannot key an entry on (mergeHeadsetEntry refuses
// it); said as such rather than as a full list.
constexpr const char* kNoRuntimeToken = "not written: the runtime's name yields no key";

// ---------------------------------------------------------------------------
// The generic per-headset row: fix.fov_trim_vertical, _outer and _nasal. One
// integer per headset, in the same list grammar the resolution uses, of which
// the row shows and edits only the worn headset's entry. A trim tuned on a
// Pimax is wrong on a Quest 3, which is what these lists exist for.
//
// 0 is "no entry": the value column reads 0, and the list never carries
// `key:0`, so a row put back to 0 leaves the file as if it had never been set.
struct HeadsetRowView {
    bool        headset = false;   // sizing coherent and labels valid
    std::string rt, sys, key;      // the worn tokens and rt/sys
    NativeRenderLabels labels{};
    edvr::native_render::HeadsetEntry entries[edvr::native_render::kHeadsetEntryMax];
    size_t      entryCount = 0;
    uint32_t    matched = 0;       // 0 none, 1 runtime/system, 2 runtime-only
    uint32_t    value = 0;         // the worn headset's entry, 0 when none
    uint32_t    lo = 0, hi = 0;    // what ONE entry may hold
};

// The bounds the ui: line declared. `headset` beside `range` is what says the
// range bounds one entry's value rather than the string, so this is where the
// generated lo/hi mean degrees.
uint32_t headsetRowLow(const MenuRowDef& d) {
    const int lo = d.lo[0] ? atoi(d.lo) : 0;
    return lo > 0 ? static_cast<uint32_t>(lo) : 0u;
}

uint32_t headsetRowHigh(const MenuRowDef& d) {
    const int hi = d.hi[0] ? atoi(d.hi) : 0;
    return hi > 0 ? static_cast<uint32_t>(hi) : edvr::native_render::kTrimDegreesMax;
}

HeadsetRowView headsetRowView(const MenuRowDef& d, const std::string& value) {
    using namespace edvr::native_render;
    HeadsetRowView v;
    v.lo = headsetRowLow(d);
    v.hi = headsetRowHigh(d);
    v.headset = currentHeadset(&v.rt, &v.sys, nullptr, nullptr, nullptr, &v.labels);
    v.entryCount = parseHeadsetEntries(value.c_str(), v.entries, nullptr, v.lo, v.hi);
    if (!v.headset) return v;
    v.key = headsetKey(v.rt, v.sys);
    v.value = resolveHeadsetValue(v.entries, v.entryCount, v.rt, v.sys, &v.matched);
    return v;
}

// The value column: the worn headset's entry, or `unknown` when no headset is
// published -- which no number could mean, and which the resolution row says
// the same way.
std::string headsetRowValue(const HeadsetRowView& v) {
    if (!v.headset) return "unknown";
    char text[16];
    snprintf(text, sizeof(text), "%u", v.value);
    return text;
}

// The always-visible hint under the rows: which key this row is keyed on, and
// what 0 does, so nobody looks for a "remove" the row does not have.
std::string headsetRowHint(const HeadsetRowView& v) {
    if (!v.headset) {
        return nativeMenuActive() ? "No headset is known yet; try again in a moment."
                                  : "Native OpenXR only; this runtime ignores it.";
    }
    return "This headset: " + v.key + ". " +
           (v.value ? "0 removes its entry." : "Not set for this headset; Left/Right sets it.");
}

// The tooltip's facts line, in place of the Text default (no range at all).
std::string headsetRowFacts(const MenuRowDef& d, const HeadsetRowView& v) {
    char text[160];
    snprintf(text, sizeof(text), "Range: %u to %u, for the headset you are wearing.  "
             "Shipped (no entry).  %s", v.lo, v.hi,
             d.applies == 1   ? "Applies at once."
             : d.applies == 2 ? "Takes effect at the next launch."
                              : "When it applies is not documented.");
    return text;
}

// The tooltip's body: the headset this row is keyed on, what it is set to,
// and every headset the list holds.
std::string headsetRowTooltip(const MenuRowDef& d, const HeadsetRowView& v) {
    std::string body;
    if (!v.headset) {
        body = nativeMenuActive()
            ? "This headset: unknown (nothing has published a headset yet)."
            : "This headset: not read (native OpenXR only; this runtime ignores the key).";
    } else {
        body = "This headset: " + v.key + " (" + v.labels.runtimeName + ", ";
        body += v.sys.empty() ? std::string("runtime reported no system name)")
                              : std::string(v.labels.systemName) + ")";
        char digits[16];
        snprintf(digits, sizeof(digits), "%u", v.value);
        if (!v.value) {
            body += std::string(". No entry for it, so ") + d.label + " does nothing here.";
        } else {
            body += std::string(". Set to ") + digits;
            if (v.matched == 2) body += " (runtime-only entry " + v.rt + ":" + digits + ")";
            body += ".";
        }
        body += "\n0 removes this headset's entry and leaves every other headset's alone.";
    }
    body += savedForText(v.entries, v.entryCount, v.headset, v.rt, v.sys, v.matched, v.value);
    return body;
}

// One Left/Right step: 1, or `mult` with Shift, held inside the row's own
// bounds. A press never moves against its own direction.
uint32_t steppedHeadsetValue(const HeadsetRowView& v, int dir, int mult) {
    const int64_t count = mult > 0 ? mult : 1;
    int64_t next = static_cast<int64_t>(v.value) + (dir > 0 ? count : -count);
    if (next < static_cast<int64_t>(v.lo)) next = static_cast<int64_t>(v.lo);
    if (next > static_cast<int64_t>(v.hi)) next = static_cast<int64_t>(v.hi);
    return static_cast<uint32_t>(next);
}

// A typed value for this row: digits only, within the row's bounds. Never the
// list -- the buffer holds one headset's number.
bool parseTypedHeadsetValue(const MenuRowDef& d, const std::string& text, uint32_t* out) {
    std::string digits = text;
    while (!digits.empty() && digits.front() == ' ') digits.erase(digits.begin());
    while (!digits.empty() && digits.back() == ' ') digits.pop_back();
    if (digits.empty() || digits.size() > 5) return false;
    uint32_t value = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    if (value < headsetRowLow(d) || value > headsetRowHigh(d)) return false;
    if (out) *out = value;
    return true;
}

// Whether a row that needs a restart has an on-disk value the running game
// does not have. A per-headset list is compared on the WORN headset's value,
// never as text: a hand edit to another headset's entry, or a canonicalising
// rewrite of the list, changes nothing here and must badge nothing.
bool rowPending(const MenuRowDef& d, const std::string& value, const std::string& snapshot) {
    if (isOpenxrRenderScaleRow(d)) return renderScaleRowPending(value);
    if (isHeadsetRow(d)) {
        const HeadsetRowView now = headsetRowView(d, value);
        return now.headset && now.value != headsetRowView(d, snapshot).value;
    }
    return value != snapshot;
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

const char* nvidiaLabel() {
    if (runtimeFlatProfile()) return flatRuntimeNativeScale() ? "DLAA" : "DLSS";
    float quality = 0.0f;
    deviceHookHmdQuality(&quality);
    return temporalNvidiaLabel(quality);
}

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
            if (strcmp(d.section, "fix") == 0 && strcmp(d.key, "temporal_aa") == 0 && c.value == "dlss") {
                c.label = nvidiaLabel();
            }
            if (runtimeFlatProfile() && strcmp(d.section, "fix") == 0 &&
                strcmp(d.key, "temporal_aa") == 0) {
                if (c.value == "off") c.label = "Off";
                else if (c.value == "on") c.label = "TAA";
                else if (c.value == "dlss") c.label = nvidiaLabel();
                else if (c.value == "fsr") c.label = "FSR3";
            }
            out.push_back(c);
        }
        pos = bar + 1;
    }
    return out;
}

// A two-way choice where one side is "leave the game alone" reads better as
// a switch than as a word to cycle: on is the fix, off is stock. The
// installer's own rule (settings_view.cpp's twoChoiceToggle), so the two
// settings windows agree about which rows are switches. The value written
// is still the choice string; only the control changes.
bool twoChoiceSwitch(const MenuRowDef& d, std::string* onValue, std::string* offValue) {
    if (d.kind != MenuKind::Choice) return false;
    const std::vector<ChoiceItem> items = choicesOf(d);
    if (items.size() != 2) return false;
    int off = -1;
    for (int i = 0; i < 2; ++i) {
        if (items[i].value == "stock" || items[i].value == "off") off = i;
    }
    if (off < 0) return false;
    if (onValue) *onValue = items[1 - off].value;
    if (offValue) *offValue = items[off].value;
    return true;
}

// What a switch is SET to, in the file's own word -- suppressed when the
// word would only repeat the switch (a plain on/off pair), kept when it
// would not (on writes "steady", off writes "stock").
std::string switchWord(const MenuRowDef& d, const std::string& value) {
    std::string on, off;
    if (!twoChoiceSwitch(d, &on, &off)) return std::string();
    if (on == "on" && off == "off") return std::string();
    const std::string current = _stricmp(value.c_str(), on.c_str()) == 0 ? on : off;
    for (const ChoiceItem& c : choicesOf(d)) {
        if (_stricmp(c.value.c_str(), current.c_str()) == 0) return c.label;
    }
    return current;
}

std::string displayValue(const MenuRowDef& d, const std::string& v) {
    if (strcmp(d.section, "fix") == 0 && strcmp(d.key, "temporal_aa") == 0 && _stricmp(v.c_str(), "dlaa") == 0) {
        return "DLAA"; // explicit legacy 1:1 mode, even below HMD Quality 1
    }
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

// THE WRITE RUNS OFF THE FRAME THREAD. The first build did the read, the
// merge, the write, the replace, the mirror copy and the backup copy on
// the render thread -- a several-millisecond hitch on every change, which
// is exactly the class of drop the Monitor page exists to attribute. Now
// the frame thread enqueues and shows the value at once; a worker does the
// I/O, serialised, coalescing repeated changes to one key while a key is
// held; and the frame thread drains the results, asking for the reload
// that applies each landed write.
struct WriteJob {
    int         def;
    std::string dotted;
    std::string value;
    std::string before;
    // Per-headset rows only: the worn headset's key and the values either
    // side, so the log and the last-write line name what changed rather than
    // printing two lists; `dropped` names malformed tokens the merge left out
    // of the list.
    std::string headset;
    std::string fromValue;
    std::string toValue;
    std::string dropped;
};
struct WriteDone {
    WriteJob    job;
    bool        ok;
    std::string err;
};
struct Writer {
    std::mutex              m;
    std::condition_variable cv;
    std::thread             thread;
    bool                    started = false;
    bool                    quit = false;
    std::vector<WriteJob>   queue;
    std::vector<WriteDone>  done;
};
// Like the panel worker, this may still be joinable when Windows terminates
// the other threads before DLL detach. Do not register a thread destructor
// with the CRT; stopWriter remains the normal explicit join path.
Writer& g_writer = *new Writer;

void writerMain() {
    for (;;) {
        std::vector<WriteJob> jobs;
        {
            std::unique_lock<std::mutex> lock(g_writer.m);
            g_writer.cv.wait(lock, [] { return g_writer.quit || !g_writer.queue.empty(); });
            if (g_writer.quit) return;
            jobs.swap(g_writer.queue);
        }
        // Coalesce: the last value for each key is the one that matters.
        std::vector<WriteJob> last;
        for (size_t i = 0; i < jobs.size(); ++i) {
            bool later = false;
            for (size_t j = i + 1; j < jobs.size(); ++j) {
                if (jobs[j].dotted == jobs[i].dotted) {
                    later = true;
                    break;
                }
            }
            if (!later) last.push_back(jobs[i]);
        }
        for (const WriteJob& job : last) {
            WriteDone d;
            d.job = job;
            d.ok = false;
            guarded("menu/write", [&] { d.ok = menuIniWrite(job.dotted, job.value, &d.err); });
            if (!d.ok && d.err.empty()) d.err = "the write faulted";
            std::lock_guard<std::mutex> lock(g_writer.m);
            g_writer.done.push_back(d);
        }
    }
}

void enqueueWrite(const WriteJob& job) {
    std::lock_guard<std::mutex> lock(g_writer.m);
    if (!g_writer.started) {
        g_writer.started = true;
        g_writer.quit = false;
        g_writer.thread = std::thread(writerMain);
    }
    g_writer.queue.push_back(job);
    g_writer.cv.notify_one();
}

void stopWriter() {
    {
        std::lock_guard<std::mutex> lock(g_writer.m);
        g_writer.quit = true;
        g_writer.cv.notify_one();
    }
    if (g_writer.started && g_writer.thread.joinable()) g_writer.thread.join();
    g_writer.started = false;
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
    // The page being read keeps its place through a rebuild. The developer
    // switch sits at the BOTTOM of the Performance page and rebuilds the
    // pages when flipped; without this the highlight jumped back to the
    // first row under the hand that had just reached the last one.
    const char* keepName = s.pages.empty() ? "" : s.pages[s.page].name;
    const int keepHighlight = s.pages.empty() ? 0 : s.pages[s.page].highlight;
    const int keepScroll = s.pages.empty() ? 0 : s.pages[s.page].scroll;
    const size_t keepCount = s.pages.empty() ? 0 : s.pages[s.page].entries.size();
    s.pages.clear();
    if (runtimeFlatProfile()) {
        Page p;
        p.name = "Flat graphics";
        for (int i = 0; i < kRowDefCount; ++i) {
            const MenuRowDef& d = kMenuRows[i];
            if (strcmp(d.section, "fix") != 0) continue;
            if (strcmp(d.key, "temporal_aa") != 0 &&
                strcmp(d.key, "temporal_aa_model") != 0) continue;
            Entry e;
            e.kind = EntryKind::Setting;
            e.def = i;
            p.entries.push_back(e);
        }
        firstSelectable(p);
        s.pages.push_back(p);
        s.page = 0;
        s.developerBuilt = s.developer;
        s.contentDirty = true;
        return;
    }
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
        p.name = "Monitor";
        p.status = true;
        p.monitor = true;
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
    // Same page, same rows: put the highlight and the scroll back. A page
    // whose rows changed (an action registered on Instruments) starts at
    // its first row as before.
    {
        Page& p = s.pages[s.page];
        if (strcmp(p.name, keepName) == 0 && p.entries.size() == keepCount &&
            keepHighlight >= 0 && keepHighlight < static_cast<int>(p.entries.size())) {
            p.highlight = keepHighlight;
            p.scroll = keepScroll;
        }
    }
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
    statusLine(c, "Runtime",
               nativeMenuActive() ? "native OpenXR (experimental)"
               : (glitchConsumerPresent() ? "not identified" : "no compositor consumer"));
    if (!nativeMenuActive()) {
        // Nothing else publishes a runtime kind now that the legacy OpenVR
        // proxy is gone, so the three lines below would read `unknown` for
        // the whole session and the row's refusals would promise a publish
        // that never comes; one line says why instead.
        statusLine(c, "Headset", "native OpenXR only (not in use on this runtime)");
    } else {
        // The worn headset as the native host named it: the raw names for
        // eyes, the sanitised key (at most 61 bytes, never cut) for a user
        // with edvr.ini open, and the recommendation the widths are
        // measured against. `unknown` before the first query.
        NativeRenderLabels labels{};
        EdvrNativeRenderSizing sizing{};
        if (nativeRenderLabels(&labels)) {
            if (labels.systemName[0]) {
                snprintf(buf, sizeof(buf), "%s via %s", labels.systemName, labels.runtimeName);
            } else {
                snprintf(buf, sizeof(buf), "%s (runtime reported no system name)", labels.runtimeName);
            }
            statusLine(c, "Headset", buf);
            statusLine(c, "Headset key",
                       edvr::native_render::headsetKey(labels.runtimeToken, labels.systemToken).c_str());
        } else {
            statusLine(c, "Headset", "unknown");
            statusLine(c, "Headset key", "unknown");
        }
        if (readNativeRenderSizing(&sizing)) {
            snprintf(buf, sizeof(buf), "%ux%u / eye (%.1f MP)", sizing.eyes[0].originalWidth,
                     sizing.eyes[0].originalHeight,
                     edvr::native_render::megapixels(sizing.eyes[0].originalWidth,
                                                     sizing.eyes[0].originalHeight));
        } else {
            snprintf(buf, sizeof(buf), "not published yet");
        }
        statusLine(c, "Recommended", buf);
    }
    if (nativeMenuActive()) statusLine(c, "Native effects", "Menu, temporal and sharpening connected; other effects pending");
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
        uint32_t rn = 0;
        double rmx = 0.0, fullL = 0.0, fullR = 0.0, centreL = 0.0, centreR = 0.0,
               periphL = 0.0, periphR = 0.0;
        const bool haveFull = temporalPassDlaaFullTotals(0, &rn, &fullL, &rmx) &&
                              temporalPassDlaaFullTotals(1, &rn, &fullR, &rmx);
        const bool haveCentre = temporalPassDlaaCentreTotals(0, &rn, &centreL, &rmx) &&
                                temporalPassDlaaCentreTotals(1, &rn, &centreR, &rmx);
        const bool havePeriph = temporalPassDlaaPeripheryTotals(0, &rn, &periphL, &rmx) &&
                                temporalPassDlaaPeripheryTotals(1, &rn, &periphR, &rmx);
        // The current engine, not whichever one has a count (F6). The three
        // per-role figures above are NGX's alone, and they keep answering
        // after a live switch to fsr -- so they are shown only while NVIDIA
        // is the engine in force.
        const char* engineWord = "NVIDIA";
        bool amdEngine = false;
        const bool haveTrained =
            temporalPassTrainedTotals(&n, &avg, &mx, &resets, &engineWord, &amdEngine) && n;
        if (!amdEngine && (haveFull || haveCentre || havePeriph)) {
            // Per role, per eye: the pooled figure below mixed roles and eyes
            // into one number and called it "ms/eye", which it was not.
            size_t len = static_cast<size_t>(snprintf(buf, sizeof(buf), "%s, NVIDIA", mode.c_str()));
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            if (haveFull) {
                len += static_cast<size_t>(snprintf(buf + len, sizeof(buf) - len,
                                                    " L %.2f R %.2f ms", fullL, fullR));
                if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            }
            if (haveCentre) {
                len += static_cast<size_t>(snprintf(buf + len, sizeof(buf) - len,
                                                    " centre L %.2f R %.2f ms", centreL, centreR));
                if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            }
            if (havePeriph) {
                len += static_cast<size_t>(snprintf(buf + len, sizeof(buf) - len,
                                                    " periphery L %.2f R %.2f ms", periphL, periphR));
                if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            }
        } else if (haveTrained) {
            snprintf(buf, sizeof(buf), "%s, %s %.2f ms/eye", mode.c_str(), engineWord, avg);
        } else if (temporalPassTotals(&n, &avg, &mx, &rej, &clip) && n) {
            snprintf(buf, sizeof(buf), "%s, %.2f ms/eye", mode.c_str(), avg);
        } else {
            snprintf(buf, sizeof(buf), "%s", mode.c_str());
        }
        statusLine(c, "Temporal AA", buf);
    }
    {
        // Stage 0 price report (docs/foveated-dlss-design-2026-09-14.md):
        // the last CLOSED window's per-region median, stereo-pair-summed.
        // Absent until 600 pairs have run once, or a treatment/size/format/
        // setting change closed a shorter window early.
        double regionMs[7] = {};
        double otherMs = 0.0;
        uint32_t pairs = 0;
        uint32_t dropped = 0;
        if (temporalPassPriceWindow(regionMs, &otherMs, &pairs, &dropped)) {
            snprintf(buf, sizeof(buf),
                     "%u pairs, median ms prep %.2f reduce %.2f periphery %.2f "
                     "centre %.2f full %.2f compose %.2f ui %.2f other %.2f, "
                     "%u dropped",
                     pairs, regionMs[0], regionMs[1], regionMs[2], regionMs[3],
                     regionMs[4], regionMs[5], regionMs[6], otherMs, dropped);
            statusLine(c, "Temporal AA price", buf);
        }
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
        char line[240];
        inputGateStatusLine(line, sizeof(line));
        statusLine(c, "Keyboard", line);
    }
    {
        // Elite's panel keys the menu answers to, or why it does not. The
        // gate is never private on THIS page by design, so the held-back
        // test is the door evidence alone (inputGateGameKeyboardSeen), not
        // the alias predicate, which would read "held back" here always.
        const State& s = g_s;
        char v[64];
        if (s.aliasSource == 0) {
            snprintf(v, sizeof(v), "not read yet");
        } else if (s.aliasSource == 1) {
            snprintf(v, sizeof(v), "off (read_game_bindings)");
        } else if (s.aliasSource == 2) {
            snprintf(v, sizeof(v), "no bindings files found");
        } else if (s.aliasSource == 3) {
            snprintf(v, sizeof(v), "none adopted (see log)");
        } else if (!s.privateWanted || !inputGateGameKeyboardSeen()) {
            // The page pair follows Tab, so it works in shared mode and on
            // a rig whose game keyboard has not reached a door; only the
            // rest is off or held back.
            bool pageAdopted = false;
            for (int i = 0; i < s.aliases.count; ++i) {
                pageAdopted |= menuAliasFollowsTab(s.aliases.alias[i].nav);
            }
            if (!s.privateWanted) {
                snprintf(v, sizeof(v), "%s (keys shared)", pageAdopted ? "page keys only" : "off");
            } else {
                snprintf(v, sizeof(v), "%sheld back (see log)", pageAdopted ? "page keys only; rest " : "");
            }
        } else {
            menuAliasStatusValue(s.aliases, v, sizeof(v));
        }
        statusLine(c, "Elite keys", v);
    }
    {
        int w = 0, h = 0;
        double ms = 0.0;
        float gpu = 0.0f;
        if (menuPanelStats(&w, &h, &ms, &gpu)) {
            // The bitmap is wider than the card when the tooltip's strip is
            // in it, so say which number is which.
            const int card =
                g_s.tooltipDelayS > 0.0f ? static_cast<int>(w / kTipRatio + 0.5f) : w;
            snprintf(buf, sizeof(buf), "%dx%d bitmap (%d card), %.1f ms to draw, %.2f ms/eye GPU", w,
                     h, card, ms, static_cast<double>(gpu));
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

// The Monitor page: fpsVR's readout from perf_monitor.h, and the frame-time
// strip underneath.
void buildMonitor(MenuContent& c) {
    PerfTile tiles[kMenuMaxTiles];
    const int n = perfMonitorTiles(tiles, kMenuMaxTiles);
    for (int i = 0; i < n; ++i) {
        strncpy(c.tiles[i].caption, tiles[i].caption, sizeof(c.tiles[i].caption) - 1);
        strncpy(c.tiles[i].value, tiles[i].value, sizeof(c.tiles[i].value) - 1);
        strncpy(c.tiles[i].sub, tiles[i].sub, sizeof(c.tiles[i].sub) - 1);
    }
    c.tileCount = n;
    c.tileColumns = 4;
    // The local render-to-submit diagnostic is separate from SteamVR's gauges.
    {
        char line[200];
        perfMonitorLocalGpuLine(line, sizeof(line));
        MenuLine& l = c.lines[c.lineCount++];
        strncpy(l.left, line, sizeof(l.left) - 1);
        l.style = kMenuNote;
        l.badge = kBadgeNone;
    }
    if (nativeMenuActive()) {
        char line[240];
        perfMonitorNativeTimingLine(line, sizeof(line));
        MenuLine& l = c.lines[c.lineCount++];
        strncpy(l.left, line, sizeof(l.left) - 1);
        l.style = kMenuNote;
        l.badge = kBadgeNone;
    }
    // One line under the gauges: the last drop and what EDVR was doing.
    {
        char line[200];
        perfMonitorLastDropLine(line, sizeof(line));
        MenuLine& l = c.lines[c.lineCount++];
        strncpy(l.left, line, sizeof(l.left) - 1);
        l.style = kMenuNote;
        l.badge = kBadgeNone;
    }
    c.compact = true;
    // Two strips, fpsVR's pair: the GPU frame the compositor measured, and
    // the render thread's own busy time.
    const int kinds[2] = {kGraphGpu, kGraphCpu};
    const char* names[2] = {"GPU", "CPU"};
    const bool nativeGraphs = nativeMenuActive();
    c.graphCount = 0;
    for (int g = 0; g < 2; ++g) {
        MenuGraph& mg = c.graphs[c.graphCount];
        mg.count = perfMonitorGraph(kinds[g], mg.samples,
                                    static_cast<int>(sizeof(mg.samples) / sizeof(mg.samples[0])),
                                    &mg.budgetMs);
        if (!mg.count) continue;
        mg.zeroIsValid = nativeGraphs;
        if (nativeGraphs && mg.budgetMs > 0.0f)
            snprintf(mg.label, sizeof(mg.label), "%s, %d samples; predicted period %.1f ms",
                     g == 0 ? "Producer GPU" : "Submit wall", mg.count, double(mg.budgetMs));
        else if (nativeGraphs)
            snprintf(mg.label, sizeof(mg.label), "%s, %d samples; no runtime reference",
                     g == 0 ? "Producer GPU" : "Submit wall", mg.count);
        else snprintf(mg.label, sizeof(mg.label), "%s, last %d frames; the line is the %.1f ms budget",
                 names[g], mg.count, static_cast<double>(mg.budgetMs));
        ++c.graphCount;
    }
}

// ---------------------------------------------------------------------------
// Content

// THE TOOLTIP'S STRIP (the fractions live in menu_panel.h, because the
// raster draws from the same numbers the model reserves). The bitmap is
// wider than the menu card by a gap and a tooltip's width. The strip is
// transparent when no tooltip is up, so the panel's size never changes as
// one comes and goes -- and the whole bitmap is SLID along its own surface
// so the CARD lands where the user was looking rather than the bitmap's
// middle.
bool tooltipsOn() { return g_s.tooltipDelayS > 0.0f; }

// The panel's half-width in METRES: the card exactly as wide as
// width_degrees asks, plus the strip beside it. Scaled in metres and not in
// degrees, because the bitmap maps linearly onto the surface while tan does
// not -- multiplying the ANGLE by the ratio would draw the card 3.6% wider
// than it was asked for at the default, and 9% at width_degrees 45.
float panelHalfW(float widthDeg, bool withTip) {
    const float card = g_s.distance * tanf(widthDeg * 0.5f * 0.0174532925f);
    return withTip ? card * kTipRatio : card;
}

// How far to slide the bitmap along its own surface so the CARD's middle
// sits on the anchor's forward. The card is the bitmap's left part, so the
// surface moves right by the difference.
//
// This is a shift along the surface and NOT a turn of the anchor. Turning
// the anchor would put the card's middle in the right direction but leave
// it facing the old one: at the default ratio the card would be seen 9
// degrees oblique, its left edge 1.50 m away and its right 1.41 m. A shift
// leaves the card exactly where it was before the strip existed -- same
// width, same distance, square to the look -- and, being recomputed every
// frame rather than latched at summon, it cannot go stale when the ini is
// edited with the panel up.
float panelShift(float halfW, bool withTip) {
    return withTip ? halfW * (kTipRatio - 1.0f) / kTipRatio : 0.0f;
}

// The bitmap's size, from the headset's own pixels per degree at the
// panel: the panel is specified in degrees, so it reads the same size in
// any headset and composites near 1:1. `withTip` adds the strip.
void sizeContent(MenuContent& c, float widthDeg, float textDeg, bool withTip = false) {
    if (runtimeFlatProfile()) {
        c.cardPx = c.widthPx = 860;
        c.capPx = 34;
        return;
    }
    float ppd = 45.0f;
    uint32_t ew = 0, eh = 0;
    float outer = 0.0f, inner = 0.0f;
    if (eyeTextureSize(&ew, &eh) && eyeTangents(&outer, &inner) && ew > 0) {
        const float spanDeg = (atanf(outer) + atanf(inner)) * 57.2957795f;
        if (spanDeg > 20.0f) ppd = static_cast<float>(ew) / spanDeg;
    }
    if (ppd < 12.0f) ppd = 12.0f;
    if (ppd > 80.0f) ppd = 80.0f;
    int card = static_cast<int>(ppd * widthDeg + 0.5f);
    if (card < 480) card = 480;
    if (card > 1600) card = 1600;
    // The ratio is applied AFTER the card is clamped, so the strip is
    // always exactly its fraction of the card and the anchor's offset,
    // which is computed from the ratio alone, stays exact.
    c.cardPx = card;
    c.widthPx = withTip ? static_cast<int>(card * kTipRatio + 0.5f) : card;
    c.capPx = static_cast<int>(ppd * textDeg + 0.5f);
    if (c.capPx < 10) c.capPx = 10;
    if (c.capPx > 80) c.capPx = 80;
}

// The footer composer's ruler: the raster's own DrawTextW at the footer's
// em, which is the content's cap (menu_panel.cpp, Font::Small).
int measureFooterLine(const char* utf8, void* ctx) {
    return menuPanelMeasureLine(utf8, static_cast<const MenuContent*>(ctx)->capPx);
}

void buildContent(MenuContent& c) {
    State& s = g_s;
    memset(&c, 0, sizeof(c));
    c.popupLine = -1;
    sizeContent(c, s.widthDeg, s.textDeg, tooltipsOn());

    // If the FPS overlay is locked to the menu, show the same readout at the
    // top of the menu panel so both are visible for A/B testing.
    if (s.overlay && s.overlayLock) {
        char line[120];
        perfMonitorOverlayLine(line, sizeof(line));
        strncpy(c.overlayLine, line, sizeof(c.overlayLine) - 1);
        c.overlayLine[sizeof(c.overlayLine) - 1] = 0;
    }

    // The tab strip: a window of pages that fits the panel, always
    // including the current one, with an arrow at whichever end has more.
    // Developer mode adds four pages, and the strip ran off the edge --
    // the tabs beyond it could not be seen, so nothing said they existed.
    {
        const int total = static_cast<int>(s.pages.size());
        const int pad = c.capPx * 8 / 10;
        const int gap = c.capPx / 2;
        const int arrow = c.capPx;    // the room an arrow takes at either end
        // The panel's own estimate of a tab's width, the raster's formula.
        auto tabWidth = [&](int i) {
            return static_cast<int>(strlen(s.pages[i].name)) * c.capPx * 6 / 10 + c.capPx;
        };
        int first = 0, last = total - 1;
        int room = c.cardPx - 2 * pad;
        // Grow outward from the current page while there is room, ending
        // before the arrows that say what is left over.
        first = last = s.page;
        room -= tabWidth(s.page);
        for (bool grew = true; grew;) {
            grew = false;
            if (last + 1 < total) {
                const int need = tabWidth(last + 1) + gap + (last + 2 < total ? arrow : 0);
                if (need <= room) {
                    room -= tabWidth(last + 1) + gap;
                    ++last;
                    grew = true;
                }
            }
            if (first > 0) {
                const int need = tabWidth(first - 1) + gap + (first - 1 > 0 ? arrow : 0);
                if (need <= room) {
                    room -= tabWidth(first - 1) + gap;
                    --first;
                    grew = true;
                }
            }
        }
        if (last - first + 1 > kMenuMaxTabs) last = first + kMenuMaxTabs - 1;
        for (int i = first; i <= last; ++i) {
            strncpy(c.tabs[c.tabCount++], s.pages[i].name, sizeof(c.tabs[0]) - 1);
        }
        c.activeTab = s.page - first;
        c.tabMoreLeft = first > 0;
        c.tabMoreRight = last < total - 1;
    }
    Page& p = s.pages[s.page];
    const int pendingN = pendingRestartCount();
    // Whether Elite's adopted keys ACT on this page right now (menu_keys.h,
    // the one predicate). The legend, the tooltip and the reminder line
    // all follow this rather than mere adoption, so the panel never names
    // a key that would do nothing -- or worse, would reach the ship.
    const bool gateHolds = inputGateHoldsGameKeyboard();
    const bool aliasesLive = s.aliases.adopted && menuAliasMayAct(gateHolds, s.editEntry >= 0, p.status);
    s.aliasesLiveShown = s.aliases.adopted && menuAliasMayAct(gateHolds, false, p.status);
    const char* navName[kNavCount];
    for (int i = 0; i < kNavCount; ++i) navName[i] = s.aliases.navName[i];

    if (p.monitor) {
        // No hint line: the tiles carry their own captions, and the height
        // is better spent on them.
        buildMonitor(c);
    } else if (p.status) {
        buildStatus(c);
        // Fifteen lines on the native path. At the raster's row pitch of
        // two caps the bitmap's 2048-px height guard (menu_panel.cpp,
        // rasterise) trips from a 51-px cap, which is the Pimax at the
        // default 1.1 deg (~46-50 ppd); the Monitor page's pitch of 1.7
        // moves that to 56, and the guard now logs when it does trip.
        c.compact = true;
        snprintf(c.hint, sizeof(c.hint), "%s",
                 "The page a support thread will ask to see. Tab or PageDown for the next page.");
    } else {
        // The tooltip waits for the look or the hand to settle on one row:
        // it is an explanation for someone who has stopped, not something
        // to flick past.
        const bool showTip =
            tooltipsOn() &&
            s.tickMs >= s.highlightSinceMs + static_cast<uint64_t>(s.tooltipDelayS * 1000.0f);
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
            l.dim = false;
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
            const bool editingThis = (s.editEntry == i);
            const bool dlssRow = isDlssPresetRow(d);
            const bool dim = dlssRow && dlssPresetDisabled();
            l.dim = dim;
            strncpy(l.left, d.label, sizeof(l.left) - 1);
            if (dlssRow) {
                const std::string mode = runtimeFlatProfile() ? Config::get().requestedTemporalMode()
                                                              : Config::get().getString("fix.temporal_aa", "off");
                snprintf(l.left, sizeof(l.left), "%s preset", _stricmp(mode.c_str(), "dlaa") == 0 ? "DLAA" : nvidiaLabel());
            }
            if (runtimeFlatProfile() && strcmp(d.section, "fix") == 0 &&
                strcmp(d.key, "temporal_aa") == 0) {
                strncpy(l.left, "Anti-aliasing", sizeof(l.left) - 1);
            }
            const bool openxrScaleRow = isOpenxrRenderScaleRow(d);
            const bool headsetRow = isHeadsetRow(d);
            // The worn headset's entry, resolved once for every site of this
            // row below (label, value, hint, tooltip, reset prompt).
            const ResolutionView res = openxrScaleRow ? resolutionView(r.value) : ResolutionView{};
            const HeadsetRowView headsetValue =
                headsetRow ? headsetRowView(d, r.value) : HeadsetRowView{};
            if (openxrScaleRow) {
                snprintf(l.left, sizeof(l.left), "%s", openxrResolutionLabel(res).c_str());
            }
            std::string v = openxrScaleRow  ? openxrResolutionValue(res)
                            : headsetRow    ? headsetRowValue(headsetValue)
                                            : displayValue(d, r.value);
            if (editingThis) {
                // What has been typed, with a caret. The raster shows a
                // typed row in the value colour whatever its kind.
                v = s.editBuf + "_";
            } else if (r.pending) {
                // The resolution preview already describes the selected
                // after-restart value.  The pending badge supplies the same
                // timing cue as every other restart row, while keeping the
                // per-eye dimensions readable.
                if (!openxrScaleRow && !headsetRow) v = displayValue(d, r.snapshot) + " -> " + v;
                l.badge = kBadgePending;
            } else if (d.applies == 2) {
                l.badge = kBadgeRestart;
            } else if (d.applies == 0) {
                l.badge = kBadgeUnknown;
            }
            strncpy(l.right, v.c_str(), sizeof(l.right) - 1);
            // Anything with two states is a switch, drawn where the value
            // would be: a plain boolean, and a two-way choice where one
            // side is "leave the game alone". The installer's own rule.
            // A row with a change waiting for a restart keeps its words,
            // because a switch cannot show two values at once.
            if (!editingThis && !r.pending) {
                std::string on;
                if (d.kind == MenuKind::Toggle) {
                    l.toggle = boolOf(r.value, boolOf(d.shipped, false)) ? 2 : 1;
                    l.right[0] = 0;
                } else if (twoChoiceSwitch(d, &on, nullptr)) {
                    l.toggle = _stricmp(r.value.c_str(), on.c_str()) == 0 ? 2 : 1;
                    // The word stays only where it says something the
                    // switch does not.
                    const std::string word = switchWord(d, r.value);
                    strncpy(l.right, word.c_str(), sizeof(l.right) - 1);
                    l.right[sizeof(l.right) - 1] = 0;
                }
            }
            l.style = editingThis ? kMenuRowEdit
                      : hi        ? kMenuRowHi
                      : dim       ? kMenuDim
                                  : kMenuRow;
            if (hi && dim && runtimeFlatProfile())
                snprintf(c.hint, sizeof(c.hint), "Choose DLSS to change its model preset.");
            if (hi && openxrScaleRow) {
                snprintf(c.hint, sizeof(c.hint), "%s", openxrResolutionHint(res).c_str());
            }
            if (hi && headsetRow) {
                snprintf(c.hint, sizeof(c.hint), "%s", headsetRowHint(headsetValue).c_str());
            }
            if (hi && isVscreenResolutionRow(d)) {
                // announce=false: this runs every frame the row is highlighted,
                // not once at startup, so the resolver must stay silent here.
                uint32_t vw = 0, vh = 0;
                resolveVScreenTargetResolution(Config::get(), &vw, &vh, /*announce=*/false);
                if (vw && vh) {
                    snprintf(c.hint, sizeof(c.hint), "Currently resolves to %ux%u.", vw, vh);
                } else {
                    snprintf(c.hint, sizeof(c.hint),
                             "No runtime width on record yet; the panel stays at the "
                             "game's own 1920x1080 until a session with VR running has "
                             "completed once.");
                }
            }
            if (hi && showTip) {
                // The tooltip beside the row: what the ini says about this
                // key, in the ini's own words, plus the facts a person
                // needs to type a value -- the range, the default, and
                // whether the change waits for a restart.
                c.popupLine = c.lineCount - 1;
                c.popupScroll = s.tooltipScroll;
                snprintf(c.popupTitle, sizeof(c.popupTitle), "%s.%s", d.section, d.key);
                // THE FACTS FIRST, THE INI'S PROSE LAST. Some comment
                // blocks run to three thousand characters, and this buffer
                // holds 1024 bytes (MenuContent.popup); if the prose led,
                // the truncation ate exactly the four things somebody about
                // to change a value needs -- its range, its shipped value,
                // when it applies, and which key does it.
                std::string body;
                if (openxrScaleRow) {
                    body += openxrResolutionFacts();
                } else if (headsetRow) {
                    body += headsetRowFacts(d, headsetValue);
                } else if (isVscreenResolutionRow(d) && d.lo[0] && d.hi[0]) {
                    body += std::string("\"auto\", or a width in pixels from ") + d.lo +
                            " to " + d.hi + ". The height always follows it at 16:9.  ";
                } else if (d.kind == MenuKind::Number && d.lo[0] && d.hi[0]) {
                    body += std::string("Range ") + d.lo + " to " + d.hi + ".  ";
                } else if (d.kind == MenuKind::Choice) {
                    std::string list;
                    for (const ChoiceItem& ch : choicesOf(d)) {
                        if (!list.empty()) list += ", ";
                        list += ch.label;
                    }
                    if (!list.empty()) body += "Choices: " + list + ".  ";
                }
                if (!openxrScaleRow && !headsetRow) {
                    body += std::string("Shipped ") + displayValue(d, d.shipped) + ".  " +
                            (d.applies == 1   ? "Applies at once."
                             : d.applies == 2 ? "Takes effect at the next launch."
                                              : "When it applies is not documented.");
                }
                if (openxrScaleRow) body += "\n" + openxrResolutionTooltip(res);
                if (headsetRow) body += "\n" + headsetRowTooltip(d, headsetValue);
                if (editingThis) {
                    body += s.editBad ? "\nThat is not a value this key accepts."
                                      : "\nEnter writes it, Escape leaves it alone.";
                } else if (s.resetArmedEntry == i && openxrScaleRow) {
                    // R clears only the worn headset's entry, never the
                    // whole list; say what it falls back to -- and when
                    // there is nothing keyed on the worn key to remove,
                    // say that instead of promising a removal the second
                    // press then declines (resolutionEntryRemovable).
                    if (!res.headset) {
                        body += std::string("\nR would remove this headset's entry, but ") +
                                (nativeMenuActive() ? "no headset sizing is published yet."
                                                    : "this runtime ignores it (native OpenXR only).");
                    } else if (!resolutionEntryRemovable(res)) {
                        body += "\nNo entry for this headset; R removes nothing.";
                    } else {
                        std::string fallback = "100%";
                        const ResolutionView after = resolutionView(edvr::native_render::removeResolutionEntry(
                            r.value, res.rt, res.sys));
                        if (after.matched == 2) {
                            char digits[16];
                            snprintf(digits, sizeof(digits), "%u", after.entryWidth);
                            fallback = after.rt + ":" + digits;
                        }
                        body += "\nPress R again to remove this headset's entry (back to " + fallback + ").";
                    }
                } else if (s.resetArmedEntry == i && headsetRow) {
                    // R clears only the worn headset's entry here too, never
                    // d.shipped (the empty list, every headset's entry).
                    if (!headsetValue.headset) {
                        body += std::string("\nR would remove this headset's entry, but ") +
                                (nativeMenuActive() ? "no headset is known yet."
                                                    : "this runtime ignores it (native OpenXR only).");
                    } else if (!wornEntryRemovable(headsetValue.headset, headsetValue.matched,
                                                   headsetValue.sys)) {
                        body += "\nNo entry for this headset; R removes nothing.";
                    } else {
                        const HeadsetRowView after = headsetRowView(
                            d, edvr::native_render::removeHeadsetEntry(
                                   r.value, headsetValue.rt, headsetValue.sys,
                                   headsetValue.lo, headsetValue.hi));
                        char back[32];
                        snprintf(back, sizeof(back), "%u", after.value);
                        body += std::string("\nPress R again to remove this headset's entry (back to ") +
                                back + ").";
                    }
                } else if (dim) {
                    body += "\nInactive: only DLSS and DLAA read this preset.";
                } else if (s.resetArmedEntry == i) {
                    body += "\nPress R again to reset it to the shipped value.";
                } else if (aliasesLive) {
                    // The keys the player already uses in the ship, named
                    // only while they act here.
                    char act[64];
                    menuComposeTipAction(navName, !(d.kind == MenuKind::Toggle || d.kind == MenuKind::Choice),
                                         act, sizeof(act));
                    body += std::string("\n") + act;
                } else if (d.kind == MenuKind::Toggle || d.kind == MenuKind::Choice) {
                    body += "\nEnter or Left/Right changes it.";
                } else {
                    body += "\nEnter types a value; Left/Right steps it.";
                }
                body += std::string("\n\n") + (d.detail[0] ? d.detail : d.hint);
                strncpy(c.popup, body.c_str(), sizeof(c.popup) - 1);
            }
        }
        if (c.lineCount == 0) {
            MenuLine& l = c.lines[c.lineCount++];
            strncpy(l.left, "Nothing on this page yet.", sizeof(l.left) - 1);
            l.style = kMenuDim;
        }
        // The DWELL is what this records, not whether a card was drawn. A
        // row that yields no tooltip -- an action, a heading, an empty
        // page -- would otherwise leave the flag clear forever, and the
        // tick below would ask for a fresh raster of the whole bitmap on
        // every frame for as long as the highlight sat there. The
        // Instruments page is built entirely from actions.
        s.tooltipUp = showTip;
    }

    // The footer: a legend line and at most one status line, composed
    // against the raster's own ruler so what is drawn is what fits.
    // MEASURED 2026-09-11: the old 115-character legend was 1520 px in a
    // 770 px line at the default width 30 / text 1.1, cut after about 55
    // characters, so "Tab page", the pending count and KEYS SHARED WITH
    // THE GAME had never been visible -- the likeliest root of "how do I
    // change tabs". The width is the card less the raster's pad either
    // side (menu_panel.cpp: pad = cap * 8 / 10).
    if (runtimeFlatProfile()) {
        const char* status = "";
        if (!s.flatKeysReadyShown) status = " | keyboard gate unavailable";
        else if (s.lastWrite.compare(0, 8, "writing ") == 0) status = " | saving";
        else if (s.lastWrite.compare(0, 7, "FAILED:") == 0) status = " | write failed";
        else if (!s.lastWrite.empty()) status = " | saved";
        snprintf(c.footer, sizeof(c.footer), "Up/Down row  Left/Right change\n%s/Esc close%s",
                 s.summonName.c_str(), status);
        return;
    }
    {
        MenuFooterInput in = {};
        in.editing = s.editEntry >= 0;
        in.statusPage = p.status;
        in.pageName = p.name;
        in.privateWanted = s.privateWanted;
        // During the fade-out the flag is already clear and the warning
        // would flash; a closed menu shares nothing it needs to warn of.
        // The tick sets the gate BEFORE building content, so on the open
        // tick this reads the gate the menu will have, not the closed
        // menu's 0 (which composed a false warning on every open).
        in.gatePrivate = !s.open || inputGatePrivate();
        // What this raster says about the gate, for the tick to compare
        // against (menuComposeFooter's rule for the fault line).
        s.sharedWarnShown = in.privateWanted && !in.statusPage && !in.gatePrivate;
        in.aliasesLive = aliasesLive;
        in.adopted = s.aliases.adopted;
        for (int i = 0; i < kNavCount; ++i) in.navName[i] = navName[i];
        in.pendingN = pendingN;
        in.widthPx = c.cardPx - 2 * (c.capPx * 8 / 10);
        menuComposeFooter(in, &measureFooterLine, &c, c.footer, sizeof(c.footer));
        // The flight instrument for the measurement above: each line's
        // width against its line, at most four footers a session, and
        // never the same footer twice. A line over the width is the
        // composer's own failure, since it measured with this ruler.
        if (s.footerLogged < 4 && s.footerLoggedText != c.footer) {
            s.footerLoggedText = c.footer;
            ++s.footerLogged;
            const char* line = c.footer;
            for (int n = 1; line && *line; ++n) {
                const char* nl = strchr(line, '\n');
                char one[160];
                const size_t len = nl ? static_cast<size_t>(nl - line) : strlen(line);
                snprintf(one, sizeof(one), "%.*s", static_cast<int>(len), line);
                const int px = menuPanelMeasureLine(one, c.capPx);
                Log::get().note("menu panel: footer line %d measures %d px in a %d px line%s.", n, px,
                                in.widthPx, px > in.widthPx ? " -- ellipsised" : "");
                line = nl ? nl + 1 : nullptr;
            }
        }
    }
}

// The toast's and the overlay's angular width.
constexpr float kToastWidthDeg = 16.0f;
constexpr float kOverlayWidthDeg = 12.0f;

void buildToastContent(MenuContent& c, const std::string& text, float widthDeg, float capScale) {
    memset(&c, 0, sizeof(c));
    c.toast = true;
    c.lineCount = 1;
    strncpy(c.lines[0].left, text.c_str(), sizeof(c.lines[0].left) - 1);
    c.lines[0].style = kMenuInfo;
    c.popupLine = -1;
    sizeContent(c, widthDeg, g_s.textDeg * capScale);
    // The card must never be wider than the bitmap it lives in: everything
    // downstream reads cardPx as a part of widthPx.
    if (c.widthPx > 1200) {
        c.widthPx = 1200;
        c.cardPx = c.widthPx;
    }
}

// ---------------------------------------------------------------------------
// Editing

void enqueueChange(WriteJob& job) {
    State& s = g_s;
    const MenuRowDef& d = kMenuRows[job.def];
    job.before = g_rows[job.def].value;
    // Show it now; the worker writes it; the reload that follows confirms it.
    g_rows[job.def].value = job.value;
    if (d.applies == 2) {
        g_rows[job.def].pending = rowPending(d, job.value, g_rows[job.def].snapshot);
    }
    s.menuWroteDotted = job.dotted;
    s.lastWrite = job.headset.empty() ? "writing " + job.dotted + " = " + job.value
                                      : "writing " + job.headset + " = " + job.toValue;
    s.contentDirty = true;
    perfMonitorNoteEvent(kEvIniWrite);
    enqueueWrite(job);
}

void applyChange(int defIndex, const std::string& fileValue) {
    WriteJob job;
    job.def = defIndex;
    job.dotted = dottedOf(kMenuRows[defIndex]);
    job.value = fileValue;
    enqueueChange(job);
}

// What one per-headset write needs to know about the row it is changing: the
// worn key, what that key resolves to now, and what ONE entry may hold. The
// resolution row fills it from its ResolutionView, a trim row from its
// HeadsetRowView, and the write below is the same for both.
struct HeadsetWrite {
    bool        headset = false;
    std::string rt, sys, key;
    uint32_t    matched = 0;
    uint32_t    entryValue = 0;   // what the worn key resolves to now
    uint32_t    lo = 1, hi = edvr::native_render::kResolutionWidthMax;
};

HeadsetWrite headsetWriteOf(const ResolutionView& v) {
    HeadsetWrite w;
    w.headset = v.headset;
    w.rt = v.rt;
    w.sys = v.sys;
    w.key = v.key;
    w.matched = v.matched;
    w.entryValue = v.entryWidth;
    w.lo = edvr::native_render::kResolutionWidthMin;
    w.hi = edvr::native_render::kResolutionWidthMax;
    return w;
}

HeadsetWrite headsetWriteOf(const HeadsetRowView& v) {
    HeadsetWrite w;
    w.headset = v.headset;
    w.rt = v.rt;
    w.sys = v.sys;
    w.key = v.key;
    w.matched = v.matched;
    w.entryValue = v.value;
    w.lo = v.lo;
    w.hi = v.hi;
    return w;
}

// The write for a per-headset list (fix.openxr_resolution and the trims): the
// worn headset's entry merged into the list (every other headset's entry kept
// -- re-serialised in the canonical `rt/sys:V` spacing and case, never resized
// or re-keyed -- malformed tokens dropped and named), the whole list written
// through the one-value merge. `value` 0 removes the entry. False, with the
// reason in the last-write line, when there is no headset to key on, the
// runtime's name yields no token, or the list is full.
bool applyHeadsetChange(int defIndex, const HeadsetWrite& w, uint32_t value) {
    using namespace edvr::native_render;
    State& s = g_s;
    if (!w.headset) {
        s.lastWrite = noHeadsetWriteText();
        s.contentDirty = true;
        return false;
    }
    if (w.rt.empty()) {
        s.lastWrite = kNoRuntimeToken;
        s.contentDirty = true;
        return false;
    }
    const std::string& before = g_rows[defIndex].value;
    std::string list;
    if (value) {
        if (!mergeHeadsetEntry(before, w.rt, w.sys, value, &list, w.lo, w.hi)) {
            s.lastWrite = kEightHeadsets;
            s.contentDirty = true;
            return false;
        }
    } else {
        list = removeHeadsetEntry(before, w.rt, w.sys, w.lo, w.hi);
    }
    WriteJob job;
    job.def = defIndex;
    job.dotted = dottedOf(kMenuRows[defIndex]);
    job.value = list;
    job.headset = w.key;
    char digits[16];
    if (w.matched == 1 || (w.matched == 2 && w.sys.empty())) {
        // The entry keyed on the worn key (the runtime-only one IS this
        // headset's own when the system token is empty).
        snprintf(digits, sizeof(digits), "%u", w.entryValue);
        job.fromValue = digits;
    } else if (w.matched == 2) {
        // No entry under the worn key, but a runtime-only fallback was in
        // effect; the log says so rather than hiding the value that ran.
        snprintf(digits, sizeof(digits), "%u", w.entryValue);
        job.fromValue = "(none; was " + w.rt + ":" + digits + ")";
    } else {
        job.fromValue = "(none)";
    }
    if (value) {
        snprintf(digits, sizeof(digits), "%u", value);
        job.toValue = digits;
    } else {
        job.toValue = "(none)";
    }
    HeadsetEntry parsed[kHeadsetEntryMax];
    std::vector<std::string> skipped;
    parseHeadsetEntries(before.c_str(), parsed, &skipped, w.lo, w.hi);
    for (const std::string& token : skipped) {
        if (!job.dropped.empty()) job.dropped += ", ";
        job.dropped += token;
    }
    enqueueChange(job);
    return true;
}

bool applyResolutionChange(int defIndex, const ResolutionView& v, uint32_t width) {
    return applyHeadsetChange(defIndex, headsetWriteOf(v), width);
}

// The worker's results, on the frame thread: the log line, the Status
// page's last-write, the reload request for each landed write, and the
// row put back to what the file says when a write failed.
void drainWrites() {
    State& s = g_s;
    std::vector<WriteDone> done;
    {
        std::lock_guard<std::mutex> lock(g_writer.m);
        if (g_writer.done.empty()) return;
        done.swap(g_writer.done);
    }
    for (const WriteDone& w : done) {
        const MenuRowDef& d = kMenuRows[w.job.def];
        if (w.ok) {
            s.pollRequest = true;
            const char* when = d.applies == 2 ? "takes effect at the next launch"
                             : d.applies == 1 ? "live"
                                              : "when it applies is not documented";
            if (!w.job.headset.empty()) {
                // The per-headset row: the key and the values, then the
                // list the file now holds.
                s.lastWrite = w.job.headset + " = " + w.job.toValue;
                Log::get().note("menu: %s %s %s -> %s (list now %s%s%s; written to edvr.ini; "
                                "%s).",
                                w.job.dotted.c_str(), w.job.headset.c_str(), w.job.fromValue.c_str(),
                                w.job.toValue.c_str(), w.job.value.empty() ? "empty" : w.job.value.c_str(),
                                w.job.dropped.empty() ? "" : "; dropped malformed ",
                                w.job.dropped.c_str(), when);
            } else {
                s.lastWrite = w.job.dotted + " = " + w.job.value;
                Log::get().note("menu: %s %s -> %s (written to edvr.ini; %s).", w.job.dotted.c_str(),
                                w.job.before.empty() ? "(default)" : w.job.before.c_str(),
                                w.job.value.c_str(), when);
            }
        } else {
            s.lastWrite = "FAILED: " + w.err;
            Log::get().note("menu: %s = %s could not be written: %s.", w.job.dotted.c_str(),
                            w.job.value.c_str(), w.err.c_str());
            g_rows[w.job.def].value = rowValue(d);
            if (d.applies == 2) {
                g_rows[w.job.def].pending = rowPending(d, g_rows[w.job.def].value,
                                                       g_rows[w.job.def].snapshot);
            }
        }
        s.contentDirty = true;
    }
}

void stepRow(int defIndex, int dir, int mult) {
    const MenuRowDef& d = kMenuRows[defIndex];
    const std::string& cur = g_rows[defIndex].value;
    if (isDlssPresetRow(d) && dlssPresetDisabled()) {
        g_s.lastWrite = "DLSS preset is inactive: select DLSS or DLAA above first.";
        g_s.contentDirty = true;
        return;
    }
    if (isOpenxrRenderScaleRow(d)) {
        // A width for the worn headset, stepped by 100 px on the grid of
        // round hundreds (steppedResolution); the generated row is Text,
        // for which stepping would otherwise be a no-op.
        const ResolutionView v = resolutionView(cur);
        if (!v.headset) {
            g_s.lastWrite = noHeadsetWriteText();
            g_s.contentDirty = true;
            return;
        }
        const uint32_t next = steppedResolution(v, dir, mult);
        if (next != v.width) applyResolutionChange(defIndex, v, next);
        return;
    }
    if (isHeadsetRow(d)) {
        // One number for the worn headset, stepped by 1 within the row's
        // bounds; the generated row is Text, for which stepping would
        // otherwise be a no-op.
        const HeadsetRowView v = headsetRowView(d, cur);
        if (!v.headset) {
            g_s.lastWrite = noHeadsetWriteText();
            g_s.contentDirty = true;
            return;
        }
        const uint32_t next = steppedHeadsetValue(v, dir, mult);
        if (next != v.value) applyHeadsetChange(defIndex, headsetWriteOf(v), next);
        return;
    }
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

// Typing a value, defined with the keys below.
void beginEdit(int entryIndex, int defIndex);
void cancelEdit();
void commitEdit();

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
        if (d.kind == MenuKind::Toggle || d.kind == MenuKind::Choice) {
            stepRow(e.def, +1, 1);
        } else {
            // A number or a free string: type it. Enter again commits.
            beginEdit(p.highlight, e.def);
        }
    }
}

// The dwell the tooltip waits on restarts whenever the row changes.
void noteHighlightMoved() {
    g_s.highlightSinceMs = g_s.tickMs;
    g_s.tooltipUp = false;
    g_s.tooltipScroll = 0;
}

// Scroll the tooltip's body. Returns whether it moved -- when it did not,
// the key is free to do what it did before, which is change the page.
bool scrollTooltip(int lines) {
    State& s = g_s;
    if (!s.tooltipUp) return false;
    const int max = menuPanelPopupScrollMax();
    // The raster reports what is left to scroll from where it is NOW, so
    // the ceiling is where it already sits plus that.
    const int ceiling = s.tooltipScroll + max;
    int next = s.tooltipScroll + lines;
    if (next < 0) next = 0;
    if (next > ceiling) next = ceiling;
    if (next == s.tooltipScroll) return false;
    s.tooltipScroll = next;
    s.contentDirty = true;
    return true;
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
            noteHighlightMoved();
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
    noteHighlightMoved();
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

// The raw key, through EDVR's own import of GetAsyncKeyState, which the
// gate never patches. A vk of 0 is no key.
bool rawKeyDown(int vk) { return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Edge-and-repeat over the raw key (menu_keys.h, keyRepeatStep). Returns
// how many presses this key delivered this tick; act=false tracks the key
// and swallows it until it is released.
int pollKey(KeyRepeat& k, uint64_t now, bool focused, bool act = true) {
    return keyRepeatStep(k, focused && rawKeyDown(k.vk), now, act);
}

enum KeyIndex {
    kUp, kDown, kLeft, kRight, kEnter, kSpace, kTab, kPgUp, kPgDn, kHome, kEnd, kReset
};

// What each fixed key does, as the one dispatcher sees it (KeyIndex
// order). Tab is PageNext here and PagePrev with Shift, decided at the
// poll; the resolver hands the rules this same table, so a panel key that
// lands on one of these is refused for the reason the table gives.
constexpr MenuNav kFixedNavs[12] = {kNavUp,       kNavDown,     kNavLeft,   kNavRight,
                                    kNavSelect,   kNavSelect,   kNavPageNext, kNavReadBack,
                                    kNavReadOn,   kNavHome,     kNavEnd,    kNavReset};

void initKeys() {
    const int vks[12] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_SPACE,
                         VK_TAB, VK_PRIOR, VK_NEXT, VK_HOME, VK_END, 'R'};
    for (int i = 0; i < 12; ++i) {
        g_s.keys[i].vk = vks[i];
        g_s.keys[i].down = false;
        g_s.keys[i].nextMs = 0;
    }
    for (int i = 0; i < kEditKeyCount; ++i) {
        g_s.editKeys[i].vk = kEditKeys[i].vk;
        g_s.editKeys[i].down = false;
        g_s.editKeys[i].nextMs = 0;
    }
}

// ---------------------------------------------------------------------------
// Elite's panel keys

// Apply the rules (menu_keys.h) to the cached slots: the fixed keys with
// their navs, Escape, the summon chord and every registered hotkey are
// what a panel key is checked against. Returns whether the table changed.
// Each alias tracker takes its vk and is PRIMED FROM THE RAW KEY: a key
// held through a rebind cannot act on its new meaning until it is pressed
// afresh. No file is read here, which is what lets a hotkey.menu change
// re-resolve at once.
bool resolveAliases() {
    State& s = g_s;
    MenuAliasInput in[kMenuUiElementCount];
    for (int i = 0; i < kMenuUiElementCount; ++i) {
        const EliteKeySlots& sl = s.aliasSlots[i];
        MenuAliasInput& e = in[i];
        e.element = kMenuUiElements[i].element;
        e.nav = kMenuUiElements[i].nav;
        e.repeats = kMenuUiElements[i].repeats;
        e.slots = sl.present ? sl.count : 0;
        for (int k = 0; k < 2; ++k) {
            e.binding[k] = sl.slot[k].binding;
            e.eliteName[k] = sl.slot[k].eliteName;
            e.keyboard[k] = sl.slot[k].keyboard;
            e.chorded[k] = sl.slot[k].chorded;
            e.modifierMain[k] = sl.slot[k].modifierMain;
        }
    }
    MenuFixedKeys fixed = {};
    for (int i = 0; i < 12; ++i) {
        fixed.vks[i] = s.keys[i].vk;
        fixed.navs[i] = kFixedNavs[i];
    }
    fixed.count = 12;
    fixed.escapeVk = VK_ESCAPE;
    fixed.summonVk = s.summon.key();
    fixed.summonMods = s.summon.mods();
    fixed.registeredCount = hotkeyRegisteredKeys(fixed.registered, 16);
    MenuAliasTable t;
    menuAliasResolve(in, kMenuUiElementCount, fixed, &t);
    // The resolver zeroes the whole table, padding included, so the same
    // slots give a memcmp-equal answer (pinned by menu_test).
    const bool changed = memcmp(&t, &s.aliases, sizeof(t)) != 0;
    s.aliases = t;
    for (int i = 0; i < kMenuAliasMax; ++i) {
        KeyRepeat& k = s.aliasKeys[i];
        k.vk = i < t.count ? t.alias[i].vk : 0;
        keyRepeatPrime(k, rawKeyDown(k.vk));
    }
    bool anyFile = false;
    for (int i = 0; i < kMenuUiElementCount; ++i) anyFile |= s.aliasSlots[i].filesSeen > 0;
    s.aliasSource = !anyFile ? 2 : !t.adopted ? 3 : 4;
    s.contentDirty = true;
    return changed;
}

// The basename of the file the answers came from, for the log.
const char* aliasFileName() {
    const State& s = g_s;
    for (int i = 0; i < kMenuUiElementCount; ++i) {
        if (s.aliasSlots[i].present && s.aliasSlots[i].file[0]) return s.aliasSlots[i].file;
    }
    return "your bindings files";
}

// The outcome, one line, after every read of the slots: what was adopted
// from which file and what was skipped and why, or which of the three
// ways there was nothing to adopt. `prefix` is the re-read's own opening
// ("your Elite bindings changed -- "); empty on the first read. A second
// line follows only for keys this build cannot name, so a log can be
// acted on. This is the line whose ABSENCE says the code never ran: the
// call after the camera keys' adoption is unconditional.
void logAliasOutcome(const char* prefix) {
    const State& s = g_s;
    const char* file = aliasFileName();
    if (s.aliasSource == 2) {
        Log::get().note("menu keys: %sno bindings files were found under Options\\Bindings, so the "
                        "menu's own keys only. (A stock control scheme keeps its file in the "
                        "game's ControlSchemes folder, which this build does not read yet.)",
                        prefix);
        return;
    }
    // Sized past Log::note's own 1200-byte line, so its "...[truncated]"
    // marker is the backstop and the skipped list is never the binding
    // cap: a hand-built file with a pad on every Primary and the cursor
    // cluster on every Secondary skips 18 items (~770 chars), and a 600-
    // byte list dropped "Home (read on; ...)" -- the very question the
    // list exists to answer.
    char skipped[1200];
    menuAliasSkippedText(s.aliases, skipped, sizeof(skipped));
    if (s.aliasSource == 3) {
        // Nothing adopted: either no panel key is on the keyboard at all,
        // or every one that is already means something here.
        bool anyKeyboard = false;
        for (int i = 0; i < s.aliases.skippedCount; ++i) {
            anyKeyboard |= s.aliases.skipped[i].reason != kSkipNotKeyboard;
        }
        if (!anyKeyboard) {
            Log::get().note("menu keys: %snone of Elite's panel keys is on a keyboard key in %s "
                            "(they are on a controller or the mouse); the menu's own keys only.",
                            prefix, file);
        } else {
            Log::get().note("menu keys: %severy keyboard panel key in %s is already one of the "
                            "menu's own (%s); the menu's own keys only.",
                            prefix, file, skipped);
        }
    } else {
        char summary[400];
        menuAliasSummary(s.aliases, summary, sizeof(summary));
        Log::get().note("menu keys: %sadopted from %s -- %s.%s%s%s Tab, the arrows, Enter and "
                        "Escape still work.",
                        prefix, file, summary, skipped[0] ? " Skipped: " : "", skipped,
                        skipped[0] ? "." : "");
    }
    std::string unnamed;
    for (int i = 0; i < s.aliases.skippedCount; ++i) {
        const MenuAliasSkipped& k = s.aliases.skipped[i];
        if (k.reason != kSkipUnnamed) continue;
        if (!unnamed.empty()) unnamed += ", ";
        unnamed += std::string(k.element) + " " + k.name;
    }
    if (!unnamed.empty()) {
        Log::get().note("menu keys: keys this build cannot name: %s (please report this line).",
                        unnamed.c_str());
    }
}

// ---------------------------------------------------------------------------
// Typing a value

// Whether what has been typed could be written. A number must parse and
// sit within the row's bounds; anything else is the author's business.
bool editBufferBad() {
    const State& s = g_s;
    if (s.editDef < 0) return false;
    const MenuRowDef& d = kMenuRows[s.editDef];
    // The per-headset rows take one headset's number -- a width in pixels,
    // 1..16384, or the row's own range -- never the list.
    if (isOpenxrRenderScaleRow(d)) return !parseTypedWidth(s.editBuf, nullptr);
    if (isHeadsetRow(d)) return !parseTypedHeadsetValue(d, s.editBuf, nullptr);
    if (isVscreenResolutionRow(d)) return !parseVscreenWidth(s.editBuf, nullptr);
    if (d.kind != MenuKind::Number) return false;
    if (s.editBuf.empty()) return true;
    char* end = nullptr;
    const double v = strtod(s.editBuf.c_str(), &end);
    if (!end || *end) return true;
    if (d.lo[0] && v < atof(d.lo)) return true;
    if (d.hi[0] && v > atof(d.hi)) return true;
    return false;
}

void beginEdit(int entryIndex, int defIndex) {
    State& s = g_s;
    std::string seed = g_rows[defIndex].value;
    if (isOpenxrRenderScaleRow(kMenuRows[defIndex])) {
        // The buffer holds the worn headset's width as digits, never the
        // list (so kEditMax cannot cut it); with no headset to key on the
        // edit is refused and the Status page says why.
        const uint32_t width = resolvedWidth(seed);
        if (!width) {
            s.lastWrite = noHeadsetWriteText();
            s.contentDirty = true;
            return;
        }
        char digits[16];
        snprintf(digits, sizeof(digits), "%u", width);
        seed = digits;
    } else if (isHeadsetRow(kMenuRows[defIndex])) {
        // The buffer holds the worn headset's number as digits, never the
        // list; with no headset to key on the edit is refused and the Status
        // page says why.
        const HeadsetRowView v = headsetRowView(kMenuRows[defIndex], seed);
        if (!v.headset) {
            s.lastWrite = noHeadsetWriteText();
            s.contentDirty = true;
            return;
        }
        char digits[16];
        snprintf(digits, sizeof(digits), "%u", v.value);
        seed = digits;
    }
    s.editEntry = entryIndex;
    s.editDef = defIndex;
    s.editBuf = seed;
    if (s.editBuf.size() > kEditMax) s.editBuf.resize(kEditMax);
    // Every typing key is seeded from the raw key: one held as the row
    // opens -- a letter that is also a panel key, or Space, which is also
    // the fixed select key -- is parked until released, so it cannot land
    // in the buffer it opened. (Belt and braces over the act=false tracking
    // outside an edit; it also closes the Space-appended-at-open case that
    // commitEdit's trim only papered over, since editBufferBad reads the
    // untrimmed buffer.)
    for (KeyRepeat& k : s.editKeys) keyRepeatPrime(k, rawKeyDown(k.vk));
    s.editBad = editBufferBad();
    s.contentDirty = true;
}

void cancelEdit() {
    State& s = g_s;
    if (s.editEntry < 0) return;
    s.editEntry = -1;
    s.editDef = -1;
    s.editBuf.clear();
    s.editBad = false;
    s.contentDirty = true;
}

void applyChange(int defIndex, const std::string& fileValue);

void commitEdit() {
    State& s = g_s;
    if (s.editEntry < 0 || s.editDef < 0) return;
    const int def = s.editDef;
    std::string v = s.editBuf;
    // Trim the spaces a person types either side of a value; the ini would
    // keep them and the next read would not match.
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    while (!v.empty() && v.back() == ' ') v.pop_back();
    const bool bad = editBufferBad();
    cancelEdit();
    const MenuRowDef& d = kMenuRows[def];
    if (bad) {
        // The per-headset row drops the dotted key: with it the line is 70
        // bytes and the Status page's 63-byte field cut the range off
        // mid-word (the row is highlighted and the key is the tooltip's
        // title).
        if (isOpenxrRenderScaleRow(d)) {
            s.lastWrite = "not written: needs a width in pixels, 1 to 16384";
        } else if (isVscreenResolutionRow(d)) {
            s.lastWrite = "not written: needs \"auto\" or a width in pixels, 640 to 8192";
        } else if (isHeadsetRow(d)) {
            // As above, the dotted key is dropped: the row is highlighted and
            // the key is the tooltip's title, and the line has to fit the
            // Status page's 63-byte field.
            char text[64];
            snprintf(text, sizeof(text), "not written: needs a number from %u to %u",
                     headsetRowLow(d), headsetRowHigh(d));
            s.lastWrite = text;
        } else {
            s.lastWrite = std::string("not written: ") + dottedOf(d) + " needs a number" +
                          (d.lo[0] && d.hi[0] ? std::string(" from ") + d.lo + " to " + d.hi : "");
        }
        s.contentDirty = true;
        return;
    }
    if (isOpenxrRenderScaleRow(d)) {
        // The typed width is the worn headset's entry; the file gets the
        // merged list, the other headsets' entries untouched.
        uint32_t width = 0;
        const ResolutionView view = resolutionView(g_rows[def].value);
        if (parseTypedWidth(v, &width) && (width != view.width || !view.entryWidth)) {
            applyResolutionChange(def, view, width);
        }
        return;
    }
    if (isHeadsetRow(d)) {
        // The typed number is the worn headset's entry; the file gets the
        // merged list, the other headsets' entries untouched. A typed 0
        // removes the entry, and is a change whenever there is one to remove.
        uint32_t value = 0;
        const HeadsetRowView view = headsetRowView(d, g_rows[def].value);
        if (parseTypedHeadsetValue(d, v, &value) && value != view.value) {
            applyHeadsetChange(def, headsetWriteOf(view), value);
        }
        return;
    }
    if (v != g_rows[def].value) applyChange(def, v);
}

// One tick of the typing keys. Returns whether anything was typed.
bool handleEditKeys(uint64_t now, bool focused) {
    State& s = g_s;
    bool any = false;
    for (int i = 0; i < kEditKeyCount; ++i) {
        const int n = pollKey(s.editKeys[i], now, focused);
        if (!n) continue;
        any = true;
        const EditKey& k = kEditKeys[i];
        for (int rep = 0; rep < n; ++rep) {
            if (k.vk == VK_BACK) {
                if (!s.editBuf.empty()) s.editBuf.pop_back();
            } else if (s.editBuf.size() < kEditMax) {
                s.editBuf.push_back(s.shiftHeld ? k.shifted : k.plain);
            }
        }
    }
    if (any) {
        s.editBad = editBufferBad();
        s.contentDirty = true;
    }
    return any;
}

void closeMenu(const char* why);

// The one dispatcher: every fixed key and every adopted Elite key is a
// MenuNav, so an alias is structurally an alias of a fixed key's action and
// there is no second switch to drift from this one. Back is the only nav
// no fixed key emits (Escape has its own edge in menuTick, because it
// must first abandon a value being typed); it closes the menu, and the
// closing key is captured into the gate's release tail, so Backspace or
// Ctrl reaches the game only after a release and a fresh press.
void dispatchNav(MenuNav nav, uint64_t now) {
    State& s = g_s;
    Page& p = s.pages[s.page];
    switch (nav) {
        case kNavUp: moveHighlight(-1); break;
        case kNavDown: moveHighlight(+1); break;
        case kNavLeft:
        case kNavRight:
            if (!p.status && !p.entries.empty() &&
                p.entries[p.highlight].kind == EntryKind::Setting) {
                stepRow(p.entries[p.highlight].def, nav == kNavLeft ? -1 : +1, s.shiftHeld ? 5 : 1);
            }
            break;
        case kNavSelect: activateEntry(); break;
        case kNavBack: closeMenu("UI_Back"); break;
        case kNavPageNext: changePage(+1); break;
        case kNavPagePrev: changePage(-1); break;
        // PageUp and PageDown read the explanation beside the row when
        // there is more of it than fits. Tab is what changes the page,
        // so these only fall back to that when nothing can scroll.
        case kNavReadBack:
            if (!scrollTooltip(-3)) changePage(-1);
            break;
        case kNavReadOn:
            if (!scrollTooltip(+3)) changePage(+1);
            break;
        case kNavHome:
            if (!p.status) { p.highlight = 0; moveHighlight(0); if (p.entries.size() && p.entries[0].kind == EntryKind::Heading) moveHighlight(+1); noteHighlightMoved(); s.contentDirty = true; }
            break;
        case kNavEnd:
            if (!p.status && !p.entries.empty()) { p.highlight = static_cast<int>(p.entries.size()) - 1; noteHighlightMoved(); s.contentDirty = true; }
            break;
        case kNavReset:
            if (!p.status && !p.entries.empty() && p.entries[p.highlight].kind == EntryKind::Setting) {
                if (s.resetArmedEntry == p.highlight && now - s.resetArmedMs < kResetArmMs) {
                    const int def = p.entries[p.highlight].def;
                    const MenuRowDef& d = kMenuRows[def];
                    if (isOpenxrRenderScaleRow(d)) {
                        // Only the worn headset's entry goes, never
                        // d.shipped (the empty list, every headset's entry).
                        const ResolutionView v = resolutionView(g_rows[def].value);
                        if (!v.headset) {
                            s.lastWrite = noHeadsetWriteText();
                            s.contentDirty = true;
                        } else if (!resolutionEntryRemovable(v)) {
                            // The key has its own Status line two above;
                            // naming it here overran the 63-byte field.
                            s.lastWrite = "no entry to remove for this headset";
                            s.contentDirty = true;
                        } else {
                            applyResolutionChange(def, v, 0);
                        }
                    } else if (isHeadsetRow(d)) {
                        // The same rule on a trim: only the worn headset's
                        // entry goes, never the whole list.
                        const HeadsetRowView v = headsetRowView(d, g_rows[def].value);
                        if (!v.headset) {
                            s.lastWrite = noHeadsetWriteText();
                            s.contentDirty = true;
                        } else if (!wornEntryRemovable(v.headset, v.matched, v.sys)) {
                            s.lastWrite = "no entry to remove for this headset";
                            s.contentDirty = true;
                        } else {
                            applyHeadsetChange(def, headsetWriteOf(v), 0);
                        }
                    } else {
                        applyChange(def, d.shipped);
                    }
                    s.resetArmedEntry = -1;
                } else {
                    s.resetArmedEntry = p.highlight;
                    s.resetArmedMs = now;
                    s.contentDirty = true;
                }
            }
            break;
        case kNavCount:
        default:
            break;
    }
}

void handleKeys(uint64_t now) {
    State& s = g_s;
    const bool focused = gameHasFocus();
    s.shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool any = false;
    Page& p = s.pages[s.page];
    // Elite's adopted keys act only while the gate PROVABLY holds the
    // game's keyboard, never while typing, never on a status page: there
    // an adopted key IS a game key, and one press of E would cycle the
    // ship's panel and the menu's page at once. The fixed keys keep their
    // documented, shared behaviour on those pages -- and so does the page
    // pair, which follows Tab. (The predicate itself is evaluated per alias
    // below, since this tick's fixed keys can change its inputs.)
    const bool gateHolds = inputGateHoldsGameKeyboard();
    // While a value is being typed the navigation keys are the editor's:
    // Enter commits, the arrows and Tab are held (Escape cancels, from the
    // tick). Everything else goes into the buffer.
    if (s.editEntry >= 0) {
        if (s.editEntry != p.highlight) cancelEdit();
    }
    if (s.editEntry >= 0) {
        if (handleEditKeys(now, focused)) any = true;
        for (int i = 0; i < 12; ++i) {
            const int n = pollKey(s.keys[i], now, focused);
            if (!n) continue;
            any = true;
            if (i == kEnter) commitEdit();
        }
        // The adopted keys are typing keys here (W, A, S, D, Space...):
        // tracked and swallowed, so one still held when Enter writes the
        // value is parked until released rather than fired on its menu
        // meaning.
        for (int i = 0; i < s.aliases.count; ++i) pollKey(s.aliasKeys[i], now, focused, false);
        if (any) {
            s.lastInputMs = now;
            s.aimParked = true;
        }
        return;
    }
    // Not typing: the typing keys are tracked against their REAL state and
    // swallowed, so one held down before an edit begins is parked and does
    // not fire the moment it does. (The old poll passed focused=false,
    // which forced every tracker to "up" each tick and tracked nothing --
    // the opposite of what its comment said; beginEdit's prime is the
    // second guard.)
    for (int i = 0; i < kEditKeyCount; ++i) pollKey(s.editKeys[i], now, focused, false);
    for (int i = 0; i < 12; ++i) {
        const int n = pollKey(s.keys[i], now, focused);
        if (!n) continue;
        any = true;
        dispatchNav(i == kTab && s.shiftHeld ? kNavPagePrev : kFixedNavs[i], now);
    }
    // Elite's own keys, through the same trackers and the same dispatcher.
    // A page key is edge-only (menu_keys.h, `repeats`): a repeat tick is
    // one where the tracker was already down. Swallowed edges are counted
    // for the held-back line below. The predicate is asked afresh before
    // EACH alias, not once for the tick: the fixed loop above, or an
    // earlier alias, can have opened a row for typing (Enter on a Number
    // row) or moved to a status page (Tab onto Monitor) this very tick,
    // and a W still held in its repeat train would otherwise fire on the
    // stale answer -- moving the highlight off the row just opened, so the
    // next tick's check cancelled the edit, or paging a second time from a
    // page where aliases are inert. Asked afresh, the held key is parked
    // until released, the swallow-until-release rule. The page pair is the
    // exception and follows Tab (menuAliasMayActNav): on Monitor and Status
    // it acts, and reaches the ship as Tab does there -- asked for after
    // the first flight, when Q/E did nothing on the very pages a player
    // wants to leave.
    bool swallowedEdge = false;
    for (int i = 0; i < s.aliases.count; ++i) {
        const MenuAlias& a = s.aliases.alias[i];
        KeyRepeat& k = s.aliasKeys[i];
        const bool wasDown = k.down;
        const bool isDown = focused && rawKeyDown(a.vk);
        const bool liveNow =
            menuAliasMayActNav(a.nav, gateHolds, s.editEntry >= 0, s.pages[s.page].status);
        const int n = keyRepeatStep(k, isDown, now, liveNow);
        if (!liveNow && isDown && !wasDown && !menuAliasFollowsTab(a.nav)) swallowedEdge = true;
        if (!n) continue;
        if (!a.repeats && wasDown) continue;
        any = true;
        dispatchNav(a.nav, now);
        if (!s.open) return;   // Back closed the menu
    }
    if (any) {
        s.lastInputMs = now;
        s.aimParked = true;
    }
    if (s.resetArmedEntry >= 0 && now - s.resetArmedMs >= kResetArmMs) {
        s.resetArmedEntry = -1;
        s.contentDirty = true;
    }
    // A rig where the flag is set but the game's keyboard has never been
    // seen reaching a door ("Tab boosts"): the aliases stay inert there by
    // design, and the log says so once, on the first swallowed press after
    // the open's own settling tick. `p` was bound at the top of the tick
    // and a fixed Tab may have moved the page since: read the page afresh.
    const bool statusNow = s.pages[s.page].status;
    if (swallowedEdge && !s.aliasHeldBackNoted && s.open && !statusNow && s.privateWanted &&
        inputGatePrivate() && !gateHolds && now - s.openedMs > 500) {
        s.aliasHeldBackNoted = true;
        Log::get().note("menu keys: your Elite panel keys are held back until the game's keyboard "
                        "is seen reaching the gate (the 'keyboard gate: the game's captured "
                        "keyboard is reaching the gate' line); the menu's own keys work meanwhile.");
    }
    // The legend follows the live predicate, never mere adoption: one extra
    // raster on the tick after open, so it never names a dead key.
    const bool legendLive = s.aliases.adopted && menuAliasMayAct(gateHolds, false, statusNow);
    if (legendLive != s.aliasesLiveShown) {
        s.aliasesLiveShown = legendLive;
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
    // A look that wanders must not take the row out from under a value
    // being typed.
    if (s.editEntry >= 0) return;
    Page& p = s.pages[s.page];
    if (p.status || p.entries.empty()) return;
    float org[3], dir[3];
    if (!headRayInAnchor(org, dir)) return;
    const float aspect = menuPanelAspect();
    if (!(aspect > 0.0f)) return;
    // The whole panel, strip included: the hit test works in the bitmap's
    // own coordinates, and menuPanelLineAt rejects the strip itself.
    const float halfW = panelHalfW(s.widthDeg, tooltipsOn());
    const float halfH = halfW * aspect;
    const float shift = panelShift(halfW, tooltipsOn());
    float su = 0.0f, sv = 0.0f;
    int line = -1;
    const bool onPanel = menuPanelHit(org, dir, s.distance, s.curve, halfW, halfH, shift, &su, &sv);
    if (onPanel) {
        line = menuPanelLineAt(su, 1.0f - sv);
        // Reading the tooltip is using the menu. Without this, resting on
        // the card generates no input at all and an idle_dismiss set by
        // hand would close the panel mid-sentence.
        s.lastInputMs = now;
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
        noteHighlightMoved();
        s.contentDirty = true;
        s.lastInputMs = now;
    }
}

// ---------------------------------------------------------------------------
// Open, close, anchor

// Anchor the panel where the head is looking, UPRIGHT IN THE WORLD: the
// anchor's yaw and pitch are the head's look direction's, its roll is
// zero, so the panel's edges are level whatever tilt the head had at the
// summon (the first build latched the whole head pose, roll included, and
// a head cocked at F8 got a cocked menu -- flown 2026-09-07). The offsets
// add to those angles: zero and zero is where you are looking; a toast
// sits a little below. R = Ry(yaw) * Rx(pitch), no Rz.
void latchAnchor(float yawDeg, float pitchDeg) {
    State& s = g_s;
    float c[12];
    if (!headPose(c)) {
        s.anchorValid = false;
        return;
    }
    // The look direction is minus the pose's third column; its yaw is
    // read on the horizon, its pitch from its rise. Looking straight up or
    // down leaves no horizon to read a yaw from, so the head's up axis
    // stands in (it points along the horizon then: forward when looking
    // down, backward when looking up).
    float fx = -c[2], fy = -c[6], fz = -c[10];
    float hx = fx, hz = fz;
    if (hx * hx + hz * hz < 0.05f * 0.05f) {
        const float sgn = fy < 0.0f ? 1.0f : -1.0f;
        hx = sgn * c[1];
        hz = sgn * c[9];
    }
    const float yaw = atan2f(-hx, -hz) + yawDeg * 0.0174532925f;
    const float rise = fy > 1.0f ? 1.0f : (fy < -1.0f ? -1.0f : fy);
    const float pitch = asinf(rise) + pitchDeg * 0.0174532925f;
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    // Ry(yaw) * Rx(pitch), row-major, with the head's position.
    const float out[12] = {cy, sy * sp, sy * cp, c[3],
                           0.0f, cp, -sp, c[7],
                           -sy, cy * sp, cy * cp, c[11]};
    memcpy(s.anchor, out, sizeof(out));
    s.anchorValid = true;
    publishMenuAnchor(out);
}

void openMenu(uint64_t now) {
    State& s = g_s;
    if (runtimeFlatProfile()) {
        inputGateInstall();
        for (KeyRepeat& k : s.keys) keyRepeatPrime(k, rawKeyDown(k.vk));
        for (KeyRepeat& k : s.editKeys) keyRepeatPrime(k, rawKeyDown(k.vk));
        buildPages();
        refreshRowValues();
        s.open = true;
        s.openedMs = s.lastInputMs = s.highlightSinceMs = now;
        s.lastDrawn = menuDrawnValue();
        s.lastDrawnMs = 0;
        s.flatEscapeDown = rawKeyDown(VK_ESCAPE);
        s.flatNativeScaleShown = flatRuntimeNativeScale();
        s.alpha = 1.0f;
        s.tooltipUp = false;
        s.contentDirty = true;
        Log::get().note("menu: flat graphics panel open; keyboard %s.",
                        s.privateWanted ? "private" : "shared");
        return;
    }
    if (!glitchConsumerPresent() && !nativeMenuAvailable()) {
        if (!s.noConsumerNoted) {
            s.noConsumerNoted = true;
            Log::get().note("menu: the menu key was pressed, but no compositor consumer has "
                            "hooked the compositor, so there is no door to draw the panel "
                            "at. The menu stays closed and the keyboard stays the game's. "
                            "See the runtime diagnosis in this log.");
            vrRuntimeExplainOnce();
        }
        return;
    }
    latchAnchor(0.0f, 0.0f);
    if (!s.anchorValid) {
        if (!s.noPoseNoted) {
            s.noPoseNoted = true;
            Log::get().note("menu: no head pose has been published yet, so the panel has "
                            "nowhere to anchor. Try again once the headset is tracking.");
        }
        return;
    }
    inputGateInstall();
    // Every tracker is seeded from the raw key: one held as the menu opens
    // -- W for thrust, Down on the way in -- does nothing until released
    // and pressed afresh. handleKeys runs only while open, so before this
    // a Down held through the summon fired once on the first tick and then
    // repeated; with W/S adopted (held constantly for thrust) the seed is
    // required, not optional, and it changes the fixed keys the same way.
    for (KeyRepeat& k : s.keys) keyRepeatPrime(k, rawKeyDown(k.vk));
    for (KeyRepeat& k : s.editKeys) keyRepeatPrime(k, rawKeyDown(k.vk));
    for (int i = 0; i < s.aliases.count; ++i) {
        keyRepeatPrime(s.aliasKeys[i], rawKeyDown(s.aliasKeys[i].vk));
    }
    if (s.developer != s.developerBuilt) buildPages();
    refreshRowValues();
    s.open = true;
    s.openedMs = now;
    s.lastInputMs = now;
    s.highlightSinceMs = now;
    s.tooltipUp = false;
    s.lastDrawnMs = now;   // grace: the first draw has not had a chance yet
    s.aimParked = false;
    s.contentDirty = true;
    s.toastUp = false;
    s.toastAlpha = 0.0f;
    s.overlayUp = false;
    s.overlayAlpha = 0.0f;
    perfMonitorNoteEvent(kEvMenu);
    Log::get().note("menu: open (%s, %.1f m, keys %s).", s.pages[s.page].name,
                    static_cast<double>(s.distance), s.privateWanted ? "private" : "shared");
}

void closeMenu(const char* why) {
    State& s = g_s;
    if (!s.open) return;
    s.open = false;
    cancelEdit();
    s.resetArmedEntry = -1;
    inputGateSetPrivate(false);
    perfMonitorNoteEvent(kEvMenu);
    Log::get().note("menu: closed (%s).", why);
}

}  // namespace

// ---------------------------------------------------------------------------

void menuAdoptGameBindings(bool enabled, const char* why) {
    State& s = g_s;
    s.readGameBindings = enabled;
    if (!enabled) {
        memset(s.aliasSlots, 0, sizeof(s.aliasSlots));
        memset(&s.aliases, 0, sizeof(s.aliases));
        for (KeyRepeat& k : s.aliasKeys) {
            k.vk = 0;
            keyRepeatPrime(k, false);
        }
        s.aliasSource = 1;
        s.contentDirty = true;
        Log::get().note("menu keys: not read from Elite (hotkey.read_game_bindings = 0); the "
                        "menu's own keys only.");
        return;
    }
    for (int i = 0; i < kMenuUiElementCount; ++i) {
        eliteBindsLookupSlots(kMenuUiElements[i].element, kEliteKeyAllowModifierMain,
                              &s.aliasSlots[i]);
    }
    const bool changed = resolveAliases();
    if (!why) {
        logAliasOutcome("");
    } else if (changed) {
        char prefix[96];
        snprintf(prefix, sizeof(prefix), "%s -- ", why);
        logAliasOutcome(prefix);
    } else {
        // Silence here is indistinguishable from the re-read being dead
        // (the camera keys learned this in the field), so the no-change
        // case says so.
        Log::get().note("menu keys: your Elite bindings files changed, but the panel keys read "
                        "the same as before.");
    }
}

void menuConfigure(Config& cfg) {
    State& s = g_s;
    if (runtimeFlatProfile()) {
        const std::string key = cfg.getString("hotkey.menu", "F8");
        if (!s.configured || key != s.summonName) {
            s.summonName = key;
            s.summon.setBinding(key.c_str());
        }
        s.privateWanted = true;
        s.aimMode = 1;
        s.tooltipDelayS = 0.0f;
        s.toasts = false;
        s.overlay = false;
        inputGateConfigure(cfg);
        if (!s.configured) {
            s.configured = true;
            initKeys();
            refreshRowValues();
            takeSnapshot();
            buildPages();
            Log::get().note("menu: flat graphics panel configured; summon with %s.", key.c_str());
        }
        return;
    }
    if (!runtimeVrProfile()) {
        s.configured = false;
        s.summon.setBinding("");
        return;
    }
    const std::string key = cfg.getString("hotkey.menu", "F8");
    const bool summonChanged = key != s.summonName || !s.configured;
    if (summonChanged) {
        s.summonName = key;
        s.summon.setBinding(key.c_str());
        if (s.summon.key() == 0 && !key.empty()) {
            Log::get().note("menu: hotkey.menu = \"%s\" bound nothing (the line above says "
                            "why), so the menu cannot be summoned this session.",
                            key.c_str());
        }
    }
    // Elite's panel keys follow the same switch as the camera keys. The
    // live flip is handled HERE and not by device_hook's fingerprint poll:
    // if the files did not change while the setting was off, the
    // fingerprint still matches and that poll never re-reads. A summon key
    // change re-resolves the cached slots without a file read (R3: a
    // panel key that is the new menu key, or half of its chord, is dropped;
    // the old key stays in hotkey.cpp's append-only registry, so a panel
    // key on it stays refused as an EDVR hotkey until the next launch).
    {
        const bool read = cfg.getBool("hotkey.read_game_bindings", true);
        if (s.configured && s.aliasSource != 0 && read != s.readGameBindings) {
            if (read) {
                Log::get().note("menu keys: hotkey.read_game_bindings turned on; reading your "
                                "Elite panel keys now.");
            }
            menuAdoptGameBindings(read, nullptr);
        } else if (s.configured && summonChanged && s.aliasSource >= 3) {
            if (resolveAliases()) {
                char prefix[96];
                snprintf(prefix, sizeof(prefix), "hotkey.menu changed to %s; re-resolved -- ",
                         key.c_str());
                logAliasOutcome(prefix);
            }
        }
    }
    const std::string kb = cfg.getString("menu.keyboard", "private");
    s.privateWanted = _stricmp(kb.c_str(), "shared") != 0;
    const std::string aim = cfg.getString("menu.aim", "keys");
    s.aimMode = _stricmp(aim.c_str(), "head") == 0 ? 0 : _stricmp(aim.c_str(), "keys") == 0 ? 1 : 2;
    float d = cfg.getFloat("menu.distance", 1.4f);
    if (!(d >= 0.5f) || d > 5.0f) d = 1.4f;
    s.distance = d;
    float c = cfg.getFloat("menu.curve", 0.2f);
    if (!(c >= 0.0f) || c > 0.9f) c = 0.2f;
    s.curve = c;
    float t = cfg.getFloat("menu.text_degrees", 1.1f);
    if (!(t >= 0.6f) || t > 3.0f) t = 1.1f;
    if (s.configured && t != s.textDeg) {
        // The floating monitor has its own reference raster. Make a text
        // setting edit wake its half-second refresh even when the measured
        // line itself has not changed.
        s.overlayTextMs = 0;
    }
    s.textDeg = t;
    float w = cfg.getFloat("menu.width_degrees", 30.0f);
    if (!(w >= 16.0f) || w > 45.0f) w = 30.0f;
    s.idleSeconds = cfg.getIntInRange("menu.idle_dismiss", 0, 0, 600);
    {
        // Under a third of a second is not a dwell, it is a flicker: those
        // values, and 0, mean no tooltip at all.
        float delay = cfg.getFloat("menu.tooltip_delay", 1.5f);
        if (!(delay >= 0.3f)) delay = 0.0f;
        if (delay > 10.0f) delay = 10.0f;
        s.tooltipDelayS = delay;
    }
    // With a tooltip beside it, the panel reaches out to
    // atan(2.14 * tan(w/2)) on the right -- 29.8 degrees at the default
    // width, 41.5 at the widest. Past about 35 the strip leaves one eye's
    // frustum on most headsets and the card renders to one eye only, which
    // is the worst possible thing to do to text somebody has stopped to
    // read. So the card is held to 36 degrees while tooltips are on.
    if (s.tooltipDelayS > 0.0f && w > 36.0f) {
        if (!s.tipWidthNoted) {
            s.tipWidthNoted = true;
            Log::get().note(
                "menu: width_degrees = %.0f is held to 36 while the settings tooltip is on, "
                "because the card beside the panel would otherwise reach past 35 degrees and "
                "be seen by one eye only. Set menu.tooltip_delay = 0 to have the full width "
                "back without it.",
                static_cast<double>(w));
        }
        w = 36.0f;
    }
    // The bitmap changes shape when either of these does, and nothing else
    // would ask for the raster that follows: no [menu] key is a menu row,
    // so this is the hand-edit-while-it-is-up path.
    if (s.configured && (w != s.widthDeg || (s.tooltipDelayS > 0.0f) != s.tooltipsWere)) {
        s.contentDirty = true;
    }
    s.widthDeg = w;
    s.tooltipsWere = s.tooltipDelayS > 0.0f;
    s.toasts = cfg.getBool("menu.toasts", true);
    {
        const bool ov = cfg.getBool("menu.fps_overlay", false);
        const bool ovLock = cfg.getBool("menu.fps_overlay_lock", false);
        float yaw = cfg.getFloat("menu.fps_overlay_yaw", 0.0f);
        float pitch = cfg.getFloat("menu.fps_overlay_pitch", 20.0f);
        if (!(yaw >= -60.0f) || yaw > 60.0f) yaw = 0.0f;
        if (!(pitch >= -45.0f) || pitch > 45.0f) pitch = 20.0f;
        if (s.configured && ov != s.overlay) {
            Log::get().note("menu: the frame-rate overlay is %s.", ov ? "on" : "off");
            if (s.open && (ovLock || s.overlayLock)) s.contentDirty = true;
        }
        if (s.configured && ovLock != s.overlayLock) {
            Log::get().note("menu: the frame-rate overlay is %s while the menu is open.",
                            ovLock ? "locked on" : "no longer locked");
            if (s.open && ov) s.contentDirty = true;
        }
        s.overlay = ov;
        s.overlayLock = ovLock;
        s.overlayYaw = yaw;
        s.overlayPitch = pitch;
    }
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
        const bool openxrScaleRow = isOpenxrRenderScaleRow(d);
        const bool headsetRow = isHeadsetRow(d);
        if (v != r.value) {
            const std::string was = r.value;
            r.value = v;
            if (!byMenu && headsetRow) {
                // The list would toast truncated, and an edit to another
                // headset's entry means nothing here: toast the worn
                // headset's own value, or say the list moved without it.
                const HeadsetRowView now = headsetRowView(d, v);
                char text[96];
                if (!now.headset) {
                    snprintf(text, sizeof(text), "%s: list changed (no headset known)", d.label);
                } else if (now.value == headsetRowView(d, was).value) {
                    snprintf(text, sizeof(text), "%s: list changed (not this headset)", d.label);
                } else {
                    snprintf(text, sizeof(text), "%s: %u for this headset", d.label, now.value);
                }
                changed.push_back(text);
            } else if (!byMenu && openxrScaleRow) {
                // The list would toast truncated; say what it means here.
                // A hand edit that touched only another headset's entry
                // changes nothing for the worn one, and says so rather
                // than announcing a width that is not pending.
                const uint32_t width = resolvedWidth(v);
                char text[96];
                if (width && width == resolvedWidth(was)) {
                    snprintf(text, sizeof(text), "OpenXR res.: list changed (not this headset)");
                } else if (width) {
                    snprintf(text, sizeof(text), "OpenXR res.: %u wide for this headset (after restart)", width);
                } else {
                    snprintf(text, sizeof(text), "OpenXR res.: list changed (after restart)");
                }
                changed.push_back(text);
            } else if (!byMenu) {
                changed.push_back(std::string(d.label) + ": " + displayValue(d, v) +
                                 (d.applies == 2 ? " (at next launch)" : ""));
            }
        }
        if (d.applies == 2 && s.snapshotTaken && openxrScaleRow) {
            // Numerically, against what the host built: a hand edit to
            // another headset's entry or a canonicalising rewrite of the
            // list badges nothing (renderScaleRowPending).
            r.pending = renderScaleRowPending(v);
            if (r.pending && !r.auditNoted) {
                r.auditNoted = true;
                const ResolutionView view = resolutionView(v);
                Log::get().note("edvr.ini: %s now gives this headset (%s) %u wide on disk but is read "
                                "when VR starts; the running value is %u wide (%s). Restart the game "
                                "to apply it.",
                                dotted.c_str(), view.key.c_str(), view.width, view.sizing.activeWidth[0],
                                percentText(edvr::native_render::widthToScale(
                                    view.sizing.activeWidth[0], view.sizing.eyes[0].originalWidth)).c_str());
            }
        } else if (d.applies == 2 && s.snapshotTaken) {
            r.pending = rowPending(d, v, r.snapshot);
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

void menuFlatBeforePresent(IDXGISwapChain* swap, unsigned flags) {
    if (!runtimeFlatProfile() || !g_s.configured || !g_s.open ||
        (flags & DXGI_PRESENT_TEST)) return;
    menuPanelCompositeFlat(swap);
}

void menuFlatResize() {
    if (!runtimeFlatProfile()) return;
    menuPanelFlatResize();
    g_s.lastDrawn = menuDrawnValue();
    g_s.lastDrawnMs = 0;
    g_s.contentDirty = true;
    inputGateSetPrivate(false);
}

void menuTick(ID3D11Device* dev) {
    State& s = g_s;
    if (!s.configured) return;
    if (runtimeFlatProfile()) {
        guardedBudget(g_budget, [&] {
            const uint64_t now = nowMs();
            s.tickMs = now;
            drainWrites();
            if (s.summon.pressed()) {
                if (s.open) closeMenu("the menu key");
                else openMenu(now);
            }
            if (s.summon.takeMissedWhileUnfocused())
                Log::get().note("menu: %s was pressed while Elite was unfocused.",
                                s.summonName.c_str());
            if (s.open && !gameHasFocus()) closeMenu("Elite lost focus");
            if (s.open) {
                const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
                if (esc && !s.flatEscapeDown) {
                    if (s.editEntry >= 0) cancelEdit();
                    else closeMenu("Escape");
                }
                s.flatEscapeDown = esc;
            }
            const uint32_t drawn = menuDrawnValue();
            if (drawn != s.lastDrawn) {
                s.lastDrawn = drawn;
                s.lastDrawnMs = now;
            }
            if (s.open && now - s.openedMs > kNotDrawnCloseMs &&
                now - s.lastDrawnMs > kNotDrawnCloseMs) {
                Log::get().note("menu: flat panel was not drawn; closing and releasing keyboard.");
                closeMenu("not drawn");
            }
            const bool drawnFresh = s.open && s.lastDrawnMs != 0 &&
                                    now - s.lastDrawnMs <= kDrawnFreshMs;
            inputGateSetPrivate(drawnFresh);
            const bool keysReady = drawnFresh && inputGateHoldsGameKeyboard();
            if (keysReady != s.flatKeysReadyShown) {
                s.flatKeysReadyShown = keysReady;
                s.contentDirty = true;
            }
            const bool nativeScale = flatRuntimeNativeScale();
            if (nativeScale != s.flatNativeScaleShown) {
                s.flatNativeScaleShown = nativeScale;
                s.contentDirty = true;
            }
            if (keysReady) handleKeys(now);
            if (s.open && dev && !menuPanelFlatRasterReady()) s.contentDirty = true;
            if (s.open && s.contentDirty) {
                s.contentDirty = false;
                MenuContent c;
                buildContent(c);
                menuPanelSubmit(c);
            }
            MenuGeometry g;
            g.dist = 1.0f;
            g.curve = 0.0f;
            g.halfW = 0.62f;
            g.alpha = s.open ? 1.0f : 0.0f;
            menuPanelSetGeometry(g);
            inputGateTick();
            if (dev) menuPanelTick(dev);
        });
        if (!g_budget.shouldRun()) inputGateSetPrivate(false);
        return;
    }
    guardedBudget(g_budget, [&] {
        static uint64_t lastNativeRevision = 0;
        const uint64_t nativeRevision = nativeMenuRevision();
        if (nativeRevision != lastNativeRevision) {
            lastNativeRevision = nativeRevision;
            closeMenu("native tracking origin or availability changed");
            s.alpha = 0.0f;
        }
        const uint64_t now = nowMs();
        const uint64_t dt = s.lastTickMs ? now - s.lastTickMs : 0;
        s.lastTickMs = now;
        // The clock the content build reads, so the tooltip's dwell and the
        // raster agree on when the row was last moved.
        s.tickMs = now;

        // The monitor's ring: one clock read and a store per frame, and the
        // compositor's sample when a new one was published. Its slow
        // samplers run only while the Monitor page is showing.
        perfMonitorFrame(dev);
        perfMonitorSetActive(s.open && s.pages[s.page].monitor);
        // Writes the worker finished since last frame.
        drainWrites();

        // The summon key: EDVR's own, focus-gated. With Shift, recentre.
        if (s.summon.pressed()) {
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (!s.open) {
                openMenu(now);
            } else if (shift) {
                latchAnchor(0.0f, 0.0f);
                s.lastInputMs = now;
                Log::get().note("menu: recentred where you are looking.");
            } else {
                closeMenu("the menu key");
            }
        }
        if (s.summon.takeMissedWhileUnfocused()) {
            Log::get().note("menu: %s was received but another application had focus. "
                            "Focus Elite's desktop window and press it again.",
                            s.summonName.c_str());
        }

        // Refresh the read-only OpenXR sizing snapshot while the menu is
        // visible. Comparing the whole POD also notices a stop/restart that
        // happened while the menu was hidden, without opening an XR session.
        if (s.open) {
            EdvrNativeRenderSizing latest{};
            const bool available = readNativeRenderSizing(&latest);
            const bool changed = available != s.renderSizingAvailable ||
                                 (available &&
                                  std::memcmp(&latest, &s.renderSizing, sizeof(latest)) != 0);
            if (changed) {
                s.renderSizingAvailable = available;
                s.renderSizing = available ? latest : EdvrNativeRenderSizing{};
                s.contentDirty = true;
                // A re-init that republished the sizing clears or raises
                // the per-headset row's badge without a write.
                if (s.snapshotTaken) {
                    for (int i = 0; i < kRowDefCount; ++i) {
                        const MenuRowDef& row = kMenuRows[i];
                        if (row.applies == 2 &&
                            (isOpenxrRenderScaleRow(row) || isHeadsetRow(row))) {
                            g_rows[i].pending = rowPending(row, g_rows[i].value,
                                                           g_rows[i].snapshot);
                        }
                    }
                }
            }
        }

        if (s.open) {
            // Escape, then the navigation keys, then the head. Escape ends
            // a value being typed before it closes the menu, so a typed
            // value can be abandoned without losing the panel.
            static bool escDown = false;
            const bool esc = gameHasFocus() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            if (esc && !escDown) {
                if (s.editEntry >= 0) {
                    cancelEdit();
                    s.lastInputMs = now;
                } else {
                    closeMenu("Escape");
                }
            }
            escDown = esc;
        }
        if (s.open) {
            handleKeys(now);
            handleAim(now);
            // The tooltip's dwell has passed: nothing else changed, so ask
            // for the one re-raster that brings it up.
            if (!s.tooltipUp && tooltipsOn() && !s.pages[s.page].status &&
                now >= s.highlightSinceMs + static_cast<uint64_t>(s.tooltipDelayS * 1000.0f)) {
                s.contentDirty = true;
            }
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

        // Toasts, when the menu is down; the overlay when nothing else is up.
        if (!s.open && s.alpha <= 0.0f) {
            if (!s.toastUp && !s.toastQueue.empty()) {
                s.toastText = s.toastQueue.front();
                s.toastQueue.erase(s.toastQueue.begin());
                latchAnchor(0.0f, kToastPitchDeg);
                if (s.anchorValid) {
                    s.toastUp = true;
                    s.overlayUp = false;
                    s.overlayAlpha = 0.0f;
                    s.toastUntilMs = now + kToastMs;
                    MenuContent c;
                    buildToastContent(c, s.toastText, kToastWidthDeg, 1.0f);
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
            // The head-locked readout: the door anchors it to each frame's
            // own pose (setMenuHeadLock below), so it rides the look with no
            // lag, the toolkit's way; its text is refreshed twice a second.
            // Never while a toast has the panel.
            if (s.overlay && !s.toastUp) {
                float pose[12];
                s.anchorValid = headPose(pose);
                if (s.anchorValid) {
                    if (!s.overlayUp || dueMs(s.overlayTextMs, 500)) {
                        s.overlayTextMs = stampMs();
                        char line[120];
                        perfMonitorOverlayLine(line, sizeof(line));
                        if (!s.overlayUp || line != s.overlayText || s.overlayTextDeg != s.textDeg) {
                            s.overlayText = line;
                            MenuContent c;
                            if (menuPanelBuildOverlayContent(c, s.overlayText.c_str(), s.textDeg,
                                                              nullptr)) {
                                s.overlayTextDeg = s.textDeg;
                                menuPanelSubmit(c);
                            }
                        }
                        s.overlayUp = true;
                    }
                }
            } else if (s.overlayUp && !s.overlay) {
                s.overlayUp = false;
                s.overlayAlpha = 0.0f;
            }
            if (s.overlayUp && !s.toastUp) {
                const float rate = dt > 0 ? static_cast<float>(dt) / static_cast<float>(kFadeMs) : 1.0f;
                const float target = 0.9f;
                if (s.overlayAlpha < target) s.overlayAlpha = s.overlayAlpha + rate > target ? target : s.overlayAlpha + rate;
            } else {
                s.overlayAlpha = 0.0f;
            }
        } else {
            s.toastUp = false;
            s.toastAlpha = 0.0f;
            s.overlayUp = false;
            s.overlayAlpha = 0.0f;
        }

        // The gate, then content, geometry, visibility. The gate is decided
        // BEFORE the content is built: the footer's fault line reads the
        // gate, and built first it read the closed menu's 0 on the open
        // tick and composed KEYS SHARED WITH THE GAME for a gate that went
        // private a few lines later -- at HEAD that sat past the old
        // single line's clip and was never seen; the two-line footer shows
        // it. Nothing between here and the old spot read the gate.
        const bool showingMenu = s.alpha > 0.0f && !s.toastUp;
        const bool drawnFresh = now - s.lastDrawnMs <= kDrawnFreshMs;
        inputGateSetPrivate(s.open && showingMenu && drawnFresh && !s.pages[s.page].status);
        // The fault line follows the gate the way the legend follows the
        // live predicate: a raster that said one thing while the gate now
        // says another is re-sent, so the warning appears when the draw
        // stalls and goes when it resumes, on a rig with nothing adopted
        // too (the legend's own comparison fires only with aliases).
        const bool sharedWarn = s.open && s.privateWanted && !s.pages[s.page].status && !inputGatePrivate();
        if (sharedWarn != s.sharedWarnShown) {
            s.sharedWarnShown = sharedWarn;
            s.contentDirty = true;
        }
        if (s.open || s.alpha > 0.0f) {
            Page& p = s.pages[s.page];
            if (p.status && dueMs(s.statusRefreshMs, p.monitor ? kMonitorRefreshMs : kStatusRefreshMs)) {
                s.statusRefreshMs = stampMs();
                s.contentDirty = true;
            }
            // The locked FPS readout refreshes at the same cadence as the
            // head-locked overlay: twice a second, cheaply.
            if (s.open && s.overlay && s.overlayLock && dueMs(s.menuOverlayTextMs, 500)) {
                s.menuOverlayTextMs = stampMs();
                s.contentDirty = true;
            }
            if (s.contentDirty) {
                s.contentDirty = false;
                MenuContent c;
                buildContent(c);
                menuPanelSubmit(c);
            }
        }
        const bool showingOverlay = !showingMenu && !s.toastUp && s.overlayUp && s.overlayAlpha > 0.0f;
        MenuGeometry g;
        g.dist = s.distance;
        g.curve = (s.toastUp || showingOverlay) ? 0.0f : s.curve;
        // The toast and the head-locked overlay are their own bitmaps with
        // no strip, so they take no shift.
        const bool menuBranch = !s.toastUp && !showingOverlay;
        const float widthDeg =
            s.toastUp ? kToastWidthDeg : showingOverlay ? kOverlayWidthDeg : s.widthDeg;
        g.halfW = panelHalfW(widthDeg, menuBranch && tooltipsOn());
        g.shift = panelShift(g.halfW, menuBranch && tooltipsOn());
        g.alpha = s.toastUp ? s.toastAlpha : showingOverlay ? s.overlayAlpha : s.alpha;
        g.overlayRaster = showingOverlay;
        if (!glitchConsumerPresent() && !nativeMenuAvailable()) {
            g.alpha = 0.0f;
            inputGateSetPrivate(false);
        }
        menuPanelSetGeometry(g);
        setMenuHeadLock(showingOverlay, s.overlayYaw, s.overlayPitch);
        setMenuVisible(g.alpha);
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
    setMenuHeadLock(false, 0.0f, 0.0f);
    inputGateShutdown();
    stopWriter();
    menuPanelShutdown();
    perfMonitorShutdown();
}

}  // namespace edvr
