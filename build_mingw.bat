@echo off
where g++ >nul 2>nul
if not %errorlevel%==0 (
    echo g++ not found on PATH.
    echo Install a MinGW-w64 toolchain ^(e.g. w64devkit or MSYS2^) and re-run this script.
    exit /b 1
)

windres resource.rc -O coff -o resource_res.o
if not %errorlevel%==0 exit /b 1

g++ -O2 -std=c++17 -municode -mwindows -I. src\main.cpp resource_res.o -o PerfOverlay.exe -lpdh -lgdi32 -luser32 -ldxgi -lshell32 -ladvapi32 -ldwmapi

if %errorlevel%==0 (
    echo.
    echo Built PerfOverlay.exe
)
