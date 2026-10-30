// Drag & Drop-Selbsttest: startet unlocker.exe (leer) und faehrt einen echten OLE-Drag einer Datei
// (CF_HDROP) mit synthetischer Maus auf das Fenster – der OLE-Drag-Manager ruft die IDropTarget von
// unlocker.exe auf (DragEnter -> Akzentrahmen + "Loslassen zum Pruefen", Drop -> Ziel setzen). Daraus
// prueft der Test die Window-Prop "UnlockerDragOver" (1 waehrend des Drags, 0 danach), den Drop-Effekt
// (COPY) und das gesetzte Ziel (Titel). Screenshots fuer die manuelle Gegenpruefung per Screen-BitBlt
// (PrintWindow liefert bei diesem Fenster schwarze Bilder – es malt nur in WM_PAINT).
//
// Architektur (exakt das Explorer-Muster; Erkenntnisse aus Sandbox-/Host-Betrieb):
//  - Der OLE-Drag-Loop ist MESSAGE-getrieben: er pumpt die Queue des Threads, auf dem DoDragDrop laeuft,
//    und injizierte Maus-Events (SendInput) landen in der Queue des Threads, der das VORDERGRUND-FENSTER
//    besitzt. Deshalb laeuft der Zieh-Thread einen normalen Message-Loop mit einem kleinen eigenen
//    Fenster (16x16, WS_EX_TOOLWINDOW) am Punkt, an dem der Knopf gedrueckt wird – und der WndProc
//    ruft beim WM_LBUTTONDOWN DoDragDrop auf, genau wie der Explorer. Der erste Input-Event (das
//    LBUTTONDOWN am Fenster selbst) muss in diese Queue, sonst bleibt der Loop tot (keine QCDs, kein
//    DragEnter). Waere stattdessen das Ziel-Fenster (unlocker.exe, fremder Prozess) im Vordergrund,
//    landen die Events in dessen Queue – genau das ist zuerst passiert.
//  - Das IDataObject lebt im STA des Zieh-Threads (OleInitialize, nicht reines CoInitializeEx(STA) –
//    sonst CO_E_NOTINITIALIZED); der modale Loop pumpt die Queue, in der die interprozessualen
//    COM-Callbacks auf das Objekt (QueryGetData/GetData) landen. GetData gibt immer eine KOPIE des
//    HGLOBALs raus (OLE ruft GetData mehrmals vor dem Drop; die einzige Ref abzugeben lieferte
//    leere Daten und drag_effect=0).
//  - Die Maus-Choreografie laeuft PARALLEL auf dem Haupt-Thread (SendInput ist thread-sicher): Cursor
//    auf das Quellen-Fenster, Knopf runter (startet den Drag), in die Kartenmitte schwenken
//    (DragEnter), hin und her (DragOver), loslassen (Drop). Der Drop-Event (Release) wird vom Loop
//    direkt gesehen.
//  - DoDragDrop-Erfolg ist DRAGDROP_S_DROP (0x40100), NICHT S_OK.
//  - Watchdog: 15 s nach Release ohne Ende -> Esc injizieren (QueryContinueDrag meldet
//    DRAGDROP_S_CANCEL), der Drag endet sauber und die Diagnoseausgabe (QCD-Zaehler) ist komplett.
//
// Aufruf: drop_self_test <unlocker.exe> <datei> [out-ordner]

#include "drop_source.h"

#include <objidl.h>
#include <shlobj.h>
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Datenobjekt für den OLE-Drag: die ECHTE Shell-Datei – genau das, was der Explorer beim Ziehen
// bereitstellt. WICHTIG: SHCreateDataObject(NULL-Ordner, absoluteItemid) erzeugt in Win11/24H2
// ein LEERES Objekt (EnumFormatEtc liefert 0 Formate, QueryGetData(CF_HDROP)=S_FALSE) – der
// DnD-Manager uebertraegt dann nichts, das Ziel akzeptiert nicht (DragLeave statt Drop,
// drag_effect=0). Die verlaessliche Variante ist IShellItem::BindToHandler(BHID_DataObject),
// der dokumentierte Weg, um das Shell-Dateidatenobjekt zu bekommen. (Aufruf auf dem STA-Thread,
// der DoDragDrop macht.)
IDataObject* make_file_dataobject(const std::wstring& path) {
    IShellItem* item = nullptr;
    const HRESULT hr_item =
        SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(&item));
    std::printf("  [dataobj] SHCreateItemFromParsingName=0x%lx, item=%p\n", (unsigned long)hr_item,
                (void*)item);
    if (FAILED(hr_item) || !item) return nullptr;

    IDataObject* obj = nullptr;
    const HRESULT hr_bind = item->BindToHandler(nullptr, BHID_DataObject, IID_IDataObject,
                                                reinterpret_cast<void**>(&obj));
    std::printf("  [dataobj] BindToHandler(BHID_DataObject)=0x%lx, obj=%p\n", (unsigned long)hr_bind,
                (void*)obj);
    item->Release();
    std::fflush(stdout);
    if (FAILED(hr_bind) || !obj) return nullptr;
    // Kontrolle: das Objekt MUSS CF_HDROP anbieten – sonst akzeptiert das Ziel den Drag nie
    // (DragEnter rejected -> DragLeave statt Drop). War der eigentliche Blocker dieses Tests.
    FORMATETC hdrop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (obj->QueryGetData(&hdrop) != S_OK) {
        std::printf("  [dataobj] FEHLER: Objekt bietet CF_HDROP nicht an (Shell-Bug?)\n");
        std::fflush(stdout);
        obj->Release();
        return nullptr;
    }
    return obj;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++failures;
}

void set_cursor(int x, int y) {
    SetCursorPos(x, y);
}
void mouse_down() {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(input));
}
void mouse_up() {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(input));
}
// Esc injizieren (Watchdog): der OLE-Loop sieht das per fEscapePressed und bricht sauber ab.
void press_escape() {
    INPUT input[2]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_ESCAPE;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = VK_ESCAPE;
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, input, sizeof(input));
}
// In kleinen Schritten bewegen, damit der OLE-Loop jede Etappe sieht.
void move_steps(int x0, int y0, int x1, int y1, int steps, int ms) {
    for (int i = 1; i <= steps; ++i) {
        set_cursor(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
        Sleep(ms);
    }
}

void dump_mouse(const char* tag) {
    const SHORT lb = GetAsyncKeyState(VK_LBUTTON);
    POINT p{};
    GetCursorPos(&p);
    std::printf("  [maus %s] linke=%s pos=(%d,%d)\n", tag, (lb & 0x8000) ? "gedrueckt" : "los", p.x, p.y);
    std::fflush(stdout);
}

DWORD prop_int(HWND hwnd, const wchar_t* name) { return (DWORD)(INT_PTR)GetPropW(hwnd, name); }
int wait_prop(HWND hwnd, const wchar_t* name, DWORD want, DWORD ms) {
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < ms) {
        if (prop_int(hwnd, name) == want) return (int)(GetTickCount() - start);
        Sleep(50);
    }
    return -1;
}
std::wstring get_text(HWND hwnd) {
    std::wstring buffer(512, L'\0');
    buffer.resize(GetWindowTextW(hwnd, buffer.data(), (int)buffer.size()));
    return buffer;
}

// Punkt wählen, der auf dem virtuellen Bildschirm ist, aber NICHT im Fenster rc – damit DragEnter beim
// Betreten des Fensters sicher ausloest (OLE feuert DragEnter nur beim Ueberqueren, nicht beim Start darin).
POINT pick_outside(const RECT& rc) {
    const int vl = GetSystemMetrics(SM_XVIRTUALSCREEN), vt = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vr = vl + GetSystemMetrics(SM_CXVIRTUALSCREEN), vb = vt + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const POINT cands[] = {
        {rc.left - 80, rc.top + 30},   {rc.right + 80, rc.top + 30},
        {rc.left + 30, rc.top - 80},   {rc.left + 30, rc.bottom + 80},
        {vl + 8, vt + 8},              {vl + 8, vb - 8},
        {vr - 8, vt + 8},              {vr - 8, vb - 8},
    };
    for (const POINT& c : cands) {
        const bool on_screen = c.x > vl && c.x < vr && c.y > vt && c.y < vb;
        const bool outside = c.x < rc.left - 2 || c.x > rc.right + 2 || c.y < rc.top - 2 || c.y > rc.bottom + 2;
        if (on_screen && outside) return c;
    }
    return {vl + 8, vt + 8};  // Fallback
}

// Screen-Capture des Fensters als BMP in den Out-Ordner (fuer die manuelle Gegenpruefung).
// Das Fenster malt seinen Inhalt (inkl. Akzentrahmen beim Drag) NUR in WM_PAINT und beantwortet
// WM_PRINTCLIENT nicht – PrintWindow liefert daher schwarze/leere Bilder. Stattdessen BitBlt vom
// echten Screen-DC (wie die gui-*-Sandbox-Tests per CopyFromScreen): so sieht man exakt das, was
// auf dem Bildschirm sichtbar ist. BMP statt PNG: kein GDI+ noetig (crasht in manchen Sitzungen),
// 32-Bit-DIB direkt geschrieben.
void screenshot(const std::string& dir, const char* name, HWND hwnd) {
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    const BOOL ok = BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> pixels(static_cast<size_t>(w) * h * 4);
    const BOOL dib = GetDIBits(mem, bmp, 0, h, pixels.data(), &bi, DIB_RGB_COLORS) > 0;
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(bmp);
    ReleaseDC(nullptr, screen);

    const std::wstring path = std::filesystem::path(to_wide(dir)) / (to_wide(name) + L".bmp");
    BYTE fhdr[14]{};
    fhdr[0] = 'B';
    fhdr[1] = 'M';
    const DWORD offbits = 14 + sizeof(BITMAPINFOHEADER);
    const DWORD size32 = offbits + (DWORD)pixels.size();
    std::memcpy(fhdr + 2, &size32, 4);
    std::memcpy(fhdr + 10, &offbits, 4);
    {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            std::printf("  screenshot %s.bmp: Datei nicht oeffenbar\n", name);
            return;
        }
        out.write(reinterpret_cast<const char*>(fhdr), 14);
        out.write(reinterpret_cast<const char*>(&bi.bmiHeader), sizeof(BITMAPINFOHEADER));
        out.write(reinterpret_cast<const char*>(pixels.data()), (std::streamsize)pixels.size());
    }
    std::printf("  screenshot %s.bmp (Screen-BitBlt %s, DIB %s, %dx%d)\n", name, ok ? "ok" : "fehlgeschlagen",
                dib ? "ok" : "fehlgeschlagen", w, h);
    std::fflush(stdout);
}

// Kontext, den das Quellen-Fenster über GWLP_USERDATA trägt (WndProc braucht Zugriff auf
// Dateipfad und Synchronisations-Handles).
struct DragContext {
    std::wstring file;  // Pfad der zu ziehenden Datei; Datenobjekt wird im STA des Zieh-Threads gebaut
    HWND src_hwnd = nullptr;  // Quellen-Fenster (vom Zieh-Thread gesetzt)
    HANDLE window_ready_evt = nullptr;  // Fenster steht, Choreografie darf starten
    std::atomic<bool> drag_started{false};  // gegen Doppelstart (Input + Post)
    std::atomic<bool> drag_done{false};
    std::atomic<bool> drag_attempted{false};
    DWORD drag_rv = 0;
    DWORD drag_effect = 0;
};

// Win32-Fenster des Quell-Prozesses – sichtbares, kleines Fenster, das der Zieh-Thread pumpt.
// DoDragDrop laeuft NICHT hier im WndProc (das WM_LBUTTONDOWN-Routing ueber reale SendInput-Events
// ist zu zuverlaessigkeits-sensibel: der 16x16-Presspunkt trifft mal das Fenster, mal nicht).
// Stattdessen: Haupt-Thread drueckt physikalisch (QCD braucht den echten Tastenzustand), wartet
// bis GetAsyncKeyState das bekraeftigt, und setzt dann button_down_evt – der Zieh-Thread ruft
// DoDragDrop direkt auf (OLE uebernimmt danach selbst die Maussteuerung).
LRESULT CALLBACK src_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Aktiv lassen, damit die injizierte Eingabe hier bleibt (nicht wegaktiviert werden).
    if (msg == WM_MOUSEACTIVATE) return MA_ACTIVATE;
    if (msg == WM_LBUTTONDOWN) {
        auto* ctx = reinterpret_cast<DragContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        // Deterministischer Start: der reale Knopfdruck ODER die Sicherheit-PostMessage (Haupt-Thread)
        // koennen ankommen – der CAS stellt sicher, dass DoDragDrop genau einmal startet.
        bool expected = false;
        if (ctx && ctx->drag_started.compare_exchange_strong(expected, true)) {
            const SHORT lb = GetAsyncKeyState(VK_LBUTTON);
            std::printf("  [wndproc] DoDragDrop startet (physikalische linke=%s)\n",
                        (lb & 0x8000) ? "gedrueckt" : "los");
            std::fflush(stdout);
            // ECHTES Shell-Dateidatenobjekt (IShellItem::BindToHandler(BHID_DataObject)) – im STA
            // dieses Threads bauen, weil OLE die interprozessualen GetData-Callbacks ueber dessen
            // Queue laeuft. Der DnD-Manager uebertraegt es prozessuebergreifend korrekt (ein
            // handgebautes HGLOBAL-Objekt kommt im Ziel als hGlobal=0 an; SHCreateDataObject(NULL,
            // absolutePidl) liefert 24H2 ein leeres Objekt – beide Varianten s. CHANGELOG).
            IDataObject* object = make_file_dataobject(ctx->file);
            if (!object) {
                ctx->drag_done.store(true);
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            IDropSource* source = dropsrc::make_drop_source();
            ctx->drag_attempted.store(true);
            ctx->drag_rv = DoDragDrop(object, source, DROPEFFECT_COPY, &ctx->drag_effect);
            ctx->drag_done.store(true);
            std::printf("  [wndproc] DoDragDrop fertig: rv=0x%lx, qcd=%zu\n", (unsigned long)ctx->drag_rv,
                        dropsrc::qcd_calls());
            std::fflush(stdout);
            source->Release();
            object->Release();  // unsere Referenz; OLE loescht das Objekt bei der letzten Release
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    if (msg == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Vordergrund setzen, inklusive Fallback: ein Thread darf das Vordergrund-Fenster nur wechseln, wenn er
// die letzte Eingabe bekommen hat – AttachThreadInput hebt die Sperre (Klassiker).
void force_foreground(HWND src) {
    for (int i = 0; i < 20; ++i) {
        if (GetForegroundWindow() == src) return;
        SetForegroundWindow(src);
        const HWND fg = GetForegroundWindow();
        if (fg && fg != src) {
            DWORD other = 0, own = GetCurrentThreadId();
            if (GetWindowThreadProcessId(fg, &other) && other != own) {
                AttachThreadInput(own, other, TRUE);
                SetForegroundWindow(src);
                AttachThreadInput(own, other, FALSE);
            }
        }
        Sleep(50);
    }
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);  // kein Puffer: bei Crash nichts verlieren
    if (argc < 3) {
        std::printf("Aufruf: drop_self_test <unlocker.exe> <datei> [out-ordner]\n");
        return 2;
    }
    const std::wstring exe = to_wide(argv[1]);
    const std::wstring file = to_wide(argv[2]);
    const std::string out_dir = argc > 3 ? std::string(argv[3]) : ".";

    // Diagnose: Ziel-stdout/stderr (GUI-Subsystem, sonst kein Konsolenausgang) auf eine Datei
    // umleiten, damit die [droptarget]-Traces sichtbar werden. Handle muss bInheritHandle haben.
    HANDLE z_out = INVALID_HANDLE_VALUE, z_err = INVALID_HANDLE_VALUE;
    const std::wstring zout_path = std::filesystem::path(to_wide(out_dir)) / L"unlocker-stdout.txt";
    const std::wstring zerr_path = std::filesystem::path(to_wide(out_dir)) / L"unlocker-stderr.txt";
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        z_out = CreateFileW(zout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        z_err = CreateFileW(zerr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    const bool have_redirect = (z_out != INVALID_HANDLE_VALUE) && (z_err != INVALID_HANDLE_VALUE);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (have_redirect) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = z_out;
        si.hStdError = z_err;
    }
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, have_redirect, CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi)) {
        std::printf("FAIL unlocker.exe nicht gestartet (Fehler %lu)\n", GetLastError());
        return 2;
    }
    if (z_out != INVALID_HANDLE_VALUE) CloseHandle(z_out);
    if (z_err != INVALID_HANDLE_VALUE) CloseHandle(z_err);
    HWND hwnd = nullptr;
    for (int i = 0; i < 100 && !hwnd; ++i) {
        hwnd = FindWindowW(L"UnlockerMainWindow", nullptr);
        if (!hwnd) Sleep(50);
    }
    if (!hwnd) {
        std::printf("FAIL Hauptfenster nicht gefunden\n");
        TerminateProcess(pi.hProcess, 0);
        return 2;
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    // bewusst NICHT SetForegroundWindow: das Ziel-Fenster im Vordergrund würde die injizierten
    // Maus-Events in dessen Queue leiten – der OLE-Loop im Quell-Prozess bekäme nichts (keine QCDs).
    check(prop_int(hwnd, L"UnlockerDragOver") == 0, "Start: DragOver-Prop = 0 (kein Drag)");
    screenshot(out_dir, "drag-empty", hwnd);

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const POINT center{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};  // Kartenmitte (leerer Zustand)
    const POINT start = pick_outside(rc);  // hier steht das Quellen-Fenster, hier wird gedrueckt

    // Das Datenobjekt (Shell-Dateidatenobjekt) wird im STA des Zieh-Threads gebaut – es darf nicht
    // im Haupt-Thread entstehen, weil OLE es dort registriert und die GetData-Callbacks ueber die
    // Queue des STA laufen, in dem es erzeugt wurde.
    DragContext ctx;
    ctx.file = file;
    ctx.window_ready_evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> t_enter{-1};
    std::atomic<bool> over_held{false};

    // Zieh-Thread: OleInitialize(STA), Quellen-Fenster am Presspunkt (start.x/start.y), dann
    // Message-Loop. Der injizierte Knopfdruck wird als WM_LBUTTONDOWN hier dispatcht und startet
    // darin DoDragDrop – exakt wie der Explorer. Das Fenster muss groß genug sein, damit der
    // Cursor-Mittelpunkt sicher darin liegt (16x16 px, unsichtbar hell – WS_EX_TOOLWINDOW gegen Taskbar).
    std::thread drag_thread([&] {
        const HRESULT hr_co = OleInitialize(nullptr);
        std::printf("  [zieh-thread] OleInitialize=%lx\n", (unsigned long)hr_co);

        const wchar_t* k_class = L"UnlockerDragSource";
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = src_wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = k_class;
        RegisterClassExW(&wc);

        const int w = 16;
        // WS_EX_TOPMOST: ueber Shell-Overlays (z. B. Shell_SystemDim von offenen Flyouts), die
        // sonst den Presspunkt abdecken und den physischen Klick abfangen.
        HWND src = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, k_class, L"unlocker drag source",
                                   WS_POPUP | WS_VISIBLE, start.x - w / 2, start.y - w / 2, w, w, nullptr,
                                   nullptr, GetModuleHandleW(nullptr), &ctx);
        std::printf("  [zieh-thread] Quellen-Fenster=%p (Fehler %lu) bei (%d,%d)\n", (void*)src,
                    GetLastError(), start.x - w / 2, start.y - w / 2);
        std::fflush(stdout);
        if (!src) {
            SetEvent(ctx.window_ready_evt);
            ctx.drag_done.store(true);
            if (SUCCEEDED(hr_co)) OleUninitialize();
            return;
        }
        SetWindowLongPtrW(src, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ctx));
        ctx.src_hwnd = src;
        force_foreground(src);
        SetEvent(ctx.window_ready_evt);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::printf("  [zieh-thread] Message-Loop beendet, OleUninitialize...\n");
        std::fflush(stdout);
        if (SUCCEEDED(hr_co)) OleUninitialize();  // Gegenstück zu OleInitialize (nicht CoUninitialize)
        std::printf("  [zieh-thread] OleUninitialize fertig\n");
        std::fflush(stdout);
    });

    // Haupt-Thread: Choreografie. Cursor auf das Quellen-Fenster, Knopf runter -> dort dispatcht
    // der Zieh-Thread WM_LBUTTONDOWN und startet DoDragDrop.
    WaitForSingleObject(ctx.window_ready_evt, 10000);
    Sleep(200);
    set_cursor(start.x, start.y);
    Sleep(50);
    // Ziel-Fenster in den Topmost-Band nach vorne heben (NUR Z-Ordnung, kein SetForegroundWindow!):
    // Fremde topmoste Overlays (Browser & Co.) liegen sonst ueber der Drop-Position und der
    // OLE-Hit-Test findet das Ziel beim Draeuschieben nie. Wer zuletzt SetWindowPos(TOPMOST)
    // ruft, liegt im Topmost-Band oben.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    mouse_down();
    for (int i = 0; i < 20 && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000); ++i) Sleep(20);
    dump_mouse("start");

    // Warten, bis der OLE-Loop via dem realen WM_LBUTTONDOWN gestartet ist (QCD wird aufgerufen).
    const DWORD qcd_wait = GetTickCount();
    while (dropsrc::qcd_calls() == 0 && !ctx.drag_attempted.load() && GetTickCount() - qcd_wait < 1500)
        Sleep(25);
    std::printf("  [haupt-thread] OLE-Loop aktiv: %s (qcd=%zu nach %lu ms)\n",
                dropsrc::qcd_calls() > 0 ? "ja" : "NEIN", dropsrc::qcd_calls(), GetTickCount() - qcd_wait);
    std::fflush(stdout);

    // Deterministischer Fallback: falls der reale Knopfdruck nicht als WM_LBUTTONDOWN in der
    // Drag-Queue angekommen ist (Input-Routing), das Fenster selbst benachrichtigen – der Knopf
    // ist zu diesem Zeitpunkt noch physikalisch gedrueckt, QCD sieht also MK_LBUTTON. Der CAS im
    // WndProc stellt sicher, dass der Drag genau einmal startet.
    if (dropsrc::qcd_calls() == 0 && !ctx.drag_attempted.load() && ctx.src_hwnd) {
        std::printf("  [haupt-thread] Fallback: WM_LBUTTONDOWN gepostet\n");
        PostMessageW(ctx.src_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKEWPARAM(8, 8));
        const DWORD fb_wait = GetTickCount();
        while (dropsrc::qcd_calls() == 0 && !ctx.drag_attempted.load() && GetTickCount() - fb_wait < 3000)
            Sleep(25);
        std::printf("  [haupt-thread] nach Fallback: qcd=%zu, attempted=%d\n", dropsrc::qcd_calls(),
                    (int)ctx.drag_attempted.load());
        std::fflush(stdout);
        if (dropsrc::qcd_calls() == 0 && !ctx.drag_attempted.load()) {
            // Drag ist nicht gestartet (Input-Routing + Fallback wirkungslos) – sauber abbrechen:
            // Knopf loslassen, Quellen-Fenster schliessen, damit der Zieh-Thread aussteigt.
            std::printf("  [haupt-thread] Drag nicht gestartet – Test wird abgebrochen\n");
            mouse_up();
            PostMessageW(ctx.src_hwnd, WM_CLOSE, 0, 0);
        }
    }

    const bool drag_started_ok = ctx.drag_attempted.load();
    if (!drag_started_ok) {
        // Kein Drag: Rest der Choreografie ueberspringen (Keine Mausbewegungen, kein Drop).
        drag_thread.join();
        CloseHandle(ctx.window_ready_evt);
        check(false, "OLE-Drag konnte nicht gestartet werden (Input-Routing)");
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        WaitForSingleObject(pi.hProcess, 3000);
        DWORD ec = 0;  // WICHTIG: nullptr als lpExitCode fuehrt zu einem Write-Access-Violation
        if (GetExitCodeProcess(pi.hProcess, &ec) && ec == STILL_ACTIVE) TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    // Ins Fenster schwenken -> DragEnter (Prop 1, Akzentrahmen).
    move_steps(start.x, start.y, center.x, center.y, 40, 12);
    dump_mouse("eingelassen");
    t_enter = wait_prop(hwnd, L"UnlockerDragOver", 1, 4000);
    std::printf("  DragOver %s\n", t_enter >= 0 ? (std::to_string(t_enter) + " ms aktiv").c_str() : "nie gemeldet");
    std::fflush(stdout);

    if (t_enter >= 0) {
        // Im Inneren schwenken (wiederholtes DragOver), Prop muss halten.
        move_steps(center.x, center.y, center.x + 60, center.y, 8, 12);
        move_steps(center.x + 60, center.y, center.x - 60, center.y, 16, 12);
        move_steps(center.x - 60, center.y, center.x, center.y, 8, 12);
        over_held = (prop_int(hwnd, L"UnlockerDragOver") == 1);
        check(over_held, "DragOver-Prop bleibt 1 waehrend des Drags");
        // Kurzes Einblenden lassen: on_drag_over_changed ruft InvalidateRect (asynchron) – der
        // Akzentrahmen muss erst WM_PAINT bekommen, sonst zeigt der Screenshot den alten Zustand.
        Sleep(250);
        screenshot(out_dir, "drag-over", hwnd);
    }

    // Loslassen -> Drop -> Ziel gesetzt.
    mouse_up();
    dump_mouse("losgelassen");

    // Watchdog: 15 s nach Release ohne Ende -> Esc (sauberer Abbruch), danach noch 10 s warten.
    // Nur wenn der Drag tatsaechlich gestartet wurde – sonst haengt er hier umsonst.
    const DWORD up_time = GetTickCount();
    bool escaped = false;
    if (ctx.drag_attempted.load()) {
        for (;;) {
            if (ctx.drag_done.load()) break;
            if (!escaped && GetTickCount() - up_time > 15000) {
                std::printf("  [watchdog] 15 s nach Release ohne Ende – Esc injiziert\n");
                press_escape();
                escaped = true;
            }
            if (GetTickCount() - up_time > 25000) break;
            Sleep(250);
        }
    }
    drag_thread.join();
    CloseHandle(ctx.window_ready_evt);
    std::printf("  [exit] 1: Zieh-Thread joined\n");
    std::fflush(stdout);

    const DWORD drag_rv = ctx.drag_rv;
    const DWORD drag_effect = ctx.drag_effect;
    const bool drag_attempted = ctx.drag_attempted.load();
    check(t_enter >= 0, "DragEnter: DragOver-Prop = 1 (Akzentrahmen aktiv)");
    const bool cancelled = drag_rv == DRAGDROP_S_CANCEL;
    const bool dropped = drag_rv == DRAGDROP_S_DROP;
    char rv_buf[32];
    std::snprintf(rv_buf, sizeof(rv_buf), "0x%lx", (unsigned long)drag_rv);
    check(drag_attempted && (drag_rv == S_OK || dropped),
          std::string("DoDragDrop beendet mit Drop (rv=") + rv_buf + (cancelled ? ", abgebrochen!" : ")"));
    check(drag_attempted && drag_effect == DROPEFFECT_COPY, "Drop-Effekt COPY (Ziel akzeptiert Dateien)");
    std::printf("  drag_effect=0x%lx\n", (unsigned long)drag_effect);

    // Drop ruft on_files_dropped -> open(): Titel traegt den Dateinamen, DragOver-Prop zurueck auf 0.
    check(wait_prop(hwnd, L"UnlockerDragOver", 0, 5000) >= 0, "nach Drop: DragOver-Prop zurueck auf 0");
    const std::wstring name = file.substr(file.find_last_of(L"\\/") + 1);
    int t_title = -1;
    for (int i = 0; i < 100; ++i) {
        if (get_text(hwnd).find(name) != std::wstring::npos) {
            t_title = i * 50;
            break;
        }
        Sleep(50);
    }
    check(t_title >= 0, "Titel enthaelt den abgelegten Dateinamen");
    std::printf("  Titel: %ls\n", get_text(hwnd).c_str());

    Sleep(900);  // Ziel-Ansicht einblenden lassen
    screenshot(out_dir, "drag-dropped", hwnd);

    check(failures == 0, "keine Fehler");
    std::printf("%s\n", failures ? "SELBFT-TEST FEHLGESCHLAGEN" : "SELBFT-TEST OK");
    std::printf("  [exit] 2: alle Checks fertig\n");
    std::fflush(stdout);

    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    std::printf("  [exit] 3: unlocker beendet\n");
    std::fflush(stdout);
    DWORD ec = 0;  // WICHTIG: nullptr als lpExitCode fuehrt zu einem Write-Access-Violation
    if (GetExitCodeProcess(pi.hProcess, &ec) && ec == STILL_ACTIVE) TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::printf("  [exit] 4: main kehrt zurueck\n");
    std::fflush(stdout);
    return failures ? 1 : 0;
}
