// Smoke-Tests für den Kern, bewusst ohne Test-Framework.
// Der Scanner blendet den eigenen Prozess aus, daher sperrt ein Kindprozess (--hold) die Testdatei.

#include "unlocker/driver_client.h"
#include "unlocker/errors.h"
#include "unlocker/file_ops.h"
#include "unlocker/path_util.h"
#include "unlocker/process.h"
#include "unlocker/scanner.h"
#include "unlocker/unlock.h"
#include "unlocker/win_handle.h"

#include <windows.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++g_failures;                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                             \
    } while (0)

std::filesystem::path temp_dir() {
    wchar_t buffer[MAX_PATH]{};
    GetTempPathW(MAX_PATH, buffer);
    return std::filesystem::path(buffer) / L"unlocker_tests";
}

std::wstring own_exe() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return path;
}

// Kindprozess: Datei exklusiv öffnen und warten, bis wir beendet werden.
int hold_file(const wchar_t* path) {
    HANDLE h = CreateFileW(unlocker::path_util::to_extended(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 1;
    Sleep(60000);
    CloseHandle(h);
    return 0;
}

// Kindprozess für den Treiber-Test: wie hold_file, schreibt zusätzlich den Handle-Wert nach report.
int hold_file_report(const wchar_t* path, const wchar_t* report) {
    HANDLE h = CreateFileW(unlocker::path_util::to_extended(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 1;
    FILE* f = nullptr;
    _wfopen_s(&f, report, L"w");
    if (f) {
        std::fprintf(f, "%llu", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(h)));
        std::fclose(f);
    }
    Sleep(60000);
    CloseHandle(h);
    return 0;
}

unlocker::WinHandle spawn_holder(const std::filesystem::path& file, DWORD& pid, const wchar_t* working_directory = nullptr,
                                 SECURITY_ATTRIBUTES* process_attributes = nullptr) {
    std::wstring cmd = L"\"" + own_exe() + L"\" --hold \"" + file.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), process_attributes, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, working_directory,
                        &si, &pi)) {
        return {};
    }
    CloseHandle(pi.hThread);
    pid = pi.dwProcessId;
    return unlocker::WinHandle(pi.hProcess);
}

void test_path_util() {
    using namespace unlocker::path_util;
    CHECK(strip_prefix(L"\\\\?\\C:\\x") == L"C:\\x");
    CHECK(strip_prefix(L"\\\\?\\UNC\\srv\\share") == L"\\\\srv\\share");
    CHECK(strip_prefix(L"C:\\x") == L"C:\\x");
    CHECK(to_extended(L"C:\\x\\y") == L"\\\\?\\C:\\x\\y");
    CHECK(to_extended(L"\\\\srv\\share\\f") == L"\\\\?\\UNC\\srv\\share\\f");
    CHECK(equals_ci(L"Explorer.EXE", L"explorer.exe"));
    CHECK(is_same_or_inside(L"C:\\a\\b", L"C:\\a"));
    CHECK(is_same_or_inside(L"C:\\A\\B", L"c:\\a\\b"));
    CHECK(is_same_or_inside(L"C:\\a\\b", L"C:\\a\\"));
    CHECK(!is_same_or_inside(L"C:\\ab", L"C:\\a"));
    CHECK(!is_same_or_inside(L"C:\\a", L"C:\\a\\b"));
}

void test_scan_and_unlock() {
    const auto dir = temp_dir();
    std::filesystem::create_directories(dir);
    const auto file = dir / L"locked.tmp";

    DWORD pid = 0;
    auto holder = spawn_holder(file, pid);
    CHECK(static_cast<bool>(holder));
    if (!holder) return;

    // Warten, bis das Kind die Datei hält.
    unlocker::ScanResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) {
            result = unlocker::scan(file);
            if (!result.lockers.empty()) break;
        }
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    CHECK(!result.is_directory);
    CHECK(result.lockers.size() == 1);
    if (!result.lockers.empty()) {
        const auto& l = result.lockers.front();
        CHECK(l.pid == pid);
        CHECK(l.kind == unlocker::LockKind::FileHandle);
        CHECK(l.handle != 0);
        CHECK(unlocker::path_util::equals_ci(l.process_name, L"core_tests.exe"));
        CHECK(unlocker::path_util::equals_ci(l.locked_path, result.target));
        CHECK(!l.is_critical);

        const auto report = unlocker::unlock(result.lockers, unlocker::UnlockMode::CloseHandles);
        CHECK(report.ok());
        CHECK(report.handles_closed == 1);
        CHECK(report.processes_terminated == 0);

        const auto after = unlocker::scan(file);
        CHECK(after.lockers.empty());
    }

    TerminateProcess(holder.get(), 0);
    WaitForSingleObject(holder.get(), 5000);
}

// SeDebugPrivilege im eigenen Token an-/abschalten. Liefert, ob es vorher aktiv war (false auch, wenn nicht vorhanden).
bool set_debug_privilege(bool enable) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw)) return false;
    unlocker::WinHandle token(raw);
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) return false;
    TOKEN_PRIVILEGES previous{};
    DWORD len = sizeof(previous);
    if (!AdjustTokenPrivileges(token.get(), FALSE, &tp, sizeof(previous), &previous, &len)) return false;
    return previous.PrivilegeCount == 1 && (previous.Privileges[0].Attributes & SE_PRIVILEGE_ENABLED) != 0;
}

// Sperrender Prozess, den wir nicht abfragen dürfen (Prozess-DACL ohne Einträge; SeDebugPrivilege wird dafür kurz
// abgeschaltet, sonst greift die DACL erhöht nicht – in der Sandbox ist es aktiv): unlock() darf die Sperre nicht
// stillschweigend als erledigt überspringen, sondern muss sie als Fehler melden.
void test_unlock_unqueryable() {
    const bool debug_was_enabled = set_debug_privilege(false);
    struct Restore {
        bool enable;
        ~Restore() {
            if (enable) set_debug_privilege(true);
        }
    } restore{debug_was_enabled};

    const auto dir = temp_dir();
    std::filesystem::create_directories(dir);
    const auto file = dir / L"unqueryable.tmp";

    SECURITY_DESCRIPTOR sd{};
    ACL empty_dacl{};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    InitializeAcl(&empty_dacl, sizeof(empty_dacl), ACL_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, &empty_dacl, FALSE);
    SECURITY_ATTRIBUTES sa{sizeof(sa), &sd, FALSE};

    DWORD pid = 0;
    auto holder = spawn_holder(file, pid, nullptr, &sa);
    CHECK(static_cast<bool>(holder));
    if (!holder) return;

    if (unlocker::WinHandle probe(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)); probe) {
        // z. B. als SYSTEM oder mit aktivem SeDebugPrivilege – dann greift die DACL nicht
        std::printf("SKIP nicht abfragbarer Prozess (DACL greift nicht, SeDebugPrivilege aktiv?)\n");
    } else {
        unlocker::ScanResult result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            std::error_code ec;
            if (std::filesystem::exists(file, ec)) {
                result = unlocker::scan(file, unlocker::ScanOptions{.use_restart_manager = false});
                if (!result.lockers.empty()) break;
            }
            if (std::chrono::steady_clock::now() > deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(result.access_denied);
        CHECK(result.lockers.size() == 1);
        if (!result.lockers.empty()) {
            CHECK(result.lockers.front().pid == pid);
            CHECK(result.lockers.front().kind == unlocker::LockKind::ProcessOnly);

            const auto report = unlocker::unlock(result.lockers, unlocker::UnlockMode::Auto);
            CHECK(!report.ok());
            CHECK(report.handles_closed == 0);
            CHECK(report.processes_terminated == 0);
            CHECK(report.failures.size() == 1);
            if (!report.failures.empty()) {
                CHECK(report.failures.front().error == unlocker::win_error(ERROR_ACCESS_DENIED));
            }
        }
    }

    TerminateProcess(holder.get(), 0);
    WaitForSingleObject(holder.get(), 5000);
}

// Arbeitsverzeichnis-Sperre: Kindprozess mit CWD im Ziel – als 8.3-Pfad gesetzt, wenn das Volume Kurznamen hat
// (so meldet der PEB "C:\Users\ADMINI~1\..."; der Scanner muss das in Langform mit den Wurzeln vergleichen).
void test_working_directory() {
    const auto dir = temp_dir() / L"cwd-test";
    std::filesystem::create_directories(dir);
    std::wstring cwd = dir.wstring();
    wchar_t short_path[MAX_PATH]{};
    if (GetShortPathNameW(dir.c_str(), short_path, MAX_PATH) > 0 && !unlocker::path_util::equals_ci(short_path, cwd)) {
        cwd = short_path;
    } else {
        std::printf("HINWEIS Arbeitsverzeichnis-Test ohne 8.3-Namen\n");
    }
    const auto outside = temp_dir() / L"cwd-outside.tmp";
    DWORD pid = 0;
    auto holder = spawn_holder(outside, pid, cwd.c_str());
    CHECK(holder);
    std::error_code ec;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && !std::filesystem::exists(outside, ec)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto result = unlocker::scan(dir);
    const auto it = std::find_if(result.lockers.begin(), result.lockers.end(), [&](const unlocker::Locker& l) {
        return l.pid == pid && l.kind == unlocker::LockKind::WorkingDirectory;
    });
    CHECK(it != result.lockers.end());
    if (it != result.lockers.end()) {
        CHECK(unlocker::path_util::equals_ci(it->locked_path, result.target));
        CHECK(it->handle != 0);
        const auto report = unlocker::unlock({*it}, unlocker::UnlockMode::CloseHandles);
        CHECK(report.handles_closed == 1);
    }
    TerminateProcess(holder.get(), 0);
    WaitForSingleObject(holder.get(), 5000);
    std::filesystem::remove(outside, ec);
    CHECK(!unlocker::file_ops::remove(dir));
}

// Treiber-Fallback (v2): use_driver=true mit User-Mode-Fehlschlag versucht den Treiber, der ohne
// .sys nicht verfügbar ist – der User-Mode-Fehler bleibt erhalten, der Grund ist dokumentiert.
// Synthetische Sperre: eigener Prozess (immer öffenbar), ungültiger Handle-Wert →
// DuplicateHandle schlägt determiniert mit ERROR_INVALID_HANDLE fehl.
void test_driver_fallback() {
    unlocker::Locker locker;
    locker.pid = GetCurrentProcessId();
    locker.process_start = {};            // null → kein PID-Reuse-Check
    locker.kind = unlocker::LockKind::FileHandle;
    locker.handle = 0x7FFF0000;           // kein Prozess hat so viele Handles → ungültig
    const auto ec_expected = std::error_code(ERROR_INVALID_HANDLE, std::system_category());

    // 1. Ohne Treiber: reiner User-Mode-Fehler.
    const auto report_plain = unlocker::unlock({locker}, unlocker::UnlockMode::CloseHandles);
    CHECK(report_plain.handles_closed == 0);
    CHECK(report_plain.failures.size() == 1);
    CHECK(report_plain.failures.empty() || report_plain.failures.front().error == ec_expected);

    // 2. Mit Treiber-Anfrage, aber ohne .sys: Fallback nicht verfügbar → gleicher Fehler.
    const auto report_driver = unlocker::unlock({locker}, unlocker::UnlockMode::CloseHandles, false, true,
                                                temp_dir() / L"unlocker_driver_missing.sys");
    CHECK(report_driver.handles_closed == 0);
    CHECK(report_driver.failures.size() == 1);
    CHECK(report_driver.failures.empty() || report_driver.failures.front().error == ec_expected);
    CHECK(std::string(unlocker::driver::last_unavailable_reason()).find("not found") != std::string::npos);

    // 3. Direkt: open_session ohne .sys → leer mit Begründung. last_unavailable_reason() liefert nie nullptr,
    // sondern "" ohne Ursache – geprüft wird deshalb der Inhalt (vorher schlug eine nullptr-Prüfung nie an).
    CHECK(!unlocker::driver::open_session(temp_dir() / L"unlocker_driver_missing_too.sys").has_value());
    const std::string direct_reason = unlocker::driver::last_unavailable_reason();
    CHECK(direct_reason.find("not found") != std::string::npos);
    CHECK(direct_reason.find("unlocker_driver_missing_too.sys") != std::string::npos);
}

// Dienst des Treibers entfernen: driver_client legt ihn bei Bedarf an und lässt ihn als On-Demand-Eintrag stehen –
// ein alter Eintrag würde mit seinem alten ImagePath wiederverwendet.
void delete_driver_service() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return;
    if (SC_HANDLE service = OpenServiceW(scm, L"UnlockerDriver", SERVICE_STOP | DELETE)) {
        SERVICE_STATUS status{};
        ControlService(service, SERVICE_CONTROL_STOP, &status);
        DeleteService(service);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
}

DWORD find_process(const wchar_t* name) {
    unlocker::WinHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot.get(), &entry); ok; ok = Process32NextW(snapshot.get(), &entry)) {
        if (unlocker::path_util::equals_ci(entry.szExeFile, name)) return entry.th32ProcessID;
    }
    return 0;
}

DWORD find_protected_process() {
    unlocker::WinHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot.get(), &entry); ok; ok = Process32NextW(snapshot.get(), &entry)) {
        if (entry.th32ProcessID > 4 && unlocker::process::is_protected_process(entry.th32ProcessID)) return entry.th32ProcessID;
    }
    return 0;
}

// Treiber end-to-end – nur mit UNLOCKER_TEST_DRIVER=<signierte .sys>, erhöht und im Testmodus, in einer VM (nie auf
// dem Arbeitsrechner, BSOD-Risiko). Der Kindprozess erlaubt per DACL nur PROCESS_QUERY_LIMITED_INFORMATION |
// SYNCHRONIZE: abfragbar (PID-Prüfung in unlock() klappt), aber DuplicateHandle braucht PROCESS_DUP_HANDLE – das
// ist der Fall, für den der Treiber da ist. Dazu die PPL-Abweisung mit ungültigem Handle-Wert (kann nichts schließen).
void test_driver_end_to_end() {
    wchar_t sys[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"UNLOCKER_TEST_DRIVER", sys, MAX_PATH) == 0) {
        std::printf("SKIP Treiber end-to-end (UNLOCKER_TEST_DRIVER nicht gesetzt)\n");
        return;
    }
    if (!unlocker::process::is_elevated()) {
        std::printf("SKIP Treiber end-to-end (nicht erhöht)\n");
        return;
    }
    delete_driver_service();
    const bool debug_was_enabled = set_debug_privilege(false);  // sonst greift die DACL nicht
    struct Restore {
        bool enable;
        ~Restore() {
            if (enable) set_debug_privilege(true);
        }
    } restore{debug_was_enabled};

    const auto dir = temp_dir();
    std::filesystem::create_directories(dir);
    const auto file = dir / L"driver-locked.tmp";
    const auto report = dir / L"driver-locked.handle";
    std::error_code ec;
    std::filesystem::remove(report, ec);

    PSECURITY_DESCRIPTOR sd = nullptr;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;0x101000;;;WD)", SDDL_REVISION_1, &sd, nullptr));
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    std::wstring cmd = L"\"" + own_exe() + L"\" --hold-report \"" + file.wstring() + L"\" \"" + report.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL spawned = CreateProcessW(nullptr, cmd.data(), &sa, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    LocalFree(sd);
    CHECK(spawned);
    if (!spawned) return;
    CloseHandle(pi.hThread);
    unlocker::WinHandle holder(pi.hProcess);

    unsigned long long value = 0;
    for (int i = 0; i < 200 && value == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        FILE* f = nullptr;
        if (_wfopen_s(&f, report.c_str(), L"r") == 0 && f) {
            if (fscanf_s(f, "%llu", &value) != 1) value = 0;
            std::fclose(f);
        }
    }
    CHECK(value != 0);

    unlocker::Locker locker;
    locker.pid = pi.dwProcessId;
    locker.kind = unlocker::LockKind::FileHandle;
    locker.handle = static_cast<std::uintptr_t>(value);
    CHECK(unlocker::process::start_time(locker.pid, locker.process_start));  // QUERY_LIMITED reicht
    CHECK(unlocker::close_remote_handle(locker.pid, locker.handle) == unlocker::win_error(ERROR_ACCESS_DENIED));

    const auto closed = unlocker::unlock({locker}, unlocker::UnlockMode::CloseHandles, false, true, sys);
    if (!closed.ok()) {
        std::printf("  Treiber: %s / Fehler %d\n", unlocker::driver::last_unavailable_reason(),
                    closed.failures.empty() ? 0 : closed.failures.front().error.value());
    }
    CHECK(closed.ok() && closed.handles_closed == 1);
    unlocker::WinHandle probe(CreateFileW(unlocker::path_util::to_extended(file).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    CHECK(static_cast<bool>(probe));  // Handle im Kind ist zu, die Datei exklusiv zu öffnen
    probe.reset();

    // PPL: geschützter Prozess (bevorzugt csrss.exe) mit ungültigem Handle-Wert. Mit SeDebugPrivilege ist er abfragbar,
    // DuplicateHandle scheitert am Schutz -> Treiber. Erwartet: abgewiesen (ERROR_ACCESS_DENIED); ERROR_INVALID_HANDLE
    // hieße, die PPL-Prüfung im Treiber greift nicht. Das Privileg ist oben abgeschaltet – hier wieder an.
    if (debug_was_enabled) set_debug_privilege(true);
    DWORD ppl_pid = find_process(L"csrss.exe");
    if (!ppl_pid || !unlocker::process::is_protected_process(ppl_pid)) ppl_pid = find_protected_process();
    if (ppl_pid) {
        unlocker::Locker ppl;
        ppl.pid = ppl_pid;
        ppl.kind = unlocker::LockKind::FileHandle;
        ppl.handle = 0x7FFF0000;
        const auto refused = unlocker::unlock({ppl}, unlocker::UnlockMode::CloseHandles, false, true, sys);
        CHECK(std::string(unlocker::driver::last_unavailable_reason()).empty());  // Treiber war wirklich gefragt
        CHECK(refused.failures.size() == 1);
        if (!refused.failures.empty()) {
            std::printf("  PPL (%ls, PID %lu): Fehler %d\n", unlocker::process::image_name_from_system(ppl_pid).c_str(), ppl_pid,
                        refused.failures.front().error.value());
            CHECK(refused.failures.front().error == unlocker::win_error(ERROR_ACCESS_DENIED));
        }
    } else {
        std::printf("FAIL PPL-Teil: kein geschützter Prozess abfragbar (SeDebugPrivilege %s)\n",
                    debug_was_enabled ? "vorhanden" : "fehlt");
        ++g_failures;
    }

    TerminateProcess(holder.get(), 0);
    WaitForSingleObject(holder.get(), 5000);
    std::filesystem::remove(file, ec);
    std::filesystem::remove(report, ec);
    delete_driver_service();
    std::printf("Treiber end-to-end: User-Mode verweigert, Treiber hat das Handle geschlossen, PPL abgewiesen\n");
}

// 8.3-Kurzname → normalize() liefert den langen Pfad (sofern Kurznamen auf dem Volume aktiv sind).
void test_short_names() {
    const auto dir = temp_dir() / L"shortname-test";
    std::filesystem::create_directories(dir);
    const auto file = dir / L"ein sehr langer dateiname mit leerzeichen.txt";
    {
        FILE* f = nullptr;
        _wfopen_s(&f, file.c_str(), L"w");
        if (f) std::fclose(f);
    }
    wchar_t short_path[MAX_PATH]{};
    const DWORD len = GetShortPathNameW(file.c_str(), short_path, MAX_PATH);
    if (len == 0 || unlocker::path_util::equals_ci(short_path, file.wstring())) {
        std::printf("SKIP 8.3-Namen (auf diesem Volume deaktiviert)\n");
        return;
    }
    const auto normalized = unlocker::path_util::normalize(short_path);
    CHECK(unlocker::path_util::equals_ci(normalized, unlocker::path_util::normalize(file)));
    CHECK(normalized.find(L'~') == std::wstring::npos);
    // long_form: nur 8.3 auflösen (für die GUI-Anzeige)
    // Vergleich mit der Langform, nicht mit file selbst: %TEMP% kann schon in 8.3-Form stehen (C:\Users\ADMINI~1\…)
    CHECK(unlocker::path_util::equals_ci(unlocker::path_util::long_form(short_path), unlocker::path_util::normalize(file)));
    CHECK(unlocker::path_util::long_form(L"C:\\gibt\\es\\nicht~1.txt") == L"C:\\gibt\\es\\nicht~1.txt");
}

// Junction: normalize() folgt ihr; remove() entfernt nur den Reparse-Point, nie den Inhalt des Ziels.
void test_junction() {
    const auto base = temp_dir() / L"junction-test";
    const auto target = base / L"target";
    const auto link = base / L"link";
    std::filesystem::create_directories(target);
    {
        FILE* f = nullptr;
        _wfopen_s(&f, (target / L"keep.txt").c_str(), L"w");
        if (f) std::fclose(f);
    }
    const std::wstring cmd = L"cmd.exe /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd;
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::printf("SKIP Junction (mklink nicht möglich)\n");
        return;
    }
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::error_code ec;
    CHECK(std::filesystem::exists(link / L"keep.txt", ec));
    const auto via_link = unlocker::path_util::normalize(link / L"keep.txt");
    const auto direct = unlocker::path_util::normalize(target / L"keep.txt");
    CHECK(unlocker::path_util::equals_ci(via_link, direct));

    // Scan über die Junction findet die Sperre auf der echten Datei
    DWORD pid = 0;
    auto holder = spawn_holder(target / L"locked.tmp", pid);
    unlocker::ScanResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(target / L"locked.tmp", ec)) {
            result = unlocker::scan(link);
            if (!result.lockers.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(!result.lockers.empty() && result.lockers.front().pid == pid);
    TerminateProcess(holder.get(), 0);
    WaitForSingleObject(holder.get(), 5000);
    std::filesystem::remove(target / L"locked.tmp", ec);

    // Sicherheitsnetz: Junction löschen darf den Zielinhalt nicht anrühren
    CHECK(!unlocker::file_ops::remove(link));
    CHECK(!std::filesystem::exists(link, ec));
    CHECK(std::filesystem::exists(target / L"keep.txt", ec));
}

// Pfade jenseits von MAX_PATH: anlegen, sperren, finden, löschen.
void test_long_paths() {
    // Ohne LongPathsEnabled-Richtlinie (Standard) scheitern normale Pfade > MAX_PATH – anlegen daher über \\?\.
    auto dir = temp_dir() / L"longpath";
    for (int i = -1; i < 12; ++i) {
        if (i >= 0) dir /= L"ein_ziemlich_langer_ordnername_" + std::to_wstring(i);
        if (!CreateDirectoryW(unlocker::path_util::to_extended(dir).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            std::printf("SKIP lange Pfade (CreateDirectoryW: %lu)\n", GetLastError());
            return;
        }
    }
    const auto file = dir / L"locked.tmp";
    CHECK(file.wstring().size() > MAX_PATH);
    CHECK(unlocker::path_util::is_directory(dir));
    CHECK(!unlocker::path_util::exists(file));

    std::error_code ec;
    DWORD pid = 0;
    auto holder = spawn_holder(file, pid);
    unlocker::ScanResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (unlocker::path_util::exists(file)) {
            result = unlocker::scan(file);
            if (!result.lockers.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(result.lockers.size() == 1);
    // Ordner-Scan über den langen Pfad (Enumeration mit \\?\)
    const auto dir_result = unlocker::scan(temp_dir() / L"longpath");
    CHECK(dir_result.is_directory);
    CHECK(dir_result.lockers.size() == 1 && dir_result.lockers.front().pid == pid);
    if (!result.lockers.empty()) {
        CHECK(result.lockers.front().kind == unlocker::LockKind::FileHandle);
        CHECK(unlocker::path_util::equals_ci(result.lockers.front().locked_path, result.target));
        const auto report = unlocker::unlock(result.lockers, unlocker::UnlockMode::CloseHandles);
        CHECK(report.handles_closed == 1);
    }
    TerminateProcess(holder.get(), 0);
    WaitForSingleObject(holder.get(), 5000);

    // Kopieren jenseits von MAX_PATH: lange Quelle (Datei und Ordner rekursiv) und langes Ziel
    const auto copies = temp_dir() / L"longpath-copy";
    CHECK(!unlocker::file_ops::copy(file, copies));
    CHECK(unlocker::path_util::exists(copies));
    std::filesystem::create_directories(temp_dir() / L"longpath-copies", ec);
    CHECK(!unlocker::file_ops::copy(dir, temp_dir() / L"longpath-copies"));
    CHECK(unlocker::path_util::exists(temp_dir() / L"longpath-copies" / dir.filename() / L"locked.tmp"));
    CHECK(!unlocker::file_ops::copy(copies, dir.parent_path()));  // Ziel ist ein vorhandener langer Ordner
    CHECK(unlocker::path_util::exists(dir.parent_path() / L"longpath-copy"));
    CHECK(!unlocker::file_ops::move(temp_dir() / L"longpath-copies", dir.parent_path()));  // Verschieben in langen Ordner
    CHECK(unlocker::path_util::exists(dir.parent_path() / L"longpath-copies" / dir.filename() / L"locked.tmp"));
    CHECK(!unlocker::path_util::exists(temp_dir() / L"longpath-copies"));
    CHECK(!unlocker::file_ops::remove(copies));

    CHECK(!unlocker::file_ops::remove(temp_dir() / L"longpath"));
    CHECK(!unlocker::path_util::exists(temp_dir() / L"longpath"));
    CHECK(!std::filesystem::exists(temp_dir() / L"longpath", ec));
}

// UNC über die administrative Freigabe (nur erreichbar mit Adminrechten, sonst SKIP).
void test_unc() {
    const auto local = temp_dir() / L"unc-test";
    std::filesystem::create_directories(local);
    // "C:\pfad" -> "\\localhost\C$\pfad"
    const std::wstring unc = L"\\\\localhost\\" + std::wstring(1, local.wstring()[0]) + L"$" + local.wstring().substr(2);
    std::error_code ec;
    if (!std::filesystem::exists(unc, ec)) {
        std::printf("SKIP UNC (\\\\localhost\\C$ nicht erreichbar)\n");
        return;
    }
    CHECK(unlocker::path_util::to_extended(unc).starts_with(L"\\\\?\\UNC\\localhost\\"));
    const auto normalized = unlocker::path_util::normalize(unc);
    CHECK(normalized.starts_with(L"\\\\localhost\\"));
    CHECK(unlocker::path_util::is_same_or_inside(normalized + L"\\x.txt", normalized));
    // Loopback-UNC wird auf den lokalen Pfad übersetzt
    CHECK(unlocker::path_util::equals_ci(unlocker::path_util::localize(unc).wstring(), local.wstring()));
    CHECK(unlocker::path_util::localize(L"\\\\fileserver.example\\share\\x").wstring() == L"\\\\fileserver.example\\share\\x");

    // Lokal gesperrte Datei über den UNC-Pfad scannen
    DWORD pid = 0;
    auto holder = spawn_holder(local / L"locked.tmp", pid);
    unlocker::ScanResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(local / L"locked.tmp", ec)) {
            result = unlocker::scan(std::filesystem::path(unc) / L"locked.tmp");
            if (!result.lockers.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::printf("UNC-Scan: %zu Sperre(n), Ziel %ls\n", result.lockers.size(), result.target.c_str());
    const bool found = std::any_of(result.lockers.begin(), result.lockers.end(),
                                   [&](const unlocker::Locker& l) { return l.pid == pid; });
    CHECK(found);
    CHECK(unlocker::path_util::equals_ci(result.target, unlocker::path_util::normalize(local / L"locked.tmp")));
    TerminateProcess(holder.get(), 0);
    WaitForSingleObject(holder.get(), 5000);
}

// Über die Freigabe geöffnet: Ein Kindprozess öffnet eine lokale Datei über \\localhost\<Laufwerk>$\... exklusiv. Die
// lokale Datei hält dann der SMB-Server (Kernel, PID 4) – früher meldete der Scan "keine Sperren". Jetzt: RemoteOpen
// (NetFileEnum), unlock() schließt sie per NetFileClose. Braucht Adminrechte und eine erreichbare Admin-Freigabe.
void test_remote_open() {
    if (!unlocker::process::is_elevated()) {
        std::printf("SKIP Freigabe-Sperre (nicht erhöht)\n");
        return;
    }
    const auto dir = temp_dir() / L"remote-open";
    std::filesystem::create_directories(dir);
    const auto file = dir / L"remote-locked.tmp";
    {
        FILE* f = nullptr;
        _wfopen_s(&f, file.c_str(), L"w");
        if (f) std::fclose(f);
    }
    const std::wstring local = unlocker::path_util::normalize(file);
    const std::wstring unc = L"\\\\localhost\\" + local.substr(0, 1) + L"$" + local.substr(2);
    if (!unlocker::path_util::exists(unc)) {
        std::printf("SKIP Freigabe-Sperre (%ls nicht erreichbar)\n", unc.c_str());
        return;
    }
    DWORD pid = 0;
    auto holder = spawn_holder(unc, pid);
    CHECK(static_cast<bool>(holder));
    if (!holder) return;

    // Über den 8.3-Namen scannen, falls das Volume einen erzeugt (CI-Runner, kern-8dot3): NetFileEnum meldet die
    // Langform – derselbe Zugriff darf nicht zusätzlich als SystemHeld auftauchen.
    std::filesystem::path scan_path = file;
    wchar_t short_path[MAX_PATH]{};
    if (GetShortPathNameW(file.c_str(), short_path, MAX_PATH) > 0 && !unlocker::path_util::equals_ci(short_path, local)) {
        scan_path = short_path;
        std::printf("Freigabe-Sperre: Scan über Kurznamen %ls\n", short_path);
    }
    unlocker::ScanResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        result = unlocker::scan(scan_path, unlocker::ScanOptions{.use_restart_manager = false});
        if (!result.lockers.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(result.lockers.size() == 1);
    if (result.lockers.size() != 1) {
        for (const auto& l : result.lockers) {
            std::printf("  Sperre: pid %lu, Art %ls, %ls\n", l.pid, unlocker::to_string(l.kind), l.locked_path.c_str());
        }
    }
    if (!result.lockers.empty()) {
        const auto& l = result.lockers.front();
        CHECK(l.kind == unlocker::LockKind::RemoteOpen);
        CHECK(l.pid == 4);
        CHECK(!l.is_critical);  // nicht der System-Prozess an sich – darf geschlossen werden
        CHECK(!l.remote_user.empty());
        // Client nur bei eindeutiger Sitzung – hier nicht erzwungen (fremde Sitzungen desselben Benutzers möglich)
        std::printf("Freigabe-Sperre: Benutzer %ls, Client %ls\n", l.remote_user.c_str(),
                    l.remote_client.empty() ? L"(unbekannt)" : l.remote_client.c_str());
        CHECK(unlocker::path_util::equals_ci(l.locked_path, local));
        CHECK(!unlocker::terminates_process(l, unlocker::UnlockMode::Auto));
        CHECK(!unlocker::terminates_process(l, unlocker::UnlockMode::TerminateProcesses));  // PID 4 nie beenden

        // Der Windows-SMB-Client kann das Schließen per Loopback überbrücken (Datei bleibt gesperrt). Verlangt wird
        // Konsistenz: Erfolg nur, wenn die Datei wirklich frei ist – sonst Error::still_locked, kein Scheinerfolg.
        const auto report = unlocker::unlock(result.lockers, unlocker::UnlockMode::Auto);
        CHECK(report.processes_terminated == 0);
        unlocker::WinHandle probe(CreateFileW(unlocker::path_util::to_extended(file).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (report.ok()) {
            CHECK(report.handles_closed == 1);
            CHECK(static_cast<bool>(probe));
            std::printf("Freigabe-Sperre: geschlossen, Datei frei\n");
        } else {
            CHECK(report.failures.size() == 1 && report.failures.front().error == make_error_code(unlocker::Error::still_locked));
            CHECK(!probe);
            std::printf("Freigabe-Sperre: Server-Zugriff geschlossen, Client hält die Datei weiter (still_locked)\n");
        }
    }
    TerminateProcess(holder.get(), 0);
    WaitForSingleObject(holder.get(), 5000);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Gesperrt, aber kein (fremder) Prozess meldet die Datei: Hier hält der Testprozess sie selbst – den blendet der Scanner
// aus. Früher hieß das "keine Sperren"; jetzt erkennt die Exklusiv-Prüfung die Sperre (Unattributed, nur Neustart).
// Eine freie Datei bleibt frei, ein Ordner mit gesperrter Datei meldet die Datei.
void test_unattributed() {
    const auto dir = temp_dir() / L"unattributed";
    std::filesystem::create_directories(dir);
    const auto file = dir / L"held.tmp";
    const auto free_file = dir / L"free.tmp";
    for (const auto& p : {file, free_file}) {
        FILE* f = nullptr;
        _wfopen_s(&f, p.c_str(), L"w");
        if (f) std::fclose(f);
    }
    const unlocker::ScanOptions options{.use_restart_manager = false};
    CHECK(unlocker::scan(free_file, options).lockers.empty());
    {
        unlocker::WinHandle held(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                             FILE_ATTRIBUTE_NORMAL, nullptr));
        CHECK(static_cast<bool>(held));

        const auto result = unlocker::scan(file, options);
        CHECK(result.lockers.size() == 1);
        if (!result.lockers.empty()) {
            const auto& l = result.lockers.front();
            CHECK(l.kind == unlocker::LockKind::Unattributed);
            CHECK(l.pid == 0);
            CHECK(!l.is_critical);
            CHECK(l.process_name.empty());
            CHECK(unlocker::path_util::equals_ci(l.locked_path, unlocker::path_util::normalize(file)));
            CHECK(!unlocker::terminates_process(l, unlocker::UnlockMode::TerminateProcesses));

            const auto report = unlocker::unlock(result.lockers, unlocker::UnlockMode::TerminateProcesses);
            CHECK(report.processes_terminated == 0 && report.handles_closed == 0);
            CHECK(report.failures.size() == 1 &&
                  report.failures.front().error == make_error_code(unlocker::Error::no_process));
        }
        CHECK(unlocker::scan(free_file, options).lockers.empty());  // Nachbar bleibt frei

        const auto folder = unlocker::scan(dir, options);
        CHECK(folder.lockers.size() == 1 && folder.lockers.front().kind == unlocker::LockKind::Unattributed &&
              unlocker::path_util::equals_ci(folder.lockers.front().locked_path, unlocker::path_util::normalize(file)));
    }
    CHECK(unlocker::scan(file, options).lockers.empty());  // Handle zu → frei
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void test_remove() {
    const auto dir = temp_dir() / L"tree";
    std::filesystem::create_directories(dir / L"sub");
    {
        FILE* f = nullptr;
        _wfopen_s(&f, (dir / L"sub" / L"ro.txt").c_str(), L"w");
        if (f) std::fclose(f);
    }
    SetFileAttributesW((dir / L"sub" / L"ro.txt").c_str(), FILE_ATTRIBUTE_READONLY);

    CHECK(!unlocker::file_ops::remove(dir));
    std::error_code ec;
    CHECK(!std::filesystem::exists(dir, ec));
}

// Löschen eines Dokuments nimmt verwaiste Office-Sperrdateien mit – aber nur passende, kleine, nicht gehaltene.
void test_remove_lock_files() {
    const auto dir = temp_dir() / L"lockfiles";
    std::filesystem::create_directories(dir);
    const auto make = [&](const wchar_t* name, std::size_t size = 16) {
        FILE* f = nullptr;
        _wfopen_s(&f, (dir / name).c_str(), L"wb");
        if (!f) return;
        const std::string data(size, 'x');
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    };
    const auto exists = [&](const wchar_t* name) { return std::filesystem::exists(dir / name); };

    make(L"Quartalsbericht.docx");
    make(L".~lock.Quartalsbericht.docx#");   // LibreOffice
    make(L"~$artalsbericht.docx");           // Word, Name vorne um zwei Zeichen gekürzt
    make(L"~$Quartalsbericht.docx", 10000);  // zu groß für eine Sperrdatei → bleibt
    CHECK(!unlocker::file_ops::remove(dir / L"Quartalsbericht.docx"));
    CHECK(!exists(L"Quartalsbericht.docx"));
    CHECK(!exists(L".~lock.Quartalsbericht.docx#"));
    CHECK(!exists(L"~$artalsbericht.docx"));
    CHECK(exists(L"~$Quartalsbericht.docx"));

    // Gekürzte Form passt auch zu einem anderen vorhandenen Dokument → dessen Sperrdatei bleibt
    make(L"Xbericht.xlsx");
    make(L"bericht.xlsx");
    make(L"~$bericht.xlsx");
    CHECK(!unlocker::file_ops::remove(dir / L"Xbericht.xlsx"));
    CHECK(exists(L"~$bericht.xlsx"));

    // Sperrdatei noch vom Programm gehalten → bleibt liegen, das Dokument ist trotzdem gelöscht
    make(L"notiz.odt");
    make(L".~lock.notiz.odt#");
    {
        unlocker::WinHandle held(CreateFileW((dir / L".~lock.notiz.odt#").c_str(), GENERIC_READ, 0, nullptr,
                                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        CHECK(static_cast<bool>(held));
        CHECK(!unlocker::file_ops::remove(dir / L"notiz.odt"));
        CHECK(!exists(L"notiz.odt"));
        CHECK(exists(L".~lock.notiz.odt#"));
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Nur erhöht: MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT) schreibt nach
// HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\PendingFileRenameOperations (REG_MULTI_SZ, Paare
// "\??\Quelle" / "" für Löschen). Eintrag wird danach wieder entfernt.
// PendingFileRenameOperations als Liste von Paaren (Quelle, Ziel; Ziel "" = löschen). REG_MULTI_SZ: jeder String endet
// mit \0, die Liste mit einem weiteren \0 – ein leeres Ziel ist also ein echter (leerer) Eintrag, kein Listenende.
std::vector<std::wstring> read_pending(HKEY key) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_MULTI_SZ) {
        return {};
    }
    std::wstring data(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, &type, reinterpret_cast<LPBYTE>(data.data()),
                         &size) != ERROR_SUCCESS) {
        return {};
    }
    data.resize(size / sizeof(wchar_t));
    // Listenende und Ende des letzten Strings abschneiden, der Rest trennt sich sauber an \0
    for (int i = 0; i < 2 && !data.empty() && data.back() == L'\0'; ++i) data.pop_back();
    std::vector<std::wstring> entries;
    if (data.empty()) return entries;
    for (std::size_t start = 0;;) {
        const std::size_t end = data.find(L'\0', start);
        entries.push_back(data.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return entries;
}

void write_pending(HKEY key, const std::vector<std::wstring>& entries) {
    if (entries.empty()) {
        RegDeleteValueW(key, L"PendingFileRenameOperations");
        return;
    }
    std::wstring data;
    for (const auto& e : entries) data += e + L'\0';
    data += L'\0';
    RegSetValueExW(key, L"PendingFileRenameOperations", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(data.data()),
                   static_cast<DWORD>(data.size() * sizeof(wchar_t)));
}

// Index der Quelle des Paars für path, sonst npos. Windows 11 24H2 schreibt z. B. "*1\??\C:\pfad" (Präfix mit Flags),
// ältere Versionen "\??\C:\pfad". Der Pfad kann in Langform stehen, auch wenn %TEMP% ein 8.3-Name ist – daher beide
// Seiten normalisieren (die Dateien existieren bis zum Neustart noch).
std::size_t find_pending(const std::vector<std::wstring>& entries, const std::filesystem::path& path) {
    const std::wstring expected = unlocker::path_util::normalize(path);
    for (std::size_t i = 0; i + 1 < entries.size(); i += 2) {
        const auto pos = entries[i].find(L"\\??\\");
        if (pos != std::wstring::npos &&
            unlocker::path_util::equals_ci(unlocker::path_util::normalize(entries[i].substr(pos + 4)), expected)) {
            return i;
        }
    }
    return std::wstring::npos;
}

void test_remove_on_reboot() {
    if (!unlocker::process::is_elevated()) {
        std::printf("SKIP remove_on_reboot (nicht erhöht)\n");
        return;
    }
    const auto dir = temp_dir() / L"reboot";
    std::filesystem::create_directories(dir);
    const auto file = dir / L"later.tmp";
    {
        FILE* f = nullptr;
        _wfopen_s(&f, file.c_str(), L"w");
        if (f) std::fclose(f);
    }
    CHECK(!unlocker::file_ops::remove_on_reboot(file));
    // Fremder Eintrag HINTER unserem: Das Aufräumen darf ihn nicht beschädigen. Früher fiel beim Einlesen das leere
    // Ziel des letzten Paars weg – ein fremder Löschauftrag am Ende wurde ohne Ziel zurückgeschrieben.
    const auto sentinel = dir / L"fremder-eintrag.tmp";
    {
        FILE* f = nullptr;
        _wfopen_s(&f, sentinel.c_str(), L"w");
        if (f) std::fclose(f);
    }
    CHECK(MoveFileExW(unlocker::path_util::to_extended(sentinel).c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT));

    HKEY key = nullptr;
    CHECK(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0,
                        KEY_READ | KEY_SET_VALUE, &key) == ERROR_SUCCESS);
    if (!key) return;

    auto entries = read_pending(key);
    CHECK(entries.size() % 2 == 0);
    const auto ours = find_pending(entries, file);
    CHECK(ours != std::wstring::npos);
    if (ours != std::wstring::npos) entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(ours),
                                                  entries.begin() + static_cast<std::ptrdiff_t>(ours) + 2);
    write_pending(key, entries);

    auto after = read_pending(key);
    CHECK(after.size() % 2 == 0);
    CHECK(find_pending(after, file) == std::wstring::npos);
    const auto other = find_pending(after, sentinel);
    CHECK(other != std::wstring::npos && after[other + 1].empty());  // fremder Löschauftrag vollständig erhalten
    if (other != std::wstring::npos) {
        after.erase(after.begin() + static_cast<std::ptrdiff_t>(other), after.begin() + static_cast<std::ptrdiff_t>(other) + 2);
        write_pending(key, after);
    }
    RegCloseKey(key);
    std::printf("remove_on_reboot: Registry-Eintrag gefunden und wieder entfernt\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wcscmp(argv[1], L"--hold") == 0) return hold_file(argv[2]);
    if (argc == 4 && std::wcscmp(argv[1], L"--hold-report") == 0) return hold_file_report(argv[2], argv[3]);

    test_path_util();
    test_scan_and_unlock();
    test_unlock_unqueryable();
    test_driver_fallback();
    test_driver_end_to_end();
    test_remove();
    test_remove_lock_files();
    test_working_directory();
    test_short_names();
    test_junction();
    test_long_paths();
    test_unc();
    test_remote_open();
    test_unattributed();
    test_remove_on_reboot();

    std::error_code ec;
    std::filesystem::remove_all(temp_dir(), ec);

    if (g_failures == 0) std::printf("OK\n");
    else std::printf("%d Fehler\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
