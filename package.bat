@echo off
setlocal
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "VERSION=%~1"
if "%VERSION%"=="" (
  echo [edvr] usage: package.bat ^<version^> [--no-dlss]
  exit /b 1
)
echo [edvr] building before packaging
call "%ROOT%\build.bat" || exit /b 1
echo [edvr] staging native release
call python "%ROOT%\tools\package_native.py" "%VERSION%" %2 || exit /b 1
echo [edvr] staging flat capture release
call python "%ROOT%\tools\package_native.py" "%VERSION%" %2 --profile flat || exit /b 1
exit /b 0
