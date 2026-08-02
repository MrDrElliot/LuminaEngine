@echo off
setlocal enableextensions

rem Regenerate IDE project files after adding or removing modules, plugins or sources.
rem First-time setup is Setup.bat. Source files themselves are discovered at build time, so this
rem is only needed to refresh what the IDE shows.

cd /d "%~dp0"
set "LUMINA_DIR=%CD%"

where dotnet >nul 2>&1
if errorlevel 1 (
    echo error: the .NET SDK is required to run LuminaBuildTool. Install .NET 10 or newer.
    endlocal
    exit /b 1
)

rem Non-blocking: generation needs no C++ toolchain, but warn early on a missing .NET SDK or an
rem old Visual Studio. SKIP_PREREQ_CHECKS=1 silences it.
if not defined SKIP_PREREQ_CHECKS (
    where powershell.exe >nul 2>&1
    if not errorlevel 1 (
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%LUMINA_DIR%\BuildScripts\CheckPrerequisites.ps1" -NonBlocking
    )
)

rem Extra arguments pass straight through, for example -Tracy=off. Persistent defaults live in
rem Engine\Build\BuildConfiguration.json.
call "%LUMINA_DIR%\LuminaBuild.bat" GenerateProjectFiles %*
if errorlevel 1 (
    echo.
    echo Project generation failed.
    endlocal
    exit /b 1
)

echo.
echo Solution generated: Lumina.sln
endlocal
exit /b 0
