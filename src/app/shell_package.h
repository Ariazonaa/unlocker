#pragma once

#include <filesystem>
#include <string>

// Windows-11-Kontextmenü (Sparse Package "Unlocker.App") aus der App ein- und ausschalten – dasselbe wie
// packaging\register-w11-menu.ps1 bzw. dessen -Remove, ohne PowerShell. Bauen und signieren kann die App das Paket
// nicht (Windows SDK, Zertifikat): das erledigt das Skript einmalig und legt es als unlocker-sparse.msix neben
// settings.json ab. Alles hier ist WinRT (PackageManager); keine Exceptions verlassen das Modul.
namespace unlocker::ui::shell_package {

// Windows 11 (Build 22000+): erst dort gibt es das neue Kontextmenü.
bool supported();

// PackageFullName des registrierten Pakets für den aktuellen Benutzer; leer = nicht registriert.
std::wstring registered_full_name();

// unlocker-sparse.msix (neben settings.json, neben der EXE oder in kontextmenu\ daneben); leer = nicht vorhanden.
std::filesystem::path package_file();

struct Outcome {
    long hr = 0;       // HRESULT, 0 = Erfolg
    std::wstring text; // Fehlertext von Windows (DeploymentResult.ErrorText bzw. HRESULT-Meldung)
};

// Blockierend – nur auf einem Hintergrund-Thread aufrufen (die Bereitstellung dauert Sekunden).
// external_location: Ordner mit unlocker.exe und unlocker_shell.dll.
Outcome enable(const std::filesystem::path& msix, const std::filesystem::path& external_location);
Outcome disable(const std::wstring& full_name);

}  // namespace unlocker::ui::shell_package
