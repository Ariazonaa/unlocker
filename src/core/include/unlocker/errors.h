#pragma once

#include <system_error>

// Eigene Fehlercodes für Fälle, die keinen Win32-Fehler haben. Die UI übersetzt sie, der Kern bleibt sprachneutral.
namespace unlocker {

enum class Error {
    critical_process = 1,   // Systemprozess – wird nicht angefasst
    protected_handle,       // Handle hat OBJ_PROTECT_CLOSE, Schließen ist nicht möglich
    protected_process,      // PPL/Protected Process – auch als Administrator nicht zugreifbar
    not_supported,          // Sperrart lässt sich nicht lösen (z. B. unbekanntes Handle)
    system_held,            // vom Kernel/einem Treiber gehalten – kein Prozess, nur beim Neustart löschbar
    still_locked,           // Freigabe-Zugriff auf dem Server geschlossen, die Datei ist aber weiter gesperrt
    no_process,             // gesperrt, aber kein Prozess gefunden – nur beim Neustart löschbar
};

const std::error_category& error_category() noexcept;
std::error_code make_error_code(Error e) noexcept;

}  // namespace unlocker

template <>
struct std::is_error_code_enum<unlocker::Error> : std::true_type {};
