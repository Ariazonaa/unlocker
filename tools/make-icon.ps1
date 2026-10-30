<#
.SYNOPSIS
    Erzeugt das App-Icon (offenes Schloss auf blauem Rounded-Square) in allen Größen:
      src\app\unlocker.ico             (16/20/24/32/40/48/64/256, PNG-komprimiert)
      packaging\Assets\logo*.png       (Sparse Package / MSIX)
    Reproduzierbar per GDI+, kein Bildeditor nötig.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$icoPath = Join-Path $root "src\app\unlocker.ico"
$assets = Join-Path $root "packaging\Assets"
New-Item -ItemType Directory -Force -Path $assets | Out-Null

function New-RoundedPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function Render-Icon([int]$S) {
    $bmp = New-Object System.Drawing.Bitmap $S, $S, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # Hintergrund: Rounded Square mit Verlauf
    $pad = [Math]::Max(0.5, $S * 0.02)
    $bg = New-RoundedPath $pad $pad ($S - 2 * $pad) ($S - 2 * $pad) ($S * 0.22)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF 0, $S),
        [System.Drawing.Color]::FromArgb(255, 0x3C, 0x9C, 0xFF), [System.Drawing.Color]::FromArgb(255, 0x0A, 0x5C, 0xCC))
    $g.FillPath($brush, $bg)
    $brush.Dispose(); $bg.Dispose()

    # Schloss-Körper
    $white = [System.Drawing.Color]::White
    $bodyW = $S * 0.50; $bodyH = $S * 0.34
    $bodyX = ($S - $bodyW) / 2; $bodyY = $S * 0.50
    $body = New-RoundedPath $bodyX $bodyY $bodyW $bodyH ([Math]::Max(1, $S * 0.07))
    $wb = New-Object System.Drawing.SolidBrush $white
    $g.FillPath($wb, $body)
    $body.Dispose()

    # Bügel (offen): links am Körper, rechts angehoben mit Lücke
    $stroke = [Math]::Max(1.2, $S * 0.085)
    $pen = New-Object System.Drawing.Pen $white, $stroke
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $r = $S * 0.145
    $cx = $S * 0.50; $cy = $S * 0.335
    $shackle = New-Object System.Drawing.Drawing2D.GraphicsPath
    $shackle.AddLine($cx - $r, $bodyY + $stroke * 0.3, $cx - $r, $cy)           # linker Schenkel bis in den Körper
    $shackle.AddArc($cx - $r, $cy - $r, 2 * $r, 2 * $r, 180, 180)              # Bogen
    $shackle.AddLine($cx + $r, $cy, $cx + $r, $S * 0.40)                        # rechter Schenkel endet über dem Körper
    $g.DrawPath($pen, $shackle)
    $shackle.Dispose(); $pen.Dispose()

    # Schlüsselloch (ab 32 px)
    if ($S -ge 32) {
        $kb = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0x0A, 0x5C, 0xCC))
        $kr = $S * 0.055
        $kx = $S * 0.50; $ky = $S * 0.635
        $g.FillEllipse($kb, $kx - $kr, $ky - $kr, 2 * $kr, 2 * $kr)
        $slot = New-RoundedPath ($kx - $kr * 0.45) $ky ($kr * 0.9) ($S * 0.11) ($kr * 0.4)
        $g.FillPath($kb, $slot)
        $slot.Dispose(); $kb.Dispose()
    }
    $wb.Dispose(); $g.Dispose()
    return $bmp
}

function Get-Png([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    return ,$ms.ToArray()   # Komma: Array nicht in die Pipeline entrollen
}

# ICO mit PNG-Einträgen (seit Vista für alle Größen erlaubt)
$sizes = @(16, 20, 24, 32, 40, 48, 64, 256)
$images = @()
foreach ($s in $sizes) {
    $bmp = Render-Icon $s
    $images += , @{ Size = $s; Png = (Get-Png $bmp) }
    $bmp.Dispose()
}
$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $ms
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $dim = if ($img.Size -ge 256) { 0 } else { $img.Size }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$img.Png.Length); $w.Write([uint32]$offset)
    $offset += $img.Png.Length
}
foreach ($img in $images) { $w.Write($img.Png) }
$w.Flush()
[System.IO.File]::WriteAllBytes($icoPath, $ms.ToArray())
$w.Dispose(); $ms.Dispose()
"ICO: $icoPath ($((Get-Item $icoPath).Length) Bytes, Größen $($sizes -join '/'))"

# MSIX-Assets
foreach ($entry in @(@("logo.png", 50), @("logo44.png", 44), @("logo150.png", 150), @("logo256.png", 256))) {
    $bmp = Render-Icon $entry[1]
    $bmp.Save((Join-Path $assets $entry[0]), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
"Assets: $assets"
