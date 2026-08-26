@echo off
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCDIR=%%i"
call "%VCDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /O2 /EHsc /MT /D_CRT_SECURE_NO_WARNINGS dmpscan.cpp /Fe:"..\dist\dmpscan.exe" dbghelp.lib