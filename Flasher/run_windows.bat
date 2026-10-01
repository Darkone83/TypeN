@echo off
setlocal
cd /d "%~dp0"

where py >nul 2>nul
if %errorlevel%==0 (
    py -3 run.py
    goto :done
)

where python >nul 2>nul
if %errorlevel%==0 (
    python run.py
    goto :done
)

echo.
echo Python 3 was not found.
echo Install Python 3.11 or newer from https://www.python.org/
echo and enable "Add Python to PATH" during installation.
echo.
pause
exit /b 1

:done
if not %errorlevel%==0 (
    echo.
    echo TypeN Flasher exited with error code %errorlevel%.
    pause
)
endlocal
