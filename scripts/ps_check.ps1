$err = $null
$null = [System.Management.Automation.Language.Parser]::ParseFile(
    'C:\AI\ScaleNG_Workspace\ScaleNG.Drive\scripts\sharpness_ab.ps1', [ref]$null, [ref]$err)
if ($err.Count -gt 0) { $err | ForEach-Object { 'ERR: ' + $_.Message } } else { 'ps-parse-ok' }