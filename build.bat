@echo off
setlocal

where cl >nul 2>nul
if %errorlevel%==0 goto :compile

for %%v in (Enterprise Professional Community BuildTools) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%v\VC\Auxiliary\Build\vcvars64.bat" (
        call "%ProgramFiles%\Microsoft Visual Studio\2022\%%v\VC\Auxiliary\Build\vcvars64.bat"
        goto :compile
    )
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%v\VC\Auxiliary\Build\vcvars64.bat" (
        call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%v\VC\Auxiliary\Build\vcvars64.bat"
        goto :compile
    )
)

echo Could not find MSVC (cl.exe).
echo Install the "Desktop development with C++" workload via Visual Studio Installer,
echo or install standalone Build Tools for Visual Studio, then re-run this script.
exit /b 1

:compile
rc.exe /nologo /fo resource.res resource.rc
if not %errorlevel%==0 exit /b 1

cl /nologo /O2 /EHsc /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I. ^
    src\main.cpp resource.res ^
    /Fe:PerfOverlay.exe ^
    /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib pdh.lib dxgi.lib shell32.lib advapi32.lib

if %errorlevel%==0 (
    echo.
    echo Built PerfOverlay.exe
)
