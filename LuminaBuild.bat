@echo off
rem Front end for LuminaBuildTool. Builds the tool if needed, then forwards every argument to it.
rem Usage: LuminaBuild.bat Build Reflector -Configuration=Development

setlocal
set LUMINA_ROOT=%~dp0
set BUILD_TOOL_PROJECT=%LUMINA_ROOT%Engine\Tools\LuminaBuildTool\LuminaBuildTool.csproj
set BUILD_TOOL_DLL=%LUMINA_ROOT%Binaries\DotNet\BuildTool\LuminaBuildTool.dll

where dotnet >nul 2>&1
if errorlevel 1 (
    echo error: the .NET SDK is required to run LuminaBuildTool. Install .NET 10 or newer.
    exit /b 1
)

rem Reached when this script is not sitting in an engine root, which is what happens when it is
rem copied somewhere on its own or LUMINA_DIR points at the wrong tree. Checked here so the failure
rem names the tree it looked in, rather than surfacing as an MSBuild error about a missing project.
if not exist "%BUILD_TOOL_PROJECT%" (
    echo error: LuminaBuildTool source is missing at "%BUILD_TOOL_PROJECT%".
    echo "%LUMINA_ROOT%" does not look like a Lumina engine root.
    exit /b 1
)

rem The C# compiler reads LIB and warns about every entry that is not a real directory (CS1668).
rem A Developer Command Prompt puts ATL/MFC paths there whether or not that optional component was
rem installed, so building from one reported it on every managed project. Cleared inside setlocal,
rem so this only affects the tool build below and nothing the caller's shell keeps.
set LIB=
set INCLUDE=

rem Always rebuild: MSBuild no-ops in well under a second, and a stale tool silently produces
rem stale rules behavior that is very hard to diagnose. Set LUMINA_SKIP_TOOL_BUILD=1 to opt out.
if not defined LUMINA_SKIP_TOOL_BUILD (
    rem Skipping the restore is a third of this step, and a fresh clone falls back to the full build below.
    dotnet build "%BUILD_TOOL_PROJECT%" -v quiet --nologo --no-restore
    if errorlevel 1 (
        dotnet build "%BUILD_TOOL_PROJECT%" -v quiet --nologo
        if errorlevel 1 exit /b 1
    )
)

rem Two different failures, and saying "did not produce" for the second one sends the reader off
rem looking at a build that never ran.
if not exist "%BUILD_TOOL_DLL%" (
    if defined LUMINA_SKIP_TOOL_BUILD (
        echo error: LuminaBuildTool is missing at "%BUILD_TOOL_DLL%" and LUMINA_SKIP_TOOL_BUILD is set.
        echo Clear LUMINA_SKIP_TOOL_BUILD and run this script again to build it.
    ) else (
        echo error: LuminaBuildTool did not produce "%BUILD_TOOL_DLL%".
    )
    exit /b 1
)

dotnet "%BUILD_TOOL_DLL%" %* -EngineRoot="%LUMINA_ROOT:~0,-1%"
exit /b %errorlevel%
