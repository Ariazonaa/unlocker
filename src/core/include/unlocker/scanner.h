#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace unlocker {

enum class LockKind {
    FileHandle,        // offenes Datei-/Verzeichnis-Handle – kann im Zielprozess geschlossen werden
    MappedImage,       // EXE/DLL ist in den Prozess geladen – nur durch Beenden des Prozesses lösbar
    ProcessOnly,       // Prozess benutzt die Datei, konkretes Handle nicht ermittelbar (Rechte / kein Deep-Scan)
    WorkingDirectory,  // Arbeitsverzeichnis des Prozesses liegt im Ziel (Handle vorhanden → schließbar)
    RemoteOpen,        // über eine Netzwerkfreigabe geöffnet (SMB-Server, PID 4); handle = Datei-ID für NetFileClose
    SystemHeld,        // vom Kernel/einem Treiber gehalten (PID 4), kein Prozess dahinter – nur beim Neustart löschbar
    Unattributed,      // gesperrt (exklusives Öffnen scheitert), aber kein Prozess meldet die Datei (pid 0) – nur Neustart
};

struct Locker {
    std::uint32_t pid = 0;
    FILETIME process_start{};      // gegen PID-Wiederverwendung
    std::wstring process_name;     // z. B. "WINWORD.EXE"
    std::wstring image_path;       // vollständiger Pfad der EXE
    std::wstring app_name;         // Anzeigename laut Restart Manager, falls bekannt
    LockKind kind = LockKind::ProcessOnly;
    std::uintptr_t handle = 0;     // bei FileHandle/WorkingDirectory; RemoteOpen: Datei-ID des Servers; 0 = unbekannt
    std::wstring remote_user;      // RemoteOpen: Benutzer, der die Datei über die Freigabe geöffnet hat
    std::wstring remote_client;    // RemoteOpen: Rechner (Name/IP) des Benutzers, nur wenn eindeutig; sonst leer
    std::wstring locked_path;      // tatsächlich betroffener Pfad (bei Ordnern: Eintrag darin)
    bool is_service = false;
    bool is_critical = false;      // Systemprozess, darf nicht beendet werden
    bool is_sensitive = false;     // explorer.exe u. ä. – Beenden nur nach Rückfrage
    bool is_protected = false;     // PPL/Protected Process – nicht zugreifbar, auch nicht als Administrator
    bool protect_close = false;    // Handle hat OBJ_PROTECT_CLOSE – lässt sich nicht schließen
    bool is_wow64 = false;         // 32-Bit-Prozess auf 64-Bit-Windows
};

// Fortschrittsmeldung: stage ist ein stabiler ASCII-Bezeichner ("fastpath", "handles", "modules", "cwd",
// "restart-manager"), total == 0 bedeutet „Anzahl unbekannt“.
struct ScanProgress {
    const wchar_t* stage = L"";
    std::size_t done = 0;
    std::size_t total = 0;
};

struct ScanOptions {
    bool deep = true;                               // Handles einzeln auflösen
    bool include_modules = true;                    // geladene EXE/DLLs berücksichtigen
    bool use_restart_manager = true;                // Stufe 2 (liefert App-Namen, etwas langsamer)
    bool trace = false;                             // Stufen-Zeiten auf stderr (zusätzlich zu UNLOCKER_TRACE=1)
    std::function<void(const ScanProgress&)> progress;  // optional, wird aus dem Scan-Thread gerufen
    std::chrono::milliseconds name_query_timeout{250};    // pro Handle (Einzelabfrage)
    std::chrono::milliseconds snapshot_timeout{1500};     // Gesamtbudget für die Pfadauflösung im Snapshot-Modus
    std::chrono::milliseconds target_timeout{0};          // Existenzprüfung je Ziel (Netzpfade); 0 = SMB-Timeout des Systems
    std::size_t max_restart_manager_files = 1000;
    // Die Kernel-Abfrage pro Datei kostet je nach Prozesszahl 1–15 ms. Ab so vielen Einträgen wird stattdessen
    // die System-Handle-Tabelle einmal komplett ausgewertet (Snapshot-Modus).
    std::size_t per_file_query_limit = 32;
};

struct ScanResult {
    std::wstring target;               // erstes Ziel, normalisiert
    std::vector<std::wstring> roots;   // alle Ziele, normalisiert
    bool is_directory = false;         // mindestens ein Ziel ist ein Ordner
    bool access_denied = false;        // mind. ein (nicht geschützter) Prozess war nicht lesbar – Elevation empfehlen
    bool has_protected = false;        // mind. ein geschützter Prozess (PPL) – auch mit Elevation nicht lösbar
    bool enriched = false;             // Restart-Manager-Daten (App-Namen, Dienste) sind eingearbeitet
    std::vector<std::wstring> missing; // Ziele, die nicht existieren oder nicht erreichbar sind (wie übergeben; Prüfung läuft im Scan)
    bool snapshot_mode = false;        // Handle-Tabelle statt Kernel-Abfrage pro Datei (große Ordner)
    bool snapshot_timeout = false;     // nicht alle Handles konnten im Zeitbudget aufgelöst werden
    std::vector<Locker> lockers;
};

// Stufe 1 + 3 (+ Module). Mit options.use_restart_manager zusätzlich enrich() – das kostet ~0,5 s,
// GUIs zeigen besser erst das schnelle Ergebnis und rufen enrich() nachträglich.
ScanResult scan(const std::filesystem::path& target, const ScanOptions& options = {});
ScanResult scan(const std::vector<std::filesystem::path>& targets, const ScanOptions& options = {});

// Stufe 2: Restart Manager liefert Anzeigenamen, Dienstkennung und ggf. weitere Prozesse.
void enrich(ScanResult& result, const std::vector<std::filesystem::path>& targets, const ScanOptions& options = {});

const wchar_t* to_string(LockKind kind) noexcept;

}  // namespace unlocker
