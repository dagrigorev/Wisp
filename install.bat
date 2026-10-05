@echo off
setlocal EnableDelayedExpansion

echo ================================================================
echo   Wisp Terminal Emulator — Installer
echo ================================================================
echo.

:: Check if Wisp.exe exists
if not exist "Wisp.exe" (
    echo ERROR: Wisp.exe not found in current directory.
    echo Please run this script from the directory containing Wisp.exe.
    echo Build first with: nmake
    pause
    exit /b 1
)

:: Set install destination
set "DEST=%LOCALAPPDATA%\Wisp"
echo Installing to: %DEST%
echo.

:: Create directory
if not exist "%DEST%" (
    mkdir "%DEST%"
    if errorlevel 1 (
        echo ERROR: Could not create %DEST%
        pause
        exit /b 1
    )
)

:: Copy executable
copy /Y "Wisp.exe" "%DEST%\Wisp.exe" >nul
if errorlevel 1 (
    echo ERROR: Failed to copy Wisp.exe to %DEST%
    pause
    exit /b 1
)
echo [OK] Copied Wisp.exe to %DEST%

:: Register in App Paths so it can be found by name
reg add "HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Wisp.exe" ^
    /ve /d "%DEST%\Wisp.exe" /f >nul
if errorlevel 1 (
    echo [WARN] Could not register App Paths entry.
) else (
    echo [OK] Registered App Paths entry.
)

:: Set Cascadia Code as console font
reg add "HKCU\Console" /v FaceName /t REG_SZ /d "Cascadia Code" /f >nul 2>&1
echo [OK] Set preferred console font.

:: Add "Open Wisp here" to Explorer background context menu
reg add "HKCU\SOFTWARE\Classes\Directory\Background\shell\Wisp" ^
    /ve /d "Open Wisp here" /f >nul
reg add "HKCU\SOFTWARE\Classes\Directory\Background\shell\Wisp" ^
    /v "Icon" /d "%DEST%\Wisp.exe,0" /f >nul
reg add "HKCU\SOFTWARE\Classes\Directory\Background\shell\Wisp\command" ^
    /ve /d "\"%DEST%\Wisp.exe\" --cwd \"%%V\"" /f >nul
echo [OK] Added Explorer context menu: "Open Wisp here" (background).

:: Add "Open Wisp here" to Explorer folder context menu
reg add "HKCU\SOFTWARE\Classes\Directory\shell\Wisp" ^
    /ve /d "Open Wisp here" /f >nul
reg add "HKCU\SOFTWARE\Classes\Directory\shell\Wisp" ^
    /v "Icon" /d "%DEST%\Wisp.exe,0" /f >nul
reg add "HKCU\SOFTWARE\Classes\Directory\shell\Wisp\command" ^
    /ve /d "\"%DEST%\Wisp.exe\" --cwd \"%%1\"" /f >nul
echo [OK] Added Explorer context menu: "Open Wisp here" (folder).

:: Optionally create a Desktop shortcut
set /p CREATE_SHORTCUT="Create Desktop shortcut? [Y/N]: "
if /i "%CREATE_SHORTCUT%"=="Y" (
    powershell -NoProfile -Command ^
        "$s=(New-Object -COM WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Desktop')+'\Wisp.lnk');" ^
        "$s.TargetPath='%DEST%\Wisp.exe';" ^
        "$s.WorkingDirectory='%DEST%';" ^
        "$s.Description='Wisp Terminal Emulator';" ^
        "$s.Save()" >nul 2>&1
    echo [OK] Created Desktop shortcut.
)

:: Optionally add to PATH
set /p ADD_PATH="Add %DEST% to user PATH? [Y/N]: "
if /i "%ADD_PATH%"=="Y" (
    for /f "tokens=2*" %%A in ('reg query "HKCU\Environment" /v PATH 2^>nul') do set "CURRENT_PATH=%%B"
    if "!CURRENT_PATH!"=="" (
        reg add "HKCU\Environment" /v PATH /t REG_EXPAND_SZ /d "%DEST%" /f >nul
    ) else (
        reg add "HKCU\Environment" /v PATH /t REG_EXPAND_SZ /d "!CURRENT_PATH!;%DEST%" /f >nul
    )
    echo [OK] Added %DEST% to user PATH.
    echo NOTE: You may need to restart applications to pick up the new PATH.
)

echo.
echo ================================================================
echo   Installation complete!
echo.
echo   You can now:
echo     - Run Wisp from Start menu (search "Wisp")
echo     - Right-click any folder in Explorer to "Open Wisp here"
echo     - Type 'wisp' in any terminal (if added to PATH)
echo ================================================================
echo.
pause
