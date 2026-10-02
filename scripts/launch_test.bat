@echo off
setlocal

:: ============================================================================
:: ScaleNG.Drive — Autonomous Launch Script (DXGI Proxy Architecture)
:: Builds, deploys, launches BeamNG directly into level, monitors ScaleNG.log
:: ============================================================================

:: ==== CONFIGURATION ====
set "BEAMNG_EXE=C:\games\BeamNG.drive\Bin64\BeamNG.drive.x64.exe"
set "BIN64_DIR=C:\games\BeamNG.drive\Bin64"
set "LEVEL=GridMap"
set "VEHICLE=pickup"
set "EXTRA_ARGS=-console -gfx d3d12"
set "TEST_DURATION_SEC=320"

:: ==== VERIFY PATHS ====
if not exist "%BEAMNG_EXE%" (
    echo [ERROR] BeamNG not found at %BEAMNG_EXE%
    exit /b 1
)
if not exist "%BIN64_DIR%" (
    echo [ERROR] Bin64 dir not found at %BIN64_DIR%
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

:: ==== DEPLOY (dxgi.dll proxy + config + nvngx_dlss.dll to Bin64) ====
echo [DEPLOY] Copying to %BIN64_DIR%...
copy /y "..\dist\dxgi.dll" "%BIN64_DIR%\" >nul
copy /y "..\dist\dxgi.ini" "%BIN64_DIR%\" >nul
copy /y "..\dist\nvngx_dlss.dll" "%BIN64_DIR%\" >nul
echo [DEPLOY] Done

:: ==== LAUNCH ====
cd /d "%BIN64_DIR%"
echo [LAUNCH] Starting BeamNG: %LEVEL% %VEHICLE% (duration: %TEST_DURATION_SEC%s)
start "" "%BEAMNG_EXE%" -level %LEVEL% -vehicle %VEHICLE% %EXTRA_ARGS%

:: ==== MONITOR LOG ====
echo [MONITOR] Waiting for ScaleNG.log to appear...
set "LOG_PATH=%BIN64_DIR%\ScaleNG.log"
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
    "  if ($line -like '*NGX] Initialized successfully*') { Write-Host '[SUCCESS] NGX initialized!' -ForegroundColor Green } " ^
    "  if ($line -like '*NGX] Feature created*') { Write-Host '[SUCCESS] NGX feature created!' -ForegroundColor Green } " ^
    "  if ($line -like '*NGX] Evaluated frame*') { Write-Host '[SUCCESS] NGX evaluation active!' -ForegroundColor Green } " ^
    "  if ($line -like '*Resource] Found DEPTH resource*') { Write-Host '[SUCCESS] Depth resource found!' -ForegroundColor Green } " ^
    "  if ($line -like '*Resource] Found MOTION VECTOR resource*') { Write-Host '[SUCCESS] MV resource found!' -ForegroundColor Green } " ^
    "  if ($line -like '*Resource] Found COLOR resource*') { Write-Host '[SUCCESS] Color resource found!' -ForegroundColor Green } " ^
    "  if ($line -like '*NGX] Evaluate failed*') { Write-Host '[FAIL] NGX evaluate failed' -ForegroundColor Red } " ^
    "  if ($line -like '*NGX] NVSDK_NGX_CreateFeature failed*') { Write-Host '[FAIL] NGX feature creation failed' -ForegroundColor Red } " ^
    "  if ($line -like '*DXGI] Failed to hook*') { Write-Host '[FAIL] DXGI hook failed' -ForegroundColor Red } " ^
    "  if ($line -like '*device removed*') { Write-Host '[CRITICAL] Device removed!' -ForegroundColor Red } " ^
    "}" 2>&1

echo [MONITOR] Test duration completed.
echo [DONE] Check BeamNG window for visual confirmation (DLSS active, FPS overlay, image quality).
echo [DONE] Press any key to exit...
pause