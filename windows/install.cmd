@echo off
rem Ozzy for Windows: runs scripts\install.ps1 as administrator (asks for it), passing the arguments on
net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process cmd -ArgumentList '/c ""%~f0" %*"' -Verb RunAs"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install.ps1" %*
