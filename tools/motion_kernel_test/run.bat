@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl.exe /nologo /O2 /MT /std:c++17 /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Febuild\motion_kernel_test\motion_kernel_test.exe /Fobuild\motion_kernel_test\motion_kernel_test.obj tools\motion_kernel_test\motion_kernel_test.cpp /link d3d11.lib d3dcompiler.lib dxgi.lib
if errorlevel 1 exit /b 1
pushd build\motion_kernel_test
motion_kernel_test.exe > results.txt
set RESULT=%ERRORLEVEL%
popd
exit /b %RESULT%
