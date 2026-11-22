@echo off
setlocal

echo ========================================================
echo BareStats Windows Uninstallation
echo ========================================================

:: Check if BareStats.exe or barestats.exe is running
set "RUNNING=0"
tasklist /FI "IMAGENAME eq BareStats.exe" 2>nul | find /I /N "BareStats.exe" >nul && set "RUNNING=1"
tasklist /FI "IMAGENAME eq barestats.exe" 2>nul | find /I /N "barestats.exe" >nul && set "RUNNING=1"

if "%RUNNING%"=="1" (
    echo BareStats is currently running.
    set /p "CHOICE=Terminate running BareStats process before uninstalling? [Y/N]: "
    if /I "%CHOICE%"=="Y" (
        echo Terminating BareStats...
        taskkill /F /IM BareStats.exe /IM barestats.exe 2>nul
    ) else if /I "%CHOICE%"=="" (
        echo Terminating BareStats...
        taskkill /F /IM BareStats.exe /IM barestats.exe 2>nul
    ) else (
        echo Warning: Skipping process termination. Files in use may not be deleted.
    )
)

:: Unregister satellite background service if installed
if exist "%~dp0BareStats.exe" (
    "%~dp0BareStats.exe" --service-uninstall >nul 2>&1
) else if exist "%~dp0barestats.exe" (
    "%~dp0barestats.exe" --service-uninstall >nul 2>&1
)

:: Remove start menu shortcut if present
if exist "%APPDATA%\Microsoft\Windows\Start Menu\Programs\BareStats.lnk" (
    del /f /q "%APPDATA%\Microsoft\Windows\Start Menu\Programs\BareStats.lnk"
)

:: Remove desktop shortcut if present
if exist "%USERPROFILE%\Desktop\BareStats.lnk" (
    del /f /q "%USERPROFILE%\Desktop\BareStats.lnk"
)

echo BareStats uninstallation completed successfully.
