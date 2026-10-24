@echo off
rem Builds the Windows x64 spike probes with MSVC (Visual Studio 2022 Build Tools). Output: build\
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set VCVARS=
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (echo no MSVC x64 toolset found & exit /b 1)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /W4 /WX /wd4201 /wd4204 /wd4152 /wd4055 /wd4054 guest_layout_probe.c /Fobuild\ /Febuild\guest_layout_probe.exe /link /DYNAMICBASE /HIGHENTROPYVA:NO || exit /b 1
rem The same probe with high-entropy ASLR, for comparison (thread stacks can then land below 2^39)
cl /nologo /O2 /W4 /WX /wd4201 /wd4204 /wd4152 /wd4055 /wd4054 guest_layout_probe.c /Fobuild\ /Febuild\guest_layout_probe_heva.exe /link /DYNAMICBASE /HIGHENTROPYVA || exit /b 1
set DYN=third_party\dynarmic
set DB=build\dynarmic
cl /nologo /std:c++20 /EHsc /O2 /MD /W3 /permissive- /Zc:preprocessor /I%DYN%\src /I%DYN%\externals\mcl\include /I%DYN%\externals\oaknut\include /Ithird_party\boost jit_boot_probe.cpp /Fobuild\ /Febuild\jit_boot_probe.exe /link /HIGHENTROPYVA:NO %DB%\src\dynarmic\dynarmic.lib %DB%\externals\fmt\fmt.lib %DB%\externals\mcl\src\mcl.lib %DB%\externals\zydis\Zydis.lib %DB%\externals\zydis\zycore\Zycore.lib || exit /b 1
