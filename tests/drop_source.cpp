#include "drop_source.h"

#include <objidl.h>

#include <atomic>
#include <cstdio>

namespace dropsrc {
namespace {

std::atomic<std::size_t> g_qcd{0};

// IDropSource für den OLE-Drag im Selbsttest: Linke loslassen -> Drop, Esc -> Abbruch, sonst weiter.
// Ohne den DROP-Zweig laeuft der Drag endlos (DoDragDrop beendet ihn nie). Jeder QCD-Aufruf wird
// gezaeht und die ersten paar mit Zustand geloggt – so sieht man sofort, ob der OLE-Loop pumpt.
class DropSource final : public IDropSource {
public:
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IDropSource)) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --refs_;
        if (remaining == 0) delete this;
        return remaining;
    }

    // IDropSource: OLE fragt in jedem Drag-Loop-Durchlauf hier.
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override {
        const std::size_t n = g_qcd.fetch_add(1) + 1;
        if (n <= 3 || n % 25 == 0) {
            std::printf("  [qcd #%zu] esc=%d linke=%s\n", n, fEscapePressed,
                        (grfKeyState & MK_LBUTTON) ? "gedrueckt" : "los");
            std::fflush(stdout);
        }
        if (fEscapePressed) return DRAGDROP_S_CANCEL;
        if (!(grfKeyState & MK_LBUTTON)) return DRAGDROP_S_DROP;  // 0x0001, winuser.h
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return S_OK; }

private:
    ULONG refs_ = 1;
};

}  // namespace

IDropSource* make_drop_source() { return new DropSource; }

std::size_t qcd_calls() { return g_qcd.load(); }

}  // namespace dropsrc
