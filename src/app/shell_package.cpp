#include "shell_package.h"

#include "settings.h"

#include <windows.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>

namespace unlocker::ui::shell_package {
namespace {

using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::Management::Deployment::AddPackageOptions;
using winrt::Windows::Management::Deployment::PackageManager;

constexpr wchar_t kPackageName[] = L"Unlocker.App";  // Identity/@Name in packaging\AppxManifest.xml
constexpr wchar_t kPackageFile[] = L"unlocker-sparse.msix";

std::filesystem::path exe_directory() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
}

DWORD build_number() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    return fn && fn(&info) == 0 ? info.dwBuildNumber : 0;
}

// Bereitstellungsvorgang abwarten. Bei Fehlern liefert GetResults() trotzdem den DeploymentResult mit ErrorText
// (Besonderheit der PackageManager-Vorgänge) – der sagt mehr als das HRESULT (z. B. „Zertifikat nicht vertrauenswürdig“).
template <typename Operation>
Outcome wait(Operation const& op) {
    Outcome outcome;
    try {
        op.get();
    } catch (winrt::hresult_error const& e) {
        outcome.hr = e.code();
        outcome.text = e.message().c_str();
        try {
            if (op.Status() == AsyncStatus::Error) {
                if (auto detail = op.GetResults().ErrorText(); !detail.empty()) outcome.text = detail.c_str();
            }
        } catch (winrt::hresult_error const&) {
            // keine Details – die ursprüngliche Meldung bleibt
        }
    }
    return outcome;
}

}  // namespace

bool supported() { return build_number() >= 22000; }

std::wstring registered_full_name() {
    try {
        PackageManager manager;
        for (auto const& package : manager.FindPackagesForUser(L"")) {
            if (package.Id().Name() == kPackageName) return std::wstring(package.Id().FullName());
        }
    } catch (winrt::hresult_error const&) {
    }
    return {};
}

std::filesystem::path package_file() {
    const auto exe_dir = exe_directory();
    for (const auto& candidate : {settings::file().parent_path() / kPackageFile, exe_dir / kPackageFile,
                                  exe_dir / L"kontextmenu" / kPackageFile}) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

Outcome enable(const std::filesystem::path& msix, const std::filesystem::path& external_location) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        PackageManager manager;
        AddPackageOptions options;
        options.ExternalLocationUri(Uri(external_location.wstring()));
        return wait(manager.AddPackageByUriAsync(Uri(msix.wstring()), options));
    } catch (winrt::hresult_error const& e) {
        return {e.code(), e.message().c_str()};
    }
}

Outcome disable(const std::wstring& full_name) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        PackageManager manager;
        return wait(manager.RemovePackageAsync(full_name));
    } catch (winrt::hresult_error const& e) {
        return {e.code(), e.message().c_str()};
    }
}

}  // namespace unlocker::ui::shell_package
