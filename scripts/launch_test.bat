@echo off
setlocal
rem Convenience entry point; forwards all options to the maintained Python runner.
set "PYTHON=%~dp0..\.venv-test\Scripts\python.exe"
if not exist "%PYTHON%" set "PYTHON=py"
"%PYTHON%" "%~dp0autonomous_test.py" %*
exit /b %errorlevel%
