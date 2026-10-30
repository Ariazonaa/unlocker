<#
.SYNOPSIS
    Baut das Sparse Package (nur Manifest) und registriert es mit externem Inhalt, damit
    "Mit Unlocker entsperren" im neuen Windows-11-Kontextmenü erscheint.

    Voraussetzungen (Windows SDK): makeappx.exe, signtool.exe.
    Dev-Variante: selbst signiertes Zertifikat. Mit -TrustCert wird es (als Admin) in
    "Vertrauenswürdige Personen" des Computers importiert; sonst muss das einmalig manuell passieren.

.EXAMPLE
    .\register-w11-menu.ps1 -BinDir "D:\src\unlocker\build\x64-release\bin" -TrustCert
    .\register-w11-menu.ps1 -Remove
#>
[CmdletBinding()]
param(
    [string]$BinDir,
    [string]$Publisher = "CN=Unlocker Dev",
    [string]$SdkBin,
    [switch]$TrustCert,
    [switch]$Remove
)

$ErrorActionPreference = "Stop"
$packageName = "Unlocker.App"
# Standard: Binaries neben dem Skript oder eine Ebene höher (Portable-ZIP), sonst der Build-Ordner
if (-not $BinDir) {
    $BinDir = @($PSScriptRoot, "$PSScriptRoot\..", "$PSScriptRoot\..\build\x64-release\bin") |
        Where-Object { Test-Path (Join-Path $_ "unlocker_shell.dll") } | Select-Object -First 1
    if (-not $BinDir) { $BinDir = Join-Path $PSScriptRoot "..\build\x64-release\bin" }
}

if ($Remove) {
    Get-AppxPackage -Name $packageName | Remove-AppxPackage
    "Sparse Package entfernt."
    return
}

$BinDir = [System.IO.Path]::GetFullPath($BinDir)
if (-not (Test-Path (Join-Path $BinDir "unlocker_shell.dll"))) { throw "unlocker_shell.dll nicht in $BinDir" }

if (-not $SdkBin) {
    # Neuestes SDK, das beide Werkzeuge wirklich hat. Nach der Versionsnummer des Elternordners sortieren – die
    # x64-Ordner heißen alle gleich, und ältere SDK-Reste (z. B. 10.0.17134) haben oft keine makeappx.exe.
    $sdkDir = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64" -Directory -ErrorAction SilentlyContinue |
        Where-Object { (Test-Path (Join-Path $_.FullName "makeappx.exe")) -and (Test-Path (Join-Path $_.FullName "signtool.exe")) } |
        Sort-Object { [version]$_.Parent.Name } -Descending | Select-Object -First 1
    if (-not $sdkDir) { throw "Windows SDK (makeappx/signtool) nicht gefunden – Pfad mit -SdkBin angeben." }
    $SdkBin = $sdkDir.FullName
}
$makeappx = Join-Path $SdkBin "makeappx.exe"
$signtool = Join-Path $SdkBin "signtool.exe"
foreach ($tool in @($makeappx, $signtool)) { if (-not (Test-Path $tool)) { throw "Nicht gefunden: $tool" } }

$work = Join-Path $env:TEMP "unlocker-sparse"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path (Join-Path $work "Assets") | Out-Null
Copy-Item (Join-Path $PSScriptRoot "AppxManifest.xml") $work

# Identität an Zertifikat und Binaries anpassen: signtool verlangt Publisher == Zertifikats-Subject, und die
# Architektur muss zu unlocker_shell.dll passen (PE-Header: 0x8664 = x64, 0xAA64 = ARM64).
$dllBytes = [System.IO.File]::ReadAllBytes((Join-Path $BinDir "unlocker_shell.dll"))
$machine = [BitConverter]::ToUInt16($dllBytes, [BitConverter]::ToInt32($dllBytes, 0x3C) + 4)
$arch = switch ($machine) { 0x8664 { "x64" } 0xAA64 { "arm64" } default { throw ("Unbekannte Architektur von unlocker_shell.dll: 0x{0:X4}" -f $machine) } }
$manifestPath = Join-Path $work "AppxManifest.xml"
$manifest = New-Object System.Xml.XmlDocument
$manifest.PreserveWhitespace = $true
$manifest.Load($manifestPath)
$manifest.Package.Identity.Publisher = $Publisher
$manifest.Package.Identity.ProcessorArchitecture = $arch
$manifest.Save($manifestPath)

# Logos aus packaging\Assets (tools\make-icon.ps1); Fallback: einfarbige Platzhalter
$assetDir = Join-Path $PSScriptRoot "Assets"
Add-Type -AssemblyName System.Drawing
foreach ($size in @(@("logo.png", 50), @("logo150.png", 150), @("logo44.png", 44))) {
    $src = Join-Path $assetDir $size[0]
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $work "Assets\$($size[0])")
        continue
    }
    $bmp = New-Object System.Drawing.Bitmap $size[1], $size[1]
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::FromArgb(0, 120, 212))
    $g.Dispose()
    $bmp.Save((Join-Path $work "Assets\$($size[0])"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

$msix = Join-Path $env:TEMP "unlocker-sparse.msix"
Remove-Item $msix -ErrorAction SilentlyContinue
& $makeappx pack /d $work /p $msix /nv /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makeappx fehlgeschlagen ($LASTEXITCODE)" }

$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $Publisher } | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher -KeyUsage DigitalSignature `
        -FriendlyName "Unlocker Dev" -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
}
$cerFile = Join-Path $env:TEMP "unlocker-dev.cer"
Export-Certificate -Cert $cert -FilePath $cerFile -Force | Out-Null

if ($TrustCert) {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "-TrustCert benötigt eine Admin-Shell."
    }
    Import-Certificate -FilePath $cerFile -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null
} elseif (-not (Get-ChildItem Cert:\LocalMachine\TrustedPeople | Where-Object { $_.Thumbprint -eq $cert.Thumbprint })) {
    Write-Host "Dev-Zertifikat ist noch nicht vertrauenswürdig. Einmalig als Admin ausführen:"
    Write-Host "  Import-Certificate -FilePath `"$cerFile`" -CertStoreLocation Cert:\LocalMachine\TrustedPeople"
}

& $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $msix | Out-Null
if ($LASTEXITCODE -ne 0) { throw "signtool fehlgeschlagen ($LASTEXITCODE)" }

Add-AppxPackage -Path $msix -ExternalLocation $BinDir

# Signiertes Paket für den Schalter in der App ablegen ("Weitere" > Windows-11-Kontextmenü): dort, wo auch
# settings.json liegt – neben der EXE im Portable-Modus, sonst in %LOCALAPPDATA%\Unlocker.
$cacheDir = if (Test-Path (Join-Path $BinDir "portable.ini")) { $BinDir } else { Join-Path $env:LOCALAPPDATA "Unlocker" }
New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
Copy-Item $msix (Join-Path $cacheDir "unlocker-sparse.msix") -Force
"Sparse Package registriert ($BinDir). Explorer ggf. neu starten: taskkill /f /im explorer.exe && start explorer"
