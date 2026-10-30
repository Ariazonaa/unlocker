#pragma once

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <vector>

// Selbst gezeichnete Controls im Fluent-Stil (dunkel). Alles auf Basis der Standard-Controls,
// damit Tastatur, Fokus und UI Automation weiter funktionieren.
namespace unlocker::ui::controls {

struct Fonts {
    HFONT text = nullptr;     // Segoe UI Variable Text, 10pt
    HFONT caption = nullptr;  // 9pt
    HFONT icon = nullptr;     // Segoe Fluent Icons
    UINT dpi = 96;
};

// Antialiasiertes abgerundetes Rechteck (GDI+). border == CLR_INVALID: kein Rand. fill == CLR_INVALID: nur Rand.
void fill_round_rect(HDC hdc, const RECT& rc, int radius, COLORREF fill, COLORREF border, float border_width = 1.0f);

// Push-Button mit Glyph (Segoe Fluent Icons, optional) und Akzentfarbe für die Primäraktion.
HWND create_button(HWND parent, HINSTANCE instance, int id, const wchar_t* text, const wchar_t* glyph, bool primary);
void draw_button(const DRAWITEMSTRUCT& dis, const Fonts& fonts);

// Breite aus Glyph + Text + Innenabstand. text == nullptr: aktueller Fenstertext.
int preferred_button_width(HWND button, const Fonts& fonts, const wchar_t* text = nullptr);

// Kompakt = nur Glyph (Fenstertext bleibt für UI Automation / Tooltips erhalten).
void set_button_compact(HWND button, bool compact);

// ListView: dunkles Theme, Spaltenkopf per Custom-Draw, Hover-Tracking, runde Ecken per Window-Region.
void style_listview(HWND list, const Fonts* fonts);
void round_corners(HWND hwnd, int radius);

// Zeile unter der Maus (-1 = keine); wird vom Subclass gepflegt, Zeilen werden bei Wechsel neu gezeichnet.
int listview_hot_item(HWND list);

// Sortierpfeil im Spaltenkopf (column = -1: keiner).
void set_header_sort(HWND list, int column, bool ascending);

// Zeilenhöhe über eine 1 px breite State-Imagelist erzwingen (Report-Ansicht hat keine direkte API dafür).
void set_listview_row_height(HWND list, int height);

// Spaltenrechtecke in Client-Koordinaten der Liste (aus dem Header, inkl. Scroll-Offset).
std::vector<RECT> listview_column_rects(HWND list);

}  // namespace unlocker::ui::controls
