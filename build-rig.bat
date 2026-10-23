@echo off
rem Build the test rig (no game needed): ENet static lib, standalone
rem F4MPServer.exe and the F4MPBot.exe test client, from vendored sources.
rem Plugin build stays in build.bat.
setlocal
cd /d "%~dp0"

where cl >nul 2>&1
if not errorlevel 1 goto build

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto nomsvc
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul

:build
rem ---- ENet 1.3.18 (MIT, vendored) ----
if not exist build mkdir build
cd vendor\enet-1.3.18
cl /nologo /c /O2 /W3 /Iinclude *.c
if errorlevel 1 exit /b 1
lib /nologo /out:..\..\build\enet-1.3.18.lib *.obj
if errorlevel 1 exit /b 1
del *.obj >nul 2>&1
cd ..\..

rem ---- common + server ----
cl /nologo /std:c++latest /EHsc /O2 /W3 /Ivendor\f4mp\common /Ivendor\enet-1.3.18\include ^
   vendor\f4mp\server\main.cpp vendor\f4mp\common\EnetTransport.cpp vendor\f4mp\common\Net.cpp ^
   vendor\f4mp\common\Redact.cpp vendor\f4mp\common\Server.cpp ^
   /Fe:build\F4MPServer.exe /link build\enet-1.3.18.lib ws2_32.lib winmm.lib
if errorlevel 1 exit /b 1

rem ---- common + bot ----
cl /nologo /std:c++latest /EHsc /O2 /W3 /Ivendor\f4mp\common /Ivendor\enet-1.3.18\include ^
   vendor\f4mp\bot\main.cpp vendor\f4mp\common\EnetTransport.cpp vendor\f4mp\common\Net.cpp ^
   vendor\f4mp\common\Redact.cpp ^
   /Fe:build\F4MPBot.exe /link build\enet-1.3.18.lib ws2_32.lib winmm.lib
if errorlevel 1 exit /b 1

rem ---- address library dump tool ----
cl /nologo /std:c++latest /EHsc /O2 /W3 /Fe:build\aldump.exe tools\aldump.c
if errorlevel 1 exit /b 1

del vendor\f4mp\common\*.obj 2>nul
del vendor\f4mp\server\*.obj vendor\f4mp\bot\*.obj 2>nul
del tools\aldump.obj 2>nul
echo built build\F4MPServer.exe, build\F4MPBot.exe and build\aldump.exe
exit /b 0

:nomsvc
echo ERROR: MSVC not found via vswhere
exit /b 1