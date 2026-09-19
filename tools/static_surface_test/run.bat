@echo off
setlocal enabledelayedexpansion
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "OUT=%ROOT%\build\obj\static_surface_test"
set "PROGFILES86=%ProgramFiles(x86)%"

where cl.exe >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%PROGFILES86%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" goto no_vs
    "!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\edvr_static_surface_vspath.txt" 2>nul
    if not exist "%TEMP%\edvr_static_surface_vspath.txt" goto no_vs
    set /p VSPATH=<"%TEMP%\edvr_static_surface_vspath.txt"
    del "%TEMP%\edvr_static_surface_vspath.txt" >nul 2>&1
    if not defined VSPATH goto no_vs
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
    if errorlevel 1 goto no_vs
)

if not exist "%OUT%" mkdir "%OUT%"
cl.exe /nologo /O2 /Gy /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OUT%\\" /Fe"%OUT%\static_surface_test.exe" ^
    "%ROOT%\tools\static_surface_test\static_surface_test.cpp" ^
    /link /INCREMENTAL:NO /OPT:REF d3d11.lib d3dcompiler.lib
if errorlevel 1 exit /b 1

cl.exe /nologo /O2 /Gy /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OUT%\\controller_" /Fe"%OUT%\controller_test.exe" ^
    "%ROOT%\tools\static_surface_test\controller_test.cpp" ^
    /link /INCREMENTAL:NO /OPT:REF d3d11.lib d3dcompiler.lib
if errorlevel 1 exit /b 1

set "CORPUS=C:\Steam\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64\edvr_logs"
if exist "%CORPUS%\shaders\ps_CB9F297EFF264251.dxbc" (
    "%OUT%\static_surface_test.exe" "%CORPUS%"
) else (
    "%OUT%\static_surface_test.exe"
)
if errorlevel 1 exit /b 1
"%OUT%\controller_test.exe"
exit /b %errorlevel%

:no_vs
echo [edvr] ERROR: no Visual Studio x64 C++ toolset found.
exit /b 1
