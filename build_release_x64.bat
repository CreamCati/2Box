@echo off
setlocal
cd /d "%~dp0"

echo === 2Box CPU-Affinity modified build ===
echo.
echo Please run this script from a Visual Studio Developer Command Prompt
echo with Visual Studio 2022 v143 and Windows 10/11 SDK installed.
echo.

msbuild 2Box.sln /m /p:Configuration=Release /p:Platform=x64
if errorlevel 1 (
    echo.
    echo BUILD FAILED.
    pause
    exit /b %errorlevel%
)

echo.
echo BUILD SUCCESS.
echo Output should be under:
echo   bin\x64\
echo.
pause
