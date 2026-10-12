@echo off
setlocal
cd /d "%~dp0"
"Bloodborne.exe" --script scripts/crash_trace.py
echo.
if errorlevel 1 (
    echo Could not collect the report. See the message above.
    pause
    exit /b 1
)
echo The crash report ZIP is in the logs folder.
pause
