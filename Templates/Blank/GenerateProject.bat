@echo off
setlocal enableextensions

rem Regenerates this project's IDE files. The engine is built from source alongside the project,
rem so its own output stays in the engine tree and is shared across projects.

cd /d "%~dp0"
set "PROJECT_DIR=%CD%"

if not defined LUMINA_DIR (
    echo LUMINA_DIR is not set. Run the engine's Setup.bat first.
    endlocal
    exit /b 1
)

if not exist "%LUMINA_DIR%\LuminaBuild.bat" (
    echo LuminaBuild.bat not found under "%LUMINA_DIR%".
    echo LUMINA_DIR does not point at a Lumina engine root.
    endlocal
    exit /b 1
)

call "%LUMINA_DIR%\LuminaBuild.bat" GenerateProjectFiles -Project="%PROJECT_DIR%" %*
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
