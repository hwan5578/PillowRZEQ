@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title Pillow RZ EQ - VST3 build

echo.
echo   Pillow RZ EQ  -  Windows VST3 build
echo   ===================================
echo.

rem ---------------------------------------------------------------- tools
where cmake >nul 2>nul
if errorlevel 1 goto :no_cmake
where git >nul 2>nul
if errorlevel 1 goto :no_git

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :no_vs
echo   Visual Studio C++ : %VSPATH%

rem ---------------------------------------------------------------- build
echo.
echo   [1/3] Configure  (first run downloads JUCE - takes a few minutes)
cmake -B build -A x64
if errorlevel 1 goto :failed

echo.
echo   [2/3] Build VST3 (Release)
cmake --build build --config Release --target PillowRZ_VST3 --parallel
if errorlevel 1 goto :failed

set "BUILT=%CD%\build\PillowRZ_artefacts\Release\VST3\Pillow RZ EQ.vst3"
if not exist "%BUILT%" goto :failed

rem ---------------------------------------------------------------- install
set "CPF=%CommonProgramFiles%"
if defined CommonProgramW6432 set "CPF=%CommonProgramW6432%"
set "DEST=%CPF%\VST3\Pillow RZ EQ.vst3"

echo.
echo   [3/3] Install to "%DEST%"
if exist "%DEST%" rmdir /s /q "%DEST%" >nul 2>nul
xcopy "%BUILT%" "%DEST%" /E /I /Y /Q >nul 2>nul
if errorlevel 1 goto :no_admin

echo.
echo   Done!  In Cubase: Studio ^> VST Plug-in Manager ^> Rescan.
echo   Vendor "Pillow Audio", plug-in "Pillow RZ EQ".
goto :ok

rem ---------------------------------------------------------------- messages
:no_cmake
echo   CMake is not installed. Install it once, then run this file again:
echo       winget install Kitware.CMake
goto :fail

:no_git
echo   Git is not installed (needed to download JUCE). Install it once:
echo       winget install Git.Git
goto :fail

:no_vs
echo   Visual Studio C++ build tools were not found. Install them once (free, a few GB):
echo       winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
echo   (Visual Studio 2022/2026 Community with "Desktop development with C++" also works.)
goto :fail

:no_admin
echo.
echo   The VST3 was built, but copying into Program Files needs administrator rights
echo   (or Cubase is open and using the plug-in).
echo   Either close Cubase and right-click this file ^> "Run as administrator",
echo   or copy this folder by hand into  %CPF%\VST3 :
echo       %BUILT%
explorer /select,"%BUILT%"
goto :fail

:failed
echo.
echo   Build failed. Copy the messages above and send them to Claude.
goto :fail

:fail
echo.
pause
exit /b 1

:ok
echo.
pause
exit /b 0
