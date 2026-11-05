<#
.SYNOPSIS
Checks a PC-decoded frame from "encoder-bench --dump" against the pixel band pattern: does the encoder keep every pixel?

.DESCRIPTION
Not for phone screenshots: screencap recomposes through the GPU, which resamples cropped YUV buffers even when the panel
itself shows them 1:1. On the phone, the "phone compositor" line of "run" is the check, and fine lines without moire by eye.
#>
param(
    [Parameter(Mandatory)] [string] $Screenshot,
    [Parameter(Mandatory)] [int] $Width,
    [Parameter(Mandatory)] [int] $Height
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$bandRows = 64
$bright = 150
$dark = 105

# Lock the pixels once; GetPixel is far too slow for a whole-frame check.
$image = [Drawing.Bitmap]::FromFile($Screenshot)
try {
    if ($image.Width -ne $Width -or $image.Height -ne $Height) {
        Write-Output "FAIL screenshot is $($image.Width)x$($image.Height), expected ${Width}x$Height"
        exit 1
    }
    $rect = New-Object Drawing.Rectangle 0, 0, $Width, $Height
    $data = $image.LockBits($rect, [Drawing.Imaging.ImageLockMode]::ReadOnly, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $bytes = New-Object byte[] ($stride * $Height)
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $image.UnlockBits($data)
} finally {
    $image.Dispose()
}

function Get-Luma([int]$x, [int]$y) {
    $offset = $y * $stride + $x * 4
    return (0.0722 * $bytes[$offset] + 0.7152 * $bytes[$offset + 1] + 0.2126 * $bytes[$offset + 2])
}

$failed = $false

# Vertical one-pixel lines: every column must alternate. Scaling of even one pixel breaks this somewhere across the width.
$verticalMismatches = 0
$row = [int]($bandRows / 2)
for ($x = 2; $x -lt $Width - 2; $x++) {
    $luma = Get-Luma $x $row
    $expectBright = ($x % 2) -eq 0
    if (($expectBright -and $luma -lt $bright) -or (-not $expectBright -and $luma -gt $dark)) { $verticalMismatches++ }
}
$verticalPercent = 100.0 * $verticalMismatches / ($Width - 4)
Write-Output ('vertical one-pixel lines: {0} of {1} columns wrong ({2:N2}%)' -f $verticalMismatches, ($Width - 4), $verticalPercent)
if ($verticalPercent -gt 1) { $failed = $true }

$horizontalMismatches = 0
$column = [int]($Width / 2)
for ($y = $bandRows + 2; $y -lt 2 * $bandRows - 2; $y++) {
    $luma = Get-Luma $column $y
    $expectBright = ($y % 2) -eq 0
    if (($expectBright -and $luma -lt $bright) -or (-not $expectBright -and $luma -gt $dark)) { $horizontalMismatches++ }
}
Write-Output "horizontal one-pixel lines: $horizontalMismatches of $($bandRows - 4) rows wrong"
if ($horizontalMismatches -gt 1) { $failed = $true }

# The white one-pixel border must land exactly on the outermost pixels, with nothing cropped.
$borders = @{
    left   = Get-Luma 0 ([int]($Height * 0.75))
    right  = Get-Luma ($Width - 1) ([int]($Height * 0.75))
    bottom = Get-Luma ([int]($Width / 2)) ($Height - 1)
}
foreach ($side in $borders.Keys) {
    Write-Output ('border {0}: luma {1:N0}' -f $side, $borders[$side])
    if ($borders[$side] -lt $bright) { $failed = $true }
}
$inside = Get-Luma 1 ([int]($Height * 0.75))
Write-Output ('one pixel inside the left border: luma {0:N0}' -f $inside)
if ($inside -gt $dark) { $failed = $true }

if ($failed) { Write-Output 'FAIL pixels are not 1:1'; exit 1 }
Write-Output 'PASS pixels are 1:1'
