@echo off
rem wwe2k14 - builds and runs the CAW library self-test (src\caw_library) with MSVC.
rem
rem   tools\caw_library\run_tests.bat            every case, including the 5000-CAW capacity run
rem   tools\caw_library\run_tests.bat --quick    the same cases without it (~2 s)
rem
rem No game files, no ReXGlue SDK and no save are needed: every case is files in a temporary folder.
setlocal
set "ROOT=%~dp0..\.."
pushd "%ROOT%" || exit /b 1

rem The newest Visual Studio with the C++ tools (vswhere ships with every VS).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSDIR=%%i"
)
if not defined VSDIR (
  echo No Visual Studio with the C++ tools was found ^(install the "Desktop development with C++" workload^).
  popd & exit /b 2
)

rem The newest MSVC toolset and the newest Windows 10/11 SDK in that install. vcvars64.bat is deliberately not used:
rem it fails on some installs, while these paths are exactly what it would set.
set "MSVC="
for /d %%v in ("%VSDIR%\VC\Tools\MSVC\*") do set "MSVC=%%v"
set "WK=%ProgramFiles(x86)%\Windows Kits\10"
set "SDKDIR="
for /d %%v in ("%WK%\Include\10.*") do set "SDKDIR=%%v"
if not defined MSVC ( echo No MSVC toolset under "%VSDIR%\VC\Tools\MSVC". & popd & exit /b 2 )
if not defined SDKDIR ( echo No Windows SDK under "%WK%\Include". & popd & exit /b 2 )
for %%v in ("%SDKDIR%") do set "SDKVER=%%~nxv"

set "PATH=%MSVC%\bin\Hostx64\x64;%WK%\bin\%SDKVER%\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%WK%\Include\%SDKVER%\ucrt;%WK%\Include\%SDKVER%\um;%WK%\Include\%SDKVER%\shared"
set "LIB=%MSVC%\lib\x64;%WK%\Lib\%SDKVER%\ucrt\x64;%WK%\Lib\%SDKVER%\um\x64"

if not exist out\caw_library mkdir out\caw_library
echo Toolset: %MSVC%
echo SDK:     %SDKVER%
cl /nologo /std:c++20 /EHsc /W4 /O2 /Fe:out\caw_library\caw_library_test.exe /Fo:out\caw_library\ ^
   src\caw_library\caw_library.cpp src\caw_library\caw_library_test.cpp
if errorlevel 1 ( echo COMPILE FAILED & popd & exit /b 3 )

out\caw_library\caw_library_test.exe %*
if errorlevel 1 ( echo TESTS FAILED & popd & exit /b 4 )
echo TESTS PASSED
popd
exit /b 0
