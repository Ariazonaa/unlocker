#include "main_window.h"

#include "dialog.h"
#include "instance.h"
#include "resource.h"
#include "settings.h"
#include "shell_package.h"
#include "strings.h"
#include "theme.h"
#include "unlocker/errors.h"
#include "unlocker/file_ops.h"
#include "unlocker/path_util.h"
#include "unlocker/process.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <windowsx.h>

#include <algorithm>
#include <format>
#include <numeric>
#include <optional>
#include <thread>

namespace unlocker::ui {
namespace {

using i18n::S;
using i18n::tr;

// std::make_wformat_args verlangt lvalues – benannte Parameter sind welche.
template <typename... Args>
std::wstring fmt(S id, Args&&... args) {
    return std::vformat(tr(id), std::make_wformat_args(args...));
}

constexpr const wchar_t* kClassName = instance::kWindowClass;
constexpr UINT WM_APP_SCAN_FAST = WM_APP + 1;      // wParam = Generation, lParam = ScanResult*
constexpr UINT WM_APP_SCAN_ENRICHED = WM_APP + 2;
constexpr UINT WM_APP_SHELL_DONE = WM_APP + 3;     // lParam = ShellToggleDone*
constexpr UINT WM_APP_SCAN_TARGETS = WM_APP + 4;   // wParam = Generation, lParam = vector<path>* (Ziele in Langform)
constexpr UINT_PTR kTimerProgress = 1;
constexpr UINT_PTR kTimerReveal = 2;
constexpr UINT_PTR kTimerCollect = 3;

constexpr int kMinWidth = 800;
constexpr int kMinHeight = 460;
constexpr int kMargin = 24;
constexpr int kGap = 8;
constexpr int kTitleHeight = 56;      // Mica-Zone
constexpr int kSubtitleHeight = 32;   // Pfad / Chips
constexpr int kProgressTop = 92;
constexpr int kCardTop = 100;
constexpr int kStatusHeight = 28;
constexpr int kButtonHeight = 34;
constexpr int kCardRadius = 8;
constexpr int kRowHeight = 30;
constexpr int kChipHeight = 24;
constexpr std::size_t kGroupThreshold = 3;     // ab so vielen Einträgen eines Prozesses wird gruppiert
constexpr std::size_t kCollapseThreshold = 10; // größere Gruppen starten eingeklappt
constexpr int kRevealRowMs = 160;
constexpr int kRevealStaggerMs = 14;
constexpr int kRevealMaxStaggerRows = 20;

constexpr wchar_t kGlyphOpen[] = L"\uE8E5";
constexpr wchar_t kGlyphUnlock[] = L"\uE785";
constexpr wchar_t kGlyphKill[] = L"\uE711";
constexpr wchar_t kGlyphDelete[] = L"\uE74D";
constexpr wchar_t kGlyphShield[] = L"\uEA18";
constexpr wchar_t kGlyphLock[] = L"\uE72E";
constexpr wchar_t kGlyphMore[] = L"\uE712";
constexpr wchar_t kGlyphDrop[] = L"\uE896";
constexpr wchar_t kGlyphClose[] = L"\uE711";
constexpr wchar_t kGlyphExpanded[] = L"\uE70D";
constexpr wchar_t kGlyphCollapsed[] = L"\uE76C";

enum Column : int { ColProcess = 0, ColPid, ColKind, ColPath };

struct ShellToggleDone {
    bool enabled = false;  // Ziel des Vorgangs: aktivieren (true) oder entfernen
    shell_package::Outcome outcome;
};

std::filesystem::path exe_directory() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
}

std::wstring error_text(const std::error_code& ec) {
    if (ec.category() == error_category()) {
        switch (static_cast<Error>(ec.value())) {
            case Error::critical_process: return tr(S::ErrCriticalProcess);
            case Error::protected_handle: return tr(S::ErrProtectedHandle);
            case Error::protected_process: return tr(S::ErrProtectedProcess);
            case Error::not_supported: return tr(S::ErrNotSupported);
            case Error::system_held: return tr(S::ErrSystemHeld);
            case Error::still_locked: return tr(S::ErrStillLocked);
            case Error::no_process: return tr(S::ErrNoProcess);
        }
    }
    if (ec.category() != std::system_category()) {
        const auto msg = ec.message();
        return std::wstring(msg.begin(), msg.end());
    }
    wchar_t* buffer = nullptr;
    const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, static_cast<DWORD>(ec.value()), 0, reinterpret_cast<LPWSTR>(&buffer), 0,
                                     nullptr);
    std::wstring text = len ? std::wstring(buffer, len) : std::format(L"Fehler {}", ec.value());
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text;
}

const wchar_t* kind_text(LockKind kind) {
    switch (kind) {
        case LockKind::FileHandle: return tr(S::KindHandle);
        case LockKind::MappedImage: return tr(S::KindModule);
        case LockKind::WorkingDirectory: return tr(S::KindCwd);
        case LockKind::RemoteOpen: return tr(S::KindRemote);
        case LockKind::SystemHeld: return tr(S::KindSystem);
        case LockKind::Unattributed: return tr(S::KindUnattributed);
        default: return tr(S::KindProcess);
    }
}

// Sperre ohne Prozess (Unattributed, pid 0): "Unbekannt" und keine PID statt "PID 0".
std::wstring display_name(const Locker& l) {
    if (l.kind == LockKind::Unattributed) return tr(S::Unknown);
    return l.process_name.empty() ? std::format(L"PID {}", l.pid) : l.process_name;
}

std::wstring pid_text(const Locker& l) { return l.kind == LockKind::Unattributed ? L"–" : std::to_wstring(l.pid); }

// Freigabe-Zugriff: "über Freigabe: <Benutzer> [von <Client>]"; leer, wenn kein Benutzer bekannt ist.
std::wstring remote_text(const Locker& l) {
    if (l.remote_user.empty()) return {};
    return l.remote_client.empty() ? fmt(S::RemoteUser, l.remote_user)
                                   : fmt(S::RemoteUserFrom, l.remote_user, l.remote_client);
}

// Zusatz hinter dem Prozessnamen: wer die Datei über die Freigabe offen hat bzw. Anzeigename laut Restart Manager.
std::wstring extra_text(const Locker& l) {
    if (!l.remote_user.empty()) return remote_text(l);
    return path_util::equals_ci(l.app_name, l.process_name) ? std::wstring() : l.app_name;
}

// Text auf Mica braucht Alpha – GDI-Text würde auf dem erweiterten Frame transparent.
void draw_text(HWND hwnd, HDC hdc, RECT rc, const std::wstring& text, HFONT font, COLORREF color, DWORD format,
               bool composited) {
    if (composited) {
        if (HTHEME theme = OpenThemeData(hwnd, L"CompositedWindow::Window")) {
            HDC mem = nullptr;
            BP_PAINTPARAMS params{};
            params.cbSize = sizeof(params);
            params.dwFlags = BPPF_ERASE;
            if (HPAINTBUFFER buffer = BeginBufferedPaint(hdc, &rc, BPBF_TOPDOWNDIB, &params, &mem)) {
                HGDIOBJ old = SelectObject(mem, font);
                DTTOPTS opts{};
                opts.dwSize = sizeof(opts);
                opts.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR;
                opts.crText = color;
                DrawThemeTextEx(theme, mem, 0, 0, text.c_str(), -1, format, &rc, &opts);
                SelectObject(mem, old);
                EndBufferedPaint(buffer, TRUE);
            }
            CloseThemeData(theme);
            return;
        }
    }
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    HGDIOBJ old = SelectObject(hdc, font);
    DrawTextW(hdc, text.c_str(), -1, &rc, format);
    SelectObject(hdc, old);
}

bool confirm(HWND owner, const controls::Fonts& fonts, const wchar_t* instruction, const std::wstring& content,
             const wchar_t* ok_text, bool warning) {
    return dialog::confirm(owner, instruction, content, ok_text, fonts,
                           warning ? dialog::Icon::Warning : dialog::Icon::Info);
}

void inform(HWND owner, const controls::Fonts& fonts, const wchar_t* instruction, const std::wstring& content,
            bool error) {
    dialog::inform(owner, instruction, content, fonts, error ? dialog::Icon::Error : dialog::Icon::Info);
}

std::wstring describe(const std::vector<Locker>& lockers) {
    std::wstring text;
    std::set<std::uint32_t> seen;
    std::set<std::wstring> seen_remote;
    for (const auto& l : lockers) {
        // Freigabe-Zugriffe laufen alle über PID 4 – wer betroffen ist, sagt erst Benutzer und Client
        if (l.kind == LockKind::RemoteOpen && !l.remote_user.empty()) {
            const std::wstring who = remote_text(l);
            if (!seen_remote.insert(who).second) continue;
            if (!text.empty()) text += L'\n';
            text += L"• " + who;
            continue;
        }
        if (!seen.insert(l.pid).second) continue;
        if (!text.empty()) text += L'\n';
        if (l.kind == LockKind::Unattributed) text += std::format(L"• {} ({})", tr(S::Unknown), l.locked_path);
        else text += std::format(L"• {} (PID {})", l.process_name.empty() ? tr(S::Unknown) : l.process_name, l.pid);
    }
    return text;
}

std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

std::wstring join_names_from(const std::vector<std::wstring>& paths, std::size_t max_items) {
    std::wstring text;
    for (std::size_t i = 0; i < paths.size() && i < max_items; ++i) {
        if (!text.empty()) text += L", ";
        text += std::filesystem::path(paths[i]).filename().wstring();
    }
    if (paths.size() > max_items) text += fmt(S::TargetsMore, paths.size() - max_items);
    return text;
}

HFONT make_font(UINT dpi, int pt, int weight, const wchar_t* face, const wchar_t* fallback = nullptr) {
    HFONT font = CreateFontW(-MulDiv(pt, static_cast<int>(dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, face);
    if (!fallback) return font;

    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font);
    wchar_t actual[LF_FACESIZE]{};
    GetTextFaceW(dc, LF_FACESIZE, actual);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    if (_wcsicmp(actual, face) == 0) return font;

    DeleteObject(font);
    return CreateFontW(-MulDiv(pt, static_cast<int>(dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, fallback);
}

std::optional<std::filesystem::path> pick_folder(HWND owner, const wchar_t* title) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    std::optional<std::filesystem::path> result;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = std::filesystem::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

void show_in_explorer(HWND owner, const std::wstring& path) {
    const std::wstring args = L"/select," + quote(path);
    ShellExecuteW(owner, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

SIZE measure(HDC dc, HFONT font, const std::wstring& text) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    return size;
}

int compare_ci(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) - CSTR_EQUAL;
}

}  // namespace

MainWindow::~MainWindow() {
    for (HFONT f : {font_title_, font_icon_large_, font_icon_small_, fonts_.text, fonts_.caption, fonts_.icon}) {
        if (f) DeleteObject(f);
    }
    if (icons_) ImageList_Destroy(icons_);
    if (accelerators_) DestroyAcceleratorTable(accelerators_);
    BufferedPaintUnInit();
}

bool MainWindow::create(HINSTANCE instance, int show_cmd) {
    instance_ = instance;
    BufferedPaintInit();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                 GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;

    const ACCEL accels[] = {
        {FVIRTKEY | FCONTROL, 'O', IDC_OPEN},
        {FVIRTKEY, VK_F5, IDM_RESCAN},
        {FVIRTKEY, VK_F2, IDM_RENAME},
        {FVIRTKEY, VK_DELETE, IDC_DELETE},
        {FVIRTKEY | FCONTROL, 'U', IDC_UNLOCK},
        {FVIRTKEY, VK_ESCAPE, IDM_DESELECT},
    };
    accelerators_ = CreateAcceleratorTableW(const_cast<ACCEL*>(accels), static_cast<int>(std::size(accels)));

    const HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, tr(S::AppName), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                      CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, this);
    if (!hwnd) return false;

    // Gespeicherte Position nur, wenn sie noch auf einem Monitor liegt
    const auto saved = settings::load();
    bool placed = false;
    if (saved.window.right > saved.window.left && saved.window.bottom > saved.window.top &&
        MonitorFromRect(&saved.window, MONITOR_DEFAULTTONULL)) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        wp.rcNormalPosition = saved.window;
        wp.showCmd = saved.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        placed = SetWindowPlacement(hwnd, &wp) != FALSE;
    }
    if (!placed) {
        const int width = dp(900);
        const int height = dp(560);
        const int x = (GetSystemMetricsForDpi(SM_CXSCREEN, dpi_) - width) / 2;
        const int y = (GetSystemMetricsForDpi(SM_CYSCREEN, dpi_) - height) / 2;
        SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(hwnd, show_cmd);
    }
    UpdateWindow(hwnd);
    return true;
}

void MainWindow::open(std::vector<std::filesystem::path> targets) {
    // Keine Existenzprüfung hier: die könnte bei nicht erreichbaren Netzpfaden die UI blockieren – das macht der Scan.
    targets_.clear();
    for (auto& t : targets) {
        std::error_code ec;
        targets_.push_back(std::filesystem::absolute(t, ec));
    }
    result_.reset();
    rows_.clear();
    clipped_rows_.clear();
    ListView_DeleteAllItems(list_);
    if (targets_.empty()) {
        clear_targets(tr(S::StatusTargetMissing), StatusKind::Error);
        return;
    }
    ShowWindow(list_, SW_SHOW);
    update_title();
    update_tooltip();
    invalidate_header();
    start_scan();
}

void MainWindow::start_collecting() {
    SetPropW(hwnd_, instance::kCollectProp, instance::collect_prop_value(elevated_));
    SetTimer(hwnd_, kTimerCollect, instance::kCollectWindowMs, nullptr);
}

// Ziele einer weiteren Instanz anhängen (doppelte ignorieren). FALSE = sammelt nicht mehr, der Absender öffnet selbst.
bool MainWindow::on_copy_data(const COPYDATASTRUCT& data) {
    if (!GetPropW(hwnd_, instance::kCollectProp)) return false;
    auto incoming = instance::parse(data);
    if (incoming.empty()) return false;
    auto merged = targets_;
    for (auto& t : incoming) {
        const bool known = std::any_of(merged.begin(), merged.end(),
                                       [&](const std::filesystem::path& m) { return path_util::equals_ci(m.wstring(), t.wstring()); });
        if (!known) merged.push_back(std::move(t));
    }
    if (merged.size() != targets_.size()) open(std::move(merged));
    start_collecting();  // Zeitfenster ab dem letzten Ziel verlängern
    return true;
}

void MainWindow::remove_target(std::size_t index) {
    if (index >= targets_.size()) return;
    auto remaining = targets_;
    remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(index));
    if (remaining.empty()) {
        clear_targets(tr(S::StatusReady), StatusKind::Neutral);
        return;
    }
    hot_chip_ = -1;
    open(std::move(remaining));
}

void MainWindow::update_title() {
    std::wstring title = tr(S::AppName);
    if (targets_.size() == 1) title = targets_.front().filename().wstring() + L" – " + tr(S::AppName);
    else if (targets_.size() > 1) title = fmt(S::TitleTargets, targets_.size());
    SetWindowTextW(hwnd_, title.c_str());
}

std::wstring MainWindow::targets_label() const {
    if (targets_.empty()) return tr(S::NoFileSelected);
    return targets_.front().wstring();
}

std::wstring MainWindow::quoted_targets() const {
    std::wstring args;
    for (const auto& t : targets_) {
        if (!args.empty()) args += L' ';
        args += quote(t.wstring());
    }
    return args;
}

LRESULT CALLBACK MainWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);
    return self->handle_message(msg, wparam, lparam);
}

LRESULT MainWindow::handle_message(UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE:
            on_create();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = {dp(kMinWidth), dp(kMinHeight)};
            return 0;
        }
        case WM_ERASEBKGND:
            on_erase_background(reinterpret_cast<HDC>(wparam));
            return 1;
        case WM_PAINT:
            on_paint();
            return 0;
        case WM_DRAWITEM: {
            const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (dis->CtlType == ODT_BUTTON) {
                controls::draw_button(*dis, fonts_);
                return TRUE;
            }
            break;
        }
        case WM_MOUSEMOVE:
            on_mouse_move({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_MOUSELEAVE:
            tracking_mouse_ = false;
            if (hot_chip_ >= 0) {
                hot_chip_ = -1;
                hot_close_ = false;
                invalidate_header();
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (on_left_click({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) return 0;
            break;
        case WM_MBUTTONDOWN: {
            // Mittelklick auf einen Chip entfernt das Ziel
            const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            for (const auto& chip : chips_) {
                if (!chip.overflow && PtInRect(&chip.rect, pt)) {
                    remove_target(chip.target);
                    return 0;
                }
            }
            break;
        }
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wparam) == ListView_GetHeader(list_)) {
                POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                if (lparam == static_cast<LPARAM>(-1)) {
                    RECT hr{};
                    GetWindowRect(ListView_GetHeader(list_), &hr);
                    pt = {hr.left + dp(8), hr.bottom};
                }
                do_header_menu(pt);
                return 0;
            }
            break;
        case WM_SETCURSOR:
            if (reinterpret_cast<HWND>(wparam) == hwnd_ && LOWORD(lparam) == HTCLIENT && hot_close_) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_DPICHANGED:
            on_dpi_changed(HIWORD(wparam), *reinterpret_cast<RECT*>(lparam));
            return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            on_accent_changed();
            return 0;
        case WM_TIMER:
            on_timer(wparam);
            return 0;
        case WM_COPYDATA:
            return on_copy_data(*reinterpret_cast<const COPYDATASTRUCT*>(lparam)) ? TRUE : FALSE;
        case WM_COMMAND:
            on_command(LOWORD(wparam));
            return 0;
        case WM_NOTIFY:
            return on_notify(*reinterpret_cast<NMHDR*>(lparam));
        case WM_APP_SCAN_FAST:
            on_scan_result(static_cast<unsigned>(wparam), std::unique_ptr<ScanResult>(reinterpret_cast<ScanResult*>(lparam)), false);
            return 0;
        case WM_APP_SCAN_ENRICHED:
            on_scan_result(static_cast<unsigned>(wparam), std::unique_ptr<ScanResult>(reinterpret_cast<ScanResult*>(lparam)), true);
            return 0;
        case WM_APP_SCAN_TARGETS: {
            const std::unique_ptr<std::vector<std::filesystem::path>> names(
                reinterpret_cast<std::vector<std::filesystem::path>*>(lparam));
            if (static_cast<unsigned>(wparam) == scan_generation_ && names->size() == targets_.size()) {
                targets_ = std::move(*names);
                update_title();
                update_tooltip();
                invalidate_header();
            }
            return 0;
        }
        case WM_APP_SHELL_DONE: {
            const std::unique_ptr<ShellToggleDone> done(reinterpret_cast<ShellToggleDone*>(lparam));
            on_shell_toggled(done->enabled, done->outcome.hr, done->outcome.text);
            return 0;
        }
        case WM_DESTROY:
            on_destroy();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void MainWindow::on_create() {
    dpi_ = GetDpiForWindow(hwnd_);
    wchar_t dpi_override[8]{};
    if (GetEnvironmentVariableW(L"UNLOCKER_DPI", dpi_override, 8) > 0) dpi_ = static_cast<UINT>(_wtoi(dpi_override));  // Layout-/Rendertest
    elevated_ = process::is_elevated();
    mica_ = theme::mica_supported();
    theme::apply_window(hwnd_, dp(kTitleHeight));

    create_fonts();

    {
        const auto saved = settings::load();
        sort_column_ = saved.sort_column;
        sort_ascending_ = saved.sort_ascending;
        for (auto name : saved.collapsed_processes) {
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            collapsed_names_.insert(name);
        }
    }

    list_ = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, 0, 0, 0, 0,
                            hwnd_, reinterpret_cast<HMENU>(IDC_LIST), instance_, nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_INFOTIP |
                                                 LVS_EX_HEADERDRAGDROP);
    if (HWND tips = ListView_GetToolTips(list_)) {
        SendMessageW(tips, TTM_SETMAXTIPWIDTH, 0, dp(640));  // mehrzeilige Zeilen-Tooltips (\n)
        theme::apply_control(tips, L"DarkMode_Explorer");
    }
    SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.text), TRUE);
    controls::style_listview(list_, &fonts_);
    controls::set_listview_row_height(list_, dp(kRowHeight));
    controls::set_header_sort(list_, sort_column_, sort_ascending_);
    create_image_list();

    const struct {
        S title;
        int width;
    } columns[] = {{S::ColProcess, 260}, {S::ColPid, 70}, {S::ColKind, 130}, {S::ColPath, 320}};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(tr(columns[i].title));
        col.cx = dp(columns[i].width);
        ListView_InsertColumn(list_, i, &col);
    }
    apply_settings_columns();

    btn_open_ = controls::create_button(hwnd_, instance_, IDC_OPEN, tr(S::BtnOpen), kGlyphOpen, false);
    btn_more_ = controls::create_button(hwnd_, instance_, IDC_MORE, tr(S::BtnMore), kGlyphMore, false);
    btn_kill_ = controls::create_button(hwnd_, instance_, IDC_KILL, tr(S::BtnKill), kGlyphKill, false);
    btn_delete_ = controls::create_button(hwnd_, instance_, IDC_DELETE, tr(S::BtnDelete), kGlyphDelete, false);
    btn_unlock_ = controls::create_button(hwnd_, instance_, IDC_UNLOCK, tr(S::BtnUnlock), kGlyphUnlock, true);
    btn_elevate_ = controls::create_button(hwnd_, instance_, IDC_ELEVATE, tr(S::BtnElevate), kGlyphShield, false);
    if (elevated_) ShowWindow(btn_elevate_, SW_HIDE);

    create_tooltip();

    drop_target_ = new DropTarget(*this);
    RegisterDragDrop(hwnd_, drop_target_);
    SetPropW(hwnd_, L"UnlockerDragOver", reinterpret_cast<HANDLE>(0));  // für Tests lesbar (0 = kein Drag)

    update_buttons();
    set_status(tr(S::StatusReady), StatusKind::Neutral);
}

void MainWindow::on_destroy() {
    save_settings();
    KillTimer(hwnd_, kTimerProgress);
    KillTimer(hwnd_, kTimerReveal);
    KillTimer(hwnd_, kTimerCollect);
    RemovePropW(hwnd_, instance::kCollectProp);
    RemovePropW(hwnd_, L"UnlockerShellBusy");
    if (drop_target_) {
        RevokeDragDrop(hwnd_);
        drop_target_->Release();
        drop_target_ = nullptr;
    }
}

void MainWindow::apply_settings_columns() {
    const auto saved = settings::load();
    for (int i = 0; i < 3; ++i) {
        if (saved.columns[i] > 0) ListView_SetColumnWidth(list_, i, dp(saved.columns[i]));
    }
    for (const int c : saved.hidden_columns) set_column_visible(c, false);
    // Reihenfolge zuletzt: Reorder toucht die Breiten nicht, passt aber die visuelle Anordnung.
    if (saved.column_order.size() == 4) {
        int order[4];
        for (int i = 0; i < 4; ++i) order[i] = saved.column_order[i];
        ListView_SetColumnOrderArray(list_, 4, order);
    }
}

void MainWindow::save_settings() const {
    settings::Settings s;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd_, &wp)) {
        s.window = wp.rcNormalPosition;
        s.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    }
    for (int i = 0; i < 3; ++i) {
        const int width = hidden_columns_.contains(i) ? column_width_[i] : ListView_GetColumnWidth(list_, i);
        s.columns[i] = MulDiv(width, 96, static_cast<int>(dpi_));
    }
    s.hidden_columns.assign(hidden_columns_.begin(), hidden_columns_.end());
    s.sort_column = sort_column_;
    s.sort_ascending = sort_ascending_;
    s.collapsed_processes.assign(collapsed_names_.begin(), collapsed_names_.end());
    {
        int order[4]{};
        if (ListView_GetColumnOrderArray(list_, 4, order)) s.column_order.assign(order, order + 4);
    }
    settings::save(s);
}

void MainWindow::create_fonts() {
    for (HFONT f : {font_title_, font_icon_large_, font_icon_small_, fonts_.text, fonts_.caption, fonts_.icon}) {
        if (f) DeleteObject(f);
    }
    fonts_.dpi = dpi_;
    font_title_ = make_font(dpi_, 20, FW_SEMIBOLD, L"Segoe UI Variable Display", L"Segoe UI");
    fonts_.text = make_font(dpi_, 10, FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI");
    fonts_.caption = make_font(dpi_, 9, FW_NORMAL, L"Segoe UI Variable Small", L"Segoe UI");
    fonts_.icon = make_font(dpi_, 11, FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    font_icon_small_ = make_font(dpi_, 8, FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    font_icon_large_ = make_font(dpi_, 40, FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    if (list_) SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.text), TRUE);
}

void MainWindow::create_image_list() {
    if (icons_) {
        ListView_SetImageList(list_, nullptr, LVSIL_SMALL);
        ImageList_Destroy(icons_);
    }
    icon_cache_.clear();
    const int size = dp(16);
    icons_ = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 16, 16);
    ListView_SetImageList(list_, icons_, LVSIL_SMALL);
}

void MainWindow::create_tooltip() {
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, instance_, nullptr);
    if (!tooltip_) return;
    theme::apply_control(tooltip_, L"DarkMode_Explorer");
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = hwnd_;
    ti.uId = 1;
    ti.rect = subtitle_rect();
    ti.lpszText = const_cast<wchar_t*>(L"");
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, dp(640));
    SendMessageW(tooltip_, TTM_ACTIVATE, FALSE, 0);
}

// Tool anlegen/ersetzen; leerer Text entfernt es.
void MainWindow::set_tooltip_tool(UINT id, const RECT& rect, const std::wstring& text) {
    if (!tooltip_) return;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwnd_;
    ti.uId = id;
    SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    if (text.empty()) return;
    ti.uFlags = TTF_SUBCLASS;
    ti.rect = rect;
    ti.lpszText = const_cast<wchar_t*>(text.c_str());  // Tooltip kopiert den Text
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

// Einzelziel: Tooltip mit vollem Pfad nur, wenn die Zeile gekürzt ist. Bei Chips übernehmen die Chip-Tools.
void MainWindow::update_tooltip() {
    if (!tooltip_) return;
    const RECT rect = subtitle_rect();
    std::wstring text;
    if (targets_.size() == 1) {
        HDC dc = GetDC(hwnd_);
        const SIZE size = measure(dc, fonts_.text, targets_.front().wstring());
        ReleaseDC(hwnd_, dc);
        if (size.cx > rect.right - rect.left) text = targets_.front().wstring();
    }
    set_tooltip_tool(1, rect, text);
    if (targets_.size() <= 1) {
        for (std::size_t i = 0; i < chip_tip_count_; ++i) set_tooltip_tool(static_cast<UINT>(100 + i), rect, L"");
        chip_tip_count_ = 0;
        chip_tip_rects_.clear();
    }
    SendMessageW(tooltip_, TTM_ACTIVATE, TRUE, 0);
}

// Tooltip pro Chip mit dem vollen Pfad (Überlauf-Chip: alle restlichen Pfade). Nur bei Änderung neu registrieren.
void MainWindow::sync_chip_tooltips() {
    if (!tooltip_) return;
    std::vector<RECT> rects;
    for (const auto& c : chips_) rects.push_back(c.rect);
    const bool same = rects.size() == chip_tip_rects_.size() &&
                      std::equal(rects.begin(), rects.end(), chip_tip_rects_.begin(), [](const RECT& a, const RECT& b) {
                          return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
                      });
    if (same) return;

    for (std::size_t i = 0; i < chip_tip_count_; ++i) set_tooltip_tool(static_cast<UINT>(100 + i), RECT{}, L"");
    for (std::size_t i = 0; i < chips_.size(); ++i) {
        const auto& c = chips_[i];
        std::wstring text;
        if (c.overflow) {
            for (std::size_t t = c.target; t < targets_.size(); ++t) {
                if (!text.empty()) text += L'\n';
                text += targets_[t].wstring();
            }
        } else {
            text = targets_[c.target].wstring();
        }
        set_tooltip_tool(static_cast<UINT>(100 + i), c.rect, text);
    }
    chip_tip_count_ = chips_.size();
    chip_tip_rects_ = std::move(rects);
}

int MainWindow::icon_for(const std::wstring& image_path) {
    if (auto it = icon_cache_.find(image_path); it != icon_cache_.end()) return it->second;

    const int size = dp(16);
    HICON icon = nullptr;
    if (!image_path.empty() && image_path.find(L'\\') != std::wstring::npos) {
        SHDefExtractIconW(image_path.c_str(), 0, 0, nullptr, &icon, static_cast<UINT>(MAKELONG(size, size)));
    }
    if (!icon) {
        SHSTOCKICONINFO info{};
        info.cbSize = sizeof(info);
        if (SUCCEEDED(SHGetStockIconInfo(SIID_APPLICATION, SHGSI_ICON | SHGSI_SMALLICON, &info))) icon = info.hIcon;
    }
    int index = -1;
    if (icon) {
        index = ImageList_AddIcon(icons_, icon);
        DestroyIcon(icon);
    }
    icon_cache_[image_path] = index;
    return index;
}

void MainWindow::on_dpi_changed(UINT dpi, const RECT& suggested) {
    const UINT old_dpi = dpi_;
    dpi_ = dpi;
    create_fonts();
    create_image_list();
    controls::set_listview_row_height(list_, dp(kRowHeight));
    theme::apply_window(hwnd_, dp(kTitleHeight));
    for (int i = 0; i < 4; ++i) {
        ListView_SetColumnWidth(list_, i, MulDiv(ListView_GetColumnWidth(list_, i), static_cast<int>(dpi_), static_cast<int>(old_dpi)));
    }
    SetWindowPos(hwnd_, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                 suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
    fill_list();
}

void MainWindow::on_accent_changed() {
    theme::refresh_palette();
    const auto& p = theme::palette();
    ListView_SetBkColor(list_, p.card_bg);
    ListView_SetTextBkColor(list_, p.card_bg);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

RECT MainWindow::title_rect() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    rc.bottom = dp(kTitleHeight);
    return rc;
}

RECT MainWindow::subtitle_rect() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    return RECT{dp(kMargin), dp(kTitleHeight), rc.right - dp(kMargin), dp(kTitleHeight) + dp(kSubtitleHeight)};
}

RECT MainWindow::progress_rect() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    return RECT{dp(kMargin), dp(kProgressTop), rc.right - dp(kMargin), dp(kProgressTop) + dp(3)};
}

RECT MainWindow::status_rect() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int margin = dp(kMargin);
    const int buttons_top = rc.bottom - margin - dp(kButtonHeight);
    return RECT{margin, buttons_top - dp(kGap) - dp(kStatusHeight), rc.right - margin, buttons_top - dp(kGap)};
}

RECT MainWindow::card_rect() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int margin = dp(kMargin);
    const RECT status = status_rect();
    return RECT{margin, dp(kCardTop), rc.right - margin, status.top - dp(kGap)};
}

void MainWindow::layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int margin = dp(kMargin);
    const int gap = dp(kGap);
    const int button_h = dp(kButtonHeight);
    const int buttons_y = rc.bottom - margin - button_h;

    const RECT card = card_rect();
    const int inset = 1;
    HDWP dwp = BeginDeferWindowPos(7);
    dwp = DeferWindowPos(dwp, list_, nullptr, card.left + inset, card.top + inset, std::max(0L, card.right - card.left - 2 * inset),
                         std::max(0L, card.bottom - card.top - 2 * inset), SWP_NOZORDER);

    const HWND right_buttons[] = {btn_unlock_, btn_delete_, btn_kill_, btn_more_, btn_open_};
    int x = rc.right - margin;
    for (HWND b : right_buttons) {
        const int w = controls::preferred_button_width(b, fonts_);
        x -= w;
        dwp = DeferWindowPos(dwp, b, nullptr, x, buttons_y, w, button_h, SWP_NOZORDER);
        x -= gap;
    }
    if (!elevated_) {
        const int full = controls::preferred_button_width(btn_elevate_, fonts_, tr(S::BtnElevate));
        const bool use_compact = (x - margin) < full;
        controls::set_button_compact(btn_elevate_, use_compact);
        dwp = DeferWindowPos(dwp, btn_elevate_, nullptr, margin, buttons_y, use_compact ? dp(44) : full, button_h, SWP_NOZORDER);
    }
    EndDeferWindowPos(dwp);

    controls::round_corners(list_, dp(kCardRadius) - inset);
    fit_last_column();
    update_tooltip();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

// Die visuell rechte, sichtbare Spalte (Breite > 0) – unabhängig von der Reihenfolge per Drag im Kopf.
int MainWindow::visual_rightmost_visible() const {
    int order[4]{};
    if (!ListView_GetColumnOrderArray(list_, 4, order)) return -1;
    for (int i = 3; i >= 0; --i) {
        const int c = order[i];
        if (!hidden_columns_.contains(c) && ListView_GetColumnWidth(list_, c) > 0) return c;
    }
    return -1;
}

// Visuell rechte sichtbare Spalte füllt die Restbreite (Default: Pfad); zweimal, weil das Verschwinden der
// horizontalen Scrollbar die Höhe und damit ggf. die vertikale Scrollbar ändert.
void MainWindow::fit_last_column() {
    const int target = visual_rightmost_visible();
    if (target < 0) return;  // alle Spalten ausgeblendet – Breiten bleiben wie gesetzt
    for (int pass = 0; pass < 2; ++pass) {
        RECT rc{};
        GetClientRect(list_, &rc);
        int used = 0;
        for (int i = 0; i < 4; ++i)
            if (i != target) used += ListView_GetColumnWidth(list_, i);
        const int width = std::max<int>(rc.right - rc.left - used, dp(160));
        if (ListView_GetColumnWidth(list_, target) != width) ListView_SetColumnWidth(list_, target, width);
    }
}

void MainWindow::on_erase_background(HDC hdc) {
    const auto& p = theme::palette();
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    HBRUSH bg = CreateSolidBrush(p.window_bg);
    if (mica_) {
        // Titelzone liegt auf dem erweiterten Frame: Schwarz mit Alpha 0 = Mica scheint durch.
        const RECT title = title_rect();
        FillRect(hdc, &title, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        RECT body = rc;
        body.top = title.bottom;
        FillRect(hdc, &body, bg);
    } else {
        FillRect(hdc, &rc, bg);
    }
    DeleteObject(bg);
}

// Ziel-Chips: Dateiname + Schließen-Kreuz, Überlauf als "+n".
void MainWindow::draw_chips(HDC hdc) {
    const auto& p = theme::palette();
    chips_.clear();
    const RECT area = subtitle_rect();
    const int chip_h = dp(kChipHeight);
    const int y = (area.top + area.bottom) / 2 - chip_h / 2;
    const int pad = dp(10);
    const int gap = dp(6);
    SetBkMode(hdc, TRANSPARENT);

    const SIZE close_size = measure(hdc, font_icon_small_, kGlyphClose);
    int x = area.left;
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        const std::wstring name = targets_[i].filename().wstring();
        const SIZE ts = measure(hdc, fonts_.caption, name);
        const int w = pad + ts.cx + gap + close_size.cx + pad;
        const std::size_t remaining = targets_.size() - i;

        // Passt dieser Chip plus ggf. ein "+n"-Chip noch hinein?
        const std::wstring overflow_text = fmt(S::ChipOverflow, remaining);
        const int overflow_w = pad + measure(hdc, fonts_.caption, overflow_text).cx + pad;
        if (x + w > area.right - (remaining > 1 ? overflow_w + gap : 0)) {
            Chip chip;
            chip.rect = RECT{x, y, std::min<LONG>(area.right, x + overflow_w), y + chip_h};
            chip.overflow = true;
            chip.target = i;
            controls::fill_round_rect(hdc, chip.rect, chip_h / 2, theme::blend(p.window_bg, p.text, 8), CLR_INVALID);
            SetTextColor(hdc, p.text_secondary);
            HGDIOBJ old = SelectObject(hdc, fonts_.caption);
            DrawTextW(hdc, overflow_text.c_str(), -1, &chip.rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SelectObject(hdc, old);
            chips_.push_back(chip);
            break;
        }

        Chip chip;
        chip.rect = RECT{x, y, x + w, y + chip_h};
        chip.close = RECT{x + pad + ts.cx + gap - dp(4), y, x + w, y + chip_h};
        chip.target = i;
        const bool hot = hot_chip_ == static_cast<int>(chips_.size());
        controls::fill_round_rect(hdc, chip.rect, chip_h / 2, theme::blend(p.window_bg, p.text, hot ? 14 : 8), CLR_INVALID);

        HGDIOBJ old = SelectObject(hdc, fonts_.caption);
        RECT tr_{x + pad, y, x + pad + ts.cx, y + chip_h};
        SetTextColor(hdc, p.text);
        DrawTextW(hdc, name.c_str(), -1, &tr_, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(hdc, font_icon_small_);
        RECT cr{x + pad + ts.cx + gap, y, x + pad + ts.cx + gap + close_size.cx, y + chip_h};
        SetTextColor(hdc, (hot && hot_close_) ? p.danger : hot ? p.text_secondary : p.text_tertiary);
        DrawTextW(hdc, kGlyphClose, -1, &cr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(hdc, old);

        chips_.push_back(chip);
        x += w + gap;
    }
}

void MainWindow::on_paint() {
    const auto& p = theme::palette();
    PAINTSTRUCT ps{};
    const HDC hdc = BeginPaint(hwnd_, &ps);

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int margin = dp(kMargin);

    RECT title{margin, dp(14), rc.right - margin, dp(50)};
    draw_text(hwnd_, hdc, title, tr(S::AppName), font_title_, p.text, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX,
              mica_);

    if (targets_.size() > 1) {
        draw_chips(hdc);
        sync_chip_tooltips();
    } else {
        chips_.clear();
        draw_text(hwnd_, hdc, subtitle_rect(), targets_label(), fonts_.text, p.text_secondary,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX, false);
    }

    if (busy_) {
        const RECT pr = progress_rect();
        const int width = pr.right - pr.left;
        controls::fill_round_rect(hdc, pr, (pr.bottom - pr.top) / 2, p.card_border, CLR_INVALID);
        const int segment = width / 4;
        const int travel = width + segment;
        const int offset = static_cast<int>((progress_tick_ * dp(6)) % static_cast<unsigned>(travel)) - segment;
        RECT seg{std::max(pr.left, pr.left + offset), pr.top, std::min(pr.right, pr.left + offset + segment), pr.bottom};
        if (seg.right > seg.left) controls::fill_round_rect(hdc, seg, (pr.bottom - pr.top) / 2, p.accent, CLR_INVALID);
    }

    const RECT card = card_rect();
    const COLORREF card_border = drag_over_ ? p.accent : p.card_border;
    controls::fill_round_rect(hdc, card, dp(kCardRadius), p.card_bg, card_border, drag_over_ ? 2.0f : 1.0f);
    if (targets_.empty()) {
        const int cy = (card.top + card.bottom) / 2;
        RECT glyph{card.left, cy - dp(70), card.right, cy - dp(10)};
        draw_text(hwnd_, hdc, glyph, drag_over_ ? kGlyphDrop : kGlyphLock, font_icon_large_,
                  drag_over_ ? p.accent : p.text_tertiary, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, false);
        RECT line1{card.left, cy - dp(4), card.right, cy + dp(20)};
        draw_text(hwnd_, hdc, line1, drag_over_ ? tr(S::DropHint) : tr(S::EmptyTitle), fonts_.text,
                  drag_over_ ? p.accent : p.text, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, false);
        if (!drag_over_) {
            RECT line2{card.left, cy + dp(22), card.right, cy + dp(44)};
            draw_text(hwnd_, hdc, line2, tr(S::EmptyHint), fonts_.caption, p.text_tertiary,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, false);
        }
    }

    const RECT status = status_rect();
    const int dot = dp(8);
    const int dot_y = (status.top + status.bottom) / 2 - dot / 2;
    COLORREF dot_color = p.text_tertiary;
    switch (status_kind_) {
        case StatusKind::Busy: dot_color = p.accent; break;
        case StatusKind::Ok: dot_color = p.success; break;
        case StatusKind::Warning: dot_color = p.warning; break;
        case StatusKind::Error: dot_color = p.danger; break;
        default: break;
    }
    RECT dot_rc{status.left, dot_y, status.left + dot, dot_y + dot};
    controls::fill_round_rect(hdc, dot_rc, dot / 2, dot_color, CLR_INVALID);
    RECT text_rc{status.left + dot + dp(10), status.top, status.right, status.bottom};
    draw_text(hwnd_, hdc, text_rc, status_, fonts_.caption, p.text_secondary,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX, false);

    EndPaint(hwnd_, &ps);
}

void MainWindow::on_mouse_move(POINT pt) {
    if (!tracking_mouse_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        tracking_mouse_ = true;
    }
    int hot = -1;
    bool close = false;
    for (std::size_t i = 0; i < chips_.size(); ++i) {
        if (chips_[i].overflow || !PtInRect(&chips_[i].rect, pt)) continue;
        hot = static_cast<int>(i);
        close = PtInRect(&chips_[i].close, pt) != FALSE;
    }
    if (hot != hot_chip_ || close != hot_close_) {
        hot_chip_ = hot;
        hot_close_ = close;
        invalidate_header();
    }
}

bool MainWindow::on_left_click(POINT pt) {
    for (const auto& chip : chips_) {
        if (!chip.overflow && PtInRect(&chip.close, pt)) {
            remove_target(chip.target);
            return true;
        }
    }
    return false;
}

// Einblend-Animation: Zeilen gleiten gestaffelt von links ein und blenden auf.
void MainWindow::start_reveal() {
    reveal_start_ = GetTickCount64();
    SetTimer(hwnd_, kTimerReveal, 30, nullptr);
}

float MainWindow::reveal_progress(int item) const {
    if (reveal_start_ == 0) return 1.0f;
    const auto elapsed = static_cast<long long>(GetTickCount64() - reveal_start_);
    const long long start = static_cast<long long>(std::min(item, kRevealMaxStaggerRows)) * kRevealStaggerMs;
    const long long t = elapsed - start;
    if (t <= 0) return 0.0f;
    if (t >= kRevealRowMs) return 1.0f;
    const float x = static_cast<float>(t) / kRevealRowMs;
    return 1.0f - (1.0f - x) * (1.0f - x);  // ease-out
}

// Zeile komplett selbst zeichnen: Gruppenkopf oder Sperre; Hover/Selektion als abgerundete Fläche mit
// Akzentbalken, Tastaturfokus als Rahmen, Icon, Prozessname + App-Name, Badges, übrige Spalten sekundär.
LRESULT MainWindow::draw_row(NMLVCUSTOMDRAW& cd) {
    const auto& p = theme::palette();
    const int item = static_cast<int>(cd.nmcd.dwItemSpec);
    const auto row_index = static_cast<std::size_t>(cd.nmcd.lItemlParam);
    if (!result_ || row_index >= rows_.size()) return CDRF_DODEFAULT;
    const Row& row = rows_[row_index];
    const Locker& l = result_->lockers[row.locker];
    const HDC hdc = cd.nmcd.hdc;

    RECT rc{};
    if (!ListView_GetItemRect(list_, item, &rc, LVIR_BOUNDS)) return CDRF_DODEFAULT;
    const bool selected = (ListView_GetItemState(list_, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    const bool focused = (ListView_GetItemState(list_, item, LVIS_FOCUSED) & LVIS_FOCUSED) != 0 && GetFocus() == list_ &&
                         !(SendMessageW(list_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    const bool hot = controls::listview_hot_item(list_) == item;
    const float reveal = reveal_progress(item);
    const int slide = static_cast<int>((1.0f - reveal) * dp(12));
    const auto fade = [&](COLORREF c) { return theme::blend(p.card_bg, c, static_cast<int>(reveal * 100)); };

    HBRUSH bg = CreateSolidBrush(p.card_bg);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    if (selected || hot) {
        RECT hl{rc.left + dp(4), rc.top + 1, rc.right - dp(4), rc.bottom - 1};
        controls::fill_round_rect(hdc, hl, dp(4), selected ? p.row_selected : p.row_hover, CLR_INVALID);
    }
    if (selected) {
        RECT bar{rc.left + dp(4), rc.top + dp(8), rc.left + dp(7), rc.bottom - dp(8)};
        controls::fill_round_rect(hdc, bar, dp(1), p.accent, CLR_INVALID);
    }
    if (focused) {
        RECT fr{rc.left + dp(4), rc.top + 1, rc.right - dp(4), rc.bottom - 1};
        controls::fill_round_rect(hdc, fr, dp(4), CLR_INVALID, p.text_secondary, 1.0f);
    }

    const auto columns = controls::listview_column_rects(list_);
    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ old = SelectObject(hdc, fonts_.text);
    const int pad = dp(10);
    const COLORREF name_color = fade(l.is_critical ? p.danger : l.is_sensitive ? p.warning : p.text);

    if (row.group) {
        // Gruppenkopf: Chevron, Icon, Name, "n Einträge", PID – alles über die VISUELLE Position der
        // logischen Spalten, damit eine Reihenfolge per Drag im Kopf nichts bricht.
        const bool proc_visible = columns.size() > static_cast<std::size_t>(ColProcess) &&
                                  ListView_GetColumnWidth(list_, ColProcess) > 0;
        const bool pid_visible = columns.size() > static_cast<std::size_t>(ColPid) && ListView_GetColumnWidth(list_, ColPid) > 0;
        const RECT& proc_rect = proc_visible ? columns[ColProcess] : (columns.empty() ? RECT{} : columns.front());
        const RECT& pid_rect = pid_visible ? columns[ColPid] : RECT{};

        if (proc_visible) {
            RECT cell{proc_rect.left + dp(10) + slide, rc.top, proc_rect.right - pad, rc.bottom};
            SelectObject(hdc, fonts_.icon);
            const bool collapsed = collapsed_.contains(row.pid);
            SetTextColor(hdc, fade(p.text_secondary));
            RECT chevron{cell.left, rc.top, cell.left + dp(16), rc.bottom};
            DrawTextW(hdc, collapsed ? kGlyphCollapsed : kGlyphExpanded, -1, &chevron, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            cell.left += dp(20);

            LVITEMW li{};
            li.mask = LVIF_IMAGE;
            li.iItem = item;
            ListView_GetItem(list_, &li);
            const int icon = dp(16);
            if (li.iImage >= 0 && icons_ && reveal > 0.5f) {
                ImageList_Draw(icons_, li.iImage, hdc, cell.left, rc.top + (rc.bottom - rc.top - icon) / 2, ILD_TRANSPARENT);
            }
            cell.left += icon + dp(8);

            const std::wstring name = display_name(l);
            SelectObject(hdc, fonts_.text);
            const SIZE ns = measure(hdc, fonts_.text, name);
            RECT nr{cell.left, rc.top, std::min(cell.right, cell.left + ns.cx + 2), rc.bottom};
            SetTextColor(hdc, name_color);
            DrawTextW(hdc, name.c_str(), -1, &nr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

            // "n Einträge" zwischen Name und Prozess-Zellrand; endet am PID-Linksrand, wenn die PID-Spalte
            // VISUELL rechts des Prozess-Rechts liegt (sonst ginge das Rect bei PID-links invertiert raus).
            const std::wstring count = fmt(S::GroupEntries, row.count);
            int cr_right = cell.right;
            if (pid_visible && pid_rect.left > proc_rect.left) cr_right = std::min<int>(cr_right, pid_rect.left - pad);
            RECT cr{nr.right + dp(10), rc.top, cr_right, rc.bottom};
            if (cr.right > cr.left) {
                SetTextColor(hdc, fade(p.text_tertiary));
                DrawTextW(hdc, count.c_str(), -1, &cr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }

        if (pid_visible) {
            RECT pr{pid_rect.left + pad, rc.top, pid_rect.right - pad, rc.bottom};
            const std::wstring pid = pid_text(l);
            SetTextColor(hdc, fade(p.text_secondary));
            DrawTextW(hdc, pid.c_str(), -1, &pr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }
        SelectObject(hdc, old);
        return CDRF_SKIPDEFAULT;
    }

    const bool in_group = std::any_of(rows_.begin(), rows_.end(), [&](const Row& r) { return r.group && r.pid == row.pid; });

    bool clipped = false;  // irgendetwas gekürzt oder weggelassen → Tooltip mit dem vollen Inhalt
    for (std::size_t c = 0; c < columns.size() && c < 4; ++c) {
        RECT cell{columns[c].left + pad, rc.top, columns[c].right - pad, rc.bottom};
        if (c == ColProcess) cell.left += slide;
        if (cell.right <= cell.left) {
            if (ListView_GetColumnWidth(list_, static_cast<int>(c)) > 0) clipped = true;  // sichtbar, aber zu schmal
            continue;
        }

        if (c == ColProcess) {
            cell.left = columns[c].left + dp(12) + slide + (in_group ? dp(20) : 0);
            LVITEMW li{};
            li.mask = LVIF_IMAGE;
            li.iItem = item;
            ListView_GetItem(list_, &li);
            const int icon = dp(16);
            if (li.iImage >= 0 && icons_ && reveal > 0.5f) {
                ImageList_Draw(icons_, li.iImage, hdc, cell.left, rc.top + (rc.bottom - rc.top - icon) / 2, ILD_TRANSPARENT);
            }
            cell.left += icon + dp(8);

            const std::wstring name = display_name(l);
            SelectObject(hdc, fonts_.text);
            const SIZE ns = measure(hdc, fonts_.text, name);
            RECT nr = cell;
            nr.right = std::min(cell.right, cell.left + ns.cx + 2);
            if (cell.left + ns.cx + 2 > cell.right) clipped = true;
            SetTextColor(hdc, name_color);
            DrawTextW(hdc, name.c_str(), -1, &nr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            int x = nr.right + dp(8);

            const struct {
                bool show;
                S text;
                COLORREF color;
            } badges[] = {{l.is_critical, S::BadgeCritical, p.danger},
                          {l.is_protected, S::BadgeProtected, p.danger},
                          {!l.is_critical && l.is_sensitive, S::BadgeSensitive, p.warning},
                          {l.is_service, S::BadgeService, p.accent}};
            SelectObject(hdc, fonts_.caption);
            for (const auto& b : badges) {
                if (!b.show) continue;
                const SIZE bs = measure(hdc, fonts_.caption, tr(b.text));
                const int bw = bs.cx + dp(12);
                if (x + bw > cell.right) {
                    clipped = true;
                    break;
                }
                RECT pill{x, rc.top + (rc.bottom - rc.top - dp(18)) / 2, x + bw, rc.top + (rc.bottom - rc.top + dp(18)) / 2};
                controls::fill_round_rect(hdc, pill, dp(9), fade(theme::blend(p.card_bg, b.color, 22)), CLR_INVALID);
                SetTextColor(hdc, fade(b.color));
                DrawTextW(hdc, tr(b.text), -1, &pill, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                x += bw + dp(6);
            }
            SelectObject(hdc, fonts_.text);

            // Zusatz in Tertiärfarbe: Anzeigename laut Restart Manager bzw. wer die Datei über die Freigabe offen hat
            const std::wstring extra = extra_text(l);

            if (!extra.empty() && x + dp(24) < cell.right) {
                RECT ar{x, rc.top, cell.right, rc.bottom};
                SetTextColor(hdc, fade(p.text_tertiary));
                DrawTextW(hdc, extra.c_str(), -1, &ar, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                if (x + measure(hdc, fonts_.text, extra).cx > cell.right) clipped = true;
            } else if (!extra.empty()) {
                clipped = true;
            }
            continue;
        }

        std::wstring text;
        if (c == ColPid) text = pid_text(l);
        else if (c == ColKind) text = kind_text(l.kind);
        else text = l.locked_path;
        SetTextColor(hdc, fade(p.text_secondary));
        DrawTextW(hdc, text.c_str(), -1, &cell, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (measure(hdc, fonts_.text, text).cx > cell.right - cell.left) clipped = true;
    }
    if (clipped) clipped_rows_.insert(row_index);
    else clipped_rows_.erase(row_index);
    SetPropW(list_, L"UnlockerClippedRows", reinterpret_cast<HANDLE>(clipped_rows_.size()));  // für Tests lesbar
    SelectObject(hdc, old);
    return CDRF_SKIPDEFAULT;
}

// Tooltip einer gekürzt gezeichneten Zeile: Prozess (+ Freigabe/App-Name), Art und voller Pfad – je eine Zeile.
void MainWindow::fill_row_tip(NMLVGETINFOTIPW& tip) const {
    if (!tip.pszText || tip.cchTextMax <= 0) return;
    tip.pszText[0] = L'\0';
    const auto row_index = static_cast<std::size_t>(tip.lParam);
    if (!result_ || row_index >= rows_.size() || rows_[row_index].group || !clipped_rows_.contains(row_index)) return;
    const Locker& l = result_->lockers[rows_[row_index].locker];

    std::wstring head = display_name(l);
    if (l.kind != LockKind::Unattributed) head += std::format(L" (PID {})", l.pid);
    const std::wstring extra = extra_text(l);

    if (!extra.empty()) head += L" – " + extra;
    const std::wstring text = head + L"\n" + kind_text(l.kind) + L"\n" + l.locked_path;
    wcsncpy_s(tip.pszText, static_cast<std::size_t>(tip.cchTextMax), text.c_str(), _TRUNCATE);
    SetPropW(list_, L"UnlockerRowTipLen", reinterpret_cast<HANDLE>(text.size()));  // für Tests lesbar
}

void MainWindow::on_drag_over_changed(bool over) {
    drag_over_ = over;
    SetPropW(hwnd_, L"UnlockerDragOver", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(over ? 1 : 0)));  // für Tests lesbar
    const RECT card = card_rect();
    InvalidateRect(hwnd_, &card, TRUE);
}

void MainWindow::on_files_dropped(std::vector<std::filesystem::path> paths) { open(std::move(paths)); }

void MainWindow::on_command(int id) {
    switch (id) {
        case IDC_OPEN: do_open_dialog(); break;
        case IDC_UNLOCK: do_unlock(UnlockMode::Auto); break;
        case IDC_KILL: do_unlock(UnlockMode::TerminateProcesses); break;
        case IDC_DELETE:
            // Entf über einem Chip entfernt nur das Ziel aus der Liste – nicht die Datei
            if (hot_chip_ >= 0 && static_cast<std::size_t>(hot_chip_) < chips_.size() && !chips_[static_cast<std::size_t>(hot_chip_)].overflow) {
                remove_target(chips_[static_cast<std::size_t>(hot_chip_)].target);
            } else {
                do_delete();
            }
            break;
        case IDC_MORE: do_more_menu(); break;
        case IDM_COLUMN_PID: set_column_visible(ColPid, hidden_columns_.contains(ColPid)); break;
        case IDM_COLUMN_KIND: set_column_visible(ColKind, hidden_columns_.contains(ColKind)); break;
        case IDM_COLUMN_PATH: set_column_visible(ColPath, hidden_columns_.contains(ColPath)); break;
        case IDC_ELEVATE: do_elevate(); break;
        case IDM_SHELL_MENU: do_toggle_shell_menu(); break;
        case IDM_RENAME: do_rename(); break;
        case IDM_MOVE: do_move_or_copy(false); break;
        case IDM_COPY: do_move_or_copy(true); break;
        case IDM_SHOW_EXPLORER: do_show_in_explorer(); break;
        case IDM_DELETE_REBOOT: do_delete_on_reboot(); break;
        case IDM_RESCAN: start_scan(); break;
        case IDM_DESELECT: ListView_SetItemState(list_, -1, 0, LVIS_SELECTED); break;
        case IDM_ROW_CLOSE_HANDLE: do_unlock(UnlockMode::CloseHandles); break;
        case IDM_ROW_KILL: do_unlock(UnlockMode::TerminateProcesses); break;
        case IDM_EXPAND_ALL: set_all_groups(false); break;
        case IDM_COLLAPSE_ALL: set_all_groups(true); break;
        case IDM_ROW_SHOW_FILE: {
            const auto sel = selected_lockers();
            if (!sel.empty()) show_in_explorer(hwnd_, sel.front().locked_path);
            break;
        }
        case IDM_ROW_SHOW_IMAGE: {
            const auto sel = selected_lockers();
            if (!sel.empty() && !sel.front().image_path.empty()) show_in_explorer(hwnd_, sel.front().image_path);
            break;
        }
        case IDM_ROW_PROPERTIES: {
            const auto sel = selected_lockers();
            if (sel.empty() || sel.front().image_path.empty()) break;
            SHELLEXECUTEINFOW info{};
            info.cbSize = sizeof(info);
            info.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_NOASYNC;
            info.hwnd = hwnd_;
            info.lpVerb = L"properties";
            info.lpFile = sel.front().image_path.c_str();
            info.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&info);
            break;
        }
    }
}

LRESULT MainWindow::on_notify(const NMHDR& hdr) {
    if (hdr.hwndFrom != list_) return 0;

    switch (hdr.code) {
        case LVN_ITEMCHANGED:
            update_buttons();
            return 0;
        case LVN_COLUMNCLICK:
            sort_by(reinterpret_cast<const NMLISTVIEW*>(&hdr)->iSubItem);
            return 0;
        case LVN_GETINFOTIPW:
            fill_row_tip(*reinterpret_cast<NMLVGETINFOTIPW*>(const_cast<NMHDR*>(&hdr)));
            return 0;
        case NM_CLICK: {
            const auto& ia = *reinterpret_cast<const NMITEMACTIVATE*>(&hdr);
            if (ia.iItem >= 0 && static_cast<std::size_t>(ia.iItem) < rows_.size() && rows_[static_cast<std::size_t>(ia.iItem)].group) {
                toggle_group(rows_[static_cast<std::size_t>(ia.iItem)].pid);
            }
            return 0;
        }
        case LVN_KEYDOWN: {
            const auto& key = *reinterpret_cast<const NMLVKEYDOWN*>(&hdr);
            const int focused = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
            if (focused >= 0 && static_cast<std::size_t>(focused) < rows_.size() && rows_[static_cast<std::size_t>(focused)].group) {
                const auto pid = rows_[static_cast<std::size_t>(focused)].pid;
                if ((key.wVKey == VK_LEFT && !collapsed_.contains(pid)) || (key.wVKey == VK_RIGHT && collapsed_.contains(pid)) ||
                    key.wVKey == VK_SPACE) {
                    toggle_group(pid);
                }
            }
            return 0;
        }
        case NM_RCLICK: {
            const auto& ia = *reinterpret_cast<const NMITEMACTIVATE*>(&hdr);
            POINT pt = ia.ptAction;
            ClientToScreen(list_, &pt);
            do_row_menu(ia.iItem, pt);
            return 0;
        }
        case NM_CUSTOMDRAW: {
            auto& cd = *reinterpret_cast<NMLVCUSTOMDRAW*>(const_cast<NMHDR*>(&hdr));
            if (cd.nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
            if (cd.nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return draw_row(cd);
            if (cd.nmcd.dwDrawStage == CDDS_POSTPAINT) {
                // Das Explorer-Theme zeichnet unter den Zeilen Spaltentrenner (LVP_COLUMNDETAIL); zudecken.
                RECT rc{};
                GetClientRect(list_, &rc);
                const int count = ListView_GetItemCount(list_);
                RECT last{};
                if (count > 0 && ListView_GetItemRect(list_, count - 1, &last, LVIR_BOUNDS)) rc.top = last.bottom;
                else if (HWND header = ListView_GetHeader(list_)) {
                    RECT hr{};
                    GetWindowRect(header, &hr);
                    MapWindowPoints(nullptr, list_, reinterpret_cast<POINT*>(&hr), 2);
                    rc.top = hr.bottom;
                }
                if (rc.bottom > rc.top) {
                    HBRUSH bg = CreateSolidBrush(theme::palette().card_bg);
                    FillRect(cd.nmcd.hdc, &rc, bg);
                    DeleteObject(bg);
                }
                return CDRF_DODEFAULT;
            }
            return CDRF_DODEFAULT;
        }
    }
    return 0;
}

// Zwei Phasen: schnelles Ergebnis (Kernel-Fast-Path + Handles) sofort, Restart-Manager-Details nachgereicht.
// Jede Phase postet eine Kopie; veraltete Generationen werden verworfen.
void MainWindow::start_scan() {
    if (targets_.empty()) return;
    const unsigned generation = ++scan_generation_;
    scanning_ = true;
    set_busy(true);
    update_buttons();
    set_status(tr(S::StatusScanning), StatusKind::Busy);

    std::thread([hwnd = hwnd_, targets = targets_, generation]() mutable {
        // 8.3-Kurznamen (Skripte, alte Programme) für Titel, Chips und Datei-Aktionen in Langform. Hier im Scan-Thread,
        // weil das bei Netzpfaden blockieren kann; das Fenster bekommt die Namen vor dem ersten Ergebnis.
        bool renamed = false;
        for (auto& t : targets) {
            std::filesystem::path full(path_util::long_form(t));
            if (full.native() != t.native()) {
                t = std::move(full);
                renamed = true;
            }
        }
        if (renamed) {
            auto names = std::make_unique<std::vector<std::filesystem::path>>(targets);
            if (PostMessageW(hwnd, WM_APP_SCAN_TARGETS, generation, reinterpret_cast<LPARAM>(names.get()))) names.release();
        }

        ScanOptions fast;
        fast.use_restart_manager = false;
        auto result = scan(targets, fast);

        auto first = std::make_unique<ScanResult>(result);
        if (PostMessageW(hwnd, WM_APP_SCAN_FAST, generation, reinterpret_cast<LPARAM>(first.get()))) first.release();

        enrich(result, targets, ScanOptions{});
        auto second = std::make_unique<ScanResult>(std::move(result));
        if (PostMessageW(hwnd, WM_APP_SCAN_ENRICHED, generation, reinterpret_cast<LPARAM>(second.get()))) second.release();
    }).detach();
}

void MainWindow::set_busy(bool busy) {
    if (busy_ == busy) return;
    busy_ = busy;
    if (busy) SetTimer(hwnd_, kTimerProgress, 30, nullptr);
    else KillTimer(hwnd_, kTimerProgress);
    RECT pr = progress_rect();
    InflateRect(&pr, 0, 2);
    InvalidateRect(hwnd_, &pr, TRUE);
}

void MainWindow::on_timer(UINT_PTR id) {
    if (id == kTimerProgress) {
        ++progress_tick_;
        const RECT pr = progress_rect();
        InvalidateRect(hwnd_, &pr, FALSE);
    } else if (id == kTimerCollect) {
        KillTimer(hwnd_, kTimerCollect);
        RemovePropW(hwnd_, instance::kCollectProp);
    } else if (id == kTimerReveal) {
        const int rows = ListView_GetItemCount(list_);
        const long long total = kRevealRowMs + static_cast<long long>(std::min(rows, kRevealMaxStaggerRows)) * kRevealStaggerMs;
        if (static_cast<long long>(GetTickCount64() - reveal_start_) > total) {
            KillTimer(hwnd_, kTimerReveal);
            reveal_start_ = 0;
        }
        InvalidateRect(list_, nullptr, FALSE);
    }
}

void MainWindow::on_scan_result(unsigned generation, std::unique_ptr<ScanResult> result, bool final) {
    if (generation != scan_generation_) return;

    const bool had_selection = ListView_GetSelectedCount(list_) > 0;
    const auto keep = had_selection ? selected_lockers() : std::vector<Locker>{};
    const bool first_fill = !result_;

    scanning_ = false;
    if (final) set_busy(false);
    result_ = std::move(result);

    if (!result_->missing.empty()) {
        std::vector<std::filesystem::path> remaining;
        for (const auto& t : targets_) {
            const bool gone = std::any_of(result_->missing.begin(), result_->missing.end(),
                                          [&](const std::wstring& m) { return path_util::equals_ci(m, t.wstring()); });
            if (!gone) remaining.push_back(t);
        }
        const std::wstring missing_label = join_names_from(result_->missing, 4);
        if (remaining.empty()) {
            clear_targets(fmt(S::StatusMissing, missing_label), StatusKind::Error);
            return;
        }
        targets_ = std::move(remaining);
        update_title();
        update_tooltip();
        invalidate_header();
    }

    rebuild_rows();
    if (first_fill) start_reveal();
    if (had_selection) restore_selection(keep);

    std::set<std::uint32_t> pids;
    for (const auto& l : result_->lockers) pids.insert(l.pid);

    std::wstring status;
    StatusKind kind = StatusKind::Ok;
    if (result_->lockers.empty()) {
        status = targets_.size() == 1 ? tr(S::StatusFreeSingle) : tr(S::StatusFreeMulti);
    } else {
        const auto n = result_->lockers.size();
        const auto m = pids.size();
        const auto* lw = i18n::plural(n, S::LockOne, S::LockMany);
        const auto* pw = i18n::plural(m, S::ProcessOne, S::ProcessMany);
        status = fmt(S::StatusLocks, n, lw, m, pw);
        kind = StatusKind::Warning;
    }
    if (result_->snapshot_timeout) status += tr(S::StatusBudget);
    if (result_->has_protected) status += tr(S::StatusProtected);
    if (result_->access_denied && !elevated_) status += tr(S::StatusAccessDenied);
    if (!result_->missing.empty()) status += L" · " + fmt(S::StatusMissing, join_names_from(result_->missing, 4));
    if (!final) status += tr(S::StatusEnriching);
    set_status(std::move(status), kind);
    update_buttons();
}

void MainWindow::sort_by(int column) {
    if (column == sort_column_) sort_ascending_ = !sort_ascending_;
    else {
        sort_column_ = column;
        sort_ascending_ = true;
    }
    controls::set_header_sort(list_, sort_column_, sort_ascending_);
    const auto keep = selected_lockers();
    const bool had_selection = ListView_GetSelectedCount(list_) > 0;
    rebuild_rows();
    if (had_selection) restore_selection(keep);
}

namespace {
std::wstring lower_name(const ScanResult& result, std::uint32_t pid) {
    for (const auto& l : result.lockers) {
        if (l.pid == pid && !l.process_name.empty()) {
            std::wstring name = l.process_name;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            return name;
        }
    }
    return {};
}
}  // namespace

// Zustand pro PID für die Sitzung; zusätzlich merken wir den Prozessnamen persistent, damit z. B. explorer.exe
// beim nächsten Start wieder eingeklappt beginnt.
void MainWindow::set_group_collapsed(std::uint32_t pid, bool collapsed) {
    if (collapsed) collapsed_.insert(pid);
    else collapsed_.erase(pid);
    seen_groups_.insert(pid);
    if (!result_) return;
    const auto name = lower_name(*result_, pid);
    if (name.empty()) return;
    if (collapsed) collapsed_names_.insert(name);
    else collapsed_names_.erase(name);
}

// Startzustand einer neuen Gruppe: gemerkter Prozessname oder sehr viele Einträge → eingeklappt.
bool MainWindow::group_initially_collapsed(std::uint32_t pid, std::size_t count) {
    if (!seen_groups_.insert(pid).second) return collapsed_.contains(pid);
    bool collapsed = count > kCollapseThreshold;
    if (result_) {
        const auto name = lower_name(*result_, pid);
        if (!name.empty() && collapsed_names_.contains(name)) collapsed = true;
    }
    if (collapsed) collapsed_.insert(pid);
    return collapsed;
}

void MainWindow::toggle_group(std::uint32_t pid) {
    set_group_collapsed(pid, !collapsed_.contains(pid));
    rebuild_rows();
    // Fokus auf den Gruppenkopf, damit Tastaturnavigation weitergeht
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        if (rows_[static_cast<std::size_t>(i)].group && rows_[static_cast<std::size_t>(i)].pid == pid) {
            ListView_SetItemState(list_, i, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
            break;
        }
    }
}

void MainWindow::set_all_groups(bool collapsed) {
    if (!result_) return;
    std::set<std::uint32_t> pids;
    for (const auto& r : rows_) {
        if (r.group) pids.insert(r.pid);
    }
    // auch Gruppen erfassen, die gerade in rows_ fehlen würden (gibt es nicht – Köpfe sind immer sichtbar)
    for (const auto pid : pids) set_group_collapsed(pid, collapsed);
    rebuild_rows();
}

// Auswahl nach Neuaufbau anhand (pid, pfad) wiederherstellen.
void MainWindow::restore_selection(const std::vector<Locker>& keep) {
    if (!result_) return;
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const Row& r = rows_[static_cast<std::size_t>(i)];
        if (r.group) continue;
        const Locker& l = result_->lockers[r.locker];
        const bool match = std::any_of(keep.begin(), keep.end(), [&](const Locker& k) {
            return k.pid == l.pid && path_util::equals_ci(k.locked_path, l.locked_path) && k.kind == l.kind;
        });
        if (match) ListView_SetItemState(list_, i, LVIS_SELECTED, LVIS_SELECTED);
    }
}

// Sortieren, dann Prozesse mit vielen Einträgen zu Gruppen zusammenfassen (Reihenfolge der Gruppen folgt
// ihrem ersten Eintrag in der Sortierung).
void MainWindow::rebuild_rows() {
    // Fokussierte Zeile merken (Gruppe: PID, sonst PID + Pfad + Art), nach dem Neuaufbau wieder fokussieren
    struct FocusKey {
        bool valid = false;
        bool group = false;
        std::uint32_t pid = 0;
        std::wstring path;
        LockKind kind = LockKind::FileHandle;
    } focus;
    if (result_) {
        const int focused = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
        if (focused >= 0 && static_cast<std::size_t>(focused) < rows_.size()) {
            const Row& r = rows_[static_cast<std::size_t>(focused)];
            focus.valid = true;
            focus.group = r.group;
            focus.pid = r.pid;
            if (!r.group && r.locker < result_->lockers.size()) {
                focus.path = result_->lockers[r.locker].locked_path;
                focus.kind = result_->lockers[r.locker].kind;
            }
        }
    }

    rows_.clear();
    clipped_rows_.clear();
    if (!result_) {
        fill_list();
        return;
    }
    const auto& lockers = result_->lockers;
    std::vector<std::size_t> order(lockers.size());
    std::iota(order.begin(), order.end(), 0);

    const auto key_compare = [&](std::size_t a, std::size_t b) {
        const Locker& x = lockers[a];
        const Locker& y = lockers[b];
        int c = 0;
        switch (sort_column_) {
            case ColProcess: c = compare_ci(x.process_name, y.process_name); break;
            case ColPid: c = x.pid < y.pid ? -1 : x.pid > y.pid ? 1 : 0; break;
            case ColKind: c = static_cast<int>(x.kind) - static_cast<int>(y.kind); break;
            default: c = compare_ci(x.locked_path, y.locked_path); break;
        }
        if (c == 0) c = x.pid < y.pid ? -1 : x.pid > y.pid ? 1 : 0;
        if (c == 0) c = compare_ci(x.locked_path, y.locked_path);
        return sort_ascending_ ? c < 0 : c > 0;
    };
    std::stable_sort(order.begin(), order.end(), key_compare);

    std::map<std::uint32_t, std::size_t> per_pid;
    for (const auto& l : lockers) ++per_pid[l.pid];

    std::set<std::uint32_t> emitted;
    for (const std::size_t idx : order) {
        const auto pid = lockers[idx].pid;
        if (per_pid[pid] < kGroupThreshold) {
            rows_.push_back(Row{false, pid, idx, 1});
            continue;
        }
        if (emitted.contains(pid)) continue;
        emitted.insert(pid);
        const bool collapsed = group_initially_collapsed(pid, per_pid[pid]);
        rows_.push_back(Row{true, pid, idx, per_pid[pid]});
        if (collapsed) continue;
        for (const std::size_t member : order) {
            if (lockers[member].pid == pid) rows_.push_back(Row{false, pid, member, 1});
        }
    }
    fill_list();

    if (focus.valid) {
        int target = -1;
        for (int i = 0; i < static_cast<int>(rows_.size()) && target < 0; ++i) {
            const Row& r = rows_[static_cast<std::size_t>(i)];
            if (r.pid != focus.pid) continue;
            if (focus.group) {
                if (r.group) target = i;
            } else if (!r.group) {
                const Locker& l = lockers[r.locker];
                if (l.kind == focus.kind && path_util::equals_ci(l.locked_path, focus.path)) target = i;
            }
        }
        // Eintrag in eingeklappter Gruppe: Fokus auf den Gruppenkopf
        if (target < 0) {
            for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
                if (rows_[static_cast<std::size_t>(i)].group && rows_[static_cast<std::size_t>(i)].pid == focus.pid) target = i;
            }
        }
        if (target >= 0) ListView_SetItemState(list_, target, LVIS_FOCUSED, LVIS_FOCUSED);
    }
}

void MainWindow::fill_list() {
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);
    if (result_) {
        int row = 0;
        for (const auto& r : rows_) {
            const Locker& l = result_->lockers[r.locker];
            std::wstring name = display_name(l);
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
            item.iItem = row;
            item.lParam = row;
            item.iImage = icon_for(l.image_path);
            item.pszText = name.data();
            ListView_InsertItem(list_, &item);

            std::wstring pid = pid_text(l);
            ListView_SetItemText(list_, row, ColPid, pid.data());
            if (!r.group) {
                ListView_SetItemText(list_, row, ColKind, const_cast<wchar_t*>(kind_text(l.kind)));
                ListView_SetItemText(list_, row, ColPath, const_cast<wchar_t*>(l.locked_path.c_str()));
            }
            ++row;
        }
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    fit_last_column();
    InvalidateRect(list_, nullptr, TRUE);
}

// Ausgewählte Zeilen → Sperren; Gruppenzeilen stehen für alle ihre Einträge. Ohne Auswahl: alle.
std::vector<Locker> MainWindow::selected_lockers() const {
    std::vector<Locker> out;
    if (!result_) return out;
    if (ListView_GetSelectedCount(list_) == 0) return result_->lockers;

    std::set<std::size_t> picked;
    for (int i = ListView_GetNextItem(list_, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(list_, i, LVNI_SELECTED)) {
        if (static_cast<std::size_t>(i) >= rows_.size()) continue;
        const Row& r = rows_[static_cast<std::size_t>(i)];
        if (r.group) {
            for (std::size_t k = 0; k < result_->lockers.size(); ++k) {
                if (result_->lockers[k].pid == r.pid) picked.insert(k);
            }
        } else {
            picked.insert(r.locker);
        }
    }
    for (const std::size_t k : picked) out.push_back(result_->lockers[k]);
    return out;
}

void MainWindow::update_buttons() {
    const bool has_target = !targets_.empty() && !scanning_;
    const bool has_lockers = has_target && result_ && !result_->lockers.empty();
    EnableWindow(btn_open_, !scanning_);
    EnableWindow(btn_more_, !scanning_);  // enthält auch zielunabhängige Einträge (Kontextmenü-Schalter)
    EnableWindow(btn_unlock_, has_lockers);
    EnableWindow(btn_kill_, has_lockers);
    EnableWindow(btn_delete_, has_target);
}

void MainWindow::do_open_dialog() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_NODEREFERENCELINKS | FOS_ALLOWMULTISELECT);
    dialog->SetTitle(tr(S::OpenDialogTitle));

    if (SUCCEEDED(dialog->Show(hwnd_))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items))) {
            DWORD count = 0;
            items->GetCount(&count);
            std::vector<std::filesystem::path> paths;
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (FAILED(items->GetItemAt(i, &item))) continue;
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    paths.emplace_back(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
            items->Release();
            if (!paths.empty()) open(std::move(paths));
        }
    }
    dialog->Release();
}

void MainWindow::do_unlock(UnlockMode mode) {
    const auto lockers = selected_lockers();
    if (lockers.empty()) return;

    // Dieselbe Regel wie unlock(): Arbeitsverzeichnis-Sperren mit Handle werden geschlossen, nicht beendet.
    const bool kills =
        std::any_of(lockers.begin(), lockers.end(), [mode](const Locker& l) { return terminates_process(l, mode); });
    const bool touches_sensitive = std::any_of(lockers.begin(), lockers.end(), [](const Locker& l) { return l.is_sensitive; });

    if (kills || touches_sensitive) {
        const auto* instruction = kills ? tr(S::ConfirmKillTitle) : tr(S::ConfirmCloseSensitiveTitle);
        if (!confirm(hwnd_, fonts_, instruction, std::wstring(tr(S::ConfirmLoseData)) + L"\n\n" + describe(lockers),
                     kills ? tr(S::BtnKillShort) : tr(S::BtnCloseShort), true)) {
            return;
        }
    }

    const auto report = unlock(lockers, mode, false);
    const auto* hw = i18n::plural(report.handles_closed, S::HandleOne, S::HandleMany);
    const auto* pw = i18n::plural(report.processes_terminated, S::ProcOne, S::ProcMany);
    std::wstring status = fmt(S::UnlockReport, report.handles_closed, hw, report.processes_terminated, pw);
    if (!report.ok()) {
        std::wstring details;
        for (const auto& f : report.failures) {
            const std::wstring who = f.locker.kind == LockKind::Unattributed ? f.locker.locked_path
                                     : !f.locker.remote_user.empty()         ? remote_text(f.locker)
                                     : std::format(L"{} (PID {})", f.locker.process_name, f.locker.pid);
            details += std::format(L"• {}: {}\n", who, error_text(f.error));
        }
        inform(hwnd_, fonts_, tr(S::UnlockFailedTitle), details, true);
    }
    set_status(std::move(status), report.ok() ? StatusKind::Ok : StatusKind::Error);
    start_scan();
}

// Vor Datei-Aktionen: Rückfrage und Sperren lösen. false = Nutzer hat abgebrochen.
bool MainWindow::unlock_before_action(const wchar_t* instruction, const wchar_t* ok_text) {
    const auto lockers = selected_lockers();
    std::wstring content = targets_.size() == 1 ? targets_label() : fmt(S::TitleTargets, targets_.size());
    if (!lockers.empty()) content += L"\n\n" + std::wstring(tr(S::UnlockBefore)) + L"\n" + describe(lockers);
    if (!confirm(hwnd_, fonts_, instruction, content, ok_text, true)) return false;
    if (!lockers.empty()) unlock(lockers, UnlockMode::Auto, false);
    return true;
}

void MainWindow::do_delete() {
    if (targets_.empty()) return;
    if (!unlock_before_action(tr(S::ConfirmDeleteTitle), tr(S::BtnDeleteShort))) return;

    std::wstring failures;
    std::vector<std::filesystem::path> remaining;
    for (const auto& t : targets_) {
        if (auto ec = file_ops::remove(t)) {
            failures += std::format(L"• {}: {}\n", t.filename().wstring(), error_text(ec));
            remaining.push_back(t);
        }
    }

    if (!failures.empty()) {
        targets_ = std::move(remaining);
        update_title();
        update_tooltip();
        invalidate_header();
        if (elevated_) {
            if (confirm(hwnd_, fonts_, tr(S::DeleteFailedRebootTitle), failures, tr(S::BtnDeleteOnReboot), true)) {
                do_delete_on_reboot();
                return;
            }
        } else {
            inform(hwnd_, fonts_, tr(S::DeleteFailedTitle), failures + L"\n" + tr(S::DeleteFailedHint), true);
        }
        start_scan();
        return;
    }

    std::wstring status;
    if (targets_.size() == 1) status = fmt(S::DeletedOne, targets_.front().wstring());
    else status = fmt(S::DeletedMany, targets_.size());
    clear_targets(std::move(status), StatusKind::Ok);
}

void MainWindow::do_delete_on_reboot() {
    if (targets_.empty()) return;
    if (!elevated_) {
        inform(hwnd_, fonts_, tr(S::AdminRequiredTitle), tr(S::AdminRequiredReboot), false);
        return;
    }
    std::wstring failures;
    for (const auto& t : targets_) {
        if (auto ec = file_ops::remove_on_reboot(t)) failures += std::format(L"• {}: {}\n", t.filename().wstring(), error_text(ec));
    }
    if (!failures.empty()) {
        inform(hwnd_, fonts_, tr(S::RebootScheduleFailed), failures, true);
        return;
    }
    set_status(targets_.size() == 1 ? std::wstring(tr(S::RebootScheduledOne)) : fmt(S::RebootScheduledMany, targets_.size()),
               StatusKind::Ok);
}

void MainWindow::do_rename() {
    if (targets_.size() != 1) {
        if (!targets_.empty()) inform(hwnd_, fonts_, tr(S::RenameTitle), tr(S::RenameSingleOnly), false);
        return;
    }
    const auto target = targets_.front();
    std::wstring name = target.filename().wstring();
    std::error_code ec;
    const std::wstring stem = std::filesystem::is_directory(target, ec) ? name : target.stem().wstring();
    if (!dialog::prompt(hwnd_, tr(S::RenameTitle), target.parent_path().wstring(), tr(S::RenameTitle), name, stem, fonts_)) return;
    if (name.empty() || name == target.filename().wstring()) return;
    if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        inform(hwnd_, fonts_, tr(S::RenameInvalidTitle), tr(S::RenameInvalidChars), true);
        return;
    }

    const auto lockers = selected_lockers();
    if (!lockers.empty()) {
        if (!confirm(hwnd_, fonts_, tr(S::RenameConfirmTitle), describe(lockers), tr(S::RenameTitle), true)) return;
        unlock(lockers, UnlockMode::Auto, false);
    }
    if (auto err = file_ops::rename(target, name)) {
        inform(hwnd_, fonts_, tr(S::RenameFailedTitle), error_text(err), true);
        start_scan();
        return;
    }
    const auto renamed = target.parent_path() / name;
    set_status(fmt(S::RenamedTo, name), StatusKind::Ok);
    open(renamed);
}

void MainWindow::do_move_or_copy(bool copy) {
    if (targets_.empty()) return;
    const auto folder = pick_folder(hwnd_, copy ? tr(S::PickCopyFolder) : tr(S::PickMoveFolder));
    if (!folder) return;

    if (!copy && !unlock_before_action(tr(S::ConfirmMoveTitle), tr(S::BtnMoveShort))) return;

    std::wstring failures;
    std::vector<std::filesystem::path> moved;
    for (const auto& t : targets_) {
        const auto ec = copy ? file_ops::copy(t, *folder) : file_ops::move(t, *folder);
        if (ec) failures += std::format(L"• {}: {}\n", t.filename().wstring(), error_text(ec));
        else moved.push_back(*folder / t.filename());
    }
    if (!failures.empty()) {
        inform(hwnd_, fonts_, copy ? tr(S::CopyPartlyFailed) : tr(S::MovePartlyFailed), failures, true);
    }

    const auto n = moved.size();
    const auto* tw = i18n::plural(n, S::TargetOne, S::TargetMany);
    const auto dest = folder->wstring();
    const StatusKind kind = failures.empty() ? StatusKind::Ok : StatusKind::Error;
    if (copy) {
        set_status(fmt(S::CopiedTo, n, tw, dest), kind);
        start_scan();
        return;
    }
    if (moved.empty()) {
        start_scan();
        return;
    }
    set_status(fmt(S::MovedTo, n, tw, dest), kind);
    open(std::move(moved));
}

void MainWindow::do_show_in_explorer() {
    if (!targets_.empty()) show_in_explorer(hwnd_, targets_.front().wstring());
}

void MainWindow::do_more_menu() {
    HMENU menu = CreatePopupMenu();
    const UINT single = targets_.size() == 1 ? MF_ENABLED : MF_GRAYED;
    const UINT any = targets_.empty() ? MF_GRAYED : MF_ENABLED;
    AppendMenuW(menu, MF_STRING | single, IDM_RENAME, tr(S::MenuRename));
    AppendMenuW(menu, MF_STRING | any, IDM_MOVE, tr(S::MenuMove));
    AppendMenuW(menu, MF_STRING | any, IDM_COPY, tr(S::MenuCopy));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | any, IDM_SHOW_EXPLORER, tr(S::MenuShowExplorer));
    AppendMenuW(menu, MF_STRING | any, IDM_RESCAN, tr(S::MenuRescan));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (elevated_ && !targets_.empty() ? MF_ENABLED : MF_GRAYED), IDM_DELETE_REBOOT,
                tr(S::MenuDeleteReboot));
    // Globale Einstellung – auch ohne Ziel erreichbar; Häkchen = Sparse Package für diesen Benutzer registriert
    if (shell_package::supported()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const bool registered = !shell_package::registered_full_name().empty();
        AppendMenuW(menu, MF_STRING | (registered ? MF_CHECKED : 0) | (shell_busy_ ? MF_GRAYED : 0), IDM_SHELL_MENU,
                    tr(S::MenuShellIntegration));
    }

    RECT rc{};
    GetWindowRect(btn_more_, &rc);
    const UINT flags = TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON;
    const int cmd = static_cast<int>(TrackPopupMenuEx(menu, flags, rc.left, rc.top - dp(4), hwnd_, nullptr));
    DestroyMenu(menu);
    if (cmd) on_command(cmd);
}

void MainWindow::do_row_menu(int item, POINT screen) {
    if (!result_) return;
    const bool has_groups = std::any_of(rows_.begin(), rows_.end(), [](const Row& r) { return r.group; });
    HMENU menu = CreatePopupMenu();

    if (item >= 0) {
        if (!(ListView_GetItemState(list_, item, LVIS_SELECTED) & LVIS_SELECTED)) {
            ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(list_, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        const auto sel = selected_lockers();
        const bool any_handle = std::any_of(sel.begin(), sel.end(), [](const Locker& l) {
            return l.kind == LockKind::FileHandle || l.kind == LockKind::RemoteOpen;
        });
        const bool has_image = !sel.empty() && !sel.front().image_path.empty() && sel.front().image_path.find(L'\\') != std::wstring::npos;

        AppendMenuW(menu, MF_STRING | (any_handle ? MF_ENABLED : MF_GRAYED), IDM_ROW_CLOSE_HANDLE, tr(S::RowCloseHandle));
        AppendMenuW(menu, MF_STRING, IDM_ROW_KILL, tr(S::RowKill));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_ROW_SHOW_FILE, tr(S::RowShowFile));
        AppendMenuW(menu, MF_STRING | (has_image ? MF_ENABLED : MF_GRAYED), IDM_ROW_SHOW_IMAGE, tr(S::RowShowImage));
        AppendMenuW(menu, MF_STRING | (has_image ? MF_ENABLED : MF_GRAYED), IDM_ROW_PROPERTIES, tr(S::RowProperties));
    }
    if (has_groups) {
        if (item >= 0) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_EXPAND_ALL, tr(S::MenuExpandAll));
        AppendMenuW(menu, MF_STRING, IDM_COLLAPSE_ALL, tr(S::MenuCollapseAll));
    }
    if (GetMenuItemCount(menu) == 0) {
        DestroyMenu(menu);
        return;
    }
    const int cmd = static_cast<int>(TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                      screen.x, screen.y, hwnd_, nullptr));
    DestroyMenu(menu);
    if (cmd) on_command(cmd);
}

void MainWindow::do_header_menu(POINT screen) {
    HMENU menu = CreatePopupMenu();
    const struct {
        int column;
        int id;
        S title;
    } items[] = {{ColPid, IDM_COLUMN_PID, S::ColPid}, {ColKind, IDM_COLUMN_KIND, S::ColKind}, {ColPath, IDM_COLUMN_PATH, S::ColPath}};
    for (const auto& it : items) {
        AppendMenuW(menu, MF_STRING | (hidden_columns_.contains(it.column) ? 0 : MF_CHECKED), static_cast<UINT_PTR>(it.id), tr(it.title));
    }
    const int cmd = static_cast<int>(TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                      screen.x, screen.y, hwnd_, nullptr));
    DestroyMenu(menu);
    if (cmd) on_command(cmd);
}

// Ausblenden = Breite 0 (ListView kennt kein Verstecken); Breite wird für das Einblenden gemerkt.
void MainWindow::set_column_visible(int column, bool visible) {
    if (column <= ColProcess || column > ColPath) return;
    if (!visible) {
        if (hidden_columns_.insert(column).second) {
            column_width_[column] = ListView_GetColumnWidth(list_, column);
            ListView_SetColumnWidth(list_, column, 0);
        }
    } else if (hidden_columns_.erase(column)) {
        const int defaults[] = {260, 70, 130, 320};
        ListView_SetColumnWidth(list_, column, column_width_[column] > 0 ? column_width_[column] : dp(defaults[column]));
    }
    fit_last_column();
    InvalidateRect(list_, nullptr, TRUE);
}

void MainWindow::do_elevate() {
    if (auto ec = process::relaunch_elevated(quoted_targets())) {
        if (ec.value() != ERROR_CANCELLED) inform(hwnd_, fonts_, tr(S::ElevateFailedTitle), error_text(ec), true);
        return;
    }
    DestroyWindow(hwnd_);
}

// Windows-11-Kontextmenü ein-/ausschalten. Die Bereitstellung dauert Sekunden und blockiert – daher auf einem
// eigenen Thread; das Ergebnis kommt als WM_APP_SHELL_DONE zurück. Prop UnlockerShellBusy = 1 solange (für Tests).
void MainWindow::do_toggle_shell_menu() {
    if (shell_busy_ || !shell_package::supported()) return;
    const std::wstring full_name = shell_package::registered_full_name();
    std::filesystem::path msix;
    if (full_name.empty()) {
        msix = shell_package::package_file();
        if (msix.empty()) {
            inform(hwnd_, fonts_, tr(S::ShellPackageMissingTitle), tr(S::ShellPackageMissingHint), false);
            return;
        }
    }
    shell_busy_ = true;
    SetPropW(hwnd_, L"UnlockerShellBusy", reinterpret_cast<HANDLE>(1));
    set_status(tr(full_name.empty() ? S::ShellEnabling : S::ShellDisabling), StatusKind::Busy);

    std::thread([hwnd = hwnd_, full_name, msix, location = exe_directory()] {
        auto done = std::make_unique<ShellToggleDone>();
        done->enabled = full_name.empty();
        done->outcome = done->enabled ? shell_package::enable(msix, location) : shell_package::disable(full_name);
        if (PostMessageW(hwnd, WM_APP_SHELL_DONE, 0, reinterpret_cast<LPARAM>(done.get()))) done.release();
    }).detach();
}

void MainWindow::on_shell_toggled(bool enabled, long hr, const std::wstring& text) {
    shell_busy_ = false;
    RemovePropW(hwnd_, L"UnlockerShellBusy");
    if (hr == 0) {
        set_status(tr(enabled ? S::ShellEnabled : S::ShellDisabled), StatusKind::Ok);
        return;
    }
    set_status(tr(S::ShellToggleFailedTitle), StatusKind::Error);
    inform(hwnd_, fonts_, tr(S::ShellToggleFailedTitle), std::format(L"{}\n\n(0x{:08X})", text, static_cast<unsigned long>(hr)),
           true);
}

void MainWindow::clear_targets(std::wstring status, StatusKind kind) {
    ++scan_generation_;  // laufende Scans verwerfen
    scanning_ = false;
    set_busy(false);
    targets_.clear();
    result_.reset();
    rows_.clear();
    clipped_rows_.clear();
    chips_.clear();
    hot_chip_ = -1;
    fill_list();
    ShowWindow(list_, SW_HIDE);
    update_title();
    update_tooltip();
    set_status(std::move(status), kind);
    update_buttons();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::set_status(std::wstring text, StatusKind kind) {
    status_ = std::move(text);
    status_kind_ = kind;
    invalidate_status();
}

void MainWindow::invalidate_header() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    rc.bottom = dp(kCardTop);
    InvalidateRect(hwnd_, &rc, TRUE);
}

void MainWindow::invalidate_status() {
    const RECT rc = status_rect();
    InvalidateRect(hwnd_, &rc, TRUE);
}

}  // namespace unlocker::ui
