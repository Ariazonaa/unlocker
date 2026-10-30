# Unlocker

Release locked files and folders on Windows 11 – small, native, portable. A lean alternative to tools like IObit
Unlocker: one portable EXE, no installer required, no background service, no bundled extras.

- Finds every process that holds a file or folder (open handles, loaded modules, working directories) – also access
  through network shares, locks held by the system itself, and files that are locked although no process reports them.
- **Unlocks without ending the program** wherever possible: it closes the handle and the application keeps running
  (verified with Word, Excel, LibreOffice, Explorer). Processes are ended only when unavoidable, e.g. when the program's
  own EXE lives in the folder.
- Then delete, rename, move or copy – or delete on the next restart. Deleting a document also removes orphaned Office
  lock files (`~$name`, `.~lock.name#`).
- GUI (dark, Fluent style), Explorer context menu, and a command line tool with text/table/CSV/JSON output.
- German and English UI (follows the Windows display language).

## Build

Requirements: Visual Studio 2022 Build Tools (MSVC v143, Windows SDK 10.0.22621+), CMake ≥ 3.25, Ninja (both ship with
the Build Tools). No other dependencies.

```powershell
.\build.ps1                 # Release -> build\x64-release\bin
.\build.ps1 -Config Debug
.\build.ps1 -Arch arm64     # cross-compile for ARM64
.\build.ps1 -Test           # incl. ctest
```

Or from a "Developer PowerShell for VS 2022":

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release
```

## Binaries

| File | Purpose |
|---|---|
| `unlocker.exe` | GUI. Pass paths as arguments or via drag & drop. |
| `unlockerc.exe` | Command line, see `unlockerc --help`. |
| `unlocker_shell.dll` | Explorer context menu (`IExplorerCommand`), registered via `packaging\`. |
| `core_tests.exe` | Smoke tests of the core library. |

## Portable ZIP

```powershell
.\packaging\make-portable.ps1 [-Build] [-Arch x64|arm64]   # -> build\portable\Unlocker-<version>-<arch>-portable.zip (+ .sha256)
```

Contains the three binaries, a short README, `LICENSE`, `portable.ini` (settings next to the EXE), the context menu
scripts with manifest and logos, and `SHA256SUMS.txt`.

## Context menu

```powershell
# Windows 11 (new menu) – sparse package; trusts a self-signed development certificate once
.\packaging\register-w11-menu.ps1

# Classic menu (Windows 10 / "Show more options")
.\packaging\register-classic-menu.ps1
```

Afterwards the Windows 11 menu can be switched on and off in the app under **More › Windows 11 context menu**. The
binaries are not code-signed yet, so SmartScreen may warn on first start.

## Command line (`unlockerc.exe`)

```
unlockerc <path...> [options]

Output        --list (default) · --format text|table|csv|json · --json · --quiet · --verbose
Scan          --no-deep · --fast (without Restart Manager) · --no-modules · --timeout <ms>
Unlock        --unlock · --kill · --allow-critical
File action   --delete [--reboot] · --rename <name> · --move <folder> · --copy <folder>
              (delete/rename/move unlock first; copy does not; delete also removes orphaned
               Office lock files ".~lock.<name>#" / "~$<name>")
              --yes skips confirmations; --quiet requires --yes for actions
```

| Exit code | Meaning |
|---|---|
| 0 | No locks (with `--list`) or action succeeded |
| 1 | Locks found (only with `--list`, i.e. without an action) |
| 2 | Error: target not found or unreachable (`--timeout`), invalid option, lock cannot be released, file action failed |
| 3 | Access denied: at least one locking process is not readable without administrator rights |

Protected processes (PPL) are *not* exit code 3 – they cannot be touched even as administrator and are reported as
`"protected": true`. Locks without a process of their own appear as PID 4 (System): opened through a network share
(`"kind": "remote"`, with `"remoteUser"` and – if unambiguous – the client machine `"remoteClient"`; unlocking closes
the access on the server and verifies that the file is free afterwards) or held by the kernel/a driver
(`"kind": "system"`, only `--delete --reboot`). If no process reports the file but it still cannot be opened
exclusively, it is listed as "no process" (`"kind": "unknown"`, PID 0, exit code 1) – so "no locks" really means free.
`--json` returns per target the scan (`lockers`), the unlock report (`unlock`) and the file action (`fileOp`).

The optional `--driver` fallback needs a signed kernel driver, which is not part of this repository yet.

## Usage

- Pass files/folders via drag & drop, **Open…** (Ctrl+O, multi-select), command line or Explorer context menu –
  several targets at once are supported.
- **Unlock** (Ctrl+U) closes handles and ends a process only for module locks; **End process** and **Delete** (Del)
  ask first.
- **More**: rename (F2), move, copy, show in Explorer, rescan (F5), delete on restart (admin).
- Right-click a row: close handle, end process, show file/program in Explorer, program properties.
- `UNLOCKER_LANG=en` forces English. Settings live in `%LOCALAPPDATA%\Unlocker\settings.json` – or next to the EXE if a
  `portable.ini` exists there.

## Tests

```powershell
.\build.ps1 -Test                    # core smoke tests (ctest): scan/unlock, junctions, paths > MAX_PATH, 8.3 names, UNC, delete-on-reboot
.\tests\host-tests.ps1               # non-admin cases (exit code 3, CWD lock) – run as a normal user
.\tests\host-dpi.ps1                 # real DPI change between two monitors
.\tests\host-preview.ps1             # UI screenshots
.\tests\sandbox\fetch-apps.ps1       # once: portable test apps (SumatraPDF, Notepad++, VLC, LibreOffice) for "praxis-apps"
.\tests\sandbox\run-sandbox.ps1      # full suite in Windows Sandbox (CLI, GUI, context menus, WOW64, PPL, network paths, DPI, real apps)
```

The sandbox run needs Windows 11 24H2+ (`wsb` CLI) and writes its log and screenshots to `build\sandbox-share\out`.
Code comments and UI tests are partly in German.

## Layout

```
src/core     core library: scanner, unlock, file operations (no UI)
src/app      Win32 GUI (dark mode, Mica header, Fluent buttons, per-monitor DPI)
src/cli      console version
src/shell    Explorer context menu
src/driver   IOCTL interface of the optional kernel driver (the driver itself is not public yet)
packaging    MSIX manifest, registration scripts, portable ZIP
tests        ctest smoke tests, host tests, Windows Sandbox suite
tools        icon generator
```

## License

MIT – see [LICENSE](LICENSE). Copyright (c) 2026 ariazonaa.
