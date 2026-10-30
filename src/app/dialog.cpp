#include "dialog.h"

#include "strings.h"
#include "theme.h"

#include <algorithm>

namespace unlocker::ui::dialog {
namespace {

constexpr wchar_t kClassName[] = L"UnlockerDialog";
constexpr int kWidth = 460;
constexpr int kMargin = 24;
constexpr int kIconSize = 32;
constexpr int kButtonWidth = 120;
constexpr int kButtonHeight = 34;
constexpr int kInputHeight = 32;
constexpr int IDC_INPUT = 100;

constexpr wchar_t kGlyphInfo[] = L"\uE946";
constexpr wchar_t kGlyphWarning[] = L"\uE7BA";
constexpr wchar_t kGlyphError[] = L"\uEA39";

struct State {
    Options* options = nullptr;
    const controls::Fonts* fonts = nullptr;
    HFONT font_instruction = nullptr;
    HFONT font_icon = nullptr;
    HBRUSH edit_brush = nullptr;
    HWND edit = nullptr;
    UINT dpi = 96;
    int result = 0;  // 0 = offen, IDOK, IDCANCEL
    RECT text_rect{};
    RECT input_rect{};
    int instruction_height = 0;  // mindestens Icon-Höhe; lange Überschriften brechen um statt "…"
    bool instruction_wraps = false;
    int dp(int v) const { return MulDiv(v, static_cast<int>(dpi), 96); }
};

const wchar_t* glyph_for(Icon icon) {
    switch (icon) {
        case Icon::Warning: return kGlyphWarning;
        case Icon::Error: return kGlyphError;
        default: return kGlyphInfo;
    }
}

COLORREF color_for(Icon icon) {
    const auto& p = theme::palette();
    switch (icon) {
        case Icon::Warning: return p.warning;
        case Icon::Error: return p.danger;
        default: return p.accent;
    }
}

void paint(HWND hwnd, State& s) {
    const auto& p = theme::palette();
    PAINTSTRUCT ps{};
    const HDC hdc = BeginPaint(hwnd, &ps);
    SetBkMode(hdc, TRANSPARENT);

    RECT icon{s.dp(kMargin), s.dp(kMargin), s.dp(kMargin) + s.dp(kIconSize), s.dp(kMargin) + s.dp(kIconSize)};
    SetTextColor(hdc, color_for(s.options->icon));
    HGDIOBJ old = SelectObject(hdc, s.font_icon);
    DrawTextW(hdc, glyph_for(s.options->icon), -1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT instruction = s.text_rect;
    instruction.bottom = instruction.top + s.instruction_height;
    SetTextColor(hdc, p.text);
    SelectObject(hdc, s.font_instruction);
    DrawTextW(hdc, s.options->instruction.c_str(), -1, &instruction,
              s.instruction_wraps ? DT_LEFT | DT_WORDBREAK | DT_NOPREFIX
                                  : DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT content = s.text_rect;
    content.top = instruction.bottom + s.dp(10);
    SetTextColor(hdc, p.text_secondary);
    SelectObject(hdc, s.fonts->text);
    DrawTextW(hdc, s.options->content.c_str(), -1, &content, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

    if (s.edit) {
        RECT frame = s.input_rect;
        InflateRect(&frame, 1, 1);
        controls::fill_round_rect(hdc, frame, s.dp(4), p.card_bg, p.button_border);
    }

    SelectObject(hdc, old);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
            break;
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(theme::palette().window_bg);
            FillRect(reinterpret_cast<HDC>(wparam), &rc, bg);
            DeleteObject(bg);
            return 1;
        }
        case WM_PAINT:
            if (s) paint(hwnd, *s);
            return 0;
        case WM_CTLCOLOREDIT:
            if (s) {
                const HDC hdc = reinterpret_cast<HDC>(wparam);
                SetTextColor(hdc, theme::palette().text);
                SetBkColor(hdc, theme::palette().card_bg);
                return reinterpret_cast<LRESULT>(s->edit_brush);
            }
            break;
        case WM_DRAWITEM: {
            const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (dis->CtlType == ODT_BUTTON && s) {
                controls::draw_button(*dis, *s->fonts);
                return TRUE;
            }
            break;
        }
        case DM_GETDEFID:
            if (s) {
                const bool cancel_default = s->options->default_cancel && !s->options->cancel_text.empty();
                return MAKELONG(cancel_default ? IDCANCEL : IDOK, DC_HASDEFID);
            }
            break;
        case WM_COMMAND:
            if (s && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
                if (LOWORD(wparam) == IDOK && s->edit) {
                    const int len = GetWindowTextLengthW(s->edit);
                    std::wstring text(static_cast<std::size_t>(len) + 1, L'\0');
                    GetWindowTextW(s->edit, text.data(), len + 1);
                    text.resize(static_cast<std::size_t>(len));
                    s->options->input = std::move(text);
                }
                s->result = LOWORD(wparam);
            }
            return 0;
        case WM_CLOSE:
            if (s) s->result = IDCANCEL;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void register_class(HINSTANCE instance) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace

bool show(HWND owner, Options& options, const controls::Fonts& fonts) {
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    register_class(instance);
    const auto& p = theme::palette();

    State s;
    s.options = &options;
    s.fonts = &fonts;
    s.dpi = GetDpiForWindow(owner);
    s.font_instruction = CreateFontW(-MulDiv(14, static_cast<int>(s.dpi), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");
    s.font_icon = CreateFontW(-MulDiv(22, static_cast<int>(s.dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe Fluent Icons");
    s.edit_brush = CreateSolidBrush(p.card_bg);

    // Textmaß für die Höhe
    const int width = s.dp(kWidth);
    const int text_left = s.dp(kMargin) + s.dp(kIconSize) + s.dp(16);
    s.text_rect = RECT{text_left, s.dp(kMargin), width - s.dp(kMargin), 0};
    {
        HDC dc = GetDC(owner);
        HGDIOBJ old = SelectObject(dc, s.font_instruction);
        RECT heading{0, 0, s.text_rect.right - s.text_rect.left, 0};
        DrawTextW(dc, options.instruction.c_str(), -1, &heading, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        s.instruction_wraps = heading.bottom > s.dp(kIconSize);
        s.instruction_height = std::max<int>(s.dp(kIconSize), heading.bottom);

        SelectObject(dc, fonts.text);
        RECT measure{0, 0, s.text_rect.right - s.text_rect.left, 0};
        if (!options.content.empty()) {
            DrawTextW(dc, options.content.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        }
        SelectObject(dc, old);
        ReleaseDC(owner, dc);
        s.text_rect.bottom =
            s.text_rect.top + s.instruction_height + (options.content.empty() ? 0 : s.dp(10) + measure.bottom);
    }
    int next_top = s.text_rect.bottom;
    if (options.has_input) {
        s.input_rect = RECT{text_left, next_top + s.dp(14), width - s.dp(kMargin), next_top + s.dp(14) + s.dp(kInputHeight)};
        next_top = s.input_rect.bottom;
    }
    const int buttons_top = next_top + s.dp(24);
    const int client_height = buttons_top + s.dp(kButtonHeight) + s.dp(kMargin);

    RECT frame{0, 0, width, client_height};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, s.dpi);

    RECT owner_rc{};
    GetWindowRect(owner, &owner_rc);
    const int x = owner_rc.left + ((owner_rc.right - owner_rc.left) - (frame.right - frame.left)) / 2;
    const int y = owner_rc.top + ((owner_rc.bottom - owner_rc.top) - (frame.bottom - frame.top)) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kClassName, i18n::tr(i18n::S::AppName), style, x, y,
                                frame.right - frame.left, frame.bottom - frame.top, owner, nullptr, instance, &s);
    if (!hwnd) {
        DeleteObject(s.font_instruction);
        DeleteObject(s.font_icon);
        DeleteObject(s.edit_brush);
        return false;
    }
    theme::apply_window(hwnd, 0);

    if (options.has_input) {
        const int pad = s.dp(8);
        s.edit = CreateWindowExW(0, L"EDIT", options.input.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                 s.input_rect.left + pad, s.input_rect.top + s.dp(7), s.input_rect.right - s.input_rect.left - 2 * pad,
                                 s.input_rect.bottom - s.input_rect.top - s.dp(14), hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INPUT)), instance, nullptr);
        SendMessageW(s.edit, WM_SETFONT, reinterpret_cast<WPARAM>(fonts.text), TRUE);
        theme::apply_control(s.edit, L"DarkMode_CFD");
        int sel_start = 0;
        int sel_end = -1;
        if (!options.input_select.empty()) {
            if (const auto pos = options.input.find(options.input_select); pos != std::wstring::npos) {
                sel_start = static_cast<int>(pos);
                sel_end = static_cast<int>(pos + options.input_select.size());
            }
        }
        SendMessageW(s.edit, EM_SETSEL, static_cast<WPARAM>(sel_start), static_cast<LPARAM>(sel_end));
    }

    const bool has_cancel = !options.cancel_text.empty();
    int bx = width - s.dp(kMargin) - s.dp(kButtonWidth);
    HWND ok = controls::create_button(hwnd, instance, IDOK, options.ok_text.c_str(), nullptr, true);
    SetWindowPos(ok, nullptr, bx, buttons_top, s.dp(kButtonWidth), s.dp(kButtonHeight), SWP_NOZORDER);
    HWND cancel = nullptr;
    if (has_cancel) {
        bx -= s.dp(8) + s.dp(kButtonWidth);
        cancel = controls::create_button(hwnd, instance, IDCANCEL, options.cancel_text.c_str(), nullptr, false);
        SetWindowPos(cancel, nullptr, bx, buttons_top, s.dp(kButtonWidth), s.dp(kButtonHeight), SWP_NOZORDER);
    }

    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    if (s.edit) SetFocus(s.edit);
    else SetFocus(has_cancel && options.default_cancel ? cancel : ok);

    MSG msg{};
    while (s.result == 0) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));
            s.result = IDCANCEL;
            break;
        }
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    DestroyWindow(hwnd);
    DeleteObject(s.font_instruction);
    DeleteObject(s.font_icon);
    DeleteObject(s.edit_brush);
    return s.result == IDOK;
}

}  // namespace unlocker::ui::dialog
