#include "unlocker/path_util.h"

#include "unlocker/win_handle.h"

#include <lm.h>
#include <winnetwk.h>

#include <cwctype>
#include <future>
#include <memory>
#include <optional>
#include <thread>

namespace unlocker::path_util {

std::wstring final_path(HANDLE file) {
    std::wstring buffer(512, L'\0');
    for (int attempt = 0; attempt < 3; ++attempt) {
        const DWORD len = GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                    FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (len == 0) return {};
        if (len < buffer.size()) {
            buffer.resize(len);
            return strip_prefix(std::move(buffer));
        }
        buffer.resize(len + 1);
    }
    return {};
}

std::wstring normalize(const std::filesystem::path& path) {
    WinHandle h(CreateFileW(to_extended(path).c_str(), FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (h) {
        if (auto resolved = final_path(h.get()); !resolved.empty()) return resolved;
    }
    return std::filesystem::absolute(path).lexically_normal().wstring();
}

std::wstring long_form(const std::filesystem::path& path) {
    const std::wstring extended = to_extended(path);
    std::wstring buffer(extended.size() + MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD len = GetLongPathNameW(extended.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0) break;
        if (len < buffer.size()) {
            buffer.resize(len);
            return strip_prefix(std::move(buffer));
        }
        buffer.resize(len + 1);
    }
    return path.wstring();
}

std::wstring strip_prefix(std::wstring path) {
    using namespace std::literals;
    if (path.starts_with(L"\\\\?\\UNC\\"sv)) {
        path.erase(2, 6);  // "\\?\UNC\srv" -> "\\srv"
    } else if (path.starts_with(L"\\\\?\\"sv)) {
        path.erase(0, 4);
    }
    return path;
}

std::wstring to_extended(const std::filesystem::path& path) {
    std::wstring abs = std::filesystem::absolute(path).lexically_normal().wstring();
    while (abs.size() > 3 && abs.ends_with(L'\\')) abs.pop_back();
    if (abs.starts_with(L"\\\\?\\")) return abs;
    if (abs.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + abs.substr(2);
    return L"\\\\?\\" + abs;
}

bool exists(const std::filesystem::path& path) {
    return GetFileAttributesW(to_extended(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool is_directory(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(to_extended(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

Reachability probe(const std::filesystem::path& path, std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) return path_util::exists(path) ? Reachability::Exists : Reachability::Missing;
    // to_extended() berührt das Netz nicht (nur GetFullPathName); der blockierende Teil ist GetFileAttributes.
    const std::wstring extended = to_extended(path);
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    std::thread([promise, extended] {
        promise->set_value(GetFileAttributesW(extended.c_str()) != INVALID_FILE_ATTRIBUTES);
    }).detach();
    if (future.wait_for(timeout) != std::future_status::ready) return Reachability::Timeout;
    return future.get() ? Reachability::Exists : Reachability::Missing;
}

namespace {

bool is_local_host(std::wstring_view host) {
    if (equals_ci(host, L"localhost") || host == L"127.0.0.1" || host == L"::1" || host == L".") return true;
    wchar_t name[256]{};
    DWORD len = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, name, &len) && equals_ci(host, name)) return true;
    len = 256;
    if (GetComputerNameExW(ComputerNameNetBIOS, name, &len) && equals_ci(host, name)) return true;
    len = 256;
    if (GetComputerNameExW(ComputerNameDnsFullyQualified, name, &len) && equals_ci(host, name)) return true;
    return false;
}

// "\\host\share\rest" -> lokaler Pfad, wenn host = dieser Rechner. Admin-Freigaben (C$) direkt,
// benannte Freigaben über NetShareGetInfo (Level 2 braucht Adminrechte; sonst bleibt der Pfad wie er ist).
std::optional<std::filesystem::path> localize_unc(std::wstring_view unc) {
    if (!unc.starts_with(L"\\\\") || unc.starts_with(L"\\\\?\\") || unc.starts_with(L"\\\\.\\")) return std::nullopt;
    const std::size_t host_end = unc.find(L'\\', 2);
    if (host_end == std::wstring_view::npos) return std::nullopt;
    const std::wstring_view host = unc.substr(2, host_end - 2);
    if (!is_local_host(host)) return std::nullopt;
    const std::size_t share_end = unc.find(L'\\', host_end + 1);
    const std::wstring_view share = unc.substr(host_end + 1, share_end == std::wstring_view::npos ? std::wstring_view::npos : share_end - host_end - 1);
    const std::wstring rest = share_end == std::wstring_view::npos ? L"" : std::wstring(unc.substr(share_end));
    if (share.empty()) return std::nullopt;

    if (share.size() == 2 && share[1] == L'$' && iswalpha(share[0])) {
        return std::filesystem::path(std::wstring(1, static_cast<wchar_t>(towupper(share[0]))) + L":" + (rest.empty() ? L"\\" : rest));
    }
    SHARE_INFO_2* info = nullptr;
    const std::wstring share_name(share);
    if (NetShareGetInfo(nullptr, const_cast<LPWSTR>(share_name.c_str()), 2, reinterpret_cast<LPBYTE*>(&info)) == NERR_Success && info) {
        std::filesystem::path local = info->shi2_path ? std::wstring(info->shi2_path) : L"";
        NetApiBufferFree(info);
        if (local.empty()) return std::nullopt;
        std::wstring joined = local.wstring();
        while (!joined.empty() && joined.back() == L'\\') joined.pop_back();
        return std::filesystem::path(joined + rest);
    }
    return std::nullopt;
}

}  // namespace

std::filesystem::path localize(const std::filesystem::path& path) {
    std::wstring text = path.wstring();
    if (text.starts_with(L"\\\\")) {
        if (auto local = localize_unc(text)) return *local;
        return path;
    }
    // Gemapptes Laufwerk? WNetGetConnection liefert den UNC-Pfad dahinter.
    if (text.size() >= 2 && text[1] == L':' && iswalpha(text[0])) {
        const std::wstring drive = text.substr(0, 2);
        wchar_t remote[1024]{};
        DWORD len = 1024;
        if (WNetGetConnectionW(drive.c_str(), remote, &len) == NO_ERROR) {
            if (auto local = localize_unc(std::wstring(remote) + text.substr(2))) return *local;
        }
    }
    return path;
}

bool equals_ci(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

bool is_same_or_inside(std::wstring_view candidate, std::wstring_view root) noexcept {
    while (!root.empty() && root.back() == L'\\') root.remove_suffix(1);
    if (candidate.size() < root.size()) return false;
    if (!equals_ci(candidate.substr(0, root.size()), root)) return false;
    return candidate.size() == root.size() || candidate[root.size()] == L'\\';
}

}  // namespace unlocker::path_util
