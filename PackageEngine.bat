@echo off
setlocal enableextensions

rem Builds the editor and zips a prebuilt engine for designers, who run it without sources or Visual Studio.
rem Output lands in Saved\Packages. Extra options pass through, e.g. -Configuration=Shipping or -Output=D:\Drops.

cd /d "%~dp0"
call "%~dp0LuminaBuild.bat" Package %*
set "RESULT=%ERRORLEVEL%"

endlocal & exit /b %RESULT%
