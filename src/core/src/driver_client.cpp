// User-Mode-Client für den Unlocker-Kernel-Treiber (v2).
//
// Strategie (dokumentierte APIs, auf admin-erhebten Unlock-Aufrufen):
//   1. Gerät \\.\UnlockerDriver öffnen – falls der Treiber schon läuft, direkt nutzen.
//   2. Sonst den Treiber ON-DEMAND laden: SCM (CreateService SERVICE_KERNEL_DRIVER + StartService),
//      dann Gerät öffnen. Der Dienst bleibt nach dem Stoppen als (nicht geladener) On-Demand-Eintrag
//      in der Registry – er belastet das System nicht und wird vom nächsten Start wiederverwendet.
//   3. Die letzte offene Session stoppt den Dienst (ControlService STOP).
//
// Fehlt die .sys, fehlt Admin, ist kein Test-Signing/WHQL aktiv oder HVCI blockiert den Start,
// ist open_session() leer – unlock() fällt sauber auf reinen User-Mode zurück.

#include "unlocker/driver_client.h"

#include "unlocker/nt.h"
#include "unlocker/win_handle.h"

#include "../../driver/include/unlocker_driver.h"

#include <winioctl.h>
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <system_error>

namespace unlocker::driver {
namespace {

const wchar_t* k_service_name = L"UnlockerDriver";

// SCM-Handles gehören CloseServiceHandle – CloseHandle (WinHandle) schlägt auf ihnen fehl und gibt nichts frei.
class ScHandle {
public:
    explicit ScHandle(SC_HANDLE h = nullptr) noexcept : h_(h) {}
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    ~ScHandle() {
        if (h_) CloseServiceHandle(h_);
    }
    void reset(SC_HANDLE h) noexcept {
        if (h_) CloseServiceHandle(h_);
        h_ = h;
    }
    SC_HANDLE get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != nullptr; }

private:
    SC_HANDLE h_;
};

std::string g_reason;
std::mutex g_reason_mutex;

void set_reason(const std::string& r) {
    std::lock_guard lock(g_reason_mutex);
    g_reason = r;
}
void clear_reason() {
    std::lock_guard lock(g_reason_mutex);
    g_reason.clear();
}

// Pfad zur .sys: explizit übergeben oder neben der eigenen EXE (unlocker_driver.sys).
std::filesystem::path find_sys(const std::filesystem::path& explicit_path) {
    if (!explicit_path.empty()) return explicit_path;
    wchar_t buffer[32768]{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, 32767);
    if (len == 0) return {};
    return std::filesystem::path(buffer).parent_path() / L"unlocker_driver.sys";
}

// Gerät öffnen; false wenn der Treiber nicht läuft.
bool open_device(HANDLE& out) {
    out = CreateFileW(UNLOCKER_USER_PATH, FILE_READ_ACCESS | FILE_WRITE_ACCESS, 0, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    return out != INVALID_HANDLE_VALUE;
}

// Treiber als On-Demand-Dienst laden und starten. true wenn danach das Gerät offen ist.
bool load_on_demand(const std::filesystem::path& sys, HANDLE& out_device) {
    std::error_code ec;
    if (!std::filesystem::exists(sys, ec)) {  // error_code-Form: wirfen ist hier nicht erlaubt
        set_reason("unlocker_driver.sys not found: " + sys.string());
        return false;
    }

    const ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
    if (!scm) {
        set_reason("SCM not reachable (error " + std::to_string(GetLastError()) + "); requires administrator");
        return false;
    }

    const std::wstring image = L"\\??\\" + sys.wstring();
    ScHandle service(OpenServiceW(scm.get(), k_service_name, SERVICE_ALL_ACCESS));
    if (!service) {
        service.reset(CreateServiceW(scm.get(), k_service_name, k_service_name, SERVICE_ALL_ACCESS,
                                     SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE, image.c_str(),
                                     nullptr, nullptr, nullptr, nullptr, nullptr));
        if (!service) {
            set_reason("cannot create service (error " + std::to_string(GetLastError()) +
                       "); requires administrator");
            return false;
        }
    }

    // StartService can report ERROR_SERVICE_ALREADY_RUNNING - that is fine.
    if (!StartServiceW(service.get(), 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        set_reason("driver start failed (error " + std::to_string(GetLastError()) +
                   "); is test-signing/WHQL signing active and HVCI not blocking it?");
        return false;
    }

    if (!open_device(out_device)) {
        set_reason("device \\\\.\\UnlockerDriver not reachable (error " +
                   std::to_string(GetLastError()) + ")");
        return false;
    }
    return true;
}

std::atomic<int> g_sessions{0};
std::atomic<bool> g_we_loaded{false};

}  // namespace

struct Session::Impl {
    WinHandle device;
};

Session::Session() = default;
Session::Session(Session&& other) noexcept : impl_(std::move(other.impl_)) {}
Session& Session::operator=(Session&& other) noexcept {
    if (this != &other) impl_ = std::move(other.impl_);
    return *this;
}
Session::~Session() {
    // Nur geöffnete Sessions (impl_ gesetzt) zählen; der in open_session() lokal erzeugte
    // Platzhalter wird sonst doppelt abgezählt, wenn er nach dem Move wegsterbt.
    if (impl_) impl_->device.reset();
    if (impl_ && g_sessions.fetch_sub(1) == 1) {
        // Letzte Session: den von uns geladenen Dienst stoppen (bleibt als On-Demand-Eintrag).
        if (g_we_loaded.exchange(false)) {
            const ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
            if (scm) {
                const ScHandle service(OpenServiceW(scm.get(), k_service_name, SERVICE_STOP));
                if (service) {
                    SERVICE_STATUS status{};
                    ControlService(service.get(), SERVICE_CONTROL_STOP, &status);
                }
            }
        }
    }
}

std::optional<Session> open_session(const std::filesystem::path& sys_path) {
    clear_reason();

    HANDLE device = nullptr;
    if (!open_device(device)) {
        // Gerät nicht da: Treiber on-demand laden.
        if (!load_on_demand(find_sys(sys_path), device)) {
            return std::nullopt;
        }
        g_we_loaded.store(true);
    }

    // Die Session erst nach Erfolg konstruieren – der Destruktor zählt sie nur, wenn sie offen
    // wurde (impl_ gesetzt), der Service-Refcount bleibt so korrekt.
    Session session;
    session.impl_ = std::make_unique<Session::Impl>();
    session.impl_->device.reset(device);
    g_sessions.fetch_add(1);
    return session;
}

const char* last_unavailable_reason() noexcept {
    static thread_local std::string copy;
    {
        std::lock_guard lock(g_reason_mutex);
        copy = g_reason;
    }
    return copy.c_str();
}

std::optional<std::error_code> Session::close_handle(std::uint32_t pid, std::uintptr_t handle) const {
    if (!impl_ || !impl_->device) return win_error(ERROR_INVALID_HANDLE);

    UNLOCKER_INPUT in{};
    in.Pid = pid;
    in.HandleValue = handle;
    UNLOCKER_OUTPUT out{};
    DWORD bytes = 0;
    if (!DeviceIoControl(impl_->device.get(), static_cast<DWORD>(IOCTL_UNLOCKER_CLOSE_HANDLE), &in,
                         static_cast<DWORD>(sizeof(in)), &out, static_cast<DWORD>(sizeof(out)), &bytes,
                         nullptr)) {
        return win_error(GetLastError());
    }
    if (out.Status != 0 /* STATUS_SUCCESS */) {
        // Der Treiber liefert einen NT-Status – als Win32-Code weiterreichen (STATUS_ACCESS_DENIED -> ERROR_ACCESS_DENIED),
        // sonst würde FormatMessage den NT-Wert als Win32-Fehler deuten.
        const auto& api = nt::api();
        const DWORD code = api.NtStatusToDosError ? api.NtStatusToDosError(out.Status) : static_cast<DWORD>(out.Status);
        return win_error(code);
    }
    return std::nullopt;
}

}  // namespace unlocker::driver
