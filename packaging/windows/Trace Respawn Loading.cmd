@echo off
setlocal
cd /d "%~dp0"
"Bloodborne.exe" --script scripts/loading_trace.py
echo.
if errorlevel 1 (
    echo Could not collect the report. See the message above.
    pause
    exit /b 1
)
echo The loading report ZIP is in the logs folder. This mode measures loading; it does not change respawn timing.
pause
