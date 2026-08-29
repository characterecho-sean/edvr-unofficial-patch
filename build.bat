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
set "OPENVR_EXPLICIT=1"
shift
shift
goto parse_args
:args_done

REM A path given explicitly must exist. The fallback below is for finding the
REM game's copy automatically; applying it to a typo'd --openvr instead built the
REM export table from a DIFFERENT openvr build, and every export the two did not
REM share then resolved to the do-nothing stub at runtime -- a silent wrong
REM answer in the one case where the user had been specific.
if defined OPENVR_EXPLICIT (
    if not exist "%OPENVR_SRC%" (
        echo [edvr] ERROR: --openvr path does not exist: %OPENVR_SRC%
        exit /b 1
    )
)

REM Where the game keeps its own copy, if --openvr was not given.
REM
REM The RENAMED ORIGINAL first: on an installed rig, Openvr\win64\
REM openvr_api.dll IS the EDVR proxy from the last install, and generating
REM the export table from our own proxy builds a proxy of a proxy -- the
REM .def names our extra exports twice and the linker aliases one to a
REM forwarding thunk (measured 2026-08-18; gen_exports.py now refuses such
REM a source outright). openvr_api_orig.dll is the true runtime whenever
REM the install steps have run, and absent before them, where the unrenamed
REM openvr_api.dll is still genuine.
if not defined OPENVR_SRC (
    set "OPENVR_GAMEDIR=%LOCALAPPDATA%\Frontier_Developments\Products\elite-dangerous-odyssey-64\Openvr\win64"
    if exist "!OPENVR_GAMEDIR!\openvr_api_orig.dll" (
        set "OPENVR_SRC=!OPENVR_GAMEDIR!\openvr_api_orig.dll"
    ) else (
        set "OPENVR_SRC=!OPENVR_GAMEDIR!\openvr_api.dll"
    )
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

REM The version baked into both DLLs, printed in the second line of every log.
REM
REM `git describe` rather than a hand-maintained constant, because the constant
REM would be wrong exactly when it matters: isolating which release a field log
REM came from used to mean correlating the link stamp against tag dates by hand
REM (done during the 2026-08-18 OpenXR Toolkit triage, twenty minutes for a
REM fact the DLL always knew). A clean tag prints as v0.7.3; a dev build names
REM itself v0.7.3-2-g650d8a9 and uncommitted changes append -dirty, so a log
REM from a build that was never a release SAYS so instead of impersonating one.
REM No git or no repo (a source-zip build) falls back to "unknown" and the
REM build carries on -- versioning must never be the reason a build fails.
set "EDVR_VER=unknown"
for /f "delims=" %%v in ('git -C "%ROOT%" describe --tags --always --dirty 2^>nul') do set "EDVR_VER=%%v"
echo [edvr] version %EDVR_VER%

set CFLAGS=/nologo /c /O2 /MT /std:c++17 /EHsc /W4 /GR- ^
 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
 /DEDVR_VERSION_STRING=\"%EDVR_VER%\" ^
 /I"%GEN%"

echo.
REM The runtime config audit data: known keys + the moved-from map,
REM generated from the same sources the late contract check verifies.
python "%ROOT%\tools\check_config_contract.py" --quiet --emit "%GEN%\config_contract_gen.h"
if errorlevel 1 ( echo [edvr] ERROR: contract header generation failed & exit /b 1 )

echo [edvr] === d3d11.dll ===
REM AMD FSR 1.0 as embeddable HLSL. Generated rather than committed so the
REM vendored headers stay byte-identical to upstream (src\d3d11\fsr\).
python "%ROOT%\tools\gen_fsr_hlsl.py" --root "%ROOT%" --out "%GEN%"
if errorlevel 1 ( echo [edvr] ERROR: FSR shader embedding failed & exit /b 1 )

python "%ROOT%\tools\gen_exports.py" --source "%SystemRoot%\System32\d3d11.dll" ^
    --tag d3d11 --out "%GEN%" ^
    --wrap D3D11CreateDevice --wrap D3D11CreateDeviceAndSwapChain ^
    --extra-export edvr_selftest_hooks ^
    --extra-export edvr_selftest_scene_draws ^
    --extra-export edvrFssHealLeft ^
    --extra-export edvrFssTheater
if errorlevel 1 ( echo [edvr] ERROR: export generation failed & exit /b 1 )

if not exist "%OBJ%\d3d11" mkdir "%OBJ%\d3d11"
ml64.exe /nologo /c /Fo"%OBJ%\d3d11\thunks.obj" "%GEN%\edvr_thunks_d3d11.asm" >nul
if errorlevel 1 ( echo [edvr] ERROR: ml64 failed & exit /b 1 )

cl.exe %CFLAGS% /Fo"%OBJ%\d3d11"\ ^
    "%ROOT%\src\common\log.cpp" "%ROOT%\src\common\config.cpp" ^
    "%ROOT%\src\common\config_audit.cpp" ^
    "%ROOT%\src\common\guard.cpp" "%ROOT%\src\common\vtable_hook.cpp" ^
    "%ROOT%\src\common\hotkey.cpp" "%ROOT%\src\common\proxy.cpp" ^
    "%ROOT%\src\common\frame_flag.cpp" ^
    "%ROOT%\src\d3d11\d3d11_proxy.cpp" "%ROOT%\src\d3d11\device_hook.cpp" ^
    "%ROOT%\src\d3d11\exposure_fix.cpp" "%ROOT%\src\d3d11\vscreen.cpp" ^
    "%ROOT%\src\d3d11\glitch_frame.cpp" "%ROOT%\src\d3d11\vscreen_res.cpp" ^
    "%ROOT%\src\d3d11\binding_shadow.cpp" "%ROOT%\src\d3d11\head_offset_gate.cpp" ^
    "%ROOT%\src\d3d11\camera_view.cpp" "%ROOT%\src\d3d11\journal_watch.cpp" ^
    "%ROOT%\src\d3d11\elite_binds.cpp" "%ROOT%\src\d3d11\draw_census.cpp" ^
    "%ROOT%\src\d3d11\fss_res.cpp" "%ROOT%\src\d3d11\fss_scan.cpp" ^
    "%ROOT%\src\d3d11\fss_panel.cpp" "%ROOT%\src\d3d11\fss_probe.cpp" ^
    "%ROOT%\src\d3d11\fss_reveal.cpp" "%ROOT%\src\d3d11\fss_ring.cpp" ^
    "%ROOT%\src\d3d11\fss_dump.cpp" "%ROOT%\src\d3d11\fss_heal.cpp" ^
    "%ROOT%\src\d3d11\fss_theater.cpp" ^
    "%ROOT%\src\d3d11\xinput_watch.cpp" ^
    "%ROOT%\src\d3d11\fss_panel_rect.cpp" ^
    "%ROOT%\src\d3d11\panel_quad.cpp" "%ROOT%\src\d3d11\panel_curve.cpp" ^
    "%ROOT%\src\d3d11\shader_sig.cpp" ^
    "%ROOT%\src\d3d11\remlok_fix.cpp" "%ROOT%\src\d3d11\holo_fix.cpp" ^
    "%ROOT%\src\d3d11\backdrop_fix.cpp" ^
    "%ROOT%\src\d3d11\scrim_fix.cpp" ^
    "%ROOT%\src\d3d11\quad_probe.cpp" ^
    "%ROOT%\src\d3d11\intro_probe.cpp" ^
    "%ROOT%\src\d3d11\intro_panel.cpp" ^
    "%ROOT%\src\d3d11\intro_upscale.cpp" ^
    "%ROOT%\src\d3d11\loader_panel.cpp" ^
    "%ROOT%\src\d3d11\witchstar_fix.cpp" "%ROOT%\src\d3d11\fov_probe.cpp" ^
    "%ROOT%\src\d3d11\cb_peek.cpp" "%ROOT%\src\d3d11\billboard_fix.cpp" ^
    "%ROOT%\src\d3d11\particle_fix.cpp" "%ROOT%\src\d3d11\shader_swap.cpp" "%ROOT%\src\d3d11\sunglare_fix.cpp"
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
    --tag openvr --out "%GEN%" --wrap VR_GetGenericInterface --lazy ^
    --extra-export edvr_selftest_system_hook ^
    --extra-export edvr_selftest_cull_guard
if errorlevel 1 ( echo [edvr] ERROR: openvr export generation failed & exit /b 1 )

REM The lazy shim MUST carry unwind info.
REM
REM It moves rsp by 128 and then calls. x64 exception handling walks the stack
REM with the .pdata tables, and a function with no entry is assumed to be a leaf
REM whose return address sits at [rsp] -- so an unwinder would read it out of the
REM shim's own shadow space and every SEH handler above would be skipped. That
REM shipped once, with thunks.obj contributing no .pdata at all.
REM
REM Asserted here rather than at runtime because the runtime paths that would
REM expose it are not reachable from a test: a faulting DllMain is caught inside
REM LoadLibrary and never propagates. Checking the generated source is exact.
findstr /C:".ENDPROLOG" "%GEN%\edvr_thunks_openvr.asm" >nul
if errorlevel 1 (
    echo [edvr] ERROR: the lazy openvr shim has no unwind directives.
    echo        A fault inside it would be uncatchable. See ASM_LAZY_HEAD in
    echo        tools\gen_exports.py.
    exit /b 1
)

if not exist "%OBJ%\openvr" mkdir "%OBJ%\openvr"
ml64.exe /nologo /c /Fo"%OBJ%\openvr\thunks.obj" "%GEN%\edvr_thunks_openvr.asm" >nul
if errorlevel 1 ( echo [edvr] ERROR: ml64 failed for openvr & exit /b 1 )

REM Hand-written, unlike the generated thunks above: the two IVRSystem_012
REM slots that return structs by value, observed by register-preserving
REM tail-jump thunks because no C signature can receive both calling
REM conventions (see system_thunks.asm). No rsp movement, so the unwind-info
REM assertion on the generated shim deliberately does not apply here.
ml64.exe /nologo /c /Fo"%OBJ%\openvr\systhunks.obj" "%ROOT%\src\openvr\system_thunks.asm" >nul
if errorlevel 1 ( echo [edvr] ERROR: ml64 failed for system_thunks & exit /b 1 )

cl.exe %CFLAGS% /Fo"%OBJ%\openvr"\ ^
    "%ROOT%\src\common\log.cpp" "%ROOT%\src\common\config.cpp" ^
    "%ROOT%\src\common\config_audit.cpp" ^
    "%ROOT%\src\common\guard.cpp" "%ROOT%\src\common\vtable_hook.cpp" ^
    "%ROOT%\src\common\hotkey.cpp" "%ROOT%\src\common\proxy.cpp" ^
    "%ROOT%\src\common\frame_flag.cpp" ^
    "%ROOT%\src\openvr\openvr_proxy.cpp" "%ROOT%\src\openvr\compositor_hook.cpp" ^
    "%ROOT%\src\openvr\head_offset.cpp" "%ROOT%\src\openvr\resubmit_shadow.cpp" ^
    "%ROOT%\src\openvr\system_hook.cpp" "%ROOT%\src\openvr\guard_crop.cpp" ^
    "%ROOT%\src\openvr\early_session.cpp" ^
    "%ROOT%\src\d3d11\elite_binds.cpp"
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

echo.
echo [edvr] === edvr-installer.exe ===
REM One executable carrying d3d11.dll, edvr.ini and -- when this build has it --
REM openvr_api.dll, as resources.
REM
REM Built AFTER both DLLs, because it embeds whatever they are at this moment.
REM A build that skipped the openvr half above produces an installer that says
REM so in its window rather than one that fails to link: the .rc is generated,
REM and the missing file is simply not named in it.
REM
REM /MANIFEST:NO is not optional. link.exe embeds a manifest of its own by
REM default, and our .rc already puts one at resource 1 -- two RT_MANIFEST
REM resources in one image, which Windows refuses to start at all: "the
REM side-by-side configuration is incorrect", before a line of our code runs.
REM It links and packages perfectly happily.
if not exist "%OBJ%\installer" mkdir "%OBJ%\installer"
python "%ROOT%\tools\gen_installer_rc.py" --root "%ROOT%" --build "%BUILD%" ^
    --out "%GEN%" --version "%EDVR_VER%"
if errorlevel 1 ( echo [edvr] ERROR: installer resource generation failed & exit /b 1 )

REM The settings window's contents, generated from edvr.ini and the accessor
REM calls in src\ -- and the gate that keeps it complete.
REM
REM A setting that is uncommented in edvr.ini is one this build ships ON. If it
REM is not also reachable from the settings window, it is invisible to everybody
REM who does not edit ini files, and nothing else in the build would notice: the
REM game reads it, the log names it, and the window that is supposed to expose
REM it simply does not. One annotation line above the key is what this asks for,
REM and it fails the build until it is there.
python "%ROOT%\tools\gen_settings_schema.py" --root "%ROOT%" --out "%GEN%"
if errorlevel 1 (
    echo [edvr] ERROR: the settings schema is incomplete ^(see above^)
    exit /b 1
)

rc.exe /nologo /fo "%OBJ%\installer\payload.res" "%GEN%\payload.rc"
if errorlevel 1 ( echo [edvr] ERROR: rc.exe failed on the installer resources & exit /b 1 )

set INSTALLER_SRC="%ROOT%\src\installer\main.cpp" "%ROOT%\src\installer\gui.cpp" ^
    "%ROOT%\src\installer\ui.cpp" "%ROOT%\src\installer\settings.cpp" ^
    "%ROOT%\src\installer\settings_view.cpp" "%ROOT%\src\installer\logbundle.cpp" ^
    "%ROOT%\src\installer\app.cpp" "%ROOT%\src\installer\plan.cpp" ^
    "%ROOT%\src\installer\apply.cpp" "%ROOT%\src\installer\detect.cpp" ^
    "%ROOT%\src\installer\probe.cpp" "%ROOT%\src\installer\iniedit.cpp" ^
    "%ROOT%\src\installer\state.cpp" "%ROOT%\src\installer\payload.cpp"
set INSTALLER_LIBS=user32.lib gdi32.lib gdiplus.lib dwmapi.lib uxtheme.lib ^
    shell32.lib ole32.lib comctl32.lib advapi32.lib version.lib bcrypt.lib kernel32.lib

cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /GR- /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /I"%GEN%" ^
    /DEDVR_VERSION_STRING=\"%EDVR_VER%\" ^
    /Fo"%OBJ%\installer"\ /Fe"%BUILD%\edvr-installer.exe" ^
    %INSTALLER_SRC% "%OBJ%\installer\payload.res" ^
    /link /INCREMENTAL:NO /SUBSYSTEM:WINDOWS /MANIFEST:NO %INSTALLER_LIBS%
if errorlevel 1 ( echo [edvr] ERROR: installer build failed & exit /b 1 )
echo [edvr] built %BUILD%\edvr-installer.exe

REM Does it START? Not a formality: a manifest Windows cannot parse, a missing
REM import, the wrong subsystem -- each of these produces an executable that
REM links without a murmur and dies before main(), with a dialog the build never
REM sees. --help reads nothing and writes nothing.
"%BUILD%\edvr-installer.exe" --help >nul || (
    echo [edvr] ERROR: the installer will not run. If Windows called it a
    echo        side-by-side configuration problem, the manifest is the suspect.
    exit /b 1
)

echo [edvr] === installer_test.exe ===
REM The planner over folders that are hard to arrange on a real machine: EDHM
REM already in the d3d11.dll slot, another mod's installer having overwritten
REM ours, a game update that put the stock openvr_api.dll back, an original
REM runtime lost to a double rename -- and the edvr.ini merge, which is the one
REM piece whose failure silently discards settings somebody tuned in a headset.
if not exist "%OBJ%\insttest" mkdir "%OBJ%\insttest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /GR- /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /I"%GEN%" ^
    /Fo"%OBJ%\insttest"\ /Fe"%BUILD%\installer_test.exe" ^
    "%ROOT%\tools\installer_test\installer_test.cpp" ^
    "%ROOT%\src\installer\plan.cpp" "%ROOT%\src\installer\apply.cpp" ^
    "%ROOT%\src\installer\detect.cpp" "%ROOT%\src\installer\probe.cpp" ^
    "%ROOT%\src\installer\iniedit.cpp" "%ROOT%\src\installer\state.cpp" ^
    "%ROOT%\src\installer\settings.cpp" "%ROOT%\src\installer\logbundle.cpp" ^
    /link /INCREMENTAL:NO %INSTALLER_LIBS%
if errorlevel 1 ( echo [edvr] ERROR: installer_test build failed & exit /b 1 )
"%BUILD%\installer_test.exe" "%ROOT%" "%BUILD%\insttest_scratch" || (
    echo [edvr] ERROR: the installer failed its own tests
    exit /b 1
)


REM Gated with `||`, not `if errorlevel 1`.
REM
REM `if errorlevel N` means "exit code >= N", and a process killed by an access
REM violation exits with a negative NTSTATUS -- so it read a CRASH as success.
REM Measured. `%errorlevel%` is no good either: these gates sit inside
REM parenthesised blocks, where it expands once at parse time. `||` keys off the
REM command's own exit code and is immune to both.
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
echo [edvr] === vtable_test.exe ===
REM The object-wrapping collision (issue #6), without needing ReShade. These
REM cells were written against the copy-and-swap-vptr mechanism and FAILED on
REM it, which is the only reason to believe them now.
if not exist "%OBJ%\vtabletest" mkdir "%OBJ%\vtabletest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\vtabletest"\ ^
    /Fe"%BUILD%\vtable_test.exe" "%ROOT%\tools\vtable_test\vtable_test.cpp" ^
    "%ROOT%\src\common\vtable_hook.cpp" "%ROOT%\src\common\guard.cpp" ^
    "%ROOT%\src\common\log.cpp" "%ROOT%\src\common\config.cpp" ^
    "%ROOT%\src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: vtable_test build failed & exit /b 1 )
"%BUILD%\vtable_test.exe" || (
    echo [edvr] ERROR: vtable hooking does not compose with object wrappers
    exit /b 1
)

echo [edvr] === config_test.exe ===
REM The real parser over the real shipped edvr.ini. The file's own layout
REM depends on two parser properties -- repeated section headers, last value
REM wins -- that were originally read out of config.cpp rather than observed,
REM and every symptom of either being false shows up in the game rather than
REM in a build.
if not exist "%OBJ%\cfgtest" mkdir "%OBJ%\cfgtest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\cfgtest"\ ^
    /Fe"%BUILD%\config_test.exe" "%ROOT%\tools\config_test\config_test.cpp" ^
    "%ROOT%\src\common\config.cpp" "%ROOT%\src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: config_test build failed & exit /b 1 )
"%BUILD%\config_test.exe" "%ROOT%" "%BUILD%\cfgscratch" || (
    echo [edvr] ERROR: the shipped edvr.ini does not parse as documented
    exit /b 1
)

echo [edvr] === gate_test.exe ===
if not exist "%OBJ%\gatetest" mkdir "%OBJ%\gatetest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\gatetest"\ /Fe"%BUILD%\gate_test.exe" ^
    "%ROOT%\tools\gate_test\gate_test.cpp" ^
    "%ROOT%\src\d3d11\head_offset_gate.cpp" "%ROOT%\src\common\config.cpp" ^
    "%ROOT%\src\common\log.cpp" "%ROOT%\src\common\frame_flag.cpp" ^
    "%ROOT%\src\d3d11\camera_view.cpp" "%ROOT%\src\common\guard.cpp" ^
    "%ROOT%\src\common\proxy.cpp" "%ROOT%\src\d3d11\journal_watch.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: gate_test build failed & exit /b 1 )
"%BUILD%\gate_test.exe" "%ROOT%" || (
    echo [edvr] ERROR: the head-offset gate arms where it should not
    exit /b 1
)

echo [edvr] === glitch_test.exe ===
REM The transition-flash detector, replayed without the game. This repo SHIPS
REM that fix, and until now had no way to run its test -- which is how a signal
REM the private ledger had already refuted stayed in a release until a user felt
REM it as judder on a planet surface.
if not exist "%OBJ%\glitchtest" mkdir "%OBJ%\glitchtest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\glitchtest"\ ^
    /Fe"%BUILD%\glitch_test.exe" "%ROOT%\tools\glitch_test\glitch_test.cpp" ^
    "%ROOT%\src\d3d11\glitch_frame.cpp" "%ROOT%\src\common\config.cpp" ^
    "%ROOT%\src\common\frame_flag.cpp" "%ROOT%\src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: glitch_test build failed & exit /b 1 )
"%BUILD%\glitch_test.exe" "%BUILD%\glitchscratch" || (
    echo [edvr] ERROR: the transition flash detector failed its own test
    exit /b 1
)

echo [edvr] === pose_test.exe ===
if not exist "%OBJ%\posetest" mkdir "%OBJ%\posetest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\posetest"\ ^
    /Fe"%BUILD%\pose_test.exe" "%ROOT%\tools\pose_test\pose_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: pose_test build failed & exit /b 1 )
"%BUILD%\pose_test.exe" || (
    echo [edvr] ERROR: the head pose arithmetic is wrong
    exit /b 1
)

echo [edvr] === fakevr.dll + openvr_smoke.exe ===
if not exist "%OBJ%\fakevr" mkdir "%OBJ%\fakevr"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG /LD ^
    /Fo"%OBJ%\fakevr\\" /Fe"%BUILD%\fakevr.dll" ^
    "%ROOT%\tools\fakevr\fakevr.cpp" /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: fakevr build failed & exit /b 1 )
if not exist "%OBJ%\openvrsmoke" mkdir "%OBJ%\openvrsmoke"
REM Links the shared guard, and what guard.cpp needs, because the harness now
REM also exercises the crash sentinel -- shared code whose two bugs were a
REM silent arm failure and a permanent lockout, neither of which shows up as a
REM crash or a wrong pixel.
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\openvrsmoke\\" /Fe"%BUILD%\openvr_smoke.exe" ^
    "%ROOT%\tools\openvr_smoke\openvr_smoke.cpp" ^
    "%ROOT%\src\common\guard.cpp" "%ROOT%\src\common\log.cpp" ^
    "%ROOT%\src\common\config.cpp" "%ROOT%\src\common\proxy.cpp" ^
    "%ROOT%\src\common\frame_flag.cpp" "%ROOT%\src\common\hotkey.cpp" ^
    "%ROOT%\src\openvr\resubmit_shadow.cpp" "%ROOT%\src\openvr\guard_crop.cpp" ^
    "%ROOT%\src\d3d11\elite_binds.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib d3d11.lib
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
    "%BUILD%\openvr_smoke.exe" "%BUILD%\vrtest" || (
        echo [edvr] ERROR: openvr startup test failed or crashed
        exit /b 1
    )
)

REM Do the code, edvr.ini and the log messages agree about setting names?
REM
REM Three settings were read from the wrong section for the whole of 0.5.x and
REM nothing anywhere said so, because a key that does not match is simply not
REM there and a missing key falls back to its default. Only run when python is
REM available; a missing interpreter must not stop a build.
echo.
echo [edvr] === install-read check ===
python "%ROOT%\tools\check_install_reads.py"
if errorlevel 1 ( echo [edvr] ERROR: a config reader runs only on the reload path & exit /b 1 )

echo [edvr] === exit-path check ===
python "%ROOT%\tools\check_exit_paths.py"
if errorlevel 1 ( echo [edvr] ERROR: cleanup that matters runs only on FreeLibrary & exit /b 1 )

echo [edvr] === draw census diff self-test ===
REM The tool that reads the census a field session paid for. A parser that
REM drifts from the DC line format fails HERE, not in the ten minutes after a
REM user finally reproduced the effect being chased.
python "%ROOT%\tools\diff_draw_census.py" --self-test || (
    echo [edvr] ERROR: the census diff tool failed its own test
    exit /b 1
)

echo [edvr] === config contract ===
where python >nul 2>&1
if errorlevel 1 (
    echo [edvr] NOTE: python not found, skipping the config contract check
) else (
    python "%ROOT%\tools\check_config_contract.py" || (
        echo [edvr] ERROR: config contract check failed or crashed
        exit /b 1
    )
)

echo.
echo [edvr] To install: copy build\d3d11.dll and edvr.ini next to
echo        EliteDangerous64.exe, and build\openvr_api.dll into
echo        Openvr\win64, replacing the game's file of that name --
echo        the original must already be renamed openvr_api_orig.dll.
echo        The two halves do NOT go in the same place. See README.md.
echo.
echo [edvr] Or hand somebody build\edvr-installer.exe: it carries the two
echo        DLLs and edvr.ini, finds Steam, Epic and Frontier installs,
echo        keeps their edvr.ini settings and repairs a clobbered install.
echo.
echo [edvr] To check the build without the game:
echo        build\smoke.exe build\d3d11.dll
exit /b 0
