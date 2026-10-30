<#
.SYNOPSIS
    Registriert "Mit Unlocker entsperren" im klassischen Explorer-Kontextmenü (HKCU, keine Adminrechte).
    Unter Windows 11 erscheint der Eintrag unter "Weitere Optionen anzeigen" (Shift+F10);
    für das neue Menü siehe register-w11-menu.ps1.

.EXAMPLE
    .\register-classic-menu.ps1 -ExePath "D:\Tools\Unlocker\unlocker.exe"
    .\register-classic-menu.ps1 -Remove
#>
[CmdletBinding()]
param(
    [string]$ExePath,
    [switch]$Remove
)

$ErrorActionPreference = "Stop"
$keys = @("HKCU:\Software\Classes\*\shell\Unlocker", "HKCU:\Software\Classes\Directory\shell\Unlocker")

# "*" im Pfad ist ein Registry-Schlüsselname, kein Wildcard -> überall -LiteralPath
if ($Remove) {
    # Entfernen braucht unlocker.exe nicht – klappt auch, wenn das Programm schon gelöscht ist
    foreach ($key in $keys) {
        if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
    }
    "Klassischer Kontextmenü-Eintrag entfernt."
    return
}

# Standard: unlocker.exe neben dem Skript oder eine Ebene höher (Portable-ZIP), sonst der Build-Ordner
if (-not $ExePath) {
    $ExePath = @("$PSScriptRoot\unlocker.exe", "$PSScriptRoot\..\unlocker.exe", "$PSScriptRoot\..\build\x64-release\bin\unlocker.exe") |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $ExePath) { throw "unlocker.exe nicht gefunden - Pfad mit -ExePath angeben." }
}
$ExePath = [System.IO.Path]::GetFullPath($ExePath)
if (-not (Test-Path -LiteralPath $ExePath)) { throw "unlocker.exe nicht gefunden: $ExePath" }

foreach ($key in $keys) {
    if (-not (Test-Path -LiteralPath "$key\command")) { New-Item -Path "$key\command" -Force | Out-Null }
    Set-ItemProperty -LiteralPath $key -Name "(default)" -Value "Mit Unlocker entsperren"
    Set-ItemProperty -LiteralPath $key -Name "Icon" -Value "`"$ExePath`",0"
    Set-ItemProperty -LiteralPath $key -Name "MultiSelectModel" -Value "Player"
    Set-ItemProperty -LiteralPath "$key\command" -Name "(default)" -Value "`"$ExePath`" `"%1`""
}

"Klassischer Kontextmenü-Eintrag registriert für $ExePath"
