# Direct pixel comparison of two captures. A sharpness RATIO cannot tell
# "DLSS output is near-identity" apart from "the handoff never reached the
# screen" -- both look like ratio ~1.00. Exact differing-pixel count can.
param(
    [string]$A,
    [string]$B,
    [double]$CropPct = 0.15
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Compare-Pair([string]$pa, [string]$pb) {
    if (-not (Test-Path $pa) -or -not (Test-Path $pb)) { Write-Output "MISSING $pa / $pb"; return }
    $ia = New-Object Drawing.Bitmap $pa
    $ib = New-Object Drawing.Bitmap $pb
    $w = [Math]::Min($ia.Width, $ib.Width)
    $h = [Math]::Min($ia.Height, $ib.Height)
    $x0 = [int]($w * $CropPct); $x1 = [int]($w * (1 - $CropPct))
    $y0 = [int]($h * $CropPct); $y1 = [int]($h * (1 - $CropPct))

    $diff = 0; $total = 0; [double]$absSum = 0; [int]$maxDelta = 0
    $rect = New-Object Drawing.Rectangle 0, 0, $w, $h
    $la = $ia.LockBits($rect, [Drawing.Imaging.ImageLockMode]::ReadOnly, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $lb = $ib.LockBits($rect, [Drawing.Imaging.ImageLockMode]::ReadOnly, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $ba = New-Object byte[] ($la.Stride * $h)
        $bb = New-Object byte[] ($lb.Stride * $h)
        [Runtime.InteropServices.Marshal]::Copy($la.Scan0, $ba, 0, $ba.Length)
        [Runtime.InteropServices.Marshal]::Copy($lb.Scan0, $bb, 0, $bb.Length)
        for ($y = $y0; $y -lt $y1; $y += 1) {
            $row = $y * $la.Stride
            for ($x = $x0; $x -lt $x1; $x += 1) {
                $i = $row + $x * 4
                $d = [Math]::Abs([int]$ba[$i] - [int]$bb[$i])
                if ($d -gt 0) { $diff++; $absSum += $d; if ($d -gt $maxDelta) { $maxDelta = $d } }
                $total++
            }
        }
    } finally {
        $ia.UnlockBits($la); $ib.UnlockBits($lb); $ia.Dispose(); $ib.Dispose()
    }
    $pct = 100.0 * $diff / $total
    $mean = if ($diff -gt 0) { $absSum / $diff } else { 0 }
    $name = ('A={0} B={1}' -f [IO.Path]::GetFileName($pa), [IO.Path]::GetFileName($pb))
    Write-Output ("{0} differing={1}/{2} ({3:N3}%) meanDelta={4:N2} maxDelta={5}" -f `
        $name, $diff, $total, $pct, $mean, $maxDelta)
}

Compare-Pair $A $B