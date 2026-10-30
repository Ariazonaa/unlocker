#include "unlocker/unlock.h"

#include "unlocker/driver_client.h"
#include "unlocker/errors.h"
#include "unlocker/path_util.h"
#include "unlocker/process.h"
#include "unlocker/win_handle.h"

#include <lm.h>

#include <optional>
#include <set>
#include <string>

namespace unlocker {
namespace {

enum class DriverAttempt {
    Succeeded,    // Treiber hat das Handle geschlossen
    Failed,       // Treiber verfügbar, aber Schließen nicht möglich (Status gemeldet)
    NotAvailable, // Treiber nicht da (keine .sys / kein Signing / kein Admin) – User-Mode-Fehler bleibt
};

// Fallback: Treiber-Session lazy öffnen (einmal pro Unlock-Aufruf) und das Handle schließen. Bei Failed steht der
// Fehler des Treibers in driver_error – der sagt mehr als der vorangegangene User-Mode-Fehler.
DriverAttempt driver_attempt(std::optional<driver::Session>& session, const std::filesystem::path& sys_path,
                             const Locker& locker, std::error_code& driver_error) {
    if (!session) session = driver::open_session(sys_path);
    if (!session) return DriverAttempt::NotAvailable;
    if (auto ec = session->close_handle(locker.pid, locker.handle)) {
        driver_error = *ec;
        return DriverAttempt::Failed;
    }
    return DriverAttempt::Succeeded;
}

enum class Liveness {
    Same,     // derselbe Prozess wie beim Scan
    Gone,     // beendet oder PID neu vergeben – die Sperre existiert so nicht mehr
    Unknown,  // nicht abfragbar (Rechte) – die Sperre kann weiter bestehen
};

Liveness liveness(const Locker& locker, std::error_code& ec) {
    WinHandle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, locker.pid));
    if (!proc) {
        const DWORD err = GetLastError();
        if (err == ERROR_INVALID_PARAMETER) return Liveness::Gone;  // PID existiert nicht (mehr)
        ec = win_error(err);
        return Liveness::Unknown;
    }
    FILETIME start{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(proc.get(), &start, &exit, &kernel, &user)) {
        ec = last_error();
        return Liveness::Unknown;
    }
    if (locker.process_start.dwLowDateTime == 0 && locker.process_start.dwHighDateTime == 0) return Liveness::Same;
    return CompareFileTime(&start, &locker.process_start) == 0 ? Liveness::Same : Liveness::Gone;
}

bool closable(const Locker& locker) noexcept {
    return (locker.kind == LockKind::FileHandle || locker.kind == LockKind::WorkingDirectory) && locker.handle != 0;
}

}  // namespace

bool terminates_process(const Locker& locker, UnlockMode mode) noexcept {
    // Sperren ohne eigenen Prozess (PID 4): niemals beenden – Freigabe-Zugriffe werden geschlossen, Systemsperren gemeldet.
    if (locker.kind == LockKind::RemoteOpen || locker.kind == LockKind::SystemHeld ||
        locker.kind == LockKind::Unattributed) {
        return false;
    }
    return mode == UnlockMode::TerminateProcesses || (mode == UnlockMode::Auto && !closable(locker));
}

std::error_code close_remote_handle(std::uint32_t pid, std::uintptr_t handle) {
    WinHandle proc(OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid));
    if (!proc) return last_error();

    HANDLE local = nullptr;
    if (!DuplicateHandle(proc.get(), reinterpret_cast<HANDLE>(handle), GetCurrentProcess(), &local, 0, FALSE,
                         DUPLICATE_CLOSE_SOURCE)) {
        return last_error();
    }
    CloseHandle(local);
    return {};
}

bool process_still_matches(const Locker& locker) {
    std::error_code ec;
    return liveness(locker, ec) == Liveness::Same;
}

UnlockReport unlock(const std::vector<Locker>& lockers, UnlockMode mode, bool allow_critical, bool use_driver,
                    const std::filesystem::path& sys_path) {
    UnlockReport report;
    std::set<std::uint32_t> terminated;
    std::optional<driver::Session> driver_session;  // lazy, nur wenn use_driver
    std::vector<const Locker*> remote_opens;        // zuletzt – erst wenn lokale Handles zu sind, sagt die Prüfung etwas

    for (const auto& locker : lockers) {
        if (locker.kind == LockKind::RemoteOpen) {
            remote_opens.push_back(&locker);
            continue;
        }
        if (locker.kind == LockKind::SystemHeld) {
            report.failures.push_back({locker, make_error_code(Error::system_held)});
            continue;
        }
        if (locker.kind == LockKind::Unattributed) {
            report.failures.push_back({locker, make_error_code(Error::no_process)});
            continue;
        }
        if (terminated.contains(locker.pid)) continue;

        if (locker.is_critical && !allow_critical) {
            report.failures.push_back({locker, make_error_code(Error::critical_process)});
            continue;
        }
        if (locker.is_protected) {
            report.failures.push_back({locker, make_error_code(Error::protected_process)});
            continue;
        }

        // Prozess bereits weg oder PID neu vergeben: Sperre existiert so nicht mehr. Nicht abfragbar heißt dagegen
        // nicht „erledigt“ – das wäre ein Scheinerfolg, die Datei bliebe gesperrt.
        std::error_code live_error;
        const auto live = liveness(locker, live_error);
        if (live == Liveness::Gone) continue;
        if (live == Liveness::Unknown) {
            report.failures.push_back({locker, live_error});
            continue;
        }

        if (terminates_process(locker, mode)) {
            if (auto ec = process::terminate(locker.pid)) {
                report.failures.push_back({locker, ec});
            } else {
                ++report.processes_terminated;
                terminated.insert(locker.pid);
            }
        } else if (closable(locker)) {
            if (locker.protect_close) {
                // DuplicateHandle(CLOSE_SOURCE) würde "Erfolg" melden, das Handle bliebe aber offen.
                report.failures.push_back({locker, make_error_code(Error::protected_handle)});
                continue;
            }
            if (auto ec = close_remote_handle(locker.pid, locker.handle)) {
                // User-Mode hat nicht geklappt (Access-Verweigert, PID-Reuse, …). Nur bei explizitem
                // --driver: Treiber als Fallback versuchen. Ohne Treiber bleibt der User-Mode-Fehler.
                if (use_driver) {
                    std::error_code driver_error;
                    switch (driver_attempt(driver_session, sys_path, locker, driver_error)) {
                        case DriverAttempt::Succeeded:
                            ++report.handles_closed;
                            break;
                        case DriverAttempt::Failed:
                            report.failures.push_back({locker, driver_error});
                            break;
                        case DriverAttempt::NotAvailable:
                        default:
                            report.failures.push_back({locker, ec});
                            break;
                    }
                } else {
                    report.failures.push_back({locker, ec});
                }
            } else {
                ++report.handles_closed;
            }
        } else {
            report.failures.push_back({locker, make_error_code(Error::not_supported)});
        }
    }

    // Über eine Freigabe geöffnet: den Zugriff auf dem Server schließen (wie Close-SmbOpenFile), kein Prozess beteiligt.
    // Der Windows-SMB-Client kann das transparent überbrücken (in der Sandbox per Loopback beobachtet: der Client schreibt
    // weiter, der Server listet nichts mehr, die Datei bleibt gesperrt) – deshalb erst als gelöst zählen, wenn sich die
    // Datei wirklich exklusiv öffnen lässt. Sonst kein Scheinerfolg, sondern Error::still_locked.
    for (const Locker* locker : remote_opens) {
        const NET_API_STATUS status = NetFileClose(nullptr, static_cast<DWORD>(locker->handle));
        if (status != NERR_Success && status != NERR_FileIdNotFound) {  // FileIdNotFound: inzwischen von selbst zu
            report.failures.push_back({*locker, win_error(status)});
            continue;
        }
        bool released = false;
        for (int attempt = 0; attempt < 10 && !released; ++attempt) {
            WinHandle probe(CreateFileW(path_util::to_extended(locker->locked_path).c_str(), GENERIC_READ, 0, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            released = probe || GetLastError() != ERROR_SHARING_VIOLATION;
            if (!released) Sleep(100);
        }
        if (!released) report.failures.push_back({*locker, make_error_code(Error::still_locked)});
        else if (status == NERR_Success) ++report.handles_closed;
    }
    return report;
}

}  // namespace unlocker
