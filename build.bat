@echo off
rem build.bat -- build aino_player.exe with MSVC.
rem
rem Deliberately written in a flat style: no parenthesised if-blocks, no caret line
rem continuations. cmd's block parser is easy to break with paths that contain
rem parentheses, and a build script that dies with "unexpected" is worse than ugly.
rem
rem INCLUDE / LIB are set by hand instead of calling vcvars64.bat: vcvars shells out
rem to reg.exe and does a pile of probing, while this project needs three paths.
setlocal

if not exist build mkdir build

rem ---- locate Visual Studio ----
set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
set "VSW=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSW%" for /f "delims=" %%i in ('"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VSPATH=%%i"
if not defined VSPATH if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community" set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\18\Community"
if not defined VSPATH if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community" set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not defined VSPATH if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools" set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
if not defined VSPATH goto nom
if not exist "%VSPATH%\VC\Tools\MSVC" goto nom

rem ---- newest toolset + newest Windows SDK ----
set "TOOLSET="
for /d %%i in ("%VSPATH%\VC\Tools\MSVC\*") do set "TOOLSET=%%i"
set "SDKV="
for /d %%i in ("%SDKROOT%\Include\10.*") do set "SDKV=%%~nxi"
if not defined TOOLSET goto nom
if not defined SDKV goto nom

set "ROOT=%~dp0"
set "INCLUDE=%TOOLSET%\include;%SDKROOT%\Include\%SDKV%\ucrt;%SDKROOT%\Include\%SDKV%\um;%SDKROOT%\Include\%SDKV%\shared;%SDKROOT%\Include\%SDKV%\winrt;%ROOT%vendor\webview2\include"
set "LIB=%TOOLSET%\lib\x64;%SDKROOT%\Lib\%SDKV%\ucrt\x64;%SDKROOT%\Lib\%SDKV%\um\x64"

echo [*] toolset %TOOLSET%
echo [*] sdk     %SDKV%
echo.

rem /SUBSYSTEM:WINDOWS keeps a console window from appearing behind the player.
"%TOOLSET%\bin\Hostx64\x64\cl.exe" /nologo /std:c++17 /utf-8 /EHsc /O2 /MT /Fo:build\ /Fe:build\aino_player.exe src\main.cpp src\login.cpp user32.lib gdi32.lib winhttp.lib winmm.lib shell32.lib ole32.lib oleaut32.lib windowscodecs.lib advapi32.lib mfplat.lib mf.lib mfuuid.lib vendor\webview2\lib\WebView2LoaderStatic.lib /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
if errorlevel 1 goto fail

del /q *.obj 2>nul
echo.
echo [ok] build\aino_player.exe
exit /b 0

:nom
echo [x] MSVC toolset or Windows SDK not found.
echo     Install Visual Studio with the "Desktop development with C++" workload.
exit /b 1

:fail
echo [x] build failed
exit /b 1
