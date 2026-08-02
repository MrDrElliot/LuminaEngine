@echo off
setlocal enableextensions

rem First-time setup: check prerequisites, build LuminaBuildTool, fetch external dependencies,
rem then generate project files. Everything after the prerequisite check runs inside the tool.

cd /d "%~dp0"
set "LUMINA_DIR=%CD%"

echo.
echo ============================================================
echo  LUMINA ENGINE SETUP
echo ============================================================
echo  Working directory: %LUMINA_DIR%
echo.

rem Warn if a previous install persisted a different LUMINA_DIR; a stale value silently links
rem the wrong engine. Reads the persisted value, not the one set above.
call :GetUserEnv LUMINA_DIR PRIOR_LUMINA_DIR
if defined PRIOR_LUMINA_DIR (
    if /I not "%PRIOR_LUMINA_DIR%"=="%LUMINA_DIR%" (
        echo [setup] NOTE: LUMINA_DIR was previously set to:
        echo            %PRIOR_LUMINA_DIR%
        echo        It will be overwritten with:
        echo            %LUMINA_DIR%
        echo        Tooling already running ^(VS, Rider^) keeps the old value until restarted.
        echo.
    )
)

rem SKIP_PREREQ_CHECKS=1 bypasses.
if not defined SKIP_PREREQ_CHECKS (
    where powershell.exe >nul 2>&1
    if errorlevel 1 (
        echo [setup] WARNING: powershell.exe not found; skipping prerequisite check.
    ) else (
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%LUMINA_DIR%\BuildScripts\CheckPrerequisites.ps1"
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
call "%LUMINA_DIR%\LuminaBuild.bat" Setup %*
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo  Generating project files
echo ============================================================
call "%LUMINA_DIR%\LuminaBuild.bat" GenerateProjectFiles
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
    if exist "%LUMINA_DIR%\Lumina.sln" (
        echo  Opening Lumina.sln...
        start "" "%LUMINA_DIR%\Lumina.sln"
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

:GetUserEnv
rem Read a persisted user-scope environment variable exactly. %1 = name, %2 = out variable.
setlocal
set "_gue_val="
where powershell.exe >nul 2>&1
if not errorlevel 1 (
    for /f "usebackq delims=" %%V in (`powershell.exe -NoProfile -Command "[Environment]::GetEnvironmentVariable('%~1','User')"`) do set "_gue_val=%%V"
) else (
    for /f "tokens=1,2,*" %%A in ('reg query "HKCU\Environment" /v "%~1" 2^>nul') do (
        if /I "%%A"=="%~1" if not defined _gue_val set "_gue_val=%%C"
    )
)
endlocal & set "%~2=%_gue_val%"
exit /b 0
