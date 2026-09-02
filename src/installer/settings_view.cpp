#include "settings_view.h"

#include <commctrl.h>
#include <windowsx.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <vector>

#include "detect.h"
#include "mirror.h"
#include "ui.h"

#pragma comment(lib, "comctl32.lib")

namespace edvr::installer {
namespace {

const wchar_t* kClassName = L"EdvrSettingsList";
const int kEditId = 9001;
const int kComboId = 9002;

// Everything below is in 96-dpi units.
const int kPad = 18;
const int kHeaderHeight = 38;
const int kRowHeight = 68;
const int kControlWidth = 240;

// Resolution is the one composite row: vscreen_res_width and _height shown
// as a single dropdown of standard 16:9 sizes, with the two raw rows
// appearing beneath it only when the pair matches no preset (or Custom was
// picked outright).
enum class ItemKind { Header, Setting, Resolution };

struct Item {
    ItemKind    kind = ItemKind::Setting;
    std::wstring title;   // headers
    size_t      row = 0;  // settings: index into the model
    int         y = 0;    // laid out, in 96-dpi units
    int         height = 0;
};

struct List {
    HWND           hwnd = nullptr;
    HWND           edit = nullptr;
    HWND           combo = nullptr;  // the resolution dropdown, shared
    SettingsModel* model = nullptr;
    std::wstring   mirrorDir;  // where each write is echoed; see mirror.h
    UINT           dpi = 96;
    int            scroll = 0;      // pixels
    int            contentHeight = 0;
    std::vector<Item> items;
    std::wstring   filter;
    size_t         editingRow = SIZE_MAX;
    std::string    error;
    bool           dragThumb = false;   // the slim scrollbar's drag state
    int            dragOffset = 0;      // grab point within the thumb
    // The resolution pair's row indices (SIZE_MAX when the schema lacks
    // them), and whether Custom stays open after being picked outright.
    size_t         resWidthRow = SIZE_MAX;
    size_t         resHeightRow = SIZE_MAX;
    bool           resolutionCustom = false;
};

List* listOf(HWND hwnd) {
    return reinterpret_cast<List*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int dp(const List* list, int v) { return ui::dp(v, list->dpi); }

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

bool isVscreenKey(const SettingDef& def, const char* key) {
    return strcmp(def.section, "fix") == 0 && strcmp(def.key, key) == 0;
}

int presetIndexFor(const std::string& w, const std::string& h) {
    const auto& presets = vscreenPresets();
    for (size_t i = 0; i < presets.size(); ++i) {
        if (w == presets[i].w && h == presets[i].h) return static_cast<int>(i);
    }
    return -1;
}

std::wstring presetLabel(const ResolutionPreset& preset) {
    return fromUtf8(std::string(preset.w) + " x " + preset.h);
}

// A row's effective value: what the file says, or what it ships as when
// the file says nothing.
std::string effectiveValue(const SettingRow& row) {
    return row.value.empty() ? std::string(row.def->shipped) : row.value;
}

void hideCombo(List* list) {
    if (list->combo) ShowWindow(list->combo, SW_HIDE);
}

bool matches(const SettingRow& row, const std::wstring& needle) {
    if (needle.empty()) return true;
    const std::wstring hay = lower(fromUtf8(std::string(row.def->label) + " " + row.def->key +
                                            " " + row.def->section + " " + row.def->description));
    return hay.find(needle) != std::wstring::npos;
}

// edvr.ini's own headings -- "The on-foot screen", "The Full System Scanner"
// -- are what the window shows. A setting with no heading above it falls back
// to its section name, which is an address rather than a heading, but is
// better than an untitled run of rows.
std::wstring headingFor(const SettingDef& def) {
    if (def.group && *def.group) return fromUtf8(def.group);
    const std::string section = def.section;
    if (section == "fix") return L"The fixes";
    if (section == "advanced") return L"Advanced";
    return fromUtf8(section);
}

void rebuildItems(List* list) {
    list->items.clear();
    if (!list->model) return;

    const std::wstring needle = lower(list->filter);
    std::wstring heading;
    bool haveHeading = false;
    int y = kPad;

    const std::vector<SettingRow>& rows = list->model->rows();

    // The resolution pair, found fresh each rebuild (the schema decides
    // whether it exists). Its raw rows show only while Custom is active.
    list->resWidthRow = list->resHeightRow = SIZE_MAX;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (isVscreenKey(*rows[i].def, "vscreen_res_width")) list->resWidthRow = i;
        if (isVscreenKey(*rows[i].def, "vscreen_res_height")) list->resHeightRow = i;
    }
    const bool haveRes = list->resWidthRow != SIZE_MAX && list->resHeightRow != SIZE_MAX;
    const bool customActive =
        haveRes && (list->resolutionCustom ||
                    presetIndexFor(effectiveValue(rows[list->resWidthRow]),
                                   effectiveValue(rows[list->resHeightRow])) < 0);

    for (size_t i = 0; i < rows.size(); ++i) {
        if (!matches(rows[i], needle)) continue;
        const std::wstring wanted = headingFor(*rows[i].def);
        if (!haveHeading || wanted != heading) {
            heading = wanted;
            haveHeading = true;
            Item header;
            header.kind = ItemKind::Header;
            header.title = heading;
            header.y = y;
            header.height = kHeaderHeight;
            list->items.push_back(header);
            y += kHeaderHeight;
        }
        if (haveRes && i == list->resWidthRow) {
            Item res;
            res.kind = ItemKind::Resolution;
            res.row = i;
            res.y = y;
            res.height = kRowHeight;
            list->items.push_back(res);
            y += kRowHeight;
            if (!customActive) continue;   // the raw width row stays hidden
        }
        if (haveRes && i == list->resHeightRow && !customActive) continue;
        Item item;
        item.kind = ItemKind::Setting;
        item.row = i;
        item.y = y;
        item.height = kRowHeight;
        list->items.push_back(item);
        y += kRowHeight;
    }
    list->contentHeight = y + kPad;
}

// Clamps the position; the bar itself is the slim thumb paint() draws (the
// native scrollbar was the one piece of stock chrome left on this window,
// and it never matched the cards it sat beside).
void updateScrollbar(List* list) {
    RECT client{};
    GetClientRect(list->hwnd, &client);
    const int page = client.bottom - client.top;
    const int content = dp(list, list->contentHeight);
    list->scroll = std::max(0, std::min(list->scroll, std::max(0, content - page)));
}

void commitEdit(List* list);

void hideEdit(List* list) {
    if (list->edit) ShowWindow(list->edit, SW_HIDE);
    list->editingRow = SIZE_MAX;
}

void scrollTo(List* list, int position) {
    commitEdit(list);
    hideEdit(list);
    hideCombo(list);
    RECT client{};
    GetClientRect(list->hwnd, &client);
    const int page = client.bottom - client.top;
    const int content = dp(list, list->contentHeight);
    const int clamped = std::max(0, std::min(position, std::max(0, content - page)));
    if (clamped == list->scroll) return;
    list->scroll = clamped;
    updateScrollbar(list);
    InvalidateRect(list->hwnd, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// geometry within a row, all in device pixels
// ---------------------------------------------------------------------------

struct RowGeometry {
    RECT label;
    RECT description;
    RECT control;      // the toggle, segments or value box
    RECT recommended;  // the pill under it
};

RowGeometry geometryFor(const List* list, const RECT& row) {
    RowGeometry geo{};
    const int pad = dp(list, kPad);
    const int controlWidth = dp(list, kControlWidth);

    geo.label = {row.left + pad, row.top + dp(list, 8), row.right - controlWidth - pad,
                 row.top + dp(list, 28)};
    geo.description = {row.left + pad, row.top + dp(list, 28), row.right - controlWidth - pad,
                       row.top + dp(list, 62)};
    geo.control = {row.right - controlWidth, row.top + dp(list, 10), row.right - pad,
                   row.top + dp(list, 38)};
    geo.recommended = {row.right - controlWidth, row.top + dp(list, 42), row.right - pad,
                       row.top + dp(list, 60)};
    return geo;
}

// Segment rectangles for a choice row, right-aligned inside the control area.
std::vector<RECT> segmentsFor(const List* list, const RowGeometry& geo, size_t count) {
    std::vector<RECT> out;
    if (count == 0) return out;
    const int gap = dp(list, 4);
    const int total = geo.control.right - geo.control.left;
    const int width = (total - gap * static_cast<int>(count - 1)) / static_cast<int>(count);
    int x = geo.control.left;
    for (size_t i = 0; i < count; ++i) {
        RECT r{x, geo.control.top, x + width, geo.control.bottom};
        out.push_back(r);
        x += width + gap;
    }
    return out;
}

RECT toggleRect(const List* list, const RowGeometry& geo) {
    const int width = dp(list, 46);
    const int height = dp(list, 24);
    RECT r{geo.control.right - width, geo.control.top + dp(list, 2), geo.control.right,
           geo.control.top + dp(list, 2) + height};
    return r;
}

RECT valueRect(const List* list, const RowGeometry& geo) {
    const int width = dp(list, 140);
    RECT r{geo.control.right - width, geo.control.top, geo.control.right, geo.control.bottom};
    return r;
}

COLORREF mix(COLORREF a, COLORREF b, int percentB) {
    const int r = (GetRValue(a) * (100 - percentB) + GetRValue(b) * percentB) / 100;
    const int g = (GetGValue(a) * (100 - percentB) + GetGValue(b) * percentB) / 100;
    const int bl = (GetBValue(a) * (100 - percentB) + GetBValue(b) * percentB) / 100;
    return RGB(r, g, bl);
}

bool isOn(const std::string& value) {
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

// ---------------------------------------------------------------------------
// painting
// ---------------------------------------------------------------------------

// Only the ones that need a restart are marked. Every other row is live,
// which the line above the list says once -- twenty rows each carrying a
// "live" badge would say it twenty times and mean less each time.
void drawRestartBadge(const List* list, HDC dc, const RECT& labelRect,
                      const std::wstring& label) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    const std::wstring mark = L"restart the game";
    const int width = ui::textWidth(dc, mark, f.caption) + dp(list, 16);
    const int labelWidth = ui::textWidth(dc, label, f.bodyBold);
    RECT badge{labelRect.left + labelWidth + dp(list, 10), labelRect.top + dp(list, 2),
               labelRect.left + labelWidth + dp(list, 10) + width,
               labelRect.bottom + dp(list, 1)};
    if (badge.right < labelRect.right) {
        ui::fillRounded(dc, badge, (badge.bottom - badge.top) / 2, mix(t.cardBg, t.warn, 18));
        ui::drawText(dc, mark, badge, f.caption, t.warn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// A two-way choice where one side is "leave the game alone" reads better as
// a switch than as two buttons: on is the fix, off is stock. The value
// written stays the choice string -- only the control changes.
bool twoChoiceToggle(const SettingRow& row, std::string* onValue,
                     std::string* offValue) {
    if (!row.def || row.def->kind != SettingKind::Choice ||
        row.choices.size() != 2) {
        return false;
    }
    int off = -1;
    for (int i = 0; i < 2; ++i) {
        if (row.choices[i].value == "stock" || row.choices[i].value == "off") {
            off = i;
        }
    }
    if (off < 0) return false;
    if (onValue) *onValue = row.choices[1 - off].value;
    if (offValue) *offValue = row.choices[off].value;
    return true;
}

// What a switch is SET to, in the file's own word. A switch shows a position,
// not a word, and nothing else on the row says that on here writes "steady"
// and off writes "stock". Suppressed where the word would only repeat the
// switch -- a bool, or a choice whose two values already are on and off -- and
// absent for a segmented choice, whose segments carry the words themselves.
std::wstring stateWord(const SettingRow& row) {
    std::string onValue, offValue;
    if (!twoChoiceToggle(row, &onValue, &offValue)) return std::wstring();
    if (onValue == "on" && offValue == "off") return std::wstring();
    const std::string current = row.value == onValue ? onValue : offValue;
    for (const Choice& choice : row.choices) {
        if (choice.value == current) return fromUtf8(choice.label);
    }
    return fromUtf8(current);
}

// The recommended value as somebody would say it: a toggle holds 1 or 0 in the
// file, and "1" under a switch names nothing.
std::wstring recommendedWord(const SettingRow& row) {
    if (row.def->kind == SettingKind::Toggle) {
        return isOn(row.def->recommended) ? L"on" : L"off";
    }
    return row.shownRecommended();
}

// The one link in a row, shown only when the value is not the recommended one,
// and worded for what it DOES. The old text named the value instead -- "use
// splash" under a switch, which the field read as a state label, and then read
// "use auto" beside an auto/on/off control as the control being wrong about
// itself. Where the recommendation IS the shipped default this is the way
// back; where it is not -- a 0.3 curve with a 0.7 distance is a tested pairing
// and neither number is the default -- it is an invitation. One word, because
// the longest helper it sits beside -- the resolution rows' default and their
// range -- leaves 74 px of the 222 this column has, and a range ellipsised to
// "640 to 819" is a wrong number rather than a shortened one.
std::wstring recommendLink(const SettingRow& row) {
    if (row.isRecommended) return std::wstring();
    if (strcmp(row.def->recommended, row.def->shipped) == 0) return L"reset";
    return L"try " + recommendedWord(row);
}

// The link's own rectangle: the whole of the click target and none of the
// quiet text beside it. The old line was one target end to end, so a click on
// the "default splash" half applied the recommendation -- which is exactly why
// one line of two phrases read as two links.
RECT recommendLinkRect(const List* list, HDC dc, const RowGeometry& geo,
                       const std::wstring& label) {
    RECT r{0, 0, 0, 0};
    if (label.empty()) return r;
    r = geo.recommended;
    r.left = r.right - ui::textWidth(dc, label, ui::fonts().caption) - dp(list, 4);
    return r;
}

void paintRow(const List* list, HDC dc, const RECT& rowRect, const SettingRow& row) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    const RowGeometry geo = geometryFor(list, rowRect);

    RECT labelRect = geo.label;
    ui::drawText(dc, fromUtf8(row.def->label), labelRect, f.bodyBold, t.text,
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (row.def->needsRestart) {
        drawRestartBadge(list, dc, labelRect, fromUtf8(row.def->label));
    }
    RECT description = geo.description;
    ui::drawText(dc, fromUtf8(row.def->summary), description, f.caption, t.subtext,
                 DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);

    switch (row.def->kind) {
        case SettingKind::Toggle: {
            const RECT box = toggleRect(list, geo);
            const bool on = isOn(row.value);
            const int height = box.bottom - box.top;
            ui::fillRounded(dc, box, height / 2, on ? t.accent : t.control);
            if (!on) ui::strokeRounded(dc, box, height / 2, t.controlBorder);
            const int knob = height / 2 - dp(list, 3);
            const int cx = on ? box.right - knob - dp(list, 4) : box.left + knob + dp(list, 4);
            ui::fillCircle(dc, cx, (box.top + box.bottom) / 2, knob,
                           on ? t.accentText : t.subtext);
            break;
        }
        case SettingKind::Choice: {
            std::string onValue;
            if (twoChoiceToggle(row, &onValue, nullptr)) {
                const RECT box = toggleRect(list, geo);
                const bool on = row.value == onValue;
                const int height = box.bottom - box.top;
                ui::fillRounded(dc, box, height / 2, on ? t.accent : t.control);
                if (!on) ui::strokeRounded(dc, box, height / 2, t.controlBorder);
                const int knob = height / 2 - dp(list, 3);
                const int cx =
                    on ? box.right - knob - dp(list, 4) : box.left + knob + dp(list, 4);
                ui::fillCircle(dc, cx, (box.top + box.bottom) / 2, knob,
                               on ? t.accentText : t.subtext);
                break;
            }
            const std::vector<RECT> segments = segmentsFor(list, geo, row.choices.size());
            for (size_t i = 0; i < segments.size(); ++i) {
                const bool selected = row.choices[i].value == row.value;
                ui::fillRounded(dc, segments[i], dp(list, 5), selected ? t.accent : t.control);
                if (!selected) ui::strokeRounded(dc, segments[i], dp(list, 5), t.controlBorder);
                RECT text = segments[i];
                ui::drawText(dc, fromUtf8(row.choices[i].label), text, f.caption,
                             selected ? t.accentText : t.text,
                             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            break;
        }
        default: {
            const RECT box = valueRect(list, geo);
            ui::fillRounded(dc, box, dp(list, 5), t.control);
            ui::strokeRounded(dc, box, dp(list, 5), t.controlBorder);
            RECT text = box;
            text.left += dp(list, 8);
            text.right -= dp(list, 8);
            const std::wstring shown =
                row.value.empty() ? std::wstring(L"(not set)") : row.shown();
            ui::drawText(dc, shown, text, f.body,
                         row.value.empty() ? t.muted : t.text,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            break;
        }
    }

    // Under the control, at most two things and never two links: one quiet
    // word for what a switch is set to (for a number, what it ships as and
    // what it will take), and -- only when the value is not the recommended
    // one -- the single link that puts it right. The link is drawn first, so a
    // long quiet half is the half that gets ellipsised.
    const bool isChoice = row.def->kind == SettingKind::Choice;
    const std::wstring link = recommendLink(row);
    const std::wstring quiet = isChoice ? stateWord(row) : row.helper();
    RECT line = geo.recommended;
    if (!link.empty()) {
        ui::drawText(dc, link, line, f.caption, t.accent, DT_RIGHT | DT_SINGLELINE);
        line.right -= ui::textWidth(dc, link, f.caption) + dp(list, 12);
    }
    if (!quiet.empty() && line.right > line.left) {
        ui::drawText(dc, quiet, line, f.caption, isChoice ? t.subtext : t.muted,
                     DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// The composite resolution row: one dropdown-shaped box showing the pair as
// "1920 x 1080" (or "Custom"), backed by the two real keys. The label and
// summary come from the width row's own def, so the ini's words are the
// window's words here too.
void paintResolutionRow(const List* list, HDC dc, const RECT& rowRect) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    const RowGeometry geo = geometryFor(list, rowRect);
    const SettingRow& width = list->model->rows()[list->resWidthRow];
    const SettingRow& height = list->model->rows()[list->resHeightRow];

    const std::wstring label = L"On-foot screen resolution";
    ui::drawText(dc, label, geo.label, f.bodyBold, t.text,
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (width.def->needsRestart) drawRestartBadge(list, dc, geo.label, label);
    ui::drawText(dc, fromUtf8(width.def->summary), geo.description, f.caption, t.subtext,
                 DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);

    const RECT box = valueRect(list, geo);
    ui::fillRounded(dc, box, dp(list, 5), t.control);
    ui::strokeRounded(dc, box, dp(list, 5), t.controlBorder);
    const int preset = presetIndexFor(effectiveValue(width), effectiveValue(height));
    RECT text = box;
    text.left += dp(list, 8);
    text.right -= dp(list, 24);
    ui::drawText(dc, preset >= 0 ? presetLabel(vscreenPresets()[preset]) : L"Custom", text,
                 f.body, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT chevron{box.right - dp(list, 22), box.top, box.right - dp(list, 6), box.bottom};
    ui::drawText(dc, L"\x25BE", chevron, f.caption, t.subtext,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void paint(List* list) {
    PAINTSTRUCT ps;
    HDC screen = BeginPaint(list->hwnd, &ps);
    RECT client{};
    GetClientRect(list->hwnd, &client);

    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
    HBITMAP previous = static_cast<HBITMAP>(SelectObject(dc, bitmap));

    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    ui::fillRect(dc, client, t.cardBg);

    if (!list->model || list->items.empty()) {
        RECT empty = client;
        empty.left += dp(list, kPad);
        empty.top += dp(list, kPad);
        ui::drawText(dc,
                     list->model ? L"Nothing matches that."
                                 : L"Pick an install on the Setup tab first.",
                     empty, f.body, t.subtext, DT_LEFT | DT_WORDBREAK);
    }

    for (const Item& item : list->items) {
        RECT rect{client.left, dp(list, item.y) - list->scroll, client.right,
                  dp(list, item.y + item.height) - list->scroll};
        if (rect.bottom < 0 || rect.top > client.bottom) continue;

        if (item.kind == ItemKind::Header) {
            RECT text = rect;
            text.left += dp(list, kPad);
            text.top += dp(list, 12);
            ui::drawText(dc, item.title, text, f.heading, t.subtext, DT_LEFT | DT_SINGLELINE);
        } else {
            if (item.kind == ItemKind::Resolution) {
                paintResolutionRow(list, dc, rect);
            } else {
                paintRow(list, dc, rect, list->model->rows()[item.row]);
            }
            RECT line{rect.left + dp(list, kPad), rect.bottom - 1, rect.right - dp(list, kPad),
                      rect.bottom};
            ui::fillRect(dc, line, t.cardBorder);
        }
    }

    ui::drawSlimScrollbar(dc, client, dp(list, list->contentHeight), client.bottom, list->scroll,
                          list->dpi, list->dragThumb);

    BitBlt(screen, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(list->hwnd, &ps);
}

// ---------------------------------------------------------------------------
// editing
// ---------------------------------------------------------------------------

void applyValue(List* list, size_t rowIndex, const std::string& value) {
    if (!list->model) return;
    if (!list->model->set(rowIndex, value)) {
        list->error = list->model->lastError();
        MessageBoxW(GetParent(list->hwnd), fromUtf8(list->error).c_str(), L"EDVR installer",
                    MB_OK | MB_ICONWARNING);
    } else {
        list->error.clear();
        // Echoed outside the game folder on every successful write, not just
        // on install: the mirror exists so a game update between now and the
        // next install cannot take today's change with it.
        updateMirrorIni(list->model->gameDir(), list->mirrorDir);
    }
    InvalidateRect(list->hwnd, nullptr, TRUE);
}

void commitEdit(List* list) {
    if (list->editingRow == SIZE_MAX || !list->edit) return;
    wchar_t buffer[512]{};
    GetWindowTextW(list->edit, buffer, 512);
    const size_t row = list->editingRow;
    list->editingRow = SIZE_MAX;  // before applying: the write repaints
    if (!list->model) return;
    std::string wanted;
    // "30", "30%" and "0.3" all mean the same on a percentage setting, and
    // anything that is not a number at all is dropped rather than written.
    if (!list->model->rows()[row].parseTyped(buffer, &wanted)) return;
    if (wanted != list->model->rows()[row].value) applyValue(list, row, wanted);
}

LRESULT CALLBACK editProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                          DWORD_PTR ref) {
    List* list = reinterpret_cast<List*>(ref);
    switch (message) {
        case WM_KEYDOWN:
            if (wparam == VK_RETURN) {
                commitEdit(list);
                hideEdit(list);
                SetFocus(list->hwnd);
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                list->editingRow = SIZE_MAX;
                hideEdit(list);
                SetFocus(list->hwnd);
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            commitEdit(list);
            hideEdit(list);
            break;
        case WM_CHAR:
            // Swallow the beep an EDIT makes on Return.
            if (wparam == VK_RETURN || wparam == VK_ESCAPE) return 0;
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void beginEdit(List* list, size_t rowIndex, const RECT& box) {
    if (!list->edit) {
        list->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10,
                                     list->hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)),
                                     nullptr, nullptr);
        SetWindowSubclass(list->edit, editProc, 1, reinterpret_cast<DWORD_PTR>(list));
    }
    SendMessageW(list->edit, WM_SETFONT, reinterpret_cast<WPARAM>(ui::fonts().body), TRUE);
    const std::wstring value = list->model->rows()[rowIndex].shown();
    SetWindowTextW(list->edit, value.c_str());
    // Sized to the text and centred in the value box: a single-line EDIT
    // draws its text at the top of the control, so a control that fills
    // the box shows the text riding high.
    const int textH = ui::textHeightPx(ui::fonts().body);
    const int boxH = box.bottom - box.top;
    const int top = box.top + (boxH > textH ? (boxH - textH) / 2 : 0);
    MoveWindow(list->edit, box.left + dp(list, 6), top,
               (box.right - box.left) - dp(list, 12), textH, TRUE);
    ShowWindow(list->edit, SW_SHOW);
    SetFocus(list->edit);
    SendMessageW(list->edit, EM_SETSEL, 0, -1);
    list->editingRow = rowIndex;
}

// The resolution dropdown is a real combo, created once and shown over the
// row's box on demand -- the inline EDIT's pattern exactly, and for the
// same reason: keyboard selection and the wheel are what a hand-rolled
// dropdown gets subtly wrong.
void openResolutionCombo(List* list, const RECT& box) {
    if (!list->combo) {
        list->combo = CreateWindowExW(0, L"COMBOBOX", L"",
                                      WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST |
                                          CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                      0, 0, 10, 10, list->hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kComboId)),
                                      nullptr, nullptr);
        SendMessageW(list->combo, WM_SETFONT,
                     reinterpret_cast<WPARAM>(ui::fonts().body), TRUE);
        if (ui::theme().dark) SetWindowTheme(list->combo, L"DarkMode_CFD", nullptr);
    }
    SendMessageW(list->combo, CB_RESETCONTENT, 0, 0);
    for (const ResolutionPreset& preset : vscreenPresets()) {
        SendMessageW(list->combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(presetLabel(preset).c_str()));
    }
    SendMessageW(list->combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom..."));

    const SettingRow& width = list->model->rows()[list->resWidthRow];
    const SettingRow& height = list->model->rows()[list->resHeightRow];
    const int preset = presetIndexFor(effectiveValue(width), effectiveValue(height));
    SendMessageW(list->combo, CB_SETCURSEL,
                 preset >= 0 ? static_cast<WPARAM>(preset) : vscreenPresets().size(), 0);

    SendMessageW(list->combo, CB_SETITEMHEIGHT, 0, dp(list, 24));
    const int fieldH = box.bottom - box.top;
    SendMessageW(list->combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1),
                 fieldH > dp(list, 8) ? fieldH - dp(list, 8) : fieldH);
    MoveWindow(list->combo, box.left, box.top, box.right - box.left, fieldH + dp(list, 220),
               TRUE);
    // The combo's own FIELD never shows: an empty window region clips it to
    // zero pixels, so the painted box underneath stays the only field
    // anybody sees and opening the list changes nothing above it. The
    // dropdown is the combo's separate popup window, which a region on the
    // combo does not touch -- so the list still drops, drawn our way, with
    // the combo's native keyboard and wheel behaviour intact.
    SetWindowRgn(list->combo, CreateRectRgn(0, 0, 0, 0), FALSE);
    ShowWindow(list->combo, SW_SHOW);
    SetFocus(list->combo);
    SendMessageW(list->combo, CB_SHOWDROPDOWN, TRUE, 0);
}

// The row whose link is under the pointer, if any. The click and the cursor
// both ask this one question, so what is clickable can never drift from what
// was drawn -- and the quiet text beside the link is no longer part of it.
bool linkHit(List* list, int x, int y, size_t* rowIndex) {
    if (!list->model) return false;
    RECT client{};
    GetClientRect(list->hwnd, &client);
    for (const Item& item : list->items) {
        if (item.kind != ItemKind::Setting) continue;
        RECT rect{client.left, dp(list, item.y) - list->scroll, client.right,
                  dp(list, item.y + item.height) - list->scroll};
        if (y < rect.top || y >= rect.bottom) continue;
        const SettingRow& row = list->model->rows()[item.row];
        const std::wstring label = recommendLink(row);
        if (label.empty()) return false;
        HDC dc = GetDC(list->hwnd);
        const RECT box = recommendLinkRect(list, dc, geometryFor(list, rect), label);
        ReleaseDC(list->hwnd, dc);
        const POINT point{x, y};
        if (!PtInRect(&box, point)) return false;
        if (rowIndex) *rowIndex = item.row;
        return true;
    }
    return false;
}

void onClick(List* list, int x, int y) {
    commitEdit(list);
    hideEdit(list);
    if (!list->model) return;

    size_t linkRow = 0;
    if (linkHit(list, x, y, &linkRow)) {
        applyValue(list, linkRow, list->model->rows()[linkRow].def->recommended);
        return;
    }

    RECT client{};
    GetClientRect(list->hwnd, &client);

    for (const Item& item : list->items) {
        if (item.kind == ItemKind::Header) continue;
        RECT rect{client.left, dp(list, item.y) - list->scroll, client.right,
                  dp(list, item.y + item.height) - list->scroll};
        if (y < rect.top || y >= rect.bottom) continue;

        if (item.kind == ItemKind::Resolution) {
            const RowGeometry geo = geometryFor(list, rect);
            const RECT box = valueRect(list, geo);
            POINT point{x, y};
            if (PtInRect(&box, point)) openResolutionCombo(list, box);
            return;
        }

        const SettingRow& row = list->model->rows()[item.row];
        const RowGeometry geo = geometryFor(list, rect);
        POINT point{x, y};

        switch (row.def->kind) {
            case SettingKind::Toggle: {
                const RECT box = toggleRect(list, geo);
                if (PtInRect(&box, point)) applyValue(list, item.row, isOn(row.value) ? "0" : "1");
                return;
            }
            case SettingKind::Choice: {
                std::string onValue, offValue;
                if (twoChoiceToggle(row, &onValue, &offValue)) {
                    const RECT box = toggleRect(list, geo);
                    if (PtInRect(&box, point)) {
                        applyValue(list, item.row,
                                   row.value == onValue ? offValue : onValue);
                    }
                    return;
                }
                const std::vector<RECT> segments = segmentsFor(list, geo, row.choices.size());
                for (size_t i = 0; i < segments.size(); ++i) {
                    if (PtInRect(&segments[i], point)) {
                        applyValue(list, item.row, row.choices[i].value);
                        return;
                    }
                }
                return;
            }
            default: {
                const RECT box = valueRect(list, geo);
                if (PtInRect(&box, point)) beginEdit(list, item.row, box);
                return;
            }
        }
    }
}

LRESULT CALLBACK listProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    List* list = listOf(hwnd);
    switch (message) {
        case WM_NCCREATE: {
            List* created = new List();
            created->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            break;
        }
        case WM_PAINT:
            if (list) paint(list);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (list) {
                rebuildItems(list);
                updateScrollbar(list);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            if (!list) break;
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            scrollTo(list, list->scroll - delta * dp(list, 40) / WHEEL_DELTA);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (list) {
                SetFocus(hwnd);
                const int x = GET_X_LPARAM(lparam);
                const int y = GET_Y_LPARAM(lparam);
                RECT client{};
                GetClientRect(hwnd, &client);
                const int content = dp(list, list->contentHeight);
                const RECT thumb = ui::slimThumb(client, content, client.bottom, list->scroll,
                                                 list->dpi);
                // The right gutter belongs to the slim scrollbar whenever
                // there is one: on the thumb starts a drag, on the track
                // jumps the thumb to the pointer.
                if (thumb.right > thumb.left && x >= client.right - dp(list, 12)) {
                    if (y >= thumb.top && y < thumb.bottom) {
                        list->dragThumb = true;
                        list->dragOffset = y - thumb.top;
                    } else {
                        const int half = (thumb.bottom - thumb.top) / 2;
                        scrollTo(list, ui::slimPosFromThumbTop(client, content, client.bottom,
                                                               y - half, list->dpi));
                        list->dragThumb = true;
                        list->dragOffset = half;
                    }
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, nullptr, TRUE);
                    return 0;
                }
                onClick(list, x, y);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (list && list->dragThumb) {
                RECT client{};
                GetClientRect(hwnd, &client);
                const int content = dp(list, list->contentHeight);
                scrollTo(list, ui::slimPosFromThumbTop(client, content, client.bottom,
                                                       GET_Y_LPARAM(lparam) - list->dragOffset,
                                                       list->dpi));
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (list && list->dragThumb) {
                list->dragThumb = false;
                if (message == WM_LBUTTONUP) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_SETCURSOR:
            // The hand over a row's link. Colour alone did not say "click me":
            // the field read one as a label, and reasonably -- it sat under a
            // switch and named a value.
            if (list && LOWORD(lparam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(hwnd, &point);
                if (linkHit(list, point.x, point.y, nullptr)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        case WM_COMMAND:
            if (list && list->combo &&
                reinterpret_cast<HWND>(lparam) == list->combo) {
                if (HIWORD(wparam) == CBN_SELCHANGE) {
                    const LRESULT sel = SendMessageW(list->combo, CB_GETCURSEL, 0, 0);
                    const auto& presets = vscreenPresets();
                    if (sel >= 0 && static_cast<size_t>(sel) < presets.size()) {
                        list->resolutionCustom = false;
                        applyValue(list, list->resWidthRow, presets[sel].w);
                        applyValue(list, list->resHeightRow, presets[sel].h);
                    } else {
                        // Custom: nothing is written; the raw width and
                        // height rows appear for typing into.
                        list->resolutionCustom = true;
                    }
                    hideCombo(list);
                    rebuildItems(list);
                    updateScrollbar(list);
                    InvalidateRect(list->hwnd, nullptr, TRUE);
                } else if (HIWORD(wparam) == CBN_CLOSEUP) {
                    hideCombo(list);
                }
                return 0;
            }
            break;
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (list && item->CtlID == kComboId) {
                ui::drawComboItem(item, list->dpi);
                return TRUE;
            }
            break;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (list && measure->CtlID == kComboId) {
                measure->itemHeight = static_cast<UINT>(dp(list, 24));
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORLISTBOX: {
            // The dropdown's list shares the EDIT's colours: the rows are
            // owner-drawn, but the listbox paints its own background around
            // them and its frame, and stock white behind house rows is the
            // exact mismatch this control exists to remove.
            const ui::Theme& t = ui::theme();
            SetBkColor(reinterpret_cast<HDC>(wparam), t.cardBg);
            static HBRUSH listBrush = nullptr;
            static COLORREF listColour = 0;
            if (!listBrush || listColour != t.cardBg) {
                if (listBrush) DeleteObject(listBrush);
                listBrush = CreateSolidBrush(t.cardBg);
                listColour = t.cardBg;
            }
            return reinterpret_cast<LRESULT>(listBrush);
        }
        case WM_CTLCOLOREDIT: {
            const ui::Theme& t = ui::theme();
            SetTextColor(reinterpret_cast<HDC>(wparam), t.text);
            SetBkColor(reinterpret_cast<HDC>(wparam), t.control);
            static HBRUSH brush = nullptr;
            static COLORREF colour = 0;
            if (!brush || colour != t.control) {
                if (brush) DeleteObject(brush);
                brush = CreateSolidBrush(t.control);
                colour = t.control;
            }
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_DESTROY:
            if (list) {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                delete list;
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

HWND createSettingsList(HWND parent, int id, UINT dpi) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = listProc;
        cls.hInstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = kClassName;
        cls.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&cls);
        registered = true;
    }
    HWND hwnd = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_TABSTOP, 0, 0, 10,
                                10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                nullptr, nullptr);
    if (hwnd) {
        List* list = listOf(hwnd);
        if (list) list->dpi = dpi;
    }
    return hwnd;
}

void settingsListSetModel(HWND hwnd, SettingsModel* model, const std::wstring& mirrorDir) {
    List* list = listOf(hwnd);
    if (!list) return;
    list->model = model;
    list->mirrorDir = mirrorDir;
    list->scroll = 0;
    rebuildItems(list);
    updateScrollbar(list);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void settingsListSetFilter(HWND hwnd, const std::wstring& needle) {
    List* list = listOf(hwnd);
    if (!list) return;
    commitEdit(list);
    hideEdit(list);
    list->filter = needle;
    list->scroll = 0;
    rebuildItems(list);
    updateScrollbar(list);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void settingsListRescale(HWND hwnd, UINT dpi) {
    List* list = listOf(hwnd);
    if (!list) return;
    list->dpi = dpi;
    rebuildItems(list);
    updateScrollbar(list);
    InvalidateRect(hwnd, nullptr, TRUE);
}

const std::string& settingsListLastError(HWND hwnd) {
    static const std::string none;
    List* list = listOf(hwnd);
    return list ? list->error : none;
}

}  // namespace edvr::installer
