#include "unlocker/scanner.h"

#include "unlocker/nt.h"
#include "unlocker/path_util.h"
#include "unlocker/process.h"
#include "unlocker/win_handle.h"

#include <lm.h>
#include <psapi.h>
#include <restartmanager.h>

#include <algorithm>
#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace unlocker {
namespace {

// UNLOCKER_TRACE=1: Dauer der einzelnen Stufen auf stderr (Diagnose).
class Trace {
public:
    explicit Trace(bool force = false)
        : enabled_(force || GetEnvironmentVariableW(L"UNLOCKER_TRACE", nullptr, 0) > 0) {
        QueryPerformanceCounter(&last_);
    }
    void mark(const wchar_t* stage) {
        if (!enabled_) return;
        LARGE_INTEGER now{}, freq{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        fwprintf(stderr, L"[trace] %-28s %6.1f ms\n", stage,
                 static_cast<double>(now.QuadPart - last_.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart));
        last_ = now;
    }

private:
    bool enabled_;
    LARGE_INTEGER last_{};
};

struct RmEntry {
    std::wstring app_name;
    bool is_service = false;
    FILETIME start{};
};

struct ProcessContext {
    WinHandle handle;  // PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, falls möglich
    bool access_denied = false;  // nicht lesbar, aber nicht geschützt → Elevation könnte helfen
    bool is_protected = false;   // PPL/Protected Process → auch mit Elevation nicht lesbar
    bool is_wow64 = false;
};

using ProcessCache = std::map<DWORD, ProcessContext>;

HANDLE open_for_query(const std::wstring& extended_path) {
    return CreateFileW(extended_path.c_str(), FILE_READ_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

// Ziele selbst plus – bei Ordnern – Einträge darunter, höchstens `limit` insgesamt. `more` meldet,
// ob darüber hinaus weitere Einträge existieren (dann lohnt der Snapshot-Modus).
std::vector<std::wstring> collect_targets(const std::vector<std::filesystem::path>& targets, std::size_t limit,
                                          bool& is_directory, bool& more) {
    std::vector<std::wstring> files;
    more = false;
    for (const auto& target : targets) {
        // Durchgehend \\?\-Pfade: Enumeration funktioniert so auch jenseits von MAX_PATH ohne LongPathsEnabled
        const std::filesystem::path extended(path_util::to_extended(target));
        files.push_back(extended.wstring());

        if (!path_util::is_directory(extended)) continue;
        is_directory = true;

        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(
            extended, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (files.size() >= limit) {
                more = true;
                return files;
            }
            files.push_back(path_util::to_extended(it->path()));
        }
    }
    return files;
}

bool inside_any_root(std::wstring_view path, const std::vector<std::wstring>& roots) {
    for (const auto& root : roots) {
        if (path_util::is_same_or_inside(path, root)) return true;
    }
    return false;
}

// Wer laut Kernel eine Datei benutzt: der System-Prozess (PID 4) und/oder andere Prozesse.
struct FileUsers {
    bool system = false;
    bool others = false;
};

// Stufe 1: Der Kernel kennt alle Prozesse mit offenem Handle auf diese Datei.
FileUsers pids_using_file(const std::wstring& extended_path, std::map<DWORD, std::wstring>& out) {
    FileUsers users;
    const auto& api = nt::api();
    if (!api.QueryInformationFile) return users;

    WinHandle file(open_for_query(extended_path));
    if (!file) return users;

    std::vector<std::byte> buffer(sizeof(nt::FileProcessIdsUsingFileInformation) + 256 * sizeof(ULONG_PTR));
    for (int attempt = 0; attempt < 5; ++attempt) {
        IO_STATUS_BLOCK iosb{};
        const NTSTATUS status =
            api.QueryInformationFile(file.get(), &iosb, buffer.data(), static_cast<ULONG>(buffer.size()),
                                     nt::FileProcessIdsUsingFileInformationClass);
        if (status == nt::StatusInfoLengthMismatch || status == nt::StatusBufferOverflow ||
            status == nt::StatusBufferTooSmall) {
            buffer.resize(buffer.size() * 4);
            continue;
        }
        if (!NT_SUCCESS(status)) return users;

        const auto* info = reinterpret_cast<const nt::FileProcessIdsUsingFileInformation*>(buffer.data());
        const DWORD own = GetCurrentProcessId();
        for (ULONG i = 0; i < info->NumberOfProcessIdsInList; ++i) {
            const auto pid = static_cast<DWORD>(info->ProcessIdList[i]);
            // erste gefundene Datei pro Prozess merken – aussagekräftiger Pfad für ProcessOnly-Einträge
            out.try_emplace(pid, path_util::strip_prefix(extended_path));
            if (pid == 4) users.system = true;
            else if (pid != 0 && pid != own) users.others = true;
        }
        return users;
    }
    return users;
}

// Von welchem Rechner ein Freigabe-Zugriff kommt: NetFileEnum nennt nur den Benutzer, die Sitzungen (NetSessionEnum)
// verbinden Benutzer und Client. Nur eindeutige Zuordnungen – hat derselbe Benutzer Sitzungen von mehreren Rechnern,
// bleibt der Client leer statt geraten.
void attach_remote_clients(ScanResult& result) {
    const bool any = std::any_of(result.lockers.begin(), result.lockers.end(), [](const Locker& l) {
        return l.kind == LockKind::RemoteOpen && !l.remote_user.empty();
    });
    if (!any) return;

    std::vector<std::pair<std::wstring, std::wstring>> sessions;  // Benutzer, Client
    DWORD resume = 0;
    NET_API_STATUS status = 0;
    do {
        SESSION_INFO_10* info = nullptr;
        DWORD read = 0, total = 0;
        status = NetSessionEnum(nullptr, nullptr, nullptr, 10, reinterpret_cast<LPBYTE*>(&info), MAX_PREFERRED_LENGTH,
                                &read, &total, &resume);
        if (status != NERR_Success && status != ERROR_MORE_DATA) break;
        for (DWORD i = 0; i < read; ++i) {
            if (!info[i].sesi10_username || !info[i].sesi10_cname) continue;
            std::wstring client = info[i].sesi10_cname;
            client.erase(0, client.find_first_not_of(L'\\'));  // "\\192.0.2.10" → "192.0.2.10"
            if (!client.empty()) sessions.emplace_back(info[i].sesi10_username, std::move(client));
        }
        if (info) NetApiBufferFree(info);
    } while (status == ERROR_MORE_DATA);

    for (auto& l : result.lockers) {
        if (l.kind != LockKind::RemoteOpen || l.remote_user.empty()) continue;
        std::wstring found;
        bool ambiguous = false;
        for (const auto& [user, client] : sessions) {
            if (!path_util::equals_ci(user, l.remote_user)) continue;
            if (found.empty()) found = client;
            else if (!path_util::equals_ci(found, client)) ambiguous = true;
        }
        if (!ambiguous) l.remote_client = found;
    }
}

// Über eine Netzwerkfreigabe geöffnete Dateien unterhalb der Ziele (SMB-Server, läuft im Kernel als PID 4).
// NetFileEnum ist dasselbe wie "Freigegebene Ordner > Geöffnete Dateien" bzw. Get-SmbOpenFile; Level 3 braucht
// Adminrechte. Rückgabe: true, wenn die Abfrage verweigert wurde (dann ist nur bekannt, DASS das System die Datei hält).
bool resolve_remote_opens(ScanResult& result) {
    bool denied = false;
    for (const auto& root : result.roots) {
        if (root.starts_with(L"\\\\")) continue;  // Ziel liegt selbst auf einem anderen Rechner
        DWORD_PTR resume = 0;
        NET_API_STATUS status = 0;
        do {
            FILE_INFO_3* files = nullptr;
            DWORD read = 0, total = 0;
            status = NetFileEnum(nullptr, const_cast<LPWSTR>(root.c_str()), nullptr, 3, reinterpret_cast<LPBYTE*>(&files),
                                 MAX_PREFERRED_LENGTH, &read, &total, &resume);
            if (status != NERR_Success && status != ERROR_MORE_DATA) {
                if (status == ERROR_ACCESS_DENIED) denied = true;
                break;
            }
            for (DWORD i = 0; i < read; ++i) {
                if (!files[i].fi3_pathname || !inside_any_root(files[i].fi3_pathname, result.roots)) continue;
                Locker locker;
                locker.pid = 4;
                locker.process_name = L"System";
                locker.image_path = L"System";
                locker.kind = LockKind::RemoteOpen;
                locker.handle = files[i].fi3_id;
                locker.locked_path = files[i].fi3_pathname;
                if (files[i].fi3_username) locker.remote_user = files[i].fi3_username;
                result.lockers.push_back(std::move(locker));
            }
            if (files) NetApiBufferFree(files);
        } while (status == ERROR_MORE_DATA);
    }
    attach_remote_clients(result);
    return denied;
}

// Meldet der Kernel für eine Datei nur PID 4 (und ist sie kein Freigabe-Zugriff), hält sie das System selbst – z. B. ein
// Treiber. PID 4 hält allerdings auch für jedes gemappte Image eine harmlose Section-Referenz; deshalb zählt es erst,
// wenn sich die Datei wirklich nicht exklusiv öffnen lässt.
// Exklusives Öffnen scheitert an einer Sharing-Verletzung – irgendetwas hält die Datei (Zugriff verweigert zählt nicht).
bool sharing_blocked(const std::wstring& path) {
    WinHandle probe(CreateFileW(path_util::to_extended(path).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    return !probe && GetLastError() == ERROR_SHARING_VIOLATION;
}

void resolve_system_held(const std::vector<std::wstring>& system_only, ScanResult& result) {
    for (const auto& path : system_only) {
        const bool known = std::any_of(result.lockers.begin(), result.lockers.end(),
                                       [&](const Locker& l) { return path_util::equals_ci(l.locked_path, path); });
        if (known || !sharing_blocked(path)) continue;
        Locker locker;
        locker.pid = 4;
        locker.process_name = L"System";
        locker.image_path = L"System";
        locker.kind = LockKind::SystemHeld;
        locker.locked_path = path;
        result.lockers.push_back(std::move(locker));
    }
}

// Letztes Netz vor "keine Sperren": Meldet niemand die Datei, lässt sie sich aber nicht exklusiv öffnen, ist sie
// trotzdem gesperrt (z. B. Datei-Mapping ohne offenes Handle) – ehrlich als "kein Prozess gefunden" melden statt "frei".
void resolve_unattributed(const std::vector<std::wstring>& paths, ScanResult& result) {
    for (const auto& path : paths) {
        if (!sharing_blocked(path)) continue;
        Locker locker;
        locker.kind = LockKind::Unattributed;
        locker.locked_path = path_util::normalize(path_util::strip_prefix(path));
        result.lockers.push_back(std::move(locker));
    }
}

// Stufe 2: Restart Manager – findet auch geladene Module und liefert Anzeigenamen.
std::map<DWORD, RmEntry> pids_via_restart_manager(const std::vector<std::wstring>& extended_paths,
                                                  std::size_t max_files) {
    std::map<DWORD, RmEntry> out;

    DWORD session = 0;
    WCHAR session_key[CCH_RM_SESSION_KEY + 1]{};
    if (RmStartSession(&session, 0, session_key) != ERROR_SUCCESS) return out;

    // RM erwartet normale Pfade ohne \\?\-Präfix.
    std::vector<std::wstring> plain;
    plain.reserve(std::min(extended_paths.size(), max_files));
    for (const auto& p : extended_paths) {
        if (plain.size() >= max_files) break;
        plain.push_back(path_util::strip_prefix(p));
    }
    std::vector<LPCWSTR> pointers;
    pointers.reserve(plain.size());
    for (const auto& p : plain) pointers.push_back(p.c_str());

    if (RmRegisterResources(session, static_cast<UINT>(pointers.size()), pointers.data(), 0, nullptr, 0, nullptr) ==
        ERROR_SUCCESS) {
        UINT needed = 0;
        UINT count = 0;
        DWORD reasons = 0;
        std::vector<RM_PROCESS_INFO> infos;
        DWORD rc = RmGetList(session, &needed, &count, nullptr, &reasons);
        while (rc == ERROR_MORE_DATA) {
            infos.resize(needed);
            count = needed;
            rc = RmGetList(session, &needed, &count, infos.data(), &reasons);
        }
        if (rc == ERROR_SUCCESS) {
            for (UINT i = 0; i < count; ++i) {
                const auto& info = infos[i];
                RmEntry entry;
                entry.app_name = info.strAppName;
                entry.is_service = info.ApplicationType == RmService;
                entry.start = info.Process.ProcessStartTime;
                out[info.Process.dwProcessId] = std::move(entry);
            }
        }
    }
    RmEndSession(session);
    return out;
}

// Stufe 3a: Handle-Tabelle des gesamten Systems.
std::vector<std::byte> snapshot_handles() {
    const auto& api = nt::api();
    if (!api.QuerySystemInformation) return {};

    std::vector<std::byte> buffer(4u << 20);
    for (int attempt = 0; attempt < 8; ++attempt) {
        ULONG needed = 0;
        const NTSTATUS status = api.QuerySystemInformation(nt::SystemExtendedHandleInformationClass, buffer.data(),
                                                           static_cast<ULONG>(buffer.size()), &needed);
        if (status == nt::StatusInfoLengthMismatch) {
            buffer.resize(std::max<std::size_t>(needed + needed / 8, buffer.size() * 2));
            continue;
        }
        if (!NT_SUCCESS(status)) return {};
        return buffer;
    }
    return {};
}

// Der Typ-Index von "File" variiert je nach Windows-Build – über ein eigenes Referenz-Handle ermitteln.
std::optional<USHORT> find_file_type_index(const nt::SystemHandleInformationEx& table, HANDLE reference) {
    const auto own_pid = GetCurrentProcessId();
    for (ULONG_PTR i = 0; i < table.NumberOfHandles; ++i) {
        const auto& e = table.Handles[i];
        if (e.UniqueProcessId == own_pid && e.HandleValue == reinterpret_cast<ULONG_PTR>(reference)) {
            return e.ObjectTypeIndex;
        }
    }
    return std::nullopt;
}

// Pfadauflösung vieler Handles parallel auf dem Threadpool mit Gesamt-Deadline. Hängt eine Abfrage
// (z. B. Netzlaufwerk), läuft ihr Callback im Hintergrund weiter und schließt das Handle selbst –
// der Scan wartet nicht darauf. Der Batch lebt per shared_ptr so lange wie der letzte Callback.
struct PathBatch {
    struct Job {
        HANDLE handle = nullptr;
        std::wstring path;
        std::atomic<bool> done{false};
    };
    std::vector<Job> jobs;
    std::atomic<std::size_t> remaining{0};
    WinHandle all_done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
};

struct PathJobRef {
    std::shared_ptr<PathBatch> batch;
    std::size_t index = 0;
};

void CALLBACK path_batch_callback(PTP_CALLBACK_INSTANCE, void* context) {
    std::unique_ptr<PathJobRef> ref(static_cast<PathJobRef*>(context));
    auto& job = ref->batch->jobs[ref->index];
    job.path = path_util::final_path(job.handle);
    CloseHandle(job.handle);
    job.done.store(true, std::memory_order_release);
    if (ref->batch->remaining.fetch_sub(1) == 1) SetEvent(ref->batch->all_done.get());
}

// Übernimmt die Handles. Nach Rückkehr sind die Pfade der fertigen Jobs gesetzt; timed_out meldet,
// ob das Budget überschritten wurde.
std::shared_ptr<PathBatch> resolve_paths(std::vector<HANDLE> handles, std::chrono::milliseconds budget, bool& timed_out,
                                         const std::function<void(const ScanProgress&)>& progress) {
    auto batch = std::make_shared<PathBatch>();
    batch->jobs = std::vector<PathBatch::Job>(handles.size());
    for (std::size_t i = 0; i < handles.size(); ++i) batch->jobs[i].handle = handles[i];
    batch->remaining = handles.size();
    timed_out = false;
    if (handles.empty()) return batch;

    for (std::size_t i = 0; i < handles.size(); ++i) {
        auto ref = std::make_unique<PathJobRef>(PathJobRef{batch, i});
        if (TrySubmitThreadpoolCallback(path_batch_callback, ref.get(), nullptr)) {
            ref.release();
        } else {
            path_batch_callback(nullptr, ref.release());
        }
    }
    // Warten bis alle fertig sind – oder bis es 300 ms lang keinen Fortschritt mehr gibt (ein hängendes
    // Handle soll nicht das ganze Budget kosten) – oder bis das Budget aufgebraucht ist.
    constexpr DWORD kSlice = 25;
    constexpr DWORD kStall = 300;
    const DWORD total = static_cast<DWORD>(std::max<long long>(budget.count(), 1));
    DWORD elapsed = 0;
    DWORD stalled = 0;
    std::size_t last_remaining = handles.size();
    for (;;) {
        const DWORD slice = std::min(kSlice, total - elapsed);
        if (WaitForSingleObject(batch->all_done.get(), slice) == WAIT_OBJECT_0) break;
        elapsed += slice;
        const std::size_t now_remaining = batch->remaining.load();
        if (progress) progress({L"handles", handles.size() - now_remaining, handles.size()});
        if (now_remaining != last_remaining) {
            last_remaining = now_remaining;
            stalled = 0;
        } else {
            stalled += kSlice;
        }
        if (elapsed >= total || stalled >= kStall) {
            timed_out = true;
            break;
        }
    }
    return batch;
}

ProcessContext& context_for(DWORD pid, ProcessCache& cache) {
    if (auto it = cache.find(pid); it != cache.end()) return it->second;
    ProcessContext ctx;
    ctx.handle.reset(OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (ctx.handle) {
        ctx.is_wow64 = process::is_wow64(ctx.handle.get());
    } else if (process::is_protected_process(pid)) {
        ctx.is_protected = true;  // PPL: OpenProcess mit VM_READ/DUP_HANDLE scheitert immer, auch als Administrator
    } else {
        ctx.access_denied = true;
    }
    return cache.emplace(pid, std::move(ctx)).first->second;
}

struct HandleCandidate {
    DWORD pid = 0;
    ULONG_PTR handle = 0;
    PVOID object = nullptr;  // FILE_OBJECT – identisch für alle Handles auf dieselbe Datei
    ULONG attributes = 0;    // OBJ_PROTECT_CLOSE (0x1) = nicht schließbar
};

// Stufe 3: Datei-Handles auflösen und gegen die Ziele vergleichen. Entweder nur für die Kandidaten-PIDs
// (pids gesetzt) oder für alle Prozesse (Snapshot-Modus, pids == nullptr). Handles auf dasselbe Kernel-
// objekt werden nur einmal aufgelöst.
void resolve_handles(const std::set<DWORD>* pids, const ScanOptions& options, ProcessCache& procs,
                     ScanResult& result) {
    std::wstring self(32768, L'\0');
    const DWORD self_len = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (self_len == 0) return;
    self.resize(self_len);
    WinHandle reference(open_for_query(path_util::to_extended(self)));
    if (!reference) return;

    const auto buffer = snapshot_handles();
    if (buffer.empty()) return;
    const auto& table = *reinterpret_cast<const nt::SystemHandleInformationEx*>(buffer.data());

    const auto file_type = find_file_type_index(table, reference.get());
    if (!file_type) return;

    const DWORD own = GetCurrentProcessId();
    std::vector<HandleCandidate> candidates;
    for (ULONG_PTR i = 0; i < table.NumberOfHandles; ++i) {
        const auto& e = table.Handles[i];
        if (e.ObjectTypeIndex != *file_type) continue;
        const auto pid = static_cast<DWORD>(e.UniqueProcessId);
        if (pid == own || pid == 0 || pid == 4) continue;
        if (pids && !pids->contains(pid)) continue;
        candidates.push_back({pid, e.HandleValue, e.Object, e.HandleAttributes});
    }

    // Pro Kernelobjekt ein Handle duplizieren (erstes zugängliches), Typ prüfen, Pfad im Batch auflösen.
    // Ohne Adminrechte blendet der Kernel die Objektadressen aus (Object == 0) – dann keine Zusammenlegung.
    std::map<PVOID, std::size_t> object_to_job;
    std::vector<std::size_t> candidate_job(candidates.size(), SIZE_MAX);
    std::vector<HANDLE> dups;
    std::set<PVOID> rejected;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        if (c.object) {
            if (const auto it = object_to_job.find(c.object); it != object_to_job.end()) {
                candidate_job[i] = it->second;
                continue;
            }
            if (rejected.contains(c.object)) continue;
        }
        auto& ctx = context_for(c.pid, procs);
        if (!ctx.handle) continue;

        HANDLE raw = nullptr;
        if (!DuplicateHandle(ctx.handle.get(), reinterpret_cast<HANDLE>(c.handle), GetCurrentProcess(), &raw, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            continue;
        }
        // Pipes/Geräte aussortieren, bevor der Name abgefragt wird – diese Abfrage beantwortet
        // der I/O-Manager selbst und blockiert nicht.
        if (GetFileType(raw) != FILE_TYPE_DISK) {
            CloseHandle(raw);
            if (c.object) rejected.insert(c.object);
            continue;
        }
        candidate_job[i] = dups.size();
        if (c.object) object_to_job[c.object] = dups.size();
        dups.push_back(raw);
    }

    bool timed_out = false;
    const auto budget = pids ? options.name_query_timeout * static_cast<int>(std::max<std::size_t>(dups.size(), 1))
                             : options.snapshot_timeout;
    const std::size_t dup_count = dups.size();
    if (options.progress) options.progress({L"handles", 0, dup_count});
    const auto batch = resolve_paths(std::move(dups), std::min(budget, options.snapshot_timeout * 4), timed_out, options.progress);
    if (options.progress) options.progress({L"handles", dup_count, dup_count});
    if (timed_out) result.snapshot_timeout = true;

    if (GetEnvironmentVariableW(L"UNLOCKER_TRACE", nullptr, 0) > 0) {
        std::size_t done = 0;
        for (const auto& j : batch->jobs) done += j.done.load() ? 1 : 0;
        fwprintf(stderr, L"[trace] handles: %zu Kandidaten, %zu dupliziert, %zu aufgelöst, timeout=%d\n",
                 candidates.size(), dup_count, done, timed_out ? 1 : 0);
    }

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        if (candidate_job[i] == SIZE_MAX) continue;
        const auto& job = batch->jobs[candidate_job[i]];
        if (!job.done.load(std::memory_order_acquire) || job.path.empty()) continue;
        if (!inside_any_root(job.path, result.roots)) continue;

        Locker locker;
        locker.pid = c.pid;
        locker.kind = LockKind::FileHandle;
        locker.handle = c.handle;
        locker.locked_path = job.path;
        locker.protect_close = (c.attributes & 0x1) != 0;  // OBJ_PROTECT_CLOSE
        result.lockers.push_back(std::move(locker));
    }
}

// Geladene EXE/DLLs im Zielpfad. Der Kernel-Fast-Path meldet auch Prozesse, die eine Datei nur als Image
// gemappt haben, daher reicht es, die Kandidaten zu prüfen – ein Vollscan aller Prozesse kostet ~0,7 s.
void resolve_modules(const std::set<DWORD>& pids, ProcessCache& procs, ScanResult& result) {
    std::vector<HMODULE> modules(1024);
    std::wstring name(32768, L'\0');

    for (const DWORD pid : pids) {
        auto& ctx = context_for(pid, procs);
        if (!ctx.handle) continue;

        DWORD needed = 0;
        const auto capacity = [&] { return static_cast<DWORD>(modules.size() * sizeof(HMODULE)); };
        if (!EnumProcessModulesEx(ctx.handle.get(), modules.data(), capacity(), &needed, LIST_MODULES_ALL)) continue;
        if (needed > capacity()) {
            modules.resize(needed / sizeof(HMODULE) + 64);
            if (!EnumProcessModulesEx(ctx.handle.get(), modules.data(), capacity(), &needed, LIST_MODULES_ALL)) {
                continue;
            }
        }

        const auto count = std::min<std::size_t>(needed / sizeof(HMODULE), modules.size());
        for (std::size_t i = 0; i < count; ++i) {
            const DWORD len =
                GetModuleFileNameExW(ctx.handle.get(), modules[i], name.data(), static_cast<DWORD>(name.size()));
            if (len == 0) continue;
            std::wstring path = path_util::strip_prefix(std::wstring(name.data(), len));
            if (!inside_any_root(path, result.roots)) continue;

            Locker locker;
            locker.pid = pid;
            locker.kind = LockKind::MappedImage;
            locker.locked_path = std::move(path);
            result.lockers.push_back(std::move(locker));
        }
    }
}

// Arbeitsverzeichnis der Kandidaten: liegt es im Ziel, wird das passende Verzeichnis-Handle als
// WorkingDirectory markiert (bleibt schließbar); ohne gefundenes Handle entsteht ein eigener Eintrag.
void resolve_working_directories(const std::set<DWORD>& pids, ProcessCache& procs, ScanResult& result) {
    for (const DWORD pid : pids) {
        auto& ctx = context_for(pid, procs);
        if (!ctx.handle) continue;
        std::wstring cwd = process::current_directory(ctx.handle.get());
        while (cwd.size() > 3 && cwd.back() == L'\\') cwd.pop_back();
        if (cwd.empty()) continue;
        // Der PEB liefert den Pfad so, wie er gesetzt wurde – ggf. mit 8.3-Namen (C:\Users\ADMINI~1\...) oder
        // anderer Schreibweise. Erst die Langform ist mit den Wurzeln und den Handle-Pfaden vergleichbar.
        cwd = path_util::normalize(cwd);
        if (!inside_any_root(cwd, result.roots)) continue;

        bool marked = false;
        for (auto& l : result.lockers) {
            if (l.pid == pid && l.kind == LockKind::FileHandle && path_util::equals_ci(l.locked_path, cwd)) {
                l.kind = LockKind::WorkingDirectory;
                marked = true;
            }
        }
        if (marked) continue;
        Locker locker;
        locker.pid = pid;
        locker.kind = LockKind::WorkingDirectory;
        locker.locked_path = cwd;
        result.lockers.push_back(std::move(locker));
    }
}

// Prozessname, Abbild, Startzeit, Kritikalität – für neue Einträge, bereits gefüllte bleiben unberührt.
void fill_metadata(ScanResult& result) {
    struct Meta {
        std::wstring image;
        std::wstring name;
        FILETIME start{};
    };
    std::map<DWORD, Meta> meta;
    for (auto& l : result.lockers) {
        if (!l.process_name.empty() || !l.image_path.empty() || l.kind == LockKind::Unattributed) continue;
        auto it = meta.find(l.pid);
        if (it == meta.end()) {
            Meta m;
            m.image = process::image_path(l.pid);
            m.name = process::name_from_path(m.image);
            if (m.name.empty()) m.name = process::image_name_from_system(l.pid);  // nicht öffenbare Prozesse (csrss, PPL)
            process::start_time(l.pid, m.start);
            it = meta.emplace(l.pid, std::move(m)).first;
        }
        l.image_path = it->second.image;
        l.process_name = it->second.name;
        l.process_start = it->second.start;
        l.is_critical = process::is_critical(l.pid, l.process_name);
        l.is_sensitive = process::is_sensitive(l.process_name);
        if (l.image_path.empty()) l.is_protected = process::is_protected_process(l.pid);
    }
}

void sort_lockers(ScanResult& result) {
    std::sort(result.lockers.begin(), result.lockers.end(), [](const Locker& a, const Locker& b) {
        if (a.pid != b.pid) return a.pid < b.pid;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.locked_path < b.locked_path;
    });
}

}  // namespace

ScanResult scan(const std::filesystem::path& target, const ScanOptions& options) {
    return scan(std::vector<std::filesystem::path>{target}, options);
}

ScanResult scan(const std::vector<std::filesystem::path>& given_targets, const ScanOptions& options) {
    Trace trace(options.trace);
    const auto report = [&](const wchar_t* stage, std::size_t done, std::size_t total) {
        if (options.progress) options.progress({stage, done, total});
    };
    // Loopback-UNC und darauf gemappte Laufwerke lokal scannen (über den Redirector unsichtbar). Fehlende oder
    // nicht erreichbare Ziele fallen hier komplett raus – jede weitere Abfrage darauf würde erneut blockieren.
    ScanResult result;
    std::vector<std::filesystem::path> targets;
    targets.reserve(given_targets.size());
    for (const auto& given : given_targets) {
        auto t = path_util::localize(given);
        if (path_util::probe(t, options.target_timeout) != path_util::Reachability::Exists) {
            result.missing.push_back(given.wstring());
            continue;
        }
        result.roots.push_back(path_util::normalize(t));
        targets.push_back(std::move(t));
    }
    if (!result.roots.empty()) result.target = result.roots.front();
    trace.mark(L"Ziele prüfen");

    bool more = false;
    const auto files = collect_targets(targets, options.per_file_query_limit, result.is_directory, more);
    result.snapshot_mode = more;
    trace.mark(L"Ziele sammeln");

    // Stufe 1: Kernel-Abfrage pro Datei – im Snapshot-Modus nur für die expliziten Ziele selbst
    // (Ordner-Handles, gemappte Images); alles darunter übernimmt die Handle-Tabelle.
    std::map<DWORD, std::wstring> pids;
    std::vector<std::wstring> system_only;  // nur PID 4 meldet die Datei – Kandidaten für SystemHeld
    const auto query = [&](const std::wstring& extended) {
        if (const auto users = pids_using_file(extended, pids); users.system && !users.others) {
            // Langform: Ziele können als 8.3-Kurzpfad kommen, NetFileEnum meldet die Langform (Abgleich unten)
            system_only.push_back(path_util::normalize(path_util::strip_prefix(extended)));
        }
    };
    if (result.snapshot_mode) {
        for (const auto& t : targets) query(path_util::to_extended(t));
    } else {
        for (std::size_t i = 0; i < files.size(); ++i) {
            report(L"fastpath", i, files.size());
            query(files[i]);
        }
    }
    report(L"fastpath", files.size(), files.size());
    pids.erase(GetCurrentProcessId());
    pids.erase(0);
    pids.erase(4);  // System hält für jedes gemappte Image eine Section-Referenz – keine lösbare Sperre
    trace.mark(L"Stufe 1 (Fast-Path)");

    std::set<DWORD> pid_set;
    for (const auto& [pid, file] : pids) pid_set.insert(pid);

    ProcessCache procs;
    if (options.deep && (result.snapshot_mode || !pid_set.empty())) {
        resolve_handles(result.snapshot_mode ? nullptr : &pid_set, options, procs, result);
    }
    for (const auto& l : result.lockers) pid_set.insert(l.pid);
    trace.mark(result.snapshot_mode ? L"Stufe 3 (Handle-Snapshot)" : L"Stufe 3 (Handles)");
    if (!pid_set.empty()) report(L"modules", 0, pid_set.size());
    if (options.include_modules && !pid_set.empty()) resolve_modules(pid_set, procs, result);
    trace.mark(L"Module");
    if (result.is_directory && !pid_set.empty()) {
        report(L"cwd", 0, pid_set.size());
        resolve_working_directories(pid_set, procs, result);
    }
    trace.mark(L"Arbeitsverzeichnisse");

    // Sperren ohne Prozess dahinter (PID 4): Freigabe-Zugriffe immer prüfen (decken im Snapshot-Modus auch Dateien unter
    // dem Ordner ab), vom System gehaltene Dateien nur, wenn der Kernel ausschließlich PID 4 meldet.
    // Ohne Adminrechte scheitert NetFileEnum immer – "als Administrator" nur empfehlen, wenn PID 4 wirklich gemeldet wurde.
    if (resolve_remote_opens(result) && !system_only.empty()) result.access_denied = true;
    resolve_system_held(system_only, result);
    trace.mark(L"Freigaben/System");

    // Prozesse ohne konkret aufgelöste Sperre trotzdem listen.
    std::set<DWORD> resolved;
    for (const auto& l : result.lockers) resolved.insert(l.pid);
    for (const auto& [pid, file] : pids) {
        if (resolved.contains(pid)) continue;
        Locker locker;
        locker.pid = pid;
        locker.kind = LockKind::ProcessOnly;
        locker.locked_path = file;
        result.lockers.push_back(std::move(locker));
    }

    // Im Snapshot-Modus nur die expliziten Ziele prüfen (sonst beliebig viele Öffnungsversuche).
    if (result.lockers.empty()) {
        std::vector<std::wstring> explicit_targets;
        for (const auto& t : targets) explicit_targets.push_back(path_util::to_extended(t));
        resolve_unattributed(result.snapshot_mode ? explicit_targets : files, result);
    }
    trace.mark(L"Exklusiv-Prüfung");

    for (const auto& [pid, ctx] : procs) {
        // Im Snapshot-Modus zählt jeder nicht lesbare Prozess, sonst nur die Kandidaten.
        const bool relevant = result.snapshot_mode || pid_set.contains(pid);
        if (ctx.access_denied && relevant) result.access_denied = true;
        if (ctx.is_protected && pid_set.contains(pid)) result.has_protected = true;
    }
    for (auto& l : result.lockers) {
        if (auto it = procs.find(l.pid); it != procs.end()) {
            l.is_protected = it->second.is_protected;
            l.is_wow64 = it->second.is_wow64;
        }
    }

    fill_metadata(result);
    sort_lockers(result);
    trace.mark(L"Metadaten");

    if (options.use_restart_manager) enrich(result, targets, options);
    return result;
}

std::vector<DWORD> all_pids() {
    std::vector<DWORD> pids(1024);
    DWORD bytes = 0;
    for (;;) {
        if (!EnumProcesses(pids.data(), static_cast<DWORD>(pids.size() * sizeof(DWORD)), &bytes)) return {};
        if (bytes < pids.size() * sizeof(DWORD)) break;
        pids.resize(pids.size() * 2);
    }
    pids.resize(bytes / sizeof(DWORD));
    return pids;
}

void enrich(ScanResult& result, const std::vector<std::filesystem::path>& given_targets, const ScanOptions& options) {
    Trace trace(options.trace);
    std::vector<std::filesystem::path> targets;
    for (const auto& t : given_targets) targets.push_back(path_util::localize(t));
    if (options.progress) options.progress({L"restart-manager", 0, 0});
    bool is_directory = false;
    bool more = false;
    const auto files = collect_targets(targets, options.max_restart_manager_files, is_directory, more);
    const DWORD own = GetCurrentProcessId();

    // Im Snapshot-Modus fehlen gemappte Images unterhalb der Ordner (keine Handles): alle Prozesse prüfen.
    if (result.snapshot_mode && options.include_modules) {
        ProcessCache procs;
        std::set<DWORD> everyone;
        for (const DWORD pid : all_pids()) {
            if (pid != own && pid != 0 && pid != 4) everyone.insert(pid);
        }
        ScanResult modules;
        modules.roots = result.roots;
        resolve_modules(everyone, procs, modules);
        for (auto& m : modules.lockers) {
            const bool known = std::any_of(result.lockers.begin(), result.lockers.end(), [&](const Locker& l) {
                return l.pid == m.pid && l.kind == m.kind && path_util::equals_ci(l.locked_path, m.locked_path);
            });
            if (!known) result.lockers.push_back(std::move(m));
        }
        trace.mark(L"Module (alle Prozesse)");
    }

    const auto rm = pids_via_restart_manager(files, options.max_restart_manager_files);
    trace.mark(L"Stufe 2 (Restart Manager)");

    for (const auto& [pid, entry] : rm) {
        if (pid == own || pid == 0 || pid == 4) continue;
        bool known = false;
        for (auto& l : result.lockers) {
            if (l.pid != pid) continue;
            known = true;
            if (l.app_name.empty()) l.app_name = entry.app_name;
            l.is_service = l.is_service || entry.is_service;
            if (l.process_start.dwLowDateTime == 0 && l.process_start.dwHighDateTime == 0) l.process_start = entry.start;
        }
        if (known) continue;

        Locker locker;
        locker.pid = pid;
        locker.kind = LockKind::ProcessOnly;
        locker.locked_path = result.target;
        locker.app_name = entry.app_name;
        locker.is_service = entry.is_service;
        result.lockers.push_back(std::move(locker));
    }
    // Hat der Restart Manager doch einen Prozess gefunden, ist die Sperre zugeordnet.
    if (std::any_of(result.lockers.begin(), result.lockers.end(),
                    [](const Locker& l) { return l.kind != LockKind::Unattributed; })) {
        std::erase_if(result.lockers, [](const Locker& l) { return l.kind == LockKind::Unattributed; });
    }

    fill_metadata(result);
    for (auto& l : result.lockers) {
        if (l.process_start.dwLowDateTime == 0 && l.process_start.dwHighDateTime == 0) {
            if (auto it = rm.find(l.pid); it != rm.end()) l.process_start = it->second.start;
        }
    }
    sort_lockers(result);
    result.enriched = true;
}

const wchar_t* to_string(LockKind kind) noexcept {
    switch (kind) {
        case LockKind::FileHandle: return L"handle";
        case LockKind::MappedImage: return L"module";
        case LockKind::ProcessOnly: return L"process";
        case LockKind::WorkingDirectory: return L"cwd";
        case LockKind::RemoteOpen: return L"remote";
        case LockKind::SystemHeld: return L"system";
        case LockKind::Unattributed: return L"unknown";
    }
    return L"?";
}

}  // namespace unlocker
