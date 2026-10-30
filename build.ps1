<#
.SYNOPSIS
    Baut Unlocker mit den VS 2022 Build Tools (Ninja + MSVC), ohne dass vorher
    eine Developer-Shell geöffnet werden muss.

.EXAMPLE
    .\build.ps1                 # Release
    .\build.ps1 -Config Debug
    .\build.ps1 -Test           # baut und führt ctest aus
    .\build.ps1 -Clean
    .\build.ps1 -Driver         # zusätzlich unlocker_driver.sys (braucht WDK) -> build\x64-driver\bin; lädt nichts
#>
[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",
    [ValidateSet("x64", "arm64")]
    [string]$Arch = "x64",
    [switch]$Test,
    [switch]$Clean,
    [switch]$Driver
)

$ErrorActionPreference = "Stop"
$preset = $Arch + "-" + $Config.ToLower()
if ($Driver) {
    if ($Arch -ne "x64" -or $Config -ne "Release") { throw "-Driver gibt es nur für x64 Release (Preset x64-driver)." }
    $preset = "x64-driver"
}
if ($Arch -eq "arm64" -and $Config -ne "Release") { throw "ARM64 gibt es nur als Release-Preset." }
if ($Arch -eq "arm64" -and $Test) { Write-Warning "ARM64-Binaries laufen auf x64 nicht – Tests werden übersprungen."; $Test = $false }
$root = $PSScriptRoot

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsPath) {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise"
    )
    $vsPath = $candidates | Where-Object { Test-Path (Join-Path $_ "Common7\Tools\VsDevCmd.bat") } | Select-Object -First 1
}
if (-not $vsPath) { throw "Visual Studio 2022 (Build Tools) nicht gefunden." }

$devCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
$cmakeDir = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$ninjaDir = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

if ($Clean -and (Test-Path (Join-Path $root "build\$preset"))) {
    Remove-Item -Recurse -Force (Join-Path $root "build\$preset")
}

$steps = "cmake --preset $preset && cmake --build --preset $preset"
if ($Test) { $steps += " && ctest --preset $preset" }

$env:PATH = "$cmakeDir;$ninjaDir;$env:PATH"
$cmdLine = "`"$devCmd`" -arch=$Arch -host_arch=x64 -no_logo && cd /d `"$root`" && $steps"
& cmd.exe /c $cmdLine
exit $LASTEXITCODE
