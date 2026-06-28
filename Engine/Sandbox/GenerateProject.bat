@echo off
setlocal enableextensions

cd /d "%~dp0"

if not defined LUMINA_DIR (
    echo LUMINA_DIR is not set. Run the engine's Setup.bat first.
    endlocal
    exit /b 1
)

set "PREMAKE_EXE=%LUMINA_DIR%\Tools\premake5.exe"

if not exist "%PREMAKE_EXE%" (
    echo premake5.exe not found at "%PREMAKE_EXE%".
    echo Run the engine's Setup.bat first.
    endlocal
    exit /b 1
)

"%PREMAKE_EXE%" vs2026
if errorlevel 1 (
    echo.
    echo Project generation failed.
    endlocal
    exit /b 1
)

echo.
echo Solution generated.
endlocal
exit /b 0
