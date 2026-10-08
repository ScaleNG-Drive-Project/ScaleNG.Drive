@echo off
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"
if not exist ".venv-test\Scripts\python.exe" py -3 -m venv .venv-test
if errorlevel 1 exit /b %errorlevel%
".venv-test\Scripts\python.exe" -m pip install --no-deps beamngpy==1.35.1
if errorlevel 1 exit /b %errorlevel%
".venv-test\Scripts\python.exe" -m pip install -r scripts\requirements-test.txt
if errorlevel 1 exit /b %errorlevel%
popd
echo Test environment ready. Run scripts\launch_test.bat.
