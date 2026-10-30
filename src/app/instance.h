#pragma once

#include "unlocker/win_handle.h"

#include <windows.h>

#include <filesystem>
#include <vector>

// Ziele einsammeln: Explorer startet das klassische Kontextmenü (MultiSelectModel=Player, "%1") einmal pro markierter
// Datei, praktisch gleichzeitig. Die erste Instanz öffnet das Fenster und nimmt kurz die Ziele der folgenden an
// (WM_COPYDATA); diese beenden sich danach. Spätere Starts öffnen wie gewohnt ein eigenes Fenster.
namespace unlocker::ui::instance {

constexpr wchar_t kWindowClass[] = L"UnlockerMainWindow";
constexpr ULONG_PTR kCopyDataTargets = 0x554C4B31;  // 'ULK1': UTF-16-Pfade, je mit L'\0' abgeschlossen
constexpr wchar_t kCollectProp[] = L"UnlockerCollect";  // am sammelnden Fenster: 1 = nicht erhöht, 2 = erhöht
constexpr UINT kCollectWindowMs = 1500;                 // so lange nach Start bzw. letztem Ziel wird gesammelt

inline HANDLE collect_prop_value(bool elevated) noexcept { return reinterpret_cast<HANDLE>(static_cast<INT_PTR>(elevated ? 2 : 1)); }

// Serialisiert den Start: wer die Sperre hält, legt gerade sein Fenster an. Erhöhte und normale Instanzen haben
// getrennte Sperren und sammeln nie füreinander.
class StartupLock {
public:
    StartupLock() = default;
    StartupLock(const StartupLock&) = delete;
    StartupLock& operator=(const StartupLock&) = delete;
    ~StartupLock() { release(); }

    // false, wenn die Sperre nicht binnen timeout_ms frei wurde – dann ohne Sperre weiter.
    bool acquire(bool elevated, DWORD timeout_ms = 3000);
    void release() noexcept;

private:
    WinHandle mutex_;
    bool owned_ = false;
};

// Übergibt die Ziele an ein sammelndes Fenster gleicher Erhöhung. true = angenommen, diese Instanz kann sich beenden.
bool forward(const std::vector<std::filesystem::path>& targets, bool elevated);

// Ziele aus einer WM_COPYDATA-Nachricht; leer, wenn die Nachricht nicht von uns stammt oder ungültig ist.
std::vector<std::filesystem::path> parse(const COPYDATASTRUCT& data);

}  // namespace unlocker::ui::instance
