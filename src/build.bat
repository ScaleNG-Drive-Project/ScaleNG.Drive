@echo off
setlocal

rem Self-contained: always run from this folder so .obj files stay inside src\build
cd /d "%~dp0"

set "SRC=%~dp0"
set "OUT=%~dp0..\dist"
set "OBJDIR=%~dp0build"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

if not exist "%VSWHERE%" (
    echo [ERROR] Visual Studio Installer not found.
    echo Install "Visual Studio Build Tools 2022" from https://visualstudio.microsoft.com/downloads/ and re-run.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCDIR=%%i"

if not defined VCDIR (
    echo [ERROR] MSVC C++ tools not found.
    echo In the Build Tools installer, select the "Desktop development with C++" workload, then re-run.
    exit /b 1
)

call "%VCDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed to initialize.
    exit /b 1
)

set "RSP=%TEMP%\ScaleNG_build.rsp"

(
    echo /nologo /O2 /EHa /std:c++17 /LD /MT /Zi /D_CRT_SECURE_NO_WARNINGS
    echo /Fo"%OBJDIR%/"
    echo /I"%SRC%"
    echo /I"%SRC%vendor\minhook\include"
    echo /I"%SRC%vendor\minhook\src"
    echo /I"%SRC%vendor\nvngx"
    echo "%SRC%main_dxgi.cpp"
    echo "%SRC%dxgi_hooks.cpp"
    echo "%SRC%events.cpp"
    echo "%SRC%resource_tracker.cpp"
    echo "%SRC%ngx_evaluator.cpp"
    echo "%SRC%present_evaluator.cpp"
    echo "%SRC%vendor\minhook\src\buffer.c"
    echo "%SRC%vendor\minhook\src\hook.c"
    echo "%SRC%vendor\minhook\src\trampoline.c"
    echo "%SRC%vendor\minhook\src\hde\hde64.c"
    echo user32.lib shell32.lib advapi32.lib dxgi.lib d3d12.lib shlwapi.lib
    echo /link /MAP:"%OUT%\dxgi.map" /DEBUG
    echo /Fe:"%OUT%\dxgi.dll"
) > "%RSP%"

cl @"%RSP%"
if errorlevel 1 (
    echo [ERROR] Compilation failed. Fix the errors above and re-run.
    del "%RSP%" >nul 2>&1
    exit /b 1
)

del "%RSP%" >nul 2>&1

if exist "%OUT%\dxgi.dll" (
    echo [OK] Built %OUT%\dxgi.dll
) else (
    echo [ERROR] Output missing.
    exit /b 1
)

rem ---- Copy nvngx_dlss.dll to dist folder ----
set "DLSS_SNIPPET=%SRC%..\dist\nvngx_dlss.dll"

if exist "%DLSS_SNIPPET%" (
    if /I "%DLSS_SNIPPET%"=="%OUT%\nvngx_dlss.dll" (
        echo [OK] nvngx_dlss.dll already present
    ) else (
        copy /y "%DLSS_SNIPPET%" "%OUT%\nvngx_dlss.dll" >nul
        if errorlevel 1 (
            echo [ERROR] Could not package nvngx_dlss.dll.
            exit /b 1
        )
        echo [OK] Packaged %OUT%\nvngx_dlss.dll
    )
) else (
    echo [ERROR] Validated nvngx_dlss.dll not found.
    exit /b 1
)

rem ---- Create default dxgi.ini ----
set "INI=%OUT%\dxgi.ini"
if not exist "%INI%" (
    echo [ScaleNG] > "%INI%"
    echo enabled=1 >> "%INI%"
    echo dlaa=1 >> "%INI%"
    echo render_scale=67 >> "%INI%"
    echo sharpness=0 >> "%INI%"
    echo perf_quality=1 >> "%INI%"
    echo mv_jittered=1 >> "%INI%"
    echo auto_exposure=1 >> "%INI%"
    echo app_id=241534720 >> "%INI%"
    echo dlss_dll_path= >> "%INI%"
    echo log_path= >> "%INI%"
    echo [OK] Created default %INI%
)

echo.
echo ============================================================
echo ScaleNG.Drive DXGI Proxy Build Complete
echo ============================================================
echo Output: %OUT%\dxgi.dll
echo Config: %OUT%\dxgi.ini
echo DLSS:   %OUT%\nvngx_dlss.dll
echo.
echo To deploy to BeamNG.drive:
echo   copy %OUT%\dxgi.dll "C:\games\BeamNG.drive\Bin64\dxgi.dll"
echo   copy %OUT%\dxgi.ini "C:\games\BeamNG.drive\Bin64\dxgi.ini"
echo   copy %OUT%\nvngx_dlss.dll "C:\games\BeamNG.drive\Bin64\nvngx_dlss.dll"
echo.
echo Then launch BeamNG with: -level GridMap -vehicle pickup -console -gfx d3d12
echo ============================================================