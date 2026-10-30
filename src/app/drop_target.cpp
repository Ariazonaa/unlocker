#include "drop_target.h"

#include <shellapi.h>

#include <cstdio>
#include <string>

namespace unlocker::ui {

namespace {
// Trace für den DnD-Selbsttest (tests/drop_self_test.cpp): UNLOCKER_DROPTARGET_TRACE=1 schreibt
// die Ereignisse auf stderr (wie UNLOCKER_TRACE im Kern). Normalbetrieb: aus, kein Overhead.
bool drop_target_tracing() {
    static const bool on = [] {
        wchar_t value[32]{};
        return GetEnvironmentVariableW(L"UNLOCKER_DROPTARGET_TRACE", value, 32) > 0 &&
               _wcsicmp(value, L"1") == 0;
    }();
    return on;
}
void dt_trace(const wchar_t* what) {
    if (!drop_target_tracing()) return;
    char narrow[256];
    WideCharToMultiByte(CP_UTF8, 0, what, -1, narrow, 256, nullptr, nullptr);
    std::fprintf(stderr, "[droptarget] %s\n", narrow);
    std::fflush(stderr);
}
}  // namespace

HRESULT DropTarget::QueryInterface(REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *out = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG DropTarget::AddRef() { return ++refs_; }

ULONG DropTarget::Release() {
    const ULONG remaining = --refs_;
    if (remaining == 0) delete this;
    return remaining;
}

bool DropTarget::has_files(IDataObject* data) {
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return data && data->QueryGetData(&fmt) == S_OK;
}

std::vector<std::filesystem::path> DropTarget::paths_from(IDataObject* data) {
    std::vector<std::filesystem::path> paths;
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (!data || FAILED(data->GetData(&fmt, &medium))) {
        dt_trace(L"paths_from: GetData fehlgeschlagen");
        return paths;
    }
    if (auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (drop_target_tracing()) {
            wchar_t line[64];
            swprintf(line, 64, L"paths_from: DragQueryFile count=%u", count);
            dt_trace(line);
        }
        std::wstring buffer(32768, L'\0');
        for (UINT i = 0; i < count; ++i) {
            const UINT len = DragQueryFileW(drop, i, buffer.data(), static_cast<UINT>(buffer.size()));
            if (len > 0) paths.emplace_back(buffer.substr(0, len));
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return paths;
}

HRESULT DropTarget::DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) {
    accepted_ = has_files(data);
    *effect = accepted_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (drop_target_tracing()) {
        wchar_t line[48];
        swprintf(line, 48, L"DragEnter (accepted=%d)", accepted_ ? 1 : 0);
        dt_trace(line);
    }
    if (accepted_) handler_.on_drag_over_changed(true);
    return S_OK;
}

HRESULT DropTarget::DragOver(DWORD, POINTL, DWORD* effect) {
    *effect = accepted_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT DropTarget::DragLeave() {
    dt_trace(L"DragLeave");
    if (accepted_) handler_.on_drag_over_changed(false);
    accepted_ = false;
    return S_OK;
}

HRESULT DropTarget::Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) {
    *effect = DROPEFFECT_NONE;
    if (accepted_) handler_.on_drag_over_changed(false);
    accepted_ = false;
    auto paths = paths_from(data);
    if (paths.empty()) return S_OK;
    *effect = DROPEFFECT_COPY;
    handler_.on_files_dropped(std::move(paths));
    return S_OK;
}

}  // namespace unlocker::ui
