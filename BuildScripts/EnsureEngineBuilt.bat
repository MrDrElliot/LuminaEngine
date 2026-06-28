@echo off
setlocal enableextensions

rem Prebuild step: builds the engine for <Configuration>+<Platform> if its libs are missing.
rem Platform=Game needs Game libs, not Editor, so the caller must pass its platform (defaults to Editor).

set "CONFIG=%~1"
if "%CONFIG%"=="" (
    echo [EnsureEngineBuilt] usage: EnsureEngineBuilt.bat ^<Configuration^> [Platform]
    exit /b 1
)

set "PLATFORM=%~2"
if "%PLATFORM%"=="" set "PLATFORM=Editor"
if /I not "%PLATFORM%"=="Editor" if /I not "%PLATFORM%"=="Game" (
    echo [EnsureEngineBuilt] Unknown platform "%PLATFORM%". Must be Editor or Game.
    exit /b 1
)

if not defined LUMINA_DIR (
    echo [EnsureEngineBuilt] LUMINA_DIR is not set. Run the engine's Setup.bat first.
    exit /b 1
)

set "ENGINE_BIN=%LUMINA_DIR%\Binaries\Windows64"
set "RUNTIME_LIB=%ENGINE_BIN%\Runtime-%CONFIG%.lib"

rem Editor lib only exists in Editor builds; skip its check under Game or we ping-pong rebuilding.
if /I "%PLATFORM%"=="Editor" (
    set "EDITOR_LIB=%ENGINE_BIN%\Editor-%CONFIG%.lib"
    if exist "%RUNTIME_LIB%" if exist "%EDITOR_LIB%" (
        exit /b 0
    )
) else (
    if exist "%RUNTIME_LIB%" (
        exit /b 0
    )
)

echo.
echo ===============================================================================
echo  [EnsureEngineBuilt] Engine libraries for "%CONFIG% ^| %PLATFORM%" are missing.
echo  Building Lumina.slnx -- one-time cost; subsequent builds reuse the cached libs.
echo ===============================================================================
echo.

rem -latest -prerelease matches any VS2026 edition (Community/Pro/Enterprise/BuildTools).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [EnsureEngineBuilt] vswhere.exe not found. Install Visual Studio 2026 or build the engine manually.
    exit /b 1
)

rem Capture stderr too, else a real vswhere failure silently leaves MSBUILD undefined.
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe 2^>nul`) do (
    set "MSBUILD=%%i"
)

if not defined MSBUILD (
    echo [EnsureEngineBuilt] MSBuild.exe not found via vswhere. Install the Visual Studio 2026 "Desktop development with C++" workload.
    exit /b 1
)

if not exist "%LUMINA_DIR%\Lumina.slnx" (
    echo [EnsureEngineBuilt] %LUMINA_DIR%\Lumina.slnx not found. Run GenerateProject.bat first.
    exit /b 1
)

"%MSBUILD%" "%LUMINA_DIR%\Lumina.slnx" -p:Configuration=%CONFIG% -p:Platform=%PLATFORM% -m -v:minimal -nologo
if errorlevel 1 (
    echo.
    echo [EnsureEngineBuilt] Engine build failed for "%CONFIG% ^| %PLATFORM%". Resolve the errors above and retry.
    exit /b 1
)

echo.
echo [EnsureEngineBuilt] Engine "%CONFIG% ^| %PLATFORM%" build complete.
endlocal
exit /b 0
