@echo off
rem Builds build\Ephemeront.exe with the MSVC command-line tools.
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Could not find vswhere.exe. Install Visual Studio or the Build Tools with the C++ workload.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo No Visual Studio install with the C++ tools was found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
if not exist build mkdir build

set CFLAGS=/nologo /utf-8 /std:c++17 /O2 /MT /EHsc /W4 /permissive- /DUNICODE /D_UNICODE /DNOMINMAX ^
 /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A00000A

cl %CFLAGS% /Fo"build\\" /Fe"build\Ephemeront.exe" src\*.cpp /link /SUBSYSTEM:WINDOWS user32.lib || exit /b 1
echo Built build\Ephemeront.exe
