#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace unlocker::process {

// Vollständiger Pfad des Prozessabbilds; "System" für PID 4. Leer, wenn nicht lesbar.
std::wstring image_path(std::uint32_t pid);

// Nur der Abbildname (z. B. "csrss.exe") aus der Systemprozessliste – funktioniert auch für Prozesse,
// die sich nicht öffnen lassen. Leer, wenn die PID nicht existiert.
std::wstring image_name_from_system(std::uint32_t pid);

std::wstring name_from_path(std::wstring_view image_path);

// Startzeit des Prozesses, um PID-Wiederverwendung zu erkennen.
bool start_time(std::uint32_t pid, FILETIME& out);

// PPL/Protected Process (ProcessProtectionInformation). Braucht nur PROCESS_QUERY_LIMITED_INFORMATION.
bool is_protected_process(std::uint32_t pid);

// 32-Bit-Prozess unter 64-Bit-Windows.
bool is_wow64(HANDLE process);

// Arbeitsverzeichnis aus dem PEB (RTL_USER_PROCESS_PARAMETERS.CurrentDirectory), auch für WOW64-Prozesse
// über den 32-Bit-PEB. Braucht PROCESS_QUERY_INFORMATION | PROCESS_VM_READ. Leer, wenn nicht lesbar.
std::wstring current_directory(HANDLE process);

// Systemprozesse, deren Beenden Windows destabilisiert.
bool is_critical(std::uint32_t pid, std::wstring_view process_name) noexcept;

// Prozesse, bei denen Beenden unangenehm, aber nicht fatal ist (explorer.exe, svchost.exe).
bool is_sensitive(std::wstring_view process_name) noexcept;

std::error_code terminate(std::uint32_t pid, std::uint32_t wait_ms = 2000);

bool enable_debug_privilege();
bool is_elevated();

// Startet die eigene EXE mit UAC-Prompt neu. ERROR_CANCELLED, wenn der Nutzer ablehnt.
std::error_code relaunch_elevated(std::wstring_view arguments);

}  // namespace unlocker::process
