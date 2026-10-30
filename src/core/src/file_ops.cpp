#include "unlocker/file_ops.h"

#include "unlocker/path_util.h"
#include "unlocker/win_handle.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace unlocker::file_ops {
namespace {

namespace fs = std::filesystem;

bool is_dot_entry(const wchar_t* name) noexcept {
    return name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

// Post-order über einen Ordnerbaum (Kinder vor Eltern); Reparse-Points werden nicht betreten.
std::error_code walk_post_order(const std::wstring& extended, const std::function<std::error_code(const std::wstring&, bool directory)>& visit) {
    const DWORD attrs = GetFileAttributesW(extended.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return last_error();

    const bool directory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool reparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    if (directory && !reparse) {
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW((extended + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                       nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find != INVALID_HANDLE_VALUE) {
            std::error_code first;
            do {
                if (is_dot_entry(data.cFileName)) continue;
                if (auto ec = walk_post_order(extended + L"\\" + data.cFileName, visit); ec && !first) first = ec;
            } while (FindNextFileW(find, &data));
            FindClose(find);
            if (first) return first;
        }
    }
    return visit(extended, directory);
}

std::error_code clear_readonly(const std::wstring& extended) {
    const DWORD attrs = GetFileAttributesW(extended.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return last_error();
    if ((attrs & FILE_ATTRIBUTE_READONLY) && !SetFileAttributesW(extended.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY)) {
        return last_error();
    }
    return {};
}

// Windows 10 RS1+: Name sofort entfernen, auch wenn andere Handles (mit FILE_SHARE_DELETE) offen sind.
std::error_code posix_delete(const std::wstring& extended, bool directory) {
    WinHandle h(CreateFileW(extended.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h) return last_error();

    FILE_DISPOSITION_INFO_EX info{};
    info.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                 FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
    if (!SetFileInformationByHandle(h.get(), FileDispositionInfoEx, &info, sizeof(info))) return last_error();
    return {};
}

std::error_code remove_single(const std::wstring& extended, bool directory) {
    clear_readonly(extended);
    const BOOL ok = directory ? RemoveDirectoryW(extended.c_str()) : DeleteFileW(extended.c_str());
    if (ok) return {};

    const DWORD err = GetLastError();
    if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED || err == ERROR_LOCK_VIOLATION) {
        if (!posix_delete(extended, directory)) return {};
    }
    return win_error(err);
}

// Gibt es im Ordner außer `except` eine Datei, deren Name auf `suffix` endet?
bool other_file_ends_with(const fs::path& dir, const std::wstring& suffix, const std::wstring& except) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(path_util::to_extended(dir / (L"*" + suffix)).c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        const std::wstring_view name = data.cFileName;
        if (!name.starts_with(L"~$") && !path_util::equals_ci(name, except)) found = true;
    } while (!found && FindNextFileW(find, &data));
    FindClose(find);
    return found;
}

// Sperrdateien von Office-Programmen zum gelöschten Dokument mit entfernen: LibreOffice ".~lock.<Name>#", Microsoft
// Office "~$<Name>" (Word kürzt längere Namen vorne um ein bis zwei Zeichen). Nur kleine Dateien (≤ 4 KB); die
// gekürzten Formen nur, wenn kein anderes Dokument im Ordner dazu passt. Hält ein Programm die Sperrdatei noch,
// bleibt sie liegen – Fehler werden bewusst ignoriert.
void remove_lock_files(const fs::path& document) {
    const std::wstring name = document.filename().wstring();
    if (name.empty() || name.starts_with(L"~$") || name.starts_with(L".~lock.")) return;
    const fs::path dir = document.parent_path();
    std::vector<std::wstring> candidates = {L".~lock." + name + L"#", L"~$" + name};
    for (std::size_t cut = 1; cut <= 2 && cut + 1 < name.size(); ++cut) {
        const std::wstring rest = name.substr(cut);
        if (!other_file_ends_with(dir, rest, name)) candidates.push_back(L"~$" + rest);
    }
    for (const auto& candidate : candidates) {
        const std::wstring extended = path_util::to_extended(dir / candidate);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(extended.c_str(), GetFileExInfoStandard, &data)) continue;
        if ((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
            data.nFileSizeHigh != 0 || data.nFileSizeLow > 4096) {
            continue;
        }
        clear_readonly(extended);
        DeleteFileW(extended.c_str());
    }
}

// Ziel als \\?\-Pfad; ist destination ein vorhandener Ordner, kommt der Quellname dazu. Die Ordnerprüfung läuft
// ebenfalls über \\?\ – std::filesystem scheitert ohne LongPathsEnabled jenseits von MAX_PATH.
std::wstring resolve_destination(const fs::path& source, const fs::path& destination) {
    if (path_util::is_directory(destination)) return path_util::to_extended(destination / source.filename());
    return path_util::to_extended(destination);
}

}  // namespace

std::error_code remove(const fs::path& path) {
    const bool directory = path_util::is_directory(path);
    if (auto ec = walk_post_order(path_util::to_extended(path), remove_single)) return ec;
    if (!directory) remove_lock_files(path);
    return {};
}

std::error_code remove_on_reboot(const fs::path& path) {
    return walk_post_order(path_util::to_extended(path), [](const std::wstring& extended, bool) -> std::error_code {
        if (!MoveFileExW(extended.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) return last_error();
        return {};
    });
}

std::error_code rename(const fs::path& path, std::wstring_view new_name) {
    const auto target = path_util::to_extended(path.parent_path() / fs::path(new_name));
    if (!MoveFileExW(path_util::to_extended(path).c_str(), target.c_str(), 0)) return last_error();
    return {};
}

std::error_code move(const fs::path& path, const fs::path& destination) {
    const auto target = resolve_destination(path, destination);
    if (!MoveFileExW(path_util::to_extended(path).c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED)) {
        return last_error();
    }
    return {};
}

std::error_code copy(const fs::path& path, const fs::path& destination) {
    std::error_code ec;
    fs::copy(fs::path(path_util::to_extended(path)), fs::path(resolve_destination(path, destination)),
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    return ec;
}

}  // namespace unlocker::file_ops
