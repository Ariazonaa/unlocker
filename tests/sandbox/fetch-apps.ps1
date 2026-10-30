<#
.SYNOPSIS
    Lädt portable Alltagsprogramme für den Sandbox-Abschnitt "praxis-apps" nach build\praxis-apps (einmalig, Cache).
    Nur offizielle Quellen, SHA-256 fest hinterlegt – bei Abweichung wird die Datei verworfen. Auf dem Host wird nichts
    davon ausgeführt; run-sandbox.ps1 bindet den Ordner schreibgeschützt in die Sandbox ein.

.EXAMPLE
    .\tests\sandbox\fetch-apps.ps1
    .\tests\sandbox\run-sandbox.ps1 -Only praxis-apps
#>
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$cache = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\build\praxis-apps"))
New-Item -ItemType Directory -Force -Path $cache | Out-Null

# Prüfsummen: Notepad++ und VLC laut offizieller .sha256-Datei; SumatraPDF veröffentlicht keine – Hash des Downloads,
# die EXE darin ist gültig signiert (Krzysztof Kowalczyk).
$apps = @(
    @{ Url = "https://www.sumatrapdfreader.org/dl/rel/3.6.1/SumatraPDF-3.6.1-64.zip"
       Sha256 = "98b33a518d42986856d225064b0cd2d3643ecf78cbf84ab873d26cc51877a544" }
    @{ Url = "https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/v8.9.8.1/npp.8.9.8.1.portable.x64.zip"
       Sha256 = "beddf5548e75c97930d1575ba1c7d113e6e2b33d0a200310e734ca08e225f414" }
    @{ Url = "https://download.videolan.org/pub/videolan/vlc/3.0.24/win64/vlc-3.0.24-win64.zip"
       Sha256 = "fcf30850371ad10c9373cc4f0f4501e7dee49e3e9ae9f20c72fb2661a1ca6323" }
    # Office-Dokument: LibreOffice-MSI (359 MB; Prüfsumme laut <url>.sha256, signiert von The Document Foundation) –
    # die Sandbox entpackt es per "msiexec /a" (keine Installation). LibreOffice Portable (.paf.exe) hat kein
    # verlässlich stilles Setup.
    @{ Url = "https://download.documentfoundation.org/libreoffice/stable/26.8.2/win/x86_64/LibreOffice_26.8.2_Win_x86-64.msi"
       Sha256 = "ff4b911403b67ee9e6249f820f9782c44bb32c94fdaa45b0da53a8e419e5307a" }
)

function Get-Sha256($path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $s = [System.IO.File]::OpenRead($path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($s)) -replace "-", "").ToLower() } finally { $s.Dispose(); $sha.Dispose() }
}

foreach ($app in $apps) {
    $file = Join-Path $cache (Split-Path $app.Url -Leaf)
    if ((Test-Path $file) -and (Get-Sha256 $file) -eq $app.Sha256) { Write-Host "vorhanden: $(Split-Path $file -Leaf)"; continue }
    Write-Host "lade $($app.Url)"
    Invoke-WebRequest $app.Url -OutFile "$file.part" -UseBasicParsing
    $hash = Get-Sha256 "$file.part"
    if ($hash -ne $app.Sha256) {
        Remove-Item "$file.part"
        throw "Prüfsumme falsch für $(Split-Path $file -Leaf): $hash (erwartet $($app.Sha256))"
    }
    Move-Item -Force "$file.part" $file
    Write-Host "ok: $(Split-Path $file -Leaf)"
}
