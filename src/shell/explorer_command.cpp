// Kontextmenü-Eintrag "Mit Unlocker entsperren" (IExplorerCommand).
// Wird im neuen Windows-11-Menü über das Sparse Package (packaging/AppxManifest.xml) registriert,
// im klassischen Menü über packaging/register-classic-menu.ps1. Die DLL übergibt nur die Pfade
// an unlocker.exe und bleibt ansonsten passiv.

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl_core.h>

#pragma warning(push)
#pragma warning(disable : 4324)  // WRL-Modulspeicher wird wegen alignas aufgefüllt – harmlos
#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/module.h>
#pragma warning(pop)

#include <string>

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::InProc;
using Microsoft::WRL::Module;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace {

HMODULE g_module = nullptr;

std::wstring module_directory() {
    std::wstring path(32768, L'\0');
    const DWORD len = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(len);
    const auto pos = path.find_last_of(L'\\');
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

}  // namespace

class __declspec(uuid("CAD0BA06-5865-421D-8C66-5D339B9E8A62")) UnlockCommand final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand, IObjectWithSite> {
public:
    // IExplorerCommand
    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* name) override {
        return SHStrDupW(L"Mit Unlocker entsperren", name);
    }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override {
        return SHStrDupW((module_directory() + L"\\unlocker.exe,0").c_str(), icon);
    }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* tip) override {
        *tip = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetCanonicalName(GUID* guid) override {
        *guid = __uuidof(this);
        return S_OK;
    }
    IFACEMETHODIMP GetState(IShellItemArray*, BOOL, EXPCMDSTATE* state) override {
        *state = ECS_ENABLED;
        return S_OK;
    }
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override {
        if (!items) return E_INVALIDARG;

        DWORD count = 0;
        items->GetCount(&count);

        std::wstring args;
        for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item;
            if (FAILED(items->GetItemAt(i, &item))) continue;
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                if (!args.empty()) args += L' ';
                args += L'"';
                args += path;
                args += L'"';
                CoTaskMemFree(path);
            }
        }
        if (args.empty()) return S_OK;

        const std::wstring exe = module_directory() + L"\\unlocker.exe";
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOASYNC;
        info.lpFile = exe.c_str();
        info.lpParameters = args.c_str();
        info.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&info) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        *flags = ECF_DEFAULT;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override {
        *commands = nullptr;
        return E_NOTIMPL;
    }

    // IObjectWithSite
    IFACEMETHODIMP SetSite(IUnknown* site) override {
        site_ = site;
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** site) override {
        return site_ ? site_.CopyTo(riid, site) : E_FAIL;
    }

private:
    ComPtr<IUnknown> site_;
};

CoCreatableClass(UnlockCommand);

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    return Module<InProc>::GetModule().GetClassObject(rclsid, riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return Module<InProc>::GetModule().GetObjectCount() == 0 ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
