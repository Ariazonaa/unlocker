#include "unlocker/nt.h"

namespace unlocker::nt {

const Api& api() {
    static const Api instance = [] {
        Api a;
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            a.QuerySystemInformation = reinterpret_cast<NtQuerySystemInformationFn>(
                GetProcAddress(ntdll, "NtQuerySystemInformation"));
            a.QueryObject = reinterpret_cast<NtQueryObjectFn>(GetProcAddress(ntdll, "NtQueryObject"));
            a.QueryInformationFile = reinterpret_cast<NtQueryInformationFileFn>(
                GetProcAddress(ntdll, "NtQueryInformationFile"));
            a.NtStatusToDosError = reinterpret_cast<RtlNtStatusToDosErrorFn>(
                GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        }
        return a;
    }();
    return instance;
}

}  // namespace unlocker::nt
