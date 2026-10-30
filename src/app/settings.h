#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

// Persistente Einstellungen: settings.json neben der EXE, wenn dort eine portable.ini liegt,
// sonst in %LOCALAPPDATA%\Unlocker. Bewusst minimales JSON (Ganzzahlen, Bool, String-Liste).
namespace unlocker::ui::settings {

struct Settings {
    RECT window{};                 // normale Fensterposition (alle 0 = unbekannt)
    bool maximized = false;
    int columns[3] = {0, 0, 0};    // Prozess, PID, Art – in 96-dpi-Einheiten; 0 = Standard
    int sort_column = 1;           // PID
    bool sort_ascending = true;
    std::vector<std::wstring> collapsed_processes;  // Prozessnamen, deren Gruppen eingeklappt starten
    std::vector<int> hidden_columns;                // Spaltenindizes (1 = PID, 2 = Art, 3 = Pfad)
    std::vector<int> column_order;                  // Permutation 0..3 (Spaltenindizes, wie LVM_SETCOLUMNORDERARRAY), leer = Standard
};

std::filesystem::path file();
Settings load();
void save(const Settings& s);

}  // namespace unlocker::ui::settings
