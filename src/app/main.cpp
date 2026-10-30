#include "instance.h"
#include "main_window.h"
#include "theme.h"
#include "unlocker/process.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <objidl.h>
#include <ole2.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <vector>
using std::max;
using std::min;
#include <gdiplus.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    std::vector<std::filesystem::path> targets;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) targets.emplace_back(argv[i]);
        LocalFree(argv);
    }

    // Mehrfachauswahl im klassischen Kontextmenü: ein Prozess pro Datei. Läuft schon ein sammelndes Fenster,
    // dorthin übergeben und beenden. Die Sperre hält, bis unser Fenster selbst sammelt (kein Wettlauf beim Start).
    const bool elevated = unlocker::process::is_elevated();
    unlocker::ui::instance::StartupLock startup;
    if (!targets.empty()) {
        startup.acquire(elevated);
        if (unlocker::ui::instance::forward(targets, elevated)) return 0;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    const HRESULT ole = OleInitialize(nullptr);  // COM (STA) + OLE-Drag&Drop

    ULONG_PTR gdiplus_token = 0;
    Gdiplus::GdiplusStartupInput gdiplus_input;
    Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr);

    unlocker::ui::theme::init();

    int exit_code = 0;
    {
        unlocker::ui::MainWindow window;
        if (!window.create(instance, show_cmd)) return 1;

        if (!targets.empty()) {
            window.open(std::move(targets));
            window.start_collecting();
        }
        startup.release();

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            // Tastenkürzel nur, wenn das Hauptfenster (nicht ein Dialog) aktiv ist
            if (GetActiveWindow() == window.hwnd() && TranslateAcceleratorW(window.hwnd(), window.accelerators(), &msg)) continue;
            if (IsDialogMessageW(window.hwnd(), &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exit_code = static_cast<int>(msg.wParam);
    }

    Gdiplus::GdiplusShutdown(gdiplus_token);
    if (SUCCEEDED(ole)) OleUninitialize();
    return exit_code;
}
