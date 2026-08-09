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

if not exist "%ROOT%\build\d3d11.dll" (
    echo [edvr] ERROR: build\d3d11.dll not found. Run build.bat first.
    exit /b 1
)

if exist "%STAGE%" rmdir /s /q "%STAGE%"
if exist "%ZIP%" del /f /q "%ZIP%"
mkdir "%STAGE%" 2>nul

copy /Y "%ROOT%\build\d3d11.dll"    "%STAGE%\d3d11.dll"   >nul
copy /Y "%ROOT%\edvr.ini"           "%STAGE%\edvr.ini"    >nul
copy /Y "%ROOT%\release\README.txt" "%STAGE%\README.txt"  >nul
copy /Y "%ROOT%\LICENSE"            "%STAGE%\LICENSE.txt" >nul

echo [edvr] staged:
dir /b "%STAGE%"

powershell -NoProfile -Command ^
  "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 ( echo [edvr] ERROR: zip failed & exit /b 1 )

echo.
echo [edvr] wrote %ZIP%
for %%F in ("%ZIP%") do echo        %%~zF bytes
powershell -NoProfile -Command ^
  "'       SHA-256 ' + (Get-FileHash '%ZIP%' -Algorithm SHA256).Hash"
echo.
echo [edvr] Before publishing, check the archive holds exactly those four files
echo        and no logs, shader dumps or game binaries.
exit /b 0
