#include "theme.h"

#include <dwmapi.h>
#include <uxtheme.h>

namespace unlocker::ui::theme {
namespace {

// dwmapi.h ab SDK 22621 – hier explizit, damit auch ältere SDKs bauen.
constexpr DWORD kAttrUseImmersiveDarkMode = 20;
constexpr DWORD kAttrSystemBackdropType = 38;
constexpr int kBackdropMainWindow = 2;

// Nicht dokumentierte uxtheme-Exporte (Ordinale), seit 1809/1903 stabil und von vielen
// Dark-Mode-Apps genutzt. Alles optional: fehlt ein Export, bleibt nur der Effekt aus.
enum class PreferredAppMode : int { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
using FlushMenuThemesFn = void(WINAPI*)();
using RefreshImmersiveColorPolicyStateFn = void(WINAPI*)();

struct UxTheme {
    SetPreferredAppModeFn SetPreferredAppMode = nullptr;
    AllowDarkModeForWindowFn AllowDarkModeForWindow = nullptr;
    FlushMenuThemesFn FlushMenuThemes = nullptr;
    RefreshImmersiveColorPolicyStateFn RefreshImmersiveColorPolicyState = nullptr;
};

DWORD build_number() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    static const DWORD build = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        RTL_OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (!fn || fn(&info) != 0) return 0ul;
        return info.dwBuildNumber;
    }();
    return build;
}

const UxTheme& uxtheme() {
    static const UxTheme instance = [] {
        UxTheme u;
        if (build_number() < 18362) return u;
        HMODULE lib = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!lib) return u;
        u.RefreshImmersiveColorPolicyState =
            reinterpret_cast<RefreshImmersiveColorPolicyStateFn>(GetProcAddress(lib, MAKEINTRESOURCEA(104)));
        u.AllowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(lib, MAKEINTRESOURCEA(133)));
        u.SetPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(lib, MAKEINTRESOURCEA(135)));
        u.FlushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(lib, MAKEINTRESOURCEA(136)));
        return u;
    }();
    return instance;
}

int luminance(COLORREF c) noexcept {
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

Palette build_palette() {
    Palette p;
    DWORD colorization = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&colorization, &opaque))) {
        const COLORREF accent = RGB((colorization >> 16) & 0xFF, (colorization >> 8) & 0xFF, colorization & 0xFF);
        // Dunkle Oberflächen brauchen einen helleren Akzent (entspricht "Accent Light 2" in WinUI).
        p.accent = blend(accent, RGB(0xFF, 0xFF, 0xFF), 35);
        p.accent_hover = blend(p.accent, p.window_bg, 10);
        p.accent_pressed = blend(p.accent, p.window_bg, 20);
    }
    p.accent_text = luminance(p.accent) > 140 ? RGB(0x00, 0x00, 0x00) : RGB(0xFF, 0xFF, 0xFF);
    p.row_hover = blend(p.card_bg, p.text, 4);
    p.row_selected = blend(p.card_bg, p.text, 8);
    return p;
}

Palette& mutable_palette() {
    static Palette instance = build_palette();
    return instance;
}

}  // namespace

const Palette& palette() { return mutable_palette(); }

void refresh_palette() { mutable_palette() = build_palette(); }

void init() {
    const auto& u = uxtheme();
    if (u.SetPreferredAppMode) u.SetPreferredAppMode(PreferredAppMode::ForceDark);
    if (u.RefreshImmersiveColorPolicyState) u.RefreshImmersiveColorPolicyState();
    if (u.FlushMenuThemes) u.FlushMenuThemes();
}

bool mica_supported() { return build_number() >= 22621; }

void apply_window(HWND hwnd, int header_height) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, kAttrUseImmersiveDarkMode, &dark, sizeof(dark));
    if (const auto& u = uxtheme(); u.AllowDarkModeForWindow) u.AllowDarkModeForWindow(hwnd, true);

    if (mica_supported()) {
        const int backdrop = kBackdropMainWindow;
        DwmSetWindowAttribute(hwnd, kAttrSystemBackdropType, &backdrop, sizeof(backdrop));
        const MARGINS margins{0, 0, header_height, 0};
        DwmExtendFrameIntoClientArea(hwnd, &margins);
    }
}

void apply_control(HWND hwnd, const wchar_t* theme_class) {
    if (const auto& u = uxtheme(); u.AllowDarkModeForWindow) u.AllowDarkModeForWindow(hwnd, true);
    SetWindowTheme(hwnd, theme_class, nullptr);
}

COLORREF blend(COLORREF a, COLORREF b, int percent_b) noexcept {
    const auto mix = [percent_b](int x, int y) { return (x * (100 - percent_b) + y * percent_b) / 100; };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

}  // namespace unlocker::ui::theme
