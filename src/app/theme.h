#pragma once

#include <windows.h>

// Dunkles Fluent-Design, bewusst ohne Light-Mode.
namespace unlocker::ui::theme {

struct Palette {
    COLORREF window_bg = RGB(0x20, 0x20, 0x20);        // solider Hintergrund (entspricht Mica-Dark-Fallback)
    COLORREF card_bg = RGB(0x2B, 0x2B, 0x2B);          // Listen-Karte
    COLORREF card_border = RGB(0x3D, 0x3D, 0x3D);
    COLORREF header_bg = RGB(0x2B, 0x2B, 0x2B);        // Spaltenkopf
    COLORREF header_separator = RGB(0x3D, 0x3D, 0x3D);
    COLORREF text = RGB(0xFF, 0xFF, 0xFF);
    COLORREF text_secondary = RGB(0xC9, 0xC9, 0xC9);
    COLORREF text_tertiary = RGB(0x8F, 0x8F, 0x8F);
    COLORREF text_disabled = RGB(0x6B, 0x6B, 0x6B);
    COLORREF danger = RGB(0xFF, 0x99, 0xA4);
    COLORREF warning = RGB(0xFC, 0xE1, 0x00);
    COLORREF success = RGB(0x6C, 0xCB, 0x5F);
    COLORREF button_bg = RGB(0x2D, 0x2D, 0x2D);
    COLORREF button_bg_hover = RGB(0x35, 0x35, 0x35);
    COLORREF button_bg_pressed = RGB(0x27, 0x27, 0x27);
    COLORREF button_border = RGB(0x3A, 0x3A, 0x3A);
    COLORREF accent = RGB(0x4C, 0xC2, 0xFF);           // wird zur Laufzeit durch Systemakzent ersetzt
    COLORREF accent_hover = RGB(0x47, 0xB1, 0xE8);
    COLORREF accent_pressed = RGB(0x42, 0xA1, 0xD2);
    COLORREF accent_text = RGB(0x00, 0x00, 0x00);
    COLORREF row_hover = RGB(0x33, 0x33, 0x33);
    COLORREF row_selected = RGB(0x3A, 0x3A, 0x3A);
};

const Palette& palette();

// Nach WM_DWMCOLORIZATIONCOLORCHANGED: Akzentfarbe neu aus dem System übernehmen.
void refresh_palette();

// Einmalig vor dem ersten Fenster: dunkle Menüs, Scrollbars, Tooltips (uxtheme, ab Win10 1903).
void init();

// Windows 11 22H2+: DWMWA_SYSTEMBACKDROP_TYPE verfügbar.
bool mica_supported();

// Dunkle Titelleiste + Mica. Der Frame wird nur um header_height Client-Pixel nach unten erweitert:
// dort zeichnen wir selbst mit Alpha, darunter liegen Standard-Controls auf solidem Hintergrund
// (GDI-Text wäre auf erweitertem Frame transparent).
void apply_window(HWND hwnd, int header_height);

// Dunkles Explorer-Theme für ein Common Control (ListView, Scrollbars, Header).
void apply_control(HWND hwnd, const wchar_t* theme_class = L"DarkMode_Explorer");

COLORREF blend(COLORREF a, COLORREF b, int percent_b) noexcept;

}  // namespace unlocker::ui::theme
