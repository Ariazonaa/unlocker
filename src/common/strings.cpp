#include "strings.h"

#include <windows.h>

namespace unlocker::i18n {
namespace {

struct Entry {
    const wchar_t* de;
    const wchar_t* en;
};

// Reihenfolge muss S entsprechen.
constexpr Entry kTable[] = {
    {L"Unlocker", L"Unlocker"},
    {L"{} Ziele – Unlocker", L"{} targets – Unlocker"},
    {L"Keine Datei ausgewählt", L"No file selected"},
    {L"{} Ziele: {}", L"{} targets: {}"},
    {L" … (+{})", L" … (+{})"},
    {L"Öffnen…", L"Open…"},
    {L"Weitere", L"More"},
    {L"Prozess beenden", L"End process"},
    {L"Löschen", L"Delete"},
    {L"Entsperren", L"Unlock"},
    {L"Als Administrator", L"As administrator"},
    {L"Prozess", L"Process"},
    {L"PID", L"PID"},
    {L"Art", L"Type"},
    {L"Pfad", L"Path"},
    {L"Handle", L"Handle"},
    {L"Geladenes Modul", L"Loaded module"},
    {L"Prozess", L"Process"},
    {L"kritisch", L"critical"},
    {L"Dienst", L"service"},
    {L"Vorsicht", L"caution"},
    {L"Dateien oder Ordner hierher ziehen", L"Drop files or folders here"},
    {L"oder über „Öffnen…“ (Strg+O) auswählen – Unlocker zeigt, welche Prozesse sperren.",
     L"or pick them via “Open…” (Ctrl+O) – Unlocker shows which processes hold them."},
    {L"Loslassen zum Prüfen", L"Release to check"},
    {L"Bereit", L"Ready"},
    {L"Suche Sperren…", L"Looking for locks…"},
    {L"Keine Sperren – die Datei ist frei.", L"No locks – the file is free."},
    {L"Keine Sperren – alle Ziele sind frei.", L"No locks – all targets are free."},
    {L"{} {} in {} {}", L"{} {} in {} {}"},
    {L"Sperre", L"lock"},
    {L"Sperren", L"locks"},
    {L"Prozess", L"process"},
    {L"Prozessen", L"processes"},
    {L" · Zeitbudget erreicht, Liste evtl. unvollständig", L" · time budget reached, list may be incomplete"},
    {L" · Einige Prozesse nicht lesbar – als Administrator neu starten",
     L" · some processes not readable – restart as administrator"},
    {L" · ergänze Details…", L" · adding details…"},
    {L"Ziel nicht gefunden", L"Target not found"},
    {L"Prozesse beenden?", L"End processes?"},
    {L"Handles in Systemprozessen schließen?", L"Close handles in system processes?"},
    {L"Nicht gespeicherte Daten in diesen Programmen gehen verloren:", L"Unsaved data in these programs will be lost:"},
    {L"Beenden", L"End"},
    {L"Schließen", L"Close"},
    {L"{} {} geschlossen, {} {} beendet", L"{} {} closed, {} {} ended"},
    {L"Handle", L"handle"},
    {L"Handles", L"handles"},
    {L"Prozess", L"process"},
    {L"Prozesse", L"processes"},
    {L"Nicht alle Sperren konnten gelöst werden", L"Not all locks could be released"},
    {L"Unbekannt", L"Unknown"},
    {L"Endgültig löschen?", L"Delete permanently?"},
    {L"Löschen", L"Delete"},
    {L"Vorher werden Sperren gelöst in:", L"Locks will be released first in:"},
    {L"Löschen fehlgeschlagen – beim nächsten Neustart löschen?", L"Delete failed – delete on next restart?"},
    {L"Beim Neustart löschen", L"Delete on restart"},
    {L"Löschen fehlgeschlagen", L"Delete failed"},
    {L"Als Administrator können die Dateien beim nächsten Neustart gelöscht werden.",
     L"As administrator the files can be deleted on the next restart."},
    {L"Gelöscht: {}", L"Deleted: {}"},
    {L"{} Ziele gelöscht", L"{} targets deleted"},
    {L"Administratorrechte nötig", L"Administrator rights required"},
    {L"„Beim Neustart löschen“ schreibt in die Systemregistrierung – bitte als Administrator neu starten.",
     L"“Delete on restart” writes to the system registry – please restart as administrator."},
    {L"Vormerken fehlgeschlagen", L"Scheduling failed"},
    {L"Wird beim nächsten Neustart gelöscht", L"Will be deleted on next restart"},
    {L"{} Ziele werden beim nächsten Neustart gelöscht", L"{} targets will be deleted on next restart"},
    {L"Umbenennen", L"Rename"},
    {L"Bitte nur ein Ziel auswählen.", L"Please select a single target."},
    {L"Ungültiger Name", L"Invalid name"},
    {L"Der Name darf keines der Zeichen \\ / : * ? \" < > | enthalten.",
     L"The name must not contain any of \\ / : * ? \" < > |."},
    {L"Sperren lösen und umbenennen?", L"Release locks and rename?"},
    {L"Umbenennen fehlgeschlagen", L"Rename failed"},
    {L"Umbenannt in {}", L"Renamed to {}"},
    {L"Zielordner zum Kopieren wählen", L"Choose destination folder for copy"},
    {L"Zielordner zum Verschieben wählen", L"Choose destination folder for move"},
    {L"Verschieben?", L"Move?"},
    {L"Verschieben", L"Move"},
    {L"Kopieren teilweise fehlgeschlagen", L"Copy partly failed"},
    {L"Verschieben teilweise fehlgeschlagen", L"Move partly failed"},
    {L"{} {} kopiert nach {}", L"{} {} copied to {}"},
    {L"{} {} verschoben nach {}", L"{} {} moved to {}"},
    {L"Ziel", L"target"},
    {L"Ziele", L"targets"},
    {L"Umbenennen…\tF2", L"Rename…\tF2"},
    {L"Verschieben nach…", L"Move to…"},
    {L"Kopieren nach…", L"Copy to…"},
    {L"Im Explorer anzeigen", L"Show in Explorer"},
    {L"Neu suchen\tF5", L"Rescan\tF5"},
    {L"Beim nächsten Neustart löschen", L"Delete on next restart"},
    {L"Handle schließen", L"Close handle"},
    {L"Prozess beenden", L"End process"},
    {L"Datei im Explorer anzeigen", L"Show file in Explorer"},
    {L"Programm im Explorer anzeigen", L"Show program in Explorer"},
    {L"Eigenschaften des Programms", L"Program properties"},
    {L"Neustart als Administrator fehlgeschlagen", L"Restart as administrator failed"},
    {L"Gesperrte Dateien auswählen", L"Select locked files"},
    {L"Abbrechen", L"Cancel"},
    {L"OK", L"OK"},
    {L"{} Einträge", L"{} entries"},
    {L"+{}", L"+{}"},
    {L"Ziel entfernen", L"Remove target"},
    {L"Alle Gruppen ausklappen", L"Expand all groups"},
    {L"Alle Gruppen einklappen", L"Collapse all groups"},
    {L"Arbeitsverzeichnis", L"Working directory"},
    {L"geschützt", L"protected"},
    {L" · geschützte Prozesse – nicht lösbar", L" · protected processes – cannot be released"},
    {L"Nicht gefunden: {}", L"Not found: {}"},
    {L"Systemprozess – wird nicht angefasst", L"System process – left untouched"},
    {L"Handle ist gegen Schließen geschützt", L"Handle is protected from closing"},
    {L"Geschützter Prozess (PPL) – auch als Administrator nicht zugreifbar", L"Protected process (PPL) – not accessible even as administrator"},
    {L"Sperrart lässt sich nicht lösen", L"Lock type cannot be released"},
    {L"Windows-11-Kontextmenü", L"Windows 11 context menu"},
    {L"Kontextmenü wird eingerichtet…", L"Setting up the context menu…"},
    {L"Kontextmenü wird entfernt…", L"Removing the context menu…"},
    {L"Windows-11-Kontextmenü aktiviert", L"Windows 11 context menu enabled"},
    {L"Windows-11-Kontextmenü deaktiviert", L"Windows 11 context menu disabled"},
    {L"Kontextmenü-Paket nicht gefunden", L"Context menu package not found"},
    {L"Das signierte Paket baut einmalig packaging\\register-w11-menu.ps1 (Windows SDK, Dev-Zertifikat). "
     L"Danach lässt sich das Kontextmenü hier ein- und ausschalten.",
     L"The signed package is built once by packaging\\register-w11-menu.ps1 (Windows SDK, dev certificate). "
     L"Afterwards the context menu can be switched on and off here."},
    {L"Kontextmenü konnte nicht geändert werden", L"Could not change the context menu"},
    {L"Netzwerkfreigabe", L"Network share"},
    {L"System (Kernel)", L"System (kernel)"},
    {L"Vom System gehalten (Kernel/Treiber) – lässt sich nicht lösen, nur beim Neustart löschen",
     L"Held by the system (kernel/driver) – cannot be released, only deleted on restart"},
    {L"über Freigabe: {}", L"via share: {}"},
    {L"über Freigabe: {} von {}", L"via share: {} from {}"},
    {L"Zugriff auf dem Server geschlossen, aber die Datei ist weiter gesperrt – das Programm beim Client beenden oder beim Neustart löschen",
     L"Share access closed on the server, but the file is still locked – end the program on the client or delete on restart"},
    {L"Gesperrt, kein Prozess gefunden", L"Locked, no process found"},
    {L"Gesperrt, aber kein Prozess gefunden – lässt sich nicht lösen, nur beim Neustart löschen",
     L"Locked, but no process found – cannot be released, only deleted on restart"},
};
static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<std::size_t>(S::Count_), "Tabelle und enum S passen nicht");

}  // namespace

bool is_german() {
    static const bool german = [] {
        wchar_t override_lang[8]{};
        if (GetEnvironmentVariableW(L"UNLOCKER_LANG", override_lang, 8) > 0) {
            return override_lang[0] == L'd' || override_lang[0] == L'D';
        }
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
    }();
    return german;
}

const wchar_t* tr(S id) {
    const auto& e = kTable[static_cast<std::size_t>(id)];
    return is_german() ? e.de : e.en;
}

}  // namespace unlocker::i18n
