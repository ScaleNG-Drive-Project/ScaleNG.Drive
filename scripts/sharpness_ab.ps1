<#
Sharpness A/B probe for the ScaleNG shadow-eval path.

Measures whether writing DLSS output into the presented backbuffer changes
measured image sharpness. Uses an OBJECTIVE metric (Laplacian variance +
mean gradient energy over a central crop) so the result does not depend on
someone's impression of "softer".

The two unknowns this separates:
  1. DLSS path softens the image (expected without jitter: DLAA has no
     subpixel samples to accumulate)  -> ON sharpness << OFF sharpness
  2. Softness comes from BeamNG's own AA, not from our injection
     -> ON sharpness ~= OFF sharpness

Why a script and not F8 by hand: adjacent ON/OFF captures share a camera
trajectory (idle camera drifts slowly), and the toggle is global-state
(GetAsyncKeyState) so the game does not need focus.

Usage:
  powershell -File scripts\sharpness_ab.ps1 -Cycles 4 -DelayMs 800

Requires the game already running with the ScaleNG ASI loaded.
#>
param(
    [int]$Cycles = 4,
    [int]$DelayMs = 800,
    [int]$SettleMs = 400,
    [string]$OutDir = (Join-Path $env:TEMP 'scaleng_ab')
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing, System.Windows.Forms, Microsoft.VisualBasic

Add-Type -ReferencedAssemblies System.Drawing, System.Windows.Forms @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class AbProbe
{
    [DllImport("user32.dll")]
    static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);

    public static void Tap(byte vk)
    {
        keybd_event(vk, 0, 0, UIntPtr.Zero);           // key down
        System.Threading.Thread.Sleep(60);
        keybd_event(vk, 0, 2, UIntPtr.Zero);           // key up
    }

    public static void Shot(string path)
    {
        var b = new Bitmap(System.Windows.Forms.Screen.PrimaryScreen.Bounds.Width,
                           System.Windows.Forms.Screen.PrimaryScreen.Bounds.Height);
        using (var g = Graphics.FromImage(b))
            g.CopyFromScreen(0, 0, 0, 0, b.Size);
        b.Save(path, ImageFormat.Png);
        b.Dispose();
    }

    // Laplacian variance + mean |gradient| over a central crop.
    // Laplacian is the standard focus/sharpness measure: it spikes on
    // edges, so blur lowers both metrics.
    // Mean luma of the central crop. Used to reject pairs where the camera
    // moved somewhere else entirely (loading screen vs gameplay) -- comparing
    // sharpness across different scenes is meaningless.
    public static double MeanLuma(string path)
    {
        double sum = 0; long n = 0;
        using (var src = new Bitmap(path))
        using (var bmp = new Bitmap(src.Width, src.Height, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(bmp))
        {
            g.DrawImage(src, 0, 0);
            var rect = new Rectangle(0, 0, bmp.Width, bmp.Height);
            var data = bmp.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try
            {
                int stride = data.Stride;
                var px = new byte[data.Stride * bmp.Height];
                Marshal.Copy(data.Scan0, px, 0, px.Length);
                int x0 = bmp.Width * 25 / 100, x1 = bmp.Width * 75 / 100;
                int y0 = bmp.Height * 15 / 100, y1 = bmp.Height * 85 / 100;
                for (int y = y0; y < y1; y += 2)
                {
                    int row = y * stride;
                    for (int x = x0; x < x1; x += 2)
                    {
                        int i = row + x * 4;
                        sum += px[i] * 0.299 + px[i + 1] * 0.587 + px[i + 2] * 0.114;
                        n++;
                    }
                }
            }
            finally { bmp.UnlockBits(data); }
        }
        return sum / n;
    }

    public static double[] Metrics(string path)
    {
        using (var src = new Bitmap(path))
        using (var bmp = new Bitmap(src.Width, src.Height, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(bmp))
        {
            g.DrawImage(src, 0, 0);
            var rect = new Rectangle(0, 0, bmp.Width, bmp.Height);
            var data = bmp.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try
            {
                int stride = data.Stride;
                var px = new byte[data.Stride * bmp.Height];
                Marshal.Copy(data.Scan0, px, 0, px.Length);

                // central crop: skip UI chrome, borders, letterboxing
                int x0 = bmp.Width * 25 / 100, x1 = bmp.Width * 75 / 100;
                int y0 = bmp.Height * 15 / 100, y1 = bmp.Height * 85 / 100;

                double sum = 0, sumSq = 0, sumGrad = 0;
                long n = 0;
                for (int y = y0 + 1; y < y1 - 1; y++)
                {
                    int row = y * stride;
                    for (int x = x0 + 1; x < x1 - 1; x++)
                    {
                        int i = row + x * 4;
                        // Rec.601 luma
                        float c = px[i] * 0.299f + px[i + 1] * 0.587f + px[i + 2] * 0.114f;
                        int j = i - 4, k = i + 4, l2 = i - stride, m = i + stride;
                        float cl = px[j] * 0.299f + px[j + 1] * 0.587f + px[j + 2] * 0.114f;
                        float cr = px[k] * 0.299f + px[k + 1] * 0.587f + px[k + 2] * 0.114f;
                        float cu = px[l2] * 0.299f + px[l2 + 1] * 0.587f + px[l2 + 2] * 0.114f;
                        float cd = px[m] * 0.299f + px[m + 1] * 0.587f + px[m + 2] * 0.114f;

                        double lap = 4.0 * c - cl - cr - cu - cd;
                        sum += lap; sumSq += lap * lap;
                        sumGrad += Math.Abs(cr - cl) + Math.Abs(cd - cu);
                        n++;
                    }
                }
                double mean = sum / n;
                double variance = sumSq / n - mean * mean;
                return new double[] { variance, sumGrad / n, n };
            }
            finally { bmp.UnlockBits(data); }
        }
    }
}
'@

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

# BeamNG spawns several processes (launcher, renderer, helpers). Only the
# windowed one is the game; AppActivate on the whole array silently targets
# the wrong process and captures whatever is foreground.
$procs = @(Get-Process -Name 'BeamNG.drive.x64' -ErrorAction SilentlyContinue)
if ($procs.Count -eq 0) { Write-Output 'ERROR: BeamNG is not running'; exit 2 }
$proc = $procs | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) {
    Write-Output ("ERROR: {0} BeamNG processes but none windowed (still loading?)" -f $procs.Count)
    exit 2
}
try { [Microsoft.VisualBasic.Interaction]::AppActivate($proc.Id) } catch { }
Start-Sleep -Milliseconds 800

Write-Output ("proc p{0} cycles={1} delay={2}ms out={3}" -f $proc.Id, $Cycles, $DelayMs, $OutDir)

$rows = @()
# Assumes handoff starts ON (steady default). Each cycle captures ON, taps F8,
# captures OFF, taps F8 back to ON -- pairs stay temporally adjacent.
for ($i = 0; $i -lt $Cycles; $i++) {
    Start-Sleep -Milliseconds $SettleMs
    $onPath = Join-Path $OutDir ("on_{0:d2}.png" -f $i)
    [AbProbe]::Shot($onPath)
    Start-Sleep -Milliseconds $DelayMs

    [AbProbe]::Tap(0x77)          # VK_F8
    Start-Sleep -Milliseconds $SettleMs
    $offPath = Join-Path $OutDir ("off_{0:d2}.png" -f $i)
    [AbProbe]::Shot($offPath)
    Start-Sleep -Milliseconds $DelayMs

    [AbProbe]::Tap(0x77)          # VK_F8 -> back to ON
    Start-Sleep -Milliseconds $DelayMs

    $on = [AbProbe]::Metrics($onPath)
    $off = [AbProbe]::Metrics($offPath)
    $onLuma = [AbProbe]::MeanLuma($onPath)
    $offLuma = [AbProbe]::MeanLuma($offPath)

    # Scene guard: if brightness differs hugely the camera is somewhere else
    # (loading screen, inside geometry) and the pair proves nothing. Flag it
    # instead of averaging it into the result.
    $lumaDelta = [math]::Abs($onLuma - $offLuma)
    $comparable = ($lumaDelta -lt 25.0) -and ($on[0] -gt 5.0) -and ($off[0] -gt 5.0)
    $why = ''
    if ($lumaDelta -ge 25.0) { $why = 'scene-mismatch' }
    elseif ($on[0] -le 5.0 -or $off[0] -le 5.0) { $why = 'blank-frame' }

    $rows += [pscustomobject]@{
        Cycle    = $i
        OnLapVar = [math]::Round($on[0], 1)
        OffLapVar = [math]::Round($off[0], 1)
        OnGrad   = [math]::Round($on[1], 3)
        OffGrad  = [math]::Round($off[1], 3)
        OnLuma   = [math]::Round($onLuma, 1)
        OffLuma  = [math]::Round($offLuma, 1)
        Usable   = $comparable
        Reason   = $why
    }
    $tag = if ($comparable) { '' } else { '  [DISCARD: ' + $why + ']' }
    Write-Output ("cycle {0}: ON lapvar={1} grad={2} luma={3} | OFF lapvar={4} grad={5} luma={6}{7}" -f `
        $i, $rows[-1].OnLapVar, $rows[-1].OnGrad, $rows[-1].OnLuma, `
        $rows[-1].OffLapVar, $rows[-1].OffGrad, $rows[-1].OffLuma, $tag)
}

$good = @($rows | Where-Object { $_.Usable })
Write-Output ''
Write-Output ("usable pairs: {0} of {1}" -f $good.Count, $rows.Count)
if ($good.Count -lt 2) {
    Write-Output 'RESULT: INCONCLUSIVE - too few comparable pairs (camera not holding still; scene changed between captures).'
    Write-Output ("shots: {0}" -f $OutDir)
    exit 3
}

$onLap = ($good | Measure-Object OnLapVar -Average).Average
$offLap = ($good | Measure-Object OffLapVar -Average).Average
$onGrd = ($good | Measure-Object OnGrad -Average).Average
$offGrd = ($good | Measure-Object OffGrad -Average).Average

Write-Output ''
Write-Output ('MEAN ON  lapvar={0:N1} grad={1:N3}' -f $onLap, $onGrd)
Write-Output ('MEAN OFF lapvar={0:N1} grad={1:N3}' -f $offLap, $offGrd)
$lapRatio = if ($offLap -ne 0) { $onLap / $offLap } else { 0 }
$grdRatio = if ($offGrd -ne 0) { $onGrd / $offGrd } else { 0 }
Write-Output ('RATIO ON/OFF lapvar={0:P1} grad={1:P1}' -f $lapRatio, $grdRatio)
if ($lapRatio -lt 0.90) {
    Write-Output 'READ: DLSS handoff measurably SOFTENS the image (expected: no jitter -> no subpixel accumulation).'
} elseif ($lapRatio -gt 1.10) {
    Write-Output 'READ: DLSS handoff measurably SHARPENS the image.'
} else {
    Write-Output 'READ: No measurable sharpness delta from the DLSS handoff.'
}
Write-Output ("shots: {0}" -f $OutDir)