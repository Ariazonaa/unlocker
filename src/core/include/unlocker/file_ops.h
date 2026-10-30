#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>

namespace unlocker::file_ops {

// Datei oder Ordner (rekursiv) löschen. Reparse-Points werden entfernt, nicht verfolgt.
// Bei Sharing-Konflikten wird POSIX-Delete (Windows 10 RS1+) versucht. Bei einer Datei werden danach verwaiste
// Office-Sperrdateien dazu entfernt (".~lock.<Name>#", "~$<Name>"; nur klein, nur eindeutig, gehaltene bleiben).
std::error_code remove(const std::filesystem::path& path);

// Beim nächsten Neustart löschen (Dateien zuerst, Ordner danach). Benötigt Administratorrechte.
std::error_code remove_on_reboot(const std::filesystem::path& path);

std::error_code rename(const std::filesystem::path& path, std::wstring_view new_name);

// Ist destination ein vorhandener Ordner, wird darin abgelegt; laufwerksübergreifend wird kopiert.
std::error_code move(const std::filesystem::path& path, const std::filesystem::path& destination);

std::error_code copy(const std::filesystem::path& path, const std::filesystem::path& destination);

}  // namespace unlocker::file_ops
