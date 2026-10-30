#include "strings.h"
#include "unlocker/driver_client.h"
#include "unlocker/errors.h"
#include "unlocker/file_ops.h"
#include "unlocker/path_util.h"
#include "unlocker/process.h"
#include "unlocker/scanner.h"
#include "unlocker/unlock.h"

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum ExitCode : int { Clean = 0, LockersFound = 1, Failure = 2, AccessDenied = 3 };

enum class FileAction { None, Delete, Rename, Move, Copy };
enum class Format { Text, Table, Csv, Json };

struct Args {
    std::vector<std::filesystem::path> targets;
    Format format = Format::Text;
    bool json = false;                     // format == Json (Kurzform für die vielen Abfragen)
    std::chrono::milliseconds timeout{0};  // --timeout: Erreichbarkeit je Ziel und Snapshot-Budget; 0 = Standard
    bool deep = true;
    bool fast = false;
    bool no_modules = false;
    bool do_unlock = false;
    bool do_kill = false;
    bool reboot = false;
    bool yes = false;
    bool allow_critical = false;
    bool use_driver = false;  // v2: Kernel-Treiber als Fallback, wenn User-Mode nicht schafft
    std::wstring driver_path;  // Pfad zur unlocker_driver.sys (leer = neben der EXE)
    bool quiet = false;
    bool verbose = false;
    bool help = false;
    FileAction action = FileAction::None;
    std::wstring action_arg;  // neuer Name bzw. Zielordner
};

// Sprache wie die GUI: Windows-Anzeigesprache Deutsch → de, sonst en.
const wchar_t* T(const wchar_t* de, const wchar_t* en) { return unlocker::i18n::is_german() ? de : en; }

// Formatstring zur Laufzeit (std::format verlangt Konstanten); benannte Parameter liefern die nötigen lvalues.
template <typename... Args>
std::wstring fmt(const wchar_t* format, Args&&... args) {
    return std::vformat(format, std::make_wformat_args(args...));
}

bool g_quiet = false;
bool g_reports_to_stderr = false;  // --format csv: stdout bleibt reine Tabelle, Berichte und Hinweise gehen nach stderr

void print(std::wstring_view text) {
    if (!g_quiet) std::fputws(text.data(), stdout);
}
void print_err(std::wstring_view text) { std::fputws(text.data(), stderr); }
void report(std::wstring_view text) {
    if (g_reports_to_stderr) print_err(text);
    else print(text);
}

void usage() {
    print(T(L"unlockerc " UNLOCKER_VERSION_WSTRING L" – gesperrte Dateien und Ordner freigeben\n\n"
            L"Verwendung: unlockerc <pfad...> [Optionen]\n\n"
            L"Anzeige\n"
            L"  --list            Sperren anzeigen (Standard)\n"
            L"  --json            Ausgabe als JSON (Scan und Aktionsergebnisse)\n"
            L"  --format <f>      text (Standard), table (Spaltenköpfe), csv (eine Zeile je Sperre, Berichte auf stderr), json\n"
            L"  --quiet           keine Ausgabe, nur Exit-Code (Aktionen brauchen dann --yes)\n"
            L"  --verbose         Stufen-Zeiten und Fortschritt auf stderr\n"
            L"Scan\n"
            L"  --no-deep         nur Prozesse ermitteln, Handles nicht auflösen (schneller)\n"
            L"  --fast            ohne Restart Manager (keine App-Namen)\n"
            L"  --no-modules      geladene EXE/DLLs nicht prüfen\n"
            L"  --timeout <ms>    Zeitlimit für die Erreichbarkeit je Ziel (Netzpfade, sonst ~20 s)\n"
            L"                    und für die Handle-Auflösung im Snapshot-Modus (Standard 1500)\n"
            L"Entsperren\n"
            L"  --unlock          Handles schließen, Prozesse nur bei Modul-Sperren beenden\n"
            L"  --kill            alle sperrenden Prozesse beenden\n"
            L"  --allow-critical  auch Systemprozesse anfassen (gefährlich)\n"
            L"  --driver [x.sys]  Kernel-Treiber als Fallback (v2), falls User-Mode nicht reicht\n"
            L"                    (braucht eine signierte unlocker_driver.sys; Admin)\n"
            L"Datei-Aktionen (nach dem Entsperren; --unlock ist dabei implizit)\n"
            L"  --delete          löschen\n"
            L"  --reboot          bei Fehlschlag beim nächsten Neustart löschen (Admin)\n"
            L"  --rename <name>   umbenennen (nur ein Ziel)\n"
            L"  --move <ordner>   in den Ordner verschieben\n"
            L"  --copy <ordner>   in den Ordner kopieren (ohne Entsperren)\n"
            L"  --yes             keine Rückfragen\n\n"
            L"Exit-Codes: 0 = frei/erfolgreich, 1 = Sperren gefunden (nur --list), 2 = Fehler, 3 = Zugriff verweigert\n",
            L"unlockerc " UNLOCKER_VERSION_WSTRING L" – release locked files and folders\n\n"
            L"Usage: unlockerc <path...> [options]\n\n"
            L"Output\n"
            L"  --list            show locks (default)\n"
            L"  --json            JSON output (scan and action results)\n"
            L"  --format <f>      text (default), table (column headers), csv (one row per lock, reports on stderr), json\n"
            L"  --quiet           no output, exit code only (actions then require --yes)\n"
            L"  --verbose         stage timings and progress on stderr\n"
            L"Scan\n"
            L"  --no-deep         only find processes, do not resolve handles (faster)\n"
            L"  --fast            skip Restart Manager (no app names)\n"
            L"  --no-modules      do not check loaded EXE/DLLs\n"
            L"  --timeout <ms>    limit for reachability of each target (network paths, otherwise ~20 s)\n"
            L"                    and for handle resolution in snapshot mode (default 1500)\n"
            L"Unlock\n"
            L"  --unlock          close handles, end processes only for module locks\n"
            L"  --kill            end all locking processes\n"
            L"  --allow-critical  touch system processes too (dangerous)\n"
            L"  --driver [x.sys]  kernel driver as fallback (v2) if user-mode cannot close it\n"
            L"                    (needs a signed unlocker_driver.sys; admin)\n"
            L"File actions (after unlocking; --unlock is implied)\n"
            L"  --delete          delete\n"
            L"  --reboot          on failure delete on next restart (admin)\n"
            L"  --rename <name>   rename (single target only)\n"
            L"  --move <folder>   move into folder\n"
            L"  --copy <folder>   copy into folder (no unlocking)\n"
            L"  --yes             no prompts\n\n"
            L"Exit codes: 0 = free/success, 1 = locks found (--list only), 2 = error, 3 = access denied\n"));
}

bool parse(int argc, wchar_t** argv, Args& args) {
    const auto need_value = [&](int& i, const wchar_t* option) -> std::optional<std::wstring> {
        if (i + 1 >= argc) {
            print_err(fmt(T(L"{} braucht einen Wert\n", L"{} requires a value\n"), option));
            return std::nullopt;
        }
        return std::wstring(argv[++i]);
    };
    const auto set_action = [&](FileAction a, std::wstring value) {
        if (args.action != FileAction::None) {
            print_err(T(L"Nur eine Datei-Aktion (--delete/--rename/--move/--copy) gleichzeitig\n",
                        L"Only one file action (--delete/--rename/--move/--copy) at a time\n"));
            return false;
        }
        args.action = a;
        args.action_arg = std::move(value);
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a = argv[i];
        if (a == L"--list") continue;
        else if (a == L"--json") args.format = Format::Json;
        else if (a == L"--format") {
            auto v = need_value(i, L"--format");
            if (!v) return false;
            if (*v == L"text") args.format = Format::Text;
            else if (*v == L"table") args.format = Format::Table;
            else if (*v == L"csv") args.format = Format::Csv;
            else if (*v == L"json") args.format = Format::Json;
            else {
                print_err(fmt(T(L"--format: unbekanntes Format {} (text|table|csv|json)\n",
                                L"--format: unknown format {} (text|table|csv|json)\n"), *v));
                return false;
            }
        } else if (a == L"--timeout") {
            auto v = need_value(i, L"--timeout");
            if (!v) return false;
            wchar_t* end = nullptr;
            const long ms = std::wcstol(v->c_str(), &end, 10);
            if (v->empty() || *end != L'\0' || ms < 0) {
                print_err(fmt(T(L"--timeout: ungültiger Wert {} (Millisekunden)\n", L"--timeout: invalid value {} (milliseconds)\n"), *v));
                return false;
            }
            args.timeout = std::chrono::milliseconds(ms);
        }
        else if (a == L"--quiet" || a == L"-q") args.quiet = true;
        else if (a == L"--verbose" || a == L"-v") args.verbose = true;
        else if (a == L"--no-deep") args.deep = false;
        else if (a == L"--fast") args.fast = true;
        else if (a == L"--no-modules") args.no_modules = true;
        else if (a == L"--unlock") args.do_unlock = true;
        else if (a == L"--kill") args.do_kill = true;
        else if (a == L"--delete") {
            if (!set_action(FileAction::Delete, L"")) return false;
        } else if (a == L"--rename") {
            auto v = need_value(i, L"--rename");
            if (!v || !set_action(FileAction::Rename, *v)) return false;
        } else if (a == L"--move") {
            auto v = need_value(i, L"--move");
            if (!v || !set_action(FileAction::Move, *v)) return false;
        } else if (a == L"--copy") {
            auto v = need_value(i, L"--copy");
            if (!v || !set_action(FileAction::Copy, *v)) return false;
        } else if (a == L"--reboot") args.reboot = true;
        else if (a == L"--yes" || a == L"-y") args.yes = true;
        else if (a == L"--allow-critical") args.allow_critical = true;
        else if (a == L"--driver") {
            args.use_driver = true;
            // Optionales Argument: --driver [pfad.sys]. Nur ein .sys-Pfad wird als Treiber genommen – sonst wäre
            // "--driver C:\datei.txt" mehrdeutig und das Ziel verschwände als Treiberpfad.
            if (i + 1 < argc &&
                unlocker::path_util::equals_ci(std::filesystem::path(argv[i + 1]).extension().wstring(), L".sys")) {
                args.driver_path = argv[++i];
            }
        } else if (a == L"--help" || a == L"-h" || a == L"/?") args.help = true;
        else if (a.starts_with(L"--")) {
            print_err(fmt(T(L"Unbekannte Option: {}\n", L"Unknown option: {}\n"), a));
            return false;
        } else {
            args.targets.emplace_back(a);
        }
    }
    if (args.action == FileAction::Rename && args.targets.size() != 1) {
        print_err(T(L"--rename erwartet genau ein Ziel\n", L"--rename expects exactly one target\n"));
        return false;
    }
    if (args.action == FileAction::Rename && args.action_arg.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        print_err(T(L"--rename: ungültiger Name\n", L"--rename: invalid name\n"));
        return false;
    }
    args.json = args.format == Format::Json;
    const bool acts = args.do_unlock || args.do_kill || args.action != FileAction::None;
    if (args.quiet && acts && !args.yes) {
        print_err(T(L"--quiet mit Aktionen braucht --yes\n", L"--quiet with actions requires --yes\n"));
        return false;
    }
    return true;
}

std::wstring json_escape(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (const wchar_t c : s) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) out += std::format(L"\\u{:04x}", static_cast<unsigned>(c));
                else out += c;
        }
    }
    return out;
}

std::wstring error_text(const std::error_code& ec) {
    if (ec.category() == unlocker::error_category()) {
        switch (static_cast<unlocker::Error>(ec.value())) {
            case unlocker::Error::critical_process: return T(L"Systemprozess – wird nicht angefasst", L"system process – left untouched");
            case unlocker::Error::protected_handle: return T(L"Handle ist gegen Schließen geschützt", L"handle is protected from closing");
            case unlocker::Error::protected_process: return T(L"geschützter Prozess (PPL)", L"protected process (PPL)");
            case unlocker::Error::not_supported: return T(L"Sperrart nicht lösbar", L"lock type cannot be released");
            case unlocker::Error::system_held:
                return T(L"vom System gehalten – nur beim Neustart löschbar (--delete --reboot)",
                         L"held by the system – can only be deleted on restart (--delete --reboot)");
            case unlocker::Error::still_locked:
                return T(L"Zugriff auf dem Server geschlossen, Datei aber weiter gesperrt – Programm beim Client beenden oder --delete --reboot",
                         L"share access closed on the server, but the file is still locked – end the program on the client or --delete --reboot");
            case unlocker::Error::no_process:
                return T(L"gesperrt, aber kein Prozess gefunden – nur beim Neustart löschbar (--delete --reboot)",
                         L"locked, but no process found – can only be deleted on restart (--delete --reboot)");
        }
    }
    if (ec.category() != std::system_category()) {
        const auto msg = ec.message();
        return std::wstring(msg.begin(), msg.end());
    }
    wchar_t* buffer = nullptr;
    const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, static_cast<DWORD>(ec.value()), 0, reinterpret_cast<LPWSTR>(&buffer), 0,
                                     nullptr);
    std::wstring text = len ? std::wstring(buffer, len) : fmt(T(L"Fehler {}", L"Error {}"), ec.value());
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text;
}

const wchar_t* kind_text(unlocker::LockKind kind) {
    switch (kind) {
        case unlocker::LockKind::FileHandle: return L"Handle";
        case unlocker::LockKind::MappedImage: return T(L"Geladenes Modul", L"Loaded module");
        case unlocker::LockKind::WorkingDirectory: return T(L"Arbeitsverz.", L"Working dir");
        case unlocker::LockKind::RemoteOpen: return T(L"Freigabe", L"Network share");
        case unlocker::LockKind::SystemHeld: return T(L"System (Kernel)", L"System (kernel)");
        case unlocker::LockKind::Unattributed: return T(L"Kein Prozess", L"No process");
        default: return T(L"Prozess", L"Process");
    }
}

const wchar_t* action_name(FileAction a) {
    switch (a) {
        case FileAction::Delete: return L"delete";
        case FileAction::Rename: return L"rename";
        case FileAction::Move: return L"move";
        case FileAction::Copy: return L"copy";
        default: return L"none";
    }
}

// Ergebnis einer Datei-Aktion für Text und JSON.
struct FileOpResult {
    FileAction action = FileAction::None;
    bool ok = false;
    bool scheduled_reboot = false;
    std::wstring new_path;
    std::error_code error;
};

// Hinweise wie „kritisch“, „dienst“, „32-bit“ – mit Klammern für die Textausgabe, als Liste für Tabelle/CSV.
std::wstring locker_notes(const unlocker::Locker& l, bool bracketed) {
    std::vector<std::wstring> notes;
    if (l.is_critical) notes.push_back(T(L"kritisch", L"critical"));
    else if (l.is_sensitive) notes.push_back(T(L"vorsicht", L"caution"));
    if (l.is_service) notes.push_back(T(L"dienst", L"service"));
    if (l.is_protected) notes.push_back(T(L"geschützt", L"protected"));
    if (l.protect_close) notes.push_back(T(L"nicht schließbar", L"not closable"));
    if (l.is_wow64) notes.push_back(L"32-bit");
    if (!l.remote_user.empty()) {
        notes.push_back(l.remote_client.empty()
                            ? fmt(T(L"freigabe: {}", L"share: {}"), l.remote_user)
                            : fmt(T(L"freigabe: {} von {}", L"share: {} from {}"), l.remote_user, l.remote_client));
    }
    if (l.kind == unlocker::LockKind::SystemHeld || l.kind == unlocker::LockKind::Unattributed) {
        notes.push_back(T(L"nur beim neustart löschbar", L"delete on restart only"));
    }
    std::wstring out;
    for (const auto& n : notes) {
        if (bracketed) out += std::format(L" [{}]", n);
        else out += std::format(L"{}{}", out.empty() ? L"" : L", ", n);
    }
    return out;
}

void print_result_notes(const unlocker::ScanResult& r) {
    if (r.snapshot_timeout) report(T(L"  (Zeitbudget erreicht – nicht alle Handles im System konnten geprüft werden)\n", L"  (time budget reached – not all handles in the system could be checked)\n"));
    if (r.has_protected) report(T(L"  (geschützte Prozesse – nicht lösbar, auch nicht als Administrator)\n", L"  (protected processes – cannot be released, not even as administrator)\n"));
    if (r.access_denied) report(T(L"  (einige Prozesse nicht lesbar – als Administrator wiederholen)\n", L"  (some processes not readable – retry as administrator)\n"));
}

void print_result_text(const unlocker::ScanResult& r) {
    print(fmt(L"{}\n", r.target));
    if (r.lockers.empty()) print(T(L"  keine Sperren\n", L"  no locks\n"));
    for (const auto& l : r.lockers) {
        print(std::format(L"  {:<6} {:<28} {:<16} {}{}\n", l.pid, l.process_name.empty() ? L"?" : l.process_name,
                          kind_text(l.kind), l.locked_path, locker_notes(l, true)));
    }
    print_result_notes(r);
}

// Tabelle mit Spaltenköpfen; Breiten aus dem Inhalt, leere Spalten (App, Hinweise) entfallen.
void print_result_table(const unlocker::ScanResult& r) {
    print(fmt(L"{}\n", r.target));
    if (r.lockers.empty()) {
        print(T(L"  keine Sperren\n", L"  no locks\n"));
        print_result_notes(r);
        return;
    }
    constexpr std::size_t kColumns = 6;
    const wchar_t* headers[kColumns] = {L"PID", T(L"Prozess", L"Process"), L"App", T(L"Art", L"Kind"), T(L"Pfad", L"Path"),
                                        T(L"Hinweise", L"Notes")};
    std::vector<std::array<std::wstring, kColumns>> rows;
    for (const auto& l : r.lockers) {
        rows.push_back({std::to_wstring(l.pid), l.process_name.empty() ? L"?" : l.process_name, l.app_name, kind_text(l.kind),
                        l.locked_path, locker_notes(l, false)});
    }
    std::array<std::size_t, kColumns> width{};
    std::array<bool, kColumns> show{};
    for (std::size_t c = 0; c < kColumns; ++c) {
        width[c] = std::wcslen(headers[c]);
        for (const auto& row : rows) {
            width[c] = std::max(width[c], row[c].size());
            if (!row[c].empty()) show[c] = true;
        }
    }
    show[0] = show[1] = show[3] = show[4] = true;
    const auto line = [&](const std::array<std::wstring, kColumns>& cells) {
        std::wstring out = L"  ";
        for (std::size_t c = 0; c < kColumns; ++c) {
            if (!show[c]) continue;
            out += std::format(L"{:<{}}", cells[c], width[c]);
            out += L"  ";
        }
        while (!out.empty() && out.back() == L' ') out.pop_back();
        print(out + L"\n");
    };
    std::array<std::wstring, kColumns> head, rule;
    for (std::size_t c = 0; c < kColumns; ++c) {
        head[c] = headers[c];
        rule[c] = std::wstring(width[c], L'-');
    }
    line(head);
    line(rule);
    for (const auto& row : rows) line(row);
    print_result_notes(r);
}

// CSV nach RFC 4180: Felder mit Komma, Anführungszeichen oder Zeilenumbruch werden in Anführungszeichen gesetzt.
// Zeilenende nur \n – der Textmodus von stdout macht daraus \r\n.
std::wstring csv_field(std::wstring_view s) {
    if (s.find_first_of(L",\"\r\n") == std::wstring_view::npos) return std::wstring(s);
    std::wstring out = L"\"";
    for (const wchar_t c : s) {
        if (c == L'"') out += L"\"\"";
        else out += c;
    }
    return out + L"\"";
}

void print_csv_header() {
    print(L"target,pid,process,app,kind,handle,path,service,critical,sensitive,protected,protectClose,wow64,remoteUser,remoteClient\n");
}

void print_result_csv(const unlocker::ScanResult& r) {
    for (const auto& l : r.lockers) {
        print(std::format(L"{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n", csv_field(r.target), l.pid,
                          csv_field(l.process_name), csv_field(l.app_name), unlocker::to_string(l.kind), l.handle,
                          csv_field(l.locked_path), l.is_service, l.is_critical, l.is_sensitive, l.is_protected,
                          l.protect_close, l.is_wow64, csv_field(l.remote_user), csv_field(l.remote_client)));
    }
    print_result_notes(r);
}

void print_result(const unlocker::ScanResult& r, Format format) {
    switch (format) {
        case Format::Table: print_result_table(r); break;
        case Format::Csv: print_result_csv(r); break;
        default: print_result_text(r); break;
    }
}

void print_unlock_text(const unlocker::UnlockReport& unlock_report) {
    report(fmt(T(L"  {} Handle(s) geschlossen, {} Prozess(e) beendet\n", L"  {} handle(s) closed, {} process(es) ended\n"),
               unlock_report.handles_closed, unlock_report.processes_terminated));
    for (const auto& f : unlock_report.failures) {
        if (f.locker.kind == unlocker::LockKind::Unattributed) {  // kein Prozess – Pfad statt PID nennen
            report(fmt(T(L"  Fehler bei {}: {}\n", L"  error for {}: {}\n"), f.locker.locked_path, error_text(f.error)));
            continue;
        }
        if (!f.locker.remote_user.empty()) {  // Freigabe-Zugriff: Benutzer/Client statt "PID 4 (System)"
            report(fmt(T(L"  Fehler bei Freigabe-Zugriff ({}): {}\n", L"  error for share access ({}): {}\n"),
                       f.locker.remote_client.empty()
                           ? f.locker.remote_user
                           : fmt(T(L"{} von {}", L"{} from {}"), f.locker.remote_user, f.locker.remote_client),
                       error_text(f.error)));
            continue;
        }
        report(fmt(T(L"  Fehler bei PID {} ({}): {}\n", L"  error for PID {} ({}): {}\n"), f.locker.pid, f.locker.process_name,
                   error_text(f.error)));
    }
}

void print_fileop_text(const FileOpResult& op) {
    if (op.ok) {
        switch (op.action) {
            case FileAction::Delete: report(T(L"  gelöscht\n", L"  deleted\n")); break;
            case FileAction::Rename: report(fmt(T(L"  umbenannt in {}\n", L"  renamed to {}\n"), op.new_path)); break;
            case FileAction::Move: report(fmt(T(L"  verschoben nach {}\n", L"  moved to {}\n"), op.new_path)); break;
            case FileAction::Copy: report(fmt(T(L"  kopiert nach {}\n", L"  copied to {}\n"), op.new_path)); break;
            default: break;
        }
        return;
    }
    if (op.scheduled_reboot) {
        report(T(L"  wird beim nächsten Neustart gelöscht\n", L"  will be deleted on next restart\n"));
        return;
    }
    const wchar_t* verb = op.action == FileAction::Delete ? T(L"Löschen", L"delete")
                          : op.action == FileAction::Rename ? T(L"Umbenennen", L"rename")
                          : op.action == FileAction::Move ? T(L"Verschieben", L"move")
                                                          : T(L"Kopieren", L"copy");
    report(fmt(T(L"  {} fehlgeschlagen: {}\n", L"  {} failed: {}\n"), verb, error_text(op.error)));
}

std::wstring json_lockers(const unlocker::ScanResult& r) {
    std::wstring out;
    for (std::size_t i = 0; i < r.lockers.size(); ++i) {
        const auto& l = r.lockers[i];
        out += std::format(L"      {{\"pid\": {}, \"process\": \"{}\", \"image\": \"{}\", \"app\": \"{}\", "
                           L"\"kind\": \"{}\", \"handle\": {}, \"path\": \"{}\", \"service\": {}, "
                           L"\"critical\": {}, \"sensitive\": {}, \"protected\": {}, \"protectClose\": {}, \"wow64\": {}, "
                           L"\"remoteUser\": \"{}\", \"remoteClient\": \"{}\"}}{}\n",
                           l.pid, json_escape(l.process_name), json_escape(l.image_path), json_escape(l.app_name),
                           unlocker::to_string(l.kind), l.handle, json_escape(l.locked_path), l.is_service, l.is_critical,
                           l.is_sensitive, l.is_protected, l.protect_close, l.is_wow64, json_escape(l.remote_user),
                           json_escape(l.remote_client),
                           i + 1 < r.lockers.size() ? L"," : L"");
    }
    return out;
}

std::wstring json_target(const unlocker::ScanResult& r, const std::optional<unlocker::UnlockReport>& unlock_report,
                         bool killed, const std::optional<FileOpResult>& op, bool last) {
    std::wstring out = std::format(
        L"  {{\n    \"target\": \"{}\",\n    \"isDirectory\": {},\n    \"snapshotMode\": {},\n    \"snapshotTimeout\": {},\n"
        L"    \"accessDenied\": {},\n    \"hasProtected\": {},\n    \"lockers\": [\n{}    ]",
        json_escape(r.target), r.is_directory, r.snapshot_mode, r.snapshot_timeout, r.access_denied, r.has_protected,
        json_lockers(r));

    if (unlock_report) {
        out += std::format(L",\n    \"unlock\": {{ \"mode\": \"{}\", \"handlesClosed\": {}, \"processesTerminated\": {}, \"failures\": [",
                           killed ? L"kill" : L"auto", unlock_report->handles_closed, unlock_report->processes_terminated);
        for (std::size_t i = 0; i < unlock_report->failures.size(); ++i) {
            const auto& f = unlock_report->failures[i];
            out += std::format(L"{}{{\"pid\": {}, \"process\": \"{}\", \"error\": \"{}\"}}", i ? L", " : L"", f.locker.pid,
                               json_escape(f.locker.process_name), json_escape(error_text(f.error)));
        }
        out += L"] }";
    }
    if (op) {
        out += std::format(L",\n    \"fileOp\": {{ \"type\": \"{}\", \"ok\": {}, \"scheduledReboot\": {}, \"path\": \"{}\", \"error\": \"{}\" }}",
                           action_name(op->action), op->ok, op->scheduled_reboot, json_escape(op->new_path),
                           op->ok ? L"" : json_escape(error_text(op->error)));
    }
    out += std::format(L"\n  }}{}\n", last ? L"" : L",");
    return out;
}

bool ask(const std::wstring& question) {
    print(question + T(L" [j/N] ", L" [y/N] "));
    std::fflush(stdout);
    wchar_t buffer[16]{};
    if (!std::fgetws(buffer, 16, stdin)) return false;
    return buffer[0] == L'j' || buffer[0] == L'J' || buffer[0] == L'y' || buffer[0] == L'Y';
}

FileOpResult run_file_action(const std::filesystem::path& target, const Args& args) {
    FileOpResult op;
    op.action = args.action;
    switch (args.action) {
        case FileAction::Delete:
            op.error = unlocker::file_ops::remove(target);
            op.ok = !op.error;
            if (!op.ok && args.reboot) {
                if (!unlocker::file_ops::remove_on_reboot(target)) op.scheduled_reboot = true;
            }
            break;
        case FileAction::Rename:
            op.error = unlocker::file_ops::rename(target, args.action_arg);
            op.ok = !op.error;
            op.new_path = (target.parent_path() / args.action_arg).wstring();
            break;
        case FileAction::Move:
            op.error = unlocker::file_ops::move(target, args.action_arg);
            op.ok = !op.error;
            op.new_path = (std::filesystem::path(args.action_arg) / target.filename()).wstring();
            break;
        case FileAction::Copy:
            op.error = unlocker::file_ops::copy(target, args.action_arg);
            op.ok = !op.error;
            op.new_path = (std::filesystem::path(args.action_arg) / target.filename()).wstring();
            break;
        default: break;
    }
    return op;
}

// Fortschritt auf stderr: eine Zeile pro Stufe, bei bekannter Gesamtzahl wird sie in-place aktualisiert.
void report_progress(const unlocker::ScanProgress& p) {
    static std::wstring last_stage;
    static bool line_open = false;
    if (last_stage != p.stage) {
        if (line_open) print_err(L"\n");
        last_stage = p.stage;
        line_open = false;
    }
    if (p.total > 0) {
        print_err(std::format(L"\r  [{}] {}/{}", p.stage, p.done, p.total));
        line_open = p.done < p.total;
        if (!line_open) print_err(L"\n");
    } else {
        print_err(std::format(L"  [{}]\n", p.stage));
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);

    Args args;
    if (!parse(argc, argv, args)) return Failure;
    g_quiet = args.quiet && !args.json;
    g_reports_to_stderr = args.format == Format::Csv;
    if (args.help || args.targets.empty()) {
        usage();
        return args.help ? Clean : Failure;
    }

    unlocker::process::enable_debug_privilege();
    const bool elevated = unlocker::process::is_elevated();

    unlocker::ScanOptions options;
    options.deep = args.deep;
    options.use_restart_manager = !args.fast;
    options.include_modules = !args.no_modules;
    options.trace = args.verbose;
    if (args.verbose) options.progress = report_progress;
    if (args.timeout.count() > 0) {
        options.target_timeout = args.timeout;
        options.snapshot_timeout = args.timeout;
    }

    // Datei-Aktionen außer Kopieren setzen Entsperren voraus
    const bool implicit_unlock = args.action == FileAction::Delete || args.action == FileAction::Rename || args.action == FileAction::Move;
    const bool unlock_requested = args.do_unlock || args.do_kill || implicit_unlock;
    const bool acts = unlock_requested || args.action != FileAction::None;

    bool any_lockers = false;
    bool any_denied = false;
    bool any_failure = false;

    if (args.json) print(L"[\n");
    if (args.format == Format::Csv) print_csv_header();

    for (std::size_t t = 0; t < args.targets.size(); ++t) {
        const auto& target = args.targets[t];
        const bool last = t + 1 == args.targets.size();
        // Erreichbarkeit vorab mit Zeitlimit prüfen – ein toter Netzpfad blockiert sonst jede Stufe des Scans.
        const auto reach = unlocker::path_util::probe(unlocker::path_util::localize(target), args.timeout);
        if (reach != unlocker::path_util::Reachability::Exists) {
            const bool timed_out = reach == unlocker::path_util::Reachability::Timeout;
            if (args.json) {
                print(std::format(L"  {{ \"target\": \"{}\", \"missing\": true, \"timeout\": {} }}{}\n", json_escape(target.wstring()),
                                  timed_out, last ? L"" : L","));
            } else if (timed_out) {
                report(fmt(T(L"{}: nicht erreichbar (Zeitlimit {} ms)\n", L"{}: not reachable (timeout {} ms)\n"), target.wstring(),
                           args.timeout.count()));
            } else {
                report(fmt(T(L"{}: nicht gefunden\n", L"{}: not found\n"), target.wstring()));
            }
            any_failure = true;
            continue;
        }

        auto result = unlocker::scan(target, options);
        any_lockers |= !result.lockers.empty();
        any_denied |= result.access_denied;

        if (!args.json) print_result(result, args.format);

        std::optional<unlocker::UnlockReport> unlock_report;
        std::optional<FileOpResult> op;

        if (acts) {
            bool proceed = args.yes || args.json || args.format == Format::Csv || g_quiet;
            if (!proceed) {
                const wchar_t* question = args.action == FileAction::Delete ? T(L"Sperren lösen und löschen?", L"Release locks and delete?")
                                          : args.action == FileAction::Rename ? T(L"Sperren lösen und umbenennen?", L"Release locks and rename?")
                                          : args.action == FileAction::Move ? T(L"Sperren lösen und verschieben?", L"Release locks and move?")
                                          : args.action == FileAction::Copy ? T(L"Kopieren?", L"Copy?")
                                          : args.do_kill ? T(L"Prozesse beenden?", L"End processes?")
                                                         : T(L"Sperren lösen?", L"Release locks?");
                proceed = ask(question);
            }
            if (proceed) {
                if (unlock_requested && !result.lockers.empty()) {
                    const auto mode = args.do_kill ? unlocker::UnlockMode::TerminateProcesses : unlocker::UnlockMode::Auto;
                    unlock_report =
                        unlocker::unlock(result.lockers, mode, args.allow_critical, args.use_driver,
                                         args.driver_path.empty() ? std::filesystem::path{}
                                                                  : std::filesystem::path(args.driver_path));
                    if (args.use_driver && !unlock_report->ok()) {
                        // Treiber war angefragt, aber nicht alle Sperren wurden gelöst: den Grund nennen
                        // (meist: .sys fehlt, kein Test-Signing/WHQL, HVCI, oder kein Admin).
                        const char* reason = unlocker::driver::last_unavailable_reason();
                        if (reason && *reason) {
                            // Reason ist per Design ASCII (technisch, code-page-unabhängig).
                            std::wstring wide;
                            for (const char* p = reason; *p; ++p) wide += static_cast<wchar_t>(*p);
                            print_err(T(L"Treiber-Grund: ", L"Driver reason: ") + wide + L"\n");
                        }
                    }
                    if (!args.json) print_unlock_text(*unlock_report);
                    // Zugriff verweigert ohne Adminrechte ist Exit 3 („als Administrator wiederholen“), sonst Exit 2.
                    for (const auto& f : unlock_report->failures) {
                        if (!elevated && f.error == std::error_code(ERROR_ACCESS_DENIED, std::system_category())) any_denied = true;
                        else any_failure = true;
                    }
                }
                if (args.action != FileAction::None) {
                    op = run_file_action(target, args);
                    if (!args.json) print_fileop_text(*op);
                    any_failure |= !(op->ok || op->scheduled_reboot);
                }
            }
        }

        if (args.json) print(json_target(result, unlock_report, args.do_kill, op, last));
    }

    if (args.json) print(L"]\n");

    if (any_failure) return Failure;
    if (any_denied) return AccessDenied;
    return any_lockers && !acts ? LockersFound : Clean;
}
