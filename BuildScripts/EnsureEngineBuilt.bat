@echo off
setlocal enableextensions

rem Prebuild step for a game project: builds the engine for <Configuration> and <TargetType> when
rem its binaries are missing, so a first build against an unbuilt configuration does not fail on a
rem missing Runtime import library.

set "CONFIG=%~1"
if "%CONFIG%"=="" (
    echo [EnsureEngineBuilt] usage: EnsureEngineBuilt.bat ^<Configuration^> [TargetType]
    exit /b 1
)

set "TARGETTYPE=%~2"
if "%TARGETTYPE%"=="" set "TARGETTYPE=Editor"
if /I not "%TARGETTYPE%"=="Editor" if /I not "%TARGETTYPE%"=="Game" (
    echo [EnsureEngineBuilt] Unknown target type "%TARGETTYPE%". Must be Editor or Game.
    exit /b 1
)

if not defined LUMINA_DIR (
    echo [EnsureEngineBuilt] LUMINA_DIR is not set. Run the engine's Setup.bat first.
    exit /b 1
)

set "ENGINE_BIN=%LUMINA_DIR%\Binaries\Windows64"
set "RUNTIME_DLL=%ENGINE_BIN%\Runtime-%CONFIG%.dll"

rem The Editor module only exists in Editor builds; checking for it under Game would rebuild forever.
if /I "%TARGETTYPE%"=="Editor" (
    if exist "%RUNTIME_DLL%" if exist "%ENGINE_BIN%\Editor-%CONFIG%.dll" exit /b 0
) else (
    if exist "%RUNTIME_DLL%" exit /b 0
)

echo.
echo ===============================================================================
echo  [EnsureEngineBuilt] Engine binaries for "%CONFIG% ^| %TARGETTYPE%" are missing.
echo  Building the engine once; later builds reuse the cached output.
echo ===============================================================================
echo.

call "%LUMINA_DIR%\LuminaBuild.bat" Build Lumina -Configuration=%CONFIG% -TargetType=%TARGETTYPE%
if errorlevel 1 (
    echo.
    echo [EnsureEngineBuilt] Engine build failed for "%CONFIG% ^| %TARGETTYPE%". Fix the errors above and retry.
    exit /b 1
)

echo.
echo [EnsureEngineBuilt] Engine "%CONFIG% ^| %TARGETTYPE%" build complete.
endlocal
exit /b 0
