#include "settings.h"

#include <shlobj.h>

#include <cwchar>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

namespace unlocker::ui::settings {
namespace {

std::filesystem::path exe_directory() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return std::filesystem::path(path).parent_path();
}

std::size_t find_value(const std::string& text, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    std::size_t pos = text.find(needle);
    if (pos == std::string::npos) return std::string::npos;
    pos = text.find(':', pos);
    if (pos == std::string::npos) return std::string::npos;
    return pos + 1;
}

// Erste Ganzzahl/Bool nach "key": – oder nach dem i-ten Element einer Liste.
bool find_int(const std::string& text, const char* key, int& out, std::size_t list_index = 0) {
    std::size_t pos = find_value(text, key);
    if (pos == std::string::npos) return false;
    for (std::size_t skip = 0; skip < list_index; ++skip) {
        pos = text.find(',', pos);
        if (pos == std::string::npos) return false;
        ++pos;
    }
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '[' || text[pos] == '\n' || text[pos] == '\r' || text[pos] == '\t')) ++pos;
    if (pos >= text.size()) return false;
    if (text.compare(pos, 4, "true") == 0) {
        out = 1;
        return true;
    }
    if (text.compare(pos, 5, "false") == 0) {
        out = 0;
        return true;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str() + pos, &end, 10);
    if (end == text.c_str() + pos) return false;
    out = static_cast<int>(value);
    return true;
}

std::wstring from_utf8(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string to_utf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// Liste ganzer Zahlen [1, 2] nach "key"; stoppt beim ersten nicht-numerischen Element.
std::vector<int> find_int_list(const std::string& text, const char* key) {
    std::vector<int> out;
    std::size_t pos = find_value(text, key);
    if (pos == std::string::npos) return out;
    pos = text.find('[', pos);
    if (pos == std::string::npos) return out;
    const std::size_t end = text.find(']', pos);
    if (end == std::string::npos) return out;
    for (int v = 0; find_int(text, key, v, out.size());) {
        out.push_back(v);
        if (out.size() > 16) break;  // Schutz gegen Fehlparsen
    }
    return out;
}

// String-Liste ["a", "b"] nach "key":
std::vector<std::wstring> find_strings(const std::string& text, const char* key) {
    std::vector<std::wstring> out;
    std::size_t pos = find_value(text, key);
    if (pos == std::string::npos) return out;
    pos = text.find('[', pos);
    if (pos == std::string::npos) return out;
    const std::size_t end = text.find(']', pos);
    if (end == std::string::npos) return out;
    while (pos < end) {
        const std::size_t open = text.find('"', pos);
        if (open == std::string::npos || open >= end) break;
        std::string item;
        std::size_t i = open + 1;
        for (; i < end; ++i) {
            if (text[i] == '\\' && i + 1 < end) {
                item += text[++i];
            } else if (text[i] == '"') {
                break;
            } else {
                item += text[i];
            }
        }
        out.push_back(from_utf8(item));
        pos = i + 1;
    }
    return out;
}

}  // namespace

std::filesystem::path file() {
    const auto exe_dir = exe_directory();
    std::error_code ec;
    if (std::filesystem::exists(exe_dir / L"portable.ini", ec)) return exe_dir / L"settings.json";

    PWSTR local = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        base = local;
        CoTaskMemFree(local);
    } else {
        base = exe_dir;
    }
    return base / L"Unlocker" / L"settings.json";
}

Settings load() {
    Settings s;
    std::ifstream in(file(), std::ios::binary);
    if (!in) return s;
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();

    int v = 0;
    if (find_int(text, "left", v)) s.window.left = v;
    if (find_int(text, "top", v)) s.window.top = v;
    if (find_int(text, "right", v)) s.window.right = v;
    if (find_int(text, "bottom", v)) s.window.bottom = v;
    if (find_int(text, "maximized", v)) s.maximized = v != 0;
    for (std::size_t i = 0; i < 3; ++i) {
        if (find_int(text, "columns", v, i)) s.columns[i] = v;
    }
    if (find_int(text, "sortColumn", v) && v >= 0 && v <= 3) s.sort_column = v;
    if (find_int(text, "sortAscending", v)) s.sort_ascending = v != 0;
    s.collapsed_processes = find_strings(text, "collapsedProcesses");
    for (std::size_t i = 0; i < 3; ++i) {
        if (find_int(text, "hiddenColumns", v, i) && v >= 1 && v <= 3) s.hidden_columns.push_back(v);
        else break;
    }
    // Spaltenreihenfolge: nur übernehmen, wenn es eine Permutation von 0..3 ist (sonst Default).
    // 0-basiert: LVM_GET/SETCOLUMNORDERARRAY liefern/setzen Spaltenindizes 0..n-1 (MSDN nennt 1..n,
    // empirisch auf Win11 24H2 jedoch 0-basiert – Default-Read liefert 0,1,2,3).
    if (const auto order = find_int_list(text, "columnOrder"); order.size() == 4) {
        bool seen[4] = {false, false, false, false};
        bool valid = true;
        for (const int c : order)
            if (c < 0 || c > 3 || seen[c]) {
                valid = false;
                break;
            }
            else seen[c] = true;
        if (valid) s.column_order = std::move(order);
    }
    return s;
}

void save(const Settings& s) {
    const auto path = file();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;

    std::string names;
    for (const auto& name : s.collapsed_processes) {
        if (!names.empty()) names += ", ";
        names += "\"" + json_escape(to_utf8(name)) + "\"";
    }
    std::string hidden;
    for (const int c : s.hidden_columns) {
        if (!hidden.empty()) hidden += ", ";
        hidden += std::to_string(c);
    }
    std::string order;
    for (const int c : s.column_order) {
        if (!order.empty()) order += ", ";
        order += std::to_string(c);
    }
    out << std::format("{{\n  \"window\": {{ \"left\": {}, \"top\": {}, \"right\": {}, \"bottom\": {}, \"maximized\": {} }},\n"
                       "  \"columns\": [{}, {}, {}],\n"
                       "  \"hiddenColumns\": [{}],\n"
                       "  \"columnOrder\": [{}],\n"
                       "  \"sortColumn\": {}, \"sortAscending\": {},\n"
                       "  \"collapsedProcesses\": [{}]\n}}\n",
                       s.window.left, s.window.top, s.window.right, s.window.bottom, s.maximized ? "true" : "false",
                       s.columns[0], s.columns[1], s.columns[2], hidden, order, s.sort_column, s.sort_ascending ? "true" : "false", names);
}

}  // namespace unlocker::ui::settings
