@echo off
rem ---------------------------------------------------------------------------
rem  Lilac build script (MSVC). Produces build\Lilac.exe
rem  The CRT is linked statically (/MT): no VC++ redistributable is required.
rem  Usage: build.bat [debug]
rem ---------------------------------------------------------------------------
setlocal enabledelayedexpansion
cd /d "%~dp0"

where cl >nul 2>nul
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo [error] Visual Studio / Build Tools not found.
        exit /b 1
    )
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
    if not defined VSINSTALL (
        echo [error] MSVC x64 toolset not found.
        exit /b 1
    )
    call "!VSINSTALL!\VC\Auxiliary\Build\vcvars64.bat" >nul
)

if not exist build mkdir build
if not exist build\obj mkdir build\obj

set "CFLAGS=/nologo /c /utf-8 /W3 /MT /O2 /GS /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS"
if /i "%1"=="debug" set "CFLAGS=/nologo /c /utf-8 /W3 /MTd /Od /Zi /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS"

set "LIBS=user32.lib gdi32.lib comdlg32.lib comctl32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib uuid.lib windowscodecs.lib advapi32.lib msimg32.lib urlmon.lib winspool.lib"

rem Sync icon.ico if placed in the project root
if exist icon.ico copy /y icon.ico res\icon.ico >nul

cl %CFLAGS% /Fobuild\obj\ src\*.c || exit /b 1
cl %CFLAGS% /wd4244 /wd4267 /wd4018 /Fobuild\obj\md4c.obj third_party\md4c\md4c.c || exit /b 1
rc /nologo /i res /i . /fobuild\obj\resource.res res\resource.rc || exit /b 1

link /nologo /OUT:build\Lilac.exe /SUBSYSTEM:WINDOWS /MANIFEST:NO build\obj\*.obj build\obj\resource.res %LIBS% || exit /b 1
if /i "%1"=="debug" (echo built build\Lilac.exe [debug]) else (echo built build\Lilac.exe)
endlocal
