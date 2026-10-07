# F9 toggle verification: presses F9 twice mid-run (REAL on, then off).
# Run AFTER the game reaches gameplay. Exits after keypresses; the game run
# continues under launch_test.
param([int]$FirstS = 75, [int]$GapS = 30)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, Microsoft.VisualBasic
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Keys {
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static void Tap(byte vk) {
        keybd_event(vk, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(80);
        keybd_event(vk, 0, 2, UIntPtr.Zero);
    }
}
'@

$procs = @(Get-Process -Name 'BeamNG.drive.x64' -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 })
if ($procs.Count -eq 0) { Write-Output 'ERROR: no windowed BeamNG'; exit 2 }
$proc = $procs[0]
Write-Output ("target p{0}, first tap in {1}s" -f $proc.Id, $FirstS)
Start-Sleep -Seconds $FirstS
try { [Microsoft.VisualBasic.Interaction]::AppActivate($proc.Id) } catch { }
Start-Sleep -Milliseconds 800
[Keys]::Tap(0x78)
Write-Output 'F9 #1 sent (expect inputs REAL)'
Start-Sleep -Seconds $GapS
$procs2 = @(Get-Process -Name 'BeamNG.drive.x64' -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 })
if ($procs2.Count -eq 0) { Write-Output 'ERROR: game gone before F9 #2'; exit 3 }
try { [Microsoft.VisualBasic.Interaction]::AppActivate($procs2[0].Id) } catch { }
Start-Sleep -Milliseconds 800
[Keys]::Tap(0x78)
Write-Output 'F9 #2 sent (expect inputs ZERO)'