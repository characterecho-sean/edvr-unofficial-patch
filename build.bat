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
REM  Usage:  build.bat [--clean] [--jobs N] [--dll-only] [--wait-lock]
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
REM  message saying to wait, and exits 1, rather than contending with the
REM  first one for the same CPU cores -- the documented cause of the
REM  vtable_test timing gate's flakes under load. --wait-lock instead waits
REM  for the lock to free and takes it in the same step, so no other build
REM  can take it in between. --rig children run concurrently by design
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
if /I "%~1"=="--wait-lock" goto arg_wait_lock
echo [edvr] unknown argument: %~1
exit /b 1
:arg_wait_lock
set "EDVR_WAIT_LOCK=1"
shift
goto parse_args
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
    REM SHIFT in parse_args changes %%0; ROOT retains the original script path.
    REM --wait-lock: the lock is taken in the same step that finds it free.
    if defined EDVR_WAIT_LOCK (
        python "%ROOT%\tools\build_lock.py" --acquire --wait --note "build.bat %*"
    ) else (
        python "%ROOT%\tools\build_lock.py" --acquire --note "build.bat %*"
    )
    REM A refusal must leave through :lock_refused. A literal "exit /b 1" inside
    REM this block stops the batch but exits 0 under cmd /c (tested 2026-10-14).
    if errorlevel 1 goto lock_refused
    set "EDVR_BUILD_GUARDED=1"
    call "%ROOT%\build.bat" %*
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

REM Reached only by goto from the lock guard above. The refusal message is
REM build_lock.py's own; this exit is what carries the failure to the caller.
:lock_refused
exit /b 1

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
REM Wall-clock phase clock: build\phase_times.json (tools\build_phases.py); build\rig_times.json holds per-rig times.
set "PHASES=%BUILD%\phase_times.json"
python tools\build_phases.py --start "%PHASES%"

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
REM The runner is proven before it is trusted with the gates below (and the rigs,
REM much later, use the same file).
python tools\run_jobs.py --self-test || exit /b 1

REM The Python gates: every tool's --self-test, the pure ones that used to sit
REM beside their generators further down (those generators still run where they
REM were), and the two source-only checks. They share nothing -- each builds its
REM fixtures in its own temp folder and reads only the checkout -- so
REM tools\run_jobs.py --gate runs them at once (--jobs, default every core),
REM prints each one's output whole under a header with its seconds, prints the
REM failures LAST so the tail of the log names them, and ends with
REM "gates: N passed in X s (serial sum Y s)": wall time against what it was.
REM Add a gate as one more --gate line; it must write nowhere but its own temp
REM folder. Notes on a few: layer_fixtures reads the three fixtures in the repo;
REM blackhole_optics (docs\black-holes.md) and sgra_optics
REM (docs\design-sagittarius-a-2026-09-30.md) are the physics those shaders are
REM held to; focus_watch is the guard run_jobs holds every rig to (no window, no
REM console, no keyboard-focus move) and its self-test starts it on real
REM processes; check_status_blocks (plain) checks the docs' Status blocks;
REM check_config_contract (plain) holds the keys the code reads, edvr.ini and the
REM log text to one another -- it reads only src\ and edvr.ini.
python tools\run_jobs.py ^
    --gate "tools\fetch_openxr_loader.py --self-test" ^
    --gate "tools\fetch_xess.py --self-test" ^
    --gate "tools\gen_installer_rc.py --self-test" ^
    --gate "tools\package_native.py --self-test" ^
    --gate "tools\install_proton.py --self-test" ^
    --gate "tools\build_diff.py --self-test" ^
    --gate "tools\build_receipt.py --self-test" ^
    --gate "tools\build_lock.py --self-test" ^
    --gate "tools\release_credits.py --self-test" ^
    --gate "tools\flash_patch_residual.py --self-test" ^
    --gate "tools\check_status_blocks.py --self-test" ^
    --gate "tools\check_status_blocks.py" ^
    --gate "tools\layer_fixtures.py --self-test" ^
    --gate "tools\blackhole_optics.py --self-test" ^
    --gate "tools\sgra_optics.py --self-test" ^
    --gate "tools\focus_watch.py --self-test" ^
    --gate "tools\check_config_contract.py --self-test" ^
    --gate "tools\check_config_contract.py" ^
    --gate "tools\gen_fsr_hlsl.py --self-test" ^
    --gate "tools\gen_smaa_hlsl.py --self-test" ^
    --gate "tools\gen_settings_schema.py --self-test" ^
    --gate "tools\gen_exports.py --self-test" ^
    --gate "tools\fetch_ngx.py --self-test" ^
    --gate "tools\build_flat_sdk_bench_proxy.py --self-test" ^
    --gate "tools\build_phases.py --self-test" ^
    --gate "tools\compile_cache.py --self-test" || exit /b 1

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
 /DEDVR_HAVE_VERSION_HEADER=1 ^
 /I"%GEN%"
REM Every compile below goes through tools\compile_cache.py, which skips a
REM compile whose sources, project headers, flags, toolchain and INCLUDE are
REM unchanged and otherwise runs cl.exe exactly as written. CC is inherited by
REM the --rig children like CFLAGS. EDVR_NO_COMPILE_CACHE=1 turns it off.
set "CC=python tools\compile_cache.py cl.exe"

echo.
REM The version used to be a /D flag, which changed every TU's flags on every
REM commit or dirty flip. It is now edvr_version.h, rewritten only when the
REM version changes; the flag is constant, so rigs that compile these files
REM without it keep their #ifndef fallbacks.
python "tools\gen_installer_rc.py" --version-header --version "%EDVR_VER%" --out "%GEN%"
if errorlevel 1 ( echo [edvr] ERROR: version header generation failed & exit /b 1 )
REM The runtime config audit data: known keys + the moved-from map,
REM generated from the same sources the late contract check verifies.
python "tools\check_config_contract.py" --quiet --emit "%GEN%\config_contract_gen.h"
if errorlevel 1 ( echo [edvr] ERROR: contract header generation failed & exit /b 1 )

python tools\build_phases.py --mark "%PHASES%" "python gates and setup"
echo [edvr] === precompiled temporal shaders ===
REM Fixed HLSL belongs in the build: compiling it in the first Present delayed
REM the intro by 18 seconds. The header carries a key over the HLSL, the
REM variant table and the compiler DLL, and is regenerated whenever that key
REM changes, so stale bytecode cannot survive a source change -- while an
REM unchanged shader costs nothing instead of the AA variant's 19 s of fxc on
REM every build (measured 2026-09-15). The generator tests never initialize a GPU.
REM AMD FSR 1.0 as embeddable HLSL. Generated rather than committed so the
REM vendored headers stay byte-identical to upstream (src\d3d11\fsr\).
python "tools\gen_fsr_hlsl.py" --root "%ROOT%" --out "%GEN%"
if errorlevel 1 ( echo [edvr] ERROR: FSR shader embedding failed & exit /b 1 )
REM SMAA 1x (MIT, src\d3d11\smaa\, pinned to upstream commit 71c806a8) as embeddable HLSL, the same way: the map temporal-off frames below native.
python "tools\gen_smaa_hlsl.py" --root "%ROOT%" --out "%GEN%"
if errorlevel 1 ( echo [edvr] ERROR: SMAA shader embedding failed & exit /b 1 )

if not exist "%OBJ%\temporalshader" mkdir "%OBJ%\temporalshader"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\temporalshader\\" /Fe"%OBJ%\temporalshader\temporal_shader_build.exe" ^
    "tools\temporal_shader_build\temporal_shader_build.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: temporal shader compiler build failed & exit /b 1 )
REM The native runtime's four stereo-renderer shaders (src\openxr\stereo_shader_source.h) are compiled here too, into a header of their own
REM (--stereo-output), so edvr_openxr_runtime.dll creates them from bytes and carries no HLSL compiler: it imported d3dcompiler_47.dll until 2026-10-01
REM for four D3DCompile calls. The runtime and the two rigs that compile d3d11_stereo.cpp find the header through /I"%GEN%"; the PE gate below fails the
REM build if either DLL imports the compiler again.
"%OBJ%\temporalshader\temporal_shader_build.exe" --self-test || exit /b 1
"%OBJ%\temporalshader\temporal_shader_build.exe" --output "%GEN%\temporal_shader_bytecode.h" --stereo-output "%GEN%\openxr_stereo_shader_bytecode.h" --dry-run || exit /b 1
"%OBJ%\temporalshader\temporal_shader_build.exe" --output "%GEN%\temporal_shader_bytecode.h" --stereo-output "%GEN%\openxr_stereo_shader_bytecode.h" || exit /b 1

python tools\build_phases.py --mark "%PHASES%" "shader generation"
echo [edvr] === d3d11.dll ===
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
    --extra-export edvr_selftest_flat_sdk_snapshot ^
    --extra-export edvr_selftest_flat_sdk_owner_view ^
    --extra-export edvr_selftest_flat_sdk_foreground_map ^
    --extra-export edvr_selftest_binding ^
    --extra-export edvr_selftest_profile_post ^
    --extra-export edvr_selftest_profile_state ^
    --extra-export edvr_selftest_profile_skip_teardown ^
    --extra-export edvrAcquireGraphicsBridge ^
    --extra-export edvrAcquireRenderBoundary ^
    --extra-export edvrAcquireNativeGraphics ^
    --extra-export edvr_selftest_graphics_bridge ^
    --extra-export edvrTemporalAa ^
    --extra-export edvrEyeCaptureUntreated ^
    --extra-export edvrTemporalAaNoteHead ^
    --extra-export edvrSharpen ^
    --extra-export edvrDepthProbeSelftest ^
    --extra-export edvrAcquireNativeMenu ^
    --extra-export edvrAcquireNativeTemporal ^
    --extra-export edvrAcquireNativeSharpen ^
    --extra-export edvrAcquireNativeFrame ^
    --extra-export edvrAcquireNativeTiming ^
    --extra-export edvrReadNativePresentTrace ^
    --extra-export "edvrNativeStartupRouting DATA" ^
    --extra-export edvrQueryOculusRouting ^
    --extra-export edvrQueryNativeRenderSettings ^
    --extra-export edvrPublishNativeRenderSizing ^
    --extra-export edvrQueryNativeRenderSizing ^
    --extra-export edvrGpuFrameEvent ^
    --extra-export edvrPluginDispatchUpdate ^
    --extra-export edvrPluginDispatchRenderEye ^
    --extra-export edvrPluginDispatchFilterInput ^
    --extra-export edvrPluginHasActivePlugins
if errorlevel 1 ( echo [edvr] ERROR: export generation failed & exit /b 1 )

REM Both halves link "%OBJ%\<half>\*.obj" -- a wildcard -- and every build
REM compiles every listed source, so an object left behind by a source that
REM was REMOVED from the list is the only thing in that directory a build did
REM not just make. It links anyway. Measured 2026-09-13: early_session.cpp
REM was deleted, its .obj stayed, and the link failed on three symbols it
REM still referenced -- the lucky case. Had those symbols still existed, the
REM removed feature would have linked straight back into the DLL with no
REM line anywhere saying so. compile_cache.py --prune (on the d3d11 compile
REM below) deletes every object no listed source produces, which makes the
REM object set exactly the source list; --keep protects the ml64 thunks.obj.
REM The cleanup happens after DLL-only promotion has verified its receipt, so a
REM rejected promotion does not disturb the previous build.

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

REM Intel XeSS-SR SDK for the flat profile. The D3D11 proxy bridges to D3D12
REM at runtime; it imports no XeSS library. A missing SDK builds a refusal stub.
set XESS=
if defined EDVR_XESS_SDK set XESS=%EDVR_XESS_SDK%
if not defined XESS if exist "%ROOT%\third_party\xess\include\xess\xess.h" set XESS=%ROOT%\third_party\xess
if not defined XESS if exist "%LOCALAPPDATA%\EDVR\xess-sdk\include\xess\xess.h" set XESS=%LOCALAPPDATA%\EDVR\xess-sdk
set XESSFLAGS=/DEDVR_HAVE_XESS=0
if defined XESS (
    python "tools\fetch_xess.py" --verify "%XESS%" || (
        echo [edvr] ERROR: the XeSS SDK at %XESS% is not the pinned one. tools\fetch_xess.py
        exit /b 1
    )
    set XESSFLAGS=/DEDVR_HAVE_XESS=1 /I"%XESS%\include"
    copy /Y "%XESS%\bin\libxess.dll" "%BUILD%\libxess.dll" >nul || exit /b 1
    copy /Y "%XESS%\LICENSE.txt" "%BUILD%\INTEL-XESS-LICENSE.txt" >nul || exit /b 1
    copy /Y "%XESS%\third-party-programs.txt" "%BUILD%\INTEL-XESS-THIRD-PARTY.txt" >nul || exit /b 1
    echo [edvr] XeSS-SR SDK: %XESS%
) else (
    if exist "%BUILD%\libxess.dll" del /q "%BUILD%\libxess.dll"
    if exist "%BUILD%\INTEL-XESS-LICENSE.txt" del /q "%BUILD%\INTEL-XESS-LICENSE.txt"
    if exist "%BUILD%\INTEL-XESS-THIRD-PARTY.txt" del /q "%BUILD%\INTEL-XESS-THIRD-PARTY.txt"
    echo [edvr] NO XeSS SDK. Flat XeSS-SR will refuse at runtime. Fetch with tools\fetch_xess.py.
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
        --context "xess_request=%EDVR_XESS_SDK%" ^
        --context "xess=%XESS%" ^
        --context "ffx_request=%EDVR_FFX_DX11%" ^
        --context "ffx=%FFX%" ^
        --context "fsr_none=%FSR_NONE%" || (
            echo [edvr] ERROR: DLL-only promotion is not allowed without the
            echo        matching green full build receipt.
            exit /b 1
        )
)

if not exist "%OBJ%\d3d11" mkdir "%OBJ%\d3d11"
ml64.exe /nologo /c /Fo"%OBJ%\d3d11\thunks.obj" "%GEN%\edvr_thunks_d3d11.asm" >nul
if errorlevel 1 ( echo [edvr] ERROR: ml64 failed & exit /b 1 )

python tools\compile_cache.py --prune --keep thunks.obj cl.exe %CFLAGS% %NGXFLAGS% %FSRFLAGS% %XESSFLAGS% /Fo"%OBJ%\d3d11"\ ^
    "src\common\log.cpp" "src\common\config.cpp" ^
    "src\common\config_audit.cpp" ^
    "src\common\guard.cpp" "src\common\vtable_hook.cpp" "src\common\code_hook.cpp" ^
    "src\common\hotkey.cpp" "src\common\proxy.cpp" ^
    "src\common\frame_flag.cpp" ^
    "src\common\iat_hook.cpp" "src\common\iniedit.cpp" ^
    "src\d3d11\input_gate.cpp" "src\d3d11\joy_watch.cpp" "src\d3d11\hotkey_capture.cpp" "src\d3d11\menu.cpp" ^
    "src\d3d11\oculus_route.cpp" ^
    "src\d3d11\menu_keys.cpp" ^
    "src\d3d11\menu_panel.cpp" "src\d3d11\perf_monitor.cpp" "src\d3d11\native_perf_history.cpp" "src\d3d11\native_benchmark_collector.cpp" ^
    "src\d3d11\stall_watch.cpp" "src\d3d11\vram_tick.cpp" ^
    "src\d3d11\native_menu.cpp" ^
    "src\d3d11\native_temporal.cpp" "src\d3d11\flat_temporal.cpp" "src\d3d11\flat_compute_capture.cpp" "src\d3d11\flat_compute_readback.cpp" ^
    "src\d3d11\flat_runtime.cpp" "src\d3d11\flat_mono_resolve.cpp" "src\d3d11\xess_engine.cpp" "src\d3d11\map_easu.cpp" "src\d3d11\map_smaa.cpp" "src\d3d11\flat_projection_scope.cpp" "src\d3d11\flat_projection_runtime.cpp" ^
    "src\d3d11\flat_camera_producer_probe.cpp" "src\d3d11\flat_camera_inject.cpp" ^
    "src\d3d11\vr_world_route.cpp" "src\d3d11\vr_world_mips.cpp" "src\d3d11\vr_camera_census.cpp" ^
    "src\d3d11\native_sharpen.cpp" ^
    "src\d3d11\native_frame.cpp" ^
    "src\d3d11\native_timing.cpp" ^
    "src\d3d11\map_wait.cpp" ^
    "src\d3d11\native_render_settings.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_frame_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\d3d11\gpu_census.cpp" "src\d3d11\gpu_pass_profile.cpp" "src\d3d11\blend_probe.cpp" ^
    "src\d3d11\d3d11_proxy.cpp" "src\d3d11\device_hook.cpp" ^
    "src\d3d11\format_support_log.cpp" ^
    "src\d3d11\graphics_bridge.cpp" ^
    "src\d3d11\render_boundary.cpp" ^
    "src\d3d11\exposure_fix.cpp" "src\d3d11\vscreen.cpp" ^
    "src\d3d11\glitch_frame.cpp" ^
    "src\d3d11\transition_flash_eye_base.cpp" ^
    "src\d3d11\explorer_cam.cpp" ^
    "src\d3d11\vscreen_res.cpp" "src\common\vscreen_auto_state.cpp" "src\d3d11\vscreen_footprint.cpp" ^
    "src\d3d11\binding_shadow.cpp" ^
    "src\d3d11\vr_runtime.cpp" ^
    "src\d3d11\journal_watch.cpp" "src\d3d11\terrain_checkerboard.cpp" ^
    "src\d3d11\elite_binds.cpp" "src\d3d11\draw_census.cpp" ^
    "src\d3d11\object_probe.cpp" ^
    "src\d3d11\pixel_probe.cpp" ^
    "src\d3d11\object_record_writer_probe.cpp" "src\d3d11\object_record_writer_hook.cpp" ^
    "src\d3d11\kinematic_eval_probe.cpp" "src\d3d11\kinematic_eval_hook.cpp" ^
    "src\d3d11\store_hook.cpp" "src\d3d11\view_source.cpp" "src\d3d11\optimized_station.cpp" "src\d3d11\pad_state_probe.cpp" "src\d3d11\hmd_reset_press.cpp" ^
    "src\d3d11\scheduler_stack_probe.cpp" "src\d3d11\scheduler_stack_hook.cpp" ^
    "src\d3d11\cull_gate_probe.cpp" ^
    "src\d3d11\engine_velocity.cpp" "src\d3d11\skin_join_gpu.cpp" "src\d3d11\skin_entity_hook.cpp" ^
    "src\d3d11\celestial_motion.cpp" ^
    "src\d3d11\fss_res.cpp" ^
    "src\d3d11\fss_panel.cpp" ^
    "src\d3d11\fss_reveal.cpp" ^
    "src\d3d11\vertex_resync_hook.cpp" "src\d3d11\light_grid_cache_hook.cpp" ^
    "src\d3d11\xinput_watch.cpp" ^
    "src\d3d11\fss_panel_rect.cpp" ^
    "src\d3d11\panel_curve.cpp" "src\d3d11\screen_motion.cpp" "src\d3d11\weapon_motion.cpp" ^
    "src\d3d11\remlok_fix.cpp" "src\d3d11\holo_fix.cpp" ^
    "src\d3d11\target_sharp.cpp" "src\d3d11\night_vision.cpp" ^
    "src\d3d11\wake_pulse.cpp" "src\d3d11\vr_ssaa_gate.cpp" "src\d3d11\vr_ssaa_hold.cpp" "src\d3d11\vr_display_observer.cpp" "src\d3d11\vr_window_trampolines.cpp" ^
    "src\d3d11\mode_watch.cpp" ^
    "src\d3d11\ui_depth.cpp" ^
    "src\d3d11\ui_layer.cpp" "src\d3d11\ui_surfaces.cpp" "src\d3d11\ui_panel_scale.cpp" "src\d3d11\orbital_width.cpp" "src\d3d11\supercruise_bars.cpp" ^
    "third_party\dxbc_hash\DxilHash.cpp" ^
    "src\d3d11\backdrop_fix.cpp" ^
    "src\d3d11\scrim_fix.cpp" ^
    "src\d3d11\quad_probe.cpp" ^
    "src\d3d11\intro_panel.cpp" ^
    "src\d3d11\intro_curve.cpp" ^
    "src\d3d11\intro_skip.cpp" ^
    "src\d3d11\aa_compare.cpp" ^
    "src\d3d11\intro_upscale.cpp" ^
    "src\d3d11\temporal_pass.cpp" ^
    "src\d3d11\depth_probe.cpp" ^
    "src\d3d11\luma_probe.cpp" ^
    "src\d3d11\sky_ring.cpp" ^
    "src\d3d11\terrain_cull_cover_hook.cpp" ^
    "src\d3d11\dlaa.cpp" ^
    "src\d3d11\fsr3_engine.cpp" ^
    "src\d3d11\sharpen_pass.cpp" ^
    "src\d3d11\flat_sharpen.cpp" "src\d3d11\flat_ui_layer.cpp" ^
    "src\d3d11\loader_panel.cpp" ^
    "src\d3d11\splash_dim.cpp" ^
    "src\d3d11\billboard_fix.cpp" ^
    "src\d3d11\particle_fix.cpp" "src\d3d11\shader_swap.cpp" "src\d3d11\sunglare_fix.cpp" ^
    "src\plugins\plugin_manager.cpp"
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
    "%OBJ%\d3d11\*.obj" "%OBJ%\d3d11\dxbc_notice.res" "%OBJ%\d3d11\version.res" kernel32.lib user32.lib gdi32.lib version.lib d3dcompiler.lib bcrypt.lib %NGXLIB% %FSRLIB%
if errorlevel 1 ( echo [edvr] ERROR: link failed & exit /b 1 )

echo [edvr] built %BUILD%\d3d11.dll
python tools\build_phases.py --mark "%PHASES%" "d3d11.dll"
copy /y "%BUILD%\d3d11.dll" "%BUILD%\edvr_openxr_graphics.dll" >nul || exit /b 1
echo [edvr] built %BUILD%\edvr_openxr_graphics.dll

REM The offline-only bench proxy (build\flat_sdk_bench_proxy.dll) is built by :rig_flat_sdk_bench_proxy among the test rigs below: it only reads
REM the production objects in %OBJ%\d3d11, and nothing in this build loads it.

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
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /LD /D_CRT_SECURE_NO_WARNINGS %EDVR_CPU_COMPILE% ^
    /I"third_party\openxr\include" /I"%GEN%" /Fo"%OBJ%\openxr_module\\" ^
    /DEDVR_HAVE_VERSION_HEADER=1 ^
    /Fe"%BUILD%\edvr_openxr_runtime.dll" "src\openxr\native_module.cpp" ^
    "src\openxr\d3d11_stereo.cpp" "src\openxr\session_binding.cpp" "src\openxr\openvr_system.cpp" ^
    "src\openxr\eye_capture.cpp" "src\openxr\skybox_capture.cpp" ^
    "src\openxr\shared_texture_transfer.cpp" "src\openxr\producer_gpu_timing.cpp" ^
    "src\openxr\device_gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\openxr\openvr_compositor.cpp" "src\openxr\openvr_auxiliary.cpp" ^
    "src\common\frame_flag.cpp" ^
    /link /INCREMENTAL:NO %EDVR_CPU_LINK% /PDB:"%BUILD%\edvr_openxr_runtime.pdb" /DEF:"src\openxr\native_module.def" "%OBJ%\openxr_module\version.res" d3d11.lib dxgi.lib user32.lib gdi32.lib
if errorlevel 1 ( echo [edvr] ERROR: native runtime module build failed & exit /b 1 )
python tools\build_phases.py --mark "%PHASES%" "runtime dll"

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
REM accident. run_jobs.py refuses, at the plan, any rig that links d3d11.lib
REM into an output in %BUILD%; a rig that wants System32's checks at run time
REM which d3d11.dll it runs on (reportSystemD3D11Only), and one that loads the
REM proxy on purpose checks that none came with its exe (reportNoD3D11Mapped).
REM The --quiet rigs hold wall-clock intervals to tight bounds; they run alone,
REM after the rest, with the whole machine. Only their test runs need that, so
REM each is split (see the rig area below): its compiles run in the pool like
REM any other rig's and only its runs wait their turn.
REM openxr_module_test loads openxr_export_fixture.dll, which
REM :rig_openxr_exports_test builds; --after holds the reader back until the
REM writer has actually finished (see the rig rules below). The same for the five
REM flat_mono_resolve_shard<K> rigs, which copy and run the exe that
REM :rig_flat_mono_resolve_test compiles.
REM The runner also fails the build if any process of any rig showed a window,
REM opened a console or took the keyboard focus from whoever is typing
REM (tools\focus_watch.py), and its log carries a "focus_watch:" line saying the
REM guard looked; a log without one did not run it. The graphics proxy takes the
REM foreground for a swap chain's window at launch (d3d11.focus_on_launch) only if
REM it is a top-level window on the desktop (src\d3d11\focus_target.h), and a rig
REM that needs a window for one uses tools\openxr_native_test\present_device.h,
REM whose window is a child of a message-only one.
set "RUN_JOBS_ARGS="
if defined EDVR_JOBS set "RUN_JOBS_ARGS=--jobs %EDVR_JOBS%"
python tools\run_jobs.py --script "%ROOT%\build.bat" --times "%BUILD%\rig_times.json" ^
    --exe-dir "%BUILD%" --quiet native_timing_test,gpu_timing_test,gpu_census_test,gpu_pass_profile_test,vtable_test,stall_sampler_test ^
    --after openxr_module_test=openxr_exports_test ^
    --after flat_mono_resolve_shard1=flat_mono_resolve_test ^
    --after flat_mono_resolve_shard2=flat_mono_resolve_test ^
    --after flat_mono_resolve_shard3=flat_mono_resolve_test ^
    --after flat_mono_resolve_shard4=flat_mono_resolve_test ^
    --after flat_mono_resolve_shard5=flat_mono_resolve_test ^
    %RUN_JOBS_ARGS% || exit /b 1
python tools\build_phases.py --mark "%PHASES%" "test rigs"

REM The config contract check (tools\check_config_contract.py) runs with the gates at the top: it reads only src\ and edvr.ini.

REM The native OpenXR build is the only build; nothing above ran a legacy path.
REM --forbid-import: neither DLL may import the HLSL compiler at load time. The runtime did (four D3DCompile calls, moved to build time 2026-10-01: its link line
REM no longer names d3dcompiler.lib either, so a compile that comes back fails to link); the d3d11 proxy loads d3dcompiler_47.dll only on demand (shader_swap.cpp).
copy /y "%BUILD%\edvr_openxr_runtime.dll" "%BUILD%\openvr_api.dll" >nul || exit /b 1
python tools\openxr_pe.py --native "%BUILD%\openvr_api.dll" --forbid-import d3dcompiler_47.dll || exit /b 1
python tools\openxr_pe.py --graphics "%BUILD%\d3d11.dll" --forbid-import d3dcompiler_47.dll || exit /b 1
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
    --context "xess_request=%EDVR_XESS_SDK%" ^
    --context "xess=%XESS%" ^
    --context "ffx_request=%EDVR_FFX_DX11%" ^
    --context "ffx=%FFX%" ^
    --context "fsr_none=%FSR_NONE%" || exit /b 1
python tools\build_phases.py --mark "%PHASES%" "final checks and receipt"
python tools\build_phases.py --report "%PHASES%"
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
python tools\openxr_pe.py --native "%BUILD%\openvr_api.dll" --forbid-import d3dcompiler_47.dll || exit /b 1
python tools\openxr_pe.py --graphics "%BUILD%\d3d11.dll" --forbid-import d3dcompiler_47.dll || exit /b 1
python tools\build_phases.py --mark "%PHASES%" "dll-only outputs"
python tools\build_phases.py --report "%PHASES%"
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
if not defined CC (
    echo [edvr] ERROR: --rig is tools\run_jobs.py's entry point; CC is not set
    exit /b 1
)
pushd "%ROOT%"
call :rig_%EDVR_RIG%
if errorlevel 1 exit /b 1
exit /b 0

:rig_smoke
echo [edvr] === smoke.exe ===
if not exist "%OBJ%\smoke" mkdir "%OBJ%\smoke"
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG ^
    /Fo"%OBJ%\smoke\\" /Fe"%BUILD%\smoke.exe" ^
    "tools\smoke\smoke.cpp" /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: smoke build failed & exit /b 1 )
echo [edvr] built %BUILD%\smoke.exe
REM The focused modes run here; until 2026-10-16 this rig only compiled smoke.exe and nothing ran any mode, which is how
REM advanced.d3d11_fixes = 0 crashed on the first Present for three releases with every gate green. --fixes-off-device presents
REM frames through a swap chain with the fixes off. All three load build\d3d11.dll, so they run one after another in this
REM rig (each clears the sentinel it uses first), under this rig's own EDVR_LOG_DIR.
"%BUILD%\smoke.exe" "%BUILD%\d3d11.dll" --fixes-off-device || (
    echo [edvr] ERROR: smoke --fixes-off-device failed: with the fixes off the proxy must still survive a Present
    exit /b 1
)
"%BUILD%\smoke.exe" "%BUILD%\d3d11.dll" --recovery-device || (
    echo [edvr] ERROR: smoke --recovery-device failed
    exit /b 1
)
"%BUILD%\smoke.exe" "%BUILD%\d3d11.dll" --foreign-device || (
    echo [edvr] ERROR: smoke --foreign-device failed
    exit /b 1
)
exit /b 0

:rig_fakechain
echo [edvr] === fakechain.dll ===
if not exist "%OBJ%\fakechain" mkdir "%OBJ%\fakechain"
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG /LD ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\vtabletest"\ ^
    /DEDVR_VTABLE_TEST /Fe"%BUILD%\vtable_test.exe" "tools\vtable_test\vtable_test.cpp" ^
    "src\d3d11\shader_swap.cpp" "src\common\vtable_hook.cpp" "src\common\code_hook.cpp" "src\common\guard.cpp" ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DEDVR_MENU_TEST /I"%GEN%" /I"third_party\openxr\include" ^
    /Fo"%OBJ%\native_menu\\" /Fe"%BUILD%\native_menu_test.exe" ^
    "tools\native_menu_test\native_menu_test.cpp" "src\d3d11\native_menu.cpp" ^
    "src\openxr\eye_capture.cpp" "src\openxr\shared_texture_transfer.cpp" "src\openxr\producer_gpu_timing.cpp" ^
    "src\d3d11\input_gate.cpp" "src\d3d11\joy_watch.cpp" "src\d3d11\menu_panel.cpp" ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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

:rig_fss_reveal_test
echo [edvr] === fss_reveal_test.exe ===
REM The FSS reveal fix (src\d3d11\fss_reveal_core.h, fss_reveal_replay.h, fss_reveal.cpp): the pure decisions (a hash-only match on another target is declined, no replay without a recorded downsample,
REM flat never replays, only the first composite of a frame decides), the real state capture and replay on a WARP device with the debug layer (the state after equals the state before, the constants are
REM read from a private copy), and source pins on the hook. No DLL, no game, no window.
if not exist "%OBJ%\fssreveal" mkdir "%OBJ%\fssreveal"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\fssreveal\\" ^
    /Fe"%BUILD%\fss_reveal_test.exe" "tools\fss_reveal_test\fss_reveal_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: fss reveal test build failed & exit /b 1 )
"%BUILD%\fss_reveal_test.exe" --dry-run || exit /b 1
"%BUILD%\fss_reveal_test.exe" --self-test "%ROOT%" || exit /b 1
exit /b 0

:rig_native_trim_test
echo [edvr] === native_trim_test.exe ===
if not exist "%OBJ%\native_trim" mkdir "%OBJ%\native_trim"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\native_trim\\" /Fe"%BUILD%\native_trim_test.exe" ^
    "tools\native_trim_test\native_trim_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: native FOV trim policy test build failed & exit /b 1 )
"%BUILD%\native_trim_test.exe" --dry-run || exit /b 1
"%BUILD%\native_trim_test.exe" --self-test || exit /b 1
exit /b 0

:rig_dlaa_mode_test
echo [edvr] === dlaa_mode_test.exe ===
if not exist "%OBJ%\dlaa_mode" mkdir "%OBJ%\dlaa_mode"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\dlaa_mode\\" /Fe"%BUILD%\dlaa_mode_test.exe" ^
    "tools\dlaa_mode_test\dlaa_mode_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: dlaa_mode_test build failed & exit /b 1 )
"%BUILD%\dlaa_mode_test.exe" --dry-run || exit /b 1
"%BUILD%\dlaa_mode_test.exe" --self-test || exit /b 1
REM tools\dlaa_mode_test\mutants.py --self-test holds the mutation list to the sources as they are; --run builds the rig against each edit.
python "tools\dlaa_mode_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_format_support_test
REM The device capability log (src\d3d11\format_support_log.h): its pure decode, line
REM formats and lock-free ledger; the two vtable slots the device hook patches, held
REM against the SDK's ID3D11DeviceVtbl; and, in an owned child process, the very hook
REM bodies on a real WARP device with the real log, every answer with the hook compared
REM with the answer without it. Built outside %BUILD%, where it can only ever load
REM System32's d3d11.dll, never the proxy beside the game DLLs.
if not exist "%OBJ%\formatsupport" mkdir "%OBJ%\formatsupport"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\formatsupport\\" ^
    /Fe"%OBJ%\formatsupport\format_support_test.exe" ^
    "tools\format_support_test\format_support_test.cpp" "tools\format_support_test\device_slots.cpp" ^
    "src\d3d11\format_support_log.cpp" "src\common\vtable_hook.cpp" "src\common\guard.cpp" ^
    "src\common\log.cpp" "src\common\config.cpp" "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: format_support_test build failed & exit /b 1 )
"%OBJ%\formatsupport\format_support_test.exe" --dry-run || exit /b 1
"%OBJ%\formatsupport\format_support_test.exe" --self-test || exit /b 1
exit /b 0

:rig_native_temporal_test
echo [edvr] === native_temporal_test.exe ===
if not exist "%OBJ%\native_temporal" mkdir "%OBJ%\native_temporal"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /I"third_party\openxr\include" ^
    /Fo"%OBJ%\native_temporal\\" /Fe"%BUILD%\native_temporal_test.exe" ^
    "tools\native_temporal_test\native_temporal_test.cpp" "src\d3d11\native_temporal.cpp" ^
    "src\d3d11\glitch_frame.cpp" "src\d3d11\vr_runtime.cpp" "src\d3d11\transition_flash_eye_base.cpp" ^
    "src\common\code_hook.cpp" "src\common\guard.cpp" ^
    "src\common\config.cpp" "src\common\frame_flag.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: native temporal test build failed & exit /b 1 )
"%BUILD%\native_temporal_test.exe" --dry-run || exit /b 1
"%BUILD%\native_temporal_test.exe" --self-test || exit /b 1
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /I"third_party\openxr\include" ^
    /Fo"%OBJ%\native_temporal\\" /Fe"%BUILD%\native_temporal_gpu_test.exe" ^
    "tools\native_temporal_test\native_temporal_gpu_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: native temporal GPU test build failed & exit /b 1 )
"%BUILD%\native_temporal_gpu_test.exe" --dry-run || exit /b 1
exit /b 0

:rig_native_sharpen_test
echo [edvr] === native_sharpen_test.exe ===
if not exist "%OBJ%\native_sharpen_test" mkdir "%OBJ%\native_sharpen_test"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /Fo"%OBJ%\native_sharpen_test\\" /Fe"%BUILD%\native_sharpen_test.exe" ^
    "tools\native_sharpen_test\native_sharpen_test.cpp" "src\d3d11\native_sharpen.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: native sharpen contract test build failed & exit /b 1 )
"%BUILD%\native_sharpen_test.exe" --dry-run || exit /b 1
"%BUILD%\native_sharpen_test.exe" --self-test || exit /b 1
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /Fo"%OBJ%\native_sharpen_test\\" /Fe"%BUILD%\native_sharpen_gpu_test.exe" ^
    "tools\native_sharpen_test\native_sharpen_gpu_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: native sharpen GPU test build failed & exit /b 1 )
"%BUILD%\native_sharpen_gpu_test.exe" --dry-run || exit /b 1
exit /b 0

:rig_flat_sharpen_test
echo [edvr] === flat_sharpen_test.exe, flat_sharpen_pass_test.exe ===
REM The flat profile's sharpening (docs\anti-aliasing.md, "Flat sharpening"), in two
REM rigs. flat_sharpen_test: the wrapper against a stubbed pass, and the flat panel's
REM row table against the flat profile's key gate -- a row whose key the gate refuses
REM reads 0 and does nothing, silently, so it fails the build here. flat_sharpen_pass_test:
REM the shipped RCAS pass on WARP against a CPU implementation of AMD's RCAS, with five
REM deliberately broken twins that must each fail. Neither links an import library for
REM the D3D11 runtime: both take Windows' own d3d11 (common\system_d3d11.h).
if not exist "%OBJ%\flat_sharpen_test" mkdir "%OBJ%\flat_sharpen_test"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\flat_sharpen_test\\" /Fe"%BUILD%\flat_sharpen_test.exe" ^
    "tools\flat_sharpen_test\flat_sharpen_test.cpp" "src\d3d11\flat_sharpen.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: flat sharpen contract test build failed & exit /b 1 )
"%BUILD%\flat_sharpen_test.exe" --dry-run || exit /b 1
"%BUILD%\flat_sharpen_test.exe" --self-test "%ROOT%" "%GEN%" || (
    echo [edvr] ERROR: the flat profile's sharpening or its panel row is wrong
    exit /b 1
)
if not exist "%OBJ%\flat_sharpen_pass_test" mkdir "%OBJ%\flat_sharpen_pass_test"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"%GEN%" ^
    /Fo"%OBJ%\flat_sharpen_pass_test\\" /Fe"%BUILD%\flat_sharpen_pass_test.exe" ^
    "tools\flat_sharpen_test\flat_sharpen_pass_test.cpp" "src\d3d11\flat_sharpen.cpp" ^
    "src\d3d11\sharpen_pass.cpp" "src\d3d11\shader_swap.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: flat sharpen pass test build failed & exit /b 1 )
"%BUILD%\flat_sharpen_pass_test.exe" --dry-run || exit /b 1
"%BUILD%\flat_sharpen_pass_test.exe" --self-test || (
    echo [edvr] ERROR: the sharpening pass disagrees with AMD's RCAS, or a broken twin passed
    exit /b 1
)
"%BUILD%\flat_sharpen_pass_test.exe" --self-test-working || (
    echo [edvr] ERROR: a flat session that sharpens its frames said the never-ran note
    exit /b 1
)
exit /b 0

:rig_openxr_trace_test
echo [edvr] === openxr_trace_test.exe ===
if not exist "%OBJ%\openxr_trace" mkdir "%OBJ%\openxr_trace"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /Fo"%OBJ%\native_timing\\" /Fe"%BUILD%\native_timing_test.exe" ^
    "tools\native_timing_test\native_timing_test.cpp" "src\d3d11\native_timing.cpp" "src\d3d11\map_wait.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: native timing contract test build failed & exit /b 1 )

%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Fo"%OBJ%\native_timing\\" /Fe"%BUILD%\native_perf_history_test.exe" ^
    "tools\native_perf_history_test\native_perf_history_test.cpp" "src\d3d11\native_perf_history.cpp" "src\d3d11\native_benchmark_collector.cpp"
if errorlevel 1 ( echo [edvr] ERROR: native perf history test build failed & exit /b 1 )

%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /I"third_party\openxr\include" ^
    /Fo"%OBJ%\native_timing\\" /Fe"%BUILD%\native_timing_gpu_test.exe" ^
    "tools\native_timing_test\native_timing_gpu_test.cpp" "src\d3d11\native_timing.cpp" "src\d3d11\map_wait.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_frame_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\common\log.cpp" "src\common\config.cpp" "src\common\proxy.cpp" "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: native timing GPU test build failed & exit /b 1 )

REM Separate XR-device queries; real WARP work and injected query failures.
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%" /Fo"%OBJ%\crashcontext\crash_context_test.obj" ^
    /Fe"%BUILD%\crash_context_test.exe" "tools\crash_context_test\crash_context_test.cpp" ^
    /link /OUT:"%BUILD%\crash_context_test.exe" /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: crash_context_test build failed & exit /b 1 )
"%BUILD%\crash_context_test.exe" --self-test || exit /b 1
exit /b 0

:rig_freeze_log_test
echo [edvr] === freeze_log_test.exe ===
REM Build gate for the freeze logging (src\common\freeze_book.h, src\openxr\long_cycle_line.h,
REM docs\freeze-diagnostics-2026-10-01.md, issue 63): the size buckets, the judge that tells a one-frame
REM Present-gap blip from a stall by the runtime's cycle, the write rule (a frame of 250 ms or more always
REM has its line), the book's counts and worst list, the runtime's rate limit and its three new lines, and by
REM source text that the glue in perf_monitor.cpp, native_runtime_host.h and native_timing.cpp still calls them.
REM It links nothing of the DLLs. tools\freeze_log_test\mutants.py --self-test holds the mutation list to the
REM sources as they are; --run builds the rig against each edit and needs the MSVC toolchain.
if not exist "%OBJ%\freezelog" mkdir "%OBJ%\freezelog"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\common" /I"src\openxr" ^
    /Fo"%OBJ%\freezelog"\ /Fe"%OBJ%\freezelog\freeze_log_test.exe" ^
    "tools\freeze_log_test\freeze_log_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: freeze_log_test build failed & exit /b 1 )
"%OBJ%\freezelog\freeze_log_test.exe" --dry-run || exit /b 1
"%OBJ%\freezelog\freeze_log_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\freeze_log_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_heartbeat_writer_test
echo [edvr] === heartbeat_writer_test.exe ===
REM Build gate for the breadcrumb heartbeat's writer thread (src\common\heartbeat_writer.h, src\common\proxy.cpp,
REM docs\freeze-diagnostics-2026-10-01.md, issue 63): the heartbeat's file write is off the render thread, and the
REM file still MEANS what it meant. The sink never runs on the posting thread and a sink stuck for 600 ms does not
REM slow post(); a writer held up writes the newest post and never an older one after a newer; it writes nothing of
REM its own (a hung render thread stops the heartbeat); a record is whole; close and closeAndDrain drop what is
REM pending and wait, for a bounded time, for what has begun; through the real breadcrumbHeartbeat and the real
REM breadcrumb file; a child that crashes (the file's last line is the crash filter's, never a heartbeat) and one
REM that is killed (the last line is a whole heartbeat); and by source text that the render thread's function
REM writes no file and DllMain and the crash filter close the heartbeat before their closing lines. The rig's
REM exe lives in its own obj directory so edvr_breadcrumbs.txt, which is written beside the exe, is its own.
REM tools\heartbeat_writer_test\mutants.py --self-test holds the mutation list to the sources as they are.
if not exist "%OBJ%\heartbeat" mkdir "%OBJ%\heartbeat"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\common" ^
    /Fo"%OBJ%\heartbeat"\ /Fe"%OBJ%\heartbeat\heartbeat_writer_test.exe" ^
    "tools\heartbeat_writer_test\heartbeat_writer_test.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: heartbeat_writer_test build failed & exit /b 1 )
"%OBJ%\heartbeat\heartbeat_writer_test.exe" --dry-run || exit /b 1
"%OBJ%\heartbeat\heartbeat_writer_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\heartbeat_writer_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_stall_sampler_test
if "%EDVR_RIG_STEP%"=="run" goto stall_sampler_test_run
echo [edvr] === stall_sampler_test.exe ===
REM Build gate for the stall sampler (src\common\stall_sampler.h, src\d3d11\stall_watch.cpp,
REM docs\freeze-diagnostics-2026-10-01.md, issue 63): the REAL capture, walk and naming code against threads the rig
REM blocks in places it knows -- a Sleep, a wait, a busy loop in this exe, a busy loop in stall_target.dll (so a
REM module that is not the exe is named), a stack 21 KB deep that uses RBP as its frame register -- and that the
REM thread it stopped is ALWAYS running again (a failed context read, a stack pointer outside the stack, an exited
REM thread, three hundred stops in a row); that the walk reads the COPY (the thread has run on and overwritten the
REM stack, and the sample still names its frames); the watchdog end to end on a real clock (1.3 s of stall, three
REM samples at 150/500/1000 ms); the policy on a fake one (thresholds, rate limit, session cap); and by source text
REM that no injection-shaped API appears and the window between the stop and the resume is straight-line. It holds
REM wall-clock intervals against a real thread, so it is a --quiet rig: its compiles run in the pool and its run
REM alone, after the others. tools\stall_sampler_test\mutants.py --self-test holds the mutation list to the sources
REM as they are; --run builds the rig against each edit (about four minutes, serial: it is judged by the clock).
if not exist "%OBJ%\stallsampler" mkdir "%OBJ%\stallsampler"
%CC% /nologo /O2 /MT /LD /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\stallsampler"\ /Fe"%OBJ%\stallsampler\stall_target.dll" ^
    "tools\stall_sampler_test\stall_target_dll.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: stall_target.dll build failed & exit /b 1 )
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\common" ^
    /Fo"%OBJ%\stallsampler"\ /Fe"%OBJ%\stallsampler\stall_sampler_test.exe" ^
    "tools\stall_sampler_test\stall_sampler_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: stall_sampler_test build failed & exit /b 1 )
if "%EDVR_RIG_STEP%"=="build" exit /b 0
:stall_sampler_test_run
"%OBJ%\stallsampler\stall_sampler_test.exe" --dry-run || exit /b 1
"%OBJ%\stallsampler\stall_sampler_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\stall_sampler_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_vram_watch_test
echo [edvr] === vram_watch_test.exe ===
REM Build gate for the graphics memory watch (src\common\vram_watch.h, src\common\vram_query.h, src\d3d11\vram_tick.cpp,
REM docs\headset-lock-vdxr-2026-10-02.md, instrument 1): the policy (a line every 30 s, every 5 s from 90% of the OS's
REM budget for the process, one at each crossing of the budget, a cap on the two cadences that a crossing is exempt from),
REM the lines' text, the machine the Present hook drives on a fake clock and a fake adapter (the OS is asked once a second,
REM the armed line, the unavailable line once for a stack without IDXGIAdapter3, a failed read said once), the DXGI read
REM against fake adapters and one real WARP adapter from System32's d3d11, and by source text that hookedPresent ticks the
REM watch once, unconditionally (so the flat profile has it too) after the stall sampler's beat. It links nothing of the DLLs.
REM tools\vram_watch_test\mutants.py --self-test holds the mutation list to the sources as they are; --run builds the rig
REM against each edit and needs the MSVC toolchain.
if not exist "%OBJ%\vramwatch" mkdir "%OBJ%\vramwatch"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\common" ^
    /Fo"%OBJ%\vramwatch"\ /Fe"%OBJ%\vramwatch\vram_watch_test.exe" ^
    "tools\vram_watch_test\vram_watch_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: vram_watch_test build failed & exit /b 1 )
"%OBJ%\vramwatch\vram_watch_test.exe" --dry-run || exit /b 1
"%OBJ%\vramwatch\vram_watch_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\vram_watch_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_skin_ledger_test
echo [edvr] === skin_ledger_test.exe ===
REM Build gate for the skin ledger (src\d3d11\skin_ledger.h, the F2 settling instrument that rides an armed eye run;
REM docs\kinematic-motion-injection-2026-09-19.md, "F2 study"): the rules (unarmed is silent and stateless, the 20-frame window and its
REM edges, the copy plan, the numbers, a whole run, a run of fewer frames than the window, never-ran told from broken, a run the process
REM ended, finished only after the report), the file layout written by the production serialiser, and by source text the glue that
REM reads the context (the dispatch hook asks before it acts, one arm, the report before the pool copies are let go, whole palette
REM copies). The rig writes a fixture run; tools\skin_palette_check.py (the offline checker) reads it with its own reader and
REM requires its verdicts, and --self-test builds synthetic runs with injected faults. It links nothing of the DLLs and needs no device.
REM tools\skin_ledger_test\mutants.py --self-test holds the mutation list to the sources as they are; --run builds the rig
REM against each edit and needs the MSVC toolchain.
if not exist "%OBJ%\skinledger" mkdir "%OBJ%\skinledger"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\d3d11" ^
    /Fo"%OBJ%\skinledger"\ /Fe"%OBJ%\skinledger\skin_ledger_test.exe" ^
    "tools\skin_ledger_test\skin_ledger_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: skin_ledger_test build failed & exit /b 1 )
"%OBJ%\skinledger\skin_ledger_test.exe" --dry-run || exit /b 1
"%OBJ%\skinledger\skin_ledger_test.exe" --self-test "%ROOT%" || exit /b 1
if not exist "%OBJ%\skinledger\fixture" mkdir "%OBJ%\skinledger\fixture"
"%OBJ%\skinledger\skin_ledger_test.exe" --fixture "%OBJ%\skinledger\fixture" || exit /b 1
python "tools\skin_palette_check.py" --self-test || exit /b 1
python "tools\skin_palette_check.py" --verify-fixture "%OBJ%\skinledger\fixture" || exit /b 1
python "tools\skin_ledger_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_skin_join_test
echo [edvr] === skin_join_test.exe ===
REM Build gate for the second skin's join (src\d3d11\skin_join.h, skin_entity_walk.h; docs\kinematic-motion-injection-2026-09-19.md, "F2 built"): the
REM CPU half of the identity of a skinned character from one frame to the next. The hook's reading of the game's entry list over a fake heap with
REM faults in it (J11), the job table's prefix join and the hook's entity join against a scripted world (spawn, despawn, reorder, a character that
REM changes its bone count, a swap, the hook disagreeing with the table, a gap in the frames, no history), the palette history's certificates, the
REM plan the GPU is handed, and the periodic line. A failed join is no history, never a guess: the cases pin it. No device needed.
REM tools\skin_join_test\mutants.py --self-test holds the mutation list to the sources as they are; --run builds the rig against each edit.
if not exist "%OBJ%\skinjoin" mkdir "%OBJ%\skinjoin"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\d3d11" ^
    /Fo"%OBJ%\skinjoin"\ /Fe"%OBJ%\skinjoin\skin_join_test.exe" ^
    "tools\skin_join_test\skin_join_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: skin_join_test build failed & exit /b 1 )
"%OBJ%\skinjoin\skin_join_test.exe" --dry-run || exit /b 1
"%OBJ%\skinjoin\skin_join_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\skin_join_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_skin_join_gpu_test
echo [edvr] === skin_join_gpu_test.exe ===
REM Build gate for the second skin's GPU join (src\d3d11\skin_join_shader.h: JoinCS and the pose table's clear, scatter and verify passes) on WARP.
REM The shipped HLSL is compiled and run beside the CPU model (skin_join.h) on scripted and random worlds and must agree with it word for word:
REM the join and info tables, the pose table, the hook cross-check bits, the statistics. The fxc trap that cost a build once is pinned (atomics
REM counted after the loops, not inside branches that `continue`). Needs no real GPU.
if not exist "%OBJ%\skinjoingpu" mkdir "%OBJ%\skinjoingpu"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\d3d11" ^
    /Fo"%OBJ%\skinjoingpu"\ /Fe"%OBJ%\skinjoingpu\skin_join_gpu_test.exe" ^
    "tools\skin_join_gpu_test\skin_join_gpu_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 ( echo [edvr] ERROR: skin_join_gpu_test build failed & exit /b 1 )
"%OBJ%\skinjoingpu\skin_join_gpu_test.exe" --dry-run || exit /b 1
"%OBJ%\skinjoingpu\skin_join_gpu_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\skin_join_gpu_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_skin_clone_test
echo [edvr] === skin_clone_test.exe ===
REM Build gate for the second skin's vertex shader patch (src\d3d11\dxbc_skin_clone.h, through dxbc_engine_velocity.h; docs\kinematic-motion-injection-
REM 2026-09-19.md, "F2 built"): the token-level clone that computes last frame's position from last frame's palette and pose. Tokens and structure
REM (the new resources, the temp renaming, the single export), every refusal (displacement before the anchor, no pose, a square root in the chain,
REM relative cb addressing, occupied slots), and on WARP the properties that make E exact: E is exactly zero when the previous state equals the
REM current, equals the CPU reference within 1e-6 when it does not, is zero and invalid without a join, and is the same for a swapped palette
REM buffer pair. --corpus DIR adds the game's own five skinned shaders from a log dump (local only, not run here).
if not exist "%OBJ%\skinclone" mkdir "%OBJ%\skinclone"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\skinclone"\ /Fe"%OBJ%\skinclone\skin_clone_test.exe" ^
    "tools\skin_clone_test\skin_clone_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 ( echo [edvr] ERROR: skin_clone_test build failed & exit /b 1 )
"%OBJ%\skinclone\skin_clone_test.exe" --dry-run || exit /b 1
"%OBJ%\skinclone\skin_clone_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\skin_clone_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_skin_engine_test
echo [edvr] === skin_engine_test.exe ===
REM Build gate for the second skin's engine side (docs\kinematic-motion-injection-2026-09-19.md, "F2 built"), on WARP. The compose: the shipped HLSL's
REM skinned reprojection against a double reference (previous position = world + camera term + E), the production mv pass on real ES/EP/scene-depth/target-7
REM resources (a valid E takes its exact motion, E = 0 is the camera term, no valid E keeps no history, rigid records never read target 7, the counters and
REM the |E| bins), and the derived blend state's target-7 modes. The engine: the linked engine_velocity.cpp and skin_join_gpu.cpp draw a skinned character
REM in both eyes through the real path (the palette chain's dispatch told to the join, the pool torn the way the game's tees report it, the patched pair
REM made and bound, target 7 created, cleared and read back through the view the compose gets): the first frame has honest no-history, a steady character
REM has E exactly zero in both eyes, a moving one has 100 x (previous - current), a changed job table has none for a frame, the three views and target 7
REM are bound for the draw and let go at the boundary, the hook's list and the prefix both identify the character, the periodic lines carry the GPU's
REM counters, a previous palette buffer too small for a job's previous rows gives no history. tools\skin_engine_test\mutants.py --self-test holds the
REM mutation list to the sources as they are; --run (needs this build's generated shader header) rebuilds the rig against each edit.
if not exist "%OBJ%\skinengine" mkdir "%OBJ%\skinengine"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /DEDVR_ENGINE_VELOCITY_RIG /DEDVR_BINDING_SHADOW_EXTERNAL /I"%GEN%" /I"src\d3d11" ^
    /Fo"%OBJ%\skinengine"\ /Fe"%OBJ%\skinengine\skin_engine_test.exe" ^
    "tools\skin_engine_test\skin_engine_test.cpp" "src\d3d11\engine_velocity.cpp" "src\d3d11\skin_join_gpu.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 ( echo [edvr] ERROR: skin_engine_test build failed & exit /b 1 )
"%OBJ%\skinengine\skin_engine_test.exe" --dry-run || exit /b 1
"%OBJ%\skinengine\skin_engine_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\skin_engine_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_skin_entity_hook_test
echo [edvr] === skin_entity_hook_test.exe ===
REM Build gate for the read-only hook on the game's skinning-job assembly (src\d3d11\skin_entity_hook.cpp, EDVR_SKIN_HOOK_TEST; docs\kinematic-motion-
REM injection-2026-09-19.md, "F2 built"). The production source carries its own test: the real CodeHook patches a synthetic function that begins with the
REM real 28-byte prologue, the real relay runs, the real guarded reads walk a heap laid out as the decompile says the list is. The original runs first,
REM the list is read whole, a wrong prologue stands the hook down with no patch, a fault or a wild pointer is counted and the game is unharmed, a second
REM node stands it down, a gate shut observes nothing, and a reader against the game's thread never gets a torn copy.
if not exist "%OBJ%\skinentityhook" mkdir "%OBJ%\skinentityhook"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DEDVR_SKIN_HOOK_TEST /I"src\d3d11" ^
    /Fo"%OBJ%\skinentityhook"\ /Fe"%OBJ%\skinentityhook\skin_entity_hook_test.exe" ^
    "tools\skin_entity_hook_test\skin_entity_hook_test.cpp" "src\d3d11\skin_entity_hook.cpp" ^
    "src\common\code_hook.cpp" "src\common\guard.cpp" "src\common\log.cpp" "src\common\config.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: skin_entity_hook_test build failed & exit /b 1 )
"%OBJ%\skinentityhook\skin_entity_hook_test.exe" --dry-run || exit /b 1
"%OBJ%\skinentityhook\skin_entity_hook_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\skin_entity_hook_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_slow_regime_test
echo [edvr] === slow_regime_test.exe ===
REM Build gate for the runtime's vendor instruments (src\openxr\vendor_events.h, end_frame_episodes.h, slow_regime.h,
REM src\common\slow_test.h, docs\headset-lock-vdxr-2026-10-02.md, instruments 2 to 4 and the test trigger): the vendor's events
REM decoded, one line each, bounded (200 lines, undecoded types named once, the last session state kept past the cap), and
REM SessionState's observer seeing every event without changing how one is handled; every xrEndFrame of 3 display periods or
REM more as an episode (the first call on its own line, ended by 8 normal calls, p50 and max from a histogram, the rate limit
REM and the end line a long episode always gets); a frame rate under 40% of the display's held for 5 s as a regime with the
REM owner that holds it (the vendor's xrEndFrame, the pose wait, the swapchain calls, EDVR's copy, EDVR's work, the game),
REM its still_slow line every 30 s and its end line; FrameBoundary's hook, on a real boundary with a fake runtime, for every
REM xrEndFrame on every path (synchronous, no-render, cleared, loading, deferred pacing, drain) and the test hold inside the
REM timed region and nowhere else; the test trigger's schedule; and by source text the host's glue. It links nothing of the
REM DLLs. tools\slow_regime_test\mutants.py --self-test holds the mutation list to the sources as they are; --run builds the
REM rig against each edit and needs the MSVC toolchain.
if not exist "%OBJ%\slowregime" mkdir "%OBJ%\slowregime"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\common" /I"src\openxr" /I"third_party\openxr\include" ^
    /Fo"%OBJ%\slowregime"\ /Fe"%OBJ%\slowregime\slow_regime_test.exe" ^
    "tools\slow_regime_test\slow_regime_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: slow_regime_test build failed & exit /b 1 )
"%OBJ%\slowregime\slow_regime_test.exe" --dry-run || exit /b 1
"%OBJ%\slowregime\slow_regime_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\slow_regime_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_flat_temporal_test
echo [edvr] === flat_temporal_test.exe ===
if not exist "%OBJ%\flattemporaltest" mkdir "%OBJ%\flattemporaltest"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\flattemporaltest"\ ^
    /Fe"%BUILD%\flat_temporal_test.exe" "tools\flat_temporal_test\flat_temporal_test.cpp" ^
    "third_party\dxbc_hash\DxilHash.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: flat temporal test build failed & exit /b 1 )
REM --write-fixture regenerates tools\flat_upscale_fixture.log (design doc section 83); anything that writes a file takes --dry-run,
REM and --dry-run writes nothing at all.
"%BUILD%\flat_temporal_test.exe" --write-fixture "%OBJ%\flattemporaltest\never_written.log" --dry-run || exit /b 1
if exist "%OBJ%\flattemporaltest\never_written.log" ( echo [edvr] ERROR: flat_temporal_test --write-fixture --dry-run wrote a file & exit /b 1 )
"%BUILD%\flat_temporal_test.exe" --self-test || exit /b 1
REM The offscreen fast path's mutation list (src\d3d11\flat_offscreen_fast.h, flat_offscreen_counts.h) is held to the headers as they are; --run builds the standalone
REM driver (tools\flat_temporal_test\flat_offscreen_test.cpp) against each edit and requires the case that owns the rule to fail.
python "tools\flat_temporal_test\offscreen_mutants.py" --self-test || exit /b 1
exit /b 0

:rig_vr_world_route_test
echo [edvr] === vr_world_route_test.exe ===
REM The VR on-foot world route (design doc section 82): the pure half, the detector's glue on the census retake's chain, the
REM key-off contract and the hook pins. Pure C++ and source scans: no D3D.
REM The curved route's pins (a curved screen does not hold the route off; the curve is named in the 5 s line and the OWNS line) hold
REM their own controls: copies of the source with one edit that must trip the pin. tools\vr_world_route_test\mutants.py --self-test
REM holds the mutation list of this rig, of vr_world_route_gpu_test and of ui_layer_world_test below to the sources as they are; its
REM --run (on demand) builds each rig against one edited production file and requires a check that names the edit to fail. Its wiring rig
REM (--run --rig wiring) proves the surface strip's wiring pins (:rig_ui_quality_test, --wiring) on edited copies of vscreen.cpp, in seconds.
if not exist "%OBJ%\vrworldroutetest" mkdir "%OBJ%\vrworldroutetest"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\vrworldroutetest"\ ^
    /Fe"%BUILD%\vr_world_route_test.exe" "tools\vr_world_route_test\vr_world_route_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: vr world route test build failed & exit /b 1 )
"%BUILD%\vr_world_route_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\vr_world_route_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_vr_world_route_gpu_test
echo [edvr] === vr_world_route_gpu_test.exe ===
REM The VR world route's runtime (vr_world_route.cpp, linked as shipped) on WARP: the real binding shadow, the real Config and
REM the real flat resolver with its backends stubbed, a synthetic copy of the census retake's chain drawn through the same
REM per-draw entry the hooks call. Key off, the happy path, every refusal, the internal flags, the latch, scene resets,
REM frame gaps and the key going off while owned. The curved screen's scenarios (the route runs with fix.panel_curvature above 0, and
REM its lines name the curve) are held by tools\vr_world_route_test\mutants.py (its --run builds this rig against an edited route).
if not exist "%OBJ%\vrworldroutegpu" mkdir "%OBJ%\vrworldroutegpu"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"%GEN%" /Fo"%OBJ%\vrworldroutegpu\\" ^
    /Fe"%BUILD%\vr_world_route_gpu_test.exe" "tools\vr_world_route_gpu_test\vr_world_route_gpu_test.cpp" ^
    "src\d3d11\vr_world_route.cpp" "src\d3d11\binding_shadow.cpp" "src\d3d11\flat_mono_resolve.cpp" "src\d3d11\map_easu.cpp" "src\d3d11\map_smaa.cpp" ^
    "src\d3d11\flat_projection_scope.cpp" "src\d3d11\flat_projection_runtime.cpp" ^
    "src\common\config.cpp" "src\common\proxy.cpp" "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO dxgi.lib d3dcompiler.lib user32.lib gdi32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: vr world route GPU test build failed & exit /b 1 )
"%BUILD%\vr_world_route_gpu_test.exe" --dry-run || exit /b 1
"%BUILD%\vr_world_route_gpu_test.exe" --self-test || exit /b 1
exit /b 0

REM Offline-only test-link proxy (build\flat_sdk_bench_proxy.dll): all normal production objects except one engine_velocity object built with its
REM existing rig guard and an inert emit fixture. Never copied into an installer or game directory. It reads the production objects in
REM %OBJ%\d3d11 (finished before any rig starts) and writes build\flat_sdk_bench_proxy.* and %OBJ%\flat_sdk_bench_proxy. Nothing in this build
REM loads it (flat_sdk_bench.py's self-test and the integration exe's use a synthetic path), so no rig is --after it.
:rig_flat_sdk_bench_proxy
echo [edvr] === flat_sdk_bench_proxy.dll ===
python "tools\build_flat_sdk_bench_proxy.py" --dry-run || exit /b 1
python "tools\build_flat_sdk_bench_proxy.py" || exit /b 1
exit /b 0

:rig_flat_sdk_integration_test
echo [edvr] === flat_sdk_integration_test.exe ===
if not exist "%OBJ%\flat_sdk_integration_test" mkdir "%OBJ%\flat_sdk_integration_test"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Fo"%OBJ%\flat_sdk_integration_test\\" /Fe"%BUILD%\flat_sdk_integration_test.exe" ^
    "tools\flat_sdk_integration_test\flat_sdk_integration_test.cpp" ^
    /link /INCREMENTAL:NO dxgi.lib d3dcompiler.lib user32.lib gdi32.lib
if errorlevel 1 ( echo [edvr] ERROR: flat SDK integration bench compile failed & exit /b 1 )
"%BUILD%\flat_sdk_integration_test.exe" --dry-run || exit /b 1
"%BUILD%\flat_sdk_integration_test.exe" --self-test || exit /b 1
python "tools\flat_sdk_bench.py" --self-test || exit /b 1
exit /b 0

:rig_flat_mono_resolve_test
echo [edvr] === flat_mono_resolve_test.exe ===
REM The compile and the cheap checks only. The suites run in :rig_flat_mono_resolve_shard1..5 (each --after this rig), because one process took
REM 276 s: `flat_mono_resolve_test.exe --self-test --shard I/5` runs a fifth of them (the unit table is kUnits in the test's .cpp; plain
REM --self-test still runs all of them, for hand use). --list-shards 5 fails the build unless the five shards between them run every unit
REM exactly once; each shard also fails unless it ran every unit the table gives it.
if not exist "%OBJ%\flat_mono_resolve_test" mkdir "%OBJ%\flat_mono_resolve_test"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"%GEN%" /Fo"%OBJ%\flat_mono_resolve_test\\" ^
    /Fe"%BUILD%\flat_mono_resolve_test.exe" "tools\flat_mono_resolve_test\flat_mono_resolve_test.cpp" ^
    "src\d3d11\flat_mono_resolve.cpp" "src\d3d11\map_easu.cpp" "src\d3d11\map_smaa.cpp" "src\d3d11\flat_projection_scope.cpp" "src\d3d11\flat_projection_runtime.cpp" ^
    "third_party\dxbc_hash\DxilHash.cpp" /link /INCREMENTAL:NO dxgi.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: flat mono resolve test build failed & exit /b 1 )
"%BUILD%\flat_mono_resolve_test.exe" --dry-run || exit /b 1
"%BUILD%\flat_mono_resolve_test.exe" --list-shards 5 || exit /b 1
exit /b 0

REM The flat_mono_resolve_test shards. Each runs a copy of the exe from a directory of its own, because the suites write fixtures next to the
REM exe (flat-pixel-fixture, flat-weapon-*-fixture) and read them back; "<label>_run" is the private-directory shape run_jobs.py deletes once
REM the rig passes and keeps, naming it, when the rig fails. The rig's cwd stays the repo root (the suites read src\ and tools\ by relative
REM path; the draw-packet suite writes build\supporter-0182, in shard 2 only). The Python fixture checks run in the shard that wrote the fixture:
REM shard 2 (pixel capture, draw capture, weapon footprint) and shard 3 (resolve inputs).
:rig_flat_mono_resolve_shard1
echo [edvr] === flat_mono_resolve_test.exe --shard 1/5 ===
call :flat_mono_resolve_shard 1 5 || exit /b 1
exit /b 0

:rig_flat_mono_resolve_shard2
echo [edvr] === flat_mono_resolve_test.exe --shard 2/5 ===
call :flat_mono_resolve_shard 2 5 || exit /b 1
python "tools\flat_pixels.py" "%SHARD_DIR%\flat-pixel-fixture" --verify-fixture || exit /b 1
python "tools\flat_draw_pixels.py" "%SHARD_DIR%\flat-pixel-fixture" --verify-fixture || exit /b 1
python "tools\flat_weapon_pixels.py" "%SHARD_DIR%\flat-weapon-fixture" --verify-fixture || exit /b 1
python "tools\flat_weapon_pixels.py" "%SHARD_DIR%\flat-weapon-partial-fixture" --verify-partial-fixture || exit /b 1
python "tools\flat_weapon_pixels.py" "%SHARD_DIR%\flat-weapon-wide-visible-fixture" --verify-fixture || exit /b 1
python "tools\flat_weapon_pixels.py" "%SHARD_DIR%\flat-weapon-wide-depth_rejected-fixture" --verify-fixture || exit /b 1
python "tools\flat_weapon_pixels.py" "%SHARD_DIR%\flat-weapon-wide-color_disabled-fixture" --verify-fixture || exit /b 1
python "tools\flat_weapon_pixels.py" "%SHARD_DIR%\flat-weapon-wide-zero_count-fixture" --verify-fixture || exit /b 1
python "tools\flat_weapon_pixels.py" "%SHARD_DIR%\flat-weapon-wide-query_unavailable-fixture" --verify-fixture || exit /b 1
python "tools\flat_weapon_pixels.py" "%SHARD_DIR%\flat-weapon-wide-query_timeout-fixture" --verify-fixture || exit /b 1
exit /b 0

:rig_flat_mono_resolve_shard3
echo [edvr] === flat_mono_resolve_test.exe --shard 3/5 ===
call :flat_mono_resolve_shard 3 5 || exit /b 1
python "tools\flat_resolve_inputs.py" --verify-fixture "%SHARD_DIR%\flat-pixel-fixture" || exit /b 1
exit /b 0

:rig_flat_mono_resolve_shard4
echo [edvr] === flat_mono_resolve_test.exe --shard 4/5 ===
call :flat_mono_resolve_shard 4 5 || exit /b 1
exit /b 0

:rig_flat_mono_resolve_shard5
echo [edvr] === flat_mono_resolve_test.exe --shard 5/5 ===
call :flat_mono_resolve_shard 5 5 || exit /b 1
exit /b 0

REM %1 = this shard, %2 = the shard count. Leaves SHARD_DIR set for the caller's fixture checks.
:flat_mono_resolve_shard
set "SHARD_DIR=%BUILD%\flat_mono_resolve_shard%1_run"
if exist "%SHARD_DIR%" rmdir /s /q "%SHARD_DIR%"
mkdir "%SHARD_DIR%"
if errorlevel 1 ( echo [edvr] ERROR: cannot make %SHARD_DIR% & exit /b 1 )
copy /y "%BUILD%\flat_mono_resolve_test.exe" "%SHARD_DIR%\flat_mono_resolve_test.exe" >nul
if errorlevel 1 ( echo [edvr] ERROR: cannot copy flat_mono_resolve_test.exe into %SHARD_DIR% & exit /b 1 )
"%SHARD_DIR%\flat_mono_resolve_test.exe" --self-test --shard %1/%2 || exit /b 1
exit /b 0

:rig_c2_derive_test
echo [edvr] === c2_derive_test.exe ===
if not exist "%OBJ%\c2derive" mkdir "%OBJ%\c2derive"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\c2derive\\" ^
    /Fe"%BUILD%\c2_derive_test.exe" "tools\c2_derive_test\c2_derive_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: c2 derive test build failed & exit /b 1 )
"%BUILD%\c2_derive_test.exe" --self-test || exit /b 1
exit /b 0

:rig_c2_warp_test
echo [edvr] === c2_warp_test.exe ===
if not exist "%OBJ%\c2warp" mkdir "%OBJ%\c2warp"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\c2warp\\" ^
    /Fe"%BUILD%\c2_warp_test.exe" "tools\c2_warp_test\c2_warp_test.cpp" ^
    /link /INCREMENTAL:NO dxgi.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: c2 warp test build failed & exit /b 1 )
"%BUILD%\c2_warp_test.exe" --self-test || exit /b 1
exit /b 0

:rig_flat_camera_stub_test
echo [edvr] === flat_camera_stub_test.exe ===
if not exist "%OBJ%\flatcamerastub" mkdir "%OBJ%\flatcamerastub"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%" /Fo"%OBJ%\flatcamerastub\\" ^
    /Fe"%BUILD%\flat_camera_stub_test.exe" "tools\flat_camera_stub_test\flat_camera_stub_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: flat camera stub test build failed & exit /b 1 )
"%BUILD%\flat_camera_stub_test.exe" --self-test || exit /b 1
exit /b 0

:rig_c2_coexist_test
echo [edvr] === c2_coexist_test.exe ===
if not exist "%OBJ%\c2coexist" mkdir "%OBJ%\c2coexist"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\c2coexist\\" ^
    /Fe"%BUILD%\c2_coexist_test.exe" "tools\c2_coexist_test\c2_coexist_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: c2 coexist test build failed & exit /b 1 )
"%BUILD%\c2_coexist_test.exe" --self-test || exit /b 1
exit /b 0

:rig_vr_camera_census_test
echo [edvr] === vr_camera_census_test.exe ===
REM The VR camera census (design doc section 82): the pure half the DLL compiles, run against the derive model, plus the
REM source scans that hold "key off = nothing" and "the detour never writes a camera" (they read src\d3d11 from the repo
REM root), plus tools\camera_census_fixture.log held to exactly what the formatters write -- the file that
REM edvr_log.py --camera-census's own self-test reads.
if not exist "%OBJ%\vrcamcensus" mkdir "%OBJ%\vrcamcensus"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\vrcamcensus\\" ^
    /Fe"%BUILD%\vr_camera_census_test.exe" "tools\vr_camera_census_test\vr_camera_census_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: vr camera census test build failed & exit /b 1 )
"%BUILD%\vr_camera_census_test.exe" --self-test || exit /b 1
exit /b 0

:rig_vr_camera_census_glue_test
echo [edvr] === vr_camera_census_glue_test.exe ===
REM The census's glue (src\d3d11\vr_camera_census.cpp) compiled for real with the real Config and Log, and stubs for what it
REM calls (the injector, the world route's draw progress, the journal, EDVR's advertised eye geometry): the observer's two
REM halves against cameras built by the game-camera derive model (no allocation, not one byte written), the boundary's
REM order, the eye draw's staging readback on a WARP device, the key-off path, the bounded log -- and then
REM tools\edvr_log.py --camera-census over the log the glue wrote, which must find the two eye cameras by the content
REM join. Needs python on PATH; runs from the repo root; takes about six seconds (one 5 s window is waited out).
if not exist "%OBJ%\vrcamcensusglue" mkdir "%OBJ%\vrcamcensusglue"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"%GEN%" /Fo"%OBJ%\vrcamcensusglue\\" ^
    /Fe"%BUILD%\vr_camera_census_glue_test.exe" "tools\vr_camera_census_glue_test\vr_camera_census_glue_test.cpp" ^
    "src\d3d11\vr_camera_census.cpp" "src\common\config.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: vr camera census glue test build failed & exit /b 1 )
"%BUILD%\vr_camera_census_glue_test.exe" --self-test || exit /b 1
exit /b 0

:rig_flat_camera_vr_test
echo [edvr] === flat_camera_vr_test.exe ===
REM The VR world route's camera injection mode (design doc section 82, stage 2): the pure half the detour runs
REM (src\d3d11\flat_camera_vr.h: the role of a kind-3 camera, the admission, the flush, the counters, the mode word) run
REM exhaustively and through scripted call sequences -- the camera-object reuse scenario among them -- plus the source pins
REM that hold the detour to it (src\d3d11\flat_camera_inject.cpp: the flat profile's regions byte-for-byte, the order of a
REM call's steps, no write reachable from a pass-through or observe-only call). The pins read src\d3d11 from the repo root
REM (the rig takes the root as its argument). tools\flat_camera_vr_test\mutants.py --self-test holds the mutation list to
REM the sources as they are; the list itself (--run, on demand, about a minute) proves the rig fails when each rule is flipped.
if not exist "%OBJ%\flatcameravr" mkdir "%OBJ%\flatcameravr"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\flatcameravr\\" ^
    /Fe"%BUILD%\flat_camera_vr_test.exe" "tools\flat_camera_vr_test\flat_camera_vr_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: flat camera vr test build failed & exit /b 1 )
"%BUILD%\flat_camera_vr_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\flat_camera_vr_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_config_test
echo [edvr] === config_test.exe ===
REM The real parser over the real shipped edvr.ini. The file's own layout
REM depends on two parser properties -- repeated section headers, last value
REM wins -- that were originally read out of config.cpp rather than observed,
REM and every symptom of either being false shows up in the game rather than
REM in a build. It also registers the generated config_contract_gen.h tables
REM (the moved-from map parsed from the real edvr.ini), so a moved key's read-
REM through is proven against what actually shipped, not a fixture.
if not exist "%OBJ%\cfgtest" mkdir "%OBJ%\cfgtest"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"%GEN%" /Fo"%OBJ%\cfgtest"\ ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\inputgatetest"\ ^
    /Fe"%BUILD%\input_gate_test.exe" "tools\input_gate_test\input_gate_test.cpp" ^
    "src\d3d11\joy_watch.cpp" "src\common\iat_hook.cpp" "src\common\vtable_hook.cpp" ^
    "src\common\hotkey.cpp" "src\common\config.cpp" "src\common\log.cpp" ^
    "src\common\guard.cpp" "src\common\frame_flag.cpp" "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib dinput8.lib xinput9_1_0.lib
if errorlevel 1 ( echo [edvr] ERROR: input_gate_test build failed & exit /b 1 )
"%BUILD%\input_gate_test.exe" || (
    echo [edvr] ERROR: the menu keyboard gate failed its device or release checks
    exit /b 1
)
exit /b 0

:rig_hotkey_capture_test
echo [edvr] === hotkey_capture_test.exe ===
REM The F8 menu's Hotkeys page below the menu: the three hotkey value formats and their round trip
REM through edvr.ini, the pad and joystick edge, the joystick table against synthetic DIJOYSTATE2
REM buffers on a read-only page, the capture state machine, the checks on a captured binding
REM (duplicate, reserved, Elite clash, the Explorer Cam lock), Elite's bindings read for keyboard,
REM pad and joystick, and which generated rows are on the page in which tier. Nothing is opened:
REM no device, no window, no game.
if not exist "%OBJ%\hotkeycapturetest" mkdir "%OBJ%\hotkeycapturetest"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"%GEN%" /Fo"%OBJ%\hotkeycapturetest"\ ^
    /Fe"%BUILD%\hotkey_capture_test.exe" "tools\hotkey_capture_test\hotkey_capture_test.cpp" ^
    "src\common\hotkey.cpp" "src\d3d11\joy_watch.cpp" "src\d3d11\hotkey_capture.cpp" ^
    "src\d3d11\elite_binds.cpp" "src\common\iniedit.cpp" "src\common\config.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: hotkey_capture_test build failed & exit /b 1 )
"%BUILD%\hotkey_capture_test.exe" "%ROOT%" || (
    echo [edvr] ERROR: the Hotkeys page failed its format, capture, joystick or row checks
    exit /b 1
)
exit /b 0

:rig_gate_test
echo [edvr] === gate_test.exe ===
if not exist "%OBJ%\gatetest" mkdir "%OBJ%\gatetest"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\gatetest"\ /Fe"%BUILD%\gate_test.exe" ^
    "tools\gate_test\gate_test.cpp" ^
    "src\common\config.cpp" ^
    "src\common\log.cpp" "src\common\frame_flag.cpp" ^
    "src\common\guard.cpp" ^
    "src\common\proxy.cpp" "src\d3d11\journal_watch.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: gate_test build failed & exit /b 1 )
"%BUILD%\gate_test.exe" "%ROOT%" || (
    echo [edvr] ERROR: gate_test failed: the journal watcher, timing, periodic work or logged-note checks
    exit /b 1
)
exit /b 0

:rig_journal_unload_test
echo [edvr] === journal_unload_test.exe ===
REM The graphics DLL's pin against a real FreeLibrary (RC4 review 2026-09-29, F1).
REM d3d11.dll runs its code on threads nobody waits for -- the journal watcher's
REM worker, and with fix.ui_quality on a fire-and-forget thread-pool callback -- and
REM a FreeLibrary that unmaps the image under one returns it into unmapped code.
REM gate_test links journal_watch.cpp into an exe, which is never unloaded, so it
REM cannot ask. This builds a DLL from the REAL journal_watch.cpp and the REAL pin
REM (src\common\module_pin.h) and, for the real worker, a CreateThread thread and a
REM pool callback of ui_surfaces.cpp's shape, blocks each inside the module, calls
REM FreeLibrary, and requires the module to stay mapped. Without the pin the
REM CreateThread thread and the pool callback must lose the image, which is what
REM shows the rig can tell; the real worker does not, because the UCRT's thread
REM start holds its module, and the rig reports that. It then loads the real
REM build\d3d11.dll, gives it a device, and reads its own log for the pin line, the
REM call in initOnceCallback as shipped. Nothing here links d3d11.lib: the exe takes
REM the proxy by path.
if not exist "%OBJ%\junload" mkdir "%OBJ%\junload"
if not exist "%OBJ%\junload\dll" mkdir "%OBJ%\junload\dll"
if not exist "%OBJ%\junload\host" mkdir "%OBJ%\junload\host"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /LD /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\junload\dll"\ /Fe"%BUILD%\journal_unload_dll.dll" ^
    "tools\journal_unload_test\journal_unload_dll.cpp" "src\d3d11\journal_watch.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" ^
    "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: journal_unload_dll build failed & exit /b 1 )
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\junload\host"\ /Fe"%BUILD%\journal_unload_test.exe" ^
    "tools\journal_unload_test\journal_unload_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: journal_unload_test build failed & exit /b 1 )
"%BUILD%\journal_unload_test.exe" "%BUILD%\d3d11.dll" || (
    echo [edvr] ERROR: the graphics DLL's pin does not keep its code mapped under a running worker
    exit /b 1
)
exit /b 0

:rig_profile_switch_test
echo [edvr] === profile_switch_test.exe ===
REM The live profile switch's mechanics (src\d3d11\mode_watch.cpp commitPosted; docs\design-unified-install-2026-10-12.md section 6). The rig
REM loads a copy of the real build\d3d11.dll on WARP and presents synthetic frames through its real Present hook, so each flip is committed
REM at a real frame boundary. It pins the state each switch leaves, the log lines, the 50-cycle count and memory, the settings-file
REM fallback, and a control in a second process that skips the teardown and must be detected. It cannot reach the flat runtime or the VR
REM UI layers under real draws (they key on Elite's shaders); that is a flight's job. The exe sits in obj, not build: a build\ exe would
REM read build\edvr-flat.ini as the settings file's fallback.
if not exist "%OBJ%\profileswitch" mkdir "%OBJ%\profileswitch"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\profileswitch\\" /Fe"%OBJ%\profileswitch\profile_switch_test.exe" ^
    "tools\profile_switch_test\profile_switch_test.cpp" ^
    /link /INCREMENTAL:NO user32.lib psapi.lib
if errorlevel 1 ( echo [edvr] ERROR: profile_switch_test build failed & exit /b 1 )
"%OBJ%\profileswitch\profile_switch_test.exe" "%BUILD%\d3d11.dll" || (
    echo [edvr] ERROR: the live profile switch's mechanics failed their test
    exit /b 1
)
exit /b 0

:rig_glitch_test
echo [edvr] === glitch_test.exe ===
REM The transition-flash engine fix's camera-buffer tap (glitch_frame.cpp),
REM driven without the game: nomination until Armed, the event window gating the
REM hand-over, the frame counter, the geometry checks (and that a stale
REM fix.transition_flash line changes nothing), and a hold's mark and verdict.
if not exist "%OBJ%\glitchtest" mkdir "%OBJ%\glitchtest"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\glitchtest"\ ^
    /Fe"%BUILD%\glitch_test.exe" "tools\glitch_test\glitch_test.cpp" ^
    "src\d3d11\glitch_frame.cpp" ^
    "src\common\config.cpp" ^
    "src\common\frame_flag.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: glitch_test build failed & exit /b 1 )
"%BUILD%\glitch_test.exe" "%BUILD%\glitchscratch" || (
    echo [edvr] ERROR: the transition flash camera tap failed its own test
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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

:rig_sky_flicker_test
echo [edvr] === sky_flicker_test.exe ===
REM The sky-flicker diagnostic build (docs\sky-flicker-2026-10-13.md, TEMPORARY): the camera-row event detector on synthetic rows built from the
REM review's numbers, the pick rules and the eye-target filter on the probe-face sequence, the burst offset, the P3 mini-render record and its join,
REM the sky ring's entry layout and ring arithmetic, the flat scene picker's candidate log -- and the sky ring's shipped HLSL run on WARP against its
REM CPU reference, across the ring's wrap and an asynchronous readback. Needs no real GPU.
if not exist "%OBJ%\skyflickertest" mkdir "%OBJ%\skyflickertest"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\d3d11" ^
    /Fo"%OBJ%\skyflickertest"\ /Fe"%OBJ%\skyflickertest\sky_flicker_test.exe" ^
    "tools\sky_flicker_test\sky_flicker_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 ( echo [edvr] ERROR: sky_flicker_test build failed & exit /b 1 )
"%OBJ%\skyflickertest\sky_flicker_test.exe" --dry-run || exit /b 1
"%OBJ%\skyflickertest\sky_flicker_test.exe" --self-test || exit /b 1
exit /b 0

:rig_ui_depth
echo [edvr] === private UI depth regression ===
REM Keep the executable away from build\d3d11.dll: these tests use system
REM D3D11 WARP and include the production coverage pass directly.
if not exist "%OBJ%\uidepthtest" mkdir "%OBJ%\uidepthtest"
%CC% /I"%GEN%" /nologo /O2 /Gy /MT /std:c++17 /EHsc /W4 /wd4702 ^
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

:rig_ui_holo_pass_test
echo [edvr] === UI and hologram pass output-identity rig ===
REM WARP, the production UI resolve bytecode from %GEN%, away from build\d3d11.dll
REM (the rig links d3d11.lib). Goldens were recorded from the unmodified shaders.
if not exist "%OBJ%\uiholopass" mkdir "%OBJ%\uiholopass"
%CC% /I"%GEN%" /nologo /O2 /Gy /MT /std:c++17 /EHsc /W4 /wd4702 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\uiholopass\\" /Fe"%OBJ%\uiholopass\ui_holo_pass_test.exe" ^
    "tools\ui_holo_pass_test\ui_holo_pass_test.cpp" ^
    /link /INCREMENTAL:NO /OPT:REF d3d11.lib d3dcompiler.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: ui_holo_pass_test build failed & exit /b 1 )
"%OBJ%\uiholopass\ui_holo_pass_test.exe" --dry-run || exit /b 1
"%OBJ%\uiholopass\ui_holo_pass_test.exe" --self-test || (
    echo [edvr] ERROR: the UI and hologram passes no longer match their recorded output
    exit /b 1
)
exit /b 0

:rig_terrain_cover_test
echo [edvr] === terrain cull cover: the rule, the signature scan, the module gate, the hook on a synthetic builder ===
REM Build gate for the terrain cull cover (src\d3d11\terrain_cull_cover_core.h and terrain_cull_cover_hook.cpp, EDVR_TERRAIN_COVER_TEST; docs\terrain-culling.md): the game builds its
REM terrain cull planes from a CENTRED frustum (vFOV/2 vertically, atan(aspect * tan) horizontally), so an eye with an asymmetric frustum is under-covered on its wide sides. The rule
REM widens arguments 5 and 6 of that builder to the largest tangents handed to the game (the three rigs' predicted values, a symmetric headset, an input already wider, bad input).
REM The production hook is compiled in with the real CodeHook and patches a synthetic builder of the same six-argument shape inside synthetic modules (the known build's stamp and
REM layout, and another build's for the signature scan): only arguments 5 and 6 change; the first call and the ten-minute heartbeat are what the log says; tools\terrain_cover_fixture.log
REM is held byte for byte; device_hook.cpp's and native_temporal.cpp's wiring are read as text.
if not exist "%OBJ%\terraincover" mkdir "%OBJ%\terraincover"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DEDVR_TERRAIN_COVER_TEST /I"src\d3d11" /I"src\common" ^
    /Fo"%OBJ%\terraincover"\ /Fe"%OBJ%\terraincover\terrain_cover_test.exe" ^
    "tools\terrain_cover_test\terrain_cover_test.cpp" "src\d3d11\terrain_cull_cover_hook.cpp" ^
    "src\common\code_hook.cpp" "src\common\guard.cpp" "src\common\log.cpp" "src\common\config.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: terrain_cover_test build failed & exit /b 1 )
"%OBJ%\terraincover\terrain_cover_test.exe" --dry-run || exit /b 1
"%OBJ%\terraincover\terrain_cover_test.exe" --self-test "%ROOT%" || exit /b 1
exit /b 0

:rig_luma_probe_test
echo [edvr] === compact luminance diagnostic regression ===
if not exist "%OBJ%\lumaprobetest" mkdir "%OBJ%\lumaprobetest"
%CC% /nologo /O2 /Gy /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\lumaprobetest\\" /Fe"%OBJ%\lumaprobetest\luma_probe_test.exe" ^
    "tools\luma_probe_test\luma_probe_test.cpp" ^
    /link /INCREMENTAL:NO /OPT:REF d3d11.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: luma_probe_test build failed & exit /b 1 )
"%OBJ%\lumaprobetest\luma_probe_test.exe" --dry-run || exit /b 1
"%OBJ%\lumaprobetest\luma_probe_test.exe" --self-test || exit /b 1
exit /b 0

:rig_hologram_depth_test
echo [edvr] === generic hologram/icon depth regression ===
REM Same shape as :rig_ui_depth: WARP, the production coverage pass
REM included directly, away from build\d3d11.dll.
if not exist "%OBJ%\holodepthtest" mkdir "%OBJ%\holodepthtest"
%CC% /I"%GEN%" /nologo /O2 /Gy /MT /std:c++17 /EHsc /W4 /wd4702 ^
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

REM Five rigs (were one, :rig_native_motion_rigs, five independent exes run one after another); each keeps its own obj directory.
:rig_holo_motion
echo [edvr] === holo_motion_test ===
if not exist "%OBJ%\holomotion" mkdir "%OBJ%\holomotion"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\holomotion\\" /Fe"%OBJ%\holomotion\holo_motion_test.exe" ^
    "tools\holo_motion_test\holo_motion_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: hologram motion test build failed & exit /b 1 )
"%OBJ%\holomotion\holo_motion_test.exe" || exit /b 1
exit /b 0

:rig_screen_motion
echo [edvr] === screen_motion_test ===
if not exist "%OBJ%\screenmotion" mkdir "%OBJ%\screenmotion"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /I"%GEN%" ^
    /Fo"%OBJ%\screenmotion\\" /Fe"%OBJ%\screenmotion\screen_motion_test.exe" ^
    "tools\screen_motion_test\screen_motion_test.cpp" "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib || exit /b 1
"%OBJ%\screenmotion\screen_motion_test.exe" --self-test || exit /b 1
exit /b 0

:rig_weapon_motion
echo [edvr] === weapon_motion_test ===
if not exist "%OBJ%\weaponmotion" mkdir "%OBJ%\weaponmotion"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\weaponmotion\\" /Fe"%OBJ%\weaponmotion\weapon_motion_test.exe" ^
    "tools\weapon_motion_test\weapon_motion_test.cpp" "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib || exit /b 1
"%OBJ%\weaponmotion\weapon_motion_test.exe" --self-test || exit /b 1
exit /b 0

:rig_identity_fusion
echo [edvr] === identity_fusion_test ===
if not exist "%OBJ%\identityfusion" mkdir "%OBJ%\identityfusion"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\identityfusion\\" /Fe"%OBJ%\identityfusion\identity_fusion_test.exe" ^
    "tools\identity_fusion_test\identity_fusion_test.cpp" "tools\identity_fusion_test\identity_fusion_replay.cpp" ^
    /link /INCREMENTAL:NO d3dcompiler.lib dxguid.lib || exit /b 1
"%OBJ%\identityfusion\identity_fusion_test.exe" --self-test || exit /b 1
python "tools\identity_fusion_capture.py" --self-test || exit /b 1
exit /b 0

:rig_night_vision
echo [edvr] === night_vision_test ===
if not exist "%OBJ%\nightvision" mkdir "%OBJ%\nightvision"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\nightvision\\" /Fe"%OBJ%\nightvision\night_vision_test.exe" ^
    "tools\night_vision_test\night_vision_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib || exit /b 1
"%OBJ%\nightvision\night_vision_test.exe" --self-test || exit /b 1
exit /b 0

:rig_intro_skip
echo [edvr] === DirectShow intro skip regression ===
if not exist "%OBJ%\introskip" mkdir "%OBJ%\introskip"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\introskip\\" /Fe"%OBJ%\introskip\intro_skip_test.exe" ^
    "tools\intro_skip_test\intro_skip_test.cpp" "src\common\iat_hook.cpp" ^
    /link /INCREMENTAL:NO ole32.lib strmiids.lib || exit /b 1
"%OBJ%\introskip\intro_skip_test.exe" --self-test || exit /b 1
python "tools\holo_motion.py" --self-test || exit /b 1
exit /b 0

:rig_aa_compare_test
echo [edvr] === AA comparison key (hotkey.compare_aa) ===
REM The toggle's state machine and notice words, the key-clash rule, Config's runtime override (set, survives a reload, never
REM writes the ini, dropped by a profile switch, honoured by the flat gate's mode read), and the source pins that wire the key.
if not exist "%OBJ%\aacompare" mkdir "%OBJ%\aacompare"
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\aacompare\\" /Fe"%OBJ%\aacompare\aa_compare_test.exe" ^
    "tools\aa_compare_test\aa_compare_test.cpp" "src\common\config.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO user32.lib || exit /b 1
"%OBJ%\aacompare\aa_compare_test.exe" "%ROOT%" || exit /b 1
exit /b 0

:rig_stellar_motion
echo [edvr] === stellar motion regression ===
if not exist "%OBJ%\stellarmotion" mkdir "%OBJ%\stellarmotion"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\stellarmotion\\" /Fe"%OBJ%\stellarmotion\stellar_motion_test.exe" ^
    "tools\stellar_motion_test\stellar_motion_test.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\common\log.cpp" "src\common\config.cpp" "src\common\proxy.cpp" "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: stellar motion test build failed & exit /b 1 )
"%OBJ%\stellarmotion\stellar_motion_test.exe" || exit /b 1
exit /b 0

:rig_terrain_retired_test
echo [edvr] === terrain retirement A/B ===
REM The proof of the 2026-10-01 retirement of advanced.terrain_motion (docs\terrain-motion-dispatch-cost-2026-09-17.md): the
REM temporal compute shader before the cut, rebuilt from the current text and the removed fragments, and after it, run on WARP over
REM scenes that carry terrain-like pixels, give byte-identical outputs for both entries with probe.w bit 8 clear -- every player
REM without the key. It compiles the shader twelve ways on twelve threads (the `main` entry alone is about 20 s in fxc) and runs
REM about 20 s. It exists for that one edit and goes when its anchors move (the rig says so); --verify-old, run once by hand with the
REM pre-retirement file, ties the rebuilt reference to the real old text.
if not exist "%OBJ%\terrainretired" mkdir "%OBJ%\terrainretired"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\terrainretired\\" /Fe"%OBJ%\terrainretired\terrain_retired_test.exe" ^
    "tools\terrain_retired_test\terrain_retired_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 ( echo [edvr] ERROR: terrain retirement test build failed & exit /b 1 )
"%OBJ%\terrainretired\terrain_retired_test.exe" --dry-run || exit /b 1
"%OBJ%\terrainretired\terrain_retired_test.exe" || exit /b 1
exit /b 0

:rig_celestial_motion_test
echo [edvr] === planet patch motion (celestial) ===
REM The planet patch motion (docs\terrain-motion-dispatch-cost-2026-09-17.md, 2026-10-06): src\common\celestial_math.h run on the REAL patch
REM constants of eye dump 180540 (tools\celestial_motion_test\fixture_180540.bin, extracted by tools\celestial_fixture.py, which re-derives
REM every reference in its own --self-test below), the motion shader's celestial path on WARP (a pixel inside a body's volume takes path 12 with
REM the body's motion, outside takes path 2, and with no records the outputs are byte-identical to the shader compiled without the path), and
REM the CPU tee of src\d3d11\celestial_motion.cpp (Map/Unmap, UpdateSubresource, the copies that invalidate, the draw capture, the census).
python "tools\celestial_fixture.py" --self-test || exit /b 1
if not exist "%OBJ%\celestialmotion" mkdir "%OBJ%\celestialmotion"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /Fo"%OBJ%\celestialmotion\\" /Fe"%OBJ%\celestialmotion\celestial_motion_test.exe" ^
    "tools\celestial_motion_test\celestial_motion_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 ( echo [edvr] ERROR: celestial motion test build failed & exit /b 1 )
"%OBJ%\celestialmotion\celestial_motion_test.exe" || exit /b 1
exit /b 0

:rig_on_foot_split_test
echo [edvr] === on_foot_split_test.exe ===
REM The ship split on foot (docs\per-object-motion.md, 2026-10-07; src\common\temporal_mode.h): Explorer Cam's ground within ten metres smeared
REM because the temporal pass sent every pixel nearer than advanced.temporal_aa_ship_metres down the HEAD path, and on foot the ground is the
REM world. While Status.json says the commander is on foot and not seated, the split the shader reads is a millimetre; the shader is not
REM touched. The rig holds: the pure verdict (on foot, in a ship, in an SRV, unknown, stale, not read, before LoadGame, the contradictions, the
REM order of the reasons, and 192 input combinations against the spec written out again), the seat from Status.json's flags (Flags bits 24-26,
REM Flags2 bits 1-2), the split's value (the configured one bit for bit off foot, a millimetre on foot, never zero, a player's zero kept), the
REM production shader on WARP over patches of two REAL eye dumps (tools\on_foot_split_fixture.py: Steam Explorer Cam 090359 and Frontier cockpit
REM 054804) -- the dump's own split reproduces what the game's shader wrote (path, flags, motion, depth), on foot no pixel of the ground takes the
REM head path and every pixel moves as the camera's rows say, the cockpit is byte for byte what it was and keeps the head path under a walking
REM camera -- six broken shaders the same judge must refuse, and the source pins (the pass asks the pure functions, tells each change of mode
REM and the totals with their zeros). tools\on_foot_split_test\mutants.py --self-test holds the mutation list to the headers as they are; the
REM list itself (--run, on demand, under a minute) proves the rig fails when each rule is flipped. A WARP device, no window.
python "tools\on_foot_split_fixture.py" --self-test || exit /b 1
if not exist "%OBJ%\onfootsplit" mkdir "%OBJ%\onfootsplit"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /I"src\common" /I"src\d3d11" /Fo"%OBJ%\onfootsplit\\" /Fe"%OBJ%\onfootsplit\on_foot_split_test.exe" ^
    "tools\on_foot_split_test\on_foot_split_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 ( echo [edvr] ERROR: on foot split test build failed & exit /b 1 )
"%OBJ%\onfootsplit\on_foot_split_test.exe" --dry-run || exit /b 1
"%OBJ%\onfootsplit\on_foot_split_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\on_foot_split_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_depth_scene_pick_test
echo [edvr] === scene depth selection cache regression ===
if not exist "%OBJ%\depthscenepick" mkdir "%OBJ%\depthscenepick"
%CC% /I"%GEN%" /nologo /O2 /Gy /MT /std:c++17 /EHsc /W4 ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
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

:rig_object_classification
echo [edvr] === object classification provenance regression ===
if not exist "%OBJ%\classification" mkdir "%OBJ%\classification"
ml64.exe /nologo /c /Fo"%OBJ%\classification\unwind_stubs.obj" ^
    "tools\object_classification_test\unwind_stubs.asm"
if errorlevel 1 ( echo [edvr] ERROR: unwind fixture build failed & exit /b 1 )
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\panelsnapshot\\" /Fe"%OBJ%\panelsnapshot\eye_panel_snapshot_test.exe" ^
    "tools\eye_panel_snapshot_test\eye_panel_snapshot_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: panel snapshot test build failed & exit /b 1 )
"%OBJ%\panelsnapshot\eye_panel_snapshot_test.exe" "%OBJ%\panelsnapshot\fixture.bin" || exit /b 1
python "tools\eye_panel_snapshot.py" --self-test || exit /b 1
python "tools\eye_panel_snapshot.py" "%OBJ%\panelsnapshot\fixture.bin" --verify-fixture || exit /b 1
REM The crisp-HUD parity gate (G-F) reads both snapshot formats; gate its
REM self-test here, after both parents' fixtures ran.
python "tools\hud_parity.py" --self-test || exit /b 1
exit /b 0

:rig_gpu_span_state_test
REM CPU policy prototype only: no shipping proxy calls this state machine yet.
if not exist "%OBJ%\gpuspanstate" mkdir "%OBJ%\gpuspanstate"
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\gpuspanstate\\" /Fe"%BUILD%\gpu_span_state_test.exe" ^
    "tools\gpu_span_state_test\gpu_span_state_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: GPU span state test build failed & exit /b 1 )
"%BUILD%\gpu_span_state_test.exe" --self-test || exit /b 1
exit /b 0

:rig_optimized_station_test
echo [edvr] === optimized_station_test.exe ===
REM The optimized station's pure half (src\d3d11\optimized_station_core.h): the seal trigger, the release rules, the pad-local frame, the
REM per-size floors, the own-ship rules and the bay volume. No DLL and no game.
if not exist "%OBJ%\optimizedstation" mkdir "%OBJ%\optimizedstation"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\optimizedstation\\" ^
    /Fe"%BUILD%\optimized_station_test.exe" "tools\optimized_station_test\optimized_station_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: optimized station test build failed & exit /b 1 )
"%BUILD%\optimized_station_test.exe" --self-test || exit /b 1
exit /b 0

:rig_exposure_chain_test
echo [edvr] === exposure_chain_test.exe ===
REM The exposure engine path (src\d3d11\exposure_chain.h, the always-on exposure sharing): the pure decision (learning, the second-eye skip, every decline,
REM fallback and latch), three pipelines of stand-in chain shaders on a WARP device with the debug layer (stock, the old share copy, the engine
REM path: eye 0 and eye 1 must come out bit-identical frame by frame through engaging, a lost confirmation and a stand-down), and source pins on
REM the hook's glue and on the removal of the Scroll Lock toggle. No DLL, no game, no window.
if not exist "%OBJ%\exposurechain" mkdir "%OBJ%\exposurechain"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\exposurechain\\" ^
    /Fe"%BUILD%\exposure_chain_test.exe" "tools\exposure_chain_test\exposure_chain_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: exposure chain test build failed & exit /b 1 )
"%BUILD%\exposure_chain_test.exe" --dry-run || exit /b 1
"%BUILD%\exposure_chain_test.exe" --self-test "%ROOT%" || exit /b 1
exit /b 0

:rig_light_grid_cache_test
echo [edvr] === light_grid_cache_test.exe ===
REM The light grid cache's pure half (src\d3d11\light_grid_cache_core.h): the identity and pin decision, the key built as the game builds its own, the state regions, the
REM bit-exact compare and the LRU, whole-state swaps against a fake builder that owns tracked allocations (an ownership audit after every frame), the forward reasons, the
REM verify-and-disable path, faults in the memory policy, the thread rule, the jitter probe and the exact line texts. No DLL and no game.
if not exist "%OBJ%\lightgridcache" mkdir "%OBJ%\lightgridcache"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\lightgridcache\\" ^
    /Fe"%BUILD%\light_grid_cache_test.exe" "tools\light_grid_cache_test\light_grid_cache_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: light grid cache test build failed & exit /b 1 )
"%BUILD%\light_grid_cache_test.exe" --self-test || exit /b 1
exit /b 0

:rig_pad_state_test
echo [edvr] === pad_state_test.exe ===
REM The pad-state probe's pure half (src\d3d11\pad_state_core.h): the identity and prologue decision, change detection, the exact line texts, the
REM bounded object table and its full-table fallback, and the line cap. Temporary, log only. No DLL and no game.
if not exist "%OBJ%\padstate" mkdir "%OBJ%\padstate"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\padstate\\" ^
    /Fe"%BUILD%\pad_state_test.exe" "tools\pad_state_test\pad_state_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: pad state test build failed & exit /b 1 )
"%BUILD%\pad_state_test.exe" --self-test || exit /b 1
exit /b 0

:rig_gpu_disjoint_clock_test
REM Shared query ownership and the real D3D11 adapter remain desk-only until
REM every existing timer has migrated. Never load the proxy beside these rigs.
if not exist "%OBJ%\gpuclock" mkdir "%OBJ%\gpuclock"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Fo"%OBJ%\gpuclock\\" /Fe"%OBJ%\gpuclock\gpu_disjoint_clock_test.exe" ^
    "tools\gpu_disjoint_clock_test\gpu_disjoint_clock_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: shared GPU clock test build failed & exit /b 1 )
"%OBJ%\gpuclock\gpu_disjoint_clock_test.exe" --dry-run || exit /b 1
"%OBJ%\gpuclock\gpu_disjoint_clock_test.exe" --self-test || exit /b 1
exit /b 0

:rig_gpu_span_d3d11_test
if not exist "%OBJ%\gpuspand3d11" mkdir "%OBJ%\gpuspand3d11"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Fo"%OBJ%\gpuspand3d11\\" /Fe"%OBJ%\gpuspand3d11\gpu_span_d3d11_test.exe" ^
    "tools\gpu_span_d3d11_test\gpu_span_d3d11_test.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: D3D11 GPU span test build failed & exit /b 1 )
"%OBJ%\gpuspand3d11\gpu_span_d3d11_test.exe" --dry-run || exit /b 1
"%OBJ%\gpuspand3d11\gpu_span_d3d11_test.exe" --self-test || exit /b 1
exit /b 0

:rig_producer_gpu_timing_test
if not exist "%OBJ%\producergputiming" mkdir "%OBJ%\producergputiming"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\gpucensus\\" ^
    /Fe"%OBJ%\gpucensus\gpu_census_test.exe" "tools\gpu_census_test\gpu_census_test.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_frame_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    "src\common\config.cpp" "src\common\proxy.cpp" "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: GPU census test build failed & exit /b 1 )
"%OBJ%\gpucensus\gpu_census_test.exe" --dry-run || exit /b 1
"%OBJ%\gpucensus\gpu_census_test.exe" --self-test || exit /b 1
exit /b 0

:rig_gpu_pass_profile_test
REM TEMPORARY GPU pass profiler (2026-10-12): the pure logic of src\d3d11\gpu_pass_profile_core.h -- classification, cube
REM runs, the stamp budget, the shader tally, the summary arithmetic and the CSV text. No D3D11 here.
if not exist "%OBJ%\gpupassprofile" mkdir "%OBJ%\gpupassprofile"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\gpupassprofile\\" ^
    /Fe"%OBJ%\gpupassprofile\gpu_pass_profile_test.exe" "tools\gpu_pass_profile_test\gpu_pass_profile_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: GPU pass profile test build failed & exit /b 1 )
"%OBJ%\gpupassprofile\gpu_pass_profile_test.exe" --self-test || exit /b 1
exit /b 0

:rig_blend_probe_test
REM TEMPORARY blend probe (2026-10-14): the pure logic of src\d3d11\blend_probe_core.h -- the window, which draws are rows, the row and
REM grid text and its line length against the log buffer, the refusal reasons and the grid indexing. No D3D11 here.
if not exist "%OBJ%\blendprobe" mkdir "%OBJ%\blendprobe"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /Fo"%OBJ%\blendprobe\\" ^
    /Fe"%OBJ%\blendprobe\blend_probe_test.exe" "tools\blend_probe_test\blend_probe_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: blend probe test build failed & exit /b 1 )
"%OBJ%\blendprobe\blend_probe_test.exe" --self-test || exit /b 1
exit /b 0

:rig_vr_world_mips_test
REM The VR on-foot world route's mipped screen (design doc section 82, src\d3d11\vr_world_mips.cpp): a real D3D11 WARP
REM device, the production module linked in as a source, away from build\d3d11.dll. The rig takes System32's device through
REM src\common\system_d3d11.h, so it links without d3d11.lib, and its exe sits under obj\, not directly in build\.
REM tools\vr_world_mips_test\mutants.py --self-test holds the rig's mutation list to the source as it is; the list itself
REM (--run, on demand, about a minute) proves the rig fails when each rule of the module is flipped.
if not exist "%OBJ%\vrworldmips" mkdir "%OBJ%\vrworldmips"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\vrworldmips\\" /Fe"%OBJ%\vrworldmips\vr_world_mips_test.exe" ^
    "tools\vr_world_mips_test\vr_world_mips_test.cpp" "src\d3d11\vr_world_mips.cpp" ^
    /link /INCREMENTAL:NO d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: vr_world_mips_test build failed & exit /b 1 )
"%OBJ%\vrworldmips\vr_world_mips_test.exe" --dry-run || exit /b 1
"%OBJ%\vrworldmips\vr_world_mips_test.exe" --self-test || exit /b 1
python "tools\vr_world_mips_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_vscreen_fit_test
echo [edvr] === vscreen_fit_test.exe ===
REM fix.vscreen_res_width = auto, fitted to what each eye shows (design doc section 82, the "vscreen auto-fit" entry): the pure half
REM (src\common\vscreen_fit.h) run by the very code the DLL runs -- the rule, the route's conditions, the footprint geometry, the stored
REM record, the log's text -- plus the state files on disk (src\common\vscreen_auto_state.cpp) and the source pins that hold the resolver,
REM the instrument's hook and its D3D calls to what the pure half assumes (they read src\ and edvr.ini from the repo root, which the rig
REM takes as its argument). tools\vscreen_fit_test\mutants.py --self-test holds the rig's mutation list to the sources as they are; the
REM list itself (--run, on demand, about a minute) proves the rig fails when each rule of the header or each wiring pin is flipped.
if not exist "%OBJ%\vscreenfit" mkdir "%OBJ%\vscreenfit"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\vscreenfit\\" /Fe"%OBJ%\vscreenfit\vscreen_fit_test.exe" ^
    "tools\vscreen_fit_test\vscreen_fit_test.cpp" "src\common\vscreen_auto_state.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: vscreen_fit_test build failed & exit /b 1 )
"%OBJ%\vscreenfit\vscreen_fit_test.exe" --dry-run || exit /b 1
"%OBJ%\vscreenfit\vscreen_fit_test.exe" --dry-run --write-fixture "%OBJ%\vscreenfit\never_written.log" || exit /b 1
if exist "%OBJ%\vscreenfit\never_written.log" ( echo [edvr] ERROR: vscreen_fit_test --dry-run --write-fixture wrote a file & exit /b 1 )
"%OBJ%\vscreenfit\vscreen_fit_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\vscreen_fit_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_terrain_checkerboard_test
echo [edvr] === terrain_checkerboard_test.exe ===
REM Elite's terrain checkerboard rendering in VR, said in the headset (design doc section 84, the VR hint): the words and the pure picks
REM (src\common\terrain_checkerboard_notice.h), the reader on fixture folders and the monitor's rule for a half-written file
REM (src\d3d11\terrain_checkerboard_reader.h), the worker thread on a real log (src\d3d11\terrain_checkerboard.cpp, linked with the real
REM Log, Config and fault guard, and then tools\edvr_log.py --terrain-checkerboard over the log it wrote: needs python on PATH) and the
REM wiring in menu.cpp as source pins (they read src\ from the repo root, which the rig takes as its argument). The fixtures are written
REM to a temp directory and removed. tools\terrain_checkerboard_test\mutants.py --self-test holds the rig's mutation list to the sources as
REM they are; the list itself (--run, on demand, about a minute) proves the rig fails when each rule of the headers, the worker or the
REM wiring is broken.
if not exist "%OBJ%\terraincheckerboard" mkdir "%OBJ%\terraincheckerboard"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\terraincheckerboard\\" /Fe"%OBJ%\terraincheckerboard\terrain_checkerboard_test.exe" ^
    "tools\terrain_checkerboard_test\terrain_checkerboard_test.cpp" "src\d3d11\terrain_checkerboard.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: terrain_checkerboard_test build failed & exit /b 1 )
"%OBJ%\terraincheckerboard\terrain_checkerboard_test.exe" --dry-run || exit /b 1
"%OBJ%\terraincheckerboard\terrain_checkerboard_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\terrain_checkerboard_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_vscreen_footprint_glue_test
echo [edvr] === vscreen_footprint_glue_test.exe ===
REM The footprint instrument's glue (src\d3d11\vscreen_footprint.cpp; design doc section 82, the "vscreen auto-fit" entry) compiled for
REM real with the real Config, Log and fault guard, and stubs for what it calls (the runtime's published eye size, the hooks' bypass
REM flag): the four sources a composite draw binds (the model rows, the clip rows at their real offsets, the quad, the SIZE record) on a
REM WARP device read back through the staging copy and the DO_NOT_WAIT map, the 30 s line, the stored median, the sources that are not
REM what the measurement assumes, the fault budget -- and then tools\edvr_log.py --vscreen-fit over the log the glue wrote. Needs python
REM on PATH; runs from the repo root; takes about a second. Its exe sits under obj\ and it takes System32's device through
REM src\common\system_d3d11.h, so it links without d3d11.lib.
if not exist "%OBJ%\vscreenfpglue" mkdir "%OBJ%\vscreenfpglue"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"%GEN%" ^
    /Fo"%OBJ%\vscreenfpglue\\" /Fe"%OBJ%\vscreenfpglue\vscreen_footprint_glue_test.exe" ^
    "tools\vscreen_footprint_glue_test\vscreen_footprint_glue_test.cpp" "src\d3d11\vscreen_footprint.cpp" "src\common\vscreen_auto_state.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: vscreen_footprint_glue_test build failed & exit /b 1 )
"%OBJ%\vscreenfpglue\vscreen_footprint_glue_test.exe" --dry-run || exit /b 1
"%OBJ%\vscreenfpglue\vscreen_footprint_glue_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openvr_abi_test
if not exist "%OBJ%\openvr_abi" mkdir "%OBJ%\openvr_abi"
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\openvr_abi\\" /Fe"%BUILD%\openvr_abi_test.exe" ^
    "tools\openvr_abi_test\openvr_abi_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: OpenVR ABI test build failed & exit /b 1 )
"%BUILD%\openvr_abi_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_probe
REM Headset-free OpenXR enumeration contract tests. The probe is standalone;
REM the shipping proxies do not load OpenXR or create an OpenXR session.
if not exist "%OBJ%\openxr_probe" mkdir "%OBJ%\openxr_probe"
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS ^
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
    %CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" ^
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
    %CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS ^
        /I"third_party\openxr\include" /I"%GEN%" /Fo"%OBJ%\openxr_native_tests\\" ^
        /Fe"%BUILD%\openxr_%%T_test.exe" "tools\openxr_%%T_test\openxr_%%T_test.cpp" ^
        "src\openxr\d3d11_stereo.cpp" "src\openxr\session_binding.cpp" "src\openxr\openvr_system.cpp" "src\openxr\eye_capture.cpp" "src\openxr\skybox_capture.cpp" ^
        "src\openxr\shared_texture_transfer.cpp" "src\openxr\producer_gpu_timing.cpp" ^
        "src\openxr\device_gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
        "src\openxr\openvr_compositor.cpp" "tools\openxr_native_test\compositor_caller.cpp" ^
        "src\openxr\openvr_auxiliary.cpp" "src\openxr\runtime_exports.cpp" ^
        "src\common\frame_flag.cpp" ^
        /link /INCREMENTAL:NO dxgi.lib d3dcompiler.lib user32.lib gdi32.lib
    if errorlevel 1 ( echo [edvr] ERROR: OpenXR %%T test build failed & exit /b 1 )
)
for %%T in (native stereo) do (
    "%BUILD%\openxr_%%T_test.exe" --dry-run || exit /b 1
    "%BUILD%\openxr_%%T_test.exe" --self-test || exit /b 1
)
REM tools\openxr_stereo_test\mutants.py --self-test holds the mutation list to d3d11_stereo.cpp as it is (a fully black frame draws nothing: case B1); --run builds the
REM rig against each edit.
python "tools\openxr_stereo_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_openxr_capture_test
if not exist "%OBJ%\openxr_capture_test" mkdir "%OBJ%\openxr_capture_test"
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
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
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG ^
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
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\native_device_test\\" /Fe"%BUILD%\native_device_test.exe" ^
    "tools\openxr_native_test\native_device_test.cpp" /link /INCREMENTAL:NO dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: native device test build failed & exit /b 1 )
"%BUILD%\native_device_test.exe" --dry-run || exit /b 1
"%BUILD%\native_device_test.exe" --self-test || exit /b 1
python tools\run_openxr_native.py --self-test || exit /b 1
exit /b 0

:rig_openxr_binding_test
if not exist "%OBJ%\openxr_binding_test" mkdir "%OBJ%\openxr_binding_test"
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_binding_test\\" /Fe"%BUILD%\openxr_binding_test.exe" ^
    "tools\openxr_binding_test\openxr_binding_test.cpp" "src\openxr\session_binding.cpp" ^
    "src\openxr\published_session.cpp" "src\common\frame_flag.cpp" ^
    /link /INCREMENTAL:NO dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: OpenXR binding test build failed & exit /b 1 )
"%BUILD%\openxr_binding_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_binding_test.exe" --self-test || exit /b 1
"%BUILD%\openxr_binding_test.exe" --published-proxy "%BUILD%\edvr_openxr_graphics.dll" || exit /b 1
exit /b 0

:rig_native_startup_test
REM Inspect actual startup behavior without creating a headset runtime. The
REM fixture is intentionally not Elite: it must leave its IAT alone.
if not exist "%OBJ%\native_startup" mkdir "%OBJ%\native_startup"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Fo"%OBJ%\native_startup\\" /Fe"%BUILD%\native_startup_test.exe" ^
    "tools\native_startup_test\native_startup_test.cpp" /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: native startup test build failed & exit /b 1 )
"%BUILD%\native_startup_test.exe" --dry-run || exit /b 1
"%BUILD%\native_startup_test.exe" --self-test "%BUILD%\edvr_openxr_graphics.dll" native || exit /b 1
exit /b 0

:rig_oculus_route_test
REM Qualify the early loader route without running Elite or a headset runtime.
if not exist "%OBJ%\oculus_route" mkdir "%OBJ%\oculus_route"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Fo"%OBJ%\elite_oculus\\" /Fe"%BUILD%\elite_oculus_test.exe" ^
    "tools\elite_oculus_test\elite_oculus_test.cpp" /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: Elite Oculus profile test build failed & exit /b 1 )
"%BUILD%\elite_oculus_test.exe" --dry-run || exit /b 1
"%BUILD%\elite_oculus_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_proxy_state_test
if not exist "%OBJ%\openxr_proxy_state_test" mkdir "%OBJ%\openxr_proxy_state_test"
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" /I"%GEN%" ^
    /Fo"%OBJ%\openxr_proxy_state_test\\" /Fe"%BUILD%\openxr_proxy_state_test.exe" ^
    "tools\openxr_proxy_state_test\openxr_proxy_state_test.cpp" ^
    "src\openxr\d3d11_stereo.cpp" "src\openxr\eye_capture.cpp" "src\openxr\skybox_capture.cpp" ^
    "src\openxr\shared_texture_transfer.cpp" "src\openxr\producer_gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO dxgi.lib d3dcompiler.lib user32.lib gdi32.lib
if errorlevel 1 ( echo [edvr] ERROR: OpenXR proxy state test build failed & exit /b 1 )
"%BUILD%\openxr_proxy_state_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_proxy_state_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_present_test
if not exist "%OBJ%\openxr_present_test" mkdir "%OBJ%\openxr_present_test"
REM Actual owned-swapchain Present hook, foreign Init caller and private work.
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_present_test\\" /Fe"%BUILD%\openxr_present_test.exe" ^
    "tools\openxr_present_test\openxr_present_test.cpp" ^
    /link /INCREMENTAL:NO dxgi.lib user32.lib
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
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_shutdown_test\\" /Fe"%BUILD%\openxr_shutdown_test.exe" ^
    "tools\openxr_shutdown_test\openxr_shutdown_test.cpp" ^
    /link /INCREMENTAL:NO dxgi.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: OpenXR stopped-Present shutdown test build failed & exit /b 1 )
"%BUILD%\openxr_shutdown_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_shutdown_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_shared_texture_test
if not exist "%OBJ%\openxr_shared_texture_test" mkdir "%OBJ%\openxr_shared_texture_test"
REM Cross-device texture handoff and consumer-only retirement after producer stop.
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\openxr_shared_texture_test\\" /Fe"%BUILD%\openxr_shared_texture_test.exe" ^
    "tools\openxr_shared_texture_test\openxr_shared_texture_test.cpp" ^
    "src\openxr\shared_texture_transfer.cpp" "src\openxr\eye_capture.cpp" "src\openxr\skybox_capture.cpp" ^
    "src\openxr\producer_gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: OpenXR shared texture test build failed & exit /b 1 )
"%BUILD%\openxr_shared_texture_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_shared_texture_test.exe" --self-test || exit /b 1
exit /b 0

:rig_xess_engine_test
REM XeSS bridge contracts and hardware readback. GPU-less or unsupported
REM adapters skip explicitly; a supported adapter must pass the bridge.
if not defined XESS (
    echo [edvr] xess_engine_test: no XeSS SDK in this build -- skipping.
    exit /b 0
)
if not exist "%OBJ%\xess_engine_test" mkdir "%OBJ%\xess_engine_test"
REM This exe lives below obj: an import of d3d11.lib from an exe directly in
REM build would accidentally load EDVR's proxy instead of the system DLL.
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX %XESSFLAGS% /I"%GEN%" ^
    /Fo"%OBJ%\xess_engine_test\\" /Fe"%OBJ%\xess_engine_test\xess_engine_test.exe" ^
    "tools\xess_engine_test\xess_engine_test.cpp" "src\d3d11\xess_engine.cpp" "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib dxgi.lib
if errorlevel 1 ( echo [edvr] ERROR: XeSS engine test build failed & exit /b 1 )
copy /y "%BUILD%\libxess.dll" "%OBJ%\xess_engine_test\libxess.dll" >nul
if errorlevel 1 ( echo [edvr] ERROR: XeSS engine test runtime staging failed & exit /b 1 )
"%OBJ%\xess_engine_test\xess_engine_test.exe" --dry-run || exit /b 1
"%OBJ%\xess_engine_test\xess_engine_test.exe" --self-test || exit /b 1
"%OBJ%\xess_engine_test\xess_engine_test.exe" --gpu-test-if-available || exit /b 1
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
%CC% /nologo /W4 /O2 /EHs /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
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
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_system_test\\" /Fe"%BUILD%\openxr_system_test.exe" ^
    "tools\openxr_system_test\openxr_system_test.cpp" "tools\openxr_system_test\abi_caller.cpp" ^
    "src\openxr\openvr_system.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: owned OpenVR system test build failed & exit /b 1 )
"%BUILD%\openxr_system_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_system_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_launch_centre_test
if not exist "%OBJ%\openxr_launch_centre_test" mkdir "%OBJ%\openxr_launch_centre_test"
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_launch_centre_test\\" /Fe"%BUILD%\openxr_launch_centre_test.exe" ^
    "tools\openxr_launch_centre_test\openxr_launch_centre_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: OpenXR launch centering test build failed & exit /b 1 )
"%BUILD%\openxr_launch_centre_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_launch_centre_test.exe" --self-test || exit /b 1
exit /b 0

:rig_seat_memory_test
REM The seat memory's decisions and its file (src\openxr\seat_memory.h, seat_store.h, src\common\seat_context.h): the decision table, the
REM transitions, the start and intro plans, the capture, the LOCAL-change compensation and the persistence round trip in a scratch folder.
if not exist "%OBJ%\seat_memory_test" mkdir "%OBJ%\seat_memory_test"
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\seat_memory_test\\" /Fe"%BUILD%\seat_memory_test.exe" ^
    "tools\seat_memory_test\seat_memory_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: seat memory test build failed & exit /b 1 )
"%BUILD%\seat_memory_test.exe" --dry-run || exit /b 1
"%BUILD%\seat_memory_test.exe" --self-test || exit /b 1
exit /b 0

:rig_seat_press_test
REM Elite's HMDReset action out of .binds text (src\d3d11\elite_binds.cpp eliteBindsHmdReset): the keyboard with modifiers, a HOTAS button, a pad
REM button, and what nothing here can watch (the mouse, an axis); the active preset's newest file answers. No device, no window, no game.
if not exist "%OBJ%\seat_press_test" mkdir "%OBJ%\seat_press_test"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\seat_press_test\\" /Fe"%BUILD%\seat_press_test.exe" "tools\seat_press_test\seat_press_test.cpp" ^
    "src\common\hotkey.cpp" "src\d3d11\elite_binds.cpp" "src\common\iniedit.cpp" "src\common\config.cpp" "src\common\log.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: seat press test build failed & exit /b 1 )
"%BUILD%\seat_press_test.exe" --dry-run || exit /b 1
"%BUILD%\seat_press_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_compositor_test
if not exist "%OBJ%\openxr_compositor_test" mkdir "%OBJ%\openxr_compositor_test"
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_compositor_test\\" /Fe"%BUILD%\openxr_compositor_test.exe" ^
    "tools\openxr_compositor_test\openxr_compositor_test.cpp" "tools\openxr_compositor_test\abi_caller.cpp" ^
    "src\openxr\openvr_compositor.cpp" "src\common\frame_flag.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: owned OpenVR compositor test build failed & exit /b 1 )
"%BUILD%\openxr_compositor_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_compositor_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_auxiliary_test
if not exist "%OBJ%\openxr_auxiliary_test" mkdir "%OBJ%\openxr_auxiliary_test"
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
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
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /LD /I"third_party\openxr\include" ^
    /Fo"%OBJ%\openxr_exports_test\\" /Fe"%BUILD%\openxr_export_fixture.dll" ^
    "tools\openxr_exports_test\fixture.cpp" "src\openxr\runtime_exports.cpp" ^
    "src\openxr\openvr_system.cpp" "src\openxr\openvr_compositor.cpp" "src\openxr\openvr_auxiliary.cpp" ^
    "src\common\frame_flag.cpp" ^
    /link /INCREMENTAL:NO /DEF:"tools\openxr_exports_test\fixture.def"
if errorlevel 1 ( echo [edvr] ERROR: OpenXR export fixture build failed & exit /b 1 )
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
    /Fo"%OBJ%\openxr_exports_test\\" /Fe"%BUILD%\openxr_exports_test.exe" ^
    "tools\openxr_exports_test\openxr_exports_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: OpenXR export test build failed & exit /b 1 )
"%BUILD%\openxr_exports_test.exe" --dry-run || exit /b 1
"%BUILD%\openxr_exports_test.exe" --self-test || exit /b 1
exit /b 0

:rig_openxr_module_test
if not exist "%OBJ%\openxr_module_test" mkdir "%OBJ%\openxr_module_test"
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\openxr_module_test\\" /Fe"%BUILD%\openxr_module_test.exe" ^
    "tools\openxr_module_test\openxr_module_test.cpp" ^
    /link /INCREMENTAL:NO dxgi.lib d3dcompiler.lib user32.lib gdi32.lib
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


%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /GR- /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /GR- /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /I"%GEN%" ^
    /Fo"%OBJ%\insttest"\ /Fe"%BUILD%\installer_test.exe" ^
    "tools\installer_test\installer_test.cpp" ^
    "src\installer\plan.cpp" "src\installer\apply.cpp" ^
    "src\installer\detect.cpp" "src\installer\probe.cpp" ^
    "src\common\iniedit.cpp" "src\installer\state.cpp" ^
    "src\installer\mirror.cpp" ^
    "src\installer\settings.cpp" "src\installer\logbundle.cpp" ^
    "src\installer\app.cpp" "src\installer\payload.cpp" ^
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
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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
%CC% /nologo /W4 /O2 /EHsc /std:c++17 /MT /DNDEBUG /LD ^
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
REM The independent Python checks run at once through tools\run_jobs.py --gate (four at a time; each one's output whole under a header with its
REM seconds, failures printed LAST, nonzero if any failed), where they used to run one after another in 134 s. Each writes only to a temp
REM folder of its own (checked 2026-10-14: openxr_pe, elite_oculus, run_openxr_frontier, cpu_profile, fetch_ffx_dx11, eye_run_ledger, the flat_*
REM readers, install_edvr, edvr_log, reflow_notes and the desk scripts all build their fixtures under tempfile), and reads only the checkout and
REM build\edvr_openxr_runtime.dll / edvr_openxr_graphics.dll, which the main flow finished before any rig started. Cwd is the repo root, so the
REM paths are relative. What stays outside the group, in order below: the profiling-only analyzer build, the two source checks whose failure
REM message says what the rule is, and the one C++ compile.
REM Notes: the census readers (diff_draw_census, crisp_ui_gates, stencil_census, draw_identity) assert the PARSE of the DC line format, so a
REM column added to the emitter that swallowed a neighbour fails here, not in the ten minutes after a flight; eye_run_ledger's row layout must
REM match LedgerDraw in src\d3d11\object_probe.cpp byte for byte; install_edvr's --dry-run must write NOTHING (not a copy, not a backup, not a
REM directory) and its backup naming is one string in one place; edvr_log's version regex must match the line Log::note() really writes,
REM timestamp prefix and all; reflow_notes must leave fenced code, tables and long URLs alone, be idempotent and write UTF-8 with no BOM; the
REM desk scripts (compile_variants .. eye_run_spin) need no build products, only numpy and Pillow.
REM --jobs 4, not every core: this group runs beside up to fifteen other rigs. At every core (2026-10-14) its gates summed 546 s
REM against 26 s alone, and run_openxr_frontier's self-test timed out the 10 s it gives a child python to start.
python tools\run_jobs.py --jobs 4 ^
    --gate "tools\openxr_pe.py --self-test" ^
    --gate "tools\openxr_pe.py --native build\edvr_openxr_runtime.dll" ^
    --gate "tools\openxr_pe.py --graphics build\edvr_openxr_graphics.dll" ^
    --gate "tools\elite_oculus.py --self-test" ^
    --gate "tools\run_openxr_frontier.py --self-test" ^
    --gate "tools\cpu_profile.py --self-test" ^
    --gate "tools\fetch_ffx_dx11.py --self-test" ^
    --gate "tools\check_install_reads.py --self-test" ^
    --gate "tools\check_exit_paths.py --self-test" ^
    --gate "tools\diff_draw_census.py --self-test" ^
    --gate "tools\crisp_ui_gates.py --self-test" ^
    --gate "tools\stencil_census.py --self-test" ^
    --gate "tools\draw_identity.py --self-test" ^
    --gate "tools\eye_run_ledger.py --self-test" ^
    --gate "tools\flat_pixels_engine.py --self-test" ^
    --gate "tools\flat_pixels.py --self-test" ^
    --gate "tools\flat_draw_pixels.py --self-test" ^
    --gate "tools\flat_draw_packets.py --self-test" ^
    --gate "tools\flat_packet_replay.py --self-test" ^
    --gate "tools\flat_resolve_inputs.py --self-test" ^
    --gate "tools\flat_weapon_pixels.py --self-test" ^
    --gate "tools\install_edvr.py --self-test" ^
    --gate "tools\edvr_log.py --self-test" ^
    --gate "tools\reflow_notes.py --self-test" ^
    --gate "tools\compile_variants.py --self-test" ^
    --gate "tools\dxbc_disasm.py --self-test" ^
    --gate "tools\eye_bmp_to_png.py --self-test" ^
    --gate "tools\diff_eye_dump.py --self-test" ^
    --gate "tools\eye_run_fit.py --self-test" ^
    --gate "tools\eye_run_shimmer.py --self-test" ^
    --gate "tools\eye_run_spin.py --self-test" || exit /b 1

REM Only profiling builds require the optional local .NET/TraceEvent analyzer.
REM Its parser tests must pass before this build can be flown for CPU capture.
if "%EDVR_PROFILE_SYMBOLS%"=="1" (
    python tools\cpu_profile.py --build-analyzer || exit /b 1
)

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

echo [edvr] === offline flat packet replay ===
if not exist "%OBJ%\flat_packet_replay" mkdir "%OBJ%\flat_packet_replay"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /Fo"%OBJ%\flat_packet_replay\replay.obj" /Fe"%BUILD%\flat_packet_replay.exe" ^
    "tools\flat_packet_replay\flat_packet_replay.cpp" /link /INCREMENTAL:NO d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: offline flat packet replay compile failed & exit /b 1 )
"%BUILD%\flat_packet_replay.exe" --self-test || exit /b 1
exit /b 0

:rig_kinematic_json_test
echo [edvr] === kinematic_json_test.exe ===
REM Build gate for the production KinematicEvalProbe JSON writer: three
REM serialization failures reached the flight journal before this rig
REM existed, and compilation cannot catch a dropped quote. The exe
REM serializes a deterministic fixture through the real writeJson; the
REM python gate strict-parses it and asserts every value round-trips.
if not exist "%OBJ%\kinematicjson" mkdir "%OBJ%\kinematicjson"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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
REM compute-state save, sentinel by sentinel. It ends with the remember cap (remember_cap_tests.h: a full
REM table of keyed shader objects writes one line per kind on the first drop, never below the cap);
REM tools\engine_velocity_test\mutants.py --self-test holds that line's mutation list to the source as it is.
if not exist "%OBJ%\enginevelocity" mkdir "%OBJ%\enginevelocity"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /DEDVR_ENGINE_VELOCITY_RIG /DEDVR_BINDING_SHADOW_EXTERNAL /I"%GEN%" ^
    /Fo"%OBJ%\enginevelocity\\" /Fe"%OBJ%\enginevelocity\engine_velocity_test.exe" ^
    "tools\engine_velocity_test\engine_velocity_test.cpp" "src\d3d11\engine_velocity.cpp" "src\d3d11\skin_join_gpu.cpp" ^
    "src\d3d11\gpu_timing.cpp" "src\d3d11\gpu_span_d3d11.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 ( echo [edvr] ERROR: engine velocity test build failed & exit /b 1 )
"%OBJ%\enginevelocity\engine_velocity_test.exe" --dry-run || exit /b 1
"%OBJ%\enginevelocity\engine_velocity_test.exe" --self-test || exit /b 1
python "tools\engine_velocity_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_engine_motion_cpu_test
echo [edvr] === engine_motion_cpu_test.exe ===
REM Build gate for engine motion's CPU instrument (src\d3d11\engine_motion_cpu.h,
REM docs\openxr-performance-review-2026-09-14.md, the 2026-09-29 carrier entry):
REM the header-only accumulator and its fold, driven with a fake clock so every
REM figure is exact -- exclusive nesting and the forward pause, thread attribution
REM (render thread and the rest), no lost update under four writers and a cutter,
REM the window's percentiles, "-" for code that never ran against 0.00 for code
REM that ran, the three report lines at their worst against the log line's limit,
REM the priming frame, the 30 s and full-window closes, the clock floor measured
REM on the real clock. It links nothing of the DLL.
if not exist "%OBJ%\enginemotioncpu" mkdir "%OBJ%\enginemotioncpu"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\enginemotioncpu\\" /Fe"%OBJ%\enginemotioncpu\engine_motion_cpu_test.exe" ^
    "tools\engine_motion_cpu_test\engine_motion_cpu_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: engine motion CPU test build failed & exit /b 1 )
"%OBJ%\enginemotioncpu\engine_motion_cpu_test.exe" --dry-run || exit /b 1
"%OBJ%\enginemotioncpu\engine_motion_cpu_test.exe" --self-test || exit /b 1
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\transitionflashprevent\\" /Fe"%BUILD%\transition_flash_prevent_test.exe" ^
    "tools\transition_flash_prevent_test\transition_flash_prevent_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: transition flash prevent test build failed & exit /b 1 )
"%BUILD%\transition_flash_prevent_test.exe" --dry-run || exit /b 1
"%BUILD%\transition_flash_prevent_test.exe" --self-test || exit /b 1
exit /b 0

:rig_explorer_cam_test
echo [edvr] === explorer_cam_test.exe ===
REM Build gate for Explorer Cam (hotkey.explorer_cam; docs\design-explorer-cam-free-camera-2026-10-07.md, "Phase 1a", "Phase 1c").
REM Part A drives src\d3d11\explorer_cam_core.h, the pure half the DLL compiles: the build-332841 identity and CodeHook's reading of all five
REM prologues, the eye keys' clamp, the exact 16 floats written into the free camera's commander-local pose, the placement machine on
REM scripted state sequences, the F5 sequencer on scripted mode sequences (enter from every mode, timeouts, the player's own TAB engaging
REM nothing, detach and re-attach, exit), F5's decision table, the camera-UI hider, the watches, the event ring, every log line's text and
REM the relays' machine code. Part B compiles the REAL glue (explorer_cam.cpp, EDVR_EXPLORER_CAM_TEST) with the real CodeHook and installs all
REM five hooks on synthetic functions that begin with the real prologues: the pose is written before the original, every pressed int is 1
REM for exactly one call and restored after it, both bypass relays answer 0 without running the function for the placed activity only and
REM hand every other call, with its arguments (the sweep's fifth on the stack), to the original, each wrong prologue stands down with one
REM line and no patch, F5 opens the camera, switches to the free camera, places, locks and hides the UI and F5 again gives the UI back and
REM closes the camera, the hotkey is checked against a fixture of the player's Elite bindings at launch and on a rebind, faults are counted
REM and the eighth ends the feature, and the keys are read from the config under their real names.
if not exist "%OBJ%\explorercam" mkdir "%OBJ%\explorercam"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DEDVR_EXPLORER_CAM_TEST /I"%GEN%" ^
    /Fo"%OBJ%\explorercam"\ /Fe"%BUILD%\explorer_cam_test.exe" ^
    "tools\explorer_cam_test\explorer_cam_test.cpp" "src\d3d11\explorer_cam.cpp" "src\d3d11\elite_binds.cpp" ^
    "src\common\hotkey.cpp" "src\common\code_hook.cpp" "src\common\guard.cpp" "src\common\log.cpp" "src\common\config.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: explorer cam test build failed & exit /b 1 )
"%BUILD%\explorer_cam_test.exe" --self-test || exit /b 1
exit /b 0

:rig_explorer_cam_fade_test
echo [edvr] === explorer_cam_fade_test.exe ===
REM Build gate for the entry fade's wait for the engine's motion (src\d3d11\explorer_cam_fade_core.h, "MOTION"; docs\kinematic-motion-injection-2026-09-19.md,
REM "F12 fixes built"): the pure comfort timeline driven with the facts the glue feeds it from the engine (engine_motion_ready.h). An entry that is otherwise
REM ready fades in at once when the engine's motion is live (the eye path's views for three frames in a row, and the second skin's join when the frame has
REM skinned jobs), holds black until it is, gives up one second after it was otherwise ready and says which condition was missing, does not wait for a join
REM in a scene with no characters, waits for nothing when the engine's motion is not armed, never delays an exit, waits on a re-attach like an entry, starts
REM the hold over when the entry stops being ready, and leaves the 3 s cap from the press as it was. The lines. No device needed.
REM tools\explorer_cam_fade_test\mutants.py --self-test holds the mutation list to the sources as they are; --run builds the rig against each edit.
if not exist "%OBJ%\explorercamfade" mkdir "%OBJ%\explorercamfade"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\d3d11" ^
    /Fo"%OBJ%\explorercamfade"\ /Fe"%OBJ%\explorercamfade\explorer_cam_fade_test.exe" ^
    "tools\explorer_cam_fade_test\explorer_cam_fade_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: explorer_cam_fade_test build failed & exit /b 1 )
"%OBJ%\explorercamfade\explorer_cam_fade_test.exe" --dry-run || exit /b 1
"%OBJ%\explorercamfade\explorer_cam_fade_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\explorer_cam_fade_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_menu_edit_hold_test
echo [edvr] === menu_edit_hold_test.exe ===
REM Build gate for the F8 menu's held numeric edits (src\d3d11\menu_edit_hold.h) and the config refresh window line (config_refresh_line.h). Holding a Number row steps
REM it every 83 ms; each step is shown at once, and a burst of steps is one write and so one config refresh on the frame thread (F16 priced the hold at 100 refreshes in
REM five seconds). The rig drives the coalescer with a frame clock and the tracker's repeat: a tap writes once on release, a long hold a bounded few, a row or page
REM switch, a close, a shutdown and any other change write what is held, nothing is written twice, a failed write puts the row back; menu.cpp's wiring and the monitor's
REM use of the formatter are read as text. No device needed.
REM tools\menu_edit_hold_test\mutants.py --self-test holds the mutation list to the sources as they are; --run builds the rig against each edit.
if not exist "%OBJ%\menueditHold" mkdir "%OBJ%\menueditHold"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\d3d11" ^
    /Fo"%OBJ%\menueditHold"\ /Fe"%OBJ%\menueditHold\menu_edit_hold_test.exe" ^
    "tools\menu_edit_hold_test\menu_edit_hold_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: menu_edit_hold_test build failed & exit /b 1 )
"%OBJ%\menueditHold\menu_edit_hold_test.exe" --dry-run || exit /b 1
"%OBJ%\menueditHold\menu_edit_hold_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\menu_edit_hold_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_vertex_resync_test
echo [edvr] === vertex_resync_test.exe ===
REM Build gate for the vertex-buffer resync (src\d3d11\vertex_resync_core.h and vertex_resync_hook.cpp, EDVR_VERTEX_RESYNC_TEST; docs\scanner-body.md, "The root cause:
REM Frontier's f3d state cache"): the game's applied vertex-buffer cache goes stale when a buffer it binds again equals its DESIRED state (SetVertexBuffer compares only that)
REM after zero-slot draws zeroed the applied copy, and the second eye's lighting resolve runs with slot 0 empty. The pure repair runs on a fake command list laid out at the real
REM offsets and replays the failing sequence (stock binds NULL; repaired binds the buffer and its offset; off counts and writes nothing; slots past the layout's count, a null
REM desired slot, more than 16 slots and null pointers are left alone). The production hook is compiled in with the real CodeHook and patches a synthetic function that begins with
REM the real 16-byte prologue of FlushIA: a wrong prologue is refused with one line and nothing patched, the right one arms, the original sees the repaired cache, the live key,
REM the eight first-sighting lines, the 60 s count, the ten-minute heartbeat (zero counts included) and the session line are what the log says, a fault is absorbed and eight
REM stand it down. device_hook.cpp's wiring and the exit line's registration with the log are
REM read as text. tools\vertex_resync_test\mutants.py --self-test holds the mutation list to the sources as they are; --run builds the rig against each edit.
if not exist "%OBJ%\vertexresync" mkdir "%OBJ%\vertexresync"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DEDVR_VERTEX_RESYNC_TEST /I"src\d3d11" /I"src\common" ^
    /Fo"%OBJ%\vertexresync"\ /Fe"%OBJ%\vertexresync\vertex_resync_test.exe" ^
    "tools\vertex_resync_test\vertex_resync_test.cpp" "src\d3d11\vertex_resync_hook.cpp" ^
    "src\common\code_hook.cpp" "src\common\guard.cpp" "src\common\log.cpp" "src\common\config.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: vertex_resync_test build failed & exit /b 1 )
"%OBJ%\vertexresync\vertex_resync_test.exe" --dry-run || exit /b 1
"%OBJ%\vertexresync\vertex_resync_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\vertex_resync_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_scheduler_stack_json_test
echo [edvr] === scheduler_stack_json_test.exe ===
REM Build gate for the production SchedulerStackProbe JSON writer: the
REM kinematic writer shipped three serialization failures before its gate
REM existed, and compilation cannot catch a dropped quote. The exe
REM serializes a deterministic fixture through the real writeJson; the
REM python gate strict-parses it and asserts every value round-trips.
if not exist "%OBJ%\schedjson" mkdir "%OBJ%\schedjson"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\schedprobe\\" /Fe"%BUILD%\scheduler_stack_probe_test.exe" ^
    "tools\scheduler_stack_probe_test\scheduler_stack_probe_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: scheduler stack probe test build failed & exit /b 1 )
"%BUILD%\scheduler_stack_probe_test.exe" --dry-run || exit /b 1
"%BUILD%\scheduler_stack_probe_test.exe" --self-test || exit /b 1
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
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

:rig_ui_layer_coverage_test
echo [edvr] === ui_layer_coverage_test.exe ===
REM Exercise the production coverage command cache and shaders on WARP.
REM Keep the executable outside build so D3D11 comes from System32.
if not exist "%OBJ%\uicoverage" mkdir "%OBJ%\uicoverage"
%CC% /I"%GEN%" /nologo /O2 /Gy /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\uicoverage\\" /Fe"%OBJ%\uicoverage\coverage_test.exe" ^
    "tools\ui_layer_coverage_test\coverage_test.cpp" ^
    /link /INCREMENTAL:NO /OPT:REF d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: UI layer coverage test build failed & exit /b 1 )
"%OBJ%\uicoverage\coverage_test.exe" --dry-run || exit /b 1
"%OBJ%\uicoverage\coverage_test.exe" --self-test || exit /b 1
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
REM same quad drawn into the frame; the 1.25 box filter; the debug view. The
REM after-UI identity (ui_after_ui_test.h, 2026-09-30): the follow through the
REM game's post pass as a truth table; the recorded post-tonemap tails of two
REM field censuses (station services, and the game's menu over it) routed
REM through the real family rule, decide, gate and follow -- no interface draw
REM left under the layer -- and composited stock against layered on the CPU
REM blend model; the known limit pinned; and a scan of src\d3d11\ui_layer.cpp
REM (the rig runs from the repo root) for the order the routing model assumes.
if not exist "%OBJ%\uiqualitytest" mkdir "%OBJ%\uiqualitytest"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\uiqualitytest\\" /Fe"%BUILD%\ui_quality_test.exe" ^
    "tools\ui_quality_test\ui_quality_test.cpp" ^
    /link /INCREMENTAL:NO d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: ui quality test build failed & exit /b 1 )
"%BUILD%\ui_quality_test.exe" --dry-run || exit /b 1
"%BUILD%\ui_quality_test.exe" --self-test || exit /b 1
exit /b 0

REM The AA-off draw path (draw_fast_path.h), the Map timer's sampler and its exact call counts (map_wait.h,
REM map_wait_sample.h), and the GPU timing guard that keeps off-state draw thunks from calling it (gpu_frame_timing.h),
REM 2026-10-13. The Map timer is linked from its own source; the GPU call is a counting stub.
:rig_aa_off_test
echo [edvr] === aa_off_test.exe ===
if not exist "%OBJ%\aaofftest" mkdir "%OBJ%\aaofftest"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\aaofftest\\" /Fe"%BUILD%\aa_off_test.exe" ^
    "tools\aa_off_test\aa_off_test.cpp" "src\d3d11\map_wait.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: aa_off test build failed & exit /b 1 )
"%BUILD%\aa_off_test.exe" --dry-run || exit /b 1
"%BUILD%\aa_off_test.exe" --self-test || exit /b 1
exit /b 0

:rig_ui_layer_seed_test
echo [edvr] === seed_test.exe ===
REM The layer's depth-stencil seed on its own (ui_layer_seed.h): the three
REM depth formats, every stencil value class, a scale, a jitter and a reused
REM seeder, read back texel for texel. Built outside build\ so its imported
REM D3D11CreateDevice resolves to System32's, not EDVR's proxy.
if not exist "%OBJ%\uilayerseed" mkdir "%OBJ%\uilayerseed"
%CC% /I"%GEN%" /nologo /O2 /Gy /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\uilayerseed\\" /Fe"%OBJ%\uilayerseed\seed_test.exe" ^
    "tools\ui_layer_seed_test\seed_test.cpp" ^
    /link /INCREMENTAL:NO /OPT:REF d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 ( echo [edvr] ERROR: ui layer seed test build failed & exit /b 1 )
"%OBJ%\uilayerseed\seed_test.exe" --self-test || exit /b 1
exit /b 0

:rig_ui_layer_world_test
echo [edvr] === ui_layer_world_test.exe ===
REM The VR on-foot world route's layer half (docs\design-flat-temporal-aa-2026-09-23.md, section 82) with
REM src\d3d11\ui_layer.cpp linked WHOLE, on WARP: its neighbours (the binding shadow, the route, the mips module,
REM the raw OM/RS entries, the journal) are stubs in the rig, the layer's own machinery is the production code. The
REM route's mode is never a take; the re-issue draws the screen composite into the eye's layer from the mipped
REM screen through the trilinear sampler (a mip chain of one flat colour a level shows which level the map's
REM minification selects) and the composite of that layer over a frame is the eye; every changed state comes back
REM after a landed re-issue and after every refusal; every refusal is counted by reason and tells the route
REM nothing; the raw entries run with the route's internal scope up. A curved screen's draw (the curve substitution's strip) is
REM planned like a flat one, and a re-issue that did not land takes no eye (held by tools\vr_world_route_test\mutants.py --run).
REM Built outside build\ like the seed rig above.
if not exist "%OBJ%\uilayerworld" mkdir "%OBJ%\uilayerworld"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /Fo"%OBJ%\uilayerworld\\" /Fe"%OBJ%\uilayerworld\ui_layer_world_test.exe" ^
    "tools\ui_layer_world_test\ui_layer_world_test.cpp" "src\d3d11\ui_layer.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" ^
    "third_party\dxbc_hash\DxilHash.cpp" ^
    /link /INCREMENTAL:NO d3dcompiler.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: ui layer world test build failed & exit /b 1 )
"%OBJ%\uilayerworld\ui_layer_world_test.exe" --dry-run || exit /b 1
"%OBJ%\uilayerworld\ui_layer_world_test.exe" --self-test || exit /b 1
exit /b 0

:rig_ui_layer_device_test
echo [edvr] === ui_layer_device_test.exe ===
REM The shared UI layer across a D3D11 device change (review 2026-10-09, P2): src\d3d11\ui_layer.cpp compiled WHOLE (through
REM tools\ui_layer_device_test\ui_layer_device_bridge.cpp, which only exposes which device each retained cache belongs to) with the
REM world rig's neighbour stubs, on two WARP devices. A frame on device A (door, the crisp take's dependencies, a HUD frame through
REM the production coverage pass, the production composite, read back), the production device-change reset (uiLayerDeviceReset),
REM the same frame on device B: every retained child -- blend cache, coverage shaders and deferred context, seeder context,
REM composite shader and parameter buffer, both layers, the composite output -- is B's, the pixels are the HUD over the frame,
REM the caller's bindings come back. Then a same-device resize (uiLayerFlatRelease) at another size: the pixel again, nothing
REM device-bound rebuilt. Built outside build\ like the world rig.
if not exist "%OBJ%\uilayerdevice" mkdir "%OBJ%\uilayerdevice"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /Fo"%OBJ%\uilayerdevice\\" /Fe"%OBJ%\uilayerdevice\ui_layer_device_test.exe" ^
    "tools\ui_layer_device_test\ui_layer_device_test.cpp" "tools\ui_layer_device_test\ui_layer_device_bridge.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" ^
    "third_party\dxbc_hash\DxilHash.cpp" ^
    /link /INCREMENTAL:NO d3dcompiler.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: ui layer device test build failed & exit /b 1 )
"%OBJ%\uilayerdevice\ui_layer_device_test.exe" --dry-run || exit /b 1
"%OBJ%\uilayerdevice\ui_layer_device_test.exe" --self-test || exit /b 1
exit /b 0

:rig_on_foot_maps_test
echo [edvr] === on_foot_maps_test.exe ===
REM The on-foot maps gate (experimental.on_foot_maps_sharp; docs\design-world-camera-motion-2026-09-30.md, Phase 1): the pure
REM half, src\d3d11\ui_maps_math.h, compiled alone -- the key, the step (2 named frames hold the panel as the world, 3 unnamed
REM release it), flight 1's runs replayed (13,044 named, 1,597 unnamed, 98, 929: a hold and four flips), the gate's combine with
REM the key off held to a frozen copy of today's journal-OR-depth for every input, the carry, the door's predicate and the text of
REM every line -- plus the source pins from the repo root (the key-off contract: nothing new is read, counted, logged or issued
REM with the key off; the recognition's two call sites; the one place the naming is told). tools\on_foot_maps_test\mutants.py
REM --self-test holds the mutation list to the header as it is; the list itself (--run, on demand, about a minute) proves the rig
REM fails when each rule is flipped. Pure C++ and source scans: no D3D.
if not exist "%OBJ%\onfootmaps" mkdir "%OBJ%\onfootmaps"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\d3d11" /Fo"%OBJ%\onfootmaps"\ ^
    /Fe"%BUILD%\on_foot_maps_test.exe" "tools\on_foot_maps_test\on_foot_maps_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: on foot maps test build failed & exit /b 1 )
"%BUILD%\on_foot_maps_test.exe" --dry-run || exit /b 1
"%BUILD%\on_foot_maps_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\on_foot_maps_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_ui_composite_census_test
echo [edvr] === ui_composite_census_test.exe ===
REM The cockpit holo panels' second shader pair and the census of the interface composites the layer leaves in the scene (docs\ui-layer-2026-09-23.md,
REM "2026-10-01: Disable GUI effects"): the pure halves, src\d3d11\ui_layer_math.h's family rule (the vertex shader Elite switches in with Disable GUI
REM effects on names the cockpit holo panels, on the lit HDR target and on the post-tonemap one, exactly as the stock one does) and
REM src\d3d11\ui_scene_composites.h's window, zero line, line with pairs, cap and detector-off wording, compiled alone -- plus the source pins from the repo
REM root (where ui_depth publishes the composite flag, the scope that consumes it, the count after both takes, the line once a window with its zeros, depth
REM left out for the new pair on purpose, the eye-run diagnostic's watch list, and the reader's fixture byte for byte; each pin carries a control).
REM tools\ui_composite_census_test\mutants.py --self-test holds the mutation list to the headers as they are; the list itself (--run, on demand, about a
REM minute) proves the rig fails when each rule is flipped. Pure C++ and source scans: no D3D.
if not exist "%OBJ%\uicompositecensus" mkdir "%OBJ%\uicompositecensus"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\d3d11" /Fo"%OBJ%\uicompositecensus"\ ^
    /Fe"%BUILD%\ui_composite_census_test.exe" "tools\ui_composite_census_test\ui_composite_census_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: ui composite census test build failed & exit /b 1 )
"%BUILD%\ui_composite_census_test.exe" --dry-run || exit /b 1
"%BUILD%\ui_composite_census_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\ui_composite_census_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_flat_map_bounce_test
 echo [edvr] === flat_map_bounce_test.exe ===
 REM Pure cached-map core and fake driver: seeded programs, C1-C8, no D3D device.
 if not exist "%OBJ%\flatmapbounce" mkdir "%OBJ%\flatmapbounce"
 %CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /I"src\d3d11" ^
     /Fo"%OBJ%\flatmapbounce\\" /Fe"%OBJ%\flatmapbounce\flat_map_bounce_test.exe" ^
     "tools\flat_map_bounce_test\flat_map_bounce_test.cpp" /link /INCREMENTAL:NO
 if errorlevel 1 ( echo [edvr] ERROR: flat map bounce test build failed & exit /b 1 )
 "%OBJ%\flatmapbounce\flat_map_bounce_test.exe" --dry-run || exit /b 1
 "%OBJ%\flatmapbounce\flat_map_bounce_test.exe" --self-test "%ROOT%" || exit /b 1
 python "tools\flat_map_bounce_test\mutants.py" --self-test || exit /b 1
 exit /b 0

:rig_particle_capture_test
echo [edvr] === particle_capture_test.exe ===
REM The particle billboards' constants capture and the Unmap readers probe (docs\particle-billboards.md, 2026-10-16), pure halves compiled alone:
REM src\d3d11\particle_capture_core.h (the shadow keeps rows 270..279 of the mapped scene block, 160 bytes, not 5376; the target's retirement
REM after 300 frames without a plume or flare, and its re-arm) and src\d3d11\unmap_read_probe.h (calls, bytes, sampled microseconds, "not
REM exercised"). A counting memcpy shim prices the copy before (the legacy whole-block copy) and after, over every mapped size, with the
REM rows the fix reads checked byte for byte; the source pins from the repo root hold the wiring (each reader timed once, the boundary
REM called once). Pure C++: no D3D.
if not exist "%OBJ%\particlecapture" mkdir "%OBJ%\particlecapture"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\d3d11" /Fo"%OBJ%\particlecapture"\ ^
    /Fe"%OBJ%\particlecapture\particle_capture_test.exe" "tools\particle_capture_test\particle_capture_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: particle capture test build failed & exit /b 1 )
"%OBJ%\particlecapture\particle_capture_test.exe" --dry-run || exit /b 1
"%OBJ%\particlecapture\particle_capture_test.exe" --self-test "%ROOT%" || exit /b 1
exit /b 0

:rig_billboard_rollfree_test
echo [edvr] === billboard_rollfree_test.exe ===
REM The roll-free scene block for the camera-facing billboards (src\d3d11\billboard_rollfree.h; docs\billboard-root-cause-2026-10-16.md) on a WARP
REM device with the D3D debug layer: the basis for any roll (levelled right, the game's lengths and handedness, refused near straight up), the copy
REM route (CopyResource, a 32-byte UpdateSubresource of a patch buffer, a partial CopySubresourceRegion into the constant-buffer copy) clean under
REM the debug layer, every byte but rows 277/278 the game's, a real draw whose vertex stage reads the roll-free rows while the pixel stage still reads
REM the game's block, the unchanged path (the old code) showing the roll, state restored, one copy per write however many draws follow, stock doing
REM nothing, several blocks, and the source pins from the repo root. Output under obj\ and System32's d3d11 (no d3d11.dll beside the exe).
if not exist "%OBJ%\billboardrollfree" mkdir "%OBJ%\billboardrollfree"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"src\d3d11" ^
    /Fo"%OBJ%\billboardrollfree\\" /Fe"%OBJ%\billboardrollfree\billboard_rollfree_test.exe" ^
    "tools\billboard_rollfree_test\billboard_rollfree_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 ( echo [edvr] ERROR: billboard rollfree test build failed & exit /b 1 )
"%OBJ%\billboardrollfree\billboard_rollfree_test.exe" --dry-run || exit /b 1
"%OBJ%\billboardrollfree\billboard_rollfree_test.exe" --self-test "%ROOT%" || exit /b 1
exit /b 0

:rig_billboard_facing_test
echo [edvr] === billboard_facing_test.exe ===
REM The explosion billboards' facing transcriptions (src\d3d11\billboard_facing_vs.h; docs\particle-billboards.md, 2026-10-16 "facing") against the game's own
REM bytecode (tools\billboard_facing_test\fixtures, held to their hash names) on a WARP device with the D3D debug layer: signature blocks and every dcl line
REM equal column for column (ours the one added b3), then the original and the transcription run on the same vertices through stream output and every
REM output register is compared -- identical with the viewer feed absent, identical in the aligned modes, identical on axis with zero roll, and off axis
REM only the basis-dependent outputs differ while each quad faces the viewer and the original's does not. Output under obj\ and System32's d3d11.
if not exist "%OBJ%\billboardfacing" mkdir "%OBJ%\billboardfacing"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"src\d3d11" ^
    /Fo"%OBJ%\billboardfacing\\" /Fe"%OBJ%\billboardfacing\billboard_facing_test.exe" ^
    "tools\billboard_facing_test\billboard_facing_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 ( echo [edvr] ERROR: billboard facing test build failed & exit /b 1 )
"%OBJ%\billboardfacing\billboard_facing_test.exe" --dry-run || exit /b 1
"%OBJ%\billboardfacing\billboard_facing_test.exe" --self-test "%ROOT%" || exit /b 1
exit /b 0

:rig_panel_curve_test
echo [edvr] === panel_curve_test.exe ===
REM The curved screen's strip (src\d3d11\panel_curve.cpp; fix.panel_curvature) compiled for real with the real Config, Log and fault
REM guard, and stubs for what it calls (the binding shadow's resolver, the screen's motion pass): on a WARP device with the game's
REM state bound (the canonical quad and its index pattern at their offsets, the SIZE record, the placement constants, a sampler,
REM the rasterizer state) and a recording draw function, the substitution and the VR world route's re-issue of the same strip are
REM compared draw for draw: the arguments, the input assembler state at the draw, the buffer bytes against an independent bend,
REM the motion pass's curve, the game's state put back (canonical, odd and empty), a live config change, a table of curvature by
REM columns by sign by gain, the one-column identity strip against the game's quad, the SIZE read after its 50 ms lag (the clock
REM is stepped, not slept), a faulting draw standing the feature down, panelCurveInfo() and the shutdown, and the rest of the
REM pipeline left alone. tools\panel_curve_test\mutants.py --self-test holds the mutation list to the sources as they are; the
REM list itself (--run, on demand, about a minute) proves the rig fails when each rule of the module is flipped. Built under obj\
REM and taking System32's device through src\common\system_d3d11.h, so it links without d3d11.lib.
if not exist "%OBJ%\panelcurve" mkdir "%OBJ%\panelcurve"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"%GEN%" /I"src\d3d11" ^
    /Fo"%OBJ%\panelcurve\\" /Fe"%OBJ%\panelcurve\panel_curve_test.exe" ^
    "tools\panel_curve_test\panel_curve_test.cpp" "src\d3d11\panel_curve.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: panel_curve_test build failed & exit /b 1 )
"%OBJ%\panelcurve\panel_curve_test.exe" --dry-run || exit /b 1
"%OBJ%\panelcurve\panel_curve_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\panel_curve_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_intro_curve_test
echo [edvr] === intro_curve_test.exe ===
REM src\d3d11\intro_panel.cpp for real (movie world lock, splash refusal, screen-space test, config, retirement) on WARP, goldens bit for bit;
REM one process per scenario because the module's latches cannot be reset. tools\intro_curve_test\mutants.py --self-test holds the mutation list to the module.
if not exist "%OBJ%\introcurve" mkdir "%OBJ%\introcurve"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"%GEN%" /I"src\d3d11" ^
    /Fo"%OBJ%\introcurve\\" /Fe"%OBJ%\introcurve\intro_curve_test.exe" ^
    "tools\intro_curve_test\intro_curve_test.cpp" "src\d3d11\intro_panel.cpp" "src\d3d11\panel_curve.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: intro_curve_test build failed & exit /b 1 )
"%OBJ%\introcurve\intro_curve_test.exe" --dry-run || exit /b 1
"%OBJ%\introcurve\intro_curve_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\intro_curve_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_intro_curve_math_test
echo [edvr] === intro_curve_math_test.exe ===
REM The intro composite's constants read as a world-space panel (src\d3d11\intro_curve_math.h; docs\shaders\intro-composite-vs.asm): the
REM pure half of the intro movie and the splash following fix.panel_curvature, compiled alone. Today's screen-space rule
REM (introCbLooksScreenSpace) against a verbatim copy of intro_panel.cpp's looksScreenSpace over every boundary and the float either
REM side of it, one slot, two slots and a deterministic sample of whole vectors at a time, and on a table of hand-derived answers; the
REM world-space reading (introReadWorldCb) on the game's two real splash captures (the panel in front, and behind after a 180-degree
REM yaw: ok, half-width 4.44444, toward +1) and the movie's stock constants (screen space, refused), every refusal in its own words
REM and in its order, NaN and infinity in each of the 20 slots, the length, w and half-size ranges at their edges, and the depth
REM direction by the signs of cb2[3].w and cb2[4].w. tools\intro_curve_math_test\mutants.py --self-test holds the mutation list to the
REM header as it is; the list itself (--run, on demand, about a minute) proves the rig fails when each rule is flipped. Pure C++: no D3D.
if not exist "%OBJ%\introcurvemath" mkdir "%OBJ%\introcurvemath"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
    /D_CRT_SECURE_NO_WARNINGS /I"src\d3d11" /Fo"%OBJ%\introcurvemath"\ ^
    /Fe"%BUILD%\intro_curve_math_test.exe" "tools\intro_curve_math_test\intro_curve_math_test.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib
if errorlevel 1 ( echo [edvr] ERROR: intro curve math test build failed & exit /b 1 )
"%BUILD%\intro_curve_math_test.exe" --dry-run || exit /b 1
"%BUILD%\intro_curve_math_test.exe" --self-test || exit /b 1
python "tools\intro_curve_math_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_intro_curve_module_test
echo [edvr] === intro_curve_module_test.exe ===
REM The splash's recogniser (src\d3d11\intro_curve.cpp; fix.panel_curvature, the game-placed composite the movie's vertex shader draws)
REM compiled for real with the real Config, Log, fault guard and the real strip (panel_curve.cpp's surface API, drawing through a
REM recording draw function), on a WARP device with the game's state bound: 80-byte constant buffers at VS slot 2 holding the game's
REM two real splash captures and the movie's stock constants, decoys in the slots round them, the sampled surface in the binding
REM shadow. A pair is learned by one copy of its 80 bytes read four ticks later: unknown through the settle frames, then armed with
REM the half-width and direction the capture gives (4.44444, toward the viewer) or left as the game drew it with the reason and the
REM 20 floats logged; a new surface or buffer is learned afresh; at curvature 0 nothing is created, copied or logged; only the
REM composite's shape is recognised; sixteen pairs and the least recently drawn pushed out; unseen for 180 frames forgotten; a flat pair copied again every 60;
REM the first rendered scene retires it for good; a fault in the draw or the readback stands only this module down; every reference
REM taken is given back (the game's buffer, the device, the readback buffers); both eyes of the splash share one strip. The module's
REM process-wide state is reset between cases through EDVR_INTRO_CURVE_RIG's two doors. tools\intro_curve_module_test\mutants.py
REM --self-test holds the mutation list to the sources as they are; the list itself (--run, on demand, about a minute) proves the rig
REM fails when each rule of the module is flipped. Built under obj\ and taking System32's device through src\common\system_d3d11.h,
REM so it links without d3d11.lib.
if not exist "%OBJ%\introcurvemodule" mkdir "%OBJ%\introcurvemodule"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DEDVR_INTRO_CURVE_RIG ^
    /DINTRO_CURVE_RIG_REAL_STRIP /I"%GEN%" /I"src\d3d11" ^
    /Fo"%OBJ%\introcurvemodule\\" /Fe"%OBJ%\introcurvemodule\intro_curve_module_test.exe" ^
    "tools\intro_curve_module_test\intro_curve_module_test.cpp" "src\d3d11\intro_curve.cpp" "src\d3d11\panel_curve.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: intro_curve_module_test build failed & exit /b 1 )
"%OBJ%\introcurvemodule\intro_curve_module_test.exe" --dry-run || exit /b 1
"%OBJ%\introcurvemodule\intro_curve_module_test.exe" --self-test || exit /b 1
python "tools\intro_curve_module_test\mutants.py" --self-test || exit /b 1
exit /b 0

:rig_surface_strip_render_test
echo [edvr] === surface_strip_render_test.exe ===
REM Which way the curved intro picture faces, looked at in pixels (the surface strip of src\d3d11\panel_curve.cpp; fix.panel_curvature). The
REM production strip, the movie's wiring (intro_panel.cpp) and the splash's (intro_curve.cpp) are compiled for real with the real Config, Log
REM and fault guard and drawn on a WARP device through a vertex shader that is the intro composite's own (docs\shaders\intro-composite-vs.asm,
REM written out as HLSL in the rig), a point-sampling pixel shader and the game's rasterizer state (cull back) onto a 128x72 target, from a
REM texture whose corners are red, green, blue and white: left stays left and up stays up for the on-foot screen, the movie and the splash
REM (the first flight showed the movie and the splash mirrored: the composite's +x runs to the viewer's left and the strip's u ran with x).
REM The same draws with the other flag come out mirrored (the controls that prove the rig can see it), and a placement whose +x runs right
REM comes out upright with the flag the other way. tools\surface_strip_render_test\mutants.py --self-test holds the mutation list to the
REM sources as they are; the list itself (--run, on demand) proves the rig fails when each rule that decides the direction is flipped.
REM Built under obj\ and taking System32's device through src\common\system_d3d11.h, so it links without d3d11.lib.
if not exist "%OBJ%\surfacestrip" mkdir "%OBJ%\surfacestrip"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"%GEN%" /I"src\d3d11" ^
    /Fo"%OBJ%\surfacestrip\\" /Fe"%OBJ%\surfacestrip\surface_strip_render_test.exe" ^
    "tools\surface_strip_render_test\surface_strip_render_test.cpp" "src\d3d11\intro_panel.cpp" "src\d3d11\intro_curve.cpp" "src\d3d11\panel_curve.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO user32.lib version.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: surface_strip_render_test build failed & exit /b 1 )
"%OBJ%\surfacestrip\surface_strip_render_test.exe" --dry-run || exit /b 1
"%OBJ%\surfacestrip\surface_strip_render_test.exe" --self-test "%ROOT%" || exit /b 1
python "tools\surface_strip_render_test\mutants.py" --self-test || exit /b 1
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
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\pixelprobe\\" /Fe"%OBJ%\pixelprobe\pixel_probe_test.exe" ^
    "tools\pixel_probe_test\pixel_probe_test.cpp" "src\d3d11\pixel_probe.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: pixel probe test build failed & exit /b 1 )
"%OBJ%\pixelprobe\pixel_probe_test.exe" --dry-run || exit /b 1
"%OBJ%\pixelprobe\pixel_probe_test.exe" --self-test || exit /b 1
exit /b 0

:rig_ui_holo_test
echo [edvr] === ui_holo_test.exe ===
REM Actual game DXBC coordinate remap, production cache/binding faults and stock stencil replay.
if not exist "%OBJ%\uiholo" mkdir "%OBJ%\uiholo"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /utf-8 ^
    /Fo"%OBJ%\uiholo\\" /Fe"%OBJ%\uiholo\ui_holo_test.exe" ^
    "tools\ui_holo_test\ui_holo_test.cpp" "src\common\guard.cpp" ^
    "third_party\dxbc_hash\DxilHash.cpp" ^
    /link /INCREMENTAL:NO d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: UI hologram test build failed & exit /b 1 )
"%OBJ%\uiholo\ui_holo_test.exe" --dry-run || exit /b 1
"%OBJ%\uiholo\ui_holo_test.exe" --self-test || exit /b 1
exit /b 0

:rig_vr_ssaa_hold_test
echo [edvr] === vr_ssaa_hold_test.exe ===
REM The VR Supersampling hold's pure decision (src\d3d11\vr_ssaa_hold_math.h, docs\vr-supersampling-gate-2026-10-10.md): the Settings.xml
REM mode parse, the held value by profile and mode, the toast, and the game's mode-request window rewrite by profile (the production
REM requestReplacement and installOne, vr_window_trampolines.cpp included whole; the DLL state is stubbed, nothing is hooked).
if not exist "%OBJ%\vrssaahold" mkdir "%OBJ%\vrssaahold"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\vrssaahold\\" /Fe"%OBJ%\vrssaahold\vr_ssaa_hold_test.exe" ^
    "tools\vr_ssaa_hold_test\vr_ssaa_hold_test.cpp" ^
    "src\common\log.cpp" "src\common\config.cpp" "src\common\proxy.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib
if errorlevel 1 ( echo [edvr] ERROR: vr_ssaa_hold_test build failed & exit /b 1 )
"%OBJ%\vrssaahold\vr_ssaa_hold_test.exe" --dry-run || exit /b 1
"%OBJ%\vrssaahold\vr_ssaa_hold_test.exe" --self-test || exit /b 1
exit /b 0

:rig_runtime_profile_test
echo [edvr] === runtime_profile_test.exe ===
REM The launch decision's pure rules (src\common\install_scope.h, docs\design-unified-install-2026-10-12.md section 3.1): the descriptor
REM parse for schema 1 and 2, the frozen v1 parser an older DLL runs, Settings.xml's 3D mode, the command line's switch, the profile by
REM scope and mode, and the launch's log line. No Windows API: the rig includes only the two pure headers.
if not exist "%OBJ%\runtimeprofile" mkdir "%OBJ%\runtimeprofile"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\runtimeprofile\\" /Fe"%OBJ%\runtimeprofile\runtime_profile_test.exe" ^
    "tools\runtime_profile_test\runtime_profile_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: runtime_profile_test build failed & exit /b 1 )
"%OBJ%\runtimeprofile\runtime_profile_test.exe" --dry-run || exit /b 1
"%OBJ%\runtimeprofile\runtime_profile_test.exe" --self-test || exit /b 1
exit /b 0

:rig_mode_watch_test
echo [edvr] === mode_watch_test.exe ===
REM The mode watch's decisions (src\d3d11\mode_watch_logic.h, docs\design-unified-install-2026-10-12.md section 3.2): the 250 ms
REM confirm, half-written reads ignored, a flip-flop dropped, the live rise, the toggles in order and the unknown launch. No Windows
REM API: the rig includes only the pure headers.
if not exist "%OBJ%\modewatch" mkdir "%OBJ%\modewatch"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\modewatch\\" /Fe"%OBJ%\modewatch\mode_watch_test.exe" ^
    "tools\mode_watch_test\mode_watch_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: mode_watch_test build failed & exit /b 1 )
"%OBJ%\modewatch\mode_watch_test.exe" --dry-run || exit /b 1
"%OBJ%\modewatch\mode_watch_test.exe" --self-test || exit /b 1
exit /b 0

:rig_cross_profile_test
echo [edvr] === cross_profile_test.exe ===
REM The cross-profile cost instrument's arithmetic and sampling (src\d3d11\cross_profile_cost.h, docs\design-unified-install-2026-10-12.md section
REM 3.3): the 1-in-64 scaling, the per-frame division, the capture scope, and the report's 30 s windows and 5-minute session totals with the
REM profile gate. Pure: the header's only Windows call is QueryPerformanceCounter.
if not exist "%OBJ%\crossprofile" mkdir "%OBJ%\crossprofile"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\crossprofile\\" /Fe"%OBJ%\crossprofile\cross_profile_test.exe" ^
    "tools\cross_profile_test\cross_profile_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: cross_profile_test build failed & exit /b 1 )
"%OBJ%\crossprofile\cross_profile_test.exe" --dry-run || exit /b 1
"%OBJ%\crossprofile\cross_profile_test.exe" --self-test || exit /b 1
exit /b 0

:rig_flat_clear_test
echo [edvr] === flat_clear_test.exe ===
REM The headset while flat's policy (src\openxr\flat_clear_state.h, docs\design-unified-install-2026-10-12.md section 12): the 250 ms quiet
REM window at its edges, the four gates, the start and stop transitions with their reasons, and the word the graphics half writes that
REM reads as VR when nobody has written it. No Windows API: the rig includes only the pure headers.
if not exist "%OBJ%\flatclear" mkdir "%OBJ%\flatclear"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%OBJ%\flatclear\\" /Fe"%OBJ%\flatclear\flat_clear_test.exe" ^
    "tools\flat_clear_test\flat_clear_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: flat_clear_test build failed & exit /b 1 )
"%OBJ%\flatclear\flat_clear_test.exe" --dry-run || exit /b 1
"%OBJ%\flatclear\flat_clear_test.exe" --self-test || exit /b 1
exit /b 0

:rig_orbital_width_test
echo [edvr] === orbital_width_test.exe ===
REM fix.ui_quality's orbit lines (src\d3d11\orbital_width*.h; docs\ui-layer-2026-09-23.md, "2026-10-06: the orbit lines"): the game's own
REM vertex shader (vs C7FA0C0F5DD49180, a fixture) and the production patched copy run on WARP over a captured draw's constants and
REM vertex streams -- byte-identical at f = 1, every strip 2 x w x f pixels wide at 0.5, 0.4, 0.7 and 0.25 -- with the coverage twin,
REM the cache, the binding (and its failed restore), the constants, the half-width read-back and the frame boundary's decision, the
REM mutants (the factor not applied, applied twice, the twin left alone) caught by the same comparators, and the draw hook's source pins
REM with their controls (--wiring runs those alone; --hardware-self-test runs the lot on the machine's own adapter, which is not
REM a gate). Built outside build\ and without d3d11.lib, like the UI hologram rig.
if not exist "%OBJ%\orbitalwidth" mkdir "%OBJ%\orbitalwidth"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /utf-8 ^
    /Fo"%OBJ%\orbitalwidth\\" /Fe"%OBJ%\orbitalwidth\orbital_width_test.exe" ^
    "tools\orbital_width_test\orbital_width_test.cpp" "src\common\guard.cpp" ^
    "third_party\dxbc_hash\DxilHash.cpp" ^
    /link /INCREMENTAL:NO d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: orbital width test build failed & exit /b 1 )
"%OBJ%\orbitalwidth\orbital_width_test.exe" --dry-run || exit /b 1
"%OBJ%\orbitalwidth\orbital_width_test.exe" --self-test || exit /b 1
exit /b 0

:rig_supercruise_bars_test
echo [edvr] === supercruise_bars_test.exe ===
REM fix.ui_quality's supercruise bars in the HDR layer (src\d3d11\supercruise_bars*.h, supercruise_bars.cpp; docs\ui-layer-2026-09-23.md, "2026-10-07"):
REM the build's own bytecode of the strip geometry shader (kSupercruiseBarsGsBytecode, compiled from supercruise_bars_shader.h's HLSL by
REM tools\temporal_shader_build and checked here to be that text's compile), the production binding, constants and rasterizer states, run on WARP in
REM front of HLSL stand-ins for the game's vs A47A3315FFF5E2E4 / ps 869FFF43E875906E that disassemble to the instruction lists of the game's own
REM (tools\supercruise_bars_test\fixtures): the tent at 0/30/45/89.9 degrees and three half-widths (the analytic tent, the width at half maximum, the area), the cut
REM at the camera plane (a segment crossing w = 0, both ends behind, no length, one end at the cut), three viewports, perspective-correct attributes,
REM seventy segments in a draw, the binding's in-and-out with the game's state exactly back (and the restore that fails, the settle, the state the game rebound),
REM a game geometry shader refused, every cull mode and winding and the scissor, the geometry stage empty after the issue and a plain issue a hardware line
REM again; the constants and states; and the mutants (the cut, the viewport axes, the perpendicular, the alpha edge, the half-width; the binding that keeps the
REM game's raster state and the one whose restore does nothing) each caught by the same cases. --wiring scans the draw hook's source with controls.
REM Built outside build\ and without d3d11.lib, like the orbit-line rig: its device is System32's.
if not exist "%OBJ%\supercruisebars" mkdir "%OBJ%\supercruisebars"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /utf-8 ^
    /Fo"%OBJ%\supercruisebars\\" /Fe"%OBJ%\supercruisebars\supercruise_bars_test.exe" ^
    "tools\supercruise_bars_test\supercruise_bars_test.cpp" "src\common\guard.cpp" ^
    /link /INCREMENTAL:NO d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: supercruise bars test build failed & exit /b 1 )
"%OBJ%\supercruisebars\supercruise_bars_test.exe" --dry-run || exit /b 1
"%OBJ%\supercruisebars\supercruise_bars_test.exe" --self-test || exit /b 1
exit /b 0

:rig_orbit_layer_test
echo [edvr] === orbit_layer_test.exe ===
REM fix.ui_quality's orbit lines in the HDR layer on the real dump (docs\ui-layer-2026-09-23.md, "2026-10-07"): the game's own vertex shader through the
REM production patched copy, cache, constants and binding (src\d3d11\orbital_width_patch.h), the production depth-stencil seed (ui_layer_seed.h) and the
REM production map and sizes (ui_layer_math.h, ui_sizing_math.h), on WARP, over one captured draw and the crop of the eye's own depth and stencil
REM beneath it (tools\orbit_layer_test\fixtures, made by tools\layer_fixtures.py): the strip 2w LAYER pixels wide (4.000), the seed the game's crop
REM texel for texel, the draw's tests the oracle's pixel for pixel (0 lit where stencil & 0x81 != 1 or the scene is nearer; thousands of pixels the
REM unobstructed line would light on the cockpit and behind the nearer scene, none in the layer), the lit fraction against the game's own 2016 draw
REM over stencil 5 at least 0.98, a line behind a nearer scene rejected and one in front accepted (a constant and a split depth), and the mutants (the
REM factor not applied, applied twice, the seed skipped, the stencil mask 0x01 and 0x80 and the reference 0, the depth test off, the seed a texel
REM away, the seed's mask without bit 7) each caught by the comparator that passes the real thing (--hardware-self-test runs the lot on the machine's own
REM adapter, which is not a gate). Built outside build\ and without d3d11.lib: its device is System32's.
if not exist "%OBJ%\orbitlayer" mkdir "%OBJ%\orbitlayer"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /utf-8 ^
    /Fo"%OBJ%\orbitlayer\\" /Fe"%OBJ%\orbitlayer\orbit_layer_test.exe" ^
    "tools\orbit_layer_test\orbit_layer_test.cpp" "src\common\guard.cpp" ^
    "third_party\dxbc_hash\DxilHash.cpp" ^
    /link /INCREMENTAL:NO d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: orbit layer test build failed & exit /b 1 )
"%OBJ%\orbitlayer\orbit_layer_test.exe" --dry-run || exit /b 1
"%OBJ%\orbitlayer\orbit_layer_test.exe" --self-test || exit /b 1
exit /b 0

:rig_supercruise_census_test
echo [edvr] === supercruise_census_test.exe ===
REM The supercruise draws in the HDR layer on the real census and the real dump (docs\ui-layer-2026-09-23.md, "2026-10-07"), pure C++ on
REM src\d3d11\ui_layer_math.h: frame 1 of the 2026-10-07 05:48:01 census (tools\supercruise_census_test\fixtures\census_054801_f1.txt, 244 draw rows)
REM turned into the facts the DLL gathers -- the space dust's rows named and redirected, one-token mutants refused (the flat route's pixel shader, blend MIN,
REM an 8-bit target, a depth state the layer cannot reproduce, a late tonemap) -- a differential over every row against the rule on main (exactly the six
REM supercruise rows change) and against the orbit lines and bars alone (only the two dust rows), the layer's depth-stencil seed rules replayed over the
REM frame (the seeds a frame needs before and after, printed), and the dust pixels of the dump (all 2043 on sky, about 1% within one render pixel of an
REM occluder, the parity bound's background at most 8 of 255). A scan holds this rig, the orbit-layer rig, the bars rig and the fixtures tool in this file.
if not exist "%OBJ%\supercruisecensus" mkdir "%OBJ%\supercruisecensus"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /utf-8 ^
    /Fo"%OBJ%\supercruisecensus\\" /Fe"%OBJ%\supercruisecensus\supercruise_census_test.exe" ^
    "tools\supercruise_census_test\supercruise_census_test.cpp" ^
    /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: supercruise census test build failed & exit /b 1 )
"%OBJ%\supercruisecensus\supercruise_census_test.exe" --dry-run || exit /b 1
"%OBJ%\supercruisecensus\supercruise_census_test.exe" --self-test || exit /b 1
exit /b 0

:rig_sharp_weapons_test
echo [edvr] === sharp_weapons_test.exe ===
REM fix.sharp_weapons (docs\dlss-blur-elements-2026-10-14.md, "Built, 2026-10-16"): the weapons-fire family rule and its one-token mutants, the jitter-cancel table,
REM the two keys' independence over the four combinations (the family gate, the decisions, the layer's two halves), the tonemap re-issue plan with its mutants,
REM three recorded census frames of the 2026-10-16 flight (tools\sharp_weapons_test\fixtures: exactly the weapon rows change decision), the source scans of the
REM wiring with a control for each, and -- on WARP, with the debug layer -- the production source pass, blend states and step bindings driven through a synthetic
REM game tonemap: the tonemap-difference composite equals T(F + L) over a lit background where the plain add is far off, and over black the two agree, with
REM mutants of the sequence caught. Built outside build\ and without d3d11.lib's build dir: its device is System32's. Runs at the repo root (the scans read sources).
if not exist "%OBJ%\sharpweapons" mkdir "%OBJ%\sharpweapons"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /utf-8 ^
    /Fo"%OBJ%\sharpweapons\\" /Fe"%OBJ%\sharpweapons\sharp_weapons_test.exe" ^
    "tools\sharp_weapons_test\sharp_weapons_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: sharp weapons test build failed & exit /b 1 )
"%OBJ%\sharpweapons\sharp_weapons_test.exe" --dry-run || exit /b 1
"%OBJ%\sharpweapons\sharp_weapons_test.exe" --self-test || exit /b 1
exit /b 0

:rig_flat_weapon_admission_test
echo [edvr] === flat_weapon_admission_test.exe ===
REM The complete flat adapter and UI layer on WARP: frame N proves the tone target, frame N+1 takes an actual weapon draw,
REM the named tone route re-issues its on/plain plan, and the flat output copy binds a nonempty published composite.
REM Includes HUD-quality-off and HUD-free quality-on cases plus stale/alien target refusals. Its System32 D3D11 device
REM lives beside its own objects, never beside build\d3d11.dll.
if not exist "%OBJ%\flatweaponadmit" mkdir "%OBJ%\flatweaponadmit"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /utf-8 ^
    /Fo"%OBJ%\flatweaponadmit\\" /Fe"%OBJ%\flatweaponadmit\flat_weapon_admission_test.exe" ^
    "tools\sharp_weapons_test\flat_weapon_admission_test.cpp" "tools\sharp_weapons_test\flat_weapon_bridge.cpp" ^
    "src\d3d11\flat_ui_layer.cpp" "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" ^
    "third_party\dxbc_hash\DxilHash.cpp" ^
    /link /INCREMENTAL:NO d3dcompiler.lib user32.lib
if errorlevel 1 ( echo [edvr] ERROR: flat weapon admission test build failed & exit /b 1 )
"%OBJ%\flatweaponadmit\flat_weapon_admission_test.exe" --dry-run || exit /b 1
"%OBJ%\flatweaponadmit\flat_weapon_admission_test.exe" --self-test || exit /b 1
exit /b 0

:rig_shield_ripple_test
echo [edvr] === shield_ripple_test.exe ===
REM advanced.shield_ripple (docs\dlss-blur-elements-2026-10-14.md, "Built, 2026-10-16: the shield refraction after the upscale"): the pair's family rule, the modes, the
REM first-draw checks' arithmetic; on WARP with the debug layer, the game's own ps_6EAA (tools\shield_ripple_test\fixtures, from edvr_logs\shaders) drawn on the UV ramp
REM against the analytic uv + d within 1e-6; the whole route with the production remap, jitter cancel and door pass (the build's bytecode) against the stock path
REM within 2/255, with seven mutants caught; the coverage instrument's method with the production marking pass; and the source scans of the wiring, each with a control.
REM Built outside build\ so its device is System32's; runs at the repo root (the scans read sources).
if not exist "%OBJ%\shieldripple" mkdir "%OBJ%\shieldripple"
%CC% /I"%GEN%" /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
    /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /utf-8 ^
    /Fo"%OBJ%\shieldripple\\" /Fe"%OBJ%\shieldripple\shield_ripple_test.exe" ^
    "tools\shield_ripple_test\shield_ripple_test.cpp" ^
    /link /INCREMENTAL:NO d3d11.lib d3dcompiler.lib
if errorlevel 1 ( echo [edvr] ERROR: shield ripple test build failed & exit /b 1 )
"%OBJ%\shieldripple\shield_ripple_test.exe" --dry-run || exit /b 1
"%OBJ%\shieldripple\shield_ripple_test.exe" --self-test || exit /b 1
exit /b 0

:rig_menu_pad_test
echo [edvr] === menu_pad_test.exe ===
if not exist "%OBJ%\menupad" mkdir "%OBJ%\menupad"
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 ^
/Fo"%OBJ%\menupad"\ /Fe"%BUILD%\menu_pad_test.exe" ^
"tools\menu_pad_test\menu_pad_test.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: menu_pad_test build failed & exit /b 1 )
"%BUILD%\menu_pad_test.exe" --dry-run || exit /b 1
"%BUILD%\menu_pad_test.exe" --self-test || exit /b 1
exit /b 0

:rig_addon_manager_test
echo [edvr] === addon_manager_test.exe ===
if not exist "%OBJ%\addon_manager_test" mkdir "%OBJ%\addon_manager_test"
%CC% /nologo /O2 /MT /LD /std:c++17 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS ^
    /I"include" /Fo"%OBJ%\addon_manager_test\\" /Fe"%BUILD%\test_addon_fixture.dll" ^
    "tools\addon_manager_test\fixture.cpp" /link /INCREMENTAL:NO
if errorlevel 1 ( echo [edvr] ERROR: addon fixture build failed & exit /b 1 )
REM The fixture's variants are the same source with one define each; every one is a DLL of its own.
for %%V in (b regfault initfail initfault settings renderfault threadreg markshutdown) do (
    %CC% /nologo /O2 /MT /LD /std:c++17 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /DFIXTURE_VARIANT_%%V ^
        /I"include" /Fo"%OBJ%\addon_manager_test\fixture_%%V.obj" /Fe"%BUILD%\test_addon_fixture_%%V.dll" ^
        "tools\addon_manager_test\fixture.cpp" /link /INCREMENTAL:NO
    if errorlevel 1 ( echo [edvr] ERROR: addon fixture variant %%V build failed & exit /b 1 )
)
%CC% /nologo /O2 /MT /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
    /I"include" /I"%GEN%" /Fo"%OBJ%\addon_manager_test\\" /Fe"%BUILD%\addon_manager_test.exe" ^
    "tools\addon_manager_test\addon_manager_test.cpp" "src\plugins\plugin_manager.cpp" ^
    "src\common\config.cpp" "src\common\log.cpp" "src\common\guard.cpp" "src\common\frame_flag.cpp" ^
    /link /INCREMENTAL:NO kernel32.lib user32.lib version.lib bcrypt.lib
if errorlevel 1 ( echo [edvr] ERROR: addon_manager_test build failed & exit /b 1 )
"%BUILD%\addon_manager_test.exe" --dry-run || exit /b 1
"%BUILD%\addon_manager_test.exe" --self-test || exit /b 1
exit /b 0

