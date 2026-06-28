@echo off
setlocal enableextensions

rem Regenerate project files after adding/removing sources or editing premake5.lua. First-time setup is Setup.bat.

cd /d "%~dp0"
set "LUMINA_DIR=%CD%"

set "PREMAKE_EXE=%LUMINA_DIR%\Tools\premake5.exe"

if not exist "%PREMAKE_EXE%" (
    echo premake5.exe not found at %PREMAKE_EXE%.
    echo Run Setup.bat first.
    endlocal
    exit /b 1
)

rem Non-blocking: regen needs no toolchain, but warn early on missing .NET 10 SDK / VS < 18.0. SKIP_PREREQ_CHECKS=1 to silence.
if not defined SKIP_PREREQ_CHECKS (
    where powershell.exe >nul 2>&1
    if not errorlevel 1 (
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%LUMINA_DIR%\BuildScripts\CheckPrerequisites.ps1" -NonBlocking
    )
)

rem Extra args pass straight through to premake (e.g. --tracy=off); persistent defaults live in BuildScripts\BuildConfig.lua.
"%PREMAKE_EXE%" vs2026 %*
if errorlevel 1 (
    echo.
    echo Project generation failed.
    endlocal
    exit /b 1
)

echo.
echo Solution generated. Open Lumina.slnx in Visual Studio 2026.
endlocal
exit /b 0
