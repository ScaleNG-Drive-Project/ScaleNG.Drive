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
    echo "%SRC%main.cpp"
    echo "%SRC%camera_cb.cpp"
    echo "%SRC%d3d12_hooks.cpp"
    echo "%SRC%dlss_ngx.cpp"
    echo "%SRC%vendor\minhook\src\buffer.c"
    echo "%SRC%vendor\minhook\src\hook.c"
    echo "%SRC%vendor\minhook\src\trampoline.c"
    echo "%SRC%vendor\minhook\src\hde\hde64.c"
    echo user32.lib
    echo /link /MAP:"%OUT%\ScaleNG.map" /DEBUG
    echo /Fe:"%OUT%\ScaleNG.dll"
) > "%RSP%"

cl @"%RSP%"
if errorlevel 1 (
    echo [ERROR] Compilation failed. Fix the errors above and re-run.
    del "%RSP%" >nul 2>&1
    exit /b 1
)

del "%RSP%" >nul 2>&1

move /y "%OUT%\ScaleNG.dll" "%OUT%\ScaleNG.asi" >nul
del /q "%OUT%\ScaleNG.exp" "%OUT%\ScaleNG.lib" >nul 2>&1
if exist "%OUT%\ScaleNG.asi" (
    echo [OK] Built %OUT%\ScaleNG.asi
) else (
    echo [ERROR] Output rename failed.
    exit /b 1
)

rem ---- NGX helper exe (cross-process bridge worker) ----
(
    echo /nologo /O2 /EHsc /std:c++17 /MT /Zi /D_CRT_SECURE_NO_WARNINGS
    echo /Fo"%OBJDIR%/helper_"
    echo "%SRC%ngxc_helper.cpp"
    echo user32.lib shell32.lib advapi32.lib
    echo /link /DEBUG
    echo /Fe:"%OUT%\ScaleNG_NGX_helper.exe"
) > "%TEMP%\ScaleNG_helper.rsp"
cl @"%TEMP%\ScaleNG_helper.rsp"
if errorlevel 1 (
    echo [ERROR] Helper compilation failed.
    del "%TEMP%\ScaleNG_helper.rsp" >nul 2>&1
    exit /b 1
)
del /q "%OBJDIR%\helper_*" >nul 2>&1
del "%TEMP%\ScaleNG_helper.rsp" >nul 2>&1
if exist "%OUT%\ScaleNG_NGX_helper.exe" (
    echo [OK] Built %OUT%\ScaleNG_NGX_helper.exe
) else (
    echo [ERROR] Helper output missing.
    exit /b 1
)