#include "settings_view.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <vector>

#include "detect.h"
#include "ui.h"

#pragma comment(lib, "comctl32.lib")

namespace edvr::installer {
namespace {

const wchar_t* kClassName = L"EdvrSettingsList";
const int kEditId = 9001;

// Everything below is in 96-dpi units.
const int kPad = 18;
const int kHeaderHeight = 38;
const int kRowHeight = 68;
const int kControlWidth = 240;

enum class ItemKind { Header, Setting };

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
    SettingsModel* model = nullptr;
    UINT           dpi = 96;
    int            scroll = 0;      // pixels
    int            contentHeight = 0;
    std::vector<Item> items;
    std::wstring   filter;
    size_t         editingRow = SIZE_MAX;
    std::string    error;
};

List* listOf(HWND hwnd) {
    return reinterpret_cast<List*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int dp(const List* list, int v) { return ui::dp(v, list->dpi); }

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
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

void updateScrollbar(List* list) {
    RECT client{};
    GetClientRect(list->hwnd, &client);
    const int page = client.bottom - client.top;
    const int content = dp(list, list->contentHeight);

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = content > 0 ? content - 1 : 0;
    info.nPage = static_cast<UINT>(page);
    list->scroll = std::max(0, std::min(list->scroll, std::max(0, content - page)));
    info.nPos = list->scroll;
    SetScrollInfo(list->hwnd, SB_VERT, &info, TRUE);
}

void commitEdit(List* list);

void hideEdit(List* list) {
    if (list->edit) ShowWindow(list->edit, SW_HIDE);
    list->editingRow = SIZE_MAX;
}

void scrollTo(List* list, int position) {
    commitEdit(list);
    hideEdit(list);
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

void paintRow(const List* list, HDC dc, const RECT& rowRect, const SettingRow& row) {
    const ui::Theme& t = ui::theme();
    const ui::Fonts& f = ui::fonts();
    const RowGeometry geo = geometryFor(list, rowRect);

    RECT labelRect = geo.label;
    ui::drawText(dc, fromUtf8(row.def->label), labelRect, f.bodyBold, t.text,
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Only the ones that need a restart are marked. Every other row is live,
    // which the line above the list says once -- twenty rows each carrying a
    // "live" badge would say it twenty times and mean less each time.
    if (row.def->needsRestart) {
        const std::wstring mark = L"restart the game";
        const int width = ui::textWidth(dc, mark, f.caption) + dp(list, 16);
        const int labelWidth = ui::textWidth(dc, fromUtf8(row.def->label), f.bodyBold);
        RECT badge{labelRect.left + labelWidth + dp(list, 10), labelRect.top + dp(list, 2),
                   labelRect.left + labelWidth + dp(list, 10) + width,
                   labelRect.bottom + dp(list, 1)};
        if (badge.right < labelRect.right) {
            ui::fillRounded(dc, badge, (badge.bottom - badge.top) / 2,
                            mix(t.cardBg, t.warn, 18));
            ui::drawText(dc, mark, badge, f.caption, t.warn,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
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

    // The recommended value, always visible: a quiet note when the current
    // value already is it, and a button when it is not. "Flagged where
    // possible" was the ask, and a number somebody can act on beats a number
    // they have to compare by eye.
    // Under the control: what the setting ships as, and what it will accept.
    // A number box with no bounds beside it is a guess, and a value somebody
    // has moved should say what it was. When the two differ this line is the
    // way back -- click it and the recommended value goes in.
    const std::wstring helper = row.helper();
    if (!helper.empty()) {
        RECT line = geo.recommended;
        if (row.isRecommended) {
            ui::drawText(dc, helper, line, f.caption, t.muted,
                         DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            const std::wstring label =
                L"use " + row.shownRecommended() + L"   \x00b7   " + helper;
            ui::drawText(dc, label, line, f.caption, t.accent,
                         DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    } else if (!row.isRecommended) {
        // A toggle away from its default still deserves the way back.
        RECT line = geo.recommended;
        ui::drawText(dc, L"use " + row.shownRecommended(), line, f.caption, t.accent,
                     DT_RIGHT | DT_SINGLELINE);
    }
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
                                 : L"Pick an install on the Install tab first.",
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
            paintRow(list, dc, rect, list->model->rows()[item.row]);
            RECT line{rect.left + dp(list, kPad), rect.bottom - 1, rect.right - dp(list, kPad),
                      rect.bottom};
            ui::fillRect(dc, line, t.cardBorder);
        }
    }

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
    MoveWindow(list->edit, box.left + dp(list, 6), box.top + dp(list, 4),
               (box.right - box.left) - dp(list, 12), (box.bottom - box.top) - dp(list, 8), TRUE);
    ShowWindow(list->edit, SW_SHOW);
    SetFocus(list->edit);
    SendMessageW(list->edit, EM_SETSEL, 0, -1);
    list->editingRow = rowIndex;
}

void onClick(List* list, int x, int y) {
    commitEdit(list);
    hideEdit(list);
    if (!list->model) return;

    RECT client{};
    GetClientRect(list->hwnd, &client);

    for (const Item& item : list->items) {
        if (item.kind != ItemKind::Setting) continue;
        RECT rect{client.left, dp(list, item.y) - list->scroll, client.right,
                  dp(list, item.y + item.height) - list->scroll};
        if (y < rect.top || y >= rect.bottom) continue;

        const SettingRow& row = list->model->rows()[item.row];
        const RowGeometry geo = geometryFor(list, rect);
        POINT point{x, y};

        if (!row.isRecommended && PtInRect(&geo.recommended, point)) {
            applyValue(list, item.row, row.def->recommended);
            return;
        }
        switch (row.def->kind) {
            case SettingKind::Toggle: {
                const RECT box = toggleRect(list, geo);
                if (PtInRect(&box, point)) applyValue(list, item.row, isOn(row.value) ? "0" : "1");
                return;
            }
            case SettingKind::Choice: {
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
        case WM_VSCROLL: {
            if (!list) break;
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &info);
            int position = list->scroll;
            switch (LOWORD(wparam)) {
                case SB_LINEUP: position -= dp(list, 24); break;
                case SB_LINEDOWN: position += dp(list, 24); break;
                case SB_PAGEUP: position -= info.nPage; break;
                case SB_PAGEDOWN: position += info.nPage; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: position = info.nTrackPos; break;
                default: break;
            }
            scrollTo(list, position);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (!list) break;
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            scrollTo(list, list->scroll - delta * dp(list, 40) / WHEEL_DELTA);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (list) {
                SetFocus(hwnd);
                onClick(list, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            }
            return 0;
        case WM_SETCURSOR:
            return DefWindowProcW(hwnd, message, wparam, lparam);
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
    HWND hwnd = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VSCROLL | WS_TABSTOP, 0, 0, 10,
                                10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                nullptr, nullptr);
    if (hwnd) {
        List* list = listOf(hwnd);
        if (list) list->dpi = dpi;
    }
    return hwnd;
}

void settingsListSetModel(HWND hwnd, SettingsModel* model) {
    List* list = listOf(hwnd);
    if (!list) return;
    list->model = model;
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
