#include "unlocker/process.h"

#include "unlocker/nt.h"
#include "unlocker/path_util.h"
#include "unlocker/win_handle.h"

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <vector>

namespace unlocker::process {

std::wstring image_path(std::uint32_t pid) {
    if (pid == 0) return L"System Idle Process";
    if (pid == 4) return L"System";

    WinHandle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!proc) return {};

    std::wstring buffer(1024, L'\0');
    DWORD len = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(proc.get(), 0, buffer.data(), &len)) return {};
    buffer.resize(len);
    return buffer;
}

std::wstring image_name_from_system(std::uint32_t pid) {
    const auto& api = nt::api();
    if (!api.QuerySystemInformation) return {};

    constexpr ULONG kSystemProcessInformation = 5;
    std::vector<std::byte> buffer(512 * 1024);
    for (int attempt = 0; attempt < 6; ++attempt) {
        ULONG needed = 0;
        const NTSTATUS status = api.QuerySystemInformation(kSystemProcessInformation, buffer.data(),
                                                           static_cast<ULONG>(buffer.size()), &needed);
        if (status == nt::StatusInfoLengthMismatch) {
            buffer.resize(std::max<std::size_t>(needed + needed / 8, buffer.size() * 2));
            continue;
        }
        if (!NT_SUCCESS(status)) return {};

        const std::byte* cursor = buffer.data();
        for (;;) {
            const auto* info = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(cursor);
            if (reinterpret_cast<ULONG_PTR>(info->UniqueProcessId) == pid) {
                if (info->ImageName.Buffer && info->ImageName.Length > 0) {
                    return std::wstring(info->ImageName.Buffer, info->ImageName.Length / sizeof(wchar_t));
                }
                return {};
            }
            if (info->NextEntryOffset == 0) break;
            cursor += info->NextEntryOffset;
        }
        return {};
    }
    return {};
}

std::wstring name_from_path(std::wstring_view image_path) {
    const auto pos = image_path.find_last_of(L"\\/");
    return std::wstring(pos == std::wstring_view::npos ? image_path : image_path.substr(pos + 1));
}

bool start_time(std::uint32_t pid, FILETIME& out) {
    WinHandle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!proc) return false;
    FILETIME exit{}, kernel{}, user{};
    return GetProcessTimes(proc.get(), &out, &exit, &kernel, &user) != FALSE;
}

namespace {

using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

NtQueryInformationProcessFn query_information_process() {
    static const auto fn = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtQueryInformationProcessFn>(GetProcAddress(ntdll, "NtQueryInformationProcess"))
                     : nullptr;
    }();
    return fn;
}

constexpr ULONG kProcessBasicInformation = 0;
constexpr ULONG kProcessWow64Information = 26;
constexpr ULONG kProcessProtectionInformation = 61;

// Offsets in RTL_USER_PROCESS_PARAMETERS (nicht in winternl.h): CurrentDirectory.DosPath
constexpr std::size_t kCurrentDirectoryOffset64 = 0x38;
constexpr std::size_t kCurrentDirectoryOffset32 = 0x24;
constexpr std::size_t kPeb32ProcessParametersOffset = 0x10;

struct UnicodeString32 {
    USHORT Length;
    USHORT MaximumLength;
    ULONG Buffer;
};

template <typename T>
bool read(HANDLE process, ULONG_PTR address, T& out) {
    SIZE_T got = 0;
    return address != 0 && ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), &out, sizeof(T), &got) &&
           got == sizeof(T);
}

std::wstring read_string(HANDLE process, ULONG_PTR buffer, USHORT length_bytes) {
    if (buffer == 0 || length_bytes == 0 || length_bytes > 64 * 1024) return {};
    std::wstring text(length_bytes / sizeof(wchar_t), L'\0');
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(buffer), text.data(), length_bytes, &got)) return {};
    text.resize(got / sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0') text.pop_back();
    return text;
}

}  // namespace

bool is_protected_process(std::uint32_t pid) {
    const auto query = query_information_process();
    if (!query) return false;
    WinHandle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!proc) return false;
    // PS_PROTECTION: Bits 0-2 Type (0 = None, 1 = ProtectedLight, 2 = Protected), Bit 3 Audit, Bits 4-7 Signer
    UCHAR protection = 0;
    ULONG len = 0;
    if (!NT_SUCCESS(query(proc.get(), kProcessProtectionInformation, &protection, sizeof(protection), &len))) return false;
    return (protection & 0x7) != 0;
}

bool is_wow64(HANDLE process) {
    BOOL wow64 = FALSE;
    return IsWow64Process(process, &wow64) && wow64;
}

std::wstring current_directory(HANDLE process) {
    const auto query = query_information_process();
    if (!query || !process) return {};

    ULONG len = 0;
    if (is_wow64(process)) {
        ULONG_PTR peb32 = 0;
        if (!NT_SUCCESS(query(process, kProcessWow64Information, &peb32, sizeof(peb32), &len)) || peb32 == 0) return {};
        ULONG params = 0;
        if (!read(process, peb32 + kPeb32ProcessParametersOffset, params)) return {};
        UnicodeString32 cwd{};
        if (!read(process, static_cast<ULONG_PTR>(params) + kCurrentDirectoryOffset32, cwd)) return {};
        return read_string(process, cwd.Buffer, cwd.Length);
    }

    PROCESS_BASIC_INFORMATION pbi{};
    if (!NT_SUCCESS(query(process, kProcessBasicInformation, &pbi, sizeof(pbi), &len)) || !pbi.PebBaseAddress) return {};
    PEB peb{};
    if (!read(process, reinterpret_cast<ULONG_PTR>(pbi.PebBaseAddress), peb) || !peb.ProcessParameters) return {};
    UNICODE_STRING cwd{};
    if (!read(process, reinterpret_cast<ULONG_PTR>(peb.ProcessParameters) + kCurrentDirectoryOffset64, cwd)) return {};
    return read_string(process, reinterpret_cast<ULONG_PTR>(cwd.Buffer), cwd.Length);
}

bool is_critical(std::uint32_t pid, std::wstring_view process_name) noexcept {
    if (pid == 0 || pid == 4) return true;
    static constexpr std::array critical = {
        L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"smss.exe", L"services.exe",
        L"lsass.exe", L"dwm.exe",     L"fontdrvhost.exe",
        L"Registry",  L"MemCompression",  // Minimal-Prozesse des Kernels (Registry hält die Hives)
    };
    for (const auto* name : critical) {
        if (path_util::equals_ci(process_name, name)) return true;
    }
    return false;
}

bool is_sensitive(std::wstring_view process_name) noexcept {
    return path_util::equals_ci(process_name, L"explorer.exe") || path_util::equals_ci(process_name, L"svchost.exe");
}

std::error_code terminate(std::uint32_t pid, std::uint32_t wait_ms) {
    WinHandle proc(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid));
    if (!proc) return last_error();
    if (!TerminateProcess(proc.get(), 1)) return last_error();
    if (WaitForSingleObject(proc.get(), wait_ms) == WAIT_TIMEOUT) return win_error(WAIT_TIMEOUT);
    return {};
}

bool enable_debug_privilege() {
    WinHandle token;
    {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw)) return false;
        token.reset(raw);
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) return false;
    if (!AdjustTokenPrivileges(token.get(), FALSE, &tp, 0, nullptr, nullptr)) return false;
    return GetLastError() == ERROR_SUCCESS;
}

bool is_elevated() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    WinHandle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size)) return false;
    return elevation.TokenIsElevated != 0;
}

std::error_code relaunch_elevated(std::wstring_view arguments) {
    std::wstring exe(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (len == 0) return last_error();
    exe.resize(len);

    const std::wstring args(arguments);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = args.empty() ? nullptr : args.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return last_error();
    return {};
}

}  // namespace unlocker::process
