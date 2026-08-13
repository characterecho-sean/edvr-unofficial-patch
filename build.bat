@echo off
setlocal enabledelayedexpansion
REM ===========================================================================
REM  EDVR build
REM
REM  Produces:
REM    build\d3d11.dll        the fixes themselves
REM    build\openvr_api.dll   optional; only needed for the transition flash fix
REM
REM  The openvr proxy needs the game's own openvr_api.dll to generate a matching
REM  export table, since -- unlike d3d11.dll -- it is not a Windows component and
REM  there is no system copy to read. It is looked for in the usual install
REM  location, or pass --openvr <path>. Without it the d3d11 proxy still builds
REM  and every other fix still works.
REM
REM  Needs Visual Studio 2022 with the C++ workload, and Python (used only to
REM  generate export thunks, so each proxy exports exactly what the original
REM  does).
REM
REM  Usage:  build.bat [--openvr <path-to-openvr_api.dll>] [--clean]
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
if /I "%~1"=="--openvr" goto arg_openvr
echo [edvr] unknown argument: %~1
exit /b 1
:arg_clean
set "DO_CLEAN=1"
shift
goto parse_args
:arg_openvr
set "OPENVR_SRC=%~2"
shift
shift
goto parse_args
:args_done

REM Where the game keeps its own copy, if --openvr was not given.
if not defined OPENVR_SRC (
    set "OPENVR_SRC=%LOCALAPPDATA%\Frontier_Developments\Products\elite-dangerous-odyssey-64\Openvr\win64\openvr_api.dll"
)
if not exist "%OPENVR_SRC%" (
    if exist "%ROOT%\reference\openvr_api.dll" set "OPENVR_SRC=%ROOT%\reference\openvr_api.dll"
)

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

if not exist "%OBJ%\d3d11" mkdir "%OBJ%\d3d11"
ml64.exe /nologo /c /Fo"%OBJ%\d3d11\thunks.obj" "%GEN%\edvr_thunks_d3d11.asm" >nul
if errorlevel 1 ( echo [edvr] ERROR: ml64 failed & exit /b 1 )

cl.exe %CFLAGS% /Fo"%OBJ%\d3d11"\ ^
    "%ROOT%\src\common\log.cpp" "%ROOT%\src\common\config.cpp" ^
    "%ROOT%\src\common\guard.cpp" "%ROOT%\src\common\vtable_hook.cpp" ^
    "%ROOT%\src\common\hotkey.cpp" "%ROOT%\src\common\proxy.cpp" ^
    "%ROOT%\src\common\frame_flag.cpp" ^
    "%ROOT%\src\d3d11\d3d11_proxy.cpp" "%ROOT%\src\d3d11\device_hook.cpp" ^
    "%ROOT%\src\d3d11\exposure_fix.cpp" "%ROOT%\src\d3d11\vscreen.cpp" ^
    "%ROOT%\src\d3d11\glitch_frame.cpp" "%ROOT%\src\d3d11\vscreen_res.cpp"
if errorlevel 1 ( echo [edvr] ERROR: compile failed & exit /b 1 )

link.exe /nologo /DLL /MACHINE:X64 /INCREMENTAL:NO ^
    /DEF:"%GEN%\edvr_d3d11.def" /OUT:"%BUILD%\d3d11.dll" ^
    "%OBJ%\d3d11\*.obj" kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: link failed & exit /b 1 )

echo [edvr] built %BUILD%\d3d11.dll

echo.
echo [edvr] === openvr_api.dll ===
if not exist "%OPENVR_SRC%" goto no_openvr

REM --lazy: this proxy must not load its real module from DllMain. The d3d11
REM side can, because the system d3d11.dll is already mapped and the call only
REM bumps a refcount; openvr_api_orig.dll is mapped by nothing, so loading it
REM there runs its DllMain under the loader lock.
python "%ROOT%\tools\gen_exports.py" --source "%OPENVR_SRC%" ^
    --tag openvr --out "%GEN%" --wrap VR_GetGenericInterface --lazy
if errorlevel 1 ( echo [edvr] ERROR: openvr export generation failed & exit /b 1 )

if not exist "%OBJ%\openvr" mkdir "%OBJ%\openvr"
ml64.exe /nologo /c /Fo"%OBJ%\openvr\thunks.obj" "%GEN%\edvr_thunks_openvr.asm" >nul
if errorlevel 1 ( echo [edvr] ERROR: ml64 failed for openvr & exit /b 1 )

cl.exe %CFLAGS% /Fo"%OBJ%\openvr"\ ^
    "%ROOT%\src\common\log.cpp" "%ROOT%\src\common\config.cpp" ^
    "%ROOT%\src\common\guard.cpp" "%ROOT%\src\common\vtable_hook.cpp" ^
    "%ROOT%\src\common\hotkey.cpp" "%ROOT%\src\common\proxy.cpp" ^
    "%ROOT%\src\common\frame_flag.cpp" ^
    "%ROOT%\src\openvr\openvr_proxy.cpp" "%ROOT%\src\openvr\compositor_hook.cpp"
if errorlevel 1 ( echo [edvr] ERROR: openvr compile failed & exit /b 1 )

link.exe /nologo /DLL /MACHINE:X64 /INCREMENTAL:NO ^
    /DEF:"%GEN%\edvr_openvr.def" /OUT:"%BUILD%\openvr_api.dll" ^
    "%OBJ%\openvr\*.obj" kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: openvr link failed & exit /b 1 )

echo [edvr] built %BUILD%\openvr_api.dll
goto openvr_done

:no_openvr
echo [edvr] SKIPPED: no openvr_api.dll to generate exports from.
echo        Looked for the game's copy, and reference\openvr_api.dll.
echo        Pass --openvr ^<path^> to build it. Everything except the
echo        transition flash fix works without it.
:openvr_done

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

echo [edvr] === fakevr.dll + openvr_smoke.exe ===
if not exist "%OBJ%\fakevr" mkdir "%OBJ%\fakevr"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG /LD ^
    /Fo"%OBJ%\fakevr\\" /Fe"%BUILD%\fakevr.dll" ^
    "%ROOT%\tools\fakevr\fakevr.cpp" /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: fakevr build failed & exit /b 1 )
if not exist "%OBJ%\openvrsmoke" mkdir "%OBJ%\openvrsmoke"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG ^
    /Fo"%OBJ%\openvrsmoke\\" /Fe"%BUILD%\openvr_smoke.exe" ^
    "%ROOT%\tools\openvr_smoke\openvr_smoke.cpp" /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: openvr_smoke build failed & exit /b 1 )
echo [edvr] built %BUILD%\openvr_smoke.exe

REM Run it. A startup test nothing runs is a startup test that rots -- the
REM fakechain harness exists because the loader-lock crash shipped once, and the
REM openvr side then recreated the same hazard with no equivalent check.
REM Skipped when the openvr proxy was not built.
if exist "%BUILD%\openvr_api.dll" (
    if not exist "%BUILD%\vrtest" mkdir "%BUILD%\vrtest"
    copy /Y "%BUILD%\openvr_api.dll" "%BUILD%\vrtest\openvr_api.dll" >nul
    copy /Y "%BUILD%\fakevr.dll" "%BUILD%\vrtest\openvr_api_orig.dll" >nul
    "%BUILD%\openvr_smoke.exe" "%BUILD%\vrtest"
    if errorlevel 1 ( echo [edvr] ERROR: openvr startup test failed & exit /b 1 )
)

REM Do the code, edvr.ini and the log messages agree about setting names?
REM
REM Three settings were read from the wrong section for the whole of 0.5.x and
REM nothing anywhere said so, because a key that does not match is simply not
REM there and a missing key falls back to its default. Only run when python is
REM available; a missing interpreter must not stop a build.
echo [edvr] === config contract ===
where python >nul 2>&1
if errorlevel 1 (
    echo [edvr] NOTE: python not found, skipping the config contract check
) else (
    python "%ROOT%\tools\check_config_contract.py"
    if errorlevel 1 ( echo [edvr] ERROR: config contract check failed & exit /b 1 )
)

echo.
echo [edvr] To install: copy build\d3d11.dll and edvr.ini next to
echo        EliteDangerous64.exe. See README.md.
echo.
echo [edvr] To check the build without the game:
echo        build\smoke.exe build\d3d11.dll
exit /b 0
