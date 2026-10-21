@echo off
rem Build f4-coop.dll (F4SE plugin) and recv.exe (ghost bot) with MSVC.
rem Expects ..\f4se-0.6.23 to be a checkout of ianpatt/f4se tag v0.6.23.
setlocal
cd /d "%~dp0"

where cl >nul 2>&1
if not errorlevel 1 goto build

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto nomsvc
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul

:build
cl /nologo /LD /std:c++17 /EHsc /O2 /W3 /I "..\f4se-0.6.23" src\main.cpp /Fe:f4-coop.dll /link ws2_32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /W3 /Fe:recv.exe tools\recv.cpp ws2_32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /W3 /Fe:client.exe tools\client.cpp ws2_32.lib
if errorlevel 1 exit /b 1
del main.obj recv.obj client.obj f4-coop.exp f4-coop.lib >nul 2>&1
echo built f4-coop.dll, recv.exe and client.exe
exit /b 0

:nomsvc
echo ERROR: MSVC not found via vswhere
exit /b 1
