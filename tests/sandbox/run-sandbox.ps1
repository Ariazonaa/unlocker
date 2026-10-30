<#
.SYNOPSIS
    Startet Windows Sandbox, kopiert Build + Packaging hinein und führt tests\sandbox\run-tests.ps1
    dort aus (CLI, GUI per UI Automation, Dark Mode, Kontextmenüs, Sparse Package).
    Ergebnisse: build\sandbox-share\out\results.log + Screenshots.

.EXAMPLE
    .\tests\sandbox\run-sandbox.ps1
    .\tests\sandbox\run-sandbox.ps1 -KeepOpen                 # Sandbox danach offen lassen
    .\tests\sandbox\run-sandbox.ps1 -Only gui-dpi,kern-unc    # nur einzelne Abschnitte
#>
[CmdletBinding()]
param(
    [string]$BinDir,
    [int]$TimeoutMinutes = 15,
    [string[]]$Only,
    [switch]$KeepOpen
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
if (-not $BinDir) { $BinDir = Join-Path $root "build\x64-release\bin" }
$BinDir = [System.IO.Path]::GetFullPath($BinDir)
$share = Join-Path $root "build\sandbox-share"

if (-not (Get-Command wsb.exe -ErrorAction SilentlyContinue)) { throw "wsb.exe (Windows Sandbox CLI, 24H2+) nicht gefunden." }
if (-not (Test-Path (Join-Path $BinDir "unlocker.exe"))) { throw "Build fehlt: $BinDir" }
# Syntaxfehler im Testskript vorher abfangen – sonst läuft in der Sandbox nichts an und der Lauf wartet bis zum Timeout
$parseErrors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot "run-tests.ps1"), [ref]$null, [ref]$parseErrors)
if ($parseErrors) {
    throw ("run-tests.ps1 hat Syntaxfehler: " + (($parseErrors | ForEach-Object { "Zeile $($_.Extent.StartLineNumber): $($_.Message)" }) -join "; "))
}

$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64" -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "makeappx.exe") } | Sort-Object Name -Descending | Select-Object -First 1

Remove-Item -Recurse -Force $share -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$share\bin", "$share\out" | Out-Null
Copy-Item "$BinDir\*.exe", "$BinDir\*.dll" "$share\bin"
# Kernel-Treiber (build.ps1 -Driver) mitgeben, falls gebaut – geladen wird er nur in der Sandbox (Abschnitt kern-driver)
$driverSys = Join-Path $root "build\x64-driver\bin\unlocker_driver.sys"
if (Test-Path $driverSys) { Copy-Item $driverSys "$share\bin" }
Copy-Item (Join-Path $root "packaging") "$share\packaging" -Recurse
Copy-Item (Join-Path $PSScriptRoot "run-tests.ps1") $share
# Portable-ZIP (packaging\make-portable.ps1) mitgeben, falls gebaut – Abschnitt "portable" testet es
$portableZip = Get-ChildItem (Join-Path $root "build\portable\Unlocker-*-x64-portable.zip") -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($portableZip) { New-Item -ItemType Directory -Force -Path "$share\portable" | Out-Null; Copy-Item $portableZip.FullName "$share\portable" }

$folders = "<MappedFolder><HostFolder>$share</HostFolder><SandboxFolder>C:\Share</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>"
if ($sdk) { $folders += "<MappedFolder><HostFolder>$($sdk.FullName)</HostFolder><SandboxFolder>C:\SDK</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>" }
# Portable Alltagsprogramme (tests\sandbox\fetch-apps.ps1) für "praxis-apps" – schreibgeschützt, ohne sie SKIP
$apps = Join-Path $root "build\praxis-apps"
if (Get-ChildItem $apps -Filter *.zip -ErrorAction SilentlyContinue) {
    $folders += "<MappedFolder><HostFolder>$apps</HostFolder><SandboxFolder>C:\Apps</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>"
}
# Netzwerk an: der Netzpfad-Test braucht eine unerreichbare SMB-Adresse (TEST-NET), ohne Netz schlägt das sofort fehl.
$config = "<Configuration><VGpu>Enable</VGpu><Networking>Default</Networking><MemoryInMB>4096</MemoryInMB><MappedFolders>$folders</MappedFolders></Configuration>"

Write-Host "Sandbox wird gestartet..."
$startRaw = & wsb start --config $config --raw 2>&1
$id = ($startRaw | Out-String) -replace '[^0-9A-Fa-f\-]', '' | ForEach-Object { if ($_ -match '[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}') { $Matches[0] } }
if (-not $id) { throw "Sandbox-ID nicht erkannt. Ausgabe: $startRaw" }
Write-Host "Sandbox-ID: $id"

Start-Process wsb.exe -ArgumentList "connect --id $id" | Out-Null
Start-Sleep -Seconds 10

$launched = $false
for ($attempt = 0; $attempt -lt 20 -and -not $launched; $attempt++) {
    $onlyArg = if ($Only) { " -Only " + ($Only -join ",") } else { "" }
    $execOut = & wsb exec --id $id -r ExistingLogin -c "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Share\run-tests.ps1$onlyArg" 2>&1
    if ($LASTEXITCODE -eq 0) { $launched = $true } else { Write-Host "  exec noch nicht möglich ($execOut), warte..."; Start-Sleep -Seconds 5 }
}
if (-not $launched) { throw "Testskript konnte nicht gestartet werden." }
Write-Host "Tests laufen in der Sandbox (Timeout $TimeoutMinutes min)..."

$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
while (-not (Test-Path "$share\out\DONE") -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 5 }

if (Test-Path "$share\out\results.log") {
    Write-Host ""
    Get-Content "$share\out\results.log" -Encoding UTF8
} else {
    Write-Host "Keine results.log – Testskript ist nicht angelaufen."
}
if (-not (Test-Path "$share\out\DONE")) { Write-Host "TIMEOUT: Tests nicht beendet." }

if (-not $KeepOpen) {
    & wsb stop --id $id | Out-Null
    Write-Host "Sandbox beendet."
} else {
    Write-Host "Sandbox bleibt offen: wsb stop --id $id"
}
