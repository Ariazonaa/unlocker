<#
    Läuft INNERHALB der Windows Sandbox (Windows PowerShell 5.1, Admin).
    Erwartet C:\Share (bin\, packaging\, out\) und optional C:\SDK (makeappx/signtool).

    Aufbau: Jeder Abschnitt ist ein benannter Block in $Sections. Ohne -Section läuft der Orchestrator:
    er kopiert die Binaries, startet jeden Abschnitt als eigenen PowerShell-Prozess mit Timeout und
    zählt am Ende PASS/FAIL aus out\results.log. Ein hängender Abschnitt kostet so nur sein Timeout.
    -Only a,b,c beschränkt den Lauf auf die genannten Abschnitte.
#>
param([string]$Section = "", [string[]]$Only = @())

$ErrorActionPreference = "Continue"
$Share = "C:\Share"
$Out = "$Share\out"
$Bin = "C:\Unlocker"
$Work = "C:\Unlocker\work"

New-Item -ItemType Directory -Force -Path $Out, $Work | Out-Null

function Log($msg) {
    "$(Get-Date -Format HH:mm:ss.fff)  $msg" | Out-File -Append -Encoding utf8 "$Out\results.log"
}
function Check($name, $cond, $detail = "") {
    if ($cond) { Log "  PASS  $name $detail" } else { Log "  FAIL  $name $detail" }
}

Add-Type -AssemblyName System.Drawing, System.Windows.Forms, UIAutomationClient, UIAutomationTypes
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class Native {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint msg, UIntPtr wParam, string lParam, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int GetMenuState(IntPtr hMenu, uint id, uint flags);
    // Zustand eines Eintrags im offenen Popup-Menü (#32768): MN_GETHMENU, dann GetMenuState (Menüs sind sitzungsweit).
    // -1 = Eintrag fehlt; sonst MF_GRAYED (0x1) | MF_CHECKED (0x8) ...
    public static int PopupItemState(IntPtr popup, uint id) {
        IntPtr menu = SendMessageW(popup, 0x01E1, IntPtr.Zero, IntPtr.Zero);
        return menu == IntPtr.Zero ? -1 : GetMenuState(menu, id, 0);
    }
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hWnd, StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr GetPropW(IntPtr hWnd, string name);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT pt);
    public delegate bool EnumProc(IntPtr hWnd, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    public static List<IntPtr> TopWindowsOf(uint pid) {
        var r = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) r.Add(h); return true; }, IntPtr.Zero);
        return r;
    }
    public static IntPtr FindTopByClass(uint pid, string cls) {
        IntPtr r = IntPtr.Zero;
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && Class(h) == cls) { r = h; return false; } return true; }, IntPtr.Zero);
        return r;
    }
    public static IntPtr FindDescendant(IntPtr parent, string cls, string text) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (h, l) => { if (Class(h) == cls && Text(h) == text) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
    public static IntPtr FindDescendantByClass(IntPtr parent, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (h, l) => { if (Class(h) == cls) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
    public static string Text(IntPtr h) { var sb = new StringBuilder(1024); GetWindowTextW(h, sb, 1024); return sb.ToString(); }
    public static string Class(IntPtr h) { var sb = new StringBuilder(256); GetClassNameW(h, sb, 256); return sb.ToString(); }
    public static int ListCount(IntPtr list) { return (int)SendMessageW(list, 0x1004, IntPtr.Zero, IntPtr.Zero); }
    public static void Click(IntPtr btn) { PostMessageW(btn, 0x00F5, IntPtr.Zero, IntPtr.Zero); }

    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
    // Anzeige-Skalierung des primären Monitors über die DisplayConfig-API (undokumentierte Pakete GET_DPI_SCALE -3 /
    // SET_DPI_SCALE -4, wie in den Einstellungen; Werte relativ zur empfohlenen Stufe). Ein synthetisches WM_DPICHANGED
    // aus einem fremden Prozess lehnt user32 ab; SPI_SETLOGICALDPIOVERRIDE wirkt in RDP-/Sandbox-Sitzungen nicht.
    [StructLayout(LayoutKind.Sequential)] public struct LUID { public uint LowPart; public int HighPart; }
    [StructLayout(LayoutKind.Sequential)] public struct DC_HEADER { public int type; public uint size; public LUID adapterId; public uint id; }
    [StructLayout(LayoutKind.Sequential)] public struct DC_DPI_GET { public DC_HEADER header; public int minRel; public int curRel; public int maxRel; }
    [StructLayout(LayoutKind.Sequential)] public struct DC_DPI_SET { public DC_HEADER header; public int rel; }
    [StructLayout(LayoutKind.Sequential, Size = 72)] public struct DC_PATH { public LUID sourceAdapterId; public uint sourceId; }
    [StructLayout(LayoutKind.Sequential, Size = 64)] public struct DC_MODE { public uint infoType; }
    [DllImport("user32.dll")] public static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);
    [DllImport("user32.dll")] public static extern int QueryDisplayConfig(uint flags, ref uint paths, [Out] DC_PATH[] pathArray, ref uint modes, [Out] DC_MODE[] modeArray, IntPtr topology);
    [DllImport("user32.dll")] public static extern int DisplayConfigGetDeviceInfo(ref DC_DPI_GET packet);
    [DllImport("user32.dll")] public static extern int DisplayConfigSetDeviceInfo(ref DC_DPI_SET packet);
    static bool FirstSource(out LUID adapter, out uint id) {
        adapter = new LUID(); id = 0;
        uint np, nm;
        if (GetDisplayConfigBufferSizes(2, out np, out nm) != 0) return false;
        var paths = new DC_PATH[np]; var modes = new DC_MODE[nm];
        if (QueryDisplayConfig(2, ref np, paths, ref nm, modes, IntPtr.Zero) != 0 || np == 0) return false;
        adapter = paths[0].sourceAdapterId; id = paths[0].sourceId;
        return true;
    }
    public static int[] GetDpiRange() {   // {min, cur, max} relativ zur Empfehlung, null wenn nicht lesbar
        LUID adapter; uint id;
        if (!FirstSource(out adapter, out id)) return null;
        var get = new DC_DPI_GET();
        get.header.type = -3; get.header.size = (uint)Marshal.SizeOf(typeof(DC_DPI_GET)); get.header.adapterId = adapter; get.header.id = id;
        if (DisplayConfigGetDeviceInfo(ref get) != 0) return null;
        return new[] { get.minRel, get.curRel, get.maxRel };
    }
    public static int SetDpiRelative(int rel) {
        LUID adapter; uint id;
        if (!FirstSource(out adapter, out id)) return -1;
        var set = new DC_DPI_SET();
        set.header.type = -4; set.header.size = (uint)Marshal.SizeOf(typeof(DC_DPI_SET)); set.header.adapterId = adapter; set.header.id = id; set.rel = rel;
        return DisplayConfigSetDeviceInfo(ref set);
    }
}
"@

# Spalten-Drag im Header (Abschnitt gui-header-drag): SendInput-Maus mit Foreground-Ausleuchtung.
Add-Type @"
using System;
using System.Threading;
using System.Runtime.InteropServices;
public static class Drag {
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy, mouseData; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int cbSize);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    static void Btn(uint flags) { INPUT i = new INPUT(); i.type = 0; i.mi.dwFlags = flags; SendInput(1, new[] { i }, Marshal.SizeOf(typeof(INPUT))); }
    public static void EnsureForeground(IntPtr h) {
        IntPtr fg = GetForegroundWindow();
        uint fp = 0, mp = 0;
        if (fg != IntPtr.Zero) GetWindowThreadProcessId(fg, out fp);
        GetWindowThreadProcessId(h, out mp);
        if (fp != 0 && fp != mp) { AttachThreadInput(fp, mp, true); SetForegroundWindow(h); AttachThreadInput(fp, mp, false); }
        else SetForegroundWindow(h);
    }
    public static void DragColumn(int x0, int y, int x1) {
        SetCursorPos(x0, y); Thread.Sleep(300);
        Btn((uint)2); Thread.Sleep(300);
        int steps = 14;
        for (int s = 1; s <= steps; s++) { SetCursorPos(x0 + (x1 - x0) * s / steps, y + (s % 2 == 0 ? 2 : -2)); Thread.Sleep(60); }
        Thread.Sleep(250);
        Btn((uint)4);
    }
}
"@

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

# Oberflächentexte je Sprache (Abschnitt gui-english setzt UNLOCKER_LANG=en)
$L = @{
    de = @{ Open = "Öffnen…"; Unlock = "Entsperren"; Kill = "Prozess beenden"; Delete = "Löschen"; More = "Weitere"; Elevate = "Als Administrator"; KillShort = "Beenden"; Rename = "Umbenennen"; Targets = "Ziele"; Cancel = "Abbrechen" }
    en = @{ Open = "Open…";   Unlock = "Unlock";     Kill = "End process";     Delete = "Delete"; More = "More";    Elevate = "As administrator"; KillShort = "End";     Rename = "Rename";     Targets = "targets"; Cancel = "Cancel" }
}
$Lang = if ($env:UNLOCKER_LANG -eq "en") { $L.en } else { $L.de }

function Screenshot($name, $hwnd = [IntPtr]::Zero) {
    try {
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
        $g.Dispose()
        if ($hwnd -ne [IntPtr]::Zero) {
            $r = New-Object Native+RECT
            if ([Native]::GetWindowRect($hwnd, [ref]$r)) {
                $x0 = [Math]::Max(0, $r.Left - 8); $y0 = [Math]::Max(0, $r.Top - 8)
                $rect = New-Object System.Drawing.Rectangle $x0, $y0, ([Math]::Min($b.Width, $r.Right + 8) - $x0), ([Math]::Min($b.Height, $r.Bottom + 8) - $y0)
                $crop = $bmp.Clone($rect, $bmp.PixelFormat)
                $bmp.Dispose()
                $bmp = $crop
            }
        }
        $bmp.Save("$Out\$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        Log "  screenshot $name.png"
    } catch { Log "  screenshot $name fehlgeschlagen: $_" }
}

function Start-Holder($path) {
    $cmd = "`$f=[System.IO.File]::Open('$path','OpenOrCreate','ReadWrite','None'); Start-Sleep 900"
    $p = Start-Process powershell -ArgumentList "-NoProfile", "-WindowStyle", "Hidden", "-Command", $cmd -PassThru
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $path) -and (Test-Locked $path)) { return $p }
        Start-Sleep -Milliseconds 100
    }
    Log "  WARN Holder für $path wurde nicht rechtzeitig aktiv"
    return $p
}
function Test-Locked($path) {
    try { $s = [System.IO.File]::Open($path, 'Open', 'ReadWrite', 'None'); $s.Close(); return $false } catch { return $true }
}
function Run-Cli([string[]]$cliArgs) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "$Bin\unlockerc.exe"
    $psi.Arguments = ($cliArgs | ForEach-Object { if ($_ -match "\s") { "`"$_`"" } else { $_ } }) -join " "
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    $psi.CreateNoWindow = $true
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    # stderr parallel lesen: nacheinander blockiert das Kind, sobald der stderr-Puffer voll ist (z. B. --verbose)
    $errTask = $p.StandardError.ReadToEndAsync()
    $text = $p.StandardOutput.ReadToEnd()
    $err = $errTask.GetAwaiter().GetResult()
    $p.WaitForExit()
    return @{ Code = $p.ExitCode; Out = ([string]$text); Err = ([string]$err); Ms = $sw.ElapsedMilliseconds }
}

# --- UIA nur für Explorer (eigene Provider), nicht für unsere Win32-Controls ---
function Uia-FindByName($root, $name, $timeoutSec = 5) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $cond = New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $name)
        $e = $root.FindFirst($TS::Descendants, $cond)
        if ($e) { return $e }
        Start-Sleep -Milliseconds 200
    }
    return $null
}
function Uia-Invoke($el) { $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }

# --- Win32-basierte GUI-Steuerung ---
function Wait-MainWindow($proc, $timeoutSec = 15) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $proc.Refresh()
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { return $proc.MainWindowHandle }
        Start-Sleep -Milliseconds 50
    }
    return [IntPtr]::Zero
}
function Find-Button($main, $text) { return [Native]::FindDescendant($main, "Button", $text) }
function Get-ListHwnd($main) { return [Native]::FindDescendantByClass($main, "SysListView32") }
function Get-RowCount($main) { $l = Get-ListHwnd $main; if ($l -eq [IntPtr]::Zero) { return -1 }; return [Native]::ListCount($l) }
function Click-Button($hwnd) { [Native]::Click($hwnd) }
function Find-Dialog($procId, $timeoutSec = 5) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        foreach ($h in [Native]::TopWindowsOf([uint32]$procId)) {
            $c = [Native]::Class($h)
            if ($c -eq "UnlockerDialog" -or $c -eq "#32770") { return $h }
        }
        Start-Sleep -Milliseconds 200
    }
    return [IntPtr]::Zero
}
function Start-Gui($target) {
    if ($target) { $p = Start-Process "$Bin\unlocker.exe" -ArgumentList "`"$target`"" -PassThru }
    else { $p = Start-Process "$Bin\unlocker.exe" -PassThru }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $h = Wait-MainWindow $p 15
    $ms = $sw.ElapsedMilliseconds
    Start-Sleep -Seconds 3
    if ($h -ne [IntPtr]::Zero) { [Native]::SetForegroundWindow($h) | Out-Null }
    return @{ Proc = $p; Hwnd = $h; StartMs = $ms }
}
function Wait-Rows($main, $expected, $timeoutSec = 10) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    do {
        $n = Get-RowCount $main
        if ($n -eq $expected) { return $n }
        Start-Sleep -Milliseconds 300
    } while ((Get-Date) -lt $deadline)
    return $n
}
function Set-SystemTheme($dark) {
    $key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"
    if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
    $v = if ($dark) { 0 } else { 1 }
    Set-ItemProperty -Path $key -Name AppsUseLightTheme -Value $v -Type DWord
    Set-ItemProperty -Path $key -Name SystemUsesLightTheme -Value $v -Type DWord
    [UIntPtr]$r = [UIntPtr]::Zero
    [Native]::SendMessageTimeout([IntPtr]0xFFFF, 0x1A, [UIntPtr]::Zero, "ImmersiveColorSet", 2, 5000, [ref]$r) | Out-Null
}
function Stop-Holders {
    Get-Process unlocker, holder, c32 -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe' OR Name = 'cmd.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -match "Start-Sleep 900|protect-holder|/k cd /d" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

# ============================================================================================
# Abschnitte
# ============================================================================================
$Sections = [ordered]@{}
$Timeouts = @{}   # Sekunden; Standard 180

$Sections["core-tests"] = {
    $so = "$env:TEMP\core-tests.txt"
    $t = Start-Process "$Bin\core_tests.exe" -NoNewWindow -Wait -PassThru -RedirectStandardOutput $so
    $tOut = Get-Content $so -Raw
    Log "  Ausgabe: $($tOut.Trim() -replace "`r?`n", ' | ')"
    Check "core_tests exit 0" ($t.ExitCode -eq 0) "(exit $($t.ExitCode))"
    Check "core_tests: Junction/Long-Path/Reboot/Rechte ohne SKIP" ($tOut -notmatch "SKIP (Junction|lange Pfade|remove_on_reboot|nicht abfragbarer Prozess)")
    if ($tOut -match "SKIP UNC") { Log "  SKIP  core_tests UNC (Admin-Freigabe in der Sandbox nicht erreichbar)" } else { Check "core_tests: UNC-Loopback getestet" $true }
}

$Sections["cli-basics"] = {
    $r = Run-Cli @("--help");  Check "--help exit 0" ($r.Code -eq 0) "(exit $($r.Code))"
    Check "--help enthält Verwendung" ($r.Out -match "Verwendung")
    $r = Run-Cli @("C:\gibt\es\nicht.txt"); Check "nicht vorhandene Datei exit 2" ($r.Code -eq 2) "(exit $($r.Code))"
    $r = Run-Cli @("--bogus"); Check "unbekannte Option exit 2" ($r.Code -eq 2) "(exit $($r.Code))"

    $f1 = "$Work\cli-lock.txt"
    $h1 = Start-Holder $f1
    Check "Holder sperrt Datei" (Test-Locked $f1)
    $r = Run-Cli @($f1)
    Log "  list ($($r.Ms) ms):`n$($r.Out)"
    Check "list exit 1 (Sperre gefunden)" ($r.Code -eq 1) "(exit $($r.Code))"
    Check "list nennt powershell.exe + Handle" ($r.Out -match "powershell\.exe" -and $r.Out -match "Handle")
    Check "list nennt PID $($h1.Id)" ($r.Out -match "\b$($h1.Id)\b")
    $r = Run-Cli @($f1, "--json")
    $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
    Check "json parsebar" ($null -ne $json)
    if ($json) {
        Check "json pid stimmt" ($json[0].lockers[0].pid -eq $h1.Id)
        Check "json kind=handle" ($json[0].lockers[0].kind -eq "handle")
        Check "json app=Windows PowerShell" ($json[0].lockers[0].app -eq "Windows PowerShell") "($($json[0].lockers[0].app))"
    }
    $r = Run-Cli @($f1, "--no-deep")
    Check "--no-deep zeigt Prozess-Sperre" ($r.Out -match "Prozess")
    $r = Run-Cli @($f1, "--unlock", "--yes")
    Check "--unlock exit 0" ($r.Code -eq 0) "(exit $($r.Code))"
    Check "--unlock: 1 Handle geschlossen" ($r.Out -match "1 Handle\(s\) geschlossen")
    Check "Datei danach frei" (-not (Test-Locked $f1))
    $h1.Refresh(); Check "Holder-Prozess lebt noch (nur Handle geschlossen)" (-not $h1.HasExited)
    $r = Run-Cli @($f1)
    Check "list danach exit 0" ($r.Code -eq 0) "(exit $($r.Code))"
    Stop-Process -Id $h1.Id -Force -ErrorAction SilentlyContinue

    $f2 = "$Work\cli-kill.txt"
    $h2 = Start-Holder $f2
    $r = Run-Cli @($f2, "--kill", "--yes")
    Check "--kill exit 0" ($r.Code -eq 0) "(exit $($r.Code))"
    Start-Sleep -Milliseconds 500
    $h2.Refresh(); Check "Holder-Prozess beendet" ($h2.HasExited)
    Check "Datei frei" (-not (Test-Locked $f2))

    $f3 = "$Work\cli-delete.txt"
    $h3 = Start-Holder $f3
    $r = Run-Cli @($f3, "--delete", "--yes")
    Check "--delete exit 0" ($r.Code -eq 0) "(exit $($r.Code))"
    Check "Datei gelöscht" (-not (Test-Path $f3))
    Stop-Process -Id $h3.Id -Force -ErrorAction SilentlyContinue

    $modDir = "$Work\moddir"
    New-Item -ItemType Directory -Force -Path $modDir | Out-Null
    Copy-Item "$Bin\core_tests.exe" "$modDir\holder.exe"
    $f4 = "$modDir\inner.tmp"
    $h4 = Start-Process "$modDir\holder.exe" -ArgumentList "--hold", "`"$f4`"" -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 1
    $r = Run-Cli @($modDir)
    Log "  Ordner-Scan ($($r.Ms) ms):`n$($r.Out)"
    Check "Ordner: Handle auf inner.tmp gefunden" ($r.Out -match "Handle\s+.*inner\.tmp")
    Check "Ordner: holder.exe als geladenes Modul" ($r.Out -match "Geladenes Modul\s+.*holder\.exe")
    $r = Run-Cli @($modDir, "--unlock", "--yes")
    Start-Sleep -Milliseconds 500
    $h4.Refresh(); Check "Auto-Modus beendet Prozess mit Modul-Sperre" ($h4.HasExited)
    $r = Run-Cli @($modDir, "--delete", "--yes")
    Check "Ordner gelöscht" (-not (Test-Path $modDir)) "(exit $($r.Code))"

    # --driver ohne .sys-Pfad: das folgende Ziel darf nicht als Treiberpfad verschluckt werden
    $fd = "$Work\cli-driver.txt"
    $hd = Start-Holder $fd
    $r = Run-Cli @("--unlock", "--driver", $fd, "--yes")
    Check "--driver <ziel>: Ziel wird entsperrt (exit 0)" ($r.Code -eq 0 -and -not (Test-Locked $fd)) "(exit $($r.Code))"
    Stop-Process -Id $hd.Id -Force -ErrorAction SilentlyContinue

    $f5 = "$Work\multi-a.txt"; $f6 = "$Work\multi-b.txt"
    $h5 = Start-Holder $f5
    Set-Content $f6 "frei"
    $r = Run-Cli @($f5, $f6)
    Check "zwei Ziele: beide gelistet" ($r.Out -match "multi-a" -and $r.Out -match "multi-b")
    Check "zwei Ziele: exit 1" ($r.Code -eq 1) "(exit $($r.Code))"
    $r = Run-Cli @($f5, "--fast")
    Log "  --fast ($($r.Ms) ms inkl. Prozessstart)"
    Check "--fast findet Sperre" ($r.Out -match "Handle")

    # Ausgabeformate
    $r = Run-Cli @($f5, "--format", "table")
    Check "--format table: Spaltenköpfe + Trennlinie" ($r.Out -match "PID\s+Prozess\s+App\s+Art\s+Pfad" -and $r.Out -match "-----")
    Check "--format table: Zeile mit PID und Prozess" ($r.Out -match "$($h5.Id)\s+powershell\.exe\s+Windows PowerShell\s+Handle")
    $r = Run-Cli @($f5, "--format", "table", "--fast")
    Check "--format table --fast: App-Spalte entfällt" ($r.Out -notmatch "\bApp\b")
    $r = Run-Cli @($f5, $f6, "--format", "csv")
    $rows = @($r.Out -split "`r?`n" | Where-Object { $_ })
    Check "--format csv: Kopfzeile" ($rows[0] -eq "target,pid,process,app,kind,handle,path,service,critical,sensitive,protected,protectClose,wow64,remoteUser,remoteClient")
    Check "--format csv: genau eine Datenzeile (freies Ziel ohne Zeile)" ($rows.Count -eq 2) "($($rows.Count) Zeilen)"
    Check "--format csv: Zeilenende CRLF ohne Leerzeilen" ($r.Out -match "remoteClient`r`n" -and $r.Out -notmatch "`r`r")
    $csv = $null; try { $csv = $r.Out | ConvertFrom-Csv } catch {}
    Check "--format csv: parsebar, pid/kind/path stimmen" ($null -ne $csv -and $csv.pid -eq $h5.Id -and $csv.kind -eq "handle" -and $csv.path -ieq $f5)
    Check "--format csv: exit 1" ($r.Code -eq 1) "(exit $($r.Code))"
    $r = Run-Cli @($f5, "--format", "csv", "--fast", "--unlock", "--yes")
    Check "--format csv --unlock: stdout bleibt CSV, Bericht auf stderr" ($r.Out -notmatch "Handle\(s\)" -and $r.Err -match "1 Handle\(s\) geschlossen")
    Check "--format csv --unlock: Datei frei" (-not (Test-Locked $f5))
    $r = Run-Cli @($f5, "--format", "bogus")
    Check "--format bogus: exit 2" ($r.Code -eq 2) "(exit $($r.Code))"
    $r = Run-Cli @($f5, "--timeout", "abc")
    Check "--timeout abc: exit 2" ($r.Code -eq 2) "(exit $($r.Code))"
    $r = Run-Cli @($f5, "--timeout")
    Check "--timeout ohne Wert: exit 2" ($r.Code -eq 2) "(exit $($r.Code))"
    Stop-Process -Id $h5.Id -Force -ErrorAction SilentlyContinue
}

$Sections["cli-actions"] = {
    $actDir = "$Work\actions"
    New-Item -ItemType Directory -Force -Path "$actDir\dest" | Out-Null
    $af = "$actDir\locked.txt"
    $ah = Start-Holder $af
    $r = Run-Cli @($af, "--fast", "--rename", "renamed.txt", "--yes")
    Check "--rename: exit 0" ($r.Code -eq 0) "(exit $($r.Code))"
    Check "--rename: Handle vorher geschlossen" ($r.Out -match "1 Handle\(s\) geschlossen")
    Check "--rename: Datei umbenannt" ((Test-Path "$actDir\renamed.txt") -and -not (Test-Path $af))
    $ah.Refresh(); Check "--rename: Holder lebt (nur Handle geschlossen)" (-not $ah.HasExited)
    Stop-Process -Id $ah.Id -Force -ErrorAction SilentlyContinue
    $ah2 = Start-Holder "$actDir\renamed.txt"
    $r = Run-Cli @("$actDir\renamed.txt", "--fast", "--move", "$actDir\dest", "--yes", "--json")
    $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
    Check "--move --json: parsebar" ($null -ne $json)
    if ($json) {
        Check "--move --json: unlock-Report (1 Handle)" ($json[0].unlock.handlesClosed -eq 1)
        Check "--move --json: fileOp.type=move, ok" ($json[0].fileOp.type -eq "move" -and $json[0].fileOp.ok -eq $true)
        Check "--move --json: neuer Pfad" ($json[0].fileOp.path -ieq "$actDir\dest\renamed.txt") "($($json[0].fileOp.path))"
    }
    Check "--move: Datei im Zielordner" ((Test-Path "$actDir\dest\renamed.txt") -and -not (Test-Path "$actDir\renamed.txt"))
    Stop-Process -Id $ah2.Id -Force -ErrorAction SilentlyContinue
    $r = Run-Cli @("$actDir\dest\renamed.txt", "--fast", "--copy", $actDir, "--yes")
    Check "--copy: Kopie da, Original bleibt" ((Test-Path "$actDir\renamed.txt") -and (Test-Path "$actDir\dest\renamed.txt")) "(exit $($r.Code))"
    $r = Run-Cli @("$actDir\renamed.txt", "--fast", "--delete", "--quiet")
    Check "--quiet ohne --yes bei Aktion: exit 2" ($r.Code -eq 2) "(exit $($r.Code))"
    $r = Run-Cli @("$actDir\renamed.txt", "--fast", "--delete", "--quiet", "--yes")
    Check "--quiet --yes --delete: exit 0, keine Ausgabe, Datei weg" ($r.Code -eq 0 -and $r.Out.Trim().Length -eq 0 -and -not (Test-Path "$actDir\renamed.txt"))
    $r = Run-Cli @("$actDir\dest\renamed.txt", "$actDir\dest", "--rename", "x")
    Check "--rename mit zwei Zielen: exit 2" ($r.Code -eq 2) "(exit $($r.Code))"
    New-Item -ItemType Directory -Force -Path "$actDir\many" | Out-Null
    1..60 | ForEach-Object { Set-Content "$actDir\many\f$_.txt" "x" }
    $r = Run-Cli @("$actDir\many", "--fast", "--verbose")
    Check "--verbose: Stufen-Zeiten auf stderr" ($r.Err -match "\[trace\]")
    Check "--verbose: Handle-Fortschritt n/n" ($r.Err -match "\[handles\] \d+/\d+")
    Check "--verbose: stdout bleibt sauber" ($r.Out -notmatch "\[trace\]")
    Remove-Item $actDir -Recurse -Force -ErrorAction SilentlyContinue
}

$Sections["cli-snapshot"] = {
    $bigDir = "$Work\bigdir"
    New-Item -ItemType Directory -Force -Path "$bigDir\sub" | Out-Null
    1..120 | ForEach-Object { Set-Content "$bigDir\sub\file-$_.txt" "x" }
    $f7 = "$bigDir\sub\file-77.txt"
    $h7 = Start-Holder $f7
    $r = Run-Cli @($bigDir, "--json")
    Log "  Ordner-Scan 122 Einträge ($($r.Ms) ms)"
    $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
    Check "Snapshot: JSON parsebar" ($null -ne $json)
    if ($json) {
        Check "Snapshot-Modus aktiv" ($json[0].snapshotMode -eq $true)
        $hit = $json[0].lockers | Where-Object { $_.pid -eq $h7.Id -and $_.kind -eq "handle" }
        Check "Snapshot findet gesperrte Datei im Unterordner" ($null -ne $hit)
        if ($hit) { Check "Snapshot: Pfad stimmt" ($hit.path -ieq $f7) "($($hit.path))" }
    }
    Stop-Process -Id $h7.Id -Force -ErrorAction SilentlyContinue
    Remove-Item $bigDir -Recurse -Force -ErrorAction SilentlyContinue

    # > 10.000 Einträge: Enumeration entfällt im Snapshot-Modus, Scanzeit darf nicht mit der Dateizahl wachsen
    $huge = "$Work\huge"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    for ($d = 0; $d -lt 12; $d++) {
        $sub = "$huge\d$d"
        [System.IO.Directory]::CreateDirectory($sub) | Out-Null
        for ($i = 0; $i -lt 1000; $i++) { [System.IO.File]::WriteAllText("$sub\f$i.txt", "x") }
    }
    Log "  12.000 Dateien angelegt in $($sw.ElapsedMilliseconds) ms"
    $deep = "$huge\d7\f777.txt"
    $hh = Start-Holder $deep
    $r = Run-Cli @($huge, "--fast", "--json")
    Log "  Scan 12.000 Einträge: $($r.Ms) ms"
    $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
    Check "12k: JSON parsebar" ($null -ne $json)
    if ($json) {
        Check "12k: Snapshot-Modus" ($json[0].snapshotMode -eq $true)
        Check "12k: kein Budget-Timeout" ($json[0].snapshotTimeout -eq $false)
        $hit = $json[0].lockers | Where-Object { $_.pid -eq $hh.Id -and $_.kind -eq "handle" }
        Check "12k: Sperre in Tiefe gefunden" ($null -ne $hit)
    }
    Check "12k: Scan unter 5 s (keine Enumeration)" ($r.Ms -lt 5000) "($($r.Ms) ms)"
    $r2 = Run-Cli @($huge, "--json")   # mit Restart Manager (nur 1.000 Dateien registriert)
    Log "  Scan 12.000 Einträge mit RM: $($r2.Ms) ms"
    Check "12k mit RM: unter 15 s" ($r2.Ms -lt 15000) "($($r2.Ms) ms)"
    $r3 = Run-Cli @($huge, "--fast", "--timeout", "1", "--json")   # Budget 1 ms: Snapshot-Modus muss das Limit melden
    $json3 = $null; try { $json3 = $r3.Out | ConvertFrom-Json } catch {}
    Check "12k --timeout 1: snapshotTimeout gemeldet" ($null -ne $json3 -and $json3[0].snapshotTimeout -eq $true)
    $r4 = Run-Cli @($huge, "--fast", "--timeout", "10000", "--json")
    $json4 = $null; try { $json4 = $r4.Out | ConvertFrom-Json } catch {}
    Check "12k --timeout 10000: vollständig, Sperre gefunden" ($null -ne $json4 -and $json4[0].snapshotTimeout -eq $false -and @($json4[0].lockers | Where-Object { $_.pid -eq $hh.Id }).Count -eq 1)
    Stop-Process -Id $hh.Id -Force -ErrorAction SilentlyContinue
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $r = Run-Cli @($huge, "--fast", "--delete", "--yes")
    Check "12k: Ordner gelöscht" (-not (Test-Path $huge)) "(exit $($r.Code), $($sw.ElapsedMilliseconds) ms)"
}
$Timeouts["cli-snapshot"] = 300

$Sections["kern-unc"] = {
    # Loopback-UNC und gemapptes Laufwerk zeigen auf lokale Dateien -> Scan muss lokal laufen
    $nf = "$Work\net.txt"
    $nh = Start-Holder $nf
    $unc = "\\localhost\C$\Unlocker\work\net.txt"
    $shareOk = Test-Path $unc
    if (-not $shareOk) {
        Log "  SKIP  Admin-Freigabe \\localhost\C$ in der Sandbox nicht erreichbar (Server-Dienst: $((Get-Service LanmanServer -ErrorAction SilentlyContinue).Status)) - Host-Test deckt UNC ab"
    } else {
    $r = Run-Cli @($unc, "--fast", "--json")
    $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
    if ($json) {
        Check "UNC-Loopback: auf lokalen Pfad übersetzt" ($json[0].target -ieq $nf) "($($json[0].target))"
        Log "  Locker: $(($json[0].lockers | ForEach-Object { "$($_.pid)/$($_.process)/$($_.kind)/$($_.path)" }) -join ' ; ')"
        Check "UNC-Loopback: Sperre gefunden" (@($json[0].lockers | Where-Object { $_.pid -eq $nh.Id -and $_.kind -eq "handle" }).Count -eq 1)
    } else { Check "UNC-Loopback: JSON" $false }
    }
    & net use Q: "\\localhost\C$" /persistent:no 2>&1 | Out-Null
    if (Test-Path "Q:\Unlocker\work\net.txt") {
        $r = Run-Cli @("Q:\Unlocker\work\net.txt", "--fast", "--json")
        $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
        if ($json) {
            Check "Gemapptes Laufwerk: auf lokalen Pfad übersetzt" ($json[0].target -ieq $nf) "($($json[0].target))"
            Log "  Locker: $(($json[0].lockers | ForEach-Object { "$($_.pid)/$($_.process)/$($_.kind)/$($_.path)" }) -join ' ; ')"
            Check "Gemapptes Laufwerk: Sperre gefunden" (@($json[0].lockers | Where-Object { $_.pid -eq $nh.Id -and $_.kind -eq "handle" }).Count -eq 1)
        } else { Check "Gemapptes Laufwerk: JSON" $false }
        $r = Run-Cli @("Q:\Unlocker\work\net.txt", "--fast", "--unlock", "--yes")
        Check "Gemapptes Laufwerk: --unlock löst die Sperre" ($r.Code -eq 0 -and -not (Test-Locked $nf))
        & net use Q: /delete /y 2>&1 | Out-Null
    } elseif ($shareOk) { Check "Laufwerk Q: gemappt" $false } else { Log "  SKIP  net use Q: (keine Freigabe)" }
    Stop-Process -Id $nh.Id -Force -ErrorAction SilentlyContinue

    # Unerreichbarer Netzpfad (TEST-NET-1): kein Hänger, UI bleibt bedienbar
    $dead = "\\192.0.2.1\share\locked.txt"
    $r = Run-Cli @($dead, "--fast", "--timeout", "2000")
    Log "  CLI toter Netzpfad --timeout 2000: $($r.Ms) ms, exit $($r.Code)"
    Check "--timeout 2000: bricht in < 5 s ab" ($r.Ms -lt 5000) "($($r.Ms) ms)"
    Check "--timeout 2000: Exit 2 + Meldung 'nicht erreichbar'" ($r.Code -eq 2 -and $r.Out -match "nicht erreichbar \(Zeitlimit 2000 ms\)")
    $r = Run-Cli @($dead, "--fast", "--timeout", "2000", "--json")
    $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
    Check "--timeout --json: missing + timeout" ($null -ne $json -and $json[0].missing -eq $true -and $json[0].timeout -eq $true)
    $r = Run-Cli @($dead, "--fast")
    Log "  CLI toter Netzpfad ohne Timeout: $($r.Ms) ms, exit $($r.Code)"
    Check "CLI: toter Netzpfad endet (kein Hänger)" ($r.Ms -lt 120000)
    Check "CLI: Exit 2 (nicht gefunden)" ($r.Code -eq 2) "(exit $($r.Code))"
    $p = Start-Process "$Bin\unlocker.exe" -ArgumentList "`"$dead`"" -PassThru
    $h = Wait-MainWindow $p 15
    Start-Sleep -Seconds 2
    if ($h -ne [IntPtr]::Zero) {
        [IntPtr]$res = [IntPtr]::Zero
        $alive = [Native]::SendMessageTimeout($h, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero, 2, 1500, [ref]$res)
        Check "GUI bleibt während des SMB-Timeouts bedienbar" ($alive -ne [IntPtr]::Zero)
        $deadline = (Get-Date).AddSeconds(120)
        while ((Get-Date) -lt $deadline -and [Native]::Text($h) -ne "Unlocker") { Start-Sleep -Seconds 1 }
        Check "GUI: toter Netzpfad als 'nicht gefunden' verworfen" ([Native]::Text($h) -eq "Unlocker") "('$([Native]::Text($h))')"
        Stop-Process -Id $p.Id -Force
    } else { Check "GUI: Fenster bei Netzpfad" $false }
}
$Timeouts["kern-unc"] = 360

# Hängender SMB-Server: Ein Prozess hält eine Datei auf \\127.0.0.1\hangtest offen; der SMB-Verkehr läuft über einen
# eigenen TCP-Proxy (Port 8445 -> 445, New-SmbMapping -TcpPort, Win11 24H2+), den der Test "einfriert" (Verbindungen
# bleiben offen, Bytes werden nicht mehr weitergereicht). Ein Snapshot-Scan eines LOKALEN Ordners fragt dann auch dieses
# Handle ab – er muss trotzdem nach dem Zeitbudget fertig sein, nicht erst nach dem SMB-Timeout (~60 s).
$Sections["kern-smb-hang"] = {
    $shareDir = "C:\hangshare"
    New-Item -ItemType Directory -Force -Path $shareDir | Out-Null
    Set-Content "$shareDir\open.txt" "x"
    Remove-SmbShare -Name hangtest -Force -ErrorAction SilentlyContinue
    # "Everyone" heißt in der deutschen Sandbox "Jeder" – Kontoname über die SID S-1-1-0
    $everyone = ([System.Security.Principal.SecurityIdentifier]"S-1-1-0").Translate([System.Security.Principal.NTAccount]).Value
    New-SmbShare -Name hangtest -Path $shareDir -FullAccess $everyone -ErrorAction Stop | Out-Null

    $freeze = "$Work\proxy-freeze"
    $accepts = "$Work\proxy-accepts"
    Remove-Item $freeze, $accepts -ErrorAction SilentlyContinue
    $proxyScript = "$Work\smb-proxy.ps1"
    @'
param([string]$Freeze, [string]$Accepts)
Add-Type @"
using System; using System.IO; using System.Net; using System.Net.Sockets; using System.Threading;
public static class Relay {
    public static volatile bool Frozen;
    static int accepted;
    public static void Run(string freezeFlag, string acceptsFile) {
        var listener = new TcpListener(IPAddress.Loopback, 8445);
        listener.Start();
        new Thread(() => { while (true) { Frozen = File.Exists(freezeFlag); Thread.Sleep(50); } }) { IsBackground = true }.Start();
        while (true) {
            var client = listener.AcceptTcpClient();
            var server = new TcpClient();
            server.Connect(IPAddress.Loopback, 445);
            File.WriteAllText(acceptsFile, (++accepted).ToString());
            Pump(client.GetStream(), server.GetStream());
            Pump(server.GetStream(), client.GetStream());
        }
    }
    // Eingefroren: nichts mehr lesen und nichts weiterreichen – die Verbindung bleibt offen, Antworten bleiben aus.
    static void Pump(Stream from, Stream to) {
        new Thread(() => {
            var buffer = new byte[65536];
            try {
                while (true) {
                    while (Frozen) Thread.Sleep(20);
                    int n = from.Read(buffer, 0, buffer.Length);
                    if (n <= 0) break;
                    while (Frozen) Thread.Sleep(20);
                    to.Write(buffer, 0, n);
                }
            } catch { }
        }) { IsBackground = true }.Start();
    }
}
"@
[Relay]::Run($Freeze, $Accepts)
'@ | Set-Content -Path $proxyScript -Encoding UTF8
    $proxy = Start-Process powershell -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden", "-File", "`"$proxyScript`"", "-Freeze", "`"$freeze`"", "-Accepts", "`"$accepts`"" -PassThru
    Start-Sleep -Seconds 3

    $mapped = $false
    try { New-SmbMapping -RemotePath "\\127.0.0.1\hangtest" -TcpPort 8445 -ErrorAction Stop | Out-Null; $mapped = $true }
    catch { Log "  New-SmbMapping -TcpPort: $_" }
    $holder = $null
    try {
        if (-not $mapped) { Log "  SKIP  SMB über alternativen Port nicht verfügbar (Win11 24H2+ nötig)"; return }
        $remote = "\\127.0.0.1\hangtest\open.txt"
        $holder = Start-Process powershell -ArgumentList "-NoProfile", "-WindowStyle", "Hidden", "-Command",
            "`$f=[System.IO.File]::Open('$remote','Open','Read','ReadWrite'); Start-Sleep 900" -PassThru
        Start-Sleep -Seconds 3
        $acceptCount = if (Test-Path $accepts) { [int](Get-Content $accepts) } else { 0 }
        Check "SMB-Verkehr läuft über den Proxy" ($acceptCount -ge 1) "($acceptCount Verbindung(en))"

        # Lokaler Ordner mit > 32 Einträgen -> Snapshot-Modus: alle Datei-Handles im System werden aufgelöst
        $snap = "$Work\smb-snap"
        New-Item -ItemType Directory -Force -Path $snap | Out-Null
        1..40 | ForEach-Object { Set-Content "$snap\f$_.txt" "x" }
        $env:UNLOCKER_TRACE = "1"
        $r = Run-Cli @($snap, "--fast", "--json")
        Log "  vorher: $($r.Ms) ms; $(($r.Err -split "`n" | Where-Object { $_ -match 'handles:' }) -join ' ')"
        Check "Scan vor dem Einfrieren normal" ($r.Code -in 0, 1 -and $r.Ms -lt 5000) "($($r.Ms) ms)"

        Set-Content $freeze "1"
        Start-Sleep -Milliseconds 500
        foreach ($round in 1..3) {
            $r = Run-Cli @($snap, "--fast", "--json")
            $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
            Log "  eingefroren #${round}: $($r.Ms) ms, exit $($r.Code); $(($r.Err -split "`n" | Where-Object { $_ -match 'handles:|Stufe 3' }) -join ' | ')"
            # < 1200 ms: der Stillstands-Abbruch (300 ms ohne Fortschritt) greift – nicht erst das Gesamtbudget (1500 ms)
            # und schon gar nicht der SMB-Timeout (~60 s)
            Check "Server hängt, Scan #${round}: Abbruch nach Stillstand (< 1200 ms statt SMB-Timeout)" ($r.Ms -lt 1200) "($($r.Ms) ms)"
            if ($round -eq 1) { Check "Server hängt: Ergebnis als unvollständig markiert (snapshotTimeout)" ($json -and $json[0].snapshotTimeout -eq $true) }
        }
        Remove-Item Env:\UNLOCKER_TRACE -ErrorAction SilentlyContinue

        # GUI: Jeder Scan lässt eine Abfrage auf dem Threadpool hängen, bis SMB aufgibt (~60 s). Mehrfaches F5 darf das
        # Fenster nicht blockieren, und die Threads dürfen sich nicht unbegrenzt stauen.
        $gui = Start-Gui $snap
        if ($gui.Hwnd -ne [IntPtr]::Zero) {
            $gui.Proc.Refresh(); $threadsBefore = $gui.Proc.Threads.Count
            foreach ($i in 1..5) {
                [Native]::PostMessageW($gui.Hwnd, 0x0111, [IntPtr]2006, [IntPtr]::Zero) | Out-Null   # IDM_RESCAN
                Start-Sleep -Milliseconds 800
            }
            Start-Sleep -Seconds 2
            $res = [IntPtr]::Zero
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $alive = [Native]::SendMessageTimeout($gui.Hwnd, 0, [IntPtr]::Zero, [IntPtr]::Zero, 2, 2000, [ref]$res) -ne [IntPtr]::Zero
            Check "Server hängt, GUI nach 5x F5 reaktionsfähig" $alive "($($sw.ElapsedMilliseconds) ms)"
            $gui.Proc.Refresh(); $threadsAfter = $gui.Proc.Threads.Count
            Check "Server hängt, GUI: Threads stauen sich nicht unbegrenzt (<= 3 je Scan)" (($threadsAfter - $threadsBefore) -le 15) "($threadsBefore -> $threadsAfter)"
            Stop-Process -Id $gui.Proc.Id -Force
        }
    } finally {
        Remove-Item $freeze -ErrorAction SilentlyContinue
        if ($holder) { Stop-Process -Id $holder.Id -Force -ErrorAction SilentlyContinue }
        Remove-SmbMapping -RemotePath "\\127.0.0.1\hangtest" -Force -ErrorAction SilentlyContinue
        Stop-Process -Id $proxy.Id -Force -ErrorAction SilentlyContinue
        Remove-SmbShare -Name hangtest -Force -ErrorAction SilentlyContinue
    }
}
$Timeouts["kern-smb-hang"] = 400

# 8.3-Kurznamen: Host und Sandbox erzeugen keine (core_tests meldet sonst SKIP) – der CI-Runner schon, dort fielen zwei
# echte Fehler auf. Eigene VHD mit eingeschalteten Kurznamen als S:, darauf core_tests (TEMP=S:\Temp), CLI und GUI.
$Sections["kern-8dot3"] = {
    $vhd = "C:\vhd\short83.vhdx"
    New-Item -ItemType Directory -Force -Path "C:\vhd" | Out-Null
    Remove-Item $vhd -ErrorAction SilentlyContinue
    $dp = "$Work\diskpart-create.txt"
    @("create vdisk file=`"$vhd`" maximum=256 type=expandable", "select vdisk file=`"$vhd`"", "attach vdisk",
      "create partition primary", "format fs=ntfs quick label=Short83", "assign letter=S") | Set-Content -Path $dp -Encoding ASCII
    $dpOut = (& diskpart /s $dp 2>&1 | Out-String)
    if (-not (Test-Path "S:\")) { Log "  FAIL  VHD S: nicht verfügbar: $(($dpOut -split "`r?`n" | Select-Object -Last 4) -join ' | ')"; return }
    try {
        Log "  fsutil vorher: $((& fsutil 8dot3name query 2>&1 | Out-String).Trim() -replace "`r?`n", ' | ')"
        # Systemweit 1 = überall aus -> auf 2 (pro Volume) stellen; dann für S: einschalten
        if ((& fsutil 8dot3name query 2>&1 | Out-String) -match "\b1\b.*\(") { & fsutil 8dot3name set 2 | Out-Null }
        Log "  fsutil S:: $((& fsutil 8dot3name set S: 0 2>&1 | Out-String).Trim() -replace "`r?`n", ' | ')"

        $dir = "S:\Ein langer Ordnername"
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $file = "$dir\gesperrte Datei mit langem Namen.txt"
        Set-Content $file "x"
        $shortFile = (& cmd /c "for %I in (`"$file`") do @echo %~sI").Trim()
        $shortDir = (& cmd /c "for %I in (`"$dir`") do @echo %~sI").Trim()
        Check "S: erzeugt Kurznamen" ($shortFile -ne $file -and $shortFile -match "~") "($shortFile)"

        # Kern-Tests auf S: – die 8.3-Fälle dürfen nicht mehr übersprungen werden
        New-Item -ItemType Directory -Force -Path "S:\Temp" | Out-Null
        $oldTemp = $env:TEMP; $oldTmp = $env:TMP
        $env:TEMP = "S:\Temp"; $env:TMP = "S:\Temp"
        $so = "$Work\core-tests-8dot3.txt"
        $t = Start-Process "$Bin\core_tests.exe" -NoNewWindow -Wait -PassThru -RedirectStandardOutput $so
        $env:TEMP = $oldTemp; $env:TMP = $oldTmp
        $tOut = Get-Content $so -Raw
        Log "  core_tests auf S:: $($tOut.Trim() -replace "`r?`n", ' | ')"
        Check "core_tests auf S: exit 0" ($t.ExitCode -eq 0) "(exit $($t.ExitCode))"
        Check "core_tests: 8.3-Auflösung getestet (kein SKIP)" ($tOut -notmatch "SKIP 8\.3")
        Check "core_tests: Arbeitsverzeichnis als 8.3-Pfad getestet" ($tOut -notmatch "ohne 8\.3")

        # CLI: Ziel als Kurzname -> Sperre gefunden, Ausgabe in Langform
        $h = Start-Holder $file
        $r = Run-Cli @($shortFile, "--json")
        $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
        Check "CLI 8.3-Ziel: Sperre gefunden (exit 1)" ($r.Code -eq 1 -and $json -and @($json[0].lockers).Count -ge 1) "(exit $($r.Code))"
        Check "CLI 8.3-Ziel: Ziel in Langform" ($json -and $json[0].target -ieq $file) "($($json[0].target))"
        Check "CLI 8.3-Ziel: Sperrpfad in Langform" ($json -and @($json[0].lockers)[0].path -ieq $file) "($(@($json[0].lockers)[0].path))"

        # GUI: Ziel als Kurzname (Titel zeigt den Dateinamen des Ziels)
        $gui = Start-Gui $shortFile
        if ($gui.Hwnd -ne [IntPtr]::Zero) {
            $n = Wait-Rows $gui.Hwnd 1
            $title = [Native]::Text($gui.Hwnd)
            Check "GUI 8.3-Ziel: Sperre gelistet" ($n -eq 1) "($n Zeilen)"
            Check "GUI 8.3-Ziel: Titel mit langem Namen" ($title -like "gesperrte Datei mit langem Namen.txt*") "($title)"
            Screenshot "gui-8dot3" $gui.Hwnd
            Stop-Process -Id $gui.Proc.Id -Force
        }

        $r = Run-Cli @($shortFile, "--unlock", "--yes")
        Check "CLI 8.3-Ziel: entsperrt" ($r.Code -eq 0 -and -not (Test-Locked $file)) "(exit $($r.Code))"
        Stop-Process -Id $h.Id -Force -ErrorAction SilentlyContinue

        # Arbeitsverzeichnis per Kurzname gesetzt (so steht es im PEB) – Ordner per Lang- und Kurzname scannen
        $cmdp = Start-Process cmd.exe -ArgumentList "/k cd /d $shortDir" -WindowStyle Hidden -PassThru
        Start-Sleep -Seconds 1
        foreach ($target in @($dir, $shortDir)) {
            $r = Run-Cli @($target, "--fast", "--json")
            $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
            $cwd = @($json[0].lockers | Where-Object { $_.pid -eq $cmdp.Id -and $_.kind -eq "cwd" })
            Check "CWD als 8.3-Pfad erkannt (Scan über $target)" ($cwd.Count -eq 1) "($($cwd.Count))"
        }
        $r = Run-Cli @($shortDir, "--delete", "--yes")
        Check "CLI 8.3-Ordner: entsperrt und gelöscht" ($r.Code -eq 0 -and -not (Test-Path $dir)) "(exit $($r.Code))"
        $cmdp.Refresh(); Check "cmd.exe läuft weiter (nur CWD-Handle geschlossen)" (-not $cmdp.HasExited)
        Stop-Process -Id $cmdp.Id -Force -ErrorAction SilentlyContinue
    } finally {
        Get-Process unlocker -ErrorAction SilentlyContinue | Stop-Process -Force
        $dd = "$Work\diskpart-detach.txt"
        @("select vdisk file=`"$vhd`"", "detach vdisk") | Set-Content -Path $dd -Encoding ASCII
        & diskpart /s $dd 2>&1 | Out-Null
    }
}
$Timeouts["kern-8dot3"] = 240

# Vom Kernel gehaltene Sperre: Ein Prozess öffnet eine lokale Datei über die Freigabe (\\127.0.0.1\C$\...) exklusiv –
# die lokale Datei hält dann der SMB-Server (Kernel, PID 4), nicht der Prozess. Unlocker darf das nicht als "frei" melden.
$Sections["kern-system-lock"] = {
    Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class FilePids {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("ntdll.dll")] static extern int NtQueryInformationFile(IntPtr h, byte[] iosb, IntPtr info, int length, int cls);
    // FileProcessIdsUsingFileInformation (47): PIDs aller Prozesse, die die Datei offen haben
    public static string Query(string path) {
        IntPtr h = CreateFileW(path, 0x80, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
        if (h == new IntPtr(-1)) return "CreateFile " + Marshal.GetLastWin32Error();
        IntPtr buf = Marshal.AllocHGlobal(65536);
        try {
            int st = NtQueryInformationFile(h, new byte[16], buf, 65536, 47);
            if (st != 0) return "Status 0x" + st.ToString("X8");
            int n = Marshal.ReadInt32(buf);
            var pids = new List<string>();
            for (int i = 0; i < n; i++) pids.Add(Marshal.ReadIntPtr(buf, 8 + i * IntPtr.Size).ToString());
            return "[" + string.Join(",", pids) + "]";
        } finally { Marshal.FreeHGlobal(buf); CloseHandle(h); }
    }
}
"@
    $f = "$Work\smb-held.txt"
    $free = "$Work\smb-free.txt"
    Set-Content $f "x"; Set-Content $free "x"
    Get-Content $free | Out-Null
    $unc = "\\127.0.0.1\C$" + $f.Substring(2)
    $holder = Start-Process powershell -ArgumentList "-NoProfile", "-WindowStyle", "Hidden", "-Command",
        "`$s=[System.IO.File]::Open('$unc','Open','ReadWrite','None'); Start-Sleep 900" -PassThru
    $deadline = (Get-Date).AddSeconds(20)
    while (-not (Test-Locked $f) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
    try {
        Check "Datei lokal gesperrt (über die Freigabe geöffnet)" (Test-Locked $f)
        $open = @(Get-SmbOpenFile -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $f })
        Log "  Get-SmbOpenFile: $($open.Count) offen ($(($open | ForEach-Object { "$($_.ClientComputerName) $($_.ClientUserName) FileId $($_.FileId)" }) -join '; '))"
        Log "  Kernel-PIDs gesperrte Datei: $([FilePids]::Query($f)), ungesperrte Vergleichsdatei: $([FilePids]::Query($free)), Holder-PID $($holder.Id)"
        $r = Run-Cli @($f, "--json")
        $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
        Log "  unlockerc: exit $($r.Code), Sperren: $(@($json[0].lockers).Count) $((@($json[0].lockers) | ForEach-Object { "$($_.pid)/$($_.process)/$($_.kind)/$($_.remoteUser)@$($_.remoteClient)" }) -join ', ')"
        Check "Vom Kernel gehaltene Sperre wird gemeldet (nicht 'keine Sperren')" ($r.Code -ne 0 -and @($json[0].lockers).Count -gt 0) "(exit $($r.Code))"
        $remote = @($json[0].lockers | Where-Object { $_.kind -eq "remote" })
        Check "Freigabe-Zugriff erkannt: kind=remote, PID 4, Benutzer, Client, nicht kritisch" ($remote.Count -eq 1 -and $remote[0].pid -eq 4 -and $remote[0].remoteUser -and $remote[0].remoteClient -and -not $remote[0].critical) "($($remote.Count), Client '$($remote[0].remoteClient)')"
        $r = Run-Cli @($f, "--format", "table")
        Check "Tabelle nennt Freigabe, Benutzer und Client" ($r.Out -match "Freigabe" -and $r.Out -match "freigabe: \S+ von \S+")

        # CLI: Entsperren schließt den Zugriff auf dem Server (NetFileClose). Per Loopback überbrückt der SMB-Client das
        # (Datei bleibt gesperrt, siehe CHANGELOG) – verlangt wird Konsistenz: Erfolg nur mit freier Datei, sonst Exit 2
        # mit klarer Meldung. Der System-Prozess wird nie beendet.
        $r = Run-Cli @($f, "--unlock", "--yes")
        $stillLocked = Test-Locked $f
        Log "  --unlock: exit $($r.Code), Datei gesperrt: ${stillLocked}: $(($r.Out -split "`r?`n" | Where-Object { $_ -match 'geschlossen|Fehler' }) -join ' | ')"
        Check "--unlock: kein Prozess beendet" ($r.Out -match ", 0 Prozess")
        Check "Get-SmbOpenFile danach leer (Server-Zugriff geschlossen)" (@(Get-SmbOpenFile -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $f }).Count -eq 0)
        Check "--unlock: Ergebnis konsistent (frei + exit 0 oder gesperrt + exit 2 mit 'weiter gesperrt')" `
            ((-not $stillLocked -and $r.Code -eq 0) -or ($stillLocked -and $r.Code -eq 2 -and $r.Out -match "weiter gesperrt")) "(exit $($r.Code), gesperrt $stillLocked)"
        if ($stillLocked) {
            # Der Server listet nichts mehr, der Kernel meldet nur PID 4 und die Datei ist gesperrt -> "vom System gehalten"
            $r = Run-Cli @($f, "--json")
            $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
            $sys = @($json[0].lockers | Where-Object { $_.kind -eq "system" })
            Check "Danach als 'vom System gehalten' gemeldet (kind=system, PID 4, nicht kritisch)" ($sys.Count -eq 1 -and $sys[0].pid -eq 4 -and -not $sys[0].critical) "($(@($json[0].lockers | ForEach-Object kind) -join ','))"
            $r = Run-Cli @($f, "--unlock", "--yes")
            Check "Systemsperre: --unlock meldet 'nur beim Neustart', exit 2" ($r.Code -eq 2 -and $r.Out -match "Neustart") "(exit $($r.Code))"
        }
        Stop-Process -Id $holder.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
        Check "Nach Ende des Client-Prozesses frei" (-not (Test-Locked $f))

        # GUI: Zeile "System – über Freigabe: <Benutzer>", Entsperren ohne Beenden-Rückfrage
        $holder = Start-Process powershell -ArgumentList "-NoProfile", "-WindowStyle", "Hidden", "-Command",
            "`$s=[System.IO.File]::Open('$unc','Open','ReadWrite','None'); Start-Sleep 900" -PassThru
        $deadline = (Get-Date).AddSeconds(20)
        while (-not (Test-Locked $f) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
        $gui = Start-Gui $f
        if ($gui.Hwnd -ne [IntPtr]::Zero) {
            $n = Wait-Rows $gui.Hwnd 1
            Check "GUI: Freigabe-Sperre gelistet" ($n -eq 1) "($n Zeilen)"
            Screenshot "gui-remote-open" $gui.Hwnd

            # "über Freigabe: <Benutzer> von <Client>" passt nicht in die Prozess-Spalte → Zeilen-Tooltip mit vollem Inhalt
            $list = Get-ListHwnd $gui.Hwnd
            Check "GUI: Zeile als gekürzt erkannt (Tooltip-Kandidat)" ([int][Native]::GetPropW($list, "UnlockerClippedRows") -ge 1)
            $header = [Native]::FindDescendantByClass($list, "SysHeader32")
            $hr = New-Object Native+RECT; [Native]::GetWindowRect($header, [ref]$hr) | Out-Null
            [Native]::SetForegroundWindow($gui.Hwnd) | Out-Null
            [Native]::SetCursorPos($hr.Left + 140, $hr.Bottom + 16) | Out-Null; Start-Sleep -Milliseconds 200
            [Native]::SetCursorPos($hr.Left + 150, $hr.Bottom + 18) | Out-Null
            $tip = [IntPtr]::Zero; $deadline = (Get-Date).AddSeconds(5)
            do {
                Start-Sleep -Milliseconds 250
                foreach ($w in [Native]::TopWindowsOf([uint32]$gui.Proc.Id)) { if ([Native]::Class($w) -eq "tooltips_class32") { $tip = $w } }
            } while ($tip -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
            Check "GUI: Zeilen-Tooltip erscheint beim Überfahren" ($tip -ne [IntPtr]::Zero)
            $tipLen = [int][Native]::GetPropW($list, "UnlockerRowTipLen")
            Check "GUI: Zeilen-Tooltip mit Inhalt (Prozess, Freigabe, Art, Pfad)" ($tipLen -gt $f.Length) "($tipLen Zeichen)"
            if ($tip -ne [IntPtr]::Zero) { Start-Sleep -Milliseconds 800; Screenshot "gui-row-tooltip" }  # Einblend-Animation abwarten
            [Native]::SetCursorPos($hr.Left - 40, $hr.Top - 120) | Out-Null; Start-Sleep -Milliseconds 300
            Click-Button (Find-Button $gui.Hwnd $Lang.Unlock)
            # Keine Beenden-Rückfrage (System wird nie beendet). Bleibt die Datei gesperrt, kommt stattdessen der
            # Fehlerdialog mit der Begründung – beides konsistent mit dem tatsächlichen Zustand.
            $dlg = Find-Dialog $gui.Proc.Id 5
            $locked = Test-Locked $f
            if ($dlg -ne [IntPtr]::Zero) {
                Start-Sleep -Milliseconds 500
                Screenshot "gui-remote-still-locked"
                $okBtn = Find-Button $dlg "OK"
                Check "GUI: Dialog ist der Fehlerbericht (nur OK, keine Beenden-Rückfrage)" ($okBtn -ne [IntPtr]::Zero -and (Find-Button $dlg $Lang.KillShort) -eq [IntPtr]::Zero)
                if ($okBtn -ne [IntPtr]::Zero) { Click-Button $okBtn }
            }
            Check "GUI: Ergebnis konsistent (frei ohne Fehler oder gesperrt mit Fehlerbericht)" `
                ((-not $locked -and $dlg -eq [IntPtr]::Zero) -or ($locked -and $dlg -ne [IntPtr]::Zero)) "(gesperrt $locked, Dialog $($dlg -ne [IntPtr]::Zero))"
            if ($locked) {
                Start-Sleep -Seconds 2
                Screenshot "gui-system-held" $gui.Hwnd
                Check "GUI: danach weiter gelistet (System)" ((Get-RowCount $gui.Hwnd) -eq 1)
            }
            Stop-Process -Id $gui.Proc.Id -Force
        }
    } finally {
        Stop-Process -Id $holder.Id -Force -ErrorAction SilentlyContinue
    }
}

$Sections["kern-wow64"] = {
    $w64 = "$Work\wow64"
    New-Item -ItemType Directory -Force -Path "$w64\cwd" | Out-Null
    Copy-Item "$env:WINDIR\SysWOW64\cmd.exe" "$w64\c32.exe"
    $cmd32 = Start-Process "$env:WINDIR\SysWOW64\cmd.exe" -ArgumentList "/k cd /d `"$w64\cwd`"" -WindowStyle Hidden -PassThru
    $img32 = Start-Process "$w64\c32.exe" -ArgumentList "/k" -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 2
    $r = Run-Cli @($w64, "--fast", "--json")
    $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
    Check "WOW64: JSON parsebar" ($null -ne $json)
    if ($json) {
        $cwdHit = $json[0].lockers | Where-Object { $_.pid -eq $cmd32.Id -and $_.kind -eq "cwd" }
        Check "32-Bit cmd.exe: Arbeitsverzeichnis erkannt (PEB32)" ($null -ne $cwdHit)
        if ($cwdHit) {
            Check "32-Bit cmd.exe: wow64-Flag" ($cwdHit.wow64 -eq $true)
            Check "32-Bit cmd.exe: CWD-Pfad stimmt" ($cwdHit.path -ieq "$w64\cwd") "($($cwdHit.path))"
            Check "32-Bit cmd.exe: Handle bekannt (schließbar)" ($cwdHit.handle -ne 0)
        }
        $modHit = $json[0].lockers | Where-Object { $_.pid -eq $img32.Id -and $_.kind -eq "module" }
        Check "32-Bit EXE: als geladenes Modul erkannt" ($null -ne $modHit)
        if ($modHit) { Check "32-Bit EXE: Pfad normalisiert" ($modHit.path -ieq "$w64\c32.exe") "($($modHit.path))" }
    }
    $r = Run-Cli @($w64, "--fast", "--unlock", "--yes")
    Check "WOW64 unlock: CWD-Handle geschlossen + Modul-Prozess beendet" ($r.Out -match "1 Handle\(s\) geschlossen, 1 Prozess\(e\) beendet")
    Start-Sleep -Milliseconds 500
    $img32.Refresh(); Check "32-Bit EXE-Prozess beendet" ($img32.HasExited)
    $cmd32.Refresh(); Check "32-Bit cmd.exe lebt noch (nur Handle geschlossen)" (-not $cmd32.HasExited)
    Stop-Process -Id $cmd32.Id, $img32.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    Remove-Item $w64 -Recurse -Force -ErrorAction SilentlyContinue
    Check "WOW64-Ordner danach löschbar" (-not (Test-Path $w64))
}

$Sections["kern-protect"] = {
    $pf = "$Work\protect.txt"
    $holderScript = "$Work\protect-holder.ps1"
    @"
Add-Type -Namespace N -Name K -MemberDefinition '[DllImport("kernel32.dll")] public static extern bool SetHandleInformation(System.IntPtr h, uint mask, uint flags);'
`$f = [System.IO.File]::Open('$pf', 'OpenOrCreate', 'ReadWrite', 'None')
[N.K]::SetHandleInformation(`$f.SafeFileHandle.DangerousGetHandle(), 2, 2) | Out-Null
Start-Sleep 900
"@ | Set-Content $holderScript -Encoding UTF8
    $ph = Start-Process powershell -ArgumentList "-NoProfile", "-WindowStyle", "Hidden", "-ExecutionPolicy", "Bypass", "-File", $holderScript -PassThru
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline -and -not ((Test-Path $pf) -and (Test-Locked $pf))) { Start-Sleep -Milliseconds 100 }
    Start-Sleep -Seconds 1
    $r = Run-Cli @($pf, "--fast", "--json")
    $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
    $hit = if ($json) { $json[0].lockers | Where-Object { $_.pid -eq $ph.Id } } else { $null }
    Check "Geschütztes Handle: Sperre gefunden" ($null -ne $hit)
    if ($hit) { Check "Geschütztes Handle: protectClose=true" ($hit.protectClose -eq $true) }
    $r = Run-Cli @($pf, "--fast", "--unlock", "--yes")
    Check "Geschütztes Handle: --unlock meldet Fehler statt Scheinerfolg" ($r.Code -eq 2 -and $r.Out -match "gesch")
    Check "Geschütztes Handle: Datei weiterhin gesperrt" (Test-Locked $pf)
    $r = Run-Cli @($pf, "--fast", "--kill", "--yes")
    Check "Geschütztes Handle: --kill löst die Sperre" ($r.Code -eq 0 -and -not (Test-Locked $pf))
    Stop-Process -Id $ph.Id -Force -ErrorAction SilentlyContinue

    $r = Run-Cli @("$env:WINDIR\System32\csrss.exe", "--fast", "--json")
    $json = $null; try { $json = $r.Out | ConvertFrom-Json } catch {}
    Check "csrss: JSON parsebar" ($null -ne $json)
    if ($json) {
        Check "csrss: hasProtected=true" ($json[0].hasProtected -eq $true)
        Check "csrss: accessDenied=false (als Admin korrekt unterschieden)" ($json[0].accessDenied -eq $false)
        Check "csrss: Einträge als protected markiert" (@($json[0].lockers | Where-Object { $_.protected -eq $true }).Count -ge 1)
        Check "csrss: Exit-Code 1 (nicht 3)" ($r.Code -eq 1) "(exit $($r.Code))"
    }
    $r = Run-Cli @("$env:WINDIR\System32\csrss.exe", "--fast", "--unlock", "--yes")
    Check "csrss: --unlock verweigert mit klarer Meldung" ($r.Code -eq 2 -and ($r.Out -match "Systemprozess|gesch"))
}

$Sections["kern-reboot"] = {
    $rbFile = "$Work\reboot-me.txt"
    Set-Content $rbFile "x"
    $gui = Start-Gui $rbFile
    if ($gui.Hwnd -ne [IntPtr]::Zero) {
        Wait-Rows $gui.Hwnd 0 | Out-Null
        [Native]::PostMessageW($gui.Hwnd, 0x0111, [IntPtr]2005, [IntPtr]::Zero) | Out-Null   # IDM_DELETE_REBOOT
        Start-Sleep -Seconds 2
        $key = "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager"
        $pending = (Get-ItemProperty $key -ErrorAction SilentlyContinue).PendingFileRenameOperations
        $entry = "\??\$rbFile"
        $match = if ($pending) { @($pending | Where-Object { $_ -like "*$entry" }) } else { @() }
        Check "PendingFileRenameOperations enthält die Datei" ($match.Count -ge 1)
        if ($pending) {
            Log "  Eintrag (24H2-Format mit Präfix): $($match -join ' | ')"
            $cleaned = @(); for ($i = 0; $i -lt $pending.Count; $i++) { if ($pending[$i] -like "*$entry") { $i++; continue }; $cleaned += $pending[$i] }
            if ($cleaned.Count -gt 0) { Set-ItemProperty $key -Name PendingFileRenameOperations -Value ([string[]]$cleaned) -Type MultiString }
            else { Remove-ItemProperty $key -Name PendingFileRenameOperations -ErrorAction SilentlyContinue }
        }
        Stop-Process -Id $gui.Proc.Id -Force
    } else { Check "Reboot-GUI: Fenster" $false }
}

# Kernel-Treiber (v2) – NUR in der Sandbox laden (BSOD träfe nur sie). Erst Voraussetzungen protokollieren
# (Secure Boot, HVCI, Testmodus), dann mit Testzertifikat signieren und als Dienst starten.
$Sections["kern-driver"] = {
    $sys = "$Bin\unlocker_driver.sys"
    if (-not (Test-Path $sys)) { Log "  SKIP  unlocker_driver.sys nicht gebaut (build.ps1 -Driver)"; return }
    try { Log "  Secure Boot: $(Confirm-SecureBootUEFI -ErrorAction Stop)" } catch { Log "  Secure Boot: $($_.Exception.Message)" }
    $dg = Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction SilentlyContinue
    if ($dg) { Log "  DeviceGuard: VBS=$($dg.VirtualizationBasedSecurityStatus) laufend=[$($dg.SecurityServicesRunning -join ',')] konfiguriert=[$($dg.SecurityServicesConfigured -join ',')] (2 = HVCI)" }
    Log "  bcdedit {current}: $(((& bcdedit /enum '{current}' 2>&1 | Out-String) -split "`r?`n" | Where-Object { $_ -match 'testsigning|nointegritychecks|hypervisor|path|osdevice' }) -join ' | ')"
    Log "  bcdedit /set testsigning on: $((& bcdedit /set testsigning on 2>&1 | Out-String).Trim())"

    $signtool = "C:\SDK\signtool.exe"
    if (-not (Test-Path $signtool)) { Log "  SKIP  signtool.exe fehlt (C:\SDK)"; return }
    # Schritte einzeln protokollieren statt abzubrechen – hier geht es darum, WO es in der Sandbox scheitert
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Unlocker Driver Test" -CertStoreLocation Cert:\CurrentUser\My
    $cer = "$Work\driver-test.cer"
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    foreach ($store in "Cert:\LocalMachine\Root", "Cert:\LocalMachine\TrustedPublisher") {
        try { Import-Certificate -FilePath $cer -CertStoreLocation $store -ErrorAction Stop | Out-Null; Log "  Zertifikat in ${store}: ok" }
        catch { Log "  Zertifikat in ${store}: $($_.Exception.Message)" }
    }
    Log "  signtool: $(((& $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $sys 2>&1 | Out-String).Trim()) -replace "`r?`n", ' | ')"

    & sc.exe delete UnlockerDriver 2>&1 | Out-Null
    Log "  sc create: $(((& sc.exe create UnlockerDriver type= kernel start= demand binPath= $sys 2>&1 | Out-String).Trim()) -replace "`r?`n", ' | ')"
    $start = ((& sc.exe start UnlockerDriver 2>&1 | Out-String).Trim()) -replace "`r?`n", ' | '
    Log "  sc start: $start"
    $running = (& sc.exe query UnlockerDriver 2>&1 | Out-String) -match "RUNNING"
    if ($running) {
        Check "Treiber geladen, Gerät \\.\UnlockerDriver vorhanden" (Test-Path '\\.\UnlockerDriver')
    } else {
        # Stand 24H2: kein BCD-Speicher in der Sandbox -> kein Testmodus -> StartService 577 (Signatur nicht prüfbar).
        # Keine Code-Regression, sondern eine Grenze der Umgebung: Laden/Testen braucht eine VM (Secure Boot aus, testsigning).
        Log "  SKIP  Treiber in der Sandbox nicht ladbar (kein Testmodus) - Laden/Testen braucht eine VM"
    }
    & sc.exe stop UnlockerDriver 2>&1 | Out-Null
    & sc.exe delete UnlockerDriver 2>&1 | Out-Null
}

$Sections["gui-basic"] = {
    Set-SystemTheme $false
    Start-Sleep -Seconds 1
    $g1file = "$Work\gui-demo.txt"
    $gh1 = Start-Holder $g1file
    $gui = Start-Gui $g1file
    Check "GUI-Fenster erscheint" ($gui.Hwnd -ne [IntPtr]::Zero) "(nach $($gui.StartMs) ms)"
    if ($gui.Hwnd -ne [IntPtr]::Zero) {
        $title = [Native]::Text($gui.Hwnd)
        Check "Titel enthält Dateiname" ($title -like "gui-demo.txt*")
        $n = Wait-Rows $gui.Hwnd 1
        Check "Liste zeigt 1 Sperre" ($n -eq 1) "($n Zeilen)"
        $gui.Proc.Refresh()
        Log "  RAM: Private $([math]::Round($gui.Proc.PrivateMemorySize64/1MB,1)) MB, WorkingSet $([math]::Round($gui.Proc.WorkingSet64/1MB,1)) MB"
        Screenshot "gui-locked" $gui.Hwnd
        foreach ($name in @($Lang.Open, $Lang.Unlock, $Lang.Kill, $Lang.Delete, $Lang.More)) {
            Check "Button '$name' vorhanden" ((Find-Button $gui.Hwnd $name) -ne [IntPtr]::Zero)
        }
        Check "Entsperren aktiviert" ([Native]::IsWindowEnabled((Find-Button $gui.Hwnd $Lang.Unlock)))
        Check "Elevate-Button bei Admin versteckt" (-not [Native]::IsWindowVisible((Find-Button $gui.Hwnd $Lang.Elevate)))
        Click-Button (Find-Button $gui.Hwnd $Lang.Unlock)
        $n = Wait-Rows $gui.Hwnd 0
        Check "nach Entsperren: Liste leer" ($n -eq 0) "($n Zeilen)"
        Check "nach Entsperren: Datei frei" (-not (Test-Locked $g1file))
        Check "nach Entsperren: Entsperren deaktiviert" (-not [Native]::IsWindowEnabled((Find-Button $gui.Hwnd $Lang.Unlock)))
        Screenshot "gui-unlocked" $gui.Hwnd
        Stop-Process -Id $gui.Proc.Id -Force
    }
    Stop-Process -Id $gh1.Id -Force -ErrorAction SilentlyContinue

    $g2file = "$Work\gui-kill.txt"
    $gh2 = Start-Holder $g2file
    $gui = Start-Gui $g2file
    if ($gui.Hwnd -ne [IntPtr]::Zero) {
        Wait-Rows $gui.Hwnd 1 | Out-Null
        Click-Button (Find-Button $gui.Hwnd $Lang.Kill)
        $dlg = Find-Dialog $gui.Proc.Id 5
        Check "Beenden-Dialog erscheint" ($dlg -ne [IntPtr]::Zero)
        if ($dlg -ne [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 700
            Screenshot "gui-confirm-kill"
            $ok = Find-Button $dlg $Lang.KillShort
            Check "Dialog hat Button '$($Lang.KillShort)'" ($ok -ne [IntPtr]::Zero)
            if ($ok -ne [IntPtr]::Zero) { Click-Button $ok }
            Start-Sleep -Seconds 2
            $gh2.Refresh(); Check "Holder beendet" ($gh2.HasExited)
            Check "Liste danach leer" ((Wait-Rows $gui.Hwnd 0) -eq 0)
        }
        Stop-Process -Id $gui.Proc.Id -Force
    }
    Stop-Process -Id $gh2.Id -Force -ErrorAction SilentlyContinue

    # Arbeitsverzeichnis-Sperre: Entsperren schließt nur das Verzeichnis-Handle – also keine Rückfrage
    # "Prozesse beenden?", und cmd.exe läuft danach weiter.
    $gcwd = "$Work\gui-cwd"
    New-Item -ItemType Directory -Force -Path "$gcwd\inner" | Out-Null
    $cwdProc = Start-Process cmd.exe -ArgumentList "/k cd /d `"$gcwd\inner`"" -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 1
    $gui = Start-Gui $gcwd
    if ($gui.Hwnd -ne [IntPtr]::Zero) {
        $deadline = (Get-Date).AddSeconds(10)
        while ((Get-RowCount $gui.Hwnd) -lt 1 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
        Check "CWD-Sperre: Zeile erscheint" ((Get-RowCount $gui.Hwnd) -ge 1) "($(Get-RowCount $gui.Hwnd) Zeilen)"
        Click-Button (Find-Button $gui.Hwnd $Lang.Unlock)
        $dlg = Find-Dialog $gui.Proc.Id 2
        Check "CWD-Sperre: Entsperren ohne Beenden-Rückfrage" ($dlg -eq [IntPtr]::Zero)
        if ($dlg -ne [IntPtr]::Zero) {
            Screenshot "gui-cwd-dialog"
            $cancel = Find-Button $dlg $Lang.Cancel
            if ($cancel -ne [IntPtr]::Zero) { Click-Button $cancel }
        }
        $n = Wait-Rows $gui.Hwnd 0
        Check "CWD-Sperre: Liste danach leer" ($n -eq 0) "($n Zeilen)"
        $cwdProc.Refresh(); Check "CWD-Sperre: cmd.exe läuft weiter" (-not $cwdProc.HasExited)
        $deleted = $false
        try { Remove-Item "$gcwd\inner" -Force -ErrorAction Stop; $deleted = $true } catch {}
        Check "CWD-Sperre: Ordner danach löschbar" $deleted
        Stop-Process -Id $gui.Proc.Id -Force
    }
    Stop-Process -Id $cwdProc.Id -Force -ErrorAction SilentlyContinue

    $g3file = "$Work\gui-delete.txt"
    $gh3 = Start-Holder $g3file
    $gui = Start-Gui $g3file
    if ($gui.Hwnd -ne [IntPtr]::Zero) {
        Wait-Rows $gui.Hwnd 1 | Out-Null
        Click-Button (Find-Button $gui.Hwnd $Lang.Delete)
        $dlg = Find-Dialog $gui.Proc.Id 5
        Check "Lösch-Dialog erscheint" ($dlg -ne [IntPtr]::Zero)
        if ($dlg -ne [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 700
            Screenshot "gui-confirm-delete"
            $ok = Find-Button $dlg $Lang.Delete
            if ($ok -ne [IntPtr]::Zero) { Click-Button $ok }
            Start-Sleep -Seconds 2
            Check "Datei gelöscht" (-not (Test-Path $g3file))
            Check "Titel zurückgesetzt" ([Native]::Text($gui.Hwnd) -eq "Unlocker")
        }
        Stop-Process -Id $gui.Proc.Id -Force
    }
    Stop-Process -Id $gh3.Id -Force -ErrorAction SilentlyContinue

    Set-SystemTheme $true
    Start-Sleep -Seconds 2
    $g4file = "$Work\gui-dark.txt"
    $gh4 = Start-Holder $g4file
    $gui = Start-Gui $g4file
    if ($gui.Hwnd -ne [IntPtr]::Zero) {
        Wait-Rows $gui.Hwnd 1 | Out-Null
        Screenshot "gui-systheme-dark" $gui.Hwnd
        Stop-Process -Id $gui.Proc.Id -Force
    }
    Stop-Process -Id $gh4.Id -Force -ErrorAction SilentlyContinue
    Set-SystemTheme $false

    $gui = Start-Gui $null
    Check "Leerstart zeigt Fenster" ($gui.Hwnd -ne [IntPtr]::Zero)
    if ($gui.Hwnd -ne [IntPtr]::Zero) {
        Screenshot "gui-empty" $gui.Hwnd
        Check "Entsperren ohne Ziel deaktiviert" (-not [Native]::IsWindowEnabled((Find-Button $gui.Hwnd $Lang.Unlock)))
        Check "Löschen ohne Ziel deaktiviert" (-not [Native]::IsWindowEnabled((Find-Button $gui.Hwnd $Lang.Delete)))
        Check "Öffnen aktiv" ([Native]::IsWindowEnabled((Find-Button $gui.Hwnd $Lang.Open)))
        Check "Liste im Leerzustand versteckt" (-not [Native]::IsWindowVisible((Get-ListHwnd $gui.Hwnd)))
        Stop-Process -Id $gui.Proc.Id -Force
    }
}
$Timeouts["gui-basic"] = 240

# IDropTarget: echter OLE-Drag per drop_self_test.exe – der Test fährt einen CF_HDROP-DoDragDrop mit
# synthetischer Maus auf das leere Fenster und prüft DragEnter (Akzentrahmen über die Prop
# UnlockerDragOver), Drop (Ziel gesetzt, Prop zurück auf 0) und Drop-Effekt COPY.
# UNLOCKER_DROPTARGET_TRACE=1 aktiviert die [droptarget]-Trace in unlocker.exe (-> $Out\unlocker-stderr.txt).
# Screenshots (BMP, Screen-BitBlt): drag-empty / drag-over / drag-dropped.
$Sections["gui-dragdrop"] = {
    $dt = "$Bin\drop_self_test.exe"
    if (-not (Test-Path $dt)) { Log "  SKIP  drop_self_test.exe nicht gebaut"; return }
    $f = "$Work\drag-drop-target.tmp"
    Set-Content -Path $f -Value "inhalt"
    foreach ($old in "$Out\drag-empty.bmp", "$Out\drag-over.bmp", "$Out\drag-dropped.bmp", "$Out\unlocker-stderr.txt") {
        Remove-Item $old -ErrorAction SilentlyContinue
    }
    # Datei-Redirection (wie dnd-run.ps1; PS 5.1 hat kein StandardOutputFilename):
    # der Test umleitet den stdout/stderr des ZIELprozesses per STARTUPINFO selbst.
    # Env-Var vor Start-Process: $p erbt sie, der Zieh-Prozess leitet sie an unlocker.exe weiter.
    # -Wait + .ExitCode: in der Sandbox (PS 5.1) liefert $p.WaitForExit() einen STALEN Prozess
    # (ExitCode bleibt leer) – erst der Cmdlet-Wait setzt die Eigenschaft (verifiziert: exit 5 → 5).
    # Hang-Schutz: interner Watchdog des Tests (180 s) + Abschnitts-Timeout (240 s) unten.
    $env:UNLOCKER_DROPTARGET_TRACE = "1"
    $p = Start-Process -FilePath $dt -ArgumentList "`"$Bin\unlocker.exe`" `"$f`" `"$Out`"" `
        -WorkingDirectory $Bin -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput "$Out\drag-drop-stdout.txt" `
        -RedirectStandardError "$Out\drag-drop-stderr.txt"
    Remove-Item Env:\UNLOCKER_DROPTARGET_TRACE -ErrorAction SilentlyContinue
    $p.Refresh()
    # ACHTUNG: nicht $out – PowerShell-Variablen sind case-insensitive und würden $Out (Out-Verzeichnis)
    # überschreiben, das Log() für den results.log-Pfad braucht ("Cannot find drive 'PASS Start'").
    $dout = if (Test-Path "$Out\drag-drop-stdout.txt") { [System.IO.File]::ReadAllText("$Out\drag-drop-stdout.txt") } else { "" }
    foreach ($line in ($dout -split "`r?`n" | Where-Object { $_ })) {
        if ($line -match "^\s*(PASS|FAIL|  )(.*)$") { Log "  $line" }
    }
    $zerr = if (Test-Path "$Out\unlocker-stderr.txt") { [System.IO.File]::ReadAllText("$Out\unlocker-stderr.txt") } else { "" }
    foreach ($line in ($zerr -split "`r?`n" | Where-Object { $_ })) {
        if ($line -match "DragEnter|Drop") { Log "  [droptarget] $line" }
    }
    Check "Drag & Drop: Exit-Code 0" ($p.ExitCode -eq 0)
    Check "Drag & Drop: DragEnter erkannt (Prop 1)" ($dout -match "PASS DragEnter: DragOver-Prop = 1")
    Check "Drag & Drop: Drop akzeptiert (Effekt COPY)" ($dout -match "PASS Drop-Effekt COPY")
    Check "Drag & Drop: Ziel gesetzt (Titel)" ($dout -match "PASS Titel enthaelt den abgelegten Dateinamen")
    Check "Drag & Drop: Prop nach Drop zurueck auf 0" ($dout -match "PASS nach Drop: DragOver-Prop zurueck auf 0")
    Check "Drag & Drop: Trace zeigt Drop (Ziel hat Datei empfangen)" ($zerr -match "DragQueryFile count=1")
    $bmps = @("$Out\drag-empty.bmp", "$Out\drag-over.bmp", "$Out\drag-dropped.bmp") | Where-Object { Test-Path $_ }
    Check "Drag & Drop: Screenshots vorhanden (empty/over/dropped)" ($bmps.Count -eq 3)
    if ($bmps.Count -eq 3) {
        # Get-FileHash fehlt hier – .NET-SHA256 direkt. Die drei Zustände (leer / Akzentrahmen / Ziel
        # gesetzt) müssen sichtbar sein: Akzentrahmen-Zustand weicht von beiden ab.
        $h = { param($path) $s = [System.IO.File]::OpenRead($path); try {
            [BitConverter]::ToString((New-Object Security.Cryptography.SHA256Managed).ComputeHash($s)) } finally { $s.Dispose() } }
        $he = & $h $bmps[0]; $ho = & $h $bmps[1]; $hd = & $h $bmps[2]
        Check "Drag & Drop: 'over'-Screenshot unterscheidet sich (Akzentrahmen)" (($ho -ne $he) -and ($ho -ne $hd))
    }
}

$Timeouts["gui-dragdrop"] = 240

$Sections["gui-english"] = {
    # Komplette Oberfläche auf Englisch; Buttons über die en-Tabelle
    $env:UNLOCKER_LANG = "en"
    $Lang = $L.en
    $r = Run-Cli @("--help")
    Check "CLI en: Usage" ($r.Out -match "Usage: unlockerc")
    $ef = "$Work\english.txt"
    $eh = Start-Holder $ef
    $r = Run-Cli @($ef, "--fast")
    Check "CLI en: Handle-Zeile ohne deutsche Begriffe" ($r.Out -match "Handle" -and $r.Out -notmatch "Prozess|Sperren")
    $gui = Start-Gui $ef
    Check "GUI en: Fenster" ($gui.Hwnd -ne [IntPtr]::Zero)
    if ($gui.Hwnd -ne [IntPtr]::Zero) {
        Wait-Rows $gui.Hwnd 1 | Out-Null
        foreach ($name in @("Open…", "Unlock", "End process", "Delete", "More")) {
            Check "GUI en: Button '$name'" ((Find-Button $gui.Hwnd $name) -ne [IntPtr]::Zero)
        }
        Screenshot "gui-english" $gui.Hwnd
        Click-Button (Find-Button $gui.Hwnd "End process")
        $dlg = Find-Dialog $gui.Proc.Id 5
        Check "GUI en: Dialog" ($dlg -ne [IntPtr]::Zero)
        if ($dlg -ne [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 500
            Screenshot "gui-english-dialog"
            Check "GUI en: Dialog-Buttons 'End'/'Cancel'" (((Find-Button $dlg "End") -ne [IntPtr]::Zero) -and ((Find-Button $dlg "Cancel") -ne [IntPtr]::Zero))
            Click-Button (Find-Button $dlg "End")
            Start-Sleep -Seconds 2
            $eh.Refresh(); Check "GUI en: Prozess beendet" ($eh.HasExited)
        }
        $p2 = Start-Process "$Bin\unlocker.exe" -ArgumentList "`"$ef`" `"$Work\gui-demo.txt`"" -PassThru
        Stop-Process -Id $gui.Proc.Id -Force
        $h2 = Wait-MainWindow $p2 10
        Start-Sleep -Seconds 2
        if ($h2 -ne [IntPtr]::Zero) { Check "GUI en: Titel '2 targets'" ([Native]::Text($h2) -like "2 targets*") "('$([Native]::Text($h2))')"; Stop-Process -Id $p2.Id -Force }
    }
    Stop-Process -Id $eh.Id -Force -ErrorAction SilentlyContinue
    Remove-Item Env:\UNLOCKER_LANG -ErrorAction SilentlyContinue
}

$Sections["gui-multi"] = {
    $m1 = "$Work\gui-multi-a.txt"; $m2 = "$Work\gui-multi-b.txt"
    $mh1 = Start-Holder $m1; $mh2 = Start-Holder $m2
    $p = Start-Process "$Bin\unlocker.exe" -ArgumentList "`"$m1`" `"$m2`"" -PassThru
    $h = Wait-MainWindow $p 15
    Start-Sleep -Seconds 3
    Check "Mehrfachziele: Fenster" ($h -ne [IntPtr]::Zero)
    if ($h -ne [IntPtr]::Zero) {
        Check "Mehrfachziele: Titel '2 Ziele'" ([Native]::Text($h) -like "2 Ziele*")
        Check "Mehrfachziele: 2 Zeilen" ((Wait-Rows $h 2) -eq 2)
        Screenshot "gui-multi" $h
        Click-Button (Find-Button $h $Lang.Unlock)
        $n = Wait-Rows $h 0
        Check "Mehrfachziele: beide entsperrt" ($n -eq 0 -and -not (Test-Locked $m1) -and -not (Test-Locked $m2))
        Stop-Process -Id $p.Id -Force
    }
    Stop-Process -Id $mh1.Id, $mh2.Id -Force -ErrorAction SilentlyContinue

    # Einsammeln: vier gleichzeitige Starts mit je einem Ziel (so startet Explorer das klassische Kontextmenü bei
    # Mehrfachauswahl) -> ein Fenster mit allen Zielen, die übrigen Prozesse geben ab und beenden sich.
    Get-Process unlocker -ErrorAction SilentlyContinue | Stop-Process -Force
    $files = @(1..4 | ForEach-Object { $f = "$Work\collect-$_.txt"; Set-Content -Path $f -Value "x"; $f })
    foreach ($f in $files) { Start-Process "$Bin\unlocker.exe" -ArgumentList "`"$f`"" | Out-Null }
    Start-Sleep -Seconds 4
    $alive = @(Get-Process unlocker -ErrorAction SilentlyContinue)
    Check "Einsammeln: nur ein unlocker.exe übrig" ($alive.Count -eq 1) "($($alive.Count) Prozesse)"
    $collector = [IntPtr]::Zero
    if ($alive.Count -ge 1) {
        $collector = Wait-MainWindow $alive[0] 5
        $title = [Native]::Text($collector)
        Check "Einsammeln: Titel '4 $($Lang.Targets)'" ($title -like "4 $($Lang.Targets)*") "($title)"
        Screenshot "gui-collect" $collector
    }
    # Nach Ablauf des Sammelfensters (1,5 s) öffnet ein weiterer Start wieder ein eigenes Fenster
    $late = Start-Process "$Bin\unlocker.exe" -ArgumentList "`"$($files[0])`"" -PassThru
    $lh = Wait-MainWindow $late 10
    Check "Späterer Start: eigenes Fenster" ($lh -ne [IntPtr]::Zero -and -not $late.HasExited)
    if ($collector -ne [IntPtr]::Zero) {
        Check "Späterer Start: Sammelfenster unverändert" ([Native]::Text($collector) -like "4 $($Lang.Targets)*") "($([Native]::Text($collector)))"
    }
    Get-Process unlocker -ErrorAction SilentlyContinue | Stop-Process -Force
}

$Sections["gui-dpi"] = {
    # Echter DPI-Wechsel: Skalierung des Monitors umschalten -> Windows schickt WM_DPICHANGED mit Vorschlagsrechteck.
    # In der Sandbox (RDP-Sitzung) ist die Skalierung nicht umschaltbar (min = max); dann deckt tests\host-dpi.ps1
    # den Wechsel zwischen zwei Monitoren ab, und hier bleibt der Start mit UNLOCKER_DPI=144 als Layout-Prüfung.
    [Native]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null   # PMv2: GetWindowRect in physischen Pixeln
    $df = "$Work\dpi.txt"
    $dh = Start-Holder $df
    $changed = $false
    try {
        $range = [Native]::GetDpiRange()
        $rangeText = if ($range) { "min/cur/max = $($range -join '/')" } else { "nicht lesbar" }
        if (-not $range -or $range[2] -le $range[1]) {
            Log "  SKIP  Skalierung in dieser Sitzung nicht umschaltbar ($rangeText) - tests\host-dpi.ps1 deckt den Monitorwechsel ab"
        } else {
            $gui = Start-Gui $df
            if ($gui.Hwnd -ne [IntPtr]::Zero) {
                Wait-Rows $gui.Hwnd 1 | Out-Null
                $list = Get-ListHwnd $gui.Hwnd
                $dpi0 = [Native]::GetDpiForWindow($gui.Hwnd)
                $w0 = [int][Native]::SendMessageW($list, 0x101D, [IntPtr]1, [IntPtr]::Zero)
                $rc0 = New-Object Native+RECT; [Native]::GetWindowRect($gui.Hwnd, [ref]$rc0) | Out-Null
                $rc = [Native]::SetDpiRelative($range[1] + 1)
                $changed = ($rc -eq 0)
                Check "Skalierung eine Stufe höher ($rangeText)" ($rc -eq 0) "(rc $rc)"
                $deadline = (Get-Date).AddSeconds(10)
                do { Start-Sleep -Milliseconds 300; $dpi1 = [Native]::GetDpiForWindow($gui.Hwnd) } while ($dpi1 -eq $dpi0 -and (Get-Date) -lt $deadline)
                Start-Sleep -Seconds 2
                Check "Fenster meldet neue DPI" ($dpi1 -gt $dpi0) "($dpi0 -> $dpi1)"
                if ($dpi1 -gt $dpi0) {
                    $w1 = [int][Native]::SendMessageW($list, 0x101D, [IntPtr]1, [IntPtr]::Zero)
                    $expected = [int][Math]::Round($w0 * $dpi1 / $dpi0)
                    Check "PID-Spalte mitskaliert" ([Math]::Abs($w1 - $expected) -le 2) "($w0 -> $w1, erwartet $expected)"
                    $rc1 = New-Object Native+RECT; [Native]::GetWindowRect($gui.Hwnd, [ref]$rc1) | Out-Null
                    $ratio = ($rc1.Right - $rc1.Left) / ($rc0.Right - $rc0.Left)
                    Check "Fenster auf Vorschlagsgröße" ([Math]::Abs($ratio - $dpi1 / $dpi0) -lt 0.08) "(Faktor $([math]::Round($ratio, 2)))"
                    Check "Liste weiterhin 1 Zeile" ((Get-RowCount $gui.Hwnd) -eq 1)
                    Screenshot "gui-dpi-high" $gui.Hwnd
                }
                [Native]::SetDpiRelative($range[1]) | Out-Null
                $changed = $false
                $deadline = (Get-Date).AddSeconds(10)
                do { Start-Sleep -Milliseconds 300; $dpi2 = [Native]::GetDpiForWindow($gui.Hwnd) } while ($dpi2 -ne $dpi0 -and (Get-Date) -lt $deadline)
                Start-Sleep -Seconds 2
                Check "Zurück auf Ausgangs-DPI" ($dpi2 -eq $dpi0) "($dpi2)"
                Check "PID-Spalte zurück" ([Math]::Abs(([int][Native]::SendMessageW($list, 0x101D, [IntPtr]1, [IntPtr]::Zero)) - $w0) -le 2)
                Click-Button (Find-Button $gui.Hwnd $Lang.Unlock)
                Check "nach DPI-Wechsel: Entsperren funktioniert" ((Wait-Rows $gui.Hwnd 0) -eq 0)
                Stop-Process -Id $gui.Proc.Id -Force
            } else { Check "DPI: Fenster" $false }
        }

        # Layout bei 150 % ab Start (UNLOCKER_DPI): Spalten, Fenstergröße und Bedienung
        if (Test-Locked $df) {
            # Geprüft wird die Standardgröße – eine von früheren Abschnitten gespeicherte Fensterposition würde sie überschreiben
            Remove-Item "$env:LOCALAPPDATA\Unlocker\settings.json" -ErrorAction SilentlyContinue
            $env:UNLOCKER_DPI = "144"
            $gui = Start-Gui $df
            Remove-Item Env:\UNLOCKER_DPI -ErrorAction SilentlyContinue
            Check "UNLOCKER_DPI=144: Fenster" ($gui.Hwnd -ne [IntPtr]::Zero)
            if ($gui.Hwnd -ne [IntPtr]::Zero) {
                Wait-Rows $gui.Hwnd 1 | Out-Null
                $list = Get-ListHwnd $gui.Hwnd
                $w = [int][Native]::SendMessageW($list, 0x101D, [IntPtr]1, [IntPtr]::Zero)
                Check "UNLOCKER_DPI=144: PID-Spalte 1,5x (105 px)" ([Math]::Abs($w - 105) -le 2) "($w)"
                $rc = New-Object Native+RECT; [Native]::GetWindowRect($gui.Hwnd, [ref]$rc) | Out-Null
                Check "UNLOCKER_DPI=144: Fenster 1,5x breit" ([Math]::Abs(($rc.Right - $rc.Left) - 1350) -le 20) "($($rc.Right - $rc.Left))"
                Screenshot "gui-dpi-144" $gui.Hwnd
                Click-Button (Find-Button $gui.Hwnd $Lang.Unlock)
                Check "UNLOCKER_DPI=144: Entsperren funktioniert" ((Wait-Rows $gui.Hwnd 0) -eq 0)
                Stop-Process -Id $gui.Proc.Id -Force
            }
        }
    } finally {
        if ($changed -and $range) { [Native]::SetDpiRelative($range[1]) | Out-Null }
        Stop-Process -Id $dh.Id -Force -ErrorAction SilentlyContinue
    }
}
$Sections["gui-groups"] = {
    $gfiles = @("$Work\grp-a.txt", "$Work\grp-b.txt", "$Work\grp-c.txt")
    $opens = ($gfiles | ForEach-Object { "[System.IO.File]::Open('$_','OpenOrCreate','ReadWrite','ReadWrite')" }) -join ";"
    $gh = Start-Process powershell -ArgumentList "-NoProfile", "-WindowStyle", "Hidden", "-Command", "`$h=@($opens); Start-Sleep 900" -PassThru
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and -not (($gfiles | ForEach-Object { Test-Path $_ }) -notcontains $false)) { Start-Sleep -Milliseconds 100 }
    Start-Sleep -Milliseconds 500
    $settingsFile = "$env:LOCALAPPDATA\Unlocker\settings.json"
    Remove-Item $settingsFile -ErrorAction SilentlyContinue
    $p = Start-Process "$Bin\unlocker.exe" -ArgumentList (($gfiles | ForEach-Object { "`"$_`"" }) -join " ") -PassThru
    $h = Wait-MainWindow $p 15
    Start-Sleep -Seconds 3
    if ($h -ne [IntPtr]::Zero) {
        $list = Get-ListHwnd $h
        Check "Gruppierung: 1 Gruppenkopf + 3 Einträge = 4 Zeilen" ((Wait-Rows $h 4) -eq 4)
        Check "Gruppierung: Titel '3 Ziele'" ([Native]::Text($h) -like "3 Ziele*")
        Screenshot "gui-groups" $h
        [Native]::PostMessageW($list, 0x0100, [IntPtr]0x28, [IntPtr]::Zero) | Out-Null
        Start-Sleep -Milliseconds 300
        [Native]::PostMessageW($list, 0x0100, [IntPtr]0x25, [IntPtr]::Zero) | Out-Null
        Check "Gruppe eingeklappt: 1 Zeile" ((Wait-Rows $h 1) -eq 1)
        [Native]::PostMessageW($list, 0x0100, [IntPtr]0x27, [IntPtr]::Zero) | Out-Null
        Check "Gruppe ausgeklappt: 4 Zeilen" ((Wait-Rows $h 4) -eq 4)

        $header = [Native]::FindDescendantByClass($list, "SysHeader32")
        $w0 = [int][Native]::SendMessageW($list, 0x101D, [IntPtr]0, [IntPtr]::Zero)
        $w1 = [int][Native]::SendMessageW($list, 0x101D, [IntPtr]1, [IntPtr]::Zero)
        $x = $w0 + $w1 + 20; $y = 10
        $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
        [Native]::PostMessageW($header, 0x0201, [IntPtr]1, $lp) | Out-Null
        [Native]::PostMessageW($header, 0x0202, [IntPtr]0, $lp) | Out-Null
        Start-Sleep -Milliseconds 800
        $sortCol = [int][Native]::GetPropW($list, "UnlockerSortCol") - 1
        $sortAsc = [int][Native]::GetPropW($list, "UnlockerSortAsc")
        Check "Sortierung: Spalte 'Art' aktiv" ($sortCol -eq 2)
        [Native]::PostMessageW($header, 0x0201, [IntPtr]1, $lp) | Out-Null
        [Native]::PostMessageW($header, 0x0202, [IntPtr]0, $lp) | Out-Null
        Start-Sleep -Milliseconds 800
        Check "Sortierung: zweiter Klick kehrt um" (([int][Native]::GetPropW($list, "UnlockerSortAsc")) -ne $sortAsc)

        $tip = [Native]::FindTopByClass([uint32]$p.Id, "tooltips_class32")
        Check "Tooltip-Fenster vorhanden" ($tip -ne [IntPtr]::Zero)
        if ($tip -ne [IntPtr]::Zero) {
            $tools = [int][Native]::SendMessageW($tip, 0x040D, [IntPtr]::Zero, [IntPtr]::Zero)
            Check "3 Chips -> 3 Tooltip-Tools" ($tools -eq 3) "($tools)"
        }

        [Native]::PostMessageW($list, 0x0100, [IntPtr]0x25, [IntPtr]::Zero) | Out-Null
        Wait-Rows $h 1 | Out-Null
        [Native]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        $p.WaitForExit(5000) | Out-Null
        Check "settings.json geschrieben" (Test-Path $settingsFile)
        if (Test-Path $settingsFile) {
            $json = Get-Content $settingsFile -Raw -Encoding UTF8
            Check "Sortierung persistiert (sortColumn 2)" ($json -match '"sortColumn":\s*2')
            Check "Eingeklappte Gruppe persistiert (powershell.exe)" ($json -match 'powershell\.exe')
        }
        $p = Start-Process "$Bin\unlocker.exe" -ArgumentList (($gfiles | ForEach-Object { "`"$_`"" }) -join " ") -PassThru
        $h = Wait-MainWindow $p 15
        Start-Sleep -Seconds 3
        if ($h -ne [IntPtr]::Zero) {
            $list = Get-ListHwnd $h
            Check "Neustart: Gruppe startet eingeklappt" ((Wait-Rows $h 1) -eq 1)
            Check "Neustart: Sortierung wiederhergestellt" (([int][Native]::GetPropW($list, "UnlockerSortCol") - 1) -eq 2)
            [Native]::PostMessageW($h, 0x0111, [IntPtr]2015, [IntPtr]::Zero) | Out-Null
            Check "Alle Gruppen ausklappen: 4 Zeilen" ((Wait-Rows $h 4) -eq 4)
            [Native]::PostMessageW($h, 0x0111, [IntPtr]2016, [IntPtr]::Zero) | Out-Null
            Check "Alle Gruppen einklappen: 1 Zeile" ((Wait-Rows $h 1) -eq 1)

            $header = [Native]::FindDescendantByClass($list, "SysHeader32")
            $hr = New-Object Native+RECT; [Native]::GetWindowRect($header, [ref]$hr) | Out-Null
            $lp = [IntPtr]((($hr.Top + 8) -shl 16) -bor (($hr.Left + 120) -band 0xFFFF))
            [Native]::PostMessageW($h, 0x007B, $header, $lp) | Out-Null
            $deadline = (Get-Date).AddSeconds(5); $popup = [IntPtr]::Zero
            do { Start-Sleep -Milliseconds 200; foreach ($w in [Native]::TopWindowsOf([uint32]$p.Id)) { if ([Native]::Class($w) -eq "#32768") { $popup = $w } } } while ($popup -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
            Check "Spaltenkopf: Rechtsklick-Menü erscheint" ($popup -ne [IntPtr]::Zero)
            if ($popup -ne [IntPtr]::Zero) { Screenshot "gui-column-menu"; [Native]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero); [Native]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 300 }
            [Native]::PostMessageW($h, 0x0111, [IntPtr]2022, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 500
            Check "Spalte 'Art' ausgeblendet (Breite 0)" (([int][Native]::SendMessageW($list, 0x101D, [IntPtr]2, [IntPtr]::Zero)) -eq 0)

            $chipPt = [IntPtr]((72 -shl 16) -bor 40)
            [Native]::PostMessageW($h, 0x0207, [IntPtr]::Zero, $chipPt) | Out-Null
            Start-Sleep -Seconds 2
            Check "Mittelklick entfernt Chip: Titel '2 Ziele'" ([Native]::Text($h) -like "2 Ziele*") "('$([Native]::Text($h))')"
            $origin = New-Object Native+POINT; $origin.X = 0; $origin.Y = 0
            [Native]::ClientToScreen($h, [ref]$origin) | Out-Null
            [Native]::SetForegroundWindow($h) | Out-Null
            [Native]::SetCursorPos($origin.X + 40, $origin.Y + 72) | Out-Null
            Start-Sleep -Milliseconds 300
            [Native]::PostMessageW($h, 0x0200, [IntPtr]::Zero, $chipPt) | Out-Null
            Start-Sleep -Milliseconds 300
            [Native]::PostMessageW($h, 0x0111, [IntPtr]1005, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Seconds 2
            $t = [Native]::Text($h)
            Check "Entf über Chip entfernt Ziel statt Datei" (($t -notlike "*Ziele*") -and (Test-Path $gfiles[0])) "('$t')"

            [Native]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            $p.WaitForExit(5000) | Out-Null
            $json2 = if (Test-Path $settingsFile) { Get-Content $settingsFile -Raw -Encoding UTF8 } else { "" }
            Check "hiddenColumns persistiert" ($json2 -match '"hiddenColumns":\s*\[2\]')
            $p = Start-Process "$Bin\unlocker.exe" -ArgumentList "`"$($gfiles[0])`"" -PassThru
            $h = Wait-MainWindow $p 15
            Start-Sleep -Seconds 2
            if ($h -ne [IntPtr]::Zero) {
                $list = Get-ListHwnd $h
                Check "Neustart: Spalte bleibt ausgeblendet" (([int][Native]::SendMessageW($list, 0x101D, [IntPtr]2, [IntPtr]::Zero)) -eq 0)
                [Native]::PostMessageW($h, 0x0111, [IntPtr]2022, [IntPtr]::Zero) | Out-Null
                Start-Sleep -Milliseconds 500
                Check "Spalte wieder eingeblendet" (([int][Native]::SendMessageW($list, 0x101D, [IntPtr]2, [IntPtr]::Zero)) -gt 0)
                Stop-Process -Id $p.Id -Force
            }
        }
        Remove-Item $settingsFile -ErrorAction SilentlyContinue
    } else { Check "Gruppierung: Fenster" $false }
    Stop-Process -Id $gh.Id -Force -ErrorAction SilentlyContinue
}
$Timeouts["gui-groups"] = 300

# Spalten per Drag im Kopf umsortieren (LVS_EX_HEADERDRAGDROP).
# WICHTIG (empirisch auf Win11 24H2 verifiziert): LVM_GET/SETCOLUMNORDERARRAY und HDM_GETITEM/RECT
# funktionieren CROSS-PROCESS NICHT (Puffer bleiben 0 / Ziel stürzt) – deshalb alles über
# Cross-Process-sichere Signale prüfen:
#   - Unterscheidender Header-Klick (HDM_ITEMDOWN/UP): iSubItem = LOGISCH, x = visuell.
#     Vor dem Drag liegt an x=295 die PID-Spalte (logisch 1, Prop 2), danach die Prozess-Spalte (logisch 0, Prop 1).
#   - Fitting: die VISUELL rechte sichtbare Spalte füllt die Restbreite (vorher Pfad, nach Pfad-Ausblenden Art).
#   - Persistenz: die App selbst schreibt columnOrder in die settings.json (in-process, zuverlässig).
# Order-Konvention: LVM_GET/SETCOLUMNORDERARRAY sind auf Win11 24H2 0-BASIS (Default [0,1,2,3],
# MSDN nennt 1..n – Fehlinformation); read[i] = logische Spalte an visuellem Platz i.
$Sections["gui-header-drag"] = {
    $hf = @("$Work\hdg-a.txt", "$Work\hdg-b.txt", "$Work\hdg-c.txt")
    $opens = ($hf | ForEach-Object { "[System.IO.File]::Open('$_','OpenOrCreate','ReadWrite','ReadWrite')" }) -join ";"
    $hh = Start-Process powershell -ArgumentList "-NoProfile", "-WindowStyle", "Hidden", "-Command", "`$h=@($opens); Start-Sleep 900" -PassThru
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and -not (($hf | ForEach-Object { Test-Path $_ }) -notcontains $false)) { Start-Sleep -Milliseconds 100 }
    Start-Sleep -Milliseconds 500
    $settingsFile = "$env:LOCALAPPDATA\Unlocker\settings.json"
    Remove-Item $settingsFile -ErrorAction SilentlyContinue
    $arg = ($hf | ForEach-Object { "`"$_`"" }) -join " "
    $p = Start-Process "$Bin\unlocker.exe" -ArgumentList $arg -PassThru
    $h = Wait-MainWindow $p 15
    Start-Sleep -Seconds 3
    if ($h -ne [IntPtr]::Zero) {
        [Drag]::EnsureForeground($h)
        $list = Get-ListHwnd $h
        $header = [Native]::FindDescendantByClass($list, "SysHeader32")
        Check "Spalten-Drag: Fenster + Header" ($list -ne [IntPtr]::Zero -and $header -ne [IntPtr]::Zero)
        Check "Spalten-Drag: 4 Zeilen (Gruppe + 3)" ((Wait-Rows $h 4) -eq 4)

        # LVS_EX_HEADERDRAGDROP (0x10) muss gesetzt sein (LVM_GETEXTENDEDLISTVIEWSTYLE = 0x1037)
        $ex = [int][Native]::SendMessageW($list, 0x1037, [IntPtr]::Zero, [IntPtr]::Zero)
        Check "Spalten-Drag: LVS_EX_HEADERDRAGDROP gesetzt" (($ex -band 0x10) -ne 0) "($ex)"

        # Breiten logisch (cross-process OK): 0=Prozess, 1=PID, 2=Art, 3=Pfad
        $w = @(); 0..3 | ForEach-Object { $w += [int][Native]::SendMessageW($list, 0x101D, [IntPtr]$_, [IntPtr]::Zero) }
        Check "Spalten-Drag: Standardbreiten (Prozess 260, PID 70, Art 130)" ($w[0] -eq 260 -and $w[1] -eq 70 -and $w[2] -eq 130) "($($w -join ','))"
        $hr = New-Object Native+RECT; [Native]::GetWindowRect($header, [ref]$hr) | Out-Null
        $lc = New-Object Native+RECT; [Native]::GetClientRect($list, [ref]$lc) | Out-Null
        $lw = $lc.Right - $lc.Left
        Check "Spalten-Drag: Pfad-Spalte füllt Restbreite" ([Math]::Abs($w[3] - ($lw - 460)) -le 12) "($($w[3]), erwartet ~$($lw - 460))"

        # Baseline: Klick an x=130 (visuell 0 = Prozess) -> sortColumn Prozess (Prop 0)
        $lp = [IntPtr](((10 -shl 16) -bor (130 -band 0xFFFF)))
        [Native]::PostMessageW($header, 0x0201, [IntPtr]1, $lp) | Out-Null
        [Native]::PostMessageW($header, 0x0202, [IntPtr]0, $lp) | Out-Null
        Start-Sleep -Milliseconds 700
        $prop = [int][Native]::GetPropW($list, "UnlockerSortCol") - 1
        Check "Spalten-Drag: Baseline-Klick x=130 sortiert Prozess" ($prop -eq 0) "($prop)"

        # DRAG: PID (visuell 1, Client-Mitte x = 260+35 = 295) vor Prozess (Client-Mitte x = 65).
        # DragColumn erwartet SCREEN-Koordinaten: Client-X + Header-Left.
        $hy = [int](($hr.Top + $hr.Bottom) / 2)
        [Drag]::EnsureForeground($h)
        Start-Sleep -Milliseconds 300
        [Drag]::DragColumn($hr.Left + 295, $hy, $hr.Left + 65)
        Start-Sleep -Milliseconds 1000

        # Unterscheidender Klick: x=295 ist nach dem Drag VISUELL Prozess -> sortColumn 0 (Prop 0).
        # Ohne Drag waere dort PID -> sortColumn 1 (Prop 1). (iSubItem ist logisch – siehe Abschnittskopf.)
        $lp = [IntPtr](((10 -shl 16) -bor (295 -band 0xFFFF)))
        [Native]::PostMessageW($header, 0x0201, [IntPtr]1, $lp) | Out-Null
        [Native]::PostMessageW($header, 0x0202, [IntPtr]0, $lp) | Out-Null
        Start-Sleep -Milliseconds 800
        $prop = [int][Native]::GetPropW($list, "UnlockerSortCol") - 1
        Check "Spalten-Drag: nach Drag sortiert x=295 Prozess (Order [1,0,2,3])" ($prop -eq 0) "($prop)"

        # Fitting muss jetzt die VISUELL rechte Spalte (Pfad) mit den andern Breiten befuellen
        $w2 = @(); 0..3 | ForEach-Object { $w2 += [int][Native]::SendMessageW($list, 0x101D, [IntPtr]$_, [IntPtr]::Zero) }
        Check "Spalten-Drag: Breiten nach Drag unveraendert (PID 70, Prozess 260, Art 130)" ($w2[1] -eq 70 -and $w2[0] -eq 260 -and $w2[2] -eq 130) "($($w2 -join ','))"
        Check "Spalten-Drag: Pfad füllt weiterhin Restbreite" ([Math]::Abs($w2[3] - ($lw - 460)) -le 12) "($($w2[3]))"
        Screenshot "gui-header-drag" $h

        # Persistenz: Schliessen -> settings.json
        [Native]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        $p.WaitForExit(5000) | Out-Null
        Check "Spalten-Drag: settings.json geschrieben" (Test-Path $settingsFile)
        $json = if (Test-Path $settingsFile) { Get-Content $settingsFile -Raw -Encoding UTF8 } else { "" }
        Check "Spalten-Drag: columnOrder [1,0,2,3] persistiert" ($json -match '"columnOrder":\s*\[1,\s*0,\s*2,\s*3\]')
        Check "Spalten-Drag: sortColumn 0 (Prozess) persistiert" ($json -match '"sortColumn":\s*0')

        # Neustart: Order + Sortierung wiederhergestellt (gleicher Klick-Trick)
        $p = Start-Process "$Bin\unlocker.exe" -ArgumentList $arg -PassThru
        $h = Wait-MainWindow $p 15
        Start-Sleep -Seconds 3
        if ($h -ne [IntPtr]::Zero) {
            [Drag]::EnsureForeground($h)
            $list = Get-ListHwnd $h
            $header = [Native]::FindDescendantByClass($list, "SysHeader32")
            Check "Spalten-Drag: Neustart – Sortierung wiederhergestellt (Prozess)" (([int][Native]::GetPropW($list, "UnlockerSortCol") - 1) -eq 0)
            # Order [1,0,2,3] -> visuell PID(0..70) Prozess(70..330) Art Pfad.
            # Klick x=35 (PID-Mitte, logisch 1, andere Spalte als aktuell Prozess) -> sortColumn 1 (Prop 1).
            # Ohne Order-Recovery (Default) laege dort Prozess (logisch 0) -> nur Toggle, Prop bliebe 0.
            $lp = [IntPtr](((10 -shl 16) -bor (35 -band 0xFFFF)))
            [Native]::PostMessageW($header, 0x0201, [IntPtr]1, $lp) | Out-Null
            [Native]::PostMessageW($header, 0x0202, [IntPtr]0, $lp) | Out-Null
            Start-Sleep -Milliseconds 800
            $prop = [int][Native]::GetPropW($list, "UnlockerSortCol") - 1
            Check "Spalten-Drag: Neustart – Order [1,0,2,3] wiederhergestellt (Klick x=35 = PID)" ($prop -eq 1) "($prop)"
            Screenshot "gui-header-drag-restart" $h

            # Fitting nach Pfad-Ausblenden: die VISUELL rechte sichtbare Spalte (Art) muss jetzt fitten
            [Native]::PostMessageW($h, 0x0111, [IntPtr]2023, [IntPtr]::Zero) | Out-Null  # IDM_COLUMN_PATH
            Start-Sleep -Milliseconds 500
            $w3 = @(); 0..3 | ForEach-Object { $w3 += [int][Native]::SendMessageW($list, 0x101D, [IntPtr]$_, [IntPtr]::Zero) }
            Check "Spalten-Drag: Pfad ausgeblendet (Breite 0)" ($w3[3] -eq 0) "($($w3 -join ','))"
            Check "Spalten-Drag: Art füllt jetzt Restbreite" ([Math]::Abs($w3[2] - ($lw - 330)) -le 12) "($($w3[2]), erwartet ~$($lw - 330))"
            Stop-Process -Id $p.Id -Force
        } else { Check "Spalten-Drag: Neustart – Fenster" $false }
        Remove-Item $settingsFile -ErrorAction SilentlyContinue
    } else { Check "Spalten-Drag: Fenster" $false }
    Stop-Process -Id $hh.Id -Force -ErrorAction SilentlyContinue
}
$Timeouts["gui-header-drag"] = 240

$Sections["gui-rename"] = {
    $rn = "$Work\gui-rename.txt"
    $rh = Start-Holder $rn
    $gui = Start-Gui $rn
    if ($gui.Hwnd -ne [IntPtr]::Zero) {
        Wait-Rows $gui.Hwnd 1 | Out-Null
        [Native]::PostMessageW($gui.Hwnd, 0x0111, [IntPtr]2001, [IntPtr]::Zero) | Out-Null
        $dlg = Find-Dialog $gui.Proc.Id 5
        Check "Umbenennen-Dialog erscheint" ($dlg -ne [IntPtr]::Zero)
        if ($dlg -ne [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 500
            Screenshot "gui-rename-dialog"
            $edit = [Native]::FindDescendantByClass($dlg, "Edit")
            Check "Dialog hat Eingabefeld" ($edit -ne [IntPtr]::Zero)
            if ($edit -ne [IntPtr]::Zero) {
                [Native]::SendMessageW($edit, 0x000C, [IntPtr]::Zero, [System.Runtime.InteropServices.Marshal]::StringToHGlobalUni("renamed-by-test.txt")) | Out-Null
                Click-Button (Find-Button $dlg $Lang.Rename)
                Start-Sleep -Milliseconds 800
                $confirmDlg = Find-Dialog $gui.Proc.Id 3
                if ($confirmDlg -ne [IntPtr]::Zero) { Click-Button (Find-Button $confirmDlg $Lang.Rename) }
                Start-Sleep -Seconds 2
                Check "Datei umbenannt" ((Test-Path "$Work\renamed-by-test.txt") -and -not (Test-Path $rn))
                Check "Titel zeigt neuen Namen" ([Native]::Text($gui.Hwnd) -like "renamed-by-test.txt*")
            }
        }
        [Native]::PostMessageW($gui.Hwnd, 0x0111, [IntPtr]1007, [IntPtr]::Zero) | Out-Null
        $deadline = (Get-Date).AddSeconds(5); $popup = [IntPtr]::Zero
        do { Start-Sleep -Milliseconds 200; foreach ($w in [Native]::TopWindowsOf([uint32]$gui.Proc.Id)) { if ([Native]::Class($w) -eq "#32768") { $popup = $w } } } while ($popup -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
        Check "Weitere-Menü erscheint" ($popup -ne [IntPtr]::Zero)
        if ($popup -ne [IntPtr]::Zero) { Start-Sleep -Milliseconds 400; Screenshot "gui-more-menu"; [Native]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero); [Native]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero) }
        Stop-Process -Id $gui.Proc.Id -Force
    }
    Stop-Process -Id $rh.Id -Force -ErrorAction SilentlyContinue
}

$Sections["shell-classic"] = {
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$Bin\packaging\register-classic-menu.ps1" -ExePath "$Bin\unlocker.exe" 2>&1 | ForEach-Object { Log "  $_" }
    $k = "HKCU:\Software\Classes\*\shell\Unlocker"
    Check "Registry-Key Datei" (Test-Path -LiteralPath "$k\command")
    Check "Registry-Key Ordner" (Test-Path -LiteralPath "HKCU:\Software\Classes\Directory\shell\Unlocker\command")
    $cmdVal = (Get-ItemProperty -LiteralPath "$k\command")."(default)"
    Check "command zeigt auf unlocker.exe" ($cmdVal -eq "`"$Bin\unlocker.exe`" `"%1`"")
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$Bin\packaging\register-classic-menu.ps1" -Remove 2>&1 | ForEach-Object { Log "  $_" }
    Check "Registry-Key entfernt" (-not (Test-Path -LiteralPath $k) -and -not (Test-Path -LiteralPath "HKCU:\Software\Classes\Directory\shell\Unlocker"))
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$Bin\packaging\register-classic-menu.ps1" -ExePath "$Bin\unlocker.exe" 2>&1 | Out-Null

    # -Remove, wenn unlocker.exe nicht (mehr) neben dem Skript liegt: Einträge trotzdem entfernen
    $lone = "$Work\script-only"
    New-Item -ItemType Directory -Force -Path $lone | Out-Null
    Copy-Item "$Bin\packaging\register-classic-menu.ps1" $lone
    $removeOut = & powershell -NoProfile -ExecutionPolicy Bypass -File "$lone\register-classic-menu.ps1" -Remove 2>&1 | Out-String
    Check "-Remove ohne unlocker.exe: Exit 0" ($LASTEXITCODE -eq 0) "(exit ${LASTEXITCODE}: $($removeOut.Trim()))"
    Check "-Remove ohne unlocker.exe: Einträge entfernt" (-not (Test-Path -LiteralPath $k))
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$Bin\packaging\register-classic-menu.ps1" -ExePath "$Bin\unlocker.exe" 2>&1 | Out-Null

    # Mehrfachauswahl wie im Explorer: Verb auf drei Dateien als Sammelaufruf (FolderItems3.InvokeVerbEx). Die Shell
    # startet dabei einen Prozess pro Datei ("%1", MultiSelectModel=Player) -> ein Fenster mit allen drei Zielen.
    Get-Process unlocker -ErrorAction SilentlyContinue | Stop-Process -Force
    $msDir = "$Work\classic-multi"
    New-Item -ItemType Directory -Force -Path $msDir | Out-Null
    1..3 | ForEach-Object { Set-Content -Path "$msDir\pick-$_.txt" -Value "x" }
    $items = (New-Object -ComObject Shell.Application).Namespace($msDir).Items()
    $items.Filter(0x40, "pick-*.txt")  # SHCONTF_NONFOLDERS
    Check "Mehrfachauswahl: 3 Dateien ausgewählt" ($items.Count -eq 3) "($($items.Count))"
    $items.InvokeVerbEx("Unlocker")
    $deadline = (Get-Date).AddSeconds(15)
    $title = ""
    do {
        Start-Sleep -Milliseconds 500
        $alive = @(Get-Process unlocker -ErrorAction SilentlyContinue)
        $title = if ($alive.Count -eq 1) { [Native]::Text((Wait-MainWindow $alive[0] 1)) } else { "" }
    } while (-not ($alive.Count -eq 1 -and $title -like "3 $($Lang.Targets)*") -and (Get-Date) -lt $deadline)
    Check "Mehrfachauswahl: ein Fenster" ($alive.Count -eq 1) "($($alive.Count) Prozesse)"
    Check "Mehrfachauswahl: Titel '3 $($Lang.Targets)'" ($title -like "3 $($Lang.Targets)*") "($title)"
    if ($alive.Count -eq 1) { Screenshot "shell-classic-multi" (Wait-MainWindow $alive[0] 1) }
    Get-Process unlocker -ErrorAction SilentlyContinue | Stop-Process -Force
}

$Sections["shell-sparse"] = {
    if (-not (Test-Path "C:\SDK\makeappx.exe")) { Log "  C:\SDK\makeappx.exe nicht vorhanden – übersprungen"; return }
    # Eigener Publisher: das Skript muss ihn ins Manifest übernehmen, sonst scheitert signtool (Subject != Publisher)
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$Bin\packaging\register-w11-menu.ps1" -BinDir $Bin -SdkBin "C:\SDK" -TrustCert -Publisher "CN=Unlocker Sandbox" 2>&1 | ForEach-Object { Log "  $_" }
    $pkg = Get-AppxPackage -Name "Unlocker.App" -ErrorAction SilentlyContinue
    Check "Sparse Package registriert" ($null -ne $pkg)
    Check "Sparse Package: Publisher aus -Publisher" ($pkg.Publisher -eq "CN=Unlocker Sandbox") "($($pkg.Publisher))"
    Check "Sparse Package: Paket für den App-Schalter abgelegt" (Test-Path "$env:LOCALAPPDATA\Unlocker\unlocker-sparse.msix")
    if ($pkg) {
        $comKey = "HKLM:\SOFTWARE\Classes\PackagedCom\Package\$($pkg.PackageFullName)\Class\{CAD0BA06-5865-421D-8C66-5D339B9E8A62}"
        Check "COM-Klasse im PackagedCom-Store" (Test-Path $comKey)
    }
    $ctxFile = "$Work\ctxmenu-demo.txt"
    $ch = Start-Holder $ctxFile
    Start-Process explorer.exe -ArgumentList $Work | Out-Null
    Start-Sleep -Seconds 4
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, "CabinetWClass")
    $exp = $AE::RootElement.FindFirst($TS::Children, $cond)
    Check "Explorer-Fenster gefunden" ($null -ne $exp)
    if ($exp) {
        [Native]::SetForegroundWindow([IntPtr]$exp.Current.NativeWindowHandle) | Out-Null
        $itemCond = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::ListItem)
        $items = $exp.FindAll($TS::Descendants, $itemCond)
        $target = $null
        foreach ($i in $items) { if ($i.Current.Name -like "ctxmenu-demo*") { $target = $i; break } }
        Check "Datei im Explorer gefunden" ($null -ne $target)
        if ($target) {
            $target.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
            $target.SetFocus()
            Start-Sleep -Milliseconds 800
            [Native]::keybd_event(0x5D, 0, 0, [UIntPtr]::Zero); [Native]::keybd_event(0x5D, 0, 2, [UIntPtr]::Zero)
            Start-Sleep -Seconds 3
            Screenshot "explorer-context-menu"
            $mi = Uia-FindByName $AE::RootElement "Mit Unlocker entsperren" 5
            Check "Menüeintrag 'Mit Unlocker entsperren' sichtbar" ($null -ne $mi)
            if ($mi) {
                try { Uia-Invoke $mi } catch { Log "  Invoke via UIA fehlgeschlagen: $_" }
                Start-Sleep -Seconds 4
                $launched = Get-Process unlocker -ErrorAction SilentlyContinue | Select-Object -First 1
                Check "unlocker.exe über Kontextmenü gestartet" ($null -ne $launched)
                if ($launched) {
                    $hl = Wait-MainWindow $launched 5
                    if ($hl -ne [IntPtr]::Zero) { Check "Kontextmenü übergibt Pfad" ([Native]::Text($hl) -like "ctxmenu-demo.txt*") }
                    Stop-Process -Id $launched.Id -Force
                }
            } else {
                [Native]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero); [Native]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero)
            }
        }
    }
    Stop-Process -Id $ch.Id -Force -ErrorAction SilentlyContinue

    # App-Schalter "Weitere" > Windows-11-Kontextmenü (IDM_SHELL_MENU 2030): ohne Ziel bedienbar, Häkchen = registriert,
    # schaltet ab und wieder an (PackageManager statt PowerShell); ohne abgelegtes Paket kommt ein Hinweis.
    Get-Process unlocker -ErrorAction SilentlyContinue | Stop-Process -Force
    $gui = Start-Gui $null
    if ($gui.Hwnd -ne [IntPtr]::Zero) {
        $h = $gui.Hwnd
        Check "App-Schalter: 'Weitere' ohne Ziel aktiv" ([Native]::IsWindowEnabled((Find-Button $h $Lang.More)))
        function Get-ShellItemState {
            [Native]::PostMessageW($h, 0x0111, [IntPtr]1007, [IntPtr]::Zero) | Out-Null   # IDC_MORE
            $deadline = (Get-Date).AddSeconds(5); $popup = [IntPtr]::Zero
            do { Start-Sleep -Milliseconds 200; foreach ($w in [Native]::TopWindowsOf([uint32]$gui.Proc.Id)) { if ([Native]::Class($w) -eq "#32768") { $popup = $w } } } while ($popup -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
            if ($popup -eq [IntPtr]::Zero) { return -2 }
            Start-Sleep -Milliseconds 300
            $state = [Native]::PopupItemState($popup, 2030)
            Screenshot "gui-shell-menu-$state"
            [Native]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero); [Native]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 400
            return $state
        }
        function Invoke-ShellToggle {
            $sw = [Diagnostics.Stopwatch]::StartNew()
            [Native]::PostMessageW($h, 0x0111, [IntPtr]2030, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 500
            $deadline = (Get-Date).AddSeconds(60)
            while ([int][Native]::GetPropW($h, "UnlockerShellBusy") -ne 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
            Log "  Schalter: $($sw.ElapsedMilliseconds) ms"
        }
        $state = Get-ShellItemState
        Check "App-Schalter: Eintrag vorhanden, mit Häkchen (registriert)" ($state -ge 0 -and ($state -band 0x8)) "(state $state)"

        Invoke-ShellToggle
        Check "App-Schalter: Paket entfernt" ($null -eq (Get-AppxPackage -Name "Unlocker.App" -ErrorAction SilentlyContinue))
        Check "App-Schalter: Häkchen weg" (-not ((Get-ShellItemState) -band 0x8))

        $cached = "$env:LOCALAPPDATA\Unlocker\unlocker-sparse.msix"
        Move-Item $cached "$cached.bak" -Force
        [Native]::PostMessageW($h, 0x0111, [IntPtr]2030, [IntPtr]::Zero) | Out-Null
        $dlg = Find-Dialog $gui.Proc.Id 5
        Check "App-Schalter: ohne Paket Hinweis-Dialog" ($dlg -ne [IntPtr]::Zero)
        if ($dlg -ne [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 500
            Screenshot "gui-shell-missing"
            Click-Button (Find-Button $dlg "OK")
            Start-Sleep -Milliseconds 500
        }
        Move-Item "$cached.bak" $cached -Force

        Invoke-ShellToggle
        $pkg = Get-AppxPackage -Name "Unlocker.App" -ErrorAction SilentlyContinue
        Check "App-Schalter: Paket wieder registriert" ($null -ne $pkg)
        if ($pkg) {
            Log "  Paket: $($pkg.PackageFullName), InstallLocation $($pkg.InstallLocation)"
            Check "App-Schalter: COM-Klasse registriert" (Test-Path "HKLM:\SOFTWARE\Classes\PackagedCom\Package\$($pkg.PackageFullName)\Class\{CAD0BA06-5865-421D-8C66-5D339B9E8A62}")
        }
        Check "App-Schalter: Häkchen wieder da" ((Get-ShellItemState) -band 0x8)
        Check "App-Schalter: kein Fehlerdialog" ((Find-Dialog $gui.Proc.Id 1) -eq [IntPtr]::Zero)
        Stop-Process -Id $gui.Proc.Id -Force
    }

    & powershell -NoProfile -ExecutionPolicy Bypass -File "$Bin\packaging\register-w11-menu.ps1" -Remove 2>&1 | ForEach-Object { Log "  $_" }
    Check "Sparse Package entfernt" ($null -eq (Get-AppxPackage -Name "Unlocker.App" -ErrorAction SilentlyContinue))
}
$Timeouts["shell-sparse"] = 300

$Sections["portable"] = {
    $zip = Get-ChildItem "$Share\portable\Unlocker-*-portable.zip" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $zip) { Log "  SKIP  kein Portable-ZIP in C:\Share\portable (packaging\make-portable.ps1)"; return }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $dest = "C:\PortableTest"
    Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
    [System.IO.Compression.ZipFile]::ExtractToDirectory($zip.FullName, $dest)
    $dir = "$dest\Unlocker"
    Check "ZIP: Wurzelordner Unlocker\ mit allen Dateien" ((Test-Path "$dir\unlocker.exe") -and (Test-Path "$dir\unlockerc.exe") -and (Test-Path "$dir\unlocker_shell.dll") -and (Test-Path "$dir\README.md") -and (Test-Path "$dir\portable.ini") -and (Test-Path "$dir\kontextmenu\register-w11-menu.ps1") -and (Test-Path "$dir\kontextmenu\AppxManifest.xml"))
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $bad = @()
    foreach ($line in Get-Content "$dir\SHA256SUMS.txt") {
        $hash, $rel = $line -split "  ", 2
        $s = [System.IO.File]::OpenRead("$dir\$rel"); $actual = ([System.BitConverter]::ToString($sha.ComputeHash($s)) -replace "-", "").ToLower(); $s.Dispose()
        if ($actual -ne $hash) { $bad += $rel }
    }
    Check "ZIP: SHA256SUMS stimmen" ($bad.Count -eq 0) "($($bad -join ', '))"
    $ver = (Get-Item "$dir\unlocker.exe").VersionInfo
    Check "ZIP: Versionsressource (alle drei Binaries)" ($ver.FileVersion -and ((Get-Item "$dir\unlockerc.exe").VersionInfo.FileVersion -eq $ver.FileVersion) -and ((Get-Item "$dir\unlocker_shell.dll").VersionInfo.FileVersion -eq $ver.FileVersion)) "($($ver.FileVersion))"
    Check "ZIP: README nennt die Version" ((Get-Content "$dir\README.md" -Raw) -match [regex]::Escape($ver.FileVersion))

    # Portable-Modus: settings.json neben der EXE, nicht in %LOCALAPPDATA%
    Remove-Item "$env:LOCALAPPDATA\Unlocker\settings.json" -ErrorAction SilentlyContinue
    $pf = "$Work\portable-lock.txt"
    $ph = Start-Holder $pf
    $p = Start-Process "$dir\unlocker.exe" -ArgumentList "`"$pf`"" -PassThru
    $h = Wait-MainWindow $p 15
    Start-Sleep -Seconds 2
    Check "ZIP: GUI startet aus dem entpackten Ordner" ($h -ne [IntPtr]::Zero)
    if ($h -ne [IntPtr]::Zero) {
        Check "ZIP: Sperre gefunden" ((Wait-Rows $h 1) -eq 1)
        [Native]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        $p.WaitForExit(5000) | Out-Null
    }
    Check "Portable-Modus: settings.json neben der EXE" (Test-Path "$dir\settings.json")
    Check "Portable-Modus: nichts in %LOCALAPPDATA%" (-not (Test-Path "$env:LOCALAPPDATA\Unlocker\settings.json"))
    Remove-Item "$dir\portable.ini"
    $p = Start-Process "$dir\unlocker.exe" -PassThru
    $h = Wait-MainWindow $p 15
    Start-Sleep -Seconds 1
    if ($h -ne [IntPtr]::Zero) { [Native]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null; $p.WaitForExit(5000) | Out-Null }
    Check "ohne portable.ini: settings.json in %LOCALAPPDATA%" (Test-Path "$env:LOCALAPPDATA\Unlocker\settings.json")
    Stop-Process -Id $ph.Id -Force -ErrorAction SilentlyContinue

    # Kontextmenü-Skripte ohne Parameter aus dem ZIP-Layout
    $help = & "$dir\unlockerc.exe" --help 2>&1
    Check "ZIP: unlockerc --help mit Version" (($help -join "`n") -match "unlockerc $([regex]::Escape($ver.FileVersion))")
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$dir\kontextmenu\register-classic-menu.ps1" 2>&1 | ForEach-Object { Log "  $_" }
    $cmdVal = (Get-ItemProperty -LiteralPath "HKCU:\Software\Classes\*\shell\Unlocker\command" -ErrorAction SilentlyContinue)."(default)"
    Check "ZIP: klassisches Menü findet unlocker.exe eine Ebene höher" ($cmdVal -eq "`"$dir\unlocker.exe`" `"%1`"") "($cmdVal)"
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$dir\kontextmenu\register-classic-menu.ps1" -Remove 2>&1 | Out-Null
    if (Test-Path "C:\SDK\makeappx.exe") {
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$dir\kontextmenu\register-w11-menu.ps1" -SdkBin "C:\SDK" -TrustCert 2>&1 | ForEach-Object { Log "  $_" }
        $pkg = Get-AppxPackage -Name "Unlocker.App" -ErrorAction SilentlyContinue
        Check "ZIP: Sparse Package aus dem ZIP-Layout registriert" ($null -ne $pkg)
        if ($pkg) {
            # InstallLocation ist bei Sparse Packages der WindowsApps-Ordner; der externe Speicherort steht in EffectiveExternalPath (WinRT)
            $ext = $null
            try {
                $null = [Windows.Management.Deployment.PackageManager, Windows.Management.Deployment, ContentType = WindowsRuntime]
                $ext = (New-Object Windows.Management.Deployment.PackageManager).FindPackageForUser("", $pkg.PackageFullName).EffectiveExternalPath
            } catch { Log "  EffectiveExternalPath nicht lesbar: $_" }
            Check "ZIP: externer Speicherort = entpackter Ordner" ($ext -ieq $dir) "($ext)"
        }
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$dir\kontextmenu\register-w11-menu.ps1" -Remove 2>&1 | Out-Null
    } else { Log "  SKIP  C:\SDK fehlt - Sparse Package aus dem ZIP nicht geprüft" }
    Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
}

$Sections["startup"] = {
    $times = @()
    for ($i = 0; $i -lt 3; $i++) {
        $p = Start-Process "$Bin\unlocker.exe" -PassThru
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while ($sw.ElapsedMilliseconds -lt 10000) { $p.Refresh(); if ($p.MainWindowHandle -ne 0) { break }; Start-Sleep -Milliseconds 10 }
        $times += $sw.ElapsedMilliseconds
        Stop-Process -Id $p.Id -Force
        Start-Sleep -Milliseconds 300
    }
    Log "  Fenster sichtbar nach: $($times -join ' / ') ms"
    Check "Start unter 300 ms" (($times | Measure-Object -Maximum).Maximum -lt 300)
}

# Praxisfälle mit dem, was die Sandbox mitbringt. Notepad, Paint, Edge, Media Player und WordPad fehlen in der Sandbox
# (24H2) – Office, PDF-Reader, Player & Co. bleiben eine manuelle Checkliste (TODO.md). Zuletzt, weil der Explorer-Fall
# Handles des Explorers schließt.
$Timeouts["praxis"] = 240
$Sections["praxis"] = {
    $dir = "$Work\praxis"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    function Scan-Json($path, [string[]]$extra = @()) {
        $r = Run-Cli (@("--json") + $extra + @($path))
        $j = $null
        try { $j = @(ConvertFrom-Json $r.Out)[0] } catch {}
        return @{ Code = $r.Code; J = $j; Lockers = @($j.lockers | Where-Object { $_ }) }
    }
    function Describe($lockers) {
        $items = @($lockers | ForEach-Object {
            $flags = @()
            if ($_.critical) { $flags += "kritisch" }; if ($_.sensitive) { $flags += "sensibel" }
            if ($_.service) { $flags += "dienst" }; if ($_.protected) { $flags += "geschützt" }
            "$($_.process)[$($_.pid)] $($_.kind) $($flags -join ',')".Trim()
        })
        if ($items.Count -eq 0) { return "keine" }
        return $items -join "; "
    }

    # Log-Datei mitlesen (tail -f): liest geteilt und schließt zwischendurch – keine Sperre
    $log = "$dir\dienst.log"; Set-Content $log "Start"
    $tail = Start-Process powershell -ArgumentList "-NoProfile", "-WindowStyle", "Hidden", "-Command", "Get-Content -Wait '$log'" -PassThru
    Start-Sleep 3
    $s = Scan-Json $log
    Check "Log mitlesen (Get-Content -Wait): keine Sperre" ($s.Code -eq 0 -and $s.Lockers.Count -eq 0 -and -not (Test-Locked $log)) "(exit $($s.Code), $(Describe $s.Lockers))"
    Stop-Process -Id $tail.Id -Force -ErrorAction SilentlyContinue

    # ZIP im Explorer geöffnet: Explorer liest das Archiv, hält es aber nicht offen
    $zip = "$dir\archiv.zip"; Compress-Archive -Path $log -DestinationPath $zip -Force
    Start-Process explorer.exe "`"$zip`""; Start-Sleep 5
    $s = Scan-Json $zip
    Check "ZIP im Explorer geöffnet: keine Sperre" ($s.Code -eq 0 -and -not (Test-Locked $zip)) "(exit $($s.Code), $(Describe $s.Lockers))"

    # Miniaturansicht-Cache: nur festhalten (Verhalten hängt vom Explorer-Stand ab)
    $thumb = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\Windows\Explorer\thumbcache_*.db" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($thumb) { $s = Scan-Json $thumb.FullName; Log "  INFO  Miniaturansicht-Cache $($thumb.Name): exit $($s.Code), Sperren: $(Describe $s.Lockers)" }

    # Ordner im Explorer offen → entsperren → löschen (der Alltagsfall): Explorer-Handles schließen, nicht beenden
    $folder = "$dir\ordner"; New-Item -ItemType Directory -Force -Path $folder | Out-Null; Set-Content "$folder\a.txt" "x"
    Start-Process explorer.exe "`"$folder`""; Start-Sleep 5
    $explorers = @(Get-Process explorer -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $s = Scan-Json $folder
    $ex = @($s.Lockers | Where-Object { $_.process -eq "explorer.exe" })
    Check "Ordner im Explorer: Explorer gefunden (Handle, sensibel, nicht kritisch)" ($s.Code -eq 1 -and $ex.Count -ge 1 -and @($ex | Where-Object { $_.kind -ne "handle" -or -not $_.sensitive -or $_.critical }).Count -eq 0) "($(Describe $s.Lockers))"
    $u = Scan-Json $folder @("--unlock", "--yes")
    Check "Ordner im Explorer: --unlock schließt Handles, beendet nichts" ($u.Code -eq 0 -and $u.J.unlock.handlesClosed -ge 1 -and $u.J.unlock.processesTerminated -eq 0) "(exit $($u.Code), Handles $($u.J.unlock.handlesClosed), beendet $($u.J.unlock.processesTerminated))"
    Start-Sleep 2
    $alive = @($explorers | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
    Check "Ordner im Explorer: Explorer läuft weiter" ($alive.Count -eq $explorers.Count) "($($alive.Count)/$($explorers.Count))"
    Remove-Item -Recurse -Force $folder -ErrorAction SilentlyContinue
    Check "Ordner im Explorer: danach löschbar" (-not (Test-Path $folder))

    # Registry-Hive: hält der Minimal-Prozess "Registry" – kritisch und geschützt, Entsperren fasst ihn nicht an
    $hive = "$env:USERPROFILE\NTUSER.DAT"
    $s = Scan-Json $hive
    $reg = @($s.Lockers | Where-Object { $_.process -eq "Registry" })
    Check "Registry-Hive: Prozess Registry gefunden, kritisch" ($s.Code -eq 1 -and $reg.Count -ge 1 -and @($reg | Where-Object { -not $_.critical }).Count -eq 0) "($(Describe $s.Lockers))"
    $u = Scan-Json $hive @("--unlock", "--yes")
    Check "Registry-Hive: --unlock verweigert (exit 2), beendet nichts" ($u.Code -eq 2 -and $u.J.unlock.processesTerminated -eq 0 -and $u.J.unlock.handlesClosed -eq 0) "(exit $($u.Code))"
    Check "Registry-Hive: Registry-Prozess läuft weiter" ([bool](Get-Process -Name Registry -ErrorAction SilentlyContinue))

    # Ereignisprotokoll: Dienst-svchost hält die .evtx – nur scannen, nie entsperren
    $s = Scan-Json "$env:windir\System32\winevt\Logs\Application.evtx"
    $svc = @($s.Lockers | Where-Object { $_.process -eq "svchost.exe" -and $_.service -and $_.sensitive })
    Check "Ereignisprotokoll: svchost als Dienst (sensibel) gefunden" ($s.Code -eq 1 -and $svc.Count -ge 1) "($(Describe $s.Lockers))"
}

# Alltagsprogramme als portable Versionen (tests\sandbox\fetch-apps.ps1 → C:\Apps, schreibgeschützt). Je Fall: Datei im
# Programm öffnen, festhalten, wie es sie hält (blockiert Umbenennen/Löschen?), und prüfen, dass Unlocker dazu passt:
# blockiert → Programm gefunden, Entsperren schließt nur Handles, Programm läuft weiter, Datei danach löschbar;
# nicht blockiert → kein falscher Alarm, Datei löschbar.
$Timeouts["praxis-apps"] = 600  # LibreOffice installieren + erster Start
$Sections["praxis-apps"] = {
    if (-not (Test-Path "C:\Apps\*.zip")) { Log "  SKIP  keine Programme (tests\sandbox\fetch-apps.ps1 auf dem Host ausführen)"; return }
    $dir = "$Work\praxis-apps"; $appDir = "$dir\programme"
    New-Item -ItemType Directory -Force -Path $appDir | Out-Null
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    foreach ($zip in Get-ChildItem "C:\Apps\*.zip") {
        $target = Join-Path $appDir $zip.BaseName
        if (-not (Test-Path $target)) { [IO.Compression.ZipFile]::ExtractToDirectory($zip.FullName, $target) }
    }
    # MSI richtig (still) installieren – nur das bringt die VC++-Laufzeit mit; ein administratives Abbild ("msiexec /a")
    # startet nicht (MSVCP140.dll fehlt). Die Sandbox wird ohnehin verworfen.
    foreach ($msi in Get-ChildItem "C:\Apps\*.msi" -ErrorAction SilentlyContinue) {
        $target = Join-Path $appDir $msi.BaseName
        if (Test-Path $target) { continue }
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $p = Start-Process msiexec.exe -ArgumentList "/i", "`"$($msi.FullName)`"", "/qn", "/norestart", "INSTALLLOCATION=`"$target`"",
            "ISCHECKFORPRODUCTUPDATES=0", "QUICKSTART=0", "REGISTER_NO_MSO_TYPES=1" -Wait -PassThru
        Log "  INFO  $($msi.Name) installiert: exit $($p.ExitCode), $([math]::Round($sw.Elapsed.TotalSeconds)) s"
    }
    function Find-Exe($name) { Get-ChildItem $appDir -Recurse -Filter $name -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName }
    function Scan-Json($path, [string[]]$extra = @()) {
        $r = Run-Cli (@("--json") + $extra + @($path))
        $j = $null
        try { $j = @(ConvertFrom-Json $r.Out)[0] } catch {}
        return @{ Code = $r.Code; J = $j; Lockers = @($j.lockers | Where-Object { $_ }) }
    }
    function Describe($lockers) {
        $items = @($lockers | ForEach-Object { "$($_.process)[$($_.pid)] $($_.kind)" })
        if ($items.Count -eq 0) { return "keine" }
        return $items -join "; "
    }
    # Umbenennen braucht dieselbe Freigabe wie Löschen – zerstört die Datei aber nicht
    function Test-Blocked($path) {
        try { Rename-Item -LiteralPath $path "$(Split-Path $path -Leaf).umb" -ErrorAction Stop; Rename-Item -LiteralPath "$path.umb" (Split-Path $path -Leaf); return $false }
        catch { return $true }
    }
    # $procName: Prozess, der die Datei hält (bei LibreOffice soffice.bin, nicht der Starter soffice.exe)
    function Test-AppCase($label, $exe, [string[]]$appArgs, $file, $procName, $waitSec = 30, [switch]$DeleteWithUnlocker) {
        if (-not $exe) { Log "  SKIP  ${label}: Programm fehlt"; return }
        $app = Start-Process $exe -ArgumentList $appArgs -PassThru
        $alive = { @(Get-Process -Name ($procName -replace '\.exe$', '') -ErrorAction SilentlyContinue).Count -gt 0 }
        # Warten, bis ein Fenster des Programms den Dateinamen im Titel zeigt (Datei geladen), höchstens $waitSec
        $base = [IO.Path]::GetFileNameWithoutExtension($file)
        $sw = [Diagnostics.Stopwatch]::StartNew(); $title = ""
        while (-not $title -and $sw.Elapsed.TotalSeconds -lt $waitSec) {
            Start-Sleep -Milliseconds 500
            foreach ($proc in @(Get-Process -Name ($procName -replace '\.exe$', '') -ErrorAction SilentlyContinue)) {
                foreach ($w in [Native]::TopWindowsOf([uint32]$proc.Id)) { $t = [Native]::Text($w); if ($t -like "*$base*") { $title = $t } }
            }
        }
        Start-Sleep 2  # Programm fertig laden lassen
        Log "  INFO  ${label}: Fenster nach $([math]::Round($sw.Elapsed.TotalSeconds, 1)) s: '$title'"
        $running = & $alive
        $blocked = Test-Blocked $file
        $s = Scan-Json $file
        $found = @($s.Lockers | Where-Object { $_.process -ieq $procName })
        Log "  INFO  ${label}: läuft=$running, Datei offen=$(Test-Locked $file), Umbenennen/Löschen blockiert=$blocked, Unlocker exit $($s.Code): $(Describe $s.Lockers)"
        Check "${label}: Programm gestartet" $running
        Screenshot "praxis-$($procName -replace '\.exe$', '')"
        if ($blocked) {
            Check "${label}: blockiert → Unlocker findet $procName" ($s.Code -eq 1 -and $found.Count -ge 1) "($(Describe $s.Lockers))"
            $u = Scan-Json $file @("--unlock", "--yes")
            Start-Sleep 1
            Check "${label}: --unlock schließt Handles, beendet nichts" ($u.Code -eq 0 -and $u.J.unlock.handlesClosed -ge 1 -and $u.J.unlock.processesTerminated -eq 0) "(exit $($u.Code), Handles $($u.J.unlock.handlesClosed), beendet $($u.J.unlock.processesTerminated))"
            Check "${label}: Programm läuft nach dem Entsperren weiter" (& $alive)
        } else {
            Check "${label}: nicht blockiert → kein Fehlalarm (exit 0 oder nur geteilter Zugriff)" ($s.Code -eq 0 -or $found.Count -ge 1) "(exit $($s.Code), $(Describe $s.Lockers))"
        }
        if ($DeleteWithUnlocker) {
            $d = Run-Cli @("--delete", "--yes", $file)
            Check "${label}: unlockerc --delete exit 0" ($d.Code -eq 0) "(exit $($d.Code))"
        } else {
            Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
        }
        Check "${label}: Datei danach löschbar" (-not (Test-Path -LiteralPath $file))
        Stop-Process -Id $app.Id -Force -ErrorAction SilentlyContinue
        Get-Process -Name ($procName -replace '\.exe$', '') -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep 1
    }

    # PDF im Viewer (SumatraPDF): minimales PDF, MuPDF repariert die fehlende xref-Tabelle
    $pdf = "$dir\handbuch.pdf"
    Set-Content -Encoding ascii $pdf "%PDF-1.4`n1 0 obj <</Type/Catalog/Pages 2 0 R>> endobj`n2 0 obj <</Type/Pages/Kids[3 0 R]/Count 1>> endobj`n3 0 obj <</Type/Page/Parent 2 0 R/MediaBox[0 0 300 200]>> endobj`ntrailer <</Root 1 0 R>>`n%%EOF"
    Test-AppCase "PDF in SumatraPDF" (Find-Exe "SumatraPDF*.exe") @("-new-window", "`"$pdf`"") $pdf "SumatraPDF-3.6.1-64.exe"

    # Text im Editor (Notepad++)
    $txt = "$dir\notizen.txt"; Set-Content $txt "Einkaufsliste`r`nMilch"
    Test-AppCase "Text in Notepad++" (Find-Exe "notepad++.exe") @("-multiInst", "-nosession", "`"$txt`"") $txt "notepad++.exe"

    # Audio im Player (VLC, Endlosschleife): 60 s Ton als WAV erzeugen
    $wav = "$dir\musik.wav"
    $rate = 8000; $samples = $rate * 60
    $bw = New-Object IO.BinaryWriter([IO.File]::Create($wav))
    $bw.Write([Text.Encoding]::ASCII.GetBytes("RIFF")); $bw.Write([int](36 + $samples * 2)); $bw.Write([Text.Encoding]::ASCII.GetBytes("WAVEfmt "))
    $bw.Write([int]16); $bw.Write([int16]1); $bw.Write([int16]1); $bw.Write([int]$rate); $bw.Write([int]($rate * 2)); $bw.Write([int16]2); $bw.Write([int16]16)
    $bw.Write([Text.Encoding]::ASCII.GetBytes("data")); $bw.Write([int]($samples * 2))
    for ($i = 0; $i -lt $samples; $i++) { $bw.Write([int16](3000 * [Math]::Sin(2 * [Math]::PI * 440 * $i / $rate))) }
    $bw.Dispose()
    Test-AppCase "Audio in VLC" (Find-Exe "vlc.exe") @("--no-qt-privacy-ask", "--no-qt-updates-notif", "--no-qt-error-dialogs", "--loop", "`"$wav`"") $wav "vlc.exe"

    # Word-Dokument in LibreOffice Writer: minimales .docx (drei XML-Teile im ZIP), eigenes Profil ohne Startassistent
    $docx = "$dir\bericht.docx"
    $zip = [IO.Compression.ZipFile]::Open($docx, "Create")
    $parts = [ordered]@{
        "[Content_Types].xml" = '<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>'
        "_rels/.rels" = '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>'
        "word/document.xml" = '<?xml version="1.0" encoding="UTF-8"?><w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>Quartalsbericht</w:t></w:r></w:p></w:body></w:document>'
    }
    foreach ($name in $parts.Keys) {
        $w = New-Object IO.StreamWriter($zip.CreateEntry($name).Open(), (New-Object Text.UTF8Encoding($false)))
        $w.Write($parts[$name]); $w.Dispose()
    }
    $zip.Dispose()
    $profileUrl = "file:///" + ("$dir\lo-profil" -replace '\\', '/')
    # erster Start mit frischem Profil: ~40 s bis zum Fenster
    Test-AppCase "Word-Dokument in LibreOffice" (Find-Exe "soffice.exe") @("-env:UserInstallation=$profileUrl", "--norestore", "--nologo", "--nofirststartwizard", "--writer", "`"$docx`"") $docx "soffice.bin" 120 -DeleteWithUnlocker
    $lock = "$dir\.~lock.bericht.docx#"
    Check "Word-Dokument in LibreOffice: Sperrdatei .~lock.bericht.docx# mit gelöscht" (-not (Test-Path -LiteralPath $lock))
}

# Screenshots für das README (nur auf Abruf: -Only readme-shots). Englische Oberfläche, neutraler Beispielordner, nur
# das Fenster (PrintWindow) – kein Desktop, keine Kontonamen. Ergebnis: $Out\readme-*.png
$Timeouts["readme-shots"] = 240
$Sections["readme-shots"] = {
    Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Shot {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
    function Save-Window($hwnd, $name) {
        $r = New-Object Shot+RECT; [Shot]::GetWindowRect($hwnd, [ref]$r) | Out-Null
        $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
        $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
        [Shot]::PrintWindow($hwnd, $hdc, 2) | Out-Null
        $g.ReleaseHdc($hdc); $g.Dispose(); $bmp.Save("$Out\$name.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        Log "  screenshot $name.png ($($r.R - $r.L)x$($r.B - $r.T))"
    }

    $dir = "C:\Projects\Quarterly Report"
    New-Item -ItemType Directory -Force -Path "$dir\assets" | Out-Null
    Set-Content "$dir\notes.txt" "Q3 numbers"; Set-Content "$dir\report.docx" "draft"; Set-Content "$dir\assets\chart.png" "x"
    # Audio für VLC (10 s Ton)
    $wav = "$dir\assets\jingle.wav"; $rate = 8000; $samples = $rate * 10
    $bw = New-Object IO.BinaryWriter([IO.File]::Create($wav))
    $bw.Write([Text.Encoding]::ASCII.GetBytes("RIFF")); $bw.Write([int](36 + $samples * 2)); $bw.Write([Text.Encoding]::ASCII.GetBytes("WAVEfmt "))
    $bw.Write([int]16); $bw.Write([int16]1); $bw.Write([int16]1); $bw.Write([int]$rate); $bw.Write([int]($rate * 2)); $bw.Write([int16]2); $bw.Write([int16]16)
    $bw.Write([Text.Encoding]::ASCII.GetBytes("data")); $bw.Write([int]($samples * 2))
    for ($i = 0; $i -lt $samples; $i++) { $bw.Write([int16](2000 * [Math]::Sin(2 * [Math]::PI * 440 * $i / $rate))) }
    $bw.Dispose()

    # Sperren: Skript hält das Dokument, Eingabeaufforderung mit Arbeitsverzeichnis im Ordner, Explorer-Fenster, VLC
    $h1 = Start-Holder "$dir\report.docx"
    $cmd = Start-Process cmd.exe -ArgumentList "/k" -WorkingDirectory $dir -WindowStyle Minimized -PassThru
    Start-Process explorer.exe -ArgumentList "`"$dir`""
    $vlcZip = Get-ChildItem "C:\Apps\vlc-*.zip" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vlcZip) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $vlcDir = "$Work\vlc"; if (-not (Test-Path $vlcDir)) { [IO.Compression.ZipFile]::ExtractToDirectory($vlcZip.FullName, $vlcDir) }
        $vlc = Get-ChildItem $vlcDir -Recurse -Filter vlc.exe | Select-Object -First 1 -ExpandProperty FullName
        Start-Process $vlc -ArgumentList "--no-qt-privacy-ask", "--no-qt-updates-notif", "--qt-start-minimized", "--loop", "`"$wav`""
    } else { Log "  INFO  VLC fehlt (fetch-apps.ps1) – Bild ohne Player" }
    Start-Sleep 6

    $env:UNLOCKER_LANG = "en"
    $gui = Start-Gui $dir
    Remove-Item Env:UNLOCKER_LANG
    if ($gui.Hwnd -eq [IntPtr]::Zero) { Check "README: GUI gestartet" $false; return }
    $n = Get-RowCount $gui.Hwnd
    Check "README: Sperren gelistet" ($n -ge 3) "($n Zeilen)"
    [Native]::PostMessageW($gui.Hwnd, 0x0111, [IntPtr]2015, [IntPtr]::Zero) | Out-Null   # alle Gruppen ausklappen
    Start-Sleep 2
    Save-Window $gui.Hwnd "readme-main"

    # Bestätigung vor dem Löschen (listet, was vorher entsperrt wird)
    [Native]::PostMessageW($gui.Hwnd, 0x0111, [IntPtr]1005, [IntPtr]::Zero) | Out-Null   # IDC_DELETE
    $dlg = Find-Dialog $gui.Proc.Id 5
    if ($dlg -ne [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 700
        Save-Window $dlg "readme-confirm"
        $cancel = Find-Button $dlg "Cancel"; if ($cancel -ne [IntPtr]::Zero) { Click-Button $cancel }
    } else { Log "  INFO  Bestätigungsdialog nicht gefunden" }
    Check "README: Ordner unverändert" (Test-Path "$dir\report.docx")
    Stop-Process -Id $gui.Proc.Id -Force -ErrorAction SilentlyContinue
    Stop-Process -Id $cmd.Id -Force -ErrorAction SilentlyContinue
    Get-Process vlc -ErrorAction SilentlyContinue | Stop-Process -Force
}

# ============================================================================================
# Kindprozess: genau einen Abschnitt ausführen
# ============================================================================================
if ($Section) {
    if (-not $Sections.Contains($Section)) { Log "  FAIL  Unbekannter Abschnitt $Section"; exit 1 }
    try { & $Sections[$Section] }
    catch { Log "  FAIL  Ausnahme in ${Section}: $_"; Log "        $($_.ScriptStackTrace -replace "`r?`n", ' <- ')" }
    finally { Stop-Holders }
    exit 0
}

# ============================================================================================
# Orchestrator
# ============================================================================================
Remove-Item "$Out\DONE", "$Out\results.log" -ErrorAction SilentlyContinue
Log "=== System ==="
$os = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
Log "  Windows $($os.DisplayVersion) Build $($os.CurrentBuildNumber).$($os.UBR), User $env:USERNAME"
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
Log "  Admin: $($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)), Screen: $([System.Windows.Forms.Screen]::PrimaryScreen.Bounds)"

Log ""
Log "=== Dateien kopieren ==="
New-Item -ItemType Directory -Force -Path "$Bin\bin" | Out-Null
Copy-Item "$Share\bin\*" "$Bin\bin" -Recurse -Force
Copy-Item "$Share\packaging" "$Bin\packaging" -Recurse -Force
Move-Item "$Bin\bin\*" $Bin -Force
Remove-Item "$Bin\bin" -Recurse -Force
Check "Binaries vorhanden" ((Test-Path "$Bin\unlocker.exe") -and (Test-Path "$Bin\unlockerc.exe") -and (Test-Path "$Bin\unlocker_shell.dll"))
Log ("  Groessen: " + ((Get-ChildItem "$Bin\*.exe", "$Bin\*.dll" | ForEach-Object { "$($_.Name)=$([math]::Round($_.Length/1KB))KB" }) -join ", "))

$total = [Diagnostics.Stopwatch]::StartNew()
$Only = @($Only | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$OnDemand = @("readme-shots")   # laufen nur, wenn ausdrücklich per -Only gewählt
foreach ($name in $Sections.Keys) {
    if ($Only.Count -gt 0 -and $Only -notcontains $name) { continue }
    if ($Only.Count -eq 0 -and $OnDemand -contains $name) { continue }
    $timeout = if ($Timeouts.ContainsKey($name)) { $Timeouts[$name] } else { 180 }
    Log ""
    Log "=== $name (Timeout $timeout s) ==="
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $child = Start-Process powershell -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden", "-File", "`"$PSCommandPath`"", "-Section", $name -PassThru
    if (-not $child.WaitForExit($timeout * 1000)) {
        Stop-Process -Id $child.Id -Force -ErrorAction SilentlyContinue
        Stop-Holders
        Log "  FAIL  Timeout nach $timeout s in Abschnitt $name (abgebrochen, Lauf geht weiter)"
    }
    Log "  --- ${name}: $([math]::Round($sw.Elapsed.TotalSeconds, 1)) s"
}

Log ""
Log "=== Ergebnis ==="
$lines = Get-Content "$Out\results.log" -Encoding UTF8
$pass = @($lines | Where-Object { $_ -match "  PASS  " }).Count
$fail = @($lines | Where-Object { $_ -match "  FAIL  " }).Count
Log "  $pass bestanden, $fail fehlgeschlagen, Gesamtdauer $([math]::Round($total.Elapsed.TotalMinutes, 1)) min"
"$pass $fail" | Out-File "$Out\DONE" -Encoding ascii
