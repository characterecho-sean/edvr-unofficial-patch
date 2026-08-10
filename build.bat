@echo off
setlocal enabledelayedexpansion
REM ===========================================================================
REM  EDVR build
REM
REM  Produces build\d3d11.dll -- a proxy that forwards every call to Windows'
REM  real d3d11.dll and adds one Direct3D copy per frame so both VR eyes share
REM  an exposure result.
REM
REM  Needs Visual Studio 2022 with the C++ workload, and Python (used only to
REM  generate the export thunks from the system d3d11.dll, so the proxy exports
REM  exactly what the original does).
REM
REM  Usage:  build.bat [--clean]
REM ===========================================================================

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "GEN=%BUILD%\gen"
set "OBJ=%BUILD%\obj"

REM The literal ")" in "Program Files (x86)" would close a parenthesised block.
set "PROGFILES86=%ProgramFiles(x86)%"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--clean" goto arg_clean
echo [edvr] unknown argument: %~1
exit /b 1
:arg_clean
set "DO_CLEAN=1"
shift
goto parse_args
:args_done

if defined DO_CLEAN (
    echo [edvr] cleaning
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
)

where cl.exe >nul 2>&1
if errorlevel 1 call :find_vs
if errorlevel 1 exit /b 1
where python.exe >nul 2>&1
if errorlevel 1 ( echo [edvr] ERROR: python is required to generate export thunks. & exit /b 1 )
goto toolchain_ok

:find_vs
set "VSWHERE=%PROGFILES86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vs
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\edvr_vspath.txt" 2>nul
if not exist "%TEMP%\edvr_vspath.txt" goto no_vs
set "VSPATH="
set /p VSPATH=<"%TEMP%\edvr_vspath.txt"
del "%TEMP%\edvr_vspath.txt" >nul 2>&1
if not defined VSPATH goto no_vs
echo [edvr] using %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo [edvr] ERROR: vcvars64 failed & exit /b 1 )
exit /b 0

:no_vs
echo [edvr] ERROR: no Visual Studio install with the x64 C++ toolset was found.
echo        Install the "Desktop development with C++" workload, or run this
echo        script from a "x64 Native Tools Command Prompt for VS".
exit /b 1

:toolchain_ok

if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%GEN%" mkdir "%GEN%"
if not exist "%OBJ%" mkdir "%OBJ%"

set CFLAGS=/nologo /c /O2 /MT /std:c++17 /EHsc /W4 /GR- ^
 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
 /I"%GEN%"

echo.
echo [edvr] === d3d11.dll ===
python "%ROOT%\tools\gen_exports.py" --source "%SystemRoot%\System32\d3d11.dll" ^
    --tag d3d11 --out "%GEN%" ^
    --wrap D3D11CreateDevice --wrap D3D11CreateDeviceAndSwapChain
if errorlevel 1 ( echo [edvr] ERROR: export generation failed & exit /b 1 )

ml64.exe /nologo /c /Fo"%OBJ%\thunks.obj" "%GEN%\edvr_thunks_d3d11.asm" >nul
if errorlevel 1 ( echo [edvr] ERROR: ml64 failed & exit /b 1 )

cl.exe %CFLAGS% /Fo"%OBJ%"\ ^
    "%ROOT%\src\common\log.cpp" "%ROOT%\src\common\config.cpp" ^
    "%ROOT%\src\common\guard.cpp" "%ROOT%\src\common\vtable_hook.cpp" ^
    "%ROOT%\src\common\hotkey.cpp" "%ROOT%\src\common\proxy.cpp" ^
    "%ROOT%\src\d3d11\d3d11_proxy.cpp" "%ROOT%\src\d3d11\device_hook.cpp" ^
    "%ROOT%\src\d3d11\exposure_fix.cpp" "%ROOT%\src\d3d11\vscreen.cpp" "%ROOT%\src\d3d11\vscreen_res.cpp"
if errorlevel 1 ( echo [edvr] ERROR: compile failed & exit /b 1 )

link.exe /nologo /DLL /MACHINE:X64 /INCREMENTAL:NO ^
    /DEF:"%GEN%\edvr_d3d11.def" /OUT:"%BUILD%\d3d11.dll" ^
    "%OBJ%\*.obj" kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: link failed & exit /b 1 )

echo [edvr] built %BUILD%\d3d11.dll

echo [edvr] === smoke.exe ===
if not exist "%OBJ%\smoke" mkdir "%OBJ%\smoke"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG ^
    /Fo"%OBJ%\smoke\\" /Fe"%BUILD%\smoke.exe" ^
    "%ROOT%\tools\smoke\smoke.cpp" /link /INCREMENTAL:NO d3d11.lib kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: smoke build failed & exit /b 1 )
echo [edvr] built %BUILD%\smoke.exe

echo [edvr] === fakechain.dll ===
if not exist "%OBJ%\fakechain" mkdir "%OBJ%\fakechain"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG /LD ^
    /Fo"%OBJ%\fakechain\\" /Fe"%BUILD%\fakechain.dll" ^
    "%ROOT%\tools\fakechain\fakechain.cpp" /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: fakechain build failed & exit /b 1 )
echo [edvr] built %BUILD%\fakechain.dll

echo.
echo [edvr] To install: copy build\d3d11.dll and edvr.ini next to
echo        EliteDangerous64.exe. See README.md.
echo.
echo [edvr] To check the build without the game:
echo        build\smoke.exe build\d3d11.dll
exit /b 0
