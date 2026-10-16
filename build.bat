@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
if not exist "build" mkdir "build"
if not exist "out" mkdir "out"
set "OUTPUT=out\hdd-noise.exe"
where cl >nul 2>nul
if not errorlevel 1 goto msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if defined VSDIR if exist "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" (
    call "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" >nul
    goto msvc
)
where g++ >nul 2>nul
if not errorlevel 1 goto mingw
echo C++ compiler not found. Install Visual Studio Desktop development with C++.
exit /b 1
:msvc
rc /nologo /c65001 /fo build\App.res App.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /utf-8 /EHsc /O2 /W4 /MT /DUNICODE /D_UNICODE /DNOMINMAX /Fo:build\ App.cpp Synth.cpp AudioEngine.cpp DiskMonitor.cpp build\App.res /Fe:%OUTPUT% /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:app.manifest user32.lib gdi32.lib shell32.lib comctl32.lib winmm.lib pdh.lib advapi32.lib
if errorlevel 1 exit /b 1
if /i "%~1"=="test" (
    cl /nologo /std:c++17 /utf-8 /EHsc /O2 /W4 /MT /I. /Fo:build\ tests\SynthTests.cpp Synth.cpp /Fe:build\SynthTests.exe
    if errorlevel 1 exit /b 1
    build\SynthTests.exe
    if errorlevel 1 exit /b 1
)
if /i "%~1"=="integration" (
    cl /nologo /std:c++17 /utf-8 /EHsc /O2 /W4 /MT /I. /Fo:build\ tests\AudioEngineTests.cpp AudioEngine.cpp Synth.cpp /Fe:build\AudioEngineTests.exe /link winmm.lib
    if errorlevel 1 exit /b 1
    build\AudioEngineTests.exe
    if errorlevel 1 exit /b 1
)
goto done
:mingw
windres -c 65001 -i App.rc -o build\App-resource.o
if errorlevel 1 exit /b 1
windres -c 65001 -i MinGWManifest.rc -o build\App-manifest.o
if errorlevel 1 exit /b 1
g++ -std=c++17 -O2 -Wall -Wextra -municode -mwindows -DUNICODE -D_UNICODE -DNOMINMAX App.cpp Synth.cpp AudioEngine.cpp DiskMonitor.cpp build\App-resource.o build\App-manifest.o -o %OUTPUT% -lwinmm -lpdh -lcomctl32 -lshell32 -lgdi32 -luser32 -ladvapi32 -static
if errorlevel 1 exit /b 1
if /i "%~1"=="test" (
    g++ -std=c++17 -O2 -I. tests\SynthTests.cpp Synth.cpp -o build\SynthTests.exe -static
    if errorlevel 1 exit /b 1
    build\SynthTests.exe
    if errorlevel 1 exit /b 1
)
if /i "%~1"=="integration" (
    g++ -std=c++17 -O2 -I. tests\AudioEngineTests.cpp AudioEngine.cpp Synth.cpp -o build\AudioEngineTests.exe -lwinmm -static
    if errorlevel 1 exit /b 1
    build\AudioEngineTests.exe
    if errorlevel 1 exit /b 1
)
:done
echo Built: %CD%\%OUTPUT%
exit /b 0
