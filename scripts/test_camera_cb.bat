@echo off
setlocal
set "ROOT=%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] Visual Studio Installer not found.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCDIR=%%i"
if not defined VCDIR (
    echo [ERROR] MSVC C++ tools not found.
    exit /b 1
)
call "%VCDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b %errorlevel%

set "TEST_EXE=%TEMP%\ScaleNGCameraCbValidationTest-%RANDOM%.exe"
pushd "%TEMP%"
cl /nologo /std:c++17 /EHsc /I"%ROOT%\src" "%ROOT%\tests\camera_cb_validation_test.cpp" "%ROOT%\src\camera_cb.cpp" /Fe:"%TEST_EXE%"
if errorlevel 1 goto compile_failed
"%TEST_EXE%"
set "TEST_RESULT=%errorlevel%"
del "%TEST_EXE%" >nul 2>&1
popd
exit /b %TEST_RESULT%

:compile_failed
set "TEST_RESULT=%errorlevel%"
popd
exit /b %TEST_RESULT%
