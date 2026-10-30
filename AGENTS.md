# AGENTS.md

Working notes for AI coding sessions (and humans) in this repository. Usage: `README.md`. Changes: `CHANGELOG.md`.

## Conventions

- UI strings come in German and English: `src/common/strings.*` (`UNLOCKER_LANG=en|de` overrides the Windows display
  language). Code comments are mostly German; new comments may be English.
- C++20, MSVC only, static CRT (`/MT`), `/W4 /permissive-`, `UNICODE`. Format: `.clang-format` (Google base, 120 columns).
- No exceptions across module boundaries – errors are `std::error_code` (system category or `unlocker::Error`).
- Long paths: the core uses `\\?\` throughout (`path_util::to_extended`) – the `LongPathsEnabled` policy is off by
  default.
- Loopback UNC (`\\localhost\C$\…`, `\\<name>\Share\…`) and mapped drives are translated to the local path before
  scanning (`path_util::localize()`); the kernel does not see local handles through the SMB redirector.
- Compare paths only via `path_util::normalize()` (long form) – 8.3 short names (CWD from the PEB, registry paths)
  caused real bugs.

## Build

```powershell
.\build.ps1                  # Release x64 -> build\x64-release\bin
.\build.ps1 -Config Debug
.\build.ps1 -Arch arm64      # release preset only, cross-compile
.\build.ps1 -Test            # + ctest
.\build.ps1 -Clean
```

- No developer prompt needed; `build.ps1` finds the VS 2022 Build Tools via vswhere. CMake and Ninja ship with the
  Build Tools but are **not on PATH** – `build.ps1` handles that; a manual `ctest` needs the full path.
- No external dependencies – Windows SDK only.
- Version: `project(... VERSION)` in `CMakeLists.txt`; version resources in `src/common/version.rc.in`.
- Stop running `unlocker`/`unlockerc`/`core_tests`/`drop_self_test` processes before rebuilding (LNK1104).
- The kernel driver source is not part of this repository; `src/driver/include` only holds its IOCTL interface, and
  `-DUNLOCKER_BUILD_DRIVER=ON` skips the driver with a message.

## Tests

| Command | What | Requirement |
|---|---|---|
| `.\build.ps1 -Test` | `core_tests` (ctest): scan/unlock, junctions, > MAX_PATH, 8.3, UNC, delete-on-reboot | delete-on-reboot only elevated, otherwise SKIP |
| `tests\host-tests.ps1` | non-admin cases (exit code 3, CWD lock) | **normal user** (elevated → SKIP) |
| `tests\host-dpi.ps1` | real `WM_DPICHANGED` | two monitors |
| `tests\host-preview.ps1` | UI screenshots (PrintWindow) | – |
| `tests\sandbox\run-sandbox.ps1` | full suite in Windows Sandbox | `wsb` CLI (Win11 24H2+), admin, interactive desktop |

- Only one sandbox at a time (`CO_E_APPSINGLEUSE`): find the old one with `wsb list`, `wsb stop --id …`.
- On demand only (`-Only readme-shots`): screenshots for `docs\` (English UI, neutral example folder).
- Results: `build\sandbox-share\out\results.log` + screenshots; select sections with `-Only gui-dpi,kern-unc`. The share
  is **deleted** before every run.
- `praxis-apps` uses portable apps fetched once by `tests\sandbox\fetch-apps.ps1` (official sources, pinned SHA-256);
  without them the section is skipped. Never run those apps on the host.
- Everything in the sandbox runs **elevated**, and DPI changes are blocked (RDP session) → layout test via
  `UNLOCKER_DPI=144`.
- 8.3 and UNC tests report `SKIP` when the volume creates no short names or `\\localhost\C$` is unreachable;
  `kern-8dot3` creates a VHD with short names for that.
- CI runners run **elevated** with **8.3 names enabled** – different from a typical dev box.
- Debug env vars: `UNLOCKER_TRACE=1` (scanner stage timings on stderr), `UNLOCKER_DPI=<dpi>`.

## Architecture (short)

- `src/core` – static core library without UI (scanner in three stages: kernel fast path → handles/modules → Restart
  Manager as `enrich()`; unlock; file ops; `path_util`). The testable core.
- `src/app` – `unlocker.exe`, GUI. **Dark only**, Fluent, GDI+ (Direct2D was evaluated and dropped). Mica only in the
  56 px title zone – on an extended frame GDI+ alpha would be transparent. Rows are fully owner-drawn.
- `src/cli` – `unlockerc.exe`. Exit codes: 0 = no locks/ok, 1 = locks found, 2 = error, 3 = access denied (PPL is
  **not** 3).
- `src/shell` – `unlocker_shell.dll` (`IExplorerCommand`); only passes paths to `unlocker.exe`.
- NT APIs are loaded dynamically via `GetProcAddress` (`src/core/include/unlocker/nt.h`) – no `ntdll.lib`.
- PID 4 (System) is hidden as a process (it holds section references for all mapped images). Real locks via PID 4 are
  still reported: opened through a share → `RemoteOpen` (`NetFileEnum`, unlock via `NetFileClose`, then verified);
  otherwise – only if the kernel reports nothing but PID 4 and exclusive open fails – `SystemHeld` (delete on restart
  only). If a scan finds nothing, the targets are opened exclusively: a sharing violation → `Unattributed` (PID 0,
  "no process") instead of "no locks". The scanner's own process is excluded (`test_unattributed` relies on that).

## Pitfalls

- PowerShell 5.1: `.Count` on a single `PSCustomObject` is empty → wrap results in `@(...)`. Variable names are
  **case-insensitive** (`$out` overwrites `$Out`). `"$var:"` is a scope prefix → `"${var}:"`.
- Save `.ps1` files containing non-ASCII characters **with a UTF-8 BOM** – Windows PowerShell 5.1 reads BOM-less files
  as ANSI and the script fails to parse.
- `Start-Process -Wait` waits for the whole process tree and polls once per second → use `$p.WaitForExit()` /
  `System.Diagnostics.Process`.
- GUI tests from PowerShell: UIA proxies for Win32 controls are unreliable → `FindWindowEx`/`BM_CLICK`/
  `LVM_GETITEMCOUNT`/`GetPropW`; UIA only for Explorer.
- Cross-process ListView messages (Win11 24H2): `LVM_GET/SETCOLUMNORDERARRAY` and `HDM_GETITEM/GETITEMRECT` do **not**
  work cross-process (a SET crashes the target). Working: `LVM_GETCOLUMNWIDTH`, `LVM_GETITEMCOUNT`,
  `LVM_GETEXTENDEDLISTVIEWSTYLE`, `GetPropW`, `HDM_ITEMDOWN/UP`.
- Localized sandbox: account names are localized ("Jeder" instead of "Everyone") → resolve via SID (`S-1-1-0`).
- SMB via loopback: `NetFileClose` removes the access from the server list, but the Windows SMB client transparently
  reopens it – the file stays locked; `unlock()` therefore verifies the result. With a **real second machine** as
  client, `NetFileClose` does free the file.
- In the sandbox `SeDebugPrivilege` is already **enabled** – tests that need "access denied" disable it first.
- Windows 11 24H2 writes `PendingFileRenameOperations` with a `*1\??\` prefix → match on the suffix.
- `*` in registry paths is a key name → always `-LiteralPath`.
- GitHub Actions steps run under Windows PowerShell 5.1 from a BOM-less temp file → keep workflow scripts **ASCII only**.
- Git: LF in the repo, CRLF on checkout (`.gitattributes`).
