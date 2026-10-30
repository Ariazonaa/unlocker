#pragma once
// User-Mode-Client für den Unlocker-Kernel-Treiber (v2).
//
// Der Treiber wird ON-DEMAND als bedienbarer Dienst geladen ("nachladbar" laut PLAN.md) und nach
// dem Schließen wieder gestoppt – er ist kein dauerhafter Bestandteil und landet nicht im
// Portable-ZIP. Fehlt die .sys oder der Systemzustand (kein Test-Signing/WHQL, HVCI), ist
// open_session() leer und unlock() arbeitet unverändert rein im User-Mode.

#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>

#include <cstdint>

namespace unlocker::driver {

// Lädt den Treiber (falls .sys vorhanden und System bereit) als bedienbaren Dienst und öffnet das
// Gerät. Mehrere Instanzen teilen sich den einmal geladenen Dienst (Refcount intern).
//
//   sys_path  Pfad zur unlocker_driver.sys (leer = neben der eigenen EXE suchen).
//
// Gibt std::nullopt zurück, wenn der Treiber nicht zur Verfügung steht; die Ursache steckt in
// last_unavailable_reason() (für UI/CLI-Anzeige).
class Session;
std::optional<Session> open_session(const std::filesystem::path& sys_path = {});

// Menschlich lesbare Begründung, warum open_session() zuletzt leer war ("" wenn offen/keine Ursache).
const char* last_unavailable_reason() noexcept;

class Session {
public:
    // Schließt ein Handle im Zielprozess per IOCTL. Liefert std::nullopt bei Erfolg, sonst den
    // NT-Status als std::error_code (System-Kategorie) – z. B. STATUS_ACCESS_DENIED bei PPL.
    std::optional<std::error_code> close_handle(std::uint32_t pid, std::uintptr_t handle) const;

    // Stoppt (und entlädt) den Treiber, wenn keine weitere Session mehr offen ist.
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Explizit in der .cpp (PIMPL): impl_ ist hier inkomplett, MSVC kann ihn sonst nicht löschen.
    Session(Session&& other) noexcept;
    Session& operator=(Session&& other) noexcept;

private:
    Session();
    friend std::optional<Session> open_session(const std::filesystem::path&);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace unlocker::driver
