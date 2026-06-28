@echo off
setlocal enableextensions

rem Bootstrap premake5, run `premake5 setup`, then generate Lumina.slnx.
rem Uses curl.exe and tar.exe (Windows 10 1803+); no Python.

cd /d "%~dp0"
set "LUMINA_DIR=%CD%"

set "PREMAKE_DIR=%LUMINA_DIR%\Tools"
set "PREMAKE_EXE=%PREMAKE_DIR%\premake5.exe"
set "PREMAKE_VERSION=5.0.0-beta2"
set "PREMAKE_URL=https://github.com/premake/premake-core/releases/download/v%PREMAKE_VERSION%/premake-%PREMAKE_VERSION%-windows.zip"

echo.
echo ============================================================
echo  LUMINA ENGINE SETUP
echo ============================================================
echo  Working directory: %LUMINA_DIR%
echo.

rem Warn if a previous install persisted a different LUMINA_DIR (stale values cause
rem "wrong engine linked" errors). Reads the persisted value, not %LUMINA_DIR%.
call :GetUserEnv LUMINA_DIR PRIOR_LUMINA_DIR
if defined PRIOR_LUMINA_DIR (
    if /I not "%PRIOR_LUMINA_DIR%"=="%LUMINA_DIR%" (
        echo [setup] NOTE: LUMINA_DIR was previously set to:
        echo            %PRIOR_LUMINA_DIR%
        echo        It will be overwritten with:
        echo            %LUMINA_DIR%
        echo        Tooling that runs outside this shell ^(VS, Rider, taskbar^) keeps the
        echo        old value until it is restarted.
        echo.
    )
)

rem Check prerequisites up front (SKIP_PREREQ_CHECKS=1 to bypass).
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

rem Bootstrap premake5 if missing.
if not exist "%PREMAKE_EXE%" (
    echo [bootstrap] premake5.exe not found, downloading %PREMAKE_VERSION%...
    if not exist "%PREMAKE_DIR%" mkdir "%PREMAKE_DIR%"

    where curl.exe >nul 2>&1
    if errorlevel 1 (
        echo [bootstrap] ERROR: curl.exe is not on PATH. Windows 10 1803+ required.
        goto :fail
    )
    where tar.exe >nul 2>&1
    if errorlevel 1 (
        echo [bootstrap] ERROR: tar.exe is not on PATH. Windows 10 1803+ required.
        goto :fail
    )

    curl.exe --fail --location --silent --show-error --output "%PREMAKE_DIR%\premake.zip" "%PREMAKE_URL%"
    if errorlevel 1 (
        echo [bootstrap] ERROR: failed to download premake5.
        goto :fail
    )

    tar.exe -xf "%PREMAKE_DIR%\premake.zip" -C "%PREMAKE_DIR%"
    if errorlevel 1 (
        echo [bootstrap] ERROR: failed to extract premake5.zip.
        goto :fail
    )

    del /q "%PREMAKE_DIR%\premake.zip" >nul 2>&1

    if not exist "%PREMAKE_EXE%" (
        echo [bootstrap] ERROR: premake5.exe still missing after extraction.
        goto :fail
    )
    echo [bootstrap] premake5 ready.
    echo.
)

set "NEED_DOWNLOAD=1"
if exist "%LUMINA_DIR%\External" set "NEED_DOWNLOAD=0"
echo %* | findstr /I /C:"--force" >nul && set "NEED_DOWNLOAD=1"

set "AUTO_YES="
echo %* | findstr /I /C:"--yes" >nul && set "AUTO_YES=1"
if defined LUMINA_SETUP_YES set "AUTO_YES=1"

if "%NEED_DOWNLOAD%"=="1" (
    call :ConfirmDownload
    if errorlevel 1 goto :cancelled
)

rem Setup.bat already confirmed above, so pass --yes to keep premake non-interactive.
"%PREMAKE_EXE%" setup --yes %*
if errorlevel 1 goto :fail

rem Confirm setx actually persisted the value (it can fail silently on locked profiles).
call :GetUserEnv LUMINA_DIR PERSISTED_LUMINA_DIR
if not defined PERSISTED_LUMINA_DIR (
    echo [setup] ERROR: LUMINA_DIR was not persisted to the user environment.
    echo         The current shell still sees it ^(set by Setup.bat^), but a fresh
    echo         shell will not. Possible causes: roaming-profile restrictions,
    echo         setx hit its 1024-char limit, or HKCU\Environment is locked down.
    goto :fail
)
rem Strip a trailing backslash so a drive-root install (H:\) still matches.
if "%PERSISTED_LUMINA_DIR:~-1%"=="\" set "PERSISTED_LUMINA_DIR=%PERSISTED_LUMINA_DIR:~0,-1%"
set "EXPECTED_LUMINA_DIR=%LUMINA_DIR%"
if "%EXPECTED_LUMINA_DIR:~-1%"=="\" set "EXPECTED_LUMINA_DIR=%EXPECTED_LUMINA_DIR:~0,-1%"
if /I not "%PERSISTED_LUMINA_DIR%"=="%EXPECTED_LUMINA_DIR%" (
    echo [setup] ERROR: LUMINA_DIR persisted with the wrong value.
    echo         Expected: %LUMINA_DIR%
    echo         Got:      %PERSISTED_LUMINA_DIR%
    goto :fail
)

rem Generate the Visual Studio solution.
echo.
echo ============================================================
echo  Generating Visual Studio 2026 solution
echo ============================================================
"%PREMAKE_EXE%" vs2026
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo  ALL DONE - Lumina.slnx generated
echo ============================================================
echo  LUMINA_DIR persisted: %PERSISTED_LUMINA_DIR%
echo  Note: already-running Visual Studio / Rider instances must be
echo  restarted to pick up the new LUMINA_DIR.
echo.

rem Open the generated solution for the user. Opt out with --no-open or LUMINA_NO_OPEN=1.
set "DO_OPEN=1"
if defined LUMINA_NO_OPEN set "DO_OPEN=0"
echo %* | findstr /I /C:"--no-open" >nul && set "DO_OPEN=0"
if "%DO_OPEN%"=="1" (
    if exist "%LUMINA_DIR%\Lumina.slnx" (
        echo  Opening Lumina.slnx...
        start "" "%LUMINA_DIR%\Lumina.slnx"
    ) else (
        echo  Lumina.slnx not found at %LUMINA_DIR%; open it manually.
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

:cancelled
echo.
echo Setup cancelled. No files were downloaded.
echo See DEPENDENCIES.md to review exactly what setup fetches, or fetch each
echo library yourself from its upstream and drop it into External\.
echo.
endlocal
exit /b 1

:GetUserEnv
rem Read a persisted User-scope env var exactly.  %1 = value name, %2 = out var.
rem PowerShell handles spaces and REG_EXPAND_SZ; reg-query fallback if it's absent.
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

:ConfirmDownload
rem Show what will be downloaded; return 1 to abort.
echo.
echo ------------------------------------------------------------
echo  EXTERNAL DEPENDENCIES
echo ------------------------------------------------------------
echo  Setup is about to DOWNLOAD a prebuilt dependency bundle
echo  ^(External.zip, ~671 MB^) and extract it into:
echo      %LUMINA_DIR%
echo.
echo  Contents:
echo    - .NET 10 runtime + hosting headers   C# scripting host
echo    - LLVM/Clang 19 ^(libclang^)          reflection codegen
echo    - Slang shader compiler               shader compilation
echo    - RenderDoc                           in-app frame capture
echo    - Tracy                               CPU/GPU profiler
echo.
echo  Host:    github.com/MrDrElliot/LuminaEngine  ^(release asset^)
echo  Details: DEPENDENCIES.md  ^(upstream sources, versions, licenses^)
echo.
echo  After download the bundle is integrity-checked against a recorded
echo  SHA-256. If you prefer not to trust the bundle, you can fetch each
echo  library from its upstream instead -- see DEPENDENCIES.md.
echo.
if defined AUTO_YES exit /b 0
set "REPLY="
set /p "REPLY=Proceed with download? [Y/n] "
if /I "%REPLY%"=="n"  exit /b 1
if /I "%REPLY%"=="no" exit /b 1
exit /b 0
