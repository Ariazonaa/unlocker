# Unlocker {{VERSION}} ({{ARCH}}) – portable

Gesperrte Dateien und Ordner unter Windows 11 freigeben: zeigt, welcher Prozess eine Datei festhält, schließt das
Handle oder beendet den Prozess, löscht/verschiebt/benennt um – auch wenn der Explorer „Datei wird verwendet“ meldet.
Keine Installation, keine Laufzeitbibliotheken, keine Dienste. Einfach entpacken und starten.

## Inhalt

| Datei | Zweck |
|---|---|
| `unlocker.exe` | Oberfläche. Datei oder Ordner als Argument übergeben oder ins Fenster ziehen. |
| `unlockerc.exe` | Kommandozeile für Skripte, `unlockerc --help`. |
| `unlocker_shell.dll` | Explorer-Kontextmenü („Mit Unlocker entsperren“) für das neue Windows-11-Menü. |
| `kontextmenu\` | Skripte zum Ein- und Austragen des Kontextmenüs (siehe unten). |
| `portable.ini` | Schaltet den Portable-Modus ein: Einstellungen bleiben in diesem Ordner. |
| `SHA256SUMS.txt` | Prüfsummen aller Dateien. |
| `LICENSE` | MIT-Lizenz, Copyright (c) 2026 ariazonaa. |

## Schnellstart

- **Oberfläche:** `unlocker.exe "C:\Pfad\datei.ext"` – oder `unlocker.exe` starten und Dateien hineinziehen.
  *Entsperren* schließt Handles (beendet Prozesse nur bei geladenen Modulen), *Prozess beenden* beendet die
  Prozesse, *Löschen* entsperrt und löscht. Weitere Aktionen (Umbenennen, Verschieben, Kopieren, beim Neustart
  löschen) unter *Weitere*.
- **Kommandozeile:**
  ```
  unlockerc "C:\Pfad\datei.ext"                   Sperren anzeigen (Exit 1 = Sperren gefunden)
  unlockerc "C:\Pfad\datei.ext" --unlock --yes    Handles schließen
  unlockerc "C:\Pfad\ordner" --delete --yes       entsperren und löschen
  unlockerc "C:\Pfad" --format csv                eine Zeile je Sperre (auch table, json)
  ```
  Exit-Codes: 0 frei/erfolgreich · 1 Sperren gefunden · 2 Fehler · 3 Zugriff verweigert (als Administrator wiederholen).
- **Administrator:** Ohne Adminrechte sind nur eigene Prozesse lesbar. Die Oberfläche bietet dann *Als Administrator*
  an; die Kommandozeile meldet Exit 3.

## Kontextmenü einrichten

Beide Varianten lassen sich mit `-Remove` wieder entfernen.

- **Windows 11, neues Menü** (Eintrag direkt im Rechtsklick-Menü) – PowerShell **als Administrator** in diesem Ordner:
  ```powershell
  .\kontextmenu\register-w11-menu.ps1 -TrustCert
  ```
  Registriert `unlocker_shell.dll` als Sparse Package. Benötigt `makeappx.exe`/`signtool.exe` aus dem Windows SDK
  und vertraut einem lokal erzeugten Entwicklerzertifikat. Den Ordner danach nicht mehr verschieben (sonst neu
  registrieren). Danach lässt sich das Menü auch in der App unter **Weitere › Windows-11-Kontextmenü** ein- und
  ausschalten.
- **Klassisches Menü** (unter „Weitere Optionen anzeigen“, keine Adminrechte, kein SDK):
  ```powershell
  .\kontextmenu\register-classic-menu.ps1
  ```

## Portable-Modus

Liegt eine Datei `portable.ini` neben `unlocker.exe`, speichert Unlocker Fensterposition, Spaltenbreiten,
Sortierung und eingeklappte Gruppen in `settings.json` **im selben Ordner**. Der Inhalt der Datei ist egal – nur
ihre Existenz zählt. Ohne `portable.ini` liegt `settings.json` in `%LOCALAPPDATA%\Unlocker`. Wer das Paket nach
`C:\Programme` kopiert, sollte `portable.ini` löschen (dort darf die EXE nichts schreiben).

## Hinweise

- Sprache folgt der Windows-Anzeigesprache (Deutsch/Englisch); `set UNLOCKER_LANG=en` erzwingt Englisch.
- Geschützte Systemprozesse (PPL) lassen sich auch als Administrator nicht entsperren – dafür gibt es
  *Beim Neustart löschen*.
- Unlock-Werkzeuge lösen bei manchen Virenscannern Heuristik-Treffer aus (Handles schließen, Debug-Privileg);
  die Prüfsummen in `SHA256SUMS.txt` belegen, dass das Paket unverändert ist.

---

## English

Release locked files and folders on Windows 11. No installation: extract and run.

- `unlocker.exe <path>` opens the GUI (or drag files onto the window); `unlockerc.exe <path> [--unlock|--kill|--delete] [--yes]`
  is the command-line tool (`--help`, `--format table|csv|json`). Exit codes: 0 free/success, 1 locks found, 2 error,
  3 access denied (retry as administrator).
- Context menu: `.\kontextmenu\register-w11-menu.ps1 -TrustCert` (admin, needs Windows SDK makeappx/signtool) for the
  Windows 11 menu, or `.\kontextmenu\register-classic-menu.ps1` for the classic menu (no admin). `-Remove` undoes it. Afterwards the Windows 11 menu can also be switched on and off
  in the app under **More › Windows 11 context menu**.
- `portable.ini` next to the EXE keeps `settings.json` in this folder; delete it to use `%LOCALAPPDATA%\Unlocker`.
- License: MIT (see `LICENSE`).
- Language follows Windows; `set UNLOCKER_LANG=en` forces English.
