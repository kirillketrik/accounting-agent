@echo off
setlocal
REM Stops and uninstalls the PrinterInventoryAgent Windows service.
REM
REM Uses sc.exe (stop + delete) exclusively - it does NOT require
REM printer_agent.exe to be present, and does not care where the service's
REM registered binary actually lives. That means this script keeps working
REM even if the exe was moved, rebuilt into a different folder, or deleted
REM out from under a still-registered service.
REM
REM Requires Administrator privileges; if not already elevated, it relaunches
REM itself elevated (you'll get a UAC prompt).

REM --- Relaunch elevated if needed ---
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Administrator privileges are required - requesting elevation...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -WorkingDirectory '%~dp0' -Verb RunAs"
    exit /b
)

set "SERVICE_NAME=PrinterInventoryAgent"

echo === Printer Inventory Agent - uninstall service ===
echo.

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 (
    echo Service "%SERVICE_NAME%" is not installed - nothing to do.
    pause
    exit /b 0
)

sc.exe query "%SERVICE_NAME%" | findstr /I "RUNNING" >nul
if not %errorlevel%==0 goto :skip_stop

echo Stopping the service...
sc.exe stop "%SERVICE_NAME%" >nul

REM Give the SCM a few seconds to actually stop it before deleting the
REM registration - sc.exe delete below doesn't wait for that itself. Written
REM as a flat goto loop (not a for /l with a goto inside it) - a goto that
REM jumps out of a parenthesized for/if block to a label still inside an
REM outer block confuses cmd.exe's parser ("was unexpected at this time").
set "WAIT_COUNT=0"
:wait_stopped
sc.exe query "%SERVICE_NAME%" | findstr /I "STOPPED" >nul
if not errorlevel 1 goto :skip_stop
set /a WAIT_COUNT+=1
if %WAIT_COUNT% GEQ 10 goto :skip_stop
timeout /t 1 /nobreak >nul
goto :wait_stopped

:skip_stop

echo Removing the "%SERVICE_NAME%" service registration...
sc.exe delete "%SERVICE_NAME%" >nul
if errorlevel 1 (
    echo ERROR: sc.exe delete failed - see output above.
    pause
    exit /b 1
)

echo.
echo Done. The service registration has been removed.
echo The exe, agent.ini, and agent.log (wherever they live) were left in
echo place - delete them yourself if you want to remove those too.
echo.
pause
