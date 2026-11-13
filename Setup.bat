@echo off
setlocal enableextensions

rem First-time setup: check prerequisites, build LuminaBuildTool, fetch external dependencies,
rem then generate project files. Everything after the prerequisite check runs inside the tool.

cd /d "%~dp0"
set "ENGINE_DIR=%CD%"

echo.
echo ============================================================
echo  LUMINA ENGINE SETUP
echo ============================================================
echo  Working directory: %ENGINE_DIR%
echo.

rem SKIP_PREREQ_CHECKS=1 bypasses.
if not defined SKIP_PREREQ_CHECKS (
    where powershell.exe >nul 2>&1
    if errorlevel 1 (
        echo [setup] WARNING: powershell.exe not found; skipping prerequisite check.
    ) else (
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ENGINE_DIR%\BuildScripts\CheckPrerequisites.ps1"
        if errorlevel 1 (
            echo.
            echo [setup] Required build prerequisites are missing ^(see above^).
            echo         Install them and re-run Setup.bat. To bypass anyway:
            echo             set SKIP_PREREQ_CHECKS=1 ^&^& Setup.bat
            goto :fail
        )
    )
)

where dotnet >nul 2>&1
if errorlevel 1 (
    echo [setup] ERROR: the .NET SDK is required to build LuminaBuildTool. Install .NET 10 or newer.
    goto :fail
)

rem Extra arguments pass through, so -Force and -Yes reach the tool.
call "%ENGINE_DIR%\LuminaBuild.bat" Setup %*
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo  Generating project files
echo ============================================================
call "%ENGINE_DIR%\LuminaBuild.bat" GenerateProjectFiles
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo  ALL DONE
echo ============================================================
echo  Build the editor with:
echo      LuminaBuild.bat Build Lumina -TargetType=Editor
echo.

rem Opt out with -no-open or LUMINA_NO_OPEN=1.
set "DO_OPEN=1"
if defined LUMINA_NO_OPEN set "DO_OPEN=0"
echo %* | findstr /I /C:"no-open" >nul && set "DO_OPEN=0"
if "%DO_OPEN%"=="1" (
    for %%S in (Lumina.slnx Lumina.sln) do if exist "%ENGINE_DIR%\%%S" (
        echo  Opening %%S...
        start "" "%ENGINE_DIR%\%%S"
    )
)

endlocal
exit /b 0

:fail
echo.
echo Setup failed. See messages above.
echo.
endlocal
exit /b 1
