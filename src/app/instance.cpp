#include "instance.h"

#include <algorithm>
#include <cwchar>
#include <string>
#include <string_view>

namespace unlocker::ui::instance {
namespace {

constexpr DWORD kMaxPayloadBytes = 4u << 20;  // Schutz gegen fremde/kaputte Nachrichten

struct Search {
    HANDLE want = nullptr;
    HWND found = nullptr;
};

BOOL CALLBACK find_collector(HWND hwnd, LPARAM lparam) {
    auto& search = *reinterpret_cast<Search*>(lparam);
    wchar_t name[64]{};
    if (GetClassNameW(hwnd, name, 64) == 0 || wcscmp(name, kWindowClass) != 0) return TRUE;
    if (GetPropW(hwnd, kCollectProp) != search.want) return TRUE;
    search.found = hwnd;
    return FALSE;
}

}  // namespace

bool StartupLock::acquire(bool elevated, DWORD timeout_ms) {
    mutex_.reset(CreateMutexW(nullptr, FALSE, elevated ? L"Local\\Unlocker.Startup.Elevated" : L"Local\\Unlocker.Startup"));
    if (!mutex_) return false;
    const DWORD wait = WaitForSingleObject(mutex_.get(), timeout_ms);
    owned_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;  // abandoned: Vorgänger abgestürzt, Sperre gehört uns
    return owned_;
}

void StartupLock::release() noexcept {
    if (owned_) ReleaseMutex(mutex_.get());
    owned_ = false;
}

bool forward(const std::vector<std::filesystem::path>& targets, bool elevated) {
    if (targets.empty()) return false;
    Search search{collect_prop_value(elevated), nullptr};
    EnumWindows(find_collector, reinterpret_cast<LPARAM>(&search));
    if (!search.found) return false;

    // Absolut auflösen: das sammelnde Fenster hat ein anderes Arbeitsverzeichnis.
    std::wstring payload;
    for (const auto& t : targets) {
        std::error_code ec;
        payload += std::filesystem::absolute(t, ec).wstring();
        payload += L'\0';
    }
    COPYDATASTRUCT data{};
    data.dwData = kCopyDataTargets;
    data.cbData = static_cast<DWORD>(payload.size() * sizeof(wchar_t));
    data.lpData = payload.data();
    DWORD_PTR accepted = FALSE;
    // Das Fenster lehnt ab (FALSE), wenn es inzwischen nicht mehr sammelt – dann eigenes Fenster.
    return SendMessageTimeoutW(search.found, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                               3000, &accepted) != 0 &&
           accepted == TRUE;
}

std::vector<std::filesystem::path> parse(const COPYDATASTRUCT& data) {
    std::vector<std::filesystem::path> targets;
    if (data.dwData != kCopyDataTargets || !data.lpData || data.cbData == 0 || data.cbData > kMaxPayloadBytes ||
        data.cbData % sizeof(wchar_t) != 0) {
        return targets;
    }
    const std::wstring_view text(static_cast<const wchar_t*>(data.lpData), data.cbData / sizeof(wchar_t));
    for (std::size_t pos = 0; pos < text.size();) {
        const std::size_t end = std::min(text.find(L'\0', pos), text.size());
        if (end > pos) targets.emplace_back(std::wstring(text.substr(pos, end - pos)));
        pos = end + 1;
    }
    return targets;
}

}  // namespace unlocker::ui::instance
