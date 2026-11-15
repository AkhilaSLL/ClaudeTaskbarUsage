# Captures a part of the primary screen to a PNG (test aid for the visual check).
# Usage: capture.ps1 -Out <png> [-X 1700] [-Y 1380] [-W 860] [-H 60] [-Scale 2]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$X = 1700,
    [int]$Y = 1380,
    [int]$W = 860,
    [int]$H = 60,
    [int]$Scale = 1
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class CtuDpi {
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
}
"@
[CtuDpi]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$bmp = New-Object System.Drawing.Bitmap $W, $H
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size $W, $H))
$g.Dispose()
if ($Scale -gt 1) {
    $big = New-Object System.Drawing.Bitmap ($W * $Scale), ($H * $Scale)
    $g2 = [System.Drawing.Graphics]::FromImage($big)
    $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $g2.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $g2.DrawImage($bmp, 0, 0, ($W * $Scale), ($H * $Scale))
    $g2.Dispose()
    $bmp.Dispose()
    $bmp = $big
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "saved $Out"
