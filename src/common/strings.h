#pragma once

#include <string>

// Oberflächentexte de/en. Sprache = Windows-Anzeigesprache (Deutsch → de, sonst en).
namespace unlocker::i18n {

enum class S {
    AppName,
    TitleTargets,            // "{} Ziele – Unlocker"
    NoFileSelected,
    TargetsLabel,            // "{} Ziele: {}"
    TargetsMore,             // " … (+{})"
    BtnOpen,
    BtnMore,
    BtnKill,
    BtnDelete,
    BtnUnlock,
    BtnElevate,
    ColProcess,
    ColPid,
    ColKind,
    ColPath,
    KindHandle,
    KindModule,
    KindProcess,
    BadgeCritical,
    BadgeService,
    BadgeSensitive,
    EmptyTitle,
    EmptyHint,
    DropHint,
    StatusReady,
    StatusScanning,
    StatusFreeSingle,
    StatusFreeMulti,
    StatusLocks,             // "{} {} in {} {}"
    LockOne,
    LockMany,
    ProcessOne,
    ProcessMany,
    StatusBudget,
    StatusAccessDenied,
    StatusEnriching,
    StatusTargetMissing,
    ConfirmKillTitle,
    ConfirmCloseSensitiveTitle,
    ConfirmLoseData,
    BtnKillShort,
    BtnCloseShort,
    UnlockReport,            // "{} {} geschlossen, {} {} beendet"
    HandleOne,
    HandleMany,
    ProcOne,
    ProcMany,
    UnlockFailedTitle,
    Unknown,
    ConfirmDeleteTitle,
    BtnDeleteShort,
    UnlockBefore,
    DeleteFailedRebootTitle,
    BtnDeleteOnReboot,
    DeleteFailedTitle,
    DeleteFailedHint,
    DeletedOne,              // "Gelöscht: {}"
    DeletedMany,             // "{} Ziele gelöscht"
    AdminRequiredTitle,
    AdminRequiredReboot,
    RebootScheduleFailed,
    RebootScheduledOne,
    RebootScheduledMany,     // "{} Ziele …"
    RenameTitle,
    RenameSingleOnly,
    RenameInvalidTitle,
    RenameInvalidChars,
    RenameConfirmTitle,
    RenameFailedTitle,
    RenamedTo,               // "Umbenannt in {}"
    PickCopyFolder,
    PickMoveFolder,
    ConfirmMoveTitle,
    BtnMoveShort,
    CopyPartlyFailed,
    MovePartlyFailed,
    CopiedTo,                // "{} {} kopiert nach {}"
    MovedTo,
    TargetOne,
    TargetMany,
    MenuRename,
    MenuMove,
    MenuCopy,
    MenuShowExplorer,
    MenuRescan,
    MenuDeleteReboot,
    RowCloseHandle,
    RowKill,
    RowShowFile,
    RowShowImage,
    RowProperties,
    ElevateFailedTitle,
    OpenDialogTitle,
    BtnCancel,
    BtnOk,
    GroupEntries,            // "{} Einträge"
    ChipOverflow,            // "+{}"
    ChipRemoveTip,           // "Ziel entfernen"
    MenuExpandAll,
    MenuCollapseAll,
    KindCwd,
    BadgeProtected,
    StatusProtected,
    StatusMissing,           // "Nicht gefunden: {}"
    ErrCriticalProcess,
    ErrProtectedHandle,
    ErrProtectedProcess,
    ErrNotSupported,
    MenuShellIntegration,
    ShellEnabling,
    ShellDisabling,
    ShellEnabled,
    ShellDisabled,
    ShellPackageMissingTitle,
    ShellPackageMissingHint,
    ShellToggleFailedTitle,
    KindRemote,
    KindSystem,
    ErrSystemHeld,
    RemoteUser,              // "über Freigabe: {}"
    RemoteUserFrom,          // "über Freigabe: {} von {}" (Benutzer, Client-Rechner)
    ErrStillLocked,
    KindUnattributed,
    ErrNoProcess,
    Count_
};

const wchar_t* tr(S id);
bool is_german();

// Singular/Plural je nach n.
inline const wchar_t* plural(std::size_t n, S one, S many) { return tr(n == 1 ? one : many); }

}  // namespace unlocker::i18n
