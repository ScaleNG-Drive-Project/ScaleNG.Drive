$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-TestImage([string]$path, [bool]$blurred) {
    $bmp = New-Object Drawing.Bitmap 200, 120
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.Clear([Drawing.Color]::Black)
    $pen = if ($blurred) { [Drawing.Pens]::DarkGray } else { [Drawing.Pens]::White }
    for ($x = 10; $x -lt 190; $x += 8) { $g.DrawLine($pen, $x, 20, $x, 100) }
    $g.Dispose()
    $bmp.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# Load the metric implementation out of sharpness_ab.ps1 without running it.
$src = Get-Content 'C:\AI\ScaleNG_Workspace\ScaleNG.Drive\scripts\sharpness_ab.ps1' -Raw
$start = $src.IndexOf('Add-Type -ReferencedAssemblies System.Drawing @''')
$open = $src.IndexOf("`n", $start) + 1
$close = $src.IndexOf("'@", $open)
$code = $src.Substring($open, $close - $open)
Add-Type -ReferencedAssemblies System.Drawing, System.Windows.Forms $code
Write-Output 'metric-class-loaded'

$sharp = Join-Path $env:TEMP 'ab_selftest_sharp.png'
$soft  = Join-Path $env:TEMP 'ab_selftest_soft.png'
New-TestImage $sharp $false
New-TestImage $soft $true

$a = [AbProbe]::Metrics($sharp)
$b = [AbProbe]::Metrics($soft)
Write-Output ('sharp lapvar={0:N2} grad={1:N3} n={2}' -f $a[0], $a[1], $a[2])
Write-Output ('soft  lapvar={0:N2} grad={1:N3} n={2}' -f $b[0], $b[1], $b[2])
if ($a[0] -gt $b[0] -and $a[1] -gt $b[1]) { Write-Output 'SELFTEST PASS: sharper image scores higher on both metrics' }
else { Write-Output 'SELFTEST FAIL: metric does not rank sharp above soft'; exit 1 }