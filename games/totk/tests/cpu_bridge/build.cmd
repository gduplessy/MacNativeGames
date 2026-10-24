@echo off
rem Windows x64 conformance test for runtime/cpu (jit backend) and the generated NVN bridge.
rem Needs Visual Studio 2022 Build Tools (MSVC x64), Python 3.14, and dynarmic built as in games/totk/spikes/x64/README.md
rem (DYNARMIC_SRC / DYNARMIC_BUILD override where it is). Then run.cmd.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set VCVARS=
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (echo no MSVC x64 toolset found & exit /b 1)
call "%VCVARS%" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
set TOTK=..\..
set NVN=%TOTK%\third_party\d2r_gfx\gfx\nvn
if not defined DYNARMIC_SRC set DYNARMIC_SRC=%TOTK%\spikes\x64\third_party\dynarmic
if not defined DYNARMIC_BUILD set DYNARMIC_BUILD=%TOTK%\spikes\x64\build\dynarmic
set DYNLIBS=%DYNARMIC_BUILD%\src\dynarmic\dynarmic.lib %DYNARMIC_BUILD%\externals\fmt\fmt.lib %DYNARMIC_BUILD%\externals\mcl\src\mcl.lib %DYNARMIC_BUILD%\externals\zydis\Zydis.lib %DYNARMIC_BUILD%\externals\zydis\zycore\Zycore.lib
if not exist build\negative mkdir build\negative
rem the committed bridge must be what the generator makes now; the probes are test-only
python %TOTK%\tools\gen_cpu_bridge.py %NVN% --check --probes build\probes.c || exit /b 1
for /f "tokens=3" %%n in ('findstr /c:"#define NVN_BRIDGE_SLOTS" %TOTK%\runtime\cpu\nvn_bridge.c') do echo #define NVN_BRIDGE_SLOTS %%n> build\slot_count.h
set INC=/I%TOTK%\runtime\cpu /I%NVN%\include /Ibuild
set TESTFLAGS=/nologo /std:c++20 /EHsc /O2 /MD /W3 /permissive- %INC% /I%DYNARMIC_SRC%\externals\oaknut\include
cl /nologo /c /std:c17 /O2 /MD /W4 /WX /wd4100 %INC% %TOTK%\runtime\cpu\nvn_bridge.c /Fobuild\nvn_bridge.obj || exit /b 1
cl /nologo /c /std:c17 /O2 /MD /W3 %INC% build\probes.c /Fobuild\probes.obj || exit /b 1
cl /nologo /c /std:c++20 /EHsc /O2 /MD /W4 /WX /wd4100 /wd4324 /permissive- %INC% /I%DYNARMIC_SRC%\src /I%DYNARMIC_SRC%\externals\mcl\include %TOTK%\runtime\cpu\cpu_jit.cpp /Fobuild\cpu_jit.obj || exit /b 1
cl %TESTFLAGS% bridge_test.cpp build\cpu_jit.obj build\nvn_bridge.obj build\probes.obj /Fobuild\ /Febuild\bridge_test.exe /link /HIGHENTROPYVA:NO %DYNLIBS% || exit /b 1
rem negative control: the same test against a bridge numbered by argument position (the Windows x64 rule); it must fail
python %TOTK%\tools\gen_cpu_bridge.py %NVN% --negative -o build\nvn_bridge_negative.c || exit /b 1
cl /nologo /c /std:c17 /O2 /MD /W3 %INC% build\nvn_bridge_negative.c /Fobuild\nvn_bridge_negative.obj || exit /b 1
cl %TESTFLAGS% bridge_test.cpp build\cpu_jit.obj build\nvn_bridge_negative.obj build\probes.obj /Fobuild\negative\ /Febuild\bridge_test_negative.exe /link /HIGHENTROPYVA:NO %DYNLIBS% || exit /b 1
