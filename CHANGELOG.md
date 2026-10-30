# Changelog

## 0.1.2 - 2026-10-30

- Windows 11 context menu registration (`context-menu\register-w11-menu.ps1`) picked a random Windows SDK when several
  are installed and failed with "makeappx.exe not found" if that was an old one. It now uses the newest SDK that
  actually contains `makeappx.exe` and `signtool.exe`.

## 0.1.1 - 2026-10-30

- Portable ZIP in English: `README.md` (with a short German section) and the folder `context-menu\` (was
  `kontextmenu\`; the app still finds a context menu package registered from the old folder).
- Context menu entry follows the Windows display language: "Unlock with Unlocker" / "Mit Unlocker entsperren"
  (new Windows 11 menu and classic menu); package manifest texts in English.

## 0.1.0 - 2026-10-30

First public version.

**Scanner**
- Three stages: kernel fast path (`FileProcessIdsUsingFileInformation`), handle and module resolution (with timeouts),
  Restart Manager details; snapshot mode for large folders.
- Long paths (`\\?\`), 8.3 short names, junctions, mapped drives and loopback UNC paths are normalized to the local path.
- Working directories (incl. WOW64 processes) as locks.
- Network shares: files opened by other machines are reported with user and – if unambiguous – client machine
  (`NetFileEnum` / `NetSessionEnum`); unlocking closes the access on the server and verifies the result.
- Locks held by the system (PID 4) and files that are locked without any process reporting them are shown honestly
  instead of "no locks".
- Critical (`csrss`, `lsass`, `Registry`, …), sensitive (Explorer, `svchost`), service and protected (PPL) processes are
  flagged; protected processes are never touched.

**Unlock and file actions**
- Closes handles without ending the program; ends processes only for module locks or on request (with PID-reuse
  protection).
- Delete (POSIX delete where possible), rename, move, copy, delete on restart.
- Deleting a document also removes orphaned Office lock files (`~$name`, `.~lock.name#`) – only small, unambiguous,
  unheld files.

**GUI**
- Dark Fluent design with Mica header, per-monitor DPI, drag & drop, multiple targets as chips, grouped rows per
  process, sortable and hideable columns, tooltips with the full content of truncated rows.
- Explorer context menu for Windows 11 (sparse package) and the classic menu; German and English.

**Command line**
- `unlockerc` with text/table/CSV/JSON output, `--unlock`, `--kill`, `--delete [--reboot]`, `--rename`, `--move`,
  `--copy`, `--timeout`, exit codes for scripts.

**Tested** with Word, Excel, LibreOffice, Edge, Paint, Photos, Windows Photo Viewer, Explorer preview pane, VS Code,
Notepad++, SumatraPDF, VLC, a running game, Windows Defender and network shares from a second machine.
