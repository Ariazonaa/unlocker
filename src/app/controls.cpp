#include "controls.h"

#include "theme.h"

#include <objidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>

namespace unlocker::ui::controls {
namespace {

struct ButtonState {
    std::wstring glyph;
    bool primary = false;
    bool hover = false;
    bool compact = false;
};

int scale(UINT dpi, int value) { return MulDiv(value, static_cast<int>(dpi), 96); }

Gdiplus::Color to_color(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

void add_round_rect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    const float d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

LRESULT CALLBACK button_subclass(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref) {
    auto* state = reinterpret_cast<ButtonState*>(ref);
    switch (msg) {
        case WM_MOUSEMOVE:
            if (!state->hover) {
                state->hover = true;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            state->hover = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, button_subclass, 0);
            delete state;
            break;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

void draw_header_item(HWND header, const NMCUSTOMDRAW& cd, const Fonts& fonts) {
    const auto& p = theme::palette();
    const HDC hdc = cd.hdc;

    HBRUSH bg = CreateSolidBrush(p.header_bg);
    FillRect(hdc, &cd.rc, bg);
    DeleteObject(bg);

    HBRUSH sep = CreateSolidBrush(p.header_separator);
    RECT bottom{cd.rc.left, cd.rc.bottom - 1, cd.rc.right, cd.rc.bottom};
    FillRect(hdc, &bottom, sep);
    RECT side{cd.rc.right - 1, cd.rc.top + scale(fonts.dpi, 7), cd.rc.right, cd.rc.bottom - scale(fonts.dpi, 7)};
    FillRect(hdc, &side, sep);
    DeleteObject(sep);

    wchar_t text[256]{};
    HDITEMW item{};
    item.mask = HDI_TEXT | HDI_FORMAT;
    item.pszText = text;
    item.cchTextMax = 256;
    Header_GetItem(header, static_cast<int>(cd.dwItemSpec), &item);

    RECT tr = cd.rc;
    tr.left += scale(fonts.dpi, 10);
    tr.right -= scale(fonts.dpi, 10);
    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ old = SelectObject(hdc, fonts.caption);

    // Sortierpfeil rechts im Item
    const int sort_column = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(GetParent(header), L"UnlockerSortCol"))) - 1;
    const bool ascending = GetPropW(GetParent(header), L"UnlockerSortAsc") != nullptr;
    const bool sorted = sort_column == static_cast<int>(cd.dwItemSpec);
    if (sorted) {
        const wchar_t* glyph = ascending ? L"\uE70E" : L"\uE70D";
        SelectObject(hdc, fonts.icon);
        SIZE gs{};
        GetTextExtentPoint32W(hdc, glyph, 1, &gs);
        RECT gr{tr.right - gs.cx, cd.rc.top, tr.right, cd.rc.bottom};
        SetTextColor(hdc, p.text_tertiary);
        DrawTextW(hdc, glyph, 1, &gr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        tr.right = gr.left - scale(fonts.dpi, 4);
        SelectObject(hdc, fonts.caption);
    }
    SetTextColor(hdc, sorted ? p.text : p.text_secondary);
    DrawTextW(hdc, text, -1, &tr,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | ((item.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT));
    SelectObject(hdc, old);
}

constexpr wchar_t kHotProp[] = L"UnlockerHotItem";  // Index + 1, 0 = keiner

void set_hot_item(HWND hwnd, int item) {
    const int previous = listview_hot_item(hwnd);
    if (previous == item) return;
    SetPropW(hwnd, kHotProp, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(item + 1)));
    RECT rc{};
    if (previous >= 0 && ListView_GetItemRect(hwnd, previous, &rc, LVIR_BOUNDS)) InvalidateRect(hwnd, &rc, FALSE);
    if (item >= 0 && ListView_GetItemRect(hwnd, item, &rc, LVIR_BOUNDS)) InvalidateRect(hwnd, &rc, FALSE);
}

LRESULT CALLBACK listview_subclass(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref) {
    const auto* fonts = reinterpret_cast<const Fonts*>(ref);
    if (msg == WM_MOUSEMOVE) {
        LVHITTESTINFO hit{};
        hit.pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int item = ListView_HitTest(hwnd, &hit);
        if (item != listview_hot_item(hwnd)) {
            set_hot_item(hwnd, item);
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
        }
    }
    if (msg == WM_MOUSELEAVE) set_hot_item(hwnd, -1);
    if (msg == WM_NCDESTROY) RemovePropW(hwnd, kHotProp);
    if (msg == WM_NOTIFY && fonts) {
        auto* hdr = reinterpret_cast<NMHDR*>(lparam);
        if (hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom == ListView_GetHeader(hwnd)) {
            auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lparam);
            if (cd->dwDrawStage == CDDS_PREPAINT) {
                // Fläche rechts der letzten Spalte (Theme-Filler) gleich mit einfärben
                RECT rc{};
                GetClientRect(hdr->hwndFrom, &rc);
                HBRUSH bg = CreateSolidBrush(theme::palette().header_bg);
                FillRect(cd->hdc, &rc, bg);
                DeleteObject(bg);
                RECT line{rc.left, rc.bottom - 1, rc.right, rc.bottom};
                HBRUSH sep = CreateSolidBrush(theme::palette().header_separator);
                FillRect(cd->hdc, &line, sep);
                DeleteObject(sep);
                return CDRF_NOTIFYITEMDRAW;
            }
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                draw_header_item(hdr->hwndFrom, *cd, *fonts);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, listview_subclass, 0);
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

}  // namespace

void fill_round_rect(HDC hdc, const RECT& rc, int radius, COLORREF fill, COLORREF border, float border_width) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const bool has_border = border != CLR_INVALID;
    const float inset = has_border ? border_width / 2 : 0.0f;
    const Gdiplus::RectF r(static_cast<float>(rc.left) + inset, static_cast<float>(rc.top) + inset,
                           static_cast<float>(rc.right - rc.left) - 2 * inset,
                           static_cast<float>(rc.bottom - rc.top) - 2 * inset);
    Gdiplus::GraphicsPath path;
    add_round_rect(path, r, max(0.5f, static_cast<float>(radius) - inset));

    if (fill != CLR_INVALID) {
        Gdiplus::SolidBrush brush(to_color(fill));
        g.FillPath(&brush, &path);
    }
    if (has_border) {
        Gdiplus::Pen pen(to_color(border), border_width);
        g.DrawPath(&pen, &path);
    }
}

HWND create_button(HWND parent, HINSTANCE instance, int id, const wchar_t* text, const wchar_t* glyph, bool primary) {
    HWND hwnd = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
                                parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (!hwnd) return nullptr;
    auto* state = new ButtonState{glyph ? glyph : L"", primary, false};
    SetWindowSubclass(hwnd, button_subclass, 0, reinterpret_cast<DWORD_PTR>(state));
    return hwnd;
}

void draw_button(const DRAWITEMSTRUCT& dis, const Fonts& fonts) {
    const auto& p = theme::palette();
    DWORD_PTR ref = 0;
    GetWindowSubclass(dis.hwndItem, button_subclass, 0, &ref);
    const ButtonState* state = reinterpret_cast<const ButtonState*>(ref);
    const bool primary = state && state->primary;
    const bool hover = state && state->hover;
    const bool disabled = (dis.itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis.itemState & ODS_SELECTED) != 0;

    const RECT rc = dis.rcItem;
    HDC mem = nullptr;
    HPAINTBUFFER buffer = BeginBufferedPaint(dis.hDC, &rc, BPBF_COMPATIBLEBITMAP, nullptr, &mem);
    const HDC hdc = buffer ? mem : dis.hDC;

    HBRUSH bg = CreateSolidBrush(p.window_bg);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    COLORREF fill, border, text;
    if (primary) {
        fill = disabled ? theme::blend(p.accent, p.window_bg, 65)
               : pressed ? p.accent_pressed
               : hover   ? p.accent_hover
                         : p.accent;
        border = fill;
        text = disabled ? theme::blend(p.accent_text, fill, 45) : p.accent_text;
    } else {
        fill = disabled ? p.button_bg : pressed ? p.button_bg_pressed : hover ? p.button_bg_hover : p.button_bg;
        border = p.button_border;
        text = disabled ? p.text_disabled : pressed ? p.text_secondary : p.text;
    }
    fill_round_rect(hdc, rc, scale(fonts.dpi, 4), fill, border);

    wchar_t label[128]{};
    if (!state || !state->compact) GetWindowTextW(dis.hwndItem, label, 128);
    const std::wstring glyph = state ? state->glyph : L"";

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, text);

    SIZE text_size{};
    HGDIOBJ old = SelectObject(hdc, fonts.text);
    GetTextExtentPoint32W(hdc, label, static_cast<int>(wcslen(label)), &text_size);
    SIZE glyph_size{};
    if (!glyph.empty()) {
        SelectObject(hdc, fonts.icon);
        GetTextExtentPoint32W(hdc, glyph.c_str(), static_cast<int>(glyph.size()), &glyph_size);
    }
    const int gap = glyph.empty() ? 0 : scale(fonts.dpi, 8);
    const int total = glyph_size.cx + gap + text_size.cx;
    int x = rc.left + ((rc.right - rc.left) - total) / 2;

    if (!glyph.empty()) {
        RECT gr{x, rc.top, x + glyph_size.cx, rc.bottom};
        DrawTextW(hdc, glyph.c_str(), -1, &gr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        x += glyph_size.cx + gap;
    }
    SelectObject(hdc, fonts.text);
    RECT tr{x, rc.top, x + text_size.cx + 2, rc.bottom};
    DrawTextW(hdc, label, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(hdc, old);

    // Fokusring im Win11-Stil: 2 px heller Ring mit 1 px Abstand zur Füllung (nur Rand, Inhalt bleibt)
    if ((dis.itemState & ODS_FOCUS) && !(dis.itemState & ODS_NOFOCUSRECT)) {
        RECT fr = rc;
        InflateRect(&fr, -1, -1);
        fill_round_rect(hdc, fr, scale(fonts.dpi, 4), CLR_INVALID, p.text, 2.0f);
    }

    if (buffer) EndBufferedPaint(buffer, TRUE);
}

int preferred_button_width(HWND button, const Fonts& fonts, const wchar_t* text) {
    DWORD_PTR ref = 0;
    GetWindowSubclass(button, button_subclass, 0, &ref);
    const ButtonState* state = reinterpret_cast<const ButtonState*>(ref);

    wchar_t label[128]{};
    if (text) wcsncpy_s(label, text, _TRUNCATE);
    else GetWindowTextW(button, label, 128);

    HDC dc = GetDC(button);
    HGDIOBJ old = SelectObject(dc, fonts.text);
    SIZE text_size{};
    GetTextExtentPoint32W(dc, label, static_cast<int>(wcslen(label)), &text_size);
    SIZE glyph_size{};
    if (state && !state->glyph.empty()) {
        SelectObject(dc, fonts.icon);
        GetTextExtentPoint32W(dc, state->glyph.c_str(), static_cast<int>(state->glyph.size()), &glyph_size);
    }
    SelectObject(dc, old);
    ReleaseDC(button, dc);

    const int gap = glyph_size.cx ? scale(fonts.dpi, 8) : 0;
    return glyph_size.cx + gap + text_size.cx + 2 * scale(fonts.dpi, 18);
}

void set_button_compact(HWND button, bool compact) {
    DWORD_PTR ref = 0;
    GetWindowSubclass(button, button_subclass, 0, &ref);
    auto* state = reinterpret_cast<ButtonState*>(ref);
    if (state && state->compact != compact) {
        state->compact = compact;
        InvalidateRect(button, nullptr, FALSE);
    }
}

void style_listview(HWND list, const Fonts* fonts) {
    const auto& p = theme::palette();
    theme::apply_control(list, L"DarkMode_Explorer");
    if (HWND header = ListView_GetHeader(list)) theme::apply_control(header, L"DarkMode_ItemsView");
    ListView_SetBkColor(list, p.card_bg);
    ListView_SetTextBkColor(list, p.card_bg);
    ListView_SetTextColor(list, p.text);
    SetWindowSubclass(list, listview_subclass, 0, reinterpret_cast<DWORD_PTR>(fonts));
}

int listview_hot_item(HWND list) {
    return static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(list, kHotProp))) - 1;
}

void set_header_sort(HWND list, int column, bool ascending) {
    SetPropW(list, L"UnlockerSortCol", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(column + 1)));
    SetPropW(list, L"UnlockerSortAsc", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(ascending ? 1 : 0)));
    if (HWND header = ListView_GetHeader(list)) InvalidateRect(header, nullptr, FALSE);
}

void set_listview_row_height(HWND list, int height) {
    if (HIMAGELIST old = ListView_GetImageList(list, LVSIL_STATE)) {
        ListView_SetImageList(list, nullptr, LVSIL_STATE);
        ImageList_Destroy(old);
    }
    HIMAGELIST state = ImageList_Create(1, height, ILC_COLOR32, 1, 1);
    ListView_SetImageList(list, state, LVSIL_STATE);
}

std::vector<RECT> listview_column_rects(HWND list) {
    std::vector<RECT> rects;
    HWND header = ListView_GetHeader(list);
    if (!header) return rects;
    RECT hr{};
    GetWindowRect(header, &hr);
    MapWindowPoints(nullptr, list, reinterpret_cast<POINT*>(&hr), 2);
    const int count = Header_GetItemCount(header);
    for (int i = 0; i < count; ++i) {
        RECT item{};
        Header_GetItemRect(header, i, &item);
        rects.push_back(RECT{hr.left + item.left, 0, hr.left + item.right, 0});
    }
    return rects;
}

void round_corners(HWND hwnd, int radius) {
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, radius * 2, radius * 2);
    SetWindowRgn(hwnd, rgn, TRUE);  // Region gehört danach dem Fenster
}

}  // namespace unlocker::ui::controls
