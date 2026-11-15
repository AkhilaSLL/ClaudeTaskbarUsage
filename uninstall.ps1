# Removes ClaudeTaskbarUsage for the current user.
# Windows PowerShell 5.1:  powershell -NoProfile -ExecutionPolicy Bypass -File uninstall.ps1 [-RemoveSettings]
#  1. Stops the installed ClaudeTaskbarUsage.exe (started from the install folder).
#  2. Removes the HKCU Run value ClaudeTaskbarUsage.
#  3. Removes %LOCALAPPDATA%\Programs\ClaudeTaskbarUsage.
#  With -RemoveSettings also %APPDATA%\ClaudeTaskbarUsage (settings.ini, last.json).
param([switch]$RemoveSettings)
$ErrorActionPreference = 'Stop'
$dir = Join-Path $env:LOCALAPPDATA 'Programs\ClaudeTaskbarUsage'
$dst = Join-Path $dir 'ClaudeTaskbarUsage.exe'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

$procs = @(Get-Process -Name 'ClaudeTaskbarUsage' -ErrorAction SilentlyContinue)
foreach ($p in $procs) {
    $path = $null
    try { $path = $p.Path } catch { $path = $null }
    if ($path -and ($path -ieq $dst)) {
        Write-Output "Stopping the installed instance (pid $($p.Id))"
        Stop-Process -Id $p.Id -Force
        $p.WaitForExit(5000) | Out-Null
    }
}

if (Get-ItemProperty -Path $runKey -Name 'ClaudeTaskbarUsage' -ErrorAction SilentlyContinue) {
    Remove-ItemProperty -Path $runKey -Name 'ClaudeTaskbarUsage'
    Write-Output 'Run value removed.'
}

if (Test-Path $dir) {
    Remove-Item -Path $dir -Recurse -Force
    Write-Output "Removed $dir"
}

if ($RemoveSettings) {
    $data = Join-Path $env:APPDATA 'ClaudeTaskbarUsage'
    if (Test-Path $data) {
        Remove-Item -Path $data -Recurse -Force
        Write-Output "Removed $data"
    }
} else {
    Write-Output "Settings kept in $(Join-Path $env:APPDATA 'ClaudeTaskbarUsage') (use -RemoveSettings to remove them)."
}
