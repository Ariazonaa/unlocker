#pragma once

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace unlocker::path_util {

// Vollständiger DOS-Pfad eines offenen Handles ohne \\?\-Präfix. Leer bei Fehler.
std::wstring final_path(HANDLE file);

// Löst Symlinks, 8.3-Namen und Groß-/Kleinschreibung auf. Fallback: lexikalisch normalisiert.
std::wstring normalize(const std::filesystem::path& path);

// 8.3-Kurznamen im Pfad in Langform (GetLongPathNameW über \\?\), absolut. Anders als normalize() bleiben UNC-Pfade,
// gemappte Laufwerke und Symlinks wie angegeben – für die Anzeige. Unverändert, wenn der Pfad nicht existiert.
// Berührt das Dateisystem (bei Netzpfaden kann das blockieren) – nicht im UI-Thread aufrufen.
std::wstring long_form(const std::filesystem::path& path);

// \\?\C:\x -> C:\x, \\?\UNC\srv\share -> \\srv\share
std::wstring strip_prefix(std::wstring path);

// Pfad als absoluten \\?\-Pfad für lange Pfade (> MAX_PATH).
std::wstring to_extended(const std::filesystem::path& path);

// Existenz-/Verzeichnisprüfung über den \\?\-Pfad: funktioniert jenseits von MAX_PATH auch ohne die
// LongPathsEnabled-Richtlinie (das longPathAware-Manifest greift nur zusammen mit ihr).
bool exists(const std::filesystem::path& path);
bool is_directory(const std::filesystem::path& path);

// Existenzprüfung mit Zeitlimit: Ein unerreichbarer Netzpfad blockiert sonst bis zum SMB-Timeout (~20 s).
// Die Abfrage läuft auf einem Hilfsthread; nach Ablauf wird Timeout gemeldet und der Thread sich selbst
// überlassen. timeout 0 = unbegrenzt.
enum class Reachability { Exists, Missing, Timeout };
Reachability probe(const std::filesystem::path& path, std::chrono::milliseconds timeout);

// UNC-Pfade auf den eigenen Rechner (\\localhost\C$\x, \\<name>\Freigabe\x) und darauf gemappte Laufwerke
// in den lokalen Pfad übersetzen. Über den SMB-Redirector sieht der Kernel lokale Handles nicht – der
// Scan muss auf dem lokalen Pfad laufen. Unverändert, wenn nicht lokal oder nicht auflösbar.
std::filesystem::path localize(const std::filesystem::path& path);

bool equals_ci(std::wstring_view a, std::wstring_view b) noexcept;

// candidate == root oder candidate liegt unterhalb von root.
bool is_same_or_inside(std::wstring_view candidate, std::wstring_view root) noexcept;

}  // namespace unlocker::path_util
