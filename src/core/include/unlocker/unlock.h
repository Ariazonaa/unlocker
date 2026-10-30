#pragma once

#include "unlocker/scanner.h"

#include <cstdint>
#include <system_error>
#include <vector>

namespace unlocker {

enum class UnlockMode {
    CloseHandles,        // nur Handles schließen; Modul-Sperren bleiben bestehen
    TerminateProcesses,  // alle sperrenden Prozesse beenden
    Auto,                // Handles schließen, Prozesse nur bei Modul-/unbekannten Sperren beenden
};

struct UnlockFailure {
    Locker locker;
    std::error_code error;
};

struct UnlockReport {
    std::size_t handles_closed = 0;
    std::size_t processes_terminated = 0;
    std::vector<UnlockFailure> failures;

    bool ok() const noexcept { return failures.empty(); }
};

// Schließt ein Handle im Zielprozess (DuplicateHandle + DUPLICATE_CLOSE_SOURCE).
std::error_code close_remote_handle(std::uint32_t pid, std::uintptr_t handle);

// false, wenn der Prozess beendet wurde, die PID inzwischen neu vergeben ist oder er sich nicht abfragen lässt.
// unlock() unterscheidet die Fälle: nur „beendet/neu vergeben“ gilt als erledigt, „nicht abfragbar“ ist ein Fehler.
bool process_still_matches(const Locker& locker);

// true, wenn unlock() diese Sperre im gegebenen Modus durch Beenden des Prozesses löst (statt ein Handle zu schließen).
// Die UI fragt damit vor dem Beenden nach – ohne eigene Kopie der Regel.
bool terminates_process(const Locker& locker, UnlockMode mode) noexcept;

// use_driver: falls User-Mode das Schließen nicht schafft, den Kernel-Treiber als Fallback nutzen
// (v2, opt-in). sys_path leitet, wo die .sys liegt (leer = neben der EXE suchen). Ohne Treiber
// (fehlend / kein Test-Signing / HVCI) fällt alles sauber auf reinen User-Mode zurück.
UnlockReport unlock(const std::vector<Locker>& lockers, UnlockMode mode, bool allow_critical = false,
                    bool use_driver = false, const std::filesystem::path& sys_path = {});

}  // namespace unlocker
