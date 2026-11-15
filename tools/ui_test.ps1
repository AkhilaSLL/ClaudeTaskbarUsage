# Test aid: drives a running test instance through its menu command path (WM_COMMAND to the hidden
# controller window), hovers the widget to show the tooltip, captures screenshots, and exits the
# instance through the menu's Exit command. Restores the mouse position afterwards.
# Usage: ui_test.ps1 -OutDir <dir> [-DataDir <dir>] [-Sample <json>] [-TestAutorun]
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$DataDir = '',
    [string]$Sample = '',
    [switch]$TestAutorun,
    [int]$WaitSec = 3
)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class CtuUi {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string n);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
  // FindWindow by class name can fail across a sandbox boundary; enumerate the windows of the process instead.
  public static IntPtr FindOfProcess(uint pid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumProc check = null;
    check = (h, l) => {
      uint p; GetWindowThreadProcessId(h, out p);
      if (p == pid) {
        var sb = new System.Text.StringBuilder(256); GetClassName(h, sb, 256);
        if (sb.ToString() == cls) { found = h; return false; }
      }
      return true;
    };
    EnumWindows((h, l) => { if (!check(h, l)) return false; EnumChildWindows(h, check, IntPtr.Zero); return found == IntPtr.Zero; }, IntPtr.Zero);
    return found;
  }
}
"@
[CtuUi]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\ClaudeTaskbarUsage.exe'
$cap = Join-Path $PSScriptRoot 'capture.ps1'
$WM_COMMAND = 0x0111
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

$env:CTU_DATA_DIR = $DataDir
$env:CTU_SAMPLE = $Sample
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds $WaitSec

$ctl = [CtuUi]::FindOfProcess([uint32]$p.Id, 'ClaudeTaskbarUsage.Controller')
$wid = [CtuUi]::FindOfProcess([uint32]$p.Id, 'ClaudeTaskbarUsage.Widget')
$tray = [CtuUi]::FindWindow('Shell_TrayWnd', $null)
$mode = 'popup'
if ([CtuUi]::GetParent($wid) -eq $tray) { $mode = 'child' }
$r = New-Object CtuUi+RECT
[CtuUi]::GetWindowRect($wid, [ref]$r) | Out-Null
$ex = [CtuUi]::GetWindowLong($wid, -20)
Write-Output ("controller {0}, widget {1} ({2}), parent {3}, rect {4},{5},{6},{7}, exstyle 0x{8:X8}" -f $ctl, $wid, $mode, [CtuUi]::GetParent($wid), $r.L, $r.T, $r.R, $r.B, $ex)

# Hover: the tooltip.
$old = New-Object CtuUi+POINT
[CtuUi]::GetCursorPos([ref]$old) | Out-Null
$cx = [int](($r.L + $r.R) / 2); $cy = [int](($r.T + $r.B) / 2)
[CtuUi]::SetCursorPos($cx - 2, $cy) | Out-Null
Start-Sleep -Milliseconds 200
[CtuUi]::SetCursorPos($cx, $cy) | Out-Null
Start-Sleep -Milliseconds 1800
& $cap -Out (Join-Path $OutDir 'tooltip.png') -X 1600 -Y 1200 -W 960 -H 240
[CtuUi]::SetCursorPos($old.X, $old.Y) | Out-Null
Start-Sleep -Milliseconds 500

# Show remaining (menu command 110), capture, and back.
[CtuUi]::PostMessage($ctl, $WM_COMMAND, [IntPtr]110, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 800
& $cap -Out (Join-Path $OutDir 'remaining.png') -X 1900 -Y 1380 -W 660 -H 60 -Scale 2
[CtuUi]::PostMessage($ctl, $WM_COMMAND, [IntPtr]110, [IntPtr]::Zero) | Out-Null

# Interval 15 min (menu command 103) and back to 5 (101): settings.ini.
[CtuUi]::PostMessage($ctl, $WM_COMMAND, [IntPtr]103, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
if ($DataDir) { Write-Output ("settings after 15 min: " + ((Get-Content (Join-Path $DataDir 'settings.ini')) -join ' ')) }
[CtuUi]::PostMessage($ctl, $WM_COMMAND, [IntPtr]101, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
if ($DataDir) { Write-Output ("settings after 5 min: " + ((Get-Content (Join-Path $DataDir 'settings.ini')) -join ' ')) }

if ($TestAutorun) {
    # Start with Windows (menu command 111): on, check, off, check.
    [CtuUi]::PostMessage($ctl, $WM_COMMAND, [IntPtr]111, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 500
    $v = (Get-ItemProperty -Path $runKey -Name 'ClaudeTaskbarUsage' -ErrorAction SilentlyContinue).ClaudeTaskbarUsage
    Write-Output "Run value after on: $v"
    [CtuUi]::PostMessage($ctl, $WM_COMMAND, [IntPtr]111, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 500
    $v = (Get-ItemProperty -Path $runKey -Name 'ClaudeTaskbarUsage' -ErrorAction SilentlyContinue).ClaudeTaskbarUsage
    Write-Output "Run value after off: '$v'"
}

# Second start: must exit at once (single instance).
$p2 = Start-Process -FilePath $exe -PassThru
$exited = $p2.WaitForExit(3000)
Write-Output "second instance exited at once: $exited (exit code $($p2.ExitCode))"

$p.Refresh()
Write-Output ("working set {0:N0} KB, private {1:N0} KB, CPU {2:N2} s" -f ($p.WorkingSet64 / 1KB), ($p.PrivateMemorySize64 / 1KB), $p.TotalProcessorTime.TotalSeconds)

# Exit (menu command 112).
[CtuUi]::PostMessage($ctl, $WM_COMMAND, [IntPtr]112, [IntPtr]::Zero) | Out-Null
$gone = $p.WaitForExit(5000)
Write-Output "exited through the menu command: $gone"
if (-not $gone) { Stop-Process -Id $p.Id -Force; Write-Output "stopped pid $($p.Id)" }
