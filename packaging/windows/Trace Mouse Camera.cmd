@echo off
setlocal
cd /d "%~dp0"
"Bloodborne.exe" --script scripts/mouse_trace.py --play
echo.
if errorlevel 1 (
    echo Could not collect the report. See the message above.
    pause
    exit /b 1
)
echo The report ZIP is in the logs folder. This mode records evidence; camera behavior is unchanged.
pause
