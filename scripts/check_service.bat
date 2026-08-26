@echo off
setlocal
REM Checks whether the PrinterInventoryAgent Windows service is running.
REM
REM Read-only (sc.exe query) - does not require Administrator privileges.
REM Exit codes, so this can also be used from a scheduled task or another
REM script: 0 = running, 1 = installed but not running, 2 = not installed.

set "SERVICE_NAME=PrinterInventoryAgent"

echo === Printer Inventory Agent - service status ===
echo.

sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 (
    echo Service "%SERVICE_NAME%" is NOT installed.
    pause
    exit /b 2
)

sc.exe query "%SERVICE_NAME%" | findstr /I "RUNNING" >nul
if %errorlevel%==0 (
    echo Service "%SERVICE_NAME%" is RUNNING.
    echo.
    sc.exe query "%SERVICE_NAME%"
    pause
    exit /b 0
) else (
    echo Service "%SERVICE_NAME%" is installed but NOT RUNNING.
    echo.
    sc.exe query "%SERVICE_NAME%"
    echo.
    echo To start it:  sc.exe start "%SERVICE_NAME%"
    pause
    exit /b 1
)
