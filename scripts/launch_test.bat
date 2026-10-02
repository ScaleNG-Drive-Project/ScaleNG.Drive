@echo off
setlocal

:: ============================================================================
:: ScaleNG.Drive — Autonomous Launch Script
:: Builds, deploys, launches BeamNG directly into level, monitors ScaleNG.log
:: ============================================================================

:: ==== CONFIGURATION ====
set "BEAMNG_EXE=C:\games\BeamNG.drive\Bin64\BeamNG.drive.x64.exe"
set "PLUGINS_DIR=C:\games\BeamNG.drive\Bin64\plugins"
set "LEVEL=smallgrid"
set "VEHICLE="
set "EXTRA_ARGS=-console -gfx d3d12"
set "TEST_DURATION_SEC=320"

:: ==== VERIFY PATHS ====
if not exist "%BEAMNG_EXE%" (
    echo [ERROR] BeamNG not found at %BEAMNG_EXE%
    exit /b 1
)
if not exist "%PLUGINS_DIR%" (
    echo [ERROR] Plugins dir not found at %PLUGINS_DIR%
    exit /b 1
)

:: ==== BUILD ====
echo [BUILD] Running src/build.bat...
cd /d "%~dp0..\src"
call build.bat
if errorlevel 1 (
    echo [ERROR] Build failed
    exit /b 1
)
echo [BUILD] Success

:: ==== DEPLOY ====
echo [DEPLOY] Copying to %PLUGINS_DIR%...
copy /y "..\dist\ScaleNG.asi" "%PLUGINS_DIR%\" >nul
copy /y "..\dist\ScaleNG.ini" "%PLUGINS_DIR%\" >nul
copy /y "..\dist\ScaleNG_NGX_helper.exe" "%PLUGINS_DIR%\" >nul
copy /y "..\dist\nvngx_dlss.dll" "%PLUGINS_DIR%\" >nul
echo [DEPLOY] Done

:: ==== LAUNCH ====
cd /d "%PLUGINS_DIR%\.."
echo [LAUNCH] Starting BeamNG: %LEVEL% %VEHICLE% (duration: %TEST_DURATION_SEC%s)
start "" "%BEAMNG_EXE%" -level %LEVEL% -vehicle %VEHICLE% %EXTRA_ARGS%

:: ==== MONITOR LOG ====
echo [MONITOR] Waiting for ScaleNG.log to appear...
set "LOG_PATH=%PLUGINS_DIR%\ScaleNG.log"
set "WAIT_COUNT=0"
:WAIT_LOG
if not exist "%LOG_PATH%" (
    timeout /t 2 >nul
    set /a WAIT_COUNT+=1
    if %WAIT_COUNT% gtr 60 (
        echo [ERROR] ScaleNG.log never appeared after 120s
        exit /b 1
    )
    goto WAIT_LOG
)
echo [MONITOR] ScaleNG.log found. Tailing for %TEST_DURATION_SEC% seconds...

:: Use PowerShell to tail with timestamps and filter for key markers
powershell -NoProfile -Command ^
    "$log = '%LOG_PATH%'; $duration = %TEST_DURATION_SEC%; $start = Get-Date; " ^
    "Write-Host '[MONITOR] Tailing started at' (Get-Date -Format 'HH:mm:ss'); " ^
    "Get-Content $log -Wait -Tail 0 | ForEach-Object { " ^
    "  $now = Get-Date; $elapsed = ($now - $start).TotalSeconds; " ^
    "  if ($elapsed -gt $duration) { exit } " ^
    "  $line = $_; " ^
    "  Write-Host ('[{0:HH:mm:ss} +{1:F1}s] {2}' -f $now, $elapsed, $line); " ^
    "  if ($line -like '*DLSS: feature created*') { Write-Host '[SUCCESS] NGX feature created!' -ForegroundColor Green } " ^
    "  if ($line -like '*DLSS injection recorded*') { Write-Host '[SUCCESS] DLSS injection active!' -ForegroundColor Green } " ^
    "  if ($line -like '*EvaluateFeature failed*') { Write-Host '[FAIL] DLSS evaluate failed' -ForegroundColor Red } " ^
    "  if ($line -like '*injection skipped*') { Write-Host '[WARN] Injection skipped' -ForegroundColor Yellow } " ^
    "  if ($line -like '*device removed*') { Write-Host '[CRITICAL] Device removed!' -ForegroundColor Red } " ^
    "}" 2>&1

echo [MONITOR] Test duration completed.
echo [DONE] Check BeamNG window for visual confirmation (FPS overlay, image quality).