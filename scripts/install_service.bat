@echo off
setlocal
REM Installs and starts the PrinterInventoryAgent Windows service.
REM
REM This script expects printer_agent.exe (and agent.ini) to sit in the SAME
REM folder as this .bat file - that's the layout scripts\build.bat produces,
REM and the layout to copy to a target machine for deployment (this whole
REM folder, not just the .exe).
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

set "SCRIPT_DIR=%~dp0"
set "EXE_PATH=%SCRIPT_DIR%printer_agent.exe"
set "SERVICE_NAME=PrinterInventoryAgent"

echo === Printer Inventory Agent - install/start service ===
echo.

if not exist "%EXE_PATH%" (
    echo ERROR: printer_agent.exe not found next to this script:
    echo   %EXE_PATH%
    echo Copy install_service.bat into the same folder as printer_agent.exe first.
    pause
    exit /b 1
)

if not exist "%SCRIPT_DIR%agent.ini" (
    echo WARNING: agent.ini not found next to printer_agent.exe.
    echo The agent will run with built-in defaults ^(placeholder server URL^)
    echo until you add one - see README.md for the format.
    echo.
)

echo Installing the "%SERVICE_NAME%" service...
"%EXE_PATH%" --install
echo.

REM sc.exe query fails (nonzero exit) if the service doesn't exist at all -
REM that's the only case worth treating as a hard failure here, since
REM "--install" above already logs and tolerates "service already installed".
sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 (
    echo ERROR: service "%SERVICE_NAME%" is not registered - install failed.
    echo Re-run this script from an elevated prompt and check the output above.
    pause
    exit /b 1
)

sc.exe query "%SERVICE_NAME%" | findstr /I "RUNNING" >nul
if %errorlevel%==0 (
    echo Service is already running.
) else (
    echo Starting the service...
    sc.exe start "%SERVICE_NAME%"
    echo.
)

echo.
echo --- Current status ---
sc.exe query "%SERVICE_NAME%"

echo.
echo Log file: %SCRIPT_DIR%agent.log
echo To watch it live:  powershell -Command "Get-Content '%SCRIPT_DIR%agent.log' -Tail 20 -Wait"
echo To stop it later:  sc.exe stop "%SERVICE_NAME%"
echo To uninstall:      "%EXE_PATH%" --uninstall
echo.
pause
