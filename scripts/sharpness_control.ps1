# Null control for the sharpness A/B.
#
# The A/B pairs are captured ~600ms apart while the camera is live, so they
# differ from scene motion alone. Without a same-delay NO-TOGGLE pair we
# cannot tell a DLSS effect from ordinary frame-to-frame drift. This captures
# pairs identically but never presses F8, giving the baseline.
param(
    [int]$Pairs = 4,
    [int]$DelayMs = 600,
    [string]$OutDir = (Join-Path $env:TEMP 'scaleng_ctrl')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing, System.Windows.Forms

function New-Shot([string]$path) {
    $b = New-Object Drawing.Bitmap([Windows.Forms.Screen]::PrimaryScreen.Bounds.Width,
                                   [Windows.Forms.Screen]::PrimaryScreen.Bounds.Height)
    $g = [Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen(0, 0, 0, 0, $b.Size)
    $g.Dispose()
    $b.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    $b.Dispose()
}

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
$procs = @(Get-Process -Name 'BeamNG.drive.x64' -ErrorAction SilentlyContinue)
if ($procs.Count -eq 0) { Write-Output 'ERROR: BeamNG not running'; exit 2 }
$proc = $procs | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { Write-Output 'ERROR: no windowed BeamNG process'; exit 2 }

Write-Output ("CONTROL proc p{0} pairs={1} delay={2}ms (no toggling)" -f $proc.Id, $Pairs, $DelayMs)
for ($i = 0; $i -lt $Pairs; $i++) {
    $a = Join-Path $OutDir ("ctrl_{0:d2}_a.png" -f $i)
    New-Shot $a
    Start-Sleep -Milliseconds $DelayMs
    $b = Join-Path $OutDir ("ctrl_{0:d2}_b.png" -f $i)
    New-Shot $b
    Write-Output ("control pair {0} captured" -f $i)
}
Write-Output ("shots: {0}" -f $OutDir)