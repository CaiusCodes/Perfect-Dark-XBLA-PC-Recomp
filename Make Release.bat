@echo off
cmake --build "%~dp0out\build\win-amd64-release" --config Release
if errorlevel 1 (
    echo.
    echo Build failed. No release ZIP was created.
    pause
    exit /b 1
)
rem Builds the launcher, stages Perfect Dark\ (launcher, README, licenses, resources\runtime),
rem audits it and makes the ZIP in out\release.
powershell.exe -NoProfile -File "%~dp0tools\Make-Release.ps1"
pause
