#pragma once
// Gemeinsamer Vertrag zwischen dem Kernel-Treiber (src/driver) und dem User-Mode-Client
// (src/core/include/unlocker/driver_client.h). C-kompatibel, damit beide Seiten denselben Header
// verwenden – IOCTL-Codes und Strukturen müssen exakt übereinstimmen.
//
// Dual-Mode: im Kernel liefert wdm.h/ntddk.h die Typen, im User-Modus windows.h. Status ist LONG,
// weil NTSTATUS in beiden Welten 32 Bit ist – das spart die winternl.h-Abhängigkeit auf der
// User-Seite.

#if defined(_KERNEL_MODE)
// Treiber-Kontext: wdm.h/ntddk.h sind bereits inkludiert (CTL_CODE, METHOD_BUFFERED, FILE_ANY_ACCESS,
// ULONG, ULONG_PTR, LONG).
#else
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Benannte Geräteknoten. Der Treiber legt \\Device\UnlockerDriver an und verlinkt \\DosDevices.
#define UNLOCKER_DEVICE_NAME L"\\Device\\UnlockerDriver"
#define UNLOCKER_DOS_NAME    L"\\DosDevices\\UnlockerDriver"
#define UNLOCKER_USER_PATH   L"\\\\.\\UnlockerDriver"

// Eigenes Device-Type (hochgestellt), damit die IOCTL-Codes nicht mit anderen Treibern kollidieren.
#define UNLOCKER_DEVICE_TYPE 0x8000

// Ein einziger IOCTL: Handle im Zielprozess schließen. Der Treiber beschafft das Prozessobjekt
// (PsLookupProcessByProcessId), öffnet es mit PROCESS_DUP_HANDLE (ObOpenObjectByPointer) und ruft
// ZwDuplicateHandle mit DUPLICATE_CLOSE_SOURCE auf – exakt das, was User-Mode über
// OpenProcess + DuplicateHandle() macht, aber aus Kernel-Modus (um Integritäts-/Rechte-Lücken zu
// überbrücken, die User-Mode nicht schafft). Protected Processes (PPL) kann auch der Treiber nicht
// schließen; der liefert ihren Status zurück.
#define IOCTL_UNLOCKER_CLOSE_HANDLE \
    CTL_CODE(UNLOCKER_DEVICE_TYPE, 0x801, FILE_ANY_ACCESS, METHOD_BUFFERED)

typedef struct _UNLOCKER_INPUT {
    ULONG  Pid;          // Zielprozess
    ULONG_PTR HandleValue;  // Handle-Wert IM Zielprozess (wie vom Scanner gemeldet)
} UNLOCKER_INPUT, *PUNLOCKER_INPUT;

typedef struct _UNLOCKER_OUTPUT {
    LONG Status;  // STATUS_SUCCESS bei Erfolg, sonst z. B. STATUS_ACCESS_DENIED / STATUS_INVALID_HANDLE
} UNLOCKER_OUTPUT, *PUNLOCKER_OUTPUT;

#ifdef __cplusplus
}
#endif
