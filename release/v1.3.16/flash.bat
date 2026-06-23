@echo off
echo =========================================
echo    Excavator Rental Timer - Setup
echo =========================================
echo.
echo Installing required Python packages...
python -m pip install --upgrade pip
python -m pip install esptool pyserial
echo.
echo Starting Flasher...
echo.
python flash.py
pause
