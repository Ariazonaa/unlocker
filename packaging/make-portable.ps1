<#
.SYNOPSIS
    Baut das Portable-ZIP: Unlocker-<version>-<arch>-portable.zip mit unlocker.exe, unlockerc.exe,
    unlocker_shell.dll, README, LICENSE, portable.ini, Kontextmenü-Skripten und SHA256SUMS.txt.

    Prüft vorher, dass alle Binaries die Version aus CMakeLists.txt tragen, und entpackt das ZIP zur Kontrolle
    noch einmal (unlockerc --help muss laufen).

.EXAMPLE
    .\packaging\make-portable.ps1                    # aus build\x64-release\bin
    .\packaging\make-portable.ps1 -Build             # vorher Release bauen
    .\packaging\make-portable.ps1 -Arch arm64 -Build
#>
[CmdletBinding()]
param(
    [string]$BinDir,
    [ValidateSet("x64", "arm64")] [string]$Arch = "x64",
    [string]$OutDir,
    [switch]$Build,
    [switch]$NoVerify
)

$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

# SHA-256 ohne Get-FileHash (das Cmdlet fehlt, wenn Windows PowerShell mit dem PSModulePath von pwsh 7 startet)
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($path)
    try { return ([System.BitConverter]::ToString($sha.ComputeHash($stream)) -replace "-", "").ToLower() }
    finally { $stream.Dispose(); $sha.Dispose() }
}
if (-not $BinDir) { $BinDir = Join-Path $root "build\$Arch-release\bin" }
if (-not $OutDir) { $OutDir = Join-Path $root "build\portable" }
$BinDir = [System.IO.Path]::GetFullPath($BinDir)

$cmake = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmake -notmatch 'project\(unlocker\s+VERSION\s+(\d+\.\d+\.\d+)') { throw "Version in CMakeLists.txt nicht gefunden." }
$version = $Matches[1]

if ($Build) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "build.ps1") -Config Release -Arch $Arch
    if ($LASTEXITCODE -ne 0) { throw "Build fehlgeschlagen ($LASTEXITCODE)" }
}

# --- Binaries prüfen: vorhanden, Versionsressource = Projektversion ---
$binaries = @("unlocker.exe", "unlockerc.exe", "unlocker_shell.dll")
foreach ($b in $binaries) {
    $path = Join-Path $BinDir $b
    if (-not (Test-Path $path)) { throw "Fehlt: $path (erst bauen: .\build.ps1 -Arch $Arch oder -Build)" }
    $fileVersion = (Get-Item $path).VersionInfo.FileVersion
    if ($fileVersion -ne $version) { throw "$b hat Version '$fileVersion', erwartet $version - Build veraltet?" }
}

$name = "Unlocker-$version-$Arch-portable"
$stageRoot = Join-Path $OutDir $name
$stage = Join-Path $stageRoot "Unlocker"
Remove-Item -Recurse -Force $stageRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $stage "context-menu\Assets") | Out-Null

foreach ($b in $binaries) { Copy-Item (Join-Path $BinDir $b) $stage }

# README mit Version/Architektur, LICENSE (Pflicht für ein Release – ohne Datei nur Warnung)
$readme = Get-Content (Join-Path $PSScriptRoot "README-portable.md") -Raw -Encoding UTF8
$readme = $readme.Replace("{{VERSION}}", $version).Replace("{{ARCH}}", $Arch)
[System.IO.File]::WriteAllText((Join-Path $stage "README.md"), $readme, (New-Object System.Text.UTF8Encoding $false))
$license = Join-Path $root "LICENSE"
if (Test-Path $license) { Copy-Item $license $stage }
else { Write-Warning "LICENSE fehlt im Repo - das ZIP enthält keine Lizenz." }

# portable.ini: die Existenz schaltet den Portable-Modus ein, der Inhalt erklärt das
@(
    "; Unlocker - Portable-Modus",
    "; Solange diese Datei neben unlocker.exe liegt, wird settings.json in diesem Ordner gespeichert",
    "; statt in %LOCALAPPDATA%\Unlocker. Datei loeschen, um die Einstellungen im Benutzerprofil abzulegen.",
    "; While this file exists next to unlocker.exe, settings.json is kept in this folder instead of",
    "; %LOCALAPPDATA%\Unlocker. Delete it to store settings in the user profile."
) | Set-Content (Join-Path $stage "portable.ini") -Encoding ASCII

# Kontextmenü: Skripte, Manifest (Architektur anpassen), Logos
Copy-Item (Join-Path $PSScriptRoot "register-classic-menu.ps1") (Join-Path $stage "context-menu")
Copy-Item (Join-Path $PSScriptRoot "register-w11-menu.ps1") (Join-Path $stage "context-menu")
$manifest = Get-Content (Join-Path $PSScriptRoot "AppxManifest.xml") -Raw -Encoding UTF8
$manifest = $manifest -replace 'ProcessorArchitecture="[^"]+"', "ProcessorArchitecture=`"$Arch`""
if ($manifest -notmatch "Version=`"$([regex]::Escape($version))\.0`"") { Write-Warning "AppxManifest.xml hat eine andere Version als $version.0" }
[System.IO.File]::WriteAllText((Join-Path $stage "context-menu\AppxManifest.xml"), $manifest, (New-Object System.Text.UTF8Encoding $false))
Copy-Item (Join-Path $PSScriptRoot "Assets\*.png") (Join-Path $stage "context-menu\Assets")

# Prüfsummen über alles (relative Pfade mit /)
$sums = Get-ChildItem $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($stage.Length + 1).Replace("\", "/")
    "$(Get-Sha256 $_.FullName)  $rel"
}
[System.IO.File]::WriteAllText((Join-Path $stage "SHA256SUMS.txt"), (($sums -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding $false))

# ZIP (Ordner "Unlocker\" als Wurzel)
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = Join-Path $OutDir "$name.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $true)
$zipHash = Get-Sha256 $zip
[System.IO.File]::WriteAllText("$zip.sha256", "$zipHash  $name.zip`n", (New-Object System.Text.UTF8Encoding $false))

# --- Kontrolle: entpacken, Prüfsummen und Startfähigkeit ---
if (-not $NoVerify) {
    $check = Join-Path $env:TEMP "unlocker-portable-check"
    Remove-Item -Recurse -Force $check -ErrorAction SilentlyContinue
    [System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $check)
    $unpacked = Join-Path $check "Unlocker"
    foreach ($line in Get-Content (Join-Path $unpacked "SHA256SUMS.txt")) {
        $hash, $rel = $line -split "  ", 2
        $actual = Get-Sha256 (Join-Path $unpacked $rel)
        if ($actual -ne $hash) { throw "Prüfsumme stimmt nicht nach dem Entpacken: $rel" }
    }
    if ($Arch -eq "x64" -or $env:PROCESSOR_ARCHITECTURE -eq "ARM64") {
        $help = & (Join-Path $unpacked "unlockerc.exe") --help 2>&1
        if ($LASTEXITCODE -ne 0 -or ($help -join "`n") -notmatch "unlockerc $([regex]::Escape($version))") {
            throw "unlockerc.exe --help aus dem ZIP liefert nicht die erwartete Ausgabe (exit $LASTEXITCODE)"
        }
    }
    Remove-Item -Recurse -Force $check -ErrorAction SilentlyContinue
}

$size = [math]::Round((Get-Item $zip).Length / 1KB)
Write-Host "Portable-ZIP: $zip ($size KB)"
Write-Host "SHA-256:      $zipHash"
Get-ChildItem $stage -Recurse -File | ForEach-Object { Write-Host ("  {0,8} KB  {1}" -f [math]::Round($_.Length / 1KB, 1), $_.FullName.Substring($stage.Length + 1)) }
