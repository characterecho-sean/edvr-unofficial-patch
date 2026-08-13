@echo off
setlocal enabledelayedexpansion
REM ===========================================================================
REM  EDVR release packaging
REM
REM  Produces dist\edvr-<version>.zip containing only what an end user needs:
REM  the d3d11 proxy, the config, a plain-text readme and the licence.
REM
REM  The zip ships the repository's own edvr.ini rather than a separate release
REM  copy, so there is nothing to drift out of sync with what is documented.
REM
REM  Usage:  package.bat <version>
REM ===========================================================================

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "VER=%~1"
if "%VER%"=="" (
    echo [edvr] usage: package.bat ^<version^>   e.g. package.bat 0.2.0
    exit /b 1
)
set "STAGE=%ROOT%\dist\edvr-%VER%"
set "ZIP=%ROOT%\dist\edvr-%VER%.zip"

REM Build, then test, then package. In that order, always.
REM
REM This used to check only that build\d3d11.dll existed. A stale binary from an
REM earlier source revision would be zipped alongside today's ini and README, and
REM smoke.exe -- built by build.bat, and named in its closing message as the way
REM to check a build -- was invoked by nothing at all. It was entirely possible
REM to ship a DLL that would have failed its own test.
echo [edvr] building before packaging
call "%ROOT%\build.bat"
if errorlevel 1 (
    echo [edvr] ERROR: build failed; nothing packaged
    exit /b 1
)

echo [edvr] running the build checks
"%ROOT%\build\smoke.exe" "%ROOT%\build\d3d11.dll"
if errorlevel 1 (
    echo [edvr] ERROR: smoke test failed; nothing packaged
    exit /b 1
)

if not exist "%ROOT%\build\d3d11.dll" (
    echo [edvr] ERROR: build\d3d11.dll not found after building.
    exit /b 1
)

if exist "%STAGE%" rmdir /s /q "%STAGE%"
if exist "%ZIP%" del /f /q "%ZIP%"
mkdir "%STAGE%" 2>nul

copy /Y "%ROOT%\build\d3d11.dll"    "%STAGE%\d3d11.dll"   >nul
copy /Y "%ROOT%\edvr.ini"           "%STAGE%\edvr.ini"    >nul
copy /Y "%ROOT%\release\README.txt" "%STAGE%\README.txt"  >nul
copy /Y "%ROOT%\LICENSE"            "%STAGE%\LICENSE.txt" >nul

REM Optional, and shipped only if it was built. It is the half of the transition
REM flash fix that can actually withhold a frame, but it installs differently
REM from d3d11.dll -- it replaces a file the game owns rather than adding one --
REM so it goes in its own folder with its own instructions rather than loose in
REM the root where it could be copied in by reflex.
if exist "%ROOT%\build\openvr_api.dll" (
    mkdir "%STAGE%\openvr" 2>nul
    copy /Y "%ROOT%\build\openvr_api.dll" "%STAGE%\openvr\openvr_api.dll" >nul
    copy /Y "%ROOT%\release\OPENVR.txt"   "%STAGE%\openvr\READ-ME-FIRST.txt" >nul
    echo [edvr] included openvr_api.dll ^(transition flash fix^)
) else (
    echo [edvr] NOTE: no build\openvr_api.dll -- packaging without the transition
    echo        flash fix. Run build.bat with the game installed to include it.
)

echo [edvr] staged:
dir /b /s "%STAGE%"

powershell -NoProfile -Command ^
  "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 ( echo [edvr] ERROR: zip failed & exit /b 1 )

echo.
echo [edvr] wrote %ZIP%
for %%F in ("%ZIP%") do echo        %%~zF bytes
powershell -NoProfile -Command ^
  "'       SHA-256 ' + (Get-FileHash '%ZIP%' -Algorithm SHA256).Hash"
echo.
echo [edvr] Before publishing, check the archive holds only the files listed
echo        above and no logs, shader dumps or game binaries. In particular it
echo        must not contain the game's own openvr_api.dll -- only ours.
exit /b 0
