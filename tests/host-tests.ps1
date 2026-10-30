<#
.SYNOPSIS
    Host-Tests für Fälle, die in der Sandbox (immer Admin) nicht reproduzierbar sind:
    - Exit-Code 3 (Zugriff verweigert) als Nicht-Admin: Datei, die ein SYSTEM-Dienst hält, aber lesbar ist.
    - Geschützter Prozess ohne Admin → Hinweis auf Administrator.
    Bitte als normaler Benutzer (nicht erhöht) ausführen.
#>
[CmdletBinding()]
param([string]$Exe)
$ErrorActionPreference = "Continue"
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if (-not $Exe) { $Exe = Join-Path $root "build\x64-release\bin\unlockerc.exe" }

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
$elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

$pass = 0; $fail = 0
function Check($name, $cond, $detail = "") {
    if ($cond) { $script:pass++; "  PASS  $name $detail" } else { $script:fail++; "  FAIL  $name $detail" }
}

# Die Nicht-Admin-Fälle sind erhöht nicht prüfbar (dann liest der Scanner alle Prozesse, Exit 1 statt 3) – die
# Admin-Seite deckt die Sandbox ab. Erhöht (z. B. CI-Runner) laufen hier nur die rechteunabhängigen Fälle.
"=== Zugriff verweigert (Nicht-Admin) ==="
$wmi = "C:\Windows\System32\wbem\Repository\OBJECTS.DATA"
if ($elevated) { "  SKIP  läuft erhöht – Exit-Code-3-Fälle nur als normaler Benutzer prüfbar" }
elseif (Test-Path $wmi) {
    $out = & $Exe $wmi --fast 2>$null | Out-String
    $code = $LASTEXITCODE
    Check "WMI-Repository: Exit-Code 3" ($code -eq 3) "(exit $code)"
    Check "WMI-Repository: svchost als Sperre gelistet" ($out -match "svchost\.exe")
    Check "WMI-Repository: Hinweis auf Administrator" ($out -match "Administrator|administrator")
    $json = & $Exe $wmi --fast --json 2>$null | ConvertFrom-Json
    Check "JSON accessDenied=true" ($json[0].accessDenied -eq $true)
    Check "JSON hasProtected=false" ($json[0].hasProtected -eq $false)
} else { "  (WMI-Repository nicht vorhanden – übersprungen)" }

"=== Geschützter Prozess ohne Admin ==="
$out = & $Exe "C:\Windows\System32\csrss.exe" --fast 2>$null | Out-String
Check "csrss: kritisch markiert" ($out -match "\[kritisch\]|\[critical\]")
if ($elevated) { Check "csrss: Exit-Code 1 (erhöht: geschützt, nicht Zugriff verweigert)" ($LASTEXITCODE -eq 1) "(exit $LASTEXITCODE)" }
else { Check "csrss: Exit-Code 3 (ohne Admin nicht unterscheidbar)" ($LASTEXITCODE -eq 3) "(exit $LASTEXITCODE)" }

"=== Arbeitsverzeichnis-Sperre ==="
$work = Join-Path $env:TEMP "unlocker-cwd-test"
New-Item -ItemType Directory -Force -Path "$work\inner" | Out-Null
$cmdp = Start-Process cmd.exe -ArgumentList "/k cd /d `"$work\inner`"" -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 1
$out = & $Exe $work --fast 2>$null | Out-String
Check "cmd.exe mit CWD im Ziel erkannt" ($out -match "cmd\.exe\s+Arbeitsverz|cmd\.exe\s+Working dir")
$out = & $Exe $work --fast --unlock --yes 2>$null | Out-String
Check "CWD-Handle geschlossen" ($out -match "1 Handle")
$deleted = $false
try { Remove-Item "$work\inner" -Force -ErrorAction Stop; $deleted = $true } catch {}
Check "Ordner danach löschbar" $deleted
Stop-Process -Id $cmdp.Id -Force -ErrorAction SilentlyContinue
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue

""
"$pass bestanden, $fail fehlgeschlagen"
exit $(if ($fail -eq 0) { 0 } else { 1 })
