#pragma once

#include <windows.h>
#include <ole2.h>

#include <filesystem>
#include <vector>

// OLE-Drop-Ziel für Dateien (CF_HDROP). Anders als DragAcceptFiles gibt es Enter/Leave-Ereignisse,
// damit die Oberfläche beim Darüberziehen reagieren kann.
namespace unlocker::ui {

class DropHandler {
public:
    virtual ~DropHandler() = default;
    virtual void on_drag_over_changed(bool over) = 0;
    virtual void on_files_dropped(std::vector<std::filesystem::path> paths) = 0;
};

class DropTarget final : public IDropTarget {
public:
    explicit DropTarget(DropHandler& handler) : handler_(handler) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDropTarget
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD key_state, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD key_state, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD key_state, POINTL point, DWORD* effect) override;

private:
    static bool has_files(IDataObject* data);
    static std::vector<std::filesystem::path> paths_from(IDataObject* data);

    DropHandler& handler_;
    ULONG refs_ = 1;
    bool accepted_ = false;
};

}  // namespace unlocker::ui
