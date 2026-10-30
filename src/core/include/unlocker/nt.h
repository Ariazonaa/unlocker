#pragma once

// Native-API-Deklarationen, die winternl.h nicht (vollständig) bereitstellt.
// Alle Funktionen werden zur Laufzeit aus ntdll.dll aufgelöst – keine ntdll.lib,
// keine Konflikte mit den unvollständigen Prototypen des SDK.

#include <windows.h>
#include <winternl.h>

namespace unlocker::nt {

constexpr ULONG SystemExtendedHandleInformationClass = 64;
constexpr ULONG ObjectNameInformationClass = 1;
constexpr ULONG ObjectTypeInformationClass = 2;
constexpr ULONG FileProcessIdsUsingFileInformationClass = 47;

constexpr NTSTATUS StatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
constexpr NTSTATUS StatusBufferOverflow = static_cast<NTSTATUS>(0x80000005L);
constexpr NTSTATUS StatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023L);

struct SystemHandleTableEntryInfoEx {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
};

struct SystemHandleInformationEx {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SystemHandleTableEntryInfoEx Handles[1];
};

struct FileProcessIdsUsingFileInformation {
    ULONG NumberOfProcessIdsInList;
    ULONG_PTR ProcessIdList[1];
};

struct ObjectNameInformation {
    UNICODE_STRING Name;
};

using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQueryObjectFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtQueryInformationFileFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(NTSTATUS);

struct Api {
    NtQuerySystemInformationFn QuerySystemInformation = nullptr;
    NtQueryObjectFn QueryObject = nullptr;
    NtQueryInformationFileFn QueryInformationFile = nullptr;
    RtlNtStatusToDosErrorFn NtStatusToDosError = nullptr;  // NT-Status (z. B. vom Treiber) -> Win32-Fehlercode

    bool available() const noexcept { return QuerySystemInformation && QueryObject && QueryInformationFile; }
};

const Api& api();

}  // namespace unlocker::nt
