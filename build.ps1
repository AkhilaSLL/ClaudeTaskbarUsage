# Builds build\ClaudeTaskbarUsage.exe and build\ctu_tests.exe, then runs the tests.
# Windows PowerShell 5.1. Needs gcc and windres from MSYS2 ucrt64 on PATH.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root 'src'
$tests = Join-Path $root 'tests'
$build = Join-Path $root 'build'
if (-not (Test-Path $build)) { New-Item -ItemType Directory -Path $build | Out-Null }

$cflags = @('-std=c11', '-Wall', '-Wextra', '-Werror', '-Os', '-municode', '-DUNICODE', '-D_UNICODE',
            '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00')
$dataSrc = @('json.c', 'usage.c', 'engine.c', 'ext.c', 'logo.c', 'renew.c') | ForEach-Object { Join-Path $src $_ }

function Invoke-Tool([string]$exe, [string[]]$toolArgs) {
    Write-Host ">" $exe ($toolArgs -join ' ')
    & $exe @toolArgs
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

# Resources: manifest (Windows 8+ compatibility, needed for a layered child window; common controls 6).
$res = Join-Path $build 'resource.o'
Invoke-Tool 'windres' @('-I', $src, (Join-Path $src 'resource.rc'), '-O', 'coff', '-o', $res)

# The widget.
$exe = Join-Path $build 'ClaudeTaskbarUsage.exe'
$renderSrc = @('render.c', 'text_d2d.c') | ForEach-Object { Join-Path $src $_ }
$appSrc = $dataSrc + $renderSrc + (@('http.c', 'widget.c', 'main.c') | ForEach-Object { Join-Path $src $_ })
Invoke-Tool 'gcc' ($cflags + $appSrc + @($res, '-o', $exe, '-static', '-mwindows', '-s',
    '-lwinhttp', '-lcomctl32', '-lgdi32', '-luser32', '-ladvapi32', '-lshell32'))

# The tests (console program), with the renderer for the off-screen render tests.
$testExe = Join-Path $build 'ctu_tests.exe'
$testCflags = $cflags | Where-Object { $_ -ne '-municode' }
Invoke-Tool 'gcc' ($testCflags + $dataSrc + $renderSrc + @((Join-Path $tests 'test_main.c'), '-o', $testExe, '-static', '-s',
    '-lgdi32', '-luser32'))

Write-Host '> running tests'
& $testExe (Join-Path $tests 'samples') (Join-Path $build 'testdata')
if ($LASTEXITCODE -ne 0) { throw "tests failed (exit code $LASTEXITCODE)" }

$size = (Get-Item $exe).Length
Write-Host ("built {0} ({1:N0} bytes)" -f $exe, $size)
