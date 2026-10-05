@echo off
setlocal

set SCRIPT_DIR=%~dp0

where pwisp.exe >NUL 2>NUL
if %ERRORLEVEL% EQU 0 (
    pwisp.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build-and-run-wisp.ps1" %*
    exit /B %ERRORLEVEL%
)

where powershell.exe >NUL 2>NUL
if %ERRORLEVEL% EQU 0 (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build-and-run-wisp.ps1" %*
    exit /B %ERRORLEVEL%
)

echo PowerShell was not found on PATH.
exit /B 1
