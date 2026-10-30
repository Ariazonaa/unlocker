<#
.SYNOPSIS
    Startet unlocker.exe mit einer gesperrten Testdatei und schreibt einen Screenshot des Fensters
    (PrintWindow, DPI-/Position-unabhängig) nach build\preview.png. Für schnelle UI-Iterationen auf dem Host.

.EXAMPLE
    .\tests\host-preview.ps1
    .\tests\host-preview.ps1 -Empty          # ohne Zieldatei (Leerzustand)
#>
[CmdletBinding()]
param(
    [string]$Exe,
    [string]$Out,
    [switch]$Empty,
    [int]$Holders = 1,        # Anzahl sperrender Prozesse (viele -> Scrollbar sichtbar)
    [int]$Files = 1,          # Anzahl Zieldateien (Mehrfachziele)
    [switch]$SelectFirst,     # erste Zeile per Tastatur selektieren
    [switch]$Dialog,          # "Prozess beenden" klicken und den Bestätigungsdialog aufnehmen
    [switch]$Rename,          # Umbenennen-Dialog (F2) aufnehmen
    [switch]$Menu,            # "Weitere"-Menü aufnehmen
    [string]$Target,          # statt Testdateien dieses Ziel öffnen (z. B. großer Ordner)
    [int]$Delay = 3000,       # ms bis zum Screenshot
    [switch]$OneHolder,       # ein einziger Prozess hält alle Dateien (Gruppierung)
    [switch]$HeaderMenu       # Rechtsklick-Menü des Spaltenkopfs aufnehmen
)
$ErrorActionPreference = "Stop"
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if (-not $Exe) { $Exe = Join-Path $root "build\x64-release\bin\unlocker.exe" }
if (-not $Out) { $Out = Join-Path $root "build\preview.png" }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class PW {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rc);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder sb, int n);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static string Cls(IntPtr h) { var sb = new System.Text.StringBuilder(256); GetClassNameW(h, sb, 256); return sb.ToString(); }
    public static string Txt(IntPtr h) { var sb = new System.Text.StringBuilder(256); GetWindowTextW(h, sb, 256); return sb.ToString(); }
    public static IntPtr FindTop(uint pid, string cls) {
        IntPtr r = IntPtr.Zero;
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h) && Cls(h) == cls) { r = h; return false; } return true; }, IntPtr.Zero);
        return r;
    }
    public static IntPtr FindButton(IntPtr parent, string text) {
        IntPtr r = IntPtr.Zero;
        EnumChildWindows(parent, (h, l) => { if (Cls(h) == "Button" && Txt(h) == text) { r = h; return false; } return true; }, IntPtr.Zero);
        return r;
    }
}
"@
[PW]::SetProcessDpiAwarenessContext([IntPtr]-4) | Out-Null   # PER_MONITOR_AWARE_V2, damit GetWindowRect physische Pixel liefert

$fileList = @(); for ($f = 0; $f -lt [Math]::Max(1, $Files); $f++) { $fileList += Join-Path $env:TEMP "unlocker-preview-$f.txt" }
$file = $fileList[0]
$holderProcs = @()
if ($OneHolder -and -not $Empty) {
    $opens = ($fileList | ForEach-Object { "[System.IO.File]::Open('$_','OpenOrCreate','ReadWrite','ReadWrite')" }) -join ";"
    $holderProcs += Start-Process powershell -ArgumentList "-NoProfile", "-WindowStyle", "Hidden", "-Command", "`$h=@($opens); Start-Sleep 60" -PassThru
    Start-Sleep -Seconds 2
    $args = if ($Target) { "`"$Target`"" } else { ($fileList | ForEach-Object { "`"$_`"" }) -join " " }
    $gui = Start-Process $Exe -ArgumentList $args -PassThru
} elseif (-not $Empty) {
    for ($i = 0; $i -lt [Math]::Max($Holders, $Files); $i++) {
        $lockFile = $fileList[$i % $fileList.Count]
        $holderProcs += Start-Process powershell -ArgumentList "-NoProfile", "-WindowStyle", "Hidden", "-Command",
            "`$f=[System.IO.File]::Open('$lockFile','OpenOrCreate','ReadWrite','ReadWrite'); Start-Sleep 60" -PassThru
    }
    Start-Sleep -Seconds ([Math]::Min(1 + $Holders / 5, 8))
    $args = if ($Target) { "`"$Target`"" } else { ($fileList | ForEach-Object { "`"$_`"" }) -join " " }
    $gui = Start-Process $Exe -ArgumentList $args -PassThru
} else {
    $gui = Start-Process $Exe -PassThru
}
Start-Sleep -Milliseconds $Delay
$gui.Refresh()
if ($SelectFirst) {
    $list = [PW]::FindWindowExW($gui.MainWindowHandle, [IntPtr]::Zero, "SysListView32", $null)
    [PW]::PostMessageW($list, 0x0007, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null       # WM_SETFOCUS reicht nicht cross-process, daher Tastatur:
    [PW]::PostMessageW($list, 0x0100, [IntPtr]0x28, [IntPtr]::Zero) | Out-Null         # WM_KEYDOWN VK_DOWN
    [PW]::PostMessageW($list, 0x0101, [IntPtr]0x28, [IntPtr]::Zero) | Out-Null         # WM_KEYUP
    Start-Sleep -Milliseconds 500
}
$capture = $gui.MainWindowHandle
if ($Dialog) {
    $btn = [PW]::FindButton($gui.MainWindowHandle, "Prozess beenden")
    [PW]::PostMessageW($btn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # BM_CLICK
    $deadline = (Get-Date).AddSeconds(5)
    do { Start-Sleep -Milliseconds 200; $dlg = [PW]::FindTop([uint32]$gui.Id, "UnlockerDialog") } while ($dlg -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
    if ($dlg -ne [IntPtr]::Zero) { $capture = $dlg; Start-Sleep -Milliseconds 500 } else { "Dialog nicht gefunden" }
}
if ($Rename) {
    [PW]::PostMessageW($gui.MainWindowHandle, 0x0111, [IntPtr]2001, [IntPtr]::Zero) | Out-Null   # WM_COMMAND IDM_RENAME
    $deadline = (Get-Date).AddSeconds(5)
    do { Start-Sleep -Milliseconds 200; $dlg = [PW]::FindTop([uint32]$gui.Id, "UnlockerDialog") } while ($dlg -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
    if ($dlg -ne [IntPtr]::Zero) { $capture = $dlg; Start-Sleep -Milliseconds 500 } else { "Dialog nicht gefunden" }
}
if ($HeaderMenu) {
    $list = [PW]::FindWindowExW($gui.MainWindowHandle, [IntPtr]::Zero, "SysListView32", $null)
    $header = [PW]::FindWindowExW($list, [IntPtr]::Zero, "SysHeader32", $null)
    $hr = New-Object PW+RECT; [PW]::GetWindowRect($header, [ref]$hr) | Out-Null
    $lp = [IntPtr]((($hr.T + 8) -shl 16) -bor (($hr.L + 120) -band 0xFFFF))
    [PW]::PostMessageW($gui.MainWindowHandle, 0x007B, $header, $lp) | Out-Null   # WM_CONTEXTMENU
    $deadline = (Get-Date).AddSeconds(5)
    do { Start-Sleep -Milliseconds 200; $popup = [PW]::FindTop([uint32]$gui.Id, "#32768") } while ($popup -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
    if ($popup -ne [IntPtr]::Zero) { $capture = $popup; Start-Sleep -Milliseconds 500 } else { "Spaltenmenü nicht gefunden" }
}
if ($Menu) {
    [PW]::PostMessageW($gui.MainWindowHandle, 0x0111, [IntPtr]1007, [IntPtr]::Zero) | Out-Null   # WM_COMMAND IDC_MORE
    $deadline = (Get-Date).AddSeconds(5)
    do { Start-Sleep -Milliseconds 200; $popup = [PW]::FindTop([uint32]$gui.Id, "#32768") } while ($popup -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
    if ($popup -ne [IntPtr]::Zero) { $capture = $popup; Start-Sleep -Milliseconds 500 } else { "Menü nicht gefunden" }
}
$r = New-Object PW+RECT
[PW]::GetWindowRect($capture, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[PW]::PrintWindow($capture, $hdc, 2) | Out-Null   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
"Screenshot: $Out ($($r.R - $r.L)x$($r.B - $r.T)), RAM Private $([math]::Round($gui.PrivateMemorySize64/1MB,1)) MB"

Stop-Process -Id $gui.Id -Force
foreach ($h in $holderProcs) { Stop-Process -Id $h.Id -Force -ErrorAction SilentlyContinue }
foreach ($f in $fileList) { Remove-Item $f -ErrorAction SilentlyContinue }
