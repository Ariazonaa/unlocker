<#
.SYNOPSIS
    DPI-Wechsel-Test auf dem Host: Fenster zwischen zwei Monitoren mit unterschiedlicher Skalierung verschieben.
    Haben beide Monitore dieselbe Skalierung, wird der Nebenmonitor für die Dauer des Tests eine Stufe höher
    gestellt (DisplayConfig SET_DPI_SCALE, wie in den Einstellungen) und danach zurückgesetzt.

    In Windows Sandbox nicht möglich (RDP-Sitzung, Skalierung nicht umschaltbar) – deshalb Host-Test.
    Braucht zwei Monitore, keine Adminrechte.

.EXAMPLE
    .\tests\host-dpi.ps1
#>
param([string]$BinDir = "")

$ErrorActionPreference = "Stop"
if (-not $BinDir) { $BinDir = Join-Path (Split-Path $PSScriptRoot -Parent) "build\x64-release\bin" }
$exe = Join-Path $BinDir "unlocker.exe"
if (-not (Test-Path $exe)) { throw "Build fehlt: $exe" }

$script:pass = 0; $script:fail = 0
function Check($name, $cond, $detail = "") {
    if ($cond) { $script:pass++; Write-Host "  PASS  $name $detail" } else { $script:fail++; Write-Host "  FAIL  $name $detail" -ForegroundColor Red }
}

Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class Dpi {
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc cb, IntPtr data);
    [DllImport("shcore.dll")] public static extern int GetDpiForMonitor(IntPtr hmon, int type, out uint x, out uint y);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool GetMonitorInfoW(IntPtr hmon, ref MONITORINFOEX mi);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hWnd, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder sb, int n);
    public delegate bool EnumProc(IntPtr hWnd, IntPtr l);
    public delegate bool MonitorEnumProc(IntPtr hmon, IntPtr hdc, ref RECT rc, IntPtr data);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct MONITORINFOEX { public uint cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice; }
    public class Monitor { public IntPtr Handle; public RECT Rect; public uint Dpi; public bool Primary; public string Device; }

    public static List<Monitor> Monitors() {
        var list = new List<Monitor>();
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr hmon, IntPtr hdc, ref RECT rc, IntPtr d) => {
            uint x, y; GetDpiForMonitor(hmon, 0, out x, out y);
            var mi = new MONITORINFOEX(); mi.cbSize = (uint)Marshal.SizeOf(typeof(MONITORINFOEX)); GetMonitorInfoW(hmon, ref mi);
            list.Add(new Monitor { Handle = hmon, Rect = mi.rcMonitor, Dpi = x, Primary = (mi.dwFlags & 1) != 0, Device = mi.szDevice });
            return true;
        }, IntPtr.Zero);
        return list;
    }

    // DisplayConfig: Skalierung pro Quelle, relativ zur empfohlenen Stufe (undokumentierte Pakete -3/-4)
    [StructLayout(LayoutKind.Sequential)] public struct LUID { public uint LowPart; public int HighPart; }
    [StructLayout(LayoutKind.Sequential)] public struct DC_HEADER { public int type; public uint size; public LUID adapterId; public uint id; }
    [StructLayout(LayoutKind.Sequential)] public struct DC_DPI_GET { public DC_HEADER header; public int minRel; public int curRel; public int maxRel; }
    [StructLayout(LayoutKind.Sequential)] public struct DC_DPI_SET { public DC_HEADER header; public int rel; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] public struct DC_SOURCE_NAME { public DC_HEADER header; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string gdiName; }
    [StructLayout(LayoutKind.Sequential, Size = 72)] public struct DC_PATH { public LUID sourceAdapterId; public uint sourceId; }
    [StructLayout(LayoutKind.Sequential, Size = 64)] public struct DC_MODE { public uint infoType; }
    [DllImport("user32.dll")] public static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);
    [DllImport("user32.dll")] public static extern int QueryDisplayConfig(uint flags, ref uint paths, [Out] DC_PATH[] pathArray, ref uint modes, [Out] DC_MODE[] modeArray, IntPtr topology);
    [DllImport("user32.dll")] public static extern int DisplayConfigGetDeviceInfo(ref DC_DPI_GET packet);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int DisplayConfigGetDeviceInfo(ref DC_SOURCE_NAME packet);
    [DllImport("user32.dll")] public static extern int DisplayConfigSetDeviceInfo(ref DC_DPI_SET packet);

    static bool FindSource(string device, out LUID adapter, out uint id) {
        adapter = new LUID(); id = 0;
        uint np, nm;
        if (GetDisplayConfigBufferSizes(2, out np, out nm) != 0) return false;
        var paths = new DC_PATH[np]; var modes = new DC_MODE[nm];
        if (QueryDisplayConfig(2, ref np, paths, ref nm, modes, IntPtr.Zero) != 0) return false;
        for (int i = 0; i < np; i++) {
            var name = new DC_SOURCE_NAME();
            name.header.type = 1; name.header.size = (uint)Marshal.SizeOf(typeof(DC_SOURCE_NAME));
            name.header.adapterId = paths[i].sourceAdapterId; name.header.id = paths[i].sourceId;
            if (DisplayConfigGetDeviceInfo(ref name) == 0 && string.Equals(name.gdiName, device, StringComparison.OrdinalIgnoreCase)) {
                adapter = paths[i].sourceAdapterId; id = paths[i].sourceId; return true;
            }
        }
        return false;
    }
    public static int[] GetScale(string device) {   // {min, cur, max} oder null
        LUID adapter; uint id;
        if (!FindSource(device, out adapter, out id)) return null;
        var get = new DC_DPI_GET();
        get.header.type = -3; get.header.size = (uint)Marshal.SizeOf(typeof(DC_DPI_GET)); get.header.adapterId = adapter; get.header.id = id;
        if (DisplayConfigGetDeviceInfo(ref get) != 0) return null;
        return new[] { get.minRel, get.curRel, get.maxRel };
    }
    public static int SetScale(string device, int rel) {
        LUID adapter; uint id;
        if (!FindSource(device, out adapter, out id)) return -1;
        var set = new DC_DPI_SET();
        set.header.type = -4; set.header.size = (uint)Marshal.SizeOf(typeof(DC_DPI_SET)); set.header.adapterId = adapter; set.header.id = id; set.rel = rel;
        return DisplayConfigSetDeviceInfo(ref set);
    }

    public static IntPtr FindChild(IntPtr parent, string cls, string text) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (h, l) => {
            var c = new StringBuilder(256); GetClassNameW(h, c, 256);
            var t = new StringBuilder(256); GetWindowTextW(h, t, 256);
            if (c.ToString() == cls && (text == null || t.ToString() == text)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

[Dpi]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null   # PMv2: physische Pixel
$monitors = [Dpi]::Monitors()
Write-Host "=== Monitore ==="
foreach ($m in $monitors) { Write-Host "  $($m.Device) $($m.Rect.Left),$($m.Rect.Top)-$($m.Rect.Right),$($m.Rect.Bottom) DPI $($m.Dpi) $(if ($m.Primary) { '(primär)' })" }
if ($monitors.Count -lt 2) { Write-Host "SKIP: nur ein Monitor – DPI-Wechsel nicht testbar."; exit 0 }
$a = $monitors | Where-Object { $_.Primary } | Select-Object -First 1
$b = $monitors | Where-Object { -not $_.Primary } | Select-Object -First 1

$changed = $false
$original = $null
$holder = $null
$app = $null
try {
    Write-Host "=== Skalierung ==="
    if ($a.Dpi -eq $b.Dpi) {
        $scale = [Dpi]::GetScale($b.Device)
        if (-not $scale) { throw "Skalierung von $($b.Device) nicht lesbar" }
        $original = $scale[1]
        if ($scale[2] -le $original) { Write-Host "SKIP: $($b.Device) lässt keine höhere Stufe zu."; exit 0 }
        $rc = [Dpi]::SetScale($b.Device, $original + 1)
        Check "Nebenmonitor $($b.Device) eine Stufe höher (rel $($original + 1))" ($rc -eq 0) "(rc $rc)"
        $changed = ($rc -eq 0)
        $deadline = (Get-Date).AddSeconds(10)
        do {
            Start-Sleep -Milliseconds 300
            $b = [Dpi]::Monitors() | Where-Object { $_.Device -eq $b.Device }
        } while ($b.Dpi -eq $a.Dpi -and (Get-Date) -lt $deadline)
        Start-Sleep -Seconds 2
        Check "Monitor meldet neue DPI" ($b.Dpi -ne $a.Dpi) "($($a.Dpi) -> $($b.Dpi))"
        if ($b.Dpi -eq $a.Dpi) { exit 1 }
    } else {
        Write-Host "  Monitore haben bereits unterschiedliche DPI ($($a.Dpi) / $($b.Dpi)) – nichts umzustellen."
    }
    $ratio = $b.Dpi / $a.Dpi

    Write-Host "=== Fenster verschieben ==="
    $file = Join-Path $env:TEMP "unlocker-dpi-test.txt"
    $holder = Start-Process powershell -ArgumentList "-NoProfile", "-WindowStyle", "Hidden", "-Command", "`$f=[System.IO.File]::Open('$file','OpenOrCreate','ReadWrite','None'); Start-Sleep 300" -PassThru
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline -and -not (Test-Path $file)) { Start-Sleep -Milliseconds 100 }
    Start-Sleep -Milliseconds 500
    $app = Start-Process $exe -ArgumentList "`"$file`"" -PassThru
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) { $app.Refresh(); if ($app.MainWindowHandle -ne 0) { break }; Start-Sleep -Milliseconds 50 }
    $h = $app.MainWindowHandle
    Check "Fenster erscheint" ($h -ne 0)
    Start-Sleep -Seconds 2
    $list = [Dpi]::FindChild($h, "SysListView32", $null)
    # Auf Monitor A positionieren (Größe beibehalten)
    [Dpi]::SetWindowPos($h, [IntPtr]::Zero, $a.Rect.Left + 120, $a.Rect.Top + 120, 0, 0, 0x0001 -bor 0x0004 -bor 0x0010) | Out-Null   # NOSIZE | NOZORDER | NOACTIVATE
    Start-Sleep -Seconds 1
    $dpiA = [Dpi]::GetDpiForWindow($h)
    $wA = [int][Dpi]::SendMessageW($list, 0x101D, [IntPtr]1, [IntPtr]::Zero)   # LVM_GETCOLUMNWIDTH, PID-Spalte
    $rcA = New-Object Dpi+RECT; [Dpi]::GetWindowRect($h, [ref]$rcA) | Out-Null
    Check "Auf Monitor A: DPI $($a.Dpi)" ($dpiA -eq $a.Dpi) "(Fenster $($rcA.Right - $rcA.Left)x$($rcA.Bottom - $rcA.Top), PID-Spalte $wA px)"

    # Auf Monitor B verschieben: Windows schickt WM_DPICHANGED mit Vorschlagsrechteck
    [Dpi]::SetWindowPos($h, [IntPtr]::Zero, $b.Rect.Left + 120, $b.Rect.Top + 120, 0, 0, 0x0001 -bor 0x0004 -bor 0x0010) | Out-Null
    $deadline = (Get-Date).AddSeconds(5)
    do { Start-Sleep -Milliseconds 200; $dpiB = [Dpi]::GetDpiForWindow($h) } while ($dpiB -eq $dpiA -and (Get-Date) -lt $deadline)
    Start-Sleep -Seconds 1
    Check "Auf Monitor B: DPI $($b.Dpi)" ($dpiB -eq $b.Dpi) "($dpiB)"
    $wB = [int][Dpi]::SendMessageW($list, 0x101D, [IntPtr]1, [IntPtr]::Zero)
    $expected = [int][Math]::Round($wA * $ratio)
    Check "PID-Spalte mitskaliert" ([Math]::Abs($wB - $expected) -le 2) "($wA -> $wB, erwartet $expected)"
    $rcB = New-Object Dpi+RECT; [Dpi]::GetWindowRect($h, [ref]$rcB) | Out-Null
    $got = ($rcB.Right - $rcB.Left) / ($rcA.Right - $rcA.Left)
    Check "Fenster auf Vorschlagsgröße" ([Math]::Abs($got - $ratio) -lt 0.08) "(Breite x$([math]::Round($got, 2)), erwartet x$([math]::Round($ratio, 2)))"
    Check "Fenster liegt auf Monitor B" ($rcB.Left -ge $b.Rect.Left -and $rcB.Left -lt $b.Rect.Right)
    $app.Refresh(); Check "App lebt" (-not $app.HasExited)
    Check "Liste zeigt weiterhin 1 Zeile" (([int][Dpi]::SendMessageW($list, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero)) -eq 1)
    Check "Buttons vorhanden" (([Dpi]::FindChild($h, "Button", "Entsperren") -ne 0) -or ([Dpi]::FindChild($h, "Button", "Unlock") -ne 0))

    # Zurück auf A
    [Dpi]::SetWindowPos($h, [IntPtr]::Zero, $a.Rect.Left + 120, $a.Rect.Top + 120, 0, 0, 0x0001 -bor 0x0004 -bor 0x0010) | Out-Null
    $deadline = (Get-Date).AddSeconds(5)
    do { Start-Sleep -Milliseconds 200; $dpiA2 = [Dpi]::GetDpiForWindow($h) } while ($dpiA2 -ne $dpiA -and (Get-Date) -lt $deadline)
    Start-Sleep -Seconds 1
    Check "Zurück auf A: DPI $($a.Dpi)" ($dpiA2 -eq $a.Dpi) "($dpiA2)"
    $wA2 = [int][Dpi]::SendMessageW($list, 0x101D, [IntPtr]1, [IntPtr]::Zero)
    Check "PID-Spalte zurück" ([Math]::Abs($wA2 - $wA) -le 2) "($wA2)"
    $rcA2 = New-Object Dpi+RECT; [Dpi]::GetWindowRect($h, [ref]$rcA2) | Out-Null
    Check "Fenstergröße zurück" ([Math]::Abs(($rcA2.Right - $rcA2.Left) - ($rcA.Right - $rcA.Left)) -le 4) "($($rcA2.Right - $rcA2.Left))"

    # Entsperren funktioniert nach den Wechseln
    $btn = [Dpi]::FindChild($h, "Button", "Entsperren"); if ($btn -eq 0) { $btn = [Dpi]::FindChild($h, "Button", "Unlock") }
    [Dpi]::PostMessageW($btn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $deadline = (Get-Date).AddSeconds(5)
    do { Start-Sleep -Milliseconds 200; $rows = [int][Dpi]::SendMessageW($list, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero) } while ($rows -ne 0 -and (Get-Date) -lt $deadline)
    Check "Entsperren nach DPI-Wechseln" ($rows -eq 0)
} finally {
    if ($app) { Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue }
    if ($holder) { Stop-Process -Id $holder.Id -Force -ErrorAction SilentlyContinue }
    if ($changed) {
        $rc = [Dpi]::SetScale($b.Device, $original)
        Write-Host "  Skalierung von $($b.Device) zurückgesetzt (rel $original, rc $rc)"
    }
    Remove-Item (Join-Path $env:TEMP "unlocker-dpi-test.txt") -ErrorAction SilentlyContinue
}
Write-Host ""
Write-Host "$script:pass bestanden, $script:fail fehlgeschlagen"
exit $(if ($script:fail -gt 0) { 1 } else { 0 })
