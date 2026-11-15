# Installs ClaudeTaskbarUsage for the current user (no admin rights).
# Windows PowerShell 5.1:  powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1
#  1. Stops a running ClaudeTaskbarUsage.exe that was started from the install folder.
#  2. Copies build\ClaudeTaskbarUsage.exe to %LOCALAPPDATA%\Programs\ClaudeTaskbarUsage\.
#  3. Sets HKCU\Software\Microsoft\Windows\CurrentVersion\Run\ClaudeTaskbarUsage to the quoted path.
#  4. Starts the installed exe.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root 'build\ClaudeTaskbarUsage.exe'
if (-not (Test-Path $src)) { throw "Not found: $src. Run build.ps1 first." }

$dir = Join-Path $env:LOCALAPPDATA 'Programs\ClaudeTaskbarUsage'
$dst = Join-Path $dir 'ClaudeTaskbarUsage.exe'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

# 1. Stop the installed instance (only the one started from the install folder).
$procs = @(Get-Process -Name 'ClaudeTaskbarUsage' -ErrorAction SilentlyContinue)
foreach ($p in $procs) {
    $path = $null
    try { $path = $p.Path } catch { $path = $null }
    if ($path -and ($path -ieq $dst)) {
        Write-Output "Stopping the installed instance (pid $($p.Id))"
        Stop-Process -Id $p.Id -Force
        $p.WaitForExit(5000) | Out-Null
    } elseif ($path) {
        Write-Warning "Another ClaudeTaskbarUsage.exe runs from $path (pid $($p.Id)). It keeps the single-instance lock, so the installed exe exits at once until that one ends."
    }
}

# 2. Copy.
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
Copy-Item -Path $src -Destination $dst -Force
Write-Output "Copied to $dst"

# 3. Start with Windows.
if (-not (Test-Path $runKey)) { New-Item -Path $runKey -Force | Out-Null }
Set-ItemProperty -Path $runKey -Name 'ClaudeTaskbarUsage' -Value ('"' + $dst + '"')
Write-Output "Run value set: `"$dst`""

# 4. Start.
Start-Process -FilePath $dst
Write-Output 'Started.'
