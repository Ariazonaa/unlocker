# Unlocker {{VERSION}} ({{ARCH}}) – portable

Release locked files and folders on Windows 11: shows which process holds a file, closes the handle or ends the
process, and deletes/moves/renames – even when Explorer says "The file is in use". No installation, no runtime
libraries, no services. Just extract and run.

## Contents

| File | Purpose |
|---|---|
| `unlocker.exe` | GUI. Pass a file or folder as argument or drag it onto the window. |
| `unlockerc.exe` | Command line for scripts, `unlockerc --help`. |
| `unlocker_shell.dll` | Explorer context menu ("Unlock with Unlocker") for the new Windows 11 menu. |
| `context-menu\` | Scripts to register and remove the context menu (see below). |
| `portable.ini` | Enables portable mode: settings stay in this folder. |
| `SHA256SUMS.txt` | Checksums of all files. |
| `LICENSE` | MIT license, Copyright (c) 2026 ariazonaa. |

## Quick start

- **GUI:** `unlocker.exe "C:\path\file.ext"` – or start `unlocker.exe` and drop files onto it.
  *Unlock* closes handles (ends processes only for loaded modules), *End process* ends the processes, *Delete*
  unlocks and deletes. More actions (rename, move, copy, delete on restart) under *More*.
- **Command line:**
  ```
  unlockerc "C:\path\file.ext"                   show locks (exit 1 = locks found)
  unlockerc "C:\path\file.ext" --unlock --yes    close handles
  unlockerc "C:\path\folder" --delete --yes      unlock and delete
  unlockerc "C:\path" --format csv               one line per lock (also table, json)
  ```
  Exit codes: 0 free/success · 1 locks found · 2 error · 3 access denied (retry as administrator).
- **Administrator:** without admin rights only your own processes are readable. The GUI then offers
  *As administrator*; the command line returns exit code 3.

## Context menu

Both variants can be removed again with `-Remove`.

- **Windows 11, new menu** (entry directly in the right-click menu) – PowerShell **as administrator** in this folder:
  ```powershell
  .\context-menu\register-w11-menu.ps1 -TrustCert
  ```
  Registers `unlocker_shell.dll` as a sparse package. Needs `makeappx.exe`/`signtool.exe` from the Windows SDK and
  trusts a locally created development certificate. Do not move the folder afterwards (or register again). The menu can
  then also be switched on and off in the app under **More › Windows 11 context menu**.
- **Classic menu** (under "Show more options", no admin rights, no SDK):
  ```powershell
  .\context-menu\register-classic-menu.ps1
  ```

## Portable mode

If a file `portable.ini` exists next to `unlocker.exe`, Unlocker stores window position, column widths, sorting and
collapsed groups in `settings.json` **in the same folder**. The content of the file does not matter – only that it
exists. Without `portable.ini`, `settings.json` lives in `%LOCALAPPDATA%\Unlocker`. If you copy the package to
`C:\Program Files`, delete `portable.ini` (the EXE may not write there).

## Notes

- The language follows the Windows display language (English/German); `set UNLOCKER_LANG=en` forces English.
- Protected system processes (PPL) cannot be unlocked, not even as administrator – use *Delete on restart* instead.
- Unlock tools trigger heuristic detections in some antivirus products (closing handles, debug privilege). The
  binaries are not code-signed yet, so SmartScreen may warn on first start; the checksums in `SHA256SUMS.txt` show that
  the package is unchanged.

---

## Deutsch

Gesperrte Dateien und Ordner unter Windows 11 freigeben. Keine Installation: entpacken und starten.

- `unlocker.exe <Pfad>` öffnet die Oberfläche (oder Dateien ins Fenster ziehen); `unlockerc.exe <Pfad>
  [--unlock|--kill|--delete] [--yes]` ist die Kommandozeile (`--help`, `--format table|csv|json`). Exit-Codes:
  0 frei/erfolgreich, 1 Sperren gefunden, 2 Fehler, 3 Zugriff verweigert (als Administrator wiederholen).
- Kontextmenü: `.\context-menu\register-w11-menu.ps1 -TrustCert` (Admin, braucht makeappx/signtool aus dem Windows SDK)
  für das Windows-11-Menü oder `.\context-menu\register-classic-menu.ps1` für das klassische Menü (ohne Admin);
  `-Remove` macht es rückgängig. Danach lässt sich das Windows-11-Menü in der App unter **Weitere ›
  Windows-11-Kontextmenü** ein- und ausschalten.
- `portable.ini` neben der EXE hält `settings.json` in diesem Ordner; ohne sie liegt sie in `%LOCALAPPDATA%\Unlocker`.
- Die Sprache folgt Windows; `set UNLOCKER_LANG=de` erzwingt Deutsch.
- Lizenz: MIT (siehe `LICENSE`).
