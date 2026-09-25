@echo off
setlocal enabledelayedexpansion
REM ===========================================================================
REM  EDVR build
REM
REM  Produces:
REM    build\d3d11.dll        native OpenXR graphics and fixes
REM    build\openvr_api.dll   native OpenXR compatibility ABI for Elite
REM    build\openxr_loader.dll   pinned Khronos loader; Windows selects runtime
REM
REM  Needs Visual Studio 2022 C++ and Python. Fetch the pinned loader once with
REM  python tools\fetch_openxr_loader.py. The build verifies it offline.
REM
REM  Usage:  build.bat [--clean] [--jobs N] [--dll-only]
REM
REM  Once the DLLs are built, the test rigs run concurrently, --jobs at a time
REM  (default: one per logical core), through tools\run_jobs.py. Each rig is a
REM  :rig_<label> subroutine at the end of this file; the runner starts it as
REM  "build.bat --rig <label>", a child that inherits this build's environment
REM  and runs that one subroutine. --rig is the runner's, not for hand use.
REM  --dll-only is the post-commit promotion path: it requires the receipt from
REM  a matching green full build, rebuilds the production DLLs, and skips rigs
REM  and the self-contained installer.
REM
REM  Only one top-level build runs at a time on this machine (tools\
REM  build_lock.py): a second invocation is refused immediately with a
REM  message saying to wait, rather than contending with the first one for
REM  the same CPU cores -- the documented cause of the vtable_test timing
REM  gate's flakes under load. --rig children run concurrently by design
REM  under the parent's own lock and are unaffected.
REM ===========================================================================

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "GEN=%BUILD%\gen"
set "OBJ=%BUILD%\obj"

REM The literal ")" in "Program Files (x86)" would close a parenthesised block.
set "PROGFILES86=%ProgramFiles(x86)%"

set "EDVR_RIG="
:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--clean" goto arg_clean
if /I "%~1"=="--jobs" goto arg_jobs
if /I "%~1"=="--dll-only" goto arg_dll_only
if /I "%~1"=="--rig" goto arg_rig
echo [edvr] unknown argument: %~1
exit /b 1
:arg_clean
set "DO_CLEAN=1"
shift
goto parse_args
:arg_jobs
set "EDVR_JOBS=%~2"
shift
shift
goto parse_args
:arg_dll_only
set "EDVR_DLL_ONLY=1"
shift
goto parse_args
:arg_rig
set "EDVR_RIG=%~2"
shift
shift
goto parse_args
:args_done
if defined EDVR_DLL_ONLY if defined DO_CLEAN (
    echo [edvr] ERROR: --dll-only cannot be combined with --clean
    exit /b 1
)
if defined EDVR_RIG goto run_rig

where python.exe >nul 2>&1
if errorlevel 1 ( echo [edvr] ERROR: python is required to generate export thunks. & exit /b 1 )

REM ===========================================================================
REM  One build at a time (see header). A second top-level invocation re-enters
REM  itself once, guarded by EDVR_BUILD_GUARDED, so the lock releases exactly
REM  once however the guarded run below finishes -- every existing "exit /b"
REM  in this file already does the right thing without any further changes.
REM ===========================================================================
if not defined EDVR_BUILD_GUARDED (
    python "%ROOT%\tools\build_lock.py" --acquire --note "%~nx0 %*"
    if errorlevel 1 exit /b 1
    set "EDVR_BUILD_GUARDED=1"
    call "%~f0" %*
    set "EDVR_BUILD_RC=!errorlevel!"
    python "%ROOT%\tools\build_lock.py" --release
    exit /b !EDVR_BUILD_RC!
)

if defined DO_CLEAN (
    echo [edvr] cleaning
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
)

where cl.exe >nul 2>&1
if errorlevel 1 call :find_vs
if errorlevel 1 exit /b 1
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

REM Sources are named RELATIVE to the repo from here on. A worktree under
REM .claude\worktrees\<name> put the d3d11 compile line past cmd's 8191
REM characters with every file spelled absolute, and cmd truncated it SILENTLY:
REM the last eight files were never compiled and the link reported them as
REM 57 unresolved externals (2026-09-07). setlocal at the top restores the
REM caller's directory on exit, so this needs no popd.
pushd "%ROOT%"

if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%GEN%" mkdir "%GEN%"
if not exist "%OBJ%" mkdir "%OBJ%"

REM A rig (or a child it stages) that ever creates its own per-instance
REM directory straight under %BUILD% -- own copy of d3d11.dll, own edvr.ini --
REM outlives the build that made it: nothing else ever deletes it, so a rig
REM removed from this file after making one leaves it forever. Found on
REM 2026-09-17 as 204 such directories (three families, all from rigs long
REM gone -- tools\vr_census_bridge_test, tools\openvr_export_census_test,
REM tools\openvr_smoke -- removed in 1a54e9e with the legacy OpenVR proxy
REM they tested), each holding its own build\d3d11.dll copy, and each of
REM those copies a separate Defender quarantine entry once flagged. Swept by
REM shape alone (tools\run_jobs.py's stale_exe_dirs), not by rig label, so
REM debris from a rig deleted since is still found; run first, before
REM anything below can fail and skip it.
python tools\run_jobs.py --sweep-exe-dir "%BUILD%" || exit /b 1

REM Dependency cache survives --clean. Never discover a loader in SteamVR.
python tools\fetch_openxr_loader.py --verify || exit /b 1
copy /y "%ROOT%\third_party\openxr\loader\openxr_loader.dll" "%BUILD%\openxr_loader.dll" >nul || exit /b 1
copy /y "%ROOT%\third_party\openxr\loader\OPENXR-LOADER-LICENSE.txt" "%BUILD%\OPENXR-LOADER-LICENSE.txt" >nul || exit /b 1
python tools\fetch_openxr_loader.py --self-test || exit /b 1
python tools\gen_installer_rc.py --self-test || exit /b 1
python tools\package_native.py --self-test || exit /b 1
python tools\build_diff.py --self-test || exit /b 1
python tools\build_receipt.py --self-test || exit /b 1
python tools\build_lock.py --self-test || exit /b 1
python tools\flash_patch_residual.py --self-test || exit /b 1

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

REM A -dirty build is fine to make and dangerous to SEND, so say so loudly.
REM
REM The version is stamped here, at compile time, from the tree as it stands.
REM Build-then-commit therefore bakes in the pre-commit describe, and the
REM binary afterwards disagrees with what `git describe` says -- which is
REM exactly what happened twice on 2026-08-30, once reaching a field tester
REM whose logs then reported a version that did not exist. The binary was
REM correct both times; only its name was wrong, which on a support path is
REM its own kind of wrong.
REM
REM Not an error: building a dirty tree is the normal inner loop, and
REM refusing it would make the guard the thing people work around. It is a
REM line you cannot miss in the output when you are about to hand the file
REM to somebody.
echo %EDVR_VER% | findstr /C:"-dirty" >nul && (
    echo [edvr] NOTE: this build is stamped %EDVR_VER% -- from a tree with
    echo [edvr]       uncommitted changes. Committing does NOT relabel it;
    echo [edvr]       commit first, then build, before sending it anywhere.
)

REM Every cl.exe call below compiles its sources across all cores. cl.exe
REM prepends the CL environment variable to its own command line, so this one
REM setting covers every invocation, the 103-file d3d11 compile included, and
REM any added later. /MP is incompatible with /E, /EP, /showIncludes and /Yc,
REM none of which are used; cl.exe refuses the combination out loud rather
REM than misbehaving. Measured 2026-09-15 on an 8-thread Ryzen: the d3d11.dll
REM section went from 53 s to 17 s, and with the shader reuse above the whole
REM build went from 365 s to 233 s, every gate green.
REM The test rigs are separate children of this process, several at a time,
REM and tools\run_jobs.py gives each of those CL=/MP4 instead, so eight rigs
REM cannot start sixty-four compiler processes between them.
set CL=/MP

REM /EHs, not /EHsc, for the whole d3d11 half. /EHc is "assume an extern "C"
REM function never throws", and AMD's FSR3 D3D11 port breaks that assumption
REM on purpose: its TIF helper (ffx_dx11.cpp) answers a failed D3D11 call
REM inside ffxFsr3UpscalerContextCreate/Dispatch -- both extern "C" -- with a
REM bare `throw 1`. Under /EHsc the try/catch in src\d3d11\fsr3_engine.cpp is
REM not required to run and the process fail-fasts instead (the review of
REM 2026-09-16, F2; the STATUS_STACK_BUFFER_OVERRUN signature in the design
REM doc's journal is that fail-fast). The flag covers every object in this
REM compile, not just fsr3_engine.cpp; the cost is unwind tables around
REM extern "C" calls.
REM Optional local symbols for CPU profiling. Keep release optimization: DEBUG
REM otherwise changes the linker's REF/ICF defaults. PDBs stay in build/.
set "EDVR_CPU_COMPILE="
set "EDVR_CPU_LINK="
if "%EDVR_PROFILE_SYMBOLS%"=="1" (
    set "EDVR_CPU_COMPILE=/Z7"
    set "EDVR_CPU_LINK=/DEBUG:FULL /OPT:REF /OPT:ICF"
)
set CFLAGS=/nologo /c /O2 /MT /std:c++17 /EHs /W4 /GR- %EDVR_CPU_COMPILE% ^
 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
 /DEDVR_VERSION_STRING=\"%EDVR_VER%\" ^
 /I"%GEN%"

echo.
REM The runtime config audit data: known keys + the moved-from map,
REM generated from the same sources the late contract check verifies.
python "tools\check_config_contract.py" --quiet --emit "%GEN%\config_contract_gen.h"
if errorlevel 1 ( echo [edvr] ERROR: contract header generation failed & exit /b 1 )

echo [edvr] === precompiled temporal shaders ===
REM Fixed HLSL belongs in the build: compiling it in the first Present delayed
REM the intro by 18 seconds. The header carries a key over the HLSL, the
REM variant table and the compiler DLL, and is regenerated whenever that key
REM changes, so stale bytecode cannot survive a source change -- while an
REM unchanged shader costs nothing instead of the AA variant's 19 s of fxc on
REM every build (measured 2026-09-15). The generator tests never initialize a GPU.
if not exist "%OBJ%\temporalshader" mkdir "%OBJ%\temporalshader"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\temporalshader\\" /Fe"%OBJ%\temporalshader\temporal_shader_build.exe" ^
    "tools\temporal_shader_build\temporal_shader_build.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: temporal shader compiler build failed & exit /b 1 )
"%OBJ%\temporalshader\temporal_shader_build.exe" --self-test || exit /b 1
"%OBJ%\temporalshader\temporal_shader_build.exe" --output "%GEN%\temporal_shader_bytecode.h" --dry-run || exit /b 1
"%OBJ%\temporalshader\temporal_shader_build.exe" --output "%GEN%\temporal_shader_bytecode.h" || exit /b 1

echo [edvr] === d3d11.dll ===
REM AMD FSR 1.0 as embeddable HLSL. Generated rather than committed so the
REM vendored headers stay byte-identical to upstream (src\d3d11\fsr\).
python "tools\gen_fsr_hlsl.py" --root "%ROOT%" --out "%GEN%"
if errorlevel 1 ( echo [edvr] ERROR: FSR shader embedding failed & exit /b 1 )

REM The settings schema -- the installer's window AND the in-headset menu's
REM row table (docs\settings-menu.md) -- generated from edvr.ini and the
REM accessor calls in src\, with the gate that keeps it complete. Here,
REM before the d3d11 compile, because menu.cpp includes menu_schema.inc.
REM
REM A setting that is uncommented in edvr.ini is one this build ships ON. If it
REM is not also reachable from the settings window, it is invisible to everybody
REM who does not edit ini files, and nothing else in the build would notice: the
REM game reads it, the log names it, and the window that is supposed to expose
REM it simply does not. One annotation line above the key is what this asks for,
REM and it fails the build until it is there.
REM
REM The generator's own test first: a key read in several places takes its
REM type from the typed read, not from whichever file is walked first. The
REM menu's getString echo of fix.render_sharpness once typed the window's
REM Sharpening row as text, and a text row that shows percentages wrote a
REM typed 20 as 2000% (issue 35). That rule fails here now, not in the window.
python "tools\gen_settings_schema.py" --self-test || (
    echo [edvr] ERROR: the settings schema generator failed its own test
    exit /b 1
)
python "tools\gen_settings_schema.py" --root "%ROOT%" --out "%GEN%"
if errorlevel 1 (
    echo [edvr] ERROR: the settings schema is incomplete ^(see above^)
    exit /b 1
)

python "tools\gen_exports.py" --source "%SystemRoot%\System32\d3d11.dll" ^
    --tag d3d11 --out "%GEN%" ^
    --wrap D3D11CreateDevice --wrap D3D11CreateDeviceAndSwapChain ^
    --extra-export edvr_selftest_hooks ^
    --extra-export edvr_selftest_scene_draws ^
    --extra-export edvr_selftest_binding ^
    --extra-export edvrAcquireGraphicsBridge ^
    --extra-export edvrAcquireRenderBoundary ^
    --extra-export edvrAcquireNativeGraphics ^
    --extra-export edvr_selftest_graphics_bridge ^
    --extra-export edvrFssHealLeft ^
    --extra-export edvrFssTheater ^
    --extra-export edvrTemporalAa ^
    --extra-export edvrEyeCaptureUntreated ^
    --extra-export edvrEyeCaptureArm ^
    --extra-export edvrTemporalAaNoteHead ^
    --extra-export edvrSharpen ^
    --extra-export edvrDepthProbeSelftest ^
    --extra-export edvrDlaaAvailable ^
    --extra-export edvrDlaaCounts ^
    --extra-export edvrMenuPanel ^
    --extra-export edvrAcquireNativeMenu ^
    --extra-export edvrAcquireNativeTemporal ^
    --extra-export edvrAcquireNativeSharpen ^
    --extra-export edvrAcquireNativeFrame ^
    --extra-export edvrAcquireNativeFss ^
    --extra-export edvrAcquireNativeTiming ^
    --extra-export edvrReadNativePresentTrace ^
    --extra-export edvrDoorGpuBegin ^
    --extra-export edvrDoorGpuEnd ^
    --extra-export "edvrNativeStartupRouting DATA" ^
    --extra-export edvrQueryOculusRouting ^
    --extra-export edvrQueryNativeRenderSettings ^
    --extra-export edvrPublishNativeRenderSizing ^
    --extra-export edvrQueryNativeRenderSizing ^
    --extra-export edvrGpuFrameEvent
if errorlevel 1 ( echo [edvr] ERROR: export generation failed & exit /b 1 )

REM Both halves link "%OBJ%\<half>\*.obj" -- a wildcard -- and every build
REM compiles every listed source, so an object left behind by a source that
REM was REMOVED from the list is the only thing in that directory a build did
REM not just make. It links anyway. Measured 2026-09-13: early_session.cpp
REM was deleted, its .obj stayed, and the link failed on three symbols it
REM still referenced -- the lucky case. Had those symbols still existed, the
REM removed feature would have linked straight back into the DLL with no
REM line anywhere saying so. Clearing the directory first costs nothing and
REM makes the object set exactly the source list. The cleanup happens after
REM DLL-only promotion has verified its receipt, so a rejected promotion does
REM not disturb the previous build.

REM NVIDIA's DLSS SDK. EDVR_NGX_SDK names a copy explicitly; else the
REM checkout's own third_party\ngx; else the machine's copy under
REM %LOCALAPPDATA%\EDVR\ngx-sdk, which is where tools\fetch_ngx.py puts it
REM (one copy every checkout and worktree finds). The d3d11 half is then
REM built with the NGX calls in, the static library linked, and the runtime
REM and NVIDIA's licence copied into the build for the installer and the
REM zip. The copy is VERIFIED first -- the files the build reads, and the
REM runtime's SHA-256 against the pin in tools\fetch_ngx.py -- and a
REM mismatch fails the build: the DLL the installer carries must be the
REM one the flights verified, and updating it is a deliberate commit.
REM Without any SDK the build still succeeds, since the code compiles
REM either way, but says so loudly: that build has no DLAA and its
REM installer carries no runtime, which is not a release (package.bat
REM refuses it unless told --no-dlss).
set NGX=
if defined EDVR_NGX_SDK set NGX=%EDVR_NGX_SDK%
if not defined NGX if exist "%ROOT%\third_party\ngx\include\nvsdk_ngx.h" set NGX=%ROOT%\third_party\ngx
if not defined NGX if exist "%LOCALAPPDATA%\EDVR\ngx-sdk\include\nvsdk_ngx.h" set NGX=%LOCALAPPDATA%\EDVR\ngx-sdk
set NGXFLAGS=
set NGXLIB=
if defined NGX (
    python "tools\fetch_ngx.py" --verify "%NGX%" || (
        echo [edvr] ERROR: the DLSS SDK at %NGX% is not the pinned one. tools\fetch_ngx.py
        echo        names the commit and the runtime's hash; fetch it again, or update the
        echo        pin on purpose.
        exit /b 1
    )
    set NGXFLAGS=/DEDVR_HAVE_NGX=1 /I"%NGX%\include"
    set NGXLIB="%NGX%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" advapi32.lib
    echo [edvr] DLSS SDK: %NGX%
    copy /Y "%NGX%\lib\Windows_x86_64\rel\nvngx_dlss.dll" "%BUILD%\" >nul
    copy /Y "%NGX%\LICENSE.txt" "%BUILD%\NVIDIA-DLSS-LICENSE.txt" >nul
) else (
    echo [edvr] ==================================================================
    echo [edvr] NO DLSS SDK. This build has no DLAA, and its installer carries no
    echo [edvr] nvngx_dlss.dll. Fine for development; NOT a release. Looked for
    echo [edvr] include\nvsdk_ngx.h in these three places, in this order:
    if defined EDVR_NGX_SDK (
        echo [edvr]   EDVR_NGX_SDK   = %EDVR_NGX_SDK%
    ) else (
        echo [edvr]   EDVR_NGX_SDK   = ^(not set^)
    )
    echo [edvr]   the checkout    = %ROOT%\third_party\ngx
    echo [edvr]   this machine    = %LOCALAPPDATA%\EDVR\ngx-sdk
    echo [edvr] To fetch the pinned SDK once for this machine:  python tools\fetch_ngx.py
    echo [edvr] ==================================================================
    if exist "%BUILD%\nvngx_dlss.dll" del /q "%BUILD%\nvngx_dlss.dll"
    if exist "%BUILD%\NVIDIA-DLSS-LICENSE.txt" del /q "%BUILD%\NVIDIA-DLSS-LICENSE.txt"
)

REM AMD's FSR 3.1 upscaler, the community Direct3D 11 port (the optiscaler
REM FidelityFX-SDK-DX11 fork; tools\fetch_ffx_dx11.py; design doc section
REM 3.2). EDVR_FFX_DX11 names a copy explicitly, or forces the no-SDK path
REM with the literal value "none" -- proving fix.temporal_aa=fsr's runtime
REM refusal even on a machine that already has the SDK staged; else the
REM checkout's own third_party\ffx-dx11; else the machine's copy under
REM %LOCALAPPDATA%\EDVR\ffx-dx11, which is where tools\fetch_ffx_dx11.py
REM puts it (one build every checkout and worktree shares). The d3d11 half
REM is then built with the FSR3 calls in and the two static libraries
REM linked. The copy is VERIFIED first -- the staged files, and each
REM library's CRT and imports, against tools\fetch_ffx_dx11.py's own
REM checks -- and a mismatch fails the build. Without any SDK the build
REM still succeeds, since the code compiles either way, but fix.temporal_aa
REM = fsr then refuses at runtime with "this build was made without AMD's
REM upscaler" (no DLL is shipped either way -- FFX links in statically).
set FFX=
set FSR_NONE=
if /i "%EDVR_FFX_DX11%"=="none" set FSR_NONE=1
if not defined FSR_NONE if defined EDVR_FFX_DX11 set FFX=%EDVR_FFX_DX11%
if not defined FSR_NONE if not defined FFX if exist "%ROOT%\third_party\ffx-dx11\include\FidelityFX\host\ffx_fsr3upscaler.h" set FFX=%ROOT%\third_party\ffx-dx11
if not defined FSR_NONE if not defined FFX if exist "%LOCALAPPDATA%\EDVR\ffx-dx11\include\FidelityFX\host\ffx_fsr3upscaler.h" set FFX=%LOCALAPPDATA%\EDVR\ffx-dx11
set FSRFLAGS=
set FSRLIB=
if defined FFX (
    python "tools\fetch_ffx_dx11.py" --verify "%FFX%" || (
        echo [edvr] ERROR: the FSR3 D3D11 port at %FFX% is not the pinned one. tools\fetch_ffx_dx11.py
        echo        names the commit; fetch it again, or update the pin on purpose.
        exit /b 1
    )
    set FSRFLAGS=/DEDVR_HAVE_FSR3=1 /I"%FFX%\include"
    set FSRLIB="%FFX%\lib\ffx_fsr3upscaler_x64.lib" "%FFX%\lib\ffx_backend_dx11_x64.lib"
    echo [edvr] FSR3 D3D11 port: %FFX%
    REM The port's MIT notice, exactly as NGX's licence is handled above. It
    REM ships because the port is COMPILED INTO the d3d11.dll we distribute --
    REM it has no DLL of its own -- so MIT's notice requirement applies to the
    REM release. tools\package_native.py carries it into the zip when it is
    REM here, and the no-port path below deletes it so a stale one cannot.
    copy /Y "%FFX%\LICENSE.txt" "%BUILD%\FIDELITYFX-SDK-DX11-LICENSE.txt" >nul
) else (
    if exist "%BUILD%\FIDELITYFX-SDK-DX11-LICENSE.txt" del /q "%BUILD%\FIDELITYFX-SDK-DX11-LICENSE.txt"
    if defined FSR_NONE (
        echo [edvr] EDVR_FFX_DX11=none: building without AMD's FSR upscaler on purpose,
        echo [edvr] to prove the no-SDK path. fix.temporal_aa=fsr will refuse at runtime.
    ) else (
        echo [edvr] ==================================================================
        echo [edvr] NO FSR3 SDK. This build has no AMD upscaler; fix.temporal_aa=fsr
        echo [edvr] refuses at runtime. Fine for development; NOT a release. Looked for
        echo [edvr] include\FidelityFX\host\ffx_fsr3upscaler.h in these three places,
        echo [edvr] in this order:
        if defined EDVR_FFX_DX11 (
            echo [edvr]   EDVR_FFX_DX11  = %EDVR_FFX_DX11%
        ) else (
            echo [edvr]   EDVR_FFX_DX11  = ^(not set^)
        )
        echo [edvr]   the checkout    = %ROOT%\third_party\ffx-dx11
        echo [edvr]   this machine    = %LOCALAPPDATA%\EDVR\ffx-dx11
        echo [edvr] To fetch the pinned SDK once for this machine:  python tools\fetch_ffx_dx11.py
        echo [edvr] ==================================================================
    )
)

if defined EDVR_DLL_ONLY (
    echo [edvr] === DLL-only promotion ===
    python tools\build_receipt.py --verify "%BUILD%\full_build_receipt.json" ^
        --root "%ROOT%" --require-clean ^
        --context "cl=%CL%" ^
        --context "profile_symbols=%EDVR_PROFILE_SYMBOLS%" ^
        --context "cpu_compile=%EDVR_CPU_COMPILE%" ^
        --context "cpu_link=%EDVR_CPU_LINK%" ^
        --context "ngx_request=%EDVR_NGX_SDK%" ^
        --context "ngx=%NGX%" ^
        --context "ffx_request=%EDVR_FFX_DX11%" ^
        --context "ffx=%FFX%" ^
        --context "fsr_none=%FSR_NONE%" || (
            echo [edvr] ERROR: DLL-only promotion is not allowed without the
            echo        matching green full build receipt.
            exit /b 1
        )
)

if not exist "%OBJ%\d3d11" mkdir "%OBJ%\d3d11"
del /q "%OBJ%\d3d11\*.obj" 2>nul
ml64.exe /nologo /c /Fo"%OBJ%\d3d11\thunks.obj" "%GEN%\edvr_thunks_d3d11.asm" >nul
if errorlevel 1 ( echo [edvr] ERROR: ml64 failed & exit /b 1 )

cl.exe %CFLAGS% %NGXFLAGS% %FSRFLAGS% /Fo"%OBJ%\d3d11"\ ^
    "src\common\log.cpp" "src\common\config.cpp" ^
    "src\common\config_audit.cpp" ^
    "src\common\guard.cpp" "src\common\vtable_hook.cpp" "src\common\code_hook.cpp" ^
    "src\common\hotkey.cpp" "src\common\proxy.cpp" ^
    "src\common\frame_flag.cpp" ^
    "src\common\iat_hook.cpp" "src\common\iniedit.cpp" ^
    "src\d3d11\input_gate.cpp" "src\d3d11\menu.cpp" ^
    "src\d3d11\oculus_route.cpp" ^
    "src\d3d11\menu_keys.cpp" ^
    "src\d3d11\menu_panel.cpp" "src\d3d11\perf_monitor.cpp" "src\d3d11\native_perf_history.cpp" "src\d3d11\native_benchmark_collector.cpp" ^
    "src\d3d11\native_menu.cpp" ^
    "src\d3d11\native_temporal.cpp" ^
    "src\d3d11\native_sharpen.cpp" ^
    "src\d3d11\native_frame.cpp" ^
    "src\d3d11\native_fss.cpp" ^
    "src\d3d11\native_timing.cpp" ^
    "src\d3d11\map_wait.cpp" ^
    "src\d3d11\native_render_settings.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_frame_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\d3d11\gpu_census.cpp" ^
    "src\d3d11\d3d11_proxy.cpp" "src\d3d11\device_hook.cpp" ^
    "src\d3d11\graphics_bridge.cpp" ^
    "src\d3d11\render_boundary.cpp" ^
    "src\d3d11\exposure_fix.cpp" "src\d3d11\vscreen.cpp" ^
    "src\d3d11\glitch_frame.cpp" "src\d3d11\transition_flash_prevent.cpp" ^
    "src\d3d11\pose_reader_watch.cpp" "src\d3d11\transition_flash_eye_base.cpp" ^
    "src\d3d11\vscreen_res.cpp" "src\common\vscreen_auto_state.cpp" ^
    "src\d3d11\binding_shadow.cpp" "src\d3d11\head_offset_gate.cpp" ^
    "src\d3d11\vr_runtime.cpp" ^
    "src\d3d11\camera_view.cpp" "src\d3d11\journal_watch.cpp" ^
    "src\d3d11\elite_binds.cpp" "src\d3d11\draw_census.cpp" ^
    "src\d3d11\object_probe.cpp" ^
    "src\d3d11\pixel_probe.cpp" ^
    "src\d3d11\object_record_writer_probe.cpp" "src\d3d11\object_record_writer_hook.cpp" ^
    "src\d3d11\kinematic_eval_probe.cpp" "src\d3d11\kinematic_eval_hook.cpp" ^
    "src\d3d11\scheduler_stack_probe.cpp" "src\d3d11\scheduler_stack_hook.cpp" ^
    "src\d3d11\static_prop_gate.cpp" "src\d3d11\cull_gate_probe.cpp" ^
    "src\d3d11\lod_governor.cpp" ^
    "src\d3d11\engine_velocity.cpp" ^
    "src\d3d11\fss_res.cpp" "src\d3d11\fss_scan.cpp" ^
    "src\d3d11\fss_panel.cpp" "src\d3d11\fss_probe.cpp" ^
    "src\d3d11\fss_reveal.cpp" "src\d3d11\fss_ring.cpp" ^
    "src\d3d11\fss_dump.cpp" "src\d3d11\fss_heal.cpp" ^
    "src\d3d11\eye_split.cpp" ^
    "src\d3d11\resolve_probe.cpp" ^
    "src\d3d11\stencil_probe.cpp" ^
    "src\d3d11\resolve_bind_fix.cpp" ^
    "src\d3d11\fss_theater.cpp" ^
    "src\d3d11\xinput_watch.cpp" ^
    "src\d3d11\fss_panel_rect.cpp" ^
    "src\d3d11\panel_curve.cpp" "src\d3d11\screen_motion.cpp" "src\d3d11\weapon_motion.cpp" ^
    "src\d3d11\shader_sig.cpp" ^
    "src\d3d11\remlok_fix.cpp" "src\d3d11\holo_fix.cpp" ^
    "src\d3d11\target_sharp.cpp" "src\d3d11\night_vision.cpp" ^
    "src\d3d11\hud_sprite.cpp" ^
    "src\d3d11\panel_upscale.cpp" ^
    "src\d3d11\wake_pulse.cpp" ^
    "src\d3d11\hud_grain.cpp" ^
    "src\d3d11\ui_depth.cpp" ^
    "src\d3d11\ui_layer.cpp" "src\d3d11\ui_surfaces.cpp" "src\d3d11\ui_panel_scale.cpp" ^
    "third_party\dxbc_hash\DxilHash.cpp" ^
    "src\d3d11\backdrop_fix.cpp" ^
    "src\d3d11\scrim_fix.cpp" ^
    "src\d3d11\quad_probe.cpp" ^
    "src\d3d11\intro_probe.cpp" ^
    "src\d3d11\intro_panel.cpp" ^
    "src\d3d11\intro_skip.cpp" ^
    "src\d3d11\intro_upscale.cpp" ^
    "src\d3d11\temporal_pass.cpp" ^
    "src\d3d11\celestial_motion.cpp" ^
    "src\d3d11\depth_probe.cpp" ^
    "src\d3d11\luma_probe.cpp" ^
    "src\d3d11\dlaa.cpp" ^
    "src\d3d11\fsr3_engine.cpp" ^
    "src\d3d11\foveation.cpp" ^
    "src\d3d11\eye_mask.cpp" ^
    "src\d3d11\sharpen_pass.cpp" ^
    "src\d3d11\loader_panel.cpp" ^
    "src\d3d11\splash_dim.cpp" ^
    "src\d3d11\billboard_fix.cpp" ^
    "src\d3d11\particle_fix.cpp" "src\d3d11\shader_swap.cpp" "src\d3d11\sunglare_fix.cpp"
if errorlevel 1 ( echo [edvr] ERROR: compile failed & exit /b 1 )

REM gdi32.lib: the settings menu's panel is rasterised with GDI (the game
REM already imports GDI32, so the DLL adds no module to the process).
rc.exe /nologo /fo "%OBJ%\d3d11\dxbc_notice.res" "third_party\dxbc_hash\notice.rc"
if errorlevel 1 ( echo [edvr] ERROR: DXBC notice resource failed & exit /b 1 )

REM Version resources for both shipped DLLs. Until now only the installer
REM carried a VERSIONINFO; an unsigned DLL with no FileVersion, CompanyName or
REM FileDescription looks less like a real build than one that has them, and
REM this is the cheap, honest way to look like one: say who built it and what
REM it is. Generated, like the installer's own block, so the version string
REM stays git describe's and nothing here hand-maintains it.
if not exist "%OBJ%\d3d11" mkdir "%OBJ%\d3d11"
if not exist "%OBJ%\openxr_module" mkdir "%OBJ%\openxr_module"
python "tools\gen_installer_rc.py" --version-rc graphics --version "%EDVR_VER%" --out "%GEN%"
if errorlevel 1 ( echo [edvr] ERROR: graphics version resource generation failed & exit /b 1 )
python "tools\gen_installer_rc.py" --version-rc runtime --version "%EDVR_VER%" --out "%GEN%"
if errorlevel 1 ( echo [edvr] ERROR: runtime version resource generation failed & exit /b 1 )
rc.exe /nologo /fo "%OBJ%\d3d11\version.res" "%GEN%\version_graphics.rc"
if errorlevel 1 ( echo [edvr] ERROR: rc.exe failed on the graphics version resource & exit /b 1 )
rc.exe /nologo /fo "%OBJ%\openxr_module\version.res" "%GEN%\version_runtime.rc"
if errorlevel 1 ( echo [edvr] ERROR: rc.exe failed on the runtime version resource & exit /b 1 )

link.exe /nologo /DLL /MACHINE:X64 /INCREMENTAL:NO %EDVR_CPU_LINK% /PDB:"%BUILD%\d3d11.pdb" ^
    /DEF:"%GEN%\edvr_d3d11.def" /OUT:"%BUILD%\d3d11.dll" ^
    "%OBJ%\d3d11\*.obj" "%OBJ%\d3d11\dxbc_notice.res" "%OBJ%\d3d11\version.res" kernel32.lib user32.lib gdi32.lib version.lib d3dcompiler.lib %NGXLIB% %FSRLIB%
if errorlevel 1 ( echo [edvr] ERROR: link failed & exit /b 1 )

echo [edvr] built %BUILD%\d3d11.dll
copy /y "%BUILD%\d3d11.dll" "%BUILD%\edvr_openxr_graphics.dll" >nul || exit /b 1
echo [edvr] built %BUILD%\edvr_openxr_graphics.dll

echo.
REM Gated with `||`, not `if errorlevel 1`.
REM
REM `if errorlevel N` means "exit code >= N", and a process killed by an access
REM violation exits with a negative NTSTATUS -- so it read a CRASH as success.
REM Measured. `%errorlevel%` is no good either: these gates sit inside
REM parenthesised blocks, where it expands once at parse time. `||` keys off the
REM command's own exit code and is immune to both.

echo.
echo [edvr] === edvr_openxr_runtime.dll ===
REM Native runtime DLL: the only supported release and installation backend.
REM Its application fixture calls the game-imported ABI without linking the host.
if not exist "%OBJ%\openxr_module" mkdir "%OBJ%\openxr_module"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /LD /D_CRT_SECURE_NO_WARNINGS %EDVR_CPU_COMPILE% ^
    /I"third_party\openxr\include" /Fo"%OBJ%\openxr_module\\" ^
    /DEDVR_VERSION_STRING=\"%EDVR_VER%\" ^
    /Fe"%BUILD%\edvr_openxr_runtime.dll" "src\openxr\native_module.cpp" ^
    "src\openxr\d3d11_stereo.cpp" "src\openxr\session_binding.cpp" "src\openxr\openvr_system.cpp" ^
    "src\openxr\eye_capture.cpp" "src\openxr\skybox_capture.cpp" ^
    "src\openxr\shared_texture_transfer.cpp" "src\openxr\producer_gpu_timing.cpp" ^
    "src\openxr\device_gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\openxr\openvr_compositor.cpp" "src\openxr\openvr_auxiliary.cpp" ^
    "src\common\frame_flag.cpp" ^
    /link /INCREMENTAL:NO %EDVR_CPU_LINK% /PDB:"%BUILD%\edvr_openxr_runtime.pdb" /DEF:"src\openxr\native_module.def" "%OBJ%\openxr_module\version.res" d3d11.lib dxgi.lib d3dcompiler.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: native runtime module build failed & exit /b 1 )

REM Shared by the installer and installer_test rigs below.
set INSTALLER_SRC="src\installer\main.cpp" "src\installer\gui.cpp" ^
    "src\installer\ui.cpp" "src\installer\settings.cpp" ^
    "src\installer\settings_view.cpp" "src\installer\logbundle.cpp" ^
    "src\installer\app.cpp" "src\installer\plan.cpp" ^
    "src\installer\apply.cpp" "src\installer\detect.cpp" ^
    "src\installer\probe.cpp" "src\common\iniedit.cpp" ^
    "src\installer\state.cpp" "src\installer\mirror.cpp" ^
    "src\installer\payload.cpp"
set INSTALLER_LIBS=user32.lib gdi32.lib gdiplus.lib dwmapi.lib uxtheme.lib ^
    shell32.lib ole32.lib comctl32.lib advapi32.lib version.lib bcrypt.lib dxgi.lib kernel32.lib

if defined EDVR_DLL_ONLY goto dll_only_finish

echo.
echo [edvr] === test rigs ===
REM Every :rig_<label> subroutine at the end of this file, run by
REM tools\run_jobs.py as concurrent "build.bat --rig <label>" children of this
REM process. Each rig owns its obj directory.
REM A rig's output is printed whole when it finishes; a failing rig stops new
REM launches and is printed last, so the tail of the log names it.
REM build\rig_times.json remembers each rig's duration so the longest start
REM first next time; delete it freely.
REM
REM Every process a rig runs -- its test exes, and the proxy DLLs some of them
REM load out of build\ on purpose -- logs under build\edvr_logs\<label>, a
REM directory of the rig's own: --exe-dir has the runner hand each child
REM EDVR_LOG_DIR (src\common\config.cpp), and a proxy keeps its crash
REM sentinels (<hook>.armed, created at hook install and deleted once the hook
REM has proven itself) in its log directory. Before that every proxy a rig
REM loaded logged to build\edvr_logs, a proxy that found another rig's
REM sentinel still armed took it for its own crash and stood down, and the
REM proxy-loading rigs had to run one at a time (run_jobs.py --serial, kept
REM for a rig whose processes share state --exe-dir cannot separate; none
REM does now). A rig that only wants a D3D11 device takes System32's export
REM through src\common\system_d3d11.h and links without d3d11.lib: an
REM imported D3D11CreateDevice resolves, for an exe in build\, to
REM build\d3d11.dll before System32's, and the test runs under EDVR's hooks by
REM accident.
REM The --quiet rigs hold wall-clock intervals to tight bounds; they run alone,
REM after the rest, with the whole machine. Only their test runs need that, so
REM each is split (see the rig area below): its compiles run in the pool like
REM any other rig's and only its runs wait their turn.
REM openxr_module_test loads openxr_export_fixture.dll, which
REM :rig_openxr_exports_test builds; --after holds the reader back until the
REM writer has actually finished (see the rig rules below).
set "RUN_JOBS_ARGS="
if defined EDVR_JOBS set "RUN_JOBS_ARGS=--jobs %EDVR_JOBS%"
python tools\run_jobs.py --self-test || exit /b 1
python tools\run_jobs.py --script "%~f0" --times "%BUILD%\rig_times.json" ^
    --exe-dir "%BUILD%" --quiet native_timing_test,gpu_timing_test,gpu_census_test,vtable_test ^
    --after openxr_module_test=openxr_exports_test ^
    %RUN_JOBS_ARGS% || exit /b 1

echo [edvr] === config contract ===
where python >nul 2>&1
if errorlevel 1 (
    echo [edvr] NOTE: python not found, skipping the config contract check
) else (
    python "tools\check_config_contract.py" || (
        echo [edvr] ERROR: config contract check failed or crashed
        exit /b 1
    )
)


REM The native OpenXR build is the only build; nothing above ran a legacy path.
copy /y "%BUILD%\edvr_openxr_runtime.dll" "%BUILD%\openvr_api.dll" >nul || exit /b 1
python tools\openxr_pe.py --native "%BUILD%\openvr_api.dll" || exit /b 1
python tools\openxr_pe.py --graphics "%BUILD%\d3d11.dll" || exit /b 1
echo.
REM The one line a release engineer has to see, after thousands of compiler
REM lines: whether the installer just built carries NVIDIA's runtime.
if exist "%BUILD%\nvngx_dlss.dll" (
    echo [edvr] DLSS runtime: CARRIED -- build\edvr-installer.exe places nvngx_dlss.dll
    echo        beside the game on machines with an NVIDIA card.
) else (
    echo [edvr] DLSS runtime: NOT CARRIED -- no DLSS SDK was found ^(the boxed notice
    echo        above says where it looked^). Not a release build.
)
echo.
python tools\package_native.py --check-installer || exit /b 1
python tools\build_receipt.py --write "%BUILD%\full_build_receipt.json" ^
    --root "%ROOT%" ^
    --context "cl=%CL%" ^
    --context "profile_symbols=%EDVR_PROFILE_SYMBOLS%" ^
    --context "cpu_compile=%EDVR_CPU_COMPILE%" ^
    --context "cpu_link=%EDVR_CPU_LINK%" ^
    --context "ngx_request=%EDVR_NGX_SDK%" ^
    --context "ngx=%NGX%" ^
    --context "ffx_request=%EDVR_FFX_DX11%" ^
    --context "ffx=%FFX%" ^
    --context "fsr_none=%FSR_NONE%" || exit /b 1
echo [edvr] Native OpenXR build and all gates passed.
echo [edvr] Install both native DLLs and the bundled loader for a test flight:
echo        python tools\install_edvr.py --target frontier --dry-run
echo        python tools\install_edvr.py --target frontier
echo        --target takes steam, frontier or a path. Settings are preserved
echo        unless --ini is specified. Windows selects the OpenXR runtime.
echo.
echo [edvr] The self-contained build\edvr-installer.exe installs the same pair,
echo        preserves graphics-mod chaining, and supports repair and uninstall.
echo [edvr] After the flight:
echo        python tools\edvr_log.py --target frontier --expect-build HEAD
exit /b 0

REM ===========================================================================
:dll_only_finish
echo.
echo [edvr] === DLL-only promotion outputs ===
copy /y "%BUILD%\edvr_openxr_runtime.dll" "%BUILD%\openvr_api.dll" >nul || exit /b 1
python tools\openxr_pe.py --native "%BUILD%\openvr_api.dll" || exit /b 1
python tools\openxr_pe.py --graphics "%BUILD%\d3d11.dll" || exit /b 1
echo [edvr] DLL-only build passed: production DLLs and loader are ready to install.
echo [edvr] Test rigs and the self-contained installer were skipped; use a full build
echo        before distributing an installer or accepting new source changes.
exit /b 0

REM ===========================================================================
REM  Test rigs. Each is a subroutine that tools\run_jobs.py runs in its own
REM  "build.bat --rig <label>" child, several at a time, once the DLLs above
REM  are built. A child jumps here from :args_done with the parent's
REM  environment: ROOT, BUILD, OBJ, GEN, CFLAGS, EDVR_VER, the
REM  INSTALLER_* lists and the compiler on PATH. Rigs run in any order and at the
REM  same time as one another, so a rig must not depend on another rig's
REM  output, must not share an obj directory, and must not write a file
REM  another rig reads -- unless that dependency is declared to run_jobs.py
REM  with --after (see the invocation above), which holds the reader back
REM  until the writer has actually finished, not merely started. This was an
REM  unenforced rule once: openxr_module_test loaded openxr_export_fixture.dll
REM  (built by :rig_openxr_exports_test) with no ordering between the two
REM  rigs, and only ran clean because a stale fixture DLL from an earlier
REM  build was normally still sitting in %BUILD%; a build whose mid-link
REM  failure had deleted it surfaced the race. --after is now the one
REM  sanctioned way to write a rig that reads another's output -- do not add
REM  a second one without it. The DLLs a rig copies are the main flow's, above.
REM
REM  A rig named in --quiet (or --serial, see tools\run_jobs.py) is written
REM  in two steps so that only its test runs are held back:
REM      :rig_<label>
REM      if "%EDVR_RIG_STEP%"=="run" goto <label>_run
REM      ... compiles ...
REM      if "%EDVR_RIG_STEP%"=="build" exit /b 0
REM      :<label>_run
REM      ... runs ...
REM      exit /b 0
REM  The runner recognises the exact "build" guard line, runs the rig once
REM  with EDVR_RIG_STEP=build beside everything else and once more with
REM  EDVR_RIG_STEP=run under the group's rule. Without the guard the whole rig
REM  runs under the rule. The run label must not start with rig_.
REM ===========================================================================
:run_rig
if not defined CFLAGS (
    echo [edvr] ERROR: --rig is tools\run_jobs.py's entry point; run build.bat without it
    exit /b 1
)
pushd "%ROOT%"
call :rig_%EDVR_RIG%
if errorlevel 1 exit /b 1
exit /b 0

:rig_smoke
echo [edvr] === smoke.exe ===
if not exist "%OBJ%\smoke" mkdir "%OBJ%\smoke"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG ^
    /Fo"%OBJ%\smoke\\" /Fe"%BUILD%\smoke.exe" ^
    "tools\smoke\smoke.cpp" /link /INCREMENTAL:NO d3d11.lib kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: smoke build failed & exit /b 1 )
echo [edvr] built %BUILD%\smoke.exe
exit /b 0

:rig_fakechain
echo [edvr] === fakechain.dll ===
if not exist "%OBJ%\fakechain" mkdir "%OBJ%\fakechain"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG /LD ^
    /Fo"%OBJ%\fakechain\\" /Fe"%BUILD%\fakechain.dll" ^
    "tools\fakechain\fakechain.cpp" /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: fakechain build failed & exit /b 1 )
echo [edvr] built %BUILD%\fakechain.dll

echo.
exit /b 0

:rig_vtable_test
if "%EDVR_RIG_STEP%"=="run" goto vtable_test_run
echo [edvr] === vtable_test.exe ===
REM The object-wrapping collision (issue #6), without needing ReShade. These
REM cells were written against the copy-and-swap-vptr mechanism and FAILED on
REM it, which is the only reason to believe them now.
if not exist "%OBJ%\vtabletest" mkdir "%OBJ%\vtabletest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\vtabletest"\ ^
    /DEDVR_VTABLE_TEST /Fe"%BUILD%\vtable_test.exe" "tools\vtable_test\vtable_test.cpp" ^
    "src\common\vtable_hook.cpp" "src\common\code_hook.cpp" "src\common\guard.cpp" ^
    "src\common\log.cpp" "src\common\config.cpp" ^
    "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: vtable_test build failed & exit /b 1 )
if "%EDVR_RIG_STEP%"=="build" exit /b 0
:vtable_test_run
"%BUILD%\vtable_test.exe" || (
    echo [edvr] ERROR: vtable hooking does not compose with object wrappers
    exit /b 1
)
exit /b 0

:rig_native_menu_test
echo [edvr] === native_menu_test.exe ===
if not exist "%OBJ%\native_menu" mkdir "%OBJ%\native_menu"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DEDVR_MENU_TEST /I"%GEN%" /I"third_party\openxr\include" ^
    /Fo"%OBJ%\native_menu\\" /Fe"%BUILD%\native_menu_test.exe" ^
    "tools\native_menu_test\native_menu_test.cpp" "src\d3d11\native_menu.cpp" ^
    "src\openxr\eye_capture.cpp" "src\openxr\shared_texture_transfer.cpp" "src\openxr\producer_gpu_timing.cpp" ^
    "src\d3d11\input_gate.cpp" "src\d3d11\menu_panel.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\d3d11\menu_keys.cpp" "src\d3d11\shader_swap.cpp" ^
    "src\common\iat_hook.cpp" "src\common\iniedit.cpp" ^
    "src\common\vtable_hook.cpp" "src\common\code_hook.cpp" "src\common\hotkey.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" ^
    "src\common\guard.cpp" "src\common\frame_flag.cpp" "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib gdi32.lib version.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: native menu test build failed & exit /b 1 )
"%BUILD%\native_menu_test.exe" --dry-run || exit /b 1
"%BUILD%\native_menu_test.exe" --self-test || exit /b 1
exit /b 0

:rig_native_frame_test
echo [edvr] === native_frame_test.exe ===
if not exist "%OBJ%\native_frame" mkdir "%OBJ%\native_frame"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /Fo"%OBJ%\native_frame\\" /Fe"%BUILD%\native_frame_test.exe" ^
    "tools\native_frame_test\native_frame_test.cpp" "src\d3d11\native_frame.cpp" ^
    "src\d3d11\native_render_settings.cpp" "src\common\vscreen_auto_state.cpp" ^
    "src\common\config.cpp" "src\common\frame_flag.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: native frame provider test build failed & exit /b 1 )
"%BUILD%\native_frame_test.exe" --dry-run || exit /b 1
"%BUILD%\native_frame_test.exe" --self-test || exit /b 1
exit /b 0

:rig_native_fss_test
echo [edvr] === native_fss_test.exe ===
if not exist "%OBJ%\native_fss" mkdir "%OBJ%\native_fss"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /Fo"%OBJ%\native_fss\\" /Fe"%BUILD%\native_fss_test.exe" ^
    "tools\native_fss_test\native_fss_test.cpp" "src\d3d11\native_fss.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: native FSS provider test build failed & exit /b 1 )
"%BUILD%\native_fss_test.exe" --dry-run || exit /b 1
"%BUILD%\native_fss_test.exe" --self-test || exit /b 1
exit /b 0

:rig_native_fss_gpu_test
echo [edvr] === native_fss_gpu_test.exe ===
if not exist "%OBJ%\native_fss_gpu" mkdir "%OBJ%\native_fss_gpu"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\native_fss_gpu\\" /Fe"%BUILD%\native_fss_gpu_test.exe" ^
    "tools\native_fss_test\native_fss_gpu_test.cpp" "src\d3d11\native_fss.cpp" "src\d3d11\fss_heal.cpp" ^
    "src\common\config.cpp" "src\common\frame_flag.cpp" "src\common\log.cpp" "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: native FSS shader test build failed & exit /b 1 )
"%BUILD%\native_fss_gpu_test.exe" --dry-run || exit /b 1
"%BUILD%\native_fss_gpu_test.exe" --self-test || exit /b 1
exit /b 0

:rig_native_cull_test
echo [edvr] === native_cull_test.exe ===
if not exist "%OBJ%\native_cull" mkdir "%OBJ%\native_cull"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\native_cull\\" /Fe"%BUILD%\native_cull_test.exe" ^
    "tools\native_cull_test\native_cull_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: native cull policy test build failed & exit /b 1 )
"%BUILD%\native_cull_test.exe" --dry-run || exit /b 1
"%BUILD%\native_cull_test.exe" --self-test || exit /b 1
exit /b 0

:rig_dlaa_mode_test
echo [edvr] === dlaa_mode_test.exe ===
if not exist "%OBJ%\dlaa_mode" mkdir "%OBJ%\dlaa_mode"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\dlaa_mode\\" /Fe"%BUILD%\dlaa_mode_test.exe" ^
    "tools\dlaa_mode_test\dlaa_mode_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: dlaa_mode_test build failed & exit /b 1 )
"%BUILD%\dlaa_mode_test.exe" --dry-run || exit /b 1
"%BUILD%\dlaa_mode_test.exe" --self-test || exit /b 1
exit /b 0

:rig_native_temporal_test
echo [edvr] === native_temporal_test.exe ===
if not exist "%OBJ%\native_temporal" mkdir "%OBJ%\native_temporal"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /I"third_party\openxr\include" ^
    /Fo"%OBJ%\native_temporal\\" /Fe"%BUILD%\native_temporal_test.exe" ^
    "tools\native_temporal_test\native_temporal_test.cpp" "src\d3d11\native_temporal.cpp" ^
    "src\common\config.cpp" "src\common\frame_flag.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: native temporal test build failed & exit /b 1 )
"%BUILD%\native_temporal_test.exe" --dry-run || exit /b 1
"%BUILD%\native_temporal_test.exe" --self-test || exit /b 1
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /I"third_party\openxr\include" ^
    /Fo"%OBJ%\native_temporal\\" /Fe"%BUILD%\native_temporal_gpu_test.exe" ^
    "tools\native_temporal_test\native_temporal_gpu_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib d3d11.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: native temporal GPU test build failed & exit /b 1 )
"%BUILD%\native_temporal_gpu_test.exe" --dry-run || exit /b 1
exit /b 0

:rig_native_sharpen_test
echo [edvr] === native_sharpen_test.exe ===
if not exist "%OBJ%\native_sharpen_test" mkdir "%OBJ%\native_sharpen_test"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /Fo"%OBJ%\native_sharpen_test\\" /Fe"%BUILD%\native_sharpen_test.exe" ^
    "tools\native_sharpen_test\native_sharpen_test.cpp" "src\d3d11\native_sharpen.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: native sharpen contract test build failed & exit /b 1 )
"%BUILD%\native_sharpen_test.exe" --dry-run || exit /b 1
"%BUILD%\native_sharpen_test.exe" --self-test || exit /b 1
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /Fo"%OBJ%\native_sharpen_test\\" /Fe"%BUILD%\native_sharpen_gpu_test.exe" ^
    "tools\native_sharpen_test\native_sharpen_gpu_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib d3d11.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: native sharpen GPU test build failed & exit /b 1 )
"%BUILD%\native_sharpen_gpu_test.exe" --dry-run || exit /b 1
exit /b 0

:rig_openxr_trace_test
echo [edvr] === openxr_trace_test.exe ===
if not exist "%OBJ%\openxr_trace" mkdir "%OBJ%\openxr_trace"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\openxr_trace\\" /Fe"%BUILD%\openxr_trace_test.exe" ^
    "tools\openxr_trace_test\openxr_trace_test.cpp" /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: native trace test build failed & exit /b 1 )
"%BUILD%\openxr_trace_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_trace_test.exe" --self-test || exit /b 1
exit /b 0

:rig_native_timing_test
if "%EDVR_RIG_STEP%"=="run" goto native_timing_test_run
echo [edvr] === native_timing_test.exe ===
if not exist "%OBJ%\native_timing" mkdir "%OBJ%\native_timing"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /Fo"%OBJ%\native_timing\\" /Fe"%BUILD%\native_timing_test.exe" ^
    "tools\native_timing_test\native_timing_test.cpp" "src\d3d11\native_timing.cpp" "src\d3d11\map_wait.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: native timing contract test build failed & exit /b 1 )

cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Fo"%OBJ%\native_timing\\" /Fe"%BUILD%\native_perf_history_test.exe" ^
    "tools\native_perf_history_test\native_perf_history_test.cpp" "src\d3d11\native_perf_history.cpp" "src\d3d11\native_benchmark_collector.cpp"
if errorlevel 1 ( echo [edvr] ERROR: native perf history test build failed & exit /b 1 )

cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /I"third_party\openxr\include" ^
    /Fo"%OBJ%\native_timing\\" /Fe"%BUILD%\native_timing_gpu_test.exe" ^
    "tools\native_timing_test\native_timing_gpu_test.cpp" "src\d3d11\native_timing.cpp" "src\d3d11\map_wait.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_frame_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\common\log.cpp" "src\common\config.cpp" "src\common\proxy.cpp" "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: native timing GPU test build failed & exit /b 1 )

REM Separate XR-device queries; real WARP work and injected query failures.
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /Fo"%OBJ%\native_timing\\" /Fe"%BUILD%\native_device_gpu_test.exe" ^
    "tools\native_device_gpu_test\native_device_gpu_test.cpp" ^
    "src\openxr\device_gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: native device GPU test build failed & exit /b 1 )
if "%EDVR_RIG_STEP%"=="build" exit /b 0
:native_timing_test_run
"%BUILD%\native_timing_test.exe" --dry-run || exit /b 1
"%BUILD%\native_timing_test.exe" --self-test || exit /b 1
"%BUILD%\native_perf_history_test.exe" --dry-run || exit /b 1
"%BUILD%\native_perf_history_test.exe" --self-test || exit /b 1
"%BUILD%\native_timing_gpu_test.exe" --dry-run || exit /b 1
"%BUILD%\native_timing_gpu_test.exe" --self-test || exit /b 1
"%BUILD%\native_device_gpu_test.exe" --dry-run || exit /b 1
"%BUILD%\native_device_gpu_test.exe" --self-test || exit /b 1
exit /b 0

:rig_crash_context_test
echo [edvr] === crash_context_test.exe ===
if not exist "%OBJ%\crashcontext" mkdir "%OBJ%\crashcontext"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%" /Fo"%OBJ%\crashcontext\crash_context_test.obj" ^
    /Fe"%BUILD%\crash_context_test.exe" "tools\crash_context_test\crash_context_test.cpp" ^
    /link /OUT:"%BUILD%\crash_context_test.exe" /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: crash_context_test build failed & exit /b 1 )
"%BUILD%\crash_context_test.exe" --self-test || exit /b 1
exit /b 0

:rig_config_test
echo [edvr] === config_test.exe ===
REM The real parser over the real shipped edvr.ini. The file's own layout
REM depends on two parser properties -- repeated section headers, last value
REM wins -- that were originally read out of config.cpp rather than observed,
REM and every symptom of either being false shows up in the game rather than
REM in a build.
if not exist "%OBJ%\cfgtest" mkdir "%OBJ%\cfgtest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\cfgtest"\ ^
    /Fe"%BUILD%\config_test.exe" "tools\config_test\config_test.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: config_test build failed & exit /b 1 )
"%BUILD%\config_test.exe" "%ROOT%" "%BUILD%\cfgscratch" || (
    echo [edvr] ERROR: the shipped edvr.ini does not parse as documented
    exit /b 1
)
exit /b 0

:rig_input_gate_test
echo [edvr] === input_gate_test.exe ===
REM Actual private DirectInput tables, buffered keys, close/release behavior,
REM and real A/W factories through the executable's early import hook.
if not exist "%OBJ%\inputgatetest" mkdir "%OBJ%\inputgatetest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\inputgatetest"\ ^
    /Fe"%BUILD%\input_gate_test.exe" "tools\input_gate_test\input_gate_test.cpp" ^
    "src\common\iat_hook.cpp" "src\common\vtable_hook.cpp" ^
    "src\common\hotkey.cpp" "src\common\config.cpp" "src\common\log.cpp" ^
    "src\common\guard.cpp" "src\common\frame_flag.cpp" "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib dinput8.lib
if errorlevel 1 ( echo [edvr] ERROR: input_gate_test build failed & exit /b 1 )
"%BUILD%\input_gate_test.exe" || (
    echo [edvr] ERROR: the menu keyboard gate failed its device or release checks
    exit /b 1
)
exit /b 0

:rig_gate_test
echo [edvr] === gate_test.exe ===
if not exist "%OBJ%\gatetest" mkdir "%OBJ%\gatetest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\gatetest"\ /Fe"%BUILD%\gate_test.exe" ^
    "tools\gate_test\gate_test.cpp" ^
    "src\d3d11\head_offset_gate.cpp" "src\d3d11\vr_runtime.cpp" ^
    "src\common\config.cpp" ^
    "src\common\log.cpp" "src\common\frame_flag.cpp" ^
    "src\d3d11\camera_view.cpp" "src\common\guard.cpp" ^
    "src\common\proxy.cpp" "src\d3d11\journal_watch.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: gate_test build failed & exit /b 1 )
"%BUILD%\gate_test.exe" "%ROOT%" || (
    echo [edvr] ERROR: the head-offset gate arms where it should not
    exit /b 1
)
exit /b 0

:rig_glitch_test
echo [edvr] === glitch_test.exe ===
REM The transition-flash detector, replayed without the game. This repo SHIPS
REM that fix, and until now had no way to run its test -- which is how a signal
REM the private ledger had already refuted stayed in a release until a user felt
REM it as judder on a planet surface.
if not exist "%OBJ%\glitchtest" mkdir "%OBJ%\glitchtest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\glitchtest"\ ^
    /Fe"%BUILD%\glitch_test.exe" "tools\glitch_test\glitch_test.cpp" ^
    "src\d3d11\glitch_frame.cpp" "src\d3d11\vr_runtime.cpp" ^
    "src\common\config.cpp" ^
    "src\common\frame_flag.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: glitch_test build failed & exit /b 1 )
"%BUILD%\glitch_test.exe" "%BUILD%\glitchscratch" || (
    echo [edvr] ERROR: the transition flash detector failed its own test
    exit /b 1
)
exit /b 0

:rig_supersample_test
echo [edvr] === supersample_test.exe ===
REM The eye-region rule (src\common\supersample_math.h): which pixels are one
REM eye's, from the Submit bounds (double-wide and flipped) -- table-tested,
REM header-only, linking nothing from src\ at all. Every pass at the door
REM reads through it, so a drift here is a drift in all of them.
if not exist "%OBJ%\sstest" mkdir "%OBJ%\sstest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\sstest"\ ^
    /Fe"%BUILD%\supersample_test.exe" ^
    "tools\supersample_test\supersample_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: supersample_test build failed & exit /b 1 )
"%BUILD%\supersample_test.exe" || (
    echo [edvr] ERROR: the eye-region rule is wrong
    exit /b 1
)
exit /b 0

:rig_temporal_test
echo [edvr] === temporal_test.exe ===
REM The temporal pass's arithmetic (src\common\temporal_math.h): the jitter
REM sequence and the SIGN of its tangent shift, the pixel-to-direction
REM mapping on a real headset's lopsided frustum, the rotation deltas from
REM the runtime's pose and the game's view rows, the reprojection walked
REM by hand against a known head turn, and the world path's camera gate
REM replaying eye run 050423's parked camera (a zero from rows a drop left
REM behind must be carried over, never accepted). Header-only, links nothing
REM from src\.
if not exist "%OBJ%\taatest" mkdir "%OBJ%\taatest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\taatest"\ ^
    /Fe"%BUILD%\temporal_test.exe" ^
    "tools\temporal_test\temporal_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: temporal_test build failed & exit /b 1 )
"%BUILD%\temporal_test.exe" || (
    echo [edvr] ERROR: the temporal pass's arithmetic is wrong
    exit /b 1
)
exit /b 0

:rig_ui_depth
echo [edvr] === private UI depth regression ===
REM Keep the executable away from build\d3d11.dll: these tests use system
REM D3D11 WARP and include the production coverage pass directly.
if not exist "%OBJ%\uidepthtest" mkdir "%OBJ%\uidepthtest"
cl.exe /nologo /O2 /Gy /MT /std:c++17 /EHsc /W4 /wd4702 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\uidepthtest\\" /Fe"%OBJ%\uidepthtest\ui_depth_test.exe" ^
    "tools\ui_depth_test\ui_depth_test.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO /OPT:REF d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: ui_depth_test build failed & exit /b 1 )
"%OBJ%\uidepthtest\ui_depth_test.exe" || (
    echo [edvr] ERROR: private UI depth regression
    exit /b 1
)
exit /b 0

:rig_hologram_depth_test
echo [edvr] === generic hologram/icon depth regression ===
REM Same shape as :rig_ui_depth: WARP, the production coverage pass
REM included directly, away from build\d3d11.dll.
if not exist "%OBJ%\holodepthtest" mkdir "%OBJ%\holodepthtest"
cl.exe /nologo /O2 /Gy /MT /std:c++17 /EHsc /W4 /wd4702 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\holodepthtest\\" /Fe"%OBJ%\holodepthtest\hologram_depth_test.exe" ^
    "tools\hologram_depth_test\hologram_depth_test.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO /OPT:REF d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: hologram_depth_test build failed & exit /b 1 )
"%OBJ%\holodepthtest\hologram_depth_test.exe" || (
    echo [edvr] ERROR: generic hologram/icon depth regression
    exit /b 1
)
exit /b 0

:rig_native_motion_rigs
echo [edvr] === native motion, fusion and night-vision rigs ===

if not exist "%OBJ%\holomotion" mkdir "%OBJ%\holomotion"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\holomotion\\" /Fe"%OBJ%\holomotion\holo_motion_test.exe" ^
    "tools\holo_motion_test\holo_motion_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: hologram motion test build failed & exit /b 1 )
"%OBJ%\holomotion\holo_motion_test.exe" || exit /b 1

if not exist "%OBJ%\screenmotion" mkdir "%OBJ%\screenmotion"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /I"%GEN%" ^
    /Fo"%OBJ%\screenmotion\\" /Fe"%OBJ%\screenmotion\screen_motion_test.exe" ^
    "tools\screen_motion_test\screen_motion_test.cpp" "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib || exit /b 1
"%OBJ%\screenmotion\screen_motion_test.exe" --self-test || exit /b 1
if not exist "%OBJ%\weaponmotion" mkdir "%OBJ%\weaponmotion"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\weaponmotion\\" /Fe"%OBJ%\weaponmotion\weapon_motion_test.exe" ^
    "tools\weapon_motion_test\weapon_motion_test.cpp" "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib || exit /b 1
"%OBJ%\weaponmotion\weapon_motion_test.exe" --self-test || exit /b 1
if not exist "%OBJ%\identityfusion" mkdir "%OBJ%\identityfusion"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\identityfusion\\" /Fe"%OBJ%\identityfusion\identity_fusion_test.exe" ^
    "tools\identity_fusion_test\identity_fusion_test.cpp" "tools\identity_fusion_test\identity_fusion_replay.cpp" ^
    /link /INCREMENTAL:NO d3dcompiler.lib dxguid.lib || exit /b 1
"%OBJ%\identityfusion\identity_fusion_test.exe" --self-test || exit /b 1
python "tools\identity_fusion_capture.py" --self-test || exit /b 1
if not exist "%OBJ%\nightvision" mkdir "%OBJ%\nightvision"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\nightvision\\" /Fe"%OBJ%\nightvision\night_vision_test.exe" ^
    "tools\night_vision_test\night_vision_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib || exit /b 1
"%OBJ%\nightvision\night_vision_test.exe" --self-test || exit /b 1
exit /b 0

:rig_intro_skip
echo [edvr] === DirectShow intro skip regression ===
if not exist "%OBJ%\introskip" mkdir "%OBJ%\introskip"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\introskip\\" /Fe"%OBJ%\introskip\intro_skip_test.exe" ^
    "tools\intro_skip_test\intro_skip_test.cpp" "src\common\iat_hook.cpp" ^
    /link /INCREMENTAL:NO ole32.lib strmiids.lib || exit /b 1
"%OBJ%\introskip\intro_skip_test.exe" --self-test || exit /b 1
python "tools\holo_motion.py" --self-test || exit /b 1
exit /b 0

:rig_stellar_motion
echo [edvr] === stellar motion regression ===
if not exist "%OBJ%\stellarmotion" mkdir "%OBJ%\stellarmotion"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\stellarmotion\\" /Fe"%OBJ%\stellarmotion\stellar_motion_test.exe" ^
    "tools\stellar_motion_test\stellar_motion_test.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\common\log.cpp" "src\common\config.cpp" "src\common\proxy.cpp" "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: stellar motion test build failed & exit /b 1 )
"%OBJ%\stellarmotion\stellar_motion_test.exe" || exit /b 1
exit /b 0

:rig_terrain_motion
echo [edvr] === terrain motion regression ===
if not exist "%OBJ%\terrainmotion" mkdir "%OBJ%\terrainmotion"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\terrainmotion\\" /Fe"%OBJ%\terrainmotion\celestial_motion_test.exe" ^
    "tools\celestial_motion_test\celestial_motion_test.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: terrain motion test build failed & exit /b 1 )
"%OBJ%\terrainmotion\celestial_motion_test.exe" || exit /b 1
python "tools\terrain_motion.py" --self-test || exit /b 1
python "tools\terrain_motion.py" "%OBJ%\terrainmotion\eye_fixture_Terrain.bin" --verify-fixture || exit /b 1
exit /b 0

:rig_depth_scene_pick_test
echo [edvr] === scene depth selection cache regression ===
if not exist "%OBJ%\depthscenepick" mkdir "%OBJ%\depthscenepick"
cl.exe /nologo /O2 /Gy /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\depthscenepick\\" /Fe"%OBJ%\depthscenepick\depth_scene_pick_test.exe" ^
    "tools\depth_scene_pick_test\depth_scene_pick_test.cpp" ^
    /link /INCREMENTAL:NO /OPT:REF
if errorlevel 1 ( echo [edvr] ERROR: scene depth selection test build failed & exit /b 1 )
"%OBJ%\depthscenepick\depth_scene_pick_test.exe" --dry-run || exit /b 1
"%OBJ%\depthscenepick\depth_scene_pick_test.exe" --self-test || exit /b 1
exit /b 0

:rig_scrim_metadata_test
echo [edvr] === scrim metadata cache regression ===
if not exist "%OBJ%\scrimmetadata" mkdir "%OBJ%\scrimmetadata"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DEDVR_SCRIM_METADATA_TEST ^
    /Fo"%OBJ%\scrimmetadata\\" /Fe"%OBJ%\scrimmetadata\scrim_metadata_test.exe" ^
    "tools\scrim_metadata_test\scrim_metadata_test.cpp" ^
    "src\d3d11\scrim_fix.cpp" "src\d3d11\binding_shadow.cpp" ^
    "src\common\vtable_hook.cpp" "src\common\code_hook.cpp" ^
    "src\common\log.cpp" "src\common\config.cpp" "src\common\proxy.cpp" ^
    "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: scrim metadata test build failed & exit /b 1 )
"%OBJ%\scrimmetadata\scrim_metadata_test.exe" --self-test || exit /b 1
exit /b 0

:rig_resolve_bind_test
echo [edvr] === resolve bind shadow regression ===
if not exist "%OBJ%\resolvebind" mkdir "%OBJ%\resolvebind"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\resolvebind\\" /Fe"%OBJ%\resolvebind\resolve_bind_test.exe" ^
    "tools\resolve_bind_test\resolve_bind_test.cpp" ^
    "src\d3d11\resolve_bind_fix.cpp" "src\d3d11\binding_shadow.cpp" ^
    "src\common\vtable_hook.cpp" "src\common\code_hook.cpp" ^
    "src\common\log.cpp" "src\common\config.cpp" "src\common\proxy.cpp" ^
    "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: resolve bind test build failed & exit /b 1 )
"%OBJ%\resolvebind\resolve_bind_test.exe" || exit /b 1
exit /b 0

:rig_object_classification
echo [edvr] === object classification provenance regression ===
if not exist "%OBJ%\classification" mkdir "%OBJ%\classification"
ml64.exe /nologo /c /Fo"%OBJ%\classification\unwind_stubs.obj" ^
    "tools\object_classification_test\unwind_stubs.asm"
if errorlevel 1 ( echo [edvr] ERROR: unwind fixture build failed & exit /b 1 )
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DEDVR_RECORD_WRITER_TEST ^
    /Fo"%OBJ%\classification\\" /Fe"%OBJ%\classification\object_classification_test.exe" ^
    "tools\object_classification_test\object_classification_test.cpp" ^
    "src\d3d11\object_record_writer_probe.cpp" "src\d3d11\object_record_writer_hook.cpp" ^
    "src\d3d11\kinematic_eval_probe.cpp" "src\d3d11\kinematic_eval_hook.cpp" ^
    "src\d3d11\scheduler_stack_probe.cpp" "src\d3d11\scheduler_stack_hook.cpp" ^
    "src\common\code_hook.cpp" "src\common\log.cpp" "src\common\config.cpp" ^
    "src\common\proxy.cpp" "src\common\guard.cpp" ^
    "%OBJ%\classification\unwind_stubs.obj" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: object classification test build failed & exit /b 1 )
"%OBJ%\classification\object_classification_test.exe" "%OBJ%\classification" || exit /b 1
python "tools\object_classification.py" --self-test || exit /b 1
python "tools\object_classification.py" "%OBJ%\classification\classification_fixture.json" || exit /b 1
exit /b 0

:rig_eye_draw_snapshot
echo [edvr] === eye draw snapshot regression ===
if not exist "%OBJ%\drawsnapshot" mkdir "%OBJ%\drawsnapshot"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\drawsnapshot\\" /Fe"%OBJ%\drawsnapshot\eye_draw_snapshot_test.exe" ^
    "tools\eye_draw_snapshot_test\eye_draw_snapshot_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: eye draw snapshot test build failed & exit /b 1 )
"%OBJ%\drawsnapshot\eye_draw_snapshot_test.exe" "%OBJ%\drawsnapshot\fixture.bin" || exit /b 1
python "tools\eye_draw_snapshot.py" --self-test || exit /b 1
python "tools\eye_draw_snapshot.py" "%OBJ%\drawsnapshot\fixture.bin" --verify-fixture || exit /b 1
python "tools\eye_depth_dump.py" --self-test || exit /b 1
python "tools\eye_depth_dump.py" "%OBJ%\drawsnapshot" || exit /b 1
python "tools\gui_draw_snapshot.py" --self-test || exit /b 1
python "tools\eye_inputs.py" --self-test || exit /b 1
python "tools\eye_decisions.py" --self-test || exit /b 1
python "tools\gui_draw_snapshot.py" "%OBJ%\drawsnapshot\fixture.bin.gui" --verify-fixture || exit /b 1
exit /b 0

:rig_eye_tonemap_snapshot
echo [edvr] === eye tone-map snapshot regression ===
if not exist "%OBJ%\tonemapsnapshot" mkdir "%OBJ%\tonemapsnapshot"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\tonemapsnapshot\\" /Fe"%OBJ%\tonemapsnapshot\eye_tonemap_snapshot_test.exe" ^
    "tools\eye_tonemap_snapshot_test\eye_tonemap_snapshot_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: tone-map snapshot test build failed & exit /b 1 )
"%OBJ%\tonemapsnapshot\eye_tonemap_snapshot_test.exe" "%OBJ%\tonemapsnapshot\fixture.bin" || exit /b 1
python "tools\eye_tonemap_snapshot.py" --self-test || exit /b 1
python "tools\eye_tonemap_snapshot.py" "%OBJ%\tonemapsnapshot\fixture.bin" --verify-fixture || exit /b 1
exit /b 0

:rig_eye_panel_snapshot
echo [edvr] === eye panel snapshot regression ===
if not exist "%OBJ%\panelsnapshot" mkdir "%OBJ%\panelsnapshot"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\panelsnapshot\\" /Fe"%OBJ%\panelsnapshot\eye_panel_snapshot_test.exe" ^
    "tools\eye_panel_snapshot_test\eye_panel_snapshot_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: panel snapshot test build failed & exit /b 1 )
"%OBJ%\panelsnapshot\eye_panel_snapshot_test.exe" "%OBJ%\panelsnapshot\fixture.bin" || exit /b 1
python "tools\eye_panel_snapshot.py" --self-test || exit /b 1
python "tools\eye_panel_snapshot.py" "%OBJ%\panelsnapshot\fixture.bin" --verify-fixture || exit /b 1
exit /b 0

:rig_vr_census_test
if not exist "%OBJ%\vrcensus" mkdir "%OBJ%\vrcensus"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\vrcensus\\" /Fe"%BUILD%\vr_census_test.exe" ^
    "tools\vr_census_test\vr_census_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: VR census budget test build failed & exit /b 1 )
"%BUILD%\vr_census_test.exe" --self-test || exit /b 1
exit /b 0

:rig_gpu_span_state_test
REM CPU policy prototype only: no shipping proxy calls this state machine yet.
if not exist "%OBJ%\gpuspanstate" mkdir "%OBJ%\gpuspanstate"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\gpuspanstate\\" /Fe"%BUILD%\gpu_span_state_test.exe" ^
    "tools\gpu_span_state_test\gpu_span_state_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: GPU span state test build failed & exit /b 1 )
"%BUILD%\gpu_span_state_test.exe" --self-test || exit /b 1
exit /b 0

:rig_gpu_disjoint_clock_test
REM Shared query ownership and the real D3D11 adapter remain desk-only until
REM every existing timer has migrated. Never load the proxy beside these rigs.
if not exist "%OBJ%\gpuclock" mkdir "%OBJ%\gpuclock"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Fo"%OBJ%\gpuclock\\" /Fe"%OBJ%\gpuclock\gpu_disjoint_clock_test.exe" ^
    "tools\gpu_disjoint_clock_test\gpu_disjoint_clock_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: shared GPU clock test build failed & exit /b 1 )
"%OBJ%\gpuclock\gpu_disjoint_clock_test.exe" --dry-run || exit /b 1
"%OBJ%\gpuclock\gpu_disjoint_clock_test.exe" --self-test || exit /b 1
exit /b 0

:rig_gpu_span_d3d11_test
if not exist "%OBJ%\gpuspand3d11" mkdir "%OBJ%\gpuspand3d11"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Fo"%OBJ%\gpuspand3d11\\" /Fe"%OBJ%\gpuspand3d11\gpu_span_d3d11_test.exe" ^
    "tools\gpu_span_d3d11_test\gpu_span_d3d11_test.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: D3D11 GPU span test build failed & exit /b 1 )
"%OBJ%\gpuspand3d11\gpu_span_d3d11_test.exe" --dry-run || exit /b 1
"%OBJ%\gpuspand3d11\gpu_span_d3d11_test.exe" --self-test || exit /b 1
exit /b 0

:rig_producer_gpu_timing_test
if not exist "%OBJ%\producergputiming" mkdir "%OBJ%\producergputiming"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Fo"%OBJ%\producergputiming\\" /Fe"%OBJ%\producergputiming\producer_gpu_timing_test.exe" ^
    "tools\producer_gpu_timing_test\producer_gpu_timing_test.cpp" ^
    "src\openxr\producer_gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: producer GPU timing test build failed & exit /b 1 )
"%OBJ%\producergputiming\producer_gpu_timing_test.exe" --dry-run || exit /b 1
"%OBJ%\producergputiming\producer_gpu_timing_test.exe" --self-test || exit /b 1
exit /b 0

:rig_gpu_live_hook_test
REM Real WARP query work through the same stacked LiveCopy mechanism used by
REM exposure and vScreen. This is desk-only; no production GPU timer is enabled.
if not exist "%OBJ%\gpulivehook" mkdir "%OBJ%\gpulivehook"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\gpulivehook\\" ^
    /Fe"%OBJ%\gpulivehook\gpu_live_hook_test.exe" ^
    "tools\gpu_live_hook_test\gpu_live_hook_test.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\common\vtable_hook.cpp" "src\common\guard.cpp" "src\common\log.cpp" ^
    "src\common\config.cpp" "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: LiveCopy GPU query test build failed & exit /b 1 )
"%OBJ%\gpulivehook\gpu_live_hook_test.exe" --dry-run || exit /b 1
"%OBJ%\gpulivehook\gpu_live_hook_test.exe" --self-test || exit /b 1
exit /b 0

:rig_gpu_timing_test
if "%EDVR_RIG_STEP%"=="run" goto gpu_timing_test_run
REM Migrated timers must share one disjoint scope, preserve pending readbacks,
REM survive transient pressure, and issue no context commands during unload.
if not exist "%OBJ%\gputiming" mkdir "%OBJ%\gputiming"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\gputiming\\" ^
    /Fe"%OBJ%\gputiming\gpu_timing_test.exe" "tools\gpu_timing_test\gpu_timing_test.cpp" "tools\gpu_timing_test\context_slots.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_frame_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\common\log.cpp" "src\common\config.cpp" "src\common\proxy.cpp" "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: shared GPU timing test build failed & exit /b 1 )
if "%EDVR_RIG_STEP%"=="build" exit /b 0
:gpu_timing_test_run
"%OBJ%\gputiming\gpu_timing_test.exe" --dry-run || exit /b 1
"%OBJ%\gputiming\gpu_timing_test.exe" --self-test || exit /b 1
exit /b 0

:rig_gpu_census_test
REM Issue #38's per-feature GPU cost census. Same shape as
REM :rig_hologram_depth_test: WARP, the production module included
REM directly, away from build\d3d11.dll -- here for its estimator math,
REM its rotation/K-cap accounting and its 30 s line's format, none of
REM which gpu_census.h exposes on purpose. gpu_timing/gpu_frame_timing/
REM gpu_span_d3d11 are real, linked sources, so the WARP round trip is a
REM real disjoint timestamp pair, not a fake.
if not exist "%OBJ%\gpucensus" mkdir "%OBJ%\gpucensus"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\gpucensus\\" ^
    /Fe"%OBJ%\gpucensus\gpu_census_test.exe" "tools\gpu_census_test\gpu_census_test.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_frame_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\common\config.cpp" "src\common\proxy.cpp" "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: GPU census test build failed & exit /b 1 )
"%OBJ%\gpucensus\gpu_census_test.exe" --dry-run || exit /b 1
"%OBJ%\gpucensus\gpu_census_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openvr_abi_test
if not exist "%OBJ%\openvr_abi" mkdir "%OBJ%\openvr_abi"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\openvr_abi\\" /Fe"%BUILD%\openvr_abi_test.exe" ^
    "tools\openvr_abi_test\openvr_abi_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: OpenVR ABI test build failed & exit /b 1 )
"%BUILD%\openvr_abi_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_probe
REM Headset-free OpenXR enumeration contract tests. The probe is standalone;
REM the shipping proxies do not load OpenXR or create an OpenXR session.
if not exist "%OBJ%\openxr_probe" mkdir "%OBJ%\openxr_probe"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS ^
    /I"third_party\openxr\include" /Fo"%OBJ%\openxr_probe\\" ^
    /Fe"%BUILD%\openxr_probe.exe" "tools\openxr_probe\openxr_probe.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: OpenXR probe build failed & exit /b 1 )
"%BUILD%\openxr_probe.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_core_tests
REM Reusable OpenXR core policies. These tests inject dispatch and geometry;
REM no loader/session is opened and the shipping proxies do not use them yet.
if not exist "%OBJ%\openxr_core" mkdir "%OBJ%\openxr_core"
for %%T in (session projection geometry head gate frame pose origin reference space lifecycle owner render_thread present_queue) do (
    cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" ^
        /Fo"%OBJ%\openxr_core\\" /Fe"%BUILD%\openxr_%%T_test.exe" ^
        "tools\openxr_%%T_test\openxr_%%T_test.cpp" /link /INCREMENTAL:NO
    if errorlevel 1 ( echo [edvr] ERROR: OpenXR %%T test build failed & exit /b 1 )
    "%BUILD%\openxr_%%T_test.exe" --dry-run || exit /b 1
    "%BUILD%\openxr_%%T_test.exe" --self-test || exit /b 1
)
exit /b 0

:rig_openxr_native_tests
REM Standalone native-session harness and fake-XR/WARP stereo renderer.
REM The build runs only desktop fixtures; real headset sessions are explicit.
REM Both take System32's d3d11 through src\common\system_d3d11.h rather than
REM importing D3D11CreateDevice, which from build\ would load the proxy.
if not exist "%OBJ%\openxr_native_tests" mkdir "%OBJ%\openxr_native_tests"
for %%T in (native stereo) do (
    cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS ^
        /I"third_party\openxr\include" /Fo"%OBJ%\openxr_native_tests\\" ^
        /Fe"%BUILD%\openxr_%%T_test.exe" "tools\openxr_%%T_test\openxr_%%T_test.cpp" ^
        "src\openxr\d3d11_stereo.cpp" "src\openxr\session_binding.cpp" "src\openxr\openvr_system.cpp" "src\openxr\eye_capture.cpp" "src\openxr\skybox_capture.cpp" ^
        "src\openxr\shared_texture_transfer.cpp" "src\openxr\producer_gpu_timing.cpp" ^
        "src\openxr\device_gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
        "src\openxr\openvr_compositor.cpp" "tools\openxr_native_test\compositor_caller.cpp" ^
        "src\openxr\openvr_auxiliary.cpp" "src\openxr\runtime_exports.cpp" ^
        "src\common\frame_flag.cpp" ^
        /link /INCREMENTAL:NO dxgi.lib d3dcompiler.lib user32.lib
    if errorlevel 1 ( echo [edvr] ERROR: OpenXR %%T test build failed & exit /b 1 )
)
for %%T in (native stereo) do (
    "%BUILD%\openxr_%%T_test.exe" --dry-run || exit /b 1
    "%BUILD%\openxr_%%T_test.exe" --self-test || exit /b 1
)
exit /b 0

:rig_openxr_capture_test
if not exist "%OBJ%\openxr_capture_test" mkdir "%OBJ%\openxr_capture_test"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\openxr_capture_test\\" /Fe"%BUILD%\openxr_capture_test.exe" ^
    "tools\openxr_capture_test\openxr_capture_test.cpp" "src\openxr\eye_capture.cpp" ^
    "src\openxr\shared_texture_transfer.cpp" "src\openxr\producer_gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: OpenXR capture test build failed & exit /b 1 )
"%BUILD%\openxr_capture_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_capture_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_skybox_test
if not exist "%OBJ%\openxr_skybox_test" mkdir "%OBJ%\openxr_skybox_test"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG ^
    /Fo"%OBJ%\openxr_skybox_test\\" /Fe"%BUILD%\openxr_skybox_test.exe" ^
    "tools\openxr_skybox_test\openxr_skybox_test.cpp" "src\openxr\skybox_capture.cpp" ^
    "src\openxr\shared_texture_transfer.cpp" "src\openxr\producer_gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: OpenXR skybox capture test build failed & exit /b 1 )
"%BUILD%\openxr_skybox_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_skybox_test.exe" --self-test || exit /b 1
exit /b 0

:rig_native_device_test
if not exist "%OBJ%\native_device_test" mkdir "%OBJ%\native_device_test"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\native_device_test\\" /Fe"%BUILD%\native_device_test.exe" ^
    "tools\openxr_native_test\native_device_test.cpp" /link /INCREMENTAL:NO d3d11.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: native device test build failed & exit /b 1 )
"%BUILD%\native_device_test.exe" --dry-run || exit /b 1
"%BUILD%\native_device_test.exe" --self-test || exit /b 1
python tools\run_openxr_native.py --self-test || exit /b 1
exit /b 0

:rig_openxr_binding_test
if not exist "%OBJ%\openxr_binding_test" mkdir "%OBJ%\openxr_binding_test"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_binding_test\\" /Fe"%BUILD%\openxr_binding_test.exe" ^
    "tools\openxr_binding_test\openxr_binding_test.cpp" "src\openxr\session_binding.cpp" ^
    "src\openxr\published_session.cpp" "src\common\frame_flag.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: OpenXR binding test build failed & exit /b 1 )
"%BUILD%\openxr_binding_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_binding_test.exe" --self-test || exit /b 1
"%BUILD%\openxr_binding_test.exe" --published-proxy "%BUILD%\edvr_openxr_graphics.dll" || exit /b 1
exit /b 0

:rig_native_startup_test
REM Inspect actual startup behavior without creating a headset runtime. The
REM fixture is intentionally not Elite: it must leave its IAT alone.
if not exist "%OBJ%\native_startup" mkdir "%OBJ%\native_startup"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Fo"%OBJ%\native_startup\\" /Fe"%BUILD%\native_startup_test.exe" ^
    "tools\native_startup_test\native_startup_test.cpp" /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: native startup test build failed & exit /b 1 )
"%BUILD%\native_startup_test.exe" --dry-run || exit /b 1
"%BUILD%\native_startup_test.exe" --self-test "%BUILD%\edvr_openxr_graphics.dll" native || exit /b 1
exit /b 0

:rig_oculus_route_test
REM Qualify the early loader route without running Elite or a headset runtime.
if not exist "%OBJ%\oculus_route" mkdir "%OBJ%\oculus_route"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DEDVR_OCULUS_ROUTE_TEST=1 ^
    /Fo"%OBJ%\oculus_route\\" /Fe"%BUILD%\oculus_route_test.exe" ^
    "tools\oculus_route_test\oculus_route_test.cpp" "src\d3d11\oculus_route.cpp" ^
    "src\common\log.cpp" "src\common\config.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: Oculus route test build failed & exit /b 1 )
"%BUILD%\oculus_route_test.exe" --dry-run || exit /b 1
"%BUILD%\oculus_route_test.exe" --self-test || exit /b 1
exit /b 0

:rig_elite_oculus_test
if not exist "%OBJ%\elite_oculus" mkdir "%OBJ%\elite_oculus"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Fo"%OBJ%\elite_oculus\\" /Fe"%BUILD%\elite_oculus_test.exe" ^
    "tools\elite_oculus_test\elite_oculus_test.cpp" /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: Elite Oculus profile test build failed & exit /b 1 )
"%BUILD%\elite_oculus_test.exe" --dry-run || exit /b 1
"%BUILD%\elite_oculus_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_proxy_state_test
if not exist "%OBJ%\openxr_proxy_state_test" mkdir "%OBJ%\openxr_proxy_state_test"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_proxy_state_test\\" /Fe"%BUILD%\openxr_proxy_state_test.exe" ^
    "tools\openxr_proxy_state_test\openxr_proxy_state_test.cpp" ^
    "src\openxr\d3d11_stereo.cpp" "src\openxr\eye_capture.cpp" "src\openxr\skybox_capture.cpp" ^
    "src\openxr\shared_texture_transfer.cpp" "src\openxr\producer_gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: OpenXR proxy state test build failed & exit /b 1 )
"%BUILD%\openxr_proxy_state_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_proxy_state_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_present_test
if not exist "%OBJ%\openxr_present_test" mkdir "%OBJ%\openxr_present_test"
REM Actual owned-swapchain Present hook, foreign Init caller and private work.
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_present_test\\" /Fe"%BUILD%\openxr_present_test.exe" ^
    "tools\openxr_present_test\openxr_present_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib dxgi.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: OpenXR Present test build failed & exit /b 1 )
"%BUILD%\openxr_present_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_present_test.exe" --self-test || exit /b 1
python "tools\test_openxr_transport.py" --self-test || exit /b 1
python "tools\test_openxr_transport.py" --dry-run || exit /b 1
python "tools\test_openxr_transport.py" || exit /b 1
exit /b 0

:rig_openxr_shutdown_test
if not exist "%OBJ%\openxr_shutdown_test" mkdir "%OBJ%\openxr_shutdown_test"
REM Stopped application Present: real graphics callback and native stop coordinator.
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_shutdown_test\\" /Fe"%BUILD%\openxr_shutdown_test.exe" ^
    "tools\openxr_shutdown_test\openxr_shutdown_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib dxgi.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: OpenXR stopped-Present shutdown test build failed & exit /b 1 )
"%BUILD%\openxr_shutdown_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_shutdown_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_shared_texture_test
if not exist "%OBJ%\openxr_shared_texture_test" mkdir "%OBJ%\openxr_shared_texture_test"
REM Cross-device texture handoff and consumer-only retirement after producer stop.
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\openxr_shared_texture_test\\" /Fe"%BUILD%\openxr_shared_texture_test.exe" ^
    "tools\openxr_shared_texture_test\openxr_shared_texture_test.cpp" ^
    "src\openxr\shared_texture_transfer.cpp" "src\openxr\eye_capture.cpp" "src\openxr\skybox_capture.cpp" ^
    "src\openxr\producer_gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: OpenXR shared texture test build failed & exit /b 1 )
"%BUILD%\openxr_shared_texture_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_shared_texture_test.exe" --self-test || exit /b 1
exit /b 0

:rig_fsr3_engine_test
REM AMD's FSR3 engine (src\d3d11\fsr3_engine.cpp) on WARP: availability, two
REM contexts, DEBUG_CHECKING silence, an at-rest convergence check, the
REM jitter/motion-vector sign registration table (design doc 3.5 item 3),
REM reset, a size change, and the VRAM query -- Track C's own desk test,
REM modeled on :rig_openxr_shared_texture_test above. Skips cleanly (still
REM green) when this build has no FSR3 SDK: there is no engine body to
REM test, the same reason nothing tests NGX's stub half either.
if not defined FSRFLAGS (
    echo [edvr] fsr3_engine_test: no FSR3 SDK in this build ^(EDVR_FFX_DX11^) -- skipping.
    exit /b 0
)
if not exist "%OBJ%\fsr3_engine_test" mkdir "%OBJ%\fsr3_engine_test"
REM /EHs, matching CFLAGS above (and for the same reason): this rig compiles
REM fsr3_engine.cpp itself, and one of its cases proves that the catch around
REM AMD's extern "C" dispatch really does catch the port's `throw 1`. Under
REM /EHsc that case would fail-fast instead of failing.
cl.exe /nologo /W4 /O2 /EHs /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX %FSRFLAGS% ^
    /Fo"%OBJ%\fsr3_engine_test\\" /Fe"%BUILD%\fsr3_engine_test.exe" ^
    "tools\fsr3_engine_test\fsr3_engine_test.cpp" "src\d3d11\fsr3_engine.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_frame_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\common\log.cpp" "src\common\config.cpp" "src\common\proxy.cpp" "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO %FSRLIB% dxgi.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: FSR3 engine test build failed & exit /b 1 )
"%BUILD%\fsr3_engine_test.exe" --dry-run || exit /b 1
"%BUILD%\fsr3_engine_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_system_test
if not exist "%OBJ%\openxr_system_test" mkdir "%OBJ%\openxr_system_test"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_system_test\\" /Fe"%BUILD%\openxr_system_test.exe" ^
    "tools\openxr_system_test\openxr_system_test.cpp" "tools\openxr_system_test\abi_caller.cpp" ^
    "src\openxr\openvr_system.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: owned OpenVR system test build failed & exit /b 1 )
"%BUILD%\openxr_system_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_system_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_launch_centre_test
if not exist "%OBJ%\openxr_launch_centre_test" mkdir "%OBJ%\openxr_launch_centre_test"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_launch_centre_test\\" /Fe"%BUILD%\openxr_launch_centre_test.exe" ^
    "tools\openxr_launch_centre_test\openxr_launch_centre_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: OpenXR launch centering test build failed & exit /b 1 )
"%BUILD%\openxr_launch_centre_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_launch_centre_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_compositor_test
if not exist "%OBJ%\openxr_compositor_test" mkdir "%OBJ%\openxr_compositor_test"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_compositor_test\\" /Fe"%BUILD%\openxr_compositor_test.exe" ^
    "tools\openxr_compositor_test\openxr_compositor_test.cpp" "tools\openxr_compositor_test\abi_caller.cpp" ^
    "src\openxr\openvr_compositor.cpp" "src\common\frame_flag.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: owned OpenVR compositor test build failed & exit /b 1 )
"%BUILD%\openxr_compositor_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_compositor_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_auxiliary_test
if not exist "%OBJ%\openxr_auxiliary_test" mkdir "%OBJ%\openxr_auxiliary_test"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\openxr_auxiliary_test\\" /Fe"%BUILD%\openxr_auxiliary_test.exe" ^
    "tools\openxr_auxiliary_test\openxr_auxiliary_test.cpp" "tools\openxr_auxiliary_test\abi_caller.cpp" ^
    "src\openxr\openvr_auxiliary.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: auxiliary interface test build failed & exit /b 1 )
"%BUILD%\openxr_auxiliary_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_auxiliary_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_exports_test
if not exist "%OBJ%\openxr_exports_test" mkdir "%OBJ%\openxr_exports_test"
REM Only a headset-free export ABI fixture; never installed or named openvr_api.
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /LD /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_exports_test\\" /Fe"%BUILD%\openxr_export_fixture.dll" ^
    "tools\openxr_exports_test\fixture.cpp" "src\openxr\runtime_exports.cpp" ^
    "src\openxr\openvr_system.cpp" "src\openxr\openvr_compositor.cpp" "src\openxr\openvr_auxiliary.cpp" ^
    "src\common\frame_flag.cpp" ^
    /link /INCREMENTAL:NO /DEF:"tools\openxr_exports_test\fixture.def"
if errorlevel 1 ( echo [edvr] ERROR: OpenXR export fixture build failed & exit /b 1 )
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\openxr_exports_test\\" /Fe"%BUILD%\openxr_exports_test.exe" ^
    "tools\openxr_exports_test\openxr_exports_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: OpenXR export test build failed & exit /b 1 )
"%BUILD%\openxr_exports_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_exports_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_module_test
if not exist "%OBJ%\openxr_module_test" mkdir "%OBJ%\openxr_module_test"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\openxr_module_test\\" /Fe"%BUILD%\openxr_module_test.exe" ^
    "tools\openxr_module_test\openxr_module_test.cpp" ^
    /link /INCREMENTAL:NO dxgi.lib d3dcompiler.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: native runtime module test build failed & exit /b 1 )
"%BUILD%\openxr_module_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_module_test.exe" --self-test || exit /b 1
"%BUILD%\openxr_module_test.exe" --self-test-bootstrap || exit /b 1
"%BUILD%\openxr_module_test.exe" --self-test-separate || exit /b 1
"%BUILD%\openxr_module_test.exe" --self-test-bootstrap-separate || exit /b 1
"%BUILD%\openxr_module_test.exe" --self-test-local || exit /b 1
exit /b 0

:rig_installer
echo [edvr] === edvr-installer.exe ===
REM The complete native pair, Khronos loader, notices and settings are embedded.
REM /MANIFEST:NO is not optional. link.exe embeds a manifest of its own by
REM default, and our .rc already puts one at resource 1 -- two RT_MANIFEST
REM resources in one image, which Windows refuses to start at all: "the
REM side-by-side configuration is incorrect", before a line of our code runs.
REM It links and packages perfectly happily.
if not exist "%OBJ%\installer" mkdir "%OBJ%\installer"
python "tools\gen_installer_rc.py" --root "%ROOT%" --build "%BUILD%" ^
    --out "%GEN%" --version "%EDVR_VER%"
if errorlevel 1 ( echo [edvr] ERROR: installer resource generation failed & exit /b 1 )

REM The settings window's contents were generated above, before the d3d11
REM compile, since the in-headset menu shares the schema.

rc.exe /nologo /fo "%OBJ%\installer\payload.res" "%GEN%\payload.rc"
if errorlevel 1 ( echo [edvr] ERROR: rc.exe failed on the installer resources & exit /b 1 )


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
exit /b 0

:rig_installer_test
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
    "tools\installer_test\installer_test.cpp" ^
    "src\installer\plan.cpp" "src\installer\apply.cpp" ^
    "src\installer\detect.cpp" "src\installer\probe.cpp" ^
    "src\common\iniedit.cpp" "src\installer\state.cpp" ^
    "src\installer\mirror.cpp" ^
    "src\installer\settings.cpp" "src\installer\logbundle.cpp" ^
    /link /INCREMENTAL:NO %INSTALLER_LIBS%
if errorlevel 1 ( echo [edvr] ERROR: installer_test build failed & exit /b 1 )
"%BUILD%\installer_test.exe" "%ROOT%" "%BUILD%\insttest_scratch" || (
    echo [edvr] ERROR: the installer failed its own tests
    exit /b 1
)
exit /b 0

:rig_native_render_settings_test
if not exist "%OBJ%\native_render_settings" mkdir "%OBJ%\native_render_settings"
REM Pure render-resolution policy: one bounded factor is applied to both
REM dimensions and the tightest runtime/D3D maximum is shared by both eyes.
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\native_render_settings\\" /Fe"%BUILD%\native_render_settings_test.exe" ^
    "tools\native_render_settings_test\native_render_settings_test.cpp" ^
    "src\d3d11\native_render_settings.cpp" "src\common\vscreen_auto_state.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: native render settings test build failed & exit /b 1 )
"%BUILD%\native_render_settings_test.exe" --dry-run || exit /b 1
"%BUILD%\native_render_settings_test.exe" --self-test || exit /b 1
exit /b 0

:rig_vr_runtime_test
echo [edvr] === vr_runtime_test.exe ===
REM Which VR back end the process is REALLY on, checked against the real DLL
REM this build just made. Guards the failure that made the module exist: a
REM perfect install the game never opened, and eight log lines telling its
REM owner the file was missing.
if not exist "%OBJ%\vrruntimetest" mkdir "%OBJ%\vrruntimetest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\vrruntimetest"\ ^
    /Fe"%BUILD%\vr_runtime_test.exe" "tools\vr_runtime_test\vr_runtime_test.cpp" ^
    "src\d3d11\vr_runtime.cpp" "src\common\log.cpp" ^
    "src\common\config.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: vr_runtime_test build failed & exit /b 1 )
if not exist "%BUILD%\vrscratch_native" mkdir "%BUILD%\vrscratch_native"
REM The native runtime DLL under the name openvr_api.dll IS what the game
REM loads; the module-list check keys on that exact base name.
if not exist "%BUILD%\vrnative" mkdir "%BUILD%\vrnative"
copy /Y "%BUILD%\edvr_openxr_runtime.dll" "%BUILD%\vrnative\openvr_api.dll" >nul
"%BUILD%\vr_runtime_test.exe" "%BUILD%\vrscratch_native" native "%BUILD%\vrnative\openvr_api.dll" || (
    echo [edvr] ERROR: the VR runtime verdict is wrong for EDVR's own openvr_api.dll
    exit /b 1
)

REM A foreign openvr_api.dll -- a real DLL that is not EDVR's -- is exactly
REM tools\fakechain\fakechain.cpp copied under that name: it exports only
REM D3D11CreateDevice, so it carries neither native-runtime export. Built
REM fresh into this rig's own obj directory rather than trusted from
REM :rig_fakechain's %BUILD%\fakechain.dll, because run_jobs.py runs rigs
REM concurrently and gives no ordering, and no shared-write guarantee,
REM between two rig labels.
if not exist "%OBJ%\vrruntimetest_fakechain" mkdir "%OBJ%\vrruntimetest_fakechain"
cl.exe /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG /LD ^
    /Fo"%OBJ%\vrruntimetest_fakechain\\" /Fe"%OBJ%\vrruntimetest_fakechain\fakechain.dll" ^
    "tools\fakechain\fakechain.cpp" /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: vr_runtime_test's own fakechain build failed & exit /b 1 )
if not exist "%BUILD%\vrscratch_foreign" mkdir "%BUILD%\vrscratch_foreign"
if not exist "%BUILD%\vrforeign" mkdir "%BUILD%\vrforeign"
copy /Y "%OBJ%\vrruntimetest_fakechain\fakechain.dll" "%BUILD%\vrforeign\openvr_api.dll" >nul
"%BUILD%\vr_runtime_test.exe" "%BUILD%\vrscratch_foreign" foreign "%BUILD%\vrforeign\openvr_api.dll" || (
    echo [edvr] ERROR: the VR runtime verdict is wrong for a foreign openvr_api.dll
    exit /b 1
)
exit /b 0

:rig_python_gates
python tools\openxr_pe.py --self-test || exit /b 1
python tools\openxr_pe.py --native "%BUILD%\edvr_openxr_runtime.dll" || exit /b 1
python tools\openxr_pe.py --graphics "%BUILD%\edvr_openxr_graphics.dll" || exit /b 1
python tools\elite_oculus.py --self-test || exit /b 1
python tools\run_openxr_frontier.py --self-test || exit /b 1
python tools\cpu_profile.py --self-test || exit /b 1
REM Only profiling builds require the optional local .NET/TraceEvent analyzer.
REM Its parser tests must pass before this build can be flown for CPU capture.
if "%EDVR_PROFILE_SYMBOLS%"=="1" (
    python tools\cpu_profile.py --build-analyzer || exit /b 1
)
python tools\fetch_ffx_dx11.py --self-test || exit /b 1

REM Do the code, edvr.ini and the log messages agree about setting names?
REM
REM Three settings were read from the wrong section for the whole of 0.5.x and
REM nothing anywhere said so, because a key that does not match is simply not
REM there and a missing key falls back to its default. Only run when python is
REM available; a missing interpreter must not stop a build.
echo.
echo [edvr] === install-read check ===
python "tools\check_install_reads.py"
if errorlevel 1 ( echo [edvr] ERROR: a config reader runs only on the reload path & exit /b 1 )

echo [edvr] === exit-path check ===
python "tools\check_exit_paths.py"
if errorlevel 1 ( echo [edvr] ERROR: cleanup that matters runs only on FreeLibrary & exit /b 1 )

echo [edvr] === draw census diff self-test ===
REM The tool that reads the census a field session paid for. A parser that
REM drifts from the DC line format fails HERE, not in the ten minutes after a
REM user finally reproduced the effect being chased.
python "tools\diff_draw_census.py" --self-test || (
    echo [edvr] ERROR: the census diff tool failed its own test
    exit /b 1
)

echo [edvr] === crisp-UI gates parse self-test ===
REM This reader had none until 2026-09-07, and it reads the same census
REM emitter the two tools around it do. It asserts the PARSE, not the gates:
REM a new trailing field that swallowed q= would leave every report subtly
REM wrong with no error anywhere.
python "%ROOT%\tools\crisp_ui_gates.py" --self-test || (
    echo [edvr] ERROR: the crisp-UI gates reader failed its own test
    exit /b 1
)

echo [edvr] === stencil census self-test ===
REM The reader that answers "which stencil bits are free" and "does the
REM stencil survive to the motion-vector dispatch" off a census
REM (docs/per-object-motion.md Phase 0). It decodes the so= column the DLL
REM started printing on 2026-09-07, and a mask read one bit out is a wrong
REM answer that looks exactly like a right one. It fails HERE.
python "%ROOT%\tools\stencil_census.py" --self-test || (
    echo [edvr] ERROR: the stencil census tool failed its own test
    exit /b 1
)

echo [edvr] === draw identity self-test ===
REM The reader that answers "does a stable per-draw identity exist" off the
REM ia=/ib= columns the DLL prints since 2026-09-08 (docs/per-object-motion.md
REM Phase 0 question 4). Its numbers decide whether the design's memo can be
REM built at all, and a key assembled one field short reads as a plausible
REM percentage. It fails HERE.
python "%ROOT%\tools\draw_identity.py" --self-test || (
    echo [edvr] ERROR: the draw identity tool failed its own test
    exit /b 1
)

echo [edvr] === eye-run ledger self-test ===
REM The reader of the object probe's eye-run ledger (draws_HHMMSS.bin beside
REM the crops, since 2026-09-10; version 2 rows since 2026-09-21 also carry
REM the pixel shader hash and the render-target token). Its row layout must
REM match LedgerDraw in src\d3d11\object_probe.cpp byte for byte -- a field
REM one off reads as a plausible table of draws. It fails HERE.
python "%ROOT%\tools\eye_run_ledger.py" --self-test || (
    echo [edvr] ERROR: the eye-run ledger tool failed its own test
    exit /b 1
)

echo [edvr] === eye-split diff self-test ===
REM The tool that compares the two eyes of one frame. It registers the
REM eyes before it compares them, because their projections are off-centre
REM by different amounts and far content does not land on the same pixel in
REM both. A sign flip in that step reads as plausible either way, and once
REM cost a fix built on tiles that had landed on the Milky Way band. It
REM fails HERE, not in the next report somebody trusts.
python "tools\diff_eye_split.py" --self-test || (
    echo [edvr] ERROR: the eye-split diff tool failed its own test
    exit /b 1
)

echo [edvr] === install self-test ===
REM The tool that puts a build next to the game. Its --dry-run must write
REM NOTHING -- not a copy, not a backup, not a directory -- and its backup
REM naming is one string in one place, which is what keeps the game
REM directories from filling with .bak files named six different ways. The
REM test asserts both, against a fake game directory in the temp folder.
python "tools\install_edvr.py" --self-test || (
    echo [edvr] ERROR: the install tool failed its own test
    exit /b 1
)

echo [edvr] === log reader self-test ===
REM The tool that answers "is this log from the build I just installed"
REM before anybody reads a counter off it. Its version regex has to match
REM the line Log::note() really writes, timestamp prefix and all: anchored
REM without that prefix it matched the synthetic logs in its own test and
REM nothing whatsoever in the field. Its fixtures now carry the prefix.
python "tools\edvr_log.py" --self-test || (
    echo [edvr] ERROR: the log reader failed its own test
    exit /b 1
)

echo [edvr] === release-note reflow self-test ===
REM The tool that reflows release notes and docs. It must leave fenced
REM code, tables and long URLs exactly as they are, must be idempotent --
REM otherwise --check can never pass -- and must write UTF-8 with no BOM,
REM because PowerShell 5.1 reads a BOM-less file as the ANSI codepage and
REM has turned an em-dash into mojibake on a published comment before.
python "tools\reflow_notes.py" --self-test || (
    echo [edvr] ERROR: the reflow tool failed its own test
    exit /b 1
)
exit /b 0

:rig_kinematic_json_test
echo [edvr] === kinematic_json_test.exe ===
REM Build gate for the production KinematicEvalProbe JSON writer: three
REM serialization failures reached the flight journal before this rig
REM existed, and compilation cannot catch a dropped quote. The exe
REM serializes a deterministic fixture through the real writeJson; the
REM python gate strict-parses it and asserts every value round-trips.
if not exist "%OBJ%\kinematicjson" mkdir "%OBJ%\kinematicjson"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\kinematicjson\\" /Fe"%BUILD%\kinematic_json_test.exe" ^
    "tools\kinematic_json_test\kinematic_json_test.cpp" "src\d3d11\kinematic_eval_probe.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: kinematic JSON writer test build failed & exit /b 1 )
"%BUILD%\kinematic_json_test.exe" --dry-run || exit /b 1
"%BUILD%\kinematic_json_test.exe" --self-test || exit /b 1
python "tools\kinematic_json_selftest.py" --self-test || exit /b 1
exit /b 0

:rig_engine_velocity_test
echo [edvr] === engine_velocity_test.exe ===
REM Build gate for engine-record velocity (part of fix.temporal_aa, phase 1;
REM docs\kinematic-motion-injection-2026-09-19.md, 2026-09-23): the DXBC
REM patcher end to end on WARP (patched pool-family shaders disassemble,
REM reflect, create and DRAW the exact slot and depth at MRT6), the emit
REM bracket against a fake engine laid out as build 332841 (previous pose,
REM self-checking marker, the disagreement gate, the table), and the
REM compose's arithmetic from the shipped HLSL text against a double
REM reference. A parser or patcher that drifts fails here, not in a flight.
REM Since the 2026-09-23 fix round it also links engine_velocity.cpp itself
REM (EDVR_ENGINE_VELOCITY_RIG: no engine image to verify; the binding shadow
REM external) and drives its draw half through the flight's and the review's
REM cases -- cb1 re-maps, interleaved eyes, source swaps, pool writes, blend
REM states, depth formats, the stand-down -- plus the temporal pass's
REM compute-state save, sentinel by sentinel.
if not exist "%OBJ%\enginevelocity" mkdir "%OBJ%\enginevelocity"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /DEDVR_ENGINE_VELOCITY_RIG /DEDVR_BINDING_SHADOW_EXTERNAL /I"%GEN%" ^
    /Fo"%OBJ%\enginevelocity\\" /Fe"%OBJ%\enginevelocity\engine_velocity_test.exe" ^
    "tools\engine_velocity_test\engine_velocity_test.cpp" "src\d3d11\engine_velocity.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 ( echo [edvr] ERROR: engine velocity test build failed & exit /b 1 )
"%OBJ%\enginevelocity\engine_velocity_test.exe" --dry-run || exit /b 1
"%OBJ%\enginevelocity\engine_velocity_test.exe" --self-test || exit /b 1
exit /b 0

:rig_kinematic_probe_test
echo [edvr] === kinematic_probe_test.exe ===
REM Build gate for the KinematicEvalProbe's observe/clock logic: the
REM 2026-09-20 review's four probe-side findings (fabricated zero baselines
REM from faulted reads, identity-crossing motion, the artificial seed frame,
REM non-finite JSON floats) each reached flight analysis before this rig
REM existed. Drives the production observe() on synthetic records, including
REM a VirtualProtect page-fault fixture.
if not exist "%OBJ%\kinematicprobe" mkdir "%OBJ%\kinematicprobe"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\kinematicprobe\\" /Fe"%BUILD%\kinematic_probe_test.exe" ^
    "tools\kinematic_probe_test\kinematic_probe_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: kinematic probe test build failed & exit /b 1 )
"%BUILD%\kinematic_probe_test.exe" --dry-run || exit /b 1
"%BUILD%\kinematic_probe_test.exe" --self-test || exit /b 1
exit /b 0

:rig_transition_flash_prevent_test
echo [edvr] === transition_flash_prevent_test.exe ===
REM Build gate for transition_flash_prevent_core.h (docs\design-transition-
REM flash-engine-fix-2026-09-23.md): the pose classifier (and that it truly
REM ignores the padding lanes [3],[7],[11],[15], not merely documents that
REM it should), the validation and per-parent guard arithmetic including
REM LRU eviction, event grouping with the alternate latch, and the ring
REM window test's overflow safety at both ends of the frame counter. No
REM game and no CodeHook -- this drives the header directly, so a wrong
REM guard boundary or a wrapped frame subtraction is caught here rather
REM than costing a test flight to notice.
if not exist "%OBJ%\transitionflashprevent" mkdir "%OBJ%\transitionflashprevent"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\transitionflashprevent\\" /Fe"%BUILD%\transition_flash_prevent_test.exe" ^
    "tools\transition_flash_prevent_test\transition_flash_prevent_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: transition flash prevent test build failed & exit /b 1 )
"%BUILD%\transition_flash_prevent_test.exe" --dry-run || exit /b 1
"%BUILD%\transition_flash_prevent_test.exe" --self-test || exit /b 1
exit /b 0

:rig_scheduler_stack_json_test
echo [edvr] === scheduler_stack_json_test.exe ===
REM Build gate for the production SchedulerStackProbe JSON writer: the
REM kinematic writer shipped three serialization failures before its gate
REM existed, and compilation cannot catch a dropped quote. The exe
REM serializes a deterministic fixture through the real writeJson; the
REM python gate strict-parses it and asserts every value round-trips.
if not exist "%OBJ%\schedjson" mkdir "%OBJ%\schedjson"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\schedjson\\" /Fe"%BUILD%\scheduler_stack_json_test.exe" ^
    "tools\scheduler_stack_json_test\scheduler_stack_json_test.cpp" ^
    "src\d3d11\scheduler_stack_probe.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: scheduler stack JSON writer test build failed & exit /b 1 )
"%BUILD%\scheduler_stack_json_test.exe" --dry-run || exit /b 1
"%BUILD%\scheduler_stack_json_test.exe" --self-test || exit /b 1
python "tools\scheduler_stack_json_selftest.py" --self-test || exit /b 1
exit /b 0

:rig_scheduler_stack_probe_test
echo [edvr] === scheduler_stack_probe_test.exe ===
REM Build gate for the SchedulerStackProbe's capture/table logic: the
REM stride-tolerant stack scan (range filter, gap budget, cap, fault
REM handling), the FNV-1a-64 signature, and the bounded per-target
REM signature table (aggregation, overflow). These run at 36-540
REM calls/frame in flight; a capture-path defect costs a test flight to
REM find. Drives the production noteEntry/captureStack on synthetic
REM stacks, including a VirtualProtect guard-page fault fixture.
if not exist "%OBJ%\schedprobe" mkdir "%OBJ%\schedprobe"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\schedprobe\\" /Fe"%BUILD%\scheduler_stack_probe_test.exe" ^
    "tools\scheduler_stack_probe_test\scheduler_stack_probe_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: scheduler stack probe test build failed & exit /b 1 )
"%BUILD%\scheduler_stack_probe_test.exe" --dry-run || exit /b 1
"%BUILD%\scheduler_stack_probe_test.exe" --self-test || exit /b 1
exit /b 0

:rig_static_prop_gate_test
echo [edvr] === static_prop_gate_test.exe ===
REM Build gate for the StaticPropGate's cache logic: the change test
REM (hit/miss/first-sight), the forced-refresh failsafe, invalidation
REM epochs, oldest-evict at capacity and the SEH fault tolerance. These run
REM at ~432 calls/frame in flight; a cache defect costs a test flight AND
REM can read as invisible success (a wrong skip just costs CPU), so the rig
REM drives the production decide() on synthetic record arrays, including a
REM VirtualProtect guard-page fixture.
if not exist "%OBJ%\staticgate" mkdir "%OBJ%\staticgate"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\staticgate\\" /Fe"%BUILD%\static_prop_gate_test.exe" ^
    "tools\static_prop_gate_test\static_prop_gate_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: static prop gate test build failed & exit /b 1 )
"%BUILD%\static_prop_gate_test.exe" --dry-run || exit /b 1
"%BUILD%\static_prop_gate_test.exe" --self-test || exit /b 1
exit /b 0

:rig_static_prop_gate_json_test
echo [edvr] === static_prop_gate_json_test.exe ===
REM Build gate for the production StaticPropGate JSON writer: the kinematic
REM writer shipped three serialization failures before its gate existed,
REM and compilation cannot catch a dropped quote. The exe serializes a
REM deterministic fixture through the real writeJson; the python gate
REM strict-parses it and asserts every value round-trips.
if not exist "%OBJ%\staticgatejson" mkdir "%OBJ%\staticgatejson"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\staticgatejson\\" /Fe"%BUILD%\static_prop_gate_json_test.exe" ^
    "tools\static_prop_gate_json_test\static_prop_gate_json_test.cpp" ^
    "src\d3d11\static_prop_gate.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: static prop gate JSON writer test build failed & exit /b 1 )
"%BUILD%\static_prop_gate_json_test.exe" --dry-run || exit /b 1
"%BUILD%\static_prop_gate_json_test.exe" --self-test || exit /b 1
python "tools\static_prop_gate_json_selftest.py" --self-test || exit /b 1
exit /b 0

:rig_cull_gate_probe_test
echo [edvr] === cull_gate_probe_test.exe ===
REM Build gate for the cull gate probe (advanced.cull_gate_capture): the
REM production observers driven with synthetic engine memory laid out as the
REM decompiles read it (render context and views, record, pose context with
REM entries, models and sub-items, the traversal's gate context, the draw
REM builder's frame around its FUN_1442B3FC0 part test), wild pointers, a
REM pose mismatch, an unverifiable frame and a foreign caller; the real
REM writer's version 3 file (with its LOD table rows, one per pointer) is then
REM parsed by the reader, which asserts every field and prints the tables. A
REM probe that records nothing or a writer one field off fails here, not ten
REM minutes into a settlement capture.
if not exist "%OBJ%\cullgate" mkdir "%OBJ%\cullgate"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\cullgate\\" /Fe"%BUILD%\cull_gate_probe_test.exe" ^
    "tools\cull_gate_probe_test\cull_gate_probe_test.cpp" ^
    "src\d3d11\cull_gate_probe.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: cull gate probe test build failed & exit /b 1 )
"%BUILD%\cull_gate_probe_test.exe" --dry-run || exit /b 1
"%BUILD%\cull_gate_probe_test.exe" "%OBJ%\cullgate\gate_fixture.bin" || exit /b 1
python "tools\cull_gate_probe.py" --self-test || exit /b 1
python "tools\cull_gate_probe.py" --verify-fixture "%OBJ%\cullgate\gate_fixture.bin" || exit /b 1
python "tools\cull_gate_probe.py" --tables-only "%OBJ%\cullgate\gate_fixture.bin" || exit /b 1
exit /b 0

:rig_lod_governor_test
echo [edvr] === lod_governor_test.exe ===
REM Build gate for the settlement LOD governor (fix.settlement_detail: auto and
REM reduced scale the game's LOD scale right after FUN_142819D90 stores it;
REM advanced.settlement_detail_observe = 1 never writes): the policy's steps
REM and hysteresis, reduced's k_max at once; the engine arithmetic the shadow
REM repeats (FUN_1442B3FC0 / FUN_144308B30's rsqrt(rcp) distance, LOD
REM distance, LOD pick, the screen-size term); the observers on synthetic
REM engine memory laid out as the decompiles read it; the setter's bracket
REM against a fake context and a fake setter (only the builder's context is
REM written, k = 1 and observe leave the game's value, the table's limit and
REM an unwritable page stand acting down, off writes the game's value back);
REM the acting counts; the per-thread counters under four threads; and the
REM frame boundary end to end against a stub native timing feed, a fake
REM engine and a captured log -- the configure line naming the mechanism,
REM the first write, step lines, 30-second summaries, NOT ACTING, "k stayed
REM 1", and silence while off. It prints the observers' cost per call. A
REM governor that writes when it should not, or never counts, fails here,
REM not in the flight that was meant to price it.
if not exist "%OBJ%\lodgov" mkdir "%OBJ%\lodgov"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\lodgov\\" /Fe"%BUILD%\lod_governor_test.exe" ^
    "tools\lod_governor_test\lod_governor_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: LOD governor test build failed & exit /b 1 )
"%BUILD%\lod_governor_test.exe" --dry-run || exit /b 1
"%BUILD%\lod_governor_test.exe" --self-test || exit /b 1
exit /b 0

:rig_ui_quality_test
echo [edvr] === ui_quality_test.exe ===
REM Build gate for fix.ui_quality (docs/ui-layer-2026-09-23.md), both halves,
REM from the same headers the DLL compiles. The PANELS (ui_quality_math.h,
REM ui_sizing_math.h): the internal resolution from the runtime's
REM recommendation (3070 x 0.65 = 1995, 3032 x 0.65 = 1970, 3070x3032 x 0.5 =
REM 1535x1516, the 2458x2824 x 0.65 = 1597x1835 of the 2026-09-23 flight);
REM 2 tan(vFOV/2) on every recorded frustum; the interface surface's shape;
REM the sizing chains' formatter and verdicts; the engine-side panel factor
REM across the flights' states and the build-332841 bytes it patches. The LAYER
REM (ui_layer_math.h): the key; the layer's size and memory; the viewport and
REM scissor map and the jitter cancel; the blend conversion table, a multiply
REM included, and a CPU model proving layer-then-composite equals the game's
REM own blends over a thousand sequences; the depth-stencil classification
REM (the menu panel's stencil write, a stencil test, read-only views); the
REM composite's footprint; the door's arming and G1; every refusal of the
REM gate, in order; the world-screen gate (the journal OR the screen's own
REM depth count, with its hysteresis, journal off both ways); the route's
REM per-eye-frame price sums. On WARP (ui_layer_shaders.h, ui_layer_seed.h): a
REM jittered quad through the redirected viewport lands on the unjittered
REM pixels at all eight Halton phases; blended draws composited equal the same
REM draws into the frame, a multiply included; a stencil-tested quad drawn
REM against the layer's seeded copy of a stencil the game wrote matches the
REM same quad drawn into the frame; the 1.25 box filter; the debug view.
if not exist "%OBJ%\uiqualitytest" mkdir "%OBJ%\uiqualitytest"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\uiqualitytest\\" /Fe"%BUILD%\ui_quality_test.exe" ^
    "tools\ui_quality_test\ui_quality_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: ui quality test build failed & exit /b 1 )
"%BUILD%\ui_quality_test.exe" --dry-run || exit /b 1
"%BUILD%\ui_quality_test.exe" --self-test || exit /b 1
REM The layer's depth-stencil seed on its own (ui_layer_seed.h): the three
REM depth formats, every stencil value class, a scale, a jitter and a reused
REM seeder, read back texel for texel. Built outside build\ so its imported
REM D3D11CreateDevice resolves to System32's, not EDVR's proxy.
if not exist "%OBJ%\uilayerseed" mkdir "%OBJ%\uilayerseed"
cl.exe /nologo /O2 /Gy /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\uilayerseed\\" /Fe"%OBJ%\uilayerseed\seed_test.exe" ^
    "tools\ui_layer_seed_test\seed_test.cpp" ^
    /link /INCREMENTAL:NO /OPT:REF d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 ( echo [edvr] ERROR: ui layer seed test build failed & exit /b 1 )
"%OBJ%\uilayerseed\seed_test.exe" --self-test || exit /b 1
exit /b 0

:rig_pixel_probe_test
echo [edvr] === pixel_probe_test.exe ===
REM Build gate for advanced.pixel_probe (src/d3d11/pixel_probe.*), the "who
REM drew this pixel" instrument: a WARP device, a 256x256 eye-sized target
REM and a scissor-clipped solid-colour draw drive the module directly, no
REM vscreen.cpp and no hooks. Asserts a draw over a probe window is
REM reported with the right point, count and hashes; a draw elsewhere, or
REM one repainting the colour already there, is not; the first draw into
REM the target is caught through the baseline; an unconfigured or unarmed
REM probe logs nothing; slot exhaustion reports drops in the summary; and
REM a malformed advanced.pixel_probe value is refused whole, not
REM half-applied.
if not exist "%OBJ%\pixelprobe" mkdir "%OBJ%\pixelprobe"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\pixelprobe\\" /Fe"%OBJ%\pixelprobe\pixel_probe_test.exe" ^
    "tools\pixel_probe_test\pixel_probe_test.cpp" "src\d3d11\pixel_probe.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: pixel probe test build failed & exit /b 1 )
"%OBJ%\pixelprobe\pixel_probe_test.exe" --dry-run || exit /b 1
"%OBJ%\pixelprobe\pixel_probe_test.exe" --self-test || exit /b 1
exit /b 0
