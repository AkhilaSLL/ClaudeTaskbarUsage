# Test aid: starts build\ClaudeTaskbarUsage.exe with test environment variables, waits, captures the
# right part of the primary taskbar, and (unless -Keep) stops the process again.
# Usage: run_test_instance.ps1 -Out <png> [-DataDir <dir>] [-Sample <json>] [-Theme light|dark] [-Mode child|popup]
#        [-WaitSec 3] [-Instance <id>] [-OffsetX <px>] [-ExtDir <dir>] [-SampleStatus <code>] [-Keep]
# -ExtDir sets CTU_EXT_DIR (folder of the extension's quota files); -SampleStatus sets CTU_SAMPLE_STATUS.
# -Instance sets CTU_INSTANCE (own single-instance mutex, so it runs next to an installed widget);
# -OffsetX sets CTU_OFFSET_X (moves the test widget, negative = left). Stops only the process it started.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$DataDir = '',
    [string]$Sample = '',
    [string]$Theme = '',
    [string]$Mode = '',
    [string]$Instance = '',
    [string]$OffsetX = '',
    [string]$ExtDir = '',
    [string]$SampleStatus = '',
    [int]$WaitSec = 3,
    [int]$X = 1700,
    [int]$W = 860,
    [int]$Scale = 1,
    [switch]$Keep
)
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = Join-Path $root 'build\ClaudeTaskbarUsage.exe'
$env:CTU_DATA_DIR = $DataDir
$env:CTU_SAMPLE = $Sample
$env:CTU_THEME = $Theme
$env:CTU_MODE = $Mode
$env:CTU_INSTANCE = $Instance
$env:CTU_OFFSET_X = $OffsetX
$env:CTU_EXT_DIR = $ExtDir
$env:CTU_SAMPLE_STATUS = $SampleStatus
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds $WaitSec
& (Join-Path $PSScriptRoot 'capture.ps1') -Out $Out -X $X -Y 1380 -W $W -H 60 -Scale $Scale
$p.Refresh()
Write-Output ("pid {0} working set {1:N0} KB, private {2:N0} KB" -f $p.Id, ($p.WorkingSet64 / 1KB), ($p.PrivateMemorySize64 / 1KB))
if (-not $Keep) {
    Stop-Process -Id $p.Id -Force
    Write-Output "stopped pid $($p.Id)"
}
